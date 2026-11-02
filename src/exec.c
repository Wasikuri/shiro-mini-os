/* Shiro Mini OS - ELF32 loader + user mode (ring 3).
 *
 * Loads a static 32-bit ELF executable into the user window
 * (1 MB..8 MB VA), maps fresh frames as user pages, adds a user
 * stack page at the top of the window, and enters ring 3 with iret
 * (see interrupt.s: enter_usermode). The program talks to the kernel
 * through int 0x80:
 *   eax=1  write(ebx=buf, ecx=len)   -> eax=len
 *   eax=2  read char                 -> eax=char (blocking)
 *   eax=3  exit(ebx=code)            -> never returns
 * On exit (or a user-mode fault) control returns to the shell.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os.h"
#include "net.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int v, size_t n);

extern int enter_usermode(uint32_t entry, uint32_t user_esp);
extern void exit_to_kernel(void);

/* embedded sample program (ld -b binary, see Makefile) */
extern const uint8_t _binary_src_user_hello_start[];
extern const uint8_t _binary_src_user_hello_end[];

static volatile int user_exit_code = 0;

struct syscall_regs {
    uint32_t edi, esi, ebp, esp_, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags;
};

void syscall_handler(struct syscall_regs *r) {
    switch (r->eax) {
        case 1: { /* write(ebx=buf, ecx=len) */
            const char *buf = (const char *)(uintptr_t)r->ebx;
            uint32_t len = r->ecx;
            if (len > 8192) len = 8192;
            for (uint32_t i = 0; i < len; i++) term_putchar(buf[i]);
            r->eax = len;
            break;
        }
        case 2: /* read char */
            r->eax = (uint8_t)keyboard_getchar();
            break;
        case 3: /* exit(ebx=code) */
            user_exit_code = (int)r->ebx;
            exit_to_kernel(); /* never returns */
            break;
        default:
            r->eax = (uint32_t)-1;
            break;
    }
}

/* called from the exception handler when ring-3 code faults */
void user_fault_exit(void) {
    user_exit_code = -1;
    exit_to_kernel(); /* never returns */
}

/* ---------- ELF loading ---------- */
struct elf32_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct elf32_phdr {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
#define PT_LOAD 1
#define USER_STACK_TOP 0x800000u
#define USER_STACK_PAGE 0x7FF000u
#define MAX_USER_PAGES 256

static uint32_t mapped_pages[MAX_USER_PAGES];
static int mapped_count = 0;

static int page_already_mapped(uint32_t va) {
    for (int i = 0; i < mapped_count; i++) if (mapped_pages[i] == va) return 1;
    return 0;
}
static int map_page(uint32_t va) {
    if (page_already_mapped(va)) return 1;
    if (mapped_count >= MAX_USER_PAGES) return 0;
    if (!mm_map_user_page(va)) return 0;
    mapped_pages[mapped_count++] = va;
    return 1;
}
static void unmap_all(void) {
    for (int i = 0; i < mapped_count; i++) mm_restore_identity(mapped_pages[i]);
    mapped_count = 0;
}

int run_elf_image(const uint8_t *img, uint32_t len) {
    if (len < sizeof(struct elf32_header)) { print("ELF: image too small.\n"); return -1; }
    const struct elf32_header *eh = (const struct elf32_header *)img;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 1) {
        print("ELF: bad magic/class.\n");
        return -1;
    }
    if (eh->type != 2 || eh->machine != 3) {
        print("ELF: not a 32-bit x86 executable.\n");
        return -1;
    }
    if (eh->phoff == 0 || eh->phnum == 0 || eh->phnum > 32) {
        print("ELF: bad program headers.\n");
        return -1;
    }
    uint32_t kend = ((uint32_t)&kernel_end + 0xFFF) & ~0xFFFu;
    mapped_count = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf32_phdr *ph = (const struct elf32_phdr *)
            (img + eh->phoff + (uint32_t)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        if (ph->memsz == 0) continue;
        uint32_t first = ph->vaddr & ~0xFFFu;
        uint32_t last = (ph->vaddr + ph->memsz - 1) & ~0xFFFu;
        if (first < 0x100000u || last >= USER_STACK_TOP) {
            print("ELF: segment outside user window.\n");
            unmap_all();
            return -1;
        }
        if (first < kend && last >= 0x100000u && first < kend) {
            print("ELF: segment overlaps kernel.\n");
            unmap_all();
            return -1;
        }
        if (ph->offset + ph->filesz > len || ph->filesz > ph->memsz) {
            print("ELF: segment out of bounds.\n");
            unmap_all();
            return -1;
        }
        for (uint32_t va = first; va <= last; va += 4096) {
            /* kernel image pages must never be remapped as user pages */
            if (va >= 0x100000u && va < kend) {
                print("ELF: segment overlaps kernel.\n");
                unmap_all();
                return -1;
            }
            if (!map_page(va)) {
                print("ELF: out of memory.\n");
                unmap_all();
                return -1;
            }
        }
        /* copy file bytes (rest of the pages is already zeroed) */
        const uint8_t *src = img + ph->offset;
        uint8_t *dst = (uint8_t *)(uintptr_t)ph->vaddr;
        for (uint32_t b = 0; b < ph->filesz; b++) dst[b] = src[b];
    }
    if (!map_page(USER_STACK_PAGE)) {
        print("ELF: no stack page.\n");
        unmap_all();
        return -1;
    }
    user_exit_code = 0;
    (void)enter_usermode(eh->entry, USER_STACK_TOP);
    int rc = user_exit_code;
    unmap_all();
    return rc;
}

int run_embedded_hello(void) {
    uint32_t size = (uint32_t)(_binary_src_user_hello_end - _binary_src_user_hello_start);
    return run_elf_image(_binary_src_user_hello_start, size);
}
