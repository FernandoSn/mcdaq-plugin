# MCDAQ — Open Ephys source plugin

**Version:** `0.9.0-rc.1` (Stage 2 release candidate; **not** a validated stable public binary)  
**Target:** Open Ephys GUI **v1.1.0**, Plugin API **10**, Windows **x64**  
**Hardware tested:** Measurement Computing **USB-204** and **USB-1608FS-Plus** with MCC **Universal Library Rev 33**.

MCDAQ adds MCC USB analog input streaming and software-polled digital inputs to the Open Ephys GUI. The work modernizes a 2019 MCDAQ plugin. The implementation includes hardware-board selection by unique ID, multiple MCDAQ instances, multichannel acquisition, native ADC point handling, and a separate digital-input polling/clock-modeling path.

## Installation for users

1. Install **Open Ephys GUI v1.1.0 x64** and the correct **Measurement Computing Universal Library** with device drivers and InstaCal. Verify both boards in InstaCal.
2. Obtain a Release **x64** `mcdaq-plugin.dll` built against GUI v1.1.0 / Plugin API 10; copy it into the **plugin directory actually used by your installation** (normally `C:\ProgramData\Open Ephys\plugins-api10\`). Do not install another copy elsewhere at the same time.
3. Ensure the matching **`cbw64.dll` from the installed UL distribution** is discoverable by the GUI; one common shared runtime directory is `C:\ProgramData\Open Ephys\shared-api10\`. Prefer the same vendor UL version and verify loading rather than replacing system DLLs indiscriminately.
4. Restart Open Ephys, add **MCDAQ** under Sources, and select the physical board. Use two instances for two boards. Attach Record Nodes to capture data.
5. Test acquisition with a short unimportant recording before experiments. Inspect the activity log for `Possible data loss`, scan errors, and final produced/consumed/delivered counts.

**This source archive does not contain** the Windows plugin DLL, MCC header/import libraries, `cbw64.dll`, UL installer, or USB drivers. There is **no prebuilt installer** in this release candidate. The user's existing tested DLL may still be the version from before metadata cleanup; rebuilding is required for the new version string.

## Features / limits

| Feature | Implemented |
|---|---|
| Supported boards | USB-204 (multiplexed ADC); USB-1608FS-Plus (simultaneous ADC) |
| Analog inputs | Select AI0–AI7 independently, at least one enabled |
| User-requested rates | 500, 1,000, 2,000, 5,000, 10,000, 25,000, 30,000 Hz **per channel** (subject to board/channel limits) |
| Voltage range | USB-204 fixed +/-10 V; USB-1608FS-Plus configurable per shared board range |
| Digital inputs | Select DI0–DI7; software-polled (not ADC-clock-synchronous) |
| Two-board operation | Two MCDAQ plugin instances, each reserving one distinct physical board |
| Configuration | Saves/restores unique hardware IDs and channel/rate/range settings |
| GUI | Channel, board, rate, and range controls locked during acquisition/recording |

### Critical timing details

- **Analog streams:** The DLL uses MCC background continuous scans, preserves integer ADC samples, and reports nominal sample rates derived from validated pacer-divider behavior. The nominal clock model assumes the tested UL/firmware behavior and does **not** calibrate the physical oscillator against an external time standard. USB-204 and USB-1608FS-Plus have different pacer logic.
- **Digital inputs:** `cbDIn` is **software-polled**, typically many milliseconds between completed reads. Short pulses can be missed, and reported TTL edges are approximate. END timestamps and midpoint edge interpolation are used; a fixed **1 ms whole-ADC-block holdback** is applied when DI is enabled to reduce late event placement. **Not suitable as a hardware timestamping or tight closed-loop substitute.**
- **Two boards are not synchronized:** Two MCDAQ instances can record concurrently but each uses its own ADC clock. No cross-board phase lock, shared time zero, or drift compensation is guaranteed. Use and validate an external common sync signal before drawing millisecond-scale cross-board conclusions.
- **Error handling:** Acquisition logs include explicit loss/error counters. A failure of hardware stop/release may require a restart of Open Ephys or USB reinitialization; test unplug/error paths separately before public use.
- **Annotations:** Built-in Open Ephys Message Center already provides timestamped text notes when events are recorded; MCDAQ does not implement another notes subsystem.

## Building on Windows (developers)

1. Obtain and **build Open Ephys GUI v1.1.0 x64** from the `plugin-GUI` repository using Visual Studio/CMake.
2. Install MCC UL Rev 33 SDK and locate `cbw.h` and the **x64 import library** supplied with that installation (commonly `cbw64.lib`; use the real path/filename from your SDK).
3. Configure this repository with CMake and the same Visual Studio generator/architecture as the GUI. For example, from a PowerShell session in the MCDAQ repository folder:

```powershell
cmake -S . -B Build -G "Visual Studio 17 2022" -A x64 `
  -DGUI_BASE_DIR="C:/path/to/plugin-GUI" `
  -DUL_INCLUDE_DIR="C:/path/to/MCC/include" `
  -DUL_LIBRARY="C:/path/to/MCC/lib/x64/cbw64.lib"
cmake --build Build --config Release
```

Use your actual Visual Studio generator if different. CMake's `INSTALL` target copies the DLL into the GUI build runtime's `plugins` folder (rather than into ProgramData), following the template. `cbw64.dll` must still be present at runtime. If you already have a working local Open Ephys CMake project, you may instead **replace only its 13 source files** with those under `Source/`, keeping your proven local CMake/SDK wiring. This is the lowest-risk way to rebuild this candidate.

This CMakeLists was adapted from the [official data-thread template](https://github.com/open-ephys-plugins/data-thread-template). Its full Windows hardware build has **not been run in this packaging environment**, so check its linker/SDK path settings against your working setup.

## Tests

On a machine with Python 3 and `g++` or `clang++` (including MSYS2/MinGW on Windows):

```sh
python tests/run_regression_tests.py
```

This runs MCC device-manager mock tests, clock estimator and timeline tests, analog-holdback integration tests, and static editor regression checks. No MCC board or Open Ephys GUI is required. Passing these **does not prove** successful Windows GUI compilation or hardware behavior.

See `docs/RELEASE_CHECKLIST.md` for the final Windows/hardware acceptance list and `docs/SOURCE_FREEZE_SHA256.json` for exact baseline file digests.

## License and attribution

The supplied source incorporates Open Ephys Data Thread template code with preserved GNU General Public License v3-or-later notices. See `LICENSE` (GPLv3 text) and `NOTICE.md`. MCC Universal Library, MCC drivers, and Open Ephys are separate dependencies. **No vendor MCC binary/header/library is redistributed here.** Check the relevant SDK/redistribution terms before distributing binaries that bundle vendor components.

## Release status

`0.9.0-rc.1` denotes a **source release candidate**. Final Windows compilation, error-path robustness checks, and a hardware smoke test on both boards are required before promoting to `1.0.0`.
