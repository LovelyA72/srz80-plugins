# JavaScript YM2151 VGM player on YM2414

Put your uncompressed YM2151 `.vgm` file beside `project.json` and name it
`test.vgm`. Open `project.json` in SRZ80 and resume the rack. You can replace
the included file without editing the script.

The script sends YM2151 register writes to the YM2414 card at the times recorded
in your VGM. It skips commands for other chips. If the file has a loop point,
playback repeats from there. Otherwise, it stops at the end. If your file has
two YM2151 chips, you'll hear only the first.

OPM and OPZ handle panning differently. The script converts left, right, and
center settings. The YM2414 card's pan bits always route a channel to at least
one output, so the script uses the operators' total levels when an OPM channel
turns both outputs off. It restores those levels when the channel turns back on.

The project sets the YM2414 clock to 4,000,000 Hz to match the included file.
If your VGM uses a different YM2151 clock, set `chip_clock_hz` in `project.json`
to that value. Unpack `.vgz` files to `.vgm` before using them.

The JavaScript `project.read` call returns raw bytes as a `Uint8Array`.
