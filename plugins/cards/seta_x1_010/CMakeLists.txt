srz80_plugin(x1_010 KIND card
  SOURCES card.cpp x1_010/x1_010.cpp
  OUTPUT audio_x1_010
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}"
  LIBS srz80_json)
