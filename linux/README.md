# Linux/Bazzite port

A direct port of the Windows fishing bot (repo root) to Linux/X11, for
Roblox running under Sober. **Does not touch the Windows project at all** -
separate directory, separate CMake project.

Written **blind** - there is no Linux machine available to compile or run
this where it was written, so treat it as a first pass to build, run, and
report back on, not a finished/verified artifact.

## What's ported and how

Same feature set as the Windows app, same state machine, same timings, same
three macros (Sell Runo/Buy Fish Head/Sell Shiro) with the same steps and
H+6/G+6/H+7 chords, same F1-F4 hotkeys. Nothing added, nothing removed.

- **Detection, tracking, fishing control, config load/save**
  (`src/detection`, `src/tracking`, `src/controller`, `src/config`,
  `src/common/Types.h` at the repo root) - reused **completely unmodified**.
  This code has zero Windows dependency already (confirmed by grepping for
  `Windows.h` across all of it), so it's compiled as-is into this build too,
  not copied or reimplemented.
- **Capture** (`src/capture/X11Capture.*`) - DXGI Desktop Duplication ->
  X11 (MIT-SHM when available, falling back to plain `XGetImage`). Same
  capture model as Windows: a fixed sub-rectangle of the screen in absolute
  screen coordinates, not a specific application window.
- **Input** (`src/input/InputManager.*`) - `SendInput` -> the XTest
  extension. Mouse movement still glides via relative motion deltas
  (`XTestFakeRelativeMotionEvent`, not an absolute warp) for the same
  reason as the Windows version: Sober runs the literal same Roblox client
  code via Wine, so if it reads raw/relative input deltas on Windows, it
  almost certainly does on Linux too.
- **App orchestration / state machine / macros** (`src/app/App.*`) - a
  line-for-line port of the Windows `App.h/.cpp`. F1-F4 and the macro
  chords are both polled via physical key state
  (`InputManager::IsPhysicalKeyDown`, the X11 counterpart to
  `GetAsyncKeyState`) rather than Windows' `RegisterHotKey` for F1-F4
  specifically - X11 has no direct equivalent, and polling is what the
  chords already needed regardless, so both now share one mechanism. Same
  user-visible behavior; this doesn't require a GUI window loop to exist
  for hotkeys to work.

## What's not ported yet: the GUI

The status window and debug overlay are the one piece not yet built - this
is a console application for now. It logs every state transition, hotkey
press, and status change exactly like the Windows build already does to its
own console window (see `App.cpp`'s `LogLine` calls - that's existing
Windows behavior, not something invented here), plus a periodic status line
standing in for what the GUI would otherwise show on screen. This is next.

One known adaptation when the GUI is built: Windows has
`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` to make the debug
overlay's on-bar highlight invisible to its own capture while still visible
on screen - X11 has no equivalent (no compositor exposes this as a standard
property). The Windows app already has a defined fallback for exactly this
situation (when that flag isn't available): skip the on-bar highlight, keep
only the side panel and keybind block, since those never overlap the
captured ROI. The Linux GUI will use that same existing fallback path
permanently, rather than the on-bar highlight - not a new or removed
feature, the exact code path the Windows version already falls back to.

## Building (Bazzite)

Bazzite's base image is immutable (rpm-ostree) - the normal way to get a
C++ toolchain is a [Distrobox](https://distrobox.it/) container (comes
preinstalled on Bazzite):

```bash
distrobox create --name fishingbot-dev -i fedora:latest
distrobox enter fishingbot-dev

sudo dnf install -y cmake gcc-c++ libX11-devel libXtst-devel libXext-devel

cd /path/to/this/repo/linux
cmake -B build
cmake --build build
```

Distrobox shares your home directory and the host's X11/Wayland sockets by
default, so the binary runs directly:

```bash
./build/fishingbot-linux
```

## Config

Same `config/config.ini` format as Windows (created next to the binary on
first run, same keys, same three macro sections). `roi.screenX`/`screenY`
need to be set to wherever Sober's Roblox window actually sits on your
screen - same manual setup as the Windows version.

## What to report back

- Does it build at all - if not, the exact compiler/linker error.
- Does `F1` actually enable the bot, does detection pick up the fishing bar
  (watch the periodic status line for `marker=present`/`target=present`),
  does the mouse-hold/T-tap control loop work.
- Do the macro chords (H+6/G+6/H+7) trigger, and do the clicks/holds
  actually land in Roblox (same open question as the Windows side has -
  Sober might ignore XTest-synthesized input the way some Windows games
  ignore SendInput-adjacent tricks).
- Any X11/XTest-specific runtime errors (e.g. `MIT-SHM` permission issues
  inside a sandboxed/containerized environment, which would show up as a
  fallback-to-XGetImage log line rather than a crash).
