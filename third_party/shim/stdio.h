/* Minimal freestanding <stdio.h> shim for the BearSSL port:
 * declarations only; BearSSL's core does no stdio I/O here. */
#ifndef SHIM_STDIO_H
#define SHIM_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct SHIM_FILE SHIM_FILE;
extern SHIM_FILE *stdout;
extern SHIM_FILE *stderr;

int printf(const char *fmt, ...);
int fprintf(SHIM_FILE *f, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

#endif
