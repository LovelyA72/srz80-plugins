# RISC-V VSN: Four Corners

An RV32IMF demo with RAM, ROM, VSN, and UART. Move around a 512×480 world
through a 256×240 viewport. Press `M` to switch between NES graphics, planar4,
packed8, packed16, and native 512×480 hires. The firmware generate tilesets in
shared RAM.

![The four environments meeting at the center of the world](preview.png)

## Run and control

Load `project.json` in SRZ80 with the `ram`, `rom`, `riscv`, `vsn`, and
`uart_console` plugins installed, then resume the rack. `demo.rom` is included.

Open the VSN video display and the console for endpoint **`vsn.uart`**. Turn on
**Live mode** in the console, focus its input field, and type:

| Key | Action |
| --- | --- |
| W / A / S / D | Move the camera up / left / down / right by 8 pixels |
| R | Return to the central crossroads (128, 120) |
| M | Cycle NES → planar4 → packed8 → packed16 → hires |
| P | Toggle the raster-interrupt split: a fixed RUINS overview band |
| ? | Print the controls |

Uppercase works too. Each character acts immediately. Line endings are ignored.
In normal console mode, submit `wwdd` to move twice up and twice right. Each
letter moves one step.

The UART prints the camera target. RV32F arithmetic moves the view toward it.
The target is the viewport's world origin, bounded to X=0…256 and Y=0…240.
Animation continues without input.

The VSN surface is always 512×480. It scales the 256×240 modes by 2× with
nearest-neighbor sampling. Hires shows the whole world and pins the camera to
the origin.

## How it works

The CPU runs at 4 MHz and boots at address zero. The ROM targets
`rv32imf_zicsr` / `ilp32f`. Startup enables floating point and initializes
`.data` and `.bss` after cold and warm resets.

NES mode uses 2bpp backgrounds and sprites. Four 1 KiB nametables have separate
attribute tables and page strides of 1024 and 2048 bytes. Each page has a
four-colour background palette. House clearings use the green palette, with red
houses drawn as sprites. VSN base registers point into shared RAM.

The first `M` press selects planar4 (MODE 2). It uses a 2bpp background with
16-bit descriptors (12-bit tile index and 4-bit palette bank) and 4bpp sprites.
A 256-entry RGB444 palette replaces the NES palette. Its first 16 entries keep
the four background banks.

Packed8 (MODE 3) uses 8×8 tiles and a 64×60 descriptor map. Packed16 (MODE 4)
uses 16×16 tiles and a 32×30 map. Hires (MODE 5) uses the packed16 formats at
native 512×480 resolution. Row 10 has a checkerboard, lines, and "HI RES" text
drawn one pixel wide. The same strip appears as 2×2 blocks in packed16.

In packed modes, RGB555 entries 0–127 form a repeating eight-stop gradient.
Each world cell has a tile that samples this field at each pixel. Terrain pixels
shift the palette index to give grass, water, roads, and ruins different levels.
Packed16 samples the field at 16×16 resolution.

Packed pixels store full palette indices. Signs use a fixed white entry.
Entries 128–255 hold the sprite ramps.

Packed modes use a 512-byte extended sprite table with 32 records. Each record
has signed X/Y coordinates, a 20-bit tile index, flags, and a size selector.
Four 16×16 crystals near the crossroads use separate RGB555 ramps. They occupy
the first four records and VSN clips them at the viewport edge. NES and planar4
use the 64-entry, 256-byte OAM table.

Sprite screen coordinates subtract the background camera position from world
coordinates. NES and planar4 sprites crossing the left or top edge are culled
because their OAM coordinates are unsigned. The eight-sprites-per-line limit is
disabled.

The CPU builds sprite records at `0x99000`. VSN copies them by DMA into the
inactive OAM table during scanout. The DMA interrupt marks the upload complete.
A delayed UART burst can defer an update to the next early vblank.

| Address | Contents |
| --- | --- |
| `0x00000000` | Firmware ROM, below `0x10000` |
| `0x00010000`–`0x00010fff` | Four nametables and attributes |
| `0x00011000`–`0x00011fff` | Background patterns and sign font |
| `0x00012000`–`0x00012fff` | NES sprite patterns |
| `0x00013000`–`0x000130ff` | First NES OAM buffer |
| `0x00013100`–`0x0001311f` | 32 NES palette indices |
| `0x00013200`–`0x000132ff` | Second NES OAM buffer |
| `0x00013300`–`0x000150ff` | planar4 descriptor map (64×60 × 2 bytes) |
| `0x00015100`–`0x0001517f` | planar4 4bpp sprite tiles |
| `0x00015200`–`0x000153ff` | RGB444 palette (256 entries) |
| `0x00015400`–`0x000155ff` | RGB555 palette (256 entries) |
| `0x00015600`–`0x000159ff` | Two extended 512-byte OAM buffers |
| `0x00015a00`–`0x000177ff` | packed8 descriptor map (64×60 × 2 bytes) |
| `0x00017800`–`0x00017f7f` | packed16 descriptor map (32×30 × 2 bytes) |
| `0x00018000`–`0x00018aff` | packed sprite tiles and four 16×16 crystals |
| `0x00020000`–`0x0005bfff` | packed8 per-cell gradient tiles (3840 × 64 bytes) |
| `0x0005c000`–`0x00097fff` | packed16 per-cell gradient tiles (960 × 256 bytes) |
| `0x00098000`–`0x000987ff` | hires fine-detail tiles (7 × 256 bytes) |
| `0x00099000`–`0x000991ff` | sprite staging buffer (DMA upload source) |
| `0x000a0000`–`0x0010ffff` | Firmware variables, field table and stack |
| `0x10000000`–`0x1000007f` | VSN native MMIO |
| `0x10000100`–`0x10000101` | UART data and status |

A 1 MiB RAM card backs `0x10000`–`0x10ffff`. The two gradient tile banks use
most of it. NES mode appears about 200 ms after reset. Building the packed banks
takes roughly another second. UART gets its mapping address from `config.base`.

## Interrupts and DMA

VSN's `nmi_signal` line pulses on vblank. The RISC-V core has no NMI input, so
the frame loop acknowledges that cause. Raster compare and DMA completion drive
`irq_signal`. The CPU handles them as machine external interrupts through
`mtvec`, `mie.MEIE`, and `mstatus.MIE`.

`P` enables a raster compare at scanline 184. The IRQ handler scrolls the bottom
56 lines to the ruins at (`256,240`) and hides sprites there. The frame loop
restores the camera and sprites in vblank. The handler clears each cause through
the write-one-to-clear pending register. In hires, the offset wraps the
background because the viewport already covers the whole world.

At boot, DMA fills the OAM buffers. A `0xff` Y byte hides an unused NES sprite.
Each frame, DMA copies the staging buffer at 16 bytes per scanline. Its IRQ sets
a flag that the frame loop waits for before publishing the table.

## Rebuild

Install GNU bare-metal RISC-V GCC/binutils, then run from this directory:

```sh
./build_rom.sh
```

`RISCV_PREFIX` overrides the default `riscv64-unknown-elf-` tool prefix.
Temporary objects go in `scratch/riscv-vsn` unless `SRZ80_SCRATCH_DIR` is set.
The build needs no libc, math library, or external assets. `linker.ld` places
firmware variables above the asset region.

Code and original artwork are MIT licensed under the repository license.
