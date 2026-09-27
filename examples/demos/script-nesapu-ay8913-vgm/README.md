# NES APU + Sunsoft 5B VGM player

Bring your own `test.vgm` and place it near `project.json`. Open `project.json` in
SRZ80 and resume the rack. The Lua script reads `test.vgm`, sends NES APU (`0xB4`)
and AY8910 family (`0xA0`) register writes to their cards, and schedules
them from VGM's 44,100 Hz sample timeline. NES DPCM (`0x67 0x66 0xC2`) blocks
update the script card's sample memory at their VGM timestamps. The APU reads
that memory through the bus. The VGM loop point repeats automatically.

The recording declares 1,789,773 Hz clocks for both chips and 2,648,857
samples (about 60.06 seconds) before its loop. The Sunsoft 5B runs its YM2149
with SEL low, halving the input clock internally. The AY card stays at the
declared 1,789,773 Hz. Lua doubles each tone period before writing it to the
AY card, so the AY runs within its usual input clock range while matching the
recording's pitch. The recording keeps noise disabled and uses fixed volumes,
so the player rejects noise or envelope use that this translation cannot match.
The NES APU's pulse timers run every second CPU cycle; triangle, noise, and
DPCM timers retain their existing timing.

The player accepts uncompressed VGM files using the supported NES APU and
AY8910 family commands and 1,789,773 Hz chip clocks. DPCM blocks can update
any part of the 16 KiB sample window, including during a loop.

The project uses fixed simulation time for repeatable playback. No CPU or
firmware build is needed. Rebuild the `nesapu` plugin if your installed
distribution predates the pulse timer fix. The script checks the file header,
sample count, command set, and DPCM block bounds before playback starts.
