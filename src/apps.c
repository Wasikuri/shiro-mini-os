/* Shiro Mini OS - installed applications (v0.6.1).
 *
 * `install <url> [name]` downloads a plain ELF32 executable over
 * http:// or https:// and stores it on SHFS under the file name
 * "app.<name>". The SHFS directory itself is the registry: there
 * is no separate registry file that could get out of sync, `apps`
 * simply lists every "app.*" file, and a crash mid-install can
 * never leave a stale entry (fs_stream_commit() is the single
 * moment an install becomes visible on disk).
 *
 * Rules:
 *  - download cap: 512 KB (streamed to disk in chunks through
 *    net_download(); never passes through the 64 KB page buffer)
 *  - duplicate installs are REJECTED: uninstall first, then
 *    install again (no silent overwrite of a running app store)
 *  - the payload must start with the ELF32 magic (checked from
 *    the first streamed bytes before commit)
 *  - apps run through the normal ELF loader in ring 3, with the
 *    same int 0x80 sandbox as every other user program
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "os.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int v, size_t n);

#define APP_PREFIX "app."
#define APP_PREFIX_LEN 4
#define APP_MAX_SIZE (512u * 1024u)
#define APP_NAME_MAX 23 /* 4 (prefix) + 23 = 27 = SHFS name limit */

/* ---------- small helpers ---------- */
static const char *next_token(const char *p, char *out, size_t max) {
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != ' ' && i < max - 1) out[i++] = *p++;
    out[i] = 0;
    return p;
}
static int name_valid(const char *name) {
    size_t n = 0;
    while (name[n]) {
        char c = name[n];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return 0;
        n++;
    }
    return n >= 1 && n <= APP_NAME_MAX;
}
static void app_filename(const char *name, char *out /* 28 */) {
    size_t i = 0;
    const char *pfx = APP_PREFIX;
    while (*pfx) out[i++] = *pfx++;
    while (*name && i < 27) out[i++] = *name++;
    out[i] = 0;
}
/* first token of `s` (app names never contain spaces) */
static void name_token(const char *s, char *out) {
    next_token(s, out, APP_NAME_MAX + 1);
}

/* ---------- install ---------- */
struct install_ctx {
    uint8_t head[16];   /* first bytes, for the ELF magic check */
    uint32_t head_len;
    uint32_t total;
    int err;            /* 0 ok, 1 too large, 2 disk write failed */
};

static int install_sink(const void *data, size_t len, void *vctx) {
    struct install_ctx *ctx = (struct install_ctx *)vctx;
    if (ctx->total + len > APP_MAX_SIZE) { ctx->err = 1; return 0; }
    if (ctx->head_len < sizeof(ctx->head)) {
        size_t take = sizeof(ctx->head) - ctx->head_len;
        if (take > len) take = len;
        memcpy(ctx->head + ctx->head_len, data, take);
        ctx->head_len += (uint32_t)take;
    }
    if (fs_stream_write(data, (uint32_t)len) != 0) { ctx->err = 2; return 0; }
    ctx->total += (uint32_t)len;
    return 1;
}

/* Derive the app name from a URL: last path component, minus any
 * query/fragment and a trailing ".elf". Result in `name`. */
static int derive_name(const char *url, char *name) {
    const char *base = url;
    for (const char *p = url; *p; p++)
        if (*p == '/') base = p + 1;
    size_t n = 0;
    while (base[n] && base[n] != '?' && base[n] != '#') n++;
    if (n > 4) { /* strip a trailing ".elf" */
        const char *suf = base + n - 4;
        if (suf[0] == '.' && suf[1] == 'e' && suf[2] == 'l' && suf[3] == 'f')
            n -= 4;
    }
    if (n == 0 || n > APP_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) name[i] = base[i];
    name[n] = 0;
    return 1;
}

void cmd_install(const char *args) {
    static char url[256];
    static char name[APP_NAME_MAX + 1];
    const char *rest = next_token(args, url, sizeof(url));
    next_token(rest, name, sizeof(name));
    if (url[0] == 0) { print("Usage: install <url> [name]\n"); return; }
    if (!fs_present()) { print("No disk.\n"); return; }
    if (name[0] == 0 && !derive_name(url, name)) {
        print("install: cannot derive an app name from the URL; pass one explicitly.\n");
        return;
    }
    if (!name_valid(name)) {
        print("install: bad app name (1-23 chars: A-Z a-z 0-9 - _ .).\n");
        return;
    }
    char fname[28];
    app_filename(name, fname);
    if (fs_file_size(fname) >= 0) {
        print("install: '"); print(name);
        print("' is already installed (uninstall it first).\n");
        return;
    }
    int brc = fs_stream_begin(fname, APP_MAX_SIZE);
    if (brc == -2) {
        print("install: '"); print(name);
        print("' is already installed (uninstall it first).\n");
        return;
    }
    if (brc != 0) {
        print("install: no free file slot or not enough contiguous disk space.\n");
        return;
    }
    struct install_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = net_download(url, install_sink, &ctx, NULL);
    if (rc != 1) {
        fs_stream_abort();
        if (rc == -1 && ctx.err == 1)
            print("install: file too large (max 512 KB).\n");
        else if (rc == -1 && ctx.err == 2)
            print("install: disk write failed.\n");
        else
            print("install: download failed.\n");
        return;
    }
    if (ctx.head_len < 5 || ctx.head[0] != 0x7F || ctx.head[1] != 'E' ||
        ctx.head[2] != 'L' || ctx.head[3] != 'F' || ctx.head[4] != 1) {
        fs_stream_abort();
        print("install: not a 32-bit ELF executable.\n");
        return;
    }
    if (fs_stream_commit() != 0) {
        print("install: could not save to disk.\n");
        return;
    }
    print("Installed "); print(name); print(" (");
    print_dec(ctx.total); print(" bytes).\n");
}

/* ---------- apps / run / uninstall ---------- */
void cmd_apps(void) {
    if (!fs_present()) { print("No disk.\n"); return; }
    char fname[28];
    int any = 0;
    for (int i = 0; fs_list_name(i, fname, sizeof(fname)); i++) {
        int is_app = 1;
        for (int j = 0; j < APP_PREFIX_LEN; j++)
            if (fname[j] != APP_PREFIX[j]) is_app = 0;
        if (!is_app) continue;
        any = 1;
        print("  "); print(fname + APP_PREFIX_LEN);
        print("  ("); print_dec((uint32_t)fs_file_size(fname));
        print(" bytes)\n");
    }
    if (!any) print("No apps installed.\n");
}

void cmd_run_app(const char *args) {
    char name[APP_NAME_MAX + 1];
    name_token(args, name);
    if (name[0] == 0) { print("Usage: run <name>\n"); return; }
    if (!fs_present()) { print("No disk.\n"); return; }
    char fname[28];
    app_filename(name, fname);
    int size = fs_file_size(fname);
    if (size < 0) {
        print("run: no app named '"); print(name);
        print("' (see 'apps').\n");
        return;
    }
    char *buf = kmalloc((size_t)size);
    if (!buf) { print("Out of memory.\n"); return; }
    int n = fs_read_file(fname, buf, (uint32_t)size);
    if (n < 0) {
        print("run: could not read the app file.\n");
        kfree(buf);
        return;
    }
    int rc = run_elf_image((const uint8_t *)buf, (uint32_t)n);
    print("Program exited with code "); print_dec((uint32_t)rc); print(".\n");
    kfree(buf);
}

void cmd_uninstall(const char *args) {
    char name[APP_NAME_MAX + 1];
    name_token(args, name);
    if (name[0] == 0) { print("Usage: uninstall <name>\n"); return; }
    if (!fs_present()) { print("No disk.\n"); return; }
    char fname[28];
    app_filename(name, fname);
    if (fs_file_size(fname) < 0) {
        print("uninstall: no app named '"); print(name); print("'.\n");
        return;
    }
    if (fs_remove_file(fname) != 0) {
        print("uninstall: could not remove '"); print(name); print("'.\n");
        return;
    }
    print("Uninstalled "); print(name); print(".\n");
}
