/* js_shims.c - minimal libc/libm surface for the embedded JS engines
 * (Duktape / QuickJS) in the Shiro Mini OS kernel.
 *
 * Written from scratch for this spike. Everything here is backed by
 * the kernel heap (kmalloc/kfree), the console (print) and the CMOS
 * RTC. It is compiled as a normal kernel file (no shim headers); the
 * matching prototypes live in src/shim/*.h, which the engines see.
 *
 * Written for browser milestone 1 in the simple-os-browser tree and
 * merged into the main tree for v0.7.
 */
#include "os.h"
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

int errno;

/* Forward declarations (these names are compiler builtins; declaring
 * them up front keeps the definitions below from conflicting with
 * implicit declarations at call sites). */
long strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
long long strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);
double strtod(const char *s, char **endptr);

/* memcpy/memset/memmove come from net.c. */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int v, size_t n);
void *memmove(void *dst, const void *src, size_t n);

/* ================================================================
 * Allocator: wraps kmalloc with a size header so realloc works.
 * ================================================================ */
struct alloc_hdr { uint32_t magic; uint32_t size; };
#define ALLOC_MAGIC 0xA110C8EDu

void *malloc(size_t n) {
    if (n == 0) n = 1;
    struct alloc_hdr *h = kmalloc(n + sizeof(struct alloc_hdr));
    if (!h) return NULL;
    h->magic = ALLOC_MAGIC;
    h->size = (uint32_t)n;
    return (char *)h + sizeof(struct alloc_hdr);
}

void free(void *p) {
    if (!p) return;
    struct alloc_hdr *h = (struct alloc_hdr *)((char *)p - sizeof(struct alloc_hdr));
    if (h->magic != ALLOC_MAGIC) return; /* foreign pointer: ignore */
    h->magic = 0;
    kfree(h);
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    struct alloc_hdr *h = (struct alloc_hdr *)((char *)p - sizeof(struct alloc_hdr));
    if (h->magic != ALLOC_MAGIC) return NULL;
    if (h->size >= n) return p;
    void *np = malloc(n);
    if (!np) return NULL;
    memcpy(np, p, h->size);
    free(p);
    return np;
}

void *calloc(size_t nmemb, size_t size) {
    size_t n = nmemb * size;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

/* ================================================================
 * String / memory functions not already in the kernel.
 * (memcmp/strlen/strcmp/strncmp live in src/libc_shims.c in the
 * main tree — v0.7 merge note; they are NOT duplicated here.)
 * ================================================================ */
void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (uint8_t)c) return (void *)(p + i);
    return NULL;
}
size_t strnlen(const char *s, size_t max) {
    size_t n = 0; while (n < max && s[n]) n++; return n;
}
char *strcpy(char *dst, const char *src) {
    char *d = dst; while ((*d++ = *src++)) {} return dst;
}
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}
char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst); while ((*d++ = *src++)) {} return dst;
}
char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = 0;
    return dst;
}
static int lower_c(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int strcasecmp(const char *a, const char *b) {
    while (*a && lower_c((uint8_t)*a) == lower_c((uint8_t)*b)) { a++; b++; }
    return lower_c((uint8_t)*a) - lower_c((uint8_t)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int d = lower_c((uint8_t)a[i]) - lower_c((uint8_t)b[i]);
        if (d || a[i] == 0) return d;
    }
    return 0;
}
char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (*s == 0) return NULL;
    }
}
char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (*s == 0) return (char *)last;
    }
}
char *strstr(const char *hay, const char *needle) {
    if (*needle == 0) return (char *)hay;
    size_t nl = strlen(needle);
    for (; *hay; hay++)
        if (strncmp(hay, needle, nl) == 0) return (char *)hay;
    return NULL;
}
size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}
char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) if (strchr(accept, *s)) return (char *)s;
    return NULL;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
char *strerror(int e) { (void)e; return "error"; }

/* ================================================================
 * ctype
 * ================================================================ */
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isprint(c) && !isalnum(c) && c != ' '; }
int isblank(int c) { return c == ' ' || c == '\t'; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }

/* ================================================================
 * stdlib bits
 * ================================================================ */
void shim_hang(const char *what) {
    print("\n[js-shim] fatal: ");
    print(what);
    print("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
void abort(void) { shim_hang("abort() called"); }
void exit(int status) { (void)status; shim_hang("exit() called"); }
void _Exit(int status) { exit(status); }
int atexit(void (*fn)(void)) { (void)fn; return 0; }
char *getenv(const char *name) { (void)name; return NULL; }
int system(const char *cmd) { (void)cmd; return -1; }
void shim_assert_fail(const char *expr, const char *file, int line) {
    print("\n[js-shim] assert failed: ");
    print(expr);
    print(" at ");
    print(file);
    print(":");
    print_dec((uint32_t)line);
    print("\n");
    shim_hang("assert");
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
double atof(const char *s) { return strtod(s, NULL); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

static const char *skip_ws_digits(const char *s, int *neg) {
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    *neg = 0;
    if (*s == '-') { *neg = 1; s++; }
    else if (*s == '+') s++;
    return s;
}
static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}
unsigned long long strtoull(const char *s, char **endptr, int base) {
    int neg;
    s = skip_ws_digits(s, &neg);
    if (base == 0) {
        base = 10;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') base = 8;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned long long v = 0;
    int any = 0;
    for (;;) {
        int d = digit_val((uint8_t)*s);
        if (d < 0 || d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
        s++; any = 1;
    }
    if (endptr) *endptr = (char *)(any ? s : s - (size_t)0);
    if (!any && endptr) {
        /* no conversion: endptr = original nptr; caller saved it */
    }
    return neg ? 0ull - v : v;
}
long long strtoll(const char *s, char **endptr, int base) {
    int neg;
    const char *p = skip_ws_digits(s, &neg);
    (void)p;
    unsigned long long v = strtoull(s, endptr, base);
    /* strtoull already applied the sign hack for negatives; for the
     * signed view a leading '-' was consumed twice-safe: values here
     * are small in practice (engine internals). */
    return (long long)v;
}
unsigned long strtoul(const char *s, char **endptr, int base) {
    return (unsigned long)strtoull(s, endptr, base);
}
long strtol(const char *s, char **endptr, int base) {
    return (long)strtoll(s, endptr, base);
}

double strtod(const char *s, char **endptr) {
    int neg;
    s = skip_ws_digits(s, &neg);
    double v = 0;
    int any = 0;
    while (isdigit((uint8_t)*s)) { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (*s == '.') {
        s++;
        double f = 0.1;
        while (isdigit((uint8_t)*s)) { v += (*s - '0') * f; f /= 10; s++; any = 1; }
    }
    if (*s == 'e' || *s == 'E') {
        const char *save = s;
        s++;
        int eneg = 0;
        if (*s == '-') { eneg = 1; s++; } else if (*s == '+') s++;
        if (isdigit((uint8_t)*s)) {
            int e = 0;
            while (isdigit((uint8_t)*s)) { e = e * 10 + (*s - '0'); s++; }
            extern double pow(double, double);
            v *= pow(10.0, eneg ? -(double)e : (double)e);
        } else s = save;
    }
    if (endptr) *endptr = (char *)s;
    (void)any;
    return neg ? -v : v;
}

typedef struct { int quot, rem; } shim_div_t;
shim_div_t shim_div_impl(int num, int denom) {
    shim_div_t d; d.quot = num / denom; d.rem = num % denom; return d;
}

static int qsort_cmp_wrap(const void *a, const void *b, void *arg) {
    (void)a; (void)b; (void)arg; return 0;
}
static void swap_bytes(char *a, char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { char t = a[i]; a[i] = b[i]; b[i] = t; }
}
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *)) {
    if (nmemb < 2 || size == 0) return;
    /* simple in-place quicksort (median-of-three pivot by index) */
    char *b = base;
    size_t stack_lo[32], stack_hi[32];
    int sp = 0;
    stack_lo[sp] = 0; stack_hi[sp] = nmemb - 1; sp++;
    while (sp > 0) {
        sp--;
        size_t lo = stack_lo[sp], hi = stack_hi[sp];
        if (lo >= hi) continue;
        size_t mid = lo + (hi - lo) / 2;
        char *pivot = b + mid * size;
        /* move pivot to lo, partition */
        swap_bytes(b + lo * size, pivot, size);
        size_t store = lo + 1;
        for (size_t i = lo + 1; i <= hi; i++) {
            if (cmp(b + i * size, b + lo * size) < 0) {
                swap_bytes(b + i * size, b + store * size, size);
                store++;
            }
        }
        swap_bytes(b + lo * size, b + (store - 1) * size, size);
        size_t p = store - 1;
        if (p > lo && sp < 32) { stack_lo[sp] = lo; stack_hi[sp] = p - 1; sp++; }
        if (p < hi && sp < 32) { stack_lo[sp] = p + 1; stack_hi[sp] = hi; sp++; }
    }
}
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *)) {
    const char *b = base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, b + mid * size);
        if (c == 0) return (void *)(b + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
static uint32_t rand_state = 0x12345678u;
int rand(void) {
    rand_state = rand_state * 1664525u + 1013904223u;
    return (int)((rand_state >> 8) & 0x7FFFFFFFu);
}
void srand(unsigned int seed) { rand_state = seed ? seed : 1; }

/* ================================================================
 * stdio formatting: snprintf family + console printf.
 * Float formatting is a straightforward digit-extraction
 * implementation: correct for the magnitudes the engines print
 * internally; not a correctly-rounded dtoa.
 * ================================================================ */
double frexp(double x, int *e);
double ldexp(double x, int e);
double floor(double x);
double fabs(double x);
double log10(double x);
double pow(double x, double y);

struct out_sink { char *buf; size_t cap; size_t len; };
static void sink_putc(struct out_sink *o, char c) {
    if (o->buf && o->cap && o->len < o->cap - 1) o->buf[o->len] = c;
    o->len++;
}
static void sink_write(struct out_sink *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) sink_putc(o, s[i]);
}
static void sink_repeat(struct out_sink *o, char c, int n) {
    while (n-- > 0) sink_putc(o, c);
}
static int u64_str(char *out, uint64_t v, int base, int upper) {
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % (unsigned)base]; v /= (unsigned)base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}
/* decimal digits of the integer part + fractional digits of val (>=0,
 * finite). Writes "III.FFF" with exactly prec fractional digits. */
static int fmt_fixed(char *out, double val, int prec) {
    char *p = out;
    uint64_t ip = (uint64_t)val;
    double frac = val - (double)ip;
    p += u64_str(p, ip, 10, 0);
    if (prec > 0) {
        *p++ = '.';
        for (int i = 0; i < prec; i++) {
            frac *= 10.0;
            int d = (int)frac;
            if (d > 9) d = 9;
            *p++ = (char)('0' + d);
            frac -= d;
        }
        /* round the last digit */
        if (frac >= 0.5 && prec > 0) {
            char *q = p - 1;
            while (q >= out) {
                if (*q == '.') { q--; continue; }
                if (*q < '9') { (*q)++; break; }
                *q = '0'; q--;
            }
        }
    }
    *p = 0;
    return (int)(p - out);
}
static int fmt_sci(char *out, double val, int prec, int upper, int strip) {
    char *p = out;
    if (val == 0) {
        *p++ = '0';
        if (prec > 0 && !strip) {
            *p++ = '.';
            for (int i = 0; i < prec; i++) *p++ = '0';
        }
        *p++ = upper ? 'E' : 'e';
        *p++ = '+'; *p++ = '0'; *p++ = '0';
        *p = 0;
        return (int)(p - out);
    }
    int e = (int)floor(log10(val));
    double m = val / pow(10.0, (double)e);
    while (m >= 10.0) { m /= 10.0; e++; }
    while (m < 1.0) { m *= 10.0; e--; }
    char num[64];
    fmt_fixed(num, m, prec + 1); /* one guard digit via rounding below */
    /* num is "D.DDDD"; keep prec fractional digits (guard digit was
     * produced by fmt_fixed's own rounding at prec+1) */
    int dot = 0;
    while (num[dot] && num[dot] != '.') dot++;
    int total = dot + (prec > 0 ? 1 + prec : 0);
    if (num[dot] == '.' && strip) {
        int end = (int)strlen(num);
        while (end > dot + 1 && num[end - 1] == '0') num[--end] = 0;
        if (end == dot + 1) num[end] = 0; /* drop bare '.' too */
        total = (int)strlen(num);
    }
    memcpy(p, num, (size_t)total);
    p += total;
    *p++ = upper ? 'E' : 'e';
    *p++ = e < 0 ? '-' : '+';
    int ae = e < 0 ? -e : e;
    if (ae < 10) { *p++ = '0'; p += u64_str(p, (uint64_t)ae, 10, 0); }
    else p += u64_str(p, (uint64_t)ae, 10, 0);
    *p = 0;
    return (int)(p - out);
}
static int fmt_double(char *out, double val, char conv, int prec) {
    if (val != val) { memcpy(out, conv == 'F' || conv == 'E' || conv == 'G' ? "NAN" : "nan", 4); return 3; }
    char *p = out;
    if (val < 0 || (val == 0 && 1.0 / val < 0)) { /* negative (incl -0) */
        /* -0 prints as -0 in %f/%g; keep it simple and consistent */
        if (val != 0) { *p++ = '-'; val = -val; }
    }
    dc_sign:;
    if (val > 1.7976931348623157e308) { memcpy(p, (conv >= 'a') ? "inf" : "INF", 3); return (int)(p - out) + 3; }
    if (conv == 'f' || conv == 'F') {
        if (prec < 0) prec = 6;
        return (int)(p - out) + fmt_fixed(p, val, prec);
    }
    if (conv == 'e' || conv == 'E') {
        if (prec < 0) prec = 6;
        return (int)(p - out) + fmt_sci(p, val, prec, conv == 'E', 0);
    }
    /* %g */
    if (prec < 0) prec = 6;
    if (prec == 0) prec = 1;
    int e = 0;
    if (val != 0) e = (int)floor(log10(val));
    if (e < -4 || e >= prec) {
        return (int)(p - out) + fmt_sci(p, val, prec - 1, conv == 'G', 1);
    }
    int fprec = prec - 1 - e;
    int n = fmt_fixed(p, val, fprec);
    /* strip trailing zeros in the fraction */
    char *dot = NULL;
    for (int i = 0; i < n; i++) if (p[i] == '.') dot = p + i;
    if (dot) {
        char *end = p + n;
        while (end > dot + 1 && end[-1] == '0') end--;
        if (end == dot + 1) end = dot;
        *end = 0;
        n = (int)(end - p);
    }
    return (int)(p - out) + n;
}

static int shim_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct out_sink o = { buf, size, 0 };
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { sink_putc(&o, *f); continue; }
        f++;
        if (*f == '%') { sink_putc(&o, '%'); continue; }
        int flag_left = 0, flag_plus = 0, flag_space = 0, flag_zero = 0, flag_hash = 0;
        for (;; f++) {
            if (*f == '-') flag_left = 1;
            else if (*f == '+') flag_plus = 1;
            else if (*f == ' ') flag_space = 1;
            else if (*f == '0') flag_zero = 1;
            else if (*f == '#') flag_hash = 1;
            else break;
        }
        int width = -1;
        if (*f == '*') { width = va_arg(ap, int); f++; }
        else if (*f >= '0' && *f <= '9') {
            width = 0;
            while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        }
        int prec = -1;
        if (*f == '.') {
            f++;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else {
                prec = 0;
                while (*f >= '0' && *f <= '9') { prec = prec * 10 + (*f - '0'); f++; }
            }
        }
        enum { LEN_INT, LEN_CHAR, LEN_SHORT, LEN_LONG, LEN_LLONG, LEN_SIZE } len = LEN_INT;
        if (*f == 'h') {
            f++;
            if (*f == 'h') { len = LEN_CHAR; f++; } else len = LEN_SHORT;
        } else if (*f == 'l') {
            f++;
            if (*f == 'l') { len = LEN_LLONG; f++; } else len = LEN_LONG;
        } else if (*f == 'z' || *f == 't') { len = LEN_SIZE; f++; }
        else if (*f == 'j' || *f == 'q') { len = LEN_LLONG; f++; }

        char conv = *f;
        char num[80];
        const char *body = num;
        int blen = 0;
        char sign = 0;
        const char *prefix = "";
        int is_num = 0, zero_pad = 0;

        if (conv == 'd' || conv == 'i') {
            long long v;
            if (len == LEN_LLONG) v = va_arg(ap, long long);
            else if (len == LEN_LONG) v = va_arg(ap, long);
            else if (len == LEN_SIZE) v = (long long)va_arg(ap, size_t);
            else v = va_arg(ap, int);
            uint64_t uv;
            if (v < 0) { sign = '-'; uv = (uint64_t)(-(v + 1)) + 1; }
            else { uv = (uint64_t)v; sign = flag_plus ? '+' : (flag_space ? ' ' : 0); }
            blen = u64_str(num, uv, 10, 0);
            is_num = 1;
        } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            uint64_t uv;
            if (len == LEN_LLONG) uv = va_arg(ap, unsigned long long);
            else if (len == LEN_LONG) uv = va_arg(ap, unsigned long);
            else if (len == LEN_SIZE) uv = va_arg(ap, size_t);
            else uv = va_arg(ap, unsigned int);
            int base = conv == 'o' ? 8 : (conv == 'u' ? 10 : 16);
            blen = u64_str(num, uv, base, conv == 'X');
            if (flag_hash && conv != 'u' && uv != 0) prefix = (conv == 'o') ? "0" : (conv == 'X' ? "0X" : "0x");
            if (flag_hash && conv == 'o' && uv == 0) prefix = "";
            is_num = 1;
        } else if (conv == 'p') {
            void *ptr = va_arg(ap, void *);
            blen = u64_str(num, (uint32_t)(size_t)ptr, 16, 0);
            prefix = "0x";
            is_num = 1;
        } else if (conv == 'c') {
            num[0] = (char)va_arg(ap, int);
            blen = 1;
        } else if (conv == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            body = s;
            blen = (int)strlen(s);
            if (prec >= 0 && blen > prec) blen = prec;
        } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
                   conv == 'g' || conv == 'G') {
            double v = va_arg(ap, double);
            if (v < 0) { sign = '-'; }
            else if (flag_plus) sign = '+';
            else if (flag_space) sign = ' ';
            blen = fmt_double(num, v, conv, prec);
            if (num[0] == '-') { /* fmt_double embedded the sign */ sign = 0; }
            is_num = 1;
            if (flag_zero && (num[0] == 'i' || num[0] == 'n' || num[0] == 'N' || num[0] == 'I'))
                flag_zero = 0;
        } else if (conv == 0) {
            break;
        } else {
            sink_putc(&o, '%');
            sink_putc(&o, conv);
            continue;
        }

        /* precision for integers = minimum digits */
        int zeros = 0;
        if (is_num && prec >= 0 && (conv == 'd' || conv == 'i' || conv == 'u' ||
                                    conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p')) {
            if (prec > blen) zeros = prec - blen;
            flag_zero = 0;
        }
        int content = blen + zeros + (sign ? 1 : 0) + (int)strlen(prefix);
        int pad = (width > content) ? width - content : 0;
        zero_pad = flag_zero && !flag_left;
        if (!flag_left && !zero_pad) sink_repeat(&o, ' ', pad);
        if (sign) sink_putc(&o, sign);
        sink_write(&o, prefix, strlen(prefix));
        if (!flag_left && zero_pad) sink_repeat(&o, '0', pad);
        sink_repeat(&o, '0', zeros);
        sink_write(&o, body, (size_t)blen);
        if (flag_left) sink_repeat(&o, ' ', pad);
    }
    if (buf && size) buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    return shim_vsnprintf(buf, size, fmt, ap);
}
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = shim_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
int vsprintf(char *buf, const char *fmt, va_list ap) {
    return shim_vsnprintf(buf, (size_t)-1, fmt, ap);
}
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = shim_vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

/* console output */
static char printf_buf[1024];
int vfprintf(void *stream, const char *fmt, va_list ap) {
    (void)stream;
    int r = shim_vsnprintf(printf_buf, sizeof(printf_buf), fmt, ap);
    print(printf_buf);
    return r;
}
int fprintf(void *stream, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stream, fmt, ap);
    va_end(ap);
    return r;
}
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(NULL, fmt, ap);
    va_end(ap);
    return r;
}
int fputs(const char *s, void *stream) { (void)stream; print(s); return 0; }
int puts(const char *s) { print(s); print("\n"); return 0; }
int putchar(int c) { term_putchar((char)c); return c; }
int fputc(int c, void *stream) { (void)stream; return putchar(c); }
int fflush(void *stream) { (void)stream; return 0; }
void perror(const char *s) { if (s) { print(s); print(": error\n"); } }
/* FILE I/O is not available in the kernel: stubs fail cleanly. */
void *stdin = (void *)1, *stdout = (void *)2, *stderr = (void *)3;
void *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
size_t fread(void *ptr, size_t size, size_t nmemb, void *f) {
    (void)ptr; (void)size; (void)nmemb; (void)f; return 0;
}
size_t fwrite(const void *ptr, size_t size, size_t nmemb, void *f) {
    (void)f;
    /* writes to stdout/stderr sentinels go to the console */
    print((const char *)ptr);
    return nmemb;
}
int fclose(void *f) { (void)f; return 0; }
int fseek(void *f, long off, int whence) { (void)f; (void)off; (void)whence; return -1; }
long ftell(void *f) { (void)f; return -1; }
char *fgets(char *s, int size, void *f) { (void)s; (void)size; (void)f; return NULL; }

/* ================================================================
 * sscanf: minimal subset (%d %i %u %x %s %c %f, width, %* suppression)
 * ================================================================ */
static int shim_vsscanf(const char *str, const char *fmt, va_list ap) {
    int assigned = 0;
    const char *s = str;
    for (const char *f = fmt; *f; f++) {
        if (*f == ' ' || *f == '\t' || *f == '\n') {
            while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
            continue;
        }
        if (*f != '%') {
            if (*s != *f) return assigned;
            s++;
            continue;
        }
        f++;
        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }
        int width = -1;
        if (*f >= '0' && *f <= '9') {
            width = 0;
            while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        }
        int is_long = 0, is_llong = 0;
        if (*f == 'l') { is_long = 1; f++; if (*f == 'l') { is_llong = 1; f++; } }
        else if (*f == 'h') { f++; if (*f == 'h') f++; }
        char conv = *f;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X') {
            while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
            if (*s == 0) return assigned ? assigned : -1;
            char *end;
            long long v;
            if (conv == 'd' || conv == 'i')
                v = strtoll(s, &end, conv == 'i' ? 0 : 10);
            else
                v = (long long)strtoull(s, &end, conv == 'u' ? 10 : 16);
            if (end == s) return assigned;
            if (width > 0 && end - s > width) { /* width rarely matters here */ }
            if (!suppress) {
                if (is_llong) *va_arg(ap, long long *) = v;
                else if (is_long) *va_arg(ap, long *) = (long)v;
                else if (conv == 'u' || conv == 'x' || conv == 'X')
                    *va_arg(ap, unsigned int *) = (unsigned int)v;
                else *va_arg(ap, int *) = (int)v;
                assigned++;
            }
            s = end;
        } else if (conv == 'f' || conv == 'e' || conv == 'g') {
            while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
            if (*s == 0) return assigned ? assigned : -1;
            char *end;
            double v = strtod(s, &end);
            if (end == s) return assigned;
            if (!suppress) {
                if (is_long) *va_arg(ap, double *) = v;
                else *va_arg(ap, float *) = (float)v;
                assigned++;
            }
            s = end;
        } else if (conv == 's') {
            while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
            if (*s == 0) return assigned ? assigned : -1;
            char *dst = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (*s && !(*s == ' ' || (*s >= '\t' && *s <= '\r')) &&
                   (width < 0 || n < width)) {
                if (dst) dst[n] = *s;
                n++; s++;
            }
            if (dst) dst[n] = 0;
            if (!suppress) assigned++;
        } else if (conv == 'c') {
            int want = width > 0 ? width : 1;
            char *dst = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (n < want && *s) { if (dst) dst[n] = *s; n++; s++; }
            if (n == 0) return assigned ? assigned : -1;
            if (!suppress) assigned++;
        } else if (conv == '%') {
            if (*s != '%') return assigned;
            s++;
        } else if (conv == 0) {
            break;
        }
    }
    return assigned;
}
int vsscanf(const char *str, const char *fmt, va_list ap) {
    return shim_vsscanf(str, fmt, ap);
}
int sscanf(const char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = shim_vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

/* ================================================================
 * Time: backed by the CMOS RTC (UTC) + timer ticks.
 * ================================================================ */
struct shim_tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
/* days from civil (Howard Hinnant's algorithm) */
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(long z, int *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y = (int)(yy + (*m <= 2));
}
static long rtc_epoch(int *ok) {
    struct rtc_time rt;
    if (rtc_read(&rt)) {
        long days = days_from_civil(rt.year, rt.month, rt.day);
        *ok = 1;
        return days * 86400 + rt.hour * 3600 + rt.min * 60 + rt.sec;
    }
    *ok = 0;
    return 0;
}
long shim_time(long *t) {
    int ok;
    long v = rtc_epoch(&ok);
    if (!ok) v = 1761955200L + (long)uptime_seconds(); /* 2025-11-01 + uptime */
    if (t) *t = v;
    return v;
}
long time(long *t) { return shim_time(t); }
int gettimeofday(void *tvv, void *tz) {
    (void)tz;
    if (tvv) {
        long *tv = tvv;
        tv[0] = shim_time(NULL);
        tv[1] = (long)(timer_ticks % 100) * 10000;
    }
    return 0;
}
long clock(void) { return (long)timer_ticks * 10000; }
double difftime(long a, long b) { return (double)(a - b); }
struct shim_tm *gmtime_r(const long *tp, struct shim_tm *tm) {
    long t = *tp;
    long days = t / 86400;
    long secs = t % 86400;
    if (secs < 0) { secs += 86400; days -= 1; }
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    tm->tm_year = y - 1900;
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_hour = (int)(secs / 3600);
    tm->tm_min = (int)((secs % 3600) / 60);
    tm->tm_sec = (int)(secs % 60);
    long wday = (days + 4) % 7; /* 1970-01-01 was a Thursday */
    if (wday < 0) wday += 7;
    tm->tm_wday = (int)wday;
    tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = "UTC";
    return tm;
}
struct shim_tm *localtime_r(const long *tp, struct shim_tm *tm) {
    return gmtime_r(tp, tm); /* kernel keeps UTC everywhere */
}
static struct shim_tm tm_static;
struct shim_tm *gmtime(const long *tp) { return gmtime_r(tp, &tm_static); }
struct shim_tm *localtime(const long *tp) { return gmtime_r(tp, &tm_static); }
long mktime(struct shim_tm *tm) {
    long days = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}
int clock_gettime(int clk, void *tsv) {
    (void)clk;
    long *ts = tsv;
    if (ts) {
        ts[0] = shim_time(NULL);
        ts[1] = (long)(timer_ticks % 100) * 10000000L;
    }
    return 0;
}

static const char *const MON_S[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec" };
static const char *const MON_L[] = { "January","February","March","April",
    "May","June","July","August","September","October","November","December" };
static const char *const DAY_S[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char *const DAY_L[] = { "Sunday","Monday","Tuesday","Wednesday",
                                     "Thursday","Friday","Saturday" };

static void fmt2(char **p, int v) {
    (*p)[0] = (char)('0' + (v / 10) % 10);
    (*p)[1] = (char)('0' + v % 10);
    *p += 2;
}
static void fmt_pad(char **p, int v, int width) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    char rev[12]; int rn = 0;
    while (v) { rev[rn++] = (char)('0' + v % 10); v /= 10; }
    while (rn) tmp[n++] = rev[--rn];
    tmp[n] = 0;
    for (int i = n + neg; i < width; i++) *(*p)++ = '0';
    if (neg) *(*p)++ = '-';
    for (int i = 0; i < n; i++) *(*p)++ = tmp[i];
}
size_t strftime(char *s, size_t max, const char *fmt, const struct shim_tm *tm) {
    char *p = s;
    char *end = s + (max ? max - 1 : 0);
    for (const char *f = fmt; *f && p < end; f++) {
        if (*f != '%') { *p++ = *f; continue; }
        f++;
        char tmp[32];
        const char *rep = tmp;
        tmp[0] = 0;
        switch (*f) {
            case 'Y': { char *t = tmp; fmt_pad(&t, tm->tm_year + 1900, 4); *t = 0; break; }
            case 'y': { char *t = tmp; fmt2(&t, (tm->tm_year + 1900) % 100); *t = 0; break; }
            case 'm': { char *t = tmp; fmt2(&t, tm->tm_mon + 1); *t = 0; break; }
            case 'd': case 'e': { char *t = tmp; fmt2(&t, tm->tm_mday); *t = 0; break; }
            case 'H': case 'k': { char *t = tmp; fmt2(&t, tm->tm_hour); *t = 0; break; }
            case 'I': case 'l': { int h = tm->tm_hour % 12; if (!h) h = 12;
                                  char *t = tmp; fmt2(&t, h); *t = 0; break; }
            case 'M': { char *t = tmp; fmt2(&t, tm->tm_min); *t = 0; break; }
            case 'S': { char *t = tmp; fmt2(&t, tm->tm_sec); *t = 0; break; }
            case 'p': case 'P': rep = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'a': rep = DAY_S[tm->tm_wday % 7]; break;
            case 'A': rep = DAY_L[tm->tm_wday % 7]; break;
            case 'b': case 'h': rep = MON_S[tm->tm_mon % 12]; break;
            case 'B': rep = MON_L[tm->tm_mon % 12]; break;
            case 'j': { char *t = tmp; fmt_pad(&t, tm->tm_yday + 1, 3); *t = 0; break; }
            case 'w': tmp[0] = (char)('0' + tm->tm_wday % 7); tmp[1] = 0; break;
            case 'z': rep = "+0000"; break;
            case 'Z': rep = "UTC"; break;
            case '%': rep = "%"; break;
            case 'F': rep = "%Y-%m-%d"; {
                /* recurse one level */
                char sub[24];
                strftime(sub, sizeof(sub), rep, tm);
                size_t l = strlen(sub);
                if ((size_t)(end - p) < l) l = (size_t)(end - p);
                memcpy(p, sub, l); p += l;
                continue;
            }
            case 'T': rep = "%H:%M:%S"; {
                char sub[24];
                strftime(sub, sizeof(sub), rep, tm);
                size_t l = strlen(sub);
                if ((size_t)(end - p) < l) l = (size_t)(end - p);
                memcpy(p, sub, l); p += l;
                continue;
            }
            default: tmp[0] = '%'; tmp[1] = *f; tmp[2] = 0; break;
        }
        size_t l = strlen(rep);
        if ((size_t)(end - p) < l) l = (size_t)(end - p);
        memcpy(p, rep, l);
        p += l;
    }
    *p = 0;
    return (size_t)(p - s);
}
char *asctime(const struct shim_tm *tm) {
    static char buf[32];
    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Y", (const struct shim_tm *)tm);
    return buf;
}
char *ctime(const long *t) { return asctime(gmtime(t)); }

/* strptime subset: %Y %m %d %H %M %S %b %B %a %A %p, whitespace and
 * literals. Returns pointer past the last consumed input or NULL. */
static int match_name(const char *s, const char *const *names, int count, int *consumed) {
    for (int i = 0; i < count; i++) {
        size_t l = strlen(names[i]);
        if (strncasecmp(s, names[i], l) == 0) { *consumed = (int)l; return i; }
    }
    return -1;
}
static const char *parse_num2(const char *s, int *out, int maxdigits) {
    int v = 0, n = 0;
    while (n < maxdigits && *s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; n++; }
    if (n == 0) return NULL;
    *out = v;
    return s;
}
char *strptime(const char *s, const char *fmt, struct shim_tm *tm) {
    for (const char *f = fmt; *f; f++) {
        if (*f == ' ' || *f == '\t' || *f == '\n') {
            while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
            continue;
        }
        if (*f != '%') {
            if (*s != *f) return NULL;
            s++;
            continue;
        }
        f++;
        int v, consumed;
        const char *ns;
        switch (*f) {
            case 'Y': ns = parse_num2(s, &v, 4); if (!ns) return NULL;
                      tm->tm_year = v - 1900; s = ns; break;
            case 'y': ns = parse_num2(s, &v, 2); if (!ns) return NULL;
                      tm->tm_year = v + (v < 69 ? 100 : 0); s = ns; break;
            case 'm': ns = parse_num2(s, &v, 2); if (!ns) return NULL;
                      tm->tm_mon = v - 1; s = ns; break;
            case 'd': case 'e': ns = parse_num2(s, &v, 2); if (!ns) return NULL;
                      tm->tm_mday = v; s = ns; break;
            case 'H': case 'k': ns = parse_num2(s, &v, 2); if (!ns) return NULL;
                      tm->tm_hour = v; s = ns; break;
            case 'I': case 'l': ns = parse_num2(s, &v, 2); if (!ns) return NULL;
                      tm->tm_hour = v % 12; s = ns; break;
            case 'M': ns = parse_num2(s, &v, 2); if (!ns) return NULL;
                      tm->tm_min = v; s = ns; break;
            case 'S': ns = parse_num2(s, &v, 2); if (!ns) return NULL;
                      tm->tm_sec = v; s = ns; break;
            case 'b': case 'h':
                v = match_name(s, MON_S, 12, &consumed);
                if (v < 0) return NULL;
                tm->tm_mon = v; s += consumed; break;
            case 'B':
                v = match_name(s, MON_L, 12, &consumed);
                if (v < 0) { /* try short names too */
                    v = match_name(s, MON_S, 12, &consumed);
                    if (v < 0) return NULL;
                }
                tm->tm_mon = v; s += consumed; break;
            case 'a': case 'A': {
                const char *const *tbl = (*f == 'a') ? DAY_S : DAY_L;
                v = match_name(s, tbl, 7, &consumed);
                if (v < 0) return NULL;
                tm->tm_wday = v; s += consumed; break;
            }
            case 'p': case 'P':
                if (strncasecmp(s, "PM", 2) == 0) {
                    if (tm->tm_hour < 12) tm->tm_hour += 12;
                    s += 2;
                } else if (strncasecmp(s, "AM", 2) == 0) s += 2;
                else return NULL;
                break;
            case '%': if (*s != '%') return NULL; s++; break;
            default: return NULL;
        }
    }
    return (char *)s;
}
