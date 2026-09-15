# Card projects are registered here
set(SRZ80_CARD_DIRECTORIES
  ram rom mmu_memory mbc5 fake_cpu z80 w65c816 video lcd1602 vdp tone adpcm dac
  ym2413 ymf262 ymw258 gbapu sid seta_x1_010 uart_console keyboard midi
  patchbay floppy riscv ctc src32)

foreach(card IN LISTS SRZ80_CARD_DIRECTORIES)
  add_subdirectory("${card}")
endforeach()
