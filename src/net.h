#ifndef NET_H
#define NET_H

/* Console functions provided by kernel.c */
void print(const char *s);
void term_putchar(char c);
void term_clear(void);
char keyboard_getchar(void);

/* Network stack (net.c) */
void net_init(void);
int  net_ready(void);
void cmd_ip(void);
void cmd_ping(const char *host);
void cmd_dns(const char *host);
void cmd_browse(const char *url);
void cmd_whois(const char *args);

/* Streaming download (used by `install` in apps.c): GETs an
 * http(s) URL and delivers the body to sink() chunk by chunk, so
 * downloads are not limited by the 64 KB browse page buffer.
 * Returns 1 on success (HTTP 200, body complete per its framing),
 * 0 on failure (message printed), -1 when sink() aborted by
 * returning 0. *out_len (if non-NULL) receives the body size. */
int net_download(const char *url,
                 int (*sink)(const void *data, size_t len, void *ctx),
                 void *ctx, uint32_t *out_len);

/* v1.0 additions. net_download_stopok: like net_download, but a
 * sink returning 0 ("buffer full") ends the transfer as success
 * code 2 with the partial body, instead of abort code -1.
 * net_post: POST an application/x-www-form-urlencoded body and
 * stream the response (same return codes as _stopok).
 * net_last_url: the final URL of the last transfer, after
 * redirects. net_cookie_clear: empty the in-memory cookie jar. */
int net_download_stopok(const char *url,
                        int (*sink)(const void *data, size_t len, void *ctx),
                        void *ctx, uint32_t *out_len);
int net_download_stopok_timeout(const char *url,
                        int (*sink)(const void *data, size_t len, void *ctx),
                        void *ctx, uint32_t *out_len, int timeout_s);
int net_post(const char *url, const void *body, uint32_t body_len,
             int (*sink)(const void *data, size_t len, void *ctx),
             void *ctx, uint32_t *out_len);
const char *net_last_url(void);
void net_cookie_clear(void);

/* Byte-stream API over the single TCP connection (used by tls.c).
 * net_tcp_read_some: >0 bytes read, 0 = peer closed (FIN) with the
 * buffer drained, -1 = connection error/reset, -2 = timeout. */
int net_dns_resolve(const char *host, uint32_t *out_ip);
const uint8_t *net_mac_addr(void);
uint32_t net_local_ip(void);
int net_tcp_connect(uint32_t ip, uint16_t port);
void net_tcp_close(void);
int net_tcp_write_all(const uint8_t *data, size_t len);
int net_tcp_read_some(uint8_t *buf, size_t max, int timeout_s);

#endif
