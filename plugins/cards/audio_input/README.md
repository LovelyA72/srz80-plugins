# Audio input card

`audio_input` exposes live host capture as a mono unsigned 8-bit PCM FIFO. The
host supplies already-gained, clamped float samples and performs the only rate
conversion, using its nearest-exact converter. The card selects one channel and
uses `clamp(floor((sample + 1) * 128), 0, 255)`; silence is `0x80`.

The card occupies 16 consecutive memory or I/O addresses. Capture starts
disabled and is controlled only by guest writes.

| Offset | Name | Meaning |
| --- | --- | --- |
| `+0` | DATA | Consuming read pops one byte; empty reads return silence and set underflow. Peek is side-effect free. |
| `+1` | CONTROL | Read bit 0 = enabled. Write bit 0 to enable, bit 1 to clear FIFO, bit 2 to commit/re-arm the staged rate. Writing zero disables capture and clears FIFO. |
| `+2` | STATUS | bit 0 enabled, bit 1 data available, bit 2 full, bit 5 selected channel absent, bit 6 underflow, bit 7 overflow. Write ones to bits 5–7 to clear sticky flags. |
| `+3..+6` | RATE | Staged little-endian 32-bit rate. Commit with CONTROL bit 2; valid range is 8,000–384,000 Hz. |
| `+7..+8` | COUNT | Little-endian queued byte count (0–4096). |
| `+9` | CHANNEL | Selected zero-based host channel (0–7). A write clears queued data. |
| `+10..+13` | TIME_MS | Low 32 bits of simulation time in milliseconds, little endian. |
| `+14..+15` | reserved | Reads return zero. |

Normal STATUS and COUNT-low reads perform one bounded nonblocking host drain;
DATA reads consume only the local FIFO. A
full local FIFO drops newest samples and sets overflow. Missing devices,
disabled host input, and startup latency simply produce no bytes. Reset,
disable, destruction, and snapshot load stop the subscription and discard live
samples. Snapshots retain rate/channel configuration but intentionally never
retain microphone data or resume capture; guest firmware must re-arm it.

Configuration accepts `sample_rate` (default 16000) and `channel` (default 0).
An older host without `host.audio_input.v1` reports the card unavailable.
