# V9938 / V9958 — remaining work

Phases 1–4 are implemented: the card loads, maps its four IO ports, runs a
scanline raster from a scheduled event, drives the rack's `IRQ` signal, publishes
a 544x626 RGBA8 surface, exposes properties, renders every V9938 mode plus the
V9958 YJK/YAE modes, and round-trips its save state. `v9938_core_test.cpp`,
`v9938_bitmap_test.cpp`, `v9938_command_test.cpp`, `v9938_yjk_test.cpp` and
`card_abi_test.cpp` cover it.

This file is what is *not* done. Every item names the exact region of the
vendored MAME original (`plugins/vdp/v9938.cpp`, unmodified) that the port still
has to take over.

---

## Phases 2 and 3 — implemented

GRAPHIC 5/6/7 now render packed bitmap pixels, mode-specific borders and
sprites. `v9938_bitmap_test.cpp` checks known palette values, VRAM interleave,
GRAPHIC 2/3 vertical scroll and wrap, R#18 vertical adjustment, 212-line
PAL/NTSC geometry, and alternating interlace fields/pages. Surface conversion
now follows the actual field row written by the renderer.

The MAME command unit is ported: ABRT, POINT, PSET, SRCH, LINE, LMMV, LMMM,
LMCM, LMMC, HMMV, HMMM, YMMM and HMMC, including logical operations,
expansion-memory selection, transfer handshakes and the 13662-unit scanline
budget. `v9938_command_test.cpp` drives the indirect register port, checks
known VRAM results, and verifies active/waiting command save and continuation.
Command logging remains a no-op, consistent with the other ported log sites.

Card state version 3 persists command counters and arguments, reconstructs
only active engines, and fixes the old size calculation's omitted address
latch. Versions 1/2 remain readable as idle command states. Old 192 KiB states
could omit their final two VRAM bytes; these load as zero.

---

## Phase 4 — V9958 (YJK) — implemented

`model: 1` now selects every difference the port had deferred:

- **`s_pal_indYJK[0x20000]`** (`v9938.cpp:355`). MAME's 512 KiB static table is
  one process-wide `yjk_palette()` built on first use, so a V9938 (or a V9958
  that never renders YJK) never pays the allocation or the one-time loop.
- **`palette_init()`** (`v9938.cpp:357-378`). The base implementation warms the
  lazy table when `m_model == MODEL_V9958`; a card may still override it.
- **YJK/YAE rendering** (`mode_graphic7`, `v9938.cpp:1373-1438`). The screen 12
  YJK branch and the screens 10/11 YAE branch are ported verbatim, reading the
  per-pixel index out of the group of four bytes.
- **`R#25`/`R#26`/`R#27`** (`v9938.cpp:877-890`). The register writes and the
  GRAPHIC 2/3/4 scroll paths were already present; `v9938_yjk_test.cpp` now
  covers the name-table bank flip and the dot-scroll mask on a V9958.
- **`S#1` bit 2** (`v9938.cpp:690`). `v9938_yjk_test.cpp` checks it at the core
  level and `card_abi_test.cpp` checks the card's chip selector end to end.

---

## Cross-cutting gaps

### A. Line-timer accuracy — measured, then decided

The card reschedules itself with `host.schedule(1e9 / line_rate_ns)` per
scanline, using the configured `raster_clock_hz`. The two other candidates from
the original evaluation are still open:

1. **A subscribed master clock at ~21.5 MHz.** Exact cadence, but 21.5M
   callbacks per simulated second, and it requires the project to enable a third
   clock at that rate. `srz80_engine_frequency` caps clocks at 50 MHz, so it is
   representable.
2. **A CPU-derived counter with phase accumulation.** Cheapest, but bakes in the
   MSX 21.477/3.579545 ratio and produces per-line jitter equal to one CPU tick.

`card_abi_test.cpp` already runs 60 simulated frames as a smoke measure. What is
missing is a decision backed by numbers: measure wall time for a frame under each
scheme, and confirm that the scheduled path's nanosecond rounding does not drift
against the engine's own time base. If it does, switch to option 1.

Related: the raster currently assumes the engine's nanosecond is a fixed
simulated quantity, which is true at the default `SRZ80_TIME_FIXED` mode. Under
`SRZ80_TIME_SYSTEM` the frame rate would track the wall clock instead. That is
worth an explicit check and a line in the card README either way.

### B. Video readback cost — unmeasured

`ui/simulation_snapshot.cpp` pulls the whole surface (1.36 MiB at 544x626 RGBA8)
through `srz80_engine_video_read` whenever the video panel is visible. The card's
query is a `memcpy`, so the cost is in the engine's chunked copy plus
`SDL_UpdateTexture`, not in rendering. The surface is also fixed at PAL height,
so an NTSC picture spends its bottom 102 rows on never-drawn black.

Open questions: measure the per-frame copy cost; decide whether the surface
should be re-registered on a PAL/NTSC switch (the host copies geometry at
registration, so this is currently impossible without a re-register) or whether
the UI should crop using the `pal` property that already exists.

### C. Colorbus (mouse and lightpen) is present but unreachable

`colorbus_x_input`, `colorbus_y_input` and `colorbus_button_input` are ported and
public on the core, and `status_r()` implements the `R#8` bit 7/6 mouse mode
(registers 3 and 5 return the delta, `S#1` returns the buttons). Nothing drives
them: the card has no input registration.

Options: `host.input.v1` byte queues (natural fit, since the deltas are
accumulated and consumed on read), or a dedicated `SrhDataProviderV1` from
`providers.h` for a "VDP" inspector page. Note that the existing `providers.h`
comment says all provider callbacks run on the simulation thread, which is where
these setters must be called from.

### D. CPU-to-VDP transfer timing — implemented in phase 3

R#44 writes and S#7 reads advance the active command when budget remains.
The native command suite covers LMCM, LMMC and HMMC transfer handshakes.

### E. IO space name discovery

The card reads its IO space name from its own config (`io_space`, default
`cpu0.io`) and looks it up through `host.resources.v1`, matching the Z80 card.
It cannot discover the name from the descriptor the host used, because the host
resolves that string before calling `create`. If a project names the Z80's IO
space something else, the VDP needs the same value in its own config. Worth a
documented convention if a third CPU card ever appears.

### F. VRAM image slots

The descriptor declares no `image_slots`, and `save_project_data` writes an empty
chunk. The engine already has the mechanism (`SrhConfig::images`,
`SrhImageSlotDescriptor`), so an MSX-style machine could ship a VRAM seed image
instead of relying on the guest to fill it. Low priority, but it is the reason
those two ABI tail entries are implemented as no-ops today.

---

## Known-good behaviours worth not regressing

These were established by measurement during phase 1 and are each covered by a
test. They are listed because they look like bugs and are not.

- **A blank screen is the correct power-on state.** `R#1` bit 6 is the display
  enable (`BL`); reset clears it, so the chip shows the backdrop until the guest
  sets it. `R#0 = 0x02, R#1 = 0x40` is the minimal GRAPHIC 2 pair.
- **`S#2` bit 1 is both the PAL select source and the field flag**, so it
  alternates every vblank. The PAL *field rate* is the stable consequence, not
  the bit's value.
- **The sprite generator defaults onto the display tables.** `R#5 << 7` and
  `R#6 << 11` are both `0x0000` after reset, so a graphical test must move them
  or sprites are drawn out of the name and pattern bytes. This bit the phase-1
  tests for a while.
- **`0x4f` in a GRAPHIC 2 colour byte means foreground pen 4 and background pen
  15**, in that order: high nibble on a set bit, low nibble on a clear one. For
  TEXT 1 the R#7 nibbles are the other way round.
- **The mode renderer starts at `R#18`'s horizontal offset**, 8 doubled pixels =
  16, and each pattern bit covers two surface pixels.
- **GRAPHIC 2/3 colour-table base uses R#3 bit 7**, while its low bits
  mask character selection. Keep name, pattern and colour tables disjoint
  when checking scrolling; the new scroll tests use a colour table at 0x2000.
