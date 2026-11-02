CC = gcc
CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-pic \
         -Ithird_party/shim -Ithird_party/bearssl/inc
LDFLAGS = -m elf_i386 -T src/linker.ld -nostdlib

OBJS = src/boot.o src/kernel.o src/mb2.o src/cpu.o src/interrupt.o src/mm.o \
       src/sched.o src/fs.o src/net.o src/exec.o src/gui.o src/rmtrap.o \
       src/hello_bin.o \
       src/tls.o src/libc_shims.o src/apps.o \
       src/browser.o src/jstest.o src/js_shims.o src/js_math.o \
       src/js_setjmp.o src/duktape.o src/stb_image.o src/wallpaper_bin.o

# --- BearSSL 0.6 (third_party/bearssl, MIT) for the TLS layer ---
# src/rand/sysrng.c is excluded on purpose: tls.c provides its own
# br_prng_seeder_system(). src/settings.c (brssl tool only) is not in
# any subdirectory and is not compiled either.
BR_DIR = third_party/bearssl
# OS autodetection is disabled (no /dev/urandom, no time(), own
# RDRAND handling in tls.c): tls.c is the only entropy source.
# The guest CPU (QEMU qemu32) has no SSE2/AES-NI, but BearSSL's
# inner.h enables BR_SSE2/BR_AES_X86NI for any x86 GCC and the TLS
# engine then runs e.g. ChaCha20-SSE2 unconditionally -> #UD crash.
# Pin every x86 SIMD path OFF; portable C implementations only.
BR_CFLAGS = -m32 -march=i686 -std=gnu99 -ffreestanding -O2 \
            -fno-stack-protector -fno-pic \
            -Ithird_party/shim -I$(BR_DIR)/inc -I$(BR_DIR)/src -w \
            -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 \
            -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -DBR_RDRAND=0 \
            -DBR_SSE2=0 -DBR_AES_X86NI=0
BR_SRCS = $(filter-out $(BR_DIR)/src/rand/sysrng.c, $(wildcard $(BR_DIR)/src/*/*.c))
BR_OBJS = $(patsubst $(BR_DIR)/src/%.c, build/br/%.o, $(BR_SRCS))

all: shiro-os.iso src/user/calc

build/kernel.bin: $(OBJS) $(BR_OBJS) src/linker.ld
	mkdir -p build isodir/boot/grub
	ld $(LDFLAGS) -o build/kernel.bin $(OBJS) $(BR_OBJS)

build/br/%.o: $(BR_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BR_CFLAGS) -c $< -o $@

src/boot.o: src/boot.s
	$(CC) $(CFLAGS) -c src/boot.s -o src/boot.o

src/interrupt.o: src/interrupt.s
	$(CC) $(CFLAGS) -c src/interrupt.s -o src/interrupt.o

src/rmtrap.o: src/rmtrap.s
	$(CC) $(CFLAGS) -c src/rmtrap.s -o src/rmtrap.o

src/%.o: src/%.c src/os.h src/net.h src/tls.h
	$(CC) $(CFLAGS) -c $< -o $@

src/tls.o: src/tls_anchors.h third_party/bearssl/inc/bearssl.h

# --- Duktape 2.7.0 (third_party/duktape-2.7.0, MIT) + JS shims ---
# Compiled against the shim headers in src/shim (the engines' view of
# libc); the shim implementations are src/js_shims.c + js_math.c +
# js_setjmp.s, built by the generic rule / the rule below.
src/duktape.o: third_party/duktape-2.7.0/src/duktape.c
	$(CC) $(CFLAGS) -O1 -w -I src/shim -I third_party/duktape-2.7.0/src -c $< -o $@

src/jstest.o: src/jstest.c src/os.h
	$(CC) $(CFLAGS) -I src/shim -I third_party/duktape-2.7.0/src -c $< -o $@

src/browser.o: src/browser.c src/os.h src/net.h
	$(CC) $(CFLAGS) -I src/shim -I third_party/duktape-2.7.0/src -c $< -o $@

src/js_setjmp.o: src/js_setjmp.s
	$(CC) -m32 -c $< -o $@

# --- stb_image v2.30 (third_party/stb, public domain/MIT): image
# decoding for the GUI wallpaper; own TU with the shim libc view.
src/stb_image.o: src/stb_image.c third_party/stb/stb_image.h
	$(CC) $(CFLAGS) -O1 -w -I src/shim -I third_party/stb -c $< -o $@

# --- desktop wallpaper asset (synthesized by tools/make_wallpaper.py),
# embedded like the hello binary ---
# The JPEG is a generated artifact: regenerate it from the synthesis
# script when a checkout does not carry it (e.g. a text-only export).
# Requires python3 + PIL.
assets/wallpaper.jpg:
	python3 tools/make_wallpaper.py assets/wallpaper.jpg

src/wallpaper_bin.o: assets/wallpaper.jpg
	ld -m elf_i386 -r -b binary -o src/wallpaper_bin.o assets/wallpaper.jpg

# --- sample user program (stage 7): freestanding ELF, embedded ---
src/user/hello: src/user/hello.c src/user/user.ld
	$(CC) -m32 -std=gnu99 -ffreestanding -O2 -Wall -fno-stack-protector -fno-pic \
	    -nostdlib -static -no-pie -T src/user/user.ld -o src/user/hello src/user/hello.c

src/hello_bin.o: src/user/hello
	ld -m elf_i386 -r -b binary -o src/hello_bin.o src/user/hello

# --- installable sample app (v0.6.1): plain ELF at a stable path;
# the QEMU harness serves this file from tests/testsite and the
# guest pulls it in with `install http://10.0.2.2/calc` ---
src/user/calc: src/user/calc.c src/user/user.ld
	$(CC) -m32 -std=gnu99 -ffreestanding -O2 -Wall -fno-stack-protector -fno-pic \
	    -nostdlib -static -no-pie -T src/user/user.ld -o src/user/calc src/user/calc.c

shiro-os.iso: build/kernel.bin grub.cfg
	mkdir -p isodir/boot/grub
	cp build/kernel.bin isodir/boot/kernel.bin
	cp grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o shiro-os.iso isodir

disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=8

run: shiro-os.iso disk.img
	qemu-system-i386 -cdrom shiro-os.iso -boot d -m 128 -vga std \
	    -drive file=disk.img,format=raw,if=ide,index=0 \
	    -device e1000,netdev=net0 -netdev user,id=net0

clean:
	rm -rf build isodir shiro-os.iso src/*.o src/user/hello src/user/calc
