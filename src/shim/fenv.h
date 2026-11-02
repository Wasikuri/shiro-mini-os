#ifndef SHIM_FENV_H
#define SHIM_FENV_H
/* Minimal <fenv.h>: x87 default rounding (nearest) is all the kernel
 * ever uses; the functions are provided in js_shims.c as stubs. */
typedef struct { unsigned short control, status; } fenv_t;
typedef unsigned short fexcept_t;
#define FE_TONEAREST  0
#define FE_DOWNWARD   1
#define FE_UPWARD     2
#define FE_TOWARDZERO 3
#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT 0x3d
int fegetround(void);
int fesetround(int round);
int feclearexcept(int excepts);
int fetestexcept(int excepts);
int fegetenv(fenv_t *env);
int fesetenv(const fenv_t *env);
#endif
