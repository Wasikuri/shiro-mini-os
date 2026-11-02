# third_party — external code used by Shiro Mini OS

Third-party components, each with a per-occasion OSS exception
granted by the user: **BearSSL** for the TLS client (v0.6),
**Duktape** for JavaScript (browser milestone 1, integrated in
v0.7), and **stb_image** for wallpaper decoding (v0.9, under the
user's Phase 4 graphics-work exception). The wallpaper asset and
the embedded VGA font are documented at the end. Everything else
in the OS is written from scratch.

## stb_image

- **Version:** v2.30 (single-header library by Sean Barrett / nothings)
- **Source:** https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
  (fetched 2026-11-02 for v0.9)
- **SHA-256:** `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`
- **License:** dual-licensed public domain (Unlicense) / MIT — the
  full license text is in the header comment of the file itself.
  We use it under the public-domain dedication.
- **Contents here:** `stb/stb_image.h` (unmodified).
- **Integration:** compiled freestanding i386 in its own TU
  (`../src/stb_image.c`) with `STBI_NO_STDIO`, `STBI_NO_SIMD`
  (the harness CPU has no SSE2, same constraint as BearSSL) and
  the memory loader only (`stbi_load_from_memory`). It links
  against the existing shim libc/libm (`../src/js_shims.c`,
  `../src/js_math.c`, string functions from the kernel). Used by
  `gui.c` to decode the desktop wallpaper (JPEG; PNG/BMP come for
  free) — see the main README's GUI section.

## Wallpaper asset

- **File:** `../assets/wallpaper.jpg` (1024×640 JPEG, ~28 KB)
- **Origin:** synthesized on the host by
  `../tools/make_wallpaper.py` (Python + Pillow): a deep-teal
  base gradient with additive aurora curtains, a light pool at
  the bottom-left and a vignette — drawn to evoke the Windows
  Vista aurora wallpaper. Not a photograph and not Microsoft
  artwork; it is an original procedural image generated for this
  project, embedded into the kernel with `ld -b binary`.

## Embedded VGA font

- **File:** `../src/font8x16.h` (256 glyphs × 16 rows, 8×16)
- **Origin:** the standard IBM VGA text font as programmed by
  the SeaBIOS VGA BIOS — the same glyphs the BIOS boot path
  captures from the VGA font planes at runtime (captured once
  from a running guest and committed as data). It exists so a
  UEFI boot, where no BIOS ever loads a font, can install text
  mode 3 with glyphs identical to the BIOS path. This font is a
  decades-old de-facto standard bitmap font; no license is known
  to be asserted on these glyph bitmaps.

## Duktape

- **Version:** 2.7.0 (released 2022-04-06)
- **Source:** https://duktape.org/duktape-2.7.0.tar.xz (fetched
  2026-11-02 for the browser milestone-1 tree, merged into this
  tree for v0.7)
- **License:** MIT — see `duktape-2.7.0/LICENSE.txt`
- **Contents here:** `duktape-2.7.0/` pruned to the amalgamated
  sources under `src/` (`duktape.c`, `duktape.h`, `duk_config.h`)
  plus license/authors/readme; the release's tools, examples and
  tests were dropped.
- **Integration:** compiled freestanding i386 (`-O1`) against the
  shim headers in `../src/shim/`; the libc/libm surface it needs is
  implemented from scratch in `../src/js_shims.c`,
  `../src/js_math.c` and `../src/js_setjmp.s` (memcpy/memmove/
  memset and memcmp/strlen/strcmp/strncmp come from elsewhere in
  the kernel: `net.c` / `libc_shims.c`). Used by `jstest` and by
  the graphical browser (`web`) for `<script>` execution with a
  small DOM subset — see the main README's browser section.
- QuickJS 2025-04-26 was compile-probed during milestone 1 and
  found feasible as a later upgrade (66 external symbols, ~857 KB
  of objects, pthread stubs needed) but is **not** integrated; the
  probe tree remains in the milestone-1 copy
  (`~/workspace/simple-os-browser/third_party/`).

## BearSSL

- **Version:** 0.6
- **Source:** official tarball `bearssl-0.6.tar.gz` from
  https://bearssl.org/ (fetched 2026-11-02)
- **Tarball SHA-256:**
  `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`
  (matches the hash published on bearssl.org)
- **License:** MIT, Copyright (c) 2016 Thomas Pornin.
  Full text: `bearssl/LICENSE.txt`.
- **Contents here:** `bearssl/inc/`, `bearssl/src/`, `LICENSE.txt`,
  `README.txt` extracted from the tarball, unmodified.

### How it is built (Makefile, `BR_CFLAGS`)

- All of `bearssl/src/*/*.c` is compiled **except**
  `src/rand/sysrng.c`: that file implements `br_prng_seeder_system()`
  with OS calls (open/read on /dev/urandom etc.). Our replacement
  lives in `../src/tls.c` (kernel entropy pool: RDRAND if CPUID
  advertises it, PIT tick jitter, RTC, MAC, addresses — mixed with
  SHA-256 and fed to BearSSL's HMAC_DRBG). `src/settings.c` (used by
  the `brssl` tool only) is not compiled either.
- OS autodetection is pinned off:
  `-DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=0
  -DBR_USE_WIN32_TIME=0 -DBR_RDRAND=0`. Certificate validation time
  comes from the CMOS RTC via `br_x509_minimal_set_time()`.
- **x86 SIMD is pinned off:** `-DBR_SSE2=0 -DBR_AES_X86NI=0` and
  `-march=i686`. BearSSL's `inner.h` enables SSE2/AES-NI code for any
  x86 GCC, and the TLS engine then uses it without a runtime CPUID
  gate on some paths — on the target CPU (QEMU `qemu32`: no SSE2)
  that crashed the guest with #UD inside `br_chacha20_sse2_run`
  when a server negotiated ECDHE-ECDSA-CHACHA20-POLY1305. Only the
  portable C implementations are used now.
- Host libc headers are unusable for `-m32` freestanding builds in
  this environment, so minimal declaration-only headers live in
  `shim/` (`string.h`, `stdlib.h`, `stdio.h`, `limits.h`). The
  matching implementations are in `../src/net.c` (memcpy/memset/
  memmove) and `../src/libc_shims.c` (memcmp/strlen/strcmp/strncmp).
- BearSSL speaks TLS 1.0–1.2 (no TLS 1.3). We enable its full client
  profile (`br_ssl_client_init_full`), i.e. in practice TLS 1.2 with
  ECDHE + RSA/ECDSA and AEAD (AES-GCM / ChaCha20-Poly1305) suites.

## Trust anchors

Anchors are compiled into the kernel as data (`../src/tls_anchors.h`,
generated — do not edit). The PEM sources are kept here in `certs/`,
one file per certificate. Regenerate after adding/replacing a PEM
with either form (the no-argument form converts every `certs/*.pem`
in sorted order and is the reproducible default):

```sh
python3 third_party/make_anchors.py > src/tls_anchors.h
python3 third_party/make_anchors.py third_party/certs/*.pem > src/tls_anchors.h
```

Adding or refreshing a root is a data change only.

**Shipped set (v0.7.1): 120 anchors.**

- **118 roots from the Mozilla CA bundle** — the bulk of the set.
  Source: `https://curl.se/ca/cacert.pem` (the curl project's
  conversion of Mozilla's `certdata.txt`), certificate data as of
  **Tue Oct 20 13:32:40 2026 GMT**, converted with mk-ca-bundle.pl
  1.35. The bundle file is preserved verbatim at
  `mozilla-cacert.pem` (SHA-256 of the file as downloaded:
  `796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe`;
  SHA-256 of the source certdata recorded in its header:
  `beb7e6dfe6499926e52c075c27bcfbe4c957f8609c575b3860273ae2806f63eb`).
  It was split into one PEM per certificate in `certs/`, named by a
  slug of the bundle label (e.g. `certs/gts-root-r1.pem`). The full
  per-certificate list is simply the `certs/` directory. This set
  includes **GTS Root R1, R3 and R4**, so Google properties verify.
  Note: **GTS Root R2 is not in the current Mozilla bundle** (Mozilla
  removed it from the root store), so it is not shipped either.
- **2 pre-existing anchors**, kept alongside the bundle:

| PEM | Subject | SHA-256 fingerprint | Purpose |
|---|---|---|---|
| `certs/shiro-test-ca.pem` | CN=Shiro Test CA, O=Shiro Mini OS Tests | `7C:1D:B6:00:D0:39:3F:CA:B7:06:F0:01:38:6C:51:C7:4B:7C:C2:33:48:8D:21:61:38:8D:C6:2E:88:DB:95:58` | Locally generated test CA for the QEMU harness (see `../tests/tls/`). Not part of the Mozilla bundle. |
| `certs/sslcom-tls-ecc-root-ca-2022.pem` | C=US, O=SSL Corporation, CN=SSL.com TLS ECC Root CA 2022 | `C3:2F:FD:9F:46:F9:36:D1:6C:36:73:99:09:59:43:4B:9A:D6:0A:AF:BB:9E:7C:F3:36:54:F1:44:CC:1B:A1:43` | Real root terminating the chain presented for https://example.com from the guest's route (identified 2026-11-02 with `openssl s_client` to the address the guest actually reaches: leaf ← Cloudflare TLS Issuing ECC CA 3 ← SSL.com TLS Transit ECC CA R2 ← this root). Not in the Mozilla bundle, kept so example.com keeps verifying. |

To refresh the Mozilla set: download a fresh `cacert.pem` from the
URL above, replace `mozilla-cacert.pem`, re-split it into
`certs/*.pem` (one PEM block per file, named by label slug; the two
non-Mozilla files above must be kept), rerun `make_anchors.py`, and
rebuild. Sites whose chains end at roots outside this set still fail
with "TLS: certificate not trusted (unknown CA)."

## Test fixtures

`../tests/tls/fixtures/` holds the test CA certificate **and private
keys** plus server certificates used by the harness (good CA on
:8443, unrelated CA on :8444). They are throwaway test material,
generated locally with openssl, valid 2026–2036, and worthless
outside the test sandbox.
