# Releasing

## Versioning

The version lives in `CMakeLists.txt` (`project(imview VERSION ...)`) and is
compiled into the executable as a Windows version resource, so
`imview.exe → Properties → Details` shows it. Bump it there; nothing else needs
editing.

`0.x.y` means preview: features may change without notice. `1.0.0` is the first
stable release and will be tagged; preview builds are not tagged.

## What a release consists of

ImView is a **single file**: `imview.exe`. Nothing is read from disk at startup.

| Item | Ships? | Why |
|---|---|---|
| `imview.exe` | yes | statically linked CRT and dependencies |
| `assets/` (EUI's fonts, icon, shaders) | no | unused; text falls back to the system font |
| `3rd/THIRD-PARTY-NOTICES.md` + licence texts | yes | required by the Apache-2.0 and MIT-style licences |
| `imexif.exe` | optional | user-supplied EXIF helper; not part of the release |

The executable needs no Visual C++ redistributable: the CRT is linked
statically (confirm with `dumpbin /dependents imview.exe` — only system DLLs
should appear).

## Building a release

```sh
git clone --recursive https://github.com/zixiangro/imview.git
cd imview
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cd build && ctest --output-on-failure
```

The dependency is pinned by the `3rd/EUI-NEO` submodule; `git submodule status`
prints the exact commit that was built. Record it in the release notes:

```sh
git submodule status          # e.g. 1f62d333... 3rd/EUI-NEO (v0.6.2)
```

`doc/notes/` and `scripts/` are machine-local and are not part of a release.

## Packaging

`CMakeLists.txt` carries the install rules, so one command produces the release
folder (binary, notices, licence texts):

```sh
cmake --install build --prefix dist/imview-0.1.0-win64
cd dist && zip -r imview-0.1.0-win64.zip imview-0.1.0-win64
```

```text
imview-0.1.0-win64/
├─ imview.exe
├─ LICENSE                     (Unlicense, applies to ImView itself)
├─ THIRD-PARTY-NOTICES.md
└─ licenses/
   ├─ EUI-NEO-Apache-2.0.txt
   ├─ GLFW-zlib.txt
   ├─ FreeType-FTL.txt
   ├─ libpng.txt
   ├─ zlib.txt
   └─ md4c-MIT.txt
```

Ship the zip, not the bare exe: the licence texts must travel with the binary.

The licence files are copied out of the pinned submodule at install time, so
they always match the code that was built. Adding a component means adding a
row to `3rd/THIRD-PARTY-NOTICES.md` and one `install(FILES ...)` line.

## Verification before publishing

| Check | Command |
|---|---|
| Logic tests | `ctest --output-on-failure` in `build/` passes |
| Version resource | right-click → Properties → Details shows `0.1.0` |
| No icon | the exe has none yet (the project has not picked one) |
| Runs from an empty directory | copy only `imview.exe` somewhere and start it |
| Chinese paths | open a folder with Chinese names; the sidebar must render them |
| Dependencies | `dumpbin /dependents imview.exe` lists system DLLs only |

## Not done yet

- No application icon (deliberate).
- No code signing: Windows SmartScreen warns on first run.
- No CI build: releases are built locally from a tagged or preview commit.
