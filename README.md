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