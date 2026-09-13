srz80_plugin(gbapu KIND card
  SOURCES card.cpp gb/apu.c gb/timing.c
  OUTPUT audio_gbapu
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/gb"
  LINK_M)
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/gb/apu.c"
  PROPERTIES COMPILE_OPTIONS "-Wno-overflow")
