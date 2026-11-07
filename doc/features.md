# Features

## Images

- Opened from the `ImView` menu, from the command line (file paths), or by
  dropping files from Explorer.
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

## Drag and drop

- Image files dropped on the main area are added to the grid: existing cells
  stay, duplicates are ignored and the four-image cap applies. The sidebar keeps
  its current visibility.
- Folders dropped on the sidebar fill free panes. One or two folders can be
  dropped at once; with both panes occupied the sidebar is not a drop target.
- A drop that contains both files and folders is filtered by destination: the
  main area takes the image files and the sidebar takes the folders.
- While a valid drop is hovering, a 2 px accent frame marks the main area or the
  sidebar. Its top edge is 1 px inside the toolbar boundary; the left, right and
  bottom edges are 2 px inside the target.
- Dropping anywhere else, and any drop in review mode, does nothing.

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

## Diff mode

- The `D` chip is enabled with exactly two decoded images whose pixel
  dimensions match. It adds a third tile below the pair; the tile is not part
  of the image grid, so it neither counts towards the four-image cap nor changes
  the review-mode rules.
- The tile compares same-coordinate pixels. With
  `d = max(abs(ΔR), abs(ΔG), abs(ΔB))`, `d == 0` is grey, `d` inside the
  tolerance is blue and larger differences are red. Each output pixel is
  `(5 * left + 5 * right + 10 * class + 10) / 20`, so the two sources stay
  visible under the class colour.
- Images of different sizes are never resampled. Enabling diff requires a
  matching pair, and a failed decode or a dimension mismatch exits the mode.
- The threshold slider at the left end of the title bar (next to the `ImView`
  menu) sets the threshold from 0 to 20 in 8-bit channel steps; the default is
  5. The value is shown to its right.
- Diff mode exits automatically when an image add or remove leaves a count
  other than two, when a decode fails, or when a completed pair has different
  dimensions. Dropping two images onto a two-image diff grid exits the mode
  first and then shows all four images normally.
- Review pair switching keeps diff mode while the new pair loads, then exits if
  the pair cannot produce a diff. The sidebar's *Open* with exactly two
  selected rows behaves the same way.
- The diff tile follows the shared zoom and pan, but has no close/move buttons,
  no rotation, no local-mode statistics, no EXIF overlay and no histogram.

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
- The sidebar is 240 px wide by default. Drag the divider on its right edge to
  resize it; the width is clamped to 160 px minimum and leaves at least 320 px
  for the grid.
- A dot marks files that are currently shown in the grid.

### Selection

| Action | Result |
|---|---|
| Single click | Selects only that row |
| `Ctrl` + click | Adds/removes a row, across both panes |
| `Shift` + click | Selects the range from the last clicked row to this row in the same pane |
| `Ctrl` + `Shift` + click | Adds that range to the existing selection |
| Right click | Opens the row menu; a row outside the current selection becomes the only selected row |

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
| Open | Replaces the grid with every selected row, at most four, left pane first |
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
- File drag and drop is disabled.
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
