# Assembler

Open a source from **Project files**, then choose **Z80**, **MC68000**, **W65C02**,
or **W65C816** in the CPU selector. The tool appears under **Tools → CPU →
Assembler**. F5 assembles, F6 loads into the selected address space with a reset,
and F4 resumes the rack. Editing also assembles after a short pause.

**Save Project** saves each source's CPU selection and initial ORG in project
state. Reopening that source restores its settings. Paths inside the project
are stored relative to the project directory, so moving the project preserves
its selections. Sources without settings default to Z80. Changing CPU or ORG
invalidates the previous assembly before loading is allowed.

The internal plugin ID `z80_assembler`, binary name `cpu_z80_assembler`, and syntax
theme setting retain their existing names for projects that already use them.
Source extensions are `.asm`, `.s`, `.z80`, `.68k`, `.65c02`, and `.65816`;
the saved CPU selection determines how every extension is assembled.

| CPU | Address range | Data byte order | Encoder |
| --- | --- | --- | --- |
| Z80 | `0000–FFFF` | Little endian | libasm Z80 |
| MC68000 | `000000–FFFFFF` | Big endian | libasm MC68000, no coprocessors |
| W65C02 | `0000–FFFF` | Little endian | libasm W65C02S |
| W65C816 | `000000–FFFFFF` | Little endian | libasm W65C816 |

The MC68000 target emits original 68000 instructions; later 680x0 instructions
are rejected. Its instructions must begin at even addresses. Execution testing
will follow when an MC68000 card is available.

## Source syntax

Labels and symbols are case insensitive. Use `label:` on any target. MC68000,
W65C02, and W65C816 also accept labels without a colon in the first column.
Comments start with `;`; MC68000 also accepts whole comment lines starting with
`*`. Z80 expressions retain Intel/Zilog syntax. The other CPUs use Motorola
numbers (`$1234`, `%1010`) and C-style arithmetic/bitwise operators; `*` in an
expression is the current address. The existing arithmetic overflow checks
apply to every target.

`CPU name` or `.cpu "name"` is optional and checks that the source matches the
selected CPU. It does not override the saved project selection. Accepted aliases
include `68000`, `65C02`, `W65C02S`, `65816`, and `W65C816S`.

Common directives (an optional leading dot is accepted):

| Directive | Meaning |
| --- | --- |
| `ORG address` | Set the output address; gaps stay unfilled |
| `name EQU value` or `name = value` | Define a constant, including forward references |
| `DB values` | Bytes or double-quoted strings; aliases `DEFB`, `DEFM`, `BYTE`, `DC.B`, `FCB` |
| `DW values` | 16-bit values in target byte order; aliases `DEFW`, `WORD`, `DC.W`, `DC`, `FDB` |
| `DD values` | 32-bit values in target byte order; aliases `DEFD`, `LONG`, `DC.L`, `DL` |
| `DS count` | Reserve bytes without emitting them; aliases `DS.B`, `RMB` |
| `DS.W count`, `DS.L count` | Reserve words or longwords without emitting them |
| `DEFS count[, fill]` | Emit bytes, default fill zero |
| `ALIGN boundary`, `EVEN` | Emit zero padding to a boundary, or to an even address |

W65C02 and W65C816 also accept `* = address` for ORG. Data declarations do not
implicitly align: use `EVEN` or `ALIGN` explicitly where needed. Overlapping
output and output beyond the CPU's address range are errors. W65C816 instructions
that straddle a 64 KiB program bank boundary are rejected; data may cross banks.

## W65C816 register widths

Each assembly pass starts with 8-bit accumulator and index widths. Set the
assembler's assumptions explicitly where the program changes register widths:

```asm
.cpu "65816"
ORG $8000
    clc
    xce
    rep #$30
.al                         ; A16 (or LONGA ON)
.xl                         ; I16 (or LONGI ON)
    lda #$1234
    ldx #$5678
    sep #$30
.as                         ; A8 (or LONGA OFF)
.xs                         ; I8 (or LONGI OFF)
    lda #$12
```

These directives emit no bytes. `REP`, `SEP`, and `XCE` emit instructions but do
not infer assembler width state through runtime control flow. Width directives
are rejected for W65C02 and the other CPUs. The existing
`examples/tutorials/w65c816-nesapu/nesapu.asm` is supported with W65C816 selected.

This remains a single-source assembler with resolved output segments, symbols,
and a source listing. It does not implement macros, include files, conditional
assembly, object files, linking, or general compatibility with every directive
of an external assembler.

## Verification

With tools and `BUILD_TESTING` enabled:

```sh
cmake --build build/gcc-release --target plugin_z80_assembler_tool assembler_test assembler_tool_test
ctest --test-dir build/gcc-release -R '^assembler(_tool)?$' --output-on-failure
```

Tests cover known encodings for all four CPUs, forward references, data byte
order, address and bank limits, register widths, and project settings. The
W65C816 tutorial is compared against its existing 64tass ROM as read-only input.
The plugin test opens sources through the host ABI, restores targets in a new
instance, and captures F6 memory loads without requiring CPU cards.
