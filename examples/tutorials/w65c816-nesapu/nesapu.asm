; A C major phrase played by the four NES APU channels one at a time.
;
; The processor resets in emulation mode, then XCE switches it into native
; mode.  It uses the 16-bit accumulator to initialize state before returning
; to 8-bit register widths for APU byte writes.  RAM occupies $0000-$7fff, the
; program ROM is mapped at $8000, and the APU register block answers at $4000-$4017.
;
;   pulse 1  - 50% duty, one octave above the written pitch
;   pulse 2  - 25% duty, at pitch
;   triangle - at pitch
;   noise    - short mode at rate $f, adding percussion
;
; Each channel plays the sixteen quarter-note slots (four bars) in turn.  Each
; quarter note lasts four sixteenth-note ticks at 240 bpm.
;
; Build with 64tass: see README.md.

.cpu "65816"

; ---- player state, in the RAM card ------------------------------------------
song_index = $0400
ticks_left = $0401
note_cache = $0402
channel_index = $0403
sequence_size = 32

* = $8000

; ---- APU registers ----------------------------------------------------------
pulse1_ctl = $4000
pulse1_lo = $4002
pulse1_hi = $4003
pulse2_ctl = $4004
pulse2_lo = $4006
pulse2_hi = $4007
tri_ctl = $4008
tri_lo = $400a
tri_hi = $400b
noise_ctl = $400c
noise_rate = $400e
noise_len = $400f
apu_status = $4015

start
    sei
    cld
    clc
    xce                 ; switch from reset emulation mode to native mode
    phk
    plb                 ; absolute data accesses now use the program's bank
    rep #$30             ; native 16-bit accumulator and index registers
.al
.xl
    lda #$0000
    sta song_index       ; one 16-bit store clears song_index and ticks_left
    sta note_cache       ; clear the cache and its adjacent byte
    sep #$30             ; APU registers and score entries are bytes
.as
.xs

    lda #$00
    sta apu_status

    ; Hold the tonal counters for the whole note.  The status register remains
    ; clear until tick starts the first note.
    lda #$bf            ; pulse 1: 50% duty, halt length, constant volume 15
    sta pulse1_ctl
    lda #$7f            ; pulse 2: 25% duty, halt length, constant volume 15
    sta pulse2_ctl
    lda #$ff            ; triangle: hold/reload the linear counter
    sta tri_ctl
    lda #$38            ; noise: halt length, constant volume 8
    sta noise_ctl
    lda #$8f            ; short mode at the fastest noise rate
    sta noise_rate

frame
    jsr tick
    jsr tick_wait
    jmp frame

; One tick is a sixteenth note (62.5 ms) at 240 bpm.  The 200-iteration inner
; loop takes about 1,000 cycles; 110 calls make a tick of about 112,000 cycles.
tick_delay
    ldy #$c8
delay_inner
    dey
    bne delay_inner
    rts

tick_wait
    ldx #$6e
delay_outer
    jsr tick_delay
    dex
    bne delay_outer
    rts

; ---- player -----------------------------------------------------------------
; Advance the score for the selected channel.  At the end of a pass, silence
; the last rest and rotate to the next channel before the phrase restarts.
tick
    lda ticks_left
    beq tick_next
    dec ticks_left
    rts

tick_next
    ldx song_index
    lda song,x
    sta note_cache
    inx
    lda song,x
    sec
    sbc #$01            ; the note loaded below occupies the current tick
    sta ticks_left
    inx
    cpx #sequence_size
    bcc tick_store_index
    ldx #$00
    inc channel_index
    lda channel_index
    cmp #$04
    bcc tick_store_index
    lda #$00
    sta channel_index
tick_store_index
    stx song_index

    lda note_cache
    cmp #$ff
    beq tick_rest
    tay                 ; keep the pitch index while selecting one channel
    lda channel_index
    beq tick_pulse1
    cmp #$01
    beq tick_pulse2
    cmp #$02
    beq tick_triangle

    lda #$08            ; noise only
    sta apu_status
    lda #$00            ; restart the noise envelope for each note
    sta noise_len
    rts

tick_pulse1
    lda #$01            ; pulse 1 only
    sta apu_status
    lda pulse1_period_lo,y
    sta pulse1_lo
    lda pulse1_period_hi,y
    sta pulse1_hi
    rts

tick_pulse2
    lda #$02            ; pulse 2 only
    sta apu_status
    lda pulse2_period_lo,y
    sta pulse2_lo
    lda pulse2_period_hi,y
    sta pulse2_hi
    rts

tick_triangle
    lda #$04            ; triangle only
    sta apu_status
    lda tri_period_lo,y
    sta tri_lo
    lda tri_period_hi,y
    sta tri_hi
    rts

tick_rest
    lda #$00            ; disable all four channels during a rest
    sta apu_status
    rts

; ---- note tables ------------------------------------------------------------
; Indices 0-8 are C4,D4,E4,F4,G4,A4,B4,C5,D5.  These pulse periods account for
; the card clocking its pulse timers once per CPU cycle; the triangle timer
; follows the chip's CPU / (32 * (period + 1)) frequency formula.
pulse1_period_lo
    .byte $ab,$7c,$52,$3f,$1c,$fd,$e1,$d5,$bd
pulse1_period_hi
    .byte $01,$01,$01,$01,$01,$00,$00,$00,$00
pulse2_period_lo
    .byte $56,$f9,$a6,$80,$3a,$fb,$c4,$ab,$7c
pulse2_period_hi
    .byte $03,$02,$02,$02,$02,$01,$01,$01,$01
tri_period_lo
    .byte $d5,$bd,$a9,$9f,$8e,$7e,$70,$6a,$5e
tri_period_hi
    .byte $00,$00,$00,$00,$00,$00,$00,$00,$00

; ---- arrangement ------------------------------------------------------------
; c  d  e  f  | g  -  -  -  | g  f  e  d  | c  -  -  -
; Each slot is a quarter note (four ticks); $ff is a rest.  The whole phrase
; is played first on pulse 1, then pulse 2, triangle, and finally noise.
song
    .byte $00,$04, $01,$04, $02,$04, $03,$04
    .byte $04,$04, $ff,$04, $ff,$04, $ff,$04
    .byte $04,$04, $03,$04, $02,$04, $01,$04
    .byte $00,$04, $ff,$04, $ff,$04, $ff,$04

* = $fffa
    .word start
    .word start
    .word start
