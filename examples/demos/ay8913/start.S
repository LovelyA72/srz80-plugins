    .option norvc
    .section .text.start
    .global _start
_start:
    la sp, __stack_top

    # Machine trap vector.  The C handler is installed before interrupts are
    # unmasked; mtvec mode 0 is a direct vector.
    la t0, trap_handler
    csrw mtvec, t0

    # Unmask the machine external interrupt source.  mstatus.MIE stays clear
    # until main has parsed the song and programmed the CTC, so no interrupt can
    # arrive before the player state is ready.
    li t0, 0x800
    csrs mie, t0

    # Copy .data from its ROM load image into RAM.
    la t0, __data_load
    la t1, __data_start
    la t2, __data_end
1:  bgeu t1, t2, 2f
    lbu t3, 0(t0)
    sb t3, 0(t1)
    addi t0, t0, 1
    addi t1, t1, 1
    j 1b

    # Zero .bss.
2:  la t1, __bss_start
    la t2, __bss_end
3:  bgeu t1, t2, 4f
    sb zero, 0(t1)
    addi t1, t1, 1
    j 3b

4:  call main
5:  j 5b
