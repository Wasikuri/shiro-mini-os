/* Shiro Mini OS - CPU setup: own GDT (with user segments + TSS), IDT,
 * PIC remap, 100 Hz PIT timer, IRQ1 keyboard with ring buffer,
 * and exception handlers that report instead of triple-faulting.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os.h"

/* ---------- port I/O ---------- */
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

/* asm helpers (interrupt.s) */
extern void gdt_flush(uint32_t gdt_ptr);
extern void idt_load(uint32_t idt_ptr);
extern void tss_load(void);
extern void irq0_stub(void);
extern void irq1_stub(void);
extern void irq12_stub(void);
extern void irq_generic_stub(void);
extern void syscall_stub(void);
extern void exc0(void);  extern void exc1(void);  extern void exc2(void);
extern void exc3(void);  extern void exc4(void);  extern void exc5(void);
extern void exc6(void);  extern void exc7(void);  extern void exc8(void);
extern void exc9(void);  extern void exc10(void); extern void exc11(void);
extern void exc12(void); extern void exc13(void); extern void exc14(void);
extern void exc15(void); extern void exc16(void); extern void exc17(void);
extern void exc18(void); extern void exc19(void); extern void exc20(void);
extern void exc21(void); extern void exc22(void); extern void exc23(void);
extern void exc24(void); extern void exc25(void); extern void exc26(void);
extern void exc27(void); extern void exc28(void); extern void exc29(void);
extern void exc30(void); extern void exc31(void);

/* ================================================================
 * GDT
 * ================================================================ */
struct gdt_entry {
    uint16_t limit_low, base_low;
    uint8_t base_middle, access, granularity, base_high;
} __attribute__((packed));
struct gdt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

#define GDT_ENTRIES 6
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdtp;

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_low = base & 0xFFFF;
    gdt[i].base_middle = (base >> 16) & 0xFF;
    gdt[i].base_high = (base >> 24) & 0xFF;
    gdt[i].limit_low = limit & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access = access;
}

/* TSS */
struct tss_entry {
    uint32_t prev_tss, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));
static struct tss_entry tss;
static uint8_t syscall_stack[16384] __attribute__((aligned(16)));

/* ================================================================
 * IDT
 * ================================================================ */
struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t always0;
    uint8_t flags;
    uint16_t base_high;
} __attribute__((packed));
struct idt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

static void idt_set(int i, uint32_t handler, uint16_t sel, uint8_t flags) {
    idt[i].base_low = handler & 0xFFFF;
    idt[i].base_high = (handler >> 16) & 0xFFFF;
    idt[i].sel = sel;
    idt[i].always0 = 0;
    idt[i].flags = flags;
}

/* ================================================================
 * Exceptions
 * ================================================================ */
struct trap_regs {
    uint32_t edi, esi, ebp, esp_, ebx, edx, ecx, eax;
    uint32_t vec, err, eip, cs, eflags;
};

static const char *const exc_names[32] = {
    "#DE Divide Error", "#DB Debug", "NMI", "#BP Breakpoint",
    "#OF Overflow", "#BR Bound Range", "#UD Invalid Opcode", "#NM No Math Coprocessor",
    "#DF Double Fault", "Coprocessor Segment Overrun", "#TS Invalid TSS", "#NP Segment Not Present",
    "#SS Stack Fault", "#GP General Protection", "#PF Page Fault", "Reserved",
    "#MF Math Fault", "#AC Alignment Check", "#MC Machine Check", "#XM SIMD Exception",
    "#VE Virtualization", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
};

/* defined in exec.c (stage 7); weak so earlier stages link without it */
__attribute__((weak)) void user_fault_exit(void);

void exception_handler(struct trap_regs *r) {
    __asm__ volatile ("cli");
    print("\n*** EXCEPTION: ");
    if (r->vec < 32) print(exc_names[r->vec]); else print("Unknown");
    print(" (vector "); print_dec(r->vec);
    print(", error "); print_hex32(r->err);
    print(") at EIP "); print_hex32(r->eip);
    if (r->vec == 14) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        print(", fault addr "); print_hex32(cr2);
    }
    print(" ***\n");
    if ((r->cs & 3) == 3 && user_fault_exit) {
        print("User program faulted; returning to shell.\n");
        user_fault_exit(); /* never returns */
    }
    print("System halted.\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

/* ================================================================
 * Timer (PIT, 100 Hz)
 * ================================================================ */
volatile uint32_t timer_ticks = 0;

uint32_t uptime_seconds(void) { return timer_ticks / 100; }

void delay_ms(uint32_t ms) {
    uint32_t end = timer_ticks + (ms + 9) / 10;
    while ((int32_t)(timer_ticks - end) < 0)
        __asm__ volatile ("hlt");
}

/* ================================================================
 * CMOS RTC (ports 0x70/0x71) — wall-clock time.
 * QEMU's RTC runs on UTC. Returns broken-down local fields as the
 * RTC reports them (UTC on QEMU); 1 on success, 0 on read failure.
 * ================================================================ */
static uint8_t cmos_read(int reg) {
    outb(0x70, (uint8_t)reg);
    return inb(0x71);
}
static int cmos_bcd(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }

int rtc_read(struct rtc_time *t) {
    /* wait out any update-in-progress, with a bounded spin */
    int i;
    for (i = 0; i < 200000; i++)
        if (!(cmos_read(0x0A) & 0x80)) break;
    if (cmos_read(0x0A) & 0x80) return 0;
    uint8_t sec = cmos_read(0x00), min = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day = cmos_read(0x07), mon = cmos_read(0x08), yr = cmos_read(0x09);
    if (cmos_read(0x00) != sec) { /* an update raced us: read once more */
        sec = cmos_read(0x00); min = cmos_read(0x02); hour = cmos_read(0x04);
        day = cmos_read(0x07); mon = cmos_read(0x08); yr = cmos_read(0x09);
    }
    uint8_t regb = cmos_read(0x0B);
    int bcd = !(regb & 0x04);
    int h24 = (regb & 0x02) != 0;
    int pm = hour & 0x80;
    hour &= 0x7F;
    if (bcd) {
        sec = (uint8_t)cmos_bcd(sec); min = (uint8_t)cmos_bcd(min);
        hour = (uint8_t)cmos_bcd(hour); day = (uint8_t)cmos_bcd(day);
        mon = (uint8_t)cmos_bcd(mon); yr = (uint8_t)cmos_bcd(yr);
    }
    if (!h24) {
        if (pm && hour != 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }
    uint8_t cen = cmos_read(0x32); /* century (BCD on QEMU), may be 0 */
    if (bcd) cen = (uint8_t)cmos_bcd(cen);
    int year = (cen >= 19 && cen <= 99) ? (int)cen * 100 + yr : 2000 + yr;
    if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 61)
        return 0;
    t->year = year; t->month = mon; t->day = day;
    t->hour = hour; t->min = min; t->sec = sec;
    return 1;
}

/* ================================================================
 * Keyboard (IRQ1 -> ring buffer of key codes)
 * ================================================================ */
static const char sc_ascii[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};
static const char sc_ascii_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ',
};

#define KBD_BUF_SIZE 256
static volatile uint16_t kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head = 0, kbd_tail = 0;
static bool kbd_shift = false, kbd_caps = false, kbd_e0 = false;
static bool kbd_ctrl = false;

static void kbd_push(uint16_t key) {
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next == kbd_tail) return; /* full: drop */
    kbd_buf[kbd_head] = key;
    kbd_head = next;
}

void keyboard_irq_handler(void) {
    uint8_t sc = inb(0x60);
    outb(0x20, 0x20); /* EOI */
    if (sc == 0xE0) { kbd_e0 = true; return; }
    if (kbd_e0) {
        kbd_e0 = false;
        if (sc == 0x1D) { kbd_ctrl = true; return; }  /* right Ctrl make */
        if (sc == 0x9D) { kbd_ctrl = false; return; } /* right Ctrl break */
        if (sc & 0x80) return; /* extended break code */
        switch (sc) {
            case 0x48: kbd_push(KEY_UP); break;
            case 0x50: kbd_push(KEY_DOWN); break;
            case 0x4B: kbd_push(KEY_LEFT); break;
            case 0x4D: kbd_push(KEY_RIGHT); break;
            case 0x47: kbd_push(KEY_HOME); break;
            case 0x4F: kbd_push(KEY_END); break;
            case 0x49: kbd_push(KEY_PGUP); break;
            case 0x51: kbd_push(KEY_PGDN); break;
            case 0x53: kbd_push(KEY_DEL); break;
            default: break; /* fake shifts etc: ignore */
        }
        return;
    }
    if (sc == 0x1D) { kbd_ctrl = true; return; }
    if (sc == 0x9D) { kbd_ctrl = false; return; }
    if (sc == 0x2A || sc == 0x36) { kbd_shift = true; return; }
    if (sc == 0xAA || sc == 0xB6) { kbd_shift = false; return; }
    if (sc == 0x3A) { kbd_caps = !kbd_caps; return; }
    if (sc & 0x80) return;
    if (sc >= 128) return;
    char c = kbd_shift ? sc_ascii_shift[sc] : sc_ascii[sc];
    if (kbd_caps && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    else if (kbd_caps && kbd_shift && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (kbd_ctrl) {
        /* Ctrl+letter -> control code 0x01..0x1A (Ctrl+S = 0x13 etc.) */
        if (c >= 'a' && c <= 'z') { kbd_push((uint8_t)(c - 'a' + 1)); return; }
        if (c >= 'A' && c <= 'Z') { kbd_push((uint8_t)(c - 'A' + 1)); return; }
        return;
    }
    if (c) kbd_push((uint8_t)c);
}

int keyboard_try_key(void) {
    if (kbd_tail == kbd_head) return -1;
    int k = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return k;
}

int keyboard_read_key(void) {
    int k;
    while ((k = keyboard_try_key()) < 0)
        __asm__ volatile ("hlt");
    return k;
}

char keyboard_getchar(void) {
    for (;;) {
        int k = keyboard_read_key();
        if (k < 256) return (char)k;
    }
}

/* ================================================================
 * PS/2 mouse (IRQ12, 3-byte packets)
 * ================================================================ */
/* Clamp bounds are runtime values: 319x199 in text mode / mode 13h,
 * raised by the GUI when a larger VBE mode is active (v0.5). */
static volatile int mouse_max_x = 319, mouse_max_y = 199;

static volatile int mouse_x = 160, mouse_y = 100;
static volatile int mouse_btn = 0; /* bit0 left, bit1 right, bit2 middle */
static int mouse_present = 0;
static int mouse_cycle = 0;
static uint8_t mouse_pkt[3];
static volatile int mouse_pkts = 0, mouse_raw = 0;
static volatile uint8_t mouse_log[48];
static volatile int mouse_logn = 0;

void mouse_irq_handler(void) {
    uint8_t data = inb(0x60);
    mouse_raw++;
    if (mouse_logn < 48) mouse_log[mouse_logn++] = data;
    outb(0xA0, 0x20); /* EOI to slave PIC */
    outb(0x20, 0x20); /* EOI to master PIC */
    switch (mouse_cycle) {
        case 0:
            /* Sync: byte 0 has bit3 set. Also reject bytes with the
             * overflow bits set: a stray ACK (0xFA) has them set and
             * would otherwise be mistaken for a packet start, eating
             * the first real packet. QEMU chunks motion at +/-127, so
             * genuine packets from it never carry overflow bits. */
            if (!(data & 0x08) || (data & 0xC0)) return;
            mouse_pkt[0] = data;
            mouse_cycle = 1;
            return;
        case 1:
            mouse_pkt[1] = data;
            mouse_cycle = 2;
            return;
        default:
            mouse_pkt[2] = data;
            mouse_cycle = 0;
            break;
    }
    mouse_btn = mouse_pkt[0] & 0x07;
    mouse_pkts++;
    if (!(mouse_pkt[0] & 0xC0)) { /* no X/Y overflow */
        int dx = (int)mouse_pkt[1] - ((mouse_pkt[0] << 4) & 0x100);
        int dy = (int)mouse_pkt[2] - ((mouse_pkt[0] << 3) & 0x100);
        int nx = mouse_x + dx;
        int ny = mouse_y - dy; /* mouse up = +dy = screen up */
        if (nx < 0) nx = 0;
        if (nx > mouse_max_x) nx = mouse_max_x;
        if (ny < 0) ny = 0;
        if (ny > mouse_max_y) ny = mouse_max_y;
        mouse_x = nx;
        mouse_y = ny;
    }
}

void mouse_state(int *x, int *y, int *buttons) {
    *x = mouse_x;
    *y = mouse_y;
    *buttons = mouse_btn;
}

void mouse_set_bounds(int max_x, int max_y) {
    mouse_max_x = max_x;
    mouse_max_y = max_y;
    if (mouse_x > max_x) mouse_x = max_x;
    if (mouse_y > max_y) mouse_y = max_y;
}

void mouse_set_pos(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > mouse_max_x) x = mouse_max_x;
    if (y > mouse_max_y) y = mouse_max_y;
    mouse_x = x;
    mouse_y = y;
}

int mouse_ok(void) { return mouse_present; }

void mouse_debug(int *x, int *y, int *b, int *pkts, int *raw) {
    *x = mouse_x;
    *y = mouse_y;
    *b = mouse_btn;
    *pkts = mouse_pkts;
    *raw = mouse_raw;
}

int mouse_log_byte(int i) {
    if (i < 0 || i >= mouse_logn) return -1;
    return mouse_log[i];
}

/* Discard pending mouse bytes (e.g. a late ACK) so packet parsing starts
 * aligned on a byte-0 boundary. Only bytes flagged as aux-port data in
 * the status register are touched; keyboard bytes are left alone. */
void mouse_flush(void) {
    /* The mouse's ACK can arrive many poll-loops after the command was
     * sent, so drain in rounds separated by busy delays. */
    for (int r = 0; r < 25; r++) {
        while ((inb(0x64) & 0x21) == 0x21) { /* data ready + mouse port */
            (void)inb(0x60);
            mouse_cycle = 0;
        }
        for (volatile int d = 0; d < 20000; d++) (void)inb(0x64);
    }
    mouse_cycle = 0;
}

/* 8042 helpers (polling; used with interrupts off during init) */
static int kbc_wait_in(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02)) return 1;
    return 0;
}
static int kbc_wait_out(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 0x01) return 1;
    return 0;
}
static void kbc_cmd(uint8_t c) { if (kbc_wait_in()) outb(0x64, c); }
static void kbc_write60(uint8_t d) { if (kbc_wait_in()) outb(0x60, d); }
static int kbc_read60(void) {
    if (!kbc_wait_out()) return -1;
    return inb(0x60);
}

void mouse_init(void) {
    /* drain any stale byte (e.g. a leftover keyboard ACK) */
    while ((inb(0x64) & 0x01) && kbc_wait_out()) (void)inb(0x60);
    kbc_cmd(0xA8);                 /* enable auxiliary (mouse) device */
    kbc_cmd(0x20);                 /* read controller command byte */
    int cb = kbc_read60();
    if (cb < 0) cb = 0x47;
    cb |= 0x02;                    /* enable mouse IRQ12 */
    cb &= ~0x20;                   /* enable mouse clock */
    kbc_cmd(0x60);                 /* write command byte */
    kbc_write60((uint8_t)cb);
    kbc_cmd(0xD4);                 /* next byte goes to the mouse */
    kbc_write60(0xF6);             /* set defaults */
    (void)kbc_read60();            /* ACK (0xFA), best effort */
    kbc_cmd(0xD4);
    kbc_write60(0xF4);             /* enable data reporting */
    (void)kbc_read60();            /* ACK, best effort */
    mouse_x = 160;
    mouse_y = 100;
    mouse_btn = 0;
    mouse_cycle = 0;
    mouse_present = 1;
    mouse_flush(); /* drop the ACK bytes so the first packet is aligned */
}

/* ================================================================
 * PIC + PIT + init
 * ================================================================ */
#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

static void pic_remap(void) {
    uint8_t m1 = inb(PIC1_DATA), m2 = inb(PIC2_DATA);
    outb(PIC1_CMD, 0x11); outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, 0x20); outb(PIC2_DATA, 0x28); /* offsets 32, 40 */
    outb(PIC1_DATA, 0x04); outb(PIC2_DATA, 0x02); /* cascade */
    outb(PIC1_DATA, 0x01); outb(PIC2_DATA, 0x01); /* 8086 mode */
    (void)m1; (void)m2;
    outb(PIC1_DATA, 0xF8); /* enable IRQ0 (timer), IRQ1 (kbd), IRQ2 (cascade) */
    outb(PIC2_DATA, 0xEF); /* enable IRQ12 (mouse) on the slave PIC */
}

static void pit_init(void) {
    uint16_t divisor = 11931; /* 1193182 / 100 Hz */
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

void cpu_init(void) {
    /* GDT: null, kernel code/data, user code/data, TSS */
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* kernel code */
    gdt_set(2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* kernel data */
    gdt_set(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); /* user code */
    gdt_set(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); /* user data */
    for (size_t i = 0; i < sizeof(tss); i++) ((uint8_t *)&tss)[i] = 0;
    tss.esp0 = (uint32_t)(syscall_stack + sizeof(syscall_stack));
    tss.ss0 = 0x10;
    tss.iomap_base = sizeof(tss);
    gdt_set(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x40);
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uint32_t)&gdt;
    gdt_flush((uint32_t)&gdtp);
    tss_load();

    /* IDT */
    for (int i = 0; i < 256; i++) idt_set(i, 0, 0, 0);
    static void *const exc_stubs[32] = {
        exc0, exc1, exc2, exc3, exc4, exc5, exc6, exc7, exc8, exc9,
        exc10, exc11, exc12, exc13, exc14, exc15, exc16, exc17, exc18, exc19,
        exc20, exc21, exc22, exc23, exc24, exc25, exc26, exc27, exc28, exc29,
        exc30, exc31
    };
    for (int i = 0; i < 32; i++)
        idt_set(i, (uint32_t)exc_stubs[i], 0x08, 0x8E);
    for (int i = 32; i < 256; i++)
        idt_set(i, (uint32_t)irq_generic_stub, 0x08, 0x8E);
    idt_set(32, (uint32_t)irq0_stub, 0x08, 0x8E);
    idt_set(33, (uint32_t)irq1_stub, 0x08, 0x8E);
    idt_set(44, (uint32_t)irq12_stub, 0x08, 0x8E); /* IRQ12 = vector 44 */
    idt_set(0x80, (uint32_t)syscall_stub, 0x08, 0xEF); /* trap gate, DPL3 */
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;
    idt_load((uint32_t)&idtp);

    pic_remap();
    pit_init();
    mouse_init(); /* polling-based; must run before sti */
    __asm__ volatile ("sti");
}
