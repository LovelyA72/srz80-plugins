OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.srodata*) }
  ASSERT(. <= 0x4000, "Firmware overlaps Work RAM")
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) }
}
