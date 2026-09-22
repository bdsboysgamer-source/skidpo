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

State machine: `OFF -> CASTING -> FISHING -> WAIT_T -> CASTING -> ...`

- **OFF -> CASTING**: on enable, a single left-click pulse.
- **CASTING -> FISHING**: detection-driven, not a fixed wait - moves to
  FISHING as soon as raw marker and target have both been continuously
  present for `castingDetectDebounceMs` (default 150ms, just filters a
  single noisy frame). `castMaxWaitMs` (default 7000ms) is a safety
  ceiling only, in case the cast animation or detection never lands, so
  this state can't get stuck forever.
- **FISHING**: the controller continuously holds/releases the left mouse
  button to keep the tracked marker inside the tracked target band. The
  no-object completion timer (`noObjectTimeoutMs`, default 2000ms) is
  driven **only** by raw per-frame detections of marker+target both being
  absent - the temporal tracker's coasted/predicted state can never
  satisfy or reset this timer by itself.
- **WAIT_T**: mouse released, `T` held for `tHoldMs` (default 3000ms),
  then one click pulse, straight back to `CASTING`.

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
- `src/detection/` - stateless per-frame detector, using two different
  strategies for the two elements:
  - **Target**: border-based, not fill-based. The target zone's left
    border renders as a solid (non-alpha-blended) run of ~24 vertically
    consecutive, near-identical pixels at a known fixed column (the GUI
    doesn't move), unlike the semi-transparent fill (which testing showed
    is unreliable to classify - its unlit tint can resemble the lit tint
    depending on background). `Detector::Detect` scans that column (plus
    a couple of neighbors for redundancy) for a run of the expected
    length, and projects its color onto the axis between the game's own
    two known in-zone/out-of-zone colors - which also gives a direct 0..1
    "is the marker in the zone right now" reading
    (`DetectionResult::targetInZoneColorFraction`), independent of
    comparing marker vs. target position.
  - **Marker**: `ColorModel::MarkerLikelihood01` scores brightness/
    neutrality against the local flank background; `CandidateScoring`
    turns that into a binary+gap-bridged mask and finds connected
    components spanning multiple bar columns (not a single-pixel line),
    scored by height, column coverage, fill ratio, and edge contrast. This
    stays independent of the target's own classification, so the marker
    remains detectable even while overlapping the target.
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
- `src/ui/StatusWindow.h` - the main control window: text status readout
  (state, timers, raw/tracked values, fps, input state) and the F1/F2/F3
  global hotkeys.
- `src/ui/DebugOverlay.h` - a borderless, click-through, always-on-top
  window drawn directly over the game near (and, via
  `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`, ON TOP of) the real
  fishing bar - visible to the human eye but excluded from every
  screen-capture API including our own Desktop Duplication, so there is no
  feedback loop into the detector even though it draws over the live ROI.
  Falls back to drawing nothing inside the ROI if that API ever isn't
  available. All debug drawing goes through
  `RoiLocalYToDebugY()`/`RoiLocalXToDebugX()`/`RoiLocalYToScreenY()` -
  never a raw roiLocal value used as a pixel coordinate directly.
- `src/app/App.h` - owns the threads, the bot state machine, and the
  thread-safe `UiSnapshot` the UI polls on a timer.

## Configuration reference (`config/config.ini`)

| Section | Key | Default | Meaning |
|---|---|---|---|
| roi | screenX/screenY/width/height | 1410,315,59,390 | fishing bar rect, screen space |
| roi | flankPixels | 16 | background sample margin each side (marker only) |
| timing | castingDetectDebounceMs | 150 | CASTING -> FISHING: both raw-present this long |
| timing | castMaxWaitMs | 7000 | CASTING safety ceiling if detection never lands |
| timing | noObjectTimeoutMs | 2000 | raw both-absent -> completion |
| timing | tHoldMs | 3000 | T hold duration |
| detection | targetBorderColumnRoiLocalX/SearchWidth | 0, 3 | which column(s) the target border is sampled from |
| detection | targetBorderRunHeightPx/TolerancePx | 24, 6 | expected flat border-run height |
| detection | targetBorderUniformityTolerance | 6 | max RGB drift within a run to still count as "flat" |
| detection | targetBorderColorAxisTolerance/Margin | 30, 0.3 | how loosely the run's color must sit on the in-zone<->out-of-zone axis |
| detection | marker* | see file | geometric acceptance thresholds (unchanged, works well) |
| tracking | maxJumpPxPerSec | 900 | implausible-jump rejection |
| tracking | missTimeoutMs | 700 | coast duration before "stale" |
| controller | deadbandPx | 10 | no-toggle band around target center |
| controller | minToggleIntervalMs | 90 | anti-chatter floor |
| controller | holdMovesMarkerUp | true | flip if your game's marker falls when held |

## Known limitations / next steps

- Input targets whatever window currently has focus (`SendInput`), per the
  v1 scope in the brief; `InputManager` is structured so a future version
  could target a specific `HWND`/process without touching callers.
- The target border's expected column, run height, and the two in-zone/
  out-of-zone colors (`detection.targetBorderColor*`) were measured
  directly against the live game and are believed accurate, but they are a
  fixed calibration - if the game's UI theme/resolution/scaling changes,
  these would need re-measuring.
- No live calibration UI; thresholds are edit-config-and-relaunch only.
- Monitor selection picks the DXGI output containing the ROI's top-left
  point at startup; changing which monitor the game is on requires a
  restart (or edit `roi.screenX/screenY` and restart).
