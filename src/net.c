/* Shiro Mini OS - minimal network stack, written from scratch
 * Intel e1000 NIC driver (polling) + Ethernet / ARP / IPv4 /
 * ICMP / UDP / DNS / TCP(client) / HTTP + tiny text browser
 * Address config via DHCP at boot (QEMU slirp hands out 10.0.2.15),
 * falling back to the static config 10.0.2.15/24, GW 10.0.2.2,
 * DNS 10.0.2.3 (also matches VirtualBox NAT).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "tls.h"

/* ---------- small libc bits (freestanding) ---------- */
/* Word-wise copies/fills (v0.8): the old byte loops made every
 * full-screen blit cost ~4x the bus traffic time under emulation,
 * which was a large part of the GUI stutter. Alignment is handled
 * by copying bytes until dst is 4-aligned, then 32-bit words when
 * src shares the alignment, then tail bytes. */
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n && ((uintptr_t)d & 3)) { *d++ = *s++; n--; }
    if (n >= 4 && (((uintptr_t)s & 3) == 0)) {
        uint32_t *dw = (uint32_t *)d;
        const uint32_t *sw = (const uint32_t *)s;
        size_t nw = n / 4;
        for (size_t i = 0; i < nw; i++) dw[i] = sw[i];
        d += nw * 4; s += nw * 4; n -= nw * 4;
    }
    while (n) { *d++ = *s++; n--; }
    return dst;
}
void *memset(void *dst, int v, size_t n) {
    uint8_t *d = dst;
    uint8_t b = (uint8_t)v;
    while (n && ((uintptr_t)d & 3)) { *d++ = b; n--; }
    if (n >= 4) {
        uint32_t wv = b | ((uint32_t)b << 8) | ((uint32_t)b << 16) |
                      ((uint32_t)b << 24);
        uint32_t *dw = (uint32_t *)d;
        size_t nw = n / 4;
        for (size_t i = 0; i < nw; i++) dw[i] = wv;
        d += nw * 4; n -= nw * 4;
    }
    while (n) { *d++ = b; n--; }
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else { for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1]; }
    return dst;
}
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int scmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
static int sncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0) return (uint8_t)a[i] - (uint8_t)b[i];
    }
    return 0;
}
static int lower_(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int cicmp(const char *a, const char *b, size_t n) { /* case-insensitive, b lowercase */
    for (size_t i = 0; i < n; i++) {
        if (lower_((uint8_t)a[i]) != (uint8_t)b[i]) return -1;
        if (a[i] == 0) return -1;
    }
    return 0;
}

/* ---------- port I/O ---------- */
static inline uint8_t inb_(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outb_(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline void outl_(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint32_t inl_(uint16_t port) {
    uint32_t r; __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static void io_wait(void) { inb_(0x80); }

/* ---------- RTC seconds (for timeouts, 1s granularity) ---------- */
static uint8_t cmos_read(uint8_t reg) { outb_(0x70, reg); return inb_(0x71); }
static uint32_t time_sec(void) {
    uint8_t b = cmos_read(0x0B);
    int binary = (b & 0x04) != 0;
    int h24 = (b & 0x02) != 0;
    uint8_t s1, s2, m, h;
    do {
        s1 = cmos_read(0x00); m = cmos_read(0x02); h = cmos_read(0x04);
        s2 = cmos_read(0x00);
    } while (s1 != s2);
    if (!binary) {
        s1 = (s1 & 0x0F) + ((s1 >> 4) * 10);
        m  = (m & 0x0F) + ((m >> 4) * 10);
        uint8_t pm = h & 0x80; h &= 0x7F;
        h = (h & 0x0F) + ((h >> 4) * 10);
        if (!h24 && pm && h != 12) h += 12;
        if (!h24 && !pm && h == 12) h = 0;
    }
    return (uint32_t)h * 3600 + (uint32_t)m * 60 + s1;
}
static int sec_elapsed(uint32_t start) {
    int d = (int)time_sec() - (int)start;
    if (d < 0) d += 86400;
    return d;
}

/* ---------- per-transfer deadline (NEXT-STEPS P0) ----------
 * A single transfer (currently: one browser image fetch) gets a
 * total budget in RTC seconds, covering DNS + TCP connect + TLS
 * + headers + body, so a stalled host cannot stretch the fetch
 * past the budget by dribbling bytes just inside the per-phase
 * idle timeouts. Set by the download wrapper below, cleared when
 * it returns; all other transfers leave it inactive (0). */
static int dl_deadline_active = 0;
static uint32_t dl_deadline_start = 0;
static int dl_deadline_secs = 0;
static int dl_expired(void) {
    return dl_deadline_active &&
           sec_elapsed(dl_deadline_start) >= dl_deadline_secs;
}

/* ---------- print helpers ---------- */
static void print_u32(uint32_t v) {
    char buf[12]; int i = 0;
    if (v == 0) { term_putchar('0'); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) term_putchar(buf[--i]);
}
static void print_ip(uint32_t ip) { /* host order a.b.c.d as stored */
    print_u32((ip >> 24) & 0xFF); term_putchar('.');
    print_u32((ip >> 16) & 0xFF); term_putchar('.');
    print_u32((ip >> 8) & 0xFF);  term_putchar('.');
    print_u32(ip & 0xFF);
}
static uint32_t parse_ip(const char *s) { /* returns 0 if not an IPv4 literal */
    uint32_t parts[4]; int p = 0; uint32_t cur = 0; int digits = 0;
    for (const char *c = s; ; c++) {
        if (*c >= '0' && *c <= '9') { cur = cur * 10 + (*c - '0'); digits++; }
        else if (*c == '.' && digits) {
            if (p >= 3) return 0;
            parts[p++] = cur; cur = 0; digits = 0;
        } else if (*c == 0 && digits && p == 3) {
            parts[3] = cur;
            return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
        } else return 0;
    }
}
static uint16_t htons_(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t htonl_(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
#define ntohs_ htons_
#define ntohl_ htonl_
static uint16_t ip_checksum(const void *data, size_t len) {
    const uint16_t *p = data; uint32_t sum = 0;
    while (len > 1) { sum += ntohs_(*p); p++; len -= 2; }
    if (len) sum += (uint32_t)(((const uint8_t *)p)[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    /* Words were summed in network byte order (via ntohs_), so the
     * complement IS the checksum value; callers store it big-endian.
     * (Previously this returned htons_(~sum), byte-swapping every
     * IPv4/ICMP checksum and making slirp drop all IP packets.) */
    return (uint16_t)(~sum);
}

/* ================================================================
 * PCI
 * ================================================================ */
static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFC);
    outl_(0xCF8, addr);
    return inl_(0xCFC);
}
static void pci_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFC);
    outl_(0xCF8, addr);
    outl_(0xCFC, v);
}

/* ================================================================
 * e1000 driver (polling)
 * ================================================================ */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EERD     0x0014
#define REG_CTRL_EXT 0x0018
#define REG_ICR      0x00C0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200
#define REG_RAL0     0x5400
#define REG_RAH0     0x5404

#define RX_DESC_COUNT 32
#define TX_DESC_COUNT 32
#define PKT_BUF_SIZE 2048

struct rx_desc { uint64_t addr; uint16_t length; uint16_t checksum; uint8_t status; uint8_t errors; uint16_t special; } __attribute__((packed));
struct tx_desc { uint64_t addr; uint16_t length; uint8_t cso; uint8_t cmd; uint8_t status; uint8_t css; uint16_t special; } __attribute__((packed));

static volatile uint32_t *nic; /* MMIO base */
static uint8_t my_mac[6];
static struct rx_desc rx_ring[RX_DESC_COUNT] __attribute__((aligned(16)));
static struct tx_desc tx_ring[TX_DESC_COUNT] __attribute__((aligned(16)));
static uint8_t rx_bufs[RX_DESC_COUNT][PKT_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_bufs[TX_DESC_COUNT][PKT_BUF_SIZE] __attribute__((aligned(16)));
static int rx_cur = 0, tx_cur = 0;
static int nic_present = 0;

static void nic_write(uint32_t reg, uint32_t v) { nic[reg / 4] = v; }
static uint32_t nic_read(uint32_t reg) { return nic[reg / 4]; }

static int e1000_init(void) {
    /* find Intel NIC (e1000 family) on PCI */
    uint8_t fbus = 0, fdev = 0, ffn = 0; int found = 0;
    for (int bus = 0; bus < 256 && !found; bus++)
        for (int dev = 0; dev < 32 && !found; dev++)
            for (int fn = 0; fn < 8 && !found; fn++) {
                uint32_t id = pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00);
                if ((id & 0xFFFF) != 0x8086) continue;
                uint16_t devid = (uint16_t)(id >> 16);
                if (devid == 0x100E || devid == 0x100F || devid == 0x10D3 || devid == 0x1015) {
                    fbus = (uint8_t)bus; fdev = (uint8_t)dev; ffn = (uint8_t)fn; found = 1;
                }
            }
    if (!found) return 0;
    uint32_t bar0 = pci_read(fbus, fdev, ffn, 0x10);
    nic = (volatile uint32_t *)(uintptr_t)(bar0 & 0xFFFFFFF0u);
    uint32_t cmd = pci_read(fbus, fdev, ffn, 0x04);
    pci_write(fbus, fdev, ffn, 0x04, cmd | 0x07); /* IO + MEM + bus master */

    /* reset */
    nic_write(REG_CTRL, nic_read(REG_CTRL) | (1u << 26));
    for (volatile int i = 0; i < 100000; i++) io_wait();
    nic_write(REG_IMC, 0xFFFFFFFF);
    (void)nic_read(REG_ICR);
    /* link up */
    nic_write(REG_CTRL, nic_read(REG_CTRL) | (1u << 6) | (1u << 5));
    /* clear multicast table */
    for (int i = 0; i < 128; i++) nic_write(REG_MTA + i * 4, 0);
    /* MAC from receive address registers */
    uint32_t ral = nic_read(REG_RAL0), rah = nic_read(REG_RAH0);
    my_mac[0] = ral & 0xFF; my_mac[1] = (ral >> 8) & 0xFF;
    my_mac[2] = (ral >> 16) & 0xFF; my_mac[3] = (ral >> 24) & 0xFF;
    my_mac[4] = rah & 0xFF; my_mac[5] = (rah >> 8) & 0xFF;

    /* RX ring */
    memset(rx_ring, 0, sizeof(rx_ring));
    for (int i = 0; i < RX_DESC_COUNT; i++)
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_bufs[i];
    nic_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    nic_write(REG_RDBAH, 0);
    nic_write(REG_RDLEN, RX_DESC_COUNT * 16);
    nic_write(REG_RDH, 0);
    nic_write(REG_RDT, RX_DESC_COUNT - 1);
    nic_write(REG_RCTL, (1u << 1) | (1u << 3) | (1u << 4) | (1u << 15)); /* EN|UPE|MPE|BAM */
    /* TX ring */
    memset(tx_ring, 0, sizeof(tx_ring));
    for (int i = 0; i < TX_DESC_COUNT; i++) {
        tx_ring[i].addr = (uint64_t)(uintptr_t)tx_bufs[i];
        tx_ring[i].status = 1;
    }
    nic_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    nic_write(REG_TDBAH, 0);
    nic_write(REG_TDLEN, TX_DESC_COUNT * 16);
    nic_write(REG_TDH, 0);
    nic_write(REG_TDT, 0);
    nic_write(REG_TIPG, 10 | (10 << 10) | (10 << 20));
    nic_write(REG_TCTL, (1u << 1) | (1u << 3)); /* EN | PSP */
    rx_cur = 0; tx_cur = 0;
    nic_present = 1;
    return 1;
}

static void eth_send(const uint8_t *frame, size_t len) {
    int idx = tx_cur;
    memcpy(tx_bufs[idx], frame, len);
    tx_ring[idx].length = (uint16_t)len;
    tx_ring[idx].cmd = 0x0B; /* EOP | IFCS | RS */
    tx_ring[idx].status = 0;
    tx_cur = (tx_cur + 1) % TX_DESC_COUNT;
    nic_write(REG_TDT, (uint32_t)tx_cur);
    uint32_t spins = 0;
    while (!(tx_ring[idx].status & 1) && spins++ < 1000000) io_wait();
}

/* forward decls of upper layers */
static void handle_frame(const uint8_t *f, size_t len);

static void net_poll(void) {
    if (!nic_present) return;
    for (int n = 0; n < 32; n++) {
        if (!(rx_ring[rx_cur].status & 1)) break;
        size_t len = rx_ring[rx_cur].length;
        if (len > PKT_BUF_SIZE) len = PKT_BUF_SIZE;
        handle_frame(rx_bufs[rx_cur], len);
        rx_ring[rx_cur].status = 0;
        nic_write(REG_RDT, (uint32_t)rx_cur);
        rx_cur = (rx_cur + 1) % RX_DESC_COUNT;
    }
}

/* ================================================================
 * Ethernet / ARP / IPv4
 * ================================================================ */
/* Runtime address config: defaults are the static config; the DHCP
 * client overwrites these at net init when it gets a lease. */
#define STATIC_IP   ((10u << 24) | (0u << 16) | (2u << 8) | 15u)
#define STATIC_GW   ((10u << 24) | (0u << 16) | (2u << 8) | 2u)
#define STATIC_DNS  ((10u << 24) | (0u << 16) | (2u << 8) | 3u)
static uint32_t cfg_ip_addr = STATIC_IP;
static uint32_t cfg_gw      = STATIC_GW;
static uint32_t cfg_dns     = STATIC_DNS;
static uint32_t cfg_mask    = 0xFFFFFF00u;
#define MY_IP   cfg_ip_addr
#define GATEWAY cfg_gw
#define DNS_SRV cfg_dns
#define NETMASK cfg_mask

static uint8_t arp_cache_mac[6];
static uint32_t arp_cache_ip = 0;
static int arp_cache_valid = 0;

static uint8_t tx_frame[1600];

static void eth_send_ip_packet(const uint8_t *dst_mac, uint16_t type, const uint8_t *payload, size_t len) {
    memcpy(tx_frame, dst_mac, 6);
    memcpy(tx_frame + 6, my_mac, 6);
    tx_frame[12] = (uint8_t)(type >> 8); tx_frame[13] = (uint8_t)(type & 0xFF);
    memcpy(tx_frame + 14, payload, len);
    eth_send(tx_frame, 14 + len);
}

static void ip_send_to(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, size_t len); /* fwd */

static int arp_resolve(uint32_t ip, uint8_t out_mac[6]) {
    if (arp_cache_valid && arp_cache_ip == ip) { memcpy(out_mac, arp_cache_mac, 6); return 1; }
    uint8_t req[42];
    memcpy(req, "\xFF\xFF\xFF\xFF\xFF\xFF", 6);
    memcpy(req + 6, my_mac, 6);
    req[12] = 0x08; req[13] = 0x06; /* ARP */
    uint8_t *a = req + 14;
    a[0] = 0; a[1] = 1;          /* htype ethernet */
    a[2] = 0x08; a[3] = 0x00;    /* ptype ipv4 */
    a[4] = 6; a[5] = 4;
    a[6] = 0; a[7] = 1;          /* request */
    memcpy(a + 8, my_mac, 6);
    a[14] = (uint8_t)(MY_IP >> 24); a[15] = (uint8_t)(MY_IP >> 16);
    a[16] = (uint8_t)(MY_IP >> 8); a[17] = (uint8_t)MY_IP;
    memset(a + 18, 0, 6);
    a[24] = (uint8_t)(ip >> 24); a[25] = (uint8_t)(ip >> 16); a[26] = (uint8_t)(ip >> 8); a[27] = (uint8_t)ip;
    uint32_t start = time_sec();
    int sent_at = -10;
    while (sec_elapsed(start) < 6) {
        if (sec_elapsed(start) != sent_at) { eth_send(req, 42); sent_at = sec_elapsed(start); }
        net_poll();
        if (arp_cache_valid && arp_cache_ip == ip) { memcpy(out_mac, arp_cache_mac, 6); return 1; }
    }
    return 0;
}

static void handle_arp(const uint8_t *f, size_t len) {
    if (len < 42) return;
    const uint8_t *a = f + 14;
    uint16_t oper = ((uint16_t)a[6] << 8) | a[7];
    uint32_t sender_ip = ((uint32_t)a[14] << 24) | ((uint32_t)a[15] << 16) | ((uint32_t)a[16] << 8) | a[17];
    uint32_t target_ip = ((uint32_t)a[24] << 24) | ((uint32_t)a[25] << 16) | ((uint32_t)a[26] << 8) | a[27];
    memcpy(arp_cache_mac, a + 8, 6);
    arp_cache_ip = sender_ip;
    arp_cache_valid = 1;
    if (oper == 1 && target_ip == MY_IP) {
        /* reply to ARP request for us */
        uint8_t rep[42];
        memcpy(rep, a + 8, 6); memcpy(rep + 6, my_mac, 6);
        rep[12] = 0x08; rep[13] = 0x06;
        uint8_t *r = rep + 14;
        memcpy(r, a, 28);
        r[6] = 0; r[7] = 2;
        memcpy(r + 8, my_mac, 6);
        r[14] = (uint8_t)(MY_IP >> 24); r[15] = (uint8_t)(MY_IP >> 16);
        r[16] = (uint8_t)(MY_IP >> 8); r[17] = (uint8_t)MY_IP;
        memcpy(r + 18, a + 8, 6);
        r[24] = (uint8_t)(sender_ip >> 24); r[25] = (uint8_t)(sender_ip >> 16);
        r[26] = (uint8_t)(sender_ip >> 8); r[27] = (uint8_t)sender_ip;
        eth_send(rep, 42);
    }
}

static uint16_t ip_id_counter = 1;
static void ip_send_to(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, size_t len) {
    uint32_t route_ip = ((dst_ip & NETMASK) == (MY_IP & NETMASK)) ? dst_ip : GATEWAY;
    uint8_t dst_mac[6];
    if (!arp_resolve(route_ip, dst_mac)) return;
    static uint8_t pkt[1560];
    size_t total = 20 + len;
    pkt[0] = 0x45; pkt[1] = 0;
    pkt[2] = (uint8_t)(total >> 8); pkt[3] = (uint8_t)(total & 0xFF);
    uint16_t id = ip_id_counter++;
    pkt[4] = (uint8_t)(id >> 8); pkt[5] = (uint8_t)(id & 0xFF);
    pkt[6] = 0x40; pkt[7] = 0; /* don't fragment */
    pkt[8] = 64; pkt[9] = proto;
    pkt[10] = 0; pkt[11] = 0;
    pkt[12] = (uint8_t)(MY_IP >> 24); pkt[13] = (uint8_t)(MY_IP >> 16);
    pkt[14] = (uint8_t)(MY_IP >> 8); pkt[15] = (uint8_t)MY_IP;
    pkt[16] = (uint8_t)(dst_ip >> 24); pkt[17] = (uint8_t)(dst_ip >> 16);
    pkt[18] = (uint8_t)(dst_ip >> 8); pkt[19] = (uint8_t)dst_ip;
    uint16_t c = ip_checksum(pkt, 20);
    pkt[10] = (uint8_t)(c >> 8); pkt[11] = (uint8_t)(c & 0xFF);
    memcpy(pkt + 20, payload, len);
    eth_send_ip_packet(dst_mac, 0x0800, pkt, total);
}

/* packet sinks filled by lower layers */
static volatile int ping_replied = 0;
static uint32_t ping_reply_from = 0;
static volatile int udp_got = 0;
static uint8_t udp_buf[2048]; static size_t udp_len = 0;
static uint16_t udp_src_port = 0, udp_dst_port_want = 0;
/* DHCP reply sink (UDP dst port 68) */
static volatile int dhcp_got = 0;
static uint8_t dhcp_buf[1500]; static size_t dhcp_len = 0;

static void handle_icmp(uint32_t src, const uint8_t *d, size_t len);
static void handle_udp(uint32_t src, const uint8_t *d, size_t len);
static void handle_tcp(uint32_t src, const uint8_t *d, size_t len);

static void handle_ip(const uint8_t *f, size_t len) {
    if (len < 14 + 20) return;
    const uint8_t *ip = f + 14;
    size_t ihl = (ip[0] & 0x0F) * 4;
    uint16_t total = ((uint16_t)ip[2] << 8) | ip[3];
    if (total > len - 14) total = (uint16_t)(len - 14);
    uint32_t dst = ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) | ((uint32_t)ip[18] << 8) | ip[19];
    uint32_t src = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) | ((uint32_t)ip[14] << 8) | ip[15];
    if (dst != MY_IP && dst != 0xFFFFFFFFu) return;
    const uint8_t *payload = ip + ihl;
    size_t plen = total - ihl;
    switch (ip[9]) {
        case 1:  handle_icmp(src, payload, plen); break;
        case 17: handle_udp(src, payload, plen); break;
        case 6:  handle_tcp(src, payload, plen); break;
        default: break;
    }
}

static void handle_frame(const uint8_t *f, size_t len) {
    if (len < 14) return;
    uint16_t type = ((uint16_t)f[12] << 8) | f[13];
    if (type == 0x0806) handle_arp(f, len);
    else if (type == 0x0800) handle_ip(f, len);
}

/* ================================================================
 * ICMP (ping)
 * ================================================================ */
static void handle_icmp(uint32_t src, const uint8_t *d, size_t len) {
    if (len < 8) return;
    if (d[0] == 0) { /* echo reply */
        uint16_t id = ((uint16_t)d[4] << 8) | d[5];
        if (id == 0x51C0) { ping_replied = 1; ping_reply_from = src; }
    }
}
static int icmp_ping(uint32_t dst) {
    uint8_t pkt[40];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 8; /* echo request */
    pkt[4] = 0x51; pkt[5] = 0xC0;
    pkt[6] = 0; pkt[7] = 1;
    memcpy(pkt + 8, "shiro-mini-os-ping!", 19);
    uint16_t c = ip_checksum(pkt, sizeof(pkt));
    pkt[2] = (uint8_t)(c >> 8); pkt[3] = (uint8_t)(c & 0xFF);
    ping_replied = 0;
    ip_send_to(dst, 1, pkt, sizeof(pkt));
    uint32_t start = time_sec();
    while (sec_elapsed(start) < 5) {
        net_poll();
        if (ping_replied) return 1;
    }
    return 0;
}

/* ================================================================
 * UDP + DNS
 * ================================================================ */
static void udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *data, size_t len) {
    static uint8_t u[1600];
    size_t total = 8 + len;
    u[0] = (uint8_t)(src_port >> 8); u[1] = (uint8_t)(src_port & 0xFF);
    u[2] = (uint8_t)(dst_port >> 8); u[3] = (uint8_t)(dst_port & 0xFF);
    u[4] = (uint8_t)(total >> 8); u[5] = (uint8_t)(total & 0xFF);
    u[6] = 0; u[7] = 0; /* checksum optional for IPv4 */
    memcpy(u + 8, data, len);
    ip_send_to(dst_ip, 17, u, total);
}
static void handle_udp(uint32_t src, const uint8_t *d, size_t len) {
    (void)src;
    if (len < 8) return;
    uint16_t sport = ((uint16_t)d[0] << 8) | d[1];
    uint16_t dport = ((uint16_t)d[2] << 8) | d[3];
    if (dport == 68) { /* DHCP reply */
        size_t plen = len - 8;
        if (plen > sizeof(dhcp_buf)) plen = sizeof(dhcp_buf);
        memcpy(dhcp_buf, d + 8, plen);
        dhcp_len = plen; dhcp_got = 1;
        return;
    }
    if (dport != udp_dst_port_want) return;
    size_t plen = len - 8;
    if (plen > sizeof(udp_buf)) plen = sizeof(udp_buf);
    memcpy(udp_buf, d + 8, plen);
    udp_len = plen; udp_src_port = sport; udp_got = 1;
}

/* ================================================================
 * DHCP client (DISCOVER/OFFER/REQUEST/ACK, broadcast)
 * ================================================================ */
static uint32_t dhcp_xid = 0x51A70001u;

/* Send a raw broadcast UDP packet with source IP 0.0.0.0 (pre-lease). */
static void dhcp_send_raw(const uint8_t *data, size_t len) {
    static uint8_t frame[1600];
    memset(frame, 0xFF, 6); /* dst MAC broadcast */
    memcpy(frame + 6, my_mac, 6);
    frame[12] = 0x08; frame[13] = 0x00;
    uint8_t *ip = frame + 14;
    size_t total = 20 + 8 + len;
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(total >> 8); ip[3] = (uint8_t)(total & 0xFF);
    uint16_t id = ip_id_counter++;
    ip[4] = (uint8_t)(id >> 8); ip[5] = (uint8_t)(id & 0xFF);
    ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = 17; /* TTL, UDP */
    ip[10] = 0; ip[11] = 0;
    ip[12] = 0; ip[13] = 0; ip[14] = 0; ip[15] = 0;          /* 0.0.0.0 */
    ip[16] = 0xFF; ip[17] = 0xFF; ip[18] = 0xFF; ip[19] = 0xFF; /* broadcast */
    uint16_t c = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)(c & 0xFF);
    uint8_t *u = ip + 20;
    u[0] = 0; u[1] = 68; /* client port */
    u[2] = 0; u[3] = 67; /* server port */
    size_t ulen = 8 + len;
    u[4] = (uint8_t)(ulen >> 8); u[5] = (uint8_t)(ulen & 0xFF);
    u[6] = 0; u[7] = 0;
    memcpy(u + 8, data, len);
    eth_send(frame, 14 + total);
}

static int dhcp_build(uint8_t *p, uint8_t msg_type, uint32_t req_ip, uint32_t server_id) {
    memset(p, 0, 300);
    p[0] = 1; p[1] = 1; p[2] = 6; p[3] = 0; /* op, htype, hlen, hops */
    p[4] = (uint8_t)(dhcp_xid >> 24); p[5] = (uint8_t)(dhcp_xid >> 16);
    p[6] = (uint8_t)(dhcp_xid >> 8); p[7] = (uint8_t)dhcp_xid;
    p[10] = 0x80; p[11] = 0x00; /* broadcast flag */
    memcpy(p + 28, my_mac, 6);  /* chaddr */
    p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99; /* magic cookie */
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = msg_type;
    if (req_ip) {
        p[o++] = 50; p[o++] = 4;
        p[o++] = (uint8_t)(req_ip >> 24); p[o++] = (uint8_t)(req_ip >> 16);
        p[o++] = (uint8_t)(req_ip >> 8); p[o++] = (uint8_t)req_ip;
    }
    if (server_id) {
        p[o++] = 54; p[o++] = 4;
        p[o++] = (uint8_t)(server_id >> 24); p[o++] = (uint8_t)(server_id >> 16);
        p[o++] = (uint8_t)(server_id >> 8); p[o++] = (uint8_t)server_id;
    }
    p[o++] = 55; p[o++] = 3; p[o++] = 1; p[o++] = 3; p[o++] = 6; /* want mask, router, DNS */
    p[o++] = 255;
    return 300; /* BOOTP minimum size */
}

/* Parse a DHCP reply; returns the message type or 0 if not ours. */
static int dhcp_parse(uint8_t *m, size_t len, uint32_t *yiaddr,
                      uint32_t *mask, uint32_t *gw, uint32_t *dns, uint32_t *server) {
    if (len < 240 || m[0] != 2) return 0;
    uint32_t xid = ((uint32_t)m[4] << 24) | ((uint32_t)m[5] << 16) |
                   ((uint32_t)m[6] << 8) | m[7];
    if (xid != dhcp_xid) return 0;
    if (m[236] != 99 || m[237] != 130 || m[238] != 83 || m[239] != 99) return 0;
    *yiaddr = ((uint32_t)m[16] << 24) | ((uint32_t)m[17] << 16) |
              ((uint32_t)m[18] << 8) | m[19];
    *server = ((uint32_t)m[20] << 24) | ((uint32_t)m[21] << 16) |
              ((uint32_t)m[22] << 8) | m[23]; /* siaddr */
    int type = 0;
    size_t o = 240;
    while (o < len) {
        uint8_t code = m[o];
        if (code == 255) break;
        if (code == 0) { o++; continue; }
        if (o + 1 >= len) break;
        uint8_t olen = m[o + 1];
        const uint8_t *v = m + o + 2;
        if (o + 2 + olen > len) break;
        if (code == 53 && olen >= 1) type = v[0];
        else if (code == 1 && olen == 4)
            *mask = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
        else if (code == 3 && olen >= 4)
            *gw = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
        else if (code == 6 && olen >= 4)
            *dns = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
        else if (code == 54 && olen == 4)
            *server = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
        o += 2 + olen;
    }
    return type;
}

/* Wait up to `secs` for a DHCP reply of the wanted type. */
static int dhcp_wait(int want_type, uint32_t *yiaddr, uint32_t *mask,
                     uint32_t *gw, uint32_t *dns, uint32_t *server, int secs) {
    uint32_t start = time_sec();
    while (sec_elapsed(start) < secs) {
        net_poll();
        if (dhcp_got) {
            dhcp_got = 0;
            int t = dhcp_parse(dhcp_buf, dhcp_len, yiaddr, mask, gw, dns, server);
            if (t == want_type) return 1;
        }
    }
    return 0;
}

static int dhcp_try(void) {
    uint8_t pkt[300];
    cfg_ip_addr = 0; /* no address until the lease is ACKed */
    for (int round = 0; round < 2; round++) {
        uint32_t yiaddr = 0, mask = 0, gw = 0, dns = 0, server = 0;
        dhcp_got = 0;
        int n = dhcp_build(pkt, 1 /* DISCOVER */, 0, 0);
        dhcp_send_raw(pkt, (size_t)n);
        if (!dhcp_wait(2 /* OFFER */, &yiaddr, &mask, &gw, &dns, &server, 4))
            continue;
        n = dhcp_build(pkt, 3 /* REQUEST */, yiaddr, server);
        dhcp_send_raw(pkt, (size_t)n);
        uint32_t y2 = 0, m2 = 0, g2 = 0, d2 = 0, s2 = 0;
        if (!dhcp_wait(5 /* ACK */, &y2, &m2, &g2, &d2, &s2, 4)) continue;
        cfg_ip_addr = y2 ? y2 : yiaddr;
        if (m2) cfg_mask = m2; else if (mask) cfg_mask = mask;
        if (g2) cfg_gw = g2; else if (gw) cfg_gw = gw;
        if (d2) cfg_dns = d2; else if (dns) cfg_dns = dns;
        arp_cache_valid = 0;
        return 1;
    }
    /* restore static fallback */
    cfg_ip_addr = STATIC_IP; cfg_gw = STATIC_GW;
    cfg_dns = STATIC_DNS; cfg_mask = 0xFFFFFF00u;
    return 0;
}

static int dns_skip_name(const uint8_t *m, int off, int mlen) {
    int guard = 0;
    while (off < mlen && guard++ < 64) {
        uint8_t c = m[off];
        if (c == 0) return off + 1;
        if ((c & 0xC0) == 0xC0) return off + 2;
        off += c + 1;
    }
    return off;
}
static int dns_build_query(const char *host, uint8_t *q) {
    int qlen = 0;
    q[qlen++] = 0x51; q[qlen++] = 0xA7; /* id */
    q[qlen++] = 0x01; q[qlen++] = 0x00; /* RD */
    q[qlen++] = 0; q[qlen++] = 1;       /* qd */
    q[qlen++] = 0; q[qlen++] = 0;
    q[qlen++] = 0; q[qlen++] = 0;
    q[qlen++] = 0; q[qlen++] = 0;
    const char *p = host;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int labellen = (int)(dot - p);
        if (labellen == 0) { p++; continue; }
        q[qlen++] = (uint8_t)labellen;
        for (int i = 0; i < labellen; i++) q[qlen++] = (uint8_t)p[i];
        p = dot;
        if (*p == '.') p++;
    }
    q[qlen++] = 0;
    q[qlen++] = 0; q[qlen++] = 1; /* A */
    q[qlen++] = 0; q[qlen++] = 1; /* IN */
    return qlen;
}

static int dns_parse_response(const uint8_t *m, int mlen, uint32_t *out_ip) {
    if (mlen < 12) return 0;
    int qd = ((int)m[4] << 8) | m[5];
    int an = ((int)m[6] << 8) | m[7];
    int off = 12;
    for (int i = 0; i < qd; i++) { off = dns_skip_name(m, off, mlen); off += 4; }
    for (int i = 0; i < an; i++) {
        off = dns_skip_name(m, off, mlen);
        if (off + 10 > mlen) return 0;
        int type = ((int)m[off] << 8) | m[off + 1];
        int rdlen = ((int)m[off + 8] << 8) | m[off + 9];
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= mlen) {
            *out_ip = ((uint32_t)m[off] << 24) | ((uint32_t)m[off + 1] << 16) |
                      ((uint32_t)m[off + 2] << 8) | m[off + 3];
            return 1;
        }
        off += rdlen;
    }
    return 0;
}

static int dns_resolve_udp(const uint8_t *q, int qlen, uint32_t *out_ip) {
    udp_dst_port_want = 53535;
    for (int attempt = 0; attempt < 3; attempt++) {
        udp_got = 0;
        udp_send(DNS_SRV, 53535, 53, q, (size_t)qlen);
        uint32_t start = time_sec();
        while (sec_elapsed(start) < 4 && !dl_expired()) {
            net_poll();
            if (!udp_got) continue;
            if (dns_parse_response(udp_buf, (int)udp_len, out_ip)) return 1;
            return 0; /* got a reply but no A record */
        }
    }
    return 0;
}

/* DNS over TCP fallback (RFC 7766 framing: 2-byte length prefix).
 * Used when the UDP resolver does not answer. The fallback server is
 * a resolver reachable at 10.0.2.100:8053 (e.g. provided by the NAT
 * setup / QEMU guestfwd in the test environment). Defined below with
 * the TCP client. */
static int dns_resolve_tcp(const uint8_t *q, int qlen, uint32_t *out_ip);

static int dns_resolve(const char *host, uint32_t *out_ip) {
    uint32_t lit = parse_ip(host);
    if (lit) { *out_ip = lit; return 1; }
    static uint8_t q[512];
    int qlen = dns_build_query(host, q);
    if (dns_resolve_udp(q, qlen, out_ip)) return 1;
    return dns_resolve_tcp(q, qlen, out_ip);
}

/* ================================================================
 * TCP (single client connection)
 * ================================================================ */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_RX_SIZE 65536
static uint8_t tcp_rxbuf[TCP_RX_SIZE];
static volatile size_t tcp_rxlen = 0;
static volatile int tcp_state = 0; /* 0 closed, 1 syn-sent, 2 established, 3 fin-received */
static volatile int tcp_error = 0;
static uint32_t tcp_remote_ip = 0;
static uint16_t tcp_remote_port = 0, tcp_local_port = 0;
static uint32_t tcp_snd_nxt = 0, tcp_rcv_nxt = 0, tcp_snd_una = 0;
static uint16_t tcp_next_src_port = 49152;

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg, size_t len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFF; sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF; sum += dst_ip & 0xFFFF;
    sum += 6; sum += (uint32_t)len;
    size_t i;
    for (i = 0; i + 1 < len; i += 2) sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
    if (i < len) sum += (uint32_t)seg[i] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void tcp_send_seg(uint8_t flags, const uint8_t *data, size_t dlen) {
    static uint8_t seg[1600];
    size_t total = 20 + dlen;
    seg[0] = (uint8_t)(tcp_local_port >> 8); seg[1] = (uint8_t)(tcp_local_port & 0xFF);
    seg[2] = (uint8_t)(tcp_remote_port >> 8); seg[3] = (uint8_t)(tcp_remote_port & 0xFF);
    uint32_t seq = tcp_snd_nxt;
    seg[4] = (uint8_t)(seq >> 24); seg[5] = (uint8_t)(seq >> 16); seg[6] = (uint8_t)(seq >> 8); seg[7] = (uint8_t)seq;
    uint32_t ack = (flags & TCP_ACK) ? tcp_rcv_nxt : 0;
    seg[8] = (uint8_t)(ack >> 24); seg[9] = (uint8_t)(ack >> 16); seg[10] = (uint8_t)(ack >> 8); seg[11] = (uint8_t)ack;
    seg[12] = 0x50; seg[13] = flags;
    seg[14] = 0xFF; seg[15] = 0xFF; /* window */
    seg[16] = 0; seg[17] = 0;
    seg[18] = 0; seg[19] = 0;
    if (dlen) memcpy(seg + 20, data, dlen);
    uint16_t c = tcp_checksum(MY_IP, tcp_remote_ip, seg, total);
    seg[16] = (uint8_t)(c >> 8); seg[17] = (uint8_t)(c & 0xFF);
    ip_send_to(tcp_remote_ip, 6, seg, total);
}

static void handle_tcp(uint32_t src, const uint8_t *d, size_t len) {
    if (len < 20) return;
    uint16_t sport = ((uint16_t)d[0] << 8) | d[1];
    uint16_t dport = ((uint16_t)d[2] << 8) | d[3];
    if (tcp_state == 0 || src != tcp_remote_ip || sport != tcp_remote_port || dport != tcp_local_port) return;
    uint32_t seq = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
    uint32_t ack = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) | ((uint32_t)d[10] << 8) | d[11];
    size_t hlen = ((d[12] >> 4) & 0xF) * 4;
    if (hlen > len) return;
    uint8_t flags = d[13];
    const uint8_t *data = d + hlen;
    size_t dlen = len - hlen;

    if (flags & TCP_RST) { tcp_error = 1; tcp_state = 0; return; }
    if (tcp_state == 1) { /* SYN-SENT */
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == tcp_snd_nxt + 1) {
            tcp_snd_nxt += 1; tcp_snd_una = tcp_snd_nxt;
            tcp_rcv_nxt = seq + 1;
            tcp_state = 2;
            tcp_send_seg(TCP_ACK, NULL, 0);
        }
        return;
    }
    if (tcp_state >= 2) {
        if ((int32_t)(ack - tcp_snd_una) > 0 && (int32_t)(ack - tcp_snd_nxt) <= 0)
            tcp_snd_una = ack;
        if (dlen > 0) {
            if (seq == tcp_rcv_nxt) {
                size_t space = TCP_RX_SIZE - tcp_rxlen;
                size_t copy = dlen < space ? dlen : space;
                if (copy) memcpy(tcp_rxbuf + tcp_rxlen, data, copy);
                tcp_rxlen += copy;
                tcp_rcv_nxt += (uint32_t)copy;
                if (copy < dlen) {
                    /* buffer full: don't advance past what we stored; ask again */
                }
            }
            /* ack current position (also handles retransmits/out-of-order) */
            tcp_send_seg(TCP_ACK, NULL, 0);
        }
        if (flags & TCP_FIN) {
            if (seq + (uint32_t)dlen == tcp_rcv_nxt) {
                tcp_rcv_nxt += 1;
                tcp_send_seg(TCP_ACK, NULL, 0);
                if (tcp_state == 2) tcp_state = 3;
            } else {
                tcp_send_seg(TCP_ACK, NULL, 0);
            }
        }
    }
}

static uint32_t isn_counter = 0x51A70000;
static int tcp_connect(uint32_t ip, uint16_t port) {
    tcp_remote_ip = ip; tcp_remote_port = port;
    tcp_local_port = tcp_next_src_port++;
    if (tcp_next_src_port > 61000) tcp_next_src_port = 49152;
    tcp_snd_nxt = isn_counter; isn_counter += 65536;
    tcp_snd_una = tcp_snd_nxt;
    tcp_rcv_nxt = 0; tcp_rxlen = 0; tcp_error = 0; tcp_state = 1;
    uint32_t start = time_sec();
    int last_sent = -10;
    while (sec_elapsed(start) < 12 && !dl_expired()) {
        if (tcp_state == 1 && sec_elapsed(start) - last_sent >= 2) {
            tcp_send_seg(TCP_SYN, NULL, 0);
            last_sent = sec_elapsed(start);
        }
        net_poll();
        if (tcp_state == 2) return 1;
        if (tcp_error) return 0;
    }
    tcp_state = 0;
    return 0;
}
static void tcp_send_data(const uint8_t *data, size_t len) {
    /* one segment is enough for our small HTTP requests */
    tcp_send_seg(TCP_PSH | TCP_ACK, data, len);
    tcp_snd_nxt += (uint32_t)len;
    uint32_t start = time_sec();
    int resent = 0;
    while (sec_elapsed(start) < 6 && !dl_expired()) {
        net_poll();
        if (tcp_snd_una >= tcp_snd_nxt) return;
        if (tcp_error) return;
        if (!resent && sec_elapsed(start) >= 3) {
            /* retransmit: step seq back by len for the resend */
            uint32_t save = tcp_snd_nxt;
            tcp_snd_nxt -= (uint32_t)len;
            tcp_send_seg(TCP_PSH | TCP_ACK, data, len);
            tcp_snd_nxt = save;
            resent = 1;
        }
    }
}
static void tcp_close(void) {
    if (tcp_state == 2 || tcp_state == 3) {
        tcp_send_seg(TCP_FIN | TCP_ACK, NULL, 0);
        tcp_snd_nxt += 1;
    }
    tcp_state = 0;
}

/* ---- byte-stream API over the single TCP connection (for tls.c) ---- */
int net_dns_resolve(const char *host, uint32_t *out_ip) {
    return dns_resolve(host, out_ip);
}
const uint8_t *net_mac_addr(void) { return my_mac; }
uint32_t net_local_ip(void) { return MY_IP; }
int net_tcp_connect(uint32_t ip, uint16_t port) { return tcp_connect(ip, port); }
void net_tcp_close(void) { tcp_close(); }

int net_tcp_write_all(const uint8_t *data, size_t len) {
    /* tcp_send_seg() uses a fixed 1600-byte segment buffer, so chunk
     * the stream into <=1400-byte sends. */
    while (len > 0) {
        size_t chunk = len > 1400 ? 1400 : len;
        tcp_send_data(data, chunk);
        if (tcp_error || tcp_state == 0) return 0;
        data += chunk; len -= chunk;
    }
    return 1;
}

int net_tcp_read_some(uint8_t *buf, size_t max, int timeout_s) {
    uint32_t start = time_sec();
    /* A per-transfer deadline caps this read too, so a TLS image
     * fetch cannot sit inside one 15 s BearSSL read past it. */
    int eff_timeout = timeout_s;
    if (dl_deadline_active) {
        int rem = dl_deadline_secs - sec_elapsed(dl_deadline_start);
        if (rem <= 0) return -2;
        if (rem < eff_timeout) eff_timeout = rem;
    }
    for (;;) {
        if (tcp_rxlen > 0) {
            size_t take = tcp_rxlen < max ? (size_t)tcp_rxlen : max;
            memcpy(buf, tcp_rxbuf, take);
            memmove(tcp_rxbuf, tcp_rxbuf + take, tcp_rxlen - take);
            tcp_rxlen -= take;
            return (int)take;
        }
        if (tcp_error) return -1;
        if (tcp_state == 3 || tcp_state == 0) return 0; /* FIN / closed */
        if (sec_elapsed(start) >= eff_timeout) return -2;
        if (dl_expired()) return -2;
        net_poll();
    }
}

/* DNS over TCP fallback server (see dns_resolve) */
static const uint32_t DNS_TCP_SRV = (10u << 24) | (0u << 16) | (2u << 8) | 100u;
#define DNS_TCP_PORT 8053

static int dns_resolve_tcp(const uint8_t *q, int qlen, uint32_t *out_ip) {
    if (!tcp_connect(DNS_TCP_SRV, DNS_TCP_PORT)) return 0;
    static uint8_t framed[520];
    framed[0] = (uint8_t)(qlen >> 8); framed[1] = (uint8_t)(qlen & 0xFF);
    memcpy(framed + 2, q, (size_t)qlen);
    tcp_send_data(framed, (size_t)qlen + 2);
    /* wait for the length-prefixed response */
    uint32_t start = time_sec();
    int ok = 0;
    while (sec_elapsed(start) < 10 && !dl_expired()) {
        net_poll();
        if (tcp_rxlen >= 2) {
            size_t mlen = ((size_t)tcp_rxbuf[0] << 8) | tcp_rxbuf[1];
            if (tcp_rxlen >= 2 + mlen) {
                ok = dns_parse_response(tcp_rxbuf + 2, (int)mlen, out_ip);
                break;
            }
        }
        if (tcp_error) break;
        if ((tcp_state == 3 || tcp_state == 0) && tcp_rxlen == 0) break;
    }
    tcp_close();
    return ok;
}

/* ================================================================
 * HTTP/HTTPS + tiny text browser (TLS via tls.c / BearSSL)
 * ================================================================ */
#define BODY_SIZE 65536
static char http_body[BODY_SIZE];
static char text_out[BODY_SIZE];

static const char *find_ci(const char *hay, const char *needle_lower) {
    size_t nl = slen(needle_lower);
    for (const char *p = hay; *p; p++)
        if (cicmp(p, needle_lower, nl) == 0) return p;
    return NULL;
}

static void print_paged(const char *s) {
    int lines = 0;
    for (const char *p = s; *p; p++) {
        term_putchar(*p);
        if (*p == '\n') {
            if (++lines >= 21) {
                lines = 0;
                print("-- more -- (any key)");
                (void)keyboard_getchar();
                print("\r                    \r");
            }
        }
    }
}

/* strip html -> plain text (handles script/style removal, basic entities) */
static void html_to_text(const char *in, size_t inlen, char *out) {
    size_t o = 0;
    int in_tag = 0, skip = 0; /* skip: 1=script 2=style */
    char tagbuf[32]; size_t tl = 0;
    for (size_t i = 0; i < inlen && o < BODY_SIZE - 8; i++) {
        char c = in[i];
        if (in_tag) {
            if (c == '>') {
                in_tag = 0;
                tagbuf[tl] = 0;
                /* decide skip mode + block-level newlines */
                if (sncmp(tagbuf, "script", 6) == 0) skip = 1;
                else if (sncmp(tagbuf, "/script", 7) == 0) skip = 0;
                else if (sncmp(tagbuf, "style", 5) == 0) skip = 2;
                else if (sncmp(tagbuf, "/style", 6) == 0) skip = 0;
                else if (!skip && (sncmp(tagbuf, "br", 2) == 0 || sncmp(tagbuf, "/p", 2) == 0 ||
                                   sncmp(tagbuf, "/div", 4) == 0 || sncmp(tagbuf, "/tr", 3) == 0 ||
                                   sncmp(tagbuf, "/li", 3) == 0 || sncmp(tagbuf, "/h1", 3) == 0 ||
                                   sncmp(tagbuf, "/h2", 3) == 0 || sncmp(tagbuf, "/h3", 3) == 0 ||
                                   sncmp(tagbuf, "p ", 2) == 0 || (tagbuf[0] == 'p' && tagbuf[1] == 0)))
                    out[o++] = '\n';
            } else if (tl < sizeof(tagbuf) - 1) {
                char lc = (char)lower_((uint8_t)c);
                if (lc == ' ') lc = ' ';
                tagbuf[tl++] = lc;
            }
            continue;
        }
        if (c == '<') {
            /* comment? */
            if (i + 3 < inlen && in[i + 1] == '!' && in[i + 2] == '-' && in[i + 3] == '-') {
                while (i + 2 < inlen && !(in[i] == '-' && in[i + 1] == '-' && in[i + 2] == '>')) i++;
                i += 2;
                continue;
            }
            in_tag = 1; tl = 0;
            continue;
        }
        if (skip) continue;
        if (c == '&') {
            if (sncmp(in + i, "&amp;", 5) == 0) { out[o++] = '&'; i += 4; continue; }
            if (sncmp(in + i, "&lt;", 4) == 0) { out[o++] = '<'; i += 3; continue; }
            if (sncmp(in + i, "&gt;", 4) == 0) { out[o++] = '>'; i += 3; continue; }
            if (sncmp(in + i, "&quot;", 6) == 0) { out[o++] = '"'; i += 5; continue; }
            if (sncmp(in + i, "&nbsp;", 6) == 0) { out[o++] = ' '; i += 5; continue; }
            if (sncmp(in + i, "&#39;", 5) == 0) { out[o++] = '\''; i += 4; continue; }
        }
        if (c == '\r') continue;
        if (c == ' ' || c == '\t') {
            if (o > 0 && out[o - 1] != ' ' && out[o - 1] != '\n') out[o++] = ' ';
            continue;
        }
        out[o++] = c;
    }
    /* trim trailing blank lines */
    while (o >= 2 && out[o - 1] == '\n' && out[o - 2] == '\n') o--;
    out[o] = 0;
}

static int http_get(const char *host, uint16_t port, const char *path, int depth);

/* --- connection staging: one byte source for plain TCP and TLS --- */
static int fetch_use_tls = 0;
static uint8_t conn_stage[8192];
static size_t conn_stage_len = 0;
static int conn_stage_eof = 0;  /* no more bytes will arrive */
static int conn_fatal = 0;      /* transport/TLS error ended the stream */

/* Pull more bytes into the (empty) stage. Returns 1 if the stage
 * holds bytes afterwards, 0 if not (EOF or nothing yet). */
static int conn_fill(void) {
    if (conn_stage_len > 0) return 1;
    if (conn_stage_eof) return 0;
    if (fetch_use_tls) {
        int n = tls_read(conn_stage, sizeof conn_stage);
        if (n > 0) { conn_stage_len = (size_t)n; return 1; }
        conn_stage_eof = 1;
        if (n < 0) conn_fatal = 1;
        return 0;
    }
    if (tcp_rxlen > 0) {
        size_t take = tcp_rxlen;
        if (take > sizeof conn_stage) take = sizeof conn_stage;
        memcpy(conn_stage, tcp_rxbuf, take);
        memmove(tcp_rxbuf, tcp_rxbuf + take, tcp_rxlen - take);
        tcp_rxlen -= take;
        conn_stage_len = take;
        return 1;
    }
    if (tcp_state == 3 || tcp_state == 0 || tcp_error) conn_stage_eof = 1;
    return 0;
}
static void conn_consume(size_t n) {
    memmove(conn_stage, conn_stage + n, conn_stage_len - n);
    conn_stage_len -= n;
}

static int http_fetch(const char *host, uint16_t port, const char *path, int depth, int use_tls) {
    uint32_t ip;
    fetch_use_tls = use_tls;
    conn_stage_len = 0; conn_stage_eof = 0; conn_fatal = 0;
    print("Resolving "); print(host); print(" ...\n");
    if (!dns_resolve(host, &ip)) { print("DNS failed.\n"); return 0; }
    print("Connecting to "); print_ip(ip); print(":"); print_u32(port); print(" ...\n");
    if (use_tls) {
        if (!tls_connect(host, ip, port)) return 0;
    } else if (!tcp_connect(ip, port)) { print("TCP connect failed.\n"); return 0; }
    static char req[1024];
    size_t rl = 0;
    const char *parts[4] = {"GET ", path, " HTTP/1.1\r\nHost: ", host};
    for (int i = 0; i < 4; i++) { size_t l = slen(parts[i]); memcpy(req + rl, parts[i], l); rl += l; }
    const char *tail = "\r\nUser-Agent: ShiroMiniOS/1.0.2\r\nConnection: close\r\n\r\n";
    size_t tl2 = slen(tail); memcpy(req + rl, tail, tl2); rl += tl2;
    if (use_tls) {
        if (!tls_write_all((const uint8_t *)req, rl)) {
            tls_print_error();
            tls_close();
            return 0;
        }
    } else {
        tcp_send_data((const uint8_t *)req, rl);
    }

    /* receive until FIN or idle timeout */
    size_t body_len = 0; int header_done = 0; size_t header_len = 0;
    uint32_t start = time_sec(); uint32_t last_data = time_sec();
    static char raw[BODY_SIZE];
    size_t raw_len = 0;
    int status = 0; int chunked = 0; char location[256]; location[0] = 0;
    while (sec_elapsed(start) < 40) {
        if (!fetch_use_tls) net_poll();
        if (conn_fill()) last_data = time_sec();
        while (conn_stage_len > 0) {
            size_t take = conn_stage_len;
            if (!header_done) {
                size_t want = take;
                if (raw_len + want > BODY_SIZE - 1) want = BODY_SIZE - 1 - raw_len;
                memcpy(raw + raw_len, conn_stage, want);
                raw_len += want;
                raw[raw_len] = 0;
                /* find header end */
                const char *he = NULL;
                for (size_t i = 0; i + 3 < raw_len; i++)
                    if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') { he = raw + i; break; }
                /* consumed all currently staged */
                conn_consume(take);
                if (he) {
                    header_done = 1;
                    header_len = (size_t)(he - raw) + 4;
                    /* status */
                    const char *sp = raw;
                    while (*sp && *sp != ' ') sp++;
                    if (*sp) status = 0, sp++;
                    while (*sp >= '0' && *sp <= '9') { status = status * 10 + (*sp - '0'); sp++; }
                    chunked = find_ci(raw, "transfer-encoding: chunked") != NULL ||
                              find_ci(raw, "transfer-encoding:chunked") != NULL;
                    const char *loc = find_ci(raw, "\nlocation:");
                    if (loc) {
                        loc += 10;
                        while (*loc == ' ') loc++;
                        size_t li = 0;
                        while (*loc && *loc != '\r' && *loc != '\n' && li < sizeof(location) - 1)
                            location[li++] = *loc++;
                        location[li] = 0;
                    }
                    print("HTTP "); print_u32((uint32_t)status); print("\n");
                    /* move remainder into body */
                    size_t rest = raw_len - header_len;
                    if (rest) memcpy(http_body, raw + header_len, rest);
                    body_len = rest;
                }
            } else {
                if (body_len + take > BODY_SIZE - 1) take = BODY_SIZE - 1 - body_len;
                memcpy(http_body + body_len, conn_stage, take);
                body_len += take;
                if (take == 0) take = conn_stage_len; /* body full: discard */
                conn_consume(take);
            }
        }
        if (conn_stage_eof) break; /* FIN, close_notify or fatal error */
        if (header_done && sec_elapsed(last_data) >= 8) break;
    }
    if (fetch_use_tls) tls_close(); else tcp_close();
    if (!header_done) {
        if (fetch_use_tls && conn_fatal && tls_last_error()) tls_print_error();
        else print("No HTTP response.\n");
        return 0;
    }
    http_body[body_len] = 0;

    if ((status == 301 || status == 302 || status == 307 || status == 308) && location[0] && depth < 3) {
        print("Redirect -> "); print(location); print("\n");
        if (location[0] == '/')
            return http_fetch(host, port, location, depth + 1, fetch_use_tls) || 1;
        return http_get(location, 80, "/", depth + 1) || 1;
    }
    if (chunked) {
        /* decode chunked body in place-ish */
        static char decoded[BODY_SIZE];
        size_t di = 0, i = 0;
        while (i < body_len) {
            size_t sz = 0; int any = 0;
            while (i < body_len && http_body[i] != '\r' && http_body[i] != '\n') {
                char c = http_body[i];
                int v = -1;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else if (c == ';') { while (i < body_len && http_body[i] != '\r' && http_body[i] != '\n') i++; break; }
                if (v >= 0) { sz = sz * 16 + (size_t)v; any = 1; }
                i++;
            }
            while (i < body_len && (http_body[i] == '\r' || http_body[i] == '\n')) i++;
            if (!any || sz == 0) break;
            if (i + sz > body_len) sz = body_len - i;
            if (di + sz > BODY_SIZE - 1) sz = BODY_SIZE - 1 - di;
            memcpy(decoded + di, http_body + i, sz);
            di += sz; i += sz;
            while (i < body_len && (http_body[i] == '\r' || http_body[i] == '\n')) i++;
        }
        decoded[di] = 0;
        memcpy(http_body, decoded, di + 1);
        body_len = di;
    }
    html_to_text(http_body, body_len, text_out);
    print("----- page text -----\n");
    print_paged(text_out);
    print("\n----- end -----\n");
    return 1;
}

/* Parse "[http|https]://host[:port]/path" (scheme may be omitted
 * = plain http); the scheme picks TLS on/off. Returns 1 on success. */
static int url_parse(const char *url, char *host, size_t hostmax,
                     uint16_t *port, char *path, size_t pathmax,
                     int *use_tls) {
    const char *p = url;
    *use_tls = 0;
    *port = 80;
    if (sncmp(p, "http://", 7) == 0) p += 7;
    else if (sncmp(p, "https://", 8) == 0) { p += 8; *use_tls = 1; *port = 443; }
    size_t hi = 0;
    while (*p && *p != '/' && *p != ':') { if (hi < hostmax - 1) host[hi++] = *p; p++; }
    host[hi] = 0;
    if (*p == ':') {
        p++; *port = 0;
        while (*p >= '0' && *p <= '9') { *port = (uint16_t)(*port * 10 + (*p - '0')); p++; }
    }
    if (*p == '/') {
        size_t pi = 0;
        while (*p) { if (pi < pathmax - 1) path[pi++] = *p; p++; }
        path[pi] = 0;
    } else { path[0] = '/'; path[1] = 0; }
    return host[0] != 0;
}

static int http_get(const char *url, uint16_t defport, const char *defpath, int depth) {
    (void)defport; (void)defpath;
    static char host[128]; static char path[512];
    uint16_t port; int use_tls;
    if (!url_parse(url, host, sizeof(host), &port, path, sizeof(path), &use_tls)) {
        print("Bad URL.\n");
        return 0;
    }
    return http_fetch(host, port, path, depth, use_tls);
}

/* ================================================================
 * Cookie jar (v1.0): in-memory only, keyed by host, cleared via
 * net_cookie_clear() when the GUI exits. Stores up to 8 hosts x
 * 4 name=value pairs from Set-Cookie headers (attributes after
 * the first ';' are dropped) and sends them on later requests
 * to the same host through the download core below.
 * ================================================================ */
#define COOKIE_HOSTS 8
#define COOKIE_PAIRS 4
static char cookie_hosts[COOKIE_HOSTS][128];
static char cookie_pairs[COOKIE_HOSTS][COOKIE_PAIRS][128];
static uint8_t cookie_npairs[COOKIE_HOSTS];
static int cookie_nhosts = 0;

void net_cookie_clear(void) { cookie_nhosts = 0; }

static int host_eq(const char *a, const char *b) {
    return slen(a) == slen(b) && sncmp(a, b, slen(a)) == 0;
}

static void cookie_store(const char *host, const char *value) {
    char pair[128];
    size_t n = 0;
    while (value[n] && value[n] != ';' && value[n] != '\r' && value[n] != '\n' &&
           n < sizeof(pair) - 1) { pair[n] = value[n]; n++; }
    while (n > 0 && pair[n - 1] == ' ') n--;
    pair[n] = 0;
    if (n == 0) return;
    size_t nl = 0;
    while (pair[nl] && pair[nl] != '=') nl++;
    if (nl == 0) return;
    int slot = -1;
    for (int i = 0; i < cookie_nhosts; i++)
        if (host_eq(cookie_hosts[i], host)) { slot = i; break; }
    if (slot < 0) {
        if (cookie_nhosts >= COOKIE_HOSTS) return;
        slot = cookie_nhosts++;
        size_t hl = slen(host);
        if (hl > sizeof(cookie_hosts[slot]) - 1) hl = sizeof(cookie_hosts[slot]) - 1;
        memcpy(cookie_hosts[slot], host, hl);
        cookie_hosts[slot][hl] = 0;
        cookie_npairs[slot] = 0;
    }
    for (int i = 0; i < cookie_npairs[slot]; i++) {
        const char *p = cookie_pairs[slot][i];
        size_t pl = 0;
        while (p[pl] && p[pl] != '=') pl++;
        if (pl == nl && sncmp(p, pair, nl) == 0) {
            memcpy(cookie_pairs[slot][i], pair, n + 1); /* replace */
            return;
        }
    }
    if (cookie_npairs[slot] < COOKIE_PAIRS) {
        memcpy(cookie_pairs[slot][cookie_npairs[slot]], pair, n + 1);
        cookie_npairs[slot]++;
    }
}

/* "name=value; name2=value2" for host into buf, or NULL if none. */
static const char *cookie_header_for(const char *host, char *buf, size_t bufsz) {
    for (int i = 0; i < cookie_nhosts; i++) {
        if (!host_eq(cookie_hosts[i], host) || cookie_npairs[i] == 0) continue;
        size_t o = 0;
        for (int j = 0; j < cookie_npairs[i]; j++) {
            const char *p = cookie_pairs[i][j];
            size_t pl = slen(p);
            size_t need = pl + (j ? 2 : 0);
            if (o + need >= bufsz) break;
            if (j) { memcpy(buf + o, "; ", 2); o += 2; }
            memcpy(buf + o, p, pl); o += pl;
        }
        buf[o] = 0;
        return o ? buf : NULL;
    }
    return NULL;
}

/* ================================================================
 * Streaming download (used by `install` in apps.c)
 * Same connection staging as http_fetch, but the response body is
 * delivered to a sink callback chunk by chunk instead of being
 * buffered into the 64 KB page buffer. Redirects are followed
 * like http_fetch (max 3). Content-Length, close-delimited and
 * chunked bodies are all supported.
 * Returns: 1 = HTTP 200 with the complete body delivered,
 *          0 = failure (message printed),
 *         -1 = the sink aborted the transfer,
 *          2 = HTTP 200, sink stopped the transfer early and the
 *              caller allowed truncation (net_download_stopok).
 * v1.0: also POST (net_post), the cookie jar above, and the
 * final URL after redirects (net_last_url).
 * ================================================================ */
struct dl_sink {
    int (*fn)(const void *data, size_t len, void *ctx);
    void *ctx;
};

/* one-shot transfer state (the stack runs one connection at a time) */
static const uint8_t *dl_post_data = NULL;
static uint32_t dl_post_len = 0;
static int dl_early_stop_ok = 0;
static char dl_last_url[640];

const char *net_last_url(void) { return dl_last_url; }

/* Body framer state: plain (Content-Length or close-delimited) or
 * a streaming chunked decoder. */
struct dl_body {
    struct dl_sink *sink;
    long cl;           /* Content-Length, -1 = until close */
    int chunked;
    uint32_t delivered;
    int state;         /* chunked: 0 size line, 1 data, 2 post-data
                          CRLF, 3 trailer lines */
    uint32_t left;     /* bytes left in the current chunk */
    char line[40];
    size_t linelen;
    int done;          /* body complete per its framing */
};

/* Feed body bytes; returns 0 if the sink aborted. */
static int dl_deliver(struct dl_body *b, const uint8_t *p, size_t n) {
    if (!b->chunked) {
        size_t take = n;
        if (b->cl >= 0) {
            uint32_t rem = (uint32_t)b->cl - b->delivered;
            if (take > rem) take = rem;
        }
        if (take > 0) {
            if (!b->sink->fn(p, take, b->sink->ctx)) return 0;
            b->delivered += (uint32_t)take;
        }
        if (b->cl >= 0 && b->delivered >= (uint32_t)b->cl) b->done = 1;
        return 1;
    }
    size_t i = 0;
    while (i < n && !b->done) {
        uint8_t c = p[i];
        switch (b->state) {
        case 0: /* chunk-size line (hex, optional ;extensions) */
            if (c == '\n') {
                uint32_t sz = 0; int any = 0;
                for (size_t j = 0; j < b->linelen; j++) {
                    char ch = b->line[j]; int v = -1;
                    if (ch >= '0' && ch <= '9') v = ch - '0';
                    else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
                    else break; /* ';' or '\r' ends the number */
                    sz = sz * 16 + (uint32_t)v; any = 1;
                }
                b->linelen = 0;
                if (!any) { b->done = 1; break; } /* garbage: stop */
                if (sz == 0) b->state = 3;
                else { b->left = sz; b->state = 1; }
            } else if (b->linelen < sizeof(b->line) - 1) {
                b->line[b->linelen++] = (char)c;
            }
            i++;
            break;
        case 1: { /* chunk data */
            size_t avail = n - i;
            size_t take = avail < b->left ? avail : b->left;
            if (!b->sink->fn(p + i, take, b->sink->ctx)) return 0;
            b->delivered += (uint32_t)take;
            i += take;
            b->left -= (uint32_t)take;
            if (b->left == 0) b->state = 2;
            break;
        }
        case 2: /* CRLF after chunk data: consume through '\n' */
            if (c == '\n') b->state = 0;
            i++;
            break;
        default: /* 3: trailer lines until an empty line */
            if (c == '\n') {
                if (b->linelen <= 1) b->done = 1; /* line was just "\r" */
                b->linelen = 0;
            } else if (b->linelen < sizeof(b->line) - 1) {
                b->line[b->linelen++] = (char)c;
            }
            i++;
            break;
        }
    }
    return 1;
}

static int dl_fetch(const char *host, uint16_t port, const char *path,
                    int use_tls, int depth, struct dl_sink *sink,
                    uint32_t *out_len);
static int dl_get(const char *url, int depth, struct dl_sink *sink,
                  uint32_t *out_len) {
    static char host[128]; static char path[512];
    uint16_t port; int use_tls;
    if (!url_parse(url, host, sizeof(host), &port, path, sizeof(path), &use_tls)) {
        print("Bad URL.\n");
        return 0;
    }
    return dl_fetch(host, port, path, use_tls, depth, sink, out_len);
}

static int dl_fetch(const char *host, uint16_t port, const char *path,
                    int use_tls, int depth, struct dl_sink *sink,
                    uint32_t *out_len) {
    uint32_t ip;
    fetch_use_tls = use_tls;
    conn_stage_len = 0; conn_stage_eof = 0; conn_fatal = 0;
    /* record the URL of this (possibly final) request */
    {
        size_t o = 0;
        const char *sch = use_tls ? "https://" : "http://";
        for (const char *p = sch; *p && o < sizeof(dl_last_url) - 1; p++)
            dl_last_url[o++] = *p;
        for (const char *p = host; *p && o < sizeof(dl_last_url) - 1; p++)
            dl_last_url[o++] = *p;
        if ((use_tls && port != 443) || (!use_tls && port != 80)) {
            char num[8]; int ni = 0; uint32_t v = port;
            dl_last_url[o++] = ':';
            do { num[ni++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && ni < 7);
            while (ni > 0 && o < sizeof(dl_last_url) - 1) dl_last_url[o++] = num[--ni];
        }
        for (const char *p = path; *p && o < sizeof(dl_last_url) - 1; p++)
            dl_last_url[o++] = *p;
        dl_last_url[o] = 0;
    }
    print("Resolving "); print(host); print(" ...\n");
    if (!dns_resolve(host, &ip)) { print("DNS failed.\n"); return 0; }
    print("Connecting to "); print_ip(ip); print(":"); print_u32(port); print(" ...\n");
    if (use_tls) {
        if (!tls_connect(host, ip, port)) return 0;
    } else if (!tcp_connect(ip, port)) { print("TCP connect failed.\n"); return 0; }
    static char req[1400];
    size_t rl = 0;
    const char *method = dl_post_data ? "POST " : "GET ";
    const char *parts[4] = {method, path, " HTTP/1.1\r\nHost: ", host};
    for (int i = 0; i < 4; i++) { size_t l = slen(parts[i]); memcpy(req + rl, parts[i], l); rl += l; }
    if (dl_post_data) {
        char num[12]; int ni = 0; uint32_t v = dl_post_len;
        const char *ph = "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ";
        size_t l = slen(ph); memcpy(req + rl, ph, l); rl += l;
        do { num[ni++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && ni < 11);
        while (ni > 0) req[rl++] = num[--ni];
    }
    {
        static char cbuf[600];
        const char *ck = cookie_header_for(host, cbuf, sizeof(cbuf));
        if (ck) {
            const char *ch = "\r\nCookie: ";
            size_t l = slen(ch); memcpy(req + rl, ch, l); rl += l;
            l = slen(ck); memcpy(req + rl, ck, l); rl += l;
        }
    }
    const char *tail = "\r\nUser-Agent: ShiroMiniOS/1.0.2\r\nConnection: close\r\n\r\n";
    size_t tl2 = slen(tail); memcpy(req + rl, tail, tl2); rl += tl2;
    if (use_tls) {
        if (!tls_write_all((const uint8_t *)req, rl)) {
            tls_print_error();
            tls_close();
            return 0;
        }
        if (dl_post_data && !tls_write_all(dl_post_data, dl_post_len)) {
            tls_print_error();
            tls_close();
            return 0;
        }
    } else {
        tcp_send_data((const uint8_t *)req, rl);
        if (dl_post_data) {
            const uint8_t *p = dl_post_data;
            uint32_t left = dl_post_len;
            while (left > 0) {
                size_t chunk = left > 1400 ? 1400 : left;
                tcp_send_data(p, chunk);
                if (tcp_error || tcp_state == 0) break;
                p += chunk; left -= (uint32_t)chunk;
            }
        }
    }

    /* --- headers: accumulate until CRLFCRLF (4 KB max) --- */
    static char hdr[4096];
    size_t hlen = 0, hdr_body_off = 0;
    int header_done = 0, hdr_failed = 0;
    uint32_t start = time_sec(); uint32_t last_data = time_sec();
    while (!header_done && sec_elapsed(start) < 40 && !dl_expired()) {
        if (!use_tls) net_poll();
        if (conn_fill()) last_data = time_sec();
        while (conn_stage_len > 0 && !header_done) {
            size_t want = conn_stage_len;
            if (hlen + want > sizeof(hdr) - 1) want = sizeof(hdr) - 1 - hlen;
            if (want == 0) { hdr_failed = 1; break; }
            memcpy(hdr + hlen, conn_stage, want);
            hlen += want;
            hdr[hlen] = 0;
            conn_consume(want);
            for (size_t i = 0; i + 3 < hlen; i++) {
                if (hdr[i] == '\r' && hdr[i + 1] == '\n' &&
                    hdr[i + 2] == '\r' && hdr[i + 3] == '\n') {
                    header_done = 1;
                    hdr_body_off = i + 4;
                    break;
                }
            }
        }
        if (hdr_failed || conn_stage_eof) break;
        if (sec_elapsed(last_data) >= 10) break;
    }
    if (!header_done) {
        if (use_tls) tls_close(); else tcp_close();
        if (use_tls && conn_fatal && tls_last_error()) tls_print_error();
        else if (hdr_failed) print("Download: response headers too large.\n");
        else print("No HTTP response.\n");
        return 0;
    }
    int status = 0;
    {
        const char *sp = hdr;
        while (*sp && *sp != ' ') sp++;
        if (*sp) sp++;
        while (*sp >= '0' && *sp <= '9') { status = status * 10 + (*sp - '0'); sp++; }
    }
    int chunked = find_ci(hdr, "transfer-encoding: chunked") != NULL ||
                  find_ci(hdr, "transfer-encoding:chunked") != NULL;
    long content_length = -1;
    const char *clp = find_ci(hdr, "\ncontent-length:");
    if (clp) {
        clp += 16; /* strlen("\ncontent-length:") */
        while (*clp == ' ') clp++;
        long v = 0; int any = 0;
        while (*clp >= '0' && *clp <= '9') { v = v * 10 + (*clp - '0'); clp++; any = 1; }
        if (any) content_length = v;
    }
    char location[256]; location[0] = 0;
    const char *loc = find_ci(hdr, "\nlocation:");
    if (loc) {
        loc += 10;
        while (*loc == ' ') loc++;
        size_t li = 0;
        while (*loc && *loc != '\r' && *loc != '\n' && li < sizeof(location) - 1)
            location[li++] = *loc++;
        location[li] = 0;
    }
    /* Set-Cookie: store pairs for this host (also on redirects) */
    {
        const char *scan = hdr;
        for (int k = 0; k < 4; k++) {
            const char *sc = find_ci(scan, "\nset-cookie:");
            if (!sc) break;
            sc += 12; /* strlen("\nset-cookie:") */
            while (*sc == ' ') sc++;
            cookie_store(host, sc);
            scan = sc;
        }
    }
    print("HTTP "); print_u32((uint32_t)status); print("\n");
    if ((status == 301 || status == 302 || status == 307 || status == 308) &&
        location[0] && depth < 3) {
        print("Redirect -> "); print(location); print("\n");
        if (use_tls) tls_close(); else tcp_close();
        /* 301/302/303: a POST is re-issued as GET (browser rule) */
        if (status != 307 && status != 308) { dl_post_data = NULL; dl_post_len = 0; }
        if (location[0] == '/')
            return dl_fetch(host, port, location, use_tls, depth + 1, sink, out_len);
        return dl_get(location, depth + 1, sink, out_len);
    }
    if (status != 200) {
        if (use_tls) tls_close(); else tcp_close();
        print("Download failed (HTTP status not 200).\n");
        return 0;
    }

    /* --- body: header leftovers first, then the staged stream --- */
    struct dl_body body;
    body.sink = sink; body.cl = content_length; body.chunked = chunked;
    body.delivered = 0; body.state = 0; body.left = 0;
    body.linelen = 0; body.done = 0;
    int aborted = 0;
    if (hdr_body_off < hlen) {
        if (!dl_deliver(&body, (const uint8_t *)hdr + hdr_body_off,
                        hlen - hdr_body_off))
            aborted = 1;
    }
    start = time_sec(); last_data = time_sec();
    while (!aborted && !body.done && sec_elapsed(start) < 120 &&
           !dl_expired()) {
        if (!use_tls) net_poll();
        if (conn_fill()) last_data = time_sec();
        while (conn_stage_len > 0 && !body.done) {
            size_t n = conn_stage_len;
            if (!dl_deliver(&body, conn_stage, n)) { aborted = 1; break; }
            conn_consume(n);
        }
        if (conn_stage_eof) break;
        if (sec_elapsed(last_data) >= 10) break;
    }
    if (use_tls) tls_close(); else tcp_close();
    if (out_len) *out_len = body.delivered;
    if (aborted) {
        /* sink said "enough": with the caller's permission that is
         * a successful (truncated) transfer, not an error */
        if (dl_early_stop_ok && status == 200) return 2;
        return -1;
    }
    if (body.chunked && body.state == 3 && conn_stage_eof)
        body.done = 1; /* peer closed right after the last chunk */
    /* A deadline break is a failure even for close-delimited
     * bodies, whose framing alone would otherwise report the
     * partial bytes as a complete transfer. */
    if (dl_expired() && !body.done) {
        print("Download incomplete (timeout).\n");
        return 0;
    }
    if (!body.done && (body.chunked || content_length >= 0)) {
        if (use_tls && conn_fatal && tls_last_error()) tls_print_error();
        print("Download incomplete.\n");
        return 0;
    }
    return 1;
}

int net_download(const char *url,
                 int (*sink)(const void *data, size_t len, void *ctx),
                 void *ctx, uint32_t *out_len) {
    struct dl_sink s;
    s.fn = sink; s.ctx = ctx;
    if (out_len) *out_len = 0;
    dl_post_data = NULL; dl_post_len = 0;
    dl_early_stop_ok = 0;
    return dl_get(url, 0, &s, out_len);
}

/* Like net_download, but a sink that stops the transfer early
 * (returns 0) yields success code 2 with the bytes delivered so
 * far instead of an abort. Used by the browser: once its page
 * buffer is full there is no point draining the rest of a huge
 * page through the narrow link. */
int net_download_stopok(const char *url,
                        int (*sink)(const void *data, size_t len, void *ctx),
                        void *ctx, uint32_t *out_len) {
    struct dl_sink s;
    s.fn = sink; s.ctx = ctx;
    if (out_len) *out_len = 0;
    dl_post_data = NULL; dl_post_len = 0;
    dl_early_stop_ok = 1;
    int r = dl_get(url, 0, &s, out_len);
    dl_early_stop_ok = 0;
    return r;
}

/* Like net_download_stopok, but the whole transfer (DNS +
 * connect + headers + body, across redirects) must finish within
 * timeout_s seconds of RTC time, or it fails with code 0. Used
 * by the browser for one image at a time: a stalled image host
 * must not hold up the rest of the page. The deadline is set
 * once here and survives the redirect recursion inside dl_get;
 * it is cleared on return so other transfers are unaffected. */
int net_download_stopok_timeout(const char *url,
                        int (*sink)(const void *data, size_t len, void *ctx),
                        void *ctx, uint32_t *out_len, int timeout_s) {
    struct dl_sink s;
    s.fn = sink; s.ctx = ctx;
    if (out_len) *out_len = 0;
    dl_post_data = NULL; dl_post_len = 0;
    dl_early_stop_ok = 1;
    dl_deadline_active = timeout_s > 0;
    dl_deadline_start = time_sec();
    dl_deadline_secs = timeout_s > 0 ? timeout_s : 0;
    int r = dl_get(url, 0, &s, out_len);
    dl_deadline_active = 0;
    dl_deadline_secs = 0;
    dl_early_stop_ok = 0;
    return r;
}

/* POST `body` (application/x-www-form-urlencoded) to url and
 * stream the response to the sink. Redirects follow the browser
 * rule: 301/302/303 re-issue as GET, 307/308 keep the body.
 * Return codes match net_download_stopok. */
int net_post(const char *url, const void *body, uint32_t body_len,
             int (*sink)(const void *data, size_t len, void *ctx),
             void *ctx, uint32_t *out_len) {
    struct dl_sink s;
    s.fn = sink; s.ctx = ctx;
    if (out_len) *out_len = 0;
    dl_post_data = (const uint8_t *)body;
    dl_post_len = body_len;
    dl_early_stop_ok = 1;
    int r = dl_get(url, 0, &s, out_len);
    dl_post_data = NULL; dl_post_len = 0;
    dl_early_stop_ok = 0;
    return r;
}

/* ================================================================
 * WHOIS client (RFC 3912, TCP port 43)
 * Query = one line of text; response = raw text until the server
 * closes the connection. Referral rule: if a response contains a
 * line starting with "refer:" (IANA style), the referral is
 * followed exactly once with the same query — also when an
 * explicit server was given, and even if the referral names the
 * same server (one repeat query, never a loop).
 * ================================================================ */
static char whois_resp[BODY_SIZE];

static const char *whois_token(const char *p, char *out, size_t max) {
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != ' ' && i < max - 1) out[i++] = *p++;
    out[i] = 0;
    return p;
}

/* Fetch one whois response from `server` into whois_resp.
 * Returns bytes received (>= 0), or -1 on error (message printed). */
static int whois_fetch(const char *server, const char *query) {
    uint32_t ip;
    print("Resolving "); print(server); print(" ...\n");
    if (!dns_resolve(server, &ip)) { print("DNS failed.\n"); return -1; }
    print("Connecting to "); print_ip(ip); print(":43 ...\n");
    if (!tcp_connect(ip, 43)) { print("TCP connect failed.\n"); return -1; }
    static char qbuf[300];
    size_t ql = 0;
    while (query[ql] && ql < sizeof(qbuf) - 3) { qbuf[ql] = query[ql]; ql++; }
    qbuf[ql++] = '\r'; qbuf[ql++] = '\n'; qbuf[ql] = 0;
    tcp_send_data((const uint8_t *)qbuf, ql);

    size_t len = 0;
    uint32_t start = time_sec(); uint32_t last_data = time_sec();
    while (sec_elapsed(start) < 40) {
        size_t before = tcp_rxlen;
        net_poll();
        if (tcp_rxlen != before) last_data = time_sec();
        while (tcp_rxlen > 0) {
            size_t take = tcp_rxlen;
            if (len + take > BODY_SIZE - 1) take = BODY_SIZE - 1 - len;
            memcpy(whois_resp + len, tcp_rxbuf, take);
            len += take;
            memmove(tcp_rxbuf, tcp_rxbuf + take, tcp_rxlen - take);
            tcp_rxlen -= take;
        }
        if (tcp_state == 3 || tcp_state == 0) break; /* FIN / closed */
        if (tcp_error) break;
        if (len > 0 && sec_elapsed(last_data) >= 6) break;  /* idle after data */
        if (len == 0 && sec_elapsed(last_data) >= 15) break; /* nothing came */
    }
    tcp_close();
    whois_resp[len] = 0;
    return (int)len;
}

/* Find a line starting with "refer:" at a line start; copy its value. */
static int whois_find_refer(const char *text, char *out, size_t max) {
    const char *p = text;
    while (*p) {
        if (cicmp(p, "refer:", 6) == 0) {
            p += 6;
            while (*p == ' ' || *p == '\t') p++;
            size_t i = 0;
            while (*p && *p != '\r' && *p != '\n' && *p != ' ' && *p != '\t' && i < max - 1)
                out[i++] = *p++;
            out[i] = 0;
            return i > 0;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

void cmd_whois(const char *args) {
    if (!nic_present) { print("No NIC.\n"); return; }
    static char query[128], server[128], refer[128];
    const char *rest = whois_token(args, query, sizeof(query));
    whois_token(rest, server, sizeof(server));
    if (query[0] == 0) { print("Usage: whois <query> [server]\n"); return; }
    if (server[0] == 0) {
        const char *def = "whois.iana.org";
        size_t i = 0; while (def[i]) { server[i] = def[i]; i++; } server[i] = 0;
    }
    for (int hop = 0; hop < 2; hop++) {
        int n = whois_fetch(server, query);
        if (n < 0) return;
        if (n == 0) {
            print("No response from "); print(server); print(".\n");
            return;
        }
        print("----- response from "); print(server); print(" -----\n");
        print_paged(whois_resp);
        if (whois_resp[n - 1] != '\n') print("\n");
        print("----- end -----\n");
        if (hop == 1) break; /* at most one referral follow */
        if (!whois_find_refer(whois_resp, refer, sizeof(refer))) break;
        print("Following referral to "); print(refer); print(" ...\n");
        size_t i = 0;
        while (refer[i] && i < sizeof(server) - 1) { server[i] = refer[i]; i++; }
        server[i] = 0;
    }
}

/* ================================================================
 * Shell commands
 * ================================================================ */
int net_ready(void) { return nic_present; }

void net_init(void) {
    if (e1000_init()) {
        print("Network: e1000 found, MAC ");
        for (int i = 0; i < 6; i++) {
            char hex[] = "0123456789ABCDEF";
            term_putchar(hex[(my_mac[i] >> 4) & 0xF]);
            term_putchar(hex[my_mac[i] & 0xF]);
            if (i < 5) term_putchar(':');
        }
        print("\nDHCP: requesting address ...\n");
        if (dhcp_try()) {
            print("DHCP: got "); print_ip(MY_IP);
            print(" (gw "); print_ip(GATEWAY);
            print(", dns "); print_ip(DNS_SRV); print(")\n");
        } else {
            print("DHCP: no answer, using static config.\n");
        }
        print("IP: "); print_ip(MY_IP);
        print("  GW: "); print_ip(GATEWAY);
        print("  DNS: "); print_ip(DNS_SRV);
        print("\n");
    } else {
        print("Network: no e1000 NIC found.\n");
    }
}

void cmd_ip(void) {
    if (!nic_present) { print("No NIC.\n"); return; }
    print("IP: "); print_ip(MY_IP); print(" mask "); print_ip(NETMASK); print("\n");
    print("Gateway: "); print_ip(GATEWAY); print("\n");
    print("DNS: "); print_ip(DNS_SRV); print("\n");
    if (arp_cache_valid) { print("ARP cache: "); print_ip(arp_cache_ip); print("\n"); }
}

void cmd_ping(const char *host) {
    if (!nic_present) { print("No NIC.\n"); return; }
    uint32_t ip;
    print("Resolving "); print(host); print(" ...\n");
    if (!dns_resolve(host, &ip)) { print("DNS failed.\n"); return; }
    print("Pinging "); print_ip(ip); print(" ...\n");
    if (icmp_ping(ip)) {
        print("Reply from "); print_ip(ping_reply_from); print("\n");
    } else {
        print("No reply (timeout).\n");
    }
}

void cmd_dns(const char *host) {
    if (!nic_present) { print("No NIC.\n"); return; }
    uint32_t ip;
    if (dns_resolve(host, &ip)) { print(host); print(" -> "); print_ip(ip); print("\n"); }
    else print("DNS failed.\n");
}

void cmd_browse(const char *url) {
    if (!nic_present) { print("No NIC.\n"); return; }
    http_get(url, 80, "/", 0);
}
