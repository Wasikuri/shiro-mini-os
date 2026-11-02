/* Multiboot header + entry point (GNU Assembler) */
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

/* Multiboot2 header (v0.9): lets the same kernel boot via GRUB's
 * multiboot2 command on BIOS and on UEFI, where the framebuffer
 * tag (boot info type 8) carries the GOP surface. Requests a
 * 1024x768x32 framebuffer; the loader may substitute its own
 * mode, and the kernel takes whatever the tag reports. */
.set MB2_MAGIC, 0xE85250D6
.section .multiboot2
.align 8
mb2_header_start:
.long MB2_MAGIC
.long 0                                     /* architecture: i386 */
.long mb2_header_end - mb2_header_start     /* header length */
.long -(MB2_MAGIC + (mb2_header_end - mb2_header_start))
.align 8
.short 5                                    /* tag: framebuffer */
.short 0                                    /* flags */
.long 20                                    /* tag size */
.long 1024                                  /* width */
.long 768                                   /* height */
.long 32                                    /* depth */
.align 8
.short 0                                    /* tag: end */
.short 0
.long 8
mb2_header_end:

.section .bss
.align 16
stack_bottom:
.skip 16384 /* 16 KiB stack */
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp
    /* pass multiboot magic (eax) and info pointer (ebx) to kernel_main */
    push %ebx
    push %eax
    call kernel_main
    add $8, %esp
    cli
1:  hlt
    jmp 1b
.size _start, . - _start
