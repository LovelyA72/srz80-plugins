; Z80 PCM DAC FIFO demonstration for SRZ80.
;
; The PCM DAC is at I/O ports $D0-$D4.  This program streams a 32-byte,
; unsigned-8-bit waveform into its FIFO faster than the FIFO consumes it.

            org     $0000

DAC_DATA    equ     $D0
DAC_CONTROL equ     $D1
DAC_RATE_LO equ     $D3
DAC_RATE_HI equ     $D4

            di

            ; Select FIFO mode and set its playback rate to 22050 Hz.
            ld      a,1
            out     (DAC_CONTROL),a
            ld      a,$22
            out     (DAC_RATE_LO),a
            ld      a,$56
            out     (DAC_RATE_HI),a

; Refill the FIFO with one waveform period.  The DAC safely discards writes
; when it is full; this loop therefore needs no status polling.
refill:
            ld      hl,wave
            ld      b,wave_end-wave
write_wave:
            ld      a,(hl)
            out     (DAC_DATA),a
            inc     hl
            djnz    write_wave
            jp      refill

; One unsigned-PCM period.  $80 is silence; the staircase makes the sampled
; output and the FIFO's linear resampling behavior easy to inspect.
wave:
            defb    $80,$98,$B0,$C8,$E0,$F0,$FF,$F0
            defb    $E0,$C8,$B0,$98,$80,$68,$50,$38
            defb    $20,$10,$00,$10,$20,$38,$50,$68
            defb    $80,$80,$80,$80,$80,$80,$80,$80
wave_end:

            ; The ROM card maps a 16 KiB image.
            defs    $4000-$,0
