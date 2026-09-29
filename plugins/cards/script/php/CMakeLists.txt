# A bounded pool of link-map namespaces isolates PHP's non-thread-safe globals.
# Each VM leases an engine and starts a fresh request.
if(WIN32)
  message(FATAL_ERROR "SRZ80_SCRIPT_PHP=ON is not supported on Windows (including MinGW-w64 cross-compilation). Configure with -DSRZ80_SCRIPT_PHP=OFF.")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "SRZ80_SCRIPT_PHP currently requires native 64-bit Linux/glibc")
endif()
include(CheckSymbolExists)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(dlmopen "dlfcn.h" SCRIPT_HAVE_DLMOPEN)
unset(CMAKE_REQUIRED_DEFINITIONS)
if(NOT SCRIPT_HAVE_DLMOPEN)
  message(FATAL_ERROR "The PHP proof of concept requires glibc dlmopen")
endif()
include(ExternalProject)
find_program(SCRIPT_PHP_MAKE make REQUIRED)
set(SRZ80_PHP_ARCHIVE "https://www.php.net/distributions/php-8.4.25.tar.gz"
    CACHE STRING "PHP archive URL or local tarball (SHA256 verified)")
set(PHP_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/php-install")
ExternalProject_Add(script_php_runtime
  URL "${SRZ80_PHP_ARCHIVE}"
  URL_HASH SHA256=5cde1da976b728fa09e6b549a4c8afcd0c180337028dacf34f73b11be50253db
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PREFIX "${CMAKE_CURRENT_BINARY_DIR}/php-build"
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${PHP_PREFIX}
    --disable-all --enable-embed=shared --disable-cli --disable-cgi
    --disable-phpdbg --disable-opcache --without-pear --without-pcre-jit
    --disable-zend-signals --with-config-file-path=/nonexistent/srz80-php
    --with-config-file-scan-dir=/nonexistent/srz80-php
    "CFLAGS=-O2 -fPIC" "CC=${CMAKE_C_COMPILER}"
  BUILD_COMMAND ${SCRIPT_PHP_MAKE} -j4
  INSTALL_COMMAND ${SCRIPT_PHP_MAKE} install
  BUILD_BYPRODUCTS "${PHP_PREFIX}/lib/libphp.so"
  LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE)

add_library(script_php_bridge SHARED php/php_bridge.c)
target_compile_definitions(script_php_bridge PRIVATE _GNU_SOURCE)
target_compile_options(script_php_bridge PRIVATE -Wall -Wextra -Wno-unused-parameter
  -Werror=implicit-function-declaration)
add_dependencies(script_php_bridge script_php_runtime)
target_include_directories(script_php_bridge PRIVATE
  "${PHP_PREFIX}/include/php" "${PHP_PREFIX}/include/php/main"
  "${PHP_PREFIX}/include/php/Zend" "${PHP_PREFIX}/include/php/TSRM"
  "${PHP_PREFIX}/include/php/sapi/embed")
target_link_libraries(script_php_bridge PRIVATE "${PHP_PREFIX}/lib/libphp.so")
set_target_properties(script_php_bridge PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${SRZ80_PLUGIN_DIR}/php"
  BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN"
  C_VISIBILITY_PRESET hidden)
add_custom_command(TARGET script_php_bridge POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PHP_PREFIX}/lib/libphp.so"
    "${SRZ80_PLUGIN_DIR}/php/libphp.so"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/php-build/src/script_php_runtime/LICENSE"
    "${SRZ80_PLUGIN_DIR}/php/PHP-LICENSE"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/php-build/src/script_php_runtime/Zend/LICENSE"
    "${SRZ80_PLUGIN_DIR}/php/ZEND-LICENSE"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_LIST_DIR}/PCRE2-LICENSE"
    "${SRZ80_PLUGIN_DIR}/php/PCRE2-LICENSE"
  VERBATIM)
target_sources(plugin_script PRIVATE php_vm.cpp)
target_compile_definitions(plugin_script PRIVATE SRZ80_SCRIPT_PHP=1)
target_link_libraries(plugin_script PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
add_dependencies(plugin_script script_php_bridge)
