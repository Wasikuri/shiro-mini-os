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

#define USER_WINDOW_BASE 0x100000u   /* user window: 1 MB .. 8 MB VA */
#define USER_STACK_TOP 0x800000u
#define USER_STACK_PAGE 0x7FF000u

/* embedded sample program (ld -b binary, see Makefile) */
extern const uint8_t _binary_src_user_hello_start[];
extern const uint8_t _binary_src_user_hello_end[];

static volatile int user_exit_code = 0;

struct syscall_regs {
    uint32_t edi, esi, ebp, esp_, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags;
};

/* A user pointer may only be dereferenced after proving that
 * [ptr, ptr+len) lies entirely inside the memory a ring-3 program
 * can legitimately own: its loaded segments and its stack, i.e.
 * [kend, USER_STACK_TOP). Kernel memory below kend (the kernel
 * image itself) and anything below the window or at/above the
 * window top is rejected. The end computation is 64-bit so the
 * range cannot wrap. (v1.0.2: in v1.0.1 the write syscall below
 * dereferenced ebx unchecked — a kernel-memory disclosure.) */
static int user_range_ok(uint32_t ptr, uint32_t len) {
    if (len == 0) return 1;
    uint32_t kend = ((uint32_t)&kernel_end + 0xFFF) & ~0xFFFu;
    if (ptr < kend) return 0;
    return (uint64_t)ptr + (uint64_t)len <= (uint64_t)USER_STACK_TOP;
}

void syscall_handler(struct syscall_regs *r) {
    switch (r->eax) {
        case 1: { /* write(ebx=buf, ecx=len) */
            const char *buf = (const char *)(uintptr_t)r->ebx;
            uint32_t len = r->ecx;
            if (len > 8192) len = 8192;
            if (!user_range_ok(r->ebx, len)) { r->eax = (uint32_t)-1; break; }
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
    if (eh->phoff == 0 || eh->phnum == 0 || eh->phnum > 32 ||
        eh->phentsize < sizeof(struct elf32_phdr) ||
        (uint64_t)eh->phoff + (uint64_t)eh->phnum * eh->phentsize > len) {
        print("ELF: bad program headers.\n");
        return -1;
    }
    uint32_t kend = ((uint32_t)&kernel_end + 0xFFF) & ~0xFFFu;
    if (eh->entry < kend || eh->entry >= USER_STACK_TOP) {
        print("ELF: entry outside user window.\n");
        return -1;
    }

    /* Pass 1: validate EVERY loadable segment before anything is
     * mapped or copied. All range arithmetic is 64-bit. In v1.0.1
     * the segment end was computed in 32 bits, so a crafted p_memsz
     * wrapped it below the kernel, every check passed, the mapping
     * loop was skipped — and the copy loop still wrote p_filesz
     * bytes to the raw p_vaddr in ring 0 (red-team round 2/3:
     * confirmed privilege escalation; see docs/security-review.md).
     * A segment is now accepted only if its whole [vaddr, vend)
     * range sits inside the user window and clear of the kernel
     * image, and its file bytes lie inside the image. */
    const struct elf32_phdr *loads[32];
    int nloads = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf32_phdr *ph = (const struct elf32_phdr *)
            (img + eh->phoff + (uint32_t)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        if (ph->memsz == 0 && ph->filesz == 0) continue;
        uint64_t vend = (uint64_t)ph->vaddr + ph->memsz; /* exclusive */
        if (ph->vaddr < USER_WINDOW_BASE || vend > USER_STACK_TOP) {
            print("ELF: segment outside user window.\n");
            return -1;
        }
        if (ph->vaddr < kend) {
            print("ELF: segment overlaps kernel.\n");
            return -1;
        }
        if (ph->filesz > ph->memsz ||
            (uint64_t)ph->offset + ph->filesz > len) {
            print("ELF: segment out of bounds.\n");
            return -1;
        }
        loads[nloads++] = ph;
    }

    /* Pass 2: map the pages of each validated segment and copy its
     * file bytes (the rest of each fresh page is already zeroed). */
    mapped_count = 0;
    for (int s = 0; s < nloads; s++) {
        const struct elf32_phdr *ph = loads[s];
        uint32_t first = ph->vaddr & ~0xFFFu;
        uint32_t last = (uint32_t)(((uint64_t)ph->vaddr + ph->memsz - 1)
                                   & ~0xFFFu);
        for (uint32_t va = first; va <= last; va += 4096) {
            if (!map_page(va)) {
                print("ELF: out of memory.\n");
                unmap_all();
                return -1;
            }
        }
        memcpy((void *)(uintptr_t)ph->vaddr, img + ph->offset, ph->filesz);
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
