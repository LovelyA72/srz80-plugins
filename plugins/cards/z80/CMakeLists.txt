srz80_plugin(z80 KIND card
  SOURCES
    card.cpp
    disasm_bridge.c
    z80-mz800/cpu-z80/cpu/z80.c
    z80-mz800/dasm-z80/z80_dasm.c
    z80-mz800/dasm-z80/z80_dasm_analysis.c
    z80-mz800/dasm-z80/z80_dasm_format.c
    z80-mz800/dasm-z80/z80_dasm_tables.c
  OUTPUT cpu_z80
  INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}/z80-mz800/cpu-z80"
    "${CMAKE_CURRENT_LIST_DIR}/z80-mz800/dasm-z80")
