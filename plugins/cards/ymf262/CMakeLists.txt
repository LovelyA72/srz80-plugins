srz80_plugin(ymf262 KIND card
  SOURCES card.cpp opl/opl3.c ymfm/ymfm_opl.cpp ymfm/ymfm_adpcm.cpp
    ymfm/ymfm_pcm.cpp
  OUTPUT audio_ymf262
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/opl" "${CMAKE_CURRENT_LIST_DIR}/ymfm")
