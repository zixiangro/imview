# Releasing

## Versioning

The version lives in `CMakeLists.txt` (`project(imview VERSION ...)`) and is
compiled into the executable as a Windows version resource, so
`imview.exe → Properties → Details` shows it. Bump it there; nothing else needs
editing.

`0.x.y` means preview: features may change without notice. `1.0.0` is the first
stable release and will be tagged; preview builds are not tagged.

## What a release consists of

ImView is a single `imview.exe`. The opt-in Explorer helper is embedded in the
executable. Nothing else is read from disk at startup; enabling the right-click
switch extracts the helper under `%LOCALAPPDATA%\ImView\ShellExt` and disabling
it removes those files. `imview.exe --register-integration` refreshes both media
candidates without opening a window; `imview.exe --uninstall-integration`
removes the registration and extracted files without opening a window.

| Item | Ships? | Why |
|---|---|---|
| `imview.exe` | yes | application binary; MSVC x64 runtime is a separate system requirement |
| `assets/` (EUI's fonts, icon, shaders) | no | unused; text falls back to the system font |
| `3rd/THIRD-PARTY-NOTICES.md` + licence texts | yes | required by the Apache-2.0 and MIT-style licences |
| `imexif.exe` | optional | user-supplied EXIF helper; not part of the release |

The current MSVC build uses `/MD`, so the target machine needs the Microsoft
Visual C++ 2015–2022 x64 runtime (`MSVCP140.dll`, `VCRUNTIME140.dll`). A future
static-CRT build would remove that requirement; check
`dumpbin /dependents imview.exe` when packaging.

## Building a release

```bat
git clone --recursive https://github.com/zixiangro/imview.git
cd imview
tools\build_ffmpeg.cmd
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cd build && ctest --output-on-failure
```

The dependencies are pinned by the `3rd/EUI-NEO` and `3rd/ffmpeg` submodules;
`git submodule status` prints the exact commits that were built. Record both in
the release notes:

```sh
git submodule status          # EUI-NEO and FFmpeg commits
```

`doc/notes/` and `scripts/` are machine-local and are not part of a release.

## Packaging

`CMakeLists.txt` carries the install rules, so one command produces the release
folder (binary, notices, licence texts):

```sh
cmake --install build --prefix dist/imview-0.2.1-win64
cd dist && zip -r imview-0.2.1-win64.zip imview-0.2.1-win64
```

```text
imview-0.2.1-win64/
├─ imview.exe
├─ LICENSE                     (Unlicense, applies to ImView itself)
├─ THIRD-PARTY-NOTICES.md
└─ licenses/
   ├─ EUI-NEO-Apache-2.0.txt
   ├─ GLFW-zlib.txt
   ├─ FreeType-FTL.txt
   ├─ libpng.txt
   ├─ zlib.txt
   ├─ md4c-MIT.txt
   └─ FFmpeg-LGPL-2.1.txt
```

Ship the zip, not the bare exe: the licence texts must travel with the binary.

The licence files are copied out of the pinned submodule at install time, so
they always match the code that was built. Adding a component means adding a
row to `3rd/THIRD-PARTY-NOTICES.md` and one `install(FILES ...)` line.

## Verification before publishing

| Check | Command |
|---|---|
| Logic tests | `ctest --output-on-failure` in `build/` passes |
| Version resource | right-click → Properties → Details shows `0.2.1` |
| No icon | the exe has none yet (the project has not picked one) |
| Runs from an empty directory | copy only `imview.exe` somewhere and start it; the right-click switch extracts its embedded helper on demand |
| Chinese paths | open a folder with Chinese names; the sidebar must render them |
| Video | open an H.264 MP4 and a VP9 WebM; hover the cell for controls, seek with the bar and its preview, pause with `Space`, step with `Ctrl`+`←`/`→` and close the cell with `x` |
| Context menu | in About enable the right-click switch; select one folder, 2-4 images, 2-4 videos, or 2 folders in Explorer and confirm one new window opens with that content (Windows 11: Show more options) |
| Audio and subtitles | play a file with AAC audio; the title-bar volume is active. Open embedded or sidecar ASS/SRT text; the `CC` button is active and the outlined subtitle appears. `S1` cycles sidecar tracks |
| Tracks | use a file with multiple video/audio tracks; `V1`/`A1` cycle them without restarting the cell |
| Playback end | let a short clip finish; it stops on its final frame with the play icon instead of looping |
| Volume and sync | a new window starts at 50%. With two videos, `S` is active; entering it pauses both, and play/±5/frame-step affect both. The progress bar seeks only its cell; when one video ends, the finished one stays at its end |
| Mixed grid | open an image and a video together; the image chips and `S` are greyed, the volume slider works, close/move still work, and the image chips return after closing the video |
| Explorer | enable Images/Videos in About, choose ImView as the default app, double-click a file; with room it joins an existing window, otherwise a new window opens the file folder in the sidebar |
| Dependencies | `dumpbin /dependents imview.exe` lists the expected system DLLs plus the MSVC runtime; the VC++ x64 redistributable must be installed on the target machine |

## Not done yet

- No application icon (deliberate).
- No code signing: Windows SmartScreen warns on first run.
- No CI build: releases are built locally from a tagged or preview commit.
