set(SCRIPT_VENDOR "${CMAKE_CURRENT_LIST_DIR}/vendor")
set(SCRIPT_LUA_SOURCE "${SCRIPT_VENDOR}/lua")
set(SCRIPT_JS_SOURCE "${SCRIPT_VENDOR}/quickjs")
# Some checkouts omit the ignored vendor directory. Fetch the same pinned
# releases in that case, without adding either runtime's standalone programs.
if(NOT EXISTS "${SCRIPT_LUA_SOURCE}/lua.h")
  FetchContent_Declare(script_lua
    URL https://www.lua.org/ftp/lua-5.5.1.tar.gz
    URL_HASH SHA256=1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_GetProperties(script_lua)
  if(NOT script_lua_POPULATED)
    FetchContent_Populate(script_lua)
  endif()
  set(SCRIPT_LUA_SOURCE "${script_lua_SOURCE_DIR}/src")
endif()
if(NOT EXISTS "${SCRIPT_JS_SOURCE}/quickjs.h")
  FetchContent_Declare(script_quickjs
    URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.15.1.tar.gz
    URL_HASH SHA256=c4e813951b7c46845096a948e978c620b11ab4cf5fd622ca09c727ec31f42623
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_GetProperties(script_quickjs)
  if(NOT script_quickjs_POPULATED)
    FetchContent_Populate(script_quickjs)
  endif()
  set(SCRIPT_JS_SOURCE "${script_quickjs_SOURCE_DIR}")
endif()

add_library(srz80_script_lua STATIC
  ${SCRIPT_LUA_SOURCE}/lapi.c ${SCRIPT_LUA_SOURCE}/lcode.c
  ${SCRIPT_LUA_SOURCE}/lctype.c ${SCRIPT_LUA_SOURCE}/ldebug.c
  ${SCRIPT_LUA_SOURCE}/ldo.c ${SCRIPT_LUA_SOURCE}/ldump.c
  ${SCRIPT_LUA_SOURCE}/lfunc.c ${SCRIPT_LUA_SOURCE}/lgc.c
  ${SCRIPT_LUA_SOURCE}/llex.c ${SCRIPT_LUA_SOURCE}/lmem.c
  ${SCRIPT_LUA_SOURCE}/lobject.c ${SCRIPT_LUA_SOURCE}/lopcodes.c
  ${SCRIPT_LUA_SOURCE}/lparser.c ${SCRIPT_LUA_SOURCE}/lstate.c
  ${SCRIPT_LUA_SOURCE}/lstring.c ${SCRIPT_LUA_SOURCE}/ltable.c
  ${SCRIPT_LUA_SOURCE}/ltm.c ${SCRIPT_LUA_SOURCE}/lundump.c
  ${SCRIPT_LUA_SOURCE}/lvm.c ${SCRIPT_LUA_SOURCE}/lzio.c
  ${SCRIPT_LUA_SOURCE}/lauxlib.c ${SCRIPT_LUA_SOURCE}/lbaselib.c
  ${SCRIPT_LUA_SOURCE}/lcorolib.c ${SCRIPT_LUA_SOURCE}/lmathlib.c
  ${SCRIPT_LUA_SOURCE}/lstrlib.c ${SCRIPT_LUA_SOURCE}/ltablib.c
  ${SCRIPT_LUA_SOURCE}/lutf8lib.c)
target_include_directories(srz80_script_lua PUBLIC "${SCRIPT_LUA_SOURCE}")
set_target_properties(srz80_script_lua PROPERTIES POSITION_INDEPENDENT_CODE YES)
target_compile_options(srz80_script_lua PRIVATE -w)

add_library(srz80_script_quickjs STATIC
  ${SCRIPT_JS_SOURCE}/dtoa.c ${SCRIPT_JS_SOURCE}/libregexp.c
  ${SCRIPT_JS_SOURCE}/libunicode.c ${SCRIPT_JS_SOURCE}/quickjs.c)
target_include_directories(srz80_script_quickjs PUBLIC "${SCRIPT_JS_SOURCE}")
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

option(SRZ80_SCRIPT_PHP "Build the experimental Linux/glibc PHP script backend" OFF)
if(SRZ80_SCRIPT_PHP)
  include("${CMAKE_CURRENT_LIST_DIR}/php/CMakeLists.txt")
endif()
