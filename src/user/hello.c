/* Sample user program for Shiro Mini OS (ring 3).
 * Freestanding; talks to the kernel via int 0x80:
 *   eax=1 write(ebx=buf, ecx=len)
 *   eax=2 read char -> eax
 *   eax=3 exit(ebx=code)
 * The "memory" clobber on the asm is load-bearing: without it GCC
 * may eliminate stores to buffers only the kernel reads (the echo
 * below silently printed NULs until this was added in v0.6.1).
 */
typedef unsigned int u32;

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
    syscall2(1, (u32)s, len);
}
static int read_char(void) { return syscall1(2, 0); }
static void sys_exit(int code) { syscall1(3, (u32)code); for (;;) { } }

void _start(void) {
    write_str("Hello from user mode (ring 3)!\n");
    write_str("Type 3 characters, I will echo them: ");
    for (int i = 0; i < 3; i++) {
        int c = read_char();
        char buf[2];
        buf[0] = (char)c; buf[1] = 0;
        write_str(buf);
    }
    write_str("\nUser program done, exiting.\n");
    sys_exit(3); /* exit code = number of echoed chars */
}
