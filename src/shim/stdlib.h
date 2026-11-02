#ifndef SHIM_STDLIB_H
#define SHIM_STDLIB_H
#include <stddef.h>
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);
void abort(void);
void exit(int status);
void _Exit(int status);
int atexit(void (*fn)(void));
char *getenv(const char *name);
int system(const char *cmd);
int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
long long strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);
double strtod(const char *s, char **endptr);
double atof(const char *s);
int abs(int v);
long labs(long v);
typedef struct { int quot, rem; } div_t;
div_t div(int num, int denom);
void qsort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *));
int rand(void);
void srand(unsigned int seed);
#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif
