OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.srodata*) }
  /* Runtime globals live in the Work RAM card at 0x4000. */
  . = 0x4000;
  .bss (NOLOAD) : { *(.bss*) *(.sbss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) }
}
