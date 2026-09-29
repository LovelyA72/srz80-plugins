; Leftward parallax starfield. z88dk z80asm; GRAPHIC 4, 256x192.
frames equ 0x8000
ready equ 0x8002
busy equ 0x8003
stars equ 0x8100
        org 0
        di
        ld sp,0xBFF0
        xor a
        ld (ready),a
        ld (busy),a
        ld (frames),a
        ld (frames+1),a
        ld hl,registers
        ld b,24
        ld c,0
init_regs:
        ld a,(hl)
        call setreg
        inc hl
        inc c
        djnz init_regs
        ld hl,palette
        ld b,32
init_palette:
        ld a,(hl)
        out (0x9A),a
        ex (sp),hl
        ex (sp),hl
        inc hl
        djnz init_palette
        ; Clear 32 KiB with display off, including the 16 KiB bank seam.
        ld hl,0
        call address
        ld de,0x8000
clear:
        xor a
        out (0x98),a
        ex (sp),hl
        ex (sp),hl
        dec de
        ld a,d
        or e
        jr nz,clear
        ld hl,initial_stars
        ld de,stars
        ld bc,128*4
        ldir
        ld ix,stars
        ld b,128
first_draw:
        call draw
        ld de,4
        add ix,de
        djnz first_draw
        ld a,0x40
        ld c,1
        call setreg
        ld a,1
        ld (ready),a
frame:
        ; Discard stale VBlank and wait for the next. Status polling stays
        ; outside control-port pairs; no IRQ wiring or handler is needed.
        in a,(0x99)
wait_vblank:
        in a,(0x99)
        and 0x80
        jr z,wait_vblank
        ld a,1
        ld (busy),a
        ld ix,stars
        ld b,128
animate:
        call star_address
        xor a
        out (0x98),a
        ex (sp),hl
        ex (sp),hl
        ld a,(ix+0)
        sub (ix+2)             ; byte underflow wraps to the right
        ld (ix+0),a
        call draw
        ld de,4
        add ix,de
        djnz animate
        ld hl,(frames)
        inc hl
        ld (frames),hl
        xor a
        ld (busy),a
        jr frame
draw:
        call star_address
        ld a,(ix+3)
        bit 0,(ix+0)
        jr nz,low_pixel
        rlca
        rlca
        rlca
        rlca
low_pixel:
        out (0x98),a
        ex (sp),hl
        ex (sp),hl
        ret
star_address:
        ; Distinct scanlines prevent erase collisions between layers.
        ; Packed bitmap address = y*128 + x/2.
        ld h,0
        ld l,(ix+1)
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld a,(ix+0)
        srl a
        or l
        ld l,a
address:
        ld a,h
        rlca
        rlca
        and 3
        ld c,14
        call setreg
        ld a,l
        out (0x99),a
        ex (sp),hl
        ex (sp),hl
        ld a,h
        and 0x3F
        or 0x40
        out (0x99),a
        ex (sp),hl
        ex (sp),hl
        ret
; RomWBW TMS_SET_X / TMS_WR protocol: value then 0x80|register;
; address low then high|0x40. Conservative 38-T recovery delays.
; Interrupts remain disabled so a status read cannot split a latch pair.
setreg:
        out (0x99),a
        ex (sp),hl
        ex (sp),hl
        ld a,c
        or 0x80
        out (0x99),a
        ex (sp),hl
        ex (sp),hl
        ret
registers:
        defb 0x06,0,0x1F,0,0,0,0,0
        defb 0x02,0,0,0,0,0,0,0
        defb 0,0,0,0,0,0,0,0
; Palette pairs: 0RRR0BBB, 00000GGG. Black, blue-grey, silver, white.
palette:
        defb 0,0, 0x23,2, 0x45,4, 0x77,7
        defs 24,0
initial_stars:
        include "stars.inc"
