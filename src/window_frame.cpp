#include "window_frame.h"

#include "eui_neo.h"

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

#pragma comment(lib, "dwmapi.lib")

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

// Edge band that behaves as a resize border, in logical units.
constexpr float kResizeBorder = 5.0f;

// The window is never put into the native "zoomed" state: Windows then forces a
// maximized rect inflated by the (invisible) frame, and SetWindowPos is ignored
// for zoomed windows. Maximizing is done by resizing to the monitor work area.
bool gMaximized = false;
bool gTitleBarDragEnabled = true;
RECT gRestoreRect{};

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
    } else {
        gMaximized = false;
        SetWindowPos(hwnd, nullptr,
                     gRestoreRect.left, gRestoreRect.top,
                     gRestoreRect.right - gRestoreRect.left,
                     gRestoreRect.bottom - gRestoreRect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK borderlessProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
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
            if (result != HTCLIENT) {
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
            const int titleHeight = static_cast<int>(gTitleBarHeight * scale);
            const int controlsStart =
                (window.right - window.left) - static_cast<int>(gControlsWidth * scale);
            const int leftControlsEnd = static_cast<int>(gLeftControlsWidth * scale);
            if (gTitleBarDragEnabled && top < titleHeight && left > leftControlsEnd &&
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

        default:
            break;
    }
    return CallWindowProcW(gPreviousProc, hwnd, message, wParam, lParam);
}

} // namespace
#endif

void installBorderlessWindow(float titleBarLogicalHeight,
                             float leftControlsWidth,
                             float rightControlsWidth) {
    gTitleBarHeight = titleBarLogicalHeight > 0.0f ? titleBarLogicalHeight : 36.0f;
    gLeftControlsWidth = std::max(0.0f, leftControlsWidth);
    gControlsWidth = rightControlsWidth > 0.0f ? rightControlsWidth : 320.0f;
#if defined(_WIN32)
    HWND hwnd = nativeWindow();
    if (hwnd == nullptr) return;

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

    // Rounded corners on Windows 11.
    enum : DWORD { kCornerPreference = 33 };
    enum : DWORD { kCornerRound = 2 };
    const DWORD preference = kCornerRound;
    DwmSetWindowAttribute(hwnd, kCornerPreference, &preference, sizeof(preference));
#endif
}

void minimizeWindow() {
#if defined(_WIN32)
    if (HWND hwnd = nativeWindow()) ShowWindow(hwnd, SW_MINIMIZE);
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

} // namespace imview::frame
