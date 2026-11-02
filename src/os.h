#ifndef OS_H
#define OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- console (kernel.c) ---------- */
void print(const char *s);
void term_putchar(char c);
void term_clear(void);
void print_dec(uint32_t v);
void print_hex32(uint32_t v);
int  term_cursor_row(void);
int  term_cursor_col(void);
void term_set_cursor(int row, int col);

/* ---------- keyboard (cpu.c, IRQ1 + ring buffer) ---------- */
#define KEY_UP    256
#define KEY_DOWN  257
#define KEY_LEFT  258
#define KEY_RIGHT 259
#define KEY_HOME  260
#define KEY_END   261
#define KEY_DEL   262
#define KEY_PGUP  263
#define KEY_PGDN  264
int  keyboard_read_key(void);   /* blocks; ASCII (<256) or KEY_* code */
char keyboard_getchar(void);    /* blocks; ASCII only (skips KEY_*) */
int  keyboard_try_key(void);    /* -1 if no key pending */

/* ---------- mouse (cpu.c, IRQ12) ---------- */
void mouse_init(void);
void mouse_state(int *x, int *y, int *buttons);
void mouse_set_bounds(int max_x, int max_y);
void mouse_set_pos(int x, int y);
int  mouse_ok(void);
void mouse_debug(int *x, int *y, int *b, int *pkts, int *raw);
int  mouse_log_byte(int i);
void mouse_flush(void);

/* ---------- gui (gui.c) ---------- */
/* Palette indices shared by gui.c and browser.c. 0..15 are the
 * standard VGA colors; 16..32 are the GUI theme colors (v0.5.1);
 * 33..39 are the browser theme colors (v0.7); 40..87 are the desktop
 * gradient ramp (COL_GRAD0 + step). The RGB values live in gui.c. */
enum {
    COL_BLACK = 0, COL_WHITE = 15,
    COL_TASKBAR = 16, COL_TASKBAR_DARK = 17,
    COL_WIN_FACE = 18, COL_EDGE_LIGHT = 19, COL_EDGE_DARK = 20,
    COL_TITLE_ACT = 21, COL_TITLE_INACT = 22, COL_TITLE_TEXT = 23,
    COL_TITLE_TEXT_INACT = 24, COL_CLOSE = 25, COL_TERM_BG = 26,
    COL_TERM_FG = 27, COL_INFO_TEXT = 28, COL_ICON = 29,
    COL_SHADOW = 30, COL_BTN_FACE = 31, COL_ACCENT = 32,
    COL_TAB_STRIP = 33, COL_TAB_INACT = 34, COL_TAB_TEXT_INACT = 35,
    COL_PILL_FOCUS = 36,
    COL_INFO_BLUE = 37, /* desktop Info tile (v0.9); NOT the amber
                         * COL_ICON, whose pixels the harness uses
                         * to locate the Info window */
    COL_GRAD0 = 40
};
void gfx_set_viewport(int x, int y, int w, int h);
void gfx_clear_viewport(void);
/* browser window client (browser.c, driven by gui.c) */
void browser_win_reset(void);
void browser_win_open_url(const char *url);
void browser_win_set_geometry(int w, int h);
void browser_win_draw(int lmx, int lmy);
int  browser_win_key(int k);
void browser_win_mouse_down(int lx, int ly);
int  browser_win_mouse_move(int lx, int ly, int buttons);
int  browser_win_pump(void);
void browser_win_resized(void);
int  browser_win_want_close(void);
void browser_win_ack_close(void);
int  browser_win_editing(void);
void gui_run(void);
void gui_run_browser(const char *url); /* GUI with the browser window open */
void cmd_gfxstats(void);           /* shell `gfxstats` command */
void settings_load(void);          /* load persisted settings (res) at boot */
void cmd_res(const char *args);    /* shell `res` command */

/* Graphics-mode services exported for the browser (browser.c).
 * gfx_enter() switches from text mode to the configured graphics
 * mode (font capture included; must be called in text mode) and
 * returns 1 on success; gfx_exit() restores text mode 3. Drawing
 * goes into the backbuffer via the gfx_* primitives (palette
 * indices, the COL_* values above); gfx_flip() composes the mouse
 * cursor (mx < 0: no cursor) and blits to the framebuffer. */
int  gfx_enter(void);
void gfx_exit(void);
int  gfx_width(void);
int  gfx_height(void);
int  gfx_is_vbe(void);
int  gfx_fallback(int *want_w, int *want_h); /* 1 if 13h fallback used */
void gfx_pixel(int x, int y, int color);
void gfx_rect(int x, int y, int w, int h, int color);
void gfx_rounded_rect(int x, int y, int w, int h, int r, int color);
void gfx_line(int x0, int y0, int x1, int y1, int color);
void gfx_text(int x, int y, const char *s, int color);
void gfx_flip(int mx, int my);
int  gfx_nearest_rgb(int r, int g, int b); /* <body bgcolor> mapping */
/* v1.0 true-color variants (0xRRGGBB) for CSS colors and images.
 * gfx_text2 style bits: 1 bold, 2 italic, 4 underline; the glyph
 * cell is 8x16 times `scale` (1..3). gfx_blit_rgba draws an RGBA
 * buffer (row stride in bytes), alpha-blended over the current
 * frame (over bg_rgb in 8bpp modes). */
void gfx_fill_rgb(int x, int y, int w, int h, uint32_t rgb);
void gfx_text2(int x, int y, const char *s, uint32_t rgb, int scale, int style);
void gfx_blit_rgba(int x, int y, int w, int h, const uint8_t *px,
                   int stride, uint32_t bg_rgb);
void gfx_blit_rgba_scaled(int x, int y, int w, int h, const uint8_t *px,
                          int sw, int sh, uint32_t bg_rgb);

/* ---------- JS engine test (jstest.c) ---------- */
void cmd_jstest(void);

/* ---------- graphical browser (browser.c) ---------- */
void browser_run(const char *url);

/* ---------- Multiboot2 boot info (mb2.c) ---------- */
/* The framebuffer tag (type 8) of a Multiboot2 boot: under UEFI
 * this is the GOP surface. fb_type 1 = direct RGB with per-field
 * positions/sizes; positions are bit offsets in the pixel word. */
struct gop_info {
    int valid;
    uint32_t addr, addr_hi;
    uint32_t pitch;             /* bytes per scanline */
    uint32_t width, height;
    uint8_t bpp, fb_type;
    uint8_t r_pos, r_size, g_pos, g_size, b_pos, b_size;
};
void boot_parse_mb2(uint32_t info_addr);
uint32_t mb2_ram_top_value(void);
int boot_is_uefi(void);
const struct gop_info *boot_gop(void);
/* gui.c: install the embedded font + text mode 3. Needed right
 * after ANY Multiboot2 boot: GRUB sets a graphics mode (VBE on
 * BIOS, GOP on UEFI) to honor the header's framebuffer request,
 * so the kernel cannot assume it starts in text mode. */
void gfx_uefi_text_init(void);

/* ---------- cpu / timer (cpu.c) ---------- */
void cpu_init(void);
extern volatile uint32_t timer_ticks;   /* 100 Hz */
uint32_t uptime_seconds(void);
void delay_ms(uint32_t ms);

/* ---------- CMOS RTC (cpu.c) ---------- */
struct rtc_time { int year, month, day, hour, min, sec; };
int rtc_read(struct rtc_time *t); /* 1 ok (QEMU RTC = UTC), 0 = read failed */

/* set while a full-screen text UI (the editor) owns the VGA screen,
 * so the corner counter task stays off it (defined in kernel.c) */
extern volatile int fullscreen_ui;

/* ---------- memory (mm.c) ---------- */
void mm_init(uint32_t mb_magic, uint32_t mb_info);
void *kmalloc(size_t n);
void kfree(void *p);
uint32_t mm_ram_mb(void);
void mm_frame_stats(uint32_t *total, uint32_t *used, uint32_t *freec);
void mm_heap_stats(uint32_t *size, uint32_t *used, uint32_t *freec);
/* map a fresh zeroed frame at user VA (4MB..8MB window), return phys or 0 */
uint32_t mm_map_user_page(uint32_t va);
void mm_restore_identity(uint32_t va);
extern uint32_t kernel_end; /* linker symbol (value = address) */

/* ---------- scheduler (sched.c) ---------- */
void sched_init(void);
int  task_create(const char *name, void (*entry)(void));
void sched_print_ps(void);
uint32_t sched_timer(uint32_t ctx); /* called from irq0 stub; returns new ctx */

/* ---------- filesystem (fs.c) ---------- */
int fs_init(void);            /* 1 = disk+fs ready, 0 = no disk */
int fs_present(void);
void fs_cmd_ls(void);
int fs_write_file(const char *name, const char *data, uint32_t len);
int fs_read_file(const char *name, char *buf, uint32_t maxlen); /* len or -1 */
int fs_remove_file(const char *name);
int fs_rename_file(const char *oldn, const char *newn); /* 0 ok, -1 no src, -2 dst exists/invalid */
int fs_list_name(int n, char *out, uint32_t max); /* nth file name (completion); 1 found, 0 none */
int fs_file_size(const char *name); /* size in bytes or -1 */
/* streaming write (one at a time): begin reserves space for up to
 * max_len bytes; write appends; commit publishes the file, abort
 * discards it. Metadata reaches the disk only at commit. */
int fs_stream_begin(const char *name, uint32_t max_len); /* 0 ok, -2 exists */
int fs_stream_write(const void *data, uint32_t len);
int fs_stream_commit(void);
void fs_stream_abort(void);

/* ---------- installed apps (apps.c) ---------- */
void cmd_install(const char *args);
void cmd_apps(void);
void cmd_run_app(const char *name);
void cmd_uninstall(const char *name);

/* ---------- user programs (exec.c) ---------- */
int run_elf_image(const uint8_t *img, uint32_t len); /* exit code or -1 */
int run_embedded_hello(void);

/* ---------- power (kernel.c) ---------- */
void do_reboot(void);
void do_poweroff(void);

#endif
