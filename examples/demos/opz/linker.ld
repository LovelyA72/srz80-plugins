OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) *(.srodata*) }
  /* ROM0 lives at address 0 and must stay clear of the RAM card at 0x4000. */
  ASSERT(. <= 0x4000, "Firmware overlaps Work RAM")
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) }
}
