# RISC-V five-second audio recorder

This five-card project automatically alternates between exactly five seconds of
RECORD and five seconds of PLAY. During RECORD, RV32I firmware reads the live
audio-input MMIO FIFO into an 80,000-byte buffer in guest RAM while the DAC is
held at silence. Missing/startup audio remains `0x80`. At the deadline capture
is disabled and its tail discarded. During PLAY, the CPU refills the DAC's
32-byte FIFO from that RAM buffer. Playback ends on its simulation-time deadline,
then the DAC is cleared to silence and a new recording replaces the old one.

Address map:

| Range | Device |
| --- | --- |
| `0x00000000` | firmware ROM |
| `0x00010000–0x0002FFFF` | 128 KiB RAM (recording plus stack) |
| `0x10000000–0x1000000F` | audio input |
| `0x10000100–0x10000104` | PCM DAC |

Input is host-resampled once, strictly nearest-exact, to 16 kHz. The card selects
host channel 0 and converts it to unsigned 8-bit PCM; no plugin or firmware
resampling is performed. The DAC's independent output conversion remains linear.

Enable global audio and audio input in **Settings > Audio**, choose a recording
device, then run the project at real-time pacing. The red recording badge is on
only in RECORD. If capture is disabled or unavailable, the cycle continues and
the missing portion plays as silence.

Build the included ROM with:

```sh
./build_rom.sh
```

Set `RISCV_PREFIX` for a differently named GNU RV32 toolchain. Intermediate
files go under repository `scratch/`; only `recorder.rom` is published beside
the example. Pause follows simulation time. Reset starts a fresh RECORD phase.
Snapshot load clears live input and leaves it disabled until firmware explicitly
re-arms it; microphone samples are never snapshot data.
