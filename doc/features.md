# Features

## Images

- Opened from the `ImView` menu, from the command line (file paths), or by
  dropping files from Explorer.
- Images and videos share the four-media cap; extra paths are ignored.
- Supported formats: `.png` `.jpg` `.jpeg` `.bmp` `.gif`.
- Files are decoded on a worker thread; a cell shows `Loading...` until ready and
  `Failed to load` when decoding fails.
- Duplicate paths are ignored.

## Video

- Video files are ordinary grid cells and share the four-media cap with images.
  A cell plays on its own; there is no cross-cell video comparison yet.
- Video support is a build-time option (`IMVIEW_ENABLE_VIDEO`). An image-only
  build omits FFmpeg/miniaudio, video extensions and video integration cases;
  the rest of this section applies to the default video-enabled build.
- Supported extensions: `.mp4` `.m4v` `.mov` `.3gp` `.3g2` `.mkv` `.webm`
  `.avi` `.flv` `.f4v` `.wmv` `.asf` `.ts` `.mts` `.m2ts` `.mpg` `.mpeg`
  `.mpe` `.vob` `.ogv` `.rm` `.rmvb` `.divx`.
- The linked FFmpeg build decodes H.264, H.265/HEVC, VP8, VP9, AV1, MPEG-1/2/4,
  MS MPEG-4, WMV1/2/3, VC-1, Flash Video, MJPEG, H.263, ProRes, Theora, VP6,
  RealVideo 3/4 and DV video. It also decodes AAC, MP3/MP2, AC-3/E-AC-3, DTS,
  TrueHD, Opus, Vorbis, FLAC, ALAC, WMA, PCM and common 3GP/FLV audio.
- Frames are fitted with `Contain`; the uncovered area is black. A new video
  starts automatically, but only one video plays by default. Opening or
  starting another video pauses the one that was playing, and a video pauses
  automatically on its final frame instead of looping.
- The cell is the hot zone for its transport. Moving the pointer into the cell
  shows the controls immediately, with no animation; moving out hides them:
  play/pause, left/right seek buttons, a seek bar, position/duration,
  video/audio/subtitle track buttons and a `CC` toggle.
- Click the picture or press `Space` to pause/resume the active video. `←` and
  `→` seek five seconds; `Ctrl`+`←`/`→` step one frame and keep the video
  paused. Clicking or dragging the seek bar seeks to that time.
- Hovering or dragging over the seek bar shows a thumbnail preview and its time.
  The preview uses a separate decoder thread that opens lazily, decodes one
  low-resolution frame per second bucket, keeps a small cache of 64 thumbnails
  and stops when the cell closes.
- Embedded text subtitle streams (ASS/SSA, SubRip, WebVTT, MOV text, MicroDVD,
  SAMI, RealText, SubViewer and plain text) are decoded and drawn as white text
  with a black outline, without a background plate. Bitmap subtitles (PGS,
  DVD/DVB) are not rendered.
- Sidecar subtitles are loaded for the common text formats `.srt`, `.ass`,
  `.ssa`, `.vtt`, `.smi`, `.sub` and `.sbv`. A sidecar must share the video
  basename or start with it plus `.`, `-` or `_`, so `movie.srt`,
  `movie.zh.srt` and `movie-en.ass` all work for `movie.mkv`. UTF-8 and UTF-16
  files are read directly; legacy ANSI text is converted through the Windows
  ANSI code page. Bitmap sidecars such as VobSub are ignored.
- The `V`, `A` and `S` buttons show the selected track (`V1`, `A2`, `S1`).
  Clicking cycles through every available video, audio or subtitle track;
  buttons with a single track are shown but disabled. Embedded and sidecar
  subtitles share the same list. The `CC` button independently shows or hides
  subtitles.
- Audio and video are separate decoders. Audio is resampled to stereo float at
  48 kHz, buffered and played through the default device. The window has one
  global volume slider and mute button in the title bar: every video follows
  it, including in a mixed image/video grid, and a new window starts at 50%.
- The close and move buttons are the same as image cells and auto-hide with the
  same cell hot zone. Videos cannot be rotated.
- While images and videos share the grid, all image-only features are disabled:
  zoom, pan, rotation, solo view, `Q`, diff, local statistics, EXIF, histogram
  and entering review mode. The related toolbar chips remain visible but
  greyed out. Removing the videos restores the image features and any toggle
  state that was already on. The global volume control stays available.
- Opening a folder or dropping one in the sidebar keeps videos and images
  together; the sidebar lists both and marks their type with an icon.

## Mixed media

- When the grid contains only images, every image feature is available.
- When it contains only videos, video controls, audio, subtitles and preview
  are available; image-only chips are greyed out.
- When both types are present, the image-only features are disabled until only
  one type remains. Close/move, sidebar browsing, playback and video transport
  stay available.

## Sync mode

- The `S` chip is available only with **more than one video** and no images.
  It is greyed out for a single video or any mixed image/video grid.
- Turning sync on pauses every video first. While it is on, play/pause, the
  five-second seeks and frame stepping are sent to every video in the grid.
  The seek bar stays **per video**, because clips can have different durations;
  clicking it only seeks the cell under the pointer.
- The mode starts with the whole group paused. If one video reaches its end,
  the group pauses instead of leaving the other videos running. The finished
  video stays on its final frame: resuming, stepping forward or seeking forward
  does not restart it from zero. Stepping backward or seeking backward from
  that state works from the end of that video.
- Turning sync off pauses all but the active video. Outside sync mode ImView
  always keeps at most one video playing; sync is the only way to play several
  videos together.
- Track selection (`V`/`A`/`S`) stays local to the cell being changed. The
  global volume applies to all videos regardless of sync mode.

## Explorer integration

Nothing is registered at startup. The About dialog exposes three independent
switches; each can be turned on or off at any time.

| Option | Effect |
|---|---|
| Images | Adds the image extensions as a Default Apps candidate under `HKCU` |
| Videos | Adds the video extensions as a Default Apps candidate under `HKCU` |
| Right-click menu | Extracts the embedded helper under `%LOCALAPPDATA%\ImView\ShellExt` and registers it for images, videos and folders; disabling removes it again |

- The media options never write an explicit `UserChoice`; the user remains in
  control of the default application.
- The right-click handler is registered under the image and video
  `SystemFileAssociations` keys and `Directory`, not under the all-files `*`
  key. Explorer therefore loads it only for the media and folder selections it
  handles.
- `imview.exe --uninstall-integration` removes the ImView registration, extracted
  helper and legacy ImView keys without opening a window or changing the user's
  explicit default apps. Windows keeps an explicit `UserChoice`, so a file type
  that was set to ImView must be pointed at another app in Settings afterwards.
- The right-click entry `Open with ImView in new window` appears only for a
  selection of one folder, two to four images, two to four videos, or one to
  two folders. Mixed selections and larger selections do not show it.
- The right-click action always starts a new ImView window: a single folder or
  up to two folders open in the sidebar; two to four images or videos fill the
  grid. Existing windows are not reused.
- Opening a file through the normal default app / double-click forwards it to
  an existing window that has room. The window is restored and brought to the
  front; the file is added rather than replacing the grid.
- If every existing window is full, or no window exists, a new window starts
  with the file and opens that file's folder in the sidebar.
- Concurrent launches are serialized briefly so two Explorer double-clicks do
  not both decide to create a window.
- On Windows 11 the legacy right-click entry appears under *Show more options*.

## Grid

- One to four media items are laid out in a grid: one column for a single cell,
  two columns otherwise.
- Every cell has the same size; rows are horizontally centred.
- Images are scaled to fit their cell without cropping or distortion (1x zoom =
  fully visible). Videos use `Contain` and letterbox with black. The remaining
  space is left empty.

## Drag and drop

- Image and video files dropped on the main area are added to the grid:
  existing cells stay, duplicates are ignored and the four-media cap applies.
  The sidebar keeps its current visibility.
- Folders dropped on the sidebar fill free panes. One or two folders can be
  dropped at once; with both panes occupied the sidebar is not a drop target.
- A drop that contains both files and folders is filtered by destination: the
  main area takes the image and video files and the sidebar takes the folders.
- While a valid drop is hovering, a 2 px accent frame marks the main area or the
  sidebar. Its top edge is 1 px inside the toolbar boundary; the left, right and
  bottom edges are 2 px inside the target.
- Dropping anywhere else, and any drop in review mode, does nothing.

## Zoom and pan

Zoom and pan are available only while the grid contains images and no videos.

- The mouse wheel over any cell zooms **all** images by the same factor.
- Zoom is clamped to the range 1x (fully fitted) to 128x.
- Above 1x, images are sampled with nearest-neighbour filtering, so magnified
  source pixels keep their exact colour instead of being blended.
- Left-drag pans all images; right-drag pans only the cell under the pointer.
- Panning is clamped: an image never shows empty space inside its cell.
- Combined panning stops at the limit of each image independently, so pushing
  past the edge is discarded rather than accumulated.

## Per-cell operations

- `1`–`4` rotate the image at that display position clockwise by 90° (images
  only).
- Each cell carries two buttons in its top-right corner; they appear only while
  the pointer is inside the cell:

| Button | Behaviour |
|---|---|
| `✕` | Removes that cell |
| four-way arrows | Drag handle: press it, drag onto another cell, release to swap the two |

- Both buttons are hidden in review mode and in solo view; the move handle also
  needs at least two cells.
- Rotating, removing and swapping only affect this session, never the files.

## Solo view

- Double-clicking an image cell shows that image full size; double-click again
  (or `Esc`) returns to the grid. Solo is disabled while a video is present.
- While solo, holding a digit `1`–`4` temporarily shows the image at that
  position and releasing returns to the solo image.
- The indicator in the title bar shows `<position> / <count>`.

## Comparing two images

- With exactly two images, holding `Q` paints the right image over the left
  cell; releasing restores the left image. Display order is never changed.

## Diff mode

- The `D` chip is enabled with exactly two decoded images whose pixel
  dimensions match; it is disabled while a video is in the grid. It adds a
  third tile below the pair; the tile is not part of the image grid, so it
  neither counts towards the four-media cap nor changes the review-mode rules.
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
  first and then shows all four media cells normally.
- Review pair switching keeps diff mode while the new pair loads, then exits if
  the pair cannot produce a diff. The sidebar's *Open* with exactly two
  selected rows behaves the same way.
- The diff tile follows the shared zoom and pan, but has no close/move buttons,
  no rotation, no local-mode statistics, no EXIF overlay and no histogram.

## Local mode (region statistics)

- `P` toggles local mode; it is disabled while a video is in the grid.
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

- Toolbar chip `E` is enabled only for image-only grids and shows, at each
  image's top-left corner, one black strip per
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
- The Open dialog hides the sidebar for images; video openings,
  sidebar-driven opens and Explorer forwards preserve it. Opening a folder
  shows it again.
- Only the first level of a folder is listed, images and videos only, sorted by
  file name (case-insensitive). Image rows show a framed picture icon; video
  rows show a play triangle.
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

Entered with the toolbar `R` chip; requires exactly two folders with at least
one valid image pair.

- Review mode reviews images only. It starts at the first pair whose existing
  entries are images, skipping file positions that contain a video on either
  side. If no such pair exists, the `R` chip is greyed out.
- Pairs are indexed by the original sidebar row: the *n*-th row of folder 1
  with the *n*-th row of folder 2, but a pair is skipped when either row is a
  video. When one folder is shorter, a remaining image row opens as a single
  image.
- `Space` advances to the next valid image pair, `B` to the previous one; video
  positions are skipped. The sidebar scrolls so both panes stay aligned by row
  index.
- The indicator shows the position among valid image pairs, `Review <n> / <total>`.
- The sidebar only accepts double-clicks: single click, selection, the context
  menu and the pane header buttons are disabled. Double-clicking a video row
  does not open it in review mode.
- File drag and drop is disabled.
- Opening media, opening a folder, removing cells and swapping cells are
  disabled: the cell buttons are not drawn and `Ctrl+O` and `Ctrl+Shift+O` do
  nothing. Zooming, panning, rotating, solo view, local mode, `Q` swapping, EXIF
  and the histogram remain available.
- Leaving review mode keeps the current images and re-enables the operations
  above. Review mode is entered and left only through the `R` chip.

## Title bar

- The window is borderless; the title bar is drawn by the application.
- Dragging the empty part moves the window. Image-only windows use the native
  move loop; with a video the app drives the drag itself so decoded frames keep
  composing while the window moves. Double-click still maximises/restores.
- Left: the `ImView` menu, followed by the global volume group (video) or diff
  slider. Centre: a status indicator. Right: mode chips and window buttons. All
  six chips are always drawn; image-only chips are greyed while a video is
  present, and `S` is available only with multiple videos and no images.
- The `ImView` menu holds `Open...`, `Open folder...`, `Toggle sidebar` and
  `About`.
- `About` opens a centred dialog with the version, a short description, the
  project homepage link, three Explorer integration switches, and the licence.
  It has `Close` and `Open project page` buttons. The right-click switch is
  disabled only if the build contains no embedded helper.
- The window can be resized from its edges and corners.
