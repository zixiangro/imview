# Shortcuts and mouse bindings

## Global

| Input | Condition | Action |
|---|---|---|
| `Ctrl+O` | Not in review mode | Open the images dialog |
| `Ctrl+Shift+O` | Not in review mode | Open the folder dialog (multi-selection; a single folder replaces the second pane) |
| `Ctrl+B` | — | Show or hide the sidebar |
| `Esc` | — | Cancel a pending move, else close the menu, else leave solo view |

Review mode is entered and left with the `R` chip only: `Ctrl+O` and
`Ctrl+Shift+O` do nothing there. `Esc` keeps its usual meaning.

## File drag and drop

| Input | Condition | Action |
|---|---|---|
| Drop image files on the main area | Not in review mode | Add them to the grid, up to the four-image cap; the sidebar keeps its current visibility |
| Drop one or two folders on the sidebar | Not in review mode, at least one pane free | Fill the free panes with those folders |
| Drop on the other region, or in review mode | — | Nothing |

The main area or the sidebar shows a 2 px accent frame while a valid drop
hovers over it. The frame is inset 1 px from the toolbar boundary and 2 px from
the other three edges.

## Grid

| Input | Condition | Action |
|---|---|---|
| Mouse wheel | Pointer over a cell, not in local mode | Zoom all images (1x to 128x) |
| Left drag | Zoom > 1x, not in local mode | Pan all images |
| Right drag | Same as above | Pan the cell under the pointer |
| `1` – `4` | Not in solo view | Rotate the image at that position 90° clockwise |
| `Q` (hold) | Exactly two images | Paint the right image over the left cell; release restores it |
| Double click | — | Show that image full size (solo view) |
| `x` button in a cell | Not in review mode | Remove that cell |
| move handle in a cell | Not in review mode, two or more cells | Drag onto another cell to swap the two |

## Diff mode

| Input | Condition | Action |
|---|---|---|
| `D` chip | Exactly two images | Enter or leave diff mode |
| Drag the threshold slider | Diff mode | Set the threshold from 0 to 20 (default 5) |
| Click the threshold track | Diff mode | Jump to that threshold and start dragging from it |

Diff mode leaves automatically when an add or remove operation leaves a count
other than two. Review pair changes and the sidebar's *Open* with exactly two
selected rows keep it on.

## Solo view

| Input | Action |
|---|---|
| `1` – `4` (hold) | Temporarily show the image at that position (digits do not rotate here) |
| Double click | Return to the grid |
| `Esc` | Return to the grid |
| Wheel / drags | Zoom and pan as in the grid |

## Review mode

| Input | Action |
|---|---|
| `Space` | Next pair |
| `B` | Previous pair |
| Double click a sidebar row | Open that pair |
| `P`, `Q`, `1` – `4`, double click, `Ctrl+B` | Still available |
| `D` / threshold slider | Still available when the pair has two images |
| `Ctrl+O`, `Ctrl+Shift+O` | Nothing; the `R` chip leaves review mode |
| Cell `x` and move handles | Not drawn |
| File and folder drops | Nothing |

Removing a cell, swapping cells, opening images and opening a folder are all
unavailable while review mode is on.

## Local mode

| Input | Action |
|---|---|
| Left drag | Draw the same relative selection on every image and recompute statistics |
| Left click | Select one pixel and compute its statistics |
| Right drag | Move the selection of the cell under the pointer only |
| Mouse wheel | Disabled while local mode is on |

## Sidebar

| Input | Action |
|---|---|
| Single click | Select one row |
| `Ctrl` + click | Toggle a row in the selection (both panes) |
| `Shift` + click | Select the range from the last clicked row to this row in the same pane |
| `Ctrl` + `Shift` + click | Add that range to the existing selection |
| Double click | Clear the grid and show that image (in review mode: show the pair) |
| Right click | Row menu: `Open`, `Add to main window`, `Show in Explorer`, `Open with...`, `Properties`. A row outside the selection becomes the only selected row. `Open` uses the whole selection. |
| Mouse wheel | Scroll that pane (both panes together in review mode) |
| Drag scrollbar | Scroll that pane |
| Drag the right divider | Resize the sidebar (160 px minimum, 320 px left for the grid) |

## Title bar

| Input | Action |
|---|---|
| Drag empty area | Move the window |
| Double click empty area | Maximise / restore |
| Drag window edge or corner | Resize |

EXIF (`E`), histogram (`H`), local mode (`P` inside the toolbar) and review
mode (`R`) are toggled with the toolbar chips only; `P` is the one chip that
also has a keyboard shortcut.
