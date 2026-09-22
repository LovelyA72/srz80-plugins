add_library(swp00_engine STATIC swp00.cpp)
target_link_libraries(swp00_engine PRIVATE srz80_sdk)
target_include_directories(swp00_engine PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(swp00_engine PROPERTIES POSITION_INDEPENDENT_CODE YES)
# The original DSP uses two's-complement wrapping arithmetic.
target_compile_options(swp00_engine PRIVATE -fwrapv)
srz80_warnings(swp00_engine)
srz80_plugin(swp00 KIND card SOURCES card.cpp OUTPUT audio_swp00
  LIBS swp00_engine srz80_json)
