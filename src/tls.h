#ifndef TLS_H
#define TLS_H

#include <stddef.h>
#include <stdint.h>

/* TLS client layer (tls.c, BearSSL engine over net.c TCP).
 * One connection at a time, like the rest of the network stack. */

/* Resolve + TCP connect + TLS handshake (SNI + certificate
 * validation against the bundled trust anchors). `host` is used for
 * SNI and hostname verification; `ip` is the already-resolved peer.
 * Prints status / error lines. Returns 1 on success. */
int tls_connect(const char *host, uint32_t ip, uint16_t port);

/* Read decrypted bytes: >0 = bytes read, 0 = clean end of stream
 * (close_notify or peer FIN at a record boundary), -1 = error
 * (see tls_last_error()). */
int tls_read(uint8_t *buf, size_t max);

/* Write all bytes encrypted. Returns 1 on success, 0 on error. */
int tls_write_all(const uint8_t *buf, size_t len);

/* Send close_notify and close the underlying TCP connection. */
void tls_close(void);

/* Last BearSSL error code (0 = none). */
int tls_last_error(void);

/* Print a human-readable line for tls_last_error(). */
void tls_print_error(void);

#endif
