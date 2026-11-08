#pragma once

#include <functional>
#include <string>
#include <vector>

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

// Updates only the left interactive cluster: the toolbar grows a slider in diff
// mode, and that area must stop acting as a native drag band while it is shown.
void setTitleBarLeftControlsWidth(float width);

void minimizeWindow();
void toggleMaximizeWindow();
void closeWindow();
bool isMaximized();

// Centres the window on the work area of the monitor it currently belongs to.
void centerWindow();

// Enables/disables the native title-bar drag band. While an in-app menu is
// open, dragging must be suppressed so clicks reach the UI and dismiss it.
void setTitleBarDragEnabled(bool enabled);

// When a video is playing, the native Windows move loop would freeze the
// rendered frame while the decoder keeps running. Use an app-driven drag in
// that case: the toolbar drag surface calls begin/drag/end and the UI loop keeps
// compositing frames. Native dragging stays in use for image-only windows.
void setCustomTitleBarDrag(bool custom);
void beginWindowMove();
void dragWindowToCursor();
void endWindowMove();

// File drag-and-drop. EUI-NEO has no drop API, so the application installs an
// OLE drop target on its own window and receives the phases here. Coordinates
// are logical client units; paths arrive in UTF-8. The handler returns true only
// when the current position is a valid drop target, which also selects the
// copy/no-drop cursor shown by the shell.
enum class FileDropPhase { Enter, Over, Leave, Drop };

struct FileDropEvent {
    FileDropPhase phase = FileDropPhase::Enter;
    float x = 0.0f;
    float y = 0.0f;
    bool hasFiles = false;
    bool hasDirectories = false;
    std::vector<std::string> paths;
};

using FileDropHandler = std::function<bool(const FileDropEvent&)>;

void setFileDropHandler(FileDropHandler handler);
void shutdownFileDrop();

// Single-instance support. Windows opened by ImView carry a window property;
// a second process forwards its media paths through WM_COPYDATA and the handler
// returns true only when this window has room for them.
void setOpenPathsHandler(std::function<bool(const std::vector<std::string>&)> handler);
bool forwardOpenPaths(const std::vector<std::string>& paths);
void shutdownWindowMessaging();

// Serializes startup just long enough for the first window to register its
// instance property before another process decides to forward or create a new
// window.
void lockInstanceStartup();
void unlockInstanceStartup();

// The native window handle (HWND on Windows), or nullptr before creation.
void* nativeWindowHandle();
void activateWindow();
bool cursorLogicalPosition(float& x, float& y);

// Logical x coordinate of the sidebar splitter's grab band. While the pointer
// is over that band the window shows the horizontal-resize cursor. Pass a
// negative value when the splitter is unavailable.
void setSidebarSplitter(float logicalX);

// Logical-to-pixel factor of the window (1.0 at 96 dpi). Pointer events inside
// the app arrive in physical pixels, so drag targeting divides by this.
float dpiScale();

} // namespace imview::frame
