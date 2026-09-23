set(SCRIPT_VENDOR "${CMAKE_CURRENT_LIST_DIR}/vendor")

add_library(srz80_script_lua STATIC
  ${SCRIPT_VENDOR}/lua/lapi.c ${SCRIPT_VENDOR}/lua/lcode.c
  ${SCRIPT_VENDOR}/lua/lctype.c ${SCRIPT_VENDOR}/lua/ldebug.c
  ${SCRIPT_VENDOR}/lua/ldo.c ${SCRIPT_VENDOR}/lua/ldump.c
  ${SCRIPT_VENDOR}/lua/lfunc.c ${SCRIPT_VENDOR}/lua/lgc.c
  ${SCRIPT_VENDOR}/lua/llex.c ${SCRIPT_VENDOR}/lua/lmem.c
  ${SCRIPT_VENDOR}/lua/lobject.c ${SCRIPT_VENDOR}/lua/lopcodes.c
  ${SCRIPT_VENDOR}/lua/lparser.c ${SCRIPT_VENDOR}/lua/lstate.c
  ${SCRIPT_VENDOR}/lua/lstring.c ${SCRIPT_VENDOR}/lua/ltable.c
  ${SCRIPT_VENDOR}/lua/ltm.c ${SCRIPT_VENDOR}/lua/lundump.c
  ${SCRIPT_VENDOR}/lua/lvm.c ${SCRIPT_VENDOR}/lua/lzio.c
  ${SCRIPT_VENDOR}/lua/lauxlib.c ${SCRIPT_VENDOR}/lua/lbaselib.c
  ${SCRIPT_VENDOR}/lua/lcorolib.c ${SCRIPT_VENDOR}/lua/lmathlib.c
  ${SCRIPT_VENDOR}/lua/lstrlib.c ${SCRIPT_VENDOR}/lua/ltablib.c
  ${SCRIPT_VENDOR}/lua/lutf8lib.c)
target_include_directories(srz80_script_lua PUBLIC "${SCRIPT_VENDOR}/lua")
set_target_properties(srz80_script_lua PROPERTIES POSITION_INDEPENDENT_CODE YES)
target_compile_options(srz80_script_lua PRIVATE -w)

add_library(srz80_script_quickjs STATIC
  ${SCRIPT_VENDOR}/quickjs/dtoa.c ${SCRIPT_VENDOR}/quickjs/libregexp.c
  ${SCRIPT_VENDOR}/quickjs/libunicode.c ${SCRIPT_VENDOR}/quickjs/quickjs.c)
target_include_directories(srz80_script_quickjs PUBLIC "${SCRIPT_VENDOR}/quickjs")
target_compile_definitions(srz80_script_quickjs PRIVATE _GNU_SOURCE)
if(WIN32)
  target_compile_definitions(srz80_script_quickjs PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()
if(NOT WIN32)
  target_link_libraries(srz80_script_quickjs PUBLIC m)
endif()
set_target_properties(srz80_script_quickjs PROPERTIES POSITION_INDEPENDENT_CODE YES)
target_compile_options(srz80_script_quickjs PRIVATE -w -funsigned-char)

srz80_plugin(script KIND card
  SOURCES card.cpp script_vm.cpp lua_vm.cpp js_vm.cpp util.cpp
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}"
  OUTPUT misc_script
  LIBS srz80_script_lua srz80_script_quickjs nlohmann_json::nlohmann_json)
