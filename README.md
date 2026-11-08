# ImView

A desktop viewer for comparing images side by side and playing common video
files. Open up to four images or videos in a single grid, inspect the same
region across images numerically, browse mixed folders in a sidebar, and step
through two folders pair by pair in review mode.

Built with C++17 on [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) with a
custom borderless window in the VS Code "Dark Modern" style. Version 0.2.0 is a
preview; the application is a single `imview.exe` with no runtime assets. The
opt-in Explorer helper is embedded in the executable.

## Features

- **Video** — open `.mp4`, `.mkv`, `.webm`, `.avi`, `.mov`, `.wmv`, `.flv`,
  `.ts`, `.mpg`, `.ogv`, `.rm` and related common files from the Open dialog,
  the command line, a drop or the sidebar. Videos are ordinary grid cells and
  count towards the four-media cap. Each cell plays audio and embedded or
  sidecar text subtitles, pauses at the end instead of looping, and shows
  hover-only transport controls: play/pause, left/right seek buttons, a seek
  bar with thumbnail preview, `V`/`A`/`S` track selectors and a subtitle toggle.
  `Ctrl`+`←`/`→` steps one frame while paused; `←`/`→` seeks five seconds.
  By default one video plays at a time; the `S` chip enables synchronized
  playback only when the grid has more than one video and no images. Hover the
  cell to reveal its controls and close/move buttons.
- **Grid** — one to four images and/or videos, auto-scaled to fit their cells.
- **Mixed media** — while images and videos share the grid, image-only tools
  (zoom, pan, rotate, `Q`, diff, EXIF, histogram, local mode) are greyed out;
  they return when only images remain. Videos keep their own controls and the
  one global volume slider stays available.
- **Explorer integration (opt-in)** — nothing is registered at startup. The
  About dialog has separate switches for images and videos as Default Apps
  candidates, plus a right-click `Open with ImView in new window`
  entry for a single folder, two to four images, two to four videos, or one to
  two folders. The right-click entry always starts a new window. Image/video
  registration is HKCU only and never changes an explicit default.
- **Zoom and pan** — the wheel zooms every image together; left-drag pans all of
  them, right-drag pans one. Panning never reveals empty space inside a cell.
  Zoomed pixels use nearest-neighbour sampling so their values stay exact.
- **Per-cell work** — rotate images with `1`–`4`; each cell has a `✕` button to
  remove it and a four-way handle to drag it onto another cell and swap the two.
  The two buttons auto-hide and appear only while the pointer is inside the cell.
- **Solo view** — double-click a cell to see it full size; hold `1`–`4` to peek at
  another image on top of it.
- **Two-image compare** — hold `Q` to paint the right image over the left cell.
- **Diff mode** — with exactly two decoded images of equal pixel size, the `D`
  chip adds a third tile. Identical pixels are grey, differences inside the
  tolerance are blue and larger differences are red; the tile blends both
  sources with that class colour. A title-bar slider sets the tolerance (0–20,
  default 5). The tile is not a grid image: it does not count towards the
  four-media cap.
- **Local mode** — drag a rectangle and read the average `Luma`, `R/G`, `B/G`,
  `Sat`, `R`, `G`, `B` of that region on every image. A single click measures
  exactly one pixel.
- **EXIF and histogram** — per-image overlays, toggled from the toolbar.
- **Folder sidebar** — one or two folders (the folder picker accepts multiple
  selections), a resizable split from the grid, image/video icons, a dot marking
  the files already in the grid, multi-select with `Ctrl` or `Shift`, and a row
  menu with *Open*, *Add to main window*, *Show in Explorer*, *Open with...* and
  *Properties*.
- **Drag and drop** — drop image or video files onto the main area to add them,
  or one or two folders onto the sidebar; a highlight marks the active drop
  region.
- **Review mode** — with two folders open, step through their images as pairs
  (`Space` next, `B` previous) with editing commands disabled. Video positions
  are skipped; the chip is disabled when no image pair exists.
- **About dialog** — version, project link and licence from the `ImView` menu.

EXIF can be taken over by an optional helper tool: if `imexif.exe`, `imexif.bat`
or `imexif.cmd` sits next to `imview.exe` (or in the working directory), it is
run as `imexif <file>` and its output lines become the EXIF text.

## Shortcuts

| Input | Action |
|---|---|
| `Ctrl`+`O` / `Ctrl`+`Shift`+`O` | Open / open a folder |
| `Ctrl`+`B` | Show or hide the sidebar |
| Wheel | Zoom all images |
| Left drag / right drag | Pan all images / pan one cell |
| `1`–`4` | Rotate the image at that position |
| Double click | Solo view (click again to leave) |
| `Q` (hold) | Compare: right image over the left cell |
| `P` | Local mode |
| `D` (toolbar, two images) | Diff mode; the adjacent slider sets the threshold |
| `E` / `H` / `R` (toolbar) | EXIF / histogram / review mode |
| `Space` / `B` | Review mode: next / previous pair |
| Cell `✕` / cell four-way handle | Remove a cell / drag it onto another to swap |
| `Shift`+click in the sidebar | Select a range from the last clicked row |
| Drop files / folders | Add images or videos to the grid, or add folders to the sidebar |
| `Esc` | Cancel a move, close the menu, or leave solo view |
| Click / `Space` (video) | Pause or resume the active video |
| `←` / `→` (video) | Seek five seconds back or forward |
| `Ctrl`+`←` / `→` (video) | Step one frame back or forward (pauses playback) |
| Click the progress bar (video) | Seek to that position; hover shows a thumbnail preview |
| `V1` / `A1` / `S1` (video controls) | Cycle video / audio / subtitle tracks |
| `S` (toolbar, >1 video) | Toggle synchronized playback; entering pauses every video |
| `Vol` / `M` (title bar) | Set the global volume / mute every video |
| `←` / `→` seek buttons (video) | Seek five seconds back or forward |

The full list is in [doc/shortcuts.md](doc/shortcuts.md).

## Build

Requires Windows 10/11, Visual Studio with the C++ workload, CMake 3.14+, and
GNU make, nasm and a POSIX shell for the FFmpeg configure step (Git for
Windows or MSYS2; `3rd/tools` can provide make and nasm). EUI-NEO and FFmpeg
are submodules, so clone recursively:

```sh
git clone --recursive https://github.com/zixiangro/imview.git
cd imview
```

Build the minimal static FFmpeg once from a normal command prompt:

```bat
tools\build_ffmpeg.cmd
```

Then build ImView:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

For an image-only binary, configure with `-DIMVIEW_ENABLE_VIDEO=OFF`; FFmpeg is
then not required and all video-related UI, formats, sync mode and integration
cases are disabled.

The result is a single `build/imview.exe`; the opt-in Explorer context-menu
helper is embedded and written to `%LOCALAPPDATA%` only when enabled. No
`assets` folder is used. The current MSVC build uses
the dynamic C++ runtime, so the target machine needs the Microsoft Visual C++
2015–2022 x64 runtime. See [doc/release.md](doc/release.md) for packaging.

Tests for the non-UI parts (UTF-8, EXIF, media paths, image decoding) run with
`ctest` in `build/`.

## Run

```bat
build\imview.exe
build\imview.exe D:\images
build\imview.exe D:\left D:\right
build\imview.exe D:\clips\sample.mp4
```

Arguments may be image files, video files or folders; folders open in the
sidebar and media joins the grid. A new window started with a file opens that
file's folder in the sidebar.

## Documentation

| Document | Contents |
|---|---|
| [doc/features.md](doc/features.md) | Every feature and its exact behaviour |
| [doc/shortcuts.md](doc/shortcuts.md) | Keyboard and mouse reference |
| [doc/interface.md](doc/interface.md) | Layout, sizes and colours |
| [doc/architecture.md](doc/architecture.md) | Source map, data flow, threading |
| [doc/eui-neo-gaps.md](doc/eui-neo-gaps.md) | Framework gaps and first-party adaptations |
| [doc/build.md](doc/build.md) | Build requirements and commands |

## Scope

Desktop, local files only. No editing, saving, exporting or network access.
Multiple windows can exist when Explorer opens media and every existing window
is full.

## Licence

ImView is released into the public domain under the [Unlicense](LICENSE): use
it, modify it, sell it, no conditions and no attribution required.

The executable links third-party libraries that keep their own licences;
[3rd/THIRD-PARTY-NOTICES.md](3rd/THIRD-PARTY-NOTICES.md) lists them, and their texts
ship with every binary release.
