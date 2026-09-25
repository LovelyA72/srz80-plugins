OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY
{
  ROM (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00004000
  RAM (rwx) : ORIGIN = 0x00004000, LENGTH = 0x00004000
}
SECTIONS
{
  .text : { *(.text.start) *(.text*) } > ROM
  .rodata : { *(.rodata*) *(.srodata*) } > ROM
  ASSERT(. <= ORIGIN(RAM), "Player firmware overlaps work RAM")

  .data : { __data_start = .; *(.data*) *(.sdata*) __data_end = .; } > RAM AT> ROM
  __data_load = LOADADDR(.data);

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss*) *(.sbss*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } > RAM

  # The stack grows down from the top of RAM.  ORIGIN + LENGTH lands on
  # 0x8000, the CTC interrupt-acknowledge alias; only sp-4.. is ever touched,
  # so the alias itself is never read or written as stack.
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);

  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) }
}
