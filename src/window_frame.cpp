#include "window_frame.h"

#include "eui_neo.h"
#include "utf8.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

// GLFW native access, used only to resolve the HWND behind the EUI window.
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

namespace imview::frame {

#if defined(_WIN32)
namespace {

HWND gWindow = nullptr;
WNDPROC gPreviousProc = nullptr;
bool gHookInstalled = false;

// Logical (96 dpi) geometry of the parts the app draws itself.
float gTitleBarHeight = 36.0f;
float gControlsWidth = 320.0f;
float gLeftControlsWidth = 0.0f;
float gSidebarSplitterX = -1.0f;

// Width of the splitter's grab band, matching kSidebarResizeHandleWidth.
constexpr float kSplitterWidth = 6.0f;

// Edge band that behaves as a resize border, in logical units.
constexpr float kResizeBorder = 5.0f;

// DWM attributes used to keep a custom borderless frame free of native
// rounding and border lines, which otherwise show as gaps around a maximised
// window.
enum : DWORD {
    kWindowCornerPreference = 33,
    kWindowBorderColor = 34,
    kVisibleFrameBorderThickness = 37,
};
enum : DWORD {
    kCornerDoNotRound = 1,
    kCornerRound = 2,
    kColorNone = 0xFFFFFFFE,
    kColorDefault = 0xFFFFFFFF,
};

void applyDwmFrameAppearance(HWND hwnd, bool maximized) {
    const DWORD corners = maximized ? kCornerDoNotRound : kCornerRound;
    DwmSetWindowAttribute(hwnd, kWindowCornerPreference, &corners, sizeof(corners));
    const DWORD borderColor = maximized ? kColorNone : kColorDefault;
    DwmSetWindowAttribute(hwnd, kWindowBorderColor, &borderColor, sizeof(borderColor));
    const DWORD borderThickness = 0;
    DwmSetWindowAttribute(hwnd, kVisibleFrameBorderThickness, &borderThickness,
                          sizeof(borderThickness));
}

// DWM still reveals the desktop at the rounded top corners of a window that is
// not in the native zoomed state. A full-window region makes the maximised
// frame rectangular; clearing it restores the rounded window.
void applyMaximizedRegion(HWND hwnd, bool maximized) {
    if (!maximized) {
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }
    RECT window{};
    if (!GetWindowRect(hwnd, &window)) return;
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    if (width <= 0 || height <= 0) return;
    HRGN region = CreateRectRgn(0, 0, width, height);
    if (region == nullptr) return;
    // On success the system owns the region and frees it with the window.
    if (SetWindowRgn(hwnd, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

// The window is never put into the native "zoomed" state: Windows then forces a
// maximized rect inflated by the (invisible) frame, and SetWindowPos is ignored
// for zoomed windows. Maximizing is done by resizing to the monitor work area.
bool gMaximized = false;
bool gTitleBarDragEnabled = true;
bool gCustomTitleBarDrag = false;
bool gCustomDragging = false;
RECT gRestoreRect{};
RECT gCustomDragStartRect{};
POINT gCustomDragStartCursor{};

// Private message: convert a system-initiated maximize into ours.
constexpr UINT kConvertMaximizeMessage = WM_APP + 0x51;

HWND nativeWindow() {
    if (gWindow != nullptr && IsWindow(gWindow)) {
        return gWindow;
    }
    auto* glfwWindow = static_cast<GLFWwindow*>(core::window::currentContextKey());
    if (glfwWindow == nullptr) {
        return nullptr;
    }
    gWindow = glfwGetWin32Window(glfwWindow);
    return gWindow;
}

float scaleFor(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

FileDropHandler gFileDropHandler;
HANDLE gStartupMutex = nullptr;

using OpenPathsHandler = std::function<bool(const std::vector<std::string>&)>;
OpenPathsHandler gOpenPathsHandler;

constexpr wchar_t kInstanceProperty[] = L"ImView.Instance.Window";
constexpr ULONG_PTR kOpenPathsCopyData = 0x494D5649; // 'IMVI'

std::vector<std::string> parseOpenPaths(const COPYDATASTRUCT& data) {
    std::vector<std::string> paths;
    if (data.lpData == nullptr || data.cbData < sizeof(wchar_t)) return paths;
    const auto* text = static_cast<const wchar_t*>(data.lpData);
    const std::size_t length = data.cbData / sizeof(wchar_t);
    std::size_t start = 0;
    while (start < length) {
        std::size_t end = start;
        while (end < length && text[end] != L'\n' && text[end] != L'\0') ++end;
        if (end > start) {
            std::wstring line(text + start, end - start);
            std::string path = imview::utf8::narrow(line.c_str());
            if (!path.empty()) paths.push_back(std::move(path));
        }
        start = end + 1;
    }
    return paths;
}

// OLE drop target, registered on the borderless window. It receives drag-over
// notifications that the GLFW/Win32 drop callback does not provide, so the
// application can highlight the destination before the user releases the items.
class FileDropTarget final : public IDropTarget {
public:
    explicit FileDropTarget(HWND window) : window_(window) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG references = InterlockedDecrement(&references_);
        if (references == 0) delete this;
        return static_cast<ULONG>(references);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point,
                                        DWORD* effect) override {
        paths_.clear();
        hasFiles_ = false;
        hasDirectories_ = false;
        readPaths(data, paths_);
        for (const std::string& path : paths_) {
            const std::wstring wide = imview::utf8::widen(path);
            const DWORD attributes = wide.empty()
                ? INVALID_FILE_ATTRIBUTES
                : GetFileAttributesW(wide.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                hasDirectories_ = true;
            } else {
                hasFiles_ = true;
            }
        }
        const bool accepted = emit(FileDropPhase::Enter, point);
        applyEffect(effect, accepted);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        const bool accepted = emit(FileDropPhase::Over, point);
        applyEffect(effect, accepted);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        FileDropEvent event;
        event.phase = FileDropPhase::Leave;
        event.hasFiles = hasFiles_;
        event.hasDirectories = hasDirectories_;
        if (gFileDropHandler) gFileDropHandler(event);
        reset();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL point,
                                   DWORD* effect) override {
        const bool accepted = emit(FileDropPhase::Drop, point);
        applyEffect(effect, accepted);
        reset();
        return S_OK;
    }

private:
    bool emit(FileDropPhase phase, POINTL screenPoint) {
        if (!gFileDropHandler) return false;
        POINT point{screenPoint.x, screenPoint.y};
        if (!ScreenToClient(window_, &point)) return false;
        const float scale = scaleFor(window_);
        FileDropEvent event;
        event.phase = phase;
        event.x = scale > 0.0f ? static_cast<float>(point.x) / scale
                               : static_cast<float>(point.x);
        event.y = scale > 0.0f ? static_cast<float>(point.y) / scale
                               : static_cast<float>(point.y);
        event.hasFiles = hasFiles_;
        event.hasDirectories = hasDirectories_;
        event.paths = paths_;
        return gFileDropHandler(event);
    }

    void reset() {
        paths_.clear();
        hasFiles_ = false;
        hasDirectories_ = false;
    }

    static void applyEffect(DWORD* effect, bool accepted) {
        if (effect != nullptr) {
            *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
    }

    // Reads CF_HDROP from the shell data object. The paths are returned as UTF-8
    // so the rest of the application can treat them like command-line paths.
    static void readPaths(IDataObject* data, std::vector<std::string>& paths) {
        if (data == nullptr) return;
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (FAILED(data->GetData(&format, &medium))) return;
        if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal != nullptr) {
            const auto drop = static_cast<HDROP>(medium.hGlobal);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            for (UINT index = 0; index < count; ++index) {
                const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                if (length == 0) continue;
                std::wstring wide(static_cast<std::size_t>(length) + 1, L'\0');
                if (DragQueryFileW(drop, index, wide.data(), length + 1) == 0) continue;
                wide.resize(length);
                std::string path = imview::utf8::narrow(wide.c_str());
                if (!path.empty()) paths.push_back(std::move(path));
            }
        }
        ReleaseStgMedium(&medium);
    }

    HWND window_ = nullptr;
    LONG references_ = 1;
    bool hasFiles_ = false;
    bool hasDirectories_ = false;
    std::vector<std::string> paths_;
};

FileDropTarget* gFileDropTarget = nullptr;
HWND gFileDropWindow = nullptr;
bool gOleInitialized = false;

void installFileDrop(HWND hwnd) {
    if (gFileDropTarget != nullptr) return;
    if (FAILED(OleInitialize(nullptr))) return;
    gOleInitialized = true;

    auto* target = new FileDropTarget(hwnd);
    if (FAILED(RegisterDragDrop(hwnd, target))) {
        target->Release();
        OleUninitialize();
        gOleInitialized = false;
        return;
    }
    gFileDropTarget = target;
    gFileDropWindow = hwnd;
}

void applyMaximized(bool maximize) {
    HWND hwnd = nativeWindow();
    if (hwnd == nullptr) {
        return;
    }
    if (maximize == gMaximized) {
        return;
    }
    if (maximize) {
        GetWindowRect(hwnd, &gRestoreRect);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            return;
        }
        gMaximized = true;
        SetWindowPos(hwnd, nullptr,
                     monitor.rcWork.left, monitor.rcWork.top,
                     monitor.rcWork.right - monitor.rcWork.left,
                     monitor.rcWork.bottom - monitor.rcWork.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        applyDwmFrameAppearance(hwnd, true);
        applyMaximizedRegion(hwnd, true);
    } else {
        gMaximized = false;
        applyMaximizedRegion(hwnd, false);
        SetWindowPos(hwnd, nullptr,
                     gRestoreRect.left, gRestoreRect.top,
                     gRestoreRect.right - gRestoreRect.left,
                     gRestoreRect.bottom - gRestoreRect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        applyDwmFrameAppearance(hwnd, false);
    }
}

bool customTitleBarHit(HWND hwnd, LPARAM lParam) {
    if (!gTitleBarDragEnabled || !gCustomTitleBarDrag) return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;
    const float scale = scaleFor(hwnd);
    const float x = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
    const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;
    const float width = static_cast<float>(client.right) / scale;
    const bool hit = y >= 0.0f && y < gTitleBarHeight &&
           x > gLeftControlsWidth && x < width - gControlsWidth;
    return hit;
}

LRESULT CALLBACK borderlessProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_LBUTTONDOWN:
            if (customTitleBarHit(hwnd, lParam)) {
                beginWindowMove();
                return 0;
            }
            break;
        case WM_NCLBUTTONDOWN:
            if (gCustomTitleBarDrag && wParam == HTCAPTION) {
                beginWindowMove();
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK:
            if (customTitleBarHit(hwnd, lParam)) {
                applyMaximized(!(gMaximized || IsZoomed(hwnd) != FALSE));
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (gCustomDragging) {
                dragWindowToCursor();
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (gCustomDragging) {
                endWindowMove();
                return 0;
            }
            break;
        case WM_COPYDATA: {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (data != nullptr && data->dwData == kOpenPathsCopyData) {
                if (!gOpenPathsHandler) return FALSE;
                const std::vector<std::string> paths = parseOpenPaths(*data);
                return gOpenPathsHandler(paths) ? TRUE : FALSE;
            }
            break;
        }
        case WM_NCCALCSIZE:
            // Claim the whole window as client area: the frame we added for the
            // native move/size loops must not be drawn.
            if (wParam == TRUE) {
                return 0;
            }
            break;

        case WM_NCHITTEST: {
            const LRESULT result =
                CallWindowProcW(gPreviousProc, hwnd, message, wParam, lParam);
            if (gCustomTitleBarDrag) {
                RECT window{};
                if (GetWindowRect(hwnd, &window)) {
                    const float scale = scaleFor(hwnd);
                    const int top = GET_Y_LPARAM(lParam) - window.top;
                    const int border = static_cast<int>(kResizeBorder * scale);
                    const int titleHeight =
                        static_cast<int>(gTitleBarHeight * scale);
                    if (top >= border && top < titleHeight) {
                        return HTCLIENT;
                    }
                }
            }
            // The previous proc may report HTCAPTION for the caption band even
            // though WM_NCCALCSIZE made it client area. When the app drives the
            // drag itself, turn that band into a normal client hit so EUI
            // receives press/drag/release while video frames keep composing.
            if (result != HTCLIENT &&
                !(gCustomTitleBarDrag && result == HTCAPTION)) {
                return result;
            }
            RECT window{};
            if (!GetWindowRect(hwnd, &window)) {
                return result;
            }
            const float scale = scaleFor(hwnd);
            const int border = static_cast<int>(kResizeBorder * scale);
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const bool maximized = gMaximized || IsZoomed(hwnd) != FALSE;
            const int left = x - window.left;
            const int top = y - window.top;
            const int right = window.right - x;
            const int bottom = window.bottom - y;

            if (!maximized) {
                const bool nearLeft = left < border;
                const bool nearRight = right < border;
                const bool nearTop = top < border;
                const bool nearBottom = bottom < border;
                if (nearTop && nearLeft) return HTTOPLEFT;
                if (nearTop && nearRight) return HTTOPRIGHT;
                if (nearBottom && nearLeft) return HTBOTTOMLEFT;
                if (nearBottom && nearRight) return HTBOTTOMRIGHT;
                if (nearLeft) return HTLEFT;
                if (nearRight) return HTRIGHT;
                if (nearTop) return HTTOP;
                if (nearBottom) return HTBOTTOM;
            }

            // Title bar band: hand it to the window manager so it runs its own
            // move loop (drag, Aero Snap). The control clusters at both ends
            // keep receiving normal client clicks. Kept active while maximized so
            // double-click still toggles.
            if (gCustomTitleBarDrag && result == HTCAPTION) {
                return HTCLIENT;
            }
            const int titleHeight = static_cast<int>(gTitleBarHeight * scale);
            const int controlsStart =
                (window.right - window.left) - static_cast<int>(gControlsWidth * scale);
            const int leftControlsEnd = static_cast<int>(gLeftControlsWidth * scale);
            if (gTitleBarDragEnabled && !gCustomTitleBarDrag &&
                top < titleHeight && left > leftControlsEnd &&
                left < controlsStart) {
                return HTCAPTION;
            }
            return result;
        }

        case WM_NCLBUTTONDBLCLK: {
            // Double-clicking our title bar toggles our own maximize instead of
            // asking Windows for a zoomed window.
            if (SendMessageW(hwnd, WM_NCHITTEST, wParam, lParam) == HTCAPTION) {
                applyMaximized(!(gMaximized || IsZoomed(hwnd) != FALSE));
                return 0;
            }
            break;
        }

        case WM_SIZE:
            if (gMaximized) {
                // Keep the rectangular region in sync with the window bounds.
                applyMaximizedRegion(hwnd, true);
            }
            if (wParam == SIZE_MAXIMIZED && !gMaximized) {
                // Maximized by the system (Aero Snap / Win+Up): convert it once
                // the native maximize has settled.
                PostMessageW(hwnd, kConvertMaximizeMessage, 0, 0);
            }
            break;

        case kConvertMaximizeMessage:
            if (!gMaximized && IsZoomed(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
                applyMaximized(true);
            }
            return 0;

        case WM_ENTERSIZEMOVE:
            // Dragging a maximized title bar restores the window first, like
            // native windows do.
            if (gMaximized) {
                applyMaximized(false);
            }
            break;

        case WM_NCACTIVATE:
        case WM_NCPAINT:
            // GLFW suppresses frame painting for undecorated windows; keep that.
            return TRUE;

        case WM_SETCURSOR:
            // GLFW overwrites the cursor for every WM_SETCURSOR in the client
            // area; claim the splitter band first so it keeps the resize shape.
            if (LOWORD(lParam) == HTCLIENT && gSidebarSplitterX >= 0.0f) {
                POINT point{};
                if (GetCursorPos(&point) && ScreenToClient(hwnd, &point)) {
                    const float scale = scaleFor(hwnd);
                    const float x = static_cast<float>(point.x) / scale;
                    const float y = static_cast<float>(point.y) / scale;
                    if (y >= gTitleBarHeight && x >= gSidebarSplitterX &&
                        x < gSidebarSplitterX + kSplitterWidth) {
                        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                        return TRUE;
                    }
                }
            }
            break;

        default:
            break;
    }
    return CallWindowProcW(gPreviousProc, hwnd, message, wParam, lParam);
}

} // namespace
#endif

void setCustomTitleBarDrag(bool custom) {
    gCustomTitleBarDrag = custom;
    if (!custom && gCustomDragging) endWindowMove();
}

void beginWindowMove() {
#if defined(_WIN32)
    HWND hwnd = nativeWindow();
    if (hwnd == nullptr) return;
    if (gMaximized || IsZoomed(hwnd) != FALSE) {
        applyMaximized(false);
    }
    GetWindowRect(hwnd, &gCustomDragStartRect);
    GetCursorPos(&gCustomDragStartCursor);
    gCustomDragging = true;
    SetCapture(hwnd);
#endif
}

void dragWindowToCursor() {
#if defined(_WIN32)
    if (!gCustomDragging) return;
    HWND hwnd = nativeWindow();
    if (hwnd == nullptr) return;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const int dx = cursor.x - gCustomDragStartCursor.x;
    const int dy = cursor.y - gCustomDragStartCursor.y;
    SetWindowPos(hwnd, nullptr,
                 gCustomDragStartRect.left + dx,
                 gCustomDragStartRect.top + dy,
                 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}

void endWindowMove() {
#if defined(_WIN32)
    if (gCustomDragging) {
        ReleaseCapture();
        gCustomDragging = false;
    }
#endif
}

void installBorderlessWindow(float titleBarLogicalHeight,
                             float leftControlsWidth,
                             float rightControlsWidth) {
    gTitleBarHeight = titleBarLogicalHeight > 0.0f ? titleBarLogicalHeight : 36.0f;
    gLeftControlsWidth = std::max(0.0f, leftControlsWidth);
    gControlsWidth = rightControlsWidth > 0.0f ? rightControlsWidth : 320.0f;
#if defined(_WIN32)
    HWND hwnd = nativeWindow();
    if (hwnd == nullptr) return;
    SetPropW(hwnd, kInstanceProperty, reinterpret_cast<HANDLE>(1));

    if (!gHookInstalled) {
        gPreviousProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(borderlessProc)));
        gHookInstalled = gPreviousProc != nullptr;
    }

    // Restore the frame bits that make Windows run its native move/size loops
    // and let DWM draw the shadow; the frame itself stays invisible because of
    // the WM_NCCALCSIZE handler above.
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    installFileDrop(hwnd);

    // Rounded corners on Windows 11, square while maximised, and no native
    // window border line around the custom frame.
    applyDwmFrameAppearance(hwnd, false);
#endif
}

void setTitleBarLeftControlsWidth(float width) {
#if defined(_WIN32)
    gLeftControlsWidth = std::max(0.0f, width);
#else
    (void)width;
#endif
}

void minimizeWindow() {
#if defined(_WIN32)
    if (HWND hwnd = nativeWindow()) ShowWindow(hwnd, SW_MINIMIZE);
#endif
}

void centerWindow() {
#if defined(_WIN32)
    HWND hwnd = nativeWindow();
    if (hwnd == nullptr) return;
    RECT window{};
    if (!GetWindowRect(hwnd, &window)) return;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return;
    }
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    const int x = monitor.rcWork.left +
                  (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    const int y = monitor.rcWork.top +
                  (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}

void toggleMaximizeWindow() {
#if defined(_WIN32)
    applyMaximized(!(gMaximized || IsZoomed(nativeWindow()) != FALSE));
#endif
}

void closeWindow() {
#if defined(_WIN32)
    if (HWND hwnd = nativeWindow()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
#endif
}

bool isMaximized() {
#if defined(_WIN32)
    HWND hwnd = nativeWindow();
    return hwnd != nullptr && (gMaximized || IsZoomed(hwnd) != FALSE);
#else
    return false;
#endif
}

float dpiScale() {
    HWND hwnd = nativeWindow();
    return hwnd != nullptr ? scaleFor(hwnd) : 1.0f;
}

void setTitleBarDragEnabled(bool enabled) {
    gTitleBarDragEnabled = enabled;
}

void setSidebarSplitter(float logicalX) {
#if defined(_WIN32)
    gSidebarSplitterX = logicalX;
#else
    (void)logicalX;
#endif
}

void setFileDropHandler(FileDropHandler handler) {
#if defined(_WIN32)
    gFileDropHandler = std::move(handler);
#else
    (void)handler;
#endif
}

void setOpenPathsHandler(std::function<bool(const std::vector<std::string>&)> handler) {
#if defined(_WIN32)
    gOpenPathsHandler = std::move(handler);
#else
    (void)handler;
#endif
}

bool forwardOpenPaths(const std::vector<std::string>& paths) {
#if defined(_WIN32)
    if (paths.empty()) return false;
    std::wstring payload;
    for (const std::string& path : paths) {
        const std::wstring wide = imview::utf8::widen(path);
        if (wide.empty()) continue;
        if (!payload.empty()) payload.push_back(L'\n');
        payload += wide;
    }
    if (payload.empty()) return false;

    struct Context {
        const std::wstring* payload = nullptr;
        bool forwarded = false;
    };
    Context context{&payload, false};
    EnumWindows([](HWND hwnd, LPARAM opaque) -> BOOL {
        auto* context = reinterpret_cast<Context*>(opaque);
        if (GetPropW(hwnd, kInstanceProperty) == nullptr) return TRUE;
        DWORD_PTR result = 0;
        COPYDATASTRUCT message{};
        message.dwData = kOpenPathsCopyData;
        message.cbData = static_cast<DWORD>(context->payload->size() * sizeof(wchar_t));
        message.lpData = const_cast<wchar_t*>(context->payload->data());
        const LRESULT sent = SendMessageTimeoutW(
            hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&message),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
        if (sent != 0 && result != 0) {
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            context->forwarded = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.forwarded;
#else
    (void)paths;
    return false;
#endif
}

void shutdownWindowMessaging() {
#if defined(_WIN32)
    gOpenPathsHandler = nullptr;
    if (HWND hwnd = nativeWindow()) {
        RemovePropW(hwnd, kInstanceProperty);
    }
#endif
}

void lockInstanceStartup() {
#if defined(_WIN32)
    if (gStartupMutex != nullptr) return;
    gStartupMutex = CreateMutexW(nullptr, FALSE, L"Local\\ImView.Instance.Startup");
    if (gStartupMutex != nullptr) {
        WaitForSingleObject(gStartupMutex, 3000);
    }
#endif
}

void unlockInstanceStartup() {
#if defined(_WIN32)
    if (gStartupMutex == nullptr) return;
    ReleaseMutex(gStartupMutex);
    CloseHandle(gStartupMutex);
    gStartupMutex = nullptr;
#endif
}

void* nativeWindowHandle() {
#if defined(_WIN32)
    return nativeWindow();
#else
    return nullptr;
#endif
}

void activateWindow() {
#if defined(_WIN32)
    HWND hwnd = nativeWindow();
    if (hwnd == nullptr) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
#endif
}

bool cursorLogicalPosition(float& x, float& y) {
#if defined(_WIN32)
    HWND hwnd = nativeWindow();
    if (hwnd == nullptr) return false;
    POINT point{};
    if (!GetCursorPos(&point)) return false;
    if (!ScreenToClient(hwnd, &point)) return false;
    const float scale = scaleFor(hwnd);
    x = scale > 0.0f ? static_cast<float>(point.x) / scale : static_cast<float>(point.x);
    y = scale > 0.0f ? static_cast<float>(point.y) / scale : static_cast<float>(point.y);
    return true;
#else
    (void)x;
    (void)y;
    return false;
#endif
}

void shutdownFileDrop() {
#if defined(_WIN32)
    gFileDropHandler = nullptr;
    if (gFileDropTarget != nullptr) {
        if (gFileDropWindow != nullptr) RevokeDragDrop(gFileDropWindow);
        gFileDropTarget->Release();
        gFileDropTarget = nullptr;
        gFileDropWindow = nullptr;
    }
    if (gOleInitialized) {
        OleUninitialize();
        gOleInitialized = false;
    }
#endif
}

} // namespace imview::frame
