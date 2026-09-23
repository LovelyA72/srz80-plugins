# Script card

The card runs one project source file in a per-card Lua 5.5.1 or QuickJS-NG
0.15.1 runtime. The host picker stores `main_file` relative to the active
project when the file is inside it. The card also accepts an absolute path
inside that project, resolves it to a project-relative name and loads it through
`host.project_files.v1`. Includes and the `project.read` / `project.write` APIs
also use that extension. Scripts do not receive operating-system file, process,
or native-module APIs.

When the host enters Running after a cold reset, the card loads a fresh runtime,
discarding the previous run's timers and subscriptions. On resume from Paused,
it checks its main file, loaded modules, and files previously read through
`project.read`, and reloads if any changed. Reload calls `on_reset(true)` before
simulation time advances. A failed reload pauses the simulation and leaves the
prior runtime available; the next Start retries even if the edited file has
been restored to its earlier contents. Warm reset also rebuilds the runtime,
then calls `on_reset(false)`. Start notifications require `host.lifecycle.v1`.

The script card maps the configured memory range (`base`, `size`). Scripts may
define `on_reset(cold)`, `on_read(address)`, and `on_write(address, value)`.
`on_read` returns a byte from 0 through 255. `peek` is intentionally
unavailable; it never executes script code. Scripts can also use:

```text
card.log(message)
card.read(space_name, address) -> byte
card.write(space_name, address, byte)
card.time_ns() -> simulated nanoseconds
card.signal_read(name) -> millivolts
card.signal_drive(name, millivolts, strength)
card.on_signal(name, callback(name, millivolts))
card.after(delay_ns, callback(simulated_time_ns)) -> timer id
project.read(relative_path) -> bytes-as-string
project.write(relative_path, bytes-as-string)
```

Lua loads the base, coroutine, math, string, table, and UTF-8 libraries. Its
`require("folder/module.lua")` loader accepts only project-relative `.lua`
modules and caches them by normalized path. JavaScript uses `import` and
`import ... from "./module.js"`; the QuickJS module loader accepts only
project-relative `.js` modules. Both loaders resolve relative to the importing
file and support cyclic imports.

The source budget is 16 MiB total across the main file and all loaded modules;
each project-file operation is limited to 16 MiB by the host extension. Each
callback is limited to approximately 10 million VM instructions, checked at
100-instruction intervals in Lua and QuickJS's 10,000-instruction interrupt
interval. Each VM is limited to 64 MiB and a 1 MiB native stack.

Snapshots save only the script's global `state` value, which must be a JSON
object. Functions, cycles, non-string object keys, and unsupported values make
save fail. Loading validates the full JSON payload before replacing `state`.
VM internals, loaded module caches, subscriptions, and pending timers are not
serialized. Timers are canceled when the card is removed, the main file is
changed, or a new run replaces the VM. Completed one-shot timers are released.

The vendored runtime sources are pinned in `vendor/UPSTREAM.md`; their license
notices are included beside the sources.
