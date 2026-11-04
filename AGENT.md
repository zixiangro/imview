# AGENT

This document describes the project as it stands. Two kinds of text are kept
strictly separate — this file and the descriptive docs state *what is*, while
local notes in `doc/notes/` hold investigation and rationale.

## What this project is

ImView is a Windows desktop image viewer and comparator built on EUI-NEO. The
application is C++17: `src/app.cpp` holds the state model and all UI
composition, and small helpers cover EXIF, decoding, the Win32 shell actions and
the borderless frame.

Start from [README.md](README.md) for the user-facing picture and
[doc/architecture.md](doc/architecture.md) for the code map.

## Repository layout

```text
CMakeLists.txt     project definition, wires EUI-NEO and the app sources
src/               application sources (all first-party code lives here)
tests/             logic tests for the non-UI parts, built as imview_tests
doc/               documentation; see "Documentation rules" below
3rd/EUI-NEO/       dependency, a git submodule (only its commit is tracked)
3rd/THIRD-PARTY-   licence notices for the shipped binary
    NOTICES.md
build/             build output, NOT tracked
scripts/           local helper scripts, NOT tracked
datas/             local sample data and captures, NOT tracked
doc/notes/         author's working notes, NOT tracked
```

Anything that only makes sense on one machine (build wrappers that import a
toolchain environment, launch helpers with absolute paths, capture utilities,
sample images, screenshots) belongs in `scripts/` or `datas/`, never in the
repository. Tracked files must build and run on any machine that satisfies the
requirements in [doc/build.md](doc/build.md).

## Build and run

Build with CMake as described in [doc/build.md](doc/build.md):

```sh
git clone --recursive https://github.com/zixiangro/imview.git
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The result is a single self-contained `build/imview.exe`; no runtime assets are
read from disk.

The linker cannot replace a running executable, so stop any running instance
before rebuilding.

`tests/tests.cpp` covers the logic that needs no window: the UTF-8 helpers, the
EXIF reader and the image decoder. Run it with `ctest` in the build directory.
Keep new parsing or format logic in a helper file so it stays testable instead of
hiding it in `src/app.cpp`. UI behaviour is still verified by building, running
and inspecting the window; `doc/notes/testing-notes.md` (local only) records the
techniques.

## Dependency rules

- `3rd/EUI-NEO` is a git submodule pinned to one commit. Never edit files under
  `3rd/`; bump the submodule commit deliberately instead. If a framework
  behaviour must change, adapt the application, or contribute upstream.
- Third-party helpers (`stb_image`) are included from the EUI-NEO tree. stb_image
  is compiled in `src/stb_wrap.cpp` with `STB_IMAGE_STATIC` so its symbols stay
  private; keep that isolation.

## Code conventions

- C++17, no exceptions used for control flow, no RTTI assumptions.
- All application code lives in `namespace app` inside `src/app.cpp` and is
  otherwise confined to its own file/namespace (see the header comments).
- `app::compose()` and `app::dslAppConfig()` are the only symbols EUI's entry
  point links against. Keep their names and linkage.
- State is a single anonymous-namespace struct reached through `state()`.
  Mutate it from UI/worker callbacks and call `app::requestUpdate()`.
- Rendering is a pure function of state: `compose()` may be called every frame,
  so it must not have side effects other than cache refreshes (the pan clamp and
  the cached cell rectangles are the only exceptions).
- Strings are UTF-8. Any `std::filesystem` call that takes a path built from a
  UTF-8 `std::string` must use `u8path`, and display text must use `u8string`.
- Reuse the existing helpers (`composeToggleChip`, `composeWindowButton`,
  `composeCellButton`, `panBy`, `setReviewScroll`, ...) rather than adding new
  ad-hoc widgets.

### EUI-NEO traps worth re-reading before touching input or images

- `on*()` builder calls force the Hand cursor; `.cursor(...)` must come last.
- A `ui.rect` without an explicit colour is opaque white.
- `ImageStream` is a one-shot mailbox; see `doc/architecture.md` for the two
  streams per image.
- `async::runOnce` silently rejects a duplicate key; use `async::restart` for
  supersede semantics.
- `HitTestMode::None` disables hit testing entirely, including `onScroll`.

## UI conventions

- Palette and metrics live in the constants block at the top of `src/app.cpp`
  (`kBackground`, `kTitleBar`, `kToolbarHeight`, ...). Use them; do not inline
  new colour literals.
- The window is borderless. Anything drawn in the 36 px title bar that must be
  clickable has to be accounted for in the left/right control widths passed to
  `imview::frame::installBorderlessWindow`.
- Four-space indentation, 100-column soft limit, braces on the same line.

## Documentation rules

Two kinds of text, kept strictly separate:

1. **Descriptive documentation** lives in `doc/*.md` and is committed. It states
   only what the code does *today*: behaviour, layout, structure, build steps.
   It must not mention history, alternatives considered, bugs that were fixed, or
   reasons for a design — no "we used to", no "this was changed because".

   | File | Owns |
   |---|---|
   | `README.md` | user-facing overview, features, shortcuts, build, run |
   | `doc/features.md` | exact behaviour of every feature |
   | `doc/shortcuts.md` | input reference |
   | `doc/interface.md` | layout, control sizes, colour tokens |
   | `doc/architecture.md` | source map, state model, data flow, threading |
   | `doc/eui-neo-gaps.md` | framework gaps and the first-party adaptations that can migrate |
   | `doc/build.md` | requirements, commands, repository layout |

2. **Working notes** live in `doc/notes/` and are **not committed** (see
   `.gitignore`). They capture investigation, framework traps, verification
   tricks, and rationale.

When a change alters behaviour, update the matching descriptive document in the
same commit. When a change only explains *why*, put that in `doc/notes/`.

## Commit hygiene

- `.gitignore` keeps out `build/`, `scripts/`, `datas/` and `doc/notes/`. Do
  not add files from those paths. `3rd/EUI-NEO` is tracked as a submodule
  gitlink only; never add its contents (a plain file under `3rd/`, such as the
  notices, is tracked normally).
- Temporary self-test hooks (`std::getenv("IMVIEW_SELFTEST_*")`) and debug dumps
  must be removed before committing; search for `SELFTEST` and for absolute
  paths that point at the author's machine.
- No machine-specific content in tracked files: no toolchain installation paths,
  no absolute directories, no launcher scripts tied to one machine.
- A commit that changes the project version number must contain only that version
  bump; feature, documentation and build changes stay in separate commits.

## Checklist before finishing a change

1. The project builds with the commands in [doc/build.md](doc/build.md).
2. The application launches and the touched feature works (see the verification
   notes in `doc/notes/testing-notes.md`).
3. No leftover debug hooks, temporary files, or screenshots in the tree.
4. `git status` shows only intended files; `scripts/`, `datas/` and `doc/notes/`
   stay untracked.
5. Descriptive docs updated if behaviour changed.
