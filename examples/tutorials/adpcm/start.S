    .option norvc
    .section .text.start
    .globl _start
_start:
    li sp, 0x14000
    call main
1:  j 1b
