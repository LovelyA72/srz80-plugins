add_library(patchbay_engine STATIC engine.cpp)
foreach(component led rgb_led button switch dip8 segment7 segment11 hc595)
  target_sources(patchbay_engine PRIVATE "components/${component}.cpp")
endforeach()
target_include_directories(patchbay_engine PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(patchbay_engine PUBLIC srz80_sdk nlohmann_json::nlohmann_json)
set_target_properties(patchbay_engine PROPERTIES
  POSITION_INDEPENDENT_CODE YES CXX_VISIBILITY_PRESET hidden)
srz80_warnings(patchbay_engine)

srz80_plugin(digitalio KIND card
  SOURCES card.cpp
  OUTPUT io_digitalio
  LIBS patchbay_engine)
