# Features

## Images

- Opened from the `ImView` menu or from the command line (file paths).
- At most **four** images are shown at once; extra paths are ignored.
- Supported formats: `.png` `.jpg` `.jpeg` `.bmp` `.gif`.
- Files are decoded on a worker thread; a cell shows `Loading...` until ready and
  `Failed to load` when decoding fails.
- Duplicate paths are ignored.

## Grid

- One to four images are laid out in a grid: one column for a single image, two
  columns otherwise.
- Every cell has the same size; rows are horizontally centred.
- Images are scaled to fit their cell without cropping or distortion (1x zoom =
  fully visible). The remaining space is left empty.

## Zoom and pan

- The mouse wheel over any cell zooms **all** images by the same factor.
- Zoom is clamped to the range 1x (fully fitted) to 128x.
- Above 1x, images are sampled with nearest-neighbour filtering, so magnified
  source pixels keep their exact colour instead of being blended.
- Left-drag pans all images; right-drag pans only the cell under the pointer.
- Panning is clamped: an image never shows empty space inside its cell.
- Combined panning stops at the limit of each image independently, so pushing
  past the edge is discarded rather than accumulated.

## Per-cell operations

- `1`–`4` rotate the image at that display position clockwise by 90°.
- Each cell carries two buttons in its top-right corner:

| Button | Behaviour |
|---|---|
| `✕` | Removes that cell |
| four-way arrows | Drag handle: press it, drag onto another cell, release to swap the two |

- Both buttons are hidden in review mode and in solo view; the move handle also
  needs at least two cells.
- Rotating, removing and swapping only affect this session, never the files.

## Solo view

- Double-clicking a cell shows that image full size; double-click again (or
  `Esc`) returns to the grid.
- While solo, holding a digit `1`–`4` temporarily shows the image at that
  position and releasing returns to the solo image.
- The indicator in the title bar shows `<position> / <count>`.

## Comparing two images

- With exactly two images, holding `Q` paints the right image over the left
  cell; releasing restores the left image. Display order is never changed.

## Local mode (region statistics)

- `P` toggles local mode.
- Left-drag draws one selection rectangle that appears at the same relative
  position on every image; the rectangle is blue while dragging and red once
  released. A click without dragging is a one-pixel ROI.
- On release each image shows four lines at its bottom-left corner with the
  average colour of the selected region:
  `Luma`, `R/G` and `B/G`, `Sat`, and `R` `G` `B` (two decimals). For a
  one-pixel ROI the values are exactly that pixel's values.
- Right-drag moves the selection of the image under the pointer only.
- While local mode is active the wheel does not zoom and dragging does not pan.
- Rotating an image, closing it, or leaving local mode clears the selections.

## EXIF summary

- Toolbar chip `E` shows, at each image's top-left corner, one black strip per
  line: file name, camera make/model, lens, aperture (`f/2.8`), shutter speed
  (`1/250s` or `2s`), `ISO 100`, and `Flash: On`/`Flash: Off`.
- Fields that are missing are skipped; a file without EXIF shows only its name.
- A helper program takes over the summary when present: `imexif.exe`,
  `imexif.bat` or `imexif.cmd` next to `imview.exe` (or in the working directory)
  is run as `imexif <file>` and its non-empty output lines are shown instead.

## Histogram

- Toolbar chip `H` shows an 80x48 semi-transparent panel near each image's
  top-right corner with the luminance distribution (256 bins) as white bars,
  normalised to the highest bin. The panel sits below the cell buttons when
  they are drawn.

## Folder sidebar

- Opened from the `ImView` menu (`Open folder...`); up to **two** folders can be
  open at once, shown as vertically equal panes. The folder picker accepts a
  multi-selection; selected folders fill the panes from the first one.
- Opened images hide the sidebar; opening a folder shows it again.
- Only the first level of a folder is listed, images only, sorted by file name
  (case-insensitive).
- Panes scroll with the wheel or by dragging the overlay scrollbar.
- A dot marks files that are currently shown in the grid.

### Selection

| Action | Result |
|---|---|
| Single click | Selects only that row |
| `Ctrl` + click | Adds/removes a row, across both panes |

### Row actions

| Action | Result |
|---|---|
| Double click | Clears the grid and shows that image |
| Right click | Opens the row menu (see below) |
| `+` in pane header | Opens the folder picker (shown while one pane exists) |
| `...` in pane header | Replaces that pane's folder |
| `x` in pane header | Closes that pane |

Row menu:

| Item | Result |
|---|---|
| Open | Replaces the grid with the clicked image |
| Add to main window | Adds the selected rows, at most four in total |
| Show in Explorer | Opens Explorer with the file selected |
| Open with... | Opens the system "Open with" chooser |
| Properties | Opens the filesystem properties dialog |

## Review mode

Entered with the toolbar `R` chip; requires exactly two folders.

- Entering immediately shows the first image of each folder, left pane first.
- Pairs are indexed by position: the *n*-th image of folder 1 with the *n*-th
  image of folder 2. When one folder is shorter, the remaining pairs open as a
  single image.
- `Space` advances to the next pair, `B` to the previous one; the side bar
  scrolls so both panes stay aligned by row index.
- The indicator shows `Review <n> / <total>`.
- The sidebar only accepts double-clicks: single click, selection, the context
  menu and the pane header buttons are disabled.
- Opening images, opening a folder, removing cells and swapping cells are
  disabled: the cell buttons are not drawn and `Ctrl+O` and `Ctrl+Shift+O` do
  nothing. Zooming, panning, rotating, solo view, local mode, `Q` swapping,
  EXIF and the histogram remain available.
- Leaving review mode keeps the current images and re-enables the operations
  above. Review mode is entered and left only through the `R` chip.

## Title bar

- The window is borderless; the title bar is drawn by the application.
- Dragging the empty part moves the window natively (Aero Snap, double-click to
  maximise).
- Left: the `ImView` menu. Centre: a status indicator. Right: mode chips and
  window buttons.
- The `ImView` menu holds `Open images...`, `Open folder...`, `Toggle sidebar`
  and `About`.
- `About` opens a centred dialog with the version, a short description, the
  project homepage link and the licence. It has `Close` and
  `Open project page` buttons.
- The window can be resized from its edges and corners.
