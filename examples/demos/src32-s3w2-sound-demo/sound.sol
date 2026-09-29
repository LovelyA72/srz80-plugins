# Source code of this demo
# you have to use my sol compiler (https://github.com/src3453/src32) to compile this

!const SGU_BASE 0x1000

fn regw (v a) :
    v a SGU_BASE add stb
;

!var i 0
0 0x802 regw
255 0x803 regw
while
    0 i regw
    i 1 add >i
    i 128 lt
end
while
    255 i regw
    i 1 add >i
    i 256 lt
end
0 >i
while
    i 256 add >i
    i 24 shr 0x800 regw
    i 16 shr 0x801 regw
    0
end