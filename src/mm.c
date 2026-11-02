/* Shiro Mini OS - memory management.
 * Own page directory: identity map of RAM with 4 MB PSE pages, except
 * the 4-8 MB window which uses a 4 KB page table (user programs live
 * there later). A bitmap frame allocator manages physical frames; a
 * first-fit free-list heap lives in a fixed identity-mapped arena.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os.h"

void *memcpy(void *dst, const void *src, size_t n); /* net.c */
void *memset(void *dst, int v, size_t n);           /* net.c */

#define PAGE_SIZE 4096u
#define MAX_RAM (512u * 1024 * 1024)
#define MAX_FRAMES (MAX_RAM / PAGE_SIZE)
#define USER_WINDOW_START 0x100000u
#define USER_WINDOW_END   0x800000u
#define HEAP_START 0x800000u
#define HEAP_SIZE  (16u * 1024 * 1024)
#define HEAP_END   (HEAP_START + HEAP_SIZE)

/* ---------- multiboot info ---------- */
struct multiboot_info {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
};
struct mmap_entry {
    uint32_t size;
    uint64_t base, length;
    uint32_t type;
};

static uint32_t ram_top = 32u * 1024 * 1024; /* fallback */
static uint32_t ram_mb = 32;

/* ---------- frame allocator (bitmap) ---------- */
static uint8_t frame_bitmap[MAX_FRAMES / 8];
static uint32_t frame_count = 0;

static void frame_mark(uint32_t frame, int used) {
    if (used) frame_bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
    else frame_bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}
static int frame_test(uint32_t frame) {
    return (frame_bitmap[frame / 8] >> (frame % 8)) & 1;
}
static uint32_t frame_alloc(void) {
    for (uint32_t f = 0; f < frame_count; f++)
        if (!frame_test(f)) { frame_mark(f, 1); return f * PAGE_SIZE; }
    return 0;
}
static void frame_free(uint32_t phys) { frame_mark(phys / PAGE_SIZE, 0); }

void mm_frame_stats(uint32_t *total, uint32_t *used, uint32_t *freec) {
    uint32_t u = 0;
    for (uint32_t f = 0; f < frame_count; f++) if (frame_test(f)) u++;
    *total = frame_count; *used = u; *freec = frame_count - u;
}
uint32_t mm_ram_mb(void) { return ram_mb; }

/* ---------- paging ---------- */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_table_low[1024] __attribute__((aligned(4096)));    /* 0-4 MB */
static uint32_t page_table_window[1024] __attribute__((aligned(4096))); /* 4-8 MB */

static void invlpg(uint32_t va) { __asm__ volatile ("invlpg (%0)" :: "r"(va) : "memory"); }

static uint32_t *pt_for(uint32_t va) {
    return (va < 0x400000u) ? page_table_low : page_table_window;
}

uint32_t mm_map_user_page(uint32_t va) {
    if (va < USER_WINDOW_START || va >= USER_WINDOW_END) return 0;
    uint32_t frame = frame_alloc();
    if (!frame) return 0;
    memset((void *)frame, 0, PAGE_SIZE); /* identity map still active */
    pt_for(va)[(va >> 12) & 0x3FF] = frame | 0x7; /* P|RW|User */
    invlpg(va);
    return frame;
}

void mm_restore_identity(uint32_t va) {
    if (va < USER_WINDOW_START || va >= USER_WINDOW_END) return;
    uint32_t *pt = pt_for(va);
    uint32_t idx = (va >> 12) & 0x3FF;
    uint32_t pte = pt[idx];
    uint32_t frame = pte & ~0xFFFu;
    if ((pte & 1) && frame != va) frame_free(frame);
    pt[idx] = va | 0x3; /* P|RW supervisor */
    invlpg(va);
}

/* ---------- heap (first-fit free list over a fixed arena) ---------- */
struct heap_block {
    uint32_t size; /* total size including header */
    struct heap_block *next;
    uint32_t free;
    uint32_t pad;
};
static struct heap_block *heap_head = NULL;

static void heap_init(void) {
    heap_head = (struct heap_block *)HEAP_START;
    heap_head->size = HEAP_SIZE;
    heap_head->next = NULL;
    heap_head->free = 1;
}

void *kmalloc(size_t n) {
    if (!heap_head) return NULL;
    uint32_t want = (uint32_t)((n + 7) & ~7u) + sizeof(struct heap_block);
    struct heap_block *b = heap_head;
    while (b) {
        if (b->free && b->size >= want) {
            if (b->size >= want + sizeof(struct heap_block) + 8) {
                struct heap_block *nb = (struct heap_block *)((uint8_t *)b + want);
                nb->size = b->size - want;
                nb->next = b->next;
                nb->free = 1;
                b->next = nb;
                b->size = want;
            }
            b->free = 0;
            return (uint8_t *)b + sizeof(struct heap_block);
        }
        b = b->next;
    }
    return NULL;
}

void kfree(void *p) {
    if (!p) return;
    struct heap_block *b = (struct heap_block *)((uint8_t *)p - sizeof(struct heap_block));
    b->free = 1;
    /* coalesce adjacent free blocks (list is address-ordered) */
    struct heap_block *cur = heap_head;
    while (cur && cur->next) {
        struct heap_block *nx = cur->next;
        if (cur->free && nx->free &&
            (uint8_t *)cur + cur->size == (uint8_t *)nx) {
            cur->size += nx->size;
            cur->next = nx->next;
        } else {
            cur = cur->next;
        }
    }
}

void mm_heap_stats(uint32_t *size, uint32_t *used, uint32_t *freec) {
    uint32_t u = 0;
    struct heap_block *b = heap_head;
    while (b) { if (!b->free) u += b->size; b = b->next; }
    *size = HEAP_SIZE; *used = u; *freec = HEAP_SIZE - u;
}

/* ---------- init ---------- */
void mm_init(uint32_t magic, uint32_t mb_addr) {
    uint32_t kend = (uint32_t)&kernel_end;
    if (magic == 0x2BADB002u && mb_addr) {
        struct multiboot_info *mb = (struct multiboot_info *)mb_addr;
        if (mb->flags & (1u << 6)) {
            uint32_t addr = mb->mmap_addr;
            uint32_t end = addr + mb->mmap_length;
            while (addr < end) {
                struct mmap_entry *e = (struct mmap_entry *)addr;
                if (e->type == 1) {
                    uint64_t top = e->base + e->length;
                    if (top > ram_top && top <= MAX_RAM) ram_top = (uint32_t)top;
                }
                addr += e->size + 4;
            }
        } else if (mb->flags & 1u) {
            ram_top = 1024 * 1024 + mb->mem_upper * 1024;
            if (ram_top > MAX_RAM) ram_top = MAX_RAM;
        }
    } else if (magic == 0x36D76289u) {
        /* Multiboot2: mb2.c already distilled the memory map */
        uint32_t t = mb2_ram_top_value();
        if (t) {
            ram_top = t;
            if (ram_top > MAX_RAM) ram_top = MAX_RAM;
        }
    }
    ram_mb = ram_top / (1024 * 1024);
    frame_count = ram_top / PAGE_SIZE;

    /* frame map: everything used except available RAM above kernel */
    for (uint32_t f = 0; f < frame_count; f++) frame_mark(f, 1);
    for (uint32_t pa = kend & ~0xFFFu; pa < ram_top; pa += PAGE_SIZE)
        frame_mark(pa / PAGE_SIZE, 0);
    /* reserve the user window's physical frames and the heap arena */
    for (uint32_t pa = USER_WINDOW_START; pa < HEAP_END; pa += PAGE_SIZE)
        frame_mark(pa / PAGE_SIZE, 1);

    /* page directory: full 4 GB identity map with 4 MB supervisor
     * pages, so RAM *and* PCI MMIO (e.g. the e1000 BAR at 0xFEBxxxxx)
     * stay reachable; only 0-8 MB uses 4 KB page tables so that user
     * pages can be mapped there (ELF load area) later. */
    for (int i = 0; i < 1024; i++)
        page_directory[i] = ((uint32_t)i << 22) | 0x83; /* P|RW|PS */
    for (int i = 0; i < 1024; i++) {
        page_table_low[i] = ((uint32_t)i * PAGE_SIZE) | 0x3;
        page_table_window[i] = (0x400000u + (uint32_t)i * PAGE_SIZE) | 0x3;
    }
    /* PDEs for the page-table ranges are user-permissive (0x7); the
     * PTEs decide access (supervisor 0x3 by default, user pages are
     * mapped with 0x7 explicitly). */
    page_directory[0] = ((uint32_t)page_table_low) | 0x7;    /* P|RW|U */
    page_directory[1] = ((uint32_t)page_table_window) | 0x7; /* P|RW|U */

    uint32_t cr3 = (uint32_t)page_directory;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3));
    uint32_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x10; /* PSE */
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000u; /* PG | WP */
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    heap_init();
}
