# SR Visual Synthesizer (VSN), native contract revision 1

VSN consumes graphics in ordinary SRZ80 shared memory. Phases 0–6 implement
NES graphics data, packed 4bpp, packed 8bpp and 16x16 packed 8bpp backgrounds,
VT planar 2bpp/4bpp background and sprites, RGB444/RGB555 palettes, the
512-byte extended sprite table and the 16x16x8 packed high-resolution mode;
this is **not an NES PPU register interface** or an unchanged NES-program
execution environment.
Physical interrupts and DMA remain phase 7. Unimplemented registers are
reserved, read zero and ignore writes. This document freezes their allocation
without advertising them.

## Installation and configuration

Plugin ID `vsn`, category Video, display name SR Visual Synthesizer. Default
mapping: `cpu0.io`, base `0x80`, exactly 128 bytes. No rack clock subscription.
Configuration is a JSON object; unknown keys and wrong types fail creation.

| Key | Default | Accepted value |
| --- | --- | --- |
| io_space | cpu0.io | nonempty UTF-8 name, at most 128 bytes; must resolve to the supplied mapping space |
| memory_space | cpu0.mem | nonempty name, at most 128 bytes |
| region | NTSC | NTSC, PAL, VGA |
| raster_clock_hz | region crystal | integer 1–1000000000 Hz |
| nmi_signal | NMI | name up to 128 bytes, empty disables; reserved until phase 7 |
| irq_signal | IRQ | name up to 128 bytes, empty disables; reserved until phase 7 |
| strict_memory | false | boolean; abort a faulty visible line and replace it with opaque black |

Requires `host.resources.v1` named lookup and `host.video.v1` extended surface
registration/timing. Signals are not required or driven in phases 0–4. Missing
spaces, invalid mapping size/overflow, mapping conflicts and unavailable required
callbacks fail with a creation diagnostic. IO and memory may be the same space:
reset has no active fetches, and every later fetch is checked against the entire
MMIO window. No recursive MMIO reads are performed. The public host ABI has no
space-size query; out-of-space errors are reported by host.read. Unclaimed addresses inside a
space return the host's configured fallback as successful reads: the current
ABI cannot distinguish these from mapped bytes. VSN therefore faults on host
errors, arithmetic overflow and MMIO aliasing; it cannot detect bus fallback.
Use fully mapped RAM/ROM for deterministic graphics assets.

## Register file

Offsets are relative to the mapping base; multi-byte fields are little-endian.
All registers reset to zero except the values explicitly given below. RO writes
are ignored. Every read, including peek, is pure. Unlisted bits read zero and
ignore writes. Rendering observes all register writes together at the next
scanline event; there are no dot-level latches. Multi-byte programming is not
atomic across scanline events.

| Offset | Bytes | Name | Access, bits and reset |
| --- | --- | --- | --- |
| 00–03 | 4 | IDENT | RO bytes `56 53 4e 01` (VSN, revision 1) |
| 04 | 1 | CONTROL | RW bit 0 master enable, 1 background, 2 sprites, 3 show BG in left 8, 4 show sprites in left 8, 5 strict 8-sprite limit, 6 8x16 NES sprites; reset 20 |
| 05 | 1 | MODE | RW 0 NES, 1 packed4, 2 planar4, 3 packed8, 4 packed16, 5 hires; values above 5 ignored |
| 06 | 1 | STATUS | bits 0 memory fault, 1 sprite-zero hit, 2 sprite overflow: sticky W1C; bit 7 live vblank, RO |
| 07 | 1 | FEATURES | RO 3f: bit 0 NES, bit 1 packed4, bit 2 planar4, bit 3 packed8, bit 4 packed16, bit 5 hires |
| 08–0f | 8 | interrupt allocation | reserved; pending/enables/routing in phase 7 |
| 10 | 2 | WIDTH | RO active logical width: 256 (modes 0–4), 512 (mode 5) |
| 12 | 2 | HEIGHT | RO active logical height: 240 (modes 0–4), 480 (mode 5) |
| 14 | 2 | SCROLL_X | RW unsigned pixel offset |
| 16 | 2 | SCROLL_Y | RW unsigned pixel offset |
| 18 | 1 | NES_COLOR | RW bit 0 grayscale; bits 1–3 RGB emphasis |
| 19–1f | 7 | geometry allocation | reserved |
| 20 | 4 | MAP_BASE | RW linear address |
| 24 | 4 | BG_TILE_BASE | RW linear address |
| 28 | 2 | MAP_ROW_STRIDE | RW packed map bytes per tile row, reset 64; zero repeats first row |
| 2a | 1 | MAP_WIDTH | RW packed map tiles, reset 32, zero means 256 |
| 2b | 1 | MAP_HEIGHT | RW packed map tiles, reset 30, zero means 256 |
| 2c | 2 | NES_PAGE_X_STRIDE | RW bytes to right nametable, reset 1024 |
| 2e | 2 | NES_PAGE_Y_STRIDE | RW bytes to lower nametable, reset 2048 |
| 30 | 4 | SPRITE_BASE | RW sprite table: NES 256-byte OAM in modes 0/2, extended 512-byte table in modes 1/3/4 |
| 34 | 4 | SPRITE_TILE_BASE | RW linear address |
| 38 | 1 | NES_PATTERN | RW bit 0 BG pattern table, bit 1 8x8 sprite pattern table |
| 39 | 1 | PLANAR | RW bit 0 BG depth, bit 1 sprite depth (0 2bpp, 1 4bpp); planar4 mode only |
| 3a–3f | 6 | sprite allocation | reserved |
| 40 | 4 | PALETTE_BASE | RW linear address |
| 44 | 1 | PALETTE_FORMAT | RO, derived: 0 NES indices in mode 0; 1 RGB444 in modes 1 and 2; 2 RGB555 in modes 3, 4 and 5 |
| 45 | 1 | BACKDROP | RW packed palette index; NES always uses palette entry 0 |
| 46–4f | 10 | palette allocation | reserved |
| 50 | 2 | raster compare allocation | reserved |
| 52 | 2 | CURRENT_X | RO zero (scanline granularity) |
| 54 | 2 | CURRENT_Y | RO next scanline |
| 56 | 8 | FRAME | RO little-endian frame count |
| 5e–5f | 2 | raster allocation | reserved |
| 60–6f | 16 | DMA allocation | reserved: source 60/u32, destination 64/u32, count 68/u32, command 6c/u8, status 6d/u8, fill 6e/u8 |
| 70–7f | 16 | extended allocation | reserved |

Interrupt allocation for phase 7: 08 pending W1C, 09 enable, with bits 0 vblank,
1 raster, 2 DMA complete, 3 DMA fault. Vblank routes to NMI; others to IRQ.
Acknowledgement clears causes only, never enables. No interrupt behavior is
implemented yet; STATUS is independent. Future DMA command bits: start 0,
fill 1, hold source 2, hold destination 3; status busy 0, complete 1, fault 2.

## Shared formats and address examples

All address expressions are evaluated without 32-bit wrapping. A result above
`ffffffff`, failed host access or MMIO alias returns `ff` and sets memory fault.
Writes use the same checks and are discarded on failure. The renderer never
writes guest memory. Fault count saturates at UINT64_MAX, records the last
address/access type and does not log per fetch. In strict mode the first failed
fetch aborts the line; the next scanline retries normally.

Packed4 maps use little-endian 16-bit descriptors: tile index bits 0–11, palette
bank bits 12–15. With scrolled/wrapped pixel `(x,y)`, the descriptor address is
`MAP_BASE + (y/8)*MAP_ROW_STRIDE + (x/8)*2`. Wrap periods are
`8*MAP_WIDTH` and `8*MAP_HEIGHT`. A tile is 32 bytes, row-major; even X is the
**high nibble**. Pixel address is `BG_TILE_BASE + tile*32 + (y%8)*4 + (x%8)/2`.
Example: tile 3, local (5,2) reads `BG_TILE_BASE+106`, low nibble. Index zero
selects BACKDROP, otherwise palette index is `bank*16+pixel`. Packed backgrounds
ignore NES clipping and pattern-table controls; their sprites use the extended
table below.

Packed8 (mode 3) uses the same descriptor map, wrap periods and scroll units as
packed4, but stores one byte per pixel and 64 bytes per tile: pixel address is
`BG_TILE_BASE + tile*64 + (y%8)*8 + (x%8)`. Example: tile 3, local (5,2) reads
`BG_TILE_BASE+213`. Index zero selects BACKDROP; any other pixel value is the
full 8-bit palette index, so descriptor bank bits are ignored.

Packed16 (mode 4) uses 16x16-pixel background tiles. The 16-bit descriptor map is
read as `MAP_BASE + (y/16)*MAP_ROW_STRIDE + (x/16)*2`, wrap periods are
`16*MAP_WIDTH` and `16*MAP_HEIGHT`, and tiles are 256 bytes: pixel address is
`BG_TILE_BASE + tile*256 + (y%16)*16 + (x%16)`. Example: tile 3, local (5,2)
reads `BG_TILE_BASE+805`. Index zero selects BACKDROP; otherwise the pixel value
is the 8-bit palette index and descriptor bank bits are ignored.

Hires (mode 5, high resolution) uses the exact same 16x16x8 packed tile format,
descriptor map, wrap periods, palette and extended sprite table as packed16, but
renders natively at the full 512x480 logical viewport instead of 256x240. Every
coordinate is in output pixels: scroll advances one pixel per unit, a 16x16 tile
covers a 16x16 output block, and a 32x30 descriptor map fills one whole frame
with the default `MAP_WIDTH`/`MAP_HEIGHT`. There is no border and no MAME-style
"pretend it is 8x8" approximation: both the even and odd rows of every 16x16
tile are sampled.

Planar4 (mode 2) uses the same 16-bit descriptor map, wrap periods and scroll
units as packed4. Tiles are decoded as bitplanes instead of nibbles: BG tile
address is `BG_TILE_BASE + tile*32 + plane*8 + (y%8)` for 4bpp (planes 0–3) or
`BG_TILE_BASE + tile*16 + plane*8 + (y%8)` for 2bpp (planes 0–1). Plane 0 is
the least-significant bit, bit 7 is leftmost:
`pixel = plane0 | plane1<<1 | plane2<<2 | plane3<<3`. Pixel zero selects
BACKDROP, otherwise palette index is `bank*16+pixel` (4bpp) or `bank*4+pixel`
(2bpp). PLANAR bit 0 selects the BG depth. This is the native linear form of the
VT03 four-plane fetch: the hardware's second `0x2000` plane bank is folded into
`plane*8` offsets within a single 32-byte tile, and the bank-derived tile
high bits become descriptor bits 0–11.

RGB444 is 256 little-endian 16-bit entries at `PALETTE_BASE+index*2`: bits 0–3
blue, 4–7 green, 8–11 red; upper bits ignored. Each channel expands by *17.
Example `0f 0a` is (170,0,255,255). The entire palette is latched once per
frame at the first enabled visible line that uses it, including lines with both
layers disabled, and held until the next frame or until MODE, PALETTE_BASE or
NES_COLOR changes mid-frame.

RGB555 is the mode 3/4 palette: 256 little-endian 16-bit entries at
`PALETTE_BASE+index*2` with blue bits 0–4, green 5–9, red 10–14 and bit 15
ignored. Each 5-bit channel expands with `(v<<3)|(v>>2)`. Example `1f 00` is
(255,0,0,255). Latching rules match RGB444, and `PALETTE_FORMAT` reports 2.

NES mode wraps the scrolled background on a logical 512x480 canvas. Each
256x240 page begins at `MAP_BASE + pageX*NES_PAGE_X_STRIDE +
pageY*NES_PAGE_Y_STRIDE`. Within a page: 960 tile bytes (32x30), followed by
64 attribute bytes. Attribute address is `page+960+(tileY/4)*8+tileX/4`;
shift is `(tileY%4/2)*4+(tileX%4/2)*2`. This selects four 16x16-pixel quadrants.
For tile (3,2), attribute byte is page+960, shift 6. Choose X/Y page strides
1024/2048 for four-screen, 1024/0 for vertical mirroring, 0/1024 for horizontal
mirroring, 0/0 for one-screen. There is no implicit cartridge mirroring.

NES patterns use 16 bytes per 8x8 tile: plane 0 rows at 0–7, plane 1 at 8–15,
bit 7 is leftmost. BG address is `BG_TILE_BASE + table*4096 + tile*16 + row`;
table is NES_PATTERN bit 0. Example tile 2, row 3 reads offsets 35 and 43.
Pixel is plane0 bit | (plane1 bit << 1). Background zero is transparent and
uses palette entry 0; nonzero is `attributeBank*4+pixel`.

NES palette memory holds 32 one-byte master-palette indices, low 6 bits used.
Entries 10/14/18/1c (hex) alias 00/04/08/0c when latched; unused sprite zero
entries never contribute visible pixels. Grayscale masks the master index
with 30 (hex). The reference colors are the original **VSN Classic** fixed digital palette
in `nes_palette.hpp`; those 64 literal RGB values define the contract. This is
an approximation, not analog composite emulation. Emphasis retains selected
RGB channels and multiplies unselected channels by 3/4 (integer truncation);
all three selected attenuates all channels by 3/4. PAL swaps the red/green
emphasis bits. Alpha is always 255.

NES OAM is exactly 64 records of `Y,tile,attributes,X`. Top is Y+1; Y=239–255
hides a sprite (no byte wrap). Attribute bits 0–1 palette, 5 behind background,
6 flip X, 7 flip Y; other bits ignored. 8x8 patterns use SPRITE_TILE_BASE plus
NES_PATTERN bit 1 *4096. 8x16 sprites use tile bit 0 for the pattern table and
`tile&fe` for the first tile; Y flip reverses the entire 16 rows. Example tile 3,
local row 10 reads offsets 4146 and 4154 from SPRITE_TILE_BASE. The 256 OAM
bytes are latched once per frame at the first visible line with sprites enabled
and held until the next frame or until MODE or SPRITE_BASE changes mid-frame.

Sprites are selected in OAM order. Nine or more in-range sprites set overflow;
strict mode renders only the first eight. Overflow is a simple count, not the
2C02 hardware overflow bug. Lowest OAM index wins ties, even if its behind-BG
flag hides it. Nonzero sprite pixels use `16+bank*4+pixel`; zero is transparent.
Sprite-zero hit requires nonzero BG and sprite 0 pixels, both enabled and not
left-clipped, and X != 255; it occurs even for a behind-BG sprite. Hit/overflow
clear at pre-render or by W1C. Sprite positions never scroll. BG and sprite
left-edge masking are independent and only affect NES and planar4 modes.

Planar4 sprites use the same 256-byte OAM `Y,tile,attribute,X` records and the
same selection, flip, priority, sprite-zero, left-mask and 8-sprite-limit rules
as NES mode, but decode tiles from a single linear base with no pattern-table
split. Sprite tile address is `SPRITE_TILE_BASE + tile*32 + plane*8 + row`
(4bpp) or `SPRITE_TILE_BASE + tile*16 + plane*8 + row` (2bpp); 8x16 sprites pair
two consecutive tiles via `tile&fe`. Palette index is `bank*16+pixel` (4bpp) or
`bank*4+pixel` (2bpp), where bank is OAM attribute bits 0–1. PLANAR bit 1
selects the sprite depth. Sprites and background share the 256-entry RGB444
palette; a nonzero sprite pixel is never interpreted as a NES master index.

Extended sprites are the packed-mode sprite format (modes 1, 3, 4 and 5). They read
the 512-byte table at SPRITE_BASE as 32 records of 16 bytes: signed little-endian
X/i16 at 0, Y/i16 at 2, tile/u32 at 4 (low 20 bits), palette/u8 at 8 (low 4
bits), flags/u8 at 9 (flip X 0, flip Y 1, behind BG 2, enable 3), size/u8 at 10
(0=8x8, 1=8x16, 2=16x16), reserved zero at 11–15. X and Y are the top-left
corner and may be negative; every pixel is clipped, so partially and fully
offscreen sprites are ordinary cases rather than hidden-byte rules. Records are
selected in table order and the lowest enabled record wins a pixel tie, even
when its behind-BG flag hides it. Size 1 pairs two consecutive 8x8 tiles via
`tile&~1`; sizes 0 and 2 each fetch one tile. Tile addressing is
`SPRITE_TILE_BASE + tile*32 + (y%8)*4 + (x%8)/2` for 4bpp and
`SPRITE_TILE_BASE + tile*64 + (y%8)*8 + (x%8)` for 8bpp, with the 16x16 forms
`tile*128 + (y%16)*8 + (x%16)/2` and `tile*256 + (y%16)*16 + (x%16)`. Mode 1
sprites are 4bpp, and the record palette is a 16-entry bank (`palette*16+pixel`);
modes 3/4 sprites are 8bpp and the pixel value is the full palette index, so the
record palette is ignored. Pixel zero is transparent. Extended sprites have no
sprite-zero hit, no 8-sprite limit, no overflow flag and no left-edge masking;
those are NES/planar4 rules. The 512 bytes are latched once per frame at the
first visible line with sprites enabled and held until the next frame or until
MODE or SPRITE_BASE changes mid-frame.

Future optional 32-bit background tile descriptors allocate tile bits 0–19,
palette 20–23, flip X 24, flip Y 25, priority 26, reserved 27–31. Later modes
must retain the extended sprite layout; unproven VT hardware forms require
separate mode IDs.

## Raster, surface and lifecycle

One scheduled event processes one line. Reset starts at pre-render (last line),
frame 0; the first event clears hit/overflow and advances to line 0/frame 1.
Visible lines 0–239, post-render 240, vblank 241 through total-2, pre-render
at total-1. Vblank status changes on processing lines 241 and total-1.

| Region | Default master Hz | Master periods/line | Total lines | Frame rate |
| --- | --- | --- | --- | --- |
| NTSC | 21477272 | 1364 = 341 dots *4 | 262 | 60.09848 Hz |
| PAL | 26601712 | 1705 = 341 dots *5 | 312 | 50.00698 Hz |
| VGA | 25175000 | 800 | 525 | 59.94048 Hz |

No odd-frame dot skip. VGA changes cadence only in phases 0–3. Custom clocks
retain periods/line. Integer remainder accumulation makes N delays sum to
`floor(N*periods*1000000000/clock)` nanoseconds; no wall clock is consulted.
Master disable stops memory fetches but not raster/frame time. Disabled lines
and reset are opaque black. Enabled lines use the configured palette backdrop.

The card always publishes a 512x480 RGBA8 surface. Modes 0–4 render a 256x240
logical viewport and are upscaled with exact nearest-neighbor 2x (each logical
pixel becomes a 2x2 block), so the whole surface is meaningful with no border.
High-resolution mode 5 renders natively at 512x480 and fills the surface
directly. Each of the 240 visible raster lines produces two surface rows in
every mode: mode 5 samples two distinct 16x16-tile rows, low-res modes duplicate
one upscaled row. The surface is double-buffered: the raster renders into a
back buffer and publishes a whole completed frame into the front buffer at the
end of the visible area (the post-render line), so a video query always copies
one complete frame rather than a mid-render image. Video and timing queries only
copy stored data. They do not fetch, render, advance counters or clear flags.
Reset (warm or cold) resets registers, counters, faults, both framebuffers and
scheduling remainder, preserving shared RAM. Destroy cancels the event and
unmaps MMIO; video providers use host owner teardown.

The standalone core has validated fixed-endian snapshots (`VSN1`, version 3;
version 2 added the second framebuffer, version 3 enlarges both framebuffers to
the 512x480 surface) for register/raster/fault/framebuffer testing. Card ABI save/load is deliberately
unavailable until phase 8: the current host API supplies no simulated scheduler
clock or post-restore hook, and `host.time_ns` may be fixed or wall-clock time.
An exact remaining-event deadline cannot be restored portably through that API.
No wall-clock reads are used. Shared memory belongs to its RAM/ROM owner.
Runtime properties are read-only.

## Implementation boundaries and licensing

All shipped VSN implementation is MIT licensed; see `LICENSE`.
No MAME code, palette tables or reference files are bundled. `PLAN.md` remains
the original roadmap; its proposed MAME extraction/licensing route is superseded
by the native implementation described here.

- `vsn_memory.hpp`: storage transport interface, independent of the host ABI.
- `vsn_layout.hpp`: pure descriptor/address/pixel decoding for tile formats.
- `vsn_renderer.hpp`: replaceable video backend with reset/frame/scanline hooks.
- `vsn_renderer.cpp`: line-local tile/palette/OAM renderer; no scheduler or host types.
- `vsn_core.*`: register device, checked memory gateway, raster and RGBA storage.
- `card.cpp`: configuration, host memory transport, MMIO, event and video registration.

A different memory structure changes the transport and/or format decoders,
without changing scheduling or compositing. A future 3D backend can keep scene,
depth and full-frame buffers internally, prepare a frame at its first scanline,
and copy its output through the same scanline interface. It does not need any
SRZ80 code. Backend selection/register extensions and serialization of persistent
backend data must accompany that feature. The current backend caches only the
frame-latched palette and sprite table between scanlines; those caches are
invalidated by `reset()`, so core snapshots need no backend payload and a load
re-latches from shared memory at the next frame.

## Build

Build target `plugin_vsn`; output `video_vsn`.
