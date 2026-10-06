# RV32IM MIDI YMW258 piano

`xk992a0.ic6` is the TG100 sample ROM; `xk731c0.ic4` is its controller ROM.
They are not provided here since they are Yamaha's property. Get them
somewhere online. Hint: tg100.7z

The project explicitly clocks the YMW258 at the TG100's 9.4 MHz rather than
the card's 9.8784 MHz generic default; the latter would play this ROM about
86 cents sharp.

It plays the TG100's 128 GM programs, with drums on channel 10.

The MIDI parser is shared with the OPL3 example. It supports
running status, interleaved real-time bytes, velocity-zero Note Off, sustain,
sostenuto, soft pedal, Reset All Controllers, All Notes Off and immediate All
Sound Off. Repeated notes release oldest-first, and All Notes Off respects
sustain and sostenuto. Volume, expression, pan, modulation, channel pressure, pitch
bend and RPN bend range/fine/coarse tuning affect sounding voices.

GM1 System On and System Reset restore defaults. GM System Off silences the
receiver and ignores notes until reset. Start and Stop cut off all notes.
Continue leaves playing notes alone. Universal Master Volume uses its MSB, and
Universal Master Fine/Coarse Tuning updates sounding voices. Short, oversized
and unsupported SysEx messages are ignored. GS, XG, GM2, bank selection,
reverb and chorus aren't supported.

Run `./build_rom.sh` with GNU RISC-V bare-metal tools to rebuild both firmware
images. The linker rejects firmware that overlaps Work RAM at `0x4000`.
