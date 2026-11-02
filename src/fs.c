/* Shiro Mini OS - ATA PIO driver (primary master) + "SHFS" filesystem.
 *
 * On-disk layout (sector = 512 bytes):
 *   sector 0        superblock: magic 0x53484653 ("SHFS"), geometry
 *   sectors 1..8    directory: 64 entries x 64 bytes
 *                     { char name[28]; u32 used, start_sector,
 *                       size_bytes, alloc_sectors; u32 reserved[5] }
 *   sectors 9..12   allocation bitmap, 1 bit per sector
 *                   (covers up to 16384 sectors = 8 MB)
 *   sectors 16..    file data, allocated as contiguous runs
 * A file is one contiguous run of sectors. On first boot (no magic)
 * the disk is formatted automatically.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os.h"
#include "net.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int v, size_t n);

/* ---------- port I/O ---------- */
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r; __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(port));
}
static void io_wait(void) { inb(0x80); }

/* ================================================================
 * ATA PIO (primary bus, master drive)
 * ================================================================ */
#define ATA_DATA   0x1F0
#define ATA_COUNT  0x1F2
#define ATA_LBA_LO 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI 0x1F5
#define ATA_DRIVE  0x1F6
#define ATA_STATUS 0x1F7
#define ATA_CMD    0x1F7
#define ATA_ST_BSY 0x80
#define ATA_ST_DRDY 0x40
#define ATA_ST_DRQ 0x08
#define ATA_ST_ERR 0x01

static int ata_present = 0;
static uint32_t ata_sectors = 0;

static int ata_wait_not_busy(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st == 0xFF) return -1; /* floating bus: no disk */
        if (!(st & ATA_ST_BSY)) return 0;
        io_wait();
    }
    return -1;
}
static int ata_wait_drq(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & ATA_ST_ERR) return -1;
        if (st & ATA_ST_DRQ) return 0;
        io_wait();
    }
    return -1;
}

static inline void outl_(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint32_t inl_(uint16_t port) {
    uint32_t r; __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static uint32_t ata_pci_read(int dev, int off) {
    outl_(0xCF8, 0x80000000u | ((uint32_t)dev << 11) | (uint32_t)(off & 0xFC));
    return inl_(0xCFC);
}
/* UEFI firmware may leave the PCI IDE controller with its I/O
 * space disabled (legacy port reads then return 0x00 and every
 * command times out). Enable I/O decoding on any class-0101
 * controller that has it off. Under BIOS everything is already
 * enabled, so this is a no-op there. */
static void ata_pci_enable(void) {
    for (int dev = 0; dev < 32; dev++) {
        uint32_t id = ata_pci_read(dev, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        uint32_t cls = ata_pci_read(dev, 0x08) >> 8;
        if ((cls >> 16) == 0x01 && ((cls >> 8) & 0xFF) == 0x01) {
            uint32_t cmd = ata_pci_read(dev, 0x04);
            if (!(cmd & 0x1)) {
                outl_(0xCF8, 0x80000000u | ((uint32_t)dev << 11) | 0x04);
                outl_(0xCFC, cmd | 0x1);
            }
        }
    }
}

static int ata_init(void) {
    ata_pci_enable();
    /* Software reset (SRST): firmware may leave the device
     * mid-command; a BIOS-booted device is already ready, so
     * this is harmless there. nIEN stays set (we poll). */
    outb(0x3F6, 0x06);
    for (int i = 0; i < 1000; i++) io_wait();
    outb(0x3F6, 0x02);
    (void)ata_wait_not_busy();
    outb(ATA_DRIVE, 0xA0); /* master */
    for (int i = 0; i < 4; i++) (void)inb(ATA_STATUS); /* 400ns delay */
    uint8_t st = inb(ATA_STATUS);
    if (st == 0xFF || st == 0x00) return 0;
    /* IDENTIFY */
    if (ata_wait_not_busy() != 0) return 0;
    outb(ATA_COUNT, 0); outb(ATA_LBA_LO, 0); outb(ATA_LBA_MID, 0); outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, 0xEC);
    if (ata_wait_drq() != 0) return 0;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    ata_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (ata_sectors == 0) return 0;
    ata_present = 1;
    return 1;
}

static int ata_rw(uint32_t lba, int write, uint8_t *buf) {
    if (ata_wait_not_busy() != 0) return -1;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    for (int i = 0; i < 4; i++) (void)inb(ATA_STATUS);
    outb(ATA_COUNT, 1);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_CMD, write ? 0x30 : 0x20);
    if (ata_wait_drq() != 0) return -1;
    uint16_t *w = (uint16_t *)buf;
    if (write) {
        for (int i = 0; i < 256; i++) outw(ATA_DATA, w[i]);
        if (ata_wait_not_busy() != 0) return -1;
        outb(ATA_CMD, 0xE7); /* flush cache */
        if (ata_wait_not_busy() != 0) return -1;
    } else {
        for (int i = 0; i < 256; i++) w[i] = inw(ATA_DATA);
    }
    return 0;
}
static int ata_read(uint32_t lba, uint8_t *buf) { return ata_rw(lba, 0, buf); }
static int ata_write(uint32_t lba, const uint8_t *buf) {
    return ata_rw(lba, 1, (uint8_t *)buf);
}

/* ================================================================
 * SHFS
 * ================================================================ */
#define FS_MAGIC 0x53484653u /* "SHFS" */
#define DIR_START 1
#define DIR_SECTORS 8
#define MAX_FILES 64
#define BITMAP_START 9
#define BITMAP_SECTORS 4
#define DATA_START 16
#define MAX_FS_SECTORS 16384

struct superblock {
    uint32_t magic, total_sectors, dir_start, dir_sectors;
    uint32_t bitmap_start, bitmap_sectors, data_start, reserved[121];
};
struct dir_entry {
    char name[28];
    uint32_t used, start_sector, size_bytes, alloc_sectors;
    uint32_t reserved[5];
};

static int disk_present = 0;
static uint32_t fs_total = 0; /* sectors managed by the FS */
static struct dir_entry dir_cache[MAX_FILES];
static uint8_t bitmap_cache[BITMAP_SECTORS * 512];
static uint8_t sector_buf[512];

int fs_present(void) { return disk_present; }

static int bit_test(uint32_t s) { return (bitmap_cache[s / 8] >> (s % 8)) & 1; }
static void bit_set(uint32_t s, int v) {
    if (v) bitmap_cache[s / 8] |= (uint8_t)(1u << (s % 8));
    else bitmap_cache[s / 8] &= (uint8_t)~(1u << (s % 8));
}

static int fs_sync_metadata(void) {
    for (int i = 0; i < DIR_SECTORS; i++)
        if (ata_write(DIR_START + i, (const uint8_t *)dir_cache + i * 512) != 0)
            return -1;
    for (int i = 0; i < BITMAP_SECTORS; i++)
        if (ata_write(BITMAP_START + i, bitmap_cache + i * 512) != 0)
            return -1;
    return 0;
}

static int fs_format(void) {
    struct superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = FS_MAGIC;
    sb.total_sectors = fs_total;
    sb.dir_start = DIR_START; sb.dir_sectors = DIR_SECTORS;
    sb.bitmap_start = BITMAP_START; sb.bitmap_sectors = BITMAP_SECTORS;
    sb.data_start = DATA_START;
    memset(dir_cache, 0, sizeof(dir_cache));
    memset(bitmap_cache, 0, sizeof(bitmap_cache));
    for (uint32_t s = 0; s < DATA_START; s++) bit_set(s, 1);
    if (ata_write(0, (const uint8_t *)&sb) != 0) return -1;
    return fs_sync_metadata();
}

int fs_init(void) {
    if (!ata_init()) {
        print("Disk: none found.\n");
        return 0;
    }
    fs_total = ata_sectors < MAX_FS_SECTORS ? ata_sectors : MAX_FS_SECTORS;
    if (ata_read(0, sector_buf) != 0) { print("Disk: read error.\n"); return 0; }
    struct superblock *sb = (struct superblock *)sector_buf;
    if (sb->magic != FS_MAGIC) {
        if (fs_format() != 0) { print("Disk: format failed.\n"); return 0; }
        print("Disk: formatted new SHFS filesystem (");
        print_dec(fs_total / 2048); print(" MB).\n");
    } else {
        for (int i = 0; i < DIR_SECTORS; i++)
            ata_read(DIR_START + i, (uint8_t *)dir_cache + i * 512);
        for (int i = 0; i < BITMAP_SECTORS; i++)
            ata_read(BITMAP_START + i, bitmap_cache + i * 512);
        print("Disk: SHFS filesystem mounted.\n");
    }
    disk_present = 1;
    return 1;
}

static int dir_find(const char *name) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!dir_cache[i].used) continue;
        int j = 0;
        while (name[j] && dir_cache[i].name[j] && name[j] == dir_cache[i].name[j]) j++;
        if (name[j] == 0 && dir_cache[i].name[j] == 0) return i;
    }
    return -1;
}

static uint32_t alloc_run(uint32_t need) {
    uint32_t run_start = 0, run_len = 0;
    for (uint32_t s = DATA_START; s < fs_total; s++) {
        if (!bit_test(s)) {
            if (run_len == 0) run_start = s;
            if (++run_len >= need) {
                for (uint32_t k = run_start; k < run_start + need; k++) bit_set(k, 1);
                return run_start;
            }
        } else run_len = 0;
    }
    return 0;
}
static void free_run(uint32_t start, uint32_t n) {
    for (uint32_t k = start; k < start + n; k++) bit_set(k, 0);
}

int fs_write_file(const char *name, const char *data, uint32_t len) {
    if (!disk_present) return -1;
    if (len > 60000) return -1;
    uint32_t need = (len + 511) / 512;
    if (need == 0) need = 1;
    int idx = dir_find(name);
    if (idx < 0) {
        for (int i = 0; i < MAX_FILES; i++) if (!dir_cache[i].used) { idx = i; break; }
        if (idx < 0) return -1;
    } else {
        free_run(dir_cache[idx].start_sector, dir_cache[idx].alloc_sectors);
    }
    uint32_t start = alloc_run(need);
    if (!start) return -1;
    static uint8_t wbuf[512];
    for (uint32_t s = 0; s < need; s++) {
        uint32_t off = s * 512;
        uint32_t chunk = (len > off) ? (len - off) : 0;
        if (chunk > 512) chunk = 512;
        memset(wbuf, 0, 512);
        for (uint32_t i = 0; i < chunk; i++) wbuf[i] = (uint8_t)data[off + i];
        if (ata_write(start + s, wbuf) != 0) return -1;
    }
    struct dir_entry *e = &dir_cache[idx];
    int j = 0;
    while (name[j] && j < 27) { e->name[j] = name[j]; j++; }
    e->name[j] = 0;
    e->used = 1;
    e->start_sector = start;
    e->size_bytes = len;
    e->alloc_sectors = need;
    return fs_sync_metadata();
}

int fs_read_file(const char *name, char *buf, uint32_t maxlen) {
    if (!disk_present) return -1;
    int idx = dir_find(name);
    if (idx < 0) return -1;
    struct dir_entry *e = &dir_cache[idx];
    uint32_t n = e->size_bytes;
    if (n > maxlen) n = maxlen;
    static uint8_t rbuf[512];
    uint32_t done = 0;
    while (done < n) {
        if (ata_read(e->start_sector + done / 512, rbuf) != 0) return -1;
        uint32_t chunk = n - done; if (chunk > 512) chunk = 512;
        /* sector may start mid-buffer: files start at sector boundary */
        for (uint32_t i = 0; i < chunk; i++) buf[done + i] = (char)rbuf[i];
        done += chunk;
    }
    return (int)n;
}

int fs_remove_file(const char *name) {
    if (!disk_present) return -1;
    int idx = dir_find(name);
    if (idx < 0) return -1;
    free_run(dir_cache[idx].start_sector, dir_cache[idx].alloc_sectors);
    dir_cache[idx].used = 0;
    dir_cache[idx].name[0] = 0;
    return fs_sync_metadata();
}

int fs_rename_file(const char *oldn, const char *newn) {
    if (!disk_present) return -1;
    int idx = dir_find(oldn);
    if (idx < 0) return -1;
    if (dir_find(newn) >= 0) return -2;
    int len = 0;
    while (newn[len]) len++;
    if (len == 0 || len > 27) return -2;
    struct dir_entry *e = &dir_cache[idx];
    int j = 0;
    while (newn[j] && j < 27) { e->name[j] = newn[j]; j++; }
    e->name[j] = 0;
    return fs_sync_metadata();
}

int fs_file_size(const char *name) {
    if (!disk_present) return -1;
    int idx = dir_find(name);
    if (idx < 0) return -1;
    return (int)dir_cache[idx].size_bytes;
}

/* ---------- streaming write (used by `install`, see apps.c) ----------
 * fs_write_file() needs the whole file in RAM and is capped at
 * 60 KB. This API instead reserves a contiguous run of
 * ceil(max_len/512) sectors up front and writes sector by sector
 * as data arrives. Crash robustness: the directory entry is
 * claimed in the RAM cache only, and metadata reaches the disk
 * solely in fs_stream_commit() — a crash or abort before commit
 * leaves the on-disk directory and bitmap exactly as they were,
 * so a half-downloaded file never becomes visible. */
void fs_stream_abort(void); /* fwd (used by fs_stream_commit below) */
static int sw_active = 0;
static int sw_idx = -1;
static uint32_t sw_start = 0, sw_reserved = 0;
static uint32_t sw_total = 0;        /* bytes accepted so far */
static uint32_t sw_sectors_done = 0; /* full sectors already written */
static uint8_t sw_buf[512];
static uint32_t sw_buflen = 0;

int fs_stream_begin(const char *name, uint32_t max_len) {
    if (!disk_present || sw_active || max_len == 0) return -1;
    if (dir_find(name) >= 0) return -2; /* already exists */
    int idx = -1;
    for (int i = 0; i < MAX_FILES; i++) if (!dir_cache[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    uint32_t need = (max_len + 511) / 512;
    uint32_t start = alloc_run(need); /* marks the RAM bitmap only */
    if (!start) return -1;
    struct dir_entry *e = &dir_cache[idx];
    int j = 0;
    while (name[j] && j < 27) { e->name[j] = name[j]; j++; }
    e->name[j] = 0;
    e->used = 1; /* RAM only until commit */
    e->start_sector = start;
    e->size_bytes = 0;
    e->alloc_sectors = need;
    sw_active = 1; sw_idx = idx;
    sw_start = start; sw_reserved = need;
    sw_total = 0; sw_sectors_done = 0; sw_buflen = 0;
    return 0;
}

int fs_stream_write(const void *data, uint32_t len) {
    if (!sw_active) return -1;
    if (sw_total + len > sw_reserved * 512u) return -1;
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        uint32_t take = 512 - sw_buflen;
        if (take > len) take = len;
        for (uint32_t i = 0; i < take; i++) sw_buf[sw_buflen + i] = p[i];
        sw_buflen += take; p += take; len -= take; sw_total += take;
        if (sw_buflen == 512) {
            if (ata_write(sw_start + sw_sectors_done, sw_buf) != 0) return -1;
            sw_sectors_done++;
            sw_buflen = 0;
        }
    }
    return 0;
}

int fs_stream_commit(void) {
    if (!sw_active) return -1;
    if (sw_buflen > 0) {
        for (uint32_t i = sw_buflen; i < 512; i++) sw_buf[i] = 0;
        if (ata_write(sw_start + sw_sectors_done, sw_buf) != 0) {
            fs_stream_abort();
            return -1;
        }
        sw_sectors_done++;
    }
    uint32_t used = sw_sectors_done ? sw_sectors_done : 1;
    struct dir_entry *e = &dir_cache[sw_idx];
    e->size_bytes = sw_total;
    e->alloc_sectors = used;
    if (sw_reserved > used) free_run(sw_start + used, sw_reserved - used);
    sw_active = 0; sw_idx = -1;
    return fs_sync_metadata(); /* the one moment it becomes visible */
}

void fs_stream_abort(void) {
    if (!sw_active) return;
    free_run(sw_start, sw_reserved); /* RAM bitmap; disk never saw it */
    dir_cache[sw_idx].used = 0;
    dir_cache[sw_idx].name[0] = 0;
    sw_active = 0; sw_idx = -1;
}

int fs_list_name(int n, char *out, uint32_t max) {
    if (!disk_present || max == 0) return 0;
    int seen = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!dir_cache[i].used) continue;
        if (seen++ != n) continue;
        uint32_t j = 0;
        while (dir_cache[i].name[j] && j < max - 1) { out[j] = dir_cache[i].name[j]; j++; }
        out[j] = 0;
        return 1;
    }
    return 0;
}

void fs_cmd_ls(void) {
    int any = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!dir_cache[i].used) continue;
        any = 1;
        print("  "); print(dir_cache[i].name);
        print("  ("); print_dec(dir_cache[i].size_bytes); print(" bytes)\n");
    }
    if (!any) print("  (empty)\n");
}
