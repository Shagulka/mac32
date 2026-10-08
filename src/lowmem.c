// Low memory: the guest's whole world is [LOW_START, LOW_END). We reserve it
// at startup so nothing else in the process lands there, then hand out pages
// and malloc blocks from it.
#include "mac32.h"
#include <sys/mman.h>
#include <string.h>
#include <os/lock.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

static g32 page_next = 0x00100000;	// below this: fixed-address executables
static os_unfair_lock page_lock = OS_UNFAIR_LOCK_INIT;

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
	len = (len + 0xfff) & ~0xfffu;
	os_unfair_lock_lock(&page_lock);
	g32 a = page_next;
	if ((uint64_t)a + len + 0x1000 > LOW_END)
		die("out of low memory (%u bytes asked)", len);
	page_next += len + 0x1000;	// one guard page between blocks
	os_unfair_lock_unlock(&page_lock);
	low_commit(a, len, prot);
	return a;
}

// Guest heap. Size classes of 16 bytes up to 2 KB come from 64 KB slabs;
// anything bigger gets its own pages. Each block has a 16-byte header so the
// payload stays 16-byte aligned, like i386 Darwin malloc.
struct hdr {
	uint32_t size;		// usable size
	uint32_t cls;		// size class, or 0xffffffff for page blocks
	uint32_t magic;
	uint32_t pad;
};
#define MAGIC 0x6d616c6c
#define NCLS 128		// 16 .. 2048
static g32 freelist[NCLS + 1];
static os_unfair_lock heap_lock = OS_UNFAIR_LOCK_INIT;
static g32 slab_cur, slab_end;

static struct hdr *H(g32 p) { return (struct hdr *)G2H(p - 16); }

g32 gmalloc(uint32_t n)
{
	if (n == 0) n = 1;
	if (n > 2048) {
		g32 b = low_pages(n + 16, PROT_READ | PROT_WRITE);
		struct hdr *h = G2H(b);
		h->size = ((n + 16 + 0xfff) & ~0xfffu) - 16;
		h->cls = 0xffffffff;
		h->magic = MAGIC;
		return b + 16;
	}
	uint32_t cls = (n + 15) / 16;
	uint32_t bsz = cls * 16 + 16;
	os_unfair_lock_lock(&heap_lock);
	g32 p = freelist[cls];
	if (p) {
		freelist[cls] = *(g32 *)G2H(p);
	} else {
		if (slab_cur + bsz > slab_end) {
			slab_cur = low_pages(0x10000, PROT_READ | PROT_WRITE);
			slab_end = slab_cur + 0x10000;
		}
		p = slab_cur + 16;
		slab_cur += bsz;
	}
	os_unfair_lock_unlock(&heap_lock);
	struct hdr *h = H(p);
	h->size = cls * 16;
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
	if (h->cls == 0xffffffff) {
		uint32_t len = h->size + 16;
		h->magic = 0;
		mmap(G2H(p - 16), len, PROT_NONE,
		     MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
		return;		// address space is not reused yet
	}
	if (h->cls == 0xfffffffe) return;	// gmemalign block: leaked
	h->magic = 0;
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
	// page blocks are page aligned + 16; over-allocate and leak the slack
	g32 b = low_pages(n + align + 16, PROT_READ | PROT_WRITE);
	g32 p = (b + 16 + align - 1) & ~(align - 1);
	struct hdr *h = H(p);
	h->size = n;
	h->cls = 0xfffffffe;	// never freed for now
	h->magic = MAGIC;
	return p;
}

g32 gstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	g32 p = gmalloc((uint32_t)n);
	memcpy(G2H(p), s, n);
	return p;
}
