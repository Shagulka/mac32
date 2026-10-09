// Low memory: the guest's whole world is [LOW_START, LOW_END). We reserve it
// at startup so nothing else in the process lands there, then hand out pages
// and malloc blocks from it.
#include "mac32.h"
#include <sys/mman.h>
#include <string.h>
#include <os/lock.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

// Old executables are linked at a fixed address from 0x1000 up (OpenTTD's
// text alone is 9 MB), so our own pages start at EXE_ZONE; the space between
// the program and EXE_ZONE is handed to the allocator once the program is in.
#define EXE_ZONE 0x10000000u
static g32 page_next = EXE_ZONE;
static os_unfair_lock page_lock = OS_UNFAIR_LOCK_INIT;

// Freed page ranges, sorted by address and merged, so address space is reused.
// Without this a long session (Source loads and frees textures and sounds all
// the time) walks off the end of the 4 GB space.
struct ext { g32 a; uint32_t len; };
static struct ext *fr;
static int nfr, capfr;

// live page blocks: start -> length (guard page included), open addressing;
// a freed slot keeps its address with length 0 so later probes still reach on
struct lslot { g32 a; uint32_t len; };
static struct lslot *live;
static uint32_t live_cap, live_used, live_n;
static uint64_t live_bytes;

static void live_put(g32 a, uint32_t len);
static void live_grow(void)
{
	struct lslot *old = live;
	uint32_t oc = live_cap;
	live_cap = oc ? (live_n * 4 > oc ? oc * 2 : oc) : 4096;
	live = calloc(live_cap, sizeof *live);
	live_used = live_n = 0;
	live_bytes = 0;
	for (uint32_t i = 0; i < oc; i++)
		if (old[i].len) live_put(old[i].a, old[i].len);
	free(old);
}
static void live_put(g32 a, uint32_t len)
{
	if ((live_used + 1) * 2 > live_cap) live_grow();
	uint32_t i = (a >> 12) * 2654435761u & (live_cap - 1);
	while (live[i].len) i = (i + 1) & (live_cap - 1);	// a is never live twice
	if (!live[i].a) live_used++;
	live[i] = (struct lslot){ a, len };
	live_n++;
	live_bytes += len;
}
static uint32_t live_take(g32 a)
{
	if (!live_cap) return 0;
	uint32_t i = (a >> 12) * 2654435761u & (live_cap - 1);
	for (; live[i].a; i = (i + 1) & (live_cap - 1))
		if (live[i].a == a && live[i].len) {
			uint32_t len = live[i].len;
			live[i].len = 0;	// tombstone: keeps later probes reachable
			live_n--;
			live_bytes -= len;
			return len;
		}
	return 0;
}

static void ext_insert(g32 a, uint32_t len)
{
	int lo = 0, hi = nfr;
	while (lo < hi) { int m = (lo + hi) / 2; if (fr[m].a < a) lo = m + 1; else hi = m; }
	int prev = lo > 0 && fr[lo - 1].a + fr[lo - 1].len == a;
	int next = lo < nfr && a + len == fr[lo].a;
	if (prev && next) {
		fr[lo - 1].len += len + fr[lo].len;
		memmove(&fr[lo], &fr[lo + 1], sizeof *fr * (size_t)(nfr - lo - 1));
		nfr--;
	} else if (prev) fr[lo - 1].len += len;
	else if (next) { fr[lo].a = a; fr[lo].len += len; }
	else {
		if (nfr == capfr) { capfr = capfr ? capfr * 2 : 1024; fr = realloc(fr, sizeof *fr * (size_t)capfr); }
		memmove(&fr[lo + 1], &fr[lo], sizeof *fr * (size_t)(nfr - lo));
		fr[lo] = (struct ext){ a, len };
		nfr++;
	}
	// the top range goes back to the bump pointer
	if (nfr && fr[nfr - 1].a + fr[nfr - 1].len == page_next) { page_next = fr[nfr - 1].a; nfr--; }
}

static void low_stats(const char *why)
{
	uint64_t freeb = 0, big = 0;
	for (int i = 0; i < nfr; i++) { freeb += fr[i].len; if (fr[i].len > big) big = fr[i].len; }
	fprintf(stderr, "[mac32] low memory %s: top %#x, %u blocks live (%llu MB), %d free ranges (%llu MB, largest %llu KB)\n",
		why, page_next, live_n, live_bytes >> 20, nfr, freeb >> 20, big >> 10);
}

// MAC32_MEMSTATS=<seconds>: log address space use that often
static void *stats_loop(void *arg)
{
	unsigned secs = (unsigned)(uintptr_t)arg;
	for (;;) {
		sleep(secs);
		os_unfair_lock_lock(&page_lock);
		low_stats("now");
		os_unfair_lock_unlock(&page_lock);
	}
	return NULL;
}

// reserve.s puts a __RESERVE segment over [LOW_START, LOW_END); check it is there
void low_init(void)
{
	// the kernel maps it as a run of 128 MB entries; check they are contiguous
	mach_vm_address_t want = LOW_START;
	while (want < LOW_END) {
		mach_vm_address_t a = want;
		mach_vm_size_t sz = 0;
		vm_region_basic_info_data_64_t info;
		mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t obj;
		if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
				   (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS ||
		    a != want || info.protection != VM_PROT_NONE)
			die("low memory is not reserved at %#llx (next region %#llx+%#llx)", want, a, sz);
		want = a + sz;
	}
	const char *ms = getenv("MAC32_MEMSTATS");
	if (ms && atoi(ms) > 0) {
		pthread_t t;
		pthread_create(&t, NULL, stats_loop, (void *)(uintptr_t)atoi(ms));
		pthread_detach(t);
	}
}

void low_commit(g32 addr, uint32_t len, int prot)
{
	g32 a = addr & ~0xfffu;
	uint32_t l = ((addr + len + 0xfff) & ~0xfffu) - a;
	void *p = mmap(G2H(a), l, prot, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (p == MAP_FAILED)
		die("low_commit %#x+%#x failed", addr, len);
}

g32 low_pages(uint32_t len, int prot)
{
	uint32_t total = ((len + 0xfff) & ~0xfffu) + 0x1000;	// one guard page after each block
	os_unfair_lock_lock(&page_lock);
	g32 a = 0;
	for (int i = 0; i < nfr; i++)			// first fit, lowest address
		if (fr[i].len >= total) {
			a = fr[i].a;
			fr[i].a += total;
			fr[i].len -= total;
			if (!fr[i].len) { memmove(&fr[i], &fr[i + 1], sizeof *fr * (size_t)(nfr - i - 1)); nfr--; }
			break;
		}
	if (!a) {
		if ((uint64_t)page_next + total > LOW_END) {
			low_stats("exhausted");
			die("out of low memory (%u bytes asked)", len);
		}
		a = page_next;
		page_next += total;
		static g32 mark = 0x10000000;		// say when the top passes each 256 MB
		if (page_next > mark && getenv("MAC32_MEMSTATS")) { low_stats("grew"); mark += 0x10000000; }
	}
	live_put(a, total);
	os_unfair_lock_unlock(&page_lock);
	low_commit(a, total - 0x1000, prot);
	return a;
}

// the main program is mapped (hi: its end, 0 if it went into low_pages):
// the rest of the space below EXE_ZONE becomes free for blocks
void low_exe_placed(g32 hi)
{
	static int done;
	if (done) return;
	done = 1;
	if (hi > EXE_ZONE) die("the program ends at %#x, above %#x where mac32's own memory starts", hi, EXE_ZONE);
	g32 from = (hi + 0xfffu) & ~0xfffu;
	if (from < 0x00100000) from = 0x00100000;
	os_unfair_lock_lock(&page_lock);
	ext_insert(from, EXE_ZONE - from);
	os_unfair_lock_unlock(&page_lock);
}

// for crash reports: what the allocator knows about a guest address
// (no locks: the process is going down anyway)
void low_describe(g32 a)
{
	for (uint32_t i = 0; i < live_cap; i++)
		if (live[i].len && a >= live[i].a && a - live[i].a < live[i].len) {
			fprintf(stderr, "  %#x is in a live block %#x+%#x%s\n", a, live[i].a, live[i].len,
				a - live[i].a >= live[i].len - 0x1000 ? " (its guard page: overrun)" : "");
			return;
		}
	for (int i = 0; i < nfr; i++)
		if (a >= fr[i].a && a - fr[i].a < fr[i].len) {
			fprintf(stderr, "  %#x is in a freed range %#x+%#x (use after free)\n", a, fr[i].a, fr[i].len);
			return;
		}
	g32 below = 0, blen = 0;
	for (uint32_t i = 0; i < live_cap; i++)
		if (live[i].len && live[i].a <= a && live[i].a > below) { below = live[i].a; blen = live[i].len; }
	fprintf(stderr, "  %#x is %s (top %#x; nearest block below: %#x+%#x)\n", a,
		a >= page_next ? "above every block mac32 made" : "not a mac32 block (program image?)", page_next, below, blen);
}

// give a low_pages block back; returns 0 if a was not one
int low_free(g32 a)
{
	os_unfair_lock_lock(&page_lock);
	uint32_t total = live_take(a);
	os_unfair_lock_unlock(&page_lock);
	if (!total) return 0;
	// drop the memory but keep the range reserved, so nothing else lands there
	mmap(G2H(a), total, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
	os_unfair_lock_lock(&page_lock);
	ext_insert(a, total);
	os_unfair_lock_unlock(&page_lock);
	return 1;
}

// length a low_pages block was made with (guard page excluded), 0 if unknown
uint32_t low_block_len(g32 a)
{
	os_unfair_lock_lock(&page_lock);
	uint32_t len = 0;
	if (live_cap) {
		uint32_t i = (a >> 12) * 2654435761u & (live_cap - 1);
		for (; live[i].a; i = (i + 1) & (live_cap - 1))
			if (live[i].a == a && live[i].len) { len = live[i].len - 0x1000; break; }
	}
	os_unfair_lock_unlock(&page_lock);
	return len;
}

// Guest heap. Size classes of 16 bytes up to 2 KB come from 64 KB slabs,
// classes a quarter power of two apart up to 32 KB from 512 KB slabs;
// anything bigger gets its own pages. Each block has a 16-byte header so the
// payload stays 16-byte aligned, like i386 Darwin malloc.
struct hdr {
	uint32_t size;		// usable size
	uint32_t cls;		// size class; PAGE_BLOCK or ALIGN_BLOCK otherwise
	uint32_t magic;
	uint32_t pad;		// ALIGN_BLOCK: start of its low_pages block
};
#define MAGIC 0x6d616c6c
#define PAGE_BLOCK 0xffffffffu
#define ALIGN_BLOCK 0xfffffffeu
#define NSMALL 128		// 16 .. 2048 step 16
#define NMED 16			// 2560 .. 32768
#define NCLS (NSMALL + NMED)
#define MED_MAX 32768
static const uint32_t med_size[NMED] = {
	2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192,
	10240, 12288, 14336, 16384, 20480, 24576, 28672, 32768,
};
static g32 freelist[NCLS + 1];
static os_unfair_lock heap_lock = OS_UNFAIR_LOCK_INIT;
static g32 slab_cur, slab_end, mslab_cur, mslab_end;

static uint32_t class_of(uint32_t n)
{
	if (n <= 2048) return (n + 15) / 16;
	uint32_t c = 0;
	while (med_size[c] < n) c++;
	return NSMALL + 1 + c;
}
static uint32_t class_size(uint32_t c) { return c <= NSMALL ? c * 16 : med_size[c - NSMALL - 1]; }

static struct hdr *H(g32 p) { return (struct hdr *)G2H(p - 16); }

// MAC32_HEAPCHECK=1: poison freed blocks and verify the poison on reuse, so a
// write after free shows up with its address instead of a corrupt free list
static int heapcheck = -1;
static int checking(void)
{
	if (heapcheck < 0) heapcheck = getenv("MAC32_HEAPCHECK") != NULL;
	return heapcheck;
}
static void verify_poison(g32 p, uint32_t size)
{
	uint8_t *b = G2H(p);
	for (uint32_t i = 4; i < size; i++)
		if (b[i] != 0xdd) {
			g32 off;
			const char *who = macho_addr_name(*(g32 *)G2H(p - 8), &off);	// freed-by return address
			die("heap: block %#x (%u bytes) written after free at +%u (byte %#x); freed from %s+%#x",
			    p, size, i, b[i], who ? who : "?", off);
		}
}

g32 gmalloc(uint32_t n)
{
	if (n == 0) n = 1;
	if (n > MED_MAX) {
		if (n > 0xffffffffu - 0x2000) return 0;
		g32 b = low_pages(n + 16, PROT_READ | PROT_WRITE);
		struct hdr *h = G2H(b);
		h->size = ((n + 16 + 0xfff) & ~0xfffu) - 16;
		h->cls = PAGE_BLOCK;
		h->magic = MAGIC;
		return b + 16;
	}
	uint32_t cls = class_of(n), csz = class_size(cls), bsz = csz + 16;
	os_unfair_lock_lock(&heap_lock);
	g32 p = freelist[cls];
	if (p) {
		if (checking()) verify_poison(p, csz);
		freelist[cls] = *(g32 *)G2H(p);
	} else if (cls <= NSMALL) {
		if (slab_cur + bsz > slab_end) {
			slab_cur = low_pages(0x10000, PROT_READ | PROT_WRITE);
			slab_end = slab_cur + 0x10000;
		}
		p = slab_cur + 16;
		slab_cur += bsz;
	} else {
		if (mslab_cur + bsz > mslab_end) {
			mslab_cur = low_pages(0x80000, PROT_READ | PROT_WRITE);
			mslab_end = mslab_cur + 0x80000;
		}
		p = mslab_cur + 16;
		mslab_cur += bsz;
	}
	os_unfair_lock_unlock(&heap_lock);
	struct hdr *h = H(p);
	h->size = csz;
	h->cls = cls;
	h->magic = MAGIC;
	return p;
}

void gfree(g32 p)
{
	if (!p) return;
	struct hdr *h = H(p);
	if (h->magic != MAGIC)
		die("gfree: bad pointer %#x", p);
	h->magic = 0;
	if (h->cls == PAGE_BLOCK) { low_free(p - 16); return; }
	if (h->cls == ALIGN_BLOCK) { low_free(h->pad); return; }
	if (checking()) memset(G2H(p), 0xdd, h->size);
	os_unfair_lock_lock(&heap_lock);
	*(g32 *)G2H(p) = freelist[h->cls];
	freelist[h->cls] = p;
	os_unfair_lock_unlock(&heap_lock);
}

uint32_t gmalloc_size(g32 p)
{
	if (!p) return 0;
	struct hdr *h = H(p);
	return h->magic == MAGIC ? h->size : 0;
}

g32 gcalloc(uint32_t n, uint32_t sz)
{
	uint64_t t = (uint64_t)n * sz;
	if (t > 0xffffffffu) return 0;
	g32 p = gmalloc((uint32_t)t);
	memset(G2H(p), 0, t);
	return p;
}

g32 grealloc(g32 p, uint32_t n)
{
	if (!p) return gmalloc(n);
	uint32_t old = gmalloc_size(p);
	if (n <= old) return p;
	g32 q = gmalloc(n);
	memcpy(G2H(q), G2H(p), old);
	gfree(p);
	return q;
}

g32 gmemalign(uint32_t align, uint32_t n)
{
	if (align <= 16) return gmalloc(n);
	// page blocks are page aligned; over-allocate and remember the block start
	g32 b = low_pages(n + align + 16, PROT_READ | PROT_WRITE);
	g32 p = (b + 16 + align - 1) & ~(align - 1);
	struct hdr *h = H(p);
	h->size = n;
	h->cls = ALIGN_BLOCK;
	h->magic = MAGIC;
	h->pad = b;
	return p;
}

g32 gstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	g32 p = gmalloc((uint32_t)n);
	memcpy(G2H(p), s, n);
	return p;
}
