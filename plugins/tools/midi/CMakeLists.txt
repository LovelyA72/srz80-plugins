add_library(midi_protocol STATIC smf.cpp)
target_include_directories(midi_protocol PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(midi_protocol PROPERTIES POSITION_INDEPENDENT_CODE YES)
srz80_warnings(midi_protocol)

add_library(midi_backend STATIC backend.cpp)
target_include_directories(midi_backend PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(midi_backend PUBLIC Threads::Threads)
set_target_properties(midi_backend PROPERTIES POSITION_INDEPENDENT_CODE YES)
srz80_warnings(midi_backend)

include(FetchContent)
FetchContent_Declare(rtmidi
  URL https://codeload.github.com/thestk/rtmidi/tar.gz/refs/tags/6.0.0
  URL_HASH SHA256=ef7bcda27fee6936b651c29ebe9544c74959d0b1583b716ce80a1c6fea7617f0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR srz80-no-upstream-cmake)
FetchContent_MakeAvailable(rtmidi)

add_library(srz80_rtmidi STATIC "${rtmidi_SOURCE_DIR}/RtMidi.cpp")
target_include_directories(srz80_rtmidi PUBLIC "${rtmidi_SOURCE_DIR}")
set_target_properties(srz80_rtmidi PROPERTIES
  POSITION_INDEPENDENT_CODE YES CXX_VISIBILITY_PRESET hidden)
if(WIN32)
  target_compile_definitions(srz80_rtmidi PRIVATE __WINDOWS_MM__)
  target_link_libraries(srz80_rtmidi PRIVATE winmm)
elseif(APPLE)
  target_compile_definitions(srz80_rtmidi PRIVATE __MACOSX_CORE__)
  target_link_libraries(srz80_rtmidi PRIVATE
    "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
else()
  find_package(ALSA REQUIRED)
  target_compile_definitions(srz80_rtmidi PRIVATE __LINUX_ALSA__)
  target_link_libraries(srz80_rtmidi PRIVATE ALSA::ALSA Threads::Threads)
endif()

srz80_plugin(midi_tool KIND tool
  SOURCES tool.cpp backend.cpp rtmidi.cpp piano.cpp
  OUTPUT io_miditool
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}"
  LIBS midi_protocol srz80_imgui srz80_rtmidi nlohmann_json::nlohmann_json
    Threads::Threads)

add_custom_command(TARGET plugin_midi_tool POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SRZ80_RUNTIME_DIR}/licenses/rtmidi"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${rtmidi_SOURCE_DIR}/LICENSE"
    "${SRZ80_RUNTIME_DIR}/licenses/rtmidi/LICENSE")
