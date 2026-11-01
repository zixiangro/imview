# Shortcuts and mouse bindings

## Global

| Input | Condition | Action |
|---|---|---|
| `Ctrl+O` | Not in review mode | Open the images dialog |
| `Ctrl+Shift+O` | Not in review mode | Open the folder dialog (replaces the second pane) |
| `Ctrl+B` | — | Show or hide the sidebar |
| `Esc` | — | Cancel a pending move, else close the menu, else leave solo view |

Review mode is entered and left with the `R` chip only: `Ctrl+O` and
`Ctrl+Shift+O` do nothing there. `Esc` keeps its usual meaning.

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
| `Ctrl+O`, `Ctrl+Shift+O` | Nothing; the `R` chip leaves review mode |
| Cell `x` and move handles | Not drawn |

Removing a cell, swapping cells, opening images and opening a folder are all
unavailable while review mode is on.

## Local mode

| Input | Action |
|---|---|
| Left drag | Draw the same relative selection on every image and recompute statistics |
| Right drag | Move the selection of the cell under the pointer only |
| Mouse wheel | Disabled while local mode is on |

## Sidebar

| Input | Action |
|---|---|
| Single click | Select one row |
| `Ctrl` + click | Toggle a row in the selection (both panes) |
| Double click | Clear the grid and show that image (in review mode: show the pair) |
| Right click | Row menu |
| Mouse wheel | Scroll that pane (both panes together in review mode) |
| Drag scrollbar | Scroll that pane |

## Title bar

| Input | Action |
|---|---|
| Drag empty area | Move the window |
| Double click empty area | Maximise / restore |
| Drag window edge or corner | Resize |

EXIF (`E`), histogram (`H`), local mode (`P` inside the toolbar) and review
mode (`R`) are toggled with the toolbar chips only; `P` is the one chip that
also has a keyboard shortcut.
