srz80_plugin(ym2414 KIND card
  SOURCES card.cpp opz_core.cpp engine.cpp ymfm/ymfm_opz.cpp
  OUTPUT audio_ym2414
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}"
  LINK_M)

# The vendored ymfm core leaves its interface stub parameters unnamed.
set_source_files_properties(ymfm/ymfm_opz.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
