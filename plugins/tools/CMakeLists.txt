# Tool projects are registered here
set(SRZ80_TOOL_DIRECTORIES
  z80_assembler patchbay midi keyboard memory_loader floppy ym2413_inspector
  ymw258_inspector swp00_inspector 3ws8pn_inspector)

foreach(tool IN LISTS SRZ80_TOOL_DIRECTORIES)
  add_subdirectory("${tool}")
endforeach()
