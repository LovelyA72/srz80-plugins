OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.srodata*) }
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) }
}
