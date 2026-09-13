set(libasm_root "${CMAKE_CURRENT_LIST_DIR}/libasm" CACHE PATH "Vendored libasm sources")
if(NOT EXISTS "${libasm_root}/src/asm_z80.cpp")
  message(FATAL_ERROR "libasm sources are missing at ${libasm_root}")
endif()

set(libasm_sources
  asm_base asm_z80 config_base reg_base reg_z80 table_z80 text_z80 text_common
  str_buffer str_scanner option_base error_reporter insn_base value value_parser
  parsers operators formatters value_formatter)
add_library(srz80_libasm STATIC)
foreach(source IN LISTS libasm_sources)
  target_sources(srz80_libasm PRIVATE "${libasm_root}/src/${source}.cpp")
endforeach()
target_include_directories(srz80_libasm SYSTEM PUBLIC "${libasm_root}/src")
target_compile_definitions(srz80_libasm PUBLIC LIBASM_ASM_NOFLOAT LIBASM_DIS_NOFLOAT)
if(WIN32)
  target_compile_options(srz80_libasm PUBLIC
    -include "${CMAKE_CURRENT_LIST_DIR}/host/byte_order.h")
endif()
set_target_properties(srz80_libasm PROPERTIES
  POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden CXX_STANDARD 14)

add_library(srz80_assembler STATIC assembler.cpp)
target_link_libraries(srz80_assembler PRIVATE srz80_libasm)
target_include_directories(srz80_assembler PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(srz80_assembler PROPERTIES
  POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
srz80_warnings(srz80_assembler)

srz80_plugin(z80_assembler_tool KIND tool
  SOURCES tool.cpp editor.cpp inline_editor.cpp source_model.cpp loader.cpp
  OUTPUT cpu_z80_assembler
  LIBS srz80_imgui srz80_assembler)

add_custom_command(TARGET plugin_z80_assembler_tool POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SRZ80_RUNTIME_DIR}/licenses/libasm"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${libasm_root}/LICENSE.md"
    "${SRZ80_RUNTIME_DIR}/licenses/libasm/LICENSE.md")
