#include <stdint.h>
#include <string.h>
#include "z80_dasm.h"

typedef struct SrhDisasmReader {
    uint16_t pc;
    const uint8_t *bytes;
    uint32_t bytes_len;
} SrhDisasmReader;

static u8 Srh_disasm_read(u16 addr, void *data) {
    SrhDisasmReader *reader = (SrhDisasmReader *)data;
    uint32_t offset = (uint32_t)(addr - reader->pc);
    if (offset < reader->bytes_len)
        return reader->bytes[offset];
    return 0xFF;
}

int srz80_z80_dasm(uint16_t pc, const uint8_t *bytes, uint32_t bytes_len,
                   uint32_t *length, uint32_t *tstates, char *text, uint32_t text_size) {
    z80_dasm_inst_t inst;
    z80_dasm_format_t format;
    SrhDisasmReader reader;
    int decoded;
    if (!bytes || !length || !tstates || !text || text_size == 0)
        return -1;
    memset(&inst, 0, sizeof(inst));
    reader.pc = pc;
    reader.bytes = bytes;
    reader.bytes_len = bytes_len;
    decoded = z80_dasm(&inst, Srh_disasm_read, &reader, pc);
    if (decoded <= 0) {
        if (text_size > 0) {
            text[0] = '\0';
        }
        *length = 1;
        *tstates = 0;
        return 0;
    }
    z80_dasm_format_default(&format);
    format.show_bytes = 1;
    format.hex_style = Z80_HEX_HASH;
    if (z80_dasm_to_str(text, (int)text_size, &inst, &format) < 0)
        return -2;
    *length = inst.length;
    *tstates = inst.t_states;
    return 0;
}
