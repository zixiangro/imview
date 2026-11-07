# Architecture

The application is a single executable that builds on EUI-NEO. All application
code lives in `src/`; EUI-NEO is used through its public headers, with the
first-party adaptations listed in [eui-neo-gaps.md](eui-neo-gaps.md).

## Source layout

| File | Responsibility |
|---|---|
| `src/app.cpp` | Application state, decoding pipeline, sidebar, review mode, and all UI composition |
| `src/stb_wrap.cpp` / `.h` | Image decoding with stb_image over a memory-mapped file |
| `src/exif.cpp` / `.h` | EXIF reader for JPEG (APP1) and PNG (`eXIf`); walks the container with seeks, so image data is never read |
| `src/shell_actions.cpp` / `.h` | Native file/folder pickers, URL/file opening, "show in Explorer", "open with", "properties" |
| `src/window_frame.cpp` / `.h` | Borderless Win32 window: custom frame, native drag/resize, window buttons, DPI scale, cursor, OLE file-drop target |
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
| Worker pool (EUI `async`) | Decoding, luma histogram, EXIF parsing, diff-map rebuilding |

Completion callbacks are delivered on the UI thread (EUI posts them to its main
queue), so the worker never touches `state()`.

Decoded pixels are handed to the UI thread through `std::shared_ptr` payloads, so
no pixel buffer is copied on the UI thread.

Each loaded image owns a GPU texture created by the application and exposed to
EUI as an external `GpuImage`. The application substitutes a shared sampler for
EUI's external-image sampler so the filter follows the zoom state: linear while
fitted, nearest while the view is magnified so source pixels stay unblended. The
two `ImageStream` mailboxes remain as the fallback for backends or devices that
reject the external image; each stream yields its frame once, so a second copy
exists for the `Q` comparison and mode switches.

`app.cpp` wraps the OpenGL sampler binding for external images and swaps in a
shared sampler whose filter follows the zoom state.

## State model

```
AppState
├─ images      grid cells, in display order
│   └─ ImageEntry: path, decoded pixels, stream, altStream, gpuImage,
│                  zoom/pan (clamped), rotation, selection + statistics,
│                  histogram, EXIF, cached overlay text (exifLines, statsLines)
├─ diffMode, diffThreshold  computed third tile, 0..20 threshold
├─ diffImage                RGBA8 diff tile (not in `images`)
├─ diffSerial / diffRequest*  worker generation and current pair key
├─ diffSliderDragStart      toolbar slider drag anchor
├─ zoom / qSwapActive / move source+target
├─ soloId, peekId            solo view
├─ localMode, showExif, showHistogram
├─ folders[0..1]             sidebar panes
│   └─ FolderPane: dir, paths, names, scroll, listHeight
├─ sidebarVisible, sidebarWidth, sidebarDragStartWidth
├─ selected[]                (pane, index) pairs, shared across panes
├─ selectionAnchor           last clicked row for Shift ranges
├─ dropTarget                none / main grid / sidebar
├─ reviewMode, reviewIndex
├─ cellClick, rowClick       double-click trackers
├─ menuOpen, menuKind, menuX/Y, menuPane, menuIndex
└─ aboutOpen
```

Rendering is a pure function of this state: `compose()` rebuilds the element
tree every frame, and `requestUpdate()` schedules the next frame.

## Data flow

```
ImView menu / sidebar double click / command line
        │
        ▼
   addPaths(paths) ─────────────► startLoad(path)
        │                              │ async::restart
        │                              ▼
        │                        decodeRgba (worker)
        │                              │ shared_ptr pixels
        │                              ▼
        │                        ImageEntry (pixels; first compose uploads
        │                        the external texture, streams stay as fallback)
        ▼
   compose() → updateDiffImage → composeGrid / composeSidebar /
               composeToolbar / composeContextMenu / composeAboutDialog
```

Diff mode sits beside the image pipeline: `updateDiffImage()` compares the pair
and restarts an `imview.diff` worker whenever the two image ids or the threshold
change. `diffPairReady()` requires two decoded images with matching pixel
dimensions; the worker rejects mismatched sizes instead of resampling them. The
result is stored in `AppState::diffImage`, which is rendered as an extra layout
cell but never added to `images`. `addPaths`, `removeImage` and decode
completion call `enforceDiffMode()`, which exits the mode for any count other
than two, a failed decode, or a completed pair with mismatched dimensions;
review pair switching passes `preserveDiff` to `addPaths` so the mode can
survive while the new pair loads.

Review mode drives the same path: `openReviewPair(n)` clears the grid and calls
`addPaths` with the two files of pair `n`, left folder first.

Explorer drags enter through the OLE drop target in `window_frame.cpp`. Its
`FileDropEvent` phases update the highlighted destination; the drop phase calls
`handleFileDrop`, which routes image files to `addPaths` and folders to
`addFoldersFromDrop`.

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
| `WM_NCCALCSIZE` | Force the client area to the full window rect so the frame style produces no visible frame |
| `WM_NCHITTEST` | Edge bands become resize borders; the title band becomes `HTCAPTION` |
| `WM_NCLBUTTONDBLCLK` | Maximise/restore using the app's own maximise |
| `WM_SIZE` | Convert a system maximise into the app's own geometry |
| `WM_SETCURSOR` | Horizontal-resize cursor over the sidebar splitter |
| `WM_ENTERSIZEMOVE` | Restore a maximised window before dragging |
| `DwmSetWindowAttribute` | Square corners and no native border line while maximised |
| `SetWindowRgn` | Full-window region while maximised, so DWM cannot reveal the desktop at rounded corners |

`IDropTarget` is registered with `RegisterDragDrop`. It converts `CF_HDROP`
paths to UTF-8, reports drag enter/over/leave/drop with logical client
coordinates, and lets the application choose `DROPEFFECT_COPY` or
`DROPEFFECT_NONE` for the shell cursor.

Maximising resizes to the monitor work area rather than entering the native
zoomed state, because a zoomed window forces a frame-inflated rect that
`SetWindowPos` cannot correct. DWM corner and border attributes are switched to
square, borderless values while maximised, and a full-window region removes the
last rounded-corner gap so the custom frame reaches all four window edges.

## Dependencies

| Dependency | Provided by | Used for |
|---|---|---|
| EUI-NEO | `3rd/EUI-NEO` | Windowing, rendering, DSL, async, text |
| stb_image | `3rd/EUI-NEO/3rd/stb_image.h` | Decoding |
| Windows SDK | system | Shell actions, borderless frame |
