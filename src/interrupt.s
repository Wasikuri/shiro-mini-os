/* Interrupt stubs, descriptor-table loaders, user-mode switch (GAS) */

.section .bss
.align 4
saved_kernel_esp: .skip 4

.section .text

/* void gdt_flush(uint32_t gdt_ptr) */
.globl gdt_flush
gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ljmp $0x08, $1f
1:  ret

/* void idt_load(uint32_t idt_ptr) */
.globl idt_load
idt_load:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

/* void tss_load(void) - TSS is GDT index 5 (selector 0x28) */
.globl tss_load
tss_load:
    mov $0x28, %ax
    ltr %ax
    ret

/* ---- exceptions ---- */
.macro EXC_NOERR n
.globl exc\n
exc\n:
    push $0
    push $\n
    jmp exc_common
.endm

.macro EXC_ERR n
.globl exc\n
exc\n:
    push $\n
    jmp exc_common
.endm

EXC_NOERR 0
EXC_NOERR 1
EXC_NOERR 2
EXC_NOERR 3
EXC_NOERR 4
EXC_NOERR 5
EXC_NOERR 6
EXC_NOERR 7
EXC_ERR   8
EXC_NOERR 9
EXC_ERR   10
EXC_ERR   11
EXC_ERR   12
EXC_ERR   13
EXC_ERR   14
EXC_NOERR 15
EXC_NOERR 16
EXC_ERR   17
EXC_NOERR 18
EXC_NOERR 19
EXC_NOERR 20
EXC_NOERR 21
EXC_NOERR 22
EXC_NOERR 23
EXC_NOERR 24
EXC_NOERR 25
EXC_NOERR 26
EXC_NOERR 27
EXC_NOERR 28
EXC_NOERR 29
EXC_NOERR 30
EXC_NOERR 31

exc_common:
    pusha
    push %esp
    call exception_handler
    add $4, %esp
    popa
1:  cli
    hlt
    jmp 1b

/* ---- IRQ0: timer -> scheduler. sched_timer(ctx) returns new ctx ---- */
.globl irq0_stub
irq0_stub:
    pusha
    push %esp
    call sched_timer
    mov %eax, %esp
    popa
    iret

/* ---- IRQ1: keyboard ---- */
.globl irq1_stub
irq1_stub:
    pusha
    call keyboard_irq_handler
    popa
    iret

/* ---- IRQ12: PS/2 mouse ---- */
.globl irq12_stub
irq12_stub:
    pusha
    call mouse_irq_handler
    popa
    iret

/* ---- any other IRQ: ignore ---- */
.globl irq_generic_stub
irq_generic_stub:
    iret

/* ---- int 0x80 syscall (trap gate: IF stays on in the handler) ---- */
.globl syscall_stub
syscall_stub:
    pusha
    push %esp
    call syscall_handler
    add $4, %esp
    popa
    iret

/* int enter_usermode(uint32_t entry, uint32_t user_esp)
 * Saves the kernel context so exit_to_kernel() can resume here.
 * Returns via the normal C path once the user program exits. */
.globl enter_usermode
enter_usermode:
    mov 4(%esp), %eax       /* entry point */
    mov 8(%esp), %ecx       /* user stack pointer */
    push %ebx
    push %esi
    push %edi
    push %ebp
    mov %esp, saved_kernel_esp
    push $0x23              /* user SS (user data 0x20 | RPL3) */
    push %ecx               /* user ESP */
    push $0x202             /* EFLAGS: IF=1, reserved bit */
    push $0x1B              /* user CS (user code 0x18 | RPL3) */
    push %eax               /* EIP */
    iret

/* void exit_to_kernel(void) - abandons the current (syscall/TSS)
 * stack and returns to the caller of enter_usermode(). */
.globl exit_to_kernel
exit_to_kernel:
    mov saved_kernel_esp, %esp
    pop %ebp
    pop %edi
    pop %esi
    pop %ebx
    ret
