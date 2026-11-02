/* jstest.c - Duktape proof-of-concept for Shiro Mini OS.
 *
 * Shell command `jstest`: creates a Duktape heap on the kernel
 * allocator (via the js_shims wrappers), evaluates a snippet that
 * exercises arithmetic, string concatenation, a recursive function
 * and Array.prototype.join, and prints the result prefixed with
 * "JSRESULT " so the QEMU harness can assert it from the text screen.
 *
 * Duktape 2.7.0 (MIT) lives in third_party/duktape-2.7.0; the libc
 * surface it needs is provided by src/js_shims.c + src/js_math.c +
 * src/js_setjmp.s (see src/shim/*.h for the headers it compiled
 * against).
 */
#include "os.h"
#include <stddef.h>
#include "duktape.h"

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
static void js_free(void *udata, void *ptr) {
    (void)udata;
    free(ptr);
}
static void js_fatal(void *udata, const char *msg) {
    (void)udata;
    print("jstest: Duktape fatal error: ");
    print(msg ? msg : "(no message)");
    print("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

static const char JSTEST_SRC[] =
    "function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2); }\n"
    "var s = 'shiro' + '-' + 'js';\n"
    "[s, 1 + 2 * 3, fib(15), 'x'.length + s.length].join('|')";

void cmd_jstest(void) {
    duk_context *ctx = duk_create_heap(js_alloc, js_realloc, js_free,
                                       NULL, js_fatal);
    if (!ctx) {
        print("jstest: duk_create_heap failed\n");
        return;
    }
    print("jstest: evaluating with Duktape...\n");
    if (duk_peval_string(ctx, JSTEST_SRC) == DUK_EXEC_SUCCESS) {
        print("JSRESULT ");
        print(duk_safe_to_string(ctx, -1));
        print("\n");
    } else {
        print("JSERROR ");
        print(duk_safe_to_string(ctx, -1));
        print("\n");
    }
    duk_pop(ctx);
    duk_destroy_heap(ctx);
}
