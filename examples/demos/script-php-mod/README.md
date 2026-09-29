# PHP ProTracker player

Requires the optional Linux/glibc PHP backend. Build instructions and limitations:
[script card](../../plugins/cards/script/README.md#experimental-php-backend).

PHP building is disabled by default. If you downloaded
your plugin pack from GitHub, then your copy does not have
PHP enabled. It's also Linux only.

Port of the supplied four-channel Lua MOD player, using the experimental PHP
script backend and four PCM DAC cards. No CPU or ROM is required.


Headers and pattern data must be complete. Sample payloads that extend past
end-of-file are shortened to the available bytes for playback, and a warning
reports the missing byte count. Loop bounds are then clamped to that effective
length; samples wholly beyond EOF are silent. This is a deliberate relaxation
of the supplied Lua player's strict sample-size check, following the
[libxmp sample loader's EOF handling](https://github.com/libxmp/libxmp/blob/master/src/loaders/sample.c).
It changes only the decoded playback state, never the module on disk.

The accepted signatures are `M.K.`, `M!K!`, `4CHN`, and `FLT4`, with 31 sample
headers. Parsing, finetuning, sample looping, effect handling and song restart
follow the supplied Lua implementation. This includes arpeggio, pitch/volume
slides, portamento, vibrato, tremolo, sample offset, jumps/breaks, speed/tempo,
and the implemented extended effects (fine slides, waveform selection,
glissando, pattern loop/delay, retrigger, note cut and note delay). Unsupported
effects retain the original player's behavior; this is not a new tracker engine.

Audio is rendered at 22,050 Hz into four 32-byte DAC FIFOs. The player primes
32 frames and then schedules 16-frame refills with `card_after()` using an
accumulated simulated-time deadline, matching the Lua player. Each callback
returns to the host; playback can repeat indefinitely.

`mod.io:0x10`, `0x20`, `0x30`, and `0x40` are the DAC bases, each occupying five
registers. The script reserves `mod.io:0`. It is listed last so its reset runs
after the DAC resets. The DAC cards duplicate mono into both stereo outputs,
so this example does not add classic Amiga channel panning. The original
quarter-scale per-voice output is preserved to leave room when mixing four voices.

As in the supplied script, live tracker/sample positions are runtime data,
not serialized into the script card's JSON `$state` snapshot. Reset/reload
starts the module from the beginning.
