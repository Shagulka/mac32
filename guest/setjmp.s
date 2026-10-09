# setjmp/longjmp for 32-bit guest code. jmp_buf: ebx esi edi ebp esp eip.
# The signal mask is not saved (signals are not delivered to the guest yet).
	.text
	.globl _setjmp, __setjmp, _sigsetjmp, _longjmp, __longjmp, _siglongjmp
_setjmp:
__setjmp:
_sigsetjmp:
	movl 4(%esp), %ecx
	movl %ebx, 0(%ecx)
	movl %esi, 4(%ecx)
	movl %edi, 8(%ecx)
	movl %ebp, 12(%ecx)
	leal 4(%esp), %edx
	movl %edx, 16(%ecx)		# esp after our return
	movl (%esp), %edx
	movl %edx, 20(%ecx)		# return address
	xorl %eax, %eax
	ret

_longjmp:
__longjmp:
_siglongjmp:
	movl 4(%esp), %ecx
	movl 8(%esp), %eax
	testl %eax, %eax
	jnz  1f
	incl %eax
1:	movl 0(%ecx), %ebx
	movl 4(%ecx), %esi
	movl 8(%ecx), %edi
	movl 12(%ecx), %ebp
	movl 16(%ecx), %esp
	jmp  *20(%ecx)
