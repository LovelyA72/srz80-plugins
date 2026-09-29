OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY
{
  ROM (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00010000
  RAM (rwx) : ORIGIN = 0x00010000, LENGTH = 0x00020000
}
SECTIONS
{
  .text : { *(.text.start) *(.text*) *(.rodata*) *(.srodata*) } > ROM
  .data : { *(.data*) *(.sdata*) } > RAM
  .bss (NOLOAD) : { *(.bss*) *(.sbss*) *(COMMON) } > RAM
  _stack_top = ORIGIN(RAM) + LENGTH(RAM);
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) }
}
