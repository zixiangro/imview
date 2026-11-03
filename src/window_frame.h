#pragma once

namespace imview::frame {

// Borderless-window support (Windows).
//
// The window is created with decorated(false), which on Win32 means a plain
// WS_POPUP window: no title bar *and* no resize frame. Windows also refuses to
// run its own move/size loops for such a window, so dragging and edge-resizing
// have to be restored explicitly.
//
// installBorderlessWindow() re-adds WS_THICKFRAME | WS_CAPTION to the window
// style (which is what makes Windows hand back its native move/size loops,
// Aero Snap, double-click-to-maximize and the DWM drop shadow) and hides the
// resulting frame by answering WM_NCCALCSIZE with a zero-height frame.
//
// Window buttons call into the same Win32 operations.

// titleBarLogicalHeight: height of the app-drawn title bar, in logical units.
// leftControlsWidth / rightControlsWidth: widths of the interactive clusters at
// the two ends of the title bar; those areas stay clickable instead of dragging.
void installBorderlessWindow(float titleBarLogicalHeight,
                             float leftControlsWidth,
                             float rightControlsWidth);

void minimizeWindow();
void toggleMaximizeWindow();
void closeWindow();
bool isMaximized();

// Centres the window on the work area of the monitor it currently belongs to.
void centerWindow();

// Enables/disables the native title-bar drag band. While an in-app menu is
// open, dragging must be suppressed so clicks reach the UI and dismiss it.
void setTitleBarDragEnabled(bool enabled);

// Logical-to-pixel factor of the window (1.0 at 96 dpi). Pointer events inside
// the app arrive in physical pixels, so drag targeting divides by this.
float dpiScale();

} // namespace imview::frame
