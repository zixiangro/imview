# Architecture

The application is a single `imview.exe`; the opt-in Explorer context-menu
helper is embedded as a resource, built on EUI-NEO. All application code lives in `src/`;
EUI-NEO is used through its public headers, with the first-party adaptations
listed in [eui-neo-gaps.md](eui-neo-gaps.md).

## Source layout

| File | Responsibility |
|---|---|
| `src/app.cpp` | Application state, decoding pipeline, sidebar, review mode, and all UI composition |
| `src/stb_wrap.cpp` / `.h` | Image decoding with stb_image over a memory-mapped file |
| `src/media_paths.cpp` / `.h` | Image/video extension classification used by the sidebar, drops, command line and file dialog |
| `src/video_player.cpp` / `.h` | Per-video FFmpeg video/audio/subtitle decoding, NV12 frames through an EUI `ImageStream`, transport state, audio device and thumbnail previewer |
| `src/video_player_stub.cpp` | No-op Player implementation used when `IMVIEW_ENABLE_VIDEO=OFF` |
| `src/exif.cpp` / `.h` | EXIF reader for JPEG (APP1) and PNG (`eXIf`); walks the container with seeks, so image data is never read |
| `src/shell_actions.cpp` / `.h` | Native pickers, URL/file opening, "show in Explorer", "open with", "properties", and the on-demand HKCU image/video/context-menu registration |
| `src/shell_menu.cpp` + `src/shell_menu_id.h` | `imview_shell.dll` build target embedded in the exe: opt-in Explorer `IShellExtInit`/`IContextMenu` handler for folder/image/video selections; launches a new window |
| `src/window_frame.cpp` / `.h` | Borderless Win32 window: custom frame, native or video-aware drag, resize, window buttons, DPI scale, cursor, OLE file-drop target, single-instance messaging and cursor polling |
| `src/utf8.h` | UTF-8/UTF-16 conversion, shared by the app and the shell actions |
| `tests/tests.cpp` | Logic tests: UTF-8, EXIF, media paths, image decoding |
| `tests/registry_guard.cpp` | Regression tests for the ImView-owned HKCU path allowlist |

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
| Worker pool (EUI `async`) | Image decoding, luma histogram, EXIF parsing, diff-map rebuilding |
| Video thread (one per cell) | FFmpeg demux, video/subtitle decode, NV12 conversion, frame pacing and seek/step commands |
| Audio thread (one per cell) | FFmpeg audio decode, libswresample conversion and a 1.36-second lock-free PCM ring feeding a miniaudio device; track changes restart it at the same timeline position |
| Preview thread (one per cell, lazy) | On-demand low-resolution thumbnail decode for the seek bar; starts on first hover and caches 64 frames |

Completion callbacks are delivered on the UI thread (EUI posts them to its main
queue), so the async workers never touch `state()`. Each video thread owns its
FFmpeg contexts; it publishes frames through the thread-safe `ImageStream` and
exposes a small mutex-protected `Status` snapshot (position, duration, paused,
audio state, subtitle text, failure). Closing a cell joins all of that player's
threads before the GPU resources are released.

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
├─ images      grid cells, in display order (images and videos)
│   └─ ImageEntry: path, isVideo, video Player, decoded pixels, stream,
│                  altStream, gpuImage, zoom/pan (clamped), rotation,
│                  selection + statistics, histogram, EXIF, overlay text
├─ volume, muted              one title-bar volume state for every video
├─ syncMode                  multi-video transport group
├─ diffMode, diffThreshold  computed third tile, 0..20 threshold
├─ diffImage                RGBA8 diff tile (not in `images`)
├─ diffSerial / diffRequest*  worker generation and current pair key
├─ diffSliderDragStart      toolbar slider drag anchor
├─ zoom / qSwapActive / move source+target
├─ activeVideoId             keyboard target for video transport
├─ soloId, peekId            solo view
├─ localMode, showExif, showHistogram
├─ folders[0..1]             sidebar panes
│   └─ FolderPane: dir, paths, names, videos[], scroll, listHeight
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
ImView menu / sidebar double click / command line / Explorer forward
        │
        ▼
   addPaths(paths)
        │
        ├─ image path ──────────────► startLoad(path)
        │                                │ async::restart
        │                                ▼
        │                          decodeRgba (worker)
        │                                │ shared_ptr pixels
        │                                ▼
        │                          ImageEntry (pixels; first compose uploads
        │                          the external texture, streams stay fallback)
        │
        └─ video path ──────────────► startVideoLoad(path)
                                         │ make_shared<video::Player>
                                         ▼
                                   video thread + audio thread
                                         │ NV12 frames → ImageStream
                                         │ PCM → miniaudio device
                                         │ text subtitles → Status
                                         ▼
                                   ImageEntry(isVideo, video)
        ▼
   compose() → updateDiffImage → composeGrid / composeSidebar /
               composeToolbar / composeContextMenu / composeAboutDialog
```

Every video cell owns an independent `imview::video::Player`. The player owns
its video thread, audio thread, lazy preview thread, `ImageStream` and status.
`enforceMediaMode()` runs after grid changes: if any video is present, it resets
zoom/pan, leaves diff/local mode and clears selections, so image-only features
stay off until the videos are removed. The toolbar reads `imageToolsEnabled()`
and draws its chips disabled rather than hiding them.

Explorer opening uses `window_frame`'s instance property and `WM_COPYDATA`.
A new process enumerates windows carrying the property and asks each one to add
the paths; the receiver checks capacity before mutating anything. If none can
accept the batch (or no window exists), the process becomes a new window and
opens the first media's folder in the sidebar. HKCU registration is opt-in and
can make ImView a Default Apps and Open With candidate for images and/or videos.

Diff mode is available only for two image cells; `diffPairReady()` rejects
videos. Review mode also reviews images only: it scans pair positions, skips a
position if either side is a video, and disables the `R` chip when no valid
image pair exists. Q, zoom, pan and local mode are also image-only.

Explorer drags enter through the OLE drop target in `window_frame.cpp`. Its
`FileDropEvent` phases update the highlighted destination; the drop phase calls
`handleFileDrop`, which routes image and video files to `addPaths` and folders
to `addFoldersFromDrop`.

## Decoding

- `decodeRgba()` maps the file read-only (`CreateFileMappingW` + `MapViewOfFile`,
  wrapped in a small RAII type) and calls `stbi_load_from_memory` with
  `STBI_rgb_alpha`, giving top-down RGBA8. Files that do not fit an `int` and
  images larger than 32768 px on a side are rejected before decoding.
- stb_image is compiled with `STB_IMAGE_STATIC` in its own translation unit, so
  its symbols never collide with the copy inside the EUI-NEO library.
- The worker also builds the luma histogram and reads the EXIF block, so the UI
  thread only copies the finished values into the entry.

## Video decoding

`src/video_player.cpp` opens a local file with `avformat_open_input`, selects the
best video, audio and text-subtitle streams and opens their decoders. Every
video, audio and text-subtitle stream is enumerated for the `V`/`A`/`S` track
buttons, and every embedded text subtitle decoder runs while the file plays so
switching tracks is immediate. The video loop drains `avcodec_receive_frame` /
feeds packets from `av_read_frame`, ignoring unrelated streams. Video frames are converted by libswscale to NV12
(or copied directly when already NV12), copied into one of three reusable Y/UV
buffer pairs and submitted to an `ImageStream` with the frame's colour space and
range. EUI's native NV12 path uploads the planes and converts them on the GPU.

Pacing uses `best_effort_timestamp` and the stream time base; a missing timestamp
falls back to the stream frame rate. The player pauses on decoder EOF instead of
looping; play from that state requests a seek to zero. A seek flushes the decoder, drops frames before the target and
resets the pacing clock. Pause stops decoding at the current frame; a seek while
paused decodes and displays one frame at the new position. Frame stepping decodes
the next frame forward, or seeks back and keeps the last frame before the current
timestamp for a backward step.

Audio is a separate FFmpeg decode thread in the same player. It converts decoded
audio with libswresample to packed stereo float at 48 kHz, writes it into a
lock-free ring buffer and a miniaudio playback device consumes it. Pause, seek
and volume/mute commands are atomic or mutex-protected; seeking stops the device,
flushes the ring and decoder, then restarts it. If the file has no audio stream
or the device cannot initialize, the video keeps playing; the global volume
group remains available for other videos in the window.

Text subtitle packets are decoded in the video thread with
`avcodec_decode_subtitle2`. ASS event lines are reduced to plain text, timed
events are kept per track and the current event is selected from the displayed
video timestamp. Sidecar text files with the same basename (`.srt`, `.ass`,
`.ssa`, `.vtt`, `.smi`, `.sub`, `.sbv`) are decoded once at open through the
same FFmpeg subtitle decoders and appended as extra tracks. Legacy ANSI text is
converted through the Windows ANSI code page. The UI draws the text with eight
black offset copies under a white fill, so there is no background plate. Bitmap
subtitle types are ignored.

The title-bar volume is owned by `AppState`; the UI writes it to every player,
and newly opened players copy it. Sync mode is also an app-level rule: entering
it pauses every player, transport helpers fan out commands when it is active,
and `enforcePlaybackRules()` keeps the default non-sync grid down to one playing
video. The player exposes an `ended` flag so the app can stop the whole sync
group when one file finishes.

The seek-bar preview is a separate `Previewer` that starts lazily on the first
hover. It has its own decoder with one thread, seeks to the requested second,
converts one frame to a 160x90-class RGBA image, caches it by one-second bucket
(64 entries) and submits it to a second `ImageStream`. It decodes only on demand
and performs no work while the pointer is away from the seek bar.

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
| `WM_LBUTTONDOWN/MOUSEMOVE/LBUTTONUP` | With a video, drive a client-side title-bar drag so UI frames keep composing; presses in the drag band are also matched into double clicks against the system interval |
| `WM_LBUTTONDBLCLK` | Maximise/restore in the client-side drag mode |
| `DwmSetWindowAttribute` | Square corners and no native border line while maximised |
| `SetWindowRgn` | Full-window region while maximised, so DWM cannot reveal the desktop at rounded corners |

`IDropTarget` is registered with `RegisterDragDrop`. It converts `CF_HDROP`
paths to UTF-8, reports drag enter/over/leave/drop with logical client
coordinates, and lets the application choose `DROPEFFECT_COPY` or
`DROPEFFECT_NONE` for the shell cursor.

The window also carries an instance property. A second process enumerates those
windows, sends the media paths through `WM_COPYDATA` and receives `TRUE` only
when the receiver added them. A named local mutex serializes the
forward-or-create decision during startup. `cursorLogicalPosition()` polls the
real cursor for the hover hot zones, and file associations are registered under
`HKCU\Software\Classes` / `RegisteredApplications` without touching the user's
`UserChoice`.

Maximising resizes to the monitor work area rather than entering the native
zoomed state, because a zoomed window forces a frame-inflated rect that
`SetWindowPos` cannot correct. DWM corner and border attributes are switched to
square, borderless values while maximised, and a full-window region removes the
last rounded-corner gap so the custom frame reaches all four window edges.
### Explorer integration

`imview.exe` does not touch the registry at startup. The About dialog's three
switches call `imview::shell` functions. Image and video registration share a
single app registration key, but each switch adds or removes only its own
ProgID, `Applications\imview.exe`/`SupportedTypes`, `OpenWithProgids` and
`Capabilities\FileAssociations` entries. Turning both off removes the
app/capabilities keys as well. Every write and delete is
restricted to fixed ImView-owned HKCU paths; registration never writes an
explicit `UserChoice` or a desktop, Start-menu, or policy key.

The right-click switch extracts the embedded DLL to
`%LOCALAPPDATA%\ImView\ShellExt\imview_shell_<content-hash>.dll`, registers it
under `HKCU\Software\Classes\CLSID\{...}\InprocServer32`, stores the current
exe path under `HKCU\Software\ImView\ContextMenu\ExePath`, and installs the
handler under `SystemFileAssociations\image\shellex\ContextMenuHandlers\ImView`,
`SystemFileAssociations\video\shellex\ContextMenuHandlers\ImView`, and
`Directory\shellex\ContextMenuHandlers\ImView`. Registration removes the
all-files handler key so Explorer loads the DLL only for images, videos and
folders. The content hash in the file name makes every embedded build a
distinct DLL path; an existing file is reused only when its size and FNV-1a
hash match the resource. The DLL receives the complete selection through
`IShellExtInit`, adds a single `IContextMenu` entry only for the supported
selection shapes, and launches `imview.exe --new-window <paths...>`. The app
parses that flag before its single-instance forwarding path, so the action
always creates a fresh window. Disabling the switch removes the registration
and the extracted DLL (or schedules its removal at reboot when Explorer still
has it mapped). `imview.exe --register-integration` writes both media
candidates with the current executable path, without opening a window.
`imview.exe --uninstall-integration` performs the cleanup without opening a
window and leaves the user's explicit default apps alone.

Windows 11 presents this legacy context-menu handler under *Show more options*.

## Packages and files

At runtime the application reads no assets. The embedded `imview_shell.dll` is
written under `%LOCALAPPDATA%\ImView\ShellExt` and loaded by Explorer only after
the user enables the right-click switch. `cmake --install` places the single
executable and the licence texts in the release folder.


## Dependencies

| Dependency | Provided by | Used for |
|---|---|---|
| EUI-NEO | `3rd/EUI-NEO` | Windowing, rendering, DSL, async, text |
| stb_image | `3rd/EUI-NEO/3rd/stb_image.h` | Image decoding |
| FFmpeg | `3rd/ffmpeg`, pre-built static libs in `build/ffmpeg-build` | Video/audio/subtitle demuxing and decoding, resampling, colour conversion and pacing |
| miniaudio | `3rd/EUI-NEO/3rd/miniaudio.h` through EUI-NEO | Audio device output |
| Windows SDK | system | Shell actions, borderless frame |
