srz80_plugin(vsn_inspector_tool KIND tool SOURCES tool.cpp
  OUTPUT video_vsn_inspector LIBS srz80_imgui nlohmann_json::nlohmann_json
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../../cards/vsn")

if(UNIX AND NOT APPLE)
  target_link_options(plugin_vsn_inspector_tool PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
  set_property(TARGET plugin_vsn_inspector_tool APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
endif()
