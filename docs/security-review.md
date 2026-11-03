# Security review — red-team exercise and the v1.0.2 fix

Date: 2026-11-03. Scope: Shiro Mini OS v1.0.1, exercised in QEMU with
throwaway disk images (the project owner's own system, in an isolated
VM; nothing outside the test guests was touched). Three rounds were
run by attacker subagents under different rules, followed by a fix
stage. This document is the honest record: what was tested, what
broke, why, what was changed, and how the fix was verified.

## Round 1 — black-box, web surface (12 rounds, no break)

The attacker got only the ISO and the surfaces a remote web attacker
would have: it ran its own HTTP server and had a driver open its
pages in the Tabby browser. Payload classes: stalled documents and
images, redirect loops, a 280 KB / 20,000-element page, 5,000-deep
element nesting, PNG/JPEG headers claiming 60000×60000, extreme CSS
values (10⁹ px boxes, −2×10⁹ margins), HTML entity edge cases (NUL,
surrogates, >U+10FFFF, 10²⁰ numeric references, 5,000-char entity
names), and form overload (5,000 options, 100 KB input value,
500 inputs).

**Result: no crash, panic, reboot, or UI hang.** The browser/OS
handled every class (document timeout ≲ 20 s, redirect cap, page-size
truncation with an on-screen notice, bogus image dimensions rejected,
CSS values clamped). One low-severity finding, still open and tracked
separately in NEXT-STEPS.md: images load strictly sequentially with
an effective ~10 s timeout per stalled image, so a hostile page with
N dead images delays full rendering by ~10 s × N (≈160 s at the
16-image cap) while the desktop stays responsive. Not exercised in
round 1: lying Content-Length, drip-feed responses, chunked
encoding, JavaScript fuzzing, TLS attacks.

Lesson recorded at the time: "not found" is not "not there". The
hole that round 2 found existed all along; it was simply not
reachable or observable from the web-only position.

## Round 2 — gray-box audit: confirmed privilege escalation

Rules changed at the owner's request ("use the method that can
reliably break it"): the attacker could read the source and audit
the ring-3 boundary — the ELF loader, the `int 0x80` handlers, and
the SHFS mount parser.

### The vulnerability — ELF loader integer wraparound (`src/exec.c`)

`run_elf_image()` validated each PT_LOAD segment like this (v1.0.1):

```c
uint32_t first = ph->vaddr & ~0xFFFu;
uint32_t last  = (ph->vaddr + ph->memsz - 1) & ~0xFFFu;
if (first < 0x100000u || last >= USER_STACK_TOP) reject;
if (first < kend && last >= 0x100000u) reject;   /* kernel overlap */
if (ph->offset + ph->filesz > len || ph->filesz > ph->memsz) reject;
for (va = first; va <= last; va += 4096) map_page(va);
/* then, unconditionally: */
for (b = 0; b < ph->filesz; b++) dst[b] = src[b];  /* dst = ph->vaddr */
```

**Root cause:** the segment end `vaddr + memsz - 1` was computed in
32-bit arithmetic. With a crafted `p_memsz`, the sum wraps, so
`last` lands *below* the kernel while `first` (the raw `p_vaddr`)
sits inside the kernel image. Every check then passes vacuously:
the window check sees `last < USER_STACK_TOP`, the overlap check
requires `last >= 0x100000` which the wrapped value fails, and the
page-mapping loop body never executes because `last < first`. But
the byte-copy loop that follows is **not** conditioned on any of
that: it copies `p_filesz` bytes to the raw `p_vaddr` with kernel
privilege. Contributing factors: the address space is an RW identity
map and the CPU features that would mitigate this (SMEP/SMAP) are
not in play, so kernel `.text` is writable through this primitive.

In short: a validation routine existed and looked plausible, but
one arithmetic overflow silently voided all of it — an
implementation bug, not a design flaw.

### The proof of concept

An 8,232-byte ELF with two PT_LOAD segments:

| Seg | vaddr | filesz | memsz | Purpose |
|-----|-------|--------|-------|---------|
| A | 0x400000 | 37 | 37 | Ordinary ring-3 trigger code; its first action is `int 0x80`. |
| B | 0x0010C110 (`syscall_handler`) | 40 | 0xFFF00000 | Wrap segment: computed `last` = 0x0000C000, so all checks pass and the copy loop writes the 40-byte payload over `syscall_handler`. |

Delivered through the normal app path — served over HTTP, then
`install http://…/pwn` and `run pwn`. On the trigger program's first
`int 0x80`, control entered the patched handler: the payload, now
running in **ring 0**, wrote `PWNED BY RING3` directly into VGA text
memory at 0xB8000 (an address ring-3 code cannot touch) and halted
the CPU (`cli; hlt`). Screenshots showed the marker overwriting the
boot banner and the uptime counter frozen; the guest never
recovered.

### Secondary issue — unchecked syscall pointer

The `int 0x80` write handler (case 1) dereferenced the user-supplied
buffer pointer with no validation (only the length was clamped to
8192). Pointing it at kernel memory discloses that memory to the
screen; pointing it at an unmapped address faults in ring 0. A
kernel-memory disclosure primitive — weaker than the loader bug, but
the same class of mistake: trusting a ring-3 value at ring 0.

## Round 3 — one-shot demonstration: durable disk change

Same loader hole, extended 48-byte payload, still one trigger and
a halt — no hooks, no resident code. On the first `int 0x80` the payload additionally **called the
kernel's own `fs_write_file`** (the function behind the shell's
`write` command) and planted `PWNED.txt` in SHFS, then painted a
4-row white-on-red banner into VGA memory and halted. Verified from
a fresh boot off the same disk: `ls` lists `PWNED.txt (104 bytes)`
and `cat` prints the planted text. So beyond code execution, the
round-2 hole also yields a **durable, unauthorized filesystem
change**: a ring-3 app whose entire legitimate API is
write/read-char/exit caused the kernel to create a file on its
behalf.

### What this meant for v1.0.1, stated plainly

On v1.0.1, `install` + `run` of an untrusted ELF was equivalent to
handing over the kernel. A malicious app could read/write all
memory and disk, operate the hardware directly, monitor everything
the user types or views (all input, display, filesystem and network
handling lives in the kernel), and persist changes on disk. The
only boundary it did not cross is the hypervisor's: this was all
inside the guest. Any claim that installed apps were "isolated" on
v1.0.1 was void.

## The fix (v1.0.2, `src/exec.c`)

**Loader — restructured into validate-then-load:**

- The program-header table itself is bounds-checked against the
  image length (64-bit), and `phentsize` must fit a real phdr.
- The entry point must lie inside the user window and clear of the
  kernel image.
- **Pass 1 validates every PT_LOAD segment before anything is
  mapped or copied.** For each segment, the end is computed in
  64-bit (`vend = (uint64_t)vaddr + memsz`, exclusive); the segment
  is rejected if `vaddr` is below the user window (1 MB), if
  `vend` exceeds the window top (8 MB), if `vaddr` intersects the
  kernel image `[0x100000, kend)`, if `filesz > memsz`, or if
  `[offset, offset+filesz)` leaves the image (64-bit check — the
  old 32-bit `offset + filesz` could wrap too). Any rejection
  rejects the whole ELF.
- **Pass 2** maps the pages and copies using only the validated
  segment list and the same bounds.

**Syscalls — user-pointer validation:**

- A helper `user_range_ok(ptr, len)` proves, in 64-bit arithmetic,
  that `[ptr, ptr+len)` lies within `[kend, USER_STACK_TOP)` — the
  memory a ring-3 program can legitimately own (its segments and
  stack). The write syscall (case 1) returns −1 without touching
  memory when the check fails. Cases 2 (read char) and 3 (exit)
  take no pointers; the default case already returned −1.

## Verification

**New regression checks** in the `apps` harness group
(`tests/qemu_test.py`; fixtures generated in Python at harness
start, served by the local test server):

1. **Wraparound ELF rejected** — a crafted image with a normal
   segment at 0x400000 plus a wrapping segment aimed into the
   kernel image (vaddr 0x110000, memsz 0xFF000000 → wrapped end
   0xF000), independent of exact kernel symbol addresses.
   `install` + `run` must end with the loader's rejection message,
   and a follow-up `version` command must still answer (shell
   alive). On v1.0.1 this image was accepted.
2. **Syscall pointer refused** — a tiny user app calls
   `write(0x100000, 64)` (kernel memory) and reports the outcome
   with its own strings: it must print `BLOCKED` (and not
   `ALLOWED`, which v1.0.1 printed after dumping kernel bytes).

**Full harness, serialized, on the shipped v1.0.2 ISO** (fresh
`tests/work` per group): default 22/22, fs 8/8, gui 23/23,
tls 4/4, apps 13/13 (the 10 pre-existing checks + 3 new reports
covering the two fixtures above), jstest 1/1, web 40/40, uefi 5/5. Legitimate apps (calc over HTTP and HTTPS,
the embedded hello) install and run unchanged.

One environmental note: the first full-suite run showed uefi
failing because this VM had lost the `grub-efi-amd64-bin` package
in a replacement, so `grub-mkrescue` had silently produced a
BIOS-only ISO (6,213,632 B, no UEFI El Torito image). After
restoring that package and rebuilding, the hybrid ISO
(12,965,888 B) passes uefi 5/5. Unrelated to the fix, recorded
here so the failure is not mistaken for a regression later.

## Reproducing the original bugs

The PoC builder scripts are deliberately not published with this
repository — they are kept in the author's local archive as a
safety choice. The mechanism, segment layout, and trigger values
are fully described above, so the bug remains reproducible in
principle from this writeup alone; the builders targeted v1.0.1
symbol addresses and are of historical/regression interest only —
v1.0.2 rejects both images at load time. The harness fixtures in
`tests/` cover the same bugs without depending on exact kernel
addresses.

## Remaining posture (unchanged honesty)

This fix closes the confirmed hole; it does not make the OS
"secure" in any general sense. There are still no logins/passwords,
no filesystem permissions, no disk encryption, no ASLR or stack
canaries; the hand-written parsers (HTML/CSS, network, SHFS) remain
largely unfuzzed, and the round-1 low-severity image-latency issue
is still open. The correct mental model stays: this is a hobby OS
whose trust boundaries are now at least enforced where they are
claimed — nothing more.
