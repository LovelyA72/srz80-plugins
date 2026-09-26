# Patch only the bytecode dispatch point of the pinned PocketPy release. Keep
# the fetched/vendor source read-only and fail configuration if it changes.
set(_source "${SCRIPT_POCKETPY_ROOT}/src/interpreter/ceval.c")
set(_output "${CMAKE_CURRENT_BINARY_DIR}/pocketpy-budget/ceval.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}")
file(READ "${_source}" _ceval)
set(_anchor "    byte = co_codes[frame->ip];\n")
set(_check [=[    byte = co_codes[frame->ip];
    if(srz80_script_python_budget_step(self->ctx)) {
        TimeoutError("Python instruction limit exceeded");
        goto __ERROR;
    }
]=])
string(FIND "${_ceval}" "${_anchor}" _position)
if(_position EQUAL -1)
  message(FATAL_ERROR "Pinned PocketPy bytecode dispatch point has changed")
endif()
string(REPLACE "${_anchor}" "${_check}" _ceval "${_ceval}")
string(PREPEND _ceval "extern int srz80_script_python_budget_step(void* context);\n")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pocketpy-budget")
file(WRITE "${_output}" "${_ceval}")
get_target_property(_sources pocketpy SOURCES)
list(REMOVE_ITEM _sources "${_source}")
list(APPEND _sources "${_output}")
set_property(TARGET pocketpy PROPERTY SOURCES "${_sources}")
target_include_directories(pocketpy PRIVATE "${SCRIPT_POCKETPY_ROOT}/include")
