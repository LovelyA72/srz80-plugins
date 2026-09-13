# V9938 / V9958 VDP card

A Yamaha V9938 video display processor for the rack, ported from the MAME 0.289
`v9938` device. It maps four IO ports, generates its own raster, drives the rack's
`IRQ` signal, and publishes a 544x626 RGBA8 video surface.
The surface also exposes card-owned scanout timing through `host.video.v1`:
frame number, next scanline and PAL/NTSC line count. The GUI transports these
values with the captured pixels; it never advances the raster. Card state version
3 preserves the frame counter and active command progress. It accepts versions
1/2 with an idle command unit; version 1 starts at frame zero. The corrected
state size includes the address latch. Old 192 KiB states that omitted their
last two VRAM bytes restore those bytes as zero.

```json
{"plugin": "vdp", "space": "cpu0.io", "base": "0x98", "size": 4, "clock": 0,
 "config": {"io_space": "cpu0.io", "model": 0, "vram_size": 131072,
            "raster_clock_hz": 21477272}}
```

| Config key | Default | Meaning |
| --- | --- | --- |
| `io_space` | `cpu0.io` | Name of the engine IO space holding the four VDP ports |
| `model` | `0` | `0` / `"V9938"` = V9938, `1` / `"V9958"` = V9958 |
| `vram_size` | `131072` | 128 KiB, 192 KiB (V9958), or 64 KiB. Range 0x20000-0x30000 |
| `raster_clock_hz` | `21477272` | VDP crystal. Range 1-50 MHz |

Ports, at `base + 0..3`: VRAM data, register/address/status, palette, indirect
register. These are the MSX VDP ports at 0x98-0x9B.

The inspector's Chip selector belongs to each card independently. Changing it
while stopped resets that VDP and persists the selected label in that card's
`config.model`; other cards keep their model and state. Numeric model values in
existing projects remain supported.

Cold reset and Stop clear VRAM, reset the VDP registers and release IRQ. Hot
reset resets registers and releases IRQ while retaining VRAM. Both publish an
opaque black frame immediately, including when paused or stopped with no raster
ticks. A running CPU can program the display again as soon as execution resumes.

## Implemented (phases 1–4)

- **TEXT 1, MULTICOLOR, GRAPHIC 1, GRAPHIC 2, GRAPHIC 3, GRAPHIC 4,
  GRAPHIC 5, GRAPHIC 6, GRAPHIC 7.**
- **Sprites**, both 8x8/16x16 modes with magnification, including the collision
  and fifth-sprite status bits.
- **The full register and status model**: all 48 control registers with their
  mask table, the ten status registers including the F/BL/CE/BD/FH/TR/VR flags,
  and the indirect access protocol on ports 1 and 3.
- **Timing**: vblank interrupt, the programmable line interrupt against R#19 and
  R#23, field alternation, blinking, PAL/NTSC and 192/212-line geometry.
- **VRAM**: 128 KiB plus the 64 KiB expansion window, with the GRAPHIC 6/7
  address interleave.
- **Save state**: registers, VRAM, raster position, palette and active/waiting commands.
- **Command unit**: POINT/PSET, search, line, logical and byte fills/copies,
  CPU transfers through R#44/S#7, and ABRT. Execution uses the MAME scanline
  budget and CE/TR handshakes; expansion VRAM and logical operations are supported.
- **The palette**: 16-entry V9938 palette with per-component programming, and the
  256-entry fixed palette GRAPHIC 7 uses.
- **V9958**: the S#1 identification bit, R#25's screen mode (and the R#25/R#26/
  R#27 GRAPHIC 2/3/4 scrolling), and the YJK (screen 12) and YAE (screens 10/11)
  GRAPHIC 7 pixel sources. The 512 KiB YJK colour table is one process-wide
  table built on first use, so a V9938 never pays for it.

Remaining integration work is in [`TODO-VDP.md`](TODO-VDP.md).

## What is not covered, and what a guest will see

| Missing | Behaviour |
| --- | --- |
| Mouse and lightpen | The colourbus inputs exist on the core and `status_r()` handles mouse mode, but nothing drives the deltas yet. |
| VRAM image slots | The descriptor declares none; a project cannot seed VRAM. |

## Files

| File | Role |
| --- | --- |
| `v9938.cpp`, `v9938.h` | Unmodified MAME 0.289 sources, kept as the diff reference. Not compiled. |
| `v9938_core.cpp` | The port. Every replacement of a MAME framework call carries a `PORT:` comment. |
| `card.cpp` | The card ABI: config, IO mapping, raster, IRQ, video surface, state, properties. |
| `v9938_core_test.cpp` | Registers, VRAM, ports, interrupts, PAL geometry, state. |
| `v9938_bitmap_test.cpp` | Line doubling, interlace, border, mid-frame mode change. |
| `v9938_command_test.cpp` | Command opcodes, logical operations, transfers, timing and state continuation. |
| `v9938_yjk_test.cpp` | V9958 identification, the lazy YJK table, screen 12 YJK, screens 10/11 YAE, R#25/R#26/R#27 scrolling, state. |
| `card_abi_test.cpp` | Loads the built plugin and drives the card ABI with a fake host. |

The core is included by `card.cpp` and compiled nowhere else, so the card is one
translation unit with no external dependency and is available in GUI-off builds.

## How the port differs from MAME

The VDP logic is the MAME code; only the framework seams changed. The recurring
replacements:

| MAME | Port |
| --- | --- |
| `m_vram_space->read_byte` / `write_byte` | `vram_barrier()` / `vram_barrier_w()` over a plain 256 KiB array, clamped to the fitted size so out-of-range reads return 0xff as the address space did |
| `device_palette_interface` | `m_pen16[]` / `m_pen256[]` tables, rebuilt from `m_pal_reg` on a state load |
| `screen()` and `bitmap_rgb32` | a fixed render buffer plus `convert_line()`, which byte-swaps MAME's `0xAARRGGBB` into the engine's RGBA8 order |
| `emu_timer` / `attotime` | one self-rescheduling `host.schedule` event per scanline |
| `devcb_write_line m_int_callback` | `irq_line()`, which drives the rack's `IRQ` signal |
| `machine().rand()` | the card's deterministic generator, which only feeds the spare bit S#2 reports |
| `s_pal_indYJK[0x20000]` | one process-wide `yjk_palette()` table built on first use, so only a model that renders YJK pays its 512 KiB |
| 44 `save_item()` calls | one field-wise `state_size`/`save_state`/`load_state` |
| `LOGMASKED` | a no-op, so the log sites stay diffable |

Two behaviours are worth knowing because they look like bugs and are not:

- **A blank screen at power-on is correct.** `R#1` bit 6 is the display enable;
  reset clears it. `R#0 = 0x02, R#1 = 0x40` is the minimal GRAPHIC 2 pair.
- **The sprite generator defaults onto the display tables.** `R#5 << 7` and
  `R#6 << 11` are both VRAM 0x0000 after reset, so a test or a program that has
  not set them gets sprites drawn out of the name and pattern bytes.

## Building and testing

[`examples/z80-vdp`](../../examples/z80-vdp/) is a runnable Z80 project that
programs this card and paints a test picture, for inspecting the port by eye in
the GUI.

The plugin is wired into the top-level `CMakeLists.txt` as `card_v9938`, built as
`video_v9938` next to the other cards, with five ctest targets: `v9938_core`,
`v9938_bitmap`, `v9938_command`, `v9938_yjk` and `card_v9938_abi`.

None of these targets needs the engine, SDL or ImGui, so they build without
configuring the GUI or waiting for the Rust engine:

```sh
cmake -S . -B build/vdp -G "MinGW Makefiles" -DSRZ80_BUILD_GUI=OFF \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build/vdp --target card_v9938 v9938_core_test v9938_bitmap_test v9938_command_test v9938_yjk_test card_v9938_test
ctest --test-dir build/vdp -R v9938 --output-on-failure
```

They also compile standalone, which is how they were developed:

```sh
cd plugins/vdp
g++ -std=c++20 -O1 -Wall -Wextra -o /tmp/v9938_core_test v9938_core_test.cpp
g++ -std=c++20 -O1 -Wall -Wextra -o /tmp/v9938_bitmap_test v9938_bitmap_test.cpp
g++ -std=c++20 -O1 -Wall -Wextra -o /tmp/v9938_yjk_test v9938_yjk_test.cpp
g++ -std=c++20 -O1 -Wall -Wextra -DSRZ80_PLUGIN_BUILD -I../../sdk/include -I../../sdk/helpers \
    -shared -o video_v9938.dll card.cpp
g++ -std=c++20 -O1 -Wall -Wextra -I../../sdk/include -o /tmp/card_abi_test card_abi_test.cpp
/tmp/card_abi_test ./video_v9938.dll
```

## Attribution

`v9938.cpp` and `v9938.h` are copyright Aaron Giles and Nathan Woods, licensed
BSD-3-Clause, and are redistributed here unmodified as the port's reference. See
[`third_party/README.md`](../../third_party/README.md). Retain that notice with
source and binary distributions.
