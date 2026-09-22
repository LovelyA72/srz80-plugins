srz80_plugin(ay8913 KIND card
  SOURCES card.cpp ay8913.cpp
  OUTPUT audio_ay8913
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}"
  LIBS srz80_json)
