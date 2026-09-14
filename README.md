# SRZ80 Plugins

This repo hosts plugins for the SRZ80.

Want to get your own plugin included? Open a pr!

## An agent designing a card?
Hello from a human! Below is an instruction for you on how to design and test a card, written by a non-human:
```
Test the card strictly as a black-box integration plugin. You may use only the compiled SRZ80 distribution (including its executable-relative plugins/ directory and runtime DLLs), the supplied SDK headers, and the card’s compiled shared library/source. Do not build or modify SRZ80, link against private host libraries, or write a fake host ABI harness.
1. Read sdk/include/srz80/abi.h and derive the card’s required configuration: plugin ID, address space, inclusive mapping range (base through base + size - 1), clock, image slots, JSON config, signals, and expected reset behavior.
2. Work in an isolated temporary copy of the compiled SRZ80 distribution. Place the candidate card DLL/SO in the executable-relative plugins/ directory. Keep the host’s runtime dependencies beside the executable; plugin discovery is relative to the executable, not the working directory.
3. Create minimal project-v2 JSON files beside any required ROM/image fixtures. Launch the headless executable as srz80 <project.json>. Capture exit code and stdout/stderr for every case.
4. First verify discovery and loading with a valid minimal project. Confirm the reported loaded-card count includes the candidate card and that the host does not report an ABI, export, dependency, or configuration error.
5. Exercise rejection paths independently: unknown plugin ID, missing required space, invalid/out-of-range base or size, overlapping mappings, malformed or wrong-type config JSON, missing/bad image, invalid signal name, and missing required optional host capability. Each must fail cleanly—no crash, hang, or partial successful load.
6. Where the installed distribution includes CPU/RAM/ROM cards, build a tiny deterministic guest program that reads and writes every exposed register or mapped byte. Test:
   - first and last mapped addresses;
   - one byte immediately below and above the mapping;
   - reset defaults and cold versus hot reset;
   - read/write masks, FIFO/status/acknowledge semantics, and read-only/write-only behavior;
   - clock-driven or scheduled behavior;
   - signal assertion/release and image/config variations.
     Use a UART, video surface, audio output, log, or other observable host behavior as the oracle. Run each project multiple times with a fixed seed and time_mode: "project"; outputs must match.
7. Use the GUI executable only for observables the headless loader cannot expose: device properties, side-effect-free memory inspection, bus monitor, video/audio, manual reset, parking/replugging, and Save/Load State. Verify that inspection/peek does not consume data or alter device state.
8. Record each test’s project JSON, command, expected observable result, actual result, and host log. Report limitations explicitly: without a test harness or a suitable guest CPU/RAM card, you can prove discovery, ABI compatibility, configuration validation, and startup/lifecycle behavior—but not directly invoke or exhaustively verify private callbacks, persistence buffers, or register semantics.
Treat crashes, access violations, host hangs, non-deterministic results, or a successful load with an incorrect mapping/configuration as failures.
```

### But how?
Good question, read on.

Exact Windows example, assuming the SRZ80 distribution contains `srz80.exe`, its runtime DLLs, and stock plugins:

1. Make an isolated test folder:

```powershell
New-Item .\scratch -ItemType Directory -Force
Copy-Item .\srz80-dist .\scratch\card-test -Recurse
Copy-Item .\my_cool_card.dll .\scratch\card-test\plugins\
New-Item .\scratch\card-test\case-valid -ItemType Directory
```

Run these commands from the repository root, with `srz80-dist` and
`my_cool_card.dll` there. The copied distribution becomes `scratch/card-test`,
which is ignored by Git.

2. Find the card’s plugin ID and required settings from its source or documentation. The project’s `"plugin"` value must equal the card’s `SrhPlugin::id`, not necessarily its DLL filename.

3. Create `scratch\card-test\case-valid\project.json`. This minimal example tests a card named `my_cool_card` that maps 16 bytes of I/O at `0xC0`:

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

Adapt `space`, `base`, `size`, `clock`, `config`, and `image` to the card’s ABI contract. If the card needs an image, add `"image": "firmware.bin"` and put that file next to `project.json`.

4. Run the headless executable from its own directory:

```powershell
Set-Location .\scratch\card-test
.\srz80.exe .\case-valid\project.json
$LASTEXITCODE
```

Expected result: exit code `0`, output reporting one loaded card, and no plugin/ABI/configuration errors.

5. Run a deliberate invalid-config test. Copy the project and change the base to an invalid value—for an 8-bit I/O space, `0x100` is out of range:

```powershell
Copy-Item .\case-valid\project.json .\case-invalid-base.json
(Get-Content .\case-invalid-base.json -Raw).Replace('"0xC0"', '"0x100"') |
  Set-Content .\case-invalid-base.json -NoNewline

.\srz80.exe .\case-invalid-base.json
$LASTEXITCODE
```

Expected result: a clean validation/load failure, nonzero exit code, and no crash.

6. To test register behavior, the project must also include a compatible CPU, RAM, and ROM card from the compiled distribution. Put a tiny ROM program in the project folder that writes known values to the card’s mapped I/O addresses and reports results through an existing observable device such as `uart_console`. Run it identically:

```powershell
.\srz80.exe .\case-registers\project.json
```

7. For GUI-only tests, launch the GUI from the distribution directory:

```powershell
.\srz80_gui.exe .\case-valid\project.json
```

Use its Log panel to confirm loading, Bus Monitor to verify accesses, memory inspection to test `peek`, and reset/state controls to verify lifecycle behavior.
