#ifndef SHIM_ASSERT_H
#define SHIM_ASSERT_H
void shim_assert_fail(const char *expr, const char *file, int line);
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : shim_assert_fail(#x, __FILE__, __LINE__))
#endif
#endif
