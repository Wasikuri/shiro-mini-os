/* browser.c - Shiro Mini OS graphical web browser (v1.0).
 *
 * Shell command: web <url>   (http:// and https://)
 *
 * Multiple TABS with per-tab history (back/forward stacks), page
 * cache (parsed lines), scroll position and title. Modern Firefox
 * (Proton) style chrome: dark tab strip with the active tab as a
 * light "floating" rounded tab, flat back/forward/reload buttons,
 * ONE rounded pill-shaped unified address bar, slim status bar.
 * The browser is a window of the desktop WM (gui.c owns the
 * frame, input routing and present; this file renders the client
 * area via the gfx_* viewport seam).
 *
 * v1.0 (browser completeness stage):
 *  - CSS subset: <style> blocks, style="" attributes and
 *    <link rel=stylesheet> sheets feed a small cascade (tag /
 *    .class / #id / inline; later rules win on ties; color and
 *    font properties inherit) for color, background-color,
 *    font-size/weight/style, text-align, text-decoration,
 *    display (incl. none), margin/padding, border (solid),
 *    width, line-height and list-style-type. Colors are true
 *    0xRRGGBB end to end (gfx_text2/gfx_fill_rgb in gui.c).
 *  - Images: <img> fetched over http/https and decoded with the
 *    in-tree stb_image (JPEG/PNG/GIF first frame), scaled into
 *    the flow with alpha blending; alt text when an image is
 *    skipped or fails (caps: 16 per page, 1 MB encoded, ~2 Mpx).
 *  - Forms: text/password/checkbox/radio/select/textarea/hidden
 *    controls, clickable and keyboard-editable; GET and POST
 *    (application/x-www-form-urlencoded via net_post) submit as
 *    normal navigations. Enter in a text field submits when the
 *    form has a submit control.
 *  - Pages up to 256 KB (the fetch stops at the cap instead of
 *    draining a huge page to the end), 1024 display lines.
 *  - Cookies: net.c's in-memory jar (per host, GUI-session only).
 *  - Navigation NEVER leaves the browser: failures keep the old
 *    page and say so in the status bar; a Duktape fatal error is
 *    recovered via longjmp instead of halting the machine.
 *
 * JavaScript: <script> blocks run after layout in a Duktape heap
 * with a small DOM subset: document.title, document.write(),
 * document.getElementById(id).textContent, console.log().
 * Scripts get NO file/network/OS APIs; a throwing script is
 * caught and never breaks rendering. See README for the exact
 * subset and its limits.
 *
 * Controls: click links/tabs/controls; Ctrl+T new tab, Ctrl+W
 * close tab, Ctrl+L address bar, Ctrl+R reload, Left/Right =
 * back/forward, PgUp/PgDn + arrows scroll, Tab moves between
 * form fields, ESC cancels editing / form focus (the window
 * manager owns ESC otherwise: GUI exit).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os.h"
#include "net.h"
#include "duktape.h"

void *memcpy(void *dst, const void *src, size_t n); /* net.c */
void *memset(void *dst, int v, size_t n);           /* net.c */
void *memmove(void *dst, const void *src, size_t n); /* net.c */

/* ---------- small string helpers ---------- */
static size_t b_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void b_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = 0;
}
static void b_strncpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static int b_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
static int b_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i] || a[i] == 0) return (uint8_t)a[i] - (uint8_t)b[i];
    return 0;
}
static char b_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int b_strcasecmp(const char *a, const char *b) {
    while (*a && b_lower(*a) == b_lower(*b)) { a++; b++; }
    return (uint8_t)b_lower(*a) - (uint8_t)b_lower(*b);
}
static int b_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (b_lower(a[i]) != b_lower(b[i]) || a[i] == 0)
            return (uint8_t)b_lower(a[i]) - (uint8_t)b_lower(b[i]);
    return 0;
}
static void b_strcat(char *dst, const char *src) {
    while (*dst) dst++;
    b_strcpy(dst, src);
}

/* ---------- layout model ---------- */
/* Colors are 0xRRGGBB throughout the layout/render path. The
 * built-in defaults below are the exact RGB values the old
 * palette indices produced in screendumps, so existing pixel
 * checks (page bg from bgcolor=, link blue) are unchanged:
 *   link blue  (85,85,255)   = old palette 9
 *   body text  (24,32,36)    = old COL_INFO_TEXT
 *   heading    (12,125,150)  = old COL_TITLE_ACT
 *   page face  (239,247,251) = old COL_WIN_FACE */
#define RGB_LINK  0x5555FFu
#define RGB_TEXT  0x182024u
#define RGB_HEAD  0x0C7D96u
#define RGB_FAINT 0x555555u
#define RGB_FACE  0xEFF7FBu

#define MAX_LINES 1024
#define LINE_TEXT 200
#define LINE_SPANS 12
/* span style bits (shared with gfx_text2) */
#define ST_BOLD 1
#define ST_ITALIC 2
#define ST_UNDER 4
struct dspan {
    uint16_t col;      /* x offset in px from the content left edge */
    uint16_t coff;     /* char offset into the line's text */
    uint8_t len;       /* chars */
    uint8_t scale;     /* font scale 1..3 */
    uint8_t style;     /* ST_* bits */
    uint32_t color;    /* 0xRRGGBB */
    int16_t link;      /* tab link index, -1 none, -2 non-navigable */
};
/* widgets embedded in a display line (at most one per line) */
#define W_NONE 0
#define W_IMG  1
#define W_CTRL 2
struct dline {
    char text[LINE_TEXT];
    struct dspan spans[LINE_SPANS];
    uint8_t nspans;
    uint16_t height;   /* full line height in px (incl. space above) */
    uint16_t ch;       /* content height in px (excl. space above) */
    uint8_t rule;      /* 1: horizontal rule */
    int32_t bg;        /* -1: none; else block background 0xRRGGBB */
    uint16_t bg_x;     /* background/border rect, content coords */
    uint16_t bg_w;
    uint16_t bg_off;   /* px from line top where the bg rect starts */
    uint8_t border;    /* bits: 1 top, 2 bottom, 4 left, 8 right */
    uint32_t border_rgb;
    uint8_t wkind;     /* W_* */
    int16_t widx;      /* image or control index */
    uint16_t wx, ww, wh; /* widget geometry (content coords / px) */
};

#define MAX_LINKS 96
#define HIST_MAX 16
#define URL_LEN 640

/* ---------- forms ---------- */
enum { CT_TEXT, CT_PASS, CT_CHECK, CT_RADIO, CT_SUBMIT, CT_BUTTON,
       CT_HIDDEN, CT_SELECT, CT_TEXTAREA };
#define MAX_CTRLS 48
#define MAX_FORMS 8
#define CTRL_VAL 192
struct ctrl {
    uint8_t type;
    char name[48];
    char value[CTRL_VAL];
    int checked;
    int form;          /* form index or -1 */
    int line;          /* display line the widget sits on */
    char opts[6][40];  /* select: option labels */
    char ovals[6][48]; /* select: option values */
    int nopts, sel;
};
struct form {
    char action[URL_LEN];
    int post;          /* 0 = GET, 1 = POST */
    int first, count;  /* range in the tab's ctrl array */
};

/* ---------- images ---------- */
#define MAX_IMGS 16
struct img {
    char url[URL_LEN];
    uint8_t *px;       /* owned RGBA at natural size (or NULL) */
    int nw, nh;        /* natural size; 0 = not decoded */
    int ok;            /* decoded and usable */
};

struct tab {
    char url[URL_LEN];
    char title[128];
    /* The line array lives on the heap, allocated lazily per tab:
     * 6 tabs x 1024 lines x ~440 B of static BSS pushed
     * _kernel_end past 0x400000, where user programs are linked,
     * and the ELF loader (correctly) refused to load them. */
    struct dline *lines;
    int nlines;
    char links[MAX_LINKS][256];
    int nlinks;
    uint32_t content_bg;
    int scroll_y;
    int total_h;
    char back[HIST_MAX][URL_LEN];
    int nback;
    char fwd[HIST_MAX][URL_LEN];
    int nfwd;
    int loaded;      /* a page (or the start page) is present */
    int is_start;    /* showing the built-in start page */
    char *html;      /* owned copy of the page source (resize reparse) */
    int layout_w;    /* client width the current layout was made for */
    struct ctrl ctrls[MAX_CTRLS];
    int nctrls;
    struct form forms[MAX_FORMS];
    int nforms;
    int focus;       /* focused ctrl index or -1 */
    struct img imgs[MAX_IMGS];
    int nimgs;
    uint32_t img_bytes; /* decoded RGBA bytes currently held */
};

#define MAX_TABS 6
static struct tab tabs[MAX_TABS];
static int ntabs = 0;
static int cur_tab = 0;

/* client size, defined in the chrome section below */
static int bv_w, bv_h;

/* ---------- fetch buffer ---------- */
#define HTML_SIZE 262144 /* 256 KB page cap (v1.0) */
static char page_html[HTML_SIZE + 1];
static size_t page_len = 0;
static int page_truncated = 0;

static int fetch_sink(const void *data, size_t len, void *ctx) {
    (void)ctx;
    if (page_len < HTML_SIZE) {
        size_t room = HTML_SIZE - page_len;
        size_t take = len < room ? len : room;
        memcpy(page_html + page_len, data, take);
        page_len += take;
    }
    /* returning 0 once full stops the transfer (net_download_
     * stopok reports that as success-with-truncation), so a huge
     * page no longer has to drain to the last byte */
    return page_len < HTML_SIZE;
}

/* Fetch `url` (http or https) into page_html. Console progress is
 * printed by net.c. Returns 1 on success (including a page cut at
 * the 256 KB cap, flagged in page_truncated). */
static int fetch_page(const char *url) {
    page_len = 0;
    page_truncated = 0;
    print("web: fetching ");
    print(url);
    print(" ...\n");
    int rc = net_download_stopok(url, fetch_sink, NULL, NULL);
    page_html[page_len] = 0;
    if (rc == 2) page_truncated = 1;
    if (rc != 1 && rc != 2) {
        print("web: fetch failed.\n");
        return 0;
    }
    return 1;
}

/* stb_image (already linked for the desktop wallpaper) */
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len,
                                     int *x, int *y,
                                     int *channels_in_file,
                                     int desired_channels);
int stbi_info_from_memory(const unsigned char *buffer, int len,
                          int *x, int *y, int *comp);
void stbi_image_free(void *retval_from_stbi_load);

/* ---------- URL resolution (from the prototype) ---------- */
static void strip_fragment(char *url) {
    for (int i = 0; url[i]; i++)
        if (url[i] == '#') { url[i] = 0; return; }
}
/* Normalize "." and ".." segments in the path part of `url`. */
static void normalize_path(char *url) {
    char *auth = url;
    const char *p = url;
    while (*p && !(p[0] == ':' && p[1] == '/' && p[2] == '/')) p++;
    if (*p) auth = (char *)p + 3;
    char *path = auth;
    while (*path && *path != '/') path++;
    if (!*path) return;
    char out[URL_LEN];
    size_t oi = 0;
    while (url + oi < path && oi < sizeof(out) - 1) { out[oi] = url[oi]; oi++; }
    const char *s = path;
    int seg_start[64];
    int nseg = 0;
    while (*s) {
        while (*s == '/') s++;
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t sl = (size_t)(e - s);
        if (sl == 0) break;
        if (sl == 1 && s[0] == '.') { /* drop */ }
        else if (sl == 2 && s[0] == '.' && s[1] == '.') {
            if (nseg > 0) { nseg--; oi = (size_t)seg_start[nseg]; }
        } else {
            if (nseg < 64) seg_start[nseg++] = (int)oi;
            if (oi < sizeof(out) - 1) out[oi++] = '/';
            for (size_t i = 0; i < sl && oi < sizeof(out) - 1; i++)
                out[oi++] = s[i];
        }
        s = e;
    }
    if (oi == (size_t)(path - url)) { /* path fully consumed: root */
        if (oi < sizeof(out) - 1) out[oi++] = '/';
    }
    out[oi] = 0;
    b_strcpy(url, out);
}
static void resolve_url(const char *base, const char *href, char *out, size_t outsz) {
    while (*href == ' ' || *href == '\t') href++;
    static char tmp[URL_LEN];
    size_t hl = b_strlen(href);
    while (hl > 0 && (href[hl - 1] == ' ' || href[hl - 1] == '\t')) hl--;
    if (hl > sizeof(tmp) - 1) hl = sizeof(tmp) - 1;
    for (size_t i = 0; i < hl; i++) tmp[i] = href[i];
    tmp[hl] = 0;

    if (tmp[0] == 0 || tmp[0] == '#') { /* same page */
        b_strncpy(out, base, outsz);
        strip_fragment(out);
        return;
    }
    int has_scheme = 0;
    for (int i = 0; tmp[i]; i++) {
        if (tmp[i] == ':') { has_scheme = 1; break; }
        if (tmp[i] == '/' || tmp[i] == '?' || tmp[i] == '#') break;
        if (!((tmp[i] >= 'a' && tmp[i] <= 'z') || (tmp[i] >= 'A' && tmp[i] <= 'Z') ||
              (tmp[i] >= '0' && tmp[i] <= '9') || tmp[i] == '+' || tmp[i] == '-' ||
              tmp[i] == '.'))
            break;
    }
    if (has_scheme) {
        strip_fragment(tmp);
        b_strncpy(out, tmp, outsz);
        return;
    }
    static char res[URL_LEN];
    size_t ri = 0;
    const char *b = base;
    if (tmp[0] == '/' && tmp[1] == '/') {
        const char *colon = b;
        while (*colon && *colon != ':') colon++;
        size_t sl = (size_t)(colon - b);
        for (size_t i = 0; i < sl && ri < sizeof(res) - 1; i++) res[ri++] = b[i];
        if (ri < sizeof(res) - 1) res[ri++] = ':';
        for (size_t i = 0; tmp[i] && ri < sizeof(res) - 1; i++) res[ri++] = tmp[i];
        res[ri] = 0;
        strip_fragment(res);
        b_strncpy(out, res, outsz);
        return;
    }
    const char *auth = b;
    while (*auth && !(auth[0] == ':' && auth[1] == '/' && auth[2] == '/')) auth++;
    if (*auth) auth += 3;
    const char *aend = auth;
    while (*aend && *aend != '/') aend++;
    size_t prefix = (size_t)(aend - b); /* scheme + authority */
    for (size_t i = 0; i < prefix && ri < sizeof(res) - 1; i++) res[ri++] = b[i];
    if (tmp[0] == '/') {
        for (size_t i = 0; tmp[i] && ri < sizeof(res) - 1; i++) res[ri++] = tmp[i];
    } else {
        const char *bp = b + prefix;
        const char *last = bp;
        for (const char *q = bp; *q && *q != '?' && *q != '#'; q++)
            if (*q == '/') last = q;
        for (const char *q = bp; q <= last && ri < sizeof(res) - 1; q++)
            res[ri++] = *q;
        if (bp == last && *bp != '/') {
            if (ri < sizeof(res) - 1) res[ri++] = '/';
        }
        for (size_t i = 0; tmp[i] && ri < sizeof(res) - 1; i++) res[ri++] = tmp[i];
    }
    res[ri] = 0;
    strip_fragment(res);
    normalize_path(res);
    b_strncpy(out, res, outsz);
}

/* Normalize user input into an absolute URL (scheme defaults to http). */
static void make_absolute(const char *in, char *out, size_t outsz) {
    while (*in == ' ') in++;
    if (b_strncmp(in, "http://", 7) == 0 || b_strncmp(in, "https://", 8) == 0) {
        b_strncpy(out, in, outsz);
        return;
    }
    char tmp[URL_LEN];
    b_strcpy(tmp, "http://");
    size_t l = b_strlen(tmp);
    size_t i = 0;
    while (in[i] && l < sizeof(tmp) - 1) tmp[l++] = in[i++];
    tmp[l] = 0;
    b_strncpy(out, tmp, outsz);
}

/* ================================================================
 * CSS subset (v1.0)
 *
 * Sources, in document order: <link rel=stylesheet> sheets and
 * <style> blocks are gathered by prepare_assets() into css_text;
 * style="" attributes are parsed per element. The cascade is:
 * inherited < UA defaults < tag rules < .class rules < #id rules
 * < inline style; within one level, later rules win. Only color
 * and the font properties inherit. Selectors with combinators
 * are reduced to their last compound (documented simplification);
 * pseudo-classes are stripped.
 * ================================================================ */
struct cprops {
    int32_t color;    /* -1 unset, else 0xRRGGBB */
    int32_t bg;       /* -1 unset */
    int font_px;      /* 0 unset */
    int8_t bold, italic, under;   /* -1 unset */
    int8_t align;     /* -1 unset, 0 left, 1 center, 2 right */
    int8_t display;   /* -1 unset, 0 normal, 1 none */
    int8_t dblock;    /* -1 unset, 1 = display:block, 0 = inline */
    int8_t list_none; /* -1 unset */
    int margin_v, margin_h, pad_v, pad_h; /* -1 unset, px */
    int32_t border;   /* -1 none, else 0xRRGGBB (solid, 1px) */
    int width_px, width_pct;      /* -1 unset */
    int lh_pct;       /* 0 unset, else line-height in % of font */
};
static void props_clear(struct cprops *p) {
    p->color = -1; p->bg = -1; p->font_px = 0;
    p->bold = p->italic = p->under = -1;
    p->align = -1; p->display = -1; p->dblock = -1; p->list_none = -1;
    p->margin_v = p->margin_h = p->pad_v = p->pad_h = -1;
    p->border = -1; p->width_px = p->width_pct = -1; p->lh_pct = 0;
}
static void props_merge(struct cprops *d, const struct cprops *s) {
    if (s->color >= 0) d->color = s->color;
    if (s->bg >= 0) d->bg = s->bg;
    if (s->font_px > 0) d->font_px = s->font_px;
    if (s->bold >= 0) d->bold = s->bold;
    if (s->italic >= 0) d->italic = s->italic;
    if (s->under >= 0) d->under = s->under;
    if (s->align >= 0) d->align = s->align;
    if (s->display >= 0) d->display = s->display;
    if (s->dblock >= 0) d->dblock = s->dblock;
    if (s->list_none >= 0) d->list_none = s->list_none;
    if (s->margin_v >= 0) d->margin_v = s->margin_v;
    if (s->margin_h >= 0) d->margin_h = s->margin_h;
    if (s->pad_v >= 0) d->pad_v = s->pad_v;
    if (s->pad_h >= 0) d->pad_h = s->pad_h;
    if (s->border >= 0) d->border = s->border;
    if (s->width_px >= 0) d->width_px = s->width_px;
    if (s->width_pct >= 0) d->width_pct = s->width_pct;
    if (s->lh_pct > 0) d->lh_pct = s->lh_pct;
}

static const struct { const char *name; uint32_t rgb; } NAMED_COLORS[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 },
    { "green", 0x008000 }, { "blue", 0x0000FF }, { "yellow", 0xFFFF00 },
    { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 },
    { "maroon", 0x800000 }, { "purple", 0x800080 }, { "fuchsia", 0xFF00FF },
    { "lime", 0x00FF00 }, { "olive", 0x808000 }, { "navy", 0x000080 },
    { "teal", 0x008080 }, { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF },
    { "magenta", 0xFF00FF }, { "orange", 0xFFA500 }, { "pink", 0xFFC0CB },
    { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "beige", 0xF5F5DC },
    { "ivory", 0xFFFFF0 }, { "coral", 0xFF7F50 }, { "salmon", 0xFA8072 },
    { "khaki", 0xF0E68C }, { "indigo", 0x4B0082 }, { "violet", 0xEE82EE },
    { "crimson", 0xDC143C }, { "tomato", 0xFF6347 }, { "skyblue", 0x87CEEB },
    { "lightblue", 0xADD8E6 }, { "lightgreen", 0x90EE90 },
    { "darkblue", 0x00008B }, { "darkgreen", 0x006400 },
    { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 },
    { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
    { "slategray", 0x708090 }, { "steelblue", 0x4682B4 },
    { "seagreen", 0x2E8B57 }, { "forestgreen", 0x228B22 },
    { "royalblue", 0x4169E1 }, { "midnightblue", 0x191970 },
    { "lavender", 0xE6E6FA }, { "snow", 0xFFFAFA },
    { "gainsboro", 0xDCDCDC }, { "whitesmoke", 0xF5F5F5 },
    { "rebeccapurple", 0x663399 },
};

/* Parse a CSS color at s (not NUL-terminated necessarily: stops
 * at the first char that can't belong). Returns 0xRRGGBB or -1. */
static int hexval(char c);
static int32_t css_color(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') {
        s++;
        int v[6], n = 0, hv;
        while (n < 6 && (hv = hexval(*s)) >= 0) { v[n++] = hv; s++; }
        if (n == 3) return (int32_t)(((uint32_t)(v[0] * 17) << 16) |
                                     ((uint32_t)(v[1] * 17) << 8) |
                                     (uint32_t)(v[2] * 17));
        if (n == 6) return (int32_t)(((uint32_t)(v[0] * 16 + v[1]) << 16) |
                                     ((uint32_t)(v[2] * 16 + v[3]) << 8) |
                                     (uint32_t)(v[4] * 16 + v[5]));
        return -1;
    }
    if (b_strncasecmp(s, "rgb", 3) == 0) {
        const char *p = s + 3;
        while (*p && *p != '(') p++;
        if (*p != '(') return -1;
        p++;
        int comp[3] = { 0, 0, 0 };
        for (int i = 0; i < 3; i++) {
            while (*p == ' ') p++;
            int v = 0, any = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
            if (!any) return -1;
            if (*p == '%') { v = v * 255 / 100; p++; }
            comp[i] = v > 255 ? 255 : v;
            while (*p == ' ' || *p == ',') p++;
        }
        return (int32_t)(((uint32_t)comp[0] << 16) | ((uint32_t)comp[1] << 8) |
                         (uint32_t)comp[2]);
    }
    char name[24];
    int ni = 0;
    while (s[ni] && ni < 23 && ((s[ni] >= 'a' && s[ni] <= 'z') ||
                                (s[ni] >= 'A' && s[ni] <= 'Z')))
        { name[ni] = b_lower(s[ni]); ni++; }
    name[ni] = 0;
    if (ni == 0) return -1;
    for (size_t i = 0; i < sizeof(NAMED_COLORS) / sizeof(NAMED_COLORS[0]); i++)
        if (b_strcmp(name, NAMED_COLORS[i].name) == 0)
            return (int32_t)NAMED_COLORS[i].rgb;
    return -1;
}

/* Extract the first color appearing anywhere in a value (for the
 * `background` and `border` shorthands). -1 if none. */
static int32_t css_color_in(const char *v) {
    for (const char *p = v; *p; p++) {
        if (*p == '#' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            int32_t c = css_color(p);
            if (c >= 0) return c;
            if (*p != '#') { while (*p && *p != ' ' && *p != '\t') p++; if (!*p) break; }
        }
    }
    return -1;
}

/* Parse a CSS length: px/pt/em/rem/%/bare number -> px (or -1).
 * kind: 0 = px result in *px; the caller handles % separately. */
static int css_len_px(const char *v, int parent_px) {
    while (*v == ' ' || *v == '\t') v++;
    if (b_strncasecmp(v, "auto", 4) == 0) return 0;
    int num = 0, den = 1, any = 0, neg = 0;
    if (*v == '-') { neg = 1; v++; }
    while (*v >= '0' && *v <= '9') { num = num * 10 + (*v - '0'); v++; any = 1; }
    if (*v == '.') {
        v++;
        while (*v >= '0' && *v <= '9') { num = num * 10 + (*v - '0'); den *= 10; v++; }
    }
    if (!any) return -1;
    int px = num / den;
    if (b_strncasecmp(v, "pt", 2) == 0) px = px * 4 / 3;
    else if (b_strncasecmp(v, "em", 2) == 0 || b_strncasecmp(v, "rem", 3) == 0)
        px = px * (parent_px > 0 ? parent_px : 16);
    else if (*v == '%') px = -2; /* percentage: caller interprets */
    return neg ? -px : px;
}

static void css_apply_decl(struct cprops *p, const char *name, const char *val) {
    if (b_strcmp(name, "color") == 0) {
        int32_t c = css_color(val);
        if (c >= 0) p->color = c;
    } else if (b_strcmp(name, "background-color") == 0) {
        int32_t c = css_color(val);
        if (c >= 0) p->bg = c;
    } else if (b_strcmp(name, "background") == 0) {
        int32_t c = css_color_in(val);
        if (c >= 0) p->bg = c;
    } else if (b_strcmp(name, "font-size") == 0) {
        static const struct { const char *k; int px; } kw[] = {
            { "xx-small", 9 }, { "x-small", 11 }, { "small", 13 },
            { "medium", 16 }, { "large", 20 }, { "x-large", 24 },
            { "xx-large", 32 },
        };
        int done = 0;
        for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
            if (b_strcasecmp(val, kw[i].k) == 0) { p->font_px = kw[i].px; done = 1; }
        if (!done) {
            if (b_strcasecmp(val, "larger") == 0) p->font_px = 20;
            else if (b_strcasecmp(val, "smaller") == 0) p->font_px = 13;
            else {
                /* % of the 16px base (parent tracking is approximate) */
                const char *q = val;
                while (*q && *q != '%') q++;
                int px = css_len_px(val, 16);
                if (*q == '%') {
                    int n = 0; const char *r = val;
                    while (*r >= '0' && *r <= '9') { n = n * 10 + (*r - '0'); r++; }
                    px = 16 * n / 100;
                }
                if (px > 0) p->font_px = px;
            }
        }
    } else if (b_strcmp(name, "font-weight") == 0) {
        if (b_strcasecmp(val, "bold") == 0 || b_strcasecmp(val, "bolder") == 0 ||
            b_strcmp(val, "600") == 0 || b_strcmp(val, "700") == 0 ||
            b_strcmp(val, "800") == 0 || b_strcmp(val, "900") == 0)
            p->bold = 1;
        else if (b_strcasecmp(val, "normal") == 0 || b_strcasecmp(val, "lighter") == 0)
            p->bold = 0;
    } else if (b_strcmp(name, "font-style") == 0) {
        p->italic = (b_strcasecmp(val, "italic") == 0 ||
                     b_strcasecmp(val, "oblique") == 0) ? 1 : 0;
    } else if (b_strcmp(name, "text-align") == 0) {
        if (b_strcasecmp(val, "center") == 0) p->align = 1;
        else if (b_strcasecmp(val, "right") == 0) p->align = 2;
        else if (b_strcasecmp(val, "left") == 0 ||
                 b_strcasecmp(val, "justify") == 0) p->align = 0;
    } else if (b_strcmp(name, "text-decoration") == 0) {
        /* scan tokens for underline / none */
        int under = -1;
        for (const char *q = val; *q; q++) {
            if (b_strncasecmp(q, "underline", 9) == 0) under = 1;
            if (b_strncasecmp(q, "none", 4) == 0 && under < 0) under = 0;
        }
        if (under >= 0) p->under = (int8_t)under;
    } else if (b_strcmp(name, "display") == 0) {
        if (b_strcasecmp(val, "none") == 0) p->display = 1;
        else {
            p->display = 0;
            if (b_strcasecmp(val, "block") == 0) p->dblock = 1;
            else if (b_strcasecmp(val, "inline") == 0) p->dblock = 0;
            else if (b_strncasecmp(val, "inline-block", 12) == 0) p->dblock = 0;
            else p->dblock = 1; /* flex/grid/list-item/table...: block-ish */
        }
    } else if (b_strcmp(name, "margin") == 0 || b_strcmp(name, "padding") == 0) {
        /* shorthand: 1-4 values; we keep vertical + horizontal */
        int vals[4], nv = 0;
        const char *q = val;
        while (*q && nv < 4) {
            while (*q == ' ' || *q == '\t') q++;
            if (!*q) break;
            int px = css_len_px(q, 16);
            vals[nv++] = px < 0 ? 0 : px;
            while (*q && *q != ' ' && *q != '\t') q++;
        }
        if (nv > 0) {
            int vv = vals[0], hh = nv > 1 ? vals[1] : vals[0];
            if (name[0] == 'm') { p->margin_v = vv; p->margin_h = hh; }
            else { p->pad_v = vv; p->pad_h = hh; }
        }
    } else if (b_strcmp(name, "margin-top") == 0 || b_strcmp(name, "margin-bottom") == 0) {
        int px = css_len_px(val, 16); if (px >= 0) p->margin_v = px;
    } else if (b_strcmp(name, "margin-left") == 0 || b_strcmp(name, "margin-right") == 0) {
        int px = css_len_px(val, 16); if (px >= 0) p->margin_h = px;
    } else if (b_strcmp(name, "padding-top") == 0 || b_strcmp(name, "padding-bottom") == 0) {
        int px = css_len_px(val, 16); if (px >= 0) p->pad_v = px;
    } else if (b_strcmp(name, "padding-left") == 0 || b_strcmp(name, "padding-right") == 0) {
        int px = css_len_px(val, 16); if (px >= 0) p->pad_h = px;
    } else if (b_strcmp(name, "border") == 0) {
        if (b_strcasecmp(val, "none") != 0 && b_strcmp(val, "0") != 0) {
            int32_t c = css_color_in(val);
            p->border = c >= 0 ? c : 0x000000; /* solid, color or black */
        }
    } else if (b_strcmp(name, "border-color") == 0) {
        int32_t c = css_color(val);
        if (c >= 0 && p->border < 0) p->border = c;
        else if (c >= 0) p->border = c;
    } else if (b_strcmp(name, "width") == 0) {
        const char *q = val;
        while (*q && *q != '%') q++;
        if (*q == '%') {
            int n = 0; const char *r = val;
            while (*r == ' ') r++;
            while (*r >= '0' && *r <= '9') { n = n * 10 + (*r - '0'); r++; }
            p->width_pct = n; p->width_px = -1;
        } else {
            int px = css_len_px(val, 16);
            if (px >= 0) { p->width_px = px; p->width_pct = -1; }
        }
    } else if (b_strcmp(name, "line-height") == 0) {
        const char *q = val;
        while (*q == ' ') q++;
        if (b_strncasecmp(q, "normal", 6) == 0) { p->lh_pct = 100; }
        else {
            int whole = 0, frac = 0, any = 0;
            while (*q >= '0' && *q <= '9') { whole = whole * 10 + (*q - '0'); q++; any = 1; }
            if (*q == '.') {
                q++;
                int d = 10;
                while (*q >= '0' && *q <= '9') { frac += (*q - '0') * d; d *= 10; q++; }
            }
            if (any) {
                if (*q == '%') p->lh_pct = whole;
                else if (*q == 'p') p->lh_pct = (whole * 100) / 16; /* px */
                else p->lh_pct = whole * 100 + frac / 10;
            }
        }
    } else if (b_strcmp(name, "list-style-type") == 0 ||
               b_strcmp(name, "list-style") == 0) {
        p->list_none = (b_strcasecmp(val, "none") == 0) ? 1 : 0;
    }
}

/* Parse a declaration block ("a: b; c: d") merging into p. */
static void css_parse_decls(const char *text, size_t len, struct cprops *p) {
    size_t i = 0;
    while (i < len) {
        while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                           text[i] == '\r' || text[i] == ';')) i++;
        if (i >= len) break;
        char name[32]; int ni = 0;
        while (i < len && text[i] != ':' && text[i] != ';') {
            if (ni < 31) name[ni++] = b_lower(text[i]);
            i++;
        }
        name[ni] = 0;
        /* trim trailing spaces of name */
        while (ni > 0 && name[ni - 1] == ' ') name[--ni] = 0;
        if (i >= len || text[i] == ';') continue;
        i++; /* ':' */
        char val[160]; int vi = 0;
        while (i < len && text[i] != ';') {
            if (vi < 158) val[vi++] = text[i];
            i++;
        }
        val[vi] = 0;
        /* trim */
        int st = 0;
        while (val[st] == ' ' || val[st] == '\t') st++;
        int en = vi;
        while (en > st && (val[en - 1] == ' ' || val[en - 1] == '\t' ||
                           val[en - 1] == '\n' || val[en - 1] == '\r')) en--;
        /* strip !important */
        if (en - st > 10 && b_strncasecmp(val + en - 10, "!important", 10) == 0) {
            en -= 10;
            while (en > st && val[en - 1] == ' ') en--;
        }
        val[en] = 0;
        if (ni > 0) css_apply_decl(p, name, val + st);
    }
}

#define MAX_RULES 128
#define RULE_TAG 0
#define RULE_CLASS 1
#define RULE_ID 2
struct crule { uint8_t kind; char sel[30]; struct cprops p; };
static struct crule css_rules[MAX_RULES];
static int ncss_rules = 0;

#define CSS_TEXT_SIZE 32768
static char css_text[CSS_TEXT_SIZE];
static size_t css_text_len = 0;

static void css_text_add(const char *s, size_t n) {
    if (css_text_len + n > CSS_TEXT_SIZE - 1)
        n = CSS_TEXT_SIZE - 1 - css_text_len;
    for (size_t i = 0; i < n; i++) css_text[css_text_len++] = s[i];
    css_text[css_text_len] = 0;
}

/* Compile css_text into css_rules (comments stripped). */
static void css_compile(void) {
    ncss_rules = 0;
    size_t i = 0, n = css_text_len;
    while (i < n) {
        /* skip whitespace and comments */
        if (css_text[i] == '/' && i + 1 < n && css_text[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(css_text[i] == '*' && css_text[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (css_text[i] == ' ' || css_text[i] == '\n' || css_text[i] == '\r' ||
            css_text[i] == '\t') { i++; continue; }
        if (css_text[i] == '@') { /* at-rule: skip to ';' or balanced block */
            while (i < n && css_text[i] != ';' && css_text[i] != '{') i++;
            if (i < n && css_text[i] == '{') {
                int depth = 0;
                while (i < n) {
                    if (css_text[i] == '{') depth++;
                    else if (css_text[i] == '}') { depth--; if (!depth) { i++; break; } }
                    i++;
                }
            } else i++;
            continue;
        }
        /* selector text up to '{' */
        size_t s0 = i;
        while (i < n && css_text[i] != '{' && css_text[i] != '}') i++;
        if (i >= n) break;
        if (css_text[i] == '}') { i++; continue; }
        size_t s1 = i;
        i++; /* '{' */
        size_t d0 = i;
        int depth = 1;
        while (i < n && depth > 0) {
            if (css_text[i] == '{') depth++;
            else if (css_text[i] == '}') depth--;
            if (depth > 0) i++;
        }
        size_t d1 = i;
        if (i < n) i++; /* '}' */
        /* one rule per comma-separated selector */
        size_t p = s0;
        while (p < s1) {
            size_t q = p;
            while (q < s1 && css_text[q] != ',') q++;
            /* trim + reduce to last compound, strip pseudo. The
             * trailing trim must happen FIRST: a space before '{'
             * ("body { ... }") is not a combinator, but the scan
             * below would treat it as one and empty the selector. */
            size_t qe = q;
            while (qe > p && (css_text[qe - 1] == ' ' || css_text[qe - 1] == '\t' ||
                              css_text[qe - 1] == '\n' || css_text[qe - 1] == '\r'))
                qe--;
            char sel[64]; int si = 0;
            size_t last = p;
            for (size_t k = p; k < qe; k++)
                if (css_text[k] == ' ' || css_text[k] == '>' ||
                    css_text[k] == '+' || css_text[k] == '~') last = k + 1;
            for (size_t k = last; k < qe && si < 63; k++) {
                char c = css_text[k];
                if (c == ' ' || c == '\t') continue;
                if (c == ':') break; /* pseudo */
                sel[si++] = c;
            }
            sel[si] = 0;
            if (si > 0 && ncss_rules < MAX_RULES) {
                struct crule *r = &css_rules[ncss_rules];
                props_clear(&r->p);
                css_parse_decls(css_text + d0, d1 - d0, &r->p);
                if (sel[0] == '.') {
                    r->kind = RULE_CLASS;
                    b_strncpy(r->sel, sel + 1, sizeof(r->sel));
                    ncss_rules++;
                } else if (sel[0] == '#') {
                    r->kind = RULE_ID;
                    b_strncpy(r->sel, sel + 1, sizeof(r->sel));
                    ncss_rules++;
                } else if (sel[0] == '*') {
                    /* universal selector: dropped */
                } else {
                    /* tag or tag.cls: a dot means class wins */
                    char *dot = NULL;
                    for (int k = 0; sel[k]; k++) if (sel[k] == '.') dot = sel + k;
                    if (dot) {
                        r->kind = RULE_CLASS;
                        b_strncpy(r->sel, dot + 1, sizeof(r->sel));
                    } else {
                        r->kind = RULE_TAG;
                        for (int k = 0; sel[k]; k++) sel[k] = b_lower(sel[k]);
                        b_strncpy(r->sel, sel, sizeof(r->sel));
                    }
                    ncss_rules++;
                }
            }
            p = q + 1;
        }
    }
}

static int class_has(const char *cls, const char *name) {
    /* cls: space-separated class list (may be "") */
    size_t nl = b_strlen(name);
    const char *p = cls;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t') e++;
        if ((size_t)(e - p) == nl && b_strncmp(p, name, nl) == 0) return 1;
        p = e;
    }
    return 0;
}

/* Compute the cascaded props for one element. `ua` carries the
 * built-in defaults for the tag (they beat inheritance but lose
 * to author rules). */
static void css_compute(const char *tag, const char *cls, const char *id,
                        const char *inline_style,
                        const struct cprops *parent, const struct cprops *ua,
                        struct cprops *out) {
    props_clear(out);
    if (parent) { /* inherited properties only */
        out->color = parent->color;
        out->font_px = parent->font_px;
        out->bold = parent->bold;
        out->italic = parent->italic;
        out->lh_pct = parent->lh_pct;
        out->align = parent->align;
    }
    if (ua) props_merge(out, ua);
    for (int i = 0; i < ncss_rules; i++)
        if (css_rules[i].kind == RULE_TAG &&
            b_strcmp(css_rules[i].sel, tag) == 0)
            props_merge(out, &css_rules[i].p);
    if (cls && cls[0])
        for (int i = 0; i < ncss_rules; i++)
            if (css_rules[i].kind == RULE_CLASS &&
                class_has(cls, css_rules[i].sel))
                props_merge(out, &css_rules[i].p);
    if (id && id[0])
        for (int i = 0; i < ncss_rules; i++)
            if (css_rules[i].kind == RULE_ID &&
                b_strcmp(css_rules[i].sel, id) == 0)
                props_merge(out, &css_rules[i].p);
    if (inline_style && inline_style[0])
        css_parse_decls(inline_style, b_strlen(inline_style), out);
}

/* font-size px -> bitmap scale bucket */
static int font_scale(int px) {
    if (px <= 0) px = 16;
    if (px >= 29) return 3;
    if (px >= 19) return 2;
    return 1;
}

/* ================================================================
 * Page assets (v1.0): stylesheets + images prepared before layout
 * ================================================================ */
struct membuf { uint8_t *buf; size_t len, cap; int overflow; };
static int membuf_sink(const void *data, size_t len, void *ctx) {
    struct membuf *m = (struct membuf *)ctx;
    if (m->len + len > m->cap) { m->overflow = 1; return 0; }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return 1;
}

/* find attr `name` in a raw tag body (text between '<' and '>') */
static int scan_attr(const char *s, const char *end, const char *name,
                     char *out, size_t outsz) {
    size_t nl = b_strlen(name);
    for (const char *p = s; p < end; p++) {
        if ((p == s || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' ||
             p[-1] == '\r' || p[-1] == '/') &&
            b_strncasecmp(p, name, nl) == 0 && p + nl <= end &&
            (p[nl] == '=' || p[nl] == ' ' || p[nl] == '\t')) {
            const char *q = p + nl;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q >= end || *q != '=') { /* bare attribute */
                if (outsz) out[0] = 0;
                return 1;
            }
            q++;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            size_t o = 0;
            if (q < end && (*q == '"' || *q == '\'')) {
                char qq = *q++;
                while (q < end && *q != qq) {
                    if (o < outsz - 1) out[o++] = *q;
                    q++;
                }
            } else {
                while (q < end && *q != ' ' && *q != '\t' && *q != '\n' &&
                       *q != '\r') {
                    if (o < outsz - 1) out[o++] = *q;
                    q++;
                }
            }
            if (outsz) out[o] = 0;
            return 1;
        }
    }
    return 0;
}

#define IMG_ENC_MAX (1024 * 1024)     /* skip encoded images > 1 MB */
#define IMG_PIX_MAX (2200 * 1000)     /* skip decodes > ~2.2 Mpx */
#define IMG_TAB_BYTES (6 * 1024 * 1024)

static void tab_free_images(struct tab *t) {
    for (int i = 0; i < t->nimgs; i++) {
        if (t->imgs[i].px) { kfree(t->imgs[i].px); t->imgs[i].px = NULL; }
    }
    t->nimgs = 0;
    t->img_bytes = 0;
}

static struct img *tab_find_img(struct tab *t, const char *url) {
    for (int i = 0; i < t->nimgs; i++)
        if (b_strcmp(t->imgs[i].url, url) == 0) return &t->imgs[i];
    return NULL;
}

/* Fetch + decode one image into the tab cache (entry is created
 * either way; ok=0 means "show alt text"). */
static void img_fetch_decode(struct tab *t, const char *url) {
    if (t->nimgs >= MAX_IMGS) return;
    struct img *im = &t->imgs[t->nimgs];
    b_strncpy(im->url, url, sizeof(im->url));
    im->px = NULL; im->nw = 0; im->nh = 0; im->ok = 0;
    t->nimgs++;
    uint8_t *enc = kmalloc(IMG_ENC_MAX);
    if (!enc) return;
    struct membuf m;
    m.buf = enc; m.len = 0; m.cap = IMG_ENC_MAX; m.overflow = 0;
    int rc = net_download_stopok(url, membuf_sink, &m, NULL);
    if ((rc != 1 && rc != 2) || m.overflow || m.len == 0) {
        kfree(enc);
        return;
    }
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(enc, (int)m.len, &w, &h, &comp) ||
        w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
        (int64_t)w * h > IMG_PIX_MAX) {
        kfree(enc);
        return;
    }
    uint8_t *px = stbi_load_from_memory(enc, (int)m.len, &w, &h, &comp, 4);
    kfree(enc);
    if (!px) return;
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4u;
    if (t->img_bytes + bytes > IMG_TAB_BYTES) {
        stbi_image_free(px);
        return;
    }
    im->px = px; im->nw = w; im->nh = h; im->ok = 1;
    t->img_bytes += bytes;
}

static const char *find_close_tag(const char *s, const char *tag); /* below */

/* Scan the page source: gather CSS (style blocks fetched sheets
 * appended to css_text) and pre-fetch/decode images (<img src>).
 * Images already in the tab cache (same URL) are reused, so a
 * resize reparse costs no network. */
static void prepare_assets(struct tab *t, const char *html) {
    css_text_len = 0;
    css_text[0] = 0;
    static char srcs[MAX_IMGS][256];
    int nsrc = 0;
    const char *s = html;
    while (*s) {
        if (*s != '<') { s++; continue; }
        if (s[1] == '!' && s[2] == '-' && s[3] == '-') {
            const char *e = s + 4;
            while (*e && !(e[0] == '-' && e[1] == '-' && e[2] == '>')) e++;
            s = *e ? e + 3 : e;
            continue;
        }
        const char *gt = s;
        while (*gt && *gt != '>') gt++;
        char tag[16];
        int ti = 0;
        const char *p = s + 1;
        if (*p == '/') p++;
        while (p < gt && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                          (*p >= '0' && *p <= '9')) && ti < 15)
            tag[ti++] = b_lower(*p++);
        tag[ti] = 0;
        int closing = (s[1] == '/');
        if (!closing && b_strcmp(tag, "style") == 0) {
            const char *close = find_close_tag(gt, "style");
            const char *end = close ? close : gt + b_strlen(gt);
            if (*gt) css_text_add(gt + 1, (size_t)(end - (gt + 1)));
            css_text_add("\n", 1);
            s = end;
            continue;
        }
        if (!closing && b_strcmp(tag, "script") == 0) {
            const char *close = find_close_tag(gt, "script");
            s = close ? close : gt + b_strlen(gt);
            continue;
        }
        if (!closing && b_strcmp(tag, "link") == 0) {
            char rel[64], href[256];
            if (scan_attr(s, gt, "rel", rel, sizeof(rel)) &&
                scan_attr(s, gt, "href", href, sizeof(href))) {
                /* rel must contain the token "stylesheet" */
                int is_sheet = 0;
                for (const char *r = rel; *r; r++)
                    if (b_strncasecmp(r, "stylesheet", 10) == 0) is_sheet = 1;
                if (is_sheet && href[0]) {
                    static char absurl[URL_LEN];
                    resolve_url(t->url, href, absurl, sizeof(absurl));
                    uint8_t *buf = kmalloc(32768);
                    if (buf) {
                        struct membuf m;
                        m.buf = buf; m.len = 0; m.cap = 32768; m.overflow = 0;
                        int rc = net_download_stopok(absurl, membuf_sink, &m, NULL);
                        if (rc == 1 || rc == 2) {
                            css_text_add((const char *)buf, m.len);
                            css_text_add("\n", 1);
                        }
                        kfree(buf);
                    }
                }
            }
        }
        if (!closing && b_strcmp(tag, "img") == 0 && nsrc < MAX_IMGS) {
            char src[256];
            if (scan_attr(s, gt, "src", src, sizeof(src)) && src[0]) {
                int dup = 0;
                for (int i = 0; i < nsrc; i++)
                    if (b_strcmp(srcs[i], src) == 0) dup = 1;
                if (!dup) b_strncpy(srcs[nsrc++], src, sizeof(srcs[0]));
            }
        }
        s = *gt ? gt + 1 : gt;
    }
    /* fetch + decode the images that are not cached yet */
    for (int i = 0; i < nsrc; i++) {
        static char absurl[URL_LEN];
        resolve_url(t->url, srcs[i], absurl, sizeof(absurl));
        if (!tab_find_img(t, absurl))
            img_fetch_decode(t, absurl);
    }
    css_compile();
}
static struct tab *lt = NULL;
static int max_cols = 37;

/* layout builder state (v1.0: pixel-based flow) */
static char lb[LINE_TEXT];
static int lb_len = 0;
static struct dspan lb_spans[LINE_SPANS];
static int lb_nspans = 0;
static int pending_gap = 0;    /* px of space above the next line */
static int pen_x = 0;          /* px cursor (page content coords) */
static int lb_indent = 0;      /* content-box left of the open line */
static int lb_avail = 0;       /* content-box width of the open line */
static int lb_h = 0;           /* content height of the open line */
static int lb_widget = 0;      /* a widget is placed on this line */
static int lb_fresh = 1;       /* line context not yet captured */
static int lb_wkind = W_NONE, lb_widx = -1;
static int lb_wx = 0, lb_ww = 0, lb_wh = 0;
/* current inline style (synced from the element stack) */
static uint32_t cur_color = RGB_TEXT;
static int cur_scale = 1;
static int cur_style = 0;
static int cur_link = -1;
static uint32_t cur_link_rgb = RGB_LINK;
static int cur_lh = 100;       /* line-height % */
static int cur_align = 0;
/* innermost block's box (page content coords) */
static int cur_indent = 0;     /* content-box left */
static int cur_width = 0;      /* content-box width */
static int cur_box_x = 0;      /* border-box left (content - padding) */
static int cur_box_w = 0;      /* border-box width */
static int32_t cur_blk_bg = -1;
static int in_li = 0;
static int li_marker = 1;      /* draw the "- " marker for this li */
static int page_w = 400;       /* content width in px for this parse */

/* element stack: computed props + layout state per open element */
#define MAX_DEPTH 40
struct estack {
    char tag[16];
    struct cprops p;
    int is_block;
    int indent, width, box_x, box_w, first_line;
    int gap_after;     /* default space after the block */
    int elem;          /* id-registry index or -1 */
    int form_idx;      /* <form>: its form index, else -1 */
    int sel_idx;       /* <select>: its ctrl index, else -1 */
    int saved_link;
    uint32_t saved_link_rgb;
    int list_none;     /* for ul/ol: suppress li markers */
};
static struct estack estack[MAX_DEPTH];
static int esp = 0;

/* form state during parse */
static int cur_form = -1;      /* open <form> index or -1 */
static int cur_select = -1;    /* ctrl index of open <select> */
static char opt_buf[64];       /* current <option> label text */
static int opt_len = 0;
static char opt_val[48];       /* current <option> value attr */
static int opt_has_val = 0, opt_selected = 0;

static int text_h(void) {      /* height of one text line at cur style */
    int h = (16 * cur_scale * cur_lh) / 100 + 2;
    return h < 18 * cur_scale ? 18 * cur_scale : h;
}

/* <script> capture (per page load, document order) */
#define MAX_SCRIPTS 8
#define SCRIPT_SIZE 4096
static char scripts[MAX_SCRIPTS][SCRIPT_SIZE];
static int nscripts = 0;

/* id-element registry (per page load) for the DOM subset */
#define MAX_ELEMS 32
struct elem {
    char id[32];
    char tag[12];
    int start;   /* first display line, -1 = no rendered content yet */
    int end;     /* last display line */
};
static struct elem elems[MAX_ELEMS];
static int nelems = 0;
static int elem_stack[MAX_ELEMS];
static int elem_sp = 0;

/* Attribute every newly appended display line to the open elements. */
static void note_new_line(int idx) {
    for (int i = 0; i < elem_sp; i++) {
        struct elem *e = &elems[elem_stack[i]];
        if (e->start < 0) e->start = idx;
        e->end = idx;
    }
}

static void line_reset(void) {
    lb_len = 0; lb_nspans = 0; lb[0] = 0;
    lb_h = 0;
    lb_widget = 0; lb_wkind = W_NONE; lb_widx = -1;
    lb_wx = lb_ww = lb_wh = 0;
    lb_fresh = 1;
    pen_x = cur_indent;
}
static void line_begin(void) {
    if (!lb_fresh) return;
    lb_fresh = 0;
    lb_indent = cur_indent;
    lb_avail = cur_width;
    pen_x = cur_indent;
}

static int next_bg_off = 0; /* margin part of the next line's gap */

static void flush_line(int force) {
    if (lb_fresh && lb_len == 0 && !lb_widget && !force) return;
    if (lt->nlines >= MAX_LINES) { line_reset(); pending_gap = 0; return; }
    struct dline *L = &lt->lines[lt->nlines];
    int used_end = pen_x;
    int shift = 0;
    if (!lb_fresh && cur_align > 0 && lb_len > 0) {
        int used = used_end - lb_indent;
        if (used < 0) used = 0;
        if (used < lb_avail)
            shift = cur_align == 1 ? (lb_avail - used) / 2 : lb_avail - used;
    }
    b_strcpy(L->text, lb);
    L->nspans = (uint8_t)lb_nspans;
    for (int i = 0; i < lb_nspans; i++) {
        L->spans[i] = lb_spans[i];
        L->spans[i].col = (uint16_t)(L->spans[i].col + shift);
    }
    int content_h = lb_h;
    if (content_h == 0) content_h = text_h(); /* forced empty line */
    L->height = (uint16_t)(pending_gap + content_h);
    L->ch = (uint16_t)content_h;
    L->rule = 0;
    L->bg = cur_blk_bg;
    L->bg_x = (uint16_t)cur_box_x;
    L->bg_w = (uint16_t)cur_box_w;
    L->bg_off = (uint16_t)next_bg_off;
    L->border = 0;
    L->border_rgb = 0;
    L->wkind = (uint8_t)lb_wkind;
    L->widx = (int16_t)lb_widx;
    L->wx = (uint16_t)(lb_wx + shift);
    L->ww = (uint16_t)lb_ww;
    L->wh = (uint16_t)lb_wh;
    note_new_line(lt->nlines);
    lt->nlines++;
    pending_gap = 0;
    next_bg_off = 0;
    line_reset();
}
static void gap_before(int px) {
    flush_line(0);
    if (px > pending_gap) pending_gap = px;
}

/* Record count chars starting at lb index `start` as span(s) with
 * the current style (merging with the previous span when possible). */
static uint16_t lb_px[LINE_TEXT];
static void span_add(int start, int count) {
    while (count > 0) {
        if (lb_nspans > 0) {
            struct dspan *sp = &lb_spans[lb_nspans - 1];
            if (sp->color == cur_color && sp->scale == (uint8_t)cur_scale &&
                sp->style == (uint8_t)cur_style &&
                sp->link == (int16_t)cur_link && sp->len < 255 &&
                (int)sp->col + (int)sp->len * 8 * cur_scale == lb_px[start]) {
                int add = count;
                if ((int)sp->len + add > 255) add = 255 - sp->len;
                sp->len = (uint8_t)(sp->len + add);
                count -= add;
                start += add;
                continue;
            }
        }
        if (lb_nspans >= LINE_SPANS) return; /* text kept, style dropped */
        struct dspan *sp = &lb_spans[lb_nspans++];
        int add = count > 255 ? 255 : count;
        sp->col = lb_px[start];
        sp->coff = (uint16_t)start;
        sp->len = (uint8_t)add;
        sp->scale = (uint8_t)cur_scale;
        sp->style = (uint8_t)cur_style;
        sp->color = cur_color;
        sp->link = (int16_t)cur_link;
        count -= add;
        start += add;
    }
}
static void emit_char(char c) {
    line_begin();
    if (lb_len >= LINE_TEXT - 1) return;
    int idx = lb_len;
    lb[idx] = c;
    lb_px[idx] = (uint16_t)pen_x;
    lb_len++;
    lb[lb_len] = 0;
    pen_x += 8 * cur_scale;
    if (lb_h < text_h()) lb_h = text_h();
    span_add(idx, 1);
}
static void emit_str(const char *s, int len) {
    for (int i = 0; i < len; i++) emit_char(s[i]);
}
static void add_word(const char *w, int len) {
    if (len <= 0) return;
    line_begin();
    if (lb_len == 0 && pen_x == lb_indent && in_li && li_marker) {
        int ss = cur_scale, st = cur_style, sl = cur_link;
        uint32_t sc = cur_color;
        cur_scale = 1; cur_style = 0; cur_color = RGB_TEXT; cur_link = -1;
        emit_str("  - ", 4);
        cur_scale = ss; cur_style = st; cur_color = sc; cur_link = sl;
    }
    int cw = 8 * cur_scale;
    if (lb_len > 0 || pen_x > lb_indent) {
        if (pen_x + cw + len * cw > lb_indent + lb_avail) {
            flush_line(0);
            line_begin();
        } else {
            emit_char(' ');
        }
    }
    int maxlen = lb_avail / cw;
    if (len > maxlen) len = maxlen; /* overlong word: truncate */
    for (int i = 0; i < len; i++) emit_char(w[i]);
}

/* Place a widget (image or form control) in the flow. */
static void place_widget(int kind, int idx, int w, int h) {
    line_begin();
    if (w > lb_avail) w = lb_avail;
    if (lb_widget || (pen_x + w > lb_indent + lb_avail &&
                      (lb_len > 0 || pen_x > lb_indent))) {
        flush_line(0);
        line_begin();
    }
    lb_widget = 1;
    lb_wkind = kind;
    lb_widx = idx;
    lb_wx = pen_x;
    lb_ww = w;
    lb_wh = h;
    pen_x += w + 6;
    if (lb_h < h) lb_h = h;
}

/* ---- element stack / block layout ---- */
static void sync_inline(void) {
    struct cprops *p = esp > 0 ? &estack[esp - 1].p : NULL;
    int32_t c = p ? p->color : -1;
    cur_color = cur_link >= 0 ? cur_link_rgb : (c >= 0 ? (uint32_t)c : RGB_TEXT);
    cur_scale = font_scale(p ? p->font_px : 0);
    cur_style = 0;
    if (p) {
        if (p->bold == 1) cur_style |= ST_BOLD;
        if (p->italic == 1) cur_style |= ST_ITALIC;
        if (p->under == 1) cur_style |= ST_UNDER;
    }
    if (cur_link >= 0) cur_style |= ST_UNDER;
    cur_lh = (p && p->lh_pct > 0) ? p->lh_pct : 100;
    cur_align = (p && p->align >= 0) ? p->align : 0;
}
static void restore_block_ctx(void) {
    for (int i = esp - 1; i >= 0; i--) {
        if (estack[i].is_block) {
            cur_indent = estack[i].indent;
            cur_width = estack[i].width;
            cur_box_x = estack[i].box_x;
            cur_box_w = estack[i].box_w;
            cur_blk_bg = estack[i].p.bg;
            return;
        }
    }
    cur_indent = 0; cur_width = page_w;
    cur_box_x = 0; cur_box_w = page_w;
    cur_blk_bg = -1;
}
static void block_start(struct estack *e, int def_gap) {
    flush_line(0);
    struct cprops *p = &e->p;
    int mh = p->margin_h >= 0 ? p->margin_h : 0;
    if (b_strcmp(e->tag, "blockquote") == 0 && p->margin_h < 0) mh = 24;
    int ph = p->pad_h >= 0 ? p->pad_h : 0;
    int pv = p->pad_v >= 0 ? p->pad_v : 0;
    int mv = p->margin_v >= 0 ? p->margin_v : def_gap;
    e->box_x = cur_indent + mh;
    e->indent = e->box_x + ph;
    int w = cur_width - 2 * mh - 2 * ph;
    if (p->width_px >= 0) w = p->width_px;
    else if (p->width_pct >= 0) w = cur_width * p->width_pct / 100;
    if (w < 40) w = 40;
    e->width = w;
    e->box_w = w + 2 * ph;
    e->first_line = lt->nlines;
    if (mv + pv > pending_gap) { pending_gap = mv + pv; next_bg_off = mv; }
    cur_indent = e->indent;
    cur_width = e->width;
    cur_box_x = e->box_x;
    cur_box_w = e->box_w;
    cur_blk_bg = p->bg;
    line_reset();
}
static void block_end(struct estack *e) {
    flush_line(0);
    struct cprops *p = &e->p;
    if (p->border >= 0) {
        for (int i = e->first_line; i < lt->nlines; i++) {
            struct dline *L = &lt->lines[i];
            if (L->border == 0) {
                L->border = 4 | 8;
                if (i == e->first_line) L->border |= 1;
                if (i == lt->nlines - 1) L->border |= 2;
                L->border_rgb = (uint32_t)p->border;
                L->bg_x = (uint16_t)e->box_x;
                L->bg_w = (uint16_t)e->box_w;
            }
        }
    }
    int pv = p->pad_v >= 0 ? p->pad_v : 0;
    int mv = p->margin_v >= 0 ? p->margin_v : e->gap_after;
    if (mv + pv > pending_gap) { pending_gap = mv + pv; next_bg_off = 0; }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int tag_is(const char *a, const char *b) { return b_strcmp(a, b) == 0; }
static int atoi_(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static int is_block_tag(const char *tag) {
    static const char *const blocks[] = {
        "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "ul",
        "ol", "table", "tr", "section", "article", "blockquote", "pre",
        "form", "fieldset", "header", "footer", "nav", "main", "aside",
        "figure", "figcaption", "dl", "dt", "dd", "address", "body"
    };
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
        if (b_strcmp(tag, blocks[i]) == 0) return 1;
    return 0;
}
static int is_void_tag(const char *tag) {
    static const char *const voids[] = {
        "br", "hr", "img", "input", "link", "meta", "area", "base",
        "col", "embed", "source", "track", "wbr"
    };
    for (size_t i = 0; i < sizeof(voids) / sizeof(voids[0]); i++)
        if (b_strcmp(tag, voids[i]) == 0) return 1;
    return 0;
}
static int def_gap_before(const char *tag) {
    if (tag_is(tag, "p") || tag_is(tag, "div") || tag_is(tag, "form") ||
        tag_is(tag, "blockquote") || tag_is(tag, "pre") ||
        tag_is(tag, "fieldset")) return 6;
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == 0) return 8;
    if (tag_is(tag, "ul") || tag_is(tag, "ol") || tag_is(tag, "table") ||
        tag_is(tag, "section") || tag_is(tag, "article")) return 4;
    return 0;
}
static int def_gap_after(const char *tag) {
    if (tag_is(tag, "p") || tag_is(tag, "div") || tag_is(tag, "form") ||
        tag_is(tag, "blockquote") || tag_is(tag, "pre") ||
        tag_is(tag, "fieldset")) return 2;
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == 0) return 4;
    if (tag_is(tag, "ul") || tag_is(tag, "ol")) return 4;
    if (tag_is(tag, "table") || tag_is(tag, "section") ||
        tag_is(tag, "article")) return 2;
    return 0;
}

/* Find "</tag" (case-insensitive) at or after s; NULL if absent. */
static const char *find_close_tag(const char *s, const char *tag) {
    size_t tl = b_strlen(tag);
    for (const char *p = s; *p; p++) {
        if (p[0] == '<' && p[1] == '/') {
            size_t i = 0;
            while (i < tl && p[2 + i] && b_lower(p[2 + i]) == tag[i]) i++;
            if (i == tl) return p;
        }
    }
    return NULL;
}

static void refresh_li(void) {
    in_li = 0;
    li_marker = 1;
    for (int i = esp - 1; i >= 0; i--) {
        if (b_strcmp(estack[i].tag, "li") == 0) {
            in_li = 1;
            for (int j = i - 1; j >= 0; j--) {
                if (b_strcmp(estack[j].tag, "ul") == 0 ||
                    b_strcmp(estack[j].tag, "ol") == 0) {
                    li_marker = estack[j].list_none ? 0 : 1;
                    break;
                }
            }
            break;
        }
    }
}

static struct estack *push_elem(const char *tag, const struct cprops *p,
                                int is_block, const char *idattr) {
    if (esp >= MAX_DEPTH) return NULL;
    struct estack *e = &estack[esp++];
    b_strncpy(e->tag, tag, sizeof(e->tag));
    e->p = *p;
    e->is_block = is_block;
    e->elem = -1;
    e->form_idx = -1;
    e->sel_idx = -1;
    e->saved_link = cur_link;
    e->saved_link_rgb = cur_link_rgb;
    e->list_none = (p->list_none == 1);
    e->indent = cur_indent; e->width = cur_width;
    e->box_x = cur_box_x; e->box_w = cur_box_w;
    e->first_line = lt->nlines;
    e->gap_after = def_gap_after(tag);
    if (is_block) block_start(e, def_gap_before(tag));
    if (idattr && idattr[0] && nelems < MAX_ELEMS) {
        struct elem *el = &elems[nelems];
        b_strncpy(el->id, idattr, sizeof(el->id));
        b_strncpy(el->tag, tag, sizeof(el->tag));
        el->start = -1;
        el->end = -1;
        e->elem = nelems;
        if (elem_sp < MAX_ELEMS) elem_stack[elem_sp++] = nelems;
        nelems++;
    }
    refresh_li();
    sync_inline();
    return e;
}
static void pop_one(void) {
    struct estack *e = &estack[esp - 1];
    if (e->is_block) {
        /* a block element with an id but no content still gets one
         * (empty) line as its slot, so textContent can target it */
        if (e->elem >= 0 && elems[e->elem].start < 0) flush_line(1);
        block_end(e);
    }
    if (e->elem >= 0) {
        for (int i = 0; i < elem_sp; i++) {
            if (elem_stack[i] == e->elem) {
                for (int j = i; j < elem_sp - 1; j++)
                    elem_stack[j] = elem_stack[j + 1];
                elem_sp--;
                break;
            }
        }
    }
    if (e->form_idx >= 0 && cur_form == e->form_idx) cur_form = -1;
    if (e->sel_idx >= 0 && cur_select == e->sel_idx) cur_select = -1;
    cur_link = e->saved_link;
    cur_link_rgb = e->saved_link_rgb;
    esp--;
    restore_block_ctx();
    refresh_li();
    sync_inline();
}
static void pop_through_tag(const char *tag) {
    for (int d = esp - 1; d >= 0; d--) {
        if (b_strcmp(estack[d].tag, tag) == 0) {
            while (esp > d) pop_one();
            return;
        }
    }
}

/* ---- form controls (creation during parse) ---- */
static struct ctrl *new_ctrl(int type) {
    if (lt->nctrls >= MAX_CTRLS) return NULL;
    struct ctrl *c = &lt->ctrls[lt->nctrls];
    memset(c, 0, sizeof(*c));
    c->type = (uint8_t)type;
    c->form = cur_form;
    c->line = -1;
    lt->nctrls++;
    if (cur_form >= 0 && cur_form < lt->nforms) lt->forms[cur_form].count++;
    return c;
}
static void commit_option(void) {
    if (cur_select < 0 || cur_select >= lt->nctrls) return;
    struct ctrl *c = &lt->ctrls[cur_select];
    while (opt_len > 0 && opt_buf[opt_len - 1] == ' ') opt_buf[--opt_len] = 0;
    int st = 0;
    while (opt_buf[st] == ' ') st++;
    if (c->nopts < 6 && opt_buf[st]) {
        b_strncpy(c->opts[c->nopts], opt_buf + st, sizeof(c->opts[0]));
        b_strncpy(c->ovals[c->nopts],
                  opt_has_val ? opt_val : opt_buf + st, sizeof(c->ovals[0]));
        if (opt_selected) c->sel = c->nopts;
        c->nopts++;
    }
    opt_len = 0;
    opt_buf[0] = 0;
    opt_has_val = 0;
    opt_selected = 0;
    opt_val[0] = 0;
}

/* Decode HTML entities from src..end into dst (NUL-terminated). */
static void decode_entities_into(char *dst, size_t dstsz,
                                 const char *src, const char *end) {
    size_t o = 0;
    for (const char *p = src; p < end && o < dstsz - 1; p++) {
        if (*p == '&') {
            const char *e = p + 1;
            char name[12];
            int ni = 0;
            while (e < end && *e != ';' && ni < 11 &&
                   ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                    (*e >= '0' && *e <= '9') || *e == '#'))
                name[ni++] = *e++;
            name[ni] = 0;
            if (e < end && *e == ';' && ni > 0) {
                char rep = 0;
                int have = 0;
                if (name[0] == '#') {
                    int v = 0, bad = 0;
                    if (name[1] == 'x' || name[1] == 'X') {
                        for (int i = 2; name[i]; i++) {
                            int hv = hexval(name[i]);
                            if (hv < 0) { bad = 1; break; }
                            v = v * 16 + hv;
                        }
                    } else {
                        for (int i = 1; name[i]; i++) {
                            if (name[i] < '0' || name[i] > '9') { bad = 1; break; }
                            v = v * 10 + (name[i] - '0');
                        }
                    }
                    if (!bad && v > 0 && v < 127) { rep = (char)v; have = 1; }
                }
                else if (b_strcasecmp(name, "amp") == 0) { rep = '&'; have = 1; }
                else if (b_strcasecmp(name, "lt") == 0) { rep = '<'; have = 1; }
                else if (b_strcasecmp(name, "gt") == 0) { rep = '>'; have = 1; }
                else if (b_strcasecmp(name, "quot") == 0) { rep = '"'; have = 1; }
                else if (b_strcasecmp(name, "apos") == 0) { rep = '\''; have = 1; }
                else if (b_strcasecmp(name, "nbsp") == 0) { rep = ' '; have = 1; }
                if (have) { dst[o++] = rep; p = e; continue; }
            }
            dst[o++] = '&';
            continue;
        }
        if (*p == '\r') continue;
        dst[o++] = *p;
    }
    dst[o] = 0;
}

static void handle_img(const struct cprops *cp, const char *src,
                       const char *alt, const char *widtha,
                       const char *heighta) {
    if (!src[0]) return;
    static char absurl[URL_LEN];
    resolve_url(lt->url, src, absurl, sizeof(absurl));
    struct img *im = tab_find_img(lt, absurl);
    int idx = im ? (int)(im - lt->imgs) : -1;
    int dw = 0, dh = 0;
    if (im && im->ok) {
        if (cp->width_px >= 0) dw = cp->width_px;
        else if (cp->width_pct >= 0) dw = cur_width * cp->width_pct / 100;
        else if (widtha[0]) dw = atoi_(widtha);
        else dw = im->nw;
        if (dw > cur_width) dw = cur_width;
        if (dw < 8) dw = 0;
        else {
            if (heighta[0]) dh = atoi_(heighta);
            else { /* nh*dw can overflow 32 bits; split the division
                    * (nw*nh <= IMG_PIX_MAX keeps each term in range,
                    * and no 64-bit libcall is available freestanding) */
                int nw_ = im->nw ? im->nw : 1;
                dh = (dw / nw_) * im->nh + ((dw % nw_) * im->nh) / nw_;
            }
            if (dh > 900) dh = 900;
            if (dh < 8) dh = 8;
        }
    }
    if (idx >= 0 && dw > 0) {
        place_widget(W_IMG, idx, dw, dh);
    } else if (alt[0]) {
        char tmp[128];
        b_strncpy(tmp, alt, sizeof(tmp));
        char *p = tmp;
        while (*p) {
            while (*p == ' ') p++;
            char *e = p;
            while (*e && *e != ' ') e++;
            if (e > p) add_word(p, (int)(e - p));
            p = e;
        }
    }
}

static void handle_input(const char *typ, const char *namea,
                         const char *valuea, int checked,
                         const char *sizea) {
    int type = CT_TEXT;
    char tl[24];
    int i = 0;
    while (typ[i] && i < 23) { tl[i] = b_lower(typ[i]); i++; }
    tl[i] = 0;
    if (b_strcmp(tl, "password") == 0) type = CT_PASS;
    else if (b_strcmp(tl, "checkbox") == 0) type = CT_CHECK;
    else if (b_strcmp(tl, "radio") == 0) type = CT_RADIO;
    else if (b_strcmp(tl, "submit") == 0) type = CT_SUBMIT;
    else if (b_strcmp(tl, "button") == 0 || b_strcmp(tl, "reset") == 0)
        type = CT_BUTTON;
    else if (b_strcmp(tl, "hidden") == 0) type = CT_HIDDEN;
    struct ctrl *c = new_ctrl(type);
    if (!c) return;
    b_strncpy(c->name, namea, sizeof(c->name));
    b_strncpy(c->value, valuea, sizeof(c->value));
    if ((type == CT_CHECK || type == CT_RADIO) && c->value[0] == 0)
        b_strcpy(c->value, "on");
    c->checked = checked != 0;
    if (type == CT_RADIO && c->checked && c->name[0]) {
        /* one checked radio per group: earlier ones turn off */
        for (int k = 0; k < lt->nctrls - 1; k++) {
            struct ctrl *o = &lt->ctrls[k];
            if (o->type == CT_RADIO && o->form == c->form &&
                b_strcmp(o->name, c->name) == 0)
                o->checked = 0;
        }
    }
    if (type == CT_HIDDEN) return;
    int idx = (int)(c - lt->ctrls);
    int w = 0, h = 22;
    if (type == CT_TEXT || type == CT_PASS) {
        int chars = sizea[0] ? atoi_(sizea) : 20;
        if (chars < 4) chars = 4;
        if (chars > 60) chars = 60;
        w = chars * 8 + 12;
    } else if (type == CT_CHECK || type == CT_RADIO) {
        w = 16; h = 16;
    } else {
        const char *lab = c->value[0] ? c->value
                                     : (type == CT_SUBMIT ? "Submit" : "Button");
        b_strncpy(c->opts[0], lab, sizeof(c->opts[0]));
        w = (int)b_strlen(lab) * 8 + 18;
    }
    place_widget(W_CTRL, idx, w, h);
    c->line = lt->nlines;
}

static int parse_oom = 0; /* set when a tab's line array failed */

/* Lazily allocate a tab's line array from the heap (kept for
 * the tab's whole lifetime; MAX_TABS bounds the total). */
static int tab_ensure_lines(struct tab *t) {
    if (t->lines) return 1;
    t->lines = kmalloc(sizeof(struct dline) * MAX_LINES);
    if (!t->lines) { parse_oom = 1; return 0; }
    memset(t->lines, 0, sizeof(struct dline) * MAX_LINES);
    return 1;
}

static void parse_html(struct tab *t, const char *html, int append) {
    lt = t;
    page_w = bv_w - 22;
    if (page_w < 80) page_w = 80;
    if (!tab_ensure_lines(t)) {
        if (!append) {
            t->nlines = 0;
            t->nlinks = 0;
            t->nctrls = 0;
            t->nforms = 0;
            t->focus = -1;
            t->content_bg = RGB_FACE;
        }
        return;
    }
    if (!append) {
        t->nlines = 0;
        t->nlinks = 0;
        t->content_bg = RGB_FACE;
        t->title[0] = 0;
        t->nctrls = 0;
        t->nforms = 0;
        t->focus = -1;
        nscripts = 0;
        nelems = 0;
    }
    elem_sp = 0;
    esp = 0;
    cur_indent = 0; cur_width = page_w;
    cur_box_x = 0; cur_box_w = page_w;
    cur_blk_bg = -1;
    cur_link = -1; cur_link_rgb = RGB_LINK;
    cur_form = -1; cur_select = -1;
    opt_len = 0; opt_buf[0] = 0;
    in_li = 0; li_marker = 1;
    pending_gap = append ? 4 : 0;
    next_bg_off = 0;
    line_reset();
    sync_inline();
    int in_head = 0, in_title = 0;
    int hidden = 0; /* depth inside display:none subtrees */
    char title_buf[128];
    int title_len = 0;

    const char *s = html;
    char word[160];
    int wlen = 0;

    #define FLUSH_WORD() do { \
        if (wlen > 0) { \
            if (in_title) { \
                for (int i = 0; i < wlen && title_len < 120; i++) \
                    title_buf[title_len++] = word[i]; \
                if (title_len < 120) title_buf[title_len++] = ' '; \
            } else if (cur_select >= 0) { \
                for (int i = 0; i < wlen && opt_len < 60; i++) \
                    opt_buf[opt_len++] = word[i]; \
                if (opt_len < 62) opt_buf[opt_len++] = ' '; \
                opt_buf[opt_len] = 0; \
            } else if (!in_head && !hidden) add_word(word, wlen); \
            wlen = 0; \
        } \
    } while (0)

    while (*s) {
        char c = *s;
        if (c == '<') {
            if (s[1] == '!' && s[2] == '-' && s[3] == '-') {
                const char *e = s + 4;
                while (*e && !(e[0] == '-' && e[1] == '-' && e[2] == '>')) e++;
                s = *e ? e + 3 : e;
                continue;
            }
            FLUSH_WORD();
            s++; /* consume '<' */
            int closing = 0;
            if (*s == '/') { closing = 1; s++; }
            if (*s == '!' || *s == '?') { /* doctype / PI: skip to '>' */
                while (*s && *s != '>') s++;
                if (*s) s++;
                continue;
            }
            char tag[16];
            int ti = 0;
            while (*s && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                          (*s >= '0' && *s <= '9')) && ti < 15)
                tag[ti++] = b_lower(*s++);
            tag[ti] = 0;
            /* attributes (static: the kernel stack is small and
             * the parser never recurses into itself) */
            static char href[256], src[256], alt[128], bgcolor[32];
            static char idattr[32], cls[128], stylea[256], typ[24];
            static char namea[48], valuea[200], actiona[256];
            static char methoda[12], widtha[16], heighta[16];
            static char rowsa[8], colsa[8], sizea[8];
            href[0] = 0; src[0] = 0; alt[0] = 0; bgcolor[0] = 0;
            idattr[0] = 0; cls[0] = 0; stylea[0] = 0; typ[0] = 0;
            namea[0] = 0; valuea[0] = 0; actiona[0] = 0;
            methoda[0] = 0; widtha[0] = 0; heighta[0] = 0;
            rowsa[0] = 0; colsa[0] = 0; sizea[0] = 0;
            int has_checked = 0, has_selected = 0, has_value = 0;
            int selfclose = 0;
            while (*s && *s != '>') {
                while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
                if (*s == '/') {
                    const char *q = s + 1;
                    while (*q == ' ') q++;
                    if (*q == '>') selfclose = 1;
                    s++;
                    continue;
                }
                if (*s == '>' || *s == 0) break;
                char aname[24]; int ai = 0;
                while (*s && *s != '=' && *s != '>' && *s != ' ' && *s != '\t' &&
                       *s != '/' && ai < 23)
                    aname[ai++] = b_lower(*s++);
                aname[ai] = 0;
                while (*s == ' ') s++;
                char aval[256]; int av = 0;
                int had_value = 0;
                if (*s == '=') {
                    had_value = 1;
                    s++;
                    while (*s == ' ') s++;
                    char q = 0;
                    if (*s == '"' || *s == '\'') { q = *s++; }
                    while (*s && (q ? *s != q : (*s != ' ' && *s != '\t' && *s != '>' &&
                                                 *s != '\r' && *s != '\n'))) {
                        if (av < 255) aval[av++] = *s;
                        s++;
                    }
                    if (q && *s == q) s++;
                }
                aval[av] = 0;
                if (b_strcmp(aname, "href") == 0) b_strcpy(href, aval);
                else if (b_strcmp(aname, "src") == 0) b_strcpy(src, aval);
                else if (b_strcmp(aname, "alt") == 0) b_strncpy(alt, aval, sizeof(alt));
                else if (b_strcmp(aname, "bgcolor") == 0) b_strcpy(bgcolor, aval);
                else if (b_strcmp(aname, "id") == 0) b_strncpy(idattr, aval, sizeof(idattr));
                else if (b_strcmp(aname, "class") == 0) b_strncpy(cls, aval, sizeof(cls));
                else if (b_strcmp(aname, "style") == 0) b_strncpy(stylea, aval, sizeof(stylea));
                else if (b_strcmp(aname, "type") == 0) b_strncpy(typ, aval, sizeof(typ));
                else if (b_strcmp(aname, "name") == 0) b_strncpy(namea, aval, sizeof(namea));
                else if (b_strcmp(aname, "value") == 0) { b_strncpy(valuea, aval, sizeof(valuea)); has_value = had_value; }
                else if (b_strcmp(aname, "action") == 0) b_strcpy(actiona, aval);
                else if (b_strcmp(aname, "method") == 0) b_strncpy(methoda, aval, sizeof(methoda));
                else if (b_strcmp(aname, "width") == 0) b_strncpy(widtha, aval, sizeof(widtha));
                else if (b_strcmp(aname, "height") == 0) b_strncpy(heighta, aval, sizeof(heighta));
                else if (b_strcmp(aname, "rows") == 0) b_strncpy(rowsa, aval, sizeof(rowsa));
                else if (b_strcmp(aname, "cols") == 0) b_strncpy(colsa, aval, sizeof(colsa));
                else if (b_strcmp(aname, "size") == 0) b_strncpy(sizea, aval, sizeof(sizea));
                else if (b_strcmp(aname, "checked") == 0) has_checked = 1;
                else if (b_strcmp(aname, "selected") == 0) has_selected = 1;
            }
            if (*s == '>') s++;

            /* raw-text elements: script/style content is NOT markup */
            if (!closing && (tag_is(tag, "script") || tag_is(tag, "style"))) {
                int want_script = tag_is(tag, "script") && !append;
                const char *close = find_close_tag(s, tag);
                const char *end = close ? close : s + b_strlen(s);
                if (want_script && nscripts < MAX_SCRIPTS) {
                    size_t n = (size_t)(end - s);
                    if (n > SCRIPT_SIZE - 1) n = SCRIPT_SIZE - 1;
                    for (size_t i = 0; i < n; i++) scripts[nscripts][i] = s[i];
                    scripts[nscripts][n] = 0;
                    nscripts++;
                }
                s = end; /* the loop parses the closing tag next */
                continue;
            }
            if (tag_is(tag, "script") || tag_is(tag, "style"))
                continue; /* stray closing tag */

            /* display:none subtrees are skipped entirely */
            if (hidden) {
                if (closing) hidden--;
                else if (!is_void_tag(tag) && !selfclose) hidden++;
                continue;
            }

            if (tag_is(tag, "head")) { in_head = !closing; continue; }
            if (tag_is(tag, "title")) {
                if (!closing) { in_title = 1; title_len = 0; }
                else {
                    in_title = 0;
                    title_buf[title_len] = 0;
                    while (title_len > 0 && title_buf[title_len - 1] == ' ')
                        title_buf[--title_len] = 0;
                    if (t->title[0] == 0 && !append)
                        b_strcpy(t->title, title_buf);
                }
                continue;
            }
            if (in_head) continue;

            /* closing tags */
            if (closing) {
                if (tag_is(tag, "option")) { commit_option(); continue; }
                if (tag_is(tag, "select") && cur_select >= 0) {
                    commit_option();
                    struct ctrl *c = &lt->ctrls[cur_select];
                    int maxl = 4;
                    for (int i = 0; i < c->nopts; i++) {
                        int l = (int)b_strlen(c->opts[i]);
                        if (l > maxl) maxl = l;
                    }
                    int w = maxl * 8 + 26;
                    place_widget(W_CTRL, cur_select, w, 22);
                    c->line = lt->nlines;
                    cur_select = -1;
                }
                if (!is_void_tag(tag)) pop_through_tag(tag);
                continue;
            }

            /* inside <select>, only <option> matters */
            if (cur_select >= 0 && !tag_is(tag, "option") &&
                !tag_is(tag, "optgroup")) {
                continue;
            }
            if (tag_is(tag, "option")) {
                commit_option();
                opt_has_val = has_value;
                if (has_value) b_strncpy(opt_val, valuea, sizeof(opt_val));
                opt_selected = has_selected;
                continue;
            }

            /* computed style for this element */
            struct cprops ua;
            props_clear(&ua);
            if (tag_is(tag, "b") || tag_is(tag, "strong") || tag_is(tag, "th"))
                ua.bold = 1;
            if (tag_is(tag, "i") || tag_is(tag, "em")) ua.italic = 1;
            if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == 0) {
                static const int hs[6] = { 32, 24, 19, 16, 13, 11 };
                ua.font_px = hs[tag[1] - '1'];
                ua.bold = 1;
            }
            struct cprops cp;
            css_compute(tag, cls, idattr, stylea,
                        esp > 0 ? &estack[esp - 1].p : NULL, &ua, &cp);
            if (cp.display == 1 && !is_void_tag(tag)) {
                if (!selfclose) hidden = 1;
                continue;
            }

            if (tag_is(tag, "body")) {
                if (cp.bg >= 0) lt->content_bg = (uint32_t)cp.bg;
                if (bgcolor[0]) {
                    int32_t bc = css_color(bgcolor);
                    if (bc >= 0) lt->content_bg = (uint32_t)bc;
                }
                push_elem(tag, &cp, 1, idattr);
            } else if (tag_is(tag, "form")) {
                int fi = -1;
                if (cur_form < 0 && lt->nforms < MAX_FORMS) {
                    fi = lt->nforms++;
                    struct form *f = &lt->forms[fi];
                    if (actiona[0])
                        resolve_url(lt->url, actiona, f->action, sizeof(f->action));
                    else
                        b_strncpy(f->action, lt->url, sizeof(f->action));
                    f->post = b_strcasecmp(methoda, "post") == 0;
                    f->first = lt->nctrls;
                    f->count = 0;
                    cur_form = fi;
                }
                struct estack *e = push_elem(tag, &cp, 1, idattr);
                if (e) e->form_idx = fi;
            } else if (tag_is(tag, "a")) {
                push_elem(tag, &cp, 0, idattr);
                if (href[0]) {
                    if (lt->nlinks < MAX_LINKS) {
                        resolve_url(lt->url, href, lt->links[lt->nlinks], 256);
                        cur_link = lt->nlinks++;
                    } else {
                        cur_link = -2; /* shown, not clickable */
                    }
                    int32_t pc = esp >= 2 ? estack[esp - 2].p.color : -1;
                    cur_link_rgb = (cp.color >= 0 && cp.color != pc)
                                       ? (uint32_t)cp.color : RGB_LINK;
                    sync_inline();
                }
            } else if (tag_is(tag, "br")) {
                flush_line(1);
            } else if (tag_is(tag, "hr")) {
                gap_before(6);
                if (lt->nlines < MAX_LINES) {
                    struct dline *L = &lt->lines[lt->nlines];
                    memset(L, 0, sizeof(*L));
                    L->rule = 1;
                    L->height = (uint16_t)(pending_gap + 8);
                    L->ch = 8;
                    L->bg = -1;
                    note_new_line(lt->nlines);
                    lt->nlines++;
                }
                pending_gap = 6;
                line_reset();
            } else if (tag_is(tag, "img")) {
                handle_img(&cp, src, alt, widtha, heighta);
            } else if (tag_is(tag, "input")) {
                handle_input(typ, namea, valuea, has_checked, sizea);
            } else if (tag_is(tag, "select")) {
                struct ctrl *c = new_ctrl(CT_SELECT);
                if (c) {
                    b_strncpy(c->name, namea, sizeof(c->name));
                    cur_select = (int)(c - lt->ctrls);
                    opt_len = 0; opt_buf[0] = 0;
                    opt_has_val = 0; opt_selected = 0;
                    struct estack *e = push_elem(tag, &cp, 0, idattr);
                    if (e) e->sel_idx = cur_select;
                }
            } else if (tag_is(tag, "textarea")) {
                const char *close = find_close_tag(s, "textarea");
                const char *end = close ? close : s + b_strlen(s);
                struct ctrl *c = new_ctrl(CT_TEXTAREA);
                if (c) {
                    b_strncpy(c->name, namea, sizeof(c->name));
                    decode_entities_into(c->value, sizeof(c->value), s, end);
                    int rows = rowsa[0] ? atoi_(rowsa) : 3;
                    int cols = colsa[0] ? atoi_(colsa) : 30;
                    if (rows < 1) rows = 1;
                    if (rows > 8) rows = 8;
                    if (cols < 8) cols = 8;
                    if (cols > 80) cols = 80;
                    int idx = (int)(c - lt->ctrls);
                    place_widget(W_CTRL, idx, cols * 8 + 12, rows * 18 + 10);
                    c->line = lt->nlines;
                }
                s = end;
            } else if (tag_is(tag, "button")) {
                const char *close = find_close_tag(s, "button");
                const char *end = close ? close : s + b_strlen(s);
                /* label: raw content with tags stripped, ws collapsed */
                char raw[256];
                decode_entities_into(raw, sizeof(raw), s, end);
                char label[64];
                int li2 = 0, insp = 0;
                for (int i = 0; raw[i] && li2 < 60; i++) {
                    if (raw[i] == '<') {
                        while (raw[i] && raw[i] != '>') i++;
                        continue;
                    }
                    if (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\n') {
                        if (li2 > 0) insp = 1;
                        continue;
                    }
                    if (insp) { label[li2++] = ' '; insp = 0; }
                    label[li2++] = raw[i];
                }
                label[li2] = 0;
                int type = (typ[0] && b_strcasecmp(typ, "button") == 0)
                               ? CT_BUTTON : CT_SUBMIT;
                struct ctrl *c = new_ctrl(type);
                if (c) {
                    b_strncpy(c->name, namea, sizeof(c->name));
                    b_strncpy(c->value, valuea, sizeof(c->value));
                    b_strncpy(c->opts[0], label[0] ? label : "Submit",
                              sizeof(c->opts[0]));
                    int idx = (int)(c - lt->ctrls);
                    place_widget(W_CTRL, idx,
                                 (int)b_strlen(c->opts[0]) * 8 + 18, 22);
                    c->line = lt->nlines;
                }
                s = end;
            } else if (is_block_tag(tag) || cp.dblock == 1) {
                push_elem(tag, &cp, 1, idattr);
            } else {
                push_elem(tag, &cp, 0, idattr);
            }
            if (selfclose && !is_void_tag(tag)) pop_through_tag(tag);
            continue;
        }
        if (in_title || !in_head) {
            if (c == '&') {
                const char *e = s + 1;
                char name[12]; int ni = 0;
                while (*e && *e != ';' && ni < 11 &&
                       ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                        (*e >= '0' && *e <= '9') || *e == '#'))
                    name[ni++] = *e++;
                name[ni] = 0;
                if (*e == ';' && ni > 0) {
                    char rep = 0; int have = 0;
                    if (name[0] == '#') {
                        int v = 0;
                        if (name[1] == 'x' || name[1] == 'X') {
                            for (int i = 2; name[i]; i++) {
                                int hv = hexval(name[i]);
                                if (hv < 0) { v = -1; break; }
                                v = v * 16 + hv;
                            }
                        } else {
                            for (int i = 1; name[i]; i++) {
                                if (name[i] < '0' || name[i] > '9') { v = -1; break; }
                                v = v * 10 + (name[i] - '0');
                            }
                        }
                        if (v > 0 && v < 127) { rep = (char)v; have = 1; }
                    } else if (b_strcasecmp(name, "amp") == 0) { rep = '&'; have = 1; }
                    else if (b_strcasecmp(name, "lt") == 0) { rep = '<'; have = 1; }
                    else if (b_strcasecmp(name, "gt") == 0) { rep = '>'; have = 1; }
                    else if (b_strcasecmp(name, "quot") == 0) { rep = '"'; have = 1; }
                    else if (b_strcasecmp(name, "apos") == 0) { rep = '\''; have = 1; }
                    else if (b_strcasecmp(name, "nbsp") == 0) { rep = ' '; have = 1; }
                    if (have) {
                        if (rep == ' ') FLUSH_WORD();
                        else if (wlen < 150) word[wlen++] = rep;
                        s = e + 1;
                        continue;
                    }
                }
                if (wlen < 150) word[wlen++] = '&';
                s++;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                FLUSH_WORD();
                s++;
                continue;
            }
            if (wlen < 150) word[wlen++] = c;
            s++;
            continue;
        }
        s++; /* skipped region (head) */
    }
    FLUSH_WORD();
    while (esp > 0) pop_one();
    flush_line(0);
    if (t->title[0] == 0 && !append) b_strcpy(t->title, "(no title)");
    #undef FLUSH_WORD
}

static void compute_total_h(struct tab *t) {
    int tot = 0;
    for (int i = 0; i < t->nlines; i++) tot += t->lines[i].height;
    t->total_h = tot;
}

/* ================================================================
 * JavaScript in pages (Duktape) — the DOM subset.
 *
 *   document.title                    read/write (tab title)
 *   document.write(str)               HTML fragment appended to
 *                                     the end of the body after all
 *                                     scripts ran (not at the
 *                                     parse point — documented)
 *   document.getElementById(id)       element object or null;
 *     el.textContent                  read/write; elements are
 *                                     tracked at display-line
 *                                     granularity (an id on a block
 *                                     element covers its lines)
 *   console.log(...)                  goes to the status bar
 *
 * Nothing else: no window timers, no network, no file access.
 * Scripts run once, after layout, in document order. Each is
 * evaluated with duk_peval_string: a throw is caught, noted in
 * the status bar, and never breaks the page.
 * ================================================================ */
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);

static void *js_alloc(void *udata, duk_size_t size) {
    (void)udata;
    return malloc(size);
}
static void *js_realloc(void *udata, void *ptr, duk_size_t size) {
    (void)udata;
    return realloc(ptr, size);
}
static void js_freefn(void *udata, void *ptr) {
    (void)udata;
    free(ptr);
}
/* A Duktape fatal error (OOM, internal error) must never halt
 * the machine — that was the one path that could make page
 * navigation look like the browser "died". The fatal handler
 * longjmps back into run_scripts, which abandons the heap (a
 * bounded leak, documented in the README) and keeps the page. */
#include <setjmp.h>
static jmp_buf js_fatal_jmp;
static void js_fatal(void *udata, const char *msg) {
    (void)udata;
    print("web: Duktape fatal error: ");
    print(msg ? msg : "(no message)");
    print("\n");
    longjmp(js_fatal_jmp, 1);
}

static struct tab *js_tab = NULL;
static char status_msg[200];

#define JS_WRITE_SIZE 8192
static char js_write_buf[JS_WRITE_SIZE];
static size_t js_write_len = 0;

static duk_ret_t js_doc_write(duk_context *c) {
    const char *s = duk_to_string(c, 0);
    if (s) {
        size_t n = b_strlen(s);
        if (js_write_len + n > JS_WRITE_SIZE - 1)
            n = JS_WRITE_SIZE - 1 - js_write_len;
        for (size_t i = 0; i < n; i++) js_write_buf[js_write_len++] = s[i];
        js_write_buf[js_write_len] = 0;
    }
    return 0;
}

static duk_ret_t js_title_get(duk_context *c) {
    duk_push_string(c, js_tab->title);
    return 1;
}
static duk_ret_t js_title_set(duk_context *c) {
    const char *s = duk_to_string(c, 0);
    if (s) b_strncpy(js_tab->title, s, sizeof(js_tab->title));
    return 0;
}

static duk_ret_t js_console_log(duk_context *c) {
    char buf[200];
    buf[0] = 0;
    duk_idx_t n = duk_get_top(c);
    for (duk_idx_t i = 0; i < n; i++) {
        if (i) b_strcat(buf, " ");
        const char *s = duk_to_string(c, i);
        if (s) {
            size_t room = sizeof(buf) - 1 - b_strlen(buf);
            size_t l = b_strlen(s);
            if (l > room) l = room;
            char *d = buf + b_strlen(buf);
            for (size_t k = 0; k < l; k++) d[k] = s[k];
            d[l] = 0;
        }
    }
    b_strcpy(status_msg, buf);
    return 0;
}

/* --- element textContent --- */
static int elem_this_idx(duk_context *c) {
    duk_push_this(c);
    duk_get_prop_string(c, -1, "\xff" "idx");
    int idx = duk_to_int(c, -1);
    duk_pop_2(c);
    return idx;
}

static duk_ret_t js_el_text_get(duk_context *c) {
    int idx = elem_this_idx(c);
    static char buf[2048];
    buf[0] = 0;
    if (idx >= 0 && idx < nelems && elems[idx].start >= 0) {
        for (int li = elems[idx].start; li <= elems[idx].end; li++) {
            if (li > elems[idx].start) b_strcat(buf, " ");
            size_t room = sizeof(buf) - 1 - b_strlen(buf);
            size_t l = b_strlen(js_tab->lines[li].text);
            if (l > room) l = room;
            char *d = buf + b_strlen(buf);
            for (size_t k = 0; k < l; k++) d[k] = js_tab->lines[li].text[k];
            d[l] = 0;
        }
    }
    duk_push_string(c, buf);
    return 1;
}

/* Replace an element's display lines with wrapped new text. The
 * line count may change; later registry entries shift along. */
static void elem_set_text(struct tab *t, struct elem *e, const char *txt) {
    if (e->start < 0) return;
    int first = e->start, last = e->end;
    uint32_t color = RGB_TEXT;
    uint8_t scale = 1, style = 0;
    if (t->lines[first].nspans > 0) {
        color = t->lines[first].spans[0].color;
        scale = t->lines[first].spans[0].scale;
        style = t->lines[first].spans[0].style;
    }
    uint16_t height0 = t->lines[first].height;

    /* word-wrap txt into at most 8 lines of max_cols columns */
    static char wrapped[8][LINE_TEXT];
    int nw = 0;
    const char *p = txt;
    while (nw < 8) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        int ll = 0;
        while (*p) {
            const char *w = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
            int wl = (int)(p - w);
            int need = wl + (ll ? 1 : 0);
            if (ll + need > max_cols) break;
            if (ll) wrapped[nw][ll++] = ' ';
            if (wl > max_cols) wl = max_cols;
            for (int i = 0; i < wl; i++) wrapped[nw][ll++] = w[i];
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\n' || *p == '\r') break;
        }
        wrapped[nw][ll] = 0;
        nw++;
        if (!*p) break;
    }
    if (nw == 0) { wrapped[0][0] = 0; nw = 1; }

    int old_count = last - first + 1;
    int delta = nw - old_count;
    if (delta > 0 && t->nlines + delta > MAX_LINES) {
        nw -= delta - (MAX_LINES - t->nlines);
        delta = nw - old_count;
        if (nw <= 0) return;
    }
    if (delta != 0) {
        int mv = t->nlines - (last + 1);
        if (mv > 0)
            memmove(&t->lines[first + nw], &t->lines[last + 1],
                    (size_t)mv * sizeof(struct dline));
        t->nlines += delta;
        for (int i = 0; i < nelems; i++) {
            if (&elems[i] == e) continue;
            if (elems[i].start > last) {
                elems[i].start += delta;
                elems[i].end += delta;
            }
        }
    }
    e->end = first + nw - 1;
    for (int i = 0; i < nw; i++) {
        struct dline *L = &t->lines[first + i];
        uint16_t keep_h = L->height;
        int32_t keep_bg = L->bg;
        uint16_t keep_bgx = L->bg_x, keep_bgw = L->bg_w;
        memset(L, 0, sizeof(*L));
        b_strcpy(L->text, wrapped[i]);
        int ln = (int)b_strlen(wrapped[i]);
        L->nspans = ln ? 1 : 0;
        L->spans[0].col = 0;
        L->spans[0].len = (uint8_t)(ln > 200 ? 200 : ln);
        L->spans[0].scale = scale;
        L->spans[0].style = style;
        L->spans[0].color = color;
        L->spans[0].link = -1;
        L->height = (i == 0) ? height0 : keep_h;
        L->ch = (uint16_t)(16 * scale + 2);
        if (L->ch > L->height) L->ch = L->height;
        L->bg = keep_bg;
        L->bg_x = keep_bgx;
        L->bg_w = keep_bgw;
        L->bg_off = 0;
    }
    compute_total_h(t);
}

static duk_ret_t js_el_text_set(duk_context *c) {
    int idx = elem_this_idx(c);
    const char *s = duk_to_string(c, 0);
    if (idx >= 0 && idx < nelems && s)
        elem_set_text(js_tab, &elems[idx], s);
    return 0;
}

static duk_ret_t js_doc_getel(duk_context *c) {
    const char *id = duk_to_string(c, 0);
    int idx = -1;
    if (id) {
        for (int i = 0; i < nelems; i++)
            if (b_strcmp(elems[i].id, id) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        duk_push_null(c);
        return 1;
    }
    duk_push_object(c);
    duk_push_int(c, idx);
    duk_put_prop_string(c, -2, "\xff" "idx");
    duk_push_string(c, elems[idx].id);
    duk_put_prop_string(c, -2, "id");
    duk_push_string(c, "textContent");
    duk_push_c_function(c, js_el_text_get, 0);
    duk_push_c_function(c, js_el_text_set, 1);
    duk_def_prop(c, -4, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER);
    return 1;
}

static void run_scripts(struct tab *t) {
    if (nscripts == 0) return;
    js_tab = t;
    js_write_len = 0;
    js_write_buf[0] = 0;
    if (setjmp(js_fatal_jmp) != 0) {
        /* js_fatal landed here: abandon the heap, keep the page */
        b_strcpy(status_msg, "Script engine error (ignored)");
        return;
    }
    duk_context *ctx = duk_create_heap(js_alloc, js_realloc, js_freefn,
                                       NULL, js_fatal);
    if (!ctx) return;
    /* document */
    duk_push_object(ctx);
    duk_push_c_function(ctx, js_doc_write, 1);
    duk_put_prop_string(ctx, -2, "write");
    duk_push_c_function(ctx, js_doc_getel, 1);
    duk_put_prop_string(ctx, -2, "getElementById");
    duk_push_string(ctx, "title");
    duk_push_c_function(ctx, js_title_get, 0);
    duk_push_c_function(ctx, js_title_set, 1);
    duk_def_prop(ctx, -4, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER);
    duk_put_global_string(ctx, "document");
    /* console */
    duk_push_object(ctx);
    duk_push_c_function(ctx, js_console_log, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "log");
    duk_put_global_string(ctx, "console");

    for (int i = 0; i < nscripts; i++) {
        if (duk_peval_string(ctx, scripts[i]) != DUK_EXEC_SUCCESS) {
            if (status_msg[0] == 0)
                b_strcpy(status_msg, "Script error (ignored)");
        }
        duk_pop(ctx);
    }
    duk_destroy_heap(ctx);
    if (js_write_len > 0) {
        parse_html(t, js_write_buf, 1);
        compute_total_h(t);
    }
}

/* ================================================================
 * Chrome (Firefox Proton style) + rendering
 * ================================================================ */
#define TAB_STRIP_H 30
#define TOOLBAR_H 38
#define STATUS_H 16

static int content_top(void) { return TAB_STRIP_H + TOOLBAR_H; }
/* The browser is a window client (v0.8): all layout uses the
 * client-area size set by the window manager, never the screen. */
static int bv_w = 640, bv_h = 480;

void browser_win_set_geometry(int w, int h) {
    if (w < 80) w = 80;
    if (h < 80) h = 80;
    bv_w = w;
    bv_h = h;
}

static int content_bottom(void) { return bv_h - STATUS_H; }

struct vis_link { int x, y, w, h, link; };
static struct vis_link vis_links[64];
static int nvis = 0;
static int hover_link = -1;   /* index into the tab's links, or -1 */

struct vis_widget { int x, y, w, h, ctrl; };
static struct vis_widget vis_widgets[64];
static int nvisw = 0;

/* address-bar editing state */
static int editing = 0;
static int edit_selall = 0;
static char edit_buf[URL_LEN];
static int edit_len = 0, edit_cur = 0;

static void start_editing(const char *initial) {
    editing = 1;
    edit_selall = 1;
    b_strncpy(edit_buf, initial, sizeof(edit_buf));
    edit_len = (int)b_strlen(edit_buf);
    edit_cur = edit_len;
}

static int tab_width(void) {
    int avail = bv_w - 12 - 26;
    int tw = ntabs > 0 ? avail / ntabs : avail;
    if (tw > 190) tw = 190;
    if (tw < 44) tw = 44;
    return tw;
}
static int tab_x(int i) { return 6 + i * tab_width(); }

#define PILL_X0 106
static int pill_x1(void) { return bv_w - 8; }
#define PILL_Y (TAB_STRIP_H + 6)
#define PILL_H (TOOLBAR_H - 12)

static void draw_tab(int i, int active) {
    struct tab *t = &tabs[i];
    int tw = tab_width();
    int x = tab_x(i);
    int w = tw - 4;
    if (active) {
        /* floating tab, visually connected to the toolbar below:
         * rounded rect whose bottom edge meets the toolbar top */
        gfx_rounded_rect(x, 5, w, TAB_STRIP_H - 3, 7, COL_WIN_FACE);
    } else {
        gfx_rounded_rect(x, 10, w, TAB_STRIP_H - 10, 6, COL_TAB_INACT);
    }
    int tcol = active ? COL_INFO_TEXT : COL_TAB_TEXT_INACT;
    int show_close = (w >= 64);
    int maxchars = (w - (show_close ? 30 : 14)) / 8;
    if (maxchars < 1) maxchars = 1;
    char buf[64];
    b_strncpy(buf, t->title[0] ? t->title : t->url, sizeof(buf));
    if ((int)b_strlen(buf) > maxchars) buf[maxchars] = 0;
    gfx_text(x + 9, active ? 9 : 12, buf, tcol);
    if (show_close) {
        int cx = x + w - 13, cy = 14;
        gfx_line(cx, cy - 3, cx + 6, cy + 3, tcol);
        gfx_line(cx + 6, cy - 3, cx, cy + 3, tcol);
    }
}

static void draw_chevron(int cx, int cy, int dir, int color) {
    /* vertex at cx + 4*dir: dir -1 points left (back), +1 right */
    gfx_line(cx - 3 * dir, cy - 6, cx + 4 * dir, cy, color);
    gfx_line(cx + 4 * dir, cy, cx - 3 * dir, cy + 6, color);
    gfx_line(cx - 3 * dir + 1, cy - 6, cx + 4 * dir + 1, cy, color);
    gfx_line(cx + 4 * dir + 1, cy, cx - 3 * dir + 1, cy + 6, color);
}

static const int8_t ARC_PTS[12][2] = {
    { 7, 0 }, { 6, 3 }, { 3, 6 }, { 0, 7 }, { -3, 6 }, { -6, 3 },
    { -7, 0 }, { -6, -3 }, { -3, -6 }, { 0, -7 }, { 3, -6 }, { 6, -3 }
};
static void draw_reload_icon(int cx, int cy, int color) {
    for (int i = 2; i < 12; i++) {
        int j = (i + 1) % 12;
        if (j == 1) break; /* leave a gap before the arrowhead point */
        gfx_line(cx + ARC_PTS[i][0], cy + ARC_PTS[i][1],
                 cx + ARC_PTS[j][0], cy + ARC_PTS[j][1], color);
    }
    gfx_line(cx + 7, cy, cx + 1, cy - 1, color);
    gfx_line(cx + 7, cy, cx + 3, cy + 5, color);
}

#define CONTENT_X 6
#define RGB_CARET 0x41C3DBu
#define RGB_CTRL_TXT 0x182024u

static void box_outline(int x, int y, int w, int h, int color) {
    gfx_line(x, y, x + w - 1, y, color);
    gfx_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    gfx_line(x, y, x, y + h - 1, color);
    gfx_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

/* Draw one form control at client coords (x, y) sized w x h. */
static void draw_ctrl(struct tab *t, int idx, int x, int y, int w, int h) {
    struct ctrl *c = &t->ctrls[idx];
    int focused = (t->focus == idx);
    int bcol = focused ? COL_PILL_FOCUS : 8;
    switch (c->type) {
    case CT_TEXT:
    case CT_PASS: {
        gfx_rounded_rect(x, y, w, h, 4, COL_WHITE);
        box_outline(x, y, w, h, bcol);
        int fit = (w - 10) / 8;
        if (fit < 1) fit = 1;
        int len = (int)b_strlen(c->value);
        char show[CTRL_VAL];
        int n = 0;
        if (c->type == CT_PASS) {
            int m = len < fit ? len : fit;
            /* show the tail when focused (caret is at the end) */
            (void)m;
            for (int i = 0; i < len && n < fit; i++) show[n++] = '*';
        } else {
            int start = 0;
            if (focused && len > fit) start = len - fit;
            for (int i = start; i < len && n < fit; i++) show[n++] = c->value[i];
        }
        show[n] = 0;
        /* password masking shows the first `fit` stars; fine */
        gfx_text2(x + 5, y + 3, show, RGB_CTRL_TXT, 1, 0);
        if (focused && (timer_ticks / 50) % 2 == 0) {
            int cxp = x + 5 + n * 8;
            if (cxp < x + w - 4) gfx_text2(cxp, y + 3, "_", RGB_CARET, 1, 0);
        }
        break;
    }
    case CT_TEXTAREA: {
        gfx_rect(x, y, w, h, COL_WHITE);
        box_outline(x, y, w, h, bcol);
        int cols = (w - 10) / 8;
        if (cols < 1) cols = 1;
        int rows = (h - 6) / 18;
        if (rows < 1) rows = 1;
        /* draw the value wrapped, keeping the tail visible when
         * the control has focus (the caret lives at the end) */
        static char lines[16][LINE_TEXT];
        int nl = 0;
        int cl = 0;
        lines[0][0] = 0;
        for (const char *p = c->value; ; p++) {
            if (*p == '\n' || *p == 0) {
                lines[nl][cl] = 0;
                if (*p == 0 || nl >= 15) break;
                nl++;
                cl = 0;
                lines[nl][0] = 0;
                continue;
            }
            if (cl >= cols) {
                lines[nl][cl] = 0;
                if (nl >= 15) break;
                nl++;
                cl = 0;
                lines[nl][0] = 0;
            }
            lines[nl][cl++] = *p;
        }
        nl++;
        int start = 0;
        if (nl > rows) start = focused ? nl - rows : 0;
        for (int i = 0; i < rows && start + i < nl; i++)
            gfx_text2(x + 5, y + 3 + i * 18, lines[start + i],
                      RGB_CTRL_TXT, 1, 0);
        if (focused && (timer_ticks / 50) % 2 == 0 && nl > 0) {
            int li = nl - 1;
            if (li >= start && li < start + rows) {
                int cxp = x + 5 + (int)b_strlen(lines[li]) * 8;
                if (cxp < x + w - 4)
                    gfx_text2(cxp, y + 3 + (li - start) * 18, "_",
                              RGB_CARET, 1, 0);
            }
        }
        break;
    }
    case CT_CHECK: {
        int by = y + (h - 14) / 2;
        gfx_rect(x, by, 14, 14, COL_WHITE);
        box_outline(x, by, 14, 14, bcol);
        if (c->checked) {
            gfx_line(x + 3, by + 7, x + 6, by + 10, COL_INFO_TEXT);
            gfx_line(x + 6, by + 10, x + 11, by + 3, COL_INFO_TEXT);
            gfx_line(x + 3, by + 8, x + 6, by + 11, COL_INFO_TEXT);
            gfx_line(x + 6, by + 11, x + 11, by + 4, COL_INFO_TEXT);
        }
        break;
    }
    case CT_RADIO: {
        int by = y + (h - 14) / 2;
        gfx_rounded_rect(x, by, 14, 14, 6, COL_WHITE);
        if (c->checked)
            gfx_rounded_rect(x + 4, by + 4, 6, 6, 3, COL_INFO_TEXT);
        break;
    }
    case CT_SUBMIT:
    case CT_BUTTON: {
        gfx_rounded_rect(x, y, w, h, 5, COL_BTN_FACE);
        box_outline(x, y, w, h, focused ? COL_PILL_FOCUS : COL_EDGE_DARK);
        const char *lab = c->opts[0];
        int tw = (int)b_strlen(lab) * 8;
        gfx_text2(x + (w - tw) / 2, y + 3, lab, RGB_CTRL_TXT, 1, 0);
        break;
    }
    case CT_SELECT: {
        gfx_rounded_rect(x, y, w, h, 5, COL_BTN_FACE);
        box_outline(x, y, w, h, bcol);
        char show[48];
        b_strncpy(show, c->nopts > 0 ? c->opts[c->sel] : "", sizeof(show));
        int fit = (w - 26) / 8;
        if (fit > 0 && (int)b_strlen(show) > fit) show[fit] = 0;
        gfx_text2(x + 6, y + 3, show, RGB_CTRL_TXT, 1, 0);
        gfx_line(x + w - 16, y + 8, x + w - 11, y + 13, COL_INFO_TEXT);
        gfx_line(x + w - 11, y + 13, x + w - 6, y + 8, COL_INFO_TEXT);
        break;
    }
    default:
        break;
    }
}

static void render(int mx, int my) {
    struct tab *t = &tabs[cur_tab];
    int W = bv_w, H = bv_h;
    int top = content_top(), bottom = content_bottom();

    /* ---- content ---- */
    gfx_fill_rgb(0, top, W, bottom - top, t->content_bg);
    nvis = 0;
    nvisw = 0;
    hover_link = -1;
    int y = top - t->scroll_y;
    for (int i = 0; i < t->nlines; i++) {
        struct dline *L = &t->lines[i];
        int lh = L->height;
        if (y + lh > top && y < bottom) {
            int cy = y + (lh - L->ch); /* content top */
            if (L->bg >= 0)
                gfx_fill_rgb(CONTENT_X + L->bg_x, y + L->bg_off, L->bg_w,
                             lh - L->bg_off, (uint32_t)L->bg);
            if (L->border) {
                int bc = gfx_nearest_rgb((int)((L->border_rgb >> 16) & 0xFF),
                                         (int)((L->border_rgb >> 8) & 0xFF),
                                         (int)(L->border_rgb & 0xFF));
                int bx = CONTENT_X + L->bg_x, bw2 = L->bg_w;
                int by0 = y + L->bg_off, by1 = y + lh - 1;
                if (L->border & 4) gfx_line(bx, by0, bx, by1, bc);
                if (L->border & 8) gfx_line(bx + bw2 - 1, by0, bx + bw2 - 1, by1, bc);
                if (L->border & 1) gfx_line(bx, by0, bx + bw2 - 1, by0, bc);
                if (L->border & 2) gfx_line(bx, by1, bx + bw2 - 1, by1, bc);
            }
            if (L->rule) {
                gfx_line(CONTENT_X, cy + 3, W - 16, cy + 3, 8);
            } else {
                for (int si = 0; si < L->nspans; si++) {
                    struct dspan *sp = &L->spans[si];
                    char seg[LINE_TEXT + 1];
                    int ln = sp->len;
                    int avail = (int)b_strlen(L->text) - sp->coff;
                    if (ln > avail) ln = avail;
                    if (ln <= 0) continue;
                    for (int k = 0; k < ln; k++) seg[k] = L->text[sp->coff + k];
                    seg[ln] = 0;
                    int sx = CONTENT_X + sp->col;
                    int sh = 16 * sp->scale;
                    gfx_text2(sx, cy, seg, sp->color, sp->scale, sp->style);
                    if (sp->link >= 0) {
                        if (nvis < 64) {
                            vis_links[nvis].x = sx;
                            vis_links[nvis].y = cy;
                            vis_links[nvis].w = ln * 8 * sp->scale;
                            vis_links[nvis].h = sh;
                            vis_links[nvis].link = sp->link;
                            nvis++;
                        }
                        if (mx >= sx && mx < sx + ln * 8 * sp->scale &&
                            my >= cy && my < cy + sh)
                            hover_link = sp->link;
                    }
                }
                if (L->wkind == W_IMG && L->widx >= 0 && L->widx < t->nimgs) {
                    struct img *im = &t->imgs[L->widx];
                    if (im->ok && im->px)
                        gfx_blit_rgba_scaled(CONTENT_X + L->wx, cy, L->ww,
                                             L->wh, im->px, im->nw, im->nh,
                                             t->content_bg);
                } else if (L->wkind == W_CTRL && L->widx >= 0 &&
                           L->widx < t->nctrls) {
                    int wx = CONTENT_X + L->wx;
                    draw_ctrl(t, L->widx, wx, cy, L->ww, L->wh);
                    if (nvisw < 64) {
                        vis_widgets[nvisw].x = wx;
                        vis_widgets[nvisw].y = cy;
                        vis_widgets[nvisw].w = L->ww;
                        vis_widgets[nvisw].h = L->wh;
                        vis_widgets[nvisw].ctrl = L->widx;
                        nvisw++;
                    }
                }
            }
        }
        y += lh;
    }
    /* scrollbar */
    int vp_h = bottom - top;
    gfx_rect(W - 10, top, 8, vp_h, COL_TAB_INACT);
    if (t->total_h > vp_h && t->total_h > 0) {
        int th = vp_h * vp_h / t->total_h;
        if (th < 16) th = 16;
        int ty = top + (vp_h - th) * t->scroll_y / (t->total_h - vp_h);
        gfx_rounded_rect(W - 9, ty, 6, th, 2, COL_ACCENT);
    }

    /* ---- tab strip ---- */
    gfx_rect(0, 0, W, TAB_STRIP_H, COL_TAB_STRIP);
    for (int i = 0; i < ntabs; i++)
        if (i != cur_tab) draw_tab(i, 0);
    draw_tab(cur_tab, 1);
    /* new-tab "+" button */
    int plus_x = tab_x(ntabs) + 2;
    gfx_text(plus_x + 4, 8, "+",
             ntabs < MAX_TABS ? COL_TAB_TEXT_INACT : COL_TAB_INACT);

    /* ---- toolbar ---- */
    gfx_rect(0, TAB_STRIP_H, W, TOOLBAR_H, COL_WIN_FACE);
    gfx_rect(0, TAB_STRIP_H + TOOLBAR_H - 1, W, 1, COL_TAB_INACT);
    int cy = TAB_STRIP_H + TOOLBAR_H / 2;
    draw_chevron(19, cy, -1, t->nback > 0 ? COL_INFO_TEXT : COL_TAB_INACT);
    draw_chevron(54, cy, +1, t->nfwd > 0 ? COL_INFO_TEXT : COL_TAB_INACT);
    draw_reload_icon(87, cy, COL_INFO_TEXT);

    /* address pill */
    int px0 = PILL_X0, px1 = pill_x1();
    if (editing) {
        gfx_rounded_rect(px0 - 2, PILL_Y - 2, px1 - px0 + 4, PILL_H + 4,
                         15, COL_PILL_FOCUS);
    }
    gfx_rounded_rect(px0, PILL_Y, px1 - px0, PILL_H, 13, COL_WHITE);
    int fit = (px1 - px0 - 20) / 8;
    if (fit < 1) fit = 1;
    if (editing) {
        int start = 0;
        if (edit_cur > fit) start = edit_cur - fit;
        if (start > edit_len - fit && edit_len - fit >= 0) start = edit_len - fit;
        if (start < 0) start = 0;
        char show[URL_LEN];
        int n = 0;
        for (int i = start; edit_buf[i] && n < fit; i++) show[n++] = edit_buf[i];
        show[n] = 0;
        gfx_text(px0 + 10, PILL_Y + 5, show, COL_INFO_TEXT);
        if ((timer_ticks / 50) % 2 == 0) {
            int cxp = px0 + 10 + (edit_cur - start) * 8;
            if (cxp < px1 - 6) gfx_text(cxp, PILL_Y + 5, "_", COL_PILL_FOCUS);
        }
    } else {
        char show[URL_LEN];
        b_strncpy(show, t->is_start ? "" : t->url, sizeof(show));
        if ((int)b_strlen(show) > fit) show[fit] = 0;
        gfx_text(px0 + 10, PILL_Y + 5, show, COL_INFO_TEXT);
    }

    /* ---- status bar ---- */
    gfx_rect(0, H - STATUS_H, W, STATUS_H, COL_TAB_STRIP);
    const char *st = status_msg;
    if (hover_link >= 0 && hover_link < t->nlinks) st = t->links[hover_link];
    char sbuf[160];
    b_strncpy(sbuf, st, sizeof(sbuf));
    int sf = (W - 12) / 8;
    if ((int)b_strlen(sbuf) > sf) sbuf[sf] = 0;
    gfx_text(6, H - STATUS_H, sbuf, COL_TAB_TEXT_INACT);
    /* no flip: the window manager presents (v0.8) */
}

static void clamp_scroll(struct tab *t) {
    int vp_h = content_bottom() - content_top();
    int maxs = t->total_h - vp_h;
    if (maxs < 0) maxs = 0;
    if (t->scroll_y > maxs) t->scroll_y = maxs;
    if (t->scroll_y < 0) t->scroll_y = 0;
}

static void set_max_cols(void) {
    int W = bv_w;
    max_cols = (W - 26) / 8;
    if (max_cols > LINE_TEXT - 2) max_cols = LINE_TEXT - 2;
    if (max_cols < 10) max_cols = 10;
}

/* ================================================================
 * Navigation / tab management
 * ================================================================ */
enum { NAV_NONE = 0, NAV_URL, NAV_BACK, NAV_FWD, NAV_RELOAD, NAV_POST };
static int nav_req = NAV_NONE;
static char nav_url[URL_LEN];
#define POST_BODY_SIZE 4096
static char post_body[POST_BODY_SIZE];

static void hist_push(char stack[][URL_LEN], int *n, const char *url) {
    if (*n >= HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++) b_strcpy(stack[i - 1], stack[i]);
        *n = HIST_MAX - 1;
    }
    b_strcpy(stack[*n], url);
    (*n)++;
}

static const char START_HTML[] =
    "<title>New Tab</title>"
    "<h1>Shiro Start</h1>"
    "<p>Type an address in the bar above and press Enter.</p>"
    "<p>Ctrl+T new tab - Ctrl+W close tab - Ctrl+L address bar</p>"
    "<p>Left/Right: back/forward - PgUp/PgDn: scroll - the window x closes</p>";

static void init_start_page(struct tab *t) {
    b_strcpy(t->url, "about:start");
    t->is_start = 1;
    t->loaded = 1;
    t->scroll_y = 0;
    t->nback = 0;
    t->nfwd = 0;
    if (t->html) { kfree(t->html); t->html = NULL; }
    parse_html(t, START_HTML, 0);
    compute_total_h(t);
    t->layout_w = bv_w;
}

/* POST `body` to `url` into page_html (form submission). */
static int fetch_post(const char *url, const char *body) {
    page_len = 0;
    page_truncated = 0;
    print("web: posting to ");
    print(url);
    print(" ...\n");
    int rc = net_post(url, body, (uint32_t)b_strlen(body),
                      fetch_sink, NULL, NULL);
    page_html[page_len] = 0;
    if (rc == 2) page_truncated = 1;
    if (rc != 1 && rc != 2) {
        print("web: post failed.\n");
        return 0;
    }
    return 1;
}

/* Fetch + history bookkeeping for the current tab (TEXT MODE side).
 * Returns 1 when page_html holds a fresh page to parse, 0 on
 * failure (the tab is left untouched — a failed navigation never
 * disturbs the current page or the browser). On success the tab
 * URL becomes the FINAL URL after any redirects. */
static int do_load(int kind, const char *url_arg) {
    struct tab *t = &tabs[cur_tab];
    static char target[URL_LEN];
    if (kind == NAV_BACK) {
        if (t->nback <= 0) return 0;
        b_strcpy(target, t->back[t->nback - 1]);
    } else if (kind == NAV_FWD) {
        if (t->nfwd <= 0) return 0;
        b_strcpy(target, t->fwd[t->nfwd - 1]);
    } else if (kind == NAV_RELOAD) {
        if (!t->loaded || t->is_start) return 0;
        b_strcpy(target, t->url);
    } else {
        b_strncpy(target, url_arg, sizeof(target));
    }
    int ok = (kind == NAV_POST) ? fetch_post(target, post_body)
                                : fetch_page(target);
    if (!ok) {
        b_strcpy(status_msg, "Could not load the page.");
        return 0;
    }
    if (kind == NAV_URL || kind == NAV_POST) {
        if (t->loaded && !t->is_start) hist_push(t->back, &t->nback, t->url);
        t->nfwd = 0;
    } else if (kind == NAV_BACK) {
        t->nback--;
        hist_push(t->fwd, &t->nfwd, t->url);
    } else if (kind == NAV_FWD) {
        t->nfwd--;
        hist_push(t->back, &t->nback, t->url);
    }
    const char *fin = net_last_url();
    if (fin && fin[0]) b_strncpy(t->url, fin, sizeof(t->url));
    else b_strcpy(t->url, target);
    t->is_start = 0;
    t->loaded = 1;
    t->scroll_y = 0;
    if (page_truncated)
        b_strcpy(status_msg, "Large page: showing the first 256 KB.");
    else
        status_msg[0] = 0;
    return 1;
}

/* Graphics-side completion of a load: prepare assets (CSS +
 * images), parse + scripts. Keeps an owned copy of the source so
 * the page can be re-laid-out when the window is resized. The
 * previous page's decoded images are released here (a resize
 * reparse keeps them via reparse_tab). */
static void finish_load(struct tab *t) {
    tab_free_images(t);
    prepare_assets(t, page_html);
    parse_html(t, page_html, 0);
    run_scripts(t);
    compute_total_h(t);
    clamp_scroll(t);
    if (t->html) { kfree(t->html); t->html = NULL; }
    t->html = kmalloc(page_len + 1);
    if (t->html) memcpy(t->html, page_html, page_len + 1);
    t->layout_w = bv_w;
}

/* Rebuild the layout from the stored source (window resized).
 * Form control values typed by the user are preserved across the
 * reparse (matched by document order). */
static void reparse_tab(struct tab *t) {
    if (!t->loaded) return;
    if (t->is_start || !t->html) {
        init_start_page(t);
        return;
    }
    static struct ctrl saved[MAX_CTRLS];
    int n_saved = t->nctrls;
    for (int i = 0; i < n_saved; i++) saved[i] = t->ctrls[i];
    int saved_focus = t->focus;
    prepare_assets(t, t->html);
    parse_html(t, t->html, 0);
    if (t->nctrls == n_saved) {
        for (int i = 0; i < n_saved; i++) {
            if (saved[i].type == t->ctrls[i].type) {
                b_strcpy(t->ctrls[i].value, saved[i].value);
                t->ctrls[i].checked = saved[i].checked;
                t->ctrls[i].sel = saved[i].sel;
            }
        }
        t->focus = saved_focus;
    }
    run_scripts(t);
    compute_total_h(t);
    clamp_scroll(t);
    t->layout_w = bv_w;
}

static void new_tab(void) {
    if (ntabs >= MAX_TABS) return;
    struct tab *t = &tabs[ntabs];
    memset(t, 0, sizeof(*t));
    ntabs++;
    cur_tab = ntabs - 1;
    init_start_page(t);
    start_editing("");
    status_msg[0] = 0;
}

/* Returns 1 if the browser should exit (last tab closed). */
static int close_tab(int idx) {
    if (ntabs <= 1) return 1;
    if (tabs[idx].html) { kfree(tabs[idx].html); tabs[idx].html = NULL; }
    if (tabs[idx].lines) { kfree(tabs[idx].lines); tabs[idx].lines = NULL; }
    tab_free_images(&tabs[idx]);
    for (int i = idx; i < ntabs - 1; i++)
        tabs[i] = tabs[i + 1];
    ntabs--;
    /* the shift left a duplicate of the last tab in the tail
     * slot; clear it so no pointer is owned twice */
    memset(&tabs[ntabs], 0, sizeof(tabs[ntabs]));
    if (cur_tab > idx) cur_tab--;
    if (cur_tab >= ntabs) cur_tab = ntabs - 1;
    editing = 0;
    return 0;
}

/* ================================================================
 * Form submission (v1.0)
 * ================================================================ */
static void url_encode(char *dst, size_t dstsz, const char *src) {
    static const char hexd[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)src; *p; p++) {
        uint8_t ch = *p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '*') {
            if (o < dstsz - 1) dst[o++] = (char)ch;
        } else if (ch == ' ') {
            if (o < dstsz - 1) dst[o++] = '+';
        } else if (o < dstsz - 3) {
            dst[o++] = '%';
            dst[o++] = hexd[ch >> 4];
            dst[o++] = hexd[ch & 15];
        }
    }
    dst[o] = 0;
}

/* Build the form data set and navigate: GET folds it into the
 * action URL's query, POST sends it as the request body. The
 * result page loads as a normal navigation (history + title). */
static void submit_form(struct tab *t, int fi, int submitter) {
    if (fi < 0 || fi >= t->nforms) return;
    struct form *f = &t->forms[fi];
    static char data[POST_BODY_SIZE];
    size_t o = 0;
    data[0] = 0;
    for (int i = f->first; i < f->first + f->count && i < t->nctrls; i++) {
        struct ctrl *c = &t->ctrls[i];
        if (c->form != fi || c->name[0] == 0) continue;
        const char *val = NULL;
        switch (c->type) {
        case CT_TEXT: case CT_PASS: case CT_HIDDEN: case CT_TEXTAREA:
            val = c->value;
            break;
        case CT_CHECK: case CT_RADIO:
            if (c->checked) val = c->value;
            break;
        case CT_SELECT:
            if (c->nopts > 0) val = c->ovals[c->sel];
            break;
        case CT_SUBMIT:
            if (i == submitter) val = c->value;
            break;
        default:
            break;
        }
        if (!val) continue;
        static char enc_n[160], enc_v[1200];
        url_encode(enc_n, sizeof(enc_n), c->name);
        url_encode(enc_v, sizeof(enc_v), val);
        size_t need = b_strlen(enc_n) + 1 + b_strlen(enc_v) + (o ? 1 : 0);
        if (o + need > sizeof(data) - 1) break;
        if (o) data[o++] = '&';
        for (const char *p = enc_n; *p; p++) data[o++] = *p;
        data[o++] = '=';
        for (const char *p = enc_v; *p; p++) data[o++] = *p;
        data[o] = 0;
    }
    if (f->post) {
        b_strncpy(nav_url, f->action, sizeof(nav_url));
        b_strncpy(post_body, data, sizeof(post_body));
        nav_req = NAV_POST;
    } else {
        static char dest[URL_LEN];
        b_strncpy(dest, f->action, sizeof(dest));
        for (int i = 0; dest[i]; i++)
            if (dest[i] == '?') { dest[i] = 0; break; }
        size_t dl = b_strlen(dest);
        if (data[0] && dl + 1 < sizeof(dest) - 1) {
            dest[dl++] = '?';
            size_t room = sizeof(dest) - 1 - dl;
            size_t n = b_strlen(data);
            if (n > room) n = room;
            for (size_t i = 0; i < n; i++) dest[dl++] = data[i];
            dest[dl] = 0;
        }
        b_strncpy(nav_url, dest, sizeof(nav_url));
        nav_req = NAV_URL;
    }
    editing = 0;
}

/* Activate a control (mouse click): focus fields, toggle
 * checkboxes, select radio, cycle selects, submit forms. */
static void activate_ctrl(struct tab *t, int idx) {
    if (idx < 0 || idx >= t->nctrls) return;
    struct ctrl *c = &t->ctrls[idx];
    switch (c->type) {
    case CT_TEXT: case CT_PASS: case CT_TEXTAREA: case CT_BUTTON:
        t->focus = idx;
        break;
    case CT_SELECT:
        t->focus = idx;
        if (c->nopts > 0) c->sel = (c->sel + 1) % c->nopts;
        break;
    case CT_CHECK:
        t->focus = idx;
        c->checked = !c->checked;
        break;
    case CT_RADIO:
        t->focus = idx;
        if (c->name[0]) {
            for (int i = 0; i < t->nctrls; i++) {
                struct ctrl *o = &t->ctrls[i];
                if (o->type == CT_RADIO && o->form == c->form &&
                    b_strcmp(o->name, c->name) == 0)
                    o->checked = 0;
            }
        }
        c->checked = 1;
        break;
    case CT_SUBMIT:
        if (c->form >= 0) submit_form(t, c->form, idx);
        break;
    default:
        break;
    }
}

/* Move keyboard focus through the tab's visible controls. */
static void focus_next(struct tab *t, int dir) {
    if (t->nctrls == 0) return;
    int start = t->focus;
    for (int step = 1; step <= t->nctrls; step++) {
        int i = start + dir * step;
        if (dir > 0) i %= t->nctrls;
        else { i %= t->nctrls; if (i < 0) i += t->nctrls; }
        if (t->ctrls[i].type == CT_HIDDEN) continue;
        t->focus = i;
        /* scroll the control's line into view */
        int line = t->ctrls[i].line;
        if (line >= 0) {
            int yoff = 0;
            for (int li = 0; li < line && li < t->nlines; li++)
                yoff += t->lines[li].height;
            int vp_h = content_bottom() - content_top();
            if (yoff < t->scroll_y) t->scroll_y = yoff;
            else if (yoff > t->scroll_y + vp_h - 40)
                t->scroll_y = yoff - vp_h + 60;
            clamp_scroll(t);
        }
        return;
    }
}

/* ================================================================
 * Window-client API (v0.8): the browser lives inside a window of
 * the desktop window manager. gui.c owns the frame, routes input,
 * and presents; this file renders the client area (via the gfx_*
 * viewport seam) and handles navigation. Fetches block the GUI
 * briefly, exactly like the old full-screen browser blocked on
 * its navigation cycle; progress text goes to the (invisible)
 * text console and the result lands in the status bar.
 * ================================================================ */
static int bwant_close = 0;  /* last tab closed: WM should hide us */
static int sb_drag = 0;      /* scrollbar drag in progress */

void browser_win_reset(void) {
    for (int i = 0; i < ntabs; i++) {
        if (tabs[i].html) { kfree(tabs[i].html); tabs[i].html = NULL; }
        if (tabs[i].lines) { kfree(tabs[i].lines); tabs[i].lines = NULL; }
        tab_free_images(&tabs[i]);
    }
    ntabs = 1;
    cur_tab = 0;
    memset(&tabs[0], 0, sizeof(tabs[0]));
    editing = 0;
    status_msg[0] = 0;
    nav_req = NAV_NONE;
    bwant_close = 0;
    sb_drag = 0;
    set_max_cols();
    init_start_page(&tabs[0]);
}

void browser_win_open_url(const char *url) {
    if (!url || !url[0]) return;
    static char first[URL_LEN];
    make_absolute(url, first, sizeof(first));
    b_strcpy(nav_url, first);
    nav_req = NAV_URL;
    editing = 0;
}

int browser_win_editing(void) { return editing; }
int browser_win_want_close(void) { return bwant_close; }
void browser_win_ack_close(void) { bwant_close = 0; }

void browser_win_draw(int lmx, int lmy) {
    render(lmx, lmy);
}

/* Do pending work: a requested navigation (fetch + parse) and/or
 * a re-layout after a resize. Returns 1 if anything changed. */
int browser_win_pump(void) {
    int did = 0;
    if (nav_req != NAV_NONE) {
        int kind = nav_req;
        nav_req = NAV_NONE;
        editing = 0;
        struct tab *t = &tabs[cur_tab];
        if (kind == NAV_RELOAD && t->is_start) {
            init_start_page(t);
        } else if (do_load(kind, nav_url)) {
            parse_oom = 0;
            finish_load(&tabs[cur_tab]);
            if (parse_oom)
                b_strcpy(status_msg, "Out of memory for this page.");
        }
        did = 1;
    }
    struct tab *t = &tabs[cur_tab];
    if (t->loaded && t->layout_w != bv_w) {
        set_max_cols();
        reparse_tab(t);
        did = 1;
    }
    return did;
}

void browser_win_resized(void) {
    set_max_cols();
    /* the pump reparses the current tab if the width changed */
}

/* Handle one key (keyboard_try_key encoding). Returns 1 when the
 * browser consumed it. ESC is consumed only to cancel address
 * editing; otherwise the window manager owns it (GUI exit). */
int browser_win_key(int k) {
    struct tab *t = &tabs[cur_tab];
    if (k == 0x14) {          /* Ctrl+T */
        new_tab();
        return 1;
    }
    if (k == 0x17) {          /* Ctrl+W */
        if (close_tab(cur_tab)) bwant_close = 1;
        return 1;
    }
    if (k == 0x0C) {          /* Ctrl+L */
        start_editing(t->is_start ? "" : t->url);
        return 1;
    }
    if (k == 0x12) {          /* Ctrl+R */
        nav_req = NAV_RELOAD;
        return 1;
    }
    if (editing) {
        if (k == 27) {        /* ESC cancels editing */
            editing = 0;
            return 1;
        }
        if (k == '\r' || k == '\n') {
            static char dest[URL_LEN];
            make_absolute(edit_buf, dest, sizeof(dest));
            b_strcpy(nav_url, dest);
            nav_req = NAV_URL;
            editing = 0;
            return 1;
        }
        if (k == '\b' || k == KEY_DEL) {
            if (edit_selall) {
                edit_buf[0] = 0;
                edit_len = edit_cur = 0;
                edit_selall = 0;
            } else if (k == '\b' && edit_cur > 0) {
                for (int i = edit_cur - 1; i < edit_len; i++)
                    edit_buf[i] = edit_buf[i + 1];
                edit_cur--;
                edit_len--;
            } else if (k == KEY_DEL && edit_cur < edit_len) {
                for (int i = edit_cur; i < edit_len; i++)
                    edit_buf[i] = edit_buf[i + 1];
                edit_len--;
            }
            return 1;
        }
        if (k == KEY_LEFT)  { if (edit_cur > 0) edit_cur--; edit_selall = 0; return 1; }
        if (k == KEY_RIGHT) { if (edit_cur < edit_len) edit_cur++; edit_selall = 0; return 1; }
        if (k == KEY_HOME)  { edit_cur = 0; edit_selall = 0; return 1; }
        if (k == KEY_END)   { edit_cur = edit_len; edit_selall = 0; return 1; }
        if (k >= 32 && k < 127) {
            if (edit_selall) {
                edit_buf[0] = 0;
                edit_len = edit_cur = 0;
                edit_selall = 0;
            }
            if (edit_len < URL_LEN - 1) {
                for (int i = edit_len; i > edit_cur; i--)
                    edit_buf[i] = edit_buf[i - 1];
                edit_buf[edit_cur++] = (char)k;
                edit_buf[++edit_len] = 0;
            }
            return 1;
        }
        return 1; /* editing swallows the rest */
    }
    /* form focus: a focused control takes the keyboard */
    if (t->focus >= 0 && t->focus < t->nctrls) {
        struct ctrl *c = &t->ctrls[t->focus];
        if (k == '\t') { focus_next(t, 1); return 1; }
        if (c->type == CT_TEXT || c->type == CT_PASS ||
            c->type == CT_TEXTAREA) {
            if (k == 27) { t->focus = -1; return 1; } /* ESC blurs */
            if (k == '\b') {
                size_t l = b_strlen(c->value);
                if (l > 0) c->value[l - 1] = 0;
                return 1;
            }
            if (k == '\r' || k == '\n') {
                if (c->type == CT_TEXTAREA) {
                    size_t l = b_strlen(c->value);
                    if (l < sizeof(c->value) - 1) {
                        c->value[l] = '\n';
                        c->value[l + 1] = 0;
                    }
                    return 1;
                }
                /* implicit submission: Enter submits when the form
                 * has a submit control */
                if (c->form >= 0 && c->form < t->nforms) {
                    struct form *f = &t->forms[c->form];
                    int sub = -1;
                    for (int i = f->first;
                         i < f->first + f->count && i < t->nctrls; i++)
                        if (t->ctrls[i].type == CT_SUBMIT) { sub = i; break; }
                    if (sub >= 0) submit_form(t, c->form, sub);
                }
                return 1;
            }
            if (k >= 32 && k < 127) {
                size_t l = b_strlen(c->value);
                if (l < sizeof(c->value) - 1) {
                    c->value[l] = (char)k;
                    c->value[l + 1] = 0;
                }
                return 1;
            }
            return 1; /* the field swallows other keys */
        }
        if (k == 27) { t->focus = -1; return 1; }
        if ((k == ' ' || k == '\r' || k == '\n') &&
            (c->type == CT_CHECK || c->type == CT_RADIO ||
             c->type == CT_SELECT || c->type == CT_SUBMIT)) {
            activate_ctrl(t, t->focus);
            return 1;
        }
    }
    if (k == '\t') {         /* Tab enters / cycles the form */
        if (t->nctrls > 0) { focus_next(t, 1); return 1; }
    }
    /* not editing */
    if (k == 27) return 0;    /* ESC belongs to the window manager */
    if (k == KEY_PGDN) {
        t->scroll_y += (content_bottom() - content_top()) - 32;
        clamp_scroll(t);
        return 1;
    }
    if (k == KEY_PGUP) {
        t->scroll_y -= (content_bottom() - content_top()) - 32;
        clamp_scroll(t);
        return 1;
    }
    if (k == KEY_DOWN) { t->scroll_y += 32; clamp_scroll(t); return 1; }
    if (k == KEY_UP)   { t->scroll_y -= 32; clamp_scroll(t); return 1; }
    if (k == KEY_LEFT)  { if (t->nback > 0) nav_req = NAV_BACK; return 1; }
    if (k == KEY_RIGHT) { if (t->nfwd > 0) nav_req = NAV_FWD; return 1; }
    if (k == KEY_END)  { t->scroll_y = t->total_h; clamp_scroll(t); return 1; }
    if (k == KEY_HOME) { t->scroll_y = 0; return 1; }
    return 0;
}

/* Mouse press at client-local (lx, ly). */
void browser_win_mouse_down(int lx, int ly) {
    struct tab *t = &tabs[cur_tab];
    if (ly < TAB_STRIP_H) {
        int tw = tab_width();
        int plus_x = tab_x(ntabs) + 2;
        if (lx >= plus_x && lx < plus_x + 24) {
            new_tab();
        } else {
            for (int i = 0; i < ntabs; i++) {
                int x = tab_x(i);
                if (lx >= x && lx < x + tw - 4) {
                    int show_close = (tw - 4 >= 64);
                    if (show_close && lx >= x + tw - 4 - 16) {
                        if (close_tab(i)) bwant_close = 1;
                    } else {
                        cur_tab = i;
                        editing = 0;
                    }
                    break;
                }
            }
        }
    } else if (ly < TAB_STRIP_H + TOOLBAR_H) {
        if (lx < 38) {
            if (t->nback > 0) nav_req = NAV_BACK;
        } else if (lx < 72) {
            if (t->nfwd > 0) nav_req = NAV_FWD;
        } else if (lx < PILL_X0) {
            nav_req = NAV_RELOAD;
        } else if (lx >= PILL_X0 && lx < pill_x1()) {
            start_editing(t->is_start ? "" : t->url);
        }
    } else if (ly >= content_top() && ly < content_bottom()) {
        if (lx >= bv_w - 10) {
            sb_drag = 1;
            int vp_h = content_bottom() - content_top();
            int rel = ly - content_top();
            if (vp_h > 0 && t->total_h > vp_h)
                t->scroll_y = (t->total_h - vp_h) * rel / vp_h;
            clamp_scroll(t);
        } else {
            int hit = 0;
            for (int i = 0; i < nvisw && !hit; i++) {
                if (lx >= vis_widgets[i].x && lx < vis_widgets[i].x + vis_widgets[i].w &&
                    ly >= vis_widgets[i].y && ly < vis_widgets[i].y + vis_widgets[i].h) {
                    activate_ctrl(t, vis_widgets[i].ctrl);
                    hit = 1;
                }
            }
            if (!hit) {
                for (int i = 0; i < nvis; i++) {
                    if (lx >= vis_links[i].x && lx < vis_links[i].x + vis_links[i].w &&
                        ly >= vis_links[i].y && ly < vis_links[i].y + vis_links[i].h) {
                        b_strcpy(nav_url, t->links[vis_links[i].link]);
                        nav_req = NAV_URL;
                        break;
                    }
                }
            }
        }
    }
}

/* Mouse move at client-local (lx, ly); buttons = PS/2 button mask.
 * Returns 1 when the client needs a redraw (scroll or hover). */
int browser_win_mouse_move(int lx, int ly, int buttons) {
    struct tab *t = &tabs[cur_tab];
    if (!(buttons & 1)) sb_drag = 0;
    if (sb_drag && (buttons & 1)) {
        int vp_h = content_bottom() - content_top();
        int rel = ly - content_top();
        if (rel < 0) rel = 0;
        if (rel > vp_h) rel = vp_h;
        if (vp_h > 0 && t->total_h > vp_h)
            t->scroll_y = (t->total_h - vp_h) * rel / vp_h;
        clamp_scroll(t);
        return 1;
    }
    if (!buttons) {
        int hov = -1;
        for (int i = 0; i < nvis; i++) {
            if (lx >= vis_links[i].x && lx < vis_links[i].x + vis_links[i].w &&
                ly >= vis_links[i].y && ly < vis_links[i].y + vis_links[i].h) {
                hov = vis_links[i].link;
                break;
            }
        }
        if (hov != hover_link) {
            hover_link = hov;
            return 1;
        }
    }
    return 0;
}

/* Shell `web` entry point: the browser is a desktop window now,
 * so this enters the GUI with the browser window open. */
void browser_run(const char *url_arg) {
    gui_run_browser(url_arg);
}
