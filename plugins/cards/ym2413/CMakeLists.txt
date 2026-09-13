srz80_plugin(ym2413 KIND card
  SOURCES card.cpp emu2413/emu2413.c
  OUTPUT audio_ym2413
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/emu2413"
  LINK_M)
