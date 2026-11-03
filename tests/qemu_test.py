#!/usr/bin/env python3
"""QEMU test harness for Shiro Mini OS.

Boots the ISO in QEMU (TCG) with an e1000 user-network (plus a
guestfwd DNS-over-TCP helper on 10.0.2.100:8053, needed because this
sandbox blocks QEMU's outbound UDP), types shell commands through the
QEMU monitor (sendkey) and reads the guest screen from VGA text memory
(xp /2000hx 0xb8000).

Usage: python3 tests/qemu_test.py [check ...]
With no arguments it runs every check. A check may reboot the VM
(fs persistence runs its own two boots).
"""
import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ISO = os.path.join(ROOT, "shiro-os.iso")
WORK = os.path.join(HERE, "work")
DISK = os.path.join(WORK, "disk.img")
SOCK = os.path.join(WORK, "qemu-mon.sock")

RESULTS = []


def report(name, ok, detail=""):
    RESULTS.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" -- {detail}" if detail and not ok else ""))


class Qemu:
    def __init__(self, fresh_disk=False):
        os.makedirs(WORK, exist_ok=True)
        if fresh_disk or not os.path.exists(DISK):
            with open(DISK, "wb") as f:
                f.truncate(8 * 1024 * 1024)
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        netdev = ("user,id=net0,"
                  f"guestfwd=tcp:10.0.2.100:8053-cmd:{os.path.join(HERE, 'dnsfwd-tcp')}")
        self.proc = subprocess.Popen([
            "qemu-system-i386", "-cdrom", ISO, "-boot", "d", "-m", "128",
            "-display", "none", "-vga", "std",
            "-drive", f"file={DISK},format=raw,if=ide,index=0",
            "-device", "e1000,netdev=net0", "-netdev", netdev,
            "-monitor", f"unix:{SOCK},server,nowait",
            "-machine", "accel=tcg",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(SOCK):
                break
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU exited during startup")
            time.sleep(0.1)

    def mon(self, cmd, wait=0.25):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                s.connect(SOCK)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        else:
            raise RuntimeError("cannot connect to QEMU monitor")
        s.settimeout(3)
        try:
            s.recv(4096)
        except socket.timeout:
            pass
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
        out = b""
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                out += d
                if out.rstrip().endswith(b"(qemu)"):
                    break
        except socket.timeout:
            pass
        s.close()
        return out.decode(errors="replace")

    def screen(self):
        out = self.mon("xp /2000hx 0xb8000", wait=0.6)
        vals = []
        for line in out.splitlines():
            m = re.match(r"\s*[0-9a-fx]+:\s*(.*)", line)
            if m:
                vals += re.findall(r"0x([0-9a-fA-F]+)", m.group(1))
        chars = []
        for v in vals[:2000]:
            c = int(v, 16) & 0xFF
            chars.append(chr(c) if 32 <= c < 127 else " ")
        return "\n".join(
            "".join(chars[i * 80:(i + 1) * 80]).rstrip() for i in range(25))

    def wait_for(self, marker, timeout=30):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if marker in self.screen():
                return True
            time.sleep(1)
        return marker in self.screen()

    KEYMAP = {'.': 'dot', '/': 'slash', '-': 'minus', ':': 'shift-semicolon',
              '_': 'shift-minus', ' ': 'spc', '>': 'shift-dot', '<': 'shift-comma',
              '(': 'shift-9', ')': 'shift-0', '+': 'shift-equal', '*': 'shift-8'}

    def sendkey(self, key):
        self.mon(f"sendkey {key}", wait=0.13)

    def screendump(self, name="frame.ppm"):
        """QEMU monitor screendump -> (width, height, rgb_bytes)."""
        path = os.path.join(WORK, name)
        if os.path.exists(path):
            os.unlink(path)
        self.mon("screendump " + path, wait=0.8)
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) > 100:
                break
            time.sleep(0.2)
        return parse_ppm(path)

    def mouse_move(self, x, y):
        self.mon("mouse_move %d %d" % (int(x), int(y)), wait=0.15)

    def mouse_button(self, state):
        self.mon("mouse_button %d" % state, wait=0.25)

    def type_text(self, text):
        for ch in text:
            if ch.isupper():
                self.sendkey(f"shift-{ch.lower()}")
            else:
                self.sendkey(self.KEYMAP.get(ch, ch))

    def cmd(self, text, marker=None, timeout=30):
        """Type a shell command + Enter; optionally wait for a marker."""
        self.type_text(text)
        self.sendkey("ret")
        if marker:
            return self.wait_for(marker, timeout)
        time.sleep(1.5)
        return True

    def quit(self):
        try:
            self.mon("quit", wait=0.2)
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def parse_ppm(path):
    data = open(path, "rb").read()
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] not in b"\r\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1  # the single whitespace byte after maxval
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def vga_rgb(c6):
    """6-bit VGA DAC triple -> 8-bit RGB as in QEMU screendumps."""
    return tuple((c << 2) | (c >> 4) for c in c6)


def count_close(pix, w, h, rgb, tol=12, region=None):
    """Count pixels close to rgb; region=(x0,y0,x1,y1) in image pixels."""
    x0, y0, x1, y1 = region or (0, 0, w, h)
    x0 = max(0, min(x0, w)); x1 = max(0, min(x1, w))
    y0 = max(0, min(y0, h)); y1 = max(0, min(y1, h))
    n = 0
    for y in range(y0, y1):
        base = y * w * 3
        for x in range(x0, x1):
            o = base + x * 3
            if (abs(pix[o] - rgb[0]) <= tol and abs(pix[o + 1] - rgb[1]) <= tol
                    and abs(pix[o + 2] - rgb[2]) <= tol):
                n += 1
    return n


def centroid_close(pix, w, h, rgb, tol=12):
    sx = sy = n = 0
    for y in range(h):
        base = y * w * 3
        for x in range(w):
            o = base + x * 3
            if (abs(pix[o] - rgb[0]) <= tol and abs(pix[o + 1] - rgb[1]) <= tol
                    and abs(pix[o + 2] - rgb[2]) <= tol):
                sx += x
                sy += y
                n += 1
    if n == 0:
        return None
    return (sx / n, sy / n, n)


def region_diff(a, b, w, region):
    x0, y0, x1, y1 = region
    n = 0
    for y in range(y0, y1):
        base = y * w * 3
        for x in range(x0, x1):
            o = base + x * 3
            if a[o] != b[o] or a[o + 1] != b[o + 1] or a[o + 2] != b[o + 2]:
                n += 1
    return n


def kill_stray_qemu():
    """Kill QEMU processes left over from a crashed harness run."""
    import signal
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == os.getpid():
            continue
        try:
            with open(f"/proc/{pid}/comm") as f:
                if f.read().strip().startswith("qemu-system"):
                    os.kill(int(pid), signal.SIGKILL)
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass


class QemuUefi(Qemu):
    """Same rig, but the hybrid ISO is booted under OVMF (UEFI):
    GRUB loads via the EFI El Torito image and starts the kernel
    with Multiboot2, so the GUI must come up on the GOP
    framebuffer instead of a BIOS VBE mode."""

    def __init__(self, fresh_disk=False):
        os.makedirs(WORK, exist_ok=True)
        if fresh_disk or not os.path.exists(DISK):
            with open(DISK, "wb") as f:
                f.truncate(8 * 1024 * 1024)
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        netdev = ("user,id=net0,"
                  f"guestfwd=tcp:10.0.2.100:8053-cmd:{os.path.join(HERE, 'dnsfwd-tcp')}")
        self.proc = subprocess.Popen([
            # OVMF (Ubuntu's `ovmf` package) is X64 firmware: it
            # triple-faults under qemu-system-i386 (no long mode),
            # so the UEFI rig must use the x86_64 emulator. The
            # 32-bit kernel itself is loaded by GRUB as Multiboot2
            # and runs in protected mode either way.
            "qemu-system-x86_64", "-cdrom", ISO, "-boot", "d", "-m", "128",
            "-display", "none", "-vga", "std",
            "-bios", "/usr/share/qemu/OVMF.fd",
            "-drive", f"file={DISK},format=raw,if=ide,index=0",
            "-device", "e1000,netdev=net0", "-netdev", netdev,
            "-monitor", f"unix:{SOCK},server,nowait",
            "-machine", "accel=tcg",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(SOCK):
                break
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU exited during startup")
            time.sleep(0.1)


def boot(fresh_disk=False, marker="Type 'help'"):
    kill_stray_qemu()
    time.sleep(0.5)
    q = Qemu(fresh_disk=fresh_disk)
    if not q.wait_for(marker, timeout=60):
        print("=== screen at boot timeout ===")
        print(q.screen())
        raise RuntimeError("guest did not reach the shell prompt")
    q.boot_screen = q.screen()  # snapshot before later output scrolls it away
    return q


# ---------------- checks ----------------

def check_boot_uptime_mem(q):
    scr = q.screen()
    report("boot: v1.0.2 banner", "Shiro Mini OS v1.0.2" in scr)
    q.cmd("uptime", marker="Uptime:")
    m1 = re.search(r"Uptime: (\d+) s", q.screen())
    time.sleep(3)
    q.cmd("uptime", marker="Uptime:")
    # the newest uptime line is the last one on screen
    vals = re.findall(r"Uptime: (\d+) s", q.screen())
    ok = False
    if vals and len(vals) >= 2:
        ok = int(vals[-1]) > int(vals[0])
    elif m1 and vals:
        ok = int(vals[-1]) > int(m1.group(1))
    report("uptime advances", ok, f"values={vals}")
    q.cmd("mem", marker="Heap:")
    scr = q.screen()
    mf = re.search(r"Frames: total (\d+) used (\d+) free (\d+)", scr)
    ok = bool(mf) and int(mf.group(1)) > 10000 and int(mf.group(3)) > 0 \
        and "Heap:" in scr and "RAM total" in scr
    report("mem output sane", ok, scr[-400:] if not ok else "")


def check_help(q):
    q.cmd("help", marker="poweroff")
    report("help lists commands", "browse <url>" in q.screen())


def check_ps_counter(q):
    c1 = q.screen().split("\n")[0][60:]
    time.sleep(3)
    c2 = q.screen().split("\n")[0][60:]
    report("corner counter advances", c1 != c2 and c1.strip() != "",
           f"c1={c1!r} c2={c2!r}")
    q.cmd("ps", marker="shell")
    scr = q.screen()
    report("ps lists tasks", "counter" in scr and "shell" in scr, scr[-500:])
    q.cmd("help", marker="poweroff")
    # Assert on the last help line: the help text grew by a line in
    # v0.5 (the `res` entry), so the first line ("Commands:") has
    # scrolled off the 25-line screen by the time the output settles.
    report("shell responsive during tasks", "poweroff" in q.screen())


def check_date(q):
    import datetime
    q.cmd("date", marker="UTC")
    m = re.search(r"(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})",
                  q.screen())
    ok = False
    detail = "no date on screen"
    if m:
        y, mo, d, hh, mm, ss = (int(g) for g in m.groups())
        try:
            guest = datetime.datetime(y, mo, d, hh, mm, ss,
                                      tzinfo=datetime.timezone.utc)
            host = datetime.datetime.now(datetime.timezone.utc)
            diff = abs((host - guest).total_seconds())
            ok = diff < 300
            detail = f"guest={guest} host={host} diff={diff:.0f}s"
        except ValueError:
            detail = f"bad fields {m.groups()}"
    report("date: RTC date/time sane and close to host UTC", ok, detail)


def check_fs(q):
    q.cmd("ls", marker=None)
    q.cmd("write hello.txt Hello persistence world", marker="Written.")
    q.cmd("cat hello.txt", marker="Hello persistence world")
    report("fs: write + cat", "Hello persistence world" in q.screen())
    q.cmd("ls", marker="hello.txt")
    report("fs: ls shows file", "hello.txt" in q.screen())
    # cp / mv (v0.5.1)
    q.cmd("cp hello.txt copy.txt", marker="Copied.")
    q.cmd("cat copy.txt", marker="Hello persistence world")
    report("fs: cp duplicates file", "Hello persistence world" in q.screen())
    q.cmd("mv copy.txt moved.txt", marker="Moved.")
    q.cmd("cat moved.txt", marker="Hello persistence world")
    ok_mv = "Hello persistence world" in q.screen()
    q.cmd("cat copy.txt", marker="No such file")
    report("fs: mv renames file", ok_mv and "No such file" in q.screen())
    # edit (v0.5.1): create note.txt, type text, Ctrl+S saves, Ctrl+Q quits
    q.cmd("edit note.txt")
    time.sleep(2)
    q.type_text("EDITED42 text")
    q.mon("sendkey ctrl-s", wait=0.4)
    time.sleep(1)
    q.mon("sendkey ctrl-q", wait=0.4)
    time.sleep(1)
    q.cmd("cat note.txt", marker="EDITED42 text")
    report("fs: edit create+type+save+cat round-trip",
           "EDITED42 text" in q.screen(), q.screen()[-300:])
    # last command of this session: history persistence probe (boot 2)
    q.cmd("echo FINALHIST", marker="FINALHIST")


def check_fs_after_reboot(q):
    # history persistence (v0.5.1): Up recalls boot 1's last command
    q.sendkey("up")
    time.sleep(0.6)
    q.sendkey("ret")
    time.sleep(1.5)
    lines = [ln for ln in q.screen().split("\n") if ln.strip() == "FINALHIST"]
    report("fs: history persists across reboot", len(lines) >= 1,
           q.screen()[-300:])
    q.cmd("cat hello.txt", marker="Hello persistence world", timeout=20)
    report("fs: persistence across reboot", "Hello persistence world" in q.screen())
    q.cmd("rm hello.txt", marker="Removed.")
    q.cmd("cat hello.txt", marker="No such file")
    report("fs: rm works", "No such file" in q.screen())


def check_dhcp(q):
    q.cmd("ip", marker="10.0.2.15", timeout=20)
    scr = q.screen()
    report("dhcp: ip shows slirp address", "10.0.2.15" in scr)
    # the lease line is in the boot snapshot (later output scrolls it away)
    boot_scr = getattr(q, "boot_screen", scr)
    report("dhcp: boot log shows lease", "DHCP: got 10.0.2.15" in boot_scr,
           boot_scr[:600])


def check_network(q):
    q.cmd("ping 10.0.2.2", marker="Reply from", timeout=30)
    report("net: ping gateway", "Reply from 10.0.2.2" in q.screen())
    q.cmd("dns example.com", marker="example.com ->", timeout=90)
    report("net: dns example.com", "example.com ->" in q.screen(),
           q.screen()[-300:])
    q.cmd("browse 10.0.2.2", marker="Hello from the test server", timeout=60)
    report("net: browse local page", "Hello from the test server" in q.screen())
    q.cmd("browse example.com", marker="Example Domain", timeout=90)
    report("net: browse example.com", "Example Domain" in q.screen(),
           q.screen()[-300:])


TLS_MARKER = "Hello from the TLS test server"


def check_tls(q):
    # Local deterministic: TLS server with a cert from the test CA
    # whose root is bundled in the kernel (tests/tls/fixtures).
    q.cmd("browse https://10.0.2.2:8443/", marker=TLS_MARKER, timeout=120)
    scr = q.screen()
    report("tls: browse local https (test CA)", TLS_MARKER in scr,
           scr[-300:])
    report("tls: negotiated TLS 1.2 with a named suite",
           "TLS: connected, TLS 1.2" in scr, scr[-300:])
    # Negative: same SANs but an unrelated CA -> must be rejected.
    q.cmd("clear")
    time.sleep(1)
    q.cmd("browse https://10.0.2.2:8444/", marker="certificate not trusted",
          timeout=120)
    scr = q.screen()
    report("tls: unrelated CA rejected",
           "certificate not trusted" in scr and TLS_MARKER not in scr,
           scr[-400:])
    # Live: example.com via the sandbox egress (cert is issued by the
    # Hatch egress CA, whose root is bundled as a trust anchor).
    q.cmd("clear")
    time.sleep(1)
    q.cmd("browse https://example.com", marker="Example Domain", timeout=120)
    report("tls: browse https://example.com (live)",
           "Example Domain" in q.screen(), q.screen()[-300:])


def check_apps(q):
    # HTTP install: the testsite server serves the built calc ELF
    # (copied to tests/testsite/calc in main()); the app name is
    # derived from the URL.
    q.cmd("install http://10.0.2.2/calc", marker="Installed calc (", timeout=90)
    scr = q.screen()
    report("apps: install over http (name from URL)",
           "Installed calc (" in scr, scr[-300:])
    # HTTPS install from the local TLS file server (:8445, test-CA
    # cert) with an explicit app name.
    q.cmd("install https://10.0.2.2:8445/calc securecalc",
          marker="Installed securecalc", timeout=120)
    scr = q.screen()
    report("apps: install over https + explicit name",
           "Installed securecalc" in scr, scr[-300:])
    q.cmd("apps", marker="securecalc")
    scr = q.screen()
    report("apps: list shows both installed apps",
           "calc" in scr and "securecalc" in scr, scr[-300:])
    # Duplicate installs are rejected.
    q.cmd("install http://10.0.2.2/calc", marker="already installed",
          timeout=30)
    report("apps: duplicate install rejected",
           "already installed" in q.screen(), q.screen()[-300:])
    # Run calc: operator precedence and parentheses.
    q.cmd("run calc", marker="calc>", timeout=20)
    q.type_text("2+3*4")
    q.sendkey("ret")
    ok1 = q.wait_for("= 14", timeout=20)
    q.type_text("(2+3)*4")
    q.sendkey("ret")
    ok2 = q.wait_for("= 20", timeout=20)
    report("apps: calc evaluates 2+3*4=14 and (2+3)*4=20",
           ok1 and ok2, q.screen()[-300:])
    q.sendkey("ret")  # empty line quits calc
    ok = q.wait_for("Program exited with code 0", timeout=20)
    report("apps: calc exits on empty line", ok, q.screen()[-300:])
    # Persistence: registry (the SHFS directory) survives a reboot.
    q.cmd("reboot")
    ok = q.wait_for("Type 'help'", timeout=60)
    report("apps: guest reboots", ok)
    if not ok:
        return
    time.sleep(1)
    q.cmd("apps", marker="securecalc")
    report("apps: registry persists across reboot",
           "calc" in q.screen() and "securecalc" in q.screen(),
           q.screen()[-300:])
    q.cmd("run calc", marker="calc>", timeout=20)
    q.type_text("1+1")
    q.sendkey("ret")
    ok = q.wait_for("= 2", timeout=20)
    q.sendkey("ret")
    q.wait_for("Program exited with code 0", timeout=20)
    report("apps: calc still runs after reboot", ok, q.screen()[-300:])
    # Uninstall both; the listing must end up empty. `clear` first so
    # the absence check cannot match scrollback from earlier output.
    q.cmd("uninstall calc", marker="Uninstalled calc.")
    q.cmd("uninstall securecalc", marker="Uninstalled securecalc")
    q.cmd("clear")
    time.sleep(1)
    q.cmd("apps", marker="No apps installed")
    report("apps: uninstall removes apps", "calc" not in q.screen(),
           q.screen()[-300:])
    # --- v1.0.2 security regressions (docs/security-review.md) ---
    # (i) The red-team round-2 wraparound ELF: on v1.0.1 its second
    # segment overwrote syscall_handler in ring 0. Now the loader
    # must reject the whole image and the shell must stay alive.
    q.cmd("install http://10.0.2.2/wrapelf", marker="Installed wrapelf (",
          timeout=90)
    q.cmd("run wrapelf", marker="Program exited with code", timeout=30)
    scr = q.screen()
    report("apps: wraparound ELF rejected at load (v1.0.2)",
           "ELF: segment outside user window." in scr
           or "ELF: segment overlaps kernel." in scr, scr[-300:])
    q.cmd("version", marker="v1.0.2")
    report("apps: shell alive after wraparound rejection",
           "Shiro Mini OS v1.0.2" in q.screen())
    # (ii) write() with a kernel-space pointer must be refused:
    # the fixture app prints BLOCKED when the syscall rejects it
    # (on v1.0.1 it printed ALLOWED after dumping kernel memory).
    q.cmd("install http://10.0.2.2/leak", marker="Installed leak (",
          timeout=90)
    q.cmd("run leak", marker="Program exited with code", timeout=30)
    scr = q.screen()
    report("apps: write syscall refuses kernel pointer (v1.0.2)",
           "BLOCKED" in scr and "ALLOWED" not in scr, scr[-300:])
    q.cmd("uninstall wrapelf", marker="Uninstalled wrapelf.")
    q.cmd("uninstall leak", marker="Uninstalled leak.")


def check_shell_qol(q):
    q.cmd("echo HISTMARK", marker="HISTMARK")
    # Up arrow recalls the command, Enter runs it again
    q.sendkey("up")
    time.sleep(0.5)
    q.sendkey("ret")
    time.sleep(1.5)
    lines = [ln for ln in q.screen().split("\n") if ln.strip() == "HISTMARK"]
    report("shell: history (up arrow)", len(lines) >= 2,
           f"hist lines={len(lines)}")
    # line editing: type "echo AB", move left twice, insert Z -> "echo ZAB"
    q.type_text("echo AB")
    q.sendkey("left")
    q.sendkey("left")
    q.type_text("Z")
    q.sendkey("ret")
    time.sleep(1.5)
    lines = [ln for ln in q.screen().split("\n") if ln.strip() == "ZAB"]
    report("shell: left-arrow line editing", len(lines) >= 1, q.screen()[-400:])
    # Tab completion of a command name: "ver" + Tab + Enter runs `version`
    q.type_text("ver")
    q.sendkey("tab")
    time.sleep(0.5)
    q.sendkey("ret")
    ok = q.wait_for("Shiro Mini OS v1.0.2", timeout=15)
    report("shell: tab completes command name", ok, q.screen()[-300:])
    # Tab completion of a file name as an argument. Assert on a bare
    # "ZDATA9" output line: the `write` command line itself contains
    # the string, so a plain substring wait would pass vacuously.
    q.cmd("write compx.txt ZDATA9", marker="Written.")
    q.type_text("cat comp")
    q.sendkey("tab")
    time.sleep(0.5)
    q.sendkey("ret")
    ok = False
    for _ in range(20):
        time.sleep(0.5)
        if any(ln.strip() == "ZDATA9" for ln in q.screen().split("\n")):
            ok = True
            break
    report("shell: tab completes file name", ok, q.screen()[-300:])


def check_run_hello(q):
    q.cmd("run hello", marker="Hello from user mode", timeout=20)
    scr = q.screen()
    report("user: hello prints greeting", "Hello from user mode" in scr)
    q.type_text("xyz")
    ok = q.wait_for("User program exited with code 3", timeout=20)
    report("user: echo + clean exit to shell", ok, q.screen()[-400:])
    # The echoed "xyz" must really appear: until v0.7 the sample's
    # syscall asm lacked a "memory" clobber, GCC eliminated the
    # stack-buffer stores, and the echo silently printed NULs while
    # this check still passed on the exit line alone.
    report("user: hello echo visible (syscall memory clobber)",
           "xyz" in q.screen(), q.screen()[-400:])
    q.cmd("version", marker="v1.0.2")
    report("user: shell alive after exit", "Shiro Mini OS v1.0.2" in q.screen())


# v0.5.1 modern theme: the desktop is a 48-step gradient between
# GRAD_TOP (4,14,19) and GRAD_BOT (2,7,10) in 6-bit DAC terms, so
# "desktop present" is tested as a channel envelope, not exact colors.
GRAD_TOP_RGB = vga_rgb((4, 14, 19))
GRAD_BOT_RGB = vga_rgb((2, 7, 10))
TASKBAR_RGBS = [vga_rgb(c) for c in [(5, 9, 11), (8, 13, 16), (2, 16, 20)]]
ICON_RGB = vga_rgb((60, 44, 13))


def count_gradient(pix, w, h, region=None):
    """Count pixels inside the desktop gradient's channel envelope."""
    x0, y0, x1, y1 = region or (0, 0, w, h)
    lo = [min(GRAD_TOP_RGB[i], GRAD_BOT_RGB[i]) - 10 for i in range(3)]
    hi = [max(GRAD_TOP_RGB[i], GRAD_BOT_RGB[i]) + 10 for i in range(3)]
    n = 0
    for y in range(y0, y1):
        base = y * w * 3
        for x in range(x0, x1):
            o = base + x * 3
            if (lo[0] <= pix[o] <= hi[0] and lo[1] <= pix[o + 1] <= hi[1]
                    and lo[2] <= pix[o + 2] <= hi[2]
                    and pix[o + 2] > pix[o] + 8):
                n += 1
    return n


def check_gui_desktop(q):
    q.cmd("gui")
    time.sleep(4)
    w, h, pix = q.screendump("g1.ppm")
    report("gui: frame is not the 720x400 text screen", (w, h) != (720, 400),
           "%dx%d" % (w, h))
    total = w * h
    ndesk = count_gradient(pix, w, h)
    report("gui: desktop background visible", ndesk > total * 0.12,
           "%d/%d px" % (ndesk, total))
    sx, sy = w / 320.0, h / 200.0
    ty = int(190 * sy)
    th = max(1, int(2 * sy))
    ntask = sum(count_close(pix, w, h, c, region=(0, ty, w, min(h, ty + th)))
                for c in TASKBAR_RGBS)
    report("gui: taskbar strip visible", ntask > w * th * 0.4, "%d px" % ntask)
    # Term typing (Term starts focused). Diff the Term content area,
    # excluding the Info window's live "Uptime:" text zone (guest
    # x 20..140, y 74..98) which changes on its own every second.
    q.type_text("echo GUIMARK")
    q.sendkey("ret")
    time.sleep(2.5)
    w2, h2, pix2 = q.screendump("g2.ppm")
    r1 = (int(103 * sx), int(98 * sy), int(308 * sx), int(172 * sy))
    r2 = (int(140 * sx), int(74 * sy), int(308 * sx), int(98 * sy))
    diff = region_diff(pix, pix2, w, r1) + region_diff(pix, pix2, w, r2)
    report("gui: term echo changes pixels", diff > 300, "diff=%d px" % diff)
    # Drag the Info window by (+40,+20) guest px via its title bar.
    c1 = centroid_close(pix2, w2, h2, ICON_RGB)
    report("gui: info icon found", c1 is not None,
           ("centroid %.1f,%.1f" % (c1[0], c1[1])) if c1 else "none")
    if c1:
        # QEMU's monitor mouse_move is RELATIVE for the PS/2 mouse here:
        # mouse_move(dx, dy) moves the guest cursor by (dx, dy), signed
        # (verified byte-for-byte against the guest's `mouse` readout).
        # The guest cursor enters the GUI at (160, 100); the Info title
        # bar center is guest (108, 22).
        q.mouse_move(108 - 160, 22 - 100)
        time.sleep(0.5)
        q.mouse_button(1)
        time.sleep(0.4)
        for _ in range(5):
            q.mouse_move(8, 4)
            time.sleep(0.15)
        time.sleep(0.4)
        q.mouse_button(0)
        time.sleep(1.2)
        w3, h3, pix3 = q.screendump("g3.ppm")
        c2 = centroid_close(pix3, w3, h3, ICON_RGB)
        if c2:
            dx = (c2[0] - c1[0]) / sx
            dy = (c2[1] - c1[1]) / sy
            ok = 22 <= dx <= 58 and 11 <= dy <= 29
            report("gui: drag moves info window", ok,
                   "delta guest px = (%.1f, %.1f), want ~(40,20)" % (dx, dy))
        else:
            report("gui: drag moves info window", False, "icon lost after drag")
    # ESC restores the text shell.
    q.sendkey("esc")
    time.sleep(2)
    q.cmd("echo RESTORED42", marker="RESTORED42")
    report("gui: esc restores text shell", "RESTORED42" in q.screen())


def mouse_move_to(q, x0, y0, x1, y1):
    """Move the guest cursor from (x0,y0) to (x1,y1) in <=100px steps
    (monitor mouse_move is a relative delta for the PS/2 mouse)."""
    dx, dy = x1 - x0, y1 - y0
    steps = max(1, int(max(abs(dx), abs(dy)) / 100) + 1)
    for i in range(steps):
        q.mouse_move(dx / steps, dy / steps)
        time.sleep(0.2)


def check_res(q):
    q.cmd("res", marker="1024x768")
    scr = q.screen()
    report("res: no-arg prints setting and choices",
           "GUI resolution:" in scr and "800x600" in scr, scr[-300:])
    q.cmd("res 800 600", marker="Resolution saved: 800x600")
    report("res: set 800x600 saved", "Resolution saved: 800x600" in q.screen())
    q.cmd("cat settings", marker="res=800x600")
    report("res: settings file contents", "res=800x600" in q.screen())
    # Launch the GUI: it must come up at 800x600 (VBE).
    q.cmd("gui")
    time.sleep(5)
    w, h, pix = q.screendump("r1.ppm")
    report("res: screendump is 800x600", (w, h) == (800, 600), "%dx%d" % (w, h))
    if (w, h) == (800, 600):
        total = w * h
        ndesk = count_gradient(pix, w, h)
        report("res: desktop background at 800x600", ndesk > total * 0.12,
               "%d/%d px" % (ndesk, total))
        ntask = sum(count_close(pix, w, h, c, region=(0, 584, w, 592))
                    for c in TASKBAR_RGBS)
        report("res: taskbar at bottom of 800x600", ntask > w * 8 * 0.4,
               "%d px" % ntask)
        # Drag the Info window by (+40,+20). The cursor enters the GUI
        # at the screen center (400,300); the Info window sits at
        # (24,20) 264 wide, its icon centroid is ~(38,52), so the title
        # bar point is ~(156,29) relative to the icon centroid.
        c1 = centroid_close(pix, w, h, ICON_RGB)
        report("res: info icon found at 800x600", c1 is not None)
        if c1:
            txp, typ = c1[0] + 118, c1[1] - 23
            mouse_move_to(q, 400, 300, txp, typ)
            time.sleep(0.5)
            q.mouse_button(1)
            time.sleep(0.4)
            for _ in range(5):
                q.mouse_move(8, 4)
                time.sleep(0.15)
            time.sleep(0.4)
            q.mouse_button(0)
            time.sleep(1.2)
            w2, h2, pix2 = q.screendump("r2.ppm")
            c2 = centroid_close(pix2, w2, h2, ICON_RGB)
            if c2:
                dx, dy = c2[0] - c1[0], c2[1] - c1[1]
                ok = 22 <= dx <= 58 and 11 <= dy <= 29
                report("res: drag moves info window at 800x600", ok,
                       "delta = (%.1f, %.1f), want ~(40,20)" % (dx, dy))
            else:
                report("res: drag moves info window at 800x600", False,
                       "icon lost after drag")
    # ESC from the VBE mode must restore the text shell.
    q.sendkey("esc")
    time.sleep(2)
    q.cmd("version", marker="v1.0.2")
    report("res: esc from VBE restores text shell, version v0.8",
           "Shiro Mini OS v1.0.2" in q.screen())
    # v0.9: 1280x1024 (two flip pages = 10.5 MB, fits the 16 MB
    # VRAM) and 1280x720 must work too; leave 800x600 saved after.
    q.cmd("res 1280 1024", marker="1280x1024")
    q.cmd("gui")
    time.sleep(6)
    w, h, pix = q.screendump("r3.ppm")
    report("res: screendump is 1280x1024", (w, h) == (1280, 1024),
           "%dx%d" % (w, h))
    if (w, h) == (1280, 1024):
        total = w * h
        ndesk = count_gradient(pix, w, h)
        report("res: desktop background at 1280x1024",
               ndesk > total * 0.12, "%d/%d px" % (ndesk, total))
    q.sendkey("esc")
    time.sleep(2)
    q.cmd("res 800 600", marker="800x600")


def check_res_persist(q):
    # Reboot the guest via its own shell command; the setting must be
    # reloaded from the settings file at boot.
    q.cmd("reboot")
    ok = q.wait_for("Type 'help'", timeout=60)
    report("res: guest reboots", ok)
    if not ok:
        return
    time.sleep(1)
    q.cmd("res", marker="GUI resolution: 800x600")
    report("res: setting survives reboot (shell view)",
           "GUI resolution: 800x600" in q.screen())
    q.cmd("gui")
    time.sleep(5)
    w, h, pix = q.screendump("r3.ppm")
    report("res: gui still 800x600 after reboot", (w, h) == (800, 600),
           "%dx%d" % (w, h))
    q.sendkey("esc")
    time.sleep(2)
    # Leave the shared test disk in the default (unset) state.
    q.cmd("rm settings", marker="Removed.")
    report("res: settings file removed (cleanup)", "Removed." in q.screen())


# Bar for the drag-period average FPS at 800x600 under QEMU TCG.
# Measured on this host (guest-time, pacer capped at 60): 37-48 fps
# across runs while the harness floods the monitor socket with
# ~4000 mouse_move commands/s (which also starves the vCPU thread);
# the guest's uncapped compose+present rate for the same workload
# is ~238 fps. The bar sits below the worst measured run so host
# load variance doesn't flake the suite; see README for the numbers.
GFXSTATS_FPS_BAR = 30.0


def check_gfxstats(q):
    """v0.8: continuous window drag at 800x600 for ~3 s, then read
    the kernel frame statistics in text mode and assert the
    drag-period average FPS."""
    q.cmd("res 800 600", marker="Resolution saved")
    q.cmd("gui")
    time.sleep(4)
    w, h, pix = q.screendump("gfx1.ppm")
    c = centroid_close(pix, w, h, ICON_RGB)
    if not c:
        report("gfxstats: drag avg fps", False, "info icon not found")
        q.sendkey("esc")
        time.sleep(1)
        q.cmd("rm settings")
        return
    tx, ty = int(c[0]) + 118, int(c[1]) - 23  # Info title grab point
    mouse_move_to(q, 400, 300, tx, ty)
    time.sleep(0.3)
    q.mouse_button(1)
    time.sleep(0.3)
    # Flood the guest with motion (no pacing sleeps): the guest's
    # own frame loop becomes the bottleneck, which is what we want
    # to measure.
    t0 = time.time()
    n = 0
    while time.time() - t0 < 3.0:
        q.mon("mouse_move %d 0" % (6 if n % 2 == 0 else -6), wait=0)
        n += 1
    q.mouse_button(0)
    time.sleep(0.5)
    q.sendkey("esc")
    time.sleep(2)
    scr = q.cmd("gfxstats", marker="avg fps:")
    if not isinstance(scr, str):
        scr = q.screen()
    m = re.search(r"avg fps: (\d+)\.(\d+)", scr)
    if m:
        fps = int(m.group(1)) + int(m.group(2)) / 10.0
        report("gfxstats: drag avg fps %.1f >= %.0f" % (fps, GFXSTATS_FPS_BAR),
               fps >= GFXSTATS_FPS_BAR, scr[-260:].replace("\n", " | "))
    else:
        report("gfxstats: drag avg fps", False, "no fps line: " + scr[-260:])
    q.cmd("rm settings")


def check_gui(q):
    check_gui_desktop(q)
    check_res(q)
    check_res_persist(q)
    check_gfxstats(q)


def check_jstest(q):
    q.cmd("jstest", marker="JSRESULT", timeout=60)
    scr = q.screen()
    report("jstest: Duktape eval result",
           "JSRESULT shiro-js|7|610|9" in scr, scr[-400:])


def save_png(path, w, h, pix):
    """Write an RGB pixel buffer as a PNG (stdlib only)."""
    import struct
    import zlib

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 6)) +
           chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


WEB_RED = (255, 85, 85)        # page 1 <body bgcolor=#FF5555>
WEB_GREEN = (85, 255, 85)      # page 2 / TLS page bgcolor=#55FF55
WEB_LINK = (85, 85, 255)       # link color (palette 9)
WEB_STRIP = vga_rgb((8, 13, 18))    # COL_TAB_STRIP
WEB_FACE = vga_rgb((59, 61, 62))    # COL_WIN_FACE (start page bg)


def wait_color(q, rgb, min_count, timeout=90, region=None):
    """Poll screendumps until `rgb` covers >= min_count pixels."""
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        w, h, pix = q.screendump("webwait.ppm")
        if count_close(pix, w, h, rgb, region=region) >= min_count:
            return (w, h, pix)
        last = (w, h, pix)
        time.sleep(1)
    return last


def count_dark(pix, w, region):
    """Count near-black text pixels in a region (glyph detector)."""
    x0, y0, x1, y1 = region
    n = 0
    for y in range(y0, min(y1, len(pix) // (w * 3))):
        base = y * w * 3
        for x in range(x0, x1):
            o = base + x * 3
            if pix[o] < 90 and pix[o + 1] < 90 and pix[o + 2] < 90:
                n += 1
    return n


def link_click_point(pix, w, h):
    """Centroid of the link-blue pixels in the first band that has
    any (a wrapped link's overall centroid can fall in a notch)."""
    miny = None
    for yy in range(h):
        base = yy * w * 3
        for xx in range(w):
            o = base + xx * 3
            if (abs(pix[o] - WEB_LINK[0]) <= 12 and
                    abs(pix[o + 1] - WEB_LINK[1]) <= 12 and
                    abs(pix[o + 2] - WEB_LINK[2]) <= 12):
                miny = yy
                break
        if miny is not None:
            break
    if miny is None:
        return None
    band = 18
    sx2 = sy2 = n2 = 0
    for yy in range(miny, min(h, miny + band)):
        base = yy * w * 3
        for xx in range(w):
            o = base + xx * 3
            if (abs(pix[o] - WEB_LINK[0]) <= 12 and
                    abs(pix[o + 1] - WEB_LINK[1]) <= 12 and
                    abs(pix[o + 2] - WEB_LINK[2]) <= 12):
                sx2 += xx
                sy2 += yy
                n2 += 1
    if not n2:
        return None
    return (sx2 / n2, sy2 / n2)


def bbox_close(pix, w, h, rgb, tol=12, region=None):
    """Bounding box (x0, y0, x1, y1, n) of pixels close to rgb."""
    x0, y0, x1, y1 = region or (0, 0, w, h)
    bx0 = by0 = bx1 = by1 = None
    n = 0
    for y in range(max(0, y0), min(y1, h)):
        base = y * w * 3
        for x in range(max(0, x0), min(x1, w)):
            o = base + x * 3
            if (abs(pix[o] - rgb[0]) <= tol and abs(pix[o + 1] - rgb[1]) <= tol
                    and abs(pix[o + 2] - rgb[2]) <= tol):
                n += 1
                bx0 = x if bx0 is None else min(bx0, x)
                bx1 = x if bx1 is None else max(bx1, x)
                by0 = y if by0 is None else min(by0, y)
                by1 = y if by1 is None else max(by1, y)
    if n == 0:
        return None
    return (bx0, by0, bx1, by1, n)


def web_v10_checks(q):
    """v1.0: CSS, images, forms, cookies, and the navigation
    robustness suite (a failed navigation must never eject to
    the shell). Runs inside check_web's maximized browser
    session on tab 0; the cursor is at (28, 184) on entry."""
    CONTENT = (2, 87, 798, 565)
    echo = subprocess.Popen(
        ["python3", os.path.join(HERE, "echo_server.py")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # NEXT-STEPS P0: a host that drips one body byte every 3 s,
    # inside the download core's 10 s idle timeout.
    drip = subprocess.Popen(
        ["python3", os.path.join(HERE, "drip_server.py")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    big = os.path.join(HERE, "testsite", "big.html")
    cur = [28, 184]

    def click(x, y):
        """Slow small-step move + click. Rapid or large PS/2
        delta bursts intermittently lose packets in the guest's
        IRQ state machine, desyncing the real cursor from the
        tracked one; ~40px steps with pauses are reliable."""
        dx, dy = x - cur[0], y - cur[1]
        steps = max(1, int(max(abs(dx), abs(dy)) / 40) + 1)
        for i in range(1, steps + 1):
            q.mouse_move(dx * i // steps - dx * (i - 1) // steps,
                         dy * i // steps - dy * (i - 1) // steps)
            time.sleep(0.22)
        cur[0], cur[1] = x, y
        time.sleep(0.4)
        q.mouse_button(1)
        time.sleep(0.4)
        q.mouse_button(0)
        time.sleep(1.0)

    def anchor():
        """Re-sync the tracked cursor with the guest's: saturate
        to the top-left corner (relative moves clamp there) in
        small steps the guest reliably consumes."""
        for _ in range(12):
            q.mouse_move(-80, -80)
            time.sleep(0.12)
        cur[0], cur[1] = 0, 0

    def focus_input(ib):
        """Click a text input and verify the teal focus ring
        appears; re-anchor and retry (packet loss insurance)."""
        for _ in range(3):
            anchor()
            click((ib[0] + ib[2]) // 2, (ib[1] + ib[3]) // 2)
            time.sleep(0.6)
            w, h, pix = q.screendump("v10focus.ppm")
            n = count_close(pix, w, h, (65, 195, 219), 40,
                            (max(0, ib[0] - 4), max(0, ib[1] - 4),
                             ib[2] + 4, ib[3] + 4))
            if n >= 50:
                return True
        return False

    def first_white_row(pix, w, y0, y1):
        """First row holding a wide run of pure-white pixels
        (a text input's top edge; a checkbox row is too narrow
        to qualify)."""
        for y in range(y0, min(y1, len(pix) // (w * 3))):
            n = 0
            base = y * w * 3
            for x in range(2, 798):
                o = base + x * 3
                if pix[o] > 249 and pix[o + 1] > 249 and pix[o + 2] > 249:
                    n += 1
            if n >= 40:
                return y
        return None

    def form_input_bbox(pix, w):
        """The (first) text input's white box on a form page."""
        row = first_white_row(pix, w, 87, 340)
        if row is None:
            return None
        return bbox_close(pix, w, 600, (255, 255, 255), 6,
                          (2, row, 798, row + 24))

    def nav(url):
        q.mon("sendkey ctrl-l", wait=0.4)
        time.sleep(0.8)
        q.type_text(url)
        q.sendkey("ret")

    def strip_ok():
        w, h, pix = q.screendump("v10strip.ppm")
        return count_close(pix, w, h, WEB_STRIP,
                           region=(0, 19, w, 49)) >= 4000

    try:
        time.sleep(0.7)
        # ---------- CSS subset ----------
        nav("10.0.2.2:8080/styled.html")
        got = wait_color(q, (51, 102, 153), 30000, timeout=90,
                         region=CONTENT)
        w, h, pix = got
        nbg = count_close(pix, w, h, (51, 102, 153), region=CONTENT)
        report("web1.0: CSS body background-color applies",
               nbg >= 30000, "bg=%d" % nbg)
        bb = bbox_close(pix, w, h, (255, 204, 0), region=(2, 87, 798, 300))
        cen_x = (bb[0] + bb[2]) / 2 if bb else -1
        report("web1.0: CSS colored heading is centered",
               bb is not None and bb[4] >= 200 and 320 <= cen_x <= 470,
               "n=%s cen_x=%s" % (bb[4] if bb else 0, cen_x))
        gb = bbox_close(pix, w, h, (0, 170, 85), region=CONTENT)
        # display:none: the padbox would sit ~26px lower if the
        # hidden paragraph still occupied a line (measured 155).
        report("web1.0: display:none removes the element",
               gb is not None and gb[1] <= 175,
               "padbox top=%s" % (gb[1] if gb else None))
        pad = None
        if gb:
            for y in range(gb[1], gb[3] + 1):
                for x in range(gb[0], gb[2] + 1):
                    o = (y * w + x) * 3
                    if pix[o] > 235 and pix[o + 1] > 235 and pix[o + 2] > 235:
                        pad = x - gb[0] if pad is None else min(pad, x - gb[0])
        report("web1.0: CSS padding offsets block content",
               pad is not None and 18 <= pad <= 30, "pad=%s" % pad)
        nx = count_close(pix, w, h, (255, 0, 255), region=CONTENT)
        report("web1.0: external <link> stylesheet applies",
               nx >= 3000, "extbox=%d" % nx)
        save_png(os.path.join(ROOT, "browser-styled-v10.png"), w, h, pix)

        # ---------- images ----------
        nav("10.0.2.2:8080/picpage.html")
        ok_img = False
        t0 = time.time()
        while time.time() - t0 < 60:
            w, h, pix = q.screendump("v10pic.ppm")
            nm = count_close(pix, w, h, (255, 0, 255), region=CONTENT)
            nc = count_close(pix, w, h, (0, 255, 255), region=CONTENT)
            if nm >= 6000 and nc >= 1500:
                ok_img = True
                break
            time.sleep(1)
        report("web1.0: <img> fetched, decoded, scaled in flow",
               ok_img, "magenta=%d cyan=%d" % (nm, nc))
        save_png(os.path.join(ROOT, "browser-pic-v10.png"), w, h, pix)

        # ---------- NEXT-STEPS P0: stalled image host ----------
        # slowimg.html has no page text at all: its only text is
        # the <img> alt, so dark pixels in the content area prove
        # the alt fallback rendered. Without a per-image total
        # deadline the dripping image would hold the page for
        # the 120 s body cap, so the 30 s wait below fails;
        # with the 5 s deadline the page lands in ~10 s.
        nav("10.0.2.2:8080/slowimg.html")
        got = wait_color(q, WEB_GREEN, 30000, timeout=30,
                         region=CONTENT)
        w, h, pix = got
        ng = count_close(pix, w, h, WEB_GREEN, region=CONTENT) \
            if got else 0
        nalt = count_dark(pix, w, CONTENT) if got else 0
        report("web1.0: stalled image host finishes the page "
               "with alt text (P0 timeout)",
               ng >= 30000 and nalt >= 50,
               "green=%d dark=%d" % (ng, nalt))
        # and the browser is still responsive afterwards. Return
        # to picpage, not another page: the failed-navigation
        # suite below expects the previous page to still be the
        # magenta/cyan image page (its "oldpage" assertion).
        nav("10.0.2.2:8080/picpage.html")
        got = wait_color(q, (255, 0, 255), 6000, timeout=60,
                         region=CONTENT)
        w, h, pix = got
        nm = count_close(pix, w, h, (255, 0, 255), region=CONTENT) \
            if got else 0
        nc = count_close(pix, w, h, (0, 255, 255), region=CONTENT) \
            if got else 0
        report("web1.0: browser stays responsive after the "
               "stalled image (P0 timeout)",
               nm >= 6000 and nc >= 1500,
               "magenta=%d cyan=%d" % (nm, nc))

        # ---------- failed navigations stay in the browser ----
        fails = [
            ("404", "10.0.2.2:8080/nope.html", 6),
            ("bad cert", "https://10.0.2.2:8444/x", 8),
            ("DNS failure", "http://nonexistent.invalid/", 20),
            ("closed port", "http://10.0.2.2:9999/", 8),
            ("redirect chain too long", "10.0.2.2:8083/r1", 8),
            ("redirect loop", "10.0.2.2:8083/loop", 8),
            # last: the silent host takes the guest's full TCP
            # connect timeout to give up; keystrokes sent during
            # the block are buffered by the keyboard IRQ and would
            # only be read afterwards.
            ("silent host", "http://10.0.2.99:8080/", 50),
        ]
        for name, url, wait in fails:
            nav(url)
            time.sleep(wait)
            w, h, pix = q.screendump("v10fail.ppm")
            nstrip = count_close(pix, w, h, WEB_STRIP,
                                 region=(0, 19, w, 49))
            nmag = count_close(pix, w, h, (255, 0, 255), region=CONTENT)
            report("web1.0: failed nav stays in browser (%s)" % name,
                   nstrip >= 4000 and nmag >= 5000,
                   "strip=%d oldpage=%d" % (nstrip, nmag))
        save_png(os.path.join(ROOT, "browser-fail-v10.png"), w, h, pix)
        # let any still-running fetch (silent host) settle: wait
        # until the frame stops changing before navigating on
        t0 = time.time()
        _, _, prev = q.screendump("v10settle.ppm")
        while time.time() - t0 < 60:
            time.sleep(3)
            w, h, pix = q.screendump("v10settle.ppm")
            if region_diff(prev, pix, w, (2, 87, 798, 581)) < 50:
                break
            prev = pix

        # ---------- redirects that fit do land ----------
        nav("10.0.2.2:8083/ok1")
        got = wait_color(q, WEB_GREEN, 30000, timeout=60, region=CONTENT)
        w, h, pix = got
        ng = count_close(pix, w, h, WEB_GREEN, region=CONTENT) if got else 0
        report("web1.0: 2-hop redirect chain lands", ng >= 30000,
               "green=%d" % ng)

        # ---------- oversized page: truncated, not fatal -------
        with open(big, "w") as f:
            f.write('<html><head><title>Big</title></head>'
                    '<body bgcolor="#FFCC00"><h1>Big page</h1>\n')
            chunk = "<p>" + ("filler text for a very large page. " * 4) + "</p>\n"
            n = 0
            while n < 300 * 1024:
                f.write(chunk)
                n += len(chunk)
            f.write("</body></html>")
        nav("10.0.2.2:8080/big.html")
        got = wait_color(q, (255, 204, 0), 30000, timeout=240,
                         region=CONTENT)
        w, h, pix = got
        ny = count_close(pix, w, h, (255, 204, 0), region=CONTENT) if got else 0
        report("web1.0: oversized page loads truncated in browser",
               ny >= 30000 and strip_ok(), "yellow=%d" % ny)

        # ---------- forms: GET round trip + checkbox ----------
        nav("10.0.2.2:8083/getform.html")
        ok_form = False
        t0 = time.time()
        while time.time() - t0 < 60:
            w, h, pix = q.screendump("v10form.ppm")
            if count_close(pix, w, h, (255, 255, 255), 6, CONTENT) >= 3000:
                ok_form = True
                break
            time.sleep(1)
        report("web1.0: form page renders its controls", ok_form)
        ib = form_input_bbox(pix, w)
        if ib:
            focus_input(ib)
            q.type_text("hello")
            time.sleep(0.5)
        nyel = 0
        for _ in range(2):
            bb2 = bbox_close(pix, w, h, (32, 52, 64), 10, (2, 87, 400, 565))
            if bb2:
                click((bb2[0] + bb2[2]) // 2, (bb2[1] + bb2[3]) // 2)
            got = wait_color(q, (255, 255, 85), 20000, timeout=30,
                             region=CONTENT)
            w, h, pix = got
            nyel = count_close(pix, w, h, (255, 255, 85),
                               region=CONTENT) if got else 0
            if nyel >= 20000:
                break
        report("web1.0: GET form submits the typed text",
               nyel >= 20000, "yellow=%d" % nyel)
        # back to the form, tick the checkbox, submit again
        click(21, 68)  # toolbar Back
        ok_form = False
        t0 = time.time()
        while time.time() - t0 < 60:
            w, h, pix = q.screendump("v10form2.ppm")
            if count_close(pix, w, h, (255, 255, 255), 6, CONTENT) >= 3000:
                ok_form = True
                break
            time.sleep(1)
        ib = form_input_bbox(pix, w)
        cb = None
        if ib:
            # type the query again (fresh form), then find the
            # checkbox: the small white box below the input
            focus_input(ib)
            q.type_text("hello")
            time.sleep(0.5)
            cb = bbox_close(pix, w, h, (255, 255, 255), 6,
                            (2, ib[3] + 2, 120, ib[3] + 110))
            if cb and cb[4] < 100:
                cb = None
        if cb:
            # click the checkbox; verify the check mark (dark
            # pixels inside the box) and retry once if absent
            for _ in range(2):
                click((cb[0] + cb[2]) // 2, (cb[1] + cb[3]) // 2)
                time.sleep(0.6)
                w, h, pix = q.screendump("v10cb.ppm")
                if count_dark(pix, w, (cb[0] + 2, cb[1] + 2,
                                       cb[2] - 1, cb[3] - 1)) >= 12:
                    break
        ng = 0
        for _ in range(2):
            bb2 = bbox_close(pix, w, h, (32, 52, 64), 10, (2, 87, 400, 565))
            if bb2:
                click((bb2[0] + bb2[2]) // 2, (bb2[1] + bb2[3]) // 2)
            got = wait_color(q, WEB_GREEN, 20000, timeout=30,
                             region=CONTENT)
            w, h, pix = got
            ng = count_close(pix, w, h, WEB_GREEN,
                             region=CONTENT) if got else 0
            if ng >= 20000:
                break
        report("web1.0: checkbox changes the submitted value",
               ng >= 20000, "green=%d" % ng)

        # ---------- forms: POST round trip (Enter submits) -----
        nav("10.0.2.2:8083/postform.html")
        ok_form = False
        t0 = time.time()
        while time.time() - t0 < 60:
            w, h, pix = q.screendump("v10post.ppm")
            if count_close(pix, w, h, (255, 255, 255), 6, CONTENT) >= 3000:
                ok_form = True
                break
            time.sleep(1)
        ib = form_input_bbox(pix, w)
        if ib:
            focus_input(ib)
            q.type_text("world")
            time.sleep(0.5)
            q.sendkey("ret")  # implicit submission
        got = wait_color(q, (255, 85, 255), 20000, timeout=60,
                         region=CONTENT)
        w, h, pix = got
        nmg = count_close(pix, w, h, (255, 85, 255),
                          region=CONTENT) if got else 0
        report("web1.0: POST form submits (Enter key)", nmg >= 20000,
               "magenta=%d" % nmg)
        save_png(os.path.join(ROOT, "browser-form-v10.png"), w, h, pix)

        # ---------- cookie jar round trip ----------
        nav("10.0.2.2:8083/cookietest")
        got = wait_color(q, WEB_RED, 20000, timeout=60, region=CONTENT)
        w, h, pix = got
        nr = count_close(pix, w, h, WEB_RED, region=CONTENT) if got else 0
        report("web1.0: cookie absent on first visit", nr >= 20000,
               "red=%d" % nr)
        q.mon("sendkey ctrl-r", wait=0.4)
        got = wait_color(q, WEB_GREEN, 20000, timeout=60, region=CONTENT)
        w, h, pix = got
        ng = count_close(pix, w, h, WEB_GREEN, region=CONTENT) if got else 0
        report("web1.0: cookie sent back on reload", ng >= 20000,
               "green=%d" % ng)
    finally:
        echo.terminate()
        drip.terminate()
        if os.path.exists(big):
            os.unlink(big)


def check_web(q):
    # Own port (8080) for the testsite so nothing else collides.
    srv = subprocess.Popen(
        ["python3", "-m", "http.server", "8080", "--directory",
         os.path.join(HERE, "testsite"), "--bind", "0.0.0.0"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.7)
    try:
        q.cmd("res 800 600", marker="800x600")  # deterministic geometry
        # --- `web` opens the GUI with the browser WINDOW on page 1 ---
        # Default window (56,36,688,468); client origin (58,55).
        q.type_text("web 10.0.2.2:8080/page1.html")
        q.sendkey("ret")
        got = wait_color(q, WEB_RED, 50000)
        w, h, pix = got
        nred = count_close(pix, w, h, WEB_RED) if got else 0
        report("web: page 1 renders in the browser window (bgcolor)",
               (w, h) == (800, 600) and nred >= 50000,
               "%d red px (%dx%d)" % (nred, w, h))
        if got:
            save_png(os.path.join(ROOT, "browser-window-v08.png"), w, h, pix)
            nstrip = count_close(pix, w, h, WEB_STRIP,
                                 region=(58, 55, 742, 85))
            report("web: tab strip inside the window (Proton chrome)",
                   nstrip >= 2000, "%d strip px" % nstrip)
        cen = link_click_point(pix, w, h) if got else None
        report("web: page 1 link visible", cen is not None)
        if not cen:
            q.sendkey("esc")
            return
        # --- maximize the window: client becomes (2,19), 796x562 ---
        mouse_move_to(q, 400, 300, 717, 45)  # maximize box (windowed)
        time.sleep(0.4)
        q.mouse_button(1)
        time.sleep(0.4)
        q.mouse_button(0)
        time.sleep(1.5)
        w, h, pix = q.screendump("webmax.ppm")
        nstrip = count_close(pix, w, h, WEB_STRIP, region=(0, 19, w, 49))
        report("web: maximize fills the work area", nstrip >= 4000,
               "%d strip px" % nstrip)
        cen = link_click_point(pix, w, h)
        if not cen:
            report("web: link click navigates to page 2", False, "link lost")
            q.sendkey("esc")
            return
        # --- link click -> page 2 ---
        mouse_move_to(q, 717, 45, int(cen[0]), int(cen[1]))
        time.sleep(0.4)
        q.mouse_button(1)
        time.sleep(0.4)
        q.mouse_button(0)
        got = wait_color(q, WEB_GREEN, 50000)
        w, h, pix = got
        ngreen = count_close(pix, w, h, WEB_GREEN) if got else 0
        nred = count_close(pix, w, h, WEB_RED) if got else -1
        report("web: link click navigates to page 2",
               ngreen >= 50000 and nred < 5000,
               "green=%d red=%d" % (ngreen, nred))
        # --- Back toolbar button (client 19,49 -> abs 21,68) ---
        mouse_move_to(q, int(cen[0]), int(cen[1]), 21, 68)
        time.sleep(0.4)
        q.mouse_button(1)
        time.sleep(0.4)
        q.mouse_button(0)
        got = wait_color(q, WEB_RED, 50000)
        w, h, pix = got
        nred = count_close(pix, w, h, WEB_RED) if got else 0
        report("web: Back button returns to page 1", nred >= 50000,
               "red=%d" % nred)
        # --- Forward toolbar button (client 54,49 -> abs 56,68) ---
        mouse_move_to(q, 21, 68, 56, 68)
        time.sleep(0.4)
        q.mouse_button(1)
        time.sleep(0.4)
        q.mouse_button(0)
        got = wait_color(q, WEB_GREEN, 50000)
        w, h, pix = got
        ngreen = count_close(pix, w, h, WEB_GREEN) if got else 0
        report("web: Forward button returns to page 2", ngreen >= 50000,
               "green=%d" % ngreen)
        # --- PgDn scrolls page 2's content ---
        _, _, before = q.screendump("webscroll.ppm")
        q.sendkey("pgdn")
        time.sleep(2)
        w, h, after = q.screendump("webscroll2.ppm")
        diff = region_diff(before, after, w, (2, 120, 790, 560))
        report("web: PgDn scrolls the page", diff > 500,
               "diff=%d px" % diff)
        q.sendkey("home")
        time.sleep(1.5)
        # --- Ctrl+T: new tab shows the start page ---
        q.mon("sendkey ctrl-t", wait=0.4)
        time.sleep(2)
        w, h, pix = q.screendump("webtab.ppm")
        nface = count_close(pix, w, h, WEB_FACE, region=(2, 87, w, 565))
        ngreen = count_close(pix, w, h, WEB_GREEN)
        report("web: Ctrl+T opens a start-page tab",
               nface >= 50000 and ngreen < 5000,
               "face=%d green=%d" % (nface, ngreen))
        # --- type an address in the new tab (bar is focused) ---
        q.type_text("10.0.2.2:8080/page1.html")
        q.sendkey("ret")
        got = wait_color(q, WEB_RED, 50000)
        w, h, pix = got
        nred = count_close(pix, w, h, WEB_RED) if got else 0
        report("web: address bar navigates the new tab", nred >= 50000,
               "red=%d" % nred)
        # --- click tab 0 (client 80,15 -> abs 82,34): page 2 kept ---
        # (cursor is still at the Forward button: moves are relative
        # and the windowed browser never re-centers the mouse)
        mouse_move_to(q, 56, 68, 82, 34)
        time.sleep(0.4)
        q.mouse_button(1)
        time.sleep(0.4)
        q.mouse_button(0)
        got = wait_color(q, WEB_GREEN, 50000)
        w, h, pix = got
        ngreen = count_close(pix, w, h, WEB_GREEN) if got else 0
        report("web: tab switch back to tab 0 (page 2 kept)",
               ngreen >= 50000, "green=%d" % ngreen)
        # --- Ctrl+W closes tab 0; the other tab (page 1) remains ---
        q.mon("sendkey ctrl-w", wait=0.4)
        got = wait_color(q, WEB_RED, 50000)
        w, h, pix = got
        nred = count_close(pix, w, h, WEB_RED) if got else 0
        ngreen = count_close(pix, w, h, WEB_GREEN) if got else -1
        report("web: Ctrl+W closes tab, other tab's page remains",
               nred >= 50000 and ngreen < 5000,
               "red=%d green=%d" % (nred, ngreen))
        # --- JS in page: Ctrl+L to jspage.html ---
        q.mon("sendkey ctrl-l", wait=0.4)
        time.sleep(0.8)
        q.type_text("10.0.2.2:8080/jspage.html")
        q.sendkey("ret")
        got = wait_color(q, (255, 255, 255), 100000,
                         region=(2, 87, 798, 565))
        w, h, pix = got
        nwhite = count_close(pix, w, h, (255, 255, 255),
                             region=(2, 87, w, 565)) if got else 0
        # Gate the glyph checks on the white body really being up:
        # on a black (hung) screen every pixel counts as "dark".
        white_ok = nwhite >= 100000
        report("web: JS page renders (white body)", white_ok,
               "white=%d" % nwhite)
        if white_ok:
            save_png(os.path.join(ROOT, "browser-max-v08.png"), w, h, pix)
            # <p id="out"> is empty in the HTML; the script fills it.
            n_out = count_dark(pix, w, (2, 113, 502, 137))
            report("web: JS textContent fills #out (throwing script "
                   "did not break later scripts)", n_out >= 120,
                   "dark=%d" % n_out)
            # document.write() appends a paragraph after #tail.
            n_wr = count_dark(pix, w, (2, 157, 502, 181))
            report("web: JS document.write appends text", n_wr >= 120,
                   "dark=%d" % n_wr)
        else:
            report("web: JS textContent fills #out (throwing script "
                   "did not break later scripts)", False, "page not up")
            report("web: JS document.write appends text", False,
                   "page not up")
        # --- close the window with its x box (abs 789,9) ---
        mouse_move_to(q, 82, 34, 789, 9)
        time.sleep(0.4)
        q.mouse_button(1)
        time.sleep(0.4)
        q.mouse_button(0)
        time.sleep(1.5)
        w, h, pix = q.screendump("webclosed.ppm")
        nstrip = count_close(pix, w, h, WEB_STRIP, region=(0, 19, w, 49))
        report("web: window close box hides the browser", nstrip < 500,
               "%d strip px" % nstrip)
        # --- reopen via the desktop Web icon (double-click) ---
        # v0.9: the Web icon is the top tile of the left icon
        # column at (10,166), tile center guest (28,184) at 800x600.
        mouse_move_to(q, 789, 9, 28, 184)
        time.sleep(0.3)
        q.mouse_button(1)
        time.sleep(0.15)
        q.mouse_button(0)
        time.sleep(0.15)
        q.mouse_button(1)
        time.sleep(0.15)
        q.mouse_button(0)
        time.sleep(1.5)
        w, h, pix = q.screendump("webreopen.ppm")
        nstrip = count_close(pix, w, h, WEB_STRIP, region=(0, 19, w, 49))
        report("web: desktop icon reopens the browser window",
               nstrip >= 4000, "%d strip px" % nstrip)
        # --- v1.0 feature + robustness suite (maximized) ---
        web_v10_checks(q)
        # --- ESC restores the text shell ---
        q.sendkey("esc")
        time.sleep(2)
        q.cmd("version", marker="v1.0.2")
        report("web: ESC restores text shell", "v1.0" in q.screen())
        # --- HTTPS page in the browser (local TLS server, :8446) ---
        q.type_text("web https://10.0.2.2:8446/tlspage.html")
        q.sendkey("ret")
        got = wait_color(q, WEB_GREEN, 50000)
        w, h, pix = got
        ngreen = count_close(pix, w, h, WEB_GREEN) if got else 0
        report("web: https page renders in browser", ngreen >= 50000,
               "green=%d" % ngreen)
        q.sendkey("esc")
        time.sleep(2)
        q.cmd("res 320 200", marker="320x200")  # leave default state
    finally:
        srv.terminate()


def check_uefi():
    """v0.9 Part C: the hybrid ISO boots under OVMF/UEFI. The
    kernel must announce the UEFI (Multiboot2) boot, reach the
    shell (text mode reinstalled by the kernel, ATA usable), and
    the GUI must come up with the desktop visible; ESC +
    gfxstats must work afterwards. NOTE: GRUB's EFI multiboot2
    loader emits NO framebuffer tag (type 8) — verified from the
    running guest's parsed state (mb2_gop.valid == 0) — so under
    UEFI the GUI uses the DISPI/VBE path on the std VGA, and the
    boot log honestly says "no GOP framebuffer tag". Runs its
    own QEMU lifecycle (the caller stops the BIOS session first;
    one monitor socket + disk cannot be shared)."""
    if not os.path.exists("/usr/share/qemu/OVMF.fd"):
        report("uefi: OVMF firmware present", False,
               "/usr/share/qemu/OVMF.fd missing")
        return
    kill_stray_qemu()
    time.sleep(0.5)
    q = QemuUefi(fresh_disk=False)
    try:
        ok = q.wait_for("Type 'help'", timeout=240)
        report("uefi: shell reached under OVMF", ok)
        if not ok:
            return
        scr = q.screen()
        report("uefi: boot log announces the UEFI boot",
               "Boot: UEFI (Multiboot2)" in scr)
        # do not inherit the shared disk's saved res (another
        # group may have left 320x200 there): pin the mode whose
        # desktop we assert below
        q.cmd("res 1024 768", marker="saved")
        q.cmd("gui")
        time.sleep(7)
        w, h, pix = q.screendump("uefi.ppm")
        report("uefi: gui comes up at 1024x768 under UEFI",
               (w, h) == (1024, 768), "%dx%d" % (w, h))
        if (w, h) == (1024, 768):
            total = w * h
            ndesk = count_gradient(pix, w, h)
            report("uefi: desktop visible under UEFI",
                   ndesk > total * 0.12, "%d/%d px" % (ndesk, total))
        q.sendkey("esc")
        time.sleep(2)
        q.cmd("gfxstats", marker="gfx:")
        report("uefi: gfxstats works after a UEFI gui session",
               "1024x768" in q.screen())
    finally:
        q.quit()


CHECKS = {
    "boot": check_boot_uptime_mem,
    "help": check_help,
    "ps": check_ps_counter,
    "date": check_date,
    "dhcp": check_dhcp,
    "net": check_network,
    "shell": check_shell_qol,
    "run": check_run_hello,
    "gui": check_gui,
    "tls": check_tls,
    "apps": check_apps,
    "jstest": check_jstest,
    "web": check_web,
}


def _elf32_image(segments, entry=0x400000):
    """Assemble a minimal ELF32 ET_EXEC image. segments is a list of
    (offset, vaddr, filesz, memsz, blob) tuples; blobs are placed at
    the given file offsets."""
    import struct
    ehsize, phent = 52, 32
    eh = bytearray(ehsize)
    eh[0:4] = b"\x7fELF"
    eh[4] = 1  # ELFCLASS32
    eh[5] = 1  # little-endian
    eh[6] = 1  # version
    struct.pack_into("<HHIIIIIHHHHHH", eh, 16,
                     2, 3, 1, entry, ehsize, 0, 0,
                     ehsize, phent, len(segments), 0, 0, 0)
    ph = b""
    size = ehsize + phent * len(segments)
    for off, vaddr, filesz, memsz, blob in segments:
        ph += struct.pack("<IIIIIIII", 1, off, vaddr, vaddr,
                          filesz, memsz, 5, 0x1000)
        size = max(size, off + len(blob))
    img = bytearray(size)
    img[0:ehsize] = eh
    img[ehsize:ehsize + len(ph)] = ph
    for off, vaddr, filesz, memsz, blob in segments:
        img[off:off + len(blob)] = blob
    return bytes(img)


def _write_wrapelf(path):
    """v1.0.2 security regression fixture (docs/security-review.md):
    segment A is an ordinary ring-3 program at 0x400000; segment B
    has vaddr inside the kernel image (0x110000) and a huge p_memsz
    (0xFF000000) chosen so the v1.0.1 loader's 32-bit segment-end
    computation wrapped below the kernel and the copy loop wrote
    segment B's bytes over kernel code. The fixed loader must
    reject the whole image before copying anything."""
    # Segment A code: write(0x400000+29, "WRAP RAN\n", 9); exit(7).
    code = b"".join([
        b"\xB8\x01\x00\x00\x00",          # mov eax,1 (write)
        b"\xBB\x1D\x00\x40\x00",          # mov ebx,0x40001D (msg)
        b"\xB9\x09\x00\x00\x00",          # mov ecx,9
        b"\xCD\x80",                      # int 0x80
        b"\xB8\x03\x00\x00\x00",          # mov eax,3 (exit)
        b"\xBB\x07\x00\x00\x00",          # mov ebx,7
        b"\xCD\x80",                      # int 0x80
        b"WRAP RAN\n",
    ])
    payload = b"\xCC" * 16  # would land on kernel code in v1.0.1
    img = _elf32_image([
        (0x1000, 0x400000, len(code), len(code), code),
        (0x2000, 0x110000, len(payload), 0xFF000000, payload),
    ])
    with open(path, "wb") as f:
        f.write(img)


def _write_leakapp(path):
    """v1.0.2 security regression fixture: a user app that calls
    write(0x100000, 64) — a pointer into kernel memory — and then
    reports the outcome using its own buffers: it prints BLOCKED
    when the syscall refuses the pointer (fixed kernel) and
    ALLOWED when the call went through (v1.0.1 also dumped the 64
    kernel bytes to the screen first)."""
    base = 0x400000
    code = bytearray()
    code += b"\xB8\x01\x00\x00\x00"       # mov eax,1 (write)
    code += b"\xBB\x00\x00\x10\x00"       # mov ebx,0x100000 (kernel!)
    code += b"\xB9\x40\x00\x00\x00"       # mov ecx,64
    code += b"\xCD\x80"                   # int 0x80
    code += b"\x83\xF8\xFF"               # cmp eax,-1
    je_at = len(code)
    code += b"\x74\x00"                   # je blocked (patched below)
    code += b"\xB8\x01\x00\x00\x00"       # mov eax,1
    a_at = len(code)
    code += b"\xBB\x00\x00\x00\x00"       # mov ebx,msg_allowed (patched)
    code += b"\xB9\x08\x00\x00\x00"       # mov ecx,8
    code += b"\xCD\x80"                   # int 0x80
    jmp_at = len(code)
    code += b"\xEB\x00"                   # jmp done (patched below)
    blocked = len(code)
    code += b"\xB8\x01\x00\x00\x00"       # mov eax,1
    b_at = len(code)
    code += b"\xBB\x00\x00\x00\x00"       # mov ebx,msg_blocked (patched)
    code += b"\xB9\x08\x00\x00\x00"       # mov ecx,8
    code += b"\xCD\x80"                   # int 0x80
    done = len(code)
    code += b"\xB8\x03\x00\x00\x00"       # mov eax,3 (exit)
    code += b"\x31\xDB"                   # xor ebx,ebx
    code += b"\xCD\x80"                   # int 0x80
    msg_blocked = len(code)
    code += b"BLOCKED\n"
    msg_allowed = len(code)
    code += b"ALLOWED\n"
    code[je_at + 1] = blocked - (je_at + 2)
    code[jmp_at + 1] = done - (jmp_at + 2)
    code[a_at + 1:a_at + 5] = (base + msg_allowed).to_bytes(4, "little")
    code[b_at + 1:b_at + 5] = (base + msg_blocked).to_bytes(4, "little")
    img = _elf32_image([(0x1000, base, len(code), len(code), bytes(code))],
                       entry=base)
    with open(path, "wb") as f:
        f.write(img)


def _write_testpic(path):
    """Write the <img> test fixture: 240x160 RGB PNG, magenta with a
    cyan top-left quarter (120x80). Stdlib only."""
    import struct, zlib
    W, H = 240, 160

    def chunk(tag, data):
        c = tag + data
        return (struct.pack(">I", len(data)) + c
                + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    rows = []
    for y in range(H):
        row = bytearray(b"\x00")
        for x in range(W):
            row += b"\x00\xff\xff" if (x < 120 and y < 80) else b"\xff\x00\xff"
        rows.append(bytes(row))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(b"".join(rows)))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    wanted = sys.argv[1:]
    # The apps check installs the built calc ELF over HTTP: stage it
    # into the testsite directory the plain server serves.
    calc_bin = os.path.join(ROOT, "src", "user", "calc")
    if os.path.exists(calc_bin):
        import shutil
        shutil.copy(calc_bin, os.path.join(HERE, "testsite", "calc"))
    # The <img> fixture is a generated artifact too (240x160 PNG,
    # magenta with a cyan top-left quarter): regenerate it when a
    # checkout does not carry the binary (text-only export).
    testpic = os.path.join(HERE, "testsite", "testpic.png")
    if not os.path.exists(testpic):
        _write_testpic(testpic)
    # v1.0.2 security regression fixtures for the apps group
    # (docs/security-review.md): regenerated every run so they can
    # never drift from the generators above.
    _write_wrapelf(os.path.join(HERE, "testsite", "wrapelf"))
    _write_leakapp(os.path.join(HERE, "testsite", "leak"))
    server = subprocess.Popen(
        ["python3", "-m", "http.server", "80", "--directory",
         os.path.join(HERE, "testsite"), "--bind", "0.0.0.0"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fx = os.path.join(HERE, "tls", "fixtures")
    tls_servers = []
    for port, crt, key in ((8443, "server.crt", "server.key"),
                           (8444, "bad-server.crt", "bad-server.key")):
        tls_servers.append(subprocess.Popen(
            ["python3", os.path.join(HERE, "tls", "tls_server.py"),
             str(port), os.path.join(fx, crt), os.path.join(fx, key),
             TLS_MARKER],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    if "apps" in wanted:
        # TLS file server on :8445 serving calc for `install https://`.
        tls_servers.append(subprocess.Popen(
            ["python3", os.path.join(HERE, "tls", "file_server.py"),
             "8445", os.path.join(fx, "server.crt"),
             os.path.join(fx, "server.key"), calc_bin],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    if "web" in wanted:
        # TLS file server on :8446 serving an HTML page for the
        # browser's HTTPS check (test-CA cert, bundled root).
        tls_servers.append(subprocess.Popen(
            ["python3", os.path.join(HERE, "tls", "file_server.py"),
             "8446", os.path.join(fx, "server.crt"),
             os.path.join(fx, "server.key"),
             os.path.join(HERE, "testsite", "tlspage.html")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    time.sleep(1)
    try:
        q = boot(fresh_disk=("fs" in wanted or "apps" in wanted
                             or not wanted))
        names = wanted or ["boot", "help", "ps", "date", "dhcp", "shell", "run", "net"]
        for name in names:
            if name == "fs":
                check_fs(q)
                q.quit()
                q = boot(fresh_disk=False)
                check_fs_after_reboot(q)
            elif name == "uefi":
                q.quit()
                check_uefi()
                q = boot(fresh_disk=False)
            elif name in CHECKS:
                CHECKS[name](q)
            else:
                print(f"unknown check: {name}")
        q.quit()
    finally:
        server.terminate()
        for s in tls_servers:
            s.terminate()
    failed = [n for n, ok in RESULTS if not ok]
    print("=" * 50)
    print(f"{len(RESULTS) - len(failed)}/{len(RESULTS)} checks passed")
    if failed:
        print("FAILED:", ", ".join(failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
