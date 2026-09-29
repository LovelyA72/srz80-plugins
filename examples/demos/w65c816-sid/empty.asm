; "Empty" : 512 byte tune by 4mat, for the Tiny SID Competition.
;
; W65C816 reset starts in 6502 emulation mode.  This port retains the
; original player and data, using a CPU delay in place of the C64 VIC raster
; wait so it runs on the intentionally C64-free SRZ80 rack.

* = $8000

voicedata = $1000
data1 = voicedata+$15
data2 = data1+$15
notehi = data2+$80
notelo = notehi+$80
; The original player rotates these twelve seed values while building the
; frequency table.  Its C64 image lived in RAM; keep the mutable copy in our
; RAM card and seed it from the ROM image at every reset.
freqsource = notelo+$80

start sei

ldx #$00
copy_freqsource
lda freqsource_seed,x
sta freqsource,x
inx
cpx #$0c
bne copy_freqsource

ldx #$00
freqloop lda #$01
pha
txa
tay
freqset pla
sta notehi+$01,y
asl
pha
lda freqsource,x
sta notelo+$01,y
rol
sta freqsource,x
bcc notinc
pla
adc #$00
pha
notinc
lda #$00
sta voicedata,y
clc
tya
adc #$0c
tay
bpl freqset
inx
cpx #$0c
bne freqloop

ldy #$0e
sty $d418
ldx #$02
startup
lda songstart,x
sta data2+$05,y
lda #$0e
sta data1+$05,y
tya
sbc #$07
tay
dex
bne startup

; Approximately 50 Hz at the project clock's 1 MHz.  The original used the
; PAL VIC raster register here, which is not present in this rack.
wait
jsr frame_delay
jsr playframe
jmp wait

frame_delay
ldx #$10
delay_outer
ldy #$00
delay_inner
dey
bne delay_inner
dex
bne delay_outer
rts

update ldy data1,x

lda voicedata+$02,x
adc instadd,y
cmp voicedata+$02,x
sta voicedata+$02,x
bcs notaddp
inc voicedata+$03,x

notaddp tya
asl
asl
adc data1+$01,x
sbc data1,x
tay
lda instwave,y
sta voicedata+$04,x
lda instdata,y
cmp #$f0
bcc justset
and #$0f
adc data1+$03,x
adc data2+$02,x
tay
lda notelo,y
sta voicedata,x
lda notehi,y
justset sta voicedata+$01,x

ignore inc data1+$01,x
lda data1+$01,x
and #$03
bne resetinst
ldy data1,x
lda instpuls,y
lsr
lsr
lsr
lsr
resetinst sta data1+$01,x

notnewnote ldy #$07
playsid lda voicedata,x
sta $d400,x
inx
dey
bne playsid

cpx #$15
bne loopplay
rts

playframe ldx #$00

loopplay dec data2+$04,x
bpl update

lda #$06
sta data2+$04,x

dec data2,x
bpl update

getmore inc data1+$04,x
getmore2 ldy data1+$04,x
lda data1+$06,x
sta data2,x
lda pattdata,y
beq notnewnote
bmi other
sta data1+$03,x
lda #$00
sta data1+$01,x
ldy data1,x
lda instpuls,y
sta voicedata+$03,x
lda instadsr,y
sta voicedata+$06,x
lda #$09
sta voicedata+$04,x
bpl ignore
other cmp #$ff
beq songadd
cmp #$df
and #$0f
bcc lengthset
sta data1,x
bpl getmore
lengthset sta data1+$06,x
bpl getmore

songadd inc data1+$05,x
songadd2 ldy data1+$05,x
lda songdata,y
bmi other2
sta data1+$04,x
bpl getmore2
other2 cmp #$ff
beq restart
and #$0f
sta data2+$02,x
bpl songadd
restart lda data2+$05,x
sta data1+$05,x
bpl songadd2

instdata .byte $f3,$f7,$f0
         .byte $f4,$f7,$f0
         .byte $10,$af,$06
         .byte $f0,$f0,$f0
         .byte $14,$0c,$e0
         .byte $fc,$ef,$f0
         .byte $fc,$fc,$f0

instwave .byte $41,$41,$41
         .byte $41,$41,$41
         .byte $41,$81,$40
         .byte $41,$41,$40
         .byte $41,$41,$80
         .byte $11,$81,$40
         .byte $11,$21,$40

instadsr .byte $6f,$6f,$95,$ec,$a9,$79,$6e
instpuls .byte $15,$18,$38,$30,$38,$3b,$36
instadd  .byte $1c,$0c,$00,$15,$00,$74,$a5

pattdata
         .byte $00,$ff
         .byte $81,$e3,$11,$1d,$82,$e4,$55,$82,$e3,$11,$81,$1d
         .byte $e4,$55,$e3,$18,$ff
         .byte $e2,$55,$e5,$30,$35,$3a,$41,$35,$3a,$29,$ff
         .byte $8f,$e0,$35,$e1,$33,$31,$00,$e0,$35,$e1,$33,$e0,$2e,$00,$ff
         .byte $e6,$82,$38,$37,$87,$ff
         .byte $33,$80,$2e,$2c,$ff
         .byte $31,$8f,$00,$81,$00,$ff
         .byte $8f,$2e,$00,$ff

songdata
         .byte $f0,$02,$02,$f5,$02,$02,$f8,$02,$fa,$02,$f1,$02,$02,$ff
         .byte $13,$ff
         .byte $1e,$1e,$2d,$33,$2d,$33,$2d,$38
         .byte $2d,$33,$2d,$33,$3e,$2d,$f7,$33,$f0,$2d,$f9,$33,$f0,$2d,$38
         .byte $2d,$33,$2d,$33,$3e,$ff

songstart .byte $00,$0e,$10
freqsource_seed .byte 12,28,45,62,81,102,123,145,169,195,221,250

; Emulation-mode NMI, RESET and IRQ/BRK vectors all restart the player.
* = $fffa
    .word start
    .word start
    .word start
