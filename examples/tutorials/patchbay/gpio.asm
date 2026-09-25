; Patchbay GPIO demo.
; Load into cpu0.memory at 0x0000 with Load + Cold Reset, then run the rack.
; GPIO P0 is input; P1 SER, P2 SRCLK, P3 RCLK, P4 /OE, P5 /MR.
                org $0000

GPIO_DATA_IN    equ $40
GPIO_DATA_OUT   equ $41
GPIO_DIRECTION  equ $42

start:
                ld sp,$fffe
                ld a,$3e
                out (GPIO_DIRECTION),a ; P1..P5 outputs; P0 remains input
                ld e,0                 ; current hexadecimal digit
                call display_digit

; Require a release before accepting a press. This makes one increment per
; click instead of repeatedly incrementing while the button is held.
wait_release:
                in a,(GPIO_DATA_IN)
                and 1
                jr nz,wait_release

wait_press:
                in a,(GPIO_DATA_IN)
                and 1
                jr z,wait_press

                inc e
                ld a,e
                and $0f
                ld e,a
                call display_digit
                jr wait_release

; Shift the active-high a..g pattern into the 74HC595, then latch it.
display_digit:
                ld a,e
                ld c,a
                ld b,0
                ld hl,digit_patterns
                add hl,bc
                ld d,(hl)
                ld c,GPIO_DATA_OUT
                ld b,8
shift_next:
                ld a,$20               ; /MR inactive; /OE active
                sla d
                jr nc,shift_zero
                or 2                   ; SER high
shift_zero:
                out (c),a
                or 4
                out (c),a              ; SRCLK rising edge
                djnz shift_next
                ld a,$28
                out (c),a              ; RCLK rising edge
                ret

digit_patterns:
                db $3f,$06,$5b,$4f,$66,$6d,$7d,$07
                db $7f,$6f,$77,$7c,$39,$5e,$79,$71
