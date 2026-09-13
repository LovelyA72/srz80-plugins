add_library(ymw258_engine STATIC ymw258.cpp)
target_include_directories(ymw258_engine PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(ymw258_engine PROPERTIES POSITION_INDEPENDENT_CODE YES)
srz80_warnings(ymw258_engine)

srz80_plugin(ymw258 KIND card
  SOURCES card.cpp
  OUTPUT audio_ymw258
  LIBS ymw258_engine nlohmann_json::nlohmann_json)
