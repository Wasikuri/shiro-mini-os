/* calc - interactive line calculator (user program, ring 3).
 *
 * The sample installable app for Shiro Mini OS v0.6.1: it is a
 * plain static ELF32 built with user.ld, exactly what `install`
 * downloads and `run` executes.
 *
 * Grammar (32-bit integer arithmetic):
 *   expr   := term   { ('+' | '-') term }
 *   term   := factor { ('*' | '/') factor }
 *   factor := number | '(' expr ')' | '-' factor | '+' factor
 * Spaces are ignored. Division truncates toward zero (C
 * semantics); division by zero and malformed input print an
 * error and the loop continues. Each result is printed as
 * "= <n>". An empty line exits (code 0).
 *
 * Kernel access is via int 0x80 only:
 *   eax=1 write(ebx=buf, ecx=len)   eax=2 read char -> eax
 *   eax=3 exit(ebx=code)
 * The asm MUST carry a "memory" clobber: without it GCC may
 * eliminate stores to a buffer that is only "read" by the kernel
 * (this silently broke stack-buffer writes — e.g. echoed input —
 * until v0.6.1; the kernel was never at fault).
 */
typedef unsigned int u32;
typedef int i32;

static int syscall1(int n, u32 a) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n), "b"(a) : "memory");
    return ret;
}
static int syscall2(int n, u32 a, u32 b) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b) : "memory");
    return ret;
}
static void write_str(const char *s) {
    u32 len = 0;
    while (s[len]) len++;
    if (len) syscall2(1, (u32)s, len);
}
static void write_num(i32 v) {
    char buf[12];
    int i = 0;
    u32 mag = v < 0 ? (u32)(-(v + 1)) + 1u : (u32)v; /* INT_MIN safe */
    if (v < 0) write_str("-");
    if (mag == 0) { write_str("0"); return; }
    while (mag) { buf[i++] = (char)('0' + (mag % 10)); mag /= 10; }
    while (i) {
        char e[2];
        e[0] = buf[--i]; e[1] = 0;
        write_str(e);
    }
}
static int read_char(void) { return syscall1(2, 0); }
static void sys_exit(int code) { syscall1(3, (u32)code); for (;;) { } }

/* Read one line with echo + Backspace editing. Returns its length. */
static int read_line(char *buf, int max) {
    int len = 0;
    for (;;) {
        int c = read_char();
        if (c == '\n' || c == '\r') {
            write_str("\n");
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) { len--; write_str("\b"); } /* kernel \b erases */
            continue;
        }
        if (c >= 32 && c < 127 && len < max - 1) {
            char e[2];
            buf[len++] = (char)c;
            e[0] = (char)c; e[1] = 0;
            write_str(e);
        }
    }
    buf[len] = 0;
    return len;
}

/* ---------- recursive-descent parser ---------- */
static const char *p;
static int perr; /* 0 ok, 1 syntax, 2 division by zero */

static void skip(void) { while (*p == ' ' || *p == '\t') p++; }

static i32 parse_expr(void);
static i32 parse_factor(void) {
    skip();
    if (*p == '(') {
        p++;
        i32 v = parse_expr();
        skip();
        if (*p == ')') p++; else perr = 1;
        return v;
    }
    if (*p == '-') { p++; return -parse_factor(); }
    if (*p == '+') { p++; return parse_factor(); }
    if (*p >= '0' && *p <= '9') {
        u32 v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (u32)(*p - '0'); p++; }
        return (i32)v;
    }
    perr = 1;
    return 0;
}
static i32 parse_term(void) {
    i32 v = parse_factor();
    for (;;) {
        skip();
        if (perr) return 0;
        if (*p == '*') {
            p++;
            v = v * parse_factor();
        } else if (*p == '/') {
            p++;
            i32 d = parse_factor();
            if (perr) return 0;
            if (d == 0) { perr = 2; return 0; }
            v = v / d;
        } else break;
    }
    return v;
}
static i32 parse_expr(void) {
    i32 v = parse_term();
    for (;;) {
        skip();
        if (perr) return 0;
        if (*p == '+') {
            p++;
            v = v + parse_term();
        } else if (*p == '-') {
            p++;
            v = v - parse_term();
        } else break;
    }
    return v;
}

void _start(void) {
    static char line[128];
    write_str("calc - integer calculator. Empty line quits.\n");
    for (;;) {
        write_str("calc> ");
        int n = read_line(line, (int)sizeof(line));
        if (n == 0) break; /* empty line: quit */
        p = line;
        perr = 0;
        i32 v = parse_expr();
        skip();
        if (!perr && *p != 0) perr = 1; /* trailing garbage */
        if (perr == 2) write_str("error: division by zero\n");
        else if (perr) write_str("error: syntax\n");
        else {
            write_str("= ");
            write_num(v);
            write_str("\n");
        }
    }
    sys_exit(0);
}
