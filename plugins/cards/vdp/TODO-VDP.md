# VDP remaining work

The card renders V9938 modes and V9958 YJK/YAE modes. It handles commands, interrupts, scanout and save state. The remaining gaps are below.

## Raster timing

Measure frame time and drift with the scheduled scanline event. Compare its nanosecond rounding with the engine time base. Check behavior under `SRZ80_TIME_SYSTEM` and document the result in the README. A subscribed 21.5 MHz master clock is an option if the scheduled path drifts, though it would require about 21.5 million callbacks per simulated second.

## Video readback

Measure the cost of copying the 1,362,176-byte surface from the published buffer to the caller, then uploading it with `SDL_UpdateTexture`. The UI reads it at 60 Hz while the video panel is visible. The fixed PAL surface includes 102 unused rows in NTSC mode. Decide whether the UI should crop using the existing `pal` property.

## Mouse and lightpen

The core exposes `colorbus_x_input`, `colorbus_y_input` and `colorbus_button_input`. `status_r()` reads them in mouse mode, but the card doesn't register an input source. Wire the inputs through `host.input.v1` or a VDP data provider. Call the setters on the simulation thread.

## IO space discovery

The card looks up `io_space` from its config, which defaults to `cpu0.io`. A project with another IO space name must set the same name in the VDP config. Document a shared convention if another CPU card needs this.

## VRAM image slots

The descriptor declares no `image_slots`, and `save_project_data` writes an empty chunk. Add a VRAM image slot if projects need to seed VRAM before the guest runs.
