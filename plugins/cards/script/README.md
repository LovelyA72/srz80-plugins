# Script card

The card runs one project source file in a per-card Lua 5.5.1, QuickJS-NG
0.15.1, or mruby 4.0.0 runtime, or optionally the experimental PHP backend below.
The host picker stores `main_file` relative to the active
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

Select a project-relative `.rb` file for mruby. Define the hooks above as
top-level methods. Ruby uses the `Card` and `Project` modules for the operations
listed above, for example `Card.log("ready")` and `Project.read("settings.txt")`.
Timer and signal callbacks are Ruby procs. The global `$state` starts as an
empty Hash; only JSON-compatible Hashes with String keys can be saved. The
mruby build includes its compiler and core library without file, process, or
network gems.
`require("folder/module.rb")` loads project-relative `.rb` files and caches
them by normalized path, including while a cyclic import is in progress.

```ruby
$state = {"value" => 0}
def on_reset(cold)
  $state["value"] = cold ? 0 : $state["value"]
end
def on_read(address)
  $state["value"]
end
def on_write(address, value)
  $state["value"] = value
end
```

The source budget is 16 MiB total across the main file and all loaded modules;
each project-file operation is limited to 16 MiB by the host extension. Each
callback is limited to approximately 10 million VM instructions, checked at
100-instruction intervals in Lua and QuickJS's 10,000-instruction interrupt
interval. mruby checks every VM instruction against a 10 million instruction
budget per callback. Lua and QuickJS VMs are limited to 64 MiB and a 1 MiB
native stack.

Snapshots save only the script's global `state` value, which must be a JSON
object. Functions, cycles, non-string object keys, and unsupported values make
save fail. Loading validates the full JSON payload before replacing `state`.
VM internals, loaded module caches, subscriptions, and pending timers are not
serialized. Timers are canceled when the card is removed, the main file is
changed, or a new run replaces the VM. Completed one-shot timers are released.

Lua, QuickJS, and mruby versions and archive hashes are pinned in `CMakeLists.txt`.
If the local `vendor/` directory is absent, CMake fetches those releases into
the build tree. mruby builds with Ruby and Rake installed on the build machine.
Their license notices are included with their sources.

## Experimental PHP backend

This optional backend embeds upstream PHP 8.4.25 with PCRE2, without a PHP
executable or server. It is optional and currently requires native 64-bit
Linux/glibc. `SRZ80_SCRIPT_PHP` defaults to `OFF`; ordinary builds neither
download nor compile PHP. The embedding approach was informed by `php-esp32`; its ESP-IDF
platform configuration and device stubs are not used here.

```sh
cmake --preset gcc-debug -DSRZ80_SCRIPT_PHP=ON -DBUILD_TESTING=ON
cmake --build build/gcc-debug --target plugin_script script_php_runtime_test --parallel 4
ctest --test-dir build/gcc-debug -R '^script_php_runtime$' --output-on-failure
```

The first build downloads and verifies the PHP source, then configures and
builds a minimal embed SAPI. GCC, GNU make and the normal PHP configure tools
are required. No system PHP installation is needed. For an offline PHP build,
set `-DSRZ80_PHP_ARCHIVE=/absolute/path/php-8.4.25.tar.gz`; the same SHA256 is
checked for local archives.

Keep this layout when copying the plugin into a host installation:

```text
plugins/libmisc_script.so
plugins/php/libscript_php_bridge.so
plugins/php/libphp.so
plugins/php/PHP-LICENSE
plugins/php/ZEND-LICENSE
plugins/php/PCRE2-LICENSE
```

Select a project-relative `.php` file as `main_file`, or open
[`examples/script-php`](../../../examples/script-php). Source files use the
normal `<?php` opening tag. The same `on_reset`, `on_read` and `on_write` hooks
work, with PHP parameters such as `function on_write(int $address, int $value)`.
`echo` and `card_log()` send output to the host log.

The host bindings accept positional or named PHP arguments:

```text
card_log(message)
card_read(space_name, address) -> byte
card_write(space_name, address, value)
card_time_ns() -> simulated nanoseconds
card_signal_read(name) -> millivolts
card_signal_drive(name, millivolts, strength)
card_on_signal(name, callback) -> callback id
card_after(delay_ns, callback) -> timer id
project_read(relative_path) -> binary string
project_write(relative_path, bytes)
```

The bridge uses explicit function and class **allowlists** in
[`php/php_bridge.c`](php/php_bridge.c). It retains standard math, `preg_*`, and
selected string, array, type and JSON functions, plus the host bindings.
Functions absent from the allowlist are removed before compiling user code;
non-allowlisted built-in classes are disabled. File, process, environment,
network, native-module and INI-changing functions are unavailable, including
indirect calls. `include`, `require`, `eval` and `exit` are also unavailable in
this first version. File data goes exclusively through the project helper.

PHP engines use isolated linker namespaces (`dlmopen`) in a bounded pool of
8 slots. Each script gets a fresh PHP request with independent globals,
functions and classes. Reload candidates coexist with the previous request;
failed reloads keep the previous script. Idle engines are reused, avoiding
static TLS exhaustion from repeatedly loading and unloading libc. Engine
libraries remain loaded for the pool's lifetime; request memory and callbacks
are released between uses. An engine whose cleanup fails is quarantined.

Leave a slot available for reloads: at most 7 simultaneous PHP cards can reload
with this pool. Other loaded libraries can lower the available glibc TLS
capacity; allocation failures are reported without replacing the old script.
Calls to each card must be serialized by the host, as with the other script
backends. Sequential thread changes and synchronous cross-card callbacks are
supported. This backend currently requires native 64-bit Linux/glibc.

Install the plugin and its adjacent `php/` directory together. The bridge ABI
is checked before starting PHP; mismatched versions report a clear load error.
Restart the host after replacing these libraries, including when upgrading
from the original implementation that exhausted TLS after repeated reloads.

Timer callbacks receive simulated time in nanoseconds; signal callbacks receive
`(name, millivolts)`. Both callbacks run synchronously on the calling host thread.

Each outer callback has a 10-million-opcode budget shared with reentrant calls.
PHP request allocations have a 64 MiB limit and Zend's native stack guard is
set to 1 MiB. PCRE JIT is disabled; matching has a 100,000-work limit, depth
limit of 1,000, and 1 MiB match-heap limit. These are not a process-wide memory
cap or a wall-clock deadline for every native builtin. A fatal error retires
that VM until reset/reload; ordinary exceptions are reported to the host.
Recurring work should schedule its next callback with `card_after()` and return
so that simulation time and other cards can advance. Timers may recur forever;
the opcode budget resets for each outer callback.

Snapshots use global `$state`, initially an empty `stdClass`. Use `stdClass`
objects for maps and ordinary sequential PHP arrays for lists:

```php
$state = (object) ['value' => 0, 'samples' => [], 'settings' => (object) []];
function on_read(int $address): int {
    global $state;
    return $state->value;
}
```

Associative PHP arrays, custom objects, closures, cycles and non-JSON values
are rejected on save; this avoids silently changing PHP value types during
JSON round trips. Snapshot size is limited to 1 MiB and nesting to 64 levels.
Timers and subscriptions are not serialized, matching the existing backends.

This is not a fully stripped PHP distribution
or an audited sandbox. Optional extensions and JIT are disabled at build time,
but unused file/network implementations inside PHP's mandatory standard/core
code are still present in `libphp.so`; scripts cannot access their normal
entry points. Removing those implementations is a separate build-trimming step.
