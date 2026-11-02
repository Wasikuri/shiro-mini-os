/* Shiro Mini OS - GUI (v0.4, resolution support v0.5).
 * Shell command `gui` switches to a graphics mode and runs a tiny
 * desktop: gradient background, a taskbar with a live clock, and two
 * draggable windows (Info + a mini Term console), driven by the PS/2
 * mouse (IRQ12) and the keyboard.
 *
 * Two video paths:
 *  - 320x200: VGA mode 13h (256-color palette) programmed directly
 *    into the VGA registers, framebuffer at 0xA0000 (v0.4 behavior,
 *    and the fallback when VBE is unavailable).
 *  - 640x480 / 800x600 / 1024x768 at 32bpp: Bochs VBE extensions
 *    (DISPI ports 0x01CE/0x01CF) on the standard VGA, linear
 *    framebuffer located via the VGA PCI BAR0. Works entirely in
 *    protected mode, no BIOS calls.
 *
 * The mode used is chosen by the persisted `res` setting (SHFS file
 * "settings", line "res=WxH"); default is 320x200.
 *
 * Rendering is double-buffered: draw into a heap backbuffer, compose
 * the mouse cursor on top in a second buffer, blit to the hardware
 * framebuffer. In 32bpp modes palette indices are translated to
 * XRGB through a table built from the same palette.
 *
 * Font: an 8x16 ASCII font is captured from VGA plane 2 while still
 * in text mode (mode 13h chain-4 rendering overwrites the low VRAM
 * where the font lives, so the captured copy is written back before
 * text mode 3 is restored on exit).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os.h"
#include "net.h"

void *memcpy(void *dst, const void *src, size_t n); /* net.c */
void *memset(void *dst, int v, size_t n);           /* net.c */

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
static inline uint32_t inl(uint16_t port) {
    uint32_t r; __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(port));
}

/* ---------- tiny string helpers ---------- */
static size_t g_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void g_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = 0;
}
static void g_strcat(char *dst, const char *src) {
    while (*dst) dst++;
    g_strcpy(dst, src);
}
static void g_strcat_dec(char *dst, uint32_t v) {
    char num[12];
    int n = 0;
    if (v == 0) num[n++] = '0';
    while (v) { num[n++] = (char)('0' + (v % 10)); v /= 10; }
    char *d = dst;
    while (*d) d++;
    while (n) *d++ = num[--n];
    *d = 0;
}
static int g_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
static int g_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0) return (uint8_t)a[i] - (uint8_t)b[i];
    }
    return 0;
}

/* ================================================================
 * Resolution setting (persisted in the SHFS file "settings")
 * ================================================================ */
static const int RES_LIST[][2] = {
    { 320, 200 }, { 640, 480 }, { 800, 600 }, { 1024, 768 },
    { 1280, 720 }, { 1280, 1024 }
};
/* v0.9: 1280x720 and 1280x1024 join the list. Page flipping needs
 * two frames in VRAM: 1280x1024x32bpp x2 = 10.5 MB, inside the
 * 16 MB of QEMU's std VGA (gfx_enter verifies the size through
 * the VBE info and degrades to blits when two pages do not fit;
 * 1920x1080x2 = 16.6 MB would not fit and is not offered). */
#define RES_COUNT (sizeof(RES_LIST) / sizeof(RES_LIST[0]))
static int cfg_w = 320, cfg_h = 200; /* default: mode 13h, as v0.4 */

static int res_supported(int w, int h) {
    for (size_t i = 0; i < RES_COUNT; i++)
        if (RES_LIST[i][0] == w && RES_LIST[i][1] == h) return 1;
    return 0;
}

static int parse_num(const char **pp) {
    const char *p = *pp;
    int v = 0, any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    *pp = p;
    return any ? v : -1;
}

void settings_load(void) {
    if (!fs_present()) return;
    static char buf[128];
    int n = fs_read_file("settings", buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    for (int i = 0; buf[i]; i++) {
        if (buf[i] == 'r' && buf[i + 1] == 'e' && buf[i + 2] == 's' &&
            buf[i + 3] == '=') {
            const char *p = buf + i + 4;
            int w = parse_num(&p);
            if (*p == 'x' || *p == 'X') p++;
            int h = parse_num(&p);
            if (w > 0 && h > 0 && res_supported(w, h)) {
                cfg_w = w;
                cfg_h = h;
            }
            return;
        }
    }
}

void cmd_res(const char *args) {
    while (*args == ' ') args++;
    /* v0.9: under a UEFI boot WITH a GOP framebuffer tag, the
     * display mode belongs to the firmware and cannot be changed
     * after ExitBootServices. (GRUB's EFI loader currently emits
     * no such tag; then the DISPI path below is used as usual.) */
    if (boot_is_uefi() && boot_gop()->valid) {
        const struct gop_info *g = boot_gop();
        print("res: UEFI boot - resolution is fixed by the firmware (GOP)");
        if (g->valid) {
            print(": ");
            print_dec(g->width);
            print("x");
            print_dec(g->height);
        }
        print("\n");
        return;
    }
    if (*args == 0) {
        print("GUI resolution: ");
        print_dec((uint32_t)cfg_w);
        print("x");
        print_dec((uint32_t)cfg_h);
        print(cfg_w == 320 ? " (mode 13h)\n" : " (VBE)\n");
        print("Choices: 320x200, 640x480, 800x600, 1024x768, "
              "1280x720, 1280x1024\n");
        print("Usage: res <width> <height>  (e.g. res 800 600)\n");
        print("The setting is saved and used by the next 'gui'.\n");
        return;
    }
    const char *p = args;
    int w = parse_num(&p);
    while (*p == ' ' || *p == 'x' || *p == 'X') p++;
    int h = parse_num(&p);
    if (w < 0 || h < 0 || !res_supported(w, h)) {
        print("res: unsupported resolution.\n");
        print("Choices: 320x200, 640x480, 800x600, 1024x768, "
              "1280x720, 1280x1024\n");
        return;
    }
    cfg_w = w;
    cfg_h = h;
    if (fs_present()) {
        char buf[24];
        g_strcpy(buf, "res=");
        g_strcat_dec(buf, (uint32_t)w);
        g_strcat(buf, "x");
        g_strcat_dec(buf, (uint32_t)h);
        g_strcat(buf, "\n");
        if (fs_write_file("settings", buf, (uint32_t)g_strlen(buf)) == 0) {
            print("Resolution saved: ");
        } else {
            print("res: could not write the settings file.\nResolution set: ");
        }
    } else {
        print("res: no disk, set for this session only: ");
    }
    print_dec((uint32_t)w);
    print("x");
    print_dec((uint32_t)h);
    print("\n");
}

/* ================================================================
 * Screen state (runtime dimensions / pixel format)
 * ================================================================ */
static int scr_w = 320, scr_h = 200;
static int fb_bpp = 8;                 /* 8 = palette index, 32 = XRGB */
static uint32_t fb_bytes = 320 * 200;  /* bytes per frame */
static uint8_t *fb_target = (uint8_t *)0xA0000; /* blit destination */
static uint32_t fb_stride = 0;  /* bytes per LFB scanline (blit dst) */
/* v1.0.1: VRAM bytes per flip page (fb_stride * scr_h). On QEMU
 * this equals fb_bytes; where the device pads scanlines (e.g.
 * VirtualBox) the page size must follow the pitch, not the
 * packed frame size, or the flipped page starts mid-frame. */
static uint32_t fb_page_bytes = 320 * 200;
/* v1.0.1: scanline pitch chosen for the VBE/DISPI path and where
 * it came from (BIOS mode info block vs the packed assumption) */
static uint32_t vbe_stride = 0;
static int vbe_pitch_bios = 0;
/* v0.9 Part C: UEFI/GOP backend state (set in gfx_enter) */
static int use_gop = 0;
static uint32_t gop_fb_addr = 0, gop_pitch = 0;
static int use_vbe = 0;
static uint32_t lfb_addr = 0;

static int taskbar_y(void) { return scr_h - 16; }

/* ================================================================
 * v0.8 rendering pipeline: dirty rectangles + page flipping.
 *
 * The scene is composited into ONE RAM backbuffer, but only the
 * invalidated regions are redrawn (clip-rect compositing). Presents
 * copy just those regions to the hidden VRAM page and flip the
 * display offset (Bochs VBE X/Y offset) during vertical retrace,
 * so a frame costs at most one partial copy and the visible page
 * never shows a half-updated image. Paths without two VRAM pages
 * (mode 13h, small VRAM) blit the same dirty regions straight to
 * the single target, retrace-timed when the blit is large.
 * ================================================================ */
struct grect { int16_t x, y, w, h; };
#define MAX_DIRTY 24
static struct grect dirty[MAX_DIRTY];
static int ndirty = 0;
/* Changes blitted to the OTHER page at the previous present: the
 * page we are about to draw to has not seen them yet (flip path). */
static struct grect dirty_acc[MAX_DIRTY];
static int ndirty_acc = 0;

static int flip_mode = 0;      /* two pages in VRAM (VBE) */
static int disp_page = 0;      /* page currently displayed */
static int draw_page = 0;      /* page presents write to */
static int page_cur_x[2] = { -1, -1 };  /* cursor burned into a page */
static int page_cur_y[2] = { -1, -1 };
static int cur_mx = -1, cur_my = -1;    /* cursor on the single target */

/* clip rectangle consulted by put_pixel()/fill_rect() */
static int clip_x0 = 0, clip_y0 = 0, clip_x1 = 1 << 20, clip_y1 = 1 << 20;
static void clip_full(void) {
    clip_x0 = 0; clip_y0 = 0; clip_x1 = scr_w; clip_y1 = scr_h;
}
static void clip_set(int x, int y, int w, int h) {
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > scr_w ? scr_w : x + w;
    clip_y1 = y + h > scr_h ? scr_h : y + h;
    if (clip_x1 < clip_x0) clip_x1 = clip_x0;
    if (clip_y1 < clip_y0) clip_y1 = clip_y0;
}

static int rect_clip(struct grect *r) {
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->x + r->w > scr_w) r->w = scr_w - r->x;
    if (r->y + r->h > scr_h) r->h = scr_h - r->y;
    return r->w > 0 && r->h > 0;
}

/* Add a rect to a dirty list, merging overlapping/touching rects;
 * a full list collapses to one bounding box. */
static void rect_list_add(struct grect *list, int *pn, int x, int y,
                          int w, int h) {
    struct grect r = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };
    if (!rect_clip(&r)) return;
    int n = *pn;
    for (int attempt = 0; attempt < 2; attempt++) {
        int merged = 0;
        for (int i = 0; i < n; i++) {
            struct grect *e = &list[i];
            if (r.x - 1 <= e->x + e->w && e->x - 1 <= r.x + r.w &&
                r.y - 1 <= e->y + e->h && e->y - 1 <= r.y + r.h) {
                int x0 = r.x < e->x ? r.x : e->x;
                int y0 = r.y < e->y ? r.y : e->y;
                int x1 = r.x + r.w > e->x + e->w ? r.x + r.w : e->x + e->w;
                int y1 = r.y + r.h > e->y + e->h ? r.y + r.h : e->y + e->h;
                r.x = (int16_t)x0; r.y = (int16_t)y0;
                r.w = (int16_t)(x1 - x0); r.h = (int16_t)(y1 - y0);
                list[i] = list[n - 1];
                n--;
                merged = 1;
                break;
            }
        }
        if (!merged) break;
    }
    if (n >= MAX_DIRTY) {
        struct grect bb = list[0];
        for (int i = 1; i < n; i++) {
            int x0 = bb.x < list[i].x ? bb.x : list[i].x;
            int y0 = bb.y < list[i].y ? bb.y : list[i].y;
            int x1 = bb.x + bb.w > list[i].x + list[i].w
                         ? bb.x + bb.w : list[i].x + list[i].w;
            int y1 = bb.y + bb.h > list[i].y + list[i].h
                         ? bb.y + bb.h : list[i].y + list[i].h;
            bb.x = (int16_t)x0; bb.y = (int16_t)y0;
            bb.w = (int16_t)(x1 - x0); bb.h = (int16_t)(y1 - y0);
        }
        int x0 = bb.x < r.x ? bb.x : r.x;
        int y0 = bb.y < r.y ? bb.y : r.y;
        int x1 = bb.x + bb.w > r.x + r.w ? bb.x + bb.w : r.x + r.w;
        int y1 = bb.y + bb.h > r.y + r.h ? bb.y + bb.h : r.y + r.h;
        bb.x = (int16_t)x0; bb.y = (int16_t)y0;
        bb.w = (int16_t)(x1 - x0); bb.h = (int16_t)(y1 - y0);
        list[0] = bb;
        n = 1;
    } else {
        list[n++] = r;
    }
    *pn = n;
}

static void gfx_dirty_add(int x, int y, int w, int h) {
    rect_list_add(dirty, &ndirty, x, y, w, h);
}
static void gfx_dirty_all(void) {
    ndirty = 0;
    gfx_dirty_add(0, 0, scr_w, scr_h);
}

/* ---------- frame statistics (live FPS + `gfxstats` snapshot) --- */
static struct {
    uint32_t frames;       /* presents since boot */
    uint32_t burst_frames; /* presents inside interaction bursts */
    uint32_t active_ticks; /* sum of present intervals <= 100 ms */
    uint32_t worst_ticks;  /* worst such interval */
    uint32_t last_tick;    /* tick of the previous present */
    uint64_t blit_bytes;   /* bytes copied to VRAM since boot */
    int retrace_seen;      /* vertical retrace was observed */
    int retrace_fails;     /* consecutive failed retrace waits */
    uint32_t ring[128];    /* recent present ticks (FPS window) */
    int ring_i;
} fstats;

static uint32_t stats_fps_x10(void) {
    uint32_t now = timer_ticks;
    int cnt = 0;
    for (int i = 0; i < 128; i++)
        if (fstats.ring[i] && now - fstats.ring[i] < 100) cnt++;
    return (uint32_t)cnt * 10;
}

/* snapshot taken at every gfx_exit(); read by the gfxstats command */
static struct {
    int valid, w, h, vbe, flip, retrace, gop;
    uint32_t frames, active_ticks, worst_ticks, fps_x10, blit_kb;
    uint32_t pitch;    /* v1.0.1: LFB scanline pitch of the session */
    int pitch_bios;    /* v1.0.1: pitch came from the BIOS mode info */
} stat_snap;

/* Bounded wait for the start of vertical retrace (0x3DA bit 3).
 * If the bit never toggles (some headless QEMU setups), three
 * failed waits mark retrace "dead" for the rest of the boot and
 * presents simply go out as fast as the frame budget allows. */
static void wait_vblank(void) {
    if (fstats.retrace_fails >= 3) return;
    uint32_t t0 = timer_ticks;
    while (!(inb(0x3DA) & 8)) {
        if (timer_ticks - t0 >= 2) {
            fstats.retrace_fails++;
            return;
        }
    }
    fstats.retrace_seen = 1;
    fstats.retrace_fails = 0;
}

/* Best-effort retrace alignment for page flips: poll briefly for
 * the retrace bit and flip as soon as it shows, but never block
 * the frame budget on it. (The DISPI Y offset is latched by the
 * display at the frame boundary anyway, so a flip cannot tear;
 * this only shaves a frame of latency when the phase is near.) */
static void wait_vblank_spin(void) {
    if (fstats.retrace_fails >= 3) return;
    for (int i = 0; i < 1024; i++) {
        if (inb(0x3DA) & 8) {
            fstats.retrace_seen = 1;
            fstats.retrace_fails = 0;
            return;
        }
    }
    fstats.retrace_fails++;
}

/* ================================================================
 * VGA register programming
 * ================================================================ */
static uint8_t *const VRAM = (uint8_t *)0xA0000;

static void vga_seq(int idx, uint8_t v) { outb(0x3C4, (uint8_t)idx); outb(0x3C5, v); }
static void vga_crtc(int idx, uint8_t v) { outb(0x3D4, (uint8_t)idx); outb(0x3D5, v); }
static void vga_gc(int idx, uint8_t v) { outb(0x3CE, (uint8_t)idx); outb(0x3CF, v); }
static void vga_ac(int idx, uint8_t v) {
    (void)inb(0x3DA); /* reset AC flip-flop */
    outb(0x3C0, (uint8_t)idx);
    outb(0x3C0, v);
}
static void vga_ac_enable(void) {
    (void)inb(0x3DA);
    outb(0x3C0, 0x20); /* PAS: video output enabled */
}

struct vga_regs {
    uint8_t misc;
    uint8_t seq[5];
    uint8_t crtc[25];
    uint8_t gc[9];
    uint8_t ac[21];
};

static void vga_apply(const struct vga_regs *r) {
    outb(0x3C2, r->misc);
    for (int i = 0; i < 5; i++) vga_seq(i, r->seq[i]);
    vga_crtc(0x11, r->crtc[0x11] & 0x7F); /* unlock CRTC regs 0-7 */
    for (int i = 0; i < 25; i++) vga_crtc(i, r->crtc[i]);
    for (int i = 0; i < 9; i++) vga_gc(i, r->gc[i]);
    for (int i = 0; i < 21; i++) vga_ac(i, r->ac[i]);
    vga_ac_enable();
}

/* Mode 13h: 320x200, 256 colors, chain-4 (canonical register table) */
static const struct vga_regs MODE13_REGS = {
    0x63,
    { 0x03, 0x01, 0x0F, 0x00, 0x0E },
    { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
      0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
      0x41, 0x00, 0x0F, 0x00, 0x00 }
};

/* Text mode 3: 80x25 (canonical register table) */
static const struct vga_regs TEXT_REGS = {
    0x67,
    { 0x03, 0x00, 0x03, 0x00, 0x02 },
    { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x81, 0xBF, 0x1F,
      0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
      0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
      0x0C, 0x00, 0x0F, 0x08, 0x00 }
};

/* Framebuffer channel layout for pixel packing and the blend
 * helpers. VBE modes are XRGB8888 (shifts 16/8/0); the GOP
 * backend (v0.9 Part C) republishes these from the Multiboot2
 * framebuffer tag. All pixel words in the backbuffer (rgb_table
 * entries included) follow this layout. Defined early because
 * build_palette already packs through them. */
static int fb_r_shift = 16, fb_g_shift = 8, fb_b_shift = 0;
static uint32_t pack_rgb(int r, int g, int b) {
    return ((uint32_t)r << fb_r_shift) | ((uint32_t)g << fb_g_shift) |
           ((uint32_t)b << fb_b_shift);
}
static void unpack_rgb(uint32_t v, int *r, int *g, int *b) {
    *r = (int)((v >> fb_r_shift) & 0xFF);
    *g = (int)((v >> fb_g_shift) & 0xFF);
    *b = (int)((v >> fb_b_shift) & 0xFF);
}

/* ---------- DAC palette ---------- */
/* Palette indices 0..15 are the standard VGA colors; 16..39 are the
 * GUI colors of the modern flat theme (v0.5.1) plus the browser
 * theme colors (v0.7); 40.. are a runtime computed desktop gradient
 * ramp. All values are 6-bit DAC RGB. The COL_* index enum lives in
 * os.h (shared with browser.c). */
#define GRAD_STEPS 48
/* desktop gradient endpoints (6-bit): deep slate blue, top -> bottom */
#define GRAD_TOP_R 4
#define GRAD_TOP_G 14
#define GRAD_TOP_B 19
#define GRAD_BOT_R 2
#define GRAD_BOT_G 7
#define GRAD_BOT_B 10
static const uint8_t PALETTE[][3] = {
    /* 0..15: standard VGA (6-bit RGB) */
    { 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 },
    { 42, 0, 0 }, { 42, 0, 42 }, { 42, 21, 0 }, { 42, 42, 42 },
    { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
    { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 },
    /* 16..: GUI colors (indices match the enum above) */
    { 5, 9, 11 },   /* 16 taskbar: dark slate */
    { 2, 16, 20 },  /* 17 taskbar pressed: deep teal */
    { 59, 61, 62 }, /* 18 window face: light gray */
    { 63, 63, 63 }, /* 19 edge light: white */
    { 8, 11, 13 },  /* 20 edge dark: window border */
    { 3, 31, 37 },  /* 21 title active: teal accent */
    { 45, 49, 51 }, /* 22 title inactive: light gray */
    { 63, 63, 63 }, /* 23 title text (on active) */
    { 8, 11, 13 },  /* 24 title text (on inactive) */
    { 55, 16, 14 }, /* 25 close button red */
    { 2, 6, 8 },    /* 26 term background: deep slate */
    { 28, 56, 34 }, /* 27 term foreground: modern green */
    { 6, 8, 9 },    /* 28 info text: near-black */
    { 60, 44, 13 }, /* 29 icon amber (unique marker color) */
    { 1, 3, 4 },    /* 30 window shadow (8bpp solid) */
    { 8, 13, 16 },  /* 31 taskbar button face */
    { 3, 31, 37 },  /* 32 accent (start pill) */
    { 8, 13, 18 },  /* 33 tab strip: dark slate blue (v0.7 browser) */
    { 17, 24, 30 }, /* 34 inactive tab face: muted slate */
    { 52, 55, 58 }, /* 35 inactive tab text: light gray */
    { 16, 48, 54 }, /* 36 address pill focus ring: bright teal */
    { 10, 22, 52 }, /* 37 info blue: desktop Info tile (v0.9) */
    { 0, 0, 0 }, { 0, 0, 0 } /* 38..39: unused */
};
#define PALETTE_COUNT (sizeof(PALETTE) / sizeof(PALETTE[0]))
#define PAL_MAX 128
static uint8_t pal_all[PAL_MAX][3];
static int pal_count = 0;
static uint32_t rgb_table[PAL_MAX];

/* Build the runtime palette (base colors + gradient ramp) and the
 * palette-index -> XRGB table used by the 32bpp VBE modes. The XRGB
 * scaling matches what QEMU's screendump applies to DAC values. */
static void build_palette(void) {
    for (size_t i = 0; i < PALETTE_COUNT; i++) {
        pal_all[i][0] = PALETTE[i][0];
        pal_all[i][1] = PALETTE[i][1];
        pal_all[i][2] = PALETTE[i][2];
    }
    for (int s = 0; s < GRAD_STEPS; s++) {
        int d = GRAD_STEPS - 1;
        pal_all[COL_GRAD0 + s][0] =
            (uint8_t)(GRAD_TOP_R + (GRAD_BOT_R - GRAD_TOP_R) * s / d);
        pal_all[COL_GRAD0 + s][1] =
            (uint8_t)(GRAD_TOP_G + (GRAD_BOT_G - GRAD_TOP_G) * s / d);
        pal_all[COL_GRAD0 + s][2] =
            (uint8_t)(GRAD_TOP_B + (GRAD_BOT_B - GRAD_TOP_B) * s / d);
    }
    pal_count = COL_GRAD0 + GRAD_STEPS;
    for (int i = 0; i < pal_count; i++) {
        uint32_t r = ((uint32_t)pal_all[i][0] << 2) | (pal_all[i][0] >> 4);
        uint32_t g = ((uint32_t)pal_all[i][1] << 2) | (pal_all[i][1] >> 4);
        uint32_t b = ((uint32_t)pal_all[i][2] << 2) | (pal_all[i][2] >> 4);
        rgb_table[i] = pack_rgb((int)r, (int)g, (int)b);
    }
}

static void vga_load_palette(void) {
    outb(0x3C8, 0);
    for (int i = 0; i < pal_count; i++) {
        outb(0x3C9, pal_all[i][0]);
        outb(0x3C9, pal_all[i][1]);
        outb(0x3C9, pal_all[i][2]);
    }
}

/* ================================================================
 * Bochs VBE (DISPI) + PCI scan for the linear framebuffer
 * ================================================================ */
#define VBE_INDEX_PORT 0x01CE
#define VBE_DATA_PORT  0x01CF
#define VBE_IDX_ID     0x00
#define VBE_IDX_XRES   0x01
#define VBE_IDX_YRES   0x02
#define VBE_IDX_BPP    0x03
#define VBE_IDX_ENABLE 0x04
#define VBE_IDX_BANK   0x05
#define VBE_IDX_VIRT_W 0x06
#define VBE_IDX_VIRT_H 0x07
#define VBE_IDX_XOFF   0x08
#define VBE_IDX_YOFF   0x09
#define VBE_IDX_VMEM64 0x0A
#define VBE_ENABLE     0x01
#define VBE_LFB_ENABLE 0x40

static void vbe_write(int idx, uint16_t v) {
    outw(VBE_INDEX_PORT, (uint16_t)idx);
    outw(VBE_DATA_PORT, v);
}
static uint16_t vbe_read(int idx) {
    outw(VBE_INDEX_PORT, (uint16_t)idx);
    return inw(VBE_DATA_PORT);
}

static int vbe_detect(void) {
    uint16_t id = vbe_read(VBE_IDX_ID);
    return id >= 0xB0C0 && id <= 0xB0C5;
}

static uint32_t pci_read32(int dev, int fn, int off) {
    uint32_t addr = 0x80000000u | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_write32(int dev, int fn, int off, uint32_t v) {
    uint32_t addr = 0x80000000u | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, v);
}

/* Find the VGA-class PCI device (bus 0) and return its BAR0 base. */
static uint32_t pci_find_vga_lfb(void) {
    for (int dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read32(dev, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue; /* empty slot */
        uint32_t classcode = pci_read32(dev, 0, 0x08) >> 8;
        if (classcode != 0x030000) continue;   /* VGA compatible */
        uint32_t bar0 = pci_read32(dev, 0, 0x10);
        if (bar0 & 0x1) continue;              /* I/O BAR: not the LFB */
        uint32_t cmd = pci_read32(dev, 0, 0x04);
        pci_write32(dev, 0, 0x04, cmd | 0x03); /* IO + MEM decode on */
        return bar0 & 0xFFFFFFF0u;
    }
    return 0;
}

/* Program a VBE mode with a virtual screen of w x virt_h (the
 * extra height hosts the second page for display-offset flipping);
 * returns 1 on success (registers verified). */
static int vbe_set_mode(int w, int h, int virt_h) {
    /* v1.0.1: the virtual screen occupies stride bytes per line
     * (fb_stride is assigned before mode programming on both the
     * VBE and GOP call paths); a padded pitch needs more VRAM. */
    uint32_t stride = fb_stride;
    if (stride < (uint32_t)w * 4u) stride = (uint32_t)w * 4u;
    uint32_t need = stride * (uint32_t)virt_h;
    uint32_t vmem = (uint32_t)vbe_read(VBE_IDX_VMEM64) * 65536u;
    if (vmem && need > vmem) return 0;
    vbe_write(VBE_IDX_ENABLE, 0);
    vbe_write(VBE_IDX_XRES, (uint16_t)w);
    vbe_write(VBE_IDX_YRES, (uint16_t)h);
    vbe_write(VBE_IDX_BPP, 32);
    vbe_write(VBE_IDX_VIRT_W, (uint16_t)w);
    vbe_write(VBE_IDX_VIRT_H, (uint16_t)virt_h);
    vbe_write(VBE_IDX_XOFF, 0);
    vbe_write(VBE_IDX_YOFF, 0);
    vbe_write(VBE_IDX_BANK, 0);
    vbe_write(VBE_IDX_ENABLE, VBE_ENABLE | VBE_LFB_ENABLE);
    /* VIRT_H readback only has to COVER virt_h: QEMU's DISPI
     * reports the total scanlines in VRAM there (16777216/3200 =
     * 5242 at 800x32bpp), not an echo of the written value. */
    return vbe_read(VBE_IDX_XRES) == (uint16_t)w &&
           vbe_read(VBE_IDX_YRES) == (uint16_t)h &&
           vbe_read(VBE_IDX_VIRT_H) >= (uint16_t)virt_h &&
           vbe_read(VBE_IDX_BPP) == 32;
}

static void vbe_disable(void) {
    vbe_write(VBE_IDX_ENABLE, 0);
}

/* ================================================================
 * VBE mode-info pitch probe (v1.0.1)
 *
 * The DISPI registers say nothing about the scanline pitch the
 * device actually scans out with: QEMU's Bochs BIOS uses exactly
 * width*4, but VirtualBox pads scanlines, and a guest that just
 * assumes packed rows renders sheared (every row starts a few
 * bytes early). The authoritative value lives in the VBE mode
 * info block (BIOS int 10h AX=4F01: LinBytesPerScanLine at
 * offset 34, BytesPerScanLine at offset 16), so on BIOS boots we
 * fetch every mode's block once via the real-mode trampoline in
 * rmtrap.s and remember the pitch of each supported resolution.
 * Under UEFI there is no BIOS left to ask (ExitBootServices), so
 * the table stays empty and the packed assumption is used, as
 * before. Any failure at any step leaves the table empty too:
 * the fallback is exactly the pre-v1.0.1 behavior.
 * ================================================================ */
extern int vbe_rm_query(void);            /* rmtrap.s */
extern char vbe_rm_blob_start[];          /* rmtrap.s blob */
extern char vbe_rm_blob_end[];

#define RM_PARAM    0x6000u   /* parameter block (layout: rmtrap.s) */
#define RM_BLOB_DST 0x7400u
#define RM_VBEINFO  0x8000u   /* VBE info block (unused after call) */
#define RM_MODEINFO 0x9000u   /* 48 mode info blocks x 256 B */
#define RM_NMODES   48

static const uint16_t vbe_dims[][2] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 },
    { 1280, 1024 }
};
#define VBE_NDIMS (int)(sizeof(vbe_dims) / sizeof(vbe_dims[0]))
static uint32_t vbe_pitch_tab[VBE_NDIMS]; /* 0 = unknown */
static int vbe_probe_done = 0;

static uint16_t rm_rd16(uint32_t addr) {
    return *(volatile uint16_t *)(uintptr_t)addr;
}

/* Build the trampoline's private flat GDT at 0x6100 (+ its
 * pseudo-descriptor at 0x60F0): null, code32, data32, code16,
 * data16 - all base 0; the 16-bit pair has 64 KB limits. */
static void vbe_rm_build_gdt(void) {
    static const uint8_t desc[5][8] = {
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xCF, 0x00 }, /* code32 */
        { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xCF, 0x00 }, /* data32 */
        { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0x00, 0x00 }, /* code16 */
        { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0x00, 0x00 }  /* data16 */
    };
    memcpy((void *)(uintptr_t)0x6100, desc, sizeof(desc));
    *(volatile uint16_t *)(uintptr_t)0x60F0 = (uint16_t)(sizeof(desc) - 1);
    *(volatile uint32_t *)(uintptr_t)0x60F2 = 0x6100u;
    /* pseudo-descriptor for the real-mode IDT (IVT at linear 0),
     * loaded by the blob before it drops to real mode */
    *(volatile uint16_t *)(uintptr_t)0x60E0 = 0x03FF;
    *(volatile uint32_t *)(uintptr_t)0x60E2 = 0;
}

static void vbe_probe(void) {
    vbe_probe_done = 1;
    if (boot_is_uefi()) return;    /* no BIOS services remain */
    memcpy((void *)(uintptr_t)RM_BLOB_DST, vbe_rm_blob_start,
           (size_t)(vbe_rm_blob_end - vbe_rm_blob_start));
    vbe_rm_build_gdt();
    *(volatile uint16_t *)(uintptr_t)(RM_PARAM + 28) = 0; /* status */
    *(volatile uint16_t *)(uintptr_t)(RM_PARAM + 30) = 0; /* count */
    vbe_rm_query();
    if (rm_rd16(RM_PARAM + 28) != 0x004F) return;  /* 4F00 failed */
    uint32_t count = rm_rd16(RM_PARAM + 30);
    if (count == 0 || count > RM_NMODES) return;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *mi = (const uint8_t *)(uintptr_t)(RM_MODEINFO + i * 256);
        uint16_t attr = (uint16_t)(mi[0] | ((uint16_t)mi[1] << 8));
        if (!(attr & 0x80)) continue;              /* no LFB */
        int w = mi[18] | ((int)mi[19] << 8);
        int h = mi[20] | ((int)mi[21] << 8);
        if (mi[25] != 32) continue;                /* bpp */
        uint32_t pitch = (uint32_t)(mi[34] | ((int)mi[35] << 8));
        if (pitch < (uint32_t)w * 4u)
            pitch = (uint32_t)(mi[16] | ((int)mi[17] << 8));
        if (pitch < (uint32_t)w * 4u) continue;
        for (int k = 0; k < VBE_NDIMS; k++)
            if (vbe_dims[k][0] == (uint16_t)w && vbe_dims[k][1] == (uint16_t)h)
                vbe_pitch_tab[k] = pitch;
    }
}

/* BIOS-reported pitch for (w,h), or 0 if unknown. Runs the
 * probe lazily on first use and caches the result. */
static uint32_t vbe_pitch_for(int w, int h) {
    if (!vbe_probe_done) vbe_probe();
    for (int k = 0; k < VBE_NDIMS; k++)
        if (vbe_dims[k][0] == (uint16_t)w && vbe_dims[k][1] == (uint16_t)h)
            return vbe_pitch_tab[k];
    return 0;
}

/* ================================================================
 * Font capture / restore (VGA plane 2, 256 glyphs x 32 bytes)
 * ================================================================ */
#define FONT_BYTES (256 * 32)
static uint8_t *font_buf = NULL;
static int font_captured = 0;

/* Point the CPU memory window at plane 2 (0xA0000), sequential access. */
static void vga_plane2_window(void) {
    vga_seq(0x02, 0x04); /* map mask: writes go to plane 2 */
    vga_seq(0x04, 0x07); /* memory mode: no chain-4, no odd/even */
    vga_gc(0x04, 0x02);  /* read map select: plane 2 */
    vga_gc(0x05, 0x00);  /* read/write mode 0, no odd/even */
    vga_gc(0x06, 0x04);  /* memory map: 0xA0000-0xAFFFF */
}

static void capture_font(void) {
    vga_plane2_window();
    memcpy(font_buf, VRAM, FONT_BYTES);
}

#include "font8x16.h"

/* Multiboot2 boot (BIOS or UEFI): GRUB leaves the display in a
 * graphics mode (VBE resp. GOP) because the kernel header asks
 * for a framebuffer. Load the embedded font into plane 2 and
 * program text mode 3 so the shell console works exactly as
 * under a Multiboot1 BIOS boot. Called from kernel_main before
 * the heap exists, so no allocation happens here; the usual
 * runtime capture in gfx_enter() later reads these same glyphs
 * back into font_buf. */
void gfx_uefi_text_init(void) {
    /* UEFI firmware may leave the VGA's PCI I/O decode disabled;
     * the DISPI ports (0x1CE/0x1CF) used by vbe_disable() below
     * are behind that decode, so without this the disable is a
     * no-op and the font store lands in the framebuffer instead
     * of plane 2. (Legacy VGA ports are decoded separately and
     * always work, which is why text timing/B8000 still came up.)
     * pci_find_vga_lfb() enables IO+MEM decode as a side effect;
     * the returned LFB address is not needed here. */
    (void)pci_find_vga_lfb();
    /* Turn the Bochs VBE pipeline OFF first, on either boot
     * path: whoever set the pre-boot graphics mode (GRUB's VBE
     * driver on BIOS, the firmware GOP stack on UEFI) leaves
     * DISPI enabled, and while it is enabled it owns the display
     * timing AND the legacy aperture (writes to 0xB8000 land in
     * the linear framebuffer instead — visible as a noise stripe
     * on the first scanlines). This is the same first step as
     * the proven gfx_exit() restore. */
    vbe_disable();
    vga_plane2_window();
    memcpy(VRAM, VGA_FONT8X16, FONT_BYTES);
    vga_apply(&TEXT_REGS);
}

static void restore_font(void) {
    vga_plane2_window();
    memcpy(VRAM, font_buf, FONT_BYTES);
}

/* ================================================================
 * Drawing (into the backbuffer; palette indices in both formats)
 * ================================================================ */
static uint8_t *backbuf = NULL;
static uint32_t bufs_bytes = 0;

static void put_pixel(uint8_t *fb, int x, int y, uint8_t color) {
    if (x < 0 || x >= scr_w || y < 0 || y >= scr_h) return;
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    if (fb_bpp == 32)
        ((uint32_t *)fb)[y * scr_w + x] = rgb_table[color];
    else
        fb[y * scr_w + x] = color;
}
static void fill_rect(uint8_t *fb, int x, int y, int w, int h, uint8_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (x < clip_x0) { w -= clip_x0 - x; x = clip_x0; }
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (x + w > clip_x1) w = clip_x1 - x;
    if (y + h > clip_y1) h = clip_y1 - y;
    if (w <= 0 || h <= 0) return;
    if (fb_bpp == 32) {
        uint32_t v = rgb_table[color];
        for (int j = 0; j < h; j++) {
            uint32_t *row = (uint32_t *)fb + (size_t)(y + j) * (size_t)scr_w + x;
            for (int i = 0; i < w; i++) row[i] = v;
        }
    } else {
        for (int j = 0; j < h; j++)
            memset(fb + (y + j) * scr_w + x, color, (size_t)w);
    }
}
/* Rows-from-edge inset of a rounded corner of radius r (row j). */
static int corner_inset(int r, int j) {
    int dj = r - 1 - j;
    int rem = r * r - dj * dj;
    int s = 0;
    while ((s + 1) * (s + 1) <= rem) s++;
    return r - s;
}

/* Filled rectangle with rounded corners (radius r). */
static void fill_rounded_rect(uint8_t *fb, int x, int y, int w, int h,
                              int r, uint8_t color) {
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < r) inset = corner_inset(r, j);
        else if (j >= h - r) inset = corner_inset(r, h - 1 - j);
        fill_rect(fb, x + inset, y + j, w - 2 * inset, 1, color);
    }
}

/* Soft drop shadow for a window: a rounded rect offset by (5,6).
 * In 32bpp modes the pixels underneath are darkened (alpha blend);
 * in mode 13h a solid dark slate stands in for the blend. */
static void draw_shadow(uint8_t *fb, int x, int y, int w, int h, int r) {
    int sx = x + 5, sy = y + 6;
    if (fb_bpp != 32) {
        fill_rounded_rect(fb, sx, sy, w, h, r, COL_SHADOW);
        return;
    }
    if (r > h / 2) r = h / 2;
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < r) inset = corner_inset(r, j);
        else if (j >= h - r) inset = corner_inset(r, h - 1 - j);
        int yy = sy + j;
        if (yy < 0 || yy >= scr_h) continue;
        if (yy < clip_y0 || yy >= clip_y1) continue;
        int xx0 = sx + inset, xx1 = sx + w - inset;
        if (xx0 < clip_x0) xx0 = clip_x0;
        if (xx1 > clip_x1) xx1 = clip_x1;
        for (int xx = xx0; xx < xx1; xx++) {
            if (xx < 0 || xx >= scr_w) continue;
            uint32_t *px = (uint32_t *)fb + (size_t)yy * (size_t)scr_w + xx;
            int rr, gg, bb;
            unpack_rgb(*px, &rr, &gg, &bb);
            rr = rr * 55 / 100; gg = gg * 55 / 100; bb = bb * 55 / 100;
            *px = pack_rgb(rr, gg, bb);
        }
    }
}

static void draw_line(uint8_t *fb, int x0, int y0, int x1, int y1, uint8_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 8x16 glyphs from the captured BIOS font (32-byte cells) */
static void draw_char(uint8_t *fb, int x, int y, char ch, uint8_t color) {
    const uint8_t *glyph = font_buf + (uint32_t)(uint8_t)ch * 32;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++)
            if (bits & (0x80 >> col))
                put_pixel(fb, x + col, y + row, color);
    }
}
static void draw_string(uint8_t *fb, int x, int y, const char *s, uint8_t color) {
    while (*s) {
        draw_char(fb, x, y, *s, color);
        s++;
        x += 8;
    }
}

/* ================================================================
 * Wallpaper (v0.9): an aurora image (assets/wallpaper.jpg,
 * synthesized by tools/make_wallpaper.py) decoded with stb_image
 * and bilinear-scaled to the current resolution once per mode.
 * The scaled copy lives in the same pixel format as the
 * backbuffer (XRGB words at 32bpp, palette indices at 8bpp), so
 * compositing the desktop is a plain row copy. Any failure
 * (decode, allocation) leaves wp_ok = 0 and the desktop falls
 * back to the classic gradient.
 * ================================================================ */
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len,
                                     int *x, int *y,
                                     int *channels_in_file,
                                     int desired_channels); /* stb_image.h */
void stbi_image_free(void *retval_from_stbi_load);      /* stb_image.h */
extern const uint8_t _binary_assets_wallpaper_jpg_start[];
extern const uint8_t _binary_assets_wallpaper_jpg_end[];

static uint8_t *wp_dec = NULL;    /* decoded RGB cache (<= 640 wide) */
static int wp_dec_w = 0, wp_dec_h = 0;
static void *wp_buf = NULL;       /* screen-sized, backbuffer format */
static uint32_t wp_key = 0;       /* mode tag wp_buf was built for */
static int wp_ok = 0;

/* nearest palette index via a 15-bit RGB LUT (built on first use) */
static uint8_t *rgb_lut = NULL;
static int wp_pal_index(int r, int g, int b) {
    if (!rgb_lut) {
        rgb_lut = kmalloc(32768);
        if (!rgb_lut) return COL_BLACK;
        for (int i = 0; i < 32768; i++) {
            int rr = ((i >> 10) & 31) * 255 / 31;
            int gg = ((i >> 5) & 31) * 255 / 31;
            int bb = (i & 31) * 255 / 31;
            rgb_lut[i] = (uint8_t)gfx_nearest_rgb(rr, gg, bb);
        }
    }
    return rgb_lut[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
}

/* bilinear sample of the decoded cache at (fx, fy) in 16.16 */
static void wp_sample(int fx, int fy, int *r, int *g, int *b) {
    int x0 = fx >> 16, y0 = fy >> 16;
    int tx = (fx >> 8) & 0xFF, ty = (fy >> 8) & 0xFF;
    if (x0 < 0) { x0 = 0; tx = 0; }
    if (y0 < 0) { y0 = 0; ty = 0; }
    if (x0 > wp_dec_w - 2) { x0 = wp_dec_w - 2; tx = 255; }
    if (y0 > wp_dec_h - 2) { y0 = wp_dec_h - 2; ty = 255; }
    const uint8_t *p = wp_dec + ((size_t)y0 * wp_dec_w + x0) * 3;
    int stride = wp_dec_w * 3;
    for (int c = 0; c < 3; c++) {
        int v00 = p[c], v10 = p[c + 3];
        int v01 = p[stride + c], v11 = p[stride + c + 3];
        int top = v00 + (v10 - v00) * tx / 255;
        int bot = v01 + (v11 - v01) * tx / 255;
        int v = top + (bot - top) * ty / 255;
        if (c == 0) *r = v;
        else if (c == 1) *g = v;
        else *b = v;
    }
}

static void wallpaper_prepare(void) {
    wp_ok = 0;
    if (!wp_dec) {
        int x = 0, y = 0, n = 0;
        const uint8_t *start = _binary_assets_wallpaper_jpg_start;
        int len = (int)(_binary_assets_wallpaper_jpg_end -
                        _binary_assets_wallpaper_jpg_start);
        uint8_t *img = stbi_load_from_memory(start, len, &x, &y, &n, 3);
        if (!img || x < 8 || y < 8) {
            if (img) stbi_image_free(img);
            return;
        }
        if (x <= 640) {
            wp_dec = img;
            wp_dec_w = x;
            wp_dec_h = y;
        } else {
            /* downscale the cache to 640 wide (the aurora is soft;
             * the screen-scale pass smooths the rest) */
            int dw = 640, dh = y * 640 / x;
            wp_dec = kmalloc((size_t)dw * dh * 3);
            if (!wp_dec) {
                stbi_image_free(img);
                return;
            }
            for (int j = 0; j < dh; j++) {
                int sy = (j * y) / dh;
                for (int i = 0; i < dw; i++) {
                    int sx = (i * x) / dw;
                    memcpy(wp_dec + ((size_t)j * dw + i) * 3,
                           img + ((size_t)sy * x + sx) * 3, 3);
                }
            }
            wp_dec_w = dw;
            wp_dec_h = dh;
            stbi_image_free(img);
        }
    }
    uint32_t key = ((uint32_t)scr_w << 16) ^ ((uint32_t)scr_h << 4) ^
                   (uint32_t)fb_bpp;
    if (wp_buf && wp_key == key) {
        wp_ok = 1;
        return;
    }
    if (wp_buf) {
        kfree(wp_buf);
        wp_buf = NULL;
    }
    wp_buf = kmalloc(fb_bytes);
    if (!wp_buf) return;
    wp_key = key;
    /* cover-fit: scale so the image covers the screen, cropping
     * the overflow (Vista wallpapers are center-cropped too) */
    int step; /* 16.16 source pixels per destination pixel */
    int ox = 0, oy = 0; /* source origin of the crop window */
    if (wp_dec_w * scr_h > wp_dec_h * scr_w) {
        /* source is wider: fit height, crop left/right */
        step = (wp_dec_h << 16) / scr_h;
        ox = ((wp_dec_w - (scr_w * step >> 16)) << 16) / 2;
    } else {
        step = (wp_dec_w << 16) / scr_w;
        oy = ((wp_dec_h - (scr_h * step >> 16)) << 16) / 2;
    }
    for (int j = 0; j < scr_h; j++) {
        int fy = oy + j * step;
        if (fb_bpp == 32) {
            uint32_t *row = (uint32_t *)wp_buf + (size_t)j * scr_w;
            for (int i = 0; i < scr_w; i++) {
                int r, g, b;
                wp_sample(ox + i * step, fy, &r, &g, &b);
                row[i] = pack_rgb(r, g, b);
            }
        } else {
            uint8_t *row = (uint8_t *)wp_buf + (size_t)j * scr_w;
            for (int i = 0; i < scr_w; i++) {
                int r, g, b;
                wp_sample(ox + i * step, fy, &r, &g, &b);
                row[i] = wp_pal_index(r, g, b);
            }
        }
    }
    wp_ok = 1;
}

/* paint the wallpaper over the current clip rect (rows above the
 * taskbar); used by draw_desktop when wp_ok */
static void draw_wallpaper(uint8_t *fb) {
    int bpp = fb_bpp / 8;
    int y0 = clip_y0 < 0 ? 0 : clip_y0;
    int y1 = clip_y1 > taskbar_y() ? taskbar_y() : clip_y1;
    int x0 = clip_x0 < 0 ? 0 : clip_x0;
    int x1 = clip_x1 > scr_w ? scr_w : clip_x1;
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++) {
        size_t off = ((size_t)y * scr_w + x0) * (size_t)bpp;
        memcpy((uint8_t *)fb + off, (const uint8_t *)wp_buf + off,
               (size_t)(x1 - x0) * (size_t)bpp);
    }
}

/* ================================================================
 * Windows
 * ================================================================ */
#define TITLE_H 18

enum { WIN_INFO = 0, WIN_TERM = 1, WIN_BROWSER = 2, WIN_COUNT = 3 };

struct window {
    int x, y, w, h;
    int visible;
    const char *title;
    int maximized;          /* browser: fills the work area */
    int sx, sy, sw, sh;     /* saved geometry while maximized */
};

static struct window wins[WIN_COUNT];
static int zorder[WIN_COUNT]; /* back -> front */

/* Term geometry follows the Term window size (set in wins_reset). */
#define TERM_COLS_MAX 63
#define TERM_MAX_LINES 48
static int term_cols = 25;
static char term_lines[TERM_MAX_LINES][TERM_COLS_MAX + 1];
static int term_nlines = 0;
static char term_input[TERM_COLS_MAX + 1];
static int term_ilen = 0;

static void wins_reset(void) {
    if (scr_w <= 320) {
        wins[WIN_INFO] = (struct window){ 16, 14, 184, 104, 1, "Info" };
        wins[WIN_TERM] = (struct window){ 100, 54, 210, 122, 1, "Term" };
        term_cols = 25;
    } else {
        wins[WIN_INFO] = (struct window){ 24, 20, 264, 132, 1, "Info" };
        int tw = scr_w - scr_w / 3;
        if (tw > 620) tw = 620;
        if (tw < 320) tw = 320;
        int th = scr_h - scr_h / 3;
        if (th > 340) th = 340;
        if (th < 160) th = 160;
        int tx = (scr_w - tw) / 2 + 30;
        int ty = (scr_h - th) / 4;
        if (tx + tw > scr_w - 4) tx = scr_w - 4 - tw;
        if (ty + th > taskbar_y() - 4) ty = taskbar_y() - 4 - th;
        wins[WIN_TERM] = (struct window){ tx, ty, tw, th, 1, "Term" };
        term_cols = (tw - 16) / 8;
        if (term_cols > TERM_COLS_MAX) term_cols = TERM_COLS_MAX;
    }
    /* Browser window: hidden until opened (icon / taskbar / web) */
    if (scr_w <= 320) {
        wins[WIN_BROWSER] = (struct window){ 28, 22, 264, 152, 0, "Web" };
    } else {
        int bw = scr_w - 112;
        if (bw > 688) bw = 688;
        int bh = scr_h - 132;
        if (bh > 468) bh = 468;
        wins[WIN_BROWSER] = (struct window){ 56, 36, bw, bh, 0, "Web" };
    }
    zorder[0] = WIN_BROWSER;
    zorder[1] = WIN_INFO;
    zorder[2] = WIN_TERM; /* Term starts focused/on top */
}

static int focused_window(void) {
    for (int i = WIN_COUNT - 1; i >= 0; i--)
        if (wins[zorder[i]].visible) return zorder[i];
    return -1;
}

static void raise_window(int id) {
    int pos = 0;
    for (int i = 0; i < WIN_COUNT; i++)
        if (zorder[i] == id) { pos = i; break; }
    for (int i = pos; i < WIN_COUNT - 1; i++) zorder[i] = zorder[i + 1];
    zorder[WIN_COUNT - 1] = id;
}

/* ---------- Term console ---------- */
static void term_add_line(const char *s) {
    if (term_nlines >= TERM_MAX_LINES) {
        for (int i = 1; i < TERM_MAX_LINES; i++)
            g_strcpy(term_lines[i - 1], term_lines[i]);
        term_nlines = TERM_MAX_LINES - 1;
    }
    int i = 0;
    while (s[i] && i < term_cols) { term_lines[term_nlines][i] = s[i]; i++; }
    term_lines[term_nlines][i] = 0;
    term_nlines++;
}

static void term_exec(void) {
    char shown[TERM_COLS_MAX + 3];
    shown[0] = '>'; shown[1] = ' ';
    g_strcpy(shown + 2, term_input);
    term_add_line(shown);
    const char *cmd = term_input;
    if (g_strcmp(cmd, "help") == 0) {
        term_add_line("Commands:");
        term_add_line(" help   this list");
        term_add_line(" clear  clear screen");
        term_add_line(" echo   print text");
        term_add_line(" about  about Term");
        term_add_line(" uptime uptime");
    } else if (g_strcmp(cmd, "clear") == 0) {
        term_nlines = 0;
    } else if (g_strncmp(cmd, "echo ", 5) == 0) {
        term_add_line(cmd + 5);
    } else if (g_strcmp(cmd, "echo") == 0) {
        term_add_line("");
    } else if (g_strcmp(cmd, "about") == 0) {
        term_add_line("Shiro Term (GUI console)");
        term_add_line("Part of Shiro Mini OS.");
    } else if (g_strcmp(cmd, "uptime") == 0) {
        char b[TERM_COLS_MAX + 1];
        g_strcpy(b, "Uptime: ");
        g_strcat_dec(b, uptime_seconds());
        g_strcat(b, " s");
        term_add_line(b);
    } else if (cmd[0] == 0) {
        /* empty line: nothing extra */
    } else {
        term_add_line("Unknown command.");
    }
    term_ilen = 0;
    term_input[0] = 0;
}

static void term_key(int k) {
    if (k == '\n' || k == '\r') {
        term_exec();
    } else if (k == '\b') {
        if (term_ilen > 0) term_input[--term_ilen] = 0;
    } else if (k >= 32 && k < 127 && term_ilen < term_cols - 2) {
        /* keep room for the "> " prompt prefix in the echo line */
        term_input[term_ilen++] = (char)k;
        term_input[term_ilen] = 0;
    }
}

/* ---------- rendering ---------- */
/* invalidation helpers (defined with the compositing code below) */
static void inval_rect(int x, int y, int w, int h);
static void inval_win(int id);
static void inval_desktop(void);
/* blend helpers (defined with the desktop code below) */
static uint32_t pack_rgb(int r, int g, int b);
static void unpack_rgb(uint32_t v, int *r, int *g, int *b);

/* Glass-style title bar (v0.9; garasu-fu, no real blur): the
 * tint is alpha-blended over whatever backdrop is already in the
 * backbuffer, with the alpha easing down the bar and a white
 * specular sheen fading across the top half — the Vista Aero
 * look, composited honestly without a blur pass. Same silhouette
 * as the solid path (rounded top corners, squared bottom). */
static void glass_title(uint8_t *fb, int x, int y, int w, int h,
                        int tint_idx) {
    int tr, tg, tb;
    unpack_rgb(rgb_table[tint_idx], &tr, &tg, &tb);
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < h - 5 && j < 5) inset = corner_inset(j, 5);
        int a = 198 - 40 * j / (h > 1 ? h - 1 : 1);
        int sheen = j * 2 < h ? 64 - 128 * j / h : 0;
        int sx = x + inset, sw = w - 2 * inset, yy = y + j;
        if (yy < clip_y0 || yy >= clip_y1) continue;
        if (sx < clip_x0) { sw -= clip_x0 - sx; sx = clip_x0; }
        if (sx + sw > clip_x1) sw = clip_x1 - sx;
        if (sw <= 0) continue;
        uint32_t *row = (uint32_t *)fb + (size_t)yy * scr_w + sx;
        for (int i = 0; i < sw; i++) {
            int r, g, b;
            unpack_rgb(row[i], &r, &g, &b);
            r = (r * (255 - a) + tr * a) / 255;
            g = (g * (255 - a) + tg * a) / 255;
            b = (b * (255 - a) + tb * a) / 255;
            if (sheen > 0) {
                r += (255 - r) * sheen / 255;
                g += (255 - g) * sheen / 255;
                b += (255 - b) * sheen / 255;
            }
            row[i] = pack_rgb(r, g, b);
        }
    }
}

static void draw_window_frame(uint8_t *fb, int id, int focused) {
    struct window *w = &wins[id];
    int x = w->x, y = w->y, wd = w->w, h = w->h;
    draw_shadow(fb, x, y, wd, h, 6);
    /* title bar: rounded top corners only (bottom squared off below);
     * glass-style blend at 32bpp, solid tint at 8bpp */
    uint8_t tcol = focused ? COL_TITLE_ACT : COL_TITLE_INACT;
    if (fb_bpp == 32) {
        /* Glass path: blend the title band over the untouched
         * backdrop FIRST (true translucency), then draw the border
         * and the face around/under it. */
        glass_title(fb, x + 1, y + 1, wd - 2, TITLE_H + 5, tcol);
        /* top border row */
        int ci0 = corner_inset(0, 6);
        fill_rect(fb, x + ci0, y, wd - 2 * ci0, 1, COL_EDGE_DARK);
        /* the band's silhouette edge pixels (border column) */
        for (int j = 1; j <= TITLE_H + 5; j++) {
            int ins = j < 6 ? corner_inset(j, 6) : 0;
            fill_rect(fb, x + ins, y + j, 1, 1, COL_EDGE_DARK);
            fill_rect(fb, x + wd - 1 - ins, y + j, 1, 1, COL_EDGE_DARK);
        }
        /* body below the band: dark outline + light face */
        fill_rounded_rect(fb, x, y + TITLE_H + 6, wd, h - TITLE_H - 6,
                          6, COL_EDGE_DARK);
        fill_rect(fb, x, y + TITLE_H + 6, 1, 6, COL_EDGE_DARK);
        fill_rect(fb, x + wd - 1, y + TITLE_H + 6, 1, 6, COL_EDGE_DARK);
        fill_rounded_rect(fb, x + 1, y + TITLE_H + 7, wd - 2,
                          h - TITLE_H - 8, 5, COL_WIN_FACE);
        /* hairline along the glass band's top row */
        int hi0 = corner_inset(0, 5);
        fill_rect(fb, x + 1 + hi0, y + 1, wd - 2 - 2 * hi0, 1,
                  COL_EDGE_LIGHT);
    } else {
        /* flat modern frame: thin dark border, light face */
        fill_rounded_rect(fb, x, y, wd, h, 6, COL_EDGE_DARK);
        fill_rounded_rect(fb, x + 1, y + 1, wd - 2, h - 2, 5, COL_WIN_FACE);
        fill_rounded_rect(fb, x + 1, y + 1, wd - 2, TITLE_H + 5, 5, tcol);
        fill_rect(fb, x + 1, y + TITLE_H - 4, wd - 2, 5, tcol);
        fill_rect(fb, x + 1, y + 1, wd - 2, 1, COL_EDGE_LIGHT);
    }
    draw_string(fb, x + 8, y + 2, w->title,
                focused ? COL_TITLE_TEXT : COL_TITLE_TEXT_INACT);
    /* minimal close button: small rounded red box with a white x */
    fill_rounded_rect(fb, x + wd - 17, y + 4, 12, 11, 3, COL_CLOSE);
    draw_line(fb, x + wd - 14, y + 7, x + wd - 9, y + 12, COL_WHITE);
    draw_line(fb, x + wd - 9, y + 7, x + wd - 14, y + 12, COL_WHITE);
    if (id == WIN_BROWSER) {
        /* maximize/restore button: small square outline */
        int bx = x + wd - 33, by = y + 4;
        fill_rounded_rect(fb, bx, by, 12, 11, 3,
                          focused ? COL_TAB_TEXT_INACT : COL_EDGE_DARK);
        if (w->maximized) {
            draw_line(fb, bx + 3, by + 4, bx + 8, by + 4, COL_WHITE);
            draw_line(fb, bx + 3, by + 4, bx + 3, by + 8, COL_WHITE);
            draw_line(fb, bx + 8, by + 4, bx + 8, by + 8, COL_WHITE);
        } else {
            draw_line(fb, bx + 3, by + 3, bx + 8, by + 3, COL_WHITE);
            draw_line(fb, bx + 3, by + 8, bx + 8, by + 8, COL_WHITE);
            draw_line(fb, bx + 3, by + 3, bx + 3, by + 8, COL_WHITE);
            draw_line(fb, bx + 8, by + 3, bx + 8, by + 8, COL_WHITE);
        }
        /* resize grip: diagonal ticks in the bottom-right corner */
        if (!w->maximized) {
            for (int i = 0; i < 3; i++)
                draw_line(fb, x + wd - 5 - i * 4, y + h - 3,
                          x + wd - 3, y + h - 5 - i * 4, COL_EDGE_DARK);
        }
    }
}

static void draw_info_content(uint8_t *fb) {
    struct window *w = &wins[WIN_INFO];
    int x = w->x, y = w->y;
    /* unique marker icon (also used by the GUI tests to track the window) */
    fill_rounded_rect(fb, x + 8, y + 26, 12, 12, 3, COL_ICON);
    char b[48];
    draw_string(fb, x + 26, y + 24, "Shiro Mini OS v1.0.1", COL_INFO_TEXT);
    g_strcpy(b, "RAM: ");
    g_strcat_dec(b, mm_ram_mb());
    g_strcat(b, " MB");
    draw_string(fb, x + 8, y + 44, b, COL_INFO_TEXT);
    g_strcpy(b, "Uptime: ");
    g_strcat_dec(b, uptime_seconds());
    g_strcat(b, " s");
    draw_string(fb, x + 8, y + 64, b, COL_INFO_TEXT);
    g_strcpy(b, "Graphics: ");
    g_strcat_dec(b, (uint32_t)scr_w);
    g_strcat(b, "x");
    g_strcat_dec(b, (uint32_t)scr_h);
    g_strcat(b, use_gop ? " GOP" : (use_vbe ? " VBE" : " 13h"));
    /* v1.0.1: show the pitch only when it is padded (differs
     * from the packed width*4), i.e. exactly the VirtualBox
     * case a user needs to confirm at a glance */
    if (use_vbe && fb_stride != (uint32_t)scr_w * 4u) {
        g_strcat(b, " pitch ");
        g_strcat_dec(b, fb_stride);
    }
    draw_string(fb, x + 8, y + 84, b, COL_INFO_TEXT);
    if (w->h >= 120) {
        uint32_t f = stats_fps_x10();
        g_strcpy(b, "FPS: ");
        g_strcat_dec(b, f / 10);
        g_strcat(b, ".");
        g_strcat_dec(b, f % 10);
        g_strcat(b, flip_mode ? " (flip)" : " (blit)");
        draw_string(fb, x + 8, y + 104, b, COL_INFO_TEXT);
    }
}

static void draw_term_content(uint8_t *fb, int blink_on) {
    struct window *w = &wins[WIN_TERM];
    int x = w->x, y = w->y;
    fill_rounded_rect(fb, x + 2, y + TITLE_H + 1, w->w - 4, w->h - TITLE_H - 3,
                      4, COL_TERM_BG);
    /* text rows that fit (completed lines + the prompt line) */
    int rows_fit = (w->h - TITLE_H - 4) / 16;
    if (rows_fit < 2) rows_fit = 2;
    int show = term_nlines;
    if (show > rows_fit - 1) show = rows_fit - 1;
    int start = term_nlines - show;
    int row = 0;
    for (int i = 0; i < show; i++, row++)
        draw_string(fb, x + 5, y + TITLE_H + 4 + row * 16,
                    term_lines[start + i], COL_TERM_FG);
    /* prompt line */
    char line[TERM_COLS_MAX + 3];
    line[0] = '>'; line[1] = ' ';
    g_strcpy(line + 2, term_input);
    draw_string(fb, x + 5, y + TITLE_H + 4 + row * 16, line, COL_TERM_FG);
    if (blink_on) {
        int cx = x + 5 + (int)g_strlen(line) * 8;
        draw_char(fb, cx, y + TITLE_H + 4 + row * 16, '_', COL_TERM_FG);
    }
}

/* taskbar button rects (shared by drawing and hit-testing) */
#define BTN_INFO_X 62
#define BTN_TERM_X 110
#define BTN_WEB_X 158
#define BTN_EXIT_X 206
#define BTN_W 44
#define BTN_H 12
static int btn_y(void) { return taskbar_y() + 2; }

static void draw_taskbar_button(uint8_t *fb, int bx, const char *label,
                                uint8_t face, uint8_t text_color) {
    fill_rounded_rect(fb, bx, btn_y(), BTN_W, BTN_H, 3, face);
    draw_string(fb, bx + 6, btn_y() - 2, label, text_color);
}

/* two-digit, zero-padded decimal append */
static void g_strcat_dec2(char *dst, uint32_t v) {
    if (v < 10) g_strcat(dst, "0");
    g_strcat_dec(dst, v);
}

static void draw_taskbar(uint8_t *fb) {
    int ty = taskbar_y();
    fill_rect(fb, 0, ty, scr_w, scr_h - ty, COL_TASKBAR);
    fill_rect(fb, 0, ty, scr_w, 1, COL_BTN_FACE);
    /* start pill in the accent color */
    fill_rounded_rect(fb, 4, ty + 2, 52, 12, 4, COL_ACCENT);
    draw_string(fb, 11, ty, "Shiro", COL_WHITE);
    int foc = focused_window();
    draw_taskbar_button(fb, BTN_INFO_X, "Info",
                        (wins[WIN_INFO].visible && foc == WIN_INFO)
                            ? COL_TASKBAR_DARK : COL_BTN_FACE, COL_WHITE);
    draw_taskbar_button(fb, BTN_TERM_X, "Term",
                        (wins[WIN_TERM].visible && foc == WIN_TERM)
                            ? COL_TASKBAR_DARK : COL_BTN_FACE, COL_WHITE);
    draw_taskbar_button(fb, BTN_WEB_X, "Web",
                        (wins[WIN_BROWSER].visible && foc == WIN_BROWSER)
                            ? COL_TASKBAR_DARK : COL_BTN_FACE, COL_WHITE);
    draw_taskbar_button(fb, BTN_EXIT_X, "Exit", COL_CLOSE, COL_WHITE);
    /* clock: wall time HH:MM:SS from the RTC when it reads cleanly,
     * uptime as T+M:SS otherwise */
    char clock[20];
    struct rtc_time rt;
    if (rtc_read(&rt)) {
        clock[0] = 0;
        g_strcat_dec2(clock, (uint32_t)rt.hour);
        g_strcat(clock, ":");
        g_strcat_dec2(clock, (uint32_t)rt.min);
        g_strcat(clock, ":");
        g_strcat_dec2(clock, (uint32_t)rt.sec);
    } else {
        uint32_t s = uptime_seconds();
        g_strcpy(clock, "T+");
        g_strcat_dec(clock, s / 60);
        g_strcat(clock, ":");
        g_strcat_dec2(clock, s % 60);
    }
    draw_string(fb, scr_w - 6 - (int)g_strlen(clock) * 8, ty, clock, COL_WHITE);
}

/* desktop icon column (v0.9): Vista-style, top-left; each cell is
 * a 36x36 tile plus a label below. Shared by drawing + hit test. */
enum { ICON_WEB = 0, ICON_TERM = 1, ICON_INFO = 2, ICON_COUNT = 3 };
static const char *const ICON_LABELS[ICON_COUNT] = { "Web", "Term", "Info" };
#define ICON_X 10
#define ICON_TILE 36
static int icon_pitch(void) { return scr_w <= 320 ? 58 : 64; }
/* at VBE widths the column starts below the Info window's
 * default footprint (24,20,264x152) so nothing overlaps; at
 * 320x200 the windows cover most of the screen anyway */
static int icon_base_y(void) { return scr_w <= 320 ? 10 : 166; }
static int icon_y(int i) { return icon_base_y() + i * icon_pitch(); }
static int icon_sel = -1;              /* selected icon or -1 */
static int icon_cell_h(void) { return icon_pitch() - 4; }

/* translucent white blend over a rect of the backbuffer (32bpp
 * only; the caller checks). a: 0..255 opacity of white. */
static void blend_white_rect(uint8_t *fb, int x, int y, int w, int h,
                             int a) {
    if (x < clip_x0) { w -= clip_x0 - x; x = clip_x0; }
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (x + w > clip_x1) w = clip_x1 - x;
    if (y + h > clip_y1) h = clip_y1 - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = (uint32_t *)fb + (size_t)(y + j) * scr_w + x;
        for (int i = 0; i < w; i++) {
            int r, g, b;
            unpack_rgb(row[i], &r, &g, &b);
            r += (255 - r) * a / 255;
            g += (255 - g) * a / 255;
            b += (255 - b) * a / 255;
            row[i] = pack_rgb(r, g, b);
        }
    }
}

static void draw_icon_glyph(uint8_t *fb, int id, int ix, int iy) {
    if (id == ICON_WEB) {
        /* teal globe tile */
        fill_rounded_rect(fb, ix, iy, ICON_TILE, ICON_TILE, 8, COL_ACCENT);
        fill_rounded_rect(fb, ix + 8, iy + 6, 20, 20, 9, COL_WHITE);
        draw_line(fb, ix + 10, iy + 16, ix + 26, iy + 16, COL_ACCENT);
        draw_line(fb, ix + 18, iy + 8, ix + 18, iy + 24, COL_ACCENT);
    } else if (id == ICON_TERM) {
        /* dark terminal tile with a green prompt */
        fill_rounded_rect(fb, ix, iy, ICON_TILE, ICON_TILE, 8, COL_TERM_BG);
        draw_string(fb, ix + 7, iy + 10, ">_", COL_TERM_FG);
    } else {
        /* blue info tile with a white i (blue, not the amber
         * COL_ICON: that color marks the Info window itself) */
        fill_rounded_rect(fb, ix, iy, ICON_TILE, ICON_TILE, 8, COL_INFO_BLUE);
        draw_string(fb, ix + 14, iy + 10, "i", COL_WHITE);
    }
}

static void draw_icon(uint8_t *fb, int id) {
    int ix = ICON_X, iy = icon_y(id);
    if (icon_sel == id) {
        if (fb_bpp == 32)
            blend_white_rect(fb, ix - 4, iy - 3, ICON_TILE + 8,
                             icon_cell_h(), 56);
        fill_rect(fb, ix - 4, iy - 3, ICON_TILE + 8, 1, COL_WHITE);
        fill_rect(fb, ix - 4, iy + icon_cell_h() - 3,
                  ICON_TILE + 8, 1, COL_WHITE);
    }
    draw_icon_glyph(fb, id, ix, iy);
    /* label with a soft shadow */
    const char *s = ICON_LABELS[id];
    int slen = 0;
    while (s[slen]) slen++;
    int lx = ix + (ICON_TILE - slen * 8) / 2;
    draw_string(fb, lx + 1, iy + ICON_TILE + 3, s, COL_EDGE_DARK);
    draw_string(fb, lx, iy + ICON_TILE + 2, s, COL_WHITE);
}

static void draw_desktop(uint8_t *fb) {
    int dt = taskbar_y();
    if (wp_ok) {
        draw_wallpaper(fb);
    } else {
        for (int y = 0; y < dt; y++) {
            int idx = COL_GRAD0 +
                      (dt > 1 ? (y * (GRAD_STEPS - 1)) / (dt - 1) : 0);
            fill_rect(fb, 0, y, scr_w, 1, (uint8_t)idx);
        }
    }
    for (int i = 0; i < ICON_COUNT; i++) draw_icon(fb, i);
    draw_string(fb, 8, dt - 16, "ESC or Exit: back to shell", COL_WHITE);
}

/* ================================================================
 * Sidebar (v0.9): Vista-style gadget strip on the right edge,
 * shown at widths >= 800. Translucent dark panel over the
 * wallpaper with an analog clock (RTC), a month calendar (RTC,
 * today highlighted) and a stats gadget (uptime / FPS / RAM).
 * The whole strip is invalidated once per second from gui_main,
 * so the clock stays live for the cost of one narrow dirty rect.
 * ================================================================ */
double sin(double); /* js_math.c */
double cos(double); /* js_math.c */
static long days_from_civil(int y, int m, int d);

#define SB_W 176
static int sidebar_on(void) { return scr_w >= 800 && scr_h >= 400; }
static int sb_x(void) { return scr_w - SB_W - 8; }

static int isqrt(int v) {
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* translucent tint panel with rounded corners (32bpp only) */
static void blend_panel(uint8_t *fb, int x, int y, int w, int h, int r,
                        int tr, int tg, int tb, int a) {
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < r) inset = corner_inset(j, r);
        else if (j >= h - r) inset = corner_inset(h - 1 - j, r);
        int sx = x + inset, sw = w - 2 * inset, yy = y + j;
        if (yy < clip_y0 || yy >= clip_y1) continue;
        if (sx < clip_x0) { sw -= clip_x0 - sx; sx = clip_x0; }
        if (sx + sw > clip_x1) sw = clip_x1 - sx;
        if (sw <= 0) continue;
        uint32_t *row = (uint32_t *)fb + (size_t)yy * scr_w + sx;
        for (int i = 0; i < sw; i++) {
            int rr, gg, bb;
            unpack_rgb(row[i], &rr, &gg, &bb);
            rr = (rr * (255 - a) + tr * a) / 255;
            gg = (gg * (255 - a) + tg * a) / 255;
            bb = (bb * (255 - a) + tb * a) / 255;
            row[i] = pack_rgb(rr, gg, bb);
        }
    }
}

static void draw_circle(uint8_t *fb, int cx, int cy, int r, int color) {
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        put_pixel(fb, cx + x, cy + y, color);
        put_pixel(fb, cx - x, cy + y, color);
        put_pixel(fb, cx + x, cy - y, color);
        put_pixel(fb, cx - x, cy - y, color);
        put_pixel(fb, cx + y, cy + x, color);
        put_pixel(fb, cx - y, cy + x, color);
        put_pixel(fb, cx + y, cy - x, color);
        put_pixel(fb, cx - y, cy - x, color);
        if (d < 0) d += 4 * x + 6;
        else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

static void draw_hand(uint8_t *fb, int cx, int cy, double ang, int len,
                      int color) {
    int ex = cx + (int)(sin(ang) * len);
    int ey = cy - (int)(cos(ang) * len);
    draw_line(fb, cx, cy, ex, ey, color);
}

static void draw_sidebar(uint8_t *fb) {
    if (!sidebar_on()) return;
    int x = sb_x(), y = 8, h = taskbar_y() - 16;
    if (fb_bpp == 32) {
        blend_panel(fb, x, y, SB_W, h, 10, 8, 30, 40, 150);
        blend_white_rect(fb, x + 10, y + 1, SB_W - 20, 1, 60);
    } else {
        fill_rounded_rect(fb, x, y, SB_W, h, 10, COL_TASKBAR);
    }
    /* frame */
    draw_line(fb, x + 10, y, x + SB_W - 11, y, COL_EDGE_LIGHT);
    draw_line(fb, x + 10, y + h - 1, x + SB_W - 11, y + h - 1,
              COL_EDGE_LIGHT);
    draw_line(fb, x, y + 10, x, y + h - 11, COL_EDGE_LIGHT);
    draw_line(fb, x + SB_W - 1, y + 10, x + SB_W - 1, y + h - 11,
              COL_EDGE_LIGHT);

    /* current time/date from the RTC (uptime fallback) */
    struct rtc_time rt;
    int hh = 0, mm = 0, ss = 0, year = 2000, mon = 1, day = 1;
    if (rtc_read(&rt)) {
        hh = rt.hour; mm = rt.min; ss = rt.sec;
        year = rt.year; mon = rt.month; day = rt.day;
    } else {
        uint32_t s = uptime_seconds();
        hh = (int)(s / 3600 % 24);
        mm = (int)(s / 60 % 60);
        ss = (int)(s % 60);
    }

    /* ---- analog clock ---- */
    int cx = x + SB_W / 2, cy = y + 62, R = 42;
    if (fb_bpp == 32) {
        for (int dy = -R; dy <= R; dy++) {
            int half = isqrt(R * R - dy * dy);
            blend_white_rect(fb, cx - half, cy + dy, 2 * half, 1, 34);
        }
    }
    draw_circle(fb, cx, cy, R, COL_WHITE);
    draw_circle(fb, cx, cy, R - 1, COL_EDGE_LIGHT);
    for (int m = 0; m < 60; m += 5) {
        double a = m * 6.283185307179586 / 60.0;
        int x0 = cx + (int)(sin(a) * (R - 7));
        int y0 = cy - (int)(cos(a) * (R - 7));
        int x1 = cx + (int)(sin(a) * (R - 3));
        int y1 = cy - (int)(cos(a) * (R - 3));
        draw_line(fb, x0, y0, x1, y1, COL_WHITE);
    }
    draw_hand(fb, cx, cy,
              ((hh % 12) + mm / 60.0) * 6.283185307179586 / 12.0,
              R * 55 / 100, COL_WHITE);
    draw_hand(fb, cx + 1, cy,
              ((hh % 12) + mm / 60.0) * 6.283185307179586 / 12.0,
              R * 55 / 100, COL_WHITE);
    draw_hand(fb, cx, cy, (mm + ss / 60.0) * 6.283185307179586 / 60.0,
              R * 80 / 100, COL_WHITE);
    draw_hand(fb, cx, cy, ss * 6.283185307179586 / 60.0,
              R * 86 / 100, COL_CLOSE);
    fill_rect(fb, cx - 1, cy - 1, 3, 3, COL_WHITE);
    /* digital readout under the clock */
    {
        char tb[9];
        tb[0] = (char)('0' + hh / 10);
        tb[1] = (char)('0' + hh % 10);
        tb[2] = ':';
        tb[3] = (char)('0' + mm / 10);
        tb[4] = (char)('0' + mm % 10);
        tb[5] = ':';
        tb[6] = (char)('0' + ss / 10);
        tb[7] = (char)('0' + ss % 10);
        tb[8] = 0;
        draw_string(fb, cx - 32, cy + R + 8, tb, COL_WHITE);
    }

    /* ---- calendar ---- */
    static const char *const MON_NAMES[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    static const int MON_DAYS[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (mon < 1 || mon > 12) { mon = 1; year = 2000; day = 1; }
    int gy = cy + R + 34;
    if (gy + 150 < y + h) {
        char title[24];
        int ti = 0;
        const char *mn = MON_NAMES[mon - 1];
        while (*mn) title[ti++] = *mn++;
        title[ti++] = ' ';
        char yb[5];
        int yy2 = year;
        for (int i = 3; i >= 0; i--) { yb[i] = (char)('0' + yy2 % 10); yy2 /= 10; }
        for (int i = 0; i < 4; i++) title[ti++] = yb[i];
        title[ti] = 0;
        draw_string(fb, x + (SB_W - ti * 8) / 2, gy, title, COL_WHITE);
        static const char WD[7] = { 'S', 'M', 'T', 'W', 'T', 'F', 'S' };
        int gx = x + (SB_W - 7 * 22) / 2;
        for (int c = 0; c < 7; c++) {
            char s[2] = { WD[c], 0 };
            draw_string(fb, gx + c * 22 + 7, gy + 20, s, COL_PILL_FOCUS);
        }
        /* weekday of the 1st (Sunday = 0) via days-from-civil */
        long z = days_from_civil(year, mon, 1);
        int first = (int)((z + 4) % 7);
        if (first < 0) first += 7;
        int mdays = MON_DAYS[mon - 1];
        if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) ||
                         year % 400 == 0))
            mdays = 29;
        for (int d = 1; d <= mdays; d++) {
            int cell = first + d - 1;
            int px = gx + (cell % 7) * 22;
            int py = gy + 38 + (cell / 7) * 16;
            char s[3];
            s[0] = d < 10 ? ' ' : (char)('0' + d / 10);
            s[1] = (char)('0' + d % 10);
            s[2] = 0;
            if (d == day) {
                fill_rounded_rect(fb, px, py - 1, 20, 14, 3, COL_ACCENT);
                draw_string(fb, px + 2, py, s, COL_WHITE);
            } else {
                draw_string(fb, px + 2, py, s, COL_WHITE);
            }
        }
        gy += 38 + 6 * 16 + 10;
    }

    /* ---- stats gadget ---- */
    if (gy + 56 < y + h) {
        draw_line(fb, x + 12, gy, x + SB_W - 12, gy, COL_EDGE_LIGHT);
        char buf[24];
        int bi = 0;
        const char *p = "Uptime ";
        while (*p) buf[bi++] = *p++;
        uint32_t up = timer_ticks / 100;
        int uh = (int)(up / 3600), um = (int)(up / 60 % 60),
            us = (int)(up % 60);
        buf[bi++] = (char)('0' + uh / 10);
        buf[bi++] = (char)('0' + uh % 10);
        buf[bi++] = ':';
        buf[bi++] = (char)('0' + um / 10);
        buf[bi++] = (char)('0' + um % 10);
        buf[bi++] = ':';
        buf[bi++] = (char)('0' + us / 10);
        buf[bi++] = (char)('0' + us % 10);
        buf[bi] = 0;
        draw_string(fb, x + 12, gy + 10, buf, COL_WHITE);
        bi = 0;
        p = "FPS ";
        while (*p) buf[bi++] = *p++;
        int ft = (int)stats_fps_x10();
        if (ft >= 100) buf[bi++] = (char)('0' + ft / 1000 % 10);
        if (ft >= 10) buf[bi++] = (char)('0' + ft / 100 % 10);
        buf[bi++] = (char)('0' + ft / 10 % 10);
        buf[bi++] = '.';
        buf[bi++] = (char)('0' + ft % 10);
        buf[bi] = 0;
        draw_string(fb, x + 12, gy + 28, buf, COL_WHITE);
        bi = 0;
        p = "RAM ";
        while (*p) buf[bi++] = *p++;
        int mb = (int)(mm_ram_mb());
        char nb[8];
        int ni = 0;
        if (mb == 0) nb[ni++] = '0';
        while (mb > 0) { nb[ni++] = (char)('0' + mb % 10); mb /= 10; }
        while (ni > 0) buf[bi++] = nb[--ni];
        p = " MB";
        while (*p) buf[bi++] = *p++;
        buf[bi] = 0;
        draw_string(fb, x + 12, gy + 46, buf, COL_WHITE);
    }
}

/* days since 1970-01-01 for a civil date (Howard Hinnant's
 * algorithm); used by the sidebar calendar */
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* mouse cursor: simple arrow sprite ('#' outline, 'O' fill, '.' clear) */
static const char *const CURSOR_SPRITE[] = {
    "#...........",
    "##..........",
    "#O#.........",
    "#OO#........",
    "#OOO#.......",
    "#OOOO#......",
    "#OOOOO#.....",
    "#OOOOOO#....",
    "#OOOOO#.....",
    "#OOOO#......",
    "#OO#........",
    "#O##........",
    "##..........",
};
#define CURSOR_W 12
#define CURSOR_H 13

static void draw_cursor(uint8_t *fb, int mx, int my) {
    /* The cursor is burned into the PRESENT target, whose scanline
     * stride is fb_stride (the GOP pitch may exceed width*bpp);
     * only the backbuffer uses the packed scr_w stride. */
    int bpp = fb_bpp / 8;
    uint32_t stride = (uint32_t)scr_w * (uint32_t)bpp;
    if (fb != backbuf) stride = fb_stride;
    for (int r = 0; r < CURSOR_H; r++) {
        const char *row = CURSOR_SPRITE[r];
        int y = my + r;
        if (y < 0 || y >= scr_h) continue;
        for (int c = 0; c < CURSOR_W; c++) {
            int x = mx + c;
            if (x < 0 || x >= scr_w) continue;
            uint8_t col;
            if (row[c] == '#') col = COL_BLACK;
            else if (row[c] == 'O') col = COL_WHITE;
            else continue;
            if (fb_bpp == 32)
                *(uint32_t *)(fb + (size_t)y * stride +
                              (size_t)x * 4) = rgb_table[col];
            else
                fb[(size_t)y * stride + (size_t)x] = col;
        }
    }
}

/* ================================================================
 * Exported graphics services (the gfx_* API declared in os.h).
 * gui_run() below uses the same enter/exit path, and browser.c is
 * built entirely on these primitives: draw into the backbuffer with
 * palette indices, then gfx_flip() to compose the cursor and blit.
 * ================================================================ */
static int gfx_active = 0;
static int gfx_fb_flag = 0;            /* mode 13h fallback was used */
static int gfx_want_w = 0, gfx_want_h = 0; /* requested mode */

/* viewport: while a window client is drawing, gfx_* coordinates
 * are client-local and clipped to the client rectangle */
static int vp_on = 0, vp_ox = 0, vp_oy = 0, vp_w = 0, vp_h = 0;
static int vp_sx0, vp_sy0, vp_sx1, vp_sy1; /* saved compose clip */
void gfx_set_viewport(int x, int y, int w, int h) {
    vp_sx0 = clip_x0; vp_sy0 = clip_y0; vp_sx1 = clip_x1; vp_sy1 = clip_y1;
    vp_on = 1; vp_ox = x; vp_oy = y; vp_w = w; vp_h = h;
    clip_x0 = x > vp_sx0 ? x : vp_sx0;
    clip_y0 = y > vp_sy0 ? y : vp_sy0;
    clip_x1 = x + w < vp_sx1 ? x + w : vp_sx1;
    clip_y1 = y + h < vp_sy1 ? y + h : vp_sy1;
    if (clip_x1 < clip_x0) clip_x1 = clip_x0;
    if (clip_y1 < clip_y0) clip_y1 = clip_y0;
}
void gfx_clear_viewport(void) {
    vp_on = 0;
    clip_x0 = vp_sx0; clip_y0 = vp_sy0; clip_x1 = vp_sx1; clip_y1 = vp_sy1;
}
int gfx_width(void) { return vp_on ? vp_w : scr_w; }
int gfx_height(void) { return vp_on ? vp_h : scr_h; }
int gfx_is_vbe(void) { return use_vbe; }
int gfx_fallback(int *want_w, int *want_h) {
    if (want_w) *want_w = gfx_want_w;
    if (want_h) *want_h = gfx_want_h;
    return gfx_fb_flag;
}
void gfx_pixel(int x, int y, int color) {
    if (vp_on) { x += vp_ox; y += vp_oy; }
    put_pixel(backbuf, x, y, (uint8_t)color);
}
void gfx_rect(int x, int y, int w, int h, int color) {
    if (vp_on) { x += vp_ox; y += vp_oy; }
    fill_rect(backbuf, x, y, w, h, (uint8_t)color);
}
void gfx_rounded_rect(int x, int y, int w, int h, int r, int color) {
    if (vp_on) { x += vp_ox; y += vp_oy; }
    fill_rounded_rect(backbuf, x, y, w, h, r, (uint8_t)color);
}
void gfx_line(int x0, int y0, int x1, int y1, int color) {
    if (vp_on) { x0 += vp_ox; x1 += vp_ox; y0 += vp_oy; y1 += vp_oy; }
    draw_line(backbuf, x0, y0, x1, y1, (uint8_t)color);
}
void gfx_text(int x, int y, const char *s, int color) {
    if (vp_on) { x += vp_ox; y += vp_oy; }
    draw_string(backbuf, x, y, s, (uint8_t)color);
}

/* ---------- true-color + styled text (v1.0, for the browser) ----
 * The primitives above speak palette indices; CSS colors and
 * decoded images are true RGB. These variants take 0xRRGGBB: at
 * 32bpp they write the packed value directly, at 8bpp they map
 * through the nearest-color LUT (wp_pal_index). */
static void put_pixel_rgb(uint8_t *fb, int x, int y, uint32_t rgb) {
    if (x < 0 || x >= scr_w || y < 0 || y >= scr_h) return;
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF),
        b = (int)(rgb & 0xFF);
    if (fb_bpp == 32)
        ((uint32_t *)fb)[y * scr_w + x] = pack_rgb(r, g, b);
    else
        fb[y * scr_w + x] = wp_pal_index(r, g, b);
}
static void fill_rect_rgb(uint8_t *fb, int x, int y, int w, int h, uint32_t rgb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (x < clip_x0) { w -= clip_x0 - x; x = clip_x0; }
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (x + w > clip_x1) w = clip_x1 - x;
    if (y + h > clip_y1) h = clip_y1 - y;
    if (w <= 0 || h <= 0) return;
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF),
        b = (int)(rgb & 0xFF);
    if (fb_bpp == 32) {
        uint32_t v = pack_rgb(r, g, b);
        for (int j = 0; j < h; j++) {
            uint32_t *row = (uint32_t *)fb + (size_t)(y + j) * (size_t)scr_w + x;
            for (int i = 0; i < w; i++) row[i] = v;
        }
    } else {
        uint8_t idx = wp_pal_index(r, g, b);
        for (int j = 0; j < h; j++)
            memset(fb + (y + j) * scr_w + x, idx, (size_t)w);
    }
}
void gfx_fill_rgb(int x, int y, int w, int h, uint32_t rgb) {
    if (vp_on) { x += vp_ox; y += vp_oy; }
    fill_rect_rgb(backbuf, x, y, w, h, rgb);
}

/* One glyph of the 8x16 BIOS font at an integer scale, with fake
 * bold (smear right by one source pixel) and italic (row shear).
 * style bits: 1 bold, 2 italic. */
static void draw_char_styled(uint8_t *fb, int x, int y, char ch,
                             uint32_t rgb, int scale, int style) {
    const uint8_t *glyph = font_buf + (uint32_t)(uint8_t)ch * 32;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        if (style & 1) bits = (uint8_t)(bits | (bits << 1));
        int shear = (style & 2) ? (15 - row) / 5 : 0;
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    put_pixel_rgb(fb, x + shear + col * scale + dx,
                                  y + row * scale + dy, rgb);
        }
    }
}
/* style bits: 1 bold, 2 italic, 4 underline. Cell is 8x16 * scale. */
void gfx_text2(int x, int y, const char *s, uint32_t rgb, int scale, int style) {
    if (vp_on) { x += vp_ox; y += vp_oy; }
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    int cx = x;
    for (const char *p = s; *p; p++) {
        draw_char_styled(backbuf, cx, y, *p, rgb, scale, style);
        cx += 8 * scale;
    }
    if (style & 4) /* underline */
        fill_rect_rgb(backbuf, x, y + 16 * scale + (scale > 1 ? 2 : 1),
                      cx - x, scale > 1 ? 2 : 1, rgb);
}

/* Blit an RGBA image scaled (nearest neighbour) from its natural
 * size (sw x sh, tightly packed RGBA) into the w x h rect at
 * (x,y). Alpha handling matches gfx_blit_rgba. */
void gfx_blit_rgba_scaled(int x, int y, int w, int h, const uint8_t *px,
                          int sw, int sh, uint32_t bg_rgb) {
    if (vp_on) { x += vp_ox; y += vp_oy; }
    if (sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    /* Source dims are capped at decode time (<= 4096/axis), so the
     * index products below stay well inside 32 bits; no 64-bit
     * division (freestanding: no __divdi3). */
    for (int j = 0; j < h; j++) {
        int sj = j * sh / h;
        const uint8_t *row = px + (size_t)sj * (size_t)sw * 4;
        for (int i = 0; i < w; i++) {
            int si = i * sw / w;
            const uint8_t *p = row + (size_t)si * 4;
            int a = p[3];
            if (a == 0) continue;
            int xx = x + i, yy = y + j;
            if (xx < 0 || xx >= scr_w || yy < 0 || yy >= scr_h) continue;
            if (xx < clip_x0 || xx >= clip_x1 || yy < clip_y0 || yy >= clip_y1)
                continue;
            if (a == 255) {
                put_pixel_rgb(backbuf, xx, yy,
                              ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
                continue;
            }
            int br, bgc, bb;
            if (fb_bpp == 32) {
                unpack_rgb(((uint32_t *)backbuf)[yy * scr_w + xx], &br, &bgc, &bb);
            } else {
                br = (int)((bg_rgb >> 16) & 0xFF);
                bgc = (int)((bg_rgb >> 8) & 0xFF);
                bb = (int)(bg_rgb & 0xFF);
            }
            int r = (p[0] * a + br * (255 - a)) / 255;
            int g = (p[1] * a + bgc * (255 - a)) / 255;
            int b = (p[2] * a + bb * (255 - a)) / 255;
            put_pixel_rgb(backbuf, xx, yy,
                          ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
        }
    }
}

/* Blit an RGBA image (row stride in bytes) at (x,y). Alpha is
 * blended over the current backbuffer pixel at 32bpp, over
 * bg_rgb at 8bpp (where the stored pixel is only an index). */
void gfx_blit_rgba(int x, int y, int w, int h, const uint8_t *px,
                   int stride, uint32_t bg_rgb) {
    if (vp_on) { x += vp_ox; y += vp_oy; }
    for (int j = 0; j < h; j++) {
        const uint8_t *row = px + (size_t)j * (size_t)stride;
        for (int i = 0; i < w; i++) {
            const uint8_t *p = row + (size_t)i * 4;
            int a = p[3];
            if (a == 0) continue;
            int xx = x + i, yy = y + j;
            if (xx < 0 || xx >= scr_w || yy < 0 || yy >= scr_h) continue;
            if (xx < clip_x0 || xx >= clip_x1 || yy < clip_y0 || yy >= clip_y1)
                continue;
            if (a == 255) {
                put_pixel_rgb(backbuf, xx, yy,
                              ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
                continue;
            }
            int br, bgc, bb;
            if (fb_bpp == 32) {
                unpack_rgb(((uint32_t *)backbuf)[yy * scr_w + xx], &br, &bgc, &bb);
            } else {
                br = (int)((bg_rgb >> 16) & 0xFF);
                bgc = (int)((bg_rgb >> 8) & 0xFF);
                bb = (int)(bg_rgb & 0xFF);
            }
            int r = (p[0] * a + br * (255 - a)) / 255;
            int g = (p[1] * a + bgc * (255 - a)) / 255;
            int b = (p[2] * a + bb * (255 - a)) / 255;
            put_pixel_rgb(backbuf, xx, yy,
                          ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
        }
    }
}
/* ---------- present path (v0.8) ---------- */
static uint8_t *page_base(int p) {
    /* v1.0.1: page size is pitch*height, not the packed fb_bytes */
    if (use_vbe) return (uint8_t *)(lfb_addr + (uint32_t)p * fb_page_bytes);
    return VRAM;
}

static void blit_rect_to(const struct grect *r, uint8_t *dst_base) {
    int bpp = fb_bpp / 8;
    for (int j = 0; j < r->h; j++) {
        size_t soff = ((size_t)(r->y + j) * (size_t)scr_w + (size_t)r->x) *
                      (size_t)bpp;
        size_t doff = (size_t)(r->y + j) * (size_t)fb_stride +
                      (size_t)r->x * (size_t)bpp;
        memcpy(dst_base + doff, backbuf + soff, (size_t)r->w * (size_t)bpp);
    }
    fstats.blit_bytes += (uint64_t)r->w * (uint64_t)r->h * (uint64_t)bpp;
}

static void stats_note_present(void) {
    uint32_t now = timer_ticks;
    if (fstats.frames > 0) {
        uint32_t iv = now - fstats.last_tick;
        if (iv <= 10) { /* <= 100 ms: part of an interaction burst */
            fstats.active_ticks += iv;
            if (iv > fstats.worst_ticks) fstats.worst_ticks = iv;
            fstats.burst_frames++;
        } else { /* idle gap: this frame starts a new burst */
            fstats.active_ticks += 1;
            fstats.burst_frames++;
        }
    } else {
        fstats.active_ticks = 1;
        fstats.burst_frames = 1;
    }
    fstats.last_tick = now;
    fstats.ring[fstats.ring_i] = now;
    fstats.ring_i = (fstats.ring_i + 1) & 127;
    fstats.frames++;
}

/* Present the backbuffer with the cursor at (mx,my) (mx < 0: no
 * cursor). full_blit=1 means the caller composed a complete frame
 * into the backbuffer (the legacy gfx_flip contract); otherwise
 * only the accumulated `dirty` regions are copied. On the flip
 * path the copy goes to the hidden page, the cursor is drawn
 * there, and the display offset is switched during retrace, so
 * the visible page never shows a half-updated frame and a frame
 * never costs more than one (usually partial) copy. */
static void present(int mx, int my, int full_blit) {
    struct grect work[2 * MAX_DIRTY + 1];
    int nw = 0;
    int tgt = flip_mode ? draw_page : 0;
    uint8_t *dst = flip_mode ? page_base(draw_page) : fb_target;

    if (full_blit) {
        work[nw].x = 0; work[nw].y = 0;
        work[nw].w = (int16_t)scr_w; work[nw].h = (int16_t)scr_h;
        nw++;
    } else {
        for (int i = 0; i < ndirty; i++) work[nw++] = dirty[i];
        if (flip_mode)
            for (int i = 0; i < ndirty_acc; i++) work[nw++] = dirty_acc[i];
    }
    /* erase the stale cursor burned into the target page earlier */
    int ocx = flip_mode ? page_cur_x[tgt] : cur_mx;
    int ocy = flip_mode ? page_cur_y[tgt] : cur_my;
    if (ocx >= 0) {
        work[nw].x = (int16_t)ocx; work[nw].y = (int16_t)ocy;
        work[nw].w = CURSOR_W; work[nw].h = CURSOR_H;
        nw++;
    }
    if (!flip_mode) {
        /* the single target is visible while we copy: time big
         * blits to the vertical retrace */
        uint32_t px = 0;
        for (int i = 0; i < nw; i++) px += (uint32_t)work[i].w * work[i].h;
        if (px * (uint32_t)(fb_bpp / 8) > 16384) wait_vblank();
    }
    clip_full();
    for (int i = 0; i < nw; i++) {
        struct grect r = work[i];
        if (rect_clip(&r)) blit_rect_to(&r, dst);
    }
    if (mx >= 0) draw_cursor(dst, mx, my);
    if (flip_mode) {
        wait_vblank_spin();
        vbe_write(VBE_IDX_YOFF, (uint16_t)(draw_page * scr_h));
        page_cur_x[draw_page] = (int16_t)mx;
        page_cur_y[draw_page] = (int16_t)my;
        disp_page = draw_page;
        draw_page = 1 - draw_page;
        /* the new draw page has not seen this frame's changes */
        ndirty_acc = 0;
        if (!full_blit) {
            for (int i = 0; i < ndirty; i++)
                rect_list_add(dirty_acc, &ndirty_acc, dirty[i].x, dirty[i].y,
                              dirty[i].w, dirty[i].h);
        }
    } else {
        cur_mx = mx;
        cur_my = my;
    }
    ndirty = 0;
    stats_note_present();
}

void gfx_flip(int mx, int my) {
    present(mx, my, 1);
}

/* Nearest palette index for an 8-bit sRGB color, over the full
 * runtime palette (standard colors + theme + gradient ramp). Used by
 * the browser for <body bgcolor>. Distances are plain RGB Euclidean;
 * exact standard-color matches (e.g. #55FF55) map to that index. */
int gfx_nearest_rgb(int r, int g, int b) {
    int best = 0;
    long bestd = -1;
    for (int i = 0; i < pal_count; i++) {
        int pr, pg, pb;
        unpack_rgb(rgb_table[i], &pr, &pg, &pb);
        long dr = (long)r - pr, dg = (long)g - pg, db = (long)b - pb;
        long d = dr * dr + dg * dg + db * db;
        if (bestd < 0 || d < bestd) { bestd = d; best = i; }
    }
    return best;
}

/* Switch from text mode to the configured graphics mode (the `res`
 * setting; VBE failure falls back to mode 13h). The VGA font is
 * captured on first use while still in text mode. Returns 1 on
 * success, 0 on allocation failure (still in text mode then). */
int gfx_enter(void) {
    int want_w = cfg_w, want_h = cfg_h;
    gfx_want_w = want_w;
    gfx_want_h = want_h;
    int fallback = 0;
    use_vbe = 0;
    use_gop = 0;
    vbe_pitch_bios = 0;
    vbe_stride = 0;
    lfb_addr = 0;
    /* v0.9 Part C: UEFI/GOP backend. On a UEFI boot, GRUB's
     * Multiboot2 framebuffer tag describes the GOP surface the
     * firmware set up: it is the display. Geometry, pitch and the
     * RGB field layout come from the tag alone (never from VBE
     * probing); the mode is the firmware's and cannot be changed
     * after ExitBootServices, so any `res` request is overridden
     * by the tag geometry. */
    const struct gop_info *g = boot_gop();
    if (boot_is_uefi() && g->valid && g->fb_type == 1 && g->bpp == 32 &&
        g->addr_hi == 0 && g->width >= 320 && g->height >= 200 &&
        g->pitch >= g->width * 4) {
        use_gop = 1;
        scr_w = (int)g->width;
        scr_h = (int)g->height;
        fb_bpp = 32;
        fb_r_shift = (int)g->r_pos;
        fb_g_shift = (int)g->g_pos;
        fb_b_shift = (int)g->b_pos;
        gop_fb_addr = g->addr;
        gop_pitch = g->pitch;
    } else if (want_w == 320 && want_h == 200) {
        scr_w = 320; scr_h = 200; fb_bpp = 8;
    } else {
        scr_w = want_w; scr_h = want_h; fb_bpp = 32;
        /* PCI first, DISPI second: UEFI firmware may leave the
         * VGA's PCI I/O decode disabled, and then the DISPI ID
         * read in vbe_detect() returns garbage even though the
         * hardware is present. pci_find_vga_lfb() enables IO+MEM
         * decode on the VGA as a side effect. */
        lfb_addr = pci_find_vga_lfb();
        if (lfb_addr && !vbe_detect()) lfb_addr = 0;
        if (lfb_addr) {
            use_vbe = 1;
            /* v1.0.1: adopt the BIOS-reported scanline pitch for
             * this mode when it is at least the packed width
             * (VirtualBox pads scanlines; QEMU reports packed).
             * The probe is a no-op under UEFI and on any failure
             * leaves the packed assumption in place. */
            uint32_t p = vbe_pitch_for(want_w, want_h);
            vbe_pitch_bios = p >= (uint32_t)want_w * 4u;
            vbe_stride = vbe_pitch_bios ? p : (uint32_t)want_w * 4u;
        } else {
            use_vbe = 0;
            fallback = 1;
            scr_w = 320; scr_h = 200; fb_bpp = 8;
        }
    }
    fb_bytes = (uint32_t)scr_w * (uint32_t)scr_h * (fb_bpp / 8);
    fb_target = use_gop ? (uint8_t *)gop_fb_addr
                        : (use_vbe ? (uint8_t *)lfb_addr : VRAM);
    fb_stride = use_gop ? gop_pitch
                        : (use_vbe ? vbe_stride
                                   : (uint32_t)scr_w * (uint32_t)(fb_bpp / 8));
    fb_page_bytes = fb_stride * (uint32_t)scr_h;
    build_palette();

    /* buffers (the font buffer is allocated once, persistently).
     * v0.8: ONE backbuffer — the old compose buffer is gone. */
    if (!font_buf) font_buf = kmalloc(FONT_BYTES);
    if (bufs_bytes != fb_bytes) {
        if (backbuf) { kfree(backbuf); backbuf = NULL; }
        backbuf = kmalloc(fb_bytes);
        if (backbuf) bufs_bytes = fb_bytes;
    }
    if (!backbuf || !font_buf) return 0;
    mouse_flush(); /* align packet parsing; drop any stale bytes */
    if (!font_captured) {
        /* GUI glyphs come from the embedded font (font8x16.h),
         * not from a plane-2 capture: identical content, and no
         * dependence on what the firmware left in the VGA (under
         * UEFI there may be no firmware font at all). */
        memcpy(font_buf, VGA_FONT8X16, FONT_BYTES);
        font_captured = 1;
    }

    /* v0.9: decode/scale the wallpaper for this mode (falls back
     * to the gradient desktop when unavailable) */
    wallpaper_prepare();

    /* reset the present pipeline for the new mode */
    flip_mode = 0;
    disp_page = 0;
    draw_page = 0;
    page_cur_x[0] = page_cur_x[1] = -1;
    page_cur_y[0] = page_cur_y[1] = -1;
    cur_mx = cur_my = -1;
    ndirty = 0;
    ndirty_acc = 0;
    clip_full();

    if (use_gop) {
        /* The firmware owns the display mode and has exited boot
         * services; the boot-time text detour left the VGA
         * scanning the text buffer, so restore scanout of the GOP
         * surface by programming the same geometry through DISPI
         * (a single page: no page flipping under GOP, the present
         * path uses dirty blits with the tag's pitch). If DISPI
         * refuses, the surface writes still land; scanout restore
         * is best-effort. */
        vga_apply(&TEXT_REGS); /* normalize VGA regs disturbed by capture */
        vbe_set_mode(scr_w, scr_h, scr_h);
        flip_mode = 0;
    } else if (use_vbe) {
        vga_apply(&TEXT_REGS); /* normalize VGA regs disturbed by capture */
        if (vbe_set_mode(scr_w, scr_h, 2 * scr_h)) {
            /* two pages in VRAM: display-offset page flipping */
            flip_mode = 1;
            draw_page = 1;
        } else if (vbe_set_mode(scr_w, scr_h, scr_h)) {
            flip_mode = 0; /* one VRAM page: dirty blits to page 0 */
        } else {
            /* DISPI programming failed: fall back to mode 13h */
            vbe_disable();
            use_vbe = 0;
            fallback = 1;
            scr_w = 320; scr_h = 200; fb_bpp = 8;
            /* fb_bytes/buffers stay allocated at the larger size: the
             * blit uses fb_bytes, so recompute it to match. */
            fb_bytes = 320u * 200u;
            fb_target = VRAM;
            fb_stride = 320;
            vga_apply(&MODE13_REGS);
            vga_load_palette();
        }
    } else {
        vga_apply(&MODE13_REGS);
        vga_load_palette();
    }
    mouse_set_bounds(scr_w - 1, scr_h - 1);
    mouse_set_pos(scr_w / 2, scr_h / 2);
    gfx_fb_flag = fallback;
    gfx_active = 1;
    gfx_dirty_all(); /* the first present must be a full frame */
    return 1;
}

/* Restore text mode 3 (font first) and the shell's mouse state. */
void gfx_exit(void) {
    if (!gfx_active) return;
    /* snapshot the frame statistics for the `gfxstats` command */
    stat_snap.valid = 1;
    stat_snap.w = scr_w;
    stat_snap.h = scr_h;
    stat_snap.vbe = use_vbe;
    stat_snap.gop = use_gop;
    stat_snap.flip = flip_mode;
    stat_snap.retrace = fstats.retrace_seen;
    stat_snap.frames = fstats.burst_frames;
    stat_snap.active_ticks = fstats.active_ticks;
    stat_snap.worst_ticks = fstats.worst_ticks;
    stat_snap.fps_x10 = stats_fps_x10();
    stat_snap.blit_kb = (uint32_t)(fstats.blit_bytes / 1024);
    stat_snap.pitch = fb_stride;
    stat_snap.pitch_bios = use_vbe && vbe_pitch_bios;
    if (use_vbe || use_gop) vbe_disable();
    mouse_set_bounds(319, 199);
    mouse_set_pos(160, 100);
    restore_font();
    vga_apply(&TEXT_REGS);
    term_clear();
    gfx_active = 0;
}

/* Shell `gfxstats`: report the last graphics session's frame
 * statistics (values accumulate over the boot; the snapshot is
 * taken at gfx_exit). "avg fps" is computed over interaction
 * bursts only: present intervals longer than 100 ms (idle gaps)
 * are excluded from both the frame count base and the time base
 * as far as the interval accounting goes. */
void cmd_gfxstats(void) {
    if (!stat_snap.valid) {
        print("gfxstats: no graphics session yet this boot.\n");
        return;
    }
    print("gfx: ");
    print_dec((uint32_t)stat_snap.w);
    print("x");
    print_dec((uint32_t)stat_snap.h);
    print(stat_snap.gop ? " GOP (UEFI)" : (stat_snap.vbe ? " VBE" : " mode 13h"));
    print(stat_snap.flip ? ", page-flip" : ", dirty blit");
    print(stat_snap.retrace ? ", vsync seen" : ", vsync not seen");
    /* v1.0.1: surface the scanline pitch actually used; on a
     * padded-pitch device (VirtualBox) this visibly differs
     * from width*4, and "(bios)" marks the mode-info source */
    if ((stat_snap.vbe || stat_snap.gop) && stat_snap.pitch) {
        print(", pitch ");
        print_dec(stat_snap.pitch);
        if (stat_snap.pitch_bios) print(" (bios)");
    }
    print("\nframes: ");
    print_dec(stat_snap.frames);
    print("  active: ");
    print_dec(stat_snap.active_ticks * 10);
    print(" ms");
    uint32_t fps_x10 = 0;
    if (stat_snap.active_ticks)
        fps_x10 = (stat_snap.frames * 1000) / stat_snap.active_ticks;
    print("  avg fps: ");
    print_dec(fps_x10 / 10);
    print(".");
    print_dec(fps_x10 % 10);
    print("\nworst frame: ");
    print_dec(stat_snap.worst_ticks * 10);
    print(" ms  blit: ");
    print_dec(stat_snap.blit_kb);
    print(" KB\n");
}

/* ================================================================
 * GUI main loop
 * ================================================================ */
static int gui_want_exit = 0;
static int drag_id = -1, drag_dx = 0, drag_dy = 0;

static void show_raise(int id) {
    int oldf = focused_window();
    wins[id].visible = 1;
    raise_window(id);
    if (oldf >= 0 && oldf != id && wins[oldf].visible) inval_win(oldf);
    inval_win(id);
    /* taskbar button faces show the focus state */
    inval_rect(0, taskbar_y(), scr_w, scr_h - taskbar_y());
}

/* ---------- browser window plumbing ---------- */
static int browser_session_live = 0;
static int browser_mouse_cap = 0;   /* press started in the client */
static int drag_mode = 0;           /* 0 = move, 1 = resize */
static uint32_t last_icon_click = 0;
static int last_icon_id = -1;

static void browser_client_rect(struct grect *r) {
    struct window *w = &wins[WIN_BROWSER];
    r->x = (int16_t)(w->x + 2);
    r->y = (int16_t)(w->y + TITLE_H + 1);
    r->w = (int16_t)(w->w - 4);
    r->h = (int16_t)(w->h - TITLE_H - 3);
}

static void sync_browser_geometry(void) {
    struct grect cr;
    browser_client_rect(&cr);
    browser_win_set_geometry(cr.w, cr.h);
}

static void open_browser_window(const char *url) {
    if (!browser_session_live) {
        sync_browser_geometry();
        browser_win_reset();
        browser_session_live = 1;
    }
    if (url) browser_win_open_url(url);
    show_raise(WIN_BROWSER);
}

static void toggle_browser_maximize(void) {
    struct window *w = &wins[WIN_BROWSER];
    inval_win(WIN_BROWSER);
    if (w->maximized) {
        w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
        w->maximized = 0;
    } else {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->x = 0; w->y = 0; w->w = scr_w; w->h = taskbar_y();
        w->maximized = 1;
    }
    inval_win(WIN_BROWSER);
    sync_browser_geometry();
    browser_win_resized();
}

static void on_mouse_down(int mx, int my) {
    if (my >= taskbar_y()) {
        if (mx >= BTN_INFO_X && mx < BTN_INFO_X + BTN_W) { show_raise(WIN_INFO); return; }
        if (mx >= BTN_TERM_X && mx < BTN_TERM_X + BTN_W) { show_raise(WIN_TERM); return; }
        if (mx >= BTN_WEB_X && mx < BTN_WEB_X + BTN_W) { open_browser_window(NULL); return; }
        if (mx >= BTN_EXIT_X && mx < BTN_EXIT_X + BTN_W) { gui_want_exit = 1; return; }
        return;
    }
    for (int i = WIN_COUNT - 1; i >= 0; i--) {
        int id = zorder[i];
        struct window *w = &wins[id];
        if (!w->visible) continue;
        if (mx < w->x || mx >= w->x + w->w || my < w->y || my >= w->y + w->h)
            continue;
        int oldf = focused_window();
        raise_window(id);
        if (oldf >= 0 && oldf != id && wins[oldf].visible) inval_win(oldf);
        inval_win(id);
        inval_rect(0, taskbar_y(), scr_w, scr_h - taskbar_y());
        /* maximize box (browser only)? */
        if (id == WIN_BROWSER &&
            mx >= w->x + w->w - 33 && mx < w->x + w->w - 21 &&
            my >= w->y + 3 && my < w->y + 15) {
            toggle_browser_maximize();
            return;
        }
        /* close box? */
        if (mx >= w->x + w->w - 16 && mx < w->x + w->w - 4 &&
            my >= w->y + 3 && my < w->y + 15) {
            w->visible = 0;
            inval_win(id);
            inval_rect(0, taskbar_y(), scr_w, scr_h - taskbar_y());
            return;
        }
        /* title bar -> start drag */
        if (my < w->y + TITLE_H) {
            if (!(id == WIN_BROWSER && w->maximized)) {
                drag_id = id;
                drag_mode = 0;
                drag_dx = mx - w->x;
                drag_dy = my - w->y;
            }
            return;
        }
        if (id == WIN_BROWSER) {
            /* resize grip in the bottom-right corner? */
            if (!w->maximized &&
                mx >= w->x + w->w - 14 && my >= w->y + w->h - 14) {
                drag_id = id;
                drag_mode = 1;
                drag_dx = mx - (w->x + w->w);
                drag_dy = my - (w->y + w->h);
                return;
            }
            /* client area: route the press to the browser */
            struct grect cr;
            browser_client_rect(&cr);
            browser_mouse_cap = 1;
            browser_win_mouse_down(mx - cr.x, my - cr.y);
            inval_win(WIN_BROWSER);
            return;
        }
        return;
    }
    /* desktop icons: single click selects, double-click opens */
    for (int i = 0; i < ICON_COUNT; i++) {
        if (mx >= ICON_X - 4 && mx < ICON_X + ICON_TILE + 4 &&
            my >= icon_y(i) - 3 && my < icon_y(i) - 3 + icon_cell_h()) {
            if (last_icon_id == i &&
                timer_ticks - last_icon_click < 100) {
                last_icon_id = -1;
                icon_sel = -1;
                if (i == ICON_WEB) open_browser_window(NULL);
                else if (i == ICON_TERM) show_raise(WIN_TERM);
                else show_raise(WIN_INFO);
            } else {
                last_icon_id = i;
                last_icon_click = timer_ticks;
                icon_sel = i;
            }
            inval_desktop();
            return;
        }
    }
    /* a click on empty desktop clears the icon selection */
    if (icon_sel >= 0) {
        icon_sel = -1;
        inval_desktop();
    }
}

/* ---------- invalidation (v0.8 dirty-rect compositing) ---------- */
static void inval_rect(int x, int y, int w, int h) {
    gfx_dirty_add(x, y, w, h);
}
/* a window's full footprint, including its drop shadow */
static void inval_win(int id) {
    struct window *w = &wins[id];
    inval_rect(w->x - 8, w->y - 8, w->w + 16, w->h + 16);
}
/* the whole desktop area above the taskbar (icon clicks) */
static void inval_desktop(void) {
    inval_rect(0, 0, scr_w, taskbar_y());
}

static int cur_blink = 1;

/* Draw the whole scene (desktop, windows back-to-front, taskbar)
 * into the backbuffer. Callers set the clip rectangle first; only
 * the clipped regions are actually recomposited. */
static void draw_scene(int mx, int my) {
    draw_desktop(backbuf);
    draw_sidebar(backbuf);
    for (int i = 0; i < WIN_COUNT; i++) {
        int id = zorder[i];
        struct window *w = &wins[id];
        if (!w->visible) continue;
        /* cheap reject: window nowhere near the clip rect */
        if (w->x - 8 >= clip_x1 || w->x + w->w + 8 <= clip_x0 ||
            w->y - 8 >= clip_y1 || w->y + w->h + 8 <= clip_y0)
            continue;
        draw_window_frame(backbuf, id, id == focused_window());
        if (id == WIN_INFO) draw_info_content(backbuf);
        else if (id == WIN_TERM) draw_term_content(backbuf, cur_blink);
        else if (id == WIN_BROWSER && browser_session_live) {
            struct grect cr;
            browser_client_rect(&cr);
            gfx_set_viewport(cr.x, cr.y, cr.w, cr.h);
            browser_win_draw(mx - cr.x, my - cr.y);
            gfx_clear_viewport();
        }
    }
    draw_taskbar(backbuf);
}

/* Recomposite the invalidated regions into the backbuffer, then
 * present them (the present step copies only those regions). */
static void compose_and_present(int mx, int my) {
    for (int i = 0; i < ndirty; i++) {
        clip_set(dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h);
        draw_scene(mx, my);
    }
    clip_full();
    present(mx, my, 0);
}

static void gui_main(const char *browser_url) {
    /* ---- enter the mode from the persisted setting ---- */
    if (!gfx_enter()) {
        print("gui: out of memory\n");
        return;
    }

    wins_reset();
    term_nlines = 0;
    term_ilen = 0;
    term_input[0] = 0;
    term_add_line("Shiro Term - type 'help'.");
    gui_want_exit = 0;
    drag_id = -1;
    drag_mode = 0;
    browser_session_live = 0;
    browser_mouse_cap = 0;
    last_icon_click = 0;
    if (browser_url) {
        /* `web <url>`: open the browser window and load the page
         * before the first frame */
        open_browser_window(browser_url);
        browser_win_pump();
        inval_win(WIN_BROWSER);
    }

    int prev_btn = 0, prev_mx = -1, prev_my = -1;
    uint32_t last_sec = 0xFFFFFFFFu;
    int last_blink = -1;
    /* 60 fps frame pacing on the 100 Hz tick: present slots are
     * spaced 2,2,1 ticks apart (16.7 ms average); the slot gate is
     * the frame budget, the retrace only aligns flips. */
    uint32_t next_slot = 0;
    int slot_phase = 0;

    int mx, my, mb;
    mouse_state(&mx, &my, &mb);
    prev_mx = mx; prev_my = my; prev_btn = mb;
    cur_blink = 1;
    compose_and_present(mx, my);
    next_slot = timer_ticks;

    while (!gui_want_exit) {
        int routed_term = 0;
        int k;
        while ((k = keyboard_try_key()) >= 0) {
            int foc = focused_window();
            if (k == 27) {
                /* ESC cancels address editing in the browser;
                 * otherwise it leaves the GUI (unchanged) */
                if (foc == WIN_BROWSER && wins[WIN_BROWSER].visible &&
                    browser_win_editing()) {
                    browser_win_key(27);
                    inval_win(WIN_BROWSER);
                    continue;
                }
                gui_want_exit = 1;
                break;
            }
            if (foc == WIN_TERM && wins[WIN_TERM].visible) {
                term_key(k);
                routed_term = 1;
            } else if (foc == WIN_BROWSER && wins[WIN_BROWSER].visible) {
                browser_win_key(k);
                inval_win(WIN_BROWSER);
            }
        }
        if (gui_want_exit) break;
        if (routed_term) inval_win(WIN_TERM);

        mouse_state(&mx, &my, &mb);
        if ((mb & 1) && !(prev_btn & 1)) on_mouse_down(mx, my);
        if (!(mb & 1) && drag_id >= 0) {
            /* drag end: a browser resize re-lays-out the page */
            if (drag_mode == 1 && drag_id == WIN_BROWSER) {
                sync_browser_geometry();
                browser_win_resized();
                inval_win(WIN_BROWSER);
            }
            drag_id = -1;
        }
        if (drag_id >= 0 && (mb & 1)) {
            struct window *w = &wins[drag_id];
            if (drag_mode == 1) {
                int nw = (mx - drag_dx) - w->x;
                int nh = (my - drag_dy) - w->y;
                if (nw < 220) nw = 220;
                if (nh < 150) nh = 150;
                if (w->x + nw > scr_w) nw = scr_w - w->x;
                if (w->y + nh > taskbar_y()) nh = taskbar_y() - w->y;
                if (nw != w->w || nh != w->h) {
                    inval_win(drag_id);
                    w->w = nw;
                    w->h = nh;
                    inval_win(drag_id);
                }
            } else {
                int nx = mx - drag_dx;
                int ny = my - drag_dy;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx > scr_w - w->w) nx = scr_w - w->w;
                if (ny > taskbar_y() - w->h) ny = taskbar_y() - w->h;
                if (nx != w->x || ny != w->y) {
                    inval_win(drag_id);
                    w->x = nx;
                    w->y = ny;
                    inval_win(drag_id);
                }
            }
        }
        /* browser mouse routing: captured drags and hover */
        if (wins[WIN_BROWSER].visible && browser_session_live) {
            struct grect cr;
            browser_client_rect(&cr);
            if ((mb & 1) && browser_mouse_cap) {
                if (browser_win_mouse_move(mx - cr.x, my - cr.y, mb))
                    inval_win(WIN_BROWSER);
            } else if (!(mb & 1)) {
                browser_mouse_cap = 0;
                if (mx >= cr.x && mx < cr.x + cr.w &&
                    my >= cr.y && my < cr.y + cr.h &&
                    focused_window() == WIN_BROWSER &&
                    browser_win_mouse_move(mx - cr.x, my - cr.y, 0))
                    inval_rect(cr.x, cr.y + cr.h - 16, cr.w, 16);
            }
        }
        /* browser background work: navigation + resize reparse */
        if (wins[WIN_BROWSER].visible && browser_session_live) {
            if (browser_win_pump()) inval_win(WIN_BROWSER);
            if (browser_win_want_close()) {
                browser_win_ack_close();
                inval_win(WIN_BROWSER);
                wins[WIN_BROWSER].visible = 0;
                inval_win(WIN_BROWSER);
                inval_rect(0, taskbar_y(), scr_w, scr_h - taskbar_y());
            }
        }
        int cursor_moved = (mx != prev_mx || my != prev_my);
        prev_btn = mb; prev_mx = mx; prev_my = my;

        uint32_t now_s = uptime_seconds();
        if (now_s != last_sec) {
            last_sec = now_s;
            /* taskbar clock + the Info window's live lines (the
             * Uptime row and the FPS row are all that change) */
            inval_rect(scr_w - 84, taskbar_y(), 84, scr_h - taskbar_y());
            if (sidebar_on()) /* sidebar clock ticks every second */
                inval_rect(sb_x() - 2, 6, SB_W + 4, taskbar_y() - 12);
            if (wins[WIN_INFO].visible) {
                struct window *iw = &wins[WIN_INFO];
                inval_rect(iw->x - 8, iw->y + 40, iw->w + 16, 20);
                inval_rect(iw->x - 8, iw->y + 100, iw->w + 16, 20);
            }
        }
        int blink_on = (timer_ticks / 50) % 2 == 0;
        if (blink_on != last_blink) {
            last_blink = blink_on;
            cur_blink = blink_on;
            /* the Term prompt cursor blink (paused while dragging):
             * only the prompt row at the window's bottom can change */
            if (drag_id < 0 && wins[WIN_TERM].visible) {
                struct window *tw = &wins[WIN_TERM];
                inval_rect(tw->x - 8, tw->y + tw->h - 34, tw->w + 16, 42);
            }
        }

        if (ndirty > 0 || cursor_moved) {
            /* the slot gate is the frame budget: it applies even
             * when the retrace is observable (QEMU's virtual
             * retrace is not a reliable 60 Hz throttle) — the
             * retrace only aligns the flip inside present(). */
            if (timer_ticks >= next_slot) {
                compose_and_present(mx, my);
                /* absolute schedule: advance from the previous slot,
                 * not from now, so the frame work doesn't stretch
                 * the period; resync if we fell a frame behind */
                next_slot += (slot_phase == 2 ? 1 : 2);
                slot_phase = (slot_phase + 1) % 3;
                if (timer_ticks >= next_slot + 2) next_slot = timer_ticks;
            }
        }
        __asm__ volatile ("hlt");
    }

    /* back to text mode (font restored first, inside gfx_exit) */
    gfx_exit();
    int fw = 0, fh = 0;
    if (gfx_fallback(&fw, &fh)) {
        print("gui: VBE mode ");
        print_dec((uint32_t)fw);
        print("x");
        print_dec((uint32_t)fh);
        print(" unavailable; used mode 13h (320x200).\n");
    }
}

void gui_run(void) {
    gui_main(NULL);
    net_cookie_clear(); /* the cookie jar lives only for a GUI session */
}

void gui_run_browser(const char *url) {
    gui_main(url);
    net_cookie_clear();
}
