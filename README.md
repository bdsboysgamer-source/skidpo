# Fishing Bot

A native Windows x64 C++20 application that automates the fishing minigame
via real-time screen capture, classical computer-vision detection, temporal
tracking, and synthetic input. Built from scratch around DXGI Desktop
Duplication + Direct3D 11 (not GDI BitBlt, not periodic screenshots).

## Build

Requirements: Visual Studio 2026 (MSVC 19.51+), CMake 3.24+, Windows SDK
10.0.26100.0+. Tested against a bundled CMake at:
`C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
(add that `bin` folder to `PATH`, or call `cmake.exe` by full path, if
`cmake` isn't already recognized in your shell).

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Output: `build\Release\fishingbot.exe`

To rebuild clean:

```bash
rmdir /s /q build
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

This was actually built and verified on this machine: MSVC 19.51.36257.0,
Windows SDK 10.0.26100.0, zero warnings, and a launch/shutdown smoke test
(window creation, console logging, DXGI capture init against the live
1920x1080 display, graceful exit with no leftover process).

## Running

Run `fishingbot.exe` (double-click, or from a shell). A console window
(logging) and a status/debug GUI window both appear.

Hotkeys (global, via `RegisterHotKey` - work regardless of focus):

- **F1** - toggle the bot on/off
- **F2** - exit
- **F3** - toggle the debug schematic panel

`config/config.ini` is created next to the executable on first run (from
built-in defaults) and reloaded on every launch; edit it to tune the ROI,
timings, detection thresholds, tracking, and controller behavior without
recompiling.

## Behavior

State machine: `OFF -> CASTING -> FISHING -> WAIT_T -> RECAST_WAIT -> CASTING -> ...`

- **OFF -> CASTING**: on enable, a single left-click pulse, then wait
  `castDelayMs` (default 7000ms).
- **FISHING**: the controller continuously holds/releases the left mouse
  button to keep the tracked marker inside the tracked target band. The
  5-second no-object completion timer (`noObjectTimeoutMs`) is driven
  **only** by raw per-frame detections of marker+target both being absent
  - the temporal tracker's coasted/predicted state can never satisfy or
  reset this timer by itself.
- **WAIT_T**: mouse released, `T` held for `tHoldMs` (default 3000ms).
- **RECAST_WAIT**: wait `recastWaitMs` (default 2000ms), then one click
  pulse, back to `CASTING`.

Disabling the bot (F1) or exiting (F2) at any point immediately releases
both the mouse button and `T`.

## Architecture

```
capture thread                 detection/control thread            UI thread
DXGI Desktop Duplication  -->   newest-frame mailbox  -->    Detector (raw, stateless)
(GPU copy of ROI+flank          (mutex+condvar, drops              |
 only, CPU staging texture)      duplicate/stale frames)            v
                                                              Tracker (alpha-beta,
                                                               jump rejection, coasting)
                                                                     |
                                                                     v
                                                              FishingController
                                                              (deadband/hysteresis/
                                                               min-toggle/confidence gate)
                                                                     |
                                                                     v
                                                              InputManager (SendInput,
                                                               idempotent state,
                                                               emergency release)
```

- `src/common/Types.h` - shared coordinate-space types and conversions
  (`screenX/Y`, `captureLocalX/Y`, `roiLocalX/Y`, `debugX/Y`), documented
  and centralized so no module re-derives a transform ad hoc.
- `src/capture/` - DXGI Desktop Duplication: selects the output containing
  the ROI, copies only the `roi.width + 2*flank` x `roi.height` sub-rect
  via `CopySubresourceRegion` into a CPU-readable staging texture, detects
  frames with no new desktop content and skips them.
- `src/detection/` - stateless per-frame detector. `ColorModel` scores
  pixels against the three prototype colors (hue-weighted, tolerant of the
  overlay's semi-transparent blending) plus a local-background deviation
  term sampled from the left/right flank columns. `CandidateScoring` turns
  the resulting score grid into a binary+gap-bridged mask and finds
  connected components spanning multiple bar columns (not a single-pixel
  line), scored by height, column coverage, fill ratio, and edge contrast.
  `Detector` runs this independently for target (color+background-relative)
  and marker (brightness/neutrality vs. local background), so the marker
  stays detectable even overlapping the target.
- `src/tracking/Tracker.h` - alpha-beta filter producing smoothed/predicted
  bands for **control only**; documented and enforced by convention never
  to be the source of truth for "is it really gone" (that's raw detection,
  read directly by the app state machine).
- `src/controller/FishingController.h` - deadband + hysteresis + minimum
  toggle interval + confidence gating over the tracked bands.
- `src/input/InputManager.h` - `SendInput` wrapper; only emits an actual
  down/up transition when the logical state changes; `EmergencyReleaseAll()`
  is safe from any thread/any number of times and is called on disable,
  exit, and both C++ and structured (SEH) exceptions in the detection
  thread.
- `src/ui/StatusWindow.h` - the visible GUI: status readout plus a debug
  schematic panel, drawn inside our own window (never overlapping the
  captured screen pixels, so there is no feedback loop into the detector).
  All debug drawing goes through `RoiLocalYToDebugY()`/`RoiLocalXToDebugX()`
  - never a raw roiLocal value used as a debug pixel coordinate directly.
- `src/app/App.h` - owns the threads, the bot state machine, and the
  thread-safe `UiSnapshot` the UI polls on a timer.

## Configuration reference (`config/config.ini`)

| Section | Key | Default | Meaning |
|---|---|---|---|
| roi | screenX/screenY/width/height | 1415,319,49,384 | fishing bar rect, screen space |
| roi | flankPixels | 16 | background sample margin each side |
| timing | castDelayMs | 7000 | CASTING -> FISHING wait |
| timing | noObjectTimeoutMs | 5000 | raw both-absent -> completion |
| timing | tHoldMs | 3000 | T hold duration |
| timing | recastWaitMs | 2000 | wait before recast click |
| detection | colorTolerance | 0.55 | prototype-color match looseness |
| detection | target/markerMin* | see file | geometric acceptance thresholds |
| tracking | maxJumpPxPerSec | 900 | implausible-jump rejection |
| tracking | missTimeoutMs | 700 | coast duration before "stale" |
| controller | deadbandPx | 10 | no-toggle band around target center |
| controller | minToggleIntervalMs | 90 | anti-chatter floor |
| controller | holdMovesMarkerUp | true | flip if your game's marker falls when held |

## Known limitations / next steps

- Input targets whatever window currently has focus (`SendInput`), per the
  v1 scope in the brief; `InputManager` is structured so a future version
  could target a specific `HWND`/process without touching callers.
- The color-prototype/background-deviation model was built from the three
  sample colors given in the brief and validated only by static review and
  a startup smoke test in this environment (no live game window was
  available to test against here) - the detector's thresholds in
  `config.ini` are the first thing to tune against a real capture if
  target/marker confidence looks off in the debug panel.
- No live calibration UI (section 19 of the brief marks this optional for
  v1); the prototypes and tolerance are edit-config-and-relaunch only.
- Monitor selection picks the DXGI output containing the ROI's top-left
  point at startup; changing which monitor the game is on requires a
  restart (or edit `roi.screenX/screenY` and restart).
