// i386 Mach-O loader. Maps images below 4 GB, rebases, binds every import
// up front (no lazy binding), and runs initializers.
//
// Two fixup styles: classic (local/external relocations + indirect symbol
// table, as in Valve's 2011-era dylibs) and LC_DYLD_INFO opcodes (what a
// modern ld emits, used by our own guest runtime libraries).
#include "mac32.h"
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <mach-o/fat.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <libkern/OSByteOrder.h>

struct sym { const char *name; g32 addr; };

#define MAXSEG 32
#define MAXDEP 128
struct image {
	char *path, *dir;
	uint8_t *f;			// i386 slice of the file (host memory)
	size_t fsize;
	struct mach_header *mh;
	g32 lo, hi;
	int32_t slide;
	struct segment_command *seg[MAXSEG];
	int nseg;
	struct symtab_command *st;
	struct dysymtab_command *dst;
	struct dyld_info_command *di;
	struct routines_command *routines;
	struct nlist *syms;
	const char *strs;
	int ndeps;
	char *depname[MAXDEP];
	struct image *dep[MAXDEP];
	uint8_t depsys[MAXDEP], depreexp[MAXDEP];
	struct sym *ex;			// exports, open-addressed hash
	uint32_t excap;
	struct sym *bya;		// every section symbol, sorted by address
	uint32_t nbya;
	g32 entry;
	int inited;
	struct image *next;
};
static struct image *images, *main_image;
static char *searchdirs[16];
static int nsearch;

void macho_add_search_dir(const char *d) { if (nsearch < 16) searchdirs[nsearch++] = strdup(d); }

// ---- symbol tables --------------------------------------------------------
static uint32_t shash(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

g32 macho_sym(struct image *img, const char *name)
{
	if (!img || !img->excap) return 0;
	for (uint32_t i = shash(name) & (img->excap - 1);; i = (i + 1) & (img->excap - 1)) {
		if (!img->ex[i].name) return 0;
		if (!strcmp(img->ex[i].name, name)) return img->ex[i].addr;
	}
}

static g32 sym_in_tree(struct image *img, const char *name)
{
	g32 a = macho_sym(img, name);
	for (int i = 0; !a && i < img->ndeps; i++)
		if (img->depreexp[i] && img->dep[i]) a = sym_in_tree(img->dep[i], name);
	return a;
}

g32 macho_find_any(const char *name)
{
	for (struct image *i = images; i; i = i->next) {
		g32 a = macho_sym(i, name);
		if (a) return a;
	}
	return host_lookup(name);
}

g32 macho_dlsym(struct image *img, const char *name)
{
	g32 a = sym_in_tree(img, name);
	return a ? a : 0;
}

static int cmp_addr(const void *a, const void *b)
{
	g32 x = ((const struct sym *)a)->addr, y = ((const struct sym *)b)->addr;
	return x < y ? -1 : x > y;
}

static void index_symbols(struct image *img)
{
	if (!img->st) return;
	uint32_t n = img->st->nsyms, nex = 0;
	img->bya = calloc(n + 1, sizeof *img->bya);
	for (uint32_t i = 0; i < n; i++) {
		struct nlist *s = &img->syms[i];
		if (s->n_type & N_STAB) continue;
		if ((s->n_type & N_TYPE) != N_SECT) continue;
		const char *nm = img->strs + s->n_un.n_strx;
		img->bya[img->nbya++] = (struct sym){ nm, s->n_value + (g32)img->slide };
		if ((s->n_type & N_EXT) && !(s->n_type & N_PEXT)) nex++;
	}
	qsort(img->bya, img->nbya, sizeof *img->bya, cmp_addr);
	img->excap = 16;
	while (img->excap < nex * 2) img->excap *= 2;
	img->ex = calloc(img->excap, sizeof *img->ex);
	for (uint32_t i = 0; i < n; i++) {
		struct nlist *s = &img->syms[i];
		if ((s->n_type & N_STAB) || (s->n_type & N_TYPE) != N_SECT) continue;
		if (!(s->n_type & N_EXT) || (s->n_type & N_PEXT)) continue;
		const char *nm = img->strs + s->n_un.n_strx;
		uint32_t h = shash(nm) & (img->excap - 1);
		while (img->ex[h].name) h = (h + 1) & (img->excap - 1);
		img->ex[h] = (struct sym){ nm, s->n_value + (g32)img->slide };
	}
}

int macho_section(struct image *img, const char *seg, const char *sect, g32 *addr, uint32_t *size);
static struct image *image_at(g32 a)
{
	for (struct image *img = images; img; img = img->next)
		if (a >= img->lo + (g32)img->slide && a < img->hi + (g32)img->slide) return img;
	return NULL;
}

// _dyld_find_unwind_sections for the guest's libunwind. i386 struct:
// {mh, dwarf_section, dwarf_section_length, compact_unwind_section, compact_unwind_section_length}
int macho_find_unwind(g32 addr, g32 out)
{
	struct image *img = image_at(addr);
	if (!img) return 0;
	uint32_t *o = G2H(out);
	g32 a;
	uint32_t sz;
	memset(o, 0, 20);
	o[0] = img->lo + (g32)img->slide;
	if (macho_section(img, "__TEXT", "__eh_frame", &a, &sz)) { o[1] = a; o[2] = sz; }
	if (macho_section(img, "__TEXT", "__unwind_info", &a, &sz)) { o[3] = a; o[4] = sz; }
	return o[2] || o[4];
}

// dladdr: i386 Dl_info {fname, fbase, sname, saddr}
int macho_dladdr(g32 addr, g32 out)
{
	struct image *img = image_at(addr);
	if (!img) return 0;
	uint32_t *o = G2H(out);
	static g32 names[256];
	static struct image *named[256];
	int k = 0;
	while (k < 255 && named[k] && named[k] != img) k++;
	if (!named[k]) { named[k] = img; names[k] = gstrdup(img->path); }
	o[0] = names[k];
	o[1] = img->lo + (g32)img->slide;
	o[2] = 0; o[3] = 0;
	uint32_t lo = 0, hi = img->nbya;
	while (lo < hi) { uint32_t m = (lo + hi) / 2; if (img->bya[m].addr <= addr) lo = m + 1; else hi = m; }
	if (lo) { o[2] = guest_cstr(img->bya[lo - 1].name + (img->bya[lo - 1].name[0] == '_')); o[3] = img->bya[lo - 1].addr; }
	return 1;
}

const char *macho_image_path(g32 a)
{
	for (struct image *img = images; img; img = img->next)
		if (a >= img->lo + (g32)img->slide && a < img->hi + (g32)img->slide) return img->path;
	return NULL;
}

const char *macho_addr_name(g32 a, g32 *off)
{
	for (struct image *img = images; img; img = img->next) {
		if (a < img->lo + (g32)img->slide || a >= img->hi + (g32)img->slide) continue;
		uint32_t lo = 0, hi = img->nbya;
		while (lo < hi) {
			uint32_t m = (lo + hi) / 2;
			if (img->bya[m].addr <= a) lo = m + 1; else hi = m;
		}
		static char buf[512];
		const char *leaf = strrchr(img->path, '/');
		leaf = leaf ? leaf + 1 : img->path;
		if (lo == 0) { *off = a - (img->lo + (g32)img->slide); snprintf(buf, sizeof buf, "%s", leaf); }
		else { *off = a - img->bya[lo - 1].addr; snprintf(buf, sizeof buf, "%s`%s", leaf, img->bya[lo - 1].name); }
		return buf;
	}
	return NULL;
}

// ---- resolving imports ----------------------------------------------------
int unresolved_count;

static g32 resolve(struct image *img, int ord, const char *name, int weak)
{
	// host overrides ("!name") win over every guest library: used to switch
	// off subsystems we cannot run yet, through their own API
	char ov[512];
	snprintf(ov, sizeof ov, "!%s", name);
	g32 a = host_lookup(ov);
	if (a) {
		LOG("override %s (needed by %s)\n", name, img->path);
		return a;
	}
	if (!(img->mh->flags & MH_TWOLEVEL) || ord == DYNAMIC_LOOKUP_ORDINAL || ord == -2) {
		a = macho_find_any(name);
	} else if (ord == SELF_LIBRARY_ORDINAL) {
		a = macho_sym(img, name);
	} else if (ord == EXECUTABLE_ORDINAL || ord == -1) {
		a = main_image ? macho_sym(main_image, name) : 0;
	} else if (ord - 1 < img->ndeps) {
		struct image *d = img->dep[ord - 1];
		if (d) a = sym_in_tree(d, name);
		else if (img->depsys[ord - 1]) a = host_lookup(name);
		if (!a) a = macho_find_any(name);	// moved symbols, our runtime libs
	}
	if (!a && weak) LOG("weak import %s missing (needed by %s): bound to 0\n", name, img->path);
	if (!a && !weak) {
		unresolved_count++;
		LOG("unresolved %s (needed by %s)\n", name, img->path);
		a = missing_stub(name);
	}
	return a;
}

static g32 resolve_nlist(struct image *img, uint32_t symidx)
{
	struct nlist *s = &img->syms[symidx];
	const char *name = img->strs + s->n_un.n_strx;
	if ((s->n_type & N_TYPE) == N_SECT) return s->n_value + (g32)img->slide;	// defined here
	return resolve(img, GET_LIBRARY_ORDINAL(s->n_desc), name, (s->n_desc & N_WEAK_REF) != 0);
}

// ---- classic fixups -------------------------------------------------------
static g32 *P(g32 a) { return G2H(a); }

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static void fix_classic(struct image *img)
{
	g32 relocbase = 0;
	for (int i = 0; i < img->nseg; i++) {
		struct segment_command *sg = img->seg[i];
		if (img->mh->flags & MH_SPLIT_SEGS) {
			if (sg->initprot & VM_PROT_WRITE) { relocbase = sg->vmaddr; break; }
		} else if (sg->fileoff == 0 && sg->filesize) { relocbase = sg->vmaddr; break; }
	}
	relocbase += (g32)img->slide;

	struct dysymtab_command *d = img->dst;
	uint32_t *done = calloc(d->nlocrel + 1, 4), ndone = 0;
	struct relocation_info *rel = (void *)(img->f + d->locreloff);
	for (uint32_t i = 0; i < d->nlocrel; i++) {
		if (rel[i].r_address & R_SCATTERED) {
			struct scattered_relocation_info *s = (void *)&rel[i];
			if (s->r_type == GENERIC_RELOC_VANILLA || s->r_type == GENERIC_RELOC_PB_LA_PTR) {
				if (s->r_length != 2) die("%s: scattered reloc length %d", img->path, s->r_length);
				*P(relocbase + s->r_address) += (g32)img->slide;
				done[ndone++] = relocbase + s->r_address;
			} else if (s->r_type != GENERIC_RELOC_PAIR) {
				LOG("%s: skip scattered reloc type %d\n", img->path, s->r_type);
			}
		} else {
			if (rel[i].r_type != GENERIC_RELOC_VANILLA || rel[i].r_pcrel || rel[i].r_length != 2)
				die("%s: odd local reloc type %d", img->path, rel[i].r_type);
			*P(relocbase + (g32)rel[i].r_address) += (g32)img->slide;
			done[ndone++] = relocbase + (g32)rel[i].r_address;
		}
	}
	qsort(done, ndone, 4, cmp_u32);

	rel = (void *)(img->f + d->extreloff);
	for (uint32_t i = 0; i < d->nextrel; i++) {
		if ((rel[i].r_address & R_SCATTERED) || !rel[i].r_extern || rel[i].r_length != 2)
			die("%s: odd external reloc", img->path);
		g32 a = resolve_nlist(img, rel[i].r_symbolnum);
		g32 at = relocbase + (g32)rel[i].r_address;
		if (rel[i].r_pcrel) *P(at) += a - (at + 4);
		else *P(at) += a;
	}

	uint32_t *ind = (void *)(img->f + d->indirectsymoff);
	for (int si = 0; si < img->nseg; si++) {
		struct segment_command *sg = img->seg[si];
		struct section *sec = (void *)(sg + 1);
		for (uint32_t k = 0; k < sg->nsects; k++, sec++) {
			uint32_t type = sec->flags & SECTION_TYPE;
			g32 base = sec->addr + (g32)img->slide;
			if (type == S_NON_LAZY_SYMBOL_POINTERS || type == S_LAZY_SYMBOL_POINTERS) {
				for (uint32_t j = 0; j < sec->size / 4; j++) {
					uint32_t is = ind[sec->reserved1 + j];
					g32 at = base + j * 4;
					if (is & INDIRECT_SYMBOL_ABS) continue;
					if (is == INDIRECT_SYMBOL_LOCAL) {
						if (img->slide && !bsearch(&at, done, ndone, 4, cmp_u32))
							*P(at) += (g32)img->slide;
						continue;
					}
					*P(at) = resolve_nlist(img, is);
				}
			} else if (type == S_SYMBOL_STUBS && sec->reserved2 == 5 &&
				   (sec->flags & S_ATTR_SELF_MODIFYING_CODE)) {
				for (uint32_t j = 0; j < sec->size / 5; j++) {
					uint32_t is = ind[sec->reserved1 + j];
					if (is & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
					g32 at = base + j * 5, t = resolve_nlist(img, is);
					uint8_t *p = G2H(at);
					int32_t relv = (int32_t)(t - (at + 5));
					p[0] = 0xe9;
					memcpy(p + 1, &relv, 4);
				}
			} else if (!strcmp(sec->sectname, "__dyld") && !strcmp(sec->segname, "__DATA") && sec->size >= 8) {
				P(base)[0] = missing_stub("dyld_lazy_symbol_binding_entry_point");
				P(base)[1] = host_lookup("dyld_func_lookup");
			}
		}
	}
	free(done);
}

// ---- LC_DYLD_INFO fixups --------------------------------------------------
static uint64_t uleb(const uint8_t **p)
{
	uint64_t v = 0;
	int sh = 0;
	uint8_t b;
	do { b = *(*p)++; v |= (uint64_t)(b & 0x7f) << sh; sh += 7; } while (b & 0x80);
	return v;
}
static int64_t sleb(const uint8_t **p)
{
	int64_t v = 0;
	int sh = 0;
	uint8_t b;
	do { b = *(*p)++; v |= (int64_t)(b & 0x7f) << sh; sh += 7; } while (b & 0x80);
	if (sh < 64 && (b & 0x40)) v |= -((int64_t)1 << sh);
	return v;
}

static void do_rebase(struct image *img, const uint8_t *p, const uint8_t *end)
{
	int type = REBASE_TYPE_POINTER;
	g32 addr = 0;
	while (p < end) {
		uint8_t op = *p & REBASE_OPCODE_MASK, imm = *p & REBASE_IMMEDIATE_MASK;
		p++;
		uint64_t n, skip;
		switch (op) {
		case REBASE_OPCODE_DONE: return;
		case REBASE_OPCODE_SET_TYPE_IMM: type = imm; break;
		case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
			addr = img->seg[imm]->vmaddr + (g32)uleb(&p) + (g32)img->slide; break;
		case REBASE_OPCODE_ADD_ADDR_ULEB: addr += (g32)uleb(&p); break;
		case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: addr += imm * 4; break;
		case REBASE_OPCODE_DO_REBASE_IMM_TIMES: n = imm; skip = 0; goto run;
		case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: n = uleb(&p); skip = 0; goto run;
		case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: n = 1; skip = uleb(&p); goto run;
		case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: n = uleb(&p); skip = uleb(&p); goto run;
		default: die("%s: rebase opcode %#x", img->path, op);
		run:
			while (n--) {
				if (type == REBASE_TYPE_TEXT_PCREL32) *P(addr) -= (g32)img->slide;
				else *P(addr) += (g32)img->slide;
				addr += 4 + (g32)skip;
			}
		}
	}
}

static void do_bind(struct image *img, const uint8_t *p, const uint8_t *end, int lazy)
{
	int ord = 0, type = BIND_TYPE_POINTER, weak = 0;
	const char *name = NULL;
	int64_t addend = 0;
	g32 addr = 0;
	while (p < end) {
		uint8_t op = *p & BIND_OPCODE_MASK, imm = *p & BIND_IMMEDIATE_MASK;
		p++;
		uint64_t n = 1, skip = 0;
		switch (op) {
		case BIND_OPCODE_DONE: if (!lazy) return; break;
		case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: ord = imm; break;
		case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: ord = (int)uleb(&p); break;
		case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: ord = imm ? (int8_t)(BIND_OPCODE_MASK | imm) : 0; break;
		case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
			name = (const char *)p;
			p += strlen(name) + 1;
			weak = (imm & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
			break;
		case BIND_OPCODE_SET_TYPE_IMM: type = imm; break;
		case BIND_OPCODE_SET_ADDEND_SLEB: addend = sleb(&p); break;
		case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
			addr = img->seg[imm]->vmaddr + (g32)uleb(&p) + (g32)img->slide; break;
		case BIND_OPCODE_ADD_ADDR_ULEB: addr += (g32)uleb(&p); break;
		case BIND_OPCODE_DO_BIND: goto run;
		case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: skip = uleb(&p); goto run;
		case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: skip = imm * 4u; goto run;
		case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: n = uleb(&p); skip = uleb(&p); goto run;
		default: die("%s: bind opcode %#x", img->path, op);
		run: {
			g32 a = resolve(img, ord, name, weak);
			while (n--) {
				if (type == BIND_TYPE_TEXT_PCREL32) *P(addr) = a - (addr + 4) + (g32)addend;
				else *P(addr) = a + (g32)addend;
				addr += 4 + (g32)skip;
			}
		}
		}
	}
}

static void fix_dyldinfo(struct image *img)
{
	struct dyld_info_command *di = img->di;
	if (img->slide && di->rebase_size)
		do_rebase(img, img->f + di->rebase_off, img->f + di->rebase_off + di->rebase_size);
	if (di->bind_size) do_bind(img, img->f + di->bind_off, img->f + di->bind_off + di->bind_size, 0);
	if (di->lazy_bind_size)
		do_bind(img, img->f + di->lazy_bind_off, img->f + di->lazy_bind_off + di->lazy_bind_size, 1);
	// weak_bind: coalescing across images is not done; first definition wins
}

// ---- loading ----------------------------------------------------------------
static struct image *find_loaded(const char *rp)
{
	for (struct image *i = images; i; i = i->next)
		if (!strcmp(i->path, rp)) return i;
	return NULL;
}

static int exists(const char *p) { struct stat s; return stat(p, &s) == 0 && S_ISREG(s.st_mode); }

static int is_system(const char *n)
{
	return !strncmp(n, "/usr/lib/", 9) || !strncmp(n, "/System/", 8) ||
	       !strncmp(n, "/usr/local/lib/", 15);
}

// dyld's search order: DYLD_LIBRARY_PATH leaf first, then the install path
static int find_dylib(struct image *from, const char *name, char *out)
{
	const char *leaf = strrchr(name, '/');
	leaf = leaf ? leaf + 1 : name;
	for (int i = 0; i < nsearch; i++) {
		snprintf(out, PATH_MAX, "%s/%s", searchdirs[i], leaf);
		if (exists(out)) return 1;
	}
	if (!strncmp(name, "@loader_path/", 13) && from) {
		snprintf(out, PATH_MAX, "%s/%s", from->dir, name + 13);
		return exists(out);
	}
	if (!strncmp(name, "@executable_path/", 17) && main_image) {
		snprintf(out, PATH_MAX, "%s/%s", main_image->dir, name + 17);
		return exists(out);
	}
	if (name[0] != '@') {
		snprintf(out, PATH_MAX, "%s", name);
		return exists(out) && !is_system(name);
	}
	return 0;
}

static struct image *load_file(const char *rp);
void objc_image_loaded(struct image *img);

static void load_deps(struct image *img)
{
	for (int i = 0; i < img->ndeps; i++) {
		char p[PATH_MAX];
		const char *n = img->depname[i];
		if (find_dylib(img, n, p)) {
			img->dep[i] = load_file(p);
			if (!img->dep[i]) die("%s: cannot load %s", img->path, p);
		} else if (is_system(n)) {
			img->depsys[i] = 1;
		} else {
			LOG("%s: dependency %s not found\n", img->path, n);
			img->depsys[i] = 1;
		}
	}
}

static uint8_t *read_slice(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return NULL;
	struct stat st;
	fstat(fd, &st);
	uint8_t *buf = malloc((size_t)st.st_size);
	if (read(fd, buf, (size_t)st.st_size) != st.st_size) die("short read %s", path);
	close(fd);
	*len = (size_t)st.st_size;
	if (OSSwapBigToHostInt32(*(uint32_t *)buf) == FAT_MAGIC) {
		struct fat_header *fh = (void *)buf;
		struct fat_arch *fa = (void *)(fh + 1);
		for (uint32_t i = 0; i < OSSwapBigToHostInt32(fh->nfat_arch); i++) {
			if (OSSwapBigToHostInt32((uint32_t)fa[i].cputype) == CPU_TYPE_I386) {
				uint32_t off = OSSwapBigToHostInt32(fa[i].offset), sz = OSSwapBigToHostInt32(fa[i].size);
				uint8_t *s = malloc(sz);
				memcpy(s, buf + off, sz);
				free(buf);
				*len = sz;
				return s;
			}
		}
		die("%s: no i386 slice", path);
	}
	return buf;
}

static struct image *load_file(const char *path)
{
	char rp[PATH_MAX];
	if (!realpath(path, rp)) return NULL;
	struct image *img = find_loaded(rp);
	if (img) return img;

	img = calloc(1, sizeof *img);
	img->path = strdup(rp);
	img->dir = strdup(rp);
	*strrchr(img->dir, '/') = 0;
	img->f = read_slice(rp, &img->fsize);
	if (!img->f) die("cannot read %s", rp);
	img->mh = (void *)img->f;
	if (img->mh->magic != MH_MAGIC || img->mh->cputype != CPU_TYPE_I386)
		die("%s: not an i386 Mach-O", rp);

	uint8_t *lc = (uint8_t *)(img->mh + 1);
	img->lo = 0xffffffff;
	for (uint32_t i = 0; i < img->mh->ncmds; i++, lc += ((struct load_command *)lc)->cmdsize) {
		struct load_command *c = (void *)lc;
		switch (c->cmd) {
		case LC_SEGMENT: {
			struct segment_command *sg = (void *)c;
			if (img->nseg == MAXSEG) die("too many segments");
			img->seg[img->nseg++] = sg;
			if (!strcmp(sg->segname, SEG_PAGEZERO) || !sg->vmsize) break;
			if (sg->vmaddr < img->lo) img->lo = sg->vmaddr;
			if (sg->vmaddr + sg->vmsize > img->hi) img->hi = sg->vmaddr + sg->vmsize;
			break;
		}
		case LC_SYMTAB: img->st = (void *)c; break;
		case LC_DYSYMTAB: img->dst = (void *)c; break;
		case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY: img->di = (void *)c; break;
		case LC_ROUTINES: img->routines = (void *)c; break;
		case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB:
		case LC_LAZY_LOAD_DYLIB: case LC_LOAD_UPWARD_DYLIB: {
			struct dylib_command *dc = (void *)c;
			if (img->ndeps == MAXDEP) die("too many dependencies");
			img->depreexp[img->ndeps] = c->cmd == LC_REEXPORT_DYLIB;
			img->depname[img->ndeps++] = (char *)c + dc->dylib.name.offset;
			break;
		}
		case LC_UNIXTHREAD: {
			// flavor, count, then i386_thread_state: eax ebx ecx edx edi esi ebp esp ss eflags eip
			uint32_t *ts = (uint32_t *)(c + 1) + 2;
			img->entry = ts[10];
			break;
		}
		case LC_MAIN: die("%s: LC_MAIN executables not supported yet", rp);
		}
	}
	if (img->st) {
		img->syms = (void *)(img->f + img->st->symoff);
		img->strs = (const char *)(img->f + img->st->stroff);
	}

	uint32_t span = img->hi - img->lo;
	if (img->mh->filetype == MH_EXECUTE && !(img->mh->flags & MH_PIE)) {
		low_commit(img->lo, span, PROT_READ | PROT_WRITE);
		img->slide = 0;
	} else {
		g32 base = low_pages(span, PROT_READ | PROT_WRITE);
		img->slide = (int32_t)(base - img->lo);
	}
	for (int i = 0; i < img->nseg; i++) {
		struct segment_command *sg = img->seg[i];
		if (!strcmp(sg->segname, SEG_PAGEZERO) || !sg->filesize) continue;
		memcpy(G2H(sg->vmaddr + (g32)img->slide), img->f + sg->fileoff,
		       sg->filesize < sg->vmsize ? sg->filesize : sg->vmsize);
	}
	if (img->entry) img->entry += (g32)img->slide;
	if (img->mh->filetype == MH_EXECUTE && !main_image) main_image = img;

	// register before loading dependencies: libraries can depend on each other
	struct image **tail = &images;
	while (*tail) tail = &(*tail)->next;
	*tail = img;
	index_symbols(img);
	LOG("loaded %s at %#x-%#x (slide %#x)\n", rp, img->lo + (g32)img->slide,
	    img->hi + (g32)img->slide, (g32)img->slide);

	load_deps(img);
	if (img->di) fix_dyldinfo(img);
	else if (img->dst) fix_classic(img);
	objc_image_loaded(img);

	for (int i = 0; i < img->nseg; i++) {
		struct segment_command *sg = img->seg[i];
		if (!strcmp(sg->segname, SEG_PAGEZERO) || !sg->vmsize) continue;
		int prot = PROT_READ;
		if (sg->initprot & VM_PROT_WRITE) prot |= PROT_WRITE;
		if (sg->initprot & VM_PROT_EXECUTE) prot |= PROT_EXEC;
		mprotect(G2H(sg->vmaddr + (g32)img->slide), (sg->vmsize + 0xfff) & ~0xfffu, prot);
	}
	return img;
}

struct image *macho_load(const char *path) { return load_file(path); }

int macho_section(struct image *img, const char *seg, const char *sect, g32 *addr, uint32_t *size)
{
	for (int si = 0; si < img->nseg; si++) {
		struct segment_command *sg = img->seg[si];
		struct section *s = (void *)(sg + 1);
		for (uint32_t k = 0; k < sg->nsects; k++, s++)
			if (!strncmp(s->segname, seg, 16) && !strncmp(s->sectname, sect, 16)) {
				*addr = s->addr + (g32)img->slide;
				*size = s->size;
				return 1;
			}
	}
	return 0;
}

struct image *macho_dlopen(const char *path)
{
	if (!path) return main_image;
	char p[PATH_MAX];
	struct image *img = NULL;
	if (find_dylib(main_image, path, p) || (path[0] != '/' && path[0] != '@' &&
	    (snprintf(p, sizeof p, "%s", path), exists(p))))
		img = load_file(p);
	if (!img) { LOG("dlopen(%s): not found\n", path); return NULL; }
	macho_run_initializers(img);
	return img;
}

g32 macho_entry(struct image *img) { return img->entry; }

// Images whose static initializers we do not run yet (MAC32_SKIP_INIT, comma
// separated leaf names). Default: libcef.dylib, Chromium for the in-game web
// pages, whose initializers need a full 32-bit GNU libstdc++ (iostreams, COW
// strings) that mac32 does not have yet.
static int skip_init(struct image *img)
{
	const char *list = getenv("MAC32_SKIP_INIT");
	if (!list) list = "libcef.dylib";
	const char *leaf = strrchr(img->path, '/') + 1;
	size_t n = strlen(leaf);
	for (const char *p = list; (p = strstr(p, leaf)); p += n)
		if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
	return 0;
}

void macho_run_initializers(struct image *img)
{
	if (img->inited) return;
	img->inited = 1;
	if (skip_init(img)) {
		fprintf(stderr, "[mac32] not running static initializers of %s (MAC32_SKIP_INIT)\n", img->path);
		for (int i = 0; i < img->ndeps; i++)
			if (img->dep[i]) macho_run_initializers(img->dep[i]);
		return;
	}
	for (int i = 0; i < img->ndeps; i++)
		if (img->dep[i]) macho_run_initializers(img->dep[i]);
	if (img->routines) {
		LOG("init routine of %s\n", img->path);
		gcall(img->routines->init_address + (g32)img->slide, 0);
	}
	for (int si = 0; si < img->nseg; si++) {
		struct segment_command *sg = img->seg[si];
		struct section *sec = (void *)(sg + 1);
		for (uint32_t k = 0; k < sg->nsects; k++, sec++) {
			if ((sec->flags & SECTION_TYPE) != S_MOD_INIT_FUNC_POINTERS) continue;
			g32 *fp = G2H(sec->addr + (g32)img->slide);
			for (uint32_t j = 0; j < sec->size / 4; j++) {
				LOG("initializer %#x in %s\n", fp[j], img->path);
				gcall(fp[j], 4, (uint32_t)g_argc, g_argv, g_envp, g_apple);
			}
		}
	}
}
