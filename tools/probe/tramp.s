# Mode-switch trampoline. Copied byte-for-byte into memory below 4 GB,
# so every address the 32-bit side sees fits in 32 bits. Uses only
# position-independent references.
#
# uint32_t tramp_call32(uint32_t eip, uint32_t esp)  (SysV x86_64)
#   eip: 32-bit target, called cdecl with whatever the caller already
#        pushed at esp (args); returns its eax.

	.text
	.globl _tramp_start, _tramp_end, _tramp_call32, _tramp_cs32, _tramp_ds32
_tramp_start:

_tramp_call32:
	push %rbx
	push %rbp
	push %r12
	push %r13
	push %r14
	push %r15
	mov  %rsp, saved_rsp(%rip)
	mov  %ss, %ax
	mov  %ax, saved_ss(%rip)

	# push the 32-bit return address (back32) onto the low stack
	lea  back32(%rip), %rax
	sub  $4, %esi
	movl %eax, (%rsi)

	movzwl _tramp_ds32(%rip), %eax
	mov  %ax, %ds
	mov  %ax, %es

	# iretq frame: ss, rsp, rflags, cs, rip
	pushq %rax
	pushq %rsi
	pushfq
	movzwl _tramp_cs32(%rip), %eax
	pushq %rax
	pushq %rdi
	iretq

	.code32
back32:
	# eax = result. Far-return to 64-bit code at back64.
	call 1f
1:	pop  %ecx
	lea  (back64 - 1b)(%ecx), %ecx
	pushl $0x2b
	pushl %ecx
	lret

	.code64
back64:
	mov  saved_ss(%rip), %cx
	mov  %cx, %ss
	xor  %ecx, %ecx
	mov  %cx, %ds
	mov  %cx, %es
	mov  saved_rsp(%rip), %rsp
	pop  %r15
	pop  %r14
	pop  %r13
	pop  %r12
	pop  %rbp
	pop  %rbx
	ret

	.p2align 3
saved_rsp:	.quad 0
saved_ss:	.word 0
	.p2align 2
far_eip:	.long 0
far_cs:		.word 0
_tramp_cs32:	.word 0x1b
_tramp_ds32:	.word 0x23
_tramp_end:
