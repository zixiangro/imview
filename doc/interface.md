# Interface

The window is borderless and uses a dark "VS Code Dark Modern" palette. All
coordinates below are logical units (96 dpi); the UI scales with the monitor DPI.

## Window

| Property | Value |
|---|---|
| Initial size | 1000 x 700 |
| Minimum size | 640 x 420 |
| Frame source | `src/window_frame.cpp` (Win32) |

## Title bar (36 px)

```
┌────────┬──────────────────────────────────┬───────────────────────────────┐
│  Open  │      2 / 4 · EXIF · 150%         │  [E][H][P] [R]  │  −  □  ✕    │
└────────┴──────────────────────────────────┴───────────────────────────────┘
```

| Element | Position | Notes |
|---|---|---|
| `Open` menu button | x = 8, w = 58 | Dropdown: Open images…, Open folder…, Toggle sidebar |
| Status text | centred, 340 wide | Empty when nothing is shown |
| Mode chips | right of centre | 24x24 each; `E`, `H`, `P`, `R` |
| Window buttons | right edge | 36 wide, full bar height |

Status text contents, joined with `·`:

| State | Example |
|---|---|
| Grid | (nothing — the image count is not shown) |
| Solo | `2 / 4` |
| Review | `Review 3 / 20` |
| Review, single image pair | `Review 7 / 20  single` |
| Active modes | `EXIF`, `Histogram`, `Local` |
| Zoom | `150%` (only above 100%) |

Dragging any part of the bar that is not a control moves the window; the popup
menus temporarily disable that so their clicks are not swallowed.

## Grid area

| Property | Value |
|---|---|
| Outer margin | 12 |
| Gap between cells | 8 |
| Cell border | 1 px `#2B2B2B` |
| Cell background | `#212121` |

## Sidebar (240 px wide)

Shown while the sidebar is visible: opening a folder brings it up, opening
images hides it, and `Ctrl`+`B` toggles it. With no folder open it is an empty
panel. One or two panes, each `(window height - title bar) / pane count` tall.

| Element | Value |
|---|---|
| Pane header | 30 px: folder name, `+` (one pane only), `...`, `x` |
| Row height | 22 px |
| Row text | file name, 11.5 px |
| Selected row | `#0B3B60` background, white text |
| Hovered row | `#FFFFFF0E` |
| File already in the grid | 4 px accent dot at the left |
| Scrollbar | 4 px wide, 7 px from the right edge, thumb opacity 0.22 / 0.38 / 0.50 |

## Overlays inside a cell

| Overlay | Position | Style |
|---|---|---|
| EXIF summary | top-left | one 60% black strip per line, white 10 px text; each strip is sized to its measured text width |
| Cell buttons | top-right, 6 px inset | two 18x18 buttons, 2 px apart: move handle then close; 45% black plate, hover 72%, accent while the move is armed |
| Histogram | below the cell buttons, right | 80x48, 60% black, white bars |
| File name | bottom, centred | 11 px secondary text over the image |
| Local statistics | bottom-left | four 60% black strips, white 11 px text |
| Selection rectangle | over the image | 1.5 px border, blue while dragging, red when set |
| Move highlight | whole cell | armed cell: 2 px accent frame; drop target: accent frame plus 18% accent fill |

The move handle is a four-way arrow drawn from 1 px bars: one shaft per axis and
two 45° strokes per tip. Close is a 9 px cross. Both glyphs are drawn from
rectangles rather than text so they stay crisp at any DPI.

## Popup menu

| Property | Value |
|---|---|
| Width | 196 for the toolbar `Open` menu, 210 for row menus |
| Item height | 26, with 4 px padding around the panel |
| Item hint | right-aligned 11 px text (`Ctrl+O`, `Ctrl+Shift+O`, `Ctrl+B`) |
| Disabled item | secondary label colour, no hover state, not clickable |
| Backdrop | full-window transparent surface that closes the menu; it also stops the title bar from acting as a drag band |

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
