/* stb_image implementation translation unit (v0.9).
 *
 * stb_image v2.30 (public domain / MIT, see third_party/README.md)
 * compiled freestanding against the shim libc in src/shim (malloc
 * family from js_shims.c, libm from js_math.c). Only the memory
 * loader is used (STBI_NO_STDIO); PNG, JPEG and BMP are enabled.
 * All SIMD paths are off (the guest CPU has no SSE2 under the
 * harness configuration, same constraint as BearSSL).
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"
