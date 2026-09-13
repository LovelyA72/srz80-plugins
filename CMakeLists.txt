cmake_minimum_required(VERSION 3.24)
project(SRZ80Plugins VERSION 0.1.2 LANGUAGES C CXX)

# Shared policy lives here; every card and tool owns its target locally.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "SRZ80 plugins require GCC")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SRZ80_BUILD_TOOLS "Build the SDL3/ImGui tool plugins" ON)

set(SRZ80_PLUGIN_DIR "${CMAKE_BINARY_DIR}/plugins")
set(SRZ80_TOOL_DIR "${CMAKE_BINARY_DIR}/tools")
set(SRZ80_RUNTIME_DIR "${CMAKE_BINARY_DIR}")
set(SRZ80_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Plugin repository root")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SRZ80_PLUGIN_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SRZ80_PLUGIN_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

function(srz80_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

add_library(srz80_sdk INTERFACE)
target_include_directories(srz80_sdk INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/helpers")

include(FetchContent)
find_package(Threads REQUIRED)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

function(srz80_plugin name)
  cmake_parse_arguments(ARG "LINK_M;NO_WARNINGS" "KIND;OUTPUT"
    "SOURCES;LIBS;INCLUDES;DEFINITIONS" ${ARGN})
  if(NOT ARG_OUTPUT)
    set(ARG_OUTPUT "${name}")
  endif()
  if(ARG_KIND STREQUAL "tool")
    set(out_dir "${SRZ80_TOOL_DIR}")
  else()
    set(out_dir "${SRZ80_PLUGIN_DIR}")
  endif()
  add_library(plugin_${name} SHARED ${ARG_SOURCES})
  target_link_libraries(plugin_${name} PRIVATE srz80_sdk ${ARG_LIBS})
  if(ARG_LINK_M AND UNIX AND NOT APPLE)
    target_link_libraries(plugin_${name} PRIVATE m)
  endif()
  target_compile_definitions(plugin_${name} PRIVATE SRZ80_PLUGIN_BUILD ${ARG_DEFINITIONS})
  if(ARG_INCLUDES)
    target_include_directories(plugin_${name} PRIVATE ${ARG_INCLUDES})
  endif()
  set_target_properties(plugin_${name} PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    RUNTIME_OUTPUT_DIRECTORY "${out_dir}" LIBRARY_OUTPUT_DIRECTORY "${out_dir}"
    OUTPUT_NAME "${ARG_OUTPUT}")
  if(WIN32)
    set_target_properties(plugin_${name} PROPERTIES PREFIX "")
  endif()
  if((WIN32 OR (UNIX AND NOT APPLE)) AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET plugin_${name} POST_BUILD
      COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:plugin_${name}>"
      COMMENT "Stripping $<TARGET_FILE_NAME:plugin_${name}>"
      VERBATIM)
  endif()
  if(NOT ARG_NO_WARNINGS)
    srz80_warnings(plugin_${name})
  endif()
endfunction()

add_subdirectory(plugins/cards)

if(SRZ80_BUILD_TOOLS)
  FetchContent_Declare(SDL3 URL https://www.libsdl.org/release/SDL3-3.4.0.tar.gz
    URL_HASH SHA256=082cbf5f429e0d80820f68dc2b507a94d4cc1b4e70817b119bbb8ec6a69584b8
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.5-docking.tar.gz
    URL_HASH SHA256=c816c20e8c75f3e15ae867350e79925502d1a6a85938bb1a73b8927e5f31f9cb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(imgui)
  add_library(srz80_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(srz80_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(srz80_imgui PUBLIC SDL3::SDL3)
  set_target_properties(srz80_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_subdirectory(plugins/tools)
endif()

add_custom_target(examples ALL COMMAND "${CMAKE_COMMAND}" -E copy_directory
  "${CMAKE_CURRENT_SOURCE_DIR}/examples" "${CMAKE_BINARY_DIR}/examples"
  COMMENT "Staging examples in ${CMAKE_BINARY_DIR}/examples" VERBATIM)
message(STATUS "Cards build into ${SRZ80_PLUGIN_DIR}")
if(SRZ80_BUILD_TOOLS)
  message(STATUS "Tools build into ${SRZ80_TOOL_DIR}")
endif()
