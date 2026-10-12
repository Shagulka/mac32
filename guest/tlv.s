# Thread-local variables (__thread) for 32-bit guest code. Each variable has a
# descriptor {thunk, key, offset} in __thread_vars; compiled code loads the
# descriptor into %eax, calls the thunk, and gets the variable's address in
# %eax. Every other register must survive, the SSE registers included, so
# save what the host call may change. The loader points thunk here and sets
# key; mac32_tlv_addr (host) returns this thread's block for key, + offset.
	.text
	.globl __tlv_bootstrap
__tlv_bootstrap:
	pushl %ecx
	pushl %edx
	subl $136, %esp
	movdqu %xmm0, 0(%esp)
	movdqu %xmm1, 16(%esp)
	movdqu %xmm2, 32(%esp)
	movdqu %xmm3, 48(%esp)
	movdqu %xmm4, 64(%esp)
	movdqu %xmm5, 80(%esp)
	movdqu %xmm6, 96(%esp)
	movdqu %xmm7, 112(%esp)
	movl %eax, 128(%esp)
	pushl %eax
	call _mac32_tlv_addr
	addl $4, %esp
	movdqu 0(%esp), %xmm0
	movdqu 16(%esp), %xmm1
	movdqu 32(%esp), %xmm2
	movdqu 48(%esp), %xmm3
	movdqu 64(%esp), %xmm4
	movdqu 80(%esp), %xmm5
	movdqu 96(%esp), %xmm6
	movdqu 112(%esp), %xmm7
	addl $136, %esp
	popl %edx
	popl %ecx
	ret
