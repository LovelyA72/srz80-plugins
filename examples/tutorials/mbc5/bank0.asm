; SRZ80 MBC5 demonstration: fixed bank and mapper control code.

            org     $0000

UART_DATA   equ     $80
RAM_ENABLE  equ     $0000
ROM_BANK_LO equ     $2000
ROM_BANK_HI equ     $3000
RAM_BANK    equ     $4000
EXT_RAM     equ     $A000
STATIC_ROM  equ     $E000

start:
            di
            ld      sp,$E000
            ld      hl,banner
            call    puts

            ; Bank 1 is selected after reset.
            ld      hl,$4000
            call    puts
            ld      a,2
            ld      (ROM_BANK_LO),a
            xor     a
            ld      (ROM_BANK_HI),a
            ld      hl,$4000
            call    puts

            ; Enable external RAM, then prove that banks 0 and 1 are distinct.
            ld      a,$0A
            ld      (RAM_ENABLE),a
            xor     a
            ld      (RAM_BANK),a
            ld      a,'0'
            ld      (EXT_RAM),a
            ld      a,1
            ld      (RAM_BANK),a
            ld      a,'1'
            ld      (EXT_RAM),a

            ld      hl,ram_text
            call    puts
            xor     a
            ld      (RAM_BANK),a
            ld      a,(EXT_RAM)
            out     (UART_DATA),a
            ld      a,' '
            out     (UART_DATA),a
            ld      a,1
            ld      (RAM_BANK),a
            ld      a,(EXT_RAM)
            out     (UART_DATA),a
            call    newline

            ld      hl,static_text
            call    puts
            ld      a,(STATIC_ROM)
            out     (UART_DATA),a
            call    newline

halted:
            halt
            jp      halted

puts:
            ld      a,(hl)
            or      a
            ret     z
            out     (UART_DATA),a
            inc     hl
            jr      puts

newline:
            ld      a,13
            out     (UART_DATA),a
            ld      a,10
            out     (UART_DATA),a
            ret

banner:     defm    "MBC5 fixed bank 0",13,10,0
ram_text:   defm    "External RAM banks: ",0
static_text:defm    "Separate ROM card: ",0

            defs    $4000-$,0
