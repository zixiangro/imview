# Interface

The window is borderless and uses a dark "VS Code Dark Modern" palette. All
coordinates below are logical units (96 dpi); the UI scales with the monitor DPI.
Borders and hairlines are snapped to the physical pixel grid so they stay at
least one device pixel wide instead of blurring across two.

## Window

| Property | Value |
|---|---|
| Initial size | 1000 x 700 |
| Initial position | centred on the monitor work area |
| Minimum size | 640 x 420 |
| Frame source | `src/window_frame.cpp` (Win32) |

## Title bar (36 px)

```
┌────────┬──────────────────────────────────┬───────────────────────────────┐
│ ImView │  Vol ────── 50% M   2 / 4 · Sync │ [D][E][H][P][R][S] │ − □ ✕  │
└────────┴──────────────────────────────────┴───────────────────────────────┘
```

| Element | Position | Notes |
|---|---|---|
| `ImView` menu button | x = 8, w = 66 | Dropdown: Open…, Open folder…, Toggle sidebar, About |
| Diff slider | x = 82, w = 182 | Only while diff mode is on: `Diff` label, 120-wide track, value |
| Volume group | x = 82, w = 212 | Only while a video is present: `Vol` label, 120-wide track, percent, `M`; shown instead of the diff slider |
| Status text | centred, 340 wide | Empty when nothing is shown; includes `Sync` while sync mode is on |
| Mode chips | right of centre | 24x24 each; `D`, `E`, `H`, `P`, `R`, `S` |
| Window buttons | right edge | 36 wide, full bar height |

The left interactive cluster passed to the borderless frame is 80 wide normally,
270 while the diff slider is visible and 302 while the volume group is visible.
The right cluster is 300 wide. These areas receive client clicks instead of
starting a native window drag.

Status text contents, joined with `·`:

| State | Example |
|---|---|
| Grid | (nothing — the media count is not shown) |
| Solo | `2 / 4` |
| Review | `Review 3 / 20` |
| Review, single image pair | `Review 7 / 20  single` |
| Active modes | `Diff`, `EXIF`, `Histogram`, `Local` |
| Sync mode | `Sync` |
| Zoom | `150%` (only above 100%) |

Dragging any part of the bar that is not a control moves the window; the popup
menus temporarily disable that so their clicks are not swallowed.

## Grid area

| Property | Value |
|---|---|
| Outer margin | 3 |
| Gap between cells | 2 |
| Cell border | 1 px `#2B2B2B` |
| Cell background | `#212121` |

The `D`, `E`, `H`, `P`, `R` and `S` chips are always drawn. The image chips are
greyed out while a video is present; image features return when only images
remain. In a mixed grid every image-only chip and `S` are disabled. `S` (sync)
is enabled only with more than one video and no images. The `R` chip still
leaves an active review mode.

Diff mode lays out three equal cells: the two images on the first row and the
computed diff tile centred on the second row. The tile uses the normal grid
geometry and has a `Diff <threshold>` strip at its top-left. Its three colours
are blue `#2F81F7` (within the tolerance), red `#E5534B` (outside it) and grey
`#6E6E6E` (identical pixels).

## Video cell

A video is a normal grid cell: one to four videos/images share the same layout.
The frame is fitted with `Contain`, the uncovered area is black, and no image
zoom/pan transforms apply. The cell is the hot zone: entering it shows the
close/move buttons and the transport immediately, without an animation; leaving
it hides them again. Image cells use the same auto-hide rule for their close and
move buttons.

The transport is a 44 px translucent black bar at the bottom of the cell:

| Element | Position | Notes |
|---|---|---|
| Seek bar | full width minus 16, top of the bar | 3 px track, accent fill, 8 px knob; click/drag seeks |
| Play/pause | left, 20x18 | triangle while paused, two bars while playing |
| Seek back / forward | next to play, 20x18 | left/right triangle icons; five seconds |
| Time | after the seek buttons | `mm:ss / mm:ss`, omitted when too narrow |
| Video track `V1` | right side, 22x18 | cycles video tracks; disabled with one |
| Audio track `A1` | right side | cycles audio tracks; `A-` when there is no audio |
| Subtitle track `S1` | right side | cycles embedded and sidecar text tracks; `S-` when none |
| `CC` | rightmost, 18x18 | accent while subtitles are on; grey without a text stream |

Hovering or dragging the seek bar draws a 160x90-class thumbnail above the bar
with its timestamp. The preview uses a separate low-resolution decoder thread
and a 64-frame cache.

Text subtitles are drawn near the bottom, above the transport when it is
visible, as white 12 px text with a black outline. There is no background plate.
Embedded and sidecar text tracks share the `S` cycle. Bitmap subtitles (PGS,
DVD/DVB) are not rendered. A failed open centres `Cannot play this video` and
the FFmpeg error line.

The window has one global volume group in the title bar: `Vol`, a 120 px slider,
the percentage and an `M` mute toggle. It applies to every video, including in
a mixed image/video grid. A new window starts at 50%; the title bar's volume
group is shown only while the grid contains a video.

## Drop feedback

While a valid Explorer drag hovers over the window, a 2 px accent frame marks
the destination: the main area for image or video files, or the sidebar for
folders. The frame is inset 1 px from the toolbar boundary and 2 px from the
other three edges.


## Sidebar (240 px wide by default)

Shown while the sidebar is visible: opening a folder brings it up, opening
images hides it, and `Ctrl`+`B` toggles it. With no folder open it is an empty
panel. One or two panes, each `(window height - title bar) / pane count` tall.
The width is adjustable from 160 px up to `window width - 320`.

| Element | Value |
|---|---|
| Resize divider | 6 px grab band centred on the right edge, 3 px on each side; cursor changes to horizontal resize |
| Pane header | 30 px: folder name, `+` (one pane only), `...`, `x` |
| Row height | 22 px |
| Row icon | 10x10 at x = 5: picture frame for images, play triangle for videos |
| Row text | file name, 11.5 px, starting at x = 26 |
| Selected row | `#0B3B60` background, white text |
| Hovered row | `#FFFFFF0E` |
| File already in the grid | 4 px accent dot after the icon |
| Scrollbar | 4 px wide, 7 px from the right edge, thumb opacity 0.22 / 0.38 / 0.50 |

## Overlays inside a cell

| Overlay | Position | Style |
|---|---|---|
| EXIF summary | top-left | one 60% black strip per line, white 10 px text; each strip is sized to its measured text width |
| Cell buttons | top-right, 6 px inset | two 18x18 buttons, 2 px apart: move handle then close; 45% black plate, hover 72%, accent while the move is armed |
| Histogram | below the cell buttons, right | 80x48, 60% black, white bars |
| File name | bottom, centred | 11 px secondary text over the image |
| Local statistics | bottom-left | four 60% black strips, white 11 px text |
| Selection rectangle | over the image | 1.5 px logical border, rounded to whole device pixels (1 px at 100% DPI), blue while dragging, red when set |
| Move highlight | whole cell | armed cell: 2 px accent frame; drop target: accent frame plus 18% accent fill |

The move handle is a four-way arrow drawn from 1 px bars: one shaft per axis and
two 45° strokes per tip. Close is a 9 px cross. Both glyphs are drawn from
rectangles rather than text so they stay crisp at any DPI.

## Popup menu

| Property | Value |
|---|---|
| Width | 196 for the toolbar `ImView` menu, 210 for row menus |
| Item height | 26, with 4 px padding around the panel |
| Items | Toolbar: `Open...`, `Open folder...`, `Toggle sidebar`, `About`. A row: `Open`, `Add to main window`, `Show in Explorer`, `Open with...`, `Properties` |
| Item hint | right-aligned 11 px text (`Ctrl+O`, `Ctrl+Shift+O`, `Ctrl+B`) |
| Disabled item | secondary label colour, no hover state, not clickable |
| Backdrop | full-window transparent surface that closes the menu; it also stops the title bar from acting as a drag band |

## About dialog

| Property | Value |
|---|---|
| Size | 540 x 470, clamped to the window with a 16 px margin |
| Position | centred in the window |
| Backdrop | full-window 55% black surface |
| Contents | title, version, description, project homepage, three integration switches, licence |
| Integration switches | `Images — Default app candidate`, `Videos — Default app candidate`, `Right-click — Open with ImView (new window)`; 14 px checkbox, whole row clickable; the right-click row extracts the embedded helper on first enable |
| Buttons | `Close` (secondary) and `Open project page` (primary) |

## Colours

| Token | Value | Used for |
|---|---|---|
| Title bar | `#181818` | title bar and sidebar |
| Editor background | `#1F1F1F` | grid area |
| Cell surface | `#212121` | cells, popup menu `#282828` |
| Border | `#2B2B2B` | hairlines and cell borders |
| Border strong | `#3C3C3C` | popup menu outline |
| Text primary | `#CCCCCC` | labels |
| Text secondary | `#9D9D9D` | status, glyphs |
| Accent | `#0078D4` | active chips, open-file dot |
| Accent hover | `#026EC1` | |
| Close-button hover | `#C42B1C` | |
| Selection drag | `#00B4FF` | |
| Selection fixed | `#FF5050` | |
