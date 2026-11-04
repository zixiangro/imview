# EUI-NEO gaps and first-party adaptations

EUI-NEO covers the DSL, composition, rendering, input, async and image streams
that ImView builds on. A few desktop behaviours have no public EUI-NEO API yet,
so ImView implements them in its own files. This page lists those adaptations
and the capability that would let them move back into the framework.

Nothing here is a fork: `3rd/EUI-NEO` stays untouched.

## Borderless window and native frame behaviour

**Gap.** `decorated(false)` produces a plain `WS_POPUP` window. Windows then
provides no native resize frame, move loop, Aero Snap, DWM shadow or maximise
geometry for it.

**Adaptation.** `src/window_frame.cpp` subclasses the GLFW window procedure and:

- re-adds `WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX`;
- removes the visible frame with `WM_NCCALCSIZE`;
- turns edge bands into resize hit-tests and the title band into `HTCAPTION`
  (`WM_NCHITTEST`);
- implements maximise/restore, DWM corner/border appearance and the maximised
  window region;
- disables the title-band drag while an in-app menu or dialog is open.

`src/window_frame.h` exposes the small window operations the app calls
(`minimize`, `maximize`, `close`, `center`, `dpiScale`, title-bar drag and
splitter cursor).

**Migration.** A public custom-title-bar / borderless-window mode with native
move, resize, snap, maximise and DWM integration would replace most of
`window_frame.cpp`.

## File drag and drop

**Gap.** EUI-NEO has no drag-enter, drag-over, drag-leave or drop events. GLFW
registers its own `WM_DROPFILES` handler, but EUI-NEO does not expose the drop
callback, and `WM_DROPFILES` only reports the final drop, not the hover position.

**Adaptation.** `src/window_frame.cpp` registers an OLE `IDropTarget` on the
application window (`RegisterDragDrop`), reads `CF_HDROP`, converts paths to
UTF-8 and reports phases and logical client coordinates through
`imview::frame::FileDropHandler`. `src/app.cpp` uses those events to:

- highlight the main grid or the sidebar while the pointer is over a valid
  destination;
- return `DROPEFFECT_COPY` or `DROPEFFECT_NONE`, which also selects the shell's
  copy/no-drop cursor;
- add image files to the grid or folders to the sidebar on drop.

**Migration.** Public drag-enter / drag-over / drag-leave / drop handlers with
client coordinates and file paths would replace the OLE target and the
`FileDropEvent` plumbing.

## Cursor shapes

**Gap.** `eui::CursorShape` only has `Arrow` and `Hand`. Every `on*()` builder
call also forces `Hand`, so non-link controls cannot ask for a size, split,
text or crosshair cursor.

**Adaptation.** `window_frame.cpp` handles `WM_SETCURSOR` and shows
`IDC_SIZEWE` while the pointer is over the sidebar splitter. Other elements use
`Arrow` or `Hand`, and `.cursor(...)` is always the last call in a builder chain
so it overrides the handler-imposed cursor.

**Migration.** Size and resize cursor values in `eui::CursorShape`, without
overriding an explicit cursor in `on*()` helpers, would remove the
`WM_SETCURSOR` splitter branch and the ordering convention.

## Zoom sampling and external GPU images

**Gap.** EUI-NEO samples scaled images with linear filtering and exposes no
nearest-neighbour option, so magnified source pixels would be blended. It also
binds its own sampler for external GPU images and has no public per-image
filtering setting.

**Adaptation.** `src/app.cpp` uploads each decoded frame into an app-owned GL
texture (`ensureGpuImage`), imports it as an external `GpuImage`, and replaces
`glad_glBindSampler` with `bindImageSampler`. One shared sampler is switched
between `GL_LINEAR` and `GL_NEAREST` from the zoom state: linear while fitted,
nearest above 1x. The `ImageStream` fallback that is used when the GPU import is
unavailable remains linearly sampled.

**Migration.** A nearest-neighbour option or per-image min/mag filter setting
on image primitives, together with a public texture-import path, would remove
the GL sampler interception and the manual upload path.

## Image stream lifetime

**Gap.** `eui::ImageStream` is a one-shot mailbox. A composed image primitive
consumes the submitted frame, and losing that primitive loses its texture.
Showing the same image in a second cell (`Q` comparison) or temporarily
replacing the grid (solo view) therefore needs extra frame ownership.

**Adaptation.** Every `ImageEntry` owns two streams (`stream` and `altStream`);
`submitFrame` fills the relevant one just before it is drawn. Solo view keeps
every non-shown image composed as a 1x1 invisible shell with the same element
ids (`composeCellKeepAlive`) so its primitive and texture stay alive.

**Migration.** A retained image handle / reusable image source, or a stream
that can feed multiple consumers, would replace the two mailboxes and the
keep-alive composition.

## Text measurement for overlays

**Gap.** Text width is not exposed through the public `eui_neo.h` API. The EXIF
overlay needs the measured width to size each black strip to its text.

**Adaptation.** `src/app.cpp` includes the internal `core/render/text.h` and
calls `core::TextPrimitive::measureTextWidth` in `composeExifOverlay`.

**Migration.** A public text-measure function, or an auto-sized text
background, would remove the internal include and the manual measurement.
