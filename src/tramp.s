# Mode switches between 64-bit host code and 32-bit guest code.
# Everything from _t_start to _t_end is copied into a page below 4 GB, so
# addresses the 32-bit side uses fit in 32 bits. Position-independent only.
#
# Rosetta facts (see MODLOG.md): enter 32-bit mode with iretq to an LDT code
# selector; leave with a far return to 0x2b. ljmp from 64-bit mode does not
# switch modes.

	.text
	.globl _t_start, _t_end, _t_call32, _t_hostcall32
	.globl _t_cs32, _t_ds32, _t_tsd_off, _t_dispatch
	.p2align 4
_t_start:

# uint64_t t_call32(g32 eip, g32 esp, double *st0_out)
#   esp: 16-byte aligned start of the cdecl argument block, already filled.
#   Returns edx:eax. If st0_out, pops st(0) into it.
#   The host rsp is kept in a pthread TSD slot (%gs:off) so calls can nest.
_t_call32:
	push %rbp
	mov  %rsp, %rbp
	push %rbx
	push %r12
	push %r13
	push %r14
	push %r15
	push %rdx			# st0_out
	mov  _t_tsd_off(%rip), %rax
	pushq %gs:(%rax)		# outer call's saved rsp
	mov  %rsp, %gs:(%rax)

	sub  $4, %esi
	lea  back32(%rip), %rax
	movl %eax, (%rsi)		# 32-bit return address

	movzwl _t_ds32(%rip), %eax
	mov  %ax, %ds
	mov  %ax, %es
	pushq %rax			# ss
	pushq %rsi			# esp
	pushfq
	movzwl _t_cs32(%rip), %eax
	pushq %rax			# cs
	pushq %rdi			# eip
	iretq

	.code32
back32:
	call 1f
1:	pop  %ecx
	lea  (back64 - 1b)(%ecx), %ecx
	pushl $0x2b
	pushl %ecx
	lret

	.code64
back64:
	mov  _t_tsd_off(%rip), %rcx
	mov  %gs:(%rcx), %rsp
	popq %gs:(%rcx)
	pop  %rcx			# st0_out
	test %rcx, %rcx
	jz   2f
	fstpl (%rcx)
2:	shl  $32, %rdx
	mov  %eax, %eax
	or   %rdx, %rax
	pop  %r15
	pop  %r14
	pop  %r13
	pop  %r12
	pop  %rbx
	pop  %rbp
	ret

# Guest -> host. Import stubs are "push $idx; jmp _t_hostcall32".
# Guest stack on entry: [idx][ret][args...]
	.code32
_t_hostcall32:
	push %ebp
	push %esi
	push %edi
	push %ebx			# [ebx][edi][esi][ebp][idx][ret][args]
	call 1f
1:	pop  %ecx
	lea  (hostcall64 - 1b)(%ecx), %ecx
	pushl $0x2b
	pushl %ecx
	lret

	.code64
hostcall64:
	mov  %esp, %esp			# clear the upper half
	mov  %rsp, %rbx
	and  $-16, %rsp
	sub  $32, %rsp			# struct hres
	movq $0, (%rsp)
	movq $0, 8(%rsp)
	movq $0, 16(%rsp)
	mov  16(%rbx), %edi		# idx
	lea  24(%rbx), %rsi		# args
	mov  %rsp, %rdx
	call *_t_dispatch(%rip)
	mov  16(%rsp), %ecx		# flags
	test $1, %ecx
	jz   3f
	fldl 8(%rsp)			# x87 stack is shared by both modes
3:	mov  (%rsp), %rax
	mov  %rax, %rdx
	shr  $32, %rdx
	lea  hostret32(%rip), %r8
	lea  hostret32_pop4(%rip), %r9
	test $2, %ecx
	cmovnz %r9, %r8
	mov  %rbx, %rsp
	movzwl _t_ds32(%rip), %ecx
	pushq %rcx			# ss
	pushq %rbx			# esp
	pushfq
	movzwl _t_cs32(%rip), %ecx
	pushq %rcx			# cs
	pushq %r8			# eip
	iretq

	.code32
hostret32:
	pop  %ebx
	pop  %edi
	pop  %esi
	pop  %ebp
	add  $4, %esp			# idx
	ret
hostret32_pop4:
	pop  %ebx
	pop  %edi
	pop  %esi
	pop  %ebp
	add  $4, %esp
	ret  $4

	.code64
	.p2align 3
_t_dispatch:	.quad 0
_t_tsd_off:	.quad 0
_t_cs32:	.word 0
_t_ds32:	.word 0x23
_t_end:
