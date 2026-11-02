#ifndef SHIM_SETJMP_H
#define SHIM_SETJMP_H
/* i386 jmp_buf: ebx, esi, edi, ebp, esp, eip + 2 spare words */
typedef struct { unsigned int regs[8]; } jmp_buf[1];
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
#endif
