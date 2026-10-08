# Reserve the guest's address range [0x1000, 0xf0000000) in the executable's
# own layout, so the kernel sets it aside at exec time before dyld, Rosetta or
# malloc can put anything there. Mapped with no access; low_commit() replaces
# pieces of it with real memory.
	.globl _low_reserve
	.zerofill __RESERVE,__low,_low_reserve,0xeffff000,12
