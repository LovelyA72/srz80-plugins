# RV32IM MIDI YMW258 piano

`xk992a0.ic6` is the TG100 sample ROM; `xk731c0.ic4` is its controller ROM.
They are not provided here since they are Yamaha's property. Get them
somewhere online. Hint: tg100.7z

The project explicitly clocks the YMW258 at the TG100's 9.4 MHz rather than
the card's 9.8784 MHz generic default; the latter would play this ROM about
86 cents sharp.