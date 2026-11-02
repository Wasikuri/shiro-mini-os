/* Minimal freestanding <stdlib.h> shim for the BearSSL port.
 * BearSSL's core never calls these; declarations only, so stray
 * includes in its sources compile in our freestanding build. */
#ifndef SHIM_STDLIB_H
#define SHIM_STDLIB_H

#include <stddef.h>

void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void abort(void);
int atoi(const char *s);

#endif
