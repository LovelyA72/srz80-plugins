file(GLOB SID_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/c64_fp/*.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/c64_fp/*.cc"
  "${CMAKE_CURRENT_LIST_DIR}/c64_fp/resample/SincResampler.cpp")
srz80_plugin(sid KIND card
  SOURCES card.cpp dsid/dsid.c ${SID_SOURCES}
  OUTPUT audio_sid
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/c64_fp" "${CMAKE_CURRENT_LIST_DIR}/dsid"
  LINK_M)
