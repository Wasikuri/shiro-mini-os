/* Shiro Mini OS - TLS client layer (v0.6)
 *
 * Engine: BearSSL 0.6 (MIT license, (c) 2016 Thomas Pornin; sources
 * in third_party/bearssl/, see third_party/README.md). We use its
 * low-level client engine + "ssl io" (br_sslio) with our own I/O
 * callbacks over net.c's from-scratch TCP client, and the
 * x509_minimal validator against the trust anchors bundled in
 * tls_anchors.h (generated from the PEMs in third_party/certs/).
 *
 * BearSSL speaks TLS 1.0..1.2, so HTTPS here is TLS 1.2 in practice.
 * No client certificates, no session resumption tickets.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os.h"
#include "net.h"
#include "tls.h"
#include "bearssl.h"
#include "tls_anchors.h"

/* ---------- contexts (static: far too big for task stacks) ---------- */
static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static br_sslio_context ioc;
static unsigned char tls_ibuf[BR_SSL_BUFSIZE_INPUT];
static unsigned char tls_obuf[BR_SSL_BUFSIZE_OUTPUT];

static int tls_active = 0;
static int tls_err = 0;          /* last BearSSL error code */
static int transport_eof = 0;    /* TCP reached FIN / was closed */
static int transport_timeout = 0;

/* ================================================================
 * Entropy
 *
 * Sources, honestly assessed (see README): RDRAND when CPUID
 * advertises it (real CPU entropy); PIT-tick jitter loop counts
 * (weak under an emulator); RTC wall time (not secret, just
 * stir); NIC MAC + own IP; kernel addresses. Everything is mixed
 * through SHA-256 and fed to BearSSL's HMAC_DRBG via TWO hooks:
 * br_prng_seeder_system() below (BearSSL calls it automatically)
 * and an explicit br_ssl_engine_inject_entropy() in tls_connect().
 * src/rand/sysrng.c is NOT compiled; this file replaces it.
 * ================================================================ */
static void cpuid1(uint32_t *ecx_out) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u));
    *ecx_out = c;
}
static int have_rdrand(void) {
    uint32_t c;
    cpuid1(&c);
    return (int)((c >> 30) & 1u);
}
static int rdrand32(uint32_t *out) {
    uint32_t v;
    unsigned char ok;
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    if (ok) { *out = v; return 1; }
    return 0;
}

static int tls_used_rdrand = 0;

static void entropy_pool(uint8_t out[64]) {
    br_sha256_context h;
    uint8_t digest[32];

    br_sha256_init(&h);
    tls_used_rdrand = have_rdrand();
    if (tls_used_rdrand) {
        for (int i = 0; i < 16; i++) {
            uint32_t v = 0;
            for (int t = 0; t < 10 && !rdrand32(&v); t++) { }
            br_sha256_update(&h, &v, sizeof v);
        }
    }
    /* PIT tick jitter: how many loop iterations fit between 100 Hz
     * timer edges varies with emulator scheduling noise. */
    for (int i = 0; i < 24; i++) {
        volatile uint32_t spin = 0;
        uint32_t t0 = timer_ticks;
        while (timer_ticks == t0) spin++;
        uint32_t tk = timer_ticks;
        br_sha256_update(&h, (const void *)&spin, sizeof spin);
        br_sha256_update(&h, &tk, sizeof tk);
    }
    struct rtc_time rt;
    if (rtc_read(&rt)) br_sha256_update(&h, &rt, sizeof rt);
    uint32_t tk = timer_ticks;
    br_sha256_update(&h, &tk, sizeof tk);
    const uint8_t *mac = net_mac_addr();
    br_sha256_update(&h, mac, 6);
    uint32_t ip = net_local_ip();
    br_sha256_update(&h, &ip, sizeof ip);
    uintptr_t a1 = (uintptr_t)&sc, a2 = (uintptr_t)out, a3 = (uintptr_t)&h;
    br_sha256_update(&h, &a1, sizeof a1);
    br_sha256_update(&h, &a2, sizeof a2);
    br_sha256_update(&h, &a3, sizeof a3);
    br_sha256_out(&h, digest);
    memcpy(out, digest, 32);
    br_sha256_init(&h);
    br_sha256_update(&h, digest, 32);
    tk = timer_ticks;
    br_sha256_update(&h, &tk, sizeof tk);
    br_sha256_out(&h, digest);
    memcpy(out + 32, digest, 32);
}

/* BearSSL "system" seeder replacement (its sysrng.c is excluded):
 * feed the PRNG from entropy_pool(). */
static int shiro_entropy_seeder(const br_prng_class **ctx) {
    uint8_t pool[64];
    entropy_pool(pool);
    (*ctx)->update(ctx, pool, sizeof pool);
    return 1;
}

br_prng_seeder br_prng_seeder_system(const char **name) {
    if (name != NULL) *name = "shiro-rdrand-jitter-rtc";
    return &shiro_entropy_seeder;
}

/* ================================================================
 * Certificate validation time from the CMOS RTC.
 * br_x509_minimal_set_time() wants days since Jan 1 of year 0
 * (proleptic Gregorian) = unix_days + 719528 (verified against the
 * conversion constant inside x509_minimal.c) and seconds of day.
 * ================================================================ */
static uint32_t days_from_civil(int y, int m, int d) {
    /* Howard Hinnant's days_from_civil: days since 1970-01-01. */
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (uint32_t)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (uint32_t)d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468);
}

static void set_validation_time(void) {
    struct rtc_time rt;
    if (!rtc_read(&rt)) return; /* leave unset: validation fails closed */
    uint32_t days = days_from_civil(rt.year, rt.month, rt.day) + 719528u;
    uint32_t secs = (uint32_t)rt.hour * 3600u + (uint32_t)rt.min * 60u + (uint32_t)rt.sec;
    br_x509_minimal_set_time(&xc, days, secs);
}

/* ================================================================
 * sslio low-level I/O over net.c TCP
 * ================================================================ */
static int tls_low_read(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    int n = net_tcp_read_some(buf, len, 15);
    if (n > 0) return n;
    if (n == -2) transport_timeout = 1;
    else transport_eof = 1; /* FIN or reset: no more bytes, ever */
    return -1;
}

static int tls_low_write(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    if (net_tcp_write_all(buf, len)) return (int)len;
    return -1;
}

/* ================================================================
 * Error reporting
 * ================================================================ */
int tls_last_error(void) { return tls_err; }

void tls_print_error(void) {
    int err = tls_err;
    if (err >= 32) { /* BR_ERR_X509_* range */
        switch (err) {
        case BR_ERR_X509_NOT_TRUSTED:
            print("TLS: certificate not trusted (unknown CA).\n"); return;
        case BR_ERR_X509_EXPIRED:
            print("TLS: certificate expired or not yet valid.\n"); return;
        case BR_ERR_X509_DN_MISMATCH:
        case BR_ERR_X509_BAD_SERVER_NAME:
            print("TLS: certificate name does not match the host.\n"); return;
        case BR_ERR_X509_BAD_SIGNATURE:
            print("TLS: bad certificate signature.\n"); return;
        default:
            print("TLS: certificate validation failed (x509 error ");
            print_dec((uint32_t)err); print(").\n"); return;
        }
    }
    if (err == BR_ERR_IO && transport_timeout) {
        print("TLS: timed out waiting for the server.\n"); return;
    }
    print("TLS: handshake/connection failed (error ");
    print_dec((uint32_t)err); print(").\n");
}

/* ================================================================
 * Public API
 * ================================================================ */
static const char *suite_name(uint16_t id) {
    switch (id) {
    case 0xC02B: return "ECDHE-ECDSA-AES128-GCM-SHA256";
    case 0xC02C: return "ECDHE-ECDSA-AES256-GCM-SHA384";
    case 0xC02F: return "ECDHE-RSA-AES128-GCM-SHA256";
    case 0xC030: return "ECDHE-RSA-AES256-GCM-SHA384";
    case 0xCCA8: return "ECDHE-RSA-CHACHA20-POLY1305";
    case 0xCCA9: return "ECDHE-ECDSA-CHACHA20-POLY1305";
    case 0xC023: return "ECDHE-ECDSA-AES128-SHA";
    case 0xC024: return "ECDHE-ECDSA-AES256-SHA";
    case 0xC027: return "ECDHE-RSA-AES128-SHA256";
    case 0xC028: return "ECDHE-RSA-AES256-SHA384";
    case 0x009C: return "RSA-AES128-GCM-SHA256";
    case 0x009D: return "RSA-AES256-GCM-SHA384";
    case 0x002F: return "RSA-AES128-SHA";
    case 0x0035: return "RSA-AES256-SHA";
    default: return NULL;
    }
}

int tls_connect(const char *host, uint32_t ip, uint16_t port) {
    tls_err = 0; tls_active = 0;
    transport_eof = 0; transport_timeout = 0;
    if (!net_tcp_connect(ip, port)) { print("TCP connect failed.\n"); return 0; }

    br_ssl_client_init_full(&sc, &xc, tls_trust_anchors, TLS_TRUST_ANCHORS_NUM);
    set_validation_time();
    /* Extra explicit entropy injection (the seeder above also runs). */
    {
        uint8_t pool[64];
        entropy_pool(pool);
        br_ssl_engine_inject_entropy(&sc.eng, pool, sizeof pool);
    }
    br_ssl_engine_set_buffers_bidi(&sc.eng, tls_ibuf, sizeof tls_ibuf,
                                   tls_obuf, sizeof tls_obuf);
    if (br_ssl_client_reset(&sc, host, 0) == 0) {
        tls_err = br_ssl_engine_last_error(&sc.eng);
        tls_print_error();
        net_tcp_close();
        return 0;
    }
    br_sslio_init(&ioc, &sc.eng, tls_low_read, NULL, tls_low_write, NULL);
    if (br_sslio_flush(&ioc) < 0) { /* flush drives the handshake */
        tls_err = br_ssl_engine_last_error(&sc.eng);
        tls_print_error();
        net_tcp_close();
        return 0;
    }
    tls_active = 1;
    print("TLS: connected, ");
    unsigned ver = br_ssl_engine_get_version(&sc.eng);
    print(ver == BR_TLS12 ? "TLS 1.2" :
          ver == BR_TLS11 ? "TLS 1.1" :
          ver == BR_TLS10 ? "TLS 1.0" : "TLS ?");
    print(", ");
    br_ssl_session_parameters sp;
    br_ssl_engine_get_session_parameters(&sc.eng, &sp);
    const char *sn = suite_name(sp.cipher_suite);
    if (sn) print(sn);
    else { print("suite 0x"); print_hex32(sp.cipher_suite); }
    print(tls_used_rdrand ? " (rdrand seed)\n" : " (jitter seed, no rdrand)\n");
    return 1;
}

int tls_read(uint8_t *buf, size_t max) {
    if (!tls_active) return -1;
    int n = br_sslio_read(&ioc, buf, max);
    if (n > 0) return n;
    int err = br_ssl_engine_last_error(&sc.eng);
    if (err == BR_ERR_OK) return 0;                    /* close_notify */
    if (err == BR_ERR_IO && transport_eof) return 0;   /* abrupt FIN at a boundary */
    tls_err = err;
    return -1;
}

int tls_write_all(const uint8_t *buf, size_t len) {
    if (!tls_active) return 0;
    if (br_sslio_write_all(&ioc, buf, len) < 0) {
        tls_err = br_ssl_engine_last_error(&sc.eng);
        return 0;
    }
    /* br_sslio_write() only buffers into the engine; the record is
     * sealed and put on the wire by an explicit flush. */
    if (br_sslio_flush(&ioc) < 0) {
        tls_err = br_ssl_engine_last_error(&sc.eng);
        return 0;
    }
    return 1;
}

void tls_close(void) {
    if (tls_active) {
        br_sslio_close(&ioc); /* best-effort close_notify */
        tls_active = 0;
    }
    net_tcp_close();
}
