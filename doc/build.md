# Build and run

## Requirements

- Windows 10/11 (the window layer uses Win32 APIs).
- A C++17 compiler.
- CMake 3.14 or newer.

Bundled third-party sources (GLFW, FreeType, libpng, zlib, stb_image, ...) are
compiled by EUI-NEO, so no system packages are needed.

## Dependency

EUI-NEO is a git submodule pinned at `3rd/EUI-NEO`. Clone recursively, or fetch
it afterwards:

```sh
git clone --recursive https://github.com/zixiangro/imview.git
# already cloned without submodules:
git submodule update --init --depth 1
```

Do not use GitHub's *Download ZIP*: the archive leaves out `3rd/EUI-NEO`.

`git submodule status` prints the exact EUI-NEO commit being built; record it
when preparing a release (see [release.md](release.md)).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The executable is written to `build/imview.exe` and is self-contained: no
`assets` folder is produced or needed. An incremental rebuild only needs the
second command.

## Tests

`build/imview_tests.exe` covers the logic that does not need a window: the UTF-8
helpers, the EXIF reader (JPEG and PNG, including malformed input) and the image
decoder. Fixtures are generated in code, so the repository holds no binary test
data.

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

Command-line arguments accept image files and folders. Folders open in the
sidebar; at most two folders are used.

## Optional helper

If `imexif.exe`, `imexif.bat` or `imexif.cmd` sits next to `imview.exe`, it is
invoked as `imexif <file>` and its multi-line output replaces the built-in EXIF
summary.

## Repository layout

```text
CMakeLists.txt          project, EUI-NEO integration, install rules
src/                    application sources
tests/                  logic tests (built as imview_tests)
res/                    Windows version resource template
doc/                    documentation
3rd/EUI-NEO/            dependency, a git submodule
3rd/THIRD-PARTY-NOTICES licences of everything linked into the binary
```
