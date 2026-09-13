set(PATCHBAY_TOOL_SOURCES tool.cpp)
foreach(component led rgb_led button switch dip8 segment7 segment11 hc595)
  list(APPEND PATCHBAY_TOOL_SOURCES "components/${component}.cpp")
endforeach()
srz80_plugin(patchbay_tool KIND tool
  SOURCES ${PATCHBAY_TOOL_SOURCES}
  OUTPUT io_patchbay
  INCLUDES "${SRZ80_SOURCE_DIR}/plugins/cards/patchbay"
  LIBS srz80_imgui nlohmann_json::nlohmann_json)
