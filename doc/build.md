# Build and run

## Requirements

- Windows 10/11 (the window layer uses Win32 APIs).
- Visual Studio with the Desktop C++ workload (MSVC x64).
- CMake 3.14 or newer.
- A POSIX shell for the FFmpeg configure script: Git for Windows, MSYS2, or a
  standalone `sh.exe` on `PATH`.
- GNU make and nasm on `PATH` for the FFmpeg build; the local `3rd/tools`
  folder can provide both.

Bundled third-party sources (GLFW, FreeType, libpng, zlib, stb_image, ...) are
compiled by EUI-NEO, so no system packages are needed.

## Dependency

EUI-NEO and FFmpeg are git submodules pinned under `3rd/`. Clone recursively,
or fetch them afterwards:

```sh
git clone --recursive https://github.com/zixiangro/imview.git
# already cloned without submodules:
git submodule update --init --depth 1
```

Do not use GitHub's *Download ZIP*: the archive leaves out the submodules.

`git submodule status` prints the exact submodule commits. Record the EUI-NEO
and FFmpeg commits when preparing a release (see [release.md](release.md)).

## FFmpeg (media support)

ImView links a minimal, statically built FFmpeg. The libraries are not built by
CMake; build them once from an ordinary command prompt before configuring
ImView:

```bat
tools\build_ffmpeg.cmd
```

The script loads the MSVC x64 environment, finds a POSIX shell (Git for
Windows, MSYS2 or a standalone busybox `sh.exe`) and writes the libraries to
`build\ffmpeg-build`. CMake reads that directory by default; use
`-DIMVIEW_FFMPEG_ROOT=<dir>` for a different location.

The configure line enables the file protocol, the common container demuxers and
the sidecar text-subtitle demuxers (`srt`, `ass`, `webvtt`, `sami`, `microdvd`,
`subviewer` and `realtext`), the video and audio decoders, the text-subtitle
decoders listed in [features.md](features.md), libswscale and libswresample. It disables programs,
network, devices, filters, encoders, muxers and auto-detected external
libraries. The exact option list lives in
`tools/build_ffmpeg.sh`; set `IMVIEW_FFMPEG_RECONFIGURE=1` to rerun configure
in an existing build directory. The helper also rebuilds `allformats.o` after
configure, because its generated demuxer list is not tracked as a Makefile
dependency, and writes a small `.imview-external-subtitles` marker so an older
build directory is reconfigured once when sidecar support was added.

## Image-only build

Video support is controlled by one CMake option:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DIMVIEW_ENABLE_VIDEO=OFF
cmake --build build --parallel
```

With `IMVIEW_ENABLE_VIDEO=OFF`, FFmpeg and miniaudio are not linked; video
extensions, video playback/transport, sync mode, video file associations and
video context-menu cases are disabled. The image viewer, sidebar, review mode
and image-only integrations are unchanged. The `video_player_stub.cpp` target
keeps the shared UI API linkable without a decoder. Running
`tools\build_ffmpeg.cmd` is not required for this build.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The build writes a single `build/imview.exe`; the opt-in Explorer context-menu
handler is compiled into it as an embedded resource. No `assets` folder is
produced or needed. An incremental rebuild only needs the second command. When
the right-click switch is enabled the helper is extracted to
`%LOCALAPPDATA%\ImView\imview_shell.dll`; disabling it removes that file.

## Tests

`build/imview_tests.exe` covers the logic that does not need a window: the UTF-8
helpers, the EXIF reader (JPEG and PNG, including malformed input), the image
and video extension checks, and the image decoder. Fixtures are generated in
code, so the repository holds no binary test data.

```sh
cd build && ctest --output-on-failure
```

Configure with `-DIMVIEW_BUILD_TESTS=OFF` to skip the test target. The window
itself is still verified by hand; see `doc/notes/testing-notes.md` (local only).

## Run

```bat
build\imview.exe
build\imview.exe D:\images
build\imview.exe D:\left D:\right
build\imview.exe D:\photos\a.jpg D:\photos\b.jpg
```

Command-line arguments accept image files, video files and folders. Folders
open in the sidebar; at most two folders are used. Media joins the grid and
counts towards the four-media cap. A new window started with a media file also
opens that file's folder in the sidebar.

## Optional helper

If `imexif.exe`, `imexif.bat` or `imexif.cmd` sits next to `imview.exe`, it is
invoked as `imexif <file>` and its multi-line output replaces the built-in EXIF
summary.

Nothing is registered at startup. The About dialog offers separate switches for
image Default Apps, video Default Apps, and the Explorer
right-click handler embedded in the executable. Registration is HKCU only and does
not change an existing default. Explorer double-clicks are forwarded to an
existing window with free space, otherwise a new window is created.

## Repository layout

```text
CMakeLists.txt          project, EUI-NEO integration, install rules
src/                    application sources
tests/                  logic tests (built as imview_tests)
res/                    Windows version resource template
doc/                    documentation
tools/                  FFmpeg build helper (CMake does not run it automatically)
3rd/EUI-NEO/            build dependency, a git submodule
3rd/ffmpeg/             FFmpeg source, a git submodule
3rd/THIRD-PARTY-NOTICES licences of everything linked into the binary
/build/ffmpeg-build/    minimal static FFmpeg output (ignored by Git)
```
