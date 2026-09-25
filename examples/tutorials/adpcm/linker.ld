OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { ROM (rx) : ORIGIN = 0, LENGTH = 64K }
SECTIONS {
    .text : { *(.text.start) *(.text*) } > ROM
    .rodata : { *(.rodata*) } > ROM
    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) }
}
