# SPDX-License-Identifier: MIT
# Rendering/core sources compile independently of the SRZ80 SDK.
add_library(vsn_core STATIC vsn_core.cpp vsn_renderer.cpp)
set_target_properties(vsn_core PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
target_include_directories(vsn_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
srz80_warnings(vsn_core)
srz80_plugin(vsn KIND card SOURCES card.cpp OUTPUT video_vsn
  LIBS vsn_core nlohmann_json::nlohmann_json)
