set(RV32EMU_ROOT "${CMAKE_CURRENT_LIST_DIR}/rv32emu")
set(CAPSTONE_ROOT "${CMAKE_CURRENT_LIST_DIR}/capstone")

set(SRZ80_RV32EMU_SOURCES
  ${RV32EMU_ROOT}/src/map.c ${RV32EMU_ROOT}/src/utils.c
  ${RV32EMU_ROOT}/src/decode.c ${RV32EMU_ROOT}/src/io.c
  ${RV32EMU_ROOT}/src/syscall.c ${RV32EMU_ROOT}/src/emulate.c
  ${RV32EMU_ROOT}/src/riscv.c ${RV32EMU_ROOT}/src/log.c
  ${RV32EMU_ROOT}/src/elf.c ${RV32EMU_ROOT}/src/cache.c
  ${RV32EMU_ROOT}/src/mpool.c)

set(SRZ80_RV32EMU_JIT 0)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64)$")
  set(SRZ80_RV32EMU_JIT 1)
  list(APPEND SRZ80_RV32EMU_SOURCES
    ${RV32EMU_ROOT}/src/jit.c ${RV32EMU_ROOT}/src/bare_jit.c)
endif()

set(SRZ80_RV32EMU_DEFINITIONS
  RV32_FEATURE_EXT_M=1 RV32_FEATURE_EXT_A=0 RV32_FEATURE_EXT_F=1 RV32_FEATURE_EXT_C=0
  RV32_FEATURE_Zicsr=1 RV32_FEATURE_Zifencei=0 RV32_FEATURE_Zba=0 RV32_FEATURE_Zbb=0
  RV32_FEATURE_Zbc=0 RV32_FEATURE_Zbs=0 RV32_FEATURE_SDL=0 RV32_FEATURE_GDBSTUB=0
  RV32_FEATURE_JIT=${SRZ80_RV32EMU_JIT} RV32_FEATURE_T2C=0 RV32_FEATURE_SYSTEM=0
  RV32_FEATURE_HOST_IO=1 RV32_FEATURE_ARCH_TEST=0 RV32_FEATURE_MOP_FUSION=0
  RV32_FEATURE_BLOCK_CHAINING=0 RV32_FEATURE_LOG_COLOR=0)

file(READ "${RV32EMU_ROOT}/mk/softfloat.mk" SRZ80_SOFTFLOAT_MAKEFILE)
string(REGEX MATCHALL "[A-Za-z0-9_/]+\\.o" SRZ80_SOFTFLOAT_OBJECTS
  "${SRZ80_SOFTFLOAT_MAKEFILE}")
set(SRZ80_SOFTFLOAT_SOURCES)
foreach(SRZ80_SOFTFLOAT_OBJECT IN LISTS SRZ80_SOFTFLOAT_OBJECTS)
  string(REPLACE ".o" ".c" SRZ80_SOFTFLOAT_SOURCE "${SRZ80_SOFTFLOAT_OBJECT}")
  list(APPEND SRZ80_SOFTFLOAT_SOURCES
    "${RV32EMU_ROOT}/src/softfloat/source/${SRZ80_SOFTFLOAT_SOURCE}")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/softfloat/platform.h"
  CONTENT "/* SoftFloat platform configuration is supplied by compile definitions. */\n#define INLINE static inline\n")

add_library(srz80_softfloat STATIC ${SRZ80_SOFTFLOAT_SOURCES})
set_target_properties(srz80_softfloat PROPERTIES POSITION_INDEPENDENT_CODE YES)
target_include_directories(srz80_softfloat PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/softfloat"
  "${RV32EMU_ROOT}/src/softfloat/source/include"
  "${RV32EMU_ROOT}/src/softfloat/source/RISCV")
target_compile_definitions(srz80_softfloat PRIVATE
  LITTLEENDIAN=1 INLINE_LEVEL=5 SOFTFLOAT_ROUND_ODD SOFTFLOAT_FAST_INT64
  SOFTFLOAT_FAST_DIV64TO32)

option(SRZ80_OPTIMIZE_RV32EMU
  "Optimize the private RISC-V interpreter, including Debug builds" ON)
if(SRZ80_OPTIMIZE_RV32EMU)
  set(SRZ80_RV32EMU_OPTIMIZATION -O2)
else()
  set(SRZ80_RV32EMU_OPTIMIZATION -O0)
endif()

add_library(srz80_rv32emu OBJECT ${SRZ80_RV32EMU_SOURCES})
set_target_properties(srz80_rv32emu PROPERTIES POSITION_INDEPENDENT_CODE YES)
target_include_directories(srz80_rv32emu PRIVATE "${RV32EMU_ROOT}/src")
target_compile_options(srz80_rv32emu PRIVATE
  "-include${RV32EMU_ROOT}/src/common.h" ${SRZ80_RV32EMU_OPTIMIZATION})
target_compile_definitions(srz80_rv32emu PRIVATE ${SRZ80_RV32EMU_DEFINITIONS})

add_library(srz80_capstone_riscv STATIC
  ${CAPSTONE_ROOT}/cs.c ${CAPSTONE_ROOT}/Mapping.c ${CAPSTONE_ROOT}/MCInst.c
  ${CAPSTONE_ROOT}/MCInstrDesc.c ${CAPSTONE_ROOT}/MCRegisterInfo.c
  ${CAPSTONE_ROOT}/SStream.c ${CAPSTONE_ROOT}/utils.c
  ${CAPSTONE_ROOT}/arch/RISCV/RISCVDisassembler.c
  ${CAPSTONE_ROOT}/arch/RISCV/RISCVInstPrinter.c
  ${CAPSTONE_ROOT}/arch/RISCV/RISCVMapping.c
  ${CAPSTONE_ROOT}/arch/RISCV/RISCVModule.c)
set_target_properties(srz80_capstone_riscv PROPERTIES POSITION_INDEPENDENT_CODE YES)
target_include_directories(srz80_capstone_riscv
  PUBLIC "${CAPSTONE_ROOT}/include"
  PRIVATE "${CAPSTONE_ROOT}" "${CAPSTONE_ROOT}/arch/RISCV")
target_compile_definitions(srz80_capstone_riscv PRIVATE
  CAPSTONE_HAS_RISCV CAPSTONE_STATIC CAPSTONE_USE_SYS_DYN_MEM)

srz80_plugin(riscv KIND card
  SOURCES card.cpp $<TARGET_OBJECTS:srz80_rv32emu>
  OUTPUT cpu_riscv
  LIBS srz80_capstone_riscv srz80_softfloat
  INCLUDES "${RV32EMU_ROOT}/src"
  DEFINITIONS ${SRZ80_RV32EMU_DEFINITIONS})
add_dependencies(plugin_riscv srz80_rv32emu)
