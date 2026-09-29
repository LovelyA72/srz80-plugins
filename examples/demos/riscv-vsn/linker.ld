/* SPDX-License-Identifier: MIT */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY
{
  ROM (rx) : ORIGIN = 0x00000000, LENGTH = 64K
  /* Reserve low RAM for graphics. Firmware and stack start at 0xa0000. */
  WORK (rw) : ORIGIN = 0x000a0000, LENGTH = 384K
}
SECTIONS
{
  .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) *(.srodata*) } > ROM
  .data : {
    . = ALIGN(4);
    _data_start = .;
    *(.data*) *(.sdata*)
    . = ALIGN(4);
    _data_end = .;
  } > WORK AT > ROM
  _data_load = LOADADDR(.data);
  .bss (NOLOAD) : {
    . = ALIGN(4);
    _bss_start = .;
    *(.bss*) *(.sbss*) *(COMMON)
    . = ALIGN(4);
    _bss_end = .;
  } > WORK
  _stack_top = ORIGIN(WORK) + LENGTH(WORK);
  ASSERT(_bss_end <= _stack_top - 4096, "Leave at least 4 KiB for the stack")
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) *(.eh_frame*) }
}
