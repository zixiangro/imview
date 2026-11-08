# Shortcuts and mouse bindings

## Global

| Input | Condition | Action |
|---|---|---|
| `Ctrl+O` | Not in review mode | Open the media picker (images and videos) |
| `Ctrl+Shift+O` | Not in review mode | Open the folder dialog (multi-selection; a single folder replaces the second pane) |
| `Ctrl+B` | — | Show or hide the sidebar |
| `Esc` | — | Cancel a pending move, close the menu, or leave solo view. It does not close a video. |

Review mode is entered and left with the `R` chip only: `Ctrl+O` and
`Ctrl+Shift+O` do nothing there. `Esc` keeps its usual meaning.

## File drag and drop

| Input | Condition | Action |
|---|---|---|
| Drop image or video files on the main area | Not in review mode | Add them to the grid up to the four-media cap |
| Drop one or two folders on the sidebar | Not in review mode, at least one pane free | Fill the free panes with those folders |
| Drop on the other region, or in review mode | — | Nothing |

The main area or the sidebar shows a 2 px accent frame while a valid drop
hovers over it. The frame is inset 1 px from the toolbar boundary and 2 px from
the other three edges.

## Grid

| Input | Condition | Action |
|---|---|---|
| Mouse wheel | Image-only grid, pointer over a cell, not in local mode | Zoom all images (1x to 128x) |
| Left drag | Image-only grid, zoom > 1x, not in local mode | Pan all images |
| Right drag | Same as above | Pan the cell under the pointer |
| `1` – `4` | Image-only grid, not in solo view | Rotate the image at that position 90° clockwise |
| `Q` (hold) | Image-only grid with exactly two images | Paint the right image over the left cell; release restores it |
| Double click | — | Show that image full size (solo view) |
| `x` button in a cell | Pointer inside the cell, not in review mode | Remove that cell |
| move handle in a cell | Pointer inside the cell, not in review mode, two or more cells | Drag onto another cell to swap the two |

## Video

Video cells use the same grid and count towards the four-media cap. Their
transport appears when the pointer enters the cell and hides when it leaves.

| Input | Action |
|---|---|
| Click on the picture | Pause or resume the active video; in sync mode, the whole group |
| `Space` | Pause or resume the active video; in review mode `Space` advances the pair instead |
| `←` / `→` | Seek five seconds back or forward |
| `Ctrl`+`←` / `Ctrl`+`→` | Step one frame back or forward and keep the video paused |
| Click/drag the seek bar | Seek to that position |
| Hover/drag over the seek bar | Show a thumbnail preview and its time |
| Left/right seek buttons | Seek five seconds back or forward |
| Play/pause button | Pause or resume |
| `V1` / `A1` / `S1` buttons | Cycle through video / audio / subtitle tracks; disabled with one track |
| `Vol` slider / `M` | Global volume and mute; new windows start at 50% |
| `CC` | Show or hide the selected text subtitle |
| Cell `x` / move handle | Remove the video cell / swap it with another cell |

Playback starts automatically but stops on the final frame instead of looping.
By default only one video plays; starting or adding another pauses the previous
one. Embedded and sidecar text subtitles are drawn with an outline; bitmap
subtitles are not rendered. `Esc` does not close a video.

While images and videos share the grid, zoom, pan, rotation, `Q`, diff, local
mode, EXIF, histogram, solo, review and sync are greyed out or inactive. Video
controls, track selection and the global volume remain available.

## Sync mode

The `S` chip is enabled only with more than one video and no images. Turning it
on pauses every video first. If one video ends, the group pauses; play resumes
the other videos while the finished one stays at its end. Backward step/seek
from that state works from the video's end.

| Input | Action |
|---|---|
| `S` chip | Enter or leave sync mode |
| Play/pause | Apply to every video together |
| `←` / `→` | Seek every video five seconds |
| `Ctrl`+`←` / `Ctrl`+`→` | Step every video by one frame |
| Seek-bar click/drag | Seek only the cell under the pointer (durations can differ) |
| Track buttons | Change the track in that cell only |
| `Vol` slider / `M` | Still global |

## Diff mode

| Input | Condition | Action |
|---|---|---|
| `D` chip | Two decoded images of equal pixel size | Enter or leave diff mode |
| Drag the threshold slider | Diff mode | Set the threshold from 0 to 20 (default 5) |
| Click the threshold track | Diff mode | Jump to that threshold and start dragging from it |

Diff mode leaves automatically when an add or remove operation leaves a count
other than two, when an image fails to decode, or when the two images have
different pixel sizes. Review pair changes and the sidebar's *Open* with exactly
two selected rows keep it on while the new pair loads.

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
| `Space` | Next pair (video controls remain available with the mouse) |
| `B` | Previous pair |
| Double click a sidebar row | Open that image pair; video rows are ignored |
| `P`, `Q`, `1` – `4`, double click, `Ctrl+B` | Still available |
| `D` / threshold slider | Still available when the pair has two images |
| `Ctrl+O`, `Ctrl+Shift+O` | Nothing; the `R` chip leaves review mode |
| Cell `x` and move handles | Not drawn |
| File and folder drops | Nothing |

Removing a cell, swapping cells, opening media and opening a folder are all
unavailable while review mode is on. Review mode uses images only and skips
positions with a video on either side; the `R` chip is disabled when no valid
image pair exists. Video cells never appear in review mode.

## Local mode

Local mode is available only while the grid contains images and no videos.

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
| Drag empty area | Move the window (with a video, an app-driven drag keeps rendering while the window follows the pointer) |
| Double click empty area | Maximise / restore |
| Drag window edge or corner | Resize |

EXIF (`E`), histogram (`H`), local mode (`P` inside the toolbar) and review
mode (`R`) are toggled with the toolbar chips only; `P` is the one chip that
also has a keyboard shortcut.
