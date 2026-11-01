# ImView

A desktop viewer for comparing images side by side. Open up to four images in a
single window, inspect the same region across all of them numerically, browse
folders in a sidebar, and step through two folders pair by pair in review mode.

Built with C++17 on [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) with a
custom borderless window in the VS Code "Dark Modern" style. Version 0.1.0 is a
preview; the shipped artifact is a single self-contained `imview.exe`.

## Features

- **Grid** — one to four images, auto-scaled to fit their cells.
- **Zoom and pan** — the wheel zooms every image together; left-drag pans all of
  them, right-drag pans one. Panning never reveals empty space inside a cell.
- **Per-cell work** — rotate with `1`–`4`; each cell has a `✕` button to remove
  it and a four-way handle to drag it onto another cell and swap the two.
- **Solo view** — double-click a cell to see it full size; hold `1`–`4` to peek at
  another image on top of it.
- **Two-image compare** — hold `Q` to paint the right image over the left cell.
- **Local mode** — drag a rectangle and read the average `Luma`, `R/G`, `B/G`,
  `Sat`, `R`, `G`, `B` of that region on every image.
- **EXIF and histogram** — per-image overlays, toggled from the toolbar.
- **Folder sidebar** — one or two folders, a dot marking the files already in
  the grid, multi-select with `Ctrl`, and a row menu with *Add to main window*,
  *Show in Explorer*, *Open with...* and *Properties*.
- **Review mode** — with two folders open, step through them as pairs (`Space`
  next, `B` previous) with editing commands disabled.

EXIF can be taken over by an optional helper tool: if `imexif.exe`, `imexif.bat`
or `imexif.cmd` sits next to `imview.exe` (or in the working directory), it is
run as `imexif <file>` and its output lines become the EXIF text.

## Shortcuts

| Input | Action |
|---|---|
| `Ctrl`+`O` / `Ctrl`+`Shift`+`O` | Open images / open a folder |
| `Ctrl`+`B` | Show or hide the sidebar |
| Wheel | Zoom all images |
| Left drag / right drag | Pan all images / pan one cell |
| `1`–`4` | Rotate the image at that position |
| Double click | Solo view (click again to leave) |
| `Q` (hold) | Compare: right image over the left cell |
| `P` | Local mode |
| `E` / `H` / `R` (toolbar) | EXIF / histogram / review mode |
| `Space` / `B` | Review mode: next / previous pair |
| Cell `✕` / cell four-way handle | Remove a cell / drag it onto another to swap |
| `Esc` | Cancel a move, close the menu, or leave solo view |

The full list is in [doc/shortcuts.md](doc/shortcuts.md).

## Build

Requires Windows 10/11, a C++17 compiler and CMake 3.14+. EUI-NEO is a
submodule, so clone recursively:

```sh
git clone --recursive https://github.com/zixiangro/imview.git
cd imview
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The result is a single self-contained `build/imview.exe` — no `assets` folder,
no Visual C++ redistributable. See [doc/release.md](doc/release.md) for
packaging.

Tests for the non-UI parts (UTF-8, EXIF, decoding) run with `ctest` in `build/`.

## Run

```bat
build\imview.exe
build\imview.exe D:\images
build\imview.exe D:\left D:\right
```

Arguments may be image files or folders; folders open in the sidebar.

## Documentation

| Document | Contents |
|---|---|
| [doc/features.md](doc/features.md) | Every feature and its exact behaviour |
| [doc/shortcuts.md](doc/shortcuts.md) | Keyboard and mouse reference |
| [doc/interface.md](doc/interface.md) | Layout, sizes and colours |
| [doc/architecture.md](doc/architecture.md) | Source map, data flow, threading |
| [doc/build.md](doc/build.md) | Build requirements and commands |

## Scope

Desktop, single window, local files only. No editing, saving, exporting, video,
or network access.

## Licence

ImView is released into the public domain under the [Unlicense](LICENSE): use
it, modify it, sell it, no conditions and no attribution required.

The executable links third-party libraries that keep their own licences;
[3rd/THIRD-PARTY-NOTICES.md](3rd/THIRD-PARTY-NOTICES.md) lists them, and their texts
ship with every binary release.
