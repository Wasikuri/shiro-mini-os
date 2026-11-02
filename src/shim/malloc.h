#ifndef SHIM_MALLOC_H
#define SHIM_MALLOC_H
#include <stddef.h>
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *memalign(size_t alignment, size_t size);
size_t malloc_usable_size(void *ptr);
#endif
