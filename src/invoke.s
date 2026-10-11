# uint64_t host_invoke(void *fn, const uint64_t ints[6], const double fps[8],
#                      const uint64_t *stk, long nstk, double *xmm_out)
# xmm_out (if not NULL) gets xmm0 and xmm1: CGPoint/CGSize come back in both.
# Calls a SysV x86_64 function with the given register and stack arguments.
	.text
	.globl _host_invoke
	.p2align 4
_host_invoke:
	push %rbp
	mov  %rsp, %rbp
	push %rbx
	push %r12			# rsp is 16-aligned here
	mov  %rdi, %r10
	mov  %rsi, %r11
	mov  %r9, %r12
	test $1, %r8
	jz   1f
	sub  $8, %rsp			# keep 16-byte alignment at the call
1:	lea  -8(%rcx,%r8,8), %rbx
	mov  %r8, %rax
2:	test %rax, %rax
	jz   3f
	pushq (%rbx)
	sub  $8, %rbx
	dec  %rax
	jmp  2b
3:	movsd   (%rdx), %xmm0
	movsd  8(%rdx), %xmm1
	movsd 16(%rdx), %xmm2
	movsd 24(%rdx), %xmm3
	movsd 32(%rdx), %xmm4
	movsd 40(%rdx), %xmm5
	movsd 48(%rdx), %xmm6
	movsd 56(%rdx), %xmm7
	mov    (%r11), %rdi
	mov   8(%r11), %rsi
	mov  16(%r11), %rdx
	mov  24(%r11), %rcx
	mov  32(%r11), %r8
	mov  40(%r11), %r9
	mov  $8, %eax			# vector registers used, for varargs callees
	call *%r10
	test %r12, %r12
	jz   4f
	movsd %xmm0, (%r12)
	movsd %xmm1, 8(%r12)
4:	lea  -16(%rbp), %rsp
	pop  %r12
	pop  %rbx
	pop  %rbp
	ret
