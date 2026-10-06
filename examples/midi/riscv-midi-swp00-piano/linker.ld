OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.srodata*) }
  ASSERT(. <= 0x8000, "Firmware overlaps Work RAM")
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) }
}
