/* Shiro Mini OS - Multiboot2 boot-info parsing (v0.9).
 *
 * kernel_main calls boot_parse_mb2() when the loader magic is the
 * Multiboot2 one (0x36D76289). The parse runs before paging is set
 * up and copies everything the kernel will ever need out of the
 * loader's boot-information block into static storage:
 *   - RAM size (memory-map tag 6, or basic-meminfo tag 4),
 *   - the framebuffer tag (type 8): the GOP surface under UEFI,
 *   - whether this is a UEFI boot (EFI system-table tag 14/15).
 * Under a BIOS boot the same block arrives from GRUB's multiboot2
 * command and simply yields RAM info (no EFI tags, and the
 * framebuffer tag, if GRUB supplies one via VBE, is ignored by
 * the gfx layer in favor of its own mode programming).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os.h"

#define MB2_TAG_ALIGN 8

static uint32_t mb2_ram_top = 0;      /* bytes, 0 = unknown */
static int mb2_uefi = 0;
static struct gop_info mb2_gop;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

void boot_parse_mb2(uint32_t info_addr) {
    mb2_ram_top = 0;
    mb2_uefi = 0;
    mb2_gop.valid = 0;
    if (!info_addr) return;
    const uint8_t *base = (const uint8_t *)info_addr;
    uint32_t total = rd32(base);
    uint32_t off = 8;
    while (off + 8 <= total) {
        const uint8_t *tag = base + off;
        uint32_t type = rd32(tag);
        uint32_t size = rd32(tag + 4);
        if (type == 0) break;
        if (size < 8) break;
        switch (type) {
        case 4: /* basic memory info: mem_lower, mem_upper (KB) */
            if (size >= 16 && !mb2_ram_top)
                mb2_ram_top = 1024u * 1024u + rd32(tag + 12) * 1024u;
            break;
        case 6: { /* memory map */
            uint32_t esz = rd32(tag + 8);
            if (esz < 24) break;
            uint64_t top = 0;
            for (uint32_t o = 16; o + esz <= size; o += esz) {
                uint64_t b = rd64(tag + o);
                uint64_t l = rd64(tag + o + 8);
                uint32_t t = rd32(tag + o + 16);
                if (t == 1 && b + l > top) top = b + l;
            }
            if (top) mb2_ram_top = (uint32_t)top;
            break;
        }
        case 8: { /* framebuffer */
            if (size < 32) break;
            mb2_gop.addr = (uint32_t)rd64(tag + 8);
            mb2_gop.addr_hi = (uint32_t)(rd64(tag + 8) >> 32);
            mb2_gop.pitch = rd32(tag + 16);
            mb2_gop.width = rd32(tag + 20);
            mb2_gop.height = rd32(tag + 24);
            mb2_gop.bpp = tag[28];
            mb2_gop.fb_type = tag[29];
            if (size >= 38 && mb2_gop.fb_type == 1) {
                mb2_gop.r_pos = tag[32];
                mb2_gop.r_size = tag[33];
                mb2_gop.g_pos = tag[34];
                mb2_gop.g_size = tag[35];
                mb2_gop.b_pos = tag[36];
                mb2_gop.b_size = tag[37];
            }
            mb2_gop.valid = 1;
            break;
        }
        case 11: /* EFI 32-bit system table */
        case 12: /* EFI 64-bit system table */
            mb2_uefi = 1;
            break;
        /* NB: tags 14/15 are the ACPI RSDPs, which BIOS GRUB
         * also supplies — they say nothing about UEFI. */
        default:
            break;
        }
        off += (size + MB2_TAG_ALIGN - 1) & ~(uint32_t)(MB2_TAG_ALIGN - 1);
    }
    /* clamp to the memory manager's ceiling */
    if (mb2_ram_top > 512u * 1024 * 1024) mb2_ram_top = 512u * 1024 * 1024;
}

uint32_t mb2_ram_top_value(void) { return mb2_ram_top; }
int boot_is_uefi(void) { return mb2_uefi; }
const struct gop_info *boot_gop(void) { return &mb2_gop; }
