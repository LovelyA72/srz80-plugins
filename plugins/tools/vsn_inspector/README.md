# VSN Inspector

Open **Tools > Video > VSN Inspector**. One `video_vsn_inspector` DLL or SO
owns three windows. Each can be docked, resized or closed independently.
Use the Windows menu in any viewer to reopen another. Closing all three
closes the tool.

Auto refresh shares an adjustable interval across the three viewers, from
50 ms to 60 seconds. The default is 500 ms. Drag the Refresh interval field
or Ctrl-click it to enter a value. Manual Refresh works independently.

Tiles shows 64 patterns per page from BG0, BG1 or sprite memory. Select a tile
with the mouse or the Tile field. The palette bank applies to 2bpp and 4bpp
patterns. Extended sprite tile geometry follows the selected sprite record.
NES sprite patterns in this atlas are individual 8x8 tiles from the selected
pattern table. The sprite viewer handles the separate 8x16 pairing rules.

Sprites shows the sprite plane, a selectable record table, raw record bytes
and a magnified selected sprite. Filters hide disabled or offscreen records
from the table. The plane keeps table priority and transparency. It displays
sprites independently of backgrounds, master enable, left clipping and the
NES eight-sprite scanline limit.
Clicking the plane selects the first visible sprite pixel at that position,
or the first enabled sprite whose bounds contain it when the pixels are
transparent. Selection outlines stay inside the plane. Offscreen records
remain selectable in the table.

Layers shows BG0 or BG1 with its palette and opacity. Viewport follows the
layer's scrolling. Map page browses the unscrolled map in screen-sized pages.
Select a cell to inspect its descriptor and open its pattern in Tiles.
Disabled layers can still be inspected. NES exposes BG1 only.

The viewers support all six VSN modes, RGB444, RGB555 and the VSN NES palette,
including grayscale and regional emphasis. Checkerboards represent transparent
pixels. Inspection is read-only.

Install the matching updated VSN card alongside the tool. Its optional
`srz80.vsn.inspector.v1` provider supplies the current register file, graphics
space, I/O space, mapping address and PAL flag. Hosts without provider support
can still load the card. No inspector provider commands are accepted.

The tool requires the host's optional asynchronous memory-read services. Update
SRZ80 alongside the inspector. Older tools keep working with the extended ABI.

Captures use side-effect-free bus peeks on the simulation worker. Each request
contains at most 64 ranges and 4096 bytes. The inspector captures registers,
palette, sprite records and visible map cells, then the referenced patterns.
It checks the storage layout again before publishing. Project or view changes
discard obsolete captures. There are no synchronous memory reads during draw.

The previous sample stays visible while a capture is pending. Decoded previews
are cached. Rebuilding stops after 64 rows or a one-millisecond budget per image
per frame, checked after each row. Auto refresh waits for capture and decoding
to finish before starting the next cycle. Unreadable bytes are reported and
shown as zero. Graphics requests stay within the selected space's maximum
address. Out-of-range graphics data and overlapping VSN MMIO are unavailable
without rejecting the rest of the capture.

Live captures span several worker requests and aren't atomic emulated frames.
They don't reproduce the renderer's per-frame palette and OAM latches. Stop the
rack for stable inspection. Large maps are paged to bound the displayed area.
