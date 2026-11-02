/* js_math.c - from-scratch libm subset for the JS engine shims.
 *
 * Double-precision implementations written for the Shiro Mini OS
 * browser spike: bit-level helpers (fabs/frexp/ldexp/copysign),
 * polynomial/Taylor cores with range reduction for the transcendentals.
 * Accuracy is ~1 ulp-1e-12 relative for common ranges - more than the
 * engines' internal uses and the spike's eval checks require; it is
 * not a correctly-rounded libm and says so.
 */
#include <stddef.h>
#include <stdint.h>

typedef union { double d; uint64_t u; } dc_t;
#define EXP_MASK 0x7FF0000000000000ULL
#define FRAC_MASK 0x000FFFFFFFFFFFFFULL

static int is_nan_d(double x) {
    dc_t v; v.d = x;
    return (v.u & EXP_MASK) == EXP_MASK && (v.u & FRAC_MASK) != 0;
}
static int is_inf_d(double x) {
    dc_t v; v.d = x;
    return (v.u & EXP_MASK) == EXP_MASK && (v.u & FRAC_MASK) == 0;
}
static double make_inf(int neg) {
    dc_t v; v.u = EXP_MASK | (neg ? 0x8000000000000000ULL : 0);
    return v.d;
}
static double make_nan(void) {
    dc_t v; v.u = EXP_MASK | 0x0008000000000000ULL;
    return v.d;
}

double fabs(double x) {
    dc_t v; v.d = x; v.u &= 0x7FFFFFFFFFFFFFFFULL; return v.d;
}
float fabsf(float x) { return (float)fabs((double)x); }
double copysign(double x, double y) {
    dc_t a, b; a.d = x; b.d = y;
    a.u = (a.u & 0x7FFFFFFFFFFFFFFFULL) | (b.u & 0x8000000000000000ULL);
    return a.d;
}
double trunc(double x) {
    if (is_nan_d(x) || is_inf_d(x)) return x;
    if (fabs(x) >= 9007199254740992.0) return x; /* 2^53: already integral */
    return (double)(int64_t)x;
}
double floor(double x) {
    if (is_nan_d(x) || is_inf_d(x)) return x;
    if (fabs(x) >= 9007199254740992.0) return x;
    int64_t t = (int64_t)x; /* truncates toward zero */
    double r = (double)t;
    if (r > x) r -= 1.0;
    return r;
}
double ceil(double x) {
    if (is_nan_d(x) || is_inf_d(x)) return x;
    if (fabs(x) >= 9007199254740992.0) return x;
    int64_t t = (int64_t)x;
    double r = (double)t;
    if (r < x) r += 1.0;
    return r;
}
double round(double x) {
    if (x >= 0) return floor(x + 0.5);
    return ceil(x - 0.5);
}
double nearbyint(double x) { return round(x); }
double rint(double x) { return round(x); }
long lrint(double x) { return (long)round(x); }
long lround(double x) { return (long)round(x); }
double fmod(double x, double y) {
    if (y == 0 || is_inf_d(x)) return make_nan();
    double q = trunc(x / y);
    return x - q * y;
}
double remainder(double x, double y) {
    double q = round(x / y);
    return x - q * y;
}
double modf(double x, double *iptr) {
    double t = trunc(x);
    *iptr = t;
    return x - t;
}
float modff(float x, float *iptr) {
    double ip; double r = modf((double)x, &ip);
    *iptr = (float)ip; return (float)r;
}
double fmax(double x, double y) {
    if (is_nan_d(x)) return y;
    if (is_nan_d(y)) return x;
    return x > y ? x : y;
}
double fmin(double x, double y) {
    if (is_nan_d(x)) return y;
    if (is_nan_d(y)) return x;
    return x < y ? x : y;
}
double fdim(double x, double y) { return x > y ? x - y : 0.0; }

double frexp(double x, int *e) {
    dc_t v; v.d = x;
    if (x == 0 || is_inf_d(x) || is_nan_d(x)) { *e = 0; return x; }
    int ex = (int)((v.u >> 52) & 0x7FF);
    if (ex == 0) { /* subnormal: scale up by 2^64 and adjust */
        double m = frexp(x * 18446744073709551616.0, e);
        *e -= 64;
        return m;
    }
    *e = ex - 1022;
    v.u = (v.u & ~(0x7FFULL << 52)) | (1022ULL << 52);
    return v.d;
}
double ldexp(double x, int e) {
    if (x == 0 || is_inf_d(x) || is_nan_d(x)) return x;
    /* scale in <=512 chunks via bit-constructed powers of two */
    while (e > 0) {
        int step = e > 512 ? 512 : e;
        dc_t s; s.u = (uint64_t)(1023 + step) << 52;
        x *= s.d; e -= step;
        if (is_inf_d(x)) return x;
    }
    while (e < 0) {
        int step = e < -512 ? 512 : -e;
        dc_t s; s.u = (uint64_t)(1023 - step) << 52;
        x *= s.d; e += step;
        if (x == 0) return x;
    }
    return x;
}
double scalbn(double x, int n) { return ldexp(x, n); }

double sqrt(double x) {
    if (x < 0) return make_nan();
    if (x == 0 || is_inf_d(x) || is_nan_d(x)) return x;
    dc_t v; v.d = x;
    v.u = (v.u >> 1) + 0x1FF7A3BEA91B9B1BULL; /* magic seed */
    double y = v.d;
    for (int i = 0; i < 6; i++) y = 0.5 * (y + x / y);
    return y;
}
double cbrt(double x) {
    if (x == 0 || is_inf_d(x) || is_nan_d(x)) return x;
    int neg = x < 0;
    double ax = fabs(x);
    int e;
    (void)frexp(ax, &e);
    double y = ldexp(1.0, (e >= 0 ? e : e - 2) / 3 + 1); /* seed ~2^(e/3) */
    for (int i = 0; i < 10; i++) y = (2.0 * y + ax / (y * y)) / 3.0;
    return neg ? -y : y;
}
double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0) return 0;
    double r = y / x;
    return x * sqrt(1.0 + r * r);
}

/* 64-bit unsigned division helpers (normally from libgcc, which the
 * kernel does not link). Shift-and-subtract; plenty fast for the
 * engines' occasional 64-bit divisions. */
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem) {
    if (d == 0) { if (rem) *rem = 0; return 0; }
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, NULL); }
uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r; (void)__udivmoddi4(n, d, &r); return r;
}

#define LN2_HI 0.6931471803691238
#define LN2_LO 1.9082149292705877e-10
#define LN2    0.6931471805599453
#define PI     3.14159265358979323846

double exp(double x) {
    if (is_nan_d(x)) return x;
    if (x > 709.782712893384) return make_inf(0);
    if (x < -745.1332191019411) return 0.0;
    double k = round(x / LN2);
    double r = x - k * LN2_HI - k * LN2_LO;
    /* Taylor for exp(r), |r| <= ln2/2 */
    double sum = 1.0, term = 1.0;
    for (int n = 1; n <= 16; n++) { term *= r / n; sum += term; }
    return ldexp(sum, (int)k);
}
double exp2(double x) { return exp(x * LN2); }
double expm1(double x) {
    if (fabs(x) < 0.25) {
        double sum = x, term = x;
        for (int n = 2; n <= 14; n++) { term *= x / n; sum += term; }
        return sum;
    }
    return exp(x) - 1.0;
}
double log(double x) {
    if (x < 0) return make_nan();
    if (x == 0) return make_inf(1);
    if (is_inf_d(x) || is_nan_d(x)) return x;
    int e;
    double m = frexp(x, &e); /* x = m * 2^e, m in [0.5, 1) */
    /* recenter m into [sqrt(1/2), sqrt(2)) */
    if (m < 0.7071067811865476) { m *= 2.0; e -= 1; }
    /* atanh series: log(m) = 2 * (z + z^3/3 + ...) with z=(m-1)/(m+1) */
    double z = (m - 1.0) / (m + 1.0);
    double z2 = z * z;
    double sum = 0.0, term = z;
    for (int n = 1; n <= 25; n += 2) { sum += term / n; term *= z2; }
    return e * LN2 + 2.0 * sum;
}
double log2(double x) { return log(x) / LN2; }
double log10(double x) { return log(x) / 2.302585092994046; }
double log1p(double x) {
    if (fabs(x) < 0.25) {
        double sum = 0.0, term = x;
        for (int n = 1; n <= 24; n++) {
            sum += (n % 2 == 1) ? term / n : -term / n;
            term *= x;
        }
        return sum;
    }
    return log(1.0 + x);
}
double pow(double x, double y) {
    if (y == 0) return 1.0;
    if (x == 1.0) return 1.0;
    if (is_nan_d(x) || is_nan_d(y)) return make_nan();
    if (x == 0) return y > 0 ? 0.0 : make_inf(0);
    /* integral exponents: exact repeated multiplication */
    if (y == trunc(y) && fabs(y) <= 64.0) {
        long n = (long)y;
        unsigned long m = n < 0 ? (unsigned long)(-n) : (unsigned long)n;
        double r = 1.0, base = x;
        while (m) { if (m & 1) r *= base; base *= base; m >>= 1; }
        return n < 0 ? 1.0 / r : r;
    }
    if (x < 0) return make_nan();
    return exp(y * log(x));
}

static double sin_series(double r) {
    double sum = r, term = r;
    for (int n = 1; n <= 10; n++) {
        term *= -r * r / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}
static double cos_series(double r) {
    double sum = 1.0, term = 1.0;
    for (int n = 1; n <= 10; n++) {
        term *= -r * r / ((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}
/* reduce x to r in [-pi/4, pi/4] plus quadrant */
static int reduce_pi2(double x, double *r) {
    double k = round(x / (PI / 2));
    double hi = 1.5707963267948966, lo = 6.123233995736766e-17;
    *r = x - k * hi - k * lo;
    long q = (long)k % 4;
    if (q < 0) q += 4;
    return (int)q;
}
double sin(double x) {
    if (is_nan_d(x) || is_inf_d(x)) return make_nan();
    double r; int q = reduce_pi2(x, &r);
    switch (q) {
        case 0: return sin_series(r);
        case 1: return cos_series(r);
        case 2: return -sin_series(r);
        default: return -cos_series(r);
    }
}
double cos(double x) {
    if (is_nan_d(x) || is_inf_d(x)) return make_nan();
    double r; int q = reduce_pi2(x, &r);
    switch (q) {
        case 0: return cos_series(r);
        case 1: return -sin_series(r);
        case 2: return -cos_series(r);
        default: return sin_series(r);
    }
}
double tan(double x) {
    double c = cos(x);
    if (c == 0) return make_nan();
    return sin(x) / c;
}
double atan(double x) {
    if (is_nan_d(x)) return x;
    if (is_inf_d(x)) return x > 0 ? PI / 2 : -PI / 2;
    int neg = 0, inv = 0;
    if (x < 0) { neg = 1; x = -x; }
    if (x > 1.0) { inv = 1; x = 1.0 / x; }
    /* two half-angle reductions bring |x| below ~0.1 */
    for (int i = 0; i < 2; i++)
        x = x / (1.0 + sqrt(1.0 + x * x));
    double x2 = x * x, sum = 0.0, term = x;
    for (int n = 1; n <= 15; n += 2) {
        sum += ((n / 2) % 2 == 0) ? term / n : -term / n;
        term *= x2;
    }
    double r = 4.0 * sum; /* 2 reductions => factor 4 */
    if (inv) r = PI / 2 - r;
    return neg ? -r : r;
}
double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0) {
        double r = atan(y / x);
        return y >= 0 ? r + PI : r - PI;
    }
    if (y > 0) return PI / 2;
    if (y < 0) return -PI / 2;
    return 0.0;
}
double asin(double x) {
    if (x < -1.0 || x > 1.0) return make_nan();
    if (x == 1.0) return PI / 2;
    if (x == -1.0) return -PI / 2;
    return atan(x / sqrt(1.0 - x * x));
}
double acos(double x) {
    if (x < -1.0 || x > 1.0) return make_nan();
    return PI / 2 - asin(x);
}
double sinh(double x) { return (exp(x) - exp(-x)) / 2.0; }
double cosh(double x) { return (exp(x) + exp(-x)) / 2.0; }
double tanh(double x) {
    if (x > 20) return 1.0;
    if (x < -20) return -1.0;
    double e = exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}
