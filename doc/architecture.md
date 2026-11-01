# Architecture

The application is a single executable that builds on EUI-NEO. All application
code lives in `src/`; EUI-NEO is used only through its public headers.

## Source layout

| File | Responsibility |
|---|---|
| `src/app.cpp` | Application state, decoding pipeline, sidebar, review mode, and all UI composition |
| `src/stb_wrap.cpp` / `.h` | Image decoding with stb_image over a memory-mapped file |
| `src/exif.cpp` / `.h` | EXIF reader for JPEG (APP1) and PNG (`eXIf`); walks the container with seeks, so image data is never read |
| `src/shell_actions.cpp` / `.h` | Native folder picker, "show in Explorer", "open with", "properties" |
| `src/window_frame.cpp` / `.h` | Borderless Win32 window: custom frame, native drag/resize, window buttons, DPI scale |
| `src/utf8.h` | UTF-8/UTF-16 conversion, shared by the app and the shell actions |
| `tests/tests.cpp` | Logic tests: UTF-8, EXIF, decoding |

`src/app.cpp` holds one anonymous-namespace state object (`state()`) and a set of
free functions that mutate it. `app::compose()` and `app::dslAppConfig()` are the
only symbols with external linkage; EUI's entry point calls them.

Pointer events reach the handlers in two coordinate systems: hover callbacks are
logical, drag callbacks are physical. `imview::frame::dpiScale()` converts the
latter, which the cell move handle needs because a pressed element keeps the
pointer capture and therefore receives no hover callbacks from other cells.

## Threading

| Thread | Work |
|---|---|
| UI / render | Composition, input, all state mutation |
| Worker pool (EUI `async`) | Decoding, luma histogram, EXIF parsing |

Completion callbacks are delivered on the UI thread (EUI posts them to its main
queue), so the worker never touches `state()`.

Decoded pixels are handed to the UI thread through `std::shared_ptr` payloads, so
no pixel buffer is copied on the UI thread.

Each loaded image owns **two** `eui::ImageStream` mailboxes holding the same
frame. A stream yields its frame once, and the renderer needs a second copy when
an image is painted in another cell (the `Q` comparison) or after a mode switch.

## State model

```
AppState
├─ images      grid cells, in display order
│   └─ ImageEntry: path, decoded pixels, stream, altStream,
│                  zoom/pan (clamped), rotation, selection + statistics,
│                  histogram, EXIF, cached overlay text (exifLines, statsLines)
├─ zoom / qSwapActive / move source+target
├─ soloId, peekId            solo view
├─ localMode, showExif, showHistogram
├─ folders[0..1]             sidebar panes
│   └─ FolderPane: dir, paths, names, scroll, listHeight
├─ selected[]                (pane, index) pairs, shared across panes
├─ reviewMode, reviewIndex
├─ cellClick, rowClick       double-click trackers
└─ menuOpen, menuKind, menuX/Y, menuPane, menuIndex
```

Rendering is a pure function of this state: `compose()` rebuilds the element
tree every frame, and `requestUpdate()` schedules the next frame.

## Data flow

```
Open menu / sidebar double click / command line
        │
        ▼
   addPaths(paths) ─────────────► startLoad(path)
        │                              │ async::restart
        │                              ▼
        │                        decodeRgba (worker)
        │                              │ shared_ptr pixels
        │                              ▼
        │                        ImageEntry + submit frame to both streams
        ▼
   compose() → composeGrid / composeSidebar / composeToolbar / composeContextMenu
```

Review mode drives the same path: `openReviewPair(n)` clears the grid and calls
`addPaths` with the two files of pair `n`, left folder first.

## Decoding

- `decodeRgba()` maps the file read-only (`CreateFileMappingW` + `MapViewOfFile`,
  wrapped in a small RAII type) and calls `stbi_load_from_memory` with
  `STBI_rgb_alpha`, giving top-down RGBA8. Files that do not fit an `int` and
  images larger than 32768 px on a side are rejected before decoding.
- stb_image is compiled with `STB_IMAGE_STATIC` in its own translation unit, so
  its symbols never collide with the copy inside the EUI-NEO library.
- The worker also builds the luma histogram and reads the EXIF block, so the UI
  thread only copies the finished values into the entry.

## Windows integration

`window_frame.cpp` subclasses the window produced by EUI:

| Message | Purpose |
|---|---|
| `WM_NCCALCSIZE` | Return 0 so the frame style produces no visible frame |
| `WM_NCHITTEST` | Edge bands become resize borders; the title band becomes `HTCAPTION` |
| `WM_NCLBUTTONDBLCLK` | Maximise/restore using the app's own maximise |
| `WM_SIZE` | Convert a system maximise into the app's own geometry |
| `WM_ENTERSIZEMOVE` | Restore a maximised window before dragging |

Maximising resizes to the monitor work area rather than entering the native
zoomed state, because a zoomed window forces a frame-inflated rect that
`SetWindowPos` cannot correct.

## Dependencies

| Dependency | Provided by | Used for |
|---|---|---|
| EUI-NEO | `3rd/EUI-NEO` | Windowing, rendering, DSL, async, text |
| stb_image | `3rd/EUI-NEO/3rd/stb_image.h` | Decoding |
| Windows SDK | system | Shell actions, borderless frame |
