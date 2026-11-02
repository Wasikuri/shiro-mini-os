ENTRY(_start)
SECTIONS
{
  . = 1M;
  .text BLOCK(4K) : ALIGN(4K)
  {
    *(.multiboot)
    *(.multiboot2)
    *(.text)
  }
  .rodata BLOCK(4K) : ALIGN(4K)
  {
    *(.rodata)
  }
  .data BLOCK(4K) : ALIGN(4K)
  {
    *(.data)
  }
  .bss BLOCK(4K) : ALIGN(4K)
  {
    *(COMMON)
    *(.bss)
  }
  _kernel_end = .;
  kernel_end = _kernel_end;
}
