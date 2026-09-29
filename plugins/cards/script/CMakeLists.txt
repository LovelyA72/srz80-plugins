set(SCRIPT_VENDOR "${CMAKE_CURRENT_LIST_DIR}/vendor")
set(SCRIPT_LUA_SOURCE "${SCRIPT_VENDOR}/lua")
set(SCRIPT_JS_SOURCE "${SCRIPT_VENDOR}/quickjs")
set(SCRIPT_MRUBY_SOURCE "${SCRIPT_VENDOR}/mruby")
set(SCRIPT_POCKETPY_SOURCE "${SCRIPT_VENDOR}/pocketpy")
set(PK_ENABLE_OS OFF CACHE BOOL "" FORCE)
set(PK_ENABLE_THREADS OFF CACHE BOOL "" FORCE)
set(PK_ENABLE_DLL OFF CACHE BOOL "" FORCE)
if(NOT EXISTS "${SCRIPT_POCKETPY_SOURCE}/include/pocketpy/pocketpy.h")
  FetchContent_Declare(script_pocketpy
    GIT_REPOSITORY https://github.com/pocketpy/pocketpy.git
    GIT_TAG 128fc43667f213e48472d8fe20fdb43be32bac22
    GIT_SHALLOW FALSE)
  FetchContent_MakeAvailable(script_pocketpy)
  set(SCRIPT_POCKETPY_ROOT "${script_pocketpy_SOURCE_DIR}")
else()
  add_subdirectory("${SCRIPT_POCKETPY_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/pocketpy")
  set(SCRIPT_POCKETPY_ROOT "${SCRIPT_POCKETPY_SOURCE}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/pocketpy_budget.cmake")
# SYSTEM keeps pocketpy's headers out of this repository's -Wpedantic warnings.
set_target_properties(pocketpy PROPERTIES POSITION_INDEPENDENT_CODE YES SYSTEM YES)
# The runtime is linked statically: keep its API private to this module and work
# around the MinGW time functions pocketpy expects (see pocketpy_compat.h).
target_compile_options(pocketpy PRIVATE -fvisibility=hidden
  -include "${CMAKE_CURRENT_LIST_DIR}/pocketpy_compat.h")
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
if(NOT EXISTS "${SCRIPT_MRUBY_SOURCE}/include/mruby.h")
  FetchContent_Declare(script_mruby
    URL https://github.com/mruby/mruby/archive/refs/tags/4.0.0.tar.gz
    URL_HASH SHA256=e2ea271dbed14e9f2b33df773ae447b747dbc242ce2675022c0a57efea85a7b4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_GetProperties(script_mruby)
  if(NOT script_mruby_POPULATED)
    FetchContent_Populate(script_mruby)
  endif()
  set(SCRIPT_MRUBY_SOURCE "${script_mruby_SOURCE_DIR}")
endif()

find_program(SCRIPT_RAKE_EXECUTABLE NAMES rake rake.bat REQUIRED)
set(SCRIPT_MRUBY_BUILD "${CMAKE_CURRENT_BINARY_DIR}/mruby-build")
if(CMAKE_CROSSCOMPILING AND WIN32)
  set(SCRIPT_MRUBY_TARGET target)
  set(SCRIPT_MRUBY_CROSS 1)
else()
  set(SCRIPT_MRUBY_TARGET host)
  set(SCRIPT_MRUBY_CROSS 0)
endif()
set(SCRIPT_MRUBY_LIBRARY "${SCRIPT_MRUBY_BUILD}/${SCRIPT_MRUBY_TARGET}/lib/libmruby.a")
configure_file("${CMAKE_CURRENT_LIST_DIR}/mruby_build.rb"
  "${CMAKE_CURRENT_BINARY_DIR}/mruby_build.rb" COPYONLY)
file(MAKE_DIRECTORY "${SCRIPT_MRUBY_BUILD}/${SCRIPT_MRUBY_TARGET}/include")
add_custom_command(OUTPUT "${SCRIPT_MRUBY_LIBRARY}"
  COMMAND ${CMAKE_COMMAND} -E env
    MRUBY_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/mruby_build.rb
    MRUBY_BUILD_DIR=${SCRIPT_MRUBY_BUILD}
    SRZ80_MRUBY_CROSS=${SCRIPT_MRUBY_CROSS}
    SRZ80_MRUBY_CC=${CMAKE_C_COMPILER}
    SRZ80_MRUBY_AR=${CMAKE_AR}
    ${SCRIPT_RAKE_EXECUTABLE} -f ${SCRIPT_MRUBY_SOURCE}/Rakefile
  WORKING_DIRECTORY "${SCRIPT_MRUBY_SOURCE}"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/mruby_build.rb"
  VERBATIM)
add_custom_target(srz80_script_mruby_build DEPENDS "${SCRIPT_MRUBY_LIBRARY}")
add_library(srz80_script_mruby STATIC IMPORTED GLOBAL)
set_target_properties(srz80_script_mruby PROPERTIES
  IMPORTED_LOCATION "${SCRIPT_MRUBY_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${SCRIPT_MRUBY_SOURCE}/include;${SCRIPT_MRUBY_BUILD}/${SCRIPT_MRUBY_TARGET}/include")
add_dependencies(srz80_script_mruby srz80_script_mruby_build)
target_compile_definitions(srz80_script_mruby INTERFACE MRB_ENABLE_DEBUG_HOOK)
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
  SOURCES card.cpp script_vm.cpp lua_vm.cpp js_vm.cpp mruby_vm.cpp python_vm.cpp util.cpp
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}"
  OUTPUT misc_script
  LIBS srz80_script_lua srz80_script_quickjs srz80_script_mruby pocketpy nlohmann_json::nlohmann_json)
# This module's sources include pocketpy.h through script_card.h; the forced
# include keeps pocketpy's export attribute out of them.
target_compile_options(plugin_script PRIVATE
  -include "${CMAKE_CURRENT_LIST_DIR}/pocketpy_compat.h")

option(SRZ80_SCRIPT_PHP "Build the experimental Linux/glibc PHP script backend" OFF)
if(SRZ80_SCRIPT_PHP)
  include("${CMAKE_CURRENT_LIST_DIR}/php/CMakeLists.txt")
endif()
