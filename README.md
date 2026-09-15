# SRZ80 Plugins

This repo hosts plugins for the SRZ80.

Want to get your own plugin included? Open a pr!

## Building

On Linux, build Windows plugins with the MinGW-w64 `x86_64-w64-mingw32` GCC/G++
cross compiler:

```sh
cmake --preset mingwcross-debug
cmake --build --preset mingwcross-debug -j 4
```

This writes Windows plugin and tool DLLs to `build/mingwcross-debug/plugins` and
`build/mingwcross-debug/tools`. Use `mingwcross-release` for a stripped release
build. Copy these directories into the matching SRZ80 Windows distribution's
`bin/` directory; that distribution supplies the MinGW runtime DLLs.

## An agent designing a card?

Hello from a human! Below is an instruction for you on how to design and test a
card, written by a non-human:

The non-human has since discovered the public engine ABI, so the GUI is no
longer part of the ritual.

## Testing a card

Card tests use two public surfaces:

- the headless `srz80` loader for discovery, ABI, configuration, and rejection
  paths; and
- the public engine ABI in `sdk/include/srz80/engine.h` for deterministic
  simulation and register behaviour.

Do not use `srz80_gui` for card validation. Do not modify SRZ80, link against
private host libraries, or write a fake host ABI harness.

### 1. Build an isolated distribution

Work inside `scratch/`; it is git-ignored. Copy the compiled SRZ80 distribution
and place the candidate plugin in the executable-relative `plugins/` directory:

```sh
mkdir -p scratch/card-test/plugins
cp -a /path/to/srz80-dist/. scratch/card-test/
cp build/gcc-debug/plugins/<your_plugin>.so scratch/card-test/plugins/
```

Plugin discovery is relative to the loader executable, so run everything from
`scratch/card-test`. The examples use POSIX shell; on Windows, use the
equivalent `Copy-Item`/`srz80.exe` commands.

### 2. Derive the card contract

Read `sdk/include/srz80/abi.h` for the card ABI and
`sdk/include/srz80/engine.h` for the engine ABI. From the card source or its
documentation, determine:

- `SrhPlugin::id` — the project `"plugin"` value, which is not necessarily the
  DLL/SO filename;
- the address space, `base`, and inclusive mapping range
  (`base..base + size - 1`);
- the clock index, image slots, JSON config keys, signal names, reset
  behaviour, and save/load state format.

### 3. Validate discovery and configuration with the CLI

Create minimal project-v2 JSON files beside any required ROM/image fixtures. For
example, a card with 16 bytes of I/O at `0xC0`:

```json
{
  "version": 2,
  "seed": 1,
  "epoch_ns": 0,
  "time_mode": "project",
  "spaces": [
    {
      "name": "cpu0.io",
      "maximum": "0xFF",
      "resolver": "priority",
      "unclaimed": "0xFF"
    }
  ],
  "clocks": [0, 0, 0],
  "cards": [
    {
      "plugin": "my_cool_card",
      "space": "cpu0.io",
      "base": "0xC0",
      "size": 16,
      "priority": 0,
      "config": {}
    }
  ]
}
```

Adapt `space`, `base`, `size`, `clock`, `config`, and `image` to the card's
contract. Then run the headless loader:

```sh
cd scratch/card-test
./srz80 case-valid/project.json
echo $?   # 0 = clean load, nonzero = validation/load failure
```

A valid project must report the loaded-card count including the candidate card
with no ABI, export, dependency, configuration, or mapping error.

Exercise rejection paths independently:

- unknown plugin ID;
- missing required space;
- out-of-range or overlapping `base`/`size`;
- malformed or wrong-type config JSON;
- missing or bad image;
- invalid signal name;
- missing required host capability/extension.

Each must fail cleanly with a nonzero exit code and no crash, hang, or
partial-success state.

### 4. Test runtime behaviour with `engine.h`

The headless loader initializes a project but does not execute the simulation.
For register semantics, clock-driven behaviour, interrupts, and reset
behaviour, build a small CLI harness against the public engine ABI:

```sh
g++ -std=c++20 -I sdk/include \
    -L scratch/card-test -Wl,-rpath,$PWD/scratch/card-test \
    scratch/your_card_test.cpp -lsrz80engine \
    -o scratch/your_card_test
```

The harness can use:

- `srz80_engine_create` / `srz80_engine_destroy`;
- `srz80_engine_load_project` /
  `srz80_engine_load_project_json`;
- `srz80_engine_find_space`, `srz80_engine_cards`, and
  `srz80_engine_clocks`;
- `srz80_engine_read` / `srz80_engine_write` for bus accesses;
- `srz80_engine_resume` followed by
  `srz80_engine_run(engine, ticks, UINT64_MAX)` to advance a known number of
  clock events;
- `srz80_engine_sample_signal`, `srz80_engine_drive_signal`,
  `srz80_engine_subscribe_signal`, and `srz80_engine_release_signal`;
- `srz80_engine_properties` and `srz80_engine_edit_property`;
- `srz80_engine_text_query` for text/UART observables;
- `srz80_engine_save_state` and `srz80_engine_load_state`;
- `srz80_engine_now`, `srz80_engine_stop_reason`, and
  `srz80_engine_run_state`.

Use `srz80_engine_candidate_create` and `srz80_engine_replace` when a load must
not disturb an existing active rack.

For CPU/interrupt/full-system behaviour, add compatible CPU, RAM, and ROM cards,
write a tiny deterministic guest program, run the engine, and read a memory
address, signal, text endpoint, or provider as the oracle.

### 5. What to exercise

For every exposed register or mapped byte, test:

- the first and last mapped addresses;
- one byte immediately below and above the mapping;
- reset defaults, cold reset, and warm reset;
- read/write masks, read-only/write-only behaviour, FIFO/status/acknowledge
  semantics, and side effects;
- clock-driven or scheduled behaviour;
- signal assertion/release behaviour;
- side-effect-free `peek` inspection.

Run each project multiple times with a fixed `seed` and
`"time_mode": "project"`; outputs must match across runs.

### 6. Record failures and limitations

For each test, record the project JSON, build command, test command, expected
observable, actual observable, and complete host log. Report limitations
explicitly: without a guest CPU/RAM/ROM or suitable provider, you may be able to
prove discovery, ABI compatibility, configuration validation, and lifecycle
behaviour, but not exhaustively verify private callbacks or register semantics.

Treat crashes, access violations, host hangs, non-deterministic results, or a
successful load with an incorrect mapping/configuration as failures.
