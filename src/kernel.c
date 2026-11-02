/* Shiro Mini OS - kernel core: VGA console + shell.
 * v0.3: interrupts/timer/scheduler/memory/FS support (see os.h).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "os.h"

/* ---------- port I/O ---------- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---------- VGA text mode (80x25) ---------- */
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
static uint16_t *const VGA = (uint16_t *)0xB8000;
static size_t term_row = 0, term_col = 0;
static uint8_t term_color = 0x0F; /* white on black */

static uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}
static void term_putentry(char c, size_t x, size_t y) {
    VGA[y * VGA_WIDTH + x] = vga_entry(c, term_color);
}
static void term_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA[(y - 1) * VGA_WIDTH + x] = VGA[y * VGA_WIDTH + x];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        term_putentry(' ', x, VGA_HEIGHT - 1);
}
void term_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            term_putentry(' ', x, y);
    term_row = 0; term_col = 0;
}
static void update_cursor(void) {
    uint16_t pos = (uint16_t)(term_row * VGA_WIDTH + term_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}
void term_putchar(char c) {
    if (c == '\r') {
        term_col = 0;
        update_cursor();
    } else if (c == '\n') {
        term_col = 0;
        if (++term_row >= VGA_HEIGHT) { term_scroll(); term_row = VGA_HEIGHT - 1; }
    } else if (c == '\b') {
        if (term_col > 0) { term_col--; term_putentry(' ', term_col, term_row); }
    } else {
        term_putentry(c, term_col, term_row);
        if (++term_col >= VGA_WIDTH) {
            term_col = 0;
            if (++term_row >= VGA_HEIGHT) { term_scroll(); term_row = VGA_HEIGHT - 1; }
        }
    }
    update_cursor();
}
void print(const char *s) { while (*s) term_putchar(*s++); }

void print_dec(uint32_t v) {
    char buf[12]; int i = 0;
    if (v == 0) { term_putchar('0'); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) term_putchar(buf[--i]);
}
void print_hex32(uint32_t v) {
    print("0x");
    const char *hex = "0123456789ABCDEF";
    for (int s = 28; s >= 0; s -= 4) term_putchar(hex[(v >> s) & 0xF]);
}
int term_cursor_row(void) { return (int)term_row; }
int term_cursor_col(void) { return (int)term_col; }
void term_set_cursor(int row, int col) {
    if (row < 0) row = 0; if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0; if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
    term_row = (size_t)row; term_col = (size_t)col;
    update_cursor();
}

/* ---------- tiny string helpers ---------- */
static int strcmp_(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
static int strncmp_(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0) return (uint8_t)a[i] - (uint8_t)b[i];
    }
    return 0;
}
static size_t strlen_(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* ---------- power ---------- */
void do_reboot(void) {
    print("Rebooting...\n");
    /* pulse the reset line via the 8042 keyboard controller */
    uint8_t tmp;
    do { tmp = inb(0x64); } while (tmp & 0x02);
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("hlt");
}
void do_poweroff(void) {
    print("Powering off...\n");
    outw(0x604, 0x2000);  /* QEMU (ACPI shutdown) */
    outw(0xB004, 0x2000); /* Bochs / old QEMU */
    outw(0x4004, 0x3400); /* VirtualBox */
    print("Poweroff not supported on this machine; halted.\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* ---------- shell ---------- */
#define CMD_MAX 256

/* set while the editor owns the screen (sched.c's corner counter
 * checks this before scribbling on the VGA text buffer) */
volatile int fullscreen_ui = 0;

/* ----- line editor with history (stage 6; persisted in v0.5.1) ----- */
#define HIST_MAX 8
static char history[HIST_MAX][CMD_MAX];
static int hist_count = 0;
static int hist_ready = 0; /* set once hist_load() has run */

static void hist_save(void) {
    if (!hist_ready || !fs_present()) return;
    static char buf[HIST_MAX * (CMD_MAX + 1)];
    size_t pos = 0;
    for (int i = 0; i < hist_count; i++) {
        size_t j = 0;
        while (history[i][j] && pos < sizeof(buf) - 2) buf[pos++] = history[i][j++];
        buf[pos++] = '\n';
    }
    fs_write_file("history", buf, (uint32_t)pos);
}

static void hist_load(void) {
    hist_ready = 1;
    if (!fs_present()) return;
    static char buf[2048];
    int n = fs_read_file("history", buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            int len = i - start;
            if (len > 0) {
                if (len > CMD_MAX - 1) len = CMD_MAX - 1;
                if (hist_count == HIST_MAX) {
                    for (int a = 1; a < HIST_MAX; a++)
                        for (int b = 0; b < CMD_MAX; b++)
                            history[a - 1][b] = history[a][b];
                    hist_count = HIST_MAX - 1;
                }
                for (int j = 0; j < len; j++)
                    history[hist_count][j] = buf[start + j];
                history[hist_count][len] = 0;
                hist_count++;
            }
            start = i + 1;
        }
    }
}

static void hist_add(const char *line) {
    if (line[0] == 0) return;
    if (hist_count > 0 && strcmp_(history[hist_count - 1], line) == 0) return;
    if (hist_count == HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++)
            for (int j = 0; j < CMD_MAX; j++) history[i - 1][j] = history[i][j];
        hist_count = HIST_MAX - 1;
    }
    size_t i = 0;
    while (line[i] && i < CMD_MAX - 1) { history[hist_count][i] = line[i]; i++; }
    history[hist_count][i] = 0;
    hist_count++;
    hist_save();
}

/* Redraw the input line starting at (start_row, start_col). */
static void redraw_line(const char *buf, size_t len, size_t cur,
                        int start_row, int start_col) {
    term_set_cursor(start_row, start_col);
    for (size_t i = 0; i < len; i++) term_putchar(buf[i]);
    term_putchar(' '); /* erase a possibly leftover char */
    /* compute cursor position for 'cur' chars after prompt start */
    size_t pos = (size_t)start_col + cur;
    term_set_cursor(start_row + (int)(pos / VGA_WIDTH), (int)(pos % VGA_WIDTH));
}

static void read_line(char *buf) {
    size_t len = 0, cur = 0;
    int hpos = hist_count; /* browsing position; == count means "current" */
    char saved[CMD_MAX]; saved[0] = 0;
    print("> ");
    int start_row = term_cursor_row();
    int start_col = term_cursor_col();
    buf[0] = 0;
    for (;;) {
        int k = keyboard_read_key();
        if (k == '\n' || k == '\r') {
            term_putchar('\n');
            break;
        } else if (k == KEY_LEFT) {
            if (cur > 0) { cur--; redraw_line(buf, len, cur, start_row, start_col); }
        } else if (k == KEY_RIGHT) {
            if (cur < len) { cur++; redraw_line(buf, len, cur, start_row, start_col); }
        } else if (k == KEY_HOME) {
            cur = 0; redraw_line(buf, len, cur, start_row, start_col);
        } else if (k == KEY_END) {
            cur = len; redraw_line(buf, len, cur, start_row, start_col);
        } else if (k == KEY_UP || k == KEY_DOWN) {
            if (k == KEY_UP && hpos > 0) {
                if (hpos == hist_count) { /* save current line */
                    size_t i; for (i = 0; i < len; i++) saved[i] = buf[i]; saved[i] = 0;
                }
                hpos--;
            } else if (k == KEY_DOWN && hpos < hist_count) {
                hpos++;
            } else continue;
            const char *src = (hpos == hist_count) ? saved : history[hpos];
            size_t i = 0;
            while (src[i] && i < CMD_MAX - 1) { buf[i] = src[i]; i++; }
            /* blank out the rest of the old line on screen */
            size_t old = len;
            buf[i] = 0; len = cur = i;
            term_set_cursor(start_row, start_col);
            print(buf);
            for (size_t s = len; s < old; s++) term_putchar(' ');
            redraw_line(buf, len, cur, start_row, start_col);
        } else if (k == KEY_DEL) {
            if (cur < len) {
                for (size_t i = cur; i < len; i++) buf[i] = buf[i + 1];
                len--;
                redraw_line(buf, len, cur, start_row, start_col);
            }
        } else if (k == '\b') {
            if (cur > 0) {
                for (size_t i = cur - 1; i < len; i++) buf[i] = buf[i + 1];
                cur--; len--;
                redraw_line(buf, len, cur, start_row, start_col);
            }
        } else if (k == '\t') {
            /* ----- Tab completion ----- */
            static const char *const CMDS[] = {
                "help", "about", "clear", "echo", "version", "uptime",
                "mem", "ps", "ip", "ping", "dns", "browse", "whois",
                "ls", "write", "cat", "rm", "cp", "mv", "edit", "date",
                "install", "apps", "run", "uninstall", "exec", "gui",
                "web", "jstest", "gfxstats",
                "res", "mouse", "reboot", "poweroff"
            };
            size_t ws = cur;
            while (ws > 0 && buf[ws - 1] != ' ') ws--;
            int first_tok = (ws == 0);
            size_t plen = cur - ws;
            static char matches[40][32];
            int nm = 0;
            if (first_tok) {
                for (size_t ci = 0; ci < sizeof(CMDS) / sizeof(CMDS[0]) && nm < 40; ci++) {
                    if (strncmp_(CMDS[ci], buf + ws, plen) == 0) {
                        size_t j = 0;
                        while (CMDS[ci][j] && j < 31) { matches[nm][j] = CMDS[ci][j]; j++; }
                        matches[nm][j] = 0;
                        nm++;
                    }
                }
            } else if (fs_present()) {
                char fname[28];
                for (int fi = 0; fs_list_name(fi, fname, sizeof(fname)) && nm < 40; fi++) {
                    if (strncmp_(fname, buf + ws, plen) == 0) {
                        size_t j = 0;
                        while (fname[j] && j < 31) { matches[nm][j] = fname[j]; j++; }
                        matches[nm][j] = 0;
                        nm++;
                    }
                }
            }
            if (nm == 1 || nm > 1) {
                /* longest common prefix of all matches */
                char lcp[32];
                size_t li = 0;
                while (matches[0][li] && li < 31) {
                    char c0 = matches[0][li];
                    int all = 1;
                    for (int mi = 1; mi < nm; mi++)
                        if (matches[mi][li] != c0) { all = 0; break; }
                    if (!all) break;
                    lcp[li++] = c0;
                }
                lcp[li] = 0;
                if (li > plen) {
                    /* replace the partial word with the common prefix;
                     * a unique match also earns a trailing space */
                    char repl[36];
                    size_t ri = 0;
                    for (size_t j = 0; j < li; j++) repl[ri++] = lcp[j];
                    if (nm == 1 && cur == len && ri < 35) repl[ri++] = ' ';
                    repl[ri] = 0;
                    size_t suffix = len - cur;
                    static char sbuf[CMD_MAX];
                    for (size_t j = 0; j < suffix; j++) sbuf[j] = buf[cur + j];
                    size_t nl = ws + ri + suffix;
                    if (nl < CMD_MAX) {
                        for (size_t j = 0; j < ri; j++) buf[ws + j] = repl[j];
                        for (size_t j = 0; j < suffix; j++) buf[ws + ri + j] = sbuf[j];
                        len = nl;
                        cur = ws + ri;
                        buf[len] = 0;
                        redraw_line(buf, len, cur, start_row, start_col);
                    }
                } else if (nm > 1) {
                    /* no extension possible: list the candidates */
                    term_putchar('\n');
                    for (int mi = 0; mi < nm; mi++) {
                        print(matches[mi]);
                        print("  ");
                    }
                    term_putchar('\n');
                    print("> ");
                    start_row = term_cursor_row();
                    start_col = term_cursor_col();
                    redraw_line(buf, len, cur, start_row, start_col);
                }
            }
        } else if (k == 27) {
            /* ignore escape */
        } else if (k >= 32 && k < 127 && len < CMD_MAX - 1) {
            for (size_t i = len; i > cur; i--) buf[i] = buf[i - 1];
            buf[cur] = (char)k;
            cur++; len++;
            buf[len] = 0;
            redraw_line(buf, len, cur, start_row, start_col);
        }
    }
    buf[len] = 0;
    hist_add(buf);
}

/* parse helpers for fs commands */
static const char *skip_spaces(const char *p) { while (*p == ' ') p++; return p; }
static const char *read_token(const char *p, char *out, size_t max) {
    p = skip_spaces(p);
    size_t i = 0;
    while (*p && *p != ' ' && i < max - 1) out[i++] = *p++;
    out[i] = 0;
    return p;
}

/* ---------- `date` (CMOS RTC; QEMU RTC is UTC) ---------- */
static void print2(int v) { if (v < 10) term_putchar('0'); print_dec((uint32_t)v); }
static void cmd_date(void) {
    struct rtc_time t;
    if (!rtc_read(&t)) { print("date: RTC read failed.\n"); return; }
    print_dec((uint32_t)t.year); print("-"); print2(t.month); print("-");
    print2(t.day); print(" ");
    print2(t.hour); print(":"); print2(t.min); print(":"); print2(t.sec);
    print(" UTC\n");
}

/* ---------- `edit`: full-screen text editor ----------
 * Flat byte buffer with '\n' line separators. Ctrl+S saves to SHFS,
 * Ctrl+Q (or ESC) quits WITHOUT saving — the status bar says so, so
 * quitting with unsaved changes is an explicit, documented choice.
 * Files larger than 16 KB are refused with a message. */
#define EDIT_MAX 16384
#define EDIT_ROWS 23 /* text rows; row 24 is the status bar */
static char ed_text[EDIT_MAX + 2];
static int ed_len = 0, ed_cur = 0, ed_top = 0, ed_modified = 0;
static char ed_msg[48];
static const char *ed_name = "";

static int ed_line_start(int line) {
    int off = 0, l = 0;
    while (l < line) {
        while (off < ed_len && ed_text[off] != '\n') off++;
        if (off >= ed_len) return -1;
        off++;
        l++;
    }
    return off;
}
static int ed_line_of(int off) {
    int l = 0;
    for (int i = 0; i < off && i < ed_len; i++)
        if (ed_text[i] == '\n') l++;
    return l;
}
static int ed_line_len(int line) {
    int s = ed_line_start(line);
    if (s < 0) return 0;
    int n = 0;
    while (s + n < ed_len && ed_text[s + n] != '\n') n++;
    return n;
}

static void ed_hw_cursor(int row, int col) {
    uint16_t pos = (uint16_t)(row * VGA_WIDTH + col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void ed_render(void) {
    int cl = ed_line_of(ed_cur);
    if (cl < ed_top) ed_top = cl;
    if (cl >= ed_top + EDIT_ROWS) ed_top = cl - EDIT_ROWS + 1;
    int cs = ed_line_start(cl);
    int ccol = cs >= 0 ? ed_cur - cs : 0;
    int hoff = ccol >= (int)VGA_WIDTH ? ccol - (int)VGA_WIDTH + 1 : 0;
    for (size_t r = 0; r < EDIT_ROWS; r++) {
        int line = ed_top + (int)r;
        int start = ed_line_start(line);
        int llen = start >= 0 ? ed_line_len(line) : 0;
        for (size_t c = 0; c < VGA_WIDTH; c++) {
            char ch = ' ';
            if (start >= 0 && (int)c + hoff < llen)
                ch = ed_text[start + hoff + (int)c];
            VGA[r * VGA_WIDTH + c] = vga_entry(ch, 0x0F);
        }
    }
    /* status bar (row 24, black on light gray):
     * " edit: name [*]  Ln 3 Col 12   Ctrl+S save  Ctrl+Q quit (no save)" */
    char st[81];
    size_t p = 0;
    const char *pfx = " edit: ";
    while (*pfx && p < 76) st[p++] = *pfx++;
    const char *nm = ed_name;
    while (*nm && p < 28) st[p++] = *nm++;
    if (ed_modified && p < 30) { st[p++] = ' '; st[p++] = '*'; }
    if (p < 79) st[p++] = ' ';
    /* "Ln <n> Col <n>" appended with hand-rolled decimal conversion */
    {
        const char *a = "Ln ";
        while (*a && p < 76) st[p++] = *a++;
        char nb[12];
        int ni = 0;
        uint32_t v = (uint32_t)(cl + 1);
        if (v == 0) nb[ni++] = '0';
        while (v) { nb[ni++] = (char)('0' + (v % 10)); v /= 10; }
        while (ni && p < 76) st[p++] = nb[--ni];
        const char *b = " Col ";
        while (*b && p < 76) st[p++] = *b++;
        ni = 0;
        v = (uint32_t)(ccol + 1);
        if (v == 0) nb[ni++] = '0';
        while (v) { nb[ni++] = (char)('0' + (v % 10)); v /= 10; }
        while (ni && p < 76) st[p++] = nb[--ni];
    }
    if (p < 79) st[p++] = ' ';
    if (p < 79) st[p++] = ' ';
    if (ed_msg[0]) {
        const char *m = ed_msg;
        while (*m && p < 79) st[p++] = *m++;
    } else {
        const char *hint = "Ctrl+S save  Ctrl+Q quit (no save)";
        while (*hint && p < 79) st[p++] = *hint++;
    }
    while (p < VGA_WIDTH) st[p++] = ' ';
    for (size_t c = 0; c < VGA_WIDTH; c++)
        VGA[24 * VGA_WIDTH + c] = vga_entry(st[c], 0x70);
    ed_hw_cursor(cl - ed_top, ccol - hoff);
}

static void ed_insert(char c) {
    if (ed_len >= EDIT_MAX) {
        const char *m = "Buffer full (16 KB).";
        size_t i = 0; while (m[i]) { ed_msg[i] = m[i]; i++; } ed_msg[i] = 0;
        return;
    }
    for (int i = ed_len; i > ed_cur; i--) ed_text[i] = ed_text[i - 1];
    ed_text[ed_cur] = c;
    ed_len++;
    ed_cur++;
    ed_text[ed_len] = 0;
    ed_modified = 1;
}
static void ed_delete_at(int off) {
    if (off < 0 || off >= ed_len) return;
    for (int i = off; i < ed_len - 1; i++) ed_text[i] = ed_text[i + 1];
    ed_len--;
    ed_text[ed_len] = 0;
    ed_modified = 1;
}

static uint16_t ed_screen_save[VGA_WIDTH * VGA_HEIGHT];

static void cmd_edit(const char *name) {
    int n = fs_read_file(name, ed_text, EDIT_MAX + 1);
    if (n > EDIT_MAX) {
        print("edit: file too large (max 16 KB).\n");
        return;
    }
    ed_len = n < 0 ? 0 : n;
    ed_text[ed_len] = 0;
    ed_cur = 0;
    ed_top = 0;
    ed_modified = 0;
    ed_msg[0] = 0;
    ed_name = name;
    /* save the shell screen, restore it on exit */
    int save_row = term_cursor_row(), save_col = term_cursor_col();
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        ed_screen_save[i] = VGA[i];
    fullscreen_ui = 1;
    ed_render();
    int done = 0;
    while (!done) {
        int k = keyboard_read_key();
        int was_save = 0;
        if (k == 0x11 || k == 27) {          /* Ctrl+Q / ESC: quit, no save */
            done = 1;
        } else if (k == 0x13) {              /* Ctrl+S: save */
            if (fs_write_file(name, ed_text, (uint32_t)ed_len) == 0) {
                const char *m = "Saved.";
                size_t i = 0; while (m[i]) { ed_msg[i] = m[i]; i++; } ed_msg[i] = 0;
                ed_modified = 0;
            } else {
                const char *m = "Save failed!";
                size_t i = 0; while (m[i]) { ed_msg[i] = m[i]; i++; } ed_msg[i] = 0;
            }
            was_save = 1;
        } else if (k == KEY_LEFT) {
            if (ed_cur > 0) ed_cur--;
        } else if (k == KEY_RIGHT) {
            if (ed_cur < ed_len) ed_cur++;
        } else if (k == KEY_UP || k == KEY_DOWN) {
            int cl = ed_line_of(ed_cur);
            int target = cl + (k == KEY_UP ? -1 : 1);
            int ts = ed_line_start(target);
            if (ts >= 0) {
                int col = ed_cur - ed_line_start(cl);
                int tl = ed_line_len(target);
                ed_cur = ts + (col < tl ? col : tl);
            }
        } else if (k == KEY_HOME) {
            ed_cur = ed_line_start(ed_line_of(ed_cur));
        } else if (k == KEY_END) {
            int cl = ed_line_of(ed_cur);
            ed_cur = ed_line_start(cl) + ed_line_len(cl);
        } else if (k == KEY_DEL) {
            ed_delete_at(ed_cur);
        } else if (k == '\b') {
            if (ed_cur > 0) { ed_cur--; ed_delete_at(ed_cur); }
        } else if (k == '\n' || k == '\r') {
            ed_insert('\n');
        } else if (k >= 32 && k < 127) {
            ed_insert((char)k);
        }
        if (!was_save && k != 0x13) ed_msg[0] = 0;
        if (!done) ed_render();
    }
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = ed_screen_save[i];
    fullscreen_ui = 0;
    term_set_cursor(save_row, save_col);
}

static void cmd_mem(void) {
    uint32_t ft, fu, ff, hs, hu, hf;
    mm_frame_stats(&ft, &fu, &ff);
    mm_heap_stats(&hs, &hu, &hf);
    print("RAM total (multiboot): "); print_dec(mm_ram_mb()); print(" MB\n");
    print("Frames: total "); print_dec(ft);
    print(" used "); print_dec(fu);
    print(" free "); print_dec(ff); print(" (4 KB each)\n");
    print("Heap: size "); print_dec(hs / 1024); print(" KB used ");
    print_dec(hu); print(" free "); print_dec(hf); print(" bytes\n");
}

static void run_command(char *line) {
    /* trailing spaces are not significant (Tab completion adds one) */
    size_t tl = strlen_(line);
    while (tl > 0 && line[tl - 1] == ' ') line[--tl] = 0;
    if (line[0] == 0) return;
    if (strcmp_(line, "help") == 0) {
        print("Commands:\n");
        print("  help          show this help\n");
        print("  about         about this OS\n");
        print("  clear         clear screen\n");
        print("  echo <text>   print text\n");
        print("  version       show version\n");
        print("  uptime        seconds since boot\n");
        print("  mem           memory usage\n");
        print("  ps            list tasks\n");
        print("  ip            show network config\n");
        print("  ping <host>   ping a host (name or IP)\n");
        print("  dns <host>    resolve a host name\n");
        print("  browse <url>  open an http(s):// page as text\n");
        print("  web <url>     open a page in the graphical browser (tabs + JS)\n");
        print("  jstest        run the Duktape JS engine self-test\n");
        print("  whois <query> [server]  whois lookup (port 43)\n");
        print("  ls            list files on disk\n");
        print("  write <name> <text...>  create/overwrite a file\n");
        print("  cat <name>    show a file\n");
        print("  rm <name>     delete a file\n");
        print("  cp <src> <dst>  copy a file\n");
        print("  mv <src> <dst>  rename a file\n");
        print("  edit <name>   edit a file (Ctrl+S save, Ctrl+Q quit)\n");
        print("  date          show date and time (RTC, UTC)\n");
        print("  install <url> [name]  download + install an app (ELF)\n");
        print("  apps          list installed apps\n");
        print("  run <name>    run an installed app ('run hello' = sample)\n");
        print("  uninstall <name>  remove an installed app\n");
        print("  exec <name>   run an ELF file stored on disk\n");
        print("  gui           start the graphical desktop\n");
        print("  gfxstats      frame statistics of the last GUI session\n");
        print("  res [w h]     show/set GUI resolution (e.g. res 800 600)\n");
        print("  mouse         show PS/2 mouse state (debug)\n");
        print("  reboot        reboot the machine\n");
        print("  poweroff      shut down (QEMU/Bochs/VirtualBox)\n");
    } else if (strcmp_(line, "about") == 0) {
        print("Shiro Mini OS - a tiny OS made for fun.\n");
        print("Interrupts, paging, multitasking, FS, TCP/IP - C + Assembly.\n");
    } else if (strcmp_(line, "clear") == 0) {
        term_clear();
    } else if (strcmp_(line, "version") == 0) {
        print("Shiro Mini OS v1.0.1 (interrupts+memory+tasks+fs+net+whois+gui+vbe+edit+rtc+tls+apps+browser+js+uefi+gop+css+forms+img)\n");
    } else if (strcmp_(line, "uptime") == 0) {
        print("Uptime: "); print_dec(uptime_seconds()); print(" s (");
        print_dec(timer_ticks); print(" ticks @ 100 Hz)\n");
    } else if (strcmp_(line, "mouse") == 0) {
        int mx, my, mb, mp, mr;
        mouse_debug(&mx, &my, &mb, &mp, &mr);
        print("Mouse: x="); print_dec((uint32_t)mx);
        print(" y="); print_dec((uint32_t)my);
        print(" btn="); print_dec((uint32_t)mb);
        print(" pkts="); print_dec((uint32_t)mp);
        print(" raw="); print_dec((uint32_t)mr);
        print(" ok="); print_dec((uint32_t)mouse_ok()); print("\n");
        print("log:");
        for (int i = 0; i < 48; i++) {
            int v = mouse_log_byte(i);
            if (v < 0) break;
            char hx[3];
            hx[0] = "0123456789ABCDEF"[(v >> 4) & 15];
            hx[1] = "0123456789ABCDEF"[v & 15];
            hx[2] = 0;
            print(" "); print(hx);
        }
        print("\n");
    } else if (strcmp_(line, "mem") == 0) {
        cmd_mem();
    } else if (strcmp_(line, "ps") == 0) {
        sched_print_ps();
    } else if (strcmp_(line, "ip") == 0) {
        cmd_ip();
    } else if (strncmp_(line, "ping ", 5) == 0) {
        cmd_ping(line + 5);
    } else if (strncmp_(line, "dns ", 4) == 0) {
        cmd_dns(line + 4);
    } else if (strncmp_(line, "browse ", 7) == 0) {
        cmd_browse(line + 7);
    } else if (strcmp_(line, "browse") == 0) {
        print("Usage: browse <url>  (http:// only)\n");
    } else if (strncmp_(line, "web ", 4) == 0) {
        browser_run(line + 4);
    } else if (strcmp_(line, "web") == 0) {
        print("Usage: web <url>  (http:// or https://)\n");
    } else if (strcmp_(line, "jstest") == 0) {
        cmd_jstest();
    } else if (strncmp_(line, "whois ", 6) == 0) {
        cmd_whois(line + 6);
    } else if (strcmp_(line, "whois") == 0) {
        print("Usage: whois <query> [server]\n");
    } else if (strcmp_(line, "ls") == 0) {
        if (fs_present()) fs_cmd_ls(); else print("No disk.\n");
    } else if (strncmp_(line, "write ", 6) == 0) {
        if (!fs_present()) { print("No disk.\n"); return; }
        char name[28];
        const char *rest = read_token(line + 6, name, sizeof(name));
        rest = skip_spaces(rest);
        if (name[0] == 0) { print("Usage: write <name> <text...>\n"); return; }
        if (fs_write_file(name, rest, strlen_(rest)) == 0) print("Written.\n");
        else print("Write failed.\n");
    } else if (strncmp_(line, "cat ", 4) == 0) {
        if (!fs_present()) { print("No disk.\n"); return; }
        static char fbuf[4096];
        int n = fs_read_file(line + 4, fbuf, sizeof(fbuf) - 1);
        if (n < 0) { print("No such file.\n"); return; }
        fbuf[n] = 0;
        print(fbuf); print("\n");
    } else if (strncmp_(line, "rm ", 3) == 0) {
        if (!fs_present()) { print("No disk.\n"); return; }
        if (fs_remove_file(line + 3) == 0) print("Removed.\n");
        else print("No such file.\n");
    } else if (strncmp_(line, "cp ", 3) == 0) {
        if (!fs_present()) { print("No disk.\n"); return; }
        char src[28], dst[28];
        const char *rest = read_token(line + 3, src, sizeof(src));
        read_token(rest, dst, sizeof(dst));
        if (src[0] == 0 || dst[0] == 0) { print("Usage: cp <src> <dst>\n"); return; }
        char probe[1];
        if (fs_read_file(dst, probe, 1) >= 0) { print("Destination exists.\n"); return; }
        char *cpbuf = kmalloc(61440);
        if (!cpbuf) { print("Out of memory.\n"); return; }
        int n = fs_read_file(src, cpbuf, 61440);
        if (n < 0) { print("No such file.\n"); kfree(cpbuf); return; }
        if (fs_write_file(dst, cpbuf, (uint32_t)n) == 0) print("Copied.\n");
        else print("Copy failed.\n");
        kfree(cpbuf);
    } else if (strncmp_(line, "mv ", 3) == 0) {
        if (!fs_present()) { print("No disk.\n"); return; }
        char src[28], dst[28];
        const char *rest = read_token(line + 3, src, sizeof(src));
        read_token(rest, dst, sizeof(dst));
        if (src[0] == 0 || dst[0] == 0) { print("Usage: mv <src> <dst>\n"); return; }
        int rc = fs_rename_file(src, dst);
        if (rc == 0) print("Moved.\n");
        else if (rc == -2) print("Destination exists.\n");
        else print("No such file.\n");
    } else if (strncmp_(line, "edit ", 5) == 0) {
        if (!fs_present()) { print("No disk.\n"); return; }
        char name[28];
        read_token(line + 5, name, sizeof(name));
        if (name[0] == 0) { print("Usage: edit <name>\n"); return; }
        cmd_edit(name);
    } else if (strcmp_(line, "edit") == 0) {
        print("Usage: edit <name>\n");
    } else if (strcmp_(line, "date") == 0) {
        cmd_date();
    } else if (strncmp_(line, "install ", 8) == 0) {
        cmd_install(line + 8);
    } else if (strcmp_(line, "install") == 0) {
        print("Usage: install <url> [name]\n");
    } else if (strcmp_(line, "apps") == 0) {
        cmd_apps();
    } else if (strncmp_(line, "uninstall ", 10) == 0) {
        cmd_uninstall(line + 10);
    } else if (strcmp_(line, "uninstall") == 0) {
        print("Usage: uninstall <name>\n");
    } else if (strncmp_(line, "run ", 4) == 0) {
        if (strcmp_(line + 4, "hello") == 0) {
            int rc = run_embedded_hello();
            print("User program exited with code "); print_dec((uint32_t)rc); print(".\n");
        } else {
            cmd_run_app(line + 4);
        }
    } else if (strcmp_(line, "run") == 0) {
        print("Usage: run <name>  (see 'apps'; 'run hello' = built-in sample)\n");
    } else if (strncmp_(line, "exec ", 5) == 0) {
        if (!fs_present()) { print("No disk.\n"); return; }
        char *ebuf = kmalloc(128 * 1024);
        if (!ebuf) { print("Out of memory.\n"); return; }
        int n = fs_read_file(line + 5, ebuf, 128 * 1024);
        if (n < 0) { print("No such file.\n"); kfree(ebuf); return; }
        int rc = run_elf_image((const uint8_t *)ebuf, (uint32_t)n);
        print("Program exited with code "); print_dec((uint32_t)rc); print(".\n");
        kfree(ebuf);
    } else if (strcmp_(line, "res") == 0) {
        cmd_res("");
    } else if (strncmp_(line, "res ", 4) == 0) {
        cmd_res(line + 4);
    } else if (strcmp_(line, "gui") == 0) {
        gui_run();
    } else if (strcmp_(line, "gfxstats") == 0) {
        cmd_gfxstats();
    } else if (strcmp_(line, "reboot") == 0) {
        do_reboot();
    } else if (strcmp_(line, "poweroff") == 0) {
        do_poweroff();
    } else if (strncmp_(line, "echo ", 5) == 0) {
        print(line + 5); print("\n");
    } else if (strcmp_(line, "echo") == 0) {
        print("\n");
    } else {
        print("Unknown command: "); print(line); print("\n");
        print("Type 'help' for commands.\n");
    }
}

void kernel_main(uint32_t magic, uint32_t mb_info) {
    /* Multiboot2: distill the boot info first; under UEFI the
     * display is in a GOP graphics mode, so text mode 3 (with the
     * embedded font) must be installed before the first print. */
    if (magic == 0x36D76289u) {
        boot_parse_mb2(mb_info);
        /* GRUB honors the Multiboot2 header's framebuffer
         * request on BIOS boots too (it sets a VBE graphics mode
         * before jumping here), and under UEFI the display is in
         * a GOP mode: either way an MB2 boot cannot assume text
         * mode, so install text mode 3 + the embedded font
         * unconditionally before printing anything. */
        gfx_uefi_text_init();
    }
    term_clear();
    term_color = 0x0B; /* cyan */
    print("========================================\n");
    print("  Shiro Mini OS v1.0.1\n");
    print("========================================\n");
    term_color = 0x0F;
    cpu_init();
    mm_init(magic, mb_info);
    sched_init();
    print("Booted OK. Interrupts, paging, scheduler on.\n");
    if (magic == 0x36D76289u && boot_is_uefi()) {
        const struct gop_info *g = boot_gop();
        print("Boot: UEFI (Multiboot2)");
        if (g->valid) {
            print(", GOP framebuffer ");
            print_dec(g->width);
            print("x");
            print_dec(g->height);
            print(" pitch ");
            print_dec(g->pitch);
            print(" @ 0x");
            print_hex32(g->addr);
        } else {
            print(", no GOP framebuffer tag");
        }
        print("\n");
    }
    fs_init();
    settings_load();
    hist_load();
    net_init();
    print("Type 'help' and press Enter.\n\n");

    static char line[CMD_MAX];
    for (;;) {
        read_line(line);
        run_command(line);
    }
}
