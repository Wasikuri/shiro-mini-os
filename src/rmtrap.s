/* rmtrap.s - real-mode BIOS query trampoline (v1.0.1).
 *
 * The VBE/DISPI path in gui.c needs the true linear-framebuffer
 * scanline pitch of each mode. On QEMU that pitch is exactly
 * width*4, but some firmware/device pairs (notably VirtualBox)
 * pad scanlines, and the ONLY authoritative source of the pitch
 * is the VBE mode info block returned by BIOS int 10h AX=4F01 -
 * which a protected-mode kernel cannot call directly. This file
 * provides:
 *
 *   int vbe_rm_query(void)   - 32-bit C-callable entry. Saves the
 *       full CPU context, switches to a private flat GDT, drops
 *       through a 16-bit segment into real mode, runs the blob's
 *       query routine (4F00 + 4F01 for every listed mode), then
 *       returns exactly the way it came. Interrupts are masked
 *       for the whole excursion.
 *
 *   vbe_rm_blob_start..end   - position-dependent blob that the
 *       C side copies to physical 0x7400 before calling. It uses
 *       ONLY fixed low-memory addresses, so it survives the copy:
 *         0x6000  parameter block (layout in gui.c)
 *         0x6100  private GDT (built by the C side)
 *         0x7000  stack top (grows down, stays below 0x7000)
 *         0x8000  VBE info block (512 B)
 *         0x9000  mode info blocks (48 x 256 B)
 *       All of this sits in free conventional memory: above the
 *       BIOS data area, below the VGA aperture, and the kernel's
 *       page tables identity-map it supervisor RW.
 *
 * Only ever called on BIOS boots (never under UEFI, where real
 * mode no longer exists) and only from task context.
 */

    .text

/* ---------------- 32-bit entry (normal kernel code) ----------- */
    .globl vbe_rm_query
    .type vbe_rm_query, @function
vbe_rm_query:
    pushf
    pushal
    cli
    sgdt 0x600C             /* saved GDTR  -> param+12 */
    sidt 0x6014             /* saved IDTR  -> param+20 */
    mov %cr0, %eax
    mov %eax, 0x6004        /* saved CR0   -> param+4  */
    mov %esp, 0x6008        /* saved ESP   -> param+8  */
    movl $vbe_rm_resume, 0x6020   /* resume EIP -> param+32 */
    mov %cs, %ax
    mov %ax, 0x6024         /* resume CS   -> param+36 */
    lgdt 0x60F0             /* private GDT pseudo-descriptor */
    ljmp $0x18, $0x7400     /* -> blob PM16 entry (code16 sel) */
vbe_rm_resume:
    popal
    popf
    ret

/* ---------------- the blob (copied to 0x7400) ----------------- */
    .globl vbe_rm_blob_start
    .globl vbe_rm_blob_end
    .set RM_BASE, 0x7400
    .set rm_main_abs,    RM_BASE + (rm_main - vbe_rm_blob_start)
    .set rm_resume_abs,  RM_BASE + (rm_resume32 - vbe_rm_blob_start)

vbe_rm_blob_start:
    .code16
    /* PM16 entry (blob offset 0): reload data segments with the
     * private 16-bit data selector, switch to the low stack,
     * then clear PG|PE in one CR0 write (the code executing here
     * is identity-mapped, so the fetch stream survives) and far
     * jump into real-mode code at segment 0. */
    mov $0x20, %ax          /* data16 selector */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7000, %esp
    lidt 0x60E0             /* real-mode IDT: base 0, limit 0x3FF.
                               The CPU (and QEMU) vector real-mode
                               INTs through the IDTR, whose base is
                               the kernel's protected-mode IDT at
                               this point - without this, int 10h
                               reads a PM gate as an IVT entry and
                               derails. Restored on the way back. */
    mov %cr0, %eax
    and $0x7FFFFFFE, %eax   /* clear PG (bit31) and PE (bit0) */
    mov %eax, %cr0
    ljmp $0x0000, $rm_main_abs

rm_main:
    /* Real mode. DS=ES=SS=0; all absolute accesses below use DS
     * while it is still 0, and %ss: overrides once DS holds the
     * mode-list segment. */
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7000, %sp
    xor %bx, %bx            /* BX = mode count */
    cld
    /* VBE info block at ES:DI = 0x0000:0x8000, signature 'VBE2' */
    movw $0x4256, %es:0x8000    /* 'V','B' */
    movw $0x3245, %es:0x8002    /* 'E','2' */
    mov $0x4F00, %ax
    mov $0x8000, %di
    int $0x10
    mov %ax, 0x601C         /* status -> param+28 (DS=0 here) */
    cmp $0x004F, %ax
    jne rm_done
    /* mode list far pointer at VBE info +14 */
    mov 0x800E, %si         /* list offset */
    mov 0x8010, %ax         /* list segment */
    mov %ax, %ds            /* DS:SI -> mode list */
rm_loop:
    lodsw                   /* AX = mode number */
    cmp $0xFFFF, %ax
    je rm_done
    cmp $48, %bx
    jae rm_done
    mov %ax, %cx            /* CX = mode for 4F01 */
    mov %bx, %di
    shl $1, %di
    mov %cx, %ss:0x6040(%di)    /* modes[count] -> param+64 */
    xor %ax, %ax
    mov %ax, %es            /* ES = 0 (BIOS may clobber it) */
    mov %bx, %di
    shl $8, %di             /* count * 256 */
    add $0x9000, %di        /* DI -> this mode's info block */
    push %cx
    push %di
    mov $256, %cx
    xor %ax, %ax
    rep stosb               /* zero the block for determinism */
    pop %di
    pop %cx
    mov $0x4F01, %ax
    int $0x10
    inc %bx
    jmp rm_loop
rm_done:
    xor %ax, %ax
    mov %ax, %ds            /* DS = 0 again */
    mov %bx, 0x601E         /* count -> param+30 */
    mov 0x6004, %eax        /* saved CR0 */
    mov %eax, %cr0          /* PE|PG back on; next fetch is the
                               hand-encoded far jump below, from
                               identity-mapped low memory */
    .byte 0x66, 0xEA        /* ljmp ptr16:32 */
    .long rm_resume_abs
    .word 0x0008            /* private code32 selector */

    .code32
rm_resume32:
    /* Protected again, private flat segments. Rebuild the
     * kernel stack, restore its GDT/IDT, and far-jump back to
     * the 32-bit entry's resume point with the kernel CS. */
    mov $0x10, %ax          /* data32 selector */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    mov 0x6008, %esp        /* saved ESP */
    lgdt 0x600C             /* saved GDTR */
    lidt 0x6014             /* saved IDTR */
    ljmp *0x6020            /* far ptr: EIP @param+32, CS @param+36 */
vbe_rm_blob_end:
