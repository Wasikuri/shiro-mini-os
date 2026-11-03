# Next steps — instructions for the following agent

For the next agent working on this repository: **read `AGENTS.md` first** (build/test commands, harness traps, source map, development policy). The items below are pre-approved feature/fix instructions from the project owner, in priority order. Work one item at a time; for each, extend the QEMU harness with checks that prove the behavior, run the full affected groups, and update `README.md` / `AGENTS.md` status notes when done.

Development policy reminder: build from scratch by default. OSS exceptions already granted by the owner are stage-scoped (BearSSL for TLS, Duktape for JS, stb_image for images, Mozilla CA bundle). Do not add new external libraries without asking the owner.

---

### P0 (security) — ELF loader integer wraparound → ring-0 code execution (CONFIRMED EXPLOIT) — ✅ DONE in v1.0.2 (2026-11-03)

**Outcome:** loader segment ranges are now computed in 64-bit and every segment is validated before anything is mapped or copied; the `int 0x80` write syscall validates its pointer against the user window. The round-2/round-3 PoCs are rejected at load, regression checks are in the `apps` harness group, and the full write-up is in `docs/security-review.md`.

**Problem:** red-team round 2 (2026-11-03) confirmed a full privilege escalation. In `src/exec.c` (`run_elf_image`), the segment end check `last = (vaddr + memsz - 1) & ~0xFFF` is computed in 32-bit arithmetic and can wrap below the kernel boundary, so a crafted ELF segment passes all range/overlap checks while the subsequent copy loop still writes `p_filesz` bytes to the raw `p_vaddr` in ring 0. The PoC overwrote `syscall_handler` itself; on the next `int 0x80` the payload ran at ring 0 (wrote a marker to VGA memory 0xB8000, then halted the machine). Full repro: `~/workspace/redteam/REPORT2.md` in the author's environment (PoC builder `make_pwn.py`).
**Do:** (1) compute segment ranges in 64-bit (or check `memsz`/`vaddr` overflow explicitly) and reject any segment whose [vaddr, vaddr+memsz) intersects [0, USER_START) or wraps; (2) validate every segment against the user window BEFORE copying anything, and make the copy loop use the same validated bounds; (3) also fix the secondary issue found in the same audit: the `int 0x80` write syscall dereferences the user pointer with no validation — check pointer+length against the user window (and clamp) before use.
**Acceptance:** new harness checks — the round-2 PoC ELF (rebuildable from the report) is rejected with a clean load error and the shell stays alive; legitimate apps (`calc`, installed sample) still install and run; syscall write with an out-of-window pointer fails cleanly instead of dumping kernel memory.

### P0 — Image loading latency in the Tabby browser (bug fix)

**Problem:** page images are fetched strictly sequentially, and the page is only fully rendered after every image resolves or times out. A black-box red-team exercise (2026-11-03, 12 rounds — see below) measured an effective ~10 s timeout per stalled image: a hostile or merely broken page with N dead images delays completion by ~10 s × N (≈160 s at the 16-image cap; this is also why image-heavy real pages such as Wikipedia appear to hang — the load is finite but very slow). The desktop itself stays responsive.
**Do:** shorten the per-image timeout (~3 s), and render the page text/layout before all images settle (fill in images as they arrive, alt text on failure). Keep the existing per-image caps.
**Acceptance:** new `web` harness checks — a page with several silent-host images reaches full text rendering within a few seconds, with alt text in place of the dead images. Existing baselines stay green (web 40, default 22, gui 23).

**Red-team note (2026-11-03):** an external-attacker subagent ran 12 black-box rounds against v1.0.1 (stalled document/images, redirect loop, 280 KB/20k-element page, 5000-deep nesting, image headers claiming 60000×60000, extreme CSS values, entity edge cases, form overload). No crash, panic, reboot, or UI hang was achieved; the only confirmed issue is the image-latency item above. Not yet exercised: lying Content-Length, drip-feed responses, chunked-encoding tricks, JS fuzzing, TLS attacks — Content-Length mismatch is the least-explored surface and a good next menu.

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

---

### Round-4 follow-ups (2026-11-03 audit of the patched build — no reachable break found)

Red-team round 4 re-ran the round-2/3 PoCs against the merged patched build (cleanly rejected) and audited the new code line by line (`~/workspace/redteam/REPORT4.md` in the author's environment). The v1.0.2 boundary holds. Three notes for future work:

- **F3 (small fix):** stylesheet fetches have no deadline — a stalled/dripping CSS host can hold a page load up to the generic 120 s cap, the same class as the fixed image issue. Apply the same fetch deadline used for images to `<link>` stylesheet loads.
- **F1 (hardening note):** `user_range_ok` in exec.c proves a syscall range lies inside `[kend, 0x800000)` but not that it belongs to the calling program. Inert today only because of the memory layout (the gap is unused scratch RAM; the heap starts at the exclusive top). If a live kernel buffer is ever placed in `[kend, 8 MB)`, this becomes a disclosure — tighten the check to the caller's mapped segments/stack if that layout ever changes.
- **F2 (crafted-disk surface, pre-existing):** fs.c trusts on-disk directory entries; a maliciously crafted disk image whose entries lie about `alloc_sectors` can drive an out-of-bounds bitmap write on delete/overwrite. Not reachable from apps or the web (all in-OS writers keep entries consistent) — relevant only if untrusted disk images are ever mounted. Validate directory-entry sector counts against the disk size at mount.
