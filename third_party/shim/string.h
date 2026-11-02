/* Minimal freestanding <string.h> for the BearSSL port (and tls.c).
 * The host's 32-bit libc headers are unavailable in this build
 * environment; these declarations match the from-scratch
 * implementations in src/net.c and src/libc_shims.c. */
#ifndef SHIM_STRING_H
#define SHIM_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int v, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

#endif
