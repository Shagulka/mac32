// Can an x86_64 process under Rosetta 2 run 32-bit i386 code?
// Maps low memory, copies the trampoline and a 32-bit function there,
// switches to 32-bit mode, calls it, and switches back.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <i386/user_ldt.h>
#include <architecture/i386/table.h>

extern char tramp_start[], tramp_end[], tramp_call32[], tramp_cs32[];

// i386: int f(int a) { return a*3 + 7; }
//   mov 4(%esp),%eax ; lea 7(%eax,%eax,2),%eax ; ret
static const uint8_t f32[] = { 0x8b,0x44,0x24,0x04, 0x8d,0x44,0x40,0x07, 0xc3 };
static const uint8_t crc32[] = {
#include "crc.inc"
};

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	uint8_t *low = mmap((void *)0x10000000, 0x100000, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (low == MAP_FAILED) { perror("mmap low"); return 1; }
	printf("low region at %p\n", low);

	size_t tlen = tramp_end - tramp_start;
	memcpy(low, tramp_start, tlen);
	memcpy(low + 0x1000, f32, sizeof f32);

	uint16_t cs = 0x1b;
	if (argc > 1 && !strcmp(argv[1], "ldt")) {
		union ldt_entry d;
		memset(&d, 0, sizeof d);
		d.code.limit00 = 0xffff; d.code.limit16 = 0xf;
		d.code.type = DESC_CODE_READ; d.code.dpl = 3; d.code.present = 1;
		d.code.opsz = 1; d.code.granular = 1;
		int sel = -1;
		for (int idx = 16; idx < 8192 && sel < 0; idx++)
			sel = i386_set_ldt(idx, &d, 1);
		if (sel < 0) { perror("i386_set_ldt"); return 1; }
		cs = (uint16_t)((sel << 3) | 7);
		printf("LDT code selector 0x%x\n", cs);
	}
	*(uint16_t *)(low + (tramp_cs32 - tramp_start)) = cs;

	if (mprotect(low, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC)) { perror("mprotect"); return 1; }

	// low stack: top at low+0x80000, push the one argument
	uint32_t esp = 0x10080000 - 4;
	*(uint32_t *)(uintptr_t)esp = 5;

	uint32_t (*call32)(uint32_t, uint32_t) =
		(void *)(low + (tramp_call32 - tramp_start));
	printf("calling 32-bit code, tramp=%p\n", (void *)call32);
	uint32_t r = call32(0x10001000, esp);
	printf("f32(5) = %u (want 22)\n", r);

	// clang-built i386 CRC32 over "123456789" in low memory
	memcpy(low + 0x2000, crc32, sizeof crc32);
	memcpy(low + 0x3000, "123456789", 9);
	mprotect(low + 0x2000, 0x1000, PROT_READ | PROT_EXEC);
	uint32_t *sp = (uint32_t *)(uintptr_t)(0x10080000 - 8);
	sp[0] = 0x10003000; sp[1] = 9;
	uint32_t c = call32(0x10002000, (uint32_t)(uintptr_t)sp);
	printf("crc32_i386(\"123456789\") = 0x%08x (want 0xcbf43926)\n", c);
	return (r == 22 && c == 0xcbf43926) ? 0 : 2;
}
