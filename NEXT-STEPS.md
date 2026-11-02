# Next steps — instructions for the following agent

For the next agent working on this repository: **read `AGENTS.md` first** (build/test commands, harness traps, source map, development policy). The items below are pre-approved feature/fix instructions from the project owner, in priority order. Work one item at a time; for each, extend the QEMU harness with checks that prove the behavior, run the full affected groups, and update `README.md` / `AGENTS.md` status notes when done.

Development policy reminder: build from scratch by default. OSS exceptions already granted by the owner are stage-scoped (BearSSL for TLS, Duktape for JS, stb_image for images, Mozilla CA bundle). Do not add new external libraries without asking the owner.

---

### P0 — Per-image fetch timeout in the Tabby browser (bug fix)

**Problem:** page images are fetched sequentially during load. If an image host stalls (observed: `upload.wikimedia.org` on some networks), the whole page load hangs indefinitely.
**Do:** give each image fetch its own short timeout (connect + read, ~3–5 s total per image). On timeout/failure, render the existing alt-text fallback and continue loading the rest of the page. Page load must always terminate.
**Acceptance:** new `web` harness checks — a page whose image points at a silent host still finishes loading, shows the alt text, and the browser stays responsive. Existing baselines stay green (web 40, default 22, gui 23).

### P1 — Decode common HTML entities in text rendering (bug fix)

**Problem:** named entities render literally — e.g. the Google homepage footer shows the raw text `&copy;`.
**Do:** decode the common set in text nodes: `&amp; &lt; &gt; &quot; &apos; &nbsp; &copy;` and numeric references `&#NNN;` / `&#xHH;` (map to the bitmap font's available glyphs; use a placeholder only when no glyph exists).
**Acceptance:** harness check with a fixture page containing entities renders the decoded characters.

### P1 — True GOP framebuffer under UEFI via a custom EFI stub loader

**Problem:** GRUB's EFI multiboot2 image emits no framebuffer tag, so under UEFI the kernel falls back to the VBE path and the GOP backend (already implemented, see `mb2_gop` in the source) never activates.
**Do:** write a minimal EFI loader application (BOOTX64.EFI) that obtains the GOP framebuffer, loads the kernel ELF itself, passes the framebuffer info through the existing multiboot2 seam, and jumps to the kernel. Keep the GRUB/BIOS path untouched.
**Acceptance:** under OVMF, the `uefi` harness group passes AND `gfxstats` / the Info window report the GOP pitch; BIOS boot unchanged.

### P2 — In-OS HTTP server demo

**Do:** add TCP listen/accept to the network stack and an `httpd` shell command that serves a fixed page (or a file from SHFS) on port 80. Demonstrable from the host through QEMU user-net host forwarding.
**Acceptance:** new harness check — while `httpd` runs in the guest, an HTTP GET from the host side returns the served page; stopping the server restores the shell.

### P2 — Persist the browser cookie jar

**Do:** save the in-memory cookie jar to SHFS when the GUI exits and reload it when the browser starts. Keep the existing jar limits (8 hosts × 4 pairs).
**Acceptance:** harness check — a cookie set in one boot is still sent after a reboot.

### P3 — Screenshots for the README

**Do:** regenerate the standard screenshot set by running the OS in QEMU (desktop, browser on a styled page, browser on Google), save PNGs under `docs/images/`, and link them from `README.md`.
**Note:** screenshots are binary files. The snapshot-based upload flow used for the initial publication is text-only, so commit images through a normal `git push` from a full clone instead.

---

Current verified baseline (v1.0.1, 2026-11-03): default 22/22, fs 8/8, gui 23/23, tls 4/4, apps 10/10, jstest 1/1, web 40/40, uefi 5/5. A text-only checkout rebuilds the hybrid ISO; see `AGENTS.md` for the toolchain (incl. `grub-efi-amd64-bin`).
