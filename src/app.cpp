// ImView - minimal dark image and video viewer built on EUI-NEO.
// Images use the 2x2 grid; video uses a separate plain playback view.

#include "eui_neo.h"
#include "eui/async.h"

#include "exif.h"
#include "media_paths.h"
#include "shell_actions.h"
#include "stb_wrap.h"
#include "utf8.h"
#include "video_player.h"
#include "window_frame.h"

#ifndef IMVIEW_ENABLE_VIDEO
#define IMVIEW_ENABLE_VIDEO 1
#endif

#include "core/render/text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(EUI_RENDER_BACKEND_OPENGL)
#include <glad/glad.h>
#endif

#ifndef IMVIEW_VERSION
#define IMVIEW_VERSION "0.2.0"
#endif
#ifndef IMVIEW_HOMEPAGE
#define IMVIEW_HOMEPAGE "https://github.com/zixiangro/imview"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace app {
namespace {

using imview::media::isImagePath;
using imview::media::isVideoPath;

constexpr int kMaxImages = 4;

// --- palette (VS Code "Dark Modern") ----------------------------------------

// Surfaces
const eui::Color kBackground{0.122f, 0.122f, 0.122f, 1.0f};      // #1F1F1F editor
const eui::Color kPanel{0.129f, 0.129f, 0.129f, 1.0f};           // #212121 cell surface
const eui::Color kTitleBar{0.094f, 0.094f, 0.094f, 1.0f};        // #181818 title bar
const eui::Color kBorder{0.169f, 0.169f, 0.169f, 1.0f};          // #2B2B2B
const eui::Color kBorderStrong{0.235f, 0.235f, 0.235f, 1.0f};    // #3C3C3C hover border

// Foreground
const eui::Color kTextPrimary{0.800f, 0.800f, 0.800f, 1.0f};     // #CCCCCC
const eui::Color kTextSecondary{0.616f, 0.616f, 0.616f, 1.0f};   // #9D9D9D

// Accent (VS Code button / focus border)
const eui::Color kAccent{0.0f, 0.471f, 0.831f, 1.0f};            // #0078D4
const eui::Color kAccentHover{0.008f, 0.431f, 0.757f, 1.0f};     // #026EC1
const eui::Color kAccentPressed{0.0f, 0.376f, 0.663f, 1.0f};     // #0060A9

const eui::Color kError{0.898f, 0.282f, 0.302f, 1.0f};
const eui::Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
const eui::Color kSelectionDrag{0.0f, 0.706f, 1.0f, 1.0f};  // #00B4FF
const eui::Color kSelectionFixed{1.0f, 0.314f, 0.314f, 1.0f}; // #FF5050

// Toolbar controls
const eui::Color kToolbarBackground = kTitleBar;
const eui::Color kToolbarLine = kBorder;
const eui::Color kChipIdle{0.0f, 0.0f, 0.0f, 0.0f};             // transparent
const eui::Color kChipIdleHover{1.0f, 1.0f, 1.0f, 0.08f};       // #FFFFFF14
const eui::Color kChipPressed{1.0f, 1.0f, 1.0f, 0.14f};
const eui::Color kWindowControlHover{1.0f, 1.0f, 1.0f, 0.08f};
const eui::Color kWindowCloseHover{0.769f, 0.169f, 0.110f, 1.0f}; // #C42B1C

constexpr float kToolbarHeight = 36.0f;
constexpr float kRadius = 4.0f;
constexpr float kCellBorderWidth = 1.0f;

constexpr const char* kVersion = IMVIEW_VERSION;
constexpr const char* kProjectUrl = IMVIEW_HOMEPAGE;

// Sidebar geometry
constexpr float kSidebarWidth = 240.0f;
constexpr float kSidebarMinWidth = 160.0f;
constexpr float kMainMinWidth = 320.0f;
constexpr float kSidebarResizeHandleWidth = 6.0f;
constexpr float kSidebarRowHeight = 22.0f;
constexpr float kSidebarHeaderHeight = 30.0f;

// --- state ------------------------------------------------------------------

#if defined(EUI_RENDER_BACKEND_OPENGL)
// Owns a texture uploaded by the application. A shared sampler, installed
// below, switches external-image filtering with the zoom state.
bool gGpuResourcesAlive = true;

struct GpuSource {
    eui::GpuDeviceInfo device;
    unsigned int texture = 0;

    ~GpuSource() {
        if (texture != 0 && gGpuResourcesAlive) {
            glDeleteTextures(1, &texture);
        }
    }
};

// EUI-NEO binds a linear sampler for external GPU images. ImView owns the
// image texture, so it substitutes one shared sampler for those non-zero
// bindings; unbind (0) calls pass through unchanged.
PFNGLBINDSAMPLERPROC g_realBindSampler = nullptr;
GLuint g_imageSampler = 0;
bool g_imageSamplerNearest = false;

void GLAPIENTRY bindImageSampler(GLuint unit, GLuint sampler) {
    if (g_realBindSampler == nullptr) return;
    const GLuint bound = sampler != 0 && g_imageSampler != 0 ? g_imageSampler : sampler;
    g_realBindSampler(unit, bound);
}

void setImageSamplerFilter(bool nearest) {
    if (g_imageSampler == 0 || g_imageSamplerNearest == nearest) return;
    g_imageSamplerNearest = nearest;
    const GLint filter = nearest ? GL_NEAREST : GL_LINEAR;
    glSamplerParameteri(g_imageSampler, GL_TEXTURE_MIN_FILTER, filter);
    glSamplerParameteri(g_imageSampler, GL_TEXTURE_MAG_FILTER, filter);
}

void installImageSampler() {
    if (g_imageSampler != 0) return;
    if (glad_glGenSamplers == nullptr || glad_glSamplerParameteri == nullptr ||
        glad_glBindSampler == nullptr) {
        return;
    }
    glGenSamplers(1, &g_imageSampler);
    if (g_imageSampler == 0) return;
    glSamplerParameteri(g_imageSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(g_imageSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(g_imageSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_imageSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_realBindSampler = glad_glBindSampler;
    glad_glBindSampler = bindImageSampler;
}

void shutdownImageSampler() {
    if (g_realBindSampler != nullptr) {
        glad_glBindSampler = g_realBindSampler;
        g_realBindSampler = nullptr;
    }
    if (g_imageSampler != 0) {
        glDeleteSamplers(1, &g_imageSampler);
        g_imageSampler = 0;
    }
    g_imageSamplerNearest = false;
}
#endif

struct ImageEntry {
    int id = 0;
    std::string path;
    std::string name;
    bool isVideo = false;
    std::shared_ptr<imview::video::Player> video;
    bool progressPreviewActive = false;
    bool videoControlsInteracting = false;
    double progressPreviewTime = 0.0;
    float progressPreviewFraction = 0.0f;
    float progressDragStartFraction = 0.0f;
    std::shared_ptr<eui::ImageStream> stream;
    // Second mailbox holding the same frame, used when this image is drawn in
    // another cell (Q cover). Each stream can only be consumed once.
    std::shared_ptr<eui::ImageStream> altStream;
    bool streamPrimed = false;
#if defined(EUI_RENDER_BACKEND_OPENGL)
    std::shared_ptr<GpuSource> gpuSource;
#endif
    std::shared_ptr<const eui::GpuImage> gpuImage;
    std::uint64_t gpuRevision = 0;
    std::shared_ptr<std::vector<std::uint8_t>> pixels; // RGBA8, top-down
    int width = 0;
    int height = 0;
    bool loading = false;
    bool loaded = false;
    bool failed = false;
    // Display state (P2+). Pan is stored per image and kept clamped, so any
    // pointer movement that would go past the cell edge is discarded instead of
    // piling up: reversing the drag re-syncs every image immediately.
    float panX = 0.0f;
    float panY = 0.0f;
    float maxPanX = 0.0f; // current clamp limits, refreshed every compose
    float maxPanY = 0.0f;
    float cellX = 0.0f, cellY = 0.0f; // logical cell rect, for drag hit-testing
    float cellW = 0.0f, cellH = 0.0f;
    int rotation = 0;  // clockwise quarter turns (0-3), P3
    // Local-mode selection (P4), normalized 0..1 within the displayed image.
    bool hasSelection = false;
    float selX0 = 0.0f, selY0 = 0.0f, selX1 = 0.0f, selY1 = 0.0f;
    bool hasStats = false;
    float statLuma = 0.0f, statRg = 0.0f, statBg = 0.0f, statSat = 0.0f;
    float statR = 0.0f, statG = 0.0f, statB = 0.0f;
    // Histogram (P5): Y luma distribution, 256 bins.
    std::array<std::uint32_t, 256> histogram{};
    std::uint32_t histogramMax = 0;
    bool hasHistogram = false;
    // EXIF summary (P5).
    imview::ExifInfo exif;
    // Lines produced by an external imexif tool, when one is available.
    std::vector<std::string> customExif;
    // Overlay text, built once per image instead of once per frame.
    std::vector<std::string> exifLines;
    std::array<std::string, 4> statsLines;
};

struct AppState {
    std::vector<ImageEntry> images;
    int nextId = 1;
    // Diff mode: a computed third image from the two grid images. It is kept
    // outside `images`, so it never counts towards the four-image cap and does
    // not change the review-mode rules. `diffMode` may survive a review pair
    // switch even when that pair has fewer than two images.
    bool diffMode = false;
    float diffThreshold = 5.0f; // 0..20, in 8-bit channel steps
    float diffSliderDragStart = 5.0f;
    ImageEntry diffImage;
    int diffSerial = 0;
    int diffRequestLeftId = -1;
    int diffRequestRightId = -1;
    const void* diffRequestLeftPixels = nullptr;
    const void* diffRequestRightPixels = nullptr;
    float diffRequestThreshold = -1.0f;
    // Shared view state.
    float zoom = 1.0f; // 1x = image fully fits its cell
    bool qSwapActive = false; // Q held: right image covers the left cell
    // Move mode: the move button arms a cell, the next clicked cell swaps with
    // it. Both cells are highlighted while the mode is armed.
    int moveSourceId = -1;
    int moveTargetId = -1;
    // Video whose controls respond to keyboard shortcuts; set when the pointer
    // enters its hot zone or one of its controls is pressed.
    int activeVideoId = -1;
    // One volume level for every video in this window. It is also copied into
    // newly opened videos, so a fresh window starts at 50%.
    float volume = 0.5f;
    bool muted = false;
    float volumeDragStart = 0.5f;
    // Sync mode: only available for two or more videos and no images. It is
    // entered paused and forwards transport commands to every video.
    bool syncMode = false;
    // Videos that have already reached the end and were handled by sync mode.
    // They stay paused at the end while the rest of the group may resume.
    std::vector<int> syncHandledEnded;
    // Solo mode: one image fills the grid (entered by double-clicking a cell).
    int soloId = -1;
    int peekId = -1; // image temporarily shown while holding another digit
    // Double-click state, one tracker per clickable surface.
    struct ClickTracker {
        int first = -1;
        int second = -1;
        double time = 0.0;
    };
    ClickTracker cellClick;
    ClickTracker rowClick;
    ClickTracker titleClick;
    // Local mode (P4).
    bool localMode = false;
    bool showHistogram = false;
    bool showExif = false;
    bool selecting = false;   // dragging a fresh shared selection
    float dragAnchorX = 0.0f, dragAnchorY = 0.0f;  // normalized
    float dragCurrentX = 0.0f, dragCurrentY = 0.0f;
    bool movingSelection = false; // right-drag moving one image's selection
    int movingCellId = -1;
    float moveCellW = 0.0f, moveCellH = 0.0f; // display size for delta normalization
    // Sidebar: up to two folder panes, split vertically.
    struct FolderPane {
        std::string dir;
        std::vector<std::string> paths;
        std::vector<std::string> names;
        std::vector<bool> videos;
        float scroll = 0.0f;
        float listHeight = 0.0f; // cached list viewport, refreshed every compose
    };
    struct RowRef {
        int pane = -1;
        int index = -1;
    };
    std::vector<FolderPane> folders;
    std::vector<RowRef> selected; // selection is shared across panes
    RowRef selectionAnchor;
    bool hasSelectionAnchor = false;
    bool sidebarVisible = true;
    float sidebarWidth = kSidebarWidth;
    float sidebarDragStartWidth = kSidebarWidth;
    // Destination highlighted while the shell drags files or folders over the
    // window. OLE reports the position before the drop, so the highlight can
    // follow the pointer.
    enum class DropTarget { None, Main, Sidebar };
    DropTarget dropTarget = DropTarget::None;
    // Review mode: two folders compared pair by pair.
    bool reviewMode = false;
    int reviewIndex = 0;
    // Context menu.
    enum class MenuKind { Row, Toolbar };
    bool menuOpen = false;
    MenuKind menuKind = MenuKind::Row;
    float menuX = 0.0f;
    float menuY = 0.0f;
    int menuPane = -1;
    int menuIndex = -1;
    // About dialog and on-demand Explorer integration.
    bool aboutOpen = false;
    bool integrationImages = false;
    bool integrationVideos = false;
    bool integrationContextMenu = false;
    bool integrationContextAvailable = false;
};

AppState& state() {
    static AppState instance;
    return instance;
}

// Hairlines are snapped to the physical pixel grid so they stay one device
// pixel wide instead of blurring across two.
float pixelScale() {
    const float scale = imview::frame::dpiScale();
    return scale > 0.0f ? scale : 1.0f;
}

float snapToPixel(float value) {
    const float scale = pixelScale();
    return std::round(value * scale) / scale;
}

float floorToPixel(float value) {
    const float scale = pixelScale();
    return std::floor(value * scale) / scale;
}

float crispLine(float width) {
    const float scale = pixelScale();
    return std::max(std::floor(width * scale), 1.0f) / scale;
}

// The sidebar keeps a usable main area for narrow windows; the stored width is
// the user's preference and is clamped to the current window each compose.
float clampSidebarWidth(float width, float screenWidth) {
    const float maximum = std::max(kSidebarMinWidth, screenWidth - kMainMinWidth);
    return std::clamp(width, kSidebarMinWidth, maximum);
}

float sidebarWidthFor(float screenWidth) {
    return snapToPixel(clampSidebarWidth(state().sidebarWidth, screenWidth));
}

// The popup menu and the About dialog swallow clicks, so the title bar must
// stop acting as a drag band while either is open. Everything that opens or
// closes one of them goes through here so the three never drift apart.
void syncTitleBarDrag() {
    imview::frame::setTitleBarDragEnabled(!state().menuOpen && !state().aboutOpen);
}

void setMenuOpen(bool open) {
    state().menuOpen = open;
    syncTitleBarDrag();
    app::requestUpdate();
}

void refreshIntegrationStatus() {
    const imview::shell::IntegrationStatus status = imview::shell::integrationStatus();
    state().integrationImages = status.images;
    state().integrationVideos = status.videos;
    state().integrationContextMenu = status.contextMenu;
    state().integrationContextAvailable = status.contextMenuAvailable;
}

void openAboutDialog() {
    state().menuOpen = false;
    state().aboutOpen = true;
    refreshIntegrationStatus();
    syncTitleBarDrag();
    app::requestUpdate();
}

void closeAboutDialog() {
    if (!state().aboutOpen) return;
    state().aboutOpen = false;
    syncTitleBarDrag();
    app::requestUpdate();
}

void toggleImageIntegration() {
    if (state().integrationImages) imview::shell::unregisterImageAssociations();
    else imview::shell::registerImageAssociations();
    refreshIntegrationStatus();
    app::requestUpdate();
}

void toggleVideoIntegration() {
    if (state().integrationVideos) imview::shell::unregisterVideoAssociations();
    else imview::shell::registerVideoAssociations();
    refreshIntegrationStatus();
    app::requestUpdate();
}

void toggleContextMenuIntegration() {
    if (state().integrationContextMenu) imview::shell::unregisterContextMenu();
    else if (!imview::shell::registerContextMenu()) {
        // The DLL is missing or could not be registered; leave the switch off.
    }
    refreshIntegrationStatus();
    app::requestUpdate();
}

// The toolbar "Open" dropdown.
void openToolbarMenu() {
    state().menuKind = AppState::MenuKind::Toolbar;
    state().menuX = 8.0f;
    state().menuY = kToolbarHeight + 2.0f;
    state().menuPane = -1;
    state().menuIndex = -1;
    setMenuOpen(!state().menuOpen);
}

// The row menu of a sidebar entry, anchored at the pointer.
void openRowMenu(int paneIndex, int index, float x, float y) {
    state().menuKind = AppState::MenuKind::Row;
    state().menuX = x;
    state().menuY = y;
    state().menuPane = paneIndex;
    state().menuIndex = index;
    setMenuOpen(true);
}

ImageEntry* findEntry(int id) {
    for (auto& entry : state().images) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

bool hasVideoMedia() {
    for (const auto& entry : state().images) {
        if (entry.isVideo) return true;
    }
    return false;
}

bool hasImageMedia() {
    for (const auto& entry : state().images) {
        if (!entry.isVideo) return true;
    }
    return false;
}

bool mixedMedia() {
    return hasVideoMedia() && hasImageMedia();
}

// Image tools stay available until a video enters the grid. Mixed media and
// video-only grids grey the image tools out; removing the videos restores them.
bool imageToolsEnabled() {
    return !hasVideoMedia();
}

int activeVideoId() {
    ImageEntry* active = findEntry(state().activeVideoId);
    if (active != nullptr && active->isVideo) return active->id;
    for (const auto& entry : state().images) {
        if (entry.isVideo) return entry.id;
    }
    return -1;
}

int videoCount() {
    int count = 0;
    for (const auto& entry : state().images) {
        if (entry.isVideo) ++count;
    }
    return count;
}

// Multiple videos may play together only in sync mode, and sync mode does not
// mix with images. One video is always available for ordinary playback.
bool syncAvailable() {
    return videoCount() > 1 && !mixedMedia();
}

void pauseOtherVideos(int keepId) {
    for (auto& entry : state().images) {
        if (!entry.isVideo || entry.id == keepId || !entry.video) continue;
        const imview::video::Status status = entry.video->status();
        if (!status.failed && !status.paused) entry.video->togglePlayPause();
    }
}

void pauseAllVideos() {
    pauseOtherVideos(-1);
}

bool videoAtEnd(const imview::video::Status& status) {
    return status.ended ||
           (status.duration > 0.0 && status.position >= status.duration - 0.05);
}

void applyGlobalVolumeToPlayers() {
    for (auto& entry : state().images) {
        if (!entry.isVideo || !entry.video) continue;
        entry.video->setVolume(state().volume);
        entry.video->setMuted(state().muted);
    }
}

// Default playback is exclusive: starting one video pauses every other video.
// In sync mode a play/pause command applies to every video together.
void toggleVideoPlayback(int id) {
    ImageEntry* target = findEntry(id);
    if (target == nullptr || !target->isVideo || !target->video) return;
    const imview::video::Status targetStatus = target->video->status();
    if (targetStatus.failed) return;
    state().activeVideoId = id;

    if (state().syncMode && syncAvailable()) {
        const bool play = targetStatus.paused;
        if (play) {
            // If the end rule has not run yet, treat the videos already at the
            // end as handled so resuming does not immediately pause the group.
            for (auto& entry : state().images) {
                if (!entry.isVideo || !entry.video) continue;
                if (!videoAtEnd(entry.video->status())) continue;
                auto& handled = state().syncHandledEnded;
                if (std::find(handled.begin(), handled.end(), entry.id) == handled.end()) {
                    handled.push_back(entry.id);
                }
            }
        }
        for (auto& entry : state().images) {
            if (!entry.isVideo || !entry.video) continue;
            const imview::video::Status status = entry.video->status();
            if (status.failed) continue;
            // A finished video stays on its final frame. It must not restart
            // from zero just because the rest of the sync group resumes.
            if (play && videoAtEnd(status)) continue;
            if (play == status.paused) entry.video->togglePlayPause();
        }
    } else {
        if (targetStatus.paused) pauseOtherVideos(id);
        target->video->togglePlayPause();
    }
    app::requestUpdate();
}

void seekVideoBy(int id, double seconds) {
    if (state().syncMode && syncAvailable()) {
        for (auto& entry : state().images) {
            if (entry.isVideo && entry.video) entry.video->seekBy(seconds);
        }
    } else if (ImageEntry* target = findEntry(id)) {
        if (target->isVideo && target->video) target->video->seekBy(seconds);
    }
    state().activeVideoId = id;
    app::requestUpdate();
}

void stepVideoFrames(int id, int delta) {
    if (state().syncMode && syncAvailable()) {
        for (auto& entry : state().images) {
            if (entry.isVideo && entry.video) entry.video->stepFrames(delta);
        }
    } else if (ImageEntry* target = findEntry(id)) {
        if (target->isVideo && target->video) target->video->stepFrames(delta);
    }
    state().activeVideoId = id;
    app::requestUpdate();
}

// Keeps the no-sync single-playback rule after asynchronous opens, and pauses
// the group when a video in sync mode reaches its end before the others.
void enforcePlaybackRules() {
    if (videoCount() == 0) return;
    if (state().syncMode) {
        bool newEnded = false;
        bool anyPlaying = false;
        auto& handled = state().syncHandledEnded;
        for (const auto& entry : state().images) {
            if (!entry.isVideo || !entry.video) continue;
            const imview::video::Status status = entry.video->status();
            if (!status.paused && !status.failed) anyPlaying = true;
            const bool atEnd = videoAtEnd(status);
            const auto known = std::find(handled.begin(), handled.end(), entry.id);
            if (atEnd) {
                if (known == handled.end()) newEnded = true;
            } else if (known != handled.end()) {
                handled.erase(known);
            }
        }
        // A video that reaches the end pauses the group once. Once it is
        // recorded, pressing play can resume the others without immediately
        // tripping this rule again.
        if (newEnded) {
            if (anyPlaying) pauseAllVideos();
            for (const auto& entry : state().images) {
                if (!entry.isVideo || !entry.video) continue;
                if (videoAtEnd(entry.video->status()) &&
                    std::find(handled.begin(), handled.end(), entry.id) == handled.end()) {
                    handled.push_back(entry.id);
                }
            }
        }
        return;
    }
    if (videoCount() < 2) return;
    int keep = activeVideoId();
    ImageEntry* active = findEntry(keep);
    bool keepPlaying = active != nullptr && active->video &&
        !active->video->status().paused && !active->video->status().failed;
    if (!keepPlaying) {
        for (const auto& entry : state().images) {
            if (!entry.isVideo || !entry.video) continue;
            const imview::video::Status status = entry.video->status();
            if (!status.failed && !status.paused) {
                keep = entry.id;
                break;
            }
        }
    }
    pauseOtherVideos(keep);
}

// Called after any grid mutation. Leaving sync mode pauses all but the active
// video; entering it is handled by the S chip.
void enforceSyncAvailability() {
    if (!state().syncMode || syncAvailable()) return;
    state().syncMode = false;
    state().syncHandledEnded.clear();
    pauseOtherVideos(activeVideoId());
}

void toggleSyncMode() {
    if (!syncAvailable()) return;
    state().syncMode = !state().syncMode;
    state().syncHandledEnded.clear();
    if (state().syncMode) {
        pauseAllVideos();
    } else {
        pauseOtherVideos(activeVideoId());
    }
    app::requestUpdate();
}

// Diff needs two decoded images with identical pixel dimensions. Mismatched
// sizes are rejected instead of being resampled into a common canvas.
bool diffPairReady() {
    const auto& images = state().images;
    if (images.size() != 2) return false;
    const ImageEntry& left = images[0];
    const ImageEntry& right = images[1];
    if (left.isVideo || right.isVideo) return false;
    return left.loaded && right.loaded && !left.failed && !right.failed &&
           left.pixels && right.pixels && left.width > 0 && left.height > 0 &&
           right.width > 0 && right.height > 0 &&
           left.width == right.width && left.height == right.height;
}

bool diffVisible() {
    return state().diffMode && diffPairReady();
}

// Drops the computed pixels but leaves the mode and request bookkeeping alone.
void resetDiffImageData() {
    ImageEntry& diff = state().diffImage;
    diff.pixels.reset();
    diff.width = 0;
    diff.height = 0;
    diff.loaded = false;
    diff.failed = false;
    diff.streamPrimed = false;
    diff.panX = 0.0f;
    diff.panY = 0.0f;
    diff.maxPanX = 0.0f;
    diff.maxPanY = 0.0f;
    diff.rotation = 0;
    diff.gpuImage.reset();
    diff.gpuRevision = 0;
#if defined(EUI_RENDER_BACKEND_OPENGL)
    diff.gpuSource.reset();
#endif
}

void clearDiffImage() {
    ++state().diffSerial;
    state().diffRequestLeftId = -1;
    state().diffRequestRightId = -1;
    state().diffRequestLeftPixels = nullptr;
    state().diffRequestRightPixels = nullptr;
    state().diffRequestThreshold = -1.0f;
    resetDiffImageData();
    state().diffImage.loading = false;
}

void exitDiffMode() {
    if (!state().diffMode) return;
    state().diffMode = false;
    clearDiffImage();
    app::requestUpdate();
}

void toggleDiffMode() {
    if (!imageToolsEnabled() && !state().diffMode) return;
    if (state().diffMode) {
        exitDiffMode();
        return;
    }
    if (!diffPairReady()) return;
    state().diffMode = true;
    state().diffImage.id = -1;
    if (!state().diffImage.stream) {
        state().diffImage.stream = std::make_shared<eui::ImageStream>(1);
    }
    app::requestUpdate();
}

// Every grid change funnels through this: the only way to stay in diff mode is
// to still have exactly two decoded images with matching dimensions. While a
// new pair is still loading the mode survives; a failed decode or a size
// mismatch ends it as soon as the outcome is known.
void enforceDiffMode() {
    if (!state().diffMode) return;
    const auto& images = state().images;
    if (images.size() != 2) {
        exitDiffMode();
        return;
    }
    const ImageEntry& left = images[0];
    const ImageEntry& right = images[1];
    const bool bothDecoded = left.loaded && right.loaded;
    if (left.isVideo || right.isVideo || left.failed || right.failed ||
        (bothDecoded && !diffPairReady())) {
        exitDiffMode();
    }
}

// Pushes the decoded frame into one of an entry's mailboxes. Streams are
// single-shot, so a stream is only refilled right before it is drawn again.
void submitFrame(const ImageEntry& entry, const std::shared_ptr<eui::ImageStream>& stream) {
    if (!stream || !entry.pixels || entry.width <= 0 || entry.height <= 0) return;
    eui::ImageFrame frame;
    frame.pixels = entry.pixels;
    frame.width = static_cast<std::uint32_t>(entry.width);
    frame.height = static_cast<std::uint32_t>(entry.height);
    frame.stride = static_cast<std::uint32_t>(entry.width) * 4u;
    frame.format = eui::ImagePixelFormat::RGBA8;
    stream->submit(std::move(frame));
}

#if defined(EUI_RENDER_BACKEND_OPENGL)
// Uploads the decoded frame once into an app-owned texture and wraps it as an
// external GpuImage. The shared image sampler is switched between LINEAR and
// NEAREST by the zoom state.
bool ensureGpuImage(ImageEntry& entry, bool nearest) {
    if (!entry.loaded || !entry.pixels || entry.width <= 0 || entry.height <= 0) {
        return false;
    }
    setImageSamplerFilter(nearest);
    if (!entry.gpuSource) {
        const eui::GpuDeviceInfo device = eui::image::gpuDevice();
        if (device.api != eui::GpuApi::OpenGL || device.identity == 0) return false;

        auto source = std::make_shared<GpuSource>();
        source->device = device;
        glGenTextures(1, &source->texture);
        if (source->texture == 0) return false;
        GLint previousTexture = 0;
        GLint previousAlignment = 4;
        GLint previousRowLength = 0;
        GLint previousSkipPixels = 0;
        GLint previousSkipRows = 0;
        GLint previousSwapBytes = GL_FALSE;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previousRowLength);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &previousSkipPixels);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &previousSkipRows);
        glGetIntegerv(GL_UNPACK_SWAP_BYTES, &previousSwapBytes);
        glBindTexture(GL_TEXTURE_2D, source->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, entry.width, entry.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, entry.pixels->data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, previousRowLength);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, previousSkipPixels);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, previousSkipRows);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, previousSwapBytes);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        entry.gpuSource = std::move(source);
    }

    if (!entry.gpuImage) {
        eui::GpuImageDescriptor descriptor{entry.gpuSource->device, entry.width,
                                           entry.height, entry.gpuSource->texture};
        std::shared_ptr<const eui::GpuImage> image =
            eui::image::importGpuImage(descriptor, entry.gpuSource);
        if (!image) return false;
        entry.gpuImage = std::move(image);
        entry.gpuRevision = 1;
    }
    return true;
}
#endif

// --- view math (P2 zoom/pan) --------------------------------------------------

constexpr float kMaxZoom = 128.0f;

// Size the image occupies at 1x inside a cell (Contain fit), honoring rotation.
void fitSize(const ImageEntry& entry, float cellWidth, float cellHeight,
             float& outWidth, float& outHeight) {
    if (entry.width <= 0 || entry.height <= 0) {
        outWidth = cellWidth;
        outHeight = cellHeight;
        return;
    }
    const bool swapped = entry.rotation % 2 == 1;
    const float imageWidth = swapped ? static_cast<float>(entry.height)
                                     : static_cast<float>(entry.width);
    const float imageHeight = swapped ? static_cast<float>(entry.width)
                                      : static_cast<float>(entry.height);
    const float scale = std::min(cellWidth / imageWidth, cellHeight / imageHeight);
    outWidth = imageWidth * scale;
    outHeight = imageHeight * scale;
}

// Pan limits for the current zoom level, in cell pixels.
void panLimits(const ImageEntry& entry, float cellWidth, float cellHeight,
               float& maxX, float& maxY) {
    float fittedWidth = 0.0f;
    float fittedHeight = 0.0f;
    fitSize(entry, cellWidth, cellHeight, fittedWidth, fittedHeight);
    const float displayedWidth = fittedWidth * state().zoom;
    const float displayedHeight = fittedHeight * state().zoom;
    maxX = std::max(0.0f, (displayedWidth - cellWidth) * 0.5f);
    maxY = std::max(0.0f, (displayedHeight - cellHeight) * 0.5f);
}

void displayOffset(const ImageEntry& entry, float cellWidth, float cellHeight,
                   float& outX, float& outY) {
    float maxX = 0.0f;
    float maxY = 0.0f;
    panLimits(entry, cellWidth, cellHeight, maxX, maxY);
    outX = std::clamp(entry.panX, -maxX, maxX);
    outY = std::clamp(entry.panY, -maxY, maxY);
}

void resetPans() {
    for (auto& entry : state().images) {
        entry.panX = 0.0f;
        entry.panY = 0.0f;
    }
    if (state().diffMode) {
        state().diffImage.panX = 0.0f;
        state().diffImage.panY = 0.0f;
    }
}

// Applies one pan step. Because the stored offset is clamped right here, any
// movement past the cell edge is dropped rather than accumulated, which keeps
// images in sync when the drag direction is reversed.
void panBy(ImageEntry& entry, float deltaX, float deltaY) {
    entry.panX = std::clamp(entry.panX + deltaX, -entry.maxPanX, entry.maxPanX);
    entry.panY = std::clamp(entry.panY + deltaY, -entry.maxPanY, entry.maxPanY);
}

void panAllBy(float deltaX, float deltaY) {
    for (auto& entry : state().images) {
        panBy(entry, deltaX, deltaY);
    }
    if (diffVisible()) panBy(state().diffImage, deltaX, deltaY);
}

bool pointerInRect(float x, float y, float width, float height) {
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    if (!imview::frame::cursorLogicalPosition(pointerX, pointerY)) return false;
    return pointerX >= x && pointerX <= x + width &&
           pointerY >= y && pointerY <= y + height;
}

// The close/move buttons live in the top-right corner; the video transport sits
// in the bottom band. Both only appear while the pointer is in these hot zones.
bool pointerInButtonHotZone(const ImageEntry& entry) {
    // The cell itself is the close/move hot zone; the buttons only exist while
    // the pointer is inside it.
    return pointerInRect(entry.cellX, entry.cellY, entry.cellW, entry.cellH);
}

// Returns the image id whose cell contains the given logical point. Hover
// callbacks already report logical units; drag callbacks report physical ones.
int cellAt(float x, float y) {
    for (const auto& entry : state().images) {
        if (x >= entry.cellX && x <= entry.cellX + entry.cellW &&
            y >= entry.cellY && y <= entry.cellY + entry.cellH) {
            return entry.id;
        }
    }
    return -1;
}

// --- folder sidebar -----------------------------------------------------------

// Defined further down (media loading pipeline). `fromSidebar` marks additions
// driven by the sidebar itself, which must not hide the sidebar.
void addPaths(const std::vector<std::string>& paths, bool fromSidebar = false,
              bool preserveDiff = false, bool preserveSidebar = false);
void clearSelections();

// Case-insensitive comparison of two paths, used to mark files that are already
// shown in the main grid.
bool samePath(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        if (a == b) continue;
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool isOpenInGrid(const std::string& path) {
    for (const auto& entry : state().images) {
        if (samePath(entry.path, path)) return true;
    }
    return false;
}

// --- folder panes -------------------------------------------------------------

bool isSelected(int pane, int index) {
    for (const auto& row : state().selected) {
        if (row.pane == pane && row.index == index) return true;
    }
    return false;
}

// Reads the image and video files of a directory (first level only, name-ascending).
void fillPaneFiles(AppState::FolderPane& pane) {
    namespace fs = std::filesystem;
    pane.paths.clear();
    pane.names.clear();
    pane.videos.clear();
    pane.scroll = 0.0f;
    std::error_code error;
    fs::directory_iterator iterator(fs::u8path(pane.dir), error);
    if (error) return;

    struct Item {
        std::string path;
        std::string name;
        std::string lower;
        bool video = false;
    };
    std::vector<Item> items;
    for (const auto& entry : iterator) {
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError) continue;
        const std::string path = entry.path().u8string();
        const bool video = isVideoPath(path);
        if (!video && !isImagePath(path)) continue;
        Item item;
        item.path = path;
        item.name = entry.path().filename().u8string();
        item.video = video;
        item.lower = item.name;
        std::transform(item.lower.begin(), item.lower.end(), item.lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const Item& left, const Item& right) {
        return left.lower < right.lower;
    });
    pane.paths.reserve(items.size());
    pane.names.reserve(items.size());
    pane.videos.reserve(items.size());
    for (auto& item : items) {
        pane.paths.push_back(std::move(item.path));
        pane.names.push_back(std::move(item.name));
        pane.videos.push_back(item.video);
    }
}

// Opens a folder in the first free pane; with two panes open the second one is
// replaced.
void addFolder(const std::string& directory) {
    namespace fs = std::filesystem;
    // Review mode owns both panes: the pair would lose its meaning.
    if (state().reviewMode) return;
    std::error_code error;
    if (directory.empty() || !fs::is_directory(fs::u8path(directory), error) || error) return;

    for (const auto& pane : state().folders) {
        if (samePath(pane.dir, directory)) {
            state().sidebarVisible = true;
            app::requestUpdate();
            return;
        }
    }

    AppState::FolderPane pane;
    pane.dir = directory;
    fillPaneFiles(pane);

    if (state().folders.size() < 2) {
        state().folders.push_back(std::move(pane));
    } else {
        // Selections belonging to the replaced pane are dropped.
        auto& selected = state().selected;
        selected.erase(std::remove_if(selected.begin(), selected.end(),
                                      [](const AppState::RowRef& row) { return row.pane == 1; }),
                       selected.end());
        state().hasSelectionAnchor = false;
        state().folders[1] = std::move(pane);
    }
    state().sidebarVisible = true;
    setMenuOpen(false);
}

// Drops add folders to free panes; with both panes occupied the drop has no
// target. The two-folder case is processed in order, so dropping two folders
// into an empty sidebar fills both panes.
void addFoldersFromDrop(const std::vector<std::string>& directories) {
    if (state().reviewMode) return;
    for (const std::string& directory : directories) {
        if (state().folders.size() >= 2) break;
        addFolder(directory);
    }
    app::requestUpdate();
}

// Replaces one pane's folder through the system picker.
void replacePaneFolder(int paneIndex) {
    if (state().reviewMode) return;
    if (paneIndex < 0 || paneIndex >= static_cast<int>(state().folders.size())) return;
    const std::string directory =
        imview::shell::pickFolder(state().folders[static_cast<std::size_t>(paneIndex)].dir);
    if (directory.empty()) return;

    AppState::FolderPane pane;
    pane.dir = directory;
    fillPaneFiles(pane);
    auto& selected = state().selected;
    selected.erase(std::remove_if(selected.begin(), selected.end(),
                                  [paneIndex](const AppState::RowRef& row) {
                                      return row.pane == paneIndex;
                                  }),
                   selected.end());
    state().hasSelectionAnchor = false;
    state().folders[static_cast<std::size_t>(paneIndex)] = std::move(pane);
    setMenuOpen(false);
}

void closePane(int paneIndex) {
    if (state().reviewMode) return;
    if (paneIndex < 0 || paneIndex >= static_cast<int>(state().folders.size())) return;
    state().folders.erase(state().folders.begin() + paneIndex);
    auto& selected = state().selected;
    for (auto& row : selected) {
        if (row.pane == paneIndex) {
            row.pane = -1;
        } else if (row.pane > paneIndex) {
            --row.pane;
        }
    }
    selected.erase(std::remove_if(selected.begin(), selected.end(),
                                  [](const AppState::RowRef& row) { return row.pane < 0; }),
                   selected.end());
    state().hasSelectionAnchor = false;
    setMenuOpen(false);
}

// Toolbar entry point: pick one or more folders and add (or replace) panes.
void openFolder() {
    if (state().reviewMode) return;
    const std::vector<std::string> directories = imview::shell::pickFolders(
        state().folders.empty() ? std::string() : state().folders.front().dir);
    if (directories.empty()) return;
    if (directories.size() == 1) {
        addFolder(directories.front());
        return;
    }
    // A multi-selection replaces the pane layout so every chosen folder gets a
    // pane; the sidebar shows at most two folders.
    state().folders.clear();
    state().selected.clear();
    state().hasSelectionAnchor = false;
    for (const std::string& directory : directories) {
        if (state().folders.size() >= 2) break;
        addFolder(directory);
    }
}

// Click handling. Ctrl toggles a row across both panes; Shift extends from the
// last clicked row in the same pane. Ctrl+Shift adds that range to an existing
// selection.
void selectRow(int paneIndex, int index, bool ctrl, bool shift) {
    if (paneIndex < 0 || paneIndex >= static_cast<int>(state().folders.size())) return;
    auto& pane = state().folders[static_cast<std::size_t>(paneIndex)];
    if (index < 0 || index >= static_cast<int>(pane.paths.size())) return;
    auto& selected = state().selected;

    const bool range = shift && state().hasSelectionAnchor &&
                       state().selectionAnchor.pane == paneIndex &&
                       state().selectionAnchor.index >= 0 &&
                       state().selectionAnchor.index < static_cast<int>(pane.paths.size());
    if (range) {
        const int first = std::min(state().selectionAnchor.index, index);
        const int last = std::max(state().selectionAnchor.index, index);
        if (!ctrl) selected.clear();
        for (int rowIndex = first; rowIndex <= last; ++rowIndex) {
            if (!isSelected(paneIndex, rowIndex)) {
                selected.push_back({paneIndex, rowIndex});
            }
        }
        app::requestUpdate();
        return;
    }

    if (ctrl) {
        const auto it = std::find_if(selected.begin(), selected.end(),
                                     [paneIndex, index](const AppState::RowRef& row) {
                                         return row.pane == paneIndex && row.index == index;
                                     });
        if (it != selected.end()) {
            selected.erase(it);
        } else {
            selected.push_back({paneIndex, index});
        }
    } else {
        selected.clear();
        selected.push_back({paneIndex, index});
    }
    state().selectionAnchor = {paneIndex, index};
    state().hasSelectionAnchor = true;
    app::requestUpdate();
}

// Selection in pane-then-index order, skipping rows whose folder is gone.
std::vector<std::string> selectedPaths() {
    std::vector<AppState::RowRef> rows = state().selected;
    std::sort(rows.begin(), rows.end(),
              [](const AppState::RowRef& left, const AppState::RowRef& right) {
                  if (left.pane != right.pane) return left.pane < right.pane;
                  return left.index < right.index;
              });
    std::vector<std::string> paths;
    for (const auto& row : rows) {
        if (row.pane < 0 || row.pane >= static_cast<int>(state().folders.size())) continue;
        const auto& pane = state().folders[static_cast<std::size_t>(row.pane)];
        if (row.index < 0 || row.index >= static_cast<int>(pane.paths.size())) continue;
        paths.push_back(pane.paths[static_cast<std::size_t>(row.index)]);
    }
    return paths;
}

// Drops the current grid while keeping the sidebar selection in place.
void resetGridForOpen() {
    state().images.clear();
    state().activeVideoId = -1;
    state().syncMode = false;
    state().syncHandledEnded.clear();
    state().soloId = -1;
    state().peekId = -1;
    state().moveSourceId = -1;
    state().moveTargetId = -1;
    state().zoom = 1.0f;
    state().localMode = false;
    state().qSwapActive = false;
    resetPans();
    clearSelections();
}

void openSingleInGrid(int paneIndex, int index) {
    if (paneIndex < 0 || paneIndex >= static_cast<int>(state().folders.size())) return;
    const auto& pane = state().folders[static_cast<std::size_t>(paneIndex)];
    if (index < 0 || index >= static_cast<int>(pane.paths.size())) return;
    resetGridForOpen();
    addPaths({pane.paths[static_cast<std::size_t>(index)]}, true);
}

// The row menu's Open: the highlighted rows decide what is shown, so a plain
// right-click on an unselected row still opens just that row (it became the
// selection when the menu opened).
void openSelectionInGrid() {
    const std::vector<std::string> paths = selectedPaths();
    if (paths.empty()) return;
    resetGridForOpen();
    addPaths(paths, true);
}

// --- review mode --------------------------------------------------------------

// Review mode is an image comparison. A pair index is valid only when neither
// side at that index is a video and at least one side has an image. Missing
// entries on the shorter side are allowed, matching the existing pair rules.
bool reviewPairIsValidImage(int index) {
    if (index < 0 || state().folders.size() != 2) return false;
    bool hasImage = false;
    for (const auto& pane : state().folders) {
        if (index >= static_cast<int>(pane.paths.size())) continue;
        if (index < static_cast<int>(pane.videos.size()) &&
            pane.videos[static_cast<std::size_t>(index)]) {
            return false;
        }
        hasImage = true;
    }
    return hasImage;
}

int reviewFilePairCount() {
    if (state().folders.size() != 2) return 0;
    return static_cast<int>(std::max(state().folders[0].paths.size(),
                                     state().folders[1].paths.size()));
}

int firstReviewImagePair() {
    const int count = reviewFilePairCount();
    for (int index = 0; index < count; ++index) {
        if (reviewPairIsValidImage(index)) return index;
    }
    return -1;
}

int reviewImagePairCount() {
    const int count = reviewFilePairCount();
    int valid = 0;
    for (int index = 0; index < count; ++index) {
        if (reviewPairIsValidImage(index)) ++valid;
    }
    return valid;
}

// 1-based position of a file index among the valid image pairs.
int reviewImageOrdinal(int index) {
    int ordinal = 0;
    for (int candidate = 0; candidate <= index; ++candidate) {
        if (reviewPairIsValidImage(candidate)) ++ordinal;
    }
    return ordinal;
}

int nextReviewImagePair(int from, int delta) {
    const int count = reviewFilePairCount();
    if (count == 0 || delta == 0) return -1;
    const int step = delta > 0 ? 1 : -1;
    for (int index = from + step; index >= 0 && index < count; index += step) {
        if (reviewPairIsValidImage(index)) return index;
    }
    return -1;
}

bool reviewAvailable() {
    return firstReviewImagePair() >= 0;
}

int reviewPairCount() {
    return reviewImagePairCount();
}

// Largest scroll value across both panes, so the two lists can be aligned by row
// index instead of by scroll percentage.
float reviewMaxScroll() {
    float maxScroll = 0.0f;
    for (const auto& pane : state().folders) {
        const float total = static_cast<float>(pane.paths.size()) * kSidebarRowHeight;
        maxScroll = std::max(maxScroll, std::max(0.0f, total - pane.listHeight));
    }
    return maxScroll;
}

// Review mode shares one scroll offset between the panes: identical row indices
// sit at the same height, so a pair always lines up even when the folders differ
// in length.
void setReviewScroll(float value) {
    const float clamped = std::clamp(value, 0.0f, reviewMaxScroll());
    for (auto& pane : state().folders) {
        pane.scroll = clamped;
    }
}

// Scrolls both panes just enough to keep the current pair visible.
void revealReviewPair() {
    if (!state().reviewMode || state().folders.size() != 2) return;
    const float rowTop = static_cast<float>(state().reviewIndex) * kSidebarRowHeight;
    const float visible = std::max(state().folders[0].listHeight,
                                   state().folders[1].listHeight);
    if (visible <= 0.0f) return;
    float scroll = state().folders[0].scroll;
    if (rowTop < scroll) {
        scroll = rowTop;
    } else if (rowTop + kSidebarRowHeight > scroll + visible) {
        scroll = rowTop + kSidebarRowHeight - visible;
    }
    setReviewScroll(scroll);
}

// Opens pair `index`: folder 1's image on the left, folder 2's on the right.
// When one folder has fewer images the missing side is simply omitted.
void openReviewPair(int index) {
    if (!reviewAvailable() || !reviewPairIsValidImage(index)) return;
    const int pair = index;

    auto& folders = state().folders;
    std::vector<std::string> paths;
    if (pair < static_cast<int>(folders[0].paths.size())) {
        paths.push_back(folders[0].paths[static_cast<std::size_t>(pair)]);
    }
    if (pair < static_cast<int>(folders[1].paths.size())) {
        paths.push_back(folders[1].paths[static_cast<std::size_t>(pair)]);
    }
    if (paths.empty()) return;

    state().reviewIndex = pair;
    state().images.clear();
    state().activeVideoId = -1;
    state().syncMode = false;
    state().syncHandledEnded.clear();
    state().soloId = -1;
    state().peekId = -1;
    state().zoom = 1.0f;
    state().localMode = false;
    state().qSwapActive = false;
    resetPans();
    clearSelections();

    // Highlight the pair rows in both panes.
    auto& selected = state().selected;
    selected.clear();
    state().hasSelectionAnchor = false;
    if (pair < static_cast<int>(folders[0].paths.size())) selected.push_back({0, pair});
    if (pair < static_cast<int>(folders[1].paths.size())) selected.push_back({1, pair});

    // Review pair switching is the one mutation allowed to keep diff mode even
    // though the grid is replaced; ordinary opens re-check the two-image rule.
    addPaths(paths, false, true);
    revealReviewPair();
}

void enterReviewMode() {
    const int first = firstReviewImagePair();
    if (first < 0) return;
    state().reviewMode = true;
    state().dropTarget = AppState::DropTarget::None;
    state().sidebarVisible = true;
    openReviewPair(first);
    app::requestUpdate();
}

void exitReviewMode() {
    if (!state().reviewMode) return;
    // Keeps whatever is displayed; only the pair navigation goes away.
    state().reviewMode = false;
    app::requestUpdate();
}

void toggleReviewMode() {
    if (state().reviewMode) {
        exitReviewMode();
    } else {
        enterReviewMode();
    }
}

void reviewStep(int delta) {
    if (!state().reviewMode || !reviewAvailable()) return;
    const int next = nextReviewImagePair(state().reviewIndex, delta);
    if (next >= 0) openReviewPair(next);
}

// Appends the selection (both panes, pane then index order), capped at four.
void addSelectionToGrid() {
    const std::vector<std::string> paths = selectedPaths();
    if (paths.empty()) return;
    addPaths(paths, true);
}

void swapEntries(int firstId, int secondId) {
    const auto first = std::find_if(state().images.begin(), state().images.end(),
                                    [firstId](const ImageEntry& entry) { return entry.id == firstId; });
    const auto second = std::find_if(state().images.begin(), state().images.end(),
                                     [secondId](const ImageEntry& entry) { return entry.id == secondId; });
    if (first == state().images.end() || second == state().images.end()) return;
    std::iter_swap(first, second);
}

// Ends a move: a real target swaps the two cells, anything else just disarms.
void finishMove() {
    const int source = state().moveSourceId;
    if (source < 0) return;
    const int target = state().moveTargetId;
    state().moveSourceId = -1;
    state().moveTargetId = -1;
    if (target >= 0 && target != source) swapEntries(source, target);
    app::requestUpdate();
}

void applyZoom(float factor) {
    if (!imageToolsEnabled()) return;
    float next = state().zoom * factor;
    next = std::clamp(next, 1.0f, kMaxZoom);
    state().zoom = next;
    if (next <= 1.0f) resetPans();
    app::requestUpdate();
}

bool zoomEnabled() {
    return !state().localMode && !hasVideoMedia();
}

bool panEnabled() {
    return state().zoom > 1.0f && !state().localMode && !hasVideoMedia();
}

// --- solo mode ----------------------------------------------------------------

// 1-based display position of an image, or 0 when it is not in the grid.
int indexOf(int id) {
    const auto& images = state().images;
    for (std::size_t index = 0; index < images.size(); ++index) {
        if (images[index].id == id) return static_cast<int>(index) + 1;
    }
    return 0;
}

bool soloActive() {
    return state().soloId >= 0 && indexOf(state().soloId) > 0;
}

// The image currently drawn on its own: the peeked one while a digit is held.
int shownSoloId() {
    if (state().peekId >= 0 && indexOf(state().peekId) > 0) return state().peekId;
    return state().soloId;
}

void enterSolo(int id) {
    if (!imageToolsEnabled()) return;
    if (!findEntry(id)) return;
    state().soloId = id;
    state().peekId = -1;
    state().zoom = 1.0f;
    resetPans();
    app::requestUpdate();
}

void exitSolo() {
    if (state().soloId < 0 && state().peekId < 0) return;
    state().soloId = -1;
    state().peekId = -1;
    app::requestUpdate();
}

void toggleSolo(int id) {
    if (!imageToolsEnabled() && !soloActive()) return;
    if (soloActive() && state().soloId == id) {
        exitSolo();
    } else {
        enterSolo(id);
    }
}

// Digit keys while solo: hold to compare against another image.
void beginPeek(int displayIndex) {
    const int count = static_cast<int>(state().images.size());
    if (displayIndex < 1 || displayIndex > count) return;
    const int id = state().images[static_cast<std::size_t>(displayIndex - 1)].id;
    if (id == state().peekId) return;
    state().peekId = id;
    app::requestUpdate();
}

void endPeek() {
    if (state().peekId < 0) return;
    state().peekId = -1;
    app::requestUpdate();
}

std::string formatExposure(double seconds) {
    if (seconds <= 0.0) return {};
    char buffer[32];
    if (seconds < 1.0) {
        const long denominator = std::lround(1.0 / seconds);
        std::snprintf(buffer, sizeof(buffer), "1/%lds", denominator > 0 ? denominator : 1L);
    } else if (std::fabs(seconds - std::round(seconds)) < 0.05) {
        std::snprintf(buffer, sizeof(buffer), "%lds", std::lround(seconds));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1fs", seconds);
    }
    return buffer;
}

// --- overlay text -------------------------------------------------------------
//
// Built when an image finishes loading (and when statistics change), so compose
// only ever copies strings into the element tree.

std::vector<std::string> buildExifLines(const ImageEntry& entry) {
    if (!entry.customExif.empty()) return entry.customExif;

    std::vector<std::string> lines;
    lines.push_back(entry.name);
    if (entry.exif.hasCamera()) {
        std::string camera = entry.exif.make;
        if (!entry.exif.model.empty()) {
            // Avoid "Canon Canon EOS 5D" for vendors that prefix the model.
            if (!camera.empty() && entry.exif.model.rfind(camera, 0) == 0) {
                camera = entry.exif.model;
            } else {
                camera = camera.empty() ? entry.exif.model : camera + " " + entry.exif.model;
            }
        }
        if (!camera.empty()) lines.push_back(camera);
    }
    if (!entry.exif.lens.empty()) lines.push_back(entry.exif.lens);
    if (entry.exif.fNumber > 0.0) {
        char buffer[32];
        const double rounded = std::round(entry.exif.fNumber * 10.0) / 10.0;
        if (std::fabs(rounded - std::round(rounded)) < 0.05) {
            std::snprintf(buffer, sizeof(buffer), "f/%.0f", rounded);
        } else {
            std::snprintf(buffer, sizeof(buffer), "f/%.1f", rounded);
        }
        lines.push_back(buffer);
    }
    const std::string exposure = formatExposure(entry.exif.exposureTime);
    if (!exposure.empty()) lines.push_back(exposure);
    if (entry.exif.iso > 0) {
        lines.push_back("ISO " + std::to_string(entry.exif.iso));
    }
    if (entry.exif.flash >= 0) {
        lines.push_back(entry.exif.flash != 0 ? "Flash: On" : "Flash: Off");
    }
    return lines;
}

std::array<std::string, 4> buildStatsLines(const ImageEntry& entry) {
    std::array<std::string, 4> lines;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "Luma:%.2f", entry.statLuma);
    lines[0] = buffer;
    std::snprintf(buffer, sizeof(buffer), "R/G:%.2f B/G:%.2f", entry.statRg, entry.statBg);
    lines[1] = buffer;
    std::snprintf(buffer, sizeof(buffer), "Sat:%.2f", entry.statSat);
    lines[2] = buffer;
    std::snprintf(buffer, sizeof(buffer), "R:%.2f G:%.2f B:%.2f",
                  entry.statR, entry.statG, entry.statB);
    lines[3] = buffer;
    return lines;
}

// --- double click -------------------------------------------------------------

// True when the same target is clicked twice inside the double-click window.
bool isDoubleClick(AppState::ClickTracker& tracker, int first, int second, double now) {
    const bool again = tracker.first == first && tracker.second == second &&
                       now - tracker.time < 0.40;
    if (again) {
        tracker = AppState::ClickTracker{};
        return true;
    }
    tracker.first = first;
    tracker.second = second;
    tracker.time = now;
    return false;
}

// --- local mode: selection + statistics (P4) ----------------------------------

// Displayed image rect inside a cell, in cell-local coordinates.
struct DisplayRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

DisplayRect displayRect(const ImageEntry& entry, float cellWidth, float cellHeight) {
    float fittedWidth = 0.0f;
    float fittedHeight = 0.0f;
    fitSize(entry, cellWidth, cellHeight, fittedWidth, fittedHeight);
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    displayOffset(entry, cellWidth, cellHeight, offsetX, offsetY);
    DisplayRect rect;
    rect.w = fittedWidth * state().zoom;
    rect.h = fittedHeight * state().zoom;
    rect.x = cellWidth * 0.5f - rect.w * 0.5f + offsetX;
    rect.y = cellHeight * 0.5f - rect.h * 0.5f + offsetY;
    return rect;
}

void clearSelections() {
    state().selecting = false;
    state().movingSelection = false;
    state().movingCellId = -1;
    for (auto& entry : state().images) {
        entry.hasSelection = false;
        entry.hasStats = false;
    }
}

// Mixed media turns off the image comparison features. The toggle state is kept
// so removing the video restores EXIF/histogram and other preferences.
void enforceMediaMode() {
    if (!hasVideoMedia()) {
        enforceSyncAvailability();
        return;
    }
    state().zoom = 1.0f;
    resetPans();
    state().localMode = false;
    state().qSwapActive = false;
    state().soloId = -1;
    state().peekId = -1;
    state().moveSourceId = -1;
    state().moveTargetId = -1;
    state().selecting = false;
    state().movingSelection = false;
    state().movingCellId = -1;
    exitDiffMode();
    clearSelections();
    if (state().activeVideoId >= 0) {
        ImageEntry* active = findEntry(state().activeVideoId);
        if (active == nullptr || !active->isVideo) state().activeVideoId = -1;
    }
    enforceSyncAvailability();
}

// Average colour of the selection, sampled from the decoded pixels.
void computeStats(ImageEntry& entry) {
    entry.hasStats = false;
    if (!entry.hasSelection || !entry.loaded || entry.width <= 0 || entry.height <= 0 ||
        !entry.pixels) {
        return;
    }
    const int imageWidth = entry.width;
    const int imageHeight = entry.height;
    const float u0 = std::min(entry.selX0, entry.selX1);
    const float u1 = std::max(entry.selX0, entry.selX1);
    const float v0 = std::min(entry.selY0, entry.selY1);
    const float v1 = std::max(entry.selY0, entry.selY1);

    constexpr int kSteps = 128;
    double sumR = 0.0;
    double sumG = 0.0;
    double sumB = 0.0;
    int count = 0;
    const auto* data = entry.pixels->data();
    // A click without dragging is still a valid 1x1 ROI. Use a single sample in
    // that case instead of repeating the same pixel 128x128 times.
    const int columns = u1 > u0 ? kSteps : 1;
    const int rows = v1 > v0 ? kSteps : 1;
    for (int row = 0; row < rows; ++row) {
        const float v = rows == 1
            ? (v0 + v1) * 0.5f
            : v0 + (v1 - v0) * ((static_cast<float>(row) + 0.5f) / kSteps);
        for (int column = 0; column < columns; ++column) {
            const float u = columns == 1
                ? (u0 + u1) * 0.5f
                : u0 + (u1 - u0) * ((static_cast<float>(column) + 0.5f) / kSteps);
            // Inverse of the clockwise display rotation.
            float su = u;
            float sv = v;
            switch (entry.rotation) {
                case 1: su = v;        sv = 1.0f - u; break;
                case 2: su = 1.0f - u; sv = 1.0f - v; break;
                case 3: su = 1.0f - v; sv = u;        break;
                default: break;
            }
            const int px = std::clamp(static_cast<int>(su * imageWidth), 0, imageWidth - 1);
            const int py = std::clamp(static_cast<int>(sv * imageHeight), 0, imageHeight - 1);
            const std::uint8_t* pixel = data + (static_cast<std::size_t>(py) * imageWidth + px) * 4u;
            sumR += pixel[0];
            sumG += pixel[1];
            sumB += pixel[2];
            ++count;
        }
    }
    if (count == 0) return;

    const float r = static_cast<float>(sumR / count) / 255.0f;
    const float g = static_cast<float>(sumG / count) / 255.0f;
    const float b = static_cast<float>(sumB / count) / 255.0f;
    entry.statR = r;
    entry.statG = g;
    entry.statB = b;
    entry.statLuma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    entry.statRg = g > 0.0001f ? r / g : 0.0f;
    entry.statBg = g > 0.0001f ? b / g : 0.0f;
    const float maximum = std::max({r, g, b});
    const float minimum = std::min({r, g, b});
    entry.statSat = maximum > 0.0001f ? (maximum - minimum) / maximum : 0.0f;
    entry.statsLines = buildStatsLines(entry);
    entry.hasStats = true;
}

void toggleLocalMode() {
    if (!imageToolsEnabled()) return;
    state().localMode = !state().localMode;
    if (!state().localMode) clearSelections();
    app::requestUpdate();
}

// --- histogram (P5) -----------------------------------------------------------

// Luma distribution of a decoded image. Built on the worker thread so the UI
// thread never scans the pixels.
struct LumaHistogram {
    std::array<std::uint32_t, 256> bins{};
    std::uint32_t maximum = 0;
    bool valid = false;
};

LumaHistogram buildHistogram(const std::vector<std::uint8_t>& pixels, int width, int height) {
    LumaHistogram histogram;
    if (width <= 0 || height <= 0) return histogram;
    const std::size_t pixelCount =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pixels.size() < pixelCount * 4u) return histogram;

    // Subsample large images: the shape of the distribution is what matters.
    const std::size_t stride = pixelCount > 4000000u ? 4u : 1u;
    for (std::size_t index = 0; index < pixelCount; index += stride) {
        const std::uint8_t* pixel = pixels.data() + index * 4u;
        const std::uint32_t luma = static_cast<std::uint32_t>(
            0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2]);
        ++histogram.bins[std::min<std::uint32_t>(luma, 255u)];
    }
    for (const std::uint32_t count : histogram.bins) {
        histogram.maximum = std::max(histogram.maximum, count);
    }
    histogram.valid = histogram.maximum > 0;
    return histogram;
}

// --- cell operations (P3) ------------------------------------------------------

void rotateImage(int displayIndex) {
    if (!imageToolsEnabled() || displayIndex < 0 ||
        displayIndex >= static_cast<int>(state().images.size())) {
        return;
    }
    ImageEntry& entry = state().images[static_cast<std::size_t>(displayIndex)];
    if (entry.isVideo) return;
    entry.rotation = (entry.rotation + 1) % 4;
    entry.hasSelection = false;
    entry.hasStats = false;
    app::requestUpdate();
}

void removeImage(int id) {
    // Review mode forbids removing images: the pair would break.
    if (state().reviewMode) return;
    auto& images = state().images;
    const auto it = std::find_if(images.begin(), images.end(),
                                 [id](const ImageEntry& entry) { return entry.id == id; });
    if (it == images.end()) return;
    images.erase(it);
    if (state().moveSourceId == id || state().moveTargetId == id) {
        state().moveSourceId = -1;
        state().moveTargetId = -1;
    }
    if (state().soloId == id) {
        state().soloId = -1;
        state().peekId = -1;
    }
    if (state().peekId == id) {
        state().peekId = -1;
    }
    if (state().activeVideoId == id) {
        state().activeVideoId = -1;
    }
    state().syncHandledEnded.erase(
        std::remove(state().syncHandledEnded.begin(), state().syncHandledEnded.end(), id),
        state().syncHandledEnded.end());
    if (images.empty()) {
        state().zoom = 1.0f;
        resetPans();
        state().qSwapActive = false;
        state().localMode = false;
        state().activeVideoId = -1;
        clearSelections();
    }
    enforceMediaMode();
    enforceDiffMode();
    app::requestUpdate();
}

// Applies the same relative selection to every image and recomputes statistics.
void applySharedSelection(float x0, float y0, float x1, float y1) {
    for (auto& item : state().images) {
        item.hasSelection = true;
        item.selX0 = x0;
        item.selY0 = y0;
        item.selX1 = x1;
        item.selY1 = y1;
        computeStats(item);
    }
    app::requestUpdate();
}

void beginQSwap() {
    // Compare helper: while Q is held the right image is drawn over the left
    // cell. Order is never changed, so this is safe in review mode too.
    if (!imageToolsEnabled() || state().qSwapActive || state().images.size() != 2) return;
    state().qSwapActive = true;
    // The cover cell switches to another stream; make sure it has a frame.
    ImageEntry& right = state().images[1];
    submitFrame(right, right.altStream);
    app::requestUpdate();
}

void endQSwap() {
    if (!state().qSwapActive) return;
    state().qSwapActive = false;
    if (state().images.empty()) return;
    // The left cell returns to its own stream, which needs a fresh frame.
    ImageEntry& left = state().images[0];
    submitFrame(left, left.stream);
    app::requestUpdate();
}

// --- input (command line) ----------------------------------------------------

struct LaunchRequest {
    std::vector<std::string> paths;
    bool newWindow = false;
    bool uninstallIntegration = false;
};

LaunchRequest launchRequest() {
    LaunchRequest request;
#if defined(_WIN32)
    int count = 0;
    LPWSTR* list = CommandLineToArgvW(GetCommandLineW(), &count);
    if (list != nullptr && count > 1) {
        for (int i = 1; i < count; ++i) {
            std::string value = imview::utf8::narrow(list[i]);
            if (value.empty()) continue;
            if (value == "--new-window") {
                request.newWindow = true;
            } else if (value == "--uninstall-integration") {
                request.uninstallIntegration = true;
            } else {
                request.paths.push_back(std::move(value));
            }
        }
    }
    if (list != nullptr) LocalFree(list);
#endif
    return request;
}

// --- loading ------------------------------------------------------------------

void startLoad(const std::string& path) {
    ImageEntry entry;
    entry.id = state().nextId++;
    entry.path = path;
    entry.name = std::filesystem::u8path(path).filename().u8string();
    entry.stream = std::make_shared<eui::ImageStream>(1);
    entry.altStream = std::make_shared<eui::ImageStream>(1);
    entry.loading = true;
    // The file name is available immediately; the rest follows on load.
    entry.exifLines.push_back(entry.name);
    state().images.push_back(std::move(entry));

    const int id = state().images.back().id;
    struct Decoded {
        std::shared_ptr<std::vector<std::uint8_t>> pixels;
        int width = 0;
        int height = 0;
        LumaHistogram histogram;
        imview::ExifInfo exif;
        std::vector<std::string> externalExif;
    };
    // restart, not runOnce: re-opening the same file while a previous decode is
    // still running must supersede it. runOnce would be rejected and the new
    // cell would stay on "Loading..." forever.
    const bool accepted = async::restart(
        "imview.load." + path,
        [path]() -> async::Result<Decoded> {
            Decoded decoded;
            decoded.pixels = std::make_shared<std::vector<std::uint8_t>>();
            std::string reason;
            if (!imview::decodeRgba(path, decoded.width, decoded.height,
                                    *decoded.pixels, reason)) {
                return async::failure<Decoded>(std::move(reason));
            }
            decoded.histogram =
                buildHistogram(*decoded.pixels, decoded.width, decoded.height);
            // A user-supplied `imexif` helper wins over the built-in reader.
            imview::shell::readExternalExif(path, decoded.externalExif);
            if (decoded.externalExif.empty()) {
                decoded.exif = imview::readExif(path);
            }
            return async::success(std::move(decoded));
        },
        [id](const async::Result<Decoded>& result) {
            ImageEntry* entry = findEntry(id);
            if (entry == nullptr) return;
            entry->loading = false;
            if (result.ok) {
                entry->pixels = result.value.pixels; // shared_ptr: no pixel copy on the UI thread
                entry->width = result.value.width;
                entry->height = result.value.height;
                entry->loaded = true;
                entry->histogram = result.value.histogram.bins;
                entry->histogramMax = result.value.histogram.maximum;
                entry->hasHistogram = result.value.histogram.valid;
                entry->exif = result.value.exif;
                entry->customExif = result.value.externalExif;
                entry->exifLines = buildExifLines(*entry);
                // Both mailboxes need a frame: one for the cell, one for the
                // Q cover or a later mode switch.
                submitFrame(*entry, entry->stream);
                submitFrame(*entry, entry->altStream);
                entry->streamPrimed = true;
            } else {
                entry->failed = true;
            }
            // A failed decode, or a pair that finished loading with mismatched
            // dimensions, leaves diff mode; a partially loaded pair stays.
            enforceDiffMode();
            app::requestUpdate();
        });

    if (!accepted) {
        if (ImageEntry* queued = findEntry(id)) {
            queued->loading = false;
            queued->failed = true;
            enforceDiffMode();
        }
    }
}

void startVideoLoad(const std::string& path) {
    ImageEntry entry;
    entry.id = state().nextId++;
    entry.path = path;
    entry.name = std::filesystem::u8path(path).filename().u8string();
    entry.isVideo = true;
    entry.video = std::make_shared<imview::video::Player>();
    entry.loading = true;
    const int id = entry.id;
    state().images.push_back(std::move(entry));
    state().activeVideoId = id;
    ImageEntry& added = state().images.back();
    added.video->setVolume(state().volume);
    added.video->setMuted(state().muted);
    added.video->open(path);
    if (state().syncMode) {
        // A newly added video joins a paused sync group.
        pauseAllVideos();
    } else {
        // Default playback is exclusive: the new video starts, older ones stop.
        pauseOtherVideos(id);
    }
    app::requestUpdate();
}

void addPaths(const std::vector<std::string>& paths, bool fromSidebar,
              bool preserveDiff, bool preserveSidebar) {
    struct Candidate {
        std::string path;
        bool video = false;
    };
    std::vector<Candidate> accepted;
    for (const std::string& path : paths) {
        // A directory argument opens the sidebar instead of a grid cell.
        std::error_code error;
        if (std::filesystem::is_directory(std::filesystem::u8path(path), error) && !error) {
            addFolder(path);
            continue;
        }
        const bool video = isVideoPath(path);
        if (!video && !isImagePath(path)) continue;
        if (state().images.size() + accepted.size() >= kMaxImages) break;
        // Case-insensitive, like the sidebar's "already open" dot: on Windows
        // the same file can arrive with different capitalisation.
        const bool present = std::any_of(
            state().images.begin(), state().images.end(),
            [&](const ImageEntry& entry) { return samePath(entry.path, path); });
        if (present) continue;
        const bool queued = std::any_of(
            accepted.begin(), accepted.end(),
            [&](const Candidate& other) { return samePath(other.path, path); });
        if (queued) continue;
        accepted.push_back({path, video});
    }

    bool acceptedVideo = false;
    for (const Candidate& candidate : accepted) {
        if (candidate.video) {
            startVideoLoad(candidate.path);
            acceptedVideo = true;
        } else {
            startLoad(candidate.path);
        }
    }
    if (!accepted.empty()) {
        enforceMediaMode();
        if (!preserveDiff) enforceDiffMode();
        // Videos keep the sidebar; Explorer add requests and sidebar-driven
        // additions preserve whatever the user already has.
        const bool keepSidebar = fromSidebar || preserveSidebar ||
                                 state().reviewMode || acceptedVideo ||
                                 hasVideoMedia();
        if (!keepSidebar) state().sidebarVisible = false;
        app::requestUpdate();
    }
}

// --- file drop ----------------------------------------------------------------

bool isDirectoryPath(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_directory(std::filesystem::u8path(path), error) && !error;
}

// A file drop is accepted only when at least one image or video would actually
// be opened (the grid is not full and an image is not already open).
bool hasNewDroppedImage(const std::vector<std::string>& paths) {
    if (state().images.size() >= kMaxImages) return false;
    for (const std::string& path : paths) {
        if (isVideoPath(path)) return true;
        if (!isImagePath(path)) continue;
        if (!isOpenInGrid(path)) return true;
    }
    return false;
}

// Folder drops add to free panes only; a full sidebar has no drop target, and a
// folder that is already open would not fill a pane.
bool hasDroppedFolderSlot(const std::vector<std::string>& paths) {
    if (state().folders.size() >= 2) return false;
    for (const std::string& path : paths) {
        if (!isDirectoryPath(path)) continue;
        const bool alreadyOpen = std::any_of(
            state().folders.begin(), state().folders.end(),
            [&](const AppState::FolderPane& pane) { return samePath(pane.dir, path); });
        if (!alreadyOpen) return true;
    }
    return false;
}

bool handleFileDrop(const imview::frame::FileDropEvent& event) {
    const bool overSidebar = state().sidebarVisible && event.y >= kToolbarHeight &&
                             event.x < state().sidebarWidth;
    const bool overMain = event.y >= kToolbarHeight &&
                          (!state().sidebarVisible || event.x >= state().sidebarWidth);

    bool accepted = false;
    AppState::DropTarget target = AppState::DropTarget::None;
    if (!state().reviewMode) {
        if (overMain && event.hasFiles) {
            accepted = hasNewDroppedImage(event.paths);
            if (accepted) target = AppState::DropTarget::Main;
        } else if (overSidebar && event.hasDirectories) {
            accepted = hasDroppedFolderSlot(event.paths);
            if (accepted) target = AppState::DropTarget::Sidebar;
        }
    }

    if (event.phase == imview::frame::FileDropPhase::Leave) {
        if (state().dropTarget != AppState::DropTarget::None) {
            state().dropTarget = AppState::DropTarget::None;
            app::requestUpdate();
        }
        return false;
    }

    if (event.phase == imview::frame::FileDropPhase::Drop) {
        if (state().dropTarget != AppState::DropTarget::None) {
            state().dropTarget = AppState::DropTarget::None;
            app::requestUpdate();
        }
        if (!accepted) return false;
        if (target == AppState::DropTarget::Main) {
            std::vector<std::string> files;
            for (const std::string& path : event.paths) {
                if (!isDirectoryPath(path) && (isImagePath(path) || isVideoPath(path))) {
                    files.push_back(path);
                }
            }
            if (files.empty()) return false;
            // fromSidebar=true keeps the sidebar visible while adding to the grid.
            addPaths(files, true);
        } else {
            std::vector<std::string> directories;
            for (const std::string& path : event.paths) {
                if (isDirectoryPath(path)) directories.push_back(path);
            }
            if (directories.empty()) return false;
            addFoldersFromDrop(directories);
        }
        return true;
    }

    if (state().dropTarget != target) {
        state().dropTarget = target;
        app::requestUpdate();
    }
    return accepted;
}

// --- file dialog ---------------------------------------------------------------

void openImages() {
    // Review mode only allows opening images through the sidebar double-click.
    if (state().reviewMode) return;
    eui::platform::FileDialogOptions options;
    options.prompt = "Open";
    std::vector<std::string> extensions;
    for (const auto& extension : imview::media::imageExtensions()) {
        extensions.push_back(extension.substr(1));
    }
    for (const auto& extension : imview::media::videoExtensions()) {
        extensions.push_back(extension.substr(1));
    }
    options.allowedExtensions = std::move(extensions);
    options.filterName = "Images and videos";
    options.allowMultiple = true;
    const eui::platform::FileDialogResult result = eui::platform::openFileDialog(options);
    if (result.selected()) addPaths(result.paths);
}

// --- Explorer / second instance -------------------------------------------------

bool canAcceptExternalPaths(const std::vector<std::string>& paths) {
    bool hasMedia = false;
    bool hasFolder = false;
    std::vector<std::string> additional;
    for (const std::string& path : paths) {
        std::error_code error;
        if (std::filesystem::is_directory(std::filesystem::u8path(path), error) && !error) {
            hasFolder = true;
            continue;
        }
        if (!isImagePath(path) && !isVideoPath(path)) continue;
        hasMedia = true;
        if (isOpenInGrid(path)) continue;
        const bool queued = std::any_of(
            additional.begin(), additional.end(),
            [&](const std::string& other) { return samePath(other, path); });
        if (!queued) additional.push_back(path);
    }
    if (!hasMedia && !hasFolder) return false;
    return state().images.size() + additional.size() <= kMaxImages;
}

bool handleExternalOpen(const std::vector<std::string>& paths) {
    if (!canAcceptExternalPaths(paths)) return false;
    if (state().reviewMode) exitReviewMode();
    addPaths(paths, true, false, true);
    imview::frame::activateWindow();
    return true;
}

std::string launchFolder(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        if (isDirectoryPath(path)) return path;
    }
    for (const std::string& path : paths) {
        if (!isImagePath(path) && !isVideoPath(path)) continue;
        const std::filesystem::path parent =
            std::filesystem::u8path(path).parent_path();
        if (!parent.empty()) return parent.u8string();
    }
    return {};
}

void shutdownMedia() {
    for (auto& entry : state().images) {
        if (entry.video) entry.video->close();
    }
    state().images.clear();
}

// --- diff image ---------------------------------------------------------------

struct DiffImageResult {
    std::shared_ptr<std::vector<std::uint8_t>> pixels;
    int width = 0;
    int height = 0;
};

// One pixel at a time, with no resampling: the two decoded images must have the
// same dimensions. The per-pixel class is chosen by the largest absolute channel
// difference (grey for identical pixels, blue inside the tolerance, red above
// it), then blended 25% left, 25% right and 50% class colour.
bool buildDiffImage(const std::vector<std::uint8_t>& left, int leftWidth, int leftHeight,
                    const std::vector<std::uint8_t>& right, int rightWidth, int rightHeight,
                    float threshold, const async::CancelToken& token,
                    DiffImageResult& result) {
    if (leftWidth <= 0 || leftHeight <= 0 || leftWidth != rightWidth ||
        leftHeight != rightHeight) {
        return false;
    }
    const int width = leftWidth;
    const int height = leftHeight;
    const std::size_t pixelCount =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (left.size() < pixelCount * 4u || right.size() < pixelCount * 4u) {
        return false;
    }

    result.width = width;
    result.height = height;
    result.pixels = std::make_shared<std::vector<std::uint8_t>>(pixelCount * 4u);
    auto& output = *result.pixels;

    constexpr std::uint8_t kBlue[3] = {0x2F, 0x81, 0xF7};
    constexpr std::uint8_t kRed[3] = {0xE5, 0x53, 0x4B};
    constexpr std::uint8_t kGrey[3] = {0x6E, 0x6E, 0x6E};
    const auto blend = [](int leftValue, int rightValue, int colourValue) {
        return static_cast<std::uint8_t>(
            (5 * leftValue + 5 * rightValue + 10 * colourValue + 10) / 20);
    };

    for (int y = 0; y < height; ++y) {
        // Threshold edits restart the task; abort promptly so stale work cannot
        // pile up behind the slider.
        if (y % 32 == 0 && token.canceled()) return false;
        const std::uint8_t* leftRow = left.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
        const std::uint8_t* rightRow = right.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
        std::uint8_t* outputRow = output.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;

        for (int x = 0; x < width; ++x) {
            const std::uint8_t* leftPixel = leftRow + static_cast<std::size_t>(x) * 4u;
            const std::uint8_t* rightPixel = rightRow + static_cast<std::size_t>(x) * 4u;
            const int deltaR = static_cast<int>(rightPixel[0]) - static_cast<int>(leftPixel[0]);
            const int deltaG = static_cast<int>(rightPixel[1]) - static_cast<int>(leftPixel[1]);
            const int deltaB = static_cast<int>(rightPixel[2]) - static_cast<int>(leftPixel[2]);
            const int strongest =
                std::max({std::abs(deltaR), std::abs(deltaG), std::abs(deltaB)});

            const std::uint8_t* colour = kGrey;
            if (strongest != 0) {
                colour = static_cast<float>(strongest) <= threshold ? kBlue : kRed;
            }
            std::uint8_t* pixel = outputRow + static_cast<std::size_t>(x) * 4u;
            pixel[0] = blend(leftPixel[0], rightPixel[0], colour[0]);
            pixel[1] = blend(leftPixel[1], rightPixel[1], colour[1]);
            pixel[2] = blend(leftPixel[2], rightPixel[2], colour[2]);
            pixel[3] = 255;
        }
    }
    return true;
}

// Starts a worker rebuild whenever the pair or the threshold changed. The
// image is not part of `images`; it lives in AppState::diffImage.
void updateDiffImage() {
    AppState& app = state();
    if (!app.diffMode || app.images.size() != 2) return;
    ImageEntry& left = app.images[0];
    ImageEntry& right = app.images[1];

    const bool samePair = app.diffRequestLeftId == left.id &&
                          app.diffRequestRightId == right.id &&
                          app.diffRequestLeftPixels == left.pixels.get() &&
                          app.diffRequestRightPixels == right.pixels.get();
    if (left.failed || right.failed ||
        (left.loaded && right.loaded && !diffPairReady())) {
        exitDiffMode();
        return;
    }
    if (!diffPairReady()) {
        if (!samePair) {
            ++app.diffSerial; // invalidate an in-flight build for another pair
            // Remember the pair while its images decode; the threshold marker
            // stays invalid so the build starts as soon as both are loaded.
            app.diffRequestLeftId = left.id;
            app.diffRequestRightId = right.id;
            app.diffRequestLeftPixels = left.pixels.get();
            app.diffRequestRightPixels = right.pixels.get();
            app.diffRequestThreshold = -1.0f;
            resetDiffImageData();
            app.diffImage.loading = true;
        }
        return;
    }
    if (samePair && app.diffRequestThreshold == app.diffThreshold) return;

    const int serial = ++app.diffSerial;
    app.diffRequestLeftId = left.id;
    app.diffRequestRightId = right.id;
    app.diffRequestLeftPixels = left.pixels.get();
    app.diffRequestRightPixels = right.pixels.get();
    app.diffRequestThreshold = app.diffThreshold;
    if (!samePair) {
        resetDiffImageData();
        app.diffImage.loading = true;
    }

    const std::shared_ptr<std::vector<std::uint8_t>> leftPixels = left.pixels;
    const std::shared_ptr<std::vector<std::uint8_t>> rightPixels = right.pixels;
    const int leftWidth = left.width;
    const int leftHeight = left.height;
    const int rightWidth = right.width;
    const int rightHeight = right.height;
    const float threshold = app.diffThreshold;

    async::restart(
        "imview.diff",
        [leftPixels, leftWidth, leftHeight, rightPixels, rightWidth, rightHeight,
         threshold](const async::CancelToken& token) -> async::Result<DiffImageResult> {
            DiffImageResult result;
            if (!buildDiffImage(*leftPixels, leftWidth, leftHeight,
                                *rightPixels, rightWidth, rightHeight,
                                threshold, token, result)) {
                return async::failure<DiffImageResult>("diff canceled or empty");
            }
            return async::success(std::move(result));
        },
        [serial](const async::Result<DiffImageResult>& result) {
            AppState& app = state();
            if (serial != app.diffSerial || !app.diffMode) return;
            ImageEntry& diff = app.diffImage;
            diff.loading = false;
            if (!result.ok) {
                diff.failed = true;
                diff.loaded = false;
                app::requestUpdate();
                return;
            }
            diff.pixels = result.value.pixels;
            diff.width = result.value.width;
            diff.height = result.value.height;
            diff.loaded = true;
            diff.failed = false;
            diff.streamPrimed = false;
            diff.gpuImage.reset();
            diff.gpuRevision = 0;
#if defined(EUI_RENDER_BACKEND_OPENGL)
            diff.gpuSource.reset();
#endif
            submitFrame(diff, diff.stream);
            diff.streamPrimed = true;
            app::requestUpdate();
        });
}

// --- compose -------------------------------------------------------------------

// Toolbar building blocks -----------------------------------------------------

// Small square toggle chip used for the E / H / P modes.
void composeToggleChip(eui::Ui& ui, const std::string& id, float x, float y,
                       float size, const std::string& label, bool active,
                       const std::function<void()>& onTap, bool enabled = true) {
    const eui::Color base = active ? kAccent : kChipIdle;
    const eui::Color hover = active ? kAccentHover : kChipIdleHover;
    const eui::Color pressed = active ? kAccentPressed : kChipPressed;
    ui.stack(id).position(x, y).size(size, size).content([&] {
        ui.rect(id + ".bg").size(size, size).color(base).radius(kRadius)
            .states(base, enabled ? hover : base, enabled ? pressed : base).build();
        ui.text(id + ".label").size(size, size).text(label)
            .color(active ? kWhite
                          : (enabled ? kTextSecondary : eui::Color{1.0f, 1.0f, 1.0f, 0.24f}))
            .fontSize(12.0f).fontWeight(400)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center).build();
        ui.rect(id + ".hit").size(size, size)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(enabled ? onTap : std::function<void()>{})
            .cursor(enabled ? eui::CursorShape::Hand : eui::CursorShape::Arrow)
            .build();
    }).build();
}

// Transparent surface that only consumes clicks.
void composeClickArea(eui::Ui& ui, const std::string& id, float width, float height,
                      const std::function<void()>& onClick) {
    ui.rect(id).size(width, height)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick(onClick)
        .cursor(eui::CursorShape::Hand)
        .build();
}

// Single-line text clipped to a rectangle, used by the About dialog.
void composeClippedText(eui::Ui& ui, const std::string& id, float x, float y,
                        float width, float height, const std::string& text,
                        const eui::Color& color, float fontSize,
                        eui::HorizontalAlign align = eui::HorizontalAlign::Left) {
    ui.stack(id + ".clip").position(x, y).size(width, height).clip().content([&] {
        ui.text(id + ".text")
            .position(0.0f, 0.0f)
            .size(width, height)
            .text(text)
            .color(color)
            .fontSize(fontSize)
            .horizontalAlign(align)
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
    }).build();
}

// Text button used by the About dialog.
void composeTextButton(eui::Ui& ui, const std::string& id, float x, float y,
                       float width, float height, const std::string& label,
                       bool primary, bool enabled,
                       const std::function<void()>& onTap) {
    const eui::Color base = primary ? kAccent : kChipIdle;
    const eui::Color hover = primary ? kAccentHover : kChipIdleHover;
    const eui::Color pressed = primary ? kAccentPressed : kChipPressed;
    ui.stack(id).position(x, y).size(width, height).content([&] {
        ui.rect(id + ".bg").size(width, height)
            .color(base)
            .radius(kRadius)
            .border(primary ? 0.0f : 1.0f, primary ? kAccent : kBorderStrong)
            .states(enabled ? base : eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? hover : eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? pressed : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .build();
        ui.text(id + ".label").size(width, height).text(label)
            .color(primary ? (enabled ? kWhite : eui::Color{1.0f, 1.0f, 1.0f, 0.38f})
                          : (enabled ? kTextPrimary : kTextSecondary))
            .fontSize(12.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
        ui.rect(id + ".hit").size(width, height)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(enabled ? onTap : std::function<void()>{})
            .cursor(enabled ? eui::CursorShape::Hand : eui::CursorShape::Arrow)
            .build();
    }).build();
}

// Small square text button used by the sidebar pane header.
void composePaneHeaderButton(eui::Ui& ui, const std::string& id, float x, float y,
                             float size, const std::string& label,
                             const std::function<void()>& onTap) {
    ui.stack(id).position(x, y).size(size, size).content([&] {
        ui.rect(id + ".bg").size(size, size)
            .color({0.0f, 0.0f, 0.0f, 0.0f}).radius(kRadius)
            .states({0.0f, 0.0f, 0.0f, 0.0f},
                    {1.0f, 1.0f, 1.0f, 0.10f},
                    {1.0f, 1.0f, 1.0f, 0.16f})
            .build();
        ui.text(id + ".label").size(size, size).text(label)
            .color(kTextSecondary).fontSize(11.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center).build();
        composeClickArea(ui, id + ".hit", size, size, onTap);
    }).build();
}

// Small window control (minimize / maximize / close).
void composeWindowButton(eui::Ui& ui, const std::string& id, float x, float y,
                         float width, float height, int glyph, bool closeStyle,
                         const std::function<void()>& onTap) {
    const eui::Color hover = closeStyle ? kWindowCloseHover : kWindowControlHover;
    ui.stack(id).position(x, y).size(width, height).content([&] {
        ui.rect(id + ".bg").size(width, height)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .states({0.0f, 0.0f, 0.0f, 0.0f}, hover, closeStyle ? hover : kChipPressed)
            .build();
        // glyph: 0 = minimize (bar), 1 = maximize/restore (square), 2 = close (x)
        const eui::Color ink = kTextSecondary;
        const float centerX = width * 0.5f;
        const float centerY = height * 0.5f;
        const float glyphLine = crispLine(1.0f);
        if (glyph == 0) {
            ui.rect(id + ".g")
                .position(snapToPixel(centerX - 5.0f), snapToPixel(centerY))
                .size(10.0f, glyphLine).color(ink).build();
        } else if (glyph == 1) {
            ui.rect(id + ".g")
                .position(snapToPixel(centerX - 4.5f), snapToPixel(centerY - 4.5f))
                .size(9.0f, 9.0f).color({0.0f, 0.0f, 0.0f, 0.0f})
                .border(glyphLine, ink).build();
        } else {
            // Match the visual weight of the 11px square / 13px bar. After the
            // 45° rotation the ink box is about (length + thickness) / sqrt(2).
            const float length = 14.0f;
            const float thickness = 1.0f;
            ui.rect(id + ".g1").position(centerX - length * 0.5f,
                                          centerY - thickness * 0.5f)
                .size(length, thickness).color(ink).rotate(0.785398f).build();
            ui.rect(id + ".g2").position(centerX - length * 0.5f,
                                          centerY - thickness * 0.5f)
                .size(length, thickness).color(ink).rotate(-0.785398f).build();
        }
        ui.rect(id + ".hit").size(width, height)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(onTap)
            .cursor(eui::CursorShape::Hand)
            .build();
    }).build();
}

// Diff threshold control in the title bar: label, slider and value. The track
// and knob are drawn directly so the whole band is one drag surface; the native
// `components::slider` stays out of the toolbar because its nested hit target
// sits inside the window's caption band.
void composeDiffThresholdSlider(eui::Ui& ui, float x, float y) {
    constexpr float kHeight = 20.0f;
    constexpr float kLabelWidth = 28.0f;
    constexpr float kSliderWidth = 120.0f;
    constexpr float kValueWidth = 28.0f;
    constexpr float kGap = 4.0f;
    constexpr float kWidth =
        kLabelWidth + kGap + kSliderWidth + kGap + kValueWidth;
    constexpr float kTrackHeight = 4.0f;
    constexpr float kKnobSize = 12.0f;
    const float value = std::clamp(state().diffThreshold / 20.0f, 0.0f, 1.0f);
    const float trackY = (kHeight - kTrackHeight) * 0.5f;
    const float knobX = value * (kSliderWidth - kKnobSize);

    ui.stack("toolbar.diff").position(x, y).size(kWidth, kHeight).content([&] {
        ui.text("toolbar.diff.label").position(0.0f, 0.0f)
            .size(kLabelWidth, kHeight).text("Diff")
            .color(kTextSecondary).fontSize(11.0f)
            .verticalAlign(eui::VerticalAlign::Center).build();

        const float sliderX = kLabelWidth + kGap;
        ui.rect("toolbar.diff.track")
            .position(snapToPixel(sliderX), snapToPixel(trackY))
            .size(kSliderWidth, kTrackHeight)
            .color({1.0f, 1.0f, 1.0f, 0.16f})
            .radius(kTrackHeight * 0.5f).build();
        if (value > 0.0f) {
            ui.rect("toolbar.diff.fill")
                .position(snapToPixel(sliderX), snapToPixel(trackY))
                .size(std::max(kTrackHeight, kSliderWidth * value), kTrackHeight)
                .color(kAccent).radius(kTrackHeight * 0.5f).build();
        }
        ui.rect("toolbar.diff.knob")
            .position(snapToPixel(sliderX + knobX),
                      snapToPixel((kHeight - kKnobSize) * 0.5f))
            .size(kKnobSize, kKnobSize).radius(kKnobSize * 0.5f)
            .color(kTextPrimary).build();

        ui.text("toolbar.diff.value")
            .position(sliderX + kSliderWidth + kGap, 0.0f)
            .size(kValueWidth, kHeight)
            .text(std::to_string(static_cast<int>(std::lround(state().diffThreshold))))
            .color(kTextPrimary).fontSize(11.0f)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .verticalAlign(eui::VerticalAlign::Center).build();

        ui.rect("toolbar.diff.hit")
            .position(sliderX, 0.0f).size(kSliderWidth, kHeight)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .acceptedButtons(eui::PointerButton::Left)
            .onPress([](const core::PointerEvent& event, const core::Rect& bounds) {
                if (event.button != eui::PointerButton::Left) return;
                const float fraction = bounds.width > 0.0f
                    ? std::clamp((static_cast<float>(event.x) - bounds.x) / bounds.width,
                                 0.0f, 1.0f)
                    : 0.0f;
                state().diffThreshold = fraction * 20.0f;
                state().diffSliderDragStart = state().diffThreshold;
                app::requestUpdate();
            })
            .onDrag([](const core::dsl::DragEvent& event) {
                const float scale = imview::frame::dpiScale();
                const float delta = scale > 0.0f
                    ? static_cast<float>(event.totalX) / scale
                    : static_cast<float>(event.totalX);
                state().diffThreshold = std::clamp(
                    state().diffSliderDragStart + delta / 120.0f * 20.0f, 0.0f, 20.0f);
                app::requestUpdate();
            })
            .cursor(eui::CursorShape::Hand)
            .build();
    }).build();
}

// Global volume control in the title bar, using the same label/slider/value
// geometry as the diff threshold. Every video in the window shares it.
void composeVolumeSlider(eui::Ui& ui, float x, float y) {
    constexpr float kHeight = 20.0f;
    constexpr float kLabelWidth = 28.0f;
    constexpr float kSliderWidth = 120.0f;
    constexpr float kValueWidth = 34.0f;
    constexpr float kGap = 4.0f;
    constexpr float kMuteWidth = 20.0f;
    constexpr float kTrackHeight = 4.0f;
    constexpr float kKnobSize = 12.0f;
    const float value = std::clamp(state().volume, 0.0f, 1.0f);
    const float trackY = (kHeight - kTrackHeight) * 0.5f;
    const float knobX = value * (kSliderWidth - kKnobSize);

    ui.stack("toolbar.volume").position(x, y).size(
        kLabelWidth + kGap + kSliderWidth + kGap + kValueWidth + 2.0f + kMuteWidth,
        kHeight).content([&] {
        ui.text("toolbar.volume.label").position(0.0f, 0.0f)
            .size(kLabelWidth, kHeight).text("Vol")
            .color(kTextSecondary).fontSize(11.0f)
            .verticalAlign(eui::VerticalAlign::Center).build();

        const float sliderX = kLabelWidth + kGap;
        ui.rect("toolbar.volume.track")
            .position(snapToPixel(sliderX), snapToPixel(trackY))
            .size(kSliderWidth, kTrackHeight)
            .color({1.0f, 1.0f, 1.0f, 0.16f})
            .radius(kTrackHeight * 0.5f).build();
        if (value > 0.0f) {
            ui.rect("toolbar.volume.fill")
                .position(snapToPixel(sliderX), snapToPixel(trackY))
                .size(std::max(kTrackHeight, kSliderWidth * value), kTrackHeight)
                .color(state().muted ? kTextSecondary : kAccent)
                .radius(kTrackHeight * 0.5f).build();
        }
        ui.rect("toolbar.volume.knob")
            .position(snapToPixel(sliderX + knobX),
                      snapToPixel((kHeight - kKnobSize) * 0.5f))
            .size(kKnobSize, kKnobSize).radius(kKnobSize * 0.5f)
            .color(kTextPrimary).build();

        ui.text("toolbar.volume.value")
            .position(sliderX + kSliderWidth + kGap, 0.0f)
            .size(kValueWidth, kHeight)
            .text(std::to_string(static_cast<int>(std::lround(value * 100.0f))) + "%")
            .color(kTextPrimary).fontSize(11.0f)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .verticalAlign(eui::VerticalAlign::Center).build();

        ui.rect("toolbar.volume.hit")
            .position(sliderX, 0.0f).size(kSliderWidth, kHeight)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .acceptedButtons(eui::PointerButton::Left)
            .onPress([](const core::PointerEvent& event, const core::Rect& bounds) {
                if (event.button != eui::PointerButton::Left || bounds.width <= 0.0f) return;
                const float fraction = std::clamp(
                    static_cast<float>((event.x - bounds.x) / bounds.width), 0.0f, 1.0f);
                state().volumeDragStart = fraction;
                state().volume = fraction;
                state().muted = false;
                applyGlobalVolumeToPlayers();
                app::requestUpdate();
            })
            .onDrag([](const core::dsl::DragEvent& event) {
                if (event.button != eui::PointerButton::Left) return;
                const float scale = imview::frame::dpiScale();
                const float delta = scale > 0.0f
                    ? static_cast<float>(event.totalX) / scale
                    : static_cast<float>(event.totalX);
                state().volume = std::clamp(
                    state().volumeDragStart + delta / 120.0f, 0.0f, 1.0f);
                state().muted = false;
                applyGlobalVolumeToPlayers();
                app::requestUpdate();
            })
            .cursor(eui::CursorShape::Hand)
            .build();

        const float muteX = sliderX + kSliderWidth + kGap + kValueWidth + 2.0f;
        ui.stack("toolbar.volume.mute").position(muteX, 0.0f)
            .size(kMuteWidth, kHeight).content([&] {
                const eui::Color base = state().muted ? kAccent : kChipIdle;
                const eui::Color hover = state().muted ? kAccentHover : kChipIdleHover;
                const eui::Color pressed = state().muted ? kAccentPressed : kChipPressed;
                ui.rect("toolbar.volume.mute.bg").size(kMuteWidth, kHeight)
                    .color(base).radius(3.0f).states(base, hover, pressed).build();
                ui.text("toolbar.volume.mute.label").size(kMuteWidth, kHeight)
                    .text("M").color(state().muted ? kWhite : kTextSecondary)
                    .fontSize(10.0f)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center).build();
                ui.rect("toolbar.volume.mute.hit").size(kMuteWidth, kHeight)
                    .color({0.0f, 0.0f, 0.0f, 0.0f})
                    .onClick([] {
                        state().muted = !state().muted;
                        applyGlobalVolumeToPlayers();
                        app::requestUpdate();
                    })
                    .cursor(eui::CursorShape::Hand)
                    .build();
            }).build();
    }).build();
}

std::string formatVideoTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const long total = std::lround(seconds);
    const long hours = total / 3600;
    const long minutes = (total / 60) % 60;
    const long remainder = total % 60;
    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%ld:%02ld:%02ld", hours, minutes, remainder);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02ld:%02ld", minutes, remainder);
    }
    return buffer;
}

void composeToolbar(eui::Ui& ui, const eui::Screen& screen) {
    const int count = static_cast<int>(state().images.size());
    const bool imageTools = imageToolsEnabled();
    const float controlHeight = 24.0f;
    const float centerY = (kToolbarHeight - controlHeight) * 0.5f;

    // Centre indicator: position, active modes and zoom, all in one run of text
    // so the numbers and separators stay tight.
    std::string status;
    if (count == 0) {
        status.clear();
    } else if (soloActive()) {
        status = std::to_string(indexOf(shownSoloId())) + " / " + std::to_string(count);
    } else if (state().reviewMode) {
        status = "Review " + std::to_string(reviewImageOrdinal(state().reviewIndex)) +
                 " / " + std::to_string(reviewPairCount());
        if (count == 1) status += "  single";
    } else {
        status.clear(); // the image count alone is not worth showing
    }

    if (imageTools) {
        std::string modes;
        const auto appendMode = [&modes](const char* label) {
            if (!modes.empty()) modes += "  \xc2\xb7  ";
            modes += label;
        };
        if (state().showExif) appendMode("EXIF");
        if (state().showHistogram) appendMode("Histogram");
        if (state().localMode) appendMode("Local");
        if (state().diffMode) appendMode("Diff");
        if (!modes.empty()) {
            if (!status.empty()) status += "  \xc2\xb7  ";
            status += modes;
        }
        const int zoomPercent = static_cast<int>(std::lround(state().zoom * 100.0f));
        if (zoomPercent > 100) {
            if (!status.empty()) status += "  \xc2\xb7  ";
            status += std::to_string(zoomPercent) + "%";
        }
    }
    if (state().syncMode) {
        if (!status.empty()) status += "  \xc2\xb7  ";
        status += "Sync";
    }

    ui.stack("toolbar").position(0.0f, 0.0f).size(screen.width, kToolbarHeight)
        .zIndex(100).content([&] {
        ui.rect("toolbar.bg").size(screen.width, kToolbarHeight)
            .color(kToolbarBackground).build();
        const float lineWidth = crispLine(1.0f);
        ui.rect("toolbar.line").position(0.0f, kToolbarHeight - lineWidth)
            .size(screen.width, lineWidth).color(kToolbarLine).build();

        // The empty part of the toolbar doubles as the title bar and is handled
        // natively via WM_NCHITTEST -> HTCAPTION (window_frame.cpp); nothing to
        // wire up here. Added first so the buttons stay clickable on top.
        ui.rect("toolbar.drag")
            .position(0.0f, 0.0f)
            .size(screen.width, kToolbarHeight)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .acceptedButtons(eui::PointerButton::Left)
            .onPress([](const core::PointerEvent& event, const core::Rect&) {
                if (event.button != eui::PointerButton::Left || !hasVideoMedia()) return;
                imview::frame::beginWindowMove();
            })
            .onDrag([](const core::dsl::DragEvent& event) {
                if (event.button == eui::PointerButton::Left) {
                    imview::frame::dragWindowToCursor();
                }
            })
            .onRelease([](const core::PointerEvent& event, const core::Rect&) {
                if (event.button == eui::PointerButton::Left) {
                    imview::frame::endWindowMove();
                }
            })
            .onClick([] {
                if (isDoubleClick(state().titleClick, 0, 0,
                                  core::window::timeSeconds())) {
                    imview::frame::toggleMaximizeWindow();
                }
            })
            .cursor(eui::CursorShape::Arrow)
            .build();

        // Left: the ImView menu button (VS Code style menu entry point).
        const float menuWidth = 66.0f;
        ui.stack("toolbar.menu").position(8.0f, centerY)
            .size(menuWidth, controlHeight).content([&] {
                const eui::Color base = kChipIdle;
                ui.rect("toolbar.menu.bg").size(menuWidth, controlHeight)
                    .color(base).radius(kRadius)
                    .states(base, kChipIdleHover, kChipPressed).build();
                ui.text("toolbar.menu.label").position(10.0f, 0.0f)
                    .size(menuWidth - 14.0f, controlHeight).text("ImView")
                    .color(kTextPrimary).fontSize(12.0f)
                    .verticalAlign(eui::VerticalAlign::Center).build();
                composeClickArea(ui, "toolbar.menu.hit", menuWidth, controlHeight,
                                 [] { openToolbarMenu(); });
            }).build();

        // Diff mode grows its threshold slider next to the menu; the window
        // frame widens the left control cluster while it is visible so the bar
        // can drag normally.
        if (state().diffMode) {
            composeDiffThresholdSlider(ui, 82.0f,
                                       (kToolbarHeight - 20.0f) * 0.5f);
        } else if (hasVideoMedia()) {
            composeVolumeSlider(ui, 82.0f, (kToolbarHeight - 20.0f) * 0.5f);
        }

        // Centre: status indicator.
        if (!status.empty()) {
            ui.text("toolbar.status").position(screen.width * 0.5f - 170.0f, 0.0f)
                .size(340.0f, kToolbarHeight)
                .text(status).color(kTextSecondary).fontSize(12.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();
        }

        // Right-aligned controls: [E][H][P]  [R]  |  [-][口][x]
        // Window buttons sit at the right end of the bar; the rest of the bar is
        // the title bar and is handled natively by the window manager (see
        // window_frame.cpp), so no drag handler is needed here.
        const float winButton = 36.0f;
        const float chipSize = controlHeight;
        float right = screen.width - 3.0f * winButton - 8.0f;

        right -= chipSize;
        composeToggleChip(ui, "toolbar.chip.s", right, centerY, chipSize, "S",
                          state().syncMode, [] { toggleSyncMode(); },
                          syncAvailable());
        right -= 2.0f + chipSize;
        composeToggleChip(ui, "toolbar.chip.r", right, centerY, chipSize, "R",
                          state().reviewMode, [] { toggleReviewMode(); },
                          reviewAvailable() && (!mixedMedia() || state().reviewMode));

        right -= 6.0f + chipSize;
        composeToggleChip(ui, "toolbar.chip.p", right, centerY, chipSize, "P",
                          state().localMode, [] { toggleLocalMode(); }, imageTools);
        right -= 2.0f + chipSize;
        composeToggleChip(ui, "toolbar.chip.h", right, centerY, chipSize, "H",
                          state().showHistogram, [] {
                              state().showHistogram = !state().showHistogram;
                              app::requestUpdate();
                          }, imageTools);
        right -= 2.0f + chipSize;
        composeToggleChip(ui, "toolbar.chip.e", right, centerY, chipSize, "E",
                          state().showExif, [] {
                              state().showExif = !state().showExif;
                              app::requestUpdate();
                          }, imageTools);
        right -= 2.0f + chipSize;
        composeToggleChip(ui, "toolbar.chip.d", right, centerY, chipSize, "D",
                          state().diffMode, [] { toggleDiffMode(); },
                          imageTools && (diffPairReady() || state().diffMode));

        // Window buttons, right-most in the bar (full height, VS Code style).
        float windowX = screen.width - 3.0f * winButton;
        composeWindowButton(ui, "toolbar.close", windowX + 2.0f * winButton, 0.0f,
                            winButton, kToolbarHeight, 2, true,
                            [] { imview::frame::closeWindow(); });
        composeWindowButton(ui, "toolbar.maximize", windowX + winButton, 0.0f,
                            winButton, kToolbarHeight, 1, false,
                            [] { imview::frame::toggleMaximizeWindow(); });
        composeWindowButton(ui, "toolbar.minimize", windowX, 0.0f,
                            winButton, kToolbarHeight, 0, false,
                            [] { imview::frame::minimizeWindow(); });
    }).build();
}

// Small square button in a cell's top-right corner, in the same visual
// language as the sidebar pane buttons. glyph: 0 = close, 1 = four-way move.
// The move button is a drag handle: press it, drag onto another cell, release.
void composeCellButton(eui::Ui& ui, const std::string& id, float x, float y,
                       int glyph, bool armed,
                       const std::function<void()>& onPress,
                       const std::function<void(float, float)>& onDrag = {},
                       const std::function<void()>& onRelease = {},
                       const std::function<void(bool)>& onHoverChanged = {}) {
    constexpr float kSize = 18.0f;
    const eui::Color idle = armed ? kAccent : eui::Color{0.0f, 0.0f, 0.0f, 0.45f};
    const eui::Color hover = armed ? kAccentHover : eui::Color{0.0f, 0.0f, 0.0f, 0.72f};
    ui.stack(id).position(x, y).size(kSize, kSize).content([&] {
        ui.rect(id + ".bg").size(kSize, kSize).radius(kRadius)
            .color(idle).states(idle, hover, kAccentPressed).build();
        const eui::Color ink = kTextPrimary;
        const float cx = kSize * 0.5f;
        const float cy = kSize * 0.5f;
        if (glyph == 0) {
            const float length = 9.0f;
            const float thickness = 1.2f;
            ui.rect(id + ".g1").position(cx - length * 0.5f, cy - thickness * 0.5f)
                .size(length, thickness).color(ink).rotate(0.785398f).build();
            ui.rect(id + ".g2").position(cx - length * 0.5f, cy - thickness * 0.5f)
                .size(length, thickness).color(ink).rotate(-0.785398f).build();
        } else {
            // Four-way arrows with a head on each end of both shafts. The heads
            // stay shorter than the gap between tips, otherwise the strokes join
            // into a diamond instead of reading as arrows.
            const float shaft = 5.0f;
            const float arm = 3.0f;
            const float thickness = 1.0f;
            ui.rect(id + ".v").position(cx - thickness * 0.5f, cy - shaft)
                .size(thickness, shaft * 2.0f).color(ink).build();
            ui.rect(id + ".h").position(cx - shaft, cy - thickness * 0.5f)
                .size(shaft * 2.0f, thickness).color(ink).build();
            const float dirs[4][2] = {{0.0f, -1.0f}, {0.0f, 1.0f},
                                      {-1.0f, 0.0f}, {1.0f, 0.0f}};
            for (int headIndex = 0; headIndex < 4; ++headIndex) {
                const float dirX = dirs[headIndex][0];
                const float dirY = dirs[headIndex][1];
                const float tipX = cx + dirX * shaft;
                const float tipY = cy + dirY * shaft;
                const float perpX = -dirY;
                const float perpY = dirX;
                for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
                    const float sign = sideIndex == 0 ? 1.0f : -1.0f;
                    // Each stroke runs back from the tip at 45 degrees.
                    const float unitX = (-dirX + perpX * sign) * 0.7071f;
                    const float unitY = (-dirY + perpY * sign) * 0.7071f;
                    const float strokeX = tipX + unitX * arm * 0.5f;
                    const float strokeY = tipY + unitY * arm * 0.5f;
                    const std::string strokeId = id + ".a" +
                        std::to_string(headIndex) + std::to_string(sideIndex);
                    ui.rect(strokeId)
                        .position(strokeX - arm * 0.5f, strokeY - thickness * 0.5f)
                        .size(arm, thickness).color(ink)
                        .rotate(std::atan2(unitY, unitX)).build();
                }
            }
        }
        auto& hit = ui.rect(id + ".hit").size(kSize, kSize)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onPress([onPress](const core::PointerEvent& event, const core::Rect&) {
                if (event.button == eui::PointerButton::Left) onPress();
            });
        if (onDrag) {
            // The pressed button keeps the pointer capture, so no other cell
            // receives hover callbacks: the target comes from the position.
            hit.onDrag([onDrag](const core::dsl::DragEvent& event) {
                const float scale = imview::frame::dpiScale();
                onDrag(static_cast<float>(event.x) / scale,
                       static_cast<float>(event.y) / scale);
            });
        }
        if (onRelease) {
            hit.onRelease([onRelease](const core::PointerEvent& event, const core::Rect&) {
                if (event.button == eui::PointerButton::Left) onRelease();
            });
        }
        if (onHoverChanged) {
            hit.onHover([onHoverChanged](bool hovered) { onHoverChanged(hovered); });
        }
        hit.cursor(eui::CursorShape::Hand).build();
    }).build();
}

// Luma histogram, drawn 80 columns wide inside an 80x48 panel.
void composeHistogramOverlay(eui::Ui& ui, const ImageEntry& entry, const std::string& id,
                             float width, bool topCornerFree) {
    constexpr float kHistogramWidth = 80.0f;
    constexpr float kHistogramHeight = 48.0f;
    constexpr int kColumns = 80;
    const float histogramX = width - kHistogramWidth - 8.0f;
    const float histogramY = topCornerFree ? 8.0f : 32.0f;

    ui.rect(id + ".hist.bg").position(histogramX, histogramY)
        .size(kHistogramWidth, kHistogramHeight)
        .color({0.0f, 0.0f, 0.0f, 0.6f}).build();
    for (int column = 0; column < kColumns; ++column) {
        const int firstBin = column * 256 / kColumns;
        const int lastBin = (column + 1) * 256 / kColumns;
        std::uint32_t peak = 0;
        for (int bin = firstBin; bin < lastBin; ++bin) {
            peak = std::max(peak, entry.histogram[static_cast<std::size_t>(bin)]);
        }
        if (peak == 0) continue;
        const float normalised = entry.histogramMax > 0
            ? static_cast<float>(peak) / static_cast<float>(entry.histogramMax)
            : 0.0f;
        const float barHeight = std::max(1.0f, normalised * (kHistogramHeight - 2.0f));
        ui.rect(id + ".hist.bar" + std::to_string(column))
            .position(histogramX + column * (kHistogramWidth / kColumns),
                      histogramY + kHistogramHeight - barHeight)
            .size(kHistogramWidth / kColumns, barHeight)
            .color({1.0f, 1.0f, 1.0f, 0.9f}).build();
    }
}

// EXIF summary, top-left: one black strip per line, sized to its measured text.
// The text sits 3 px inside the strip, so both sides get the same padding.
void composeExifOverlay(eui::Ui& ui, const ImageEntry& entry, const std::string& id,
                        float width) {
    constexpr float kLineHeight = 14.0f;
    for (std::size_t index = 0; index < entry.exifLines.size(); ++index) {
        const std::string& line = entry.exifLines[index];
        const float lineY = 8.0f + static_cast<float>(index) * kLineHeight;
        const float textWidth = core::TextPrimitive::measureTextWidth(line, {}, 10.0f, 400);
        const float lineWidth = std::min(width - 16.0f, 6.0f + textWidth);
        ui.rect(id + ".exifbg" + std::to_string(index))
            .position(8.0f, lineY).size(lineWidth, kLineHeight - 1.0f)
            .color({0.0f, 0.0f, 0.0f, 0.6f}).build();
        ui.text(id + ".exif" + std::to_string(index))
            .position(11.0f, lineY).size(lineWidth - 4.0f, kLineHeight)
            .text(line).color(kWhite).fontSize(10.0f).build();
    }
}

// Statistics block, bottom-left: four lines of white text on black strips.
void composeStatsOverlay(eui::Ui& ui, const ImageEntry& entry, const std::string& id,
                         float height) {
    constexpr float kLineHeight = 15.0f;
    constexpr float kBlockWidth = 150.0f;
    constexpr int kLineCount = 4;
    const float blockHeight = kLineHeight * kLineCount + 4.0f;
    const float blockY = height - blockHeight - 8.0f;
    for (int index = 0; index < kLineCount; ++index) {
        ui.rect(id + ".statbg" + std::to_string(index))
            .position(8.0f, blockY + index * kLineHeight)
            .size(kBlockWidth, kLineHeight - 1.0f)
            .color({0.0f, 0.0f, 0.0f, 0.6f}).build();
        ui.text(id + ".stat" + std::to_string(index))
            .position(12.0f, blockY + index * kLineHeight)
            .size(kBlockWidth - 8.0f, kLineHeight)
            .text(entry.statsLines[static_cast<std::size_t>(index)])
            .color(kWhite).fontSize(11.0f).build();
    }
}

// Click, drag and scroll surface of a cell, plus the two corner buttons.
void composeCellInteractions(eui::Ui& ui, int entryId, const std::string& id,
                             float width, float height, const DisplayRect& shown,
                             bool showCloseButton, bool showMoveButton, bool moveSource) {
    // Input layer. Left-drag: pan (or paint a selection in local mode).
    // Right-drag: pan one image (or move its selection in local mode).
    // Wheel: zoom (disabled in local mode).
    ui.rect(id + ".hit").size(width, height)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .acceptedButtons(eui::PointerButton::Left | eui::PointerButton::Right)
        .onPress([entryId, shown, width, height](const core::PointerEvent& event,
                                                 const core::Rect& bounds) {
            const float localX = static_cast<float>(event.x) - bounds.x;
            const float localY = static_cast<float>(event.y) - bounds.y;
            const float scale = bounds.width > 0.0f ? width / bounds.width : 1.0f;
            const float px = localX * scale;
            const float py = localY * scale;
            const float u = shown.w > 0.0f ? std::clamp((px - shown.x) / shown.w, 0.0f, 1.0f) : 0.0f;
            const float v = shown.h > 0.0f ? std::clamp((py - shown.y) / shown.h, 0.0f, 1.0f) : 0.0f;
            if (!state().localMode) {
                if (event.button != eui::PointerButton::Left) return;
                // Double-click toggles solo mode.
                if (isDoubleClick(state().cellClick, entryId, -1,
                                  core::window::timeSeconds())) {
                    toggleSolo(entryId);
                }
                return;
            }
            if (event.button == eui::PointerButton::Left) {
                // Start a fresh synchronized selection on every image.
                clearSelections();
                state().selecting = true;
                state().dragAnchorX = u;
                state().dragAnchorY = v;
                state().dragCurrentX = u;
                state().dragCurrentY = v;
                app::requestUpdate();
            } else if (event.button == eui::PointerButton::Right) {
                ImageEntry* target = findEntry(entryId);
                if (target != nullptr && target->hasSelection) {
                    state().movingSelection = true;
                    state().movingCellId = entryId;
                    state().moveCellW = shown.w;
                    state().moveCellH = shown.h;
                    app::requestUpdate();
                }
            }
        })
        .onRelease([entryId](const core::PointerEvent& event, const core::Rect&) {
            if (!state().localMode) {
                // A move that ends over another cell swaps the two.
                finishMove();
                return;
            }
            if (event.button == eui::PointerButton::Left && state().selecting) {
                state().selecting = false;
                const float x0 = std::min(state().dragAnchorX, state().dragCurrentX);
                const float x1 = std::max(state().dragAnchorX, state().dragCurrentX);
                const float y0 = std::min(state().dragAnchorY, state().dragCurrentY);
                const float y1 = std::max(state().dragAnchorY, state().dragCurrentY);
                // Even a click without movement is a one-pixel ROI.
                applySharedSelection(x0, y0, x1, y1);
                app::requestUpdate();
            } else if (event.button == eui::PointerButton::Right &&
                       state().movingSelection) {
                state().movingSelection = false;
                ImageEntry* target = findEntry(state().movingCellId);
                state().movingCellId = -1;
                if (target != nullptr) {
                    computeStats(*target);
                }
                app::requestUpdate();
            }
        })
        .onMove([entryId, shown, width, height](const core::PointerEvent& event,
                                                const core::Rect& bounds) {
            if (state().moveSourceId >= 0) {
                const int target = entryId == state().moveSourceId ? -1 : entryId;
                if (target != state().moveTargetId) {
                    state().moveTargetId = target;
                    app::requestUpdate();
                }
                return true;
            }
            if (!state().localMode) return false;
            const float localX = static_cast<float>(event.x) - bounds.x;
            const float localY = static_cast<float>(event.y) - bounds.y;
            const float scale = bounds.width > 0.0f ? width / bounds.width : 1.0f;
            const float px = localX * scale;
            const float py = localY * scale;
            const float u = shown.w > 0.0f ? std::clamp((px - shown.x) / shown.w, 0.0f, 1.0f) : 0.0f;
            const float v = shown.h > 0.0f ? std::clamp((py - shown.y) / shown.h, 0.0f, 1.0f) : 0.0f;
            if (state().selecting) {
                state().dragCurrentX = u;
                state().dragCurrentY = v;
                app::requestUpdate();
                return true;
            }
            if (state().movingSelection && state().movingCellId == entryId) {
                ImageEntry* target = findEntry(entryId);
                if (target == nullptr) return false;
                const float dx = state().moveCellW > 0.0f
                    ? static_cast<float>(event.deltaX) * scale / state().moveCellW : 0.0f;
                const float dy = state().moveCellH > 0.0f
                    ? static_cast<float>(event.deltaY) * scale / state().moveCellH : 0.0f;
                const float selWidth = std::fabs(target->selX1 - target->selX0);
                const float selHeight = std::fabs(target->selY1 - target->selY0);
                float nx0 = target->selX0 + dx;
                float ny0 = target->selY0 + dy;
                nx0 = std::clamp(nx0, 0.0f, std::max(0.0f, 1.0f - selWidth));
                ny0 = std::clamp(ny0, 0.0f, std::max(0.0f, 1.0f - selHeight));
                target->selX0 = nx0;
                target->selY0 = ny0;
                target->selX1 = nx0 + selWidth;
                target->selY1 = ny0 + selHeight;
                app::requestUpdate();
                return true;
            }
            return false;
        })
        .onDrag([entryId](const core::dsl::DragEvent& event) {
            if (!panEnabled()) return;
            if (event.button == eui::PointerButton::Left) {
                panAllBy(static_cast<float>(event.deltaX), static_cast<float>(event.deltaY));
            } else if (event.button == eui::PointerButton::Right) {
                ImageEntry* target = findEntry(entryId);
                if (target == nullptr) return;
                panBy(*target, static_cast<float>(event.deltaX), static_cast<float>(event.deltaY));
            }
            app::requestUpdate();
        })
        .onScroll([](const core::ScrollEvent& event) {
            if (!zoomEnabled() || event.y == 0.0) return;
            applyZoom(static_cast<float>(std::pow(1.1, event.y)));
        })
        .onHover([](bool) { app::requestUpdate(); })
        // NOTE: on*() handlers force the Hand cursor, so this must come last.
        .cursor(eui::CursorShape::Arrow)
        .build();

    // Cell buttons sit above the hit area so they win the click.
    constexpr float kButtonSize = 18.0f;
    constexpr float kButtonGap = 2.0f;
    constexpr float kButtonMargin = 6.0f;
    float buttonX = width - kButtonMargin - kButtonSize;
    const float buttonY = kButtonMargin;
    if (showCloseButton) {
        composeCellButton(ui, id + ".close", buttonX, buttonY, 0, false,
                          [entryId] { removeImage(entryId); }, {}, {},
                          [](bool) { app::requestUpdate(); });
        buttonX -= kButtonSize + kButtonGap;
    }
    if (showMoveButton) {
        // Press picks the cell up; the cell bodies report which cell the
        // pointer is over; releasing anywhere drops it on that cell.
        composeCellButton(
            ui, id + ".move", buttonX, buttonY, 1, moveSource,
            // Press: pick the cell up.
            [entryId] {
                state().moveSourceId = entryId;
                state().moveTargetId = -1;
                app::requestUpdate();
            },
            // Drag: highlight the cell under the pointer.
            [entryId](float pointerX, float pointerY) {
                if (state().moveSourceId != entryId) return;
                const int under = cellAt(pointerX, pointerY);
                const int target = under == entryId ? -1 : under;
                if (target != state().moveTargetId) {
                    state().moveTargetId = target;
                    app::requestUpdate();
                }
            },
            [] { finishMove(); },
            [](bool) { app::requestUpdate(); });
    }
}

// --- video cell controls ------------------------------------------------------

float videoBarHeight(float cellHeight) {
    return std::min(44.0f, std::max(24.0f, cellHeight * 0.45f));
}

void composeVideoTextButton(eui::Ui& ui, const std::string& id, float x, float y,
                            float width, float height, const std::string& label,
                            bool active, bool enabled,
                            const std::function<void()>& onTap,
                            float fontSize = 9.5f) {
    const eui::Color base = active ? kAccent : eui::Color{0.0f, 0.0f, 0.0f, 0.0f};
    const eui::Color hover = active ? kAccentHover : eui::Color{1.0f, 1.0f, 1.0f, 0.10f};
    const eui::Color pressed = active ? kAccentPressed : eui::Color{1.0f, 1.0f, 1.0f, 0.16f};
    ui.stack(id).position(x, y).size(width, height).content([&] {
        ui.rect(id + ".bg").size(width, height).radius(3.0f)
            .color(enabled ? base : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .states(enabled ? base : eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? hover : eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? pressed : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .build();
        ui.text(id + ".label").size(width, height).text(label)
            .color(enabled ? (active ? kWhite : kTextPrimary)
                           : eui::Color{1.0f, 1.0f, 1.0f, 0.28f})
            .fontSize(fontSize)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center).build();
        ui.rect(id + ".hit").size(width, height)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(enabled ? onTap : std::function<void()>{})
            .onHover([](bool) { app::requestUpdate(); })
            .cursor(enabled ? eui::CursorShape::Hand : eui::CursorShape::Arrow)
            .build();
    }).build();
}

void composeVideoSeekButton(eui::Ui& ui, const std::string& id, float x, float y,
                            float width, float height, bool forward, bool enabled,
                            const std::function<void()>& onTap) {
    ui.stack(id).position(x, y).size(width, height).content([&] {
        ui.rect(id + ".bg").size(width, height).radius(3.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .states({0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? eui::Color{1.0f, 1.0f, 1.0f, 0.10f}
                            : eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? eui::Color{1.0f, 1.0f, 1.0f, 0.16f}
                            : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .build();
        const eui::Color ink = enabled ? kTextPrimary
                                       : eui::Color{1.0f, 1.0f, 1.0f, 0.28f};
        if (forward) {
            ui.polygon(id + ".arrow").position(4.0f, 3.0f).size(11.0f, 12.0f)
                .points({{2.0f, 0.0f}, {11.0f, 6.0f}, {2.0f, 12.0f}})
                .color(ink).build();
        } else {
            ui.polygon(id + ".arrow").position(5.0f, 3.0f).size(11.0f, 12.0f)
                .points({{9.0f, 0.0f}, {0.0f, 6.0f}, {9.0f, 12.0f}})
                .color(ink).build();
        }
        ui.rect(id + ".hit").size(width, height)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(enabled ? onTap : std::function<void()>{})
            .onHover([](bool) { app::requestUpdate(); })
            .cursor(enabled ? eui::CursorShape::Hand : eui::CursorShape::Arrow)
            .build();
    }).build();
}

void composeVideoPlayButton(eui::Ui& ui, const std::string& id, float x, float y,
                            float width, float height, bool paused, bool enabled,
                            const std::function<void()>& onTap) {
    ui.stack(id).position(x, y).size(width, height).content([&] {
        ui.rect(id + ".bg").size(width, height).radius(3.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .states({0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? eui::Color{1.0f, 1.0f, 1.0f, 0.10f}
                            : eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? eui::Color{1.0f, 1.0f, 1.0f, 0.16f}
                            : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .build();
        const eui::Color ink = enabled ? kTextPrimary
                                       : eui::Color{1.0f, 1.0f, 1.0f, 0.28f};
        if (paused) {
            ui.polygon(id + ".play").position(5.0f, 3.0f).size(10.0f, 12.0f)
                .points({{0.0f, 0.0f}, {10.0f, 6.0f}, {0.0f, 12.0f}})
                .color(ink).build();
        } else {
            ui.rect(id + ".pause1").position(6.0f, 3.0f).size(3.0f, 12.0f)
                .color(ink).build();
            ui.rect(id + ".pause2").position(11.0f, 3.0f).size(3.0f, 12.0f)
                .color(ink).build();
        }
        ui.rect(id + ".hit").size(width, height)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(enabled ? onTap : std::function<void()>{})
            .onHover([](bool) { app::requestUpdate(); })
            .cursor(enabled ? eui::CursorShape::Hand : eui::CursorShape::Arrow)
            .build();
    }).build();
}

void seekVideoFraction(int entryId, float fraction) {
    ImageEntry* entry = findEntry(entryId);
    if (entry == nullptr || !entry->video) return;
    const imview::video::Status status = entry->video->status();
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    const double target = status.duration > 0.0 ? status.duration * fraction : 0.0;
    entry->progressPreviewFraction = fraction;
    entry->progressPreviewTime = target;
    // The progress bar is intentionally per-video: durations need not match,
    // so a sync-mode click only seeks the cell under the pointer.
    entry->video->seekTo(target);
    state().activeVideoId = entryId;
    app::requestUpdate();
}

void previewVideoFraction(int entryId, float fraction) {
    ImageEntry* entry = findEntry(entryId);
    if (entry == nullptr || !entry->video) return;
    const imview::video::Status status = entry->video->status();
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    entry->progressPreviewFraction = fraction;
    entry->progressPreviewTime = status.duration > 0.0 ? status.duration * fraction : 0.0;
    entry->progressPreviewActive = true;
    entry->video->requestPreview(entry->progressPreviewTime);
    state().activeVideoId = entryId;
    app::requestUpdate();
}

void composeVideoProgress(eui::Ui& ui, ImageEntry& entry,
                          const imview::video::Status& status,
                          float x, float y, float width) {
    const float fraction = status.duration > 0.0
        ? static_cast<float>(std::clamp(status.position / status.duration, 0.0, 1.0))
        : 0.0f;
    constexpr float kHeight = 14.0f;
    constexpr float kTrackY = 5.0f;
    constexpr float kTrackHeight = 3.0f;
    constexpr float kKnobSize = 8.0f;
    const int entryId = entry.id;
    ui.stack("cell" + std::to_string(entryId) + ".progress")
        .position(x, y).size(width, kHeight).content([&] {
        ui.rect("cell" + std::to_string(entryId) + ".progress.track")
            .position(0.0f, kTrackY).size(width, kTrackHeight)
            .color({1.0f, 1.0f, 1.0f, 0.20f}).radius(kTrackHeight * 0.5f).build();
        if (fraction > 0.0f) {
            ui.rect("cell" + std::to_string(entryId) + ".progress.fill")
                .position(0.0f, kTrackY)
                .size(std::max(kTrackHeight, width * fraction), kTrackHeight)
                .color(kAccent).radius(kTrackHeight * 0.5f).build();
        }
        if (entry.progressPreviewActive) {
            const float marker = width * entry.progressPreviewFraction;
            ui.rect("cell" + std::to_string(entryId) + ".progress.preview.mark")
                .position(marker - 1.0f, kTrackY - 2.0f).size(2.0f, kTrackHeight + 4.0f)
                .color(kWhite).build();
        }
        ui.rect("cell" + std::to_string(entryId) + ".progress.knob")
            .position(width * fraction - kKnobSize * 0.5f,
                      (kHeight - kKnobSize) * 0.5f)
            .size(kKnobSize, kKnobSize).radius(kKnobSize * 0.5f)
            .color(kTextPrimary).build();
        ui.rect("cell" + std::to_string(entryId) + ".progress.hit")
            .size(width, kHeight)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .acceptedButtons(eui::PointerButton::Left)
            .onPress([entryId, width](const core::PointerEvent& event,
                                      const core::Rect& bounds) {
                ImageEntry* target = findEntry(entryId);
                if (target == nullptr || !target->video || bounds.width <= 0.0f) return;
                const float fraction = std::clamp(
                    static_cast<float>((event.x - bounds.x) / bounds.width), 0.0f, 1.0f);
                target->videoControlsInteracting = true;
                target->progressDragStartFraction = fraction;
                seekVideoFraction(entryId, fraction);
                target->progressPreviewActive = true;
                target->video->requestPreview(target->progressPreviewTime);
                app::requestUpdate();
            })
            .onDrag([entryId, width](const core::dsl::DragEvent& event) {
                if (event.button != eui::PointerButton::Left) return;
                ImageEntry* target = findEntry(entryId);
                if (target == nullptr || !target->video) return;
                const float scale = imview::frame::dpiScale();
                const float delta = scale > 0.0f
                    ? static_cast<float>(event.totalX) / scale / std::max(1.0f, width)
                    : 0.0f;
                previewVideoFraction(entryId,
                    target->progressDragStartFraction + delta);
            })
            .onRelease([entryId](const core::PointerEvent& event, const core::Rect&) {
                if (event.button != eui::PointerButton::Left) return;
                ImageEntry* target = findEntry(entryId);
                if (target == nullptr || !target->video) return;
                target->videoControlsInteracting = false;
                seekVideoFraction(entryId, target->progressPreviewFraction);
                target->progressPreviewActive = false;
                app::requestUpdate();
            })
            .onMove([entryId](const core::PointerEvent& event, const core::Rect& bounds) {
                if (bounds.width <= 0.0f) return;
                previewVideoFraction(entryId, std::clamp(
                    static_cast<float>((event.x - bounds.x) / bounds.width), 0.0f, 1.0f));
            })
            .onHover([entryId](bool hovered) {
                ImageEntry* target = findEntry(entryId);
                if (target != nullptr && !hovered && !target->videoControlsInteracting) {
                    target->progressPreviewActive = false;
                }
                app::requestUpdate();
            })
            .cursor(eui::CursorShape::Arrow).build();
    }).build();
}

void composeVideoControls(eui::Ui& ui, ImageEntry& entry,
                          const imview::video::Status& status,
                          float x, float y, float width, float height) {
    const float barHeight = videoBarHeight(height);
    const float barY = y + height - barHeight;
    const int entryId = entry.id;
    const std::string id = "cell" + std::to_string(entryId);
    ui.rect(id + ".controls.bg").position(x, barY).size(width, barHeight)
        .color({0.0f, 0.0f, 0.0f, 0.64f}).build();
    const float progressX = x + 8.0f;
    const float progressWidth = std::max(20.0f, width - 16.0f);
    composeVideoProgress(ui, entry, status, progressX, barY + 3.0f, progressWidth);

    const float rowY = barY + 21.0f;
    constexpr float kButton = 20.0f;
    constexpr float kGap = 2.0f;
    float left = x + 8.0f;
    composeVideoPlayButton(ui, id + ".control.play", left, rowY, kButton, 18.0f,
                           status.paused, !status.failed,
                           [entryId] { toggleVideoPlayback(entryId); });
    left += kButton + kGap;
    composeVideoSeekButton(ui, id + ".control.back", left, rowY, kButton, 18.0f,
                           false, !status.failed, [entryId] {
                               seekVideoBy(entryId, -5.0);
                           });
    left += kButton + kGap;
    composeVideoSeekButton(ui, id + ".control.forward", left, rowY, kButton, 18.0f,
                           true, !status.failed, [entryId] {
                               seekVideoBy(entryId, 5.0);
                           });
    left += kButton + kGap;

    constexpr float kTrackWidth = 22.0f;
    constexpr float kCcWidth = 18.0f;
    const float right = x + width - 8.0f;
    const float ccX = right - kCcWidth;
    const float subtitleX = ccX - kGap - kTrackWidth;
    const float audioX = subtitleX - kGap - kTrackWidth;
    const float videoX = audioX - kGap - kTrackWidth;
    const float timeX = left;
    const float timeWidth = std::max(0.0f, videoX - 6.0f - timeX);

    if (timeWidth >= 30.0f) {
        std::string time = formatVideoTime(status.position);
        if (status.duration > 0.0) time += " / " + formatVideoTime(status.duration);
        composeClippedText(ui, id + ".control.time", timeX, rowY, timeWidth, 18.0f,
                           time, kTextSecondary, 10.5f);
    }

    const auto cycleVideoTrack = [entryId, status] {
        if (ImageEntry* target = findEntry(entryId)) {
            if (target->video && status.videoTrackCount > 1) {
                target->video->setVideoTrack(
                    (status.videoTrack + 1) % status.videoTrackCount);
            }
        }
    };
    const auto cycleAudioTrack = [entryId, status] {
        if (ImageEntry* target = findEntry(entryId)) {
            if (target->video && status.audioTrackCount > 1) {
                target->video->setAudioTrack(
                    (status.audioTrack + 1) % status.audioTrackCount);
            }
        }
    };
    const auto cycleSubtitleTrack = [entryId, status] {
        if (ImageEntry* target = findEntry(entryId)) {
            if (target->video && status.subtitleTrackCount > 1) {
                target->video->setSubtitleTrack(
                    (status.subtitleTrack + 1) % status.subtitleTrackCount);
            }
        }
    };
    const std::string videoLabel = status.videoTrackCount > 0
        ? "V" + std::to_string(status.videoTrack + 1) : "V-";
    const std::string audioLabel = status.audioTrackCount > 0
        ? "A" + std::to_string(status.audioTrack + 1) : "A-";
    const std::string subtitleLabel = status.subtitleTrackCount > 0 &&
                                      status.subtitleTrack >= 0
        ? "S" + std::to_string(status.subtitleTrack + 1) : "S-";

    composeVideoTextButton(ui, id + ".control.vtrack", videoX, rowY,
                           kTrackWidth, 18.0f, videoLabel, false,
                           status.videoTrackCount > 1, cycleVideoTrack);
    composeVideoTextButton(ui, id + ".control.atrack", audioX, rowY,
                           kTrackWidth, 18.0f, audioLabel, false,
                           status.audioTrackCount > 1, cycleAudioTrack);
    composeVideoTextButton(ui, id + ".control.strack", subtitleX, rowY,
                           kTrackWidth, 18.0f, subtitleLabel, false,
                           status.subtitleTrackCount > 1, cycleSubtitleTrack);
    composeVideoTextButton(ui, id + ".control.cc", ccX, rowY, kCcWidth, 18.0f,
                           "CC", status.hasSubtitles && status.subtitlesEnabled,
                           status.hasSubtitles, [entryId] {
                               if (ImageEntry* target = findEntry(entryId)) {
                                   if (target->video) target->video->toggleSubtitles();
                               }
                           });
}

void composeVideoPreview(eui::Ui& ui, ImageEntry& entry,
                         const imview::video::Status& status,
                         float x, float y, float width, float height) {
    if (!entry.progressPreviewActive || !entry.video) return;
    const auto stream = entry.video->previewStream();
    if (!stream) return;
    const float barHeight = videoBarHeight(height);
    const float previewWidth = std::min(160.0f, std::max(60.0f, width - 16.0f));
    const float previewHeight = previewWidth * 9.0f / 16.0f;
    const float previewX = x + std::clamp(
        entry.progressPreviewFraction * width - previewWidth * 0.5f,
        4.0f, std::max(4.0f, width - previewWidth - 4.0f));
    const float previewY = std::max(
        y + 4.0f, y + height - barHeight - previewHeight - 22.0f);
    const std::string id = "cell" + std::to_string(entry.id) + ".preview";
    ui.rect(id + ".bg").position(previewX, previewY)
        .size(previewWidth, previewHeight + 18.0f)
        .color({0.0f, 0.0f, 0.0f, 0.82f}).radius(3.0f).build();
    ui.image(id + ".image").position(previewX, previewY)
        .size(previewWidth, previewHeight).stream(stream)
        .fit(eui::ImageFit::Contain).build();
    ui.text(id + ".time").position(previewX, previewY + previewHeight)
        .size(previewWidth, 18.0f)
        .text(formatVideoTime(entry.progressPreviewTime))
        .color(kTextPrimary).fontSize(10.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center).build();
    (void)status;
}

void composeVideoSubtitle(eui::Ui& ui, ImageEntry& entry,
                          const imview::video::Status& status,
                          float x, float y, float width, float height,
                          bool controlsVisible) {
    if (!status.hasSubtitles || !status.subtitlesEnabled || status.subtitle.empty()) return;
    const float barHeight = controlsVisible ? videoBarHeight(height) : 0.0f;
    const float bottom = y + height - barHeight - 10.0f;
    const float textHeight = std::min(58.0f, std::max(28.0f, height * 0.24f));
    const float textY = std::max(y + 4.0f, bottom - textHeight);
    const std::string id = "cell" + std::to_string(entry.id) + ".subtitle";
    const float textWidth = std::max(20.0f, width - 24.0f);
    const float textX = x + 12.0f;
    const float outline = 1.15f;
    const std::array<std::array<float, 2>, 8> offsets = {{
        {{-outline, 0.0f}}, {{outline, 0.0f}},
        {{0.0f, -outline}}, {{0.0f, outline}},
        {{-outline, -outline}}, {{outline, -outline}},
        {{-outline, outline}}, {{outline, outline}},
    }};
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        ui.text(id + ".outline" + std::to_string(index))
            .position(textX + offsets[index][0], textY + offsets[index][1])
            .size(textWidth, textHeight)
            .text(status.subtitle).color({0.0f, 0.0f, 0.0f, 1.0f})
            .fontSize(12.0f).wrap(true).lineHeight(15.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center).build();
    }
    ui.text(id + ".text").position(textX, textY)
        .size(textWidth, textHeight)
        .text(status.subtitle).color(kWhite)
        .fontSize(12.0f).wrap(true).lineHeight(15.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center).build();
}

void composeVideoCell(eui::Ui& ui, ImageEntry& entry, float x, float y,
                      float width, float height) {
    const std::string id = "cell" + std::to_string(entry.id);
    const float border = crispLine(kCellBorderWidth);
    const float innerX = border;
    const float innerY = border;
    const float innerWidth = std::max(1.0f, width - 2.0f * border);
    const float innerHeight = std::max(1.0f, height - 2.0f * border);
    entry.cellX = x;
    entry.cellY = y;
    entry.cellW = width;
    entry.cellH = height;

    const imview::video::Status status = entry.video ? entry.video->status()
                                                     : imview::video::Status{};
    const bool inGrid = !state().reviewMode && !soloActive();
    const bool moveSource = state().moveSourceId == entry.id;
    const bool moveTarget = state().moveTargetId == entry.id;
    const bool buttonHot = pointerInButtonHotZone(entry);
    const bool showCloseButton = inGrid && (buttonHot || moveSource);
    const bool showMoveButton = inGrid && state().images.size() > 1 &&
                                (buttonHot || moveSource);
    // The video cell is its own controls hot zone: entering it reveals the
    // transport, leaving it hides the transport again.
    const bool controlsVisible = buttonHot || entry.videoControlsInteracting;

    ui.stack(id).position(x, y).size(width, height).clip().content([&] {
        ui.rect(id + ".bg").size(width, height).color({0.0f, 0.0f, 0.0f, 1.0f})
            .border(border, kBorder).build();
        const auto stream = entry.video ? entry.video->stream() : nullptr;
        if (stream) {
            ui.image(id + ".img").position(innerX, innerY)
                .size(innerWidth, innerHeight).stream(stream)
                .fit(eui::ImageFit::Contain).build();
        }
        if (moveSource || moveTarget) {
            ui.rect(id + ".move.frame").size(width, height)
                .color(moveTarget ? eui::Color{0.0f, 0.471f, 0.831f, 0.18f}
                                  : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .border(crispLine(2.0f), kAccent).build();
        }
        if (status.failed) {
            ui.text(id + ".error").size(width, height)
                .text("Cannot play this video").color(kError).fontSize(13.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();
            composeClippedText(ui, id + ".reason", 16.0f, height * 0.5f + 18.0f,
                               std::max(1.0f, width - 32.0f), 20.0f,
                               status.error, kTextSecondary, 10.5f,
                               eui::HorizontalAlign::Center);
        } else if (!status.hasFrame) {
            ui.text(id + ".loading").size(width, height)
                .text("Opening video...").color(kTextSecondary).fontSize(12.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();
        }

        composeVideoSubtitle(ui, entry, status, innerX, innerY, innerWidth,
                             innerHeight, controlsVisible);

        // Full-cell hit area: click toggles playback; moving into the cell
        // requests a compose so the hot-zone controls can appear.
        ui.rect(id + ".hit").size(width, height)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick([entryId = entry.id] {
                toggleVideoPlayback(entryId);
            })
            .onPress([entryId = entry.id](const core::PointerEvent& event, const core::Rect&) {
                if (event.button != eui::PointerButton::Left) return;
                state().activeVideoId = entryId;
            })
            .onHover([entryId = entry.id](bool hovered) {
                if (hovered) state().activeVideoId = entryId;
                app::requestUpdate();
            })
            .cursor(eui::CursorShape::Arrow).build();

        // Hot zones sit above the hit area. The bottom one reveals the video
        // transport; the corner one reveals close/move.
        ui.rect(id + ".hot.controls")
            .position(innerX, innerY + std::max(0.0f, innerHeight - 74.0f))
            .size(innerWidth, std::min(74.0f, innerHeight))
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick([entryId = entry.id] {
                toggleVideoPlayback(entryId);
            })
            .onHover([entryId = entry.id](bool hovered) {
                if (hovered) state().activeVideoId = entryId;
                app::requestUpdate();
            })
            .cursor(eui::CursorShape::Arrow).build();

        if (controlsVisible && !status.failed) {
            composeVideoPreview(ui, entry, status, innerX, innerY, innerWidth,
                                innerHeight);
            composeVideoControls(ui, entry, status, innerX, innerY, innerWidth,
                                 innerHeight);
        }

        if (showCloseButton) {
            constexpr float kButtonSize = 18.0f;
            constexpr float kButtonGap = 2.0f;
            constexpr float kButtonMargin = 6.0f;
            float buttonX = innerWidth - kButtonMargin - kButtonSize;
            const float buttonY = kButtonMargin;
            composeCellButton(ui, id + ".close", buttonX, buttonY, 0, false,
                              [entryId = entry.id] { removeImage(entryId); }, {}, {},
                              [](bool) { app::requestUpdate(); });
            buttonX -= kButtonSize + kButtonGap;
            if (showMoveButton) {
                composeCellButton(
                    ui, id + ".move", buttonX, buttonY, 1, moveSource,
                    [entryId = entry.id] {
                        state().moveSourceId = entryId;
                        state().moveTargetId = -1;
                        app::requestUpdate();
                    },
                    [entryId = entry.id](float pointerX, float pointerY) {
                        if (state().moveSourceId != entryId) return;
                        const int under = cellAt(pointerX, pointerY);
                        const int target = under == entryId ? -1 : under;
                        if (target != state().moveTargetId) {
                            state().moveTargetId = target;
                            app::requestUpdate();
                        }
                    },
                    [] { finishMove(); },
                    [](bool) { app::requestUpdate(); });
            }
        }
    }).build();
}

void composeCell(eui::Ui& ui, ImageEntry& entry, float x, float y,
                 float width, float height,
                 const std::shared_ptr<eui::ImageStream>& streamOverride = nullptr,
                 ImageEntry* sourceOverride = nullptr) {
    if (entry.isVideo && streamOverride == nullptr && sourceOverride == nullptr) {
        composeVideoCell(ui, entry, x, y, width, height);
        return;
    }
    const std::string id = "cell" + std::to_string(entry.id);
    const float zoom = state().zoom;
    ImageEntry& source = sourceOverride != nullptr ? *sourceOverride : entry;

    // Keep the cached cell rect and pan limits, and re-clamp the stored offset
    // after zoom changes, in one place.
    entry.cellX = x;
    entry.cellY = y;
    entry.cellW = width;
    entry.cellH = height;
    const float border = crispLine(kCellBorderWidth);
    const float innerX = border;
    const float innerY = border;
    const float innerWidth = std::max(1.0f, width - 2.0f * border);
    const float innerHeight = std::max(1.0f, height - 2.0f * border);
    panLimits(entry, innerWidth, innerHeight, entry.maxPanX, entry.maxPanY);
    entry.panX = std::clamp(entry.panX, -entry.maxPanX, entry.maxPanX);
    entry.panY = std::clamp(entry.panY, -entry.maxPanY, entry.maxPanY);

    float offsetX = 0.0f;
    float offsetY = 0.0f;
    displayOffset(entry, innerWidth, innerHeight, offsetX, offsetY);
    DisplayRect shown = displayRect(entry, innerWidth, innerHeight);
    shown.x += innerX;
    shown.y += innerY;
    const bool localMode = state().localMode;
    const int entryId = entry.id;
    // Move mode: the armed cell gets an accent frame, the hovered target a tint.
    const bool moveSource = state().moveSourceId == entryId;
    const bool moveTarget = state().moveTargetId == entryId;
    // Cell buttons: close and move only in the normal grid. Review mode keeps
    // its pair fixed, and solo view shows a single image with no neighbour, so
    // neither button exists in those states. Move also needs a second cell.
    // Both buttons auto-hide and appear only while the pointer is in the
    // top-right hot zone (or while a move is armed).
    const bool inGrid = !state().reviewMode && !soloActive();
    const bool buttonHot = pointerInButtonHotZone(entry);
    const bool showCloseButton = inGrid && (buttonHot || moveSource);
    const bool showMoveButton = inGrid && state().images.size() > 1 &&
                                (buttonHot || moveSource);

    // Reserve the top-right corner while the cell buttons can appear, so the
    // histogram does not jump when they auto-hide.
    const bool topCornerFree = !inGrid;

    // Prefer the app-owned external texture; the shared sampler provides
    // NEAREST filtering while zoomed. Streams remain the fallback on backends
    // or devices where the external texture cannot be imported.
    bool useGpuImage = false;
#if defined(EUI_RENDER_BACKEND_OPENGL)
    useGpuImage = ensureGpuImage(source, state().zoom > 1.0f);
    if (useGpuImage) {
        // A later switch back to the stream path needs a fresh frame.
        source.streamPrimed = false;
    }
#endif
    if (!useGpuImage && !streamOverride && !source.streamPrimed && source.loaded) {
        submitFrame(source, source.stream);
        source.streamPrimed = true;
    }

    ui.stack(id).position(x, y).size(width, height).clip().content([&] {
        // Flat surface with a hairline border, like a VS Code editor group.
        ui.rect(id + ".bg").size(width, height).color(kPanel)
            .border(border, kBorder)
            .build();
        // The viewport is the untransformed clip frame; only the image moves
        // inside it, so its id and stream mailbox survive every pan/zoom step.
        ui.stack(id + ".viewport")
            .position(innerX, innerY)
            .size(innerWidth, innerHeight)
            .clip()
            .content([&] {
                auto image = ui.image(id + ".img");
                image.position(0.0f, 0.0f)
                    .size(innerWidth, innerHeight)
                    .fit(eui::ImageFit::Contain)
                    .transformOrigin(0.5f, 0.5f)
                    .scale(zoom, zoom)
                    .rotate(static_cast<float>(entry.rotation) * 1.5707963f)
                    .translate(offsetX, offsetY);
                if (useGpuImage) {
                    image.texture(source.gpuImage, source.gpuRevision);
                } else {
                    image.stream(streamOverride ? streamOverride : source.stream);
                }
                image.build();
            }).build();
        if (moveSource || moveTarget) {
            ui.rect(id + ".move.frame").size(width, height)
                .color(moveTarget ? eui::Color{0.0f, 0.471f, 0.831f, 0.18f}
                                  : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .border(crispLine(2.0f), kAccent).build();
        }
        if (entry.loading) {
            ui.text(id + ".status").size(width, height).text("Loading...")
                .color(kTextSecondary).fontSize(12.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();
        } else if (entry.failed) {
            ui.text(id + ".status").size(width, height).text("Failed to load")
                .color(kError).fontSize(12.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();
        }

        // Selection overlay (local mode / active drag).
        const bool dragging = state().selecting;
        float sx0 = 0.0f, sy0 = 0.0f, sx1 = 0.0f, sy1 = 0.0f;
        bool drawSelection = false;
        if (dragging) {
            sx0 = std::min(state().dragAnchorX, state().dragCurrentX);
            sx1 = std::max(state().dragAnchorX, state().dragCurrentX);
            sy0 = std::min(state().dragAnchorY, state().dragCurrentY);
            sy1 = std::max(state().dragAnchorY, state().dragCurrentY);
            drawSelection = true;
        } else if (localMode && entry.hasSelection) {
            sx0 = std::min(entry.selX0, entry.selX1);
            sx1 = std::max(entry.selX0, entry.selX1);
            sy0 = std::min(entry.selY0, entry.selY1);
            sy1 = std::max(entry.selY0, entry.selY1);
            drawSelection = true;
        }
        if (drawSelection) {
            // A one-pixel ROI still needs a visible marker.
            const float selectionWidth =
                std::max(snapToPixel((sx1 - sx0) * shown.w), 1.0f);
            const float selectionHeight =
                std::max(snapToPixel((sy1 - sy0) * shown.h), 1.0f);
            ui.rect(id + ".sel")
                .position(snapToPixel(shown.x + sx0 * shown.w),
                          snapToPixel(shown.y + sy0 * shown.h))
                .size(selectionWidth, selectionHeight)
                .color({0.0f, 0.0f, 0.0f, 0.0f})
                .border(crispLine(1.5f), dragging ? kSelectionDrag : kSelectionFixed)
                .build();
        }

        const bool imageTools = imageToolsEnabled();
        if (imageTools && state().showHistogram && entry.hasHistogram) {
            composeHistogramOverlay(ui, entry, id, width, topCornerFree);
        }

        if (imageTools && state().showExif && !entry.exifLines.empty()) {
            composeExifOverlay(ui, entry, id, width);
        }

        if (imageTools && localMode && entry.hasStats) {
            composeStatsOverlay(ui, entry, id, height);
        }

        composeCellInteractions(ui, entryId, id, width, height, shown,
                                showCloseButton, showMoveButton, moveSource);
    }).build();
}

// The computed diff tile. It renders like a third image but has no close or
// move handle, and it never joins the local-mode selection or EXIF overlays.
// Wheel zoom and pan still act together with the two source images.
void composeDiffCell(eui::Ui& ui, float x, float y, float width, float height) {
    ImageEntry& entry = state().diffImage;
    const std::string id = "diff";
    const float border = crispLine(kCellBorderWidth);
    const float innerX = border;
    const float innerY = border;
    const float innerWidth = std::max(1.0f, width - 2.0f * border);
    const float innerHeight = std::max(1.0f, height - 2.0f * border);

    panLimits(entry, innerWidth, innerHeight, entry.maxPanX, entry.maxPanY);
    entry.panX = std::clamp(entry.panX, -entry.maxPanX, entry.maxPanX);
    entry.panY = std::clamp(entry.panY, -entry.maxPanY, entry.maxPanY);

    float offsetX = 0.0f;
    float offsetY = 0.0f;
    displayOffset(entry, innerWidth, innerHeight, offsetX, offsetY);
    DisplayRect shown = displayRect(entry, innerWidth, innerHeight);
    shown.x += innerX;
    shown.y += innerY;

    bool useGpuImage = false;
#if defined(EUI_RENDER_BACKEND_OPENGL)
    useGpuImage = ensureGpuImage(entry, state().zoom > 1.0f);
    if (useGpuImage) entry.streamPrimed = false;
#endif
    if (!useGpuImage && !entry.streamPrimed && entry.loaded) {
        submitFrame(entry, entry.stream);
        entry.streamPrimed = true;
    }

    ui.stack(id).position(x, y).size(width, height).clip().content([&] {
        ui.rect(id + ".bg").size(width, height).color(kPanel)
            .border(border, kBorder).build();

        if (entry.loaded) {
            // Same untransformed viewport as the normal cells: the border and
            // the input layer stay outside, only the image is clipped here.
            ui.stack(id + ".viewport")
                .position(innerX, innerY)
                .size(innerWidth, innerHeight)
                .clip()
                .content([&] {
                    auto image = ui.image(id + ".img");
                    image.position(0.0f, 0.0f)
                        .size(innerWidth, innerHeight)
                        .fit(eui::ImageFit::Contain)
                        .transformOrigin(0.5f, 0.5f)
                        .scale(state().zoom, state().zoom)
                        .translate(offsetX, offsetY);
                    if (useGpuImage) {
                        image.texture(entry.gpuImage, entry.gpuRevision);
                    } else {
                        image.stream(entry.stream);
                    }
                    image.build();
                }).build();
        }

        if (entry.loaded) {
            const std::string badge =
                "Diff  " + std::to_string(static_cast<int>(std::lround(state().diffThreshold)));
            const float textWidth = core::TextPrimitive::measureTextWidth(
                badge, {}, 10.0f, 400);
            const float badgeWidth = std::min(width - 16.0f, 6.0f + textWidth);
            ui.rect(id + ".badge.bg").position(8.0f, 8.0f)
                .size(badgeWidth, 14.0f)
                .color({0.0f, 0.0f, 0.0f, 0.6f}).build();
            ui.text(id + ".badge").position(11.0f, 8.0f)
                .size(std::max(0.0f, badgeWidth - 4.0f), 14.0f)
                .text(badge).color(kWhite).fontSize(10.0f).build();
        } else if (entry.failed) {
            ui.text(id + ".status").size(width, height).text("Diff unavailable")
                .color(kError).fontSize(12.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();
        } else {
            ui.text(id + ".status").size(width, height).text("Computing diff...")
                .color(kTextSecondary).fontSize(12.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();
        }

        ui.rect(id + ".hit").size(width, height)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .acceptedButtons(eui::PointerButton::Left | eui::PointerButton::Right)
            .onDrag([](const core::dsl::DragEvent& event) {
                if (!panEnabled()) return;
                if (event.button == eui::PointerButton::Left) {
                    panAllBy(static_cast<float>(event.deltaX),
                             static_cast<float>(event.deltaY));
                } else if (event.button == eui::PointerButton::Right) {
                    panBy(state().diffImage, static_cast<float>(event.deltaX),
                          static_cast<float>(event.deltaY));
                }
                app::requestUpdate();
            })
            .onScroll([](const core::ScrollEvent& event) {
                if (!zoomEnabled() || event.y == 0.0) return;
                applyZoom(static_cast<float>(std::pow(1.1, event.y)));
            })
            .cursor(eui::CursorShape::Arrow)
            .build();
    }).build();
}

// Keeps an image's GPU texture alive while another one is shown full size.
//
// The image stream is a one-shot mailbox: once the framework has consumed the
// single submitted frame, a destroyed ImagePrimitive cannot restore it. Since
// primitives are keyed by element id, composing a tiny invisible shell with the
// same ids keeps the instance (and its texture) alive across mode switches.
void composeCellKeepAlive(eui::Ui& ui, ImageEntry& entry) {
    const std::string id = "cell" + std::to_string(entry.id);
    bool useGpuImage = false;
#if defined(EUI_RENDER_BACKEND_OPENGL)
    useGpuImage = ensureGpuImage(entry, state().zoom > 1.0f);
#endif
    if (!useGpuImage && !entry.streamPrimed && entry.loaded) {
        submitFrame(entry, entry.stream);
        entry.streamPrimed = true;
    }
    ui.stack(id).position(0.0f, 0.0f).size(1.0f, 1.0f).opacity(0.0f).content([&] {
        auto image = ui.image(id + ".img");
        image.size(1.0f, 1.0f).fit(eui::ImageFit::Contain);
        if (useGpuImage) {
            image.texture(entry.gpuImage, entry.gpuRevision);
        } else {
            image.stream(entry.stream);
        }
        image.build();
    }).build();
}

void composeGrid(eui::Ui& ui, const eui::Screen& screen) {
    const float margin = 3.0f;
    const float gap = 2.0f;
    const int count = static_cast<int>(state().images.size());
    const float top = kToolbarHeight;
    const float left = state().sidebarVisible ? sidebarWidthFor(screen.width) : 0.0f;
    const float availableWidth = screen.width - left - margin * 2.0f;
    const float availableHeight = screen.height - top - margin * 2.0f;

    // Solo mode: the current (or peeked) image fills the whole grid area.
    if (soloActive()) {
        const int shownId = shownSoloId();
        ImageEntry* shown = findEntry(shownId);
        if (shown != nullptr) {
            for (auto& entry : state().images) {
                if (entry.id != shownId) {
                    composeCellKeepAlive(ui, entry);
                }
            }
            composeCell(ui, *shown, snapToPixel(left + margin),
                        snapToPixel(top + margin),
                        floorToPixel(availableWidth), floorToPixel(availableHeight));
            return;
        }
    }

    // Diff mode adds one computed tile. It is counted only for layout here, not
    // in AppState::images, so the four-image cap and review pair stay unchanged.
    const bool showDiff = state().diffMode && count == 2 &&
                          !state().images[0].isVideo && !state().images[1].isVideo;
    const int layoutCount = count + (showDiff ? 1 : 0);
    const int cols = layoutCount == 1 ? 1 : 2;
    const int rows = (layoutCount + cols - 1) / cols;
    const float cellWidth = floorToPixel((availableWidth - gap * (cols - 1)) / cols);
    const float cellHeight = floorToPixel((availableHeight - gap * (rows - 1)) / rows);
    const float totalHeight = rows * cellHeight + (rows - 1) * gap;
    const float y0 = snapToPixel(top + margin + (availableHeight - totalHeight) * 0.5f);

    // Q cover: the right image is painted in the left cell while held.
    std::shared_ptr<eui::ImageStream> leftCover;
    ImageEntry* leftCoverSource = nullptr;
    if (state().qSwapActive && count == 2 &&
        !state().images[0].isVideo && !state().images[1].isVideo) {
        leftCover = state().images[1].altStream;
        leftCoverSource = &state().images[1];
    }

    for (int index = 0; index < layoutCount; ++index) {
        const int row = index / cols;
        const int col = index % cols;
        const int cellsInRow = std::min(cols, layoutCount - row * cols);
        const float rowWidth = cellsInRow * cellWidth + (cellsInRow - 1) * gap;
        const float x = snapToPixel(left + margin + (availableWidth - rowWidth) * 0.5f +
                                    col * (cellWidth + gap));
        const float y = snapToPixel(y0 + row * (cellHeight + gap));
        if (index < count) {
            composeCell(ui, state().images[static_cast<std::size_t>(index)],
                        x, y, cellWidth, cellHeight,
                        index == 0 ? leftCover : nullptr,
                        index == 0 ? leftCoverSource : nullptr);
        } else {
            composeDiffCell(ui, x, y, cellWidth, cellHeight);
        }
    }
}

// --- sidebar rendering --------------------------------------------------------

void composeAboutIntegrationRow(eui::Ui& ui, const std::string& id,
                                float x, float y, float width,
                                const std::string& label, bool checked,
                                bool enabled, const std::function<void()>& onTap) {
    constexpr float kHeight = 24.0f;
    const eui::Color hover = eui::Color{1.0f, 1.0f, 1.0f, 0.06f};
    ui.stack(id).position(x, y).size(width, kHeight).content([&] {
        ui.rect(id + ".bg").size(width, kHeight).radius(3.0f)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .states({0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? hover : eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                    enabled ? eui::Color{1.0f, 1.0f, 1.0f, 0.12f}
                            : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .build();
        const float boxY = (kHeight - 14.0f) * 0.5f;
        ui.rect(id + ".box").position(0.0f, boxY).size(14.0f, 14.0f)
            .radius(2.0f)
            .color(checked ? kAccent : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .border(crispLine(1.0f), checked ? kAccent : kBorderStrong)
            .build();
        if (checked) {
            ui.text(id + ".check").position(0.0f, boxY - 1.0f)
                .size(14.0f, 14.0f).text("\xE2\x9C\x93")
                .color(kWhite).fontSize(10.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();
        }
        ui.text(id + ".label").position(22.0f, 0.0f)
            .size(std::max(0.0f, width - 22.0f), kHeight)
            .text(label)
            .color(enabled ? kTextPrimary
                           : eui::Color{1.0f, 1.0f, 1.0f, 0.32f})
            .fontSize(11.0f)
            .verticalAlign(eui::VerticalAlign::Center).build();
        ui.rect(id + ".hit").size(width, kHeight)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .onClick(enabled ? onTap : std::function<void()>{})
            .cursor(enabled ? eui::CursorShape::Hand : eui::CursorShape::Arrow)
            .build();
    }).build();
}

void composeAboutDialog(eui::Ui& ui, const eui::Screen& screen) {
    if (!state().aboutOpen) return;

    const float width = std::min(540.0f, screen.width - 32.0f);
    const float height = std::min(470.0f, screen.height - 32.0f);
    const float x = snapToPixel((screen.width - width) * 0.5f);
    const float y = snapToPixel((screen.height - height) * 0.5f);
    const float innerX = 24.0f;
    const float innerW = width - 48.0f;
    const float buttonsY = height - 48.0f;

    ui.rect("about.backdrop").size(screen.width, screen.height).zIndex(500)
        .color({0.0f, 0.0f, 0.0f, 0.55f})
        .onClick([] {})
        .cursor(eui::CursorShape::Arrow)
        .build();
    ui.stack("about").position(x, y).size(width, height).zIndex(501).content([&] {
        ui.rect("about.bg").size(width, height).color(kPanel)
            .radius(5.0f).border(crispLine(1.0f), kBorderStrong)
            .shadow(24.0f, 0.0f, 10.0f, {0.0f, 0.0f, 0.0f, 0.55f}).build();
        ui.text("about.title").position(innerX, 18.0f)
            .size(innerW, 24.0f).text("About ImView")
            .color(kTextPrimary).fontSize(16.0f).build();
        ui.text("about.version").position(innerX, 52.0f)
            .size(innerW, 18.0f)
            .text(std::string("Version ") + kVersion)
            .color(kTextSecondary).fontSize(11.0f).build();
        ui.text("about.body").position(innerX, 78.0f)
            .size(innerW, 40.0f)
            .text("A Windows desktop viewer for comparing images side by side "
                  "and playing videos. Zoom and pan up to four images together, "
                  "and inspect regions numerically.")
            .color(kTextPrimary).fontSize(11.0f).wrap(true)
            .lineHeight(16.0f).build();
        ui.text("about.project.label").position(innerX, 126.0f)
            .size(innerW, 16.0f).text("PROJECT HOMEPAGE")
            .color(kTextSecondary).fontSize(10.0f).build();
        composeClippedText(ui, "about.project", innerX, 144.0f,
                           innerW, 18.0f, kProjectUrl, kAccent, 11.0f);
        ui.text("about.integration.label").position(innerX, 176.0f)
            .size(innerW, 16.0f)
            .text("DEFAULT APPS / EXPLORER")
            .color(kTextSecondary).fontSize(10.0f).build();
        composeAboutIntegrationRow(
            ui, "about.integration.images", innerX, 196.0f, innerW,
            "Images — Default app candidate",
            state().integrationImages, true, [] { toggleImageIntegration(); });
#if IMVIEW_ENABLE_VIDEO
        composeAboutIntegrationRow(
            ui, "about.integration.videos", innerX, 224.0f, innerW,
            "Videos — Default app candidate",
            state().integrationVideos, true, [] { toggleVideoIntegration(); });
#endif
        const std::string contextLabel = state().integrationContextAvailable
            ? "Right-click — Open with ImView (new window)"
            : "Right-click — unavailable in this build";
        composeAboutIntegrationRow(
            ui, "about.integration.context", innerX, 252.0f, innerW,
            contextLabel, state().integrationContextMenu,
            state().integrationContextAvailable,
            [] { toggleContextMenuIntegration(); });
        ui.text("about.license").position(innerX, 288.0f)
            .size(innerW, 16.0f)
            .text("Released into the public domain under the Unlicense.")
            .color(kTextSecondary).fontSize(10.5f).build();
        composeTextButton(ui, "about.close", width - 256.0f, buttonsY,
                          90.0f, 30.0f, "Close", false, true,
                          [] { closeAboutDialog(); });
        composeTextButton(ui, "about.open", width - 160.0f, buttonsY,
                          136.0f, 30.0f, "Open project page", true, true,
                          [] { imview::shell::openUrl(kProjectUrl); });
    }).build();
}

void composeContextMenu(eui::Ui& ui, const eui::Screen& screen) {
    if (!state().menuOpen) return;
    const bool openMenu = state().menuKind == AppState::MenuKind::Toolbar;
    const float itemHeight = 26.0f;
    const float menuWidth = openMenu ? 196.0f : 210.0f;
    const char* rowLabels[] = {"Open", "Add to main window", "Show in Explorer",
                               "Open with...", "Properties"};
    const char* openLabels[] = {"Open...", "Open folder...", "Toggle sidebar",
                                "About"};
    const char* rowHints[] = {"", "", "", "", ""};
    const char* openHints[] = {"Ctrl+O", "Ctrl+Shift+O", "Ctrl+B", ""};
    const char* const* labels = openMenu ? openLabels : rowLabels;
    const char* const* hints = openMenu ? openHints : rowHints;
    const int itemCount = openMenu ? 4 : 5;
    const float menuHeight = itemHeight * static_cast<float>(itemCount) + 8.0f;
    const float x = snapToPixel(
        std::clamp(state().menuX, 8.0f, std::max(8.0f, screen.width - menuWidth - 8.0f)));
    const float y = snapToPixel(
        std::clamp(state().menuY, 8.0f, std::max(8.0f, screen.height - menuHeight - 8.0f)));

    // Click-anywhere backdrop dismisses the menu; added before the panel so the
    // items stay clickable.
    ui.rect("menu.backdrop").size(screen.width, screen.height)
        .zIndex(300)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([] { setMenuOpen(false); })
        .cursor(eui::CursorShape::Arrow)
        .build();

    ui.stack("menu").position(x, y).size(menuWidth, menuHeight).zIndex(400).content([&] {
        ui.rect("menu.bg").size(menuWidth, menuHeight)
            .color({0.157f, 0.157f, 0.157f, 1.0f}).radius(5.0f)
            .border(crispLine(1.0f), kBorderStrong)
            .shadow(16.0f, 0.0f, 6.0f, {0.0f, 0.0f, 0.0f, 0.5f}).build();
        const int index = state().menuIndex;
        const int paneIndex = state().menuPane;
        for (int item = 0; item < itemCount; ++item) {
            const float itemY = 4.0f + static_cast<float>(item) * itemHeight;
            const std::string itemId = "menu.item" + std::to_string(item);
            const bool itemDisabled = openMenu && state().reviewMode && item < 2;
            ui.stack(itemId).position(4.0f, itemY)
                .size(menuWidth - 8.0f, itemHeight)
                .content([&, item, paneIndex, index, itemId, itemDisabled] {
                    ui.rect(itemId + ".bg").size(menuWidth - 8.0f, itemHeight)
                        .color({0.0f, 0.0f, 0.0f, 0.0f}).radius(3.0f)
                        .states({0.0f, 0.0f, 0.0f, 0.0f},
                                {1.0f, 1.0f, 1.0f, 0.10f},
                                {1.0f, 1.0f, 1.0f, 0.16f}).build();
                    ui.text(itemId + ".label")
                        .position(8.0f, 0.0f).size(menuWidth - 24.0f, itemHeight)
                        .text(labels[item])
                        .color(itemDisabled ? kTextSecondary : kTextPrimary)
                        .fontSize(12.0f)
                        .verticalAlign(eui::VerticalAlign::Center).build();
                    if (hints[item][0] != '\0') {
                        ui.text(itemId + ".hint")
                            .position(8.0f, 0.0f).size(menuWidth - 20.0f, itemHeight)
                            .text(hints[item]).color(
                                itemDisabled ? eui::Color{0.35f, 0.35f, 0.35f, 1.0f}
                                             : kTextSecondary)
                                .fontSize(11.0f)
                            .horizontalAlign(eui::HorizontalAlign::Right)
                            .verticalAlign(eui::VerticalAlign::Center).build();
                    }
                    if (itemDisabled) return;
                    composeClickArea(ui, itemId + ".hit", menuWidth - 8.0f, itemHeight,
                                     [item, openMenu, paneIndex, index] {
                        setMenuOpen(false);
                        if (openMenu) {
                            switch (item) {
                                case 0: openImages(); break;
                                case 1: openFolder(); break;
                                case 2:
                                    state().sidebarVisible = !state().sidebarVisible;
                                    break;
                                case 3: openAboutDialog(); break;
                                default: break;
                            }
                            app::requestUpdate();
                            return;
                        }
                        if (paneIndex < 0 ||
                            paneIndex >= static_cast<int>(state().folders.size())) {
                            app::requestUpdate();
                            return;
                        }
                        const auto& pane = state().folders[static_cast<std::size_t>(paneIndex)];
                        if (index < 0 || index >= static_cast<int>(pane.paths.size())) {
                            app::requestUpdate();
                            return;
                        }
                        const std::string path = pane.paths[static_cast<std::size_t>(index)];
                        switch (item) {
                            case 0: openSelectionInGrid(); break;
                            case 1: addSelectionToGrid(); break;
                            case 2: imview::shell::showInExplorer(path); break;
                            case 3: imview::shell::openWith(path); break;
                            case 4: imview::shell::showProperties(path); break;
                            default: break;
                        }
                        app::requestUpdate();
                    });
                }).build();
        }
    }).build();
}

// One row of a pane's file list: background, "already open" dot, name and the
// click / context-menu surface on top.
void composeSidebarRow(eui::Ui& ui, int paneIndex, const AppState::FolderPane& pane,
                       int index, float width) {
    const std::string paneId = "pane" + std::to_string(paneIndex);
        const float rowY =
            static_cast<float>(index) * kSidebarRowHeight - pane.scroll;
        const std::string rowId = paneId + ".row" + std::to_string(index);
        const bool rowSelected = isSelected(paneIndex, index);
        const bool open = isOpenInGrid(pane.paths[static_cast<std::size_t>(index)]);
        const bool video = index < static_cast<int>(pane.videos.size()) &&
                           pane.videos[static_cast<std::size_t>(index)];
        ui.stack(rowId).position(0.0f, rowY)
            .size(width, kSidebarRowHeight)
            .content([&, index, rowSelected, open, video, rowId, width] {
                const eui::Color base = rowSelected
                    ? eui::Color{0.043f, 0.231f, 0.376f, 1.0f}
                    : eui::Color{0.0f, 0.0f, 0.0f, 0.0f};
                const eui::Color hover = rowSelected
                    ? base
                    : eui::Color{1.0f, 1.0f, 1.0f, 0.055f};
                ui.rect(rowId + ".bg").size(width, kSidebarRowHeight)
                    .color(base).states(base, hover, hover).build();
                if (video) {
                    ui.polygon(rowId + ".icon")
                        .position(6.0f, 7.0f).size(8.0f, 8.0f)
                        .points({{0.0f, 0.0f}, {8.0f, 4.0f}, {0.0f, 8.0f}})
                        .color(rowSelected ? kWhite : kTextSecondary).build();
                } else {
                    ui.rect(rowId + ".icon.frame")
                        .position(5.0f, 7.0f).size(10.0f, 8.0f)
                        .color({0.0f, 0.0f, 0.0f, 0.0f})
                        .border(1.0f, rowSelected ? kWhite : kTextSecondary)
                        .radius(1.0f).build();
                    ui.polygon(rowId + ".icon.mountain")
                        .position(6.0f, 11.0f).size(8.0f, 3.0f)
                        .points({{0.0f, 3.0f}, {2.5f, 0.5f}, {4.0f, 2.0f},
                                 {5.5f, 1.0f}, {8.0f, 3.0f}})
                        .color(rowSelected ? kWhite : kTextSecondary).build();
                }
                if (open) {
                    ui.rect(rowId + ".dot")
                        .position(18.0f, kSidebarRowHeight * 0.5f - 2.0f)
                        .size(4.0f, 4.0f).radius(2.0f).color(kAccent).build();
                }
                ui.text(rowId + ".name").position(26.0f, 0.0f)
                    .size(width - 34.0f, kSidebarRowHeight)
                    .text(pane.names[static_cast<std::size_t>(index)])
                    .color(rowSelected ? kWhite
                                       : (open ? kTextPrimary : kTextSecondary))
                    .fontSize(11.5f)
                    .verticalAlign(eui::VerticalAlign::Center).build();
            }).build();

        ui.stack(rowId + ".hitstack").position(0.0f, rowY)
            .size(width, kSidebarRowHeight).content([&, index, rowId, width] {
                ui.rect(rowId + ".hit").size(width, kSidebarRowHeight)
                    .color({0.0f, 0.0f, 0.0f, 0.0f})
                    .onPress([paneIndex, index](const core::PointerEvent& event,
                                                const core::Rect&) {
                        if (event.button != eui::PointerButton::Left) return;
                        if (isDoubleClick(state().rowClick, paneIndex, index,
                                          core::window::timeSeconds())) {
                            if (state().reviewMode) {
                                // Review: open the matching pair.
                                openReviewPair(index);
                                app::requestUpdate();
                            } else {
                                selectRow(paneIndex, index, false, false);
                                openSingleInGrid(paneIndex, index);
                            }
                            return;
                        }
                        // Review mode disables every other sidebar action.
                        if (state().reviewMode) return;
                        selectRow(paneIndex, index, event.modifiers.control,
                                  event.modifiers.shift);
                    })
                    .onContextMenu([paneIndex, index](const core::PointerEvent& event,
                                                      const core::Rect&) {
                        if (state().reviewMode) return;
                        if (state().folders.size() <=
                            static_cast<std::size_t>(paneIndex)) return;
                        const auto& target =
                            state().folders[static_cast<std::size_t>(paneIndex)];
                        if (index < 0 ||
                            index >= static_cast<int>(target.paths.size())) return;
                        if (!isSelected(paneIndex, index)) {
                            selectRow(paneIndex, index, false, false);
                        }
                        openRowMenu(paneIndex, index, static_cast<float>(event.x),
                                    static_cast<float>(event.y));
                    })
                    .cursor(eui::CursorShape::Arrow)
                    .build();
            }).build();
}

// Overlay scrollbar, shown only when the list overflows. In review mode both
// panes use the longer list's geometry so the two thumbs stay aligned.
void composeSidebarScrollbar(eui::Ui& ui, int paneIndex, const AppState::FolderPane& pane,
                             float listTop, float listHeight, float width) {
    const std::string paneId = "pane" + std::to_string(paneIndex);
    // Overlay scrollbar (VS Code style): only when the list overflows.
    // In review mode both panes use the longer list's geometry so the two
    // thumbs stay visually aligned.
    const float totalRows =
        static_cast<float>(pane.paths.size()) * kSidebarRowHeight;
    float scrollTotal = totalRows;
    if (state().reviewMode && state().folders.size() == 2) {
        scrollTotal = std::max(
            static_cast<float>(state().folders[0].paths.size()) * kSidebarRowHeight,
            static_cast<float>(state().folders[1].paths.size()) * kSidebarRowHeight);
    }
    if (scrollTotal > listHeight + 0.5f) {
        const float maxScroll = scrollTotal - listHeight;
        const float trackHeight = listHeight;
        const float thumbHeight = std::max(
            24.0f, trackHeight * (listHeight / scrollTotal));
        const float progress = maxScroll > 0.0f
            ? std::clamp(pane.scroll / maxScroll, 0.0f, 1.0f)
            : 0.0f;
        const float thumbY = listTop + (trackHeight - thumbHeight) * progress;
        ui.rect(paneId + ".scrollbar").position(width - 7.0f, thumbY)
            .size(4.0f, thumbHeight).radius(2.0f)
            .color({1.0f, 1.0f, 1.0f, 0.22f})
            .states({1.0f, 1.0f, 1.0f, 0.22f},
                    {1.0f, 1.0f, 1.0f, 0.38f},
                    {1.0f, 1.0f, 1.0f, 0.50f})
            .build();
        // Wider invisible grab area: dragging scrolls the pane.
        const float travel = trackHeight - thumbHeight;
        ui.rect(paneId + ".scrollbar.hit").position(width - 10.0f, thumbY)
            .size(9.0f, thumbHeight)
            .color({0.0f, 0.0f, 0.0f, 0.0f})
            .acceptedButtons(eui::PointerButton::Left)
            .onDrag([paneIndex, maxScroll, travel](const core::dsl::DragEvent& event) {
                if (travel <= 0.0f) return;
                auto& target = state().folders[static_cast<std::size_t>(paneIndex)];
                const float next = target.scroll +
                    static_cast<float>(event.deltaY) * (maxScroll / travel);
                if (state().reviewMode) {
                    setReviewScroll(next);
                } else {
                    target.scroll = std::clamp(next, 0.0f, maxScroll);
                }
                app::requestUpdate();
            })
            .cursor(eui::CursorShape::Arrow)
            .build();
    }

}

void composeSidebar(eui::Ui& ui, const eui::Screen& screen) {
    if (!state().sidebarVisible) return;
    const float top = kToolbarHeight;
    const float totalHeight = screen.height - top;
    const float width = sidebarWidthFor(screen.width);
    state().sidebarWidth = width;
    const float lineWidth = crispLine(1.0f);
    const int paneCount = static_cast<int>(state().folders.size());
    const int visiblePanes = std::max(1, paneCount);
    const float paneHeight = totalHeight / static_cast<float>(visiblePanes);
    ui.stack("sidebar").position(0.0f, top).size(width, totalHeight)
        .zIndex(60).content([&] {
        ui.rect("sidebar.bg").size(width, totalHeight)
            .color({0.094f, 0.094f, 0.094f, 1.0f}).build();
        ui.rect("sidebar.edge").position(width - lineWidth, 0.0f)
            .size(lineWidth, totalHeight).color(kBorder).build();

        if (paneCount == 0) {
            return;
        }

        for (int paneIndex = 0; paneIndex < paneCount; ++paneIndex) {
            auto& pane = state().folders[static_cast<std::size_t>(paneIndex)];
            // The last pane takes the remaining space so rounding cannot make the
            // two panes differ in height.
            const float paneTop = snapToPixel(static_cast<float>(paneIndex) * paneHeight);
            const float paneBottom = paneIndex + 1 == paneCount
                ? totalHeight
                : snapToPixel(static_cast<float>(paneIndex + 1) * paneHeight);
            const float thisPaneHeight = paneBottom - paneTop;
            const float listTop = paneTop + kSidebarHeaderHeight;
            const float listHeight = std::max(0.0f, thisPaneHeight - kSidebarHeaderHeight);
            pane.listHeight = listHeight;
            const std::string paneId = "pane" + std::to_string(paneIndex);
            const int count = static_cast<int>(pane.paths.size());

            // Header: folder name plus add / change / close buttons.
            ui.text(paneId + ".title").position(10.0f, paneTop)
                .size(width - 78.0f, kSidebarHeaderHeight)
                .text(std::filesystem::u8path(pane.dir).filename().u8string())
                .color(kTextSecondary).fontSize(11.0f)
                .verticalAlign(eui::VerticalAlign::Center).build();
            // Header buttons are hidden in review mode: the sidebar is read-only
            // there, only double-clicking a row is allowed.
            if (!state().reviewMode) {
                // Only one pane: offer to add the second folder.
                if (paneCount == 1) {
                    composePaneHeaderButton(ui, paneId + ".add", width - 70.0f,
                                            paneTop + 4.0f, 20.0f, "+",
                                            [] { openFolder(); });
                }
                composePaneHeaderButton(ui, paneId + ".open", width - 46.0f,
                                        paneTop + 4.0f, 20.0f, "...",
                                        [paneIndex] { replacePaneFolder(paneIndex); });
                composePaneHeaderButton(ui, paneId + ".close", width - 24.0f,
                                        paneTop + 4.0f, 20.0f, "x",
                                        [paneIndex] { closePane(paneIndex); });
            }
            ui.rect(paneId + ".divider")
                .position(0.0f, paneTop + kSidebarHeaderHeight - lineWidth)
                .size(width, lineWidth).color(kBorder).build();


            if (count == 0) {
                ui.text(paneId + ".empty").position(10.0f, listTop + 10.0f)
                    .size(width - 20.0f, 36.0f)
                    .text("No media in folder").color(kTextSecondary).fontSize(11.5f)
                    .build();
            }

            const int firstRow =
                std::max(0, static_cast<int>(pane.scroll / kSidebarRowHeight));
            const int visibleRows = static_cast<int>(listHeight / kSidebarRowHeight) + 2;
            const int lastRow = std::min(count, firstRow + visibleRows);

            ui.stack(paneId + ".list").position(0.0f, listTop)
                .size(width, listHeight).clip().content([&] {
                // Wheel surface first so rows stay clickable on top of it.
                ui.rect(paneId + ".wheel").size(width, std::max(listHeight, 1.0f))
                    .color({0.0f, 0.0f, 0.0f, 0.0f})
                    .onScroll([paneIndex, listHeight](const core::ScrollEvent& event) {
                        if (event.y == 0.0) return;
                        auto& target = state().folders[static_cast<std::size_t>(paneIndex)];
                        if (state().reviewMode) {
                            // Review: both panes share one row-aligned offset.
                            setReviewScroll(target.scroll -
                                            static_cast<float>(event.y) * 40.0f);
                        } else {
                            const float total =
                                static_cast<float>(target.paths.size()) * kSidebarRowHeight;
                            const float maxScroll = std::max(0.0f, total - listHeight);
                            target.scroll = std::clamp(
                                target.scroll - static_cast<float>(event.y) * 40.0f,
                                0.0f, maxScroll);
                        }
                        app::requestUpdate();
                    })
                    .cursor(eui::CursorShape::Arrow)
                    .build();

                for (int index = firstRow; index < lastRow; ++index) {
                    composeSidebarRow(ui, paneIndex, pane, index, width);
                }
            }).build();

            composeSidebarScrollbar(ui, paneIndex, pane, listTop, listHeight, width);
            // Divider between the two panes.
            if (paneIndex + 1 < paneCount) {
                ui.rect(paneId + ".split").position(0.0f, paneBottom - lineWidth)
                    .size(width, lineWidth).color(kBorder).build();
            }
        }
    }).build();
}

// Vertical grab band centred on the sidebar's right edge: half lies inside the
// sidebar, half in the main area.
void composeSidebarResizeHandle(eui::Ui& ui, const eui::Screen& screen, float width) {
    const float top = kToolbarHeight;
    const float height = screen.height - top;
    const float screenWidth = screen.width;
    ui.rect("sidebar.splitter")
        .position(width - kSidebarResizeHandleWidth * 0.5f, top)
        .size(kSidebarResizeHandleWidth, height)
        .zIndex(61)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .states({0.0f, 0.0f, 0.0f, 0.0f},
                {1.0f, 1.0f, 1.0f, 0.07f},
                {0.0f, 0.471f, 0.831f, 0.30f})
        .acceptedButtons(eui::PointerButton::Left)
        .onPress([](const core::PointerEvent& event, const core::Rect&) {
            if (event.button != eui::PointerButton::Left) return;
            state().sidebarDragStartWidth = state().sidebarWidth;
        })
        .onDrag([screenWidth](const core::dsl::DragEvent& event) {
            if (event.button != eui::PointerButton::Left) return;
            const float scale = imview::frame::dpiScale();
            const float delta = scale > 0.0f
                ? static_cast<float>(event.totalX) / scale
                : static_cast<float>(event.totalX);
            state().sidebarWidth = clampSidebarWidth(
                state().sidebarDragStartWidth + delta, screenWidth);
            app::requestUpdate();
        })
        .cursor(eui::CursorShape::Arrow)
        .build();
}

// Accent frame around the region that will receive the current shell drag. The
// frame follows the pointer because the OLE drop target reports DragOver.
void composeDropHighlight(eui::Ui& ui, const eui::Screen& screen, float sidebarWidth) {
    if (state().dropTarget == AppState::DropTarget::None) return;
    constexpr float kTopInset = 1.0f;
    constexpr float kSideInset = 2.0f;
    float x = 0.0f;
    float width = screen.width;
    if (state().dropTarget == AppState::DropTarget::Sidebar) {
        width = sidebarWidth;
    } else if (state().sidebarVisible) {
        x = sidebarWidth;
        width -= sidebarWidth;
    }
    // Align the origin first, then derive the bottom edge from the aligned
    // top so the frame never drifts by the rounding difference.
    const float frameX = snapToPixel(x + kSideInset);
    const float frameY = snapToPixel(kToolbarHeight + kTopInset);
    const float frameWidth = width - kSideInset * 2.0f;
    const float frameHeight = screen.height - kSideInset - frameY;
    const float lineWidth = crispLine(2.0f);
    if (frameWidth <= 0.0f || frameHeight <= 0.0f) return;
    ui.rect("drop.highlight")
        .position(frameX, frameY)
        .size(floorToPixel(frameWidth), floorToPixel(frameHeight))
        .zIndex(62)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .border(lineWidth, kAccent)
        .build();
}

} // namespace

// NOTE: must have external linkage; EUI's entry point calls it via
// qualified name lookup from another translation unit.
void compose(eui::Ui& ui, const eui::Screen& screen) {
    // Keep sync availability and the one-playing-video rule true even after
    // asynchronous opens or a video reaching its end.
    enforceSyncAvailability();
    enforcePlaybackRules();
    // The native Windows move loop blocks EUI's frame loop. With a video in the
    // grid, the toolbar drag surface drives a normal client-side move instead,
    // so decoded frames keep being composed while the window follows the mouse.
    imview::frame::setCustomTitleBarDrag(hasVideoMedia());
    // Refreshes the computed diff (pair or threshold changed) before the grid
    // asks for its texture.
    updateDiffImage();
    // The threshold slider extends the left control cluster; without this the
    // native caption band would swallow its drags.
    const float leftControlsWidth = state().diffMode
        ? 270.0f
        : (hasVideoMedia() ? 302.0f : 80.0f);
    imview::frame::setTitleBarLeftControlsWidth(leftControlsWidth);
    const float sidebarWidth = sidebarWidthFor(screen.width);
    imview::frame::setSidebarSplitter(
        state().sidebarVisible && !state().menuOpen && !state().aboutOpen
            ? sidebarWidth - kSidebarResizeHandleWidth * 0.5f
            : -1.0f);
    ui.stack("root").size(screen.width, screen.height)
        .cursor(eui::CursorShape::Arrow)
        .content([&] {
            if (!state().images.empty()) {
                composeGrid(ui, screen);
            }
            composeSidebar(ui, screen);
            if (state().sidebarVisible) {
                composeSidebarResizeHandle(ui, screen, sidebarWidth);
            }
            composeDropHighlight(ui, screen, sidebarWidth);
            composeToolbar(ui, screen);
            composeContextMenu(ui, screen);
            composeAboutDialog(ui, screen);
        }).build();
}

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("ImView")
        .pageId("imview")
        .clearColor(kBackground)
        .windowSize(1000, 700)
        .centerWindow()
        .minWindowSize(640, 420)
        .decorated(false)  // borderless: ImView draws its own title bar
        .fps(120.0)
        .onStart([] {
            const LaunchRequest request = launchRequest();
            // Maintenance mode: remove every ImView integration key and the
            // extracted helper without touching the user's explicit defaults.
            if (request.uninstallIntegration) {
                std::exit(imview::shell::uninstallIntegration() ? 0 : 1);
            }
            // Serialize the forward-or-create decision so two Explorer launches
            // cannot both decide that no window exists.
            imview::frame::lockInstanceStartup();
            // Explorer double-click: add to the newest window that still has
            // room. The context-menu DLL uses --new-window, which bypasses this
            // path and always creates a fresh window.
            if (!request.newWindow && !request.paths.empty() &&
                imview::frame::forwardOpenPaths(request.paths)) {
                imview::frame::unlockInstanceStartup();
                imview::frame::closeWindow();
                return;
            }

            // 36px title bar; the left cluster holds the menu, optional volume
            // or diff slider, and the right cluster holds the mode chips and
            // window buttons.
            installImageSampler();
            imview::frame::setFileDropHandler(handleFileDrop);
            imview::frame::installBorderlessWindow(kToolbarHeight, 80.0f, 300.0f);
            imview::frame::centerWindow();
            imview::frame::setOpenPathsHandler(handleExternalOpen);

            if (!request.paths.empty()) {
                const std::string folder = launchFolder(request.paths);
                if (!folder.empty()) addFolder(folder);
            }
            // New windows started with media keep the sidebar and add every
            // requested file to the grid.
            addPaths(request.paths, false, false, true);
            imview::frame::unlockInstanceStartup();
        })
        .onShutdown([] {
            // Stop all decoders before the renderer loses its device.
            shutdownMedia();
            imview::frame::shutdownWindowMessaging();
            imview::frame::shutdownFileDrop();
#if defined(EUI_RENDER_BACKEND_OPENGL)
            shutdownImageSampler();
            // GpuSource destructors may run after the GPU device is destroyed;
            // skip their GL cleanup and let the process release the textures.
            gGpuResourcesAlive = false;
#endif
        })
        .onKeyEvent([](const eui::KeyEvent& event) {
            if (state().aboutOpen) {
                if (event.action == eui::KeyAction::Press &&
                    event.key == eui::InputKey::Escape) {
                    closeAboutDialog();
                }
                return;
            }
            // The video that last received pointer attention receives the
            // transport shortcuts. Space stays as review-next in review mode.
            if (ImageEntry* activeVideo = findEntry(activeVideoId())) {
                if (activeVideo->video && activeVideo->isVideo &&
                    (event.action == eui::KeyAction::Press ||
                     event.action == eui::KeyAction::Repeat)) {
                    if (event.key == eui::InputKey::Left &&
                        event.modifiers.control) {
                        stepVideoFrames(activeVideo->id, -1);
                        return;
                    }
                    if (event.key == eui::InputKey::Right &&
                        event.modifiers.control) {
                        stepVideoFrames(activeVideo->id, 1);
                        return;
                    }
                    if (event.key == eui::InputKey::Left) {
                        seekVideoBy(activeVideo->id, -5.0);
                        return;
                    }
                    if (event.key == eui::InputKey::Right) {
                        seekVideoBy(activeVideo->id, 5.0);
                        return;
                    }
                    if (event.key == eui::InputKey::Space &&
                        !state().reviewMode &&
                        event.action == eui::KeyAction::Press) {
                        toggleVideoPlayback(activeVideo->id);
                        return;
                    }
                }
            }
            if (event.action == eui::KeyAction::Press) {
                if (soloActive()) {
                    // In solo mode the digits are a compare pad: hold one to
                    // temporarily show that image over the current one.
                    int peekIndex = 0;
                    switch (event.key) {
                        case eui::InputKey::Digit1: peekIndex = 1; break;
                        case eui::InputKey::Digit2: peekIndex = 2; break;
                        case eui::InputKey::Digit3: peekIndex = 3; break;
                        case eui::InputKey::Digit4: peekIndex = 4; break;
                        case eui::InputKey::Escape:
                            if (state().menuOpen) {
                                setMenuOpen(false);
                                return;
                            }
                            exitSolo();
                            return;
                        default: break;
                    }
                    if (peekIndex > 0) {
                        beginPeek(peekIndex);
                        return;
                    }
                }
                switch (event.key) {
                    // Ctrl shortcuts first: they must win over the bare keys.
                    case eui::InputKey::O:
                        if (event.modifiers.control) {
                            // Review mode keeps its pair: no new images or folders.
                            if (state().reviewMode) return;
                            if (event.modifiers.shift) {
                                openFolder();
                            } else {
                                openImages();
                            }
                            return;
                        }
                        break;
                    case eui::InputKey::B:
                        if (event.modifiers.control) {
                            state().sidebarVisible = !state().sidebarVisible;
                            app::requestUpdate();
                            return;
                        }
                        break;
                    default:
                        break;
                }
                switch (event.key) {
                    case eui::InputKey::Digit1: rotateImage(0); break;
                    case eui::InputKey::Digit2: rotateImage(1); break;
                    case eui::InputKey::Digit3: rotateImage(2); break;
                    case eui::InputKey::Digit4: rotateImage(3); break;
                    case eui::InputKey::Q:
                        beginQSwap();
                        break;
                    case eui::InputKey::Escape:
                        if (state().moveSourceId >= 0) {
                            state().moveSourceId = -1;
                            state().moveTargetId = -1;
                            app::requestUpdate();
                        } else if (state().menuOpen) {
                            setMenuOpen(false);
                        } else {
                            exitSolo();
                        }
                        break;
                    case eui::InputKey::Space:
                        reviewStep(1);
                        break;
                    case eui::InputKey::B:
                        reviewStep(-1);
                        break;
                    case eui::InputKey::P:
                        toggleLocalMode();
                        break;
                    default:
                        break;
                }
            } else if (event.action == eui::KeyAction::Release) {
                switch (event.key) {
                    case eui::InputKey::Q:
                        endQSwap();
                        break;
                    case eui::InputKey::Digit1:
                    case eui::InputKey::Digit2:
                    case eui::InputKey::Digit3:
                    case eui::InputKey::Digit4:
                        endPeek();
                        break;
                    default:
                        break;
                }
            }
        });
    return config;
}

} // namespace app
