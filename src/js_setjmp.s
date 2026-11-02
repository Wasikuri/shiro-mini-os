/* Minimal i386 setjmp/longjmp for the JS engine shims.
 * jmp_buf layout (see src/shim/setjmp.h): 8 words;
 * [0]=ebx [1]=esi [2]=edi [3]=ebp [4]=esp [5]=eip [6..7]=spare.
 */
    .text
    .globl setjmp
    .type setjmp, @function
setjmp:
    movl 4(%esp), %eax        /* env */
    movl %ebx, 0(%eax)
    movl %esi, 4(%eax)
    movl %edi, 8(%eax)
    movl %ebp, 12(%eax)
    leal 4(%esp), %ecx
    movl %ecx, 16(%eax)       /* esp after the caller's ret */
    movl (%esp), %ecx
    movl %ecx, 20(%eax)       /* return address */
    xorl %eax, %eax
    ret

    .globl longjmp
    .type longjmp, @function
longjmp:
    movl 4(%esp), %edx        /* env */
    movl 8(%esp), %eax        /* val */
    testl %eax, %eax
    jne 1f
    incl %eax                 /* longjmp must not return 0 */
1:
    movl 0(%edx), %ebx
    movl 4(%edx), %esi
    movl 8(%edx), %edi
    movl 12(%edx), %ebp
    movl 16(%edx), %esp
    movl 20(%edx), %ecx
    jmp *%ecx
