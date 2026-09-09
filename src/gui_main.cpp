#include "app/RecorderEngine.h"
#include "app/Shortcut.h"
#include "ui/SaveToasts.h"
#include "audio/AudioSessionScanner.h"
#include "playback/PreviewAudio.h"
#include "playback/ThumbnailSelection.h"
#include "platform/windows/MediaTools.h"
#include "resources/resource.h"
#include "editing/TimelineEdit.h"
#include "app/FfmpegProgress.h"

#include <Windows.h>
#include <windowsx.h>
#include <ShlObj.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <mfplay.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <semaphore>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t windowClassName[] = L"NexPlayMainWindow";
constexpr wchar_t videoWindowClassName[] = L"NexPlayVideoSurface";
constexpr wchar_t fullscreenWindowClassName[] = L"NexPlayFullscreenWindow";
constexpr wchar_t trayMenuWindowClassName[] = L"NexPlayTrayMenuWindow";
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT statusMessage = WM_APP + 2;
constexpr UINT editorDoneMessage = WM_APP + 3;
constexpr UINT autoStartMessage = WM_APP + 4;
constexpr UINT exitFullscreenMessage = WM_APP + 5;
constexpr UINT thumbnailReadyMessage = WM_APP + 6;
constexpr UINT clearEditorFocusMessage = WM_APP + 7;
constexpr UINT togglePlaybackMessage = WM_APP + 8;
constexpr UINT saveProgressMessage = WM_APP + 9;
constexpr UINT activateInstanceMessage = WM_APP + 10;
constexpr UINT editorProgressMessage = WM_APP + 11;
constexpr int saveHotkeyId = 1;
constexpr int stopHotkeyId = 2;
float windowWidth = 1240.0F;
float windowHeight = 820.0F;
constexpr float trayMenuWidth = 310.0F;
constexpr float trayMenuHeight = 238.0F;
constexpr DWORD mainWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
constexpr DWORD mainWindowExStyle = WS_EX_APPWINDOW;

void initializeCustomFrame(HWND window) {
    // WS_CAPTION stays for native maximize/restore transitions. Force Windows to
    // recalculate the non-client area before the first visible frame.
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

constexpr D2D1_COLOR_F background = {0.035F, 0.039F, 0.047F, 1.0F};
constexpr D2D1_COLOR_F sidebar = {0.048F, 0.052F, 0.063F, 1.0F};
constexpr D2D1_COLOR_F card = {0.065F, 0.071F, 0.084F, 1.0F};
constexpr D2D1_COLOR_F field = {0.090F, 0.098F, 0.114F, 1.0F};
constexpr D2D1_COLOR_F border = {0.145F, 0.157F, 0.180F, 1.0F};
D2D1_COLOR_F primary = {0.435F, 0.259F, 1.0F, 1.0F};
D2D1_COLOR_F primaryHover = {0.565F, 0.400F, 1.0F, 1.0F};
D2D1_COLOR_F accentSecondary = {0.176F, 0.651F, 1.0F, 1.0F};
constexpr D2D1_COLOR_F white = {0.965F, 0.969F, 0.988F, 1.0F};
constexpr D2D1_COLOR_F muted = {0.565F, 0.604F, 0.659F, 1.0F};
constexpr D2D1_COLOR_F green = {0.173F, 0.949F, 0.616F, 1.0F};
constexpr D2D1_COLOR_F red = {1.0F, 0.286F, 0.376F, 1.0F};

enum class Page { replay, clips, editor, settings };
enum class HitTarget {
    none,
    minimize,
    maximize,
    close,
    replayPage,
    clipsPage,
    startStop,
    save,
    refreshAudio,
    microphone,
    createAudioGroup,
    audioGroupName,
    confirmAudioGroup,
    cancelAudioGroup,
    openClips,
    durationField,
    resolutionField,
    fpsField,
    bitrateField,
    editorBack,
    editorPlay,
    editorSave,
    editorName,
    editorFullscreen,
    editorMergeAudio,
    editorCutMode,
    settingsPage,
    autostartToggle,
    autoBufferToggle,
    saveHotkey,
    stopHotkey,
    saveHotkeyToggle,
    stopHotkeyToggle,
    toastTopLeft,
    toastTopRight,
    toastBottomLeft,
    toastBottomRight,
    accentPlane,
    accentHue,
    count,
};

enum class DragHandle { none, start, end, playhead, audioStart, audioEnd };
enum class HotkeyCapture { none, save, stop };
enum class ColorDrag { none, plane, hue };

struct Rect final {
    float left{};
    float top{};
    float right{};
    float bottom{};

    [[nodiscard]] bool contains(const float x, const float y) const noexcept {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
    [[nodiscard]] D2D1_RECT_F d2d() const noexcept {
        return D2D1::RectF(left, top, right, bottom);
    }
};

// Shared layout for painting, pointer hit tests and the native video surface.
constexpr float contentLeft = 236.0F;
constexpr float audioRowHeight = 42.0F;
constexpr float editorAudioRowHeight = 46.0F;
float contentRight = 1208;
Rect minimizeRect, maximizeRect, closeRect;
Rect replayNavRect, clipsNavRect, settingsNavRect;
Rect startRect, saveRect, refreshRect, microphoneRect, createAudioGroupRect;
Rect audioGroupDialogRect, audioGroupNameRect, cancelAudioGroupRect, confirmAudioGroupRect;
Rect openClipsRect, clipsListRect, durationFieldRect, resolutionFieldRect, fpsFieldRect, bitrateFieldRect;
Rect editorBackRect, editorPlayRect, editorFullscreenRect, editorMergeAudioRect, editorCutModeRect;
Rect editorSaveRect, editorNameRect, editorTimelineRect;
Rect autostartRect, autoBufferRect, saveHotkeyRect, stopHotkeyRect;
Rect saveHotkeyToggleRect, stopHotkeyToggleRect, notificationsPanelRect;
std::array<Rect, 4> toastCornerRects;
constexpr std::array toastCornerLabels{L"Lewy górny", L"Prawy górny", L"Lewy dolny",
                                       L"Prawy dolny"};
Rect accentPlaneRect, accentHueRect, accentPreviewRect;
Rect replayHeroRect, qualityPanelRect, audioPanelRect, audioRowsRect;
Rect previewPanelRect, videoSurfaceRect, inspectorRect, timelinePanelRect, editorAudioRowsRect;
Rect startupPanelRect, hotkeysPanelRect, colorPanelRect;
int audioVisibleRows = 5, editorVisibleTracks = 3;
int clipColumns = 3, clipVisibleRows = 2;
float clipWidth = 310, clipHeight = 244;

void computeLayout(const float widthPixels, const float heightPixels) {
    windowWidth = widthPixels;
    windowHeight = heightPixels;
    contentRight = windowWidth - 32;
    const float width = contentRight - contentLeft;
    minimizeRect = {windowWidth - 138, 0, windowWidth - 92, 44};
    maximizeRect = {windowWidth - 92, 0, windowWidth - 46, 44};
    closeRect = {windowWidth - 46, 0, windowWidth, 44};
    replayNavRect = {12, 126, 196, 172};
    clipsNavRect = {12, 178, 196, 224};
    settingsNavRect = {12, 230, 196, 276};
    const float split = contentRight - 336;
    replayHeroRect = {contentLeft, 166, split - 16, 408};
    qualityPanelRect = {split, 166, contentRight, 408};
    startRect = {contentLeft + 24, 335, contentLeft + 180, 377};
    saveRect = {contentLeft + 192, 335, split - 40, 377};
    durationFieldRect = {split + 20, 278, split + 158, 326};
    fpsFieldRect = {split + 174, 278, contentRight - 20, 326};
    bitrateFieldRect = {split + 174, 346, contentRight - 20, 390};
    resolutionFieldRect = {split + 20, 346, split + 158, 390};
    audioPanelRect = {contentLeft, 430, contentRight, windowHeight - 60};
    audioRowsRect = {contentLeft + 16, 530, contentRight - 16, windowHeight - 76};
    audioVisibleRows =
        std::max(1, static_cast<int>((audioRowsRect.bottom - audioRowsRect.top) / audioRowHeight));
    refreshRect = {contentRight - 110, 452, contentRight - 20, 488};
    microphoneRect = {contentRight - 250, 452, contentRight - 126, 488};
    createAudioGroupRect = {contentRight - 360, 452, contentRight - 266, 488};
    const float cx = windowWidth * 0.5F, cy = windowHeight * 0.5F;
    audioGroupDialogRect = {cx - 256, cy - 128, cx + 256, cy + 128};
    audioGroupNameRect = {cx - 224, cy - 22, cx + 224, cy + 24};
    cancelAudioGroupRect = {cx - 18, cy + 64, cx + 96, cy + 104};
    confirmAudioGroupRect = {cx + 108, cy + 64, cx + 224, cy + 104};
    openClipsRect = {contentRight - 142, 96, contentRight, 134};
    clipsListRect = {contentLeft, 170, contentRight, windowHeight - 60};
    clipColumns = std::max(2, static_cast<int>((width + 20) / 300));
    clipWidth = (width - (clipColumns - 1) * 20) / clipColumns;
    clipHeight = clipWidth * 9 / 16 + 76;
    clipVisibleRows = std::max(
        1, static_cast<int>((clipsListRect.bottom - clipsListRect.top + 20) / (clipHeight + 20)));
    editorBackRect = {contentLeft, 78, contentLeft + 104, 114};
    const float timelineTop =
        std::clamp(windowHeight * 0.56F, 398.0F, std::max(398.0F, windowHeight - 260));
    previewPanelRect = {contentLeft, 134, contentRight - 280, timelineTop - 16};
    videoSurfaceRect = {previewPanelRect.left + 8, 166, previewPanelRect.right - 8,
                        previewPanelRect.bottom - 54};
    editorPlayRect = {previewPanelRect.left + 12, previewPanelRect.bottom - 44,
                      previewPanelRect.left + 52, previewPanelRect.bottom - 8};
    editorFullscreenRect = {previewPanelRect.right - 52, previewPanelRect.bottom - 44,
                            previewPanelRect.right - 12, previewPanelRect.bottom - 8};
    inspectorRect = {contentRight - 264, 134, contentRight, timelineTop - 16};
    editorNameRect = {inspectorRect.left + 16, 214, contentRight - 16, 256};
    editorMergeAudioRect = {inspectorRect.left + 16, 274, contentRight - 16, 312};
    editorCutModeRect = {inspectorRect.left + 16, 326, contentRight - 16, 362};
    editorSaveRect = {contentRight - 184, 78, contentRight, 116};
    timelinePanelRect = {contentLeft, timelineTop, contentRight, windowHeight - 60};
    editorTimelineRect = {contentLeft + 208, timelineTop + 86, contentRight - 20,
                          timelineTop + 122};
    editorAudioRowsRect = {contentLeft + 16, timelineTop + 134, contentRight - 16,
                           windowHeight - 74};
    editorVisibleTracks =
        std::max(1, static_cast<int>((editorAudioRowsRect.bottom - editorAudioRowsRect.top) /
                                     editorAudioRowHeight));
    const float settingsSplit = contentLeft + width * 0.57F;
    startupPanelRect = {contentLeft, 166, settingsSplit - 16, 408};
    hotkeysPanelRect = {contentLeft, 426, settingsSplit - 16, windowHeight - 60};
    colorPanelRect = {settingsSplit, 166, contentRight, windowHeight - 254};
    notificationsPanelRect = {settingsSplit, windowHeight - 238, contentRight, windowHeight - 60};
    const float cornerWidth = (contentRight - settingsSplit - 60) / 2;
    for (int i = 0; i < 4; ++i) {
        const float x = settingsSplit + 24 + (i % 2) * (cornerWidth + 12);
        const float y = notificationsPanelRect.top + 78 + (i / 2) * 40;
        toastCornerRects[i] = {x, y, x + cornerWidth, y + 32};
    }
    autostartRect = {contentLeft + 20, 236, settingsSplit - 36, 304};
    autoBufferRect = {contentLeft + 20, 316, settingsSplit - 36, 390};
    saveHotkeyRect = {contentLeft + 20, 502, settingsSplit - 36, 556};
    stopHotkeyRect = {contentLeft + 20, 566, settingsSplit - 36, 620};
    saveHotkeyToggleRect = {saveHotkeyRect.right - 52, saveHotkeyRect.top + 16,
                            saveHotkeyRect.right - 12, saveHotkeyRect.top + 38};
    stopHotkeyToggleRect = {stopHotkeyRect.right - 52, stopHotkeyRect.top + 16,
                            stopHotkeyRect.right - 12, stopHotkeyRect.top + 38};
    const float pickerSide =
        std::min(colorPanelRect.right - colorPanelRect.left - 76, colorPanelRect.bottom - 374);
    accentPlaneRect = {settingsSplit + 24, 262, settingsSplit + 24 + pickerSide, 262 + pickerSide};
    accentHueRect = {accentPlaneRect.right + 14, 262, accentPlaneRect.right + 32, 262 + pickerSide};
    accentPreviewRect = {settingsSplit + 24, accentPlaneRect.bottom + 28, contentRight - 24,
                         accentPlaneRect.bottom + 92};
}
void updateLayout(const HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    computeLayout(static_cast<float>(std::max(1L, client.right)),
                  static_cast<float>(std::max(1L, client.bottom)));
}
[[nodiscard]] D2D1_POINT_2F designPoint(const HWND, const float x, const float y) {
    return {x, y};
}
[[nodiscard]] RECT physicalRect(const HWND, const Rect rectangle) {
    return {static_cast<LONG>(std::lround(rectangle.left)),
            static_cast<LONG>(std::lround(rectangle.top)),
            static_cast<LONG>(std::lround(rectangle.right)),
            static_cast<LONG>(std::lround(rectangle.bottom))};
}
[[nodiscard]] Rect aspectFitRect(const HWND, const Rect bounds, const float ratio) {
    const float w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    if (w <= 0 || h <= 0 || ratio <= 0)
        return bounds;
    const float fitW = std::min(w, h * ratio), fitH = fitW / ratio;
    const float cx = (bounds.left + bounds.right) * 0.5F, cy = (bounds.top + bounds.bottom) * 0.5F;
    return {cx - fitW * 0.5F, cy - fitH * 0.5F, cx + fitW * 0.5F, cy + fitH * 0.5F};
}
[[nodiscard]] Rect fixedAspectRect(const HWND, const Rect bounds) {
    return bounds;
}
[[nodiscard]] Rect clipCardRect(const int visible) {
    const float left = contentLeft + (visible % clipColumns) * (clipWidth + 20);
    const float top = clipsListRect.top + (visible / clipColumns) * (clipHeight + 20);
    return {left, top, left + clipWidth, top + clipHeight};
}

struct AudioRow final {
    DWORD processId{};
    std::wstring name;
    bool included{true};
    bool groupSelected{};
    std::wstring groupName;
};

struct EditorAudioTrack final {
    int streamIndex{};
    std::wstring name;
    bool included{true};
    double start{};
    double end{};
    std::uint32_t trackId{};
};

struct RemovedEditorAudioTrack {
    std::size_t index{};
    EditorAudioTrack track;
};

struct ClipPreview final {
    double duration{};
    double framesPerSecond{};
    std::map<int, std::vector<std::uint8_t>> frames;
    int lastScrubFrame{-1};
};

struct ClipRow final {
    std::filesystem::path path;
    std::uintmax_t size{};
    std::filesystem::file_time_type modified{};
    std::shared_ptr<ClipPreview> preview;
};

struct ThumbnailResult final {
    std::filesystem::path clip;
    int frameIndex{};
    int firstFrameIndex{};
    double duration{};
    double framesPerSecond{};
    std::vector<std::vector<std::uint8_t>> frames;
};

std::counting_semaphore<2> thumbnailSlots{2};

struct ResolutionPreset final {
    const wchar_t* label;
    std::uint32_t width;
    std::uint32_t height;
};

constexpr std::array resolutionPresets{
    ResolutionPreset{L"Natywna", 0, 0},
    ResolutionPreset{L"1920 × 1080", 1'920, 1'080},
    ResolutionPreset{L"2560 × 1440", 2'560, 1'440},
    ResolutionPreset{L"3840 × 2160", 3'840, 2'160},
    ResolutionPreset{L"7680 × 4320", 7'680, 4'320},
};

struct AppState final {
    nexplay::app::RecorderEngine engine;
    Page page{Page::replay};
    HitTarget hover{HitTarget::none};
    std::vector<AudioRow> audioRows;
    std::vector<ClipRow> clips;
    std::set<std::pair<std::filesystem::path, int>> thumbnailsPending;
    std::set<std::pair<std::filesystem::path, int>> thumbnailFailures;
    std::set<std::filesystem::path> thumbnailClipActive;
    std::map<std::filesystem::path, int> thumbnailDesiredFrame;
    std::map<std::filesystem::path, std::shared_ptr<ClipPreview>> thumbnailMemory;
    int audioScroll{};
    bool audioGroupDialogOpen{};
    std::wstring audioGroupName;
    int clipScroll{};
    bool microphone{true};
    HitTarget activeField{HitTarget::none};
    bool replaceFieldOnInput{};
    std::wstring durationText{L"10"};
    std::wstring fpsText{L"60"};
    std::wstring bitrateText{L"25"};
    std::size_t resolutionPreset{};
    std::filesystem::path selectedClip;
    std::wstring editorName;
    double editorDuration{};
    LONG editorVideoWidth{};
    LONG editorVideoHeight{};
    double playPosition{};
    double trimStart{};
    double trimEnd{};
    std::vector<EditorAudioTrack> editorAudioTracks;
    std::vector<RemovedEditorAudioTrack> removedEditorAudioTracks;
    nexplay::playback::PreviewAudio previewAudio;
    int editorAudioScroll{};
    int activeEditorAudioTrack{-1};
    bool mergeEditorAudio{};
    bool cutEditorSelection{};
    bool editorExporting{};
    unsigned int editorExportId{};
    int editorExportPercent{};
    std::filesystem::path editorExportClip;
    DragHandle dragHandle{DragHandle::none};
    bool playing{};
    bool fullscreen{};
    bool fullscreenScrubbing{};
    int embeddedVideoRefreshFrames{};
    bool clipContextMenuOpen{};
    std::filesystem::path clipContextMenuClip;
    Rect clipContextMenuRect{};
    bool autostart{};
    bool autoBuffer{};
    bool saveHotkeyEnabled{true};
    bool stopHotkeyEnabled{true};
    nexplay::ui::ToastCorner toastCorner{nexplay::ui::ToastCorner::bottomRight};
    nexplay::ui::SaveToasts saveToasts;
    UINT saveHotkeyVk{VK_F8};
    UINT saveHotkeyModifiers{};
    UINT stopHotkeyVk{VK_F9};
    UINT stopHotkeyModifiers{};
    HotkeyCapture hotkeyCapture{HotkeyCapture::none};
    ColorDrag colorDrag{ColorDrag::none};
    float accentHue{0.704F};
    float accentSaturation{0.741F};
    float accentValue{1.0F};
    float autostartAnimation{};
    float autoBufferAnimation{};
    float microphoneAnimation{1.0F};
    float mouseX{-1.0F};
    float mouseY{-1.0F};
    BOOL clientAnimations{TRUE};
    Page renderedPage{Page::replay};
    ULONGLONG pageTransitionStart{};
    std::array<float, static_cast<std::size_t>(HitTarget::count)> hoverAnimation{};
    std::wstring status{L"Gotowy do uruchomienia"};
    NOTIFYICONDATAW tray{};
    HWND trayMenuWindow{};
    HWND videoWindow{};
    HWND fullscreenWindow{};
    HWND fullscreenVideoWindow{};
    HWND mainWindow{};
    ComPtr<IMFPMediaPlayer> mediaPlayer;
    ComPtr<IMFPMediaPlayer> fullscreenPlayer;
    std::vector<std::jthread> editorJobs;
    std::vector<std::jthread> thumbnailJobs;
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IWICImagingFactory> wicFactory;
    ComPtr<ID2D1HwndRenderTarget> windowRenderTarget;
    ComPtr<ID2D1RenderTarget> renderTarget;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> thumbnailBitmaps;
    ComPtr<ID2D1HwndRenderTarget> fullscreenRenderTarget;
    ComPtr<ID2D1SolidColorBrush> fullscreenBrush;
    ComPtr<ID2D1HwndRenderTarget> trayMenuRenderTarget;
    ComPtr<ID2D1SolidColorBrush> trayMenuBrush;
    ComPtr<IDWriteTextFormat> titleFormat;
    ComPtr<IDWriteTextFormat> headingFormat;
    ComPtr<IDWriteTextFormat> bodyFormat;
    ComPtr<IDWriteTextFormat> smallFormat;
    ComPtr<IDWriteTextFormat> buttonFormat;
    ComPtr<IDWriteTextFormat> brandFormat;
    ComPtr<IDWriteTextFormat> trayMenuTitleFormat;
    ComPtr<IDWriteTextFormat> trayMenuBodyFormat;
    ComPtr<IDWriteTextFormat> trayMenuSmallFormat;
    int trayMenuHover{-1};
    std::array<float, 3> trayMenuHoverAnimation{};
};

HHOOK keyboardHook{};
HWND keyboardHookWindow{};
AppState* keyboardHookState{};
std::array<bool, 256> keyboardKeysDown{};

void updateEditorVisibility(AppState& state);
void updateEditorPlayback(AppState& state);
void setStatus(HWND window, AppState& state, std::wstring text);
void exitFullscreen(AppState& state);
void seekEditor(AppState& state, double seconds);
[[nodiscard]] std::wstring timeLabel(double seconds);
[[nodiscard]] std::wstring preciseTimeLabel(double seconds);
LRESULT CALLBACK fullscreenWindowProcedure(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK trayMenuWindowProcedure(HWND, UINT, WPARAM, LPARAM);

LRESULT CALLBACK videoWindowProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    HWND owner = reinterpret_cast<HWND>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        owner = static_cast<HWND>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
    }
    auto* state = owner != nullptr
        ? reinterpret_cast<AppState*>(GetWindowLongPtrW(owner, GWLP_USERDATA))
        : nullptr;
    IMFPMediaPlayer* player = nullptr;
    if (state != nullptr) {
        player = window == state->fullscreenVideoWindow
            ? state->fullscreenPlayer.Get()
            : state->mediaPlayer.Get();
    }
    if (message == WM_KEYDOWN &&
        (wParam == VK_SPACE || wParam == VK_LEFT || wParam == VK_RIGHT ||
         wParam == VK_HOME || wParam == VK_END)) {
        if (const HWND parent = GetParent(window); parent != nullptr) {
            SendMessageW(parent, message, wParam, lParam);
        }
        return 0;
    }
    if (message == WM_LBUTTONDOWN && owner != nullptr) {
        PostMessageW(owner, clearEditorFocusMessage, 0, 0);
    }
    if (message == WM_LBUTTONUP && owner != nullptr) {
        PostMessageW(owner, togglePlaybackMessage, 0, 0);
        return 0;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        if (owner != nullptr) PostMessageW(owner, exitFullscreenMessage, 0, 0);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        if (player != nullptr) {
            player->UpdateVideo();
        } else {
            FillRect(paint.hdc, &paint.rcPaint,
                     reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_SIZE || (message == WM_SHOWWINDOW && wParam != FALSE)) {
        if (player != nullptr) player->UpdateVideo();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

[[nodiscard]] std::filesystem::path clipsDirectory() {
    PWSTR videosPath{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videosPath)) ||
        videosPath == nullptr) {
        throw std::runtime_error("Nie mozna otworzyc folderu Wideo.");
    }
    const auto path = std::filesystem::path(videosPath) / L"NexPlay" / L"Clips";
    CoTaskMemFree(videosPath);
    std::filesystem::create_directories(path);
    return path;
}

constexpr wchar_t settingsRegistryPath[] = L"Software\\NexPlay";
constexpr wchar_t runRegistryPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

[[nodiscard]] DWORD readSettingDword(const wchar_t* name, const DWORD fallback) {
    DWORD value{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, settingsRegistryPath, name,
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

void writeSettingDword(const wchar_t* name, const DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, settingsRegistryPath, name,
                    REG_DWORD, &value, sizeof(value));
}

[[nodiscard]] D2D1_COLOR_F hsvColor(
    float hue, const float saturation, const float value) noexcept {
    hue -= std::floor(hue);
    const float sector = hue * 6.0F;
    const int index = static_cast<int>(std::floor(sector));
    const float fraction = sector - std::floor(sector);
    const float p = value * (1.0F - saturation);
    const float q = value * (1.0F - saturation * fraction);
    const float t = value * (1.0F - saturation * (1.0F - fraction));
    switch (index % 6) {
    case 0: return {value, t, p, 1.0F};
    case 1: return {q, value, p, 1.0F};
    case 2: return {p, value, t, 1.0F};
    case 3: return {p, q, value, 1.0F};
    case 4: return {t, p, value, 1.0F};
    default: return {value, p, q, 1.0F};
    }
}

void rgbToHsv(const D2D1_COLOR_F color, float& hue, float& saturation, float& value) {
    const float maximum = std::max({color.r, color.g, color.b});
    const float minimum = std::min({color.r, color.g, color.b});
    const float delta = maximum - minimum;
    value = maximum;
    saturation = maximum <= 0.0001F ? 0.0F : delta / maximum;
    if (delta <= 0.0001F) {
        hue = 0.0F;
    } else if (maximum == color.r) {
        hue = std::fmod((color.g - color.b) / delta, 6.0F) / 6.0F;
    } else if (maximum == color.g) {
        hue = ((color.b - color.r) / delta + 2.0F) / 6.0F;
    } else {
        hue = ((color.r - color.g) / delta + 4.0F) / 6.0F;
    }
    if (hue < 0.0F) hue += 1.0F;
}

void applyAccentColor(AppState& state) {
    primary = hsvColor(state.accentHue, state.accentSaturation, state.accentValue);
    primaryHover = {
        primary.r + (1.0F - primary.r) * 0.20F,
        primary.g + (1.0F - primary.g) * 0.20F,
        primary.b + (1.0F - primary.b) * 0.20F,
        1.0F,
    };
    accentSecondary = hsvColor(
        state.accentHue + 0.075F,
        std::clamp(state.accentSaturation * 0.88F, 0.0F, 1.0F),
        std::clamp(state.accentValue * 1.05F, 0.0F, 1.0F));
    state.saveToasts.configure(state.toastCorner, primary);
}

[[nodiscard]] DWORD packedAccentColor(const AppState& state) {
    const auto color = hsvColor(
        state.accentHue, state.accentSaturation, state.accentValue);
    const DWORD redChannel = static_cast<DWORD>(std::lround(color.r * 255.0F));
    const DWORD greenChannel = static_cast<DWORD>(std::lround(color.g * 255.0F));
    const DWORD blueChannel = static_cast<DWORD>(std::lround(color.b * 255.0F));
    return (redChannel << 16) | (greenChannel << 8) | blueChannel;
}

void saveAccentColor(const AppState& state) {
    writeSettingDword(L"AccentColor", packedAccentColor(state));
}

[[nodiscard]] std::wstring accentHexLabel(const AppState& state) {
    wchar_t text[16]{};
    swprintf_s(text, L"#%06lX", packedAccentColor(state));
    return text;
}

[[nodiscard]] bool isModifierKey(const WPARAM key) noexcept {
    return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
        key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
        key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
        key == VK_LWIN || key == VK_RWIN;
}

[[nodiscard]] UINT pressedHotkeyModifiers() noexcept {
    UINT modifiers{};
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) modifiers |= MOD_CONTROL;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= MOD_ALT;
    return modifiers;
}

[[nodiscard]] std::wstring keyName(const UINT virtualKey) {
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
    }
    if ((virtualKey >= L'0' && virtualKey <= L'9') ||
        (virtualKey >= L'A' && virtualKey <= L'Z')) {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    switch (virtualKey) {
    case VK_SPACE: return L"Spacja";
    case VK_LEFT: return L"←";
    case VK_RIGHT: return L"→";
    case VK_UP: return L"↑";
    case VK_DOWN: return L"↓";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_RETURN: return L"Enter";
    case VK_TAB: return L"Tab";
    case VK_BACK: return L"Backspace";
    default: break;
    }
    UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    if (virtualKey == VK_LEFT || virtualKey == VK_RIGHT || virtualKey == VK_UP ||
        virtualKey == VK_DOWN || virtualKey == VK_PRIOR || virtualKey == VK_NEXT ||
        virtualKey == VK_END || virtualKey == VK_HOME || virtualKey == VK_INSERT ||
        virtualKey == VK_DELETE || virtualKey == VK_DIVIDE || virtualKey == VK_NUMLOCK) {
        scanCode |= 0x100;
    }
    wchar_t name[64]{};
    if (GetKeyNameTextW(static_cast<LONG>(scanCode << 16), name,
                        static_cast<int>(std::size(name))) > 0) {
        return name;
    }
    return L"Klawisz " + std::to_wstring(virtualKey);
}

[[nodiscard]] std::wstring hotkeyLabel(const UINT modifiers, const UINT virtualKey) {
    std::wstring label;
    const auto append = [&label](const wchar_t* part) {
        if (!label.empty()) label += L" + ";
        label += part;
    };
    if ((modifiers & MOD_CONTROL) != 0) append(L"Ctrl");
    if ((modifiers & MOD_ALT) != 0) append(L"Alt");
    if ((modifiers & MOD_SHIFT) != 0) append(L"Shift");
    if (!label.empty()) label += L" + ";
    label += keyName(virtualKey);
    return label;
}

LRESULT CALLBACK passiveKeyboardProcedure(
    const int code, const WPARAM message, const LPARAM parameter) {
    if (code == HC_ACTION && keyboardHookState != nullptr &&
        keyboardHookWindow != nullptr) {
        const auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(parameter);
        if (key->vkCode < keyboardKeysDown.size()) {
            const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            const bool released = message == WM_KEYUP || message == WM_SYSKEYUP;
            if (released) {
                keyboardKeysDown[key->vkCode] = false;
            } else if (pressed) {
                const bool firstPress = !keyboardKeysDown[key->vkCode];
                keyboardKeysDown[key->vkCode] = true;
                if (firstPress &&
                    keyboardHookState->hotkeyCapture == HotkeyCapture::none) {
                    UINT modifiers{};
                    if (keyboardKeysDown[VK_CONTROL] ||
                        keyboardKeysDown[VK_LCONTROL] || keyboardKeysDown[VK_RCONTROL]) {
                        modifiers |= MOD_CONTROL;
                    }
                    if (keyboardKeysDown[VK_SHIFT] ||
                        keyboardKeysDown[VK_LSHIFT] || keyboardKeysDown[VK_RSHIFT]) {
                        modifiers |= MOD_SHIFT;
                    }
                    if (keyboardKeysDown[VK_MENU] ||
                        keyboardKeysDown[VK_LMENU] || keyboardKeysDown[VK_RMENU]) {
                        modifiers |= MOD_ALT;
                    }
                    if (nexplay::app::shortcutMatches(
                            keyboardHookState->saveHotkeyEnabled, keyboardHookState->saveHotkeyVk,
                            keyboardHookState->saveHotkeyModifiers, key->vkCode, modifiers)) {
                        PostMessageW(keyboardHookWindow, WM_HOTKEY, saveHotkeyId, 0);
                    } else if (nexplay::app::shortcutMatches(keyboardHookState->stopHotkeyEnabled,
                                                             keyboardHookState->stopHotkeyVk,
                                                             keyboardHookState->stopHotkeyModifiers,
                                                             key->vkCode, modifiers)) {
                        PostMessageW(keyboardHookWindow, WM_HOTKEY, stopHotkeyId, 0);
                    }
                }
            }
        }
    }
    // Pasywny hook nigdy nie przechwytuje klawisza — aplikacja na pierwszym
    // planie nadal otrzymuje F8/F9 lub skonfigurowaną kombinację.
    return CallNextHookEx(keyboardHook, code, message, parameter);
}

[[nodiscard]] bool autostartIsEnabled() {
    wchar_t command[1024]{};
    DWORD size = sizeof(command);
    return RegGetValueW(HKEY_CURRENT_USER, runRegistryPath, L"NexPlay",
                        RRF_RT_REG_SZ, nullptr, command, &size) == ERROR_SUCCESS;
}

[[nodiscard]] bool setAutostartEnabled(const bool enabled) {
    if (!enabled) {
        const LSTATUS result = RegDeleteKeyValueW(
            HKEY_CURRENT_USER, runRegistryPath, L"NexPlay");
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
    wchar_t executable[32'768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length == std::size(executable)) return false;
    const std::wstring command = L"\"" + std::wstring(executable, length) +
        L"\" --autostart";
    return RegSetKeyValueW(
        HKEY_CURRENT_USER, runRegistryPath, L"NexPlay", REG_SZ,
        command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) ==
        ERROR_SUCCESS;
}

void loadPersistentSettings(AppState& state) {
    state.autostart = autostartIsEnabled();
    state.autoBuffer = readSettingDword(L"AutoBuffer", 0) != 0;
    state.saveHotkeyEnabled = readSettingDword(L"SaveHotkeyEnabled", 1) != 0;
    state.stopHotkeyEnabled = readSettingDword(L"StopHotkeyEnabled", 1) != 0;
    state.toastCorner =
        static_cast<nexplay::ui::ToastCorner>(std::min(3UL, readSettingDword(L"ToastCorner", 3)));
    state.autostartAnimation = state.autostart ? 1.0F : 0.0F;
    state.autoBufferAnimation = state.autoBuffer ? 1.0F : 0.0F;
    state.microphone = readSettingDword(L"Microphone", 1) != 0;
    state.microphoneAnimation = state.microphone ? 1.0F : 0.0F;
    state.durationText = std::to_wstring(
        std::clamp(readSettingDword(L"BufferSeconds", 10), 10UL, 1'200UL));
    state.fpsText = std::to_wstring(
        std::clamp(readSettingDword(L"FramesPerSecond", 60), 1UL, 1'000UL));
    state.bitrateText = std::to_wstring(
        std::clamp(readSettingDword(L"BitrateMbps", 25), 1UL, 1'000UL));
    state.resolutionPreset = std::min<std::size_t>(
        readSettingDword(L"ResolutionPreset", 0), resolutionPresets.size() - 1);
    state.saveHotkeyVk = std::clamp<UINT>(readSettingDword(L"SaveHotkeyVk", VK_F8), 1, 254);
    state.saveHotkeyModifiers = readSettingDword(L"SaveHotkeyModifiers", 0) &
        (MOD_CONTROL | MOD_ALT | MOD_SHIFT);
    state.stopHotkeyVk = std::clamp<UINT>(readSettingDword(L"StopHotkeyVk", VK_F9), 1, 254);
    state.stopHotkeyModifiers = readSettingDword(L"StopHotkeyModifiers", 0) &
        (MOD_CONTROL | MOD_ALT | MOD_SHIFT);
    const DWORD accent = readSettingDword(L"AccentColor", 0x6F42FF);
    const D2D1_COLOR_F accentColor{
        static_cast<float>((accent >> 16) & 0xFF) / 255.0F,
        static_cast<float>((accent >> 8) & 0xFF) / 255.0F,
        static_cast<float>(accent & 0xFF) / 255.0F,
        1.0F,
    };
    rgbToHsv(accentColor, state.accentHue, state.accentSaturation, state.accentValue);
    applyAccentColor(state);
}

void savePersistentRecordingSettings(const AppState& state) {
    writeSettingDword(L"AutoBuffer", state.autoBuffer ? 1 : 0);
    writeSettingDword(L"Microphone", state.microphone ? 1 : 0);
    writeSettingDword(L"BufferSeconds", static_cast<DWORD>(wcstoul(state.durationText.c_str(), nullptr, 10)));
    writeSettingDword(L"FramesPerSecond", static_cast<DWORD>(wcstoul(state.fpsText.c_str(), nullptr, 10)));
    writeSettingDword(L"BitrateMbps", static_cast<DWORD>(wcstoul(state.bitrateText.c_str(), nullptr, 10)));
    writeSettingDword(L"ResolutionPreset", static_cast<DWORD>(state.resolutionPreset));
}

[[nodiscard]] std::wstring quoteProcessArgument(const std::wstring& argument) {
    std::wstring quoted(1, L'"');
    std::size_t backslashes{};
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] DWORD runHiddenProcess(const std::vector<std::wstring>& arguments) {
    auto command = nexplay::platform::prepareMediaCommand(arguments);
    if (!command) return ERROR_FILE_NOT_FOUND;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(command->executable.c_str(), command->line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return GetLastError();
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode;
}

[[nodiscard]] std::string runHiddenProcessCapture(
    const std::vector<std::wstring>& arguments) {
    auto command = nexplay::platform::prepareMediaCommand(arguments);
    if (!command) return {};
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe{};
    HANDLE writePipe{};
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(command->executable.c_str(), command->line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &startup, &process)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return {};
    }
    CloseHandle(writePipe);
    std::string output;
    std::array<char, 512> buffer{};
    DWORD read{};
    while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) &&
           read > 0) {
        output.append(buffer.data(), read);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return output;
}

[[nodiscard]] std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

[[nodiscard]] std::vector<EditorAudioTrack> probeEditorAudioTracks(
    const std::filesystem::path& clip) {
    const std::string output = runHiddenProcessCapture({
        L"ffprobe.exe", L"-v", L"error", L"-select_streams", L"a",
        L"-show_entries", L"stream=index,id:stream_tags=handler_name,title",
        L"-of", L"default=noprint_wrappers=0:nokey=0",
        quoteProcessArgument(clip.wstring()),
    });
    std::vector<EditorAudioTrack> tracks;
    std::istringstream lines(output);
    std::string line;
    int streamIndex = -1;
    std::uint32_t trackId{};
    std::wstring name;
    const auto finishTrack = [&] {
        if (streamIndex < 0) return;
        if (name.empty()) name = L"Ścieżka audio " + std::to_wstring(tracks.size() + 1);
        tracks.push_back({.streamIndex = streamIndex, .name = std::move(name), .trackId = trackId});
        streamIndex = -1;
        trackId = 0;
        name.clear();
    };
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[STREAM]") {
            streamIndex = -1;
            trackId = 0;
            name.clear();
        } else if (line == "[/STREAM]") {
            finishTrack();
        } else if (line.starts_with("index=")) {
            try { streamIndex = std::stoi(line.substr(6)); }
            catch (...) { streamIndex = -1; }
        } else if (line.starts_with("id=")) {
            try { trackId = std::stoul(line.substr(3), nullptr, 0); }
            catch (...) { trackId = 0; }
        } else if (line.starts_with("TAG:handler_name=")) {
            name = utf8ToWide(line.substr(17));
        } else if (name.empty() && line.starts_with("TAG:title=")) {
            name = utf8ToWide(line.substr(10));
        }
    }
    finishTrack();
    return tracks;
}

[[nodiscard]] std::wstring secondsArgument(const double seconds) {
    wchar_t text[32]{};
    swprintf_s(text, L"%.3f", seconds);
    return text;
}

void removeLegacyThumbnailCache() noexcept {
    wchar_t temporary[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    if (length == 0 || length >= std::size(temporary)) return;
    const auto legacy = std::filesystem::path(temporary) / L"NexPlay" / L"Thumbnails";
    if (legacy.filename() != L"Thumbnails" || legacy.parent_path().filename() != L"NexPlay") {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(legacy, error);
}

[[nodiscard]] double parseFrameRate(const std::string& text) {
    const std::size_t slash = text.find('/');
    if (slash == std::string::npos) return std::stod(text);
    const double numerator = std::stod(text.substr(0, slash));
    const double denominator = std::stod(text.substr(slash + 1));
    return denominator == 0.0 ? 0.0 : numerator / denominator;
}

[[nodiscard]] std::vector<std::vector<std::uint8_t>> splitMjpegFrames(
    const std::string& stream) {
    std::vector<std::vector<std::uint8_t>> frames;
    std::size_t start = std::string::npos;
    for (std::size_t index = 0; index + 1 < stream.size(); ++index) {
        const auto first = static_cast<unsigned char>(stream[index]);
        const auto second = static_cast<unsigned char>(stream[index + 1]);
        if (start == std::string::npos && first == 0xFF && second == 0xD8) {
            start = index;
            ++index;
        } else if (start != std::string::npos && first == 0xFF && second == 0xD9) {
            const std::size_t end = index + 2;
            frames.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(start),
                                stream.begin() + static_cast<std::ptrdiff_t>(end));
            start = std::string::npos;
            ++index;
        }
    }
    return frames;
}

[[nodiscard]] ThumbnailResult generateThumbnail(
    const std::filesystem::path& clip, const int frameIndex,
    double duration, double framesPerSecond) {
    ThumbnailResult result{
        .clip = clip,
        .frameIndex = frameIndex,
        .firstFrameIndex = frameIndex,
        .duration = duration,
        .framesPerSecond = framesPerSecond,
    };
    try {
        if (!std::isfinite(duration) || duration <= 0.0) {
            const std::string durationOutput = runHiddenProcessCapture({
                L"ffprobe.exe", L"-v", L"error", L"-show_entries", L"format=duration",
                L"-of", L"default=noprint_wrappers=1:nokey=1",
                quoteProcessArgument(clip.wstring()),
            });
            duration = std::stod(durationOutput);
            result.duration = duration;
        }
        if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0) {
            const std::string rateOutput = runHiddenProcessCapture({
                L"ffprobe.exe", L"-v", L"error", L"-select_streams", L"v:0",
                L"-show_entries", L"stream=avg_frame_rate",
                L"-of", L"default=noprint_wrappers=1:nokey=1",
                quoteProcessArgument(clip.wstring()),
            });
            framesPerSecond = parseFrameRate(rateOutput);
            result.framesPerSecond = framesPerSecond;
        }
        if (!std::isfinite(duration) || duration <= 0.0 ||
            !std::isfinite(framesPerSecond) || framesPerSecond <= 0.0) {
            return result;
        }
        const std::wstring scaleFilter = quoteProcessArgument(
            L"scale=352:198:force_original_aspect_ratio=decrease:force_divisible_by=2,"
            L"pad=352:198:(ow-iw)/2:(oh-ih)/2");
        constexpr int previewWindowSize = 9;
        constexpr int framesBeforeCursor = previewWindowSize / 2;
        const int totalFrames = std::max(
            1, static_cast<int>(std::ceil(duration * framesPerSecond)));
        result.firstFrameIndex = std::clamp(
            frameIndex - framesBeforeCursor, 0, totalFrames - 1);
        const int frameCount = std::min(
            previewWindowSize, totalFrames - result.firstFrameIndex);
        // Seek just before the frame boundary, not halfway through the frame:
        // accurate seeking discards frames before -ss, causing a one-frame offset.
        const double timestamp = std::max(0.0, result.firstFrameIndex / framesPerSecond - 0.000001);
        wchar_t seekTimestamp[48]{};
        swprintf_s(seekTimestamp, L"%.9f", timestamp);
        const std::vector<std::wstring> arguments{
            L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"quiet",
            L"-ss", seekTimestamp, L"-i", quoteProcessArgument(clip.wstring()),
            L"-frames:v", std::to_wstring(frameCount), L"-an", L"-vf", scaleFilter,
            L"-q:v", L"3", L"-f", L"image2pipe", L"-vcodec", L"mjpeg", L"pipe:1",
        };
        result.frames = splitMjpegFrames(runHiddenProcessCapture(arguments));
    } catch (...) {
    }
    return result;
}

void requestThumbnail(
    const HWND window, AppState& state,
    const std::filesystem::path& clip, const int requestedFrame) {
    const int frameIndex = std::max(0, requestedFrame);
    auto& preview = state.thumbnailMemory[clip];
    if (preview == nullptr) preview = std::make_shared<ClipPreview>();
    state.thumbnailDesiredFrame[clip] = frameIndex;
    if (preview->frames.contains(frameIndex) ||
        state.thumbnailFailures.contains({clip, frameIndex}) ||
        state.thumbnailClipActive.contains(clip)) {
        return;
    }
    state.thumbnailClipActive.insert(clip);
    state.thumbnailsPending.insert({clip, frameIndex});
    const double knownDuration = preview->duration;
    const double knownFrameRate = preview->framesPerSecond;
    state.thumbnailJobs.emplace_back(
        [window, clip, frameIndex, knownDuration, knownFrameRate] {
        thumbnailSlots.acquire();
        auto result = std::make_unique<ThumbnailResult>(
            generateThumbnail(clip, frameIndex, knownDuration, knownFrameRate));
        thumbnailSlots.release();
        if (PostMessageW(window, thumbnailReadyMessage, 0,
                         reinterpret_cast<LPARAM>(result.get()))) {
            result.release();
        }
    });
}

[[nodiscard]] bool validClipName(const std::wstring& name) {
    return !name.empty() && name.size() <= 100 &&
        name.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos &&
        name.back() != L'.' && name.back() != L' ';
}

[[nodiscard]] bool validAudioGroupName(const std::wstring& name) {
    return !name.empty() && name.size() <= 48 && validClipName(name);
}

[[nodiscard]] std::size_t selectedAudioRowCount(const AppState& state) {
    return static_cast<std::size_t>(std::ranges::count_if(
        state.audioRows, [](const AudioRow& row) { return row.groupSelected; }));
}

[[nodiscard]] std::wstring selectedExistingAudioGroup(const AppState& state) {
    std::wstring group;
    std::size_t selected{};
    for (const auto& row : state.audioRows) {
        if (!row.groupSelected) continue;
        ++selected;
        if (row.groupName.empty()) return {};
        if (group.empty()) group = row.groupName;
        else if (group != row.groupName) return {};
    }
    return selected >= 2 ? group : std::wstring{};
}

struct EditorResult final {
    bool success{};
    std::filesystem::path output;
    std::wstring message;
    unsigned int exportId{};
};

[[nodiscard]] EditorResult exportEditedClip(
    const std::filesystem::path& input,
    std::wstring outputName,
    const double start,
    const double end,
    const std::vector<EditorAudioTrack>& audioTracks,
    const bool mergeAudio,
    const bool cutSelection,
    const double fullDuration,
    const std::function<void(int)>& progress = {}) {
    const double selectedDuration = end - start;
    if (!validClipName(outputName) || selectedDuration < 0.05 ||
        (!cutSelection && selectedDuration < 0.1) ||
        (cutSelection && (fullDuration <= 0.0 || fullDuration - selectedDuration < 0.1))) {
        return {false, {}, L"Sprawdź nazwę oraz zakres przycięcia."};
    }
    if (outputName.ends_with(L".mp4")) outputName.resize(outputName.size() - 4);
    auto output = input.parent_path() / (outputName + L".mp4");
    for (int suffix = 2; std::filesystem::exists(output); ++suffix) {
        output = input.parent_path() /
            (outputName + L"-" + std::to_wstring(suffix) + L".mp4");
    }
    const auto temporary = input.parent_path() /
        (L".nexplay-edit-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()) + L".mp4");
    struct ExportTemporary {
        std::filesystem::path path;
        ~ExportTemporary() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{temporary};

    const auto keptSegments =
        nexplay::editing::TimelineEdit{start, end, fullDuration, cutSelection}.keptRanges();
    if (keptSegments.empty()) return {false, {}, L"Nie można wyciąć całego klipu."};
    double outputDuration{};
    for (const auto& segment : keptSegments) outputDuration += segment.end - segment.start;

    std::vector<std::wstring> arguments{
        L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"error", L"-y",
        L"-nostdin", L"-nostats", L"-stats_period", L"0.1", L"-progress", L"pipe:1",
        L"-filter_complex_threads", L"2", L"-threads", L"2",
        L"-hwaccel", L"cuda", L"-hwaccel_output_format", L"cuda",
        L"-i", quoteProcessArgument(input.wstring()),
    };
    std::vector<const EditorAudioTrack*> includedTracks;
    for (const auto& track : audioTracks) {
        // Muting preserves a silent stream. Only deleting removes it from this list.
        includedTracks.push_back(&track);
    }

    std::wstring filters;
    const auto appendFilter = [&filters](std::wstring filter) {
        if (!filters.empty()) filters.push_back(L';');
        filters += std::move(filter);
    };
    for (std::size_t segmentIndex = 0; segmentIndex < keptSegments.size(); ++segmentIndex) {
        const auto& segment = keptSegments[segmentIndex];
        appendFilter(
            L"[0:v:0]trim=start=" + secondsArgument(segment.start) +
            L":end=" + secondsArgument(segment.end) +
            L",setpts=PTS-STARTPTS[vseg" + std::to_wstring(segmentIndex) + L"]");
    }
    std::wstring videoOutput;
    if (keptSegments.size() == 1) {
        videoOutput = L"[vseg0]";
    } else {
        std::wstring concat;
        for (std::size_t index = 0; index < keptSegments.size(); ++index) {
            concat += L"[vseg" + std::to_wstring(index) + L"]";
        }
        concat += L"concat=n=" + std::to_wstring(keptSegments.size()) +
            L":v=1:a=0[vout]";
        appendFilter(std::move(concat));
        videoOutput = L"[vout]";
    }

    for (std::size_t trackIndex = 0; trackIndex < includedTracks.size(); ++trackIndex) {
        const auto& track = *includedTracks[trackIndex];
        for (std::size_t segmentIndex = 0;
             segmentIndex < keptSegments.size(); ++segmentIndex) {
            const auto& segment = keptSegments[segmentIndex];
            const double duration = segment.end - segment.start;
            const double audibleStart = std::clamp(
                track.start - segment.start, 0.0, duration);
            const double audibleEnd = std::clamp(
                track.end - segment.start, 0.0, duration);
            appendFilter(
                L"[0:" + std::to_wstring(track.streamIndex) +
                L"]atrim=start=" + secondsArgument(segment.start) +
                L":end=" + secondsArgument(segment.end) +
                L",asetpts=PTS-STARTPTS," +
                (track.included ? L"volume=0:enable='lt(t," +
                    secondsArgument(audibleStart) + L")+gte(t," +
                    secondsArgument(audibleEnd) + L")'" : L"volume=0") +
                L",apad=whole_dur=" +
                secondsArgument(duration) + L",atrim=duration=" + secondsArgument(duration) +
                L"[a" + std::to_wstring(trackIndex) +
                L"seg" + std::to_wstring(segmentIndex) + L"]");
        }
        if (keptSegments.size() == 1) {
            appendFilter(
                L"[a" + std::to_wstring(trackIndex) + L"seg0]anull[a" +
                std::to_wstring(trackIndex) + L"]");
        } else {
            std::wstring concat;
            for (std::size_t index = 0; index < keptSegments.size(); ++index) {
                concat += L"[a" + std::to_wstring(trackIndex) + L"seg" +
                    std::to_wstring(index) + L"]";
            }
            concat += L"concat=n=" + std::to_wstring(keptSegments.size()) +
                L":v=0:a=1[a" + std::to_wstring(trackIndex) + L"]";
            appendFilter(std::move(concat));
        }
    }
    if (mergeAudio && includedTracks.size() > 1) {
        std::wstring mix;
        for (std::size_t index = 0; index < includedTracks.size(); ++index) {
            mix += L"[a" + std::to_wstring(index) + L"]";
        }
        mix += L"amix=inputs=" + std::to_wstring(includedTracks.size()) +
            L":duration=longest:dropout_transition=0:normalize=1[amix]";
        appendFilter(std::move(mix));
    }

    arguments.insert(arguments.end(), {L"-filter_complex", filters, L"-map", videoOutput});
    if (mergeAudio && includedTracks.size() > 1) {
        arguments.insert(arguments.end(), {L"-map", L"[amix]"});
    } else {
        for (std::size_t index = 0; index < includedTracks.size(); ++index) {
            arguments.insert(arguments.end(), {
                L"-map", L"[a" + std::to_wstring(index) + L"]",
            });
        }
    }
    if (includedTracks.empty()) arguments.push_back(L"-an");

    arguments.insert(arguments.end(), {
        L"-map_metadata", L"0", L"-c:v", L"h264_nvenc", L"-preset", L"p5",
        L"-cq", L"18",
    });
    if (!includedTracks.empty()) {
        arguments.insert(arguments.end(), {L"-c:a", L"aac", L"-b:a", L"192k"});
        if (mergeAudio && includedTracks.size() > 1) {
            arguments.insert(arguments.end(), {
                L"-metadata:s:a:0", quoteProcessArgument(L"handler_name=NexPlay Mix"),
            });
        } else {
            for (std::size_t index = 0; index < includedTracks.size(); ++index) {
                arguments.push_back(L"-metadata:s:a:" + std::to_wstring(index));
                arguments.push_back(quoteProcessArgument(
                    L"handler_name=" + includedTracks[index]->name));
            }
        }
    }
    arguments.insert(arguments.end(), {
        // Local playback can seek to the MP4 index at the end. Avoid rewriting
        // multi-GB media a second time just to move that index to the beginning.
        L"-t", secondsArgument(outputDuration),
        quoteProcessArgument(temporary.wstring()),
    });
    if (nexplay::app::runFfmpegProgress(arguments, outputDuration, progress) != 0) {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return {false, {}, L"Eksport GPU nie powiódł się. Sprawdź klip, sterownik NVIDIA i aktualne narzędzia FFmpeg z NVDEC/NVENC. Nie przełączono na CPU."};
    }
    if (!std::filesystem::is_regular_file(temporary) || std::filesystem::file_size(temporary) == 0)
        return {false, {}, L"Eksport nie utworzył poprawnego pliku."};
    std::error_code error;
    std::filesystem::rename(temporary, output, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return {false, {}, L"Nie można zapisać pliku wynikowego."};
    }
    if (progress) progress(100);
    return {true, output, L"Gotowe"};
}

void createTextFormat(
    IDWriteFactory* factory,
    const wchar_t* family,
    const float size,
    const DWRITE_FONT_WEIGHT weight,
    ComPtr<IDWriteTextFormat>& output) {
    if (FAILED(factory->CreateTextFormat(
            family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"pl-PL", &output))) {
        throw std::runtime_error("Nie mozna przygotowac czcionek interfejsu.");
    }
    output->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

void ensureGraphics(const HWND window, AppState& state, IWICBitmap* offscreen = nullptr) {
    if (state.renderTarget != nullptr)
        return;
    if (state.d2dFactory == nullptr && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                                                IID_PPV_ARGS(&state.d2dFactory)))) {
        throw std::runtime_error("Nie mozna uruchomic renderowania interfejsu.");
    }
    if (state.writeFactory == nullptr &&
        FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown **>(state.writeFactory.GetAddressOf())))) {
        throw std::runtime_error("Nie mozna uruchomic tekstu interfejsu.");
    }
    if (state.wicFactory == nullptr &&
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&state.wicFactory)))) {
        throw std::runtime_error("Nie mozna uruchomic obslugi miniatur.");
    }
    RECT client{};
    GetClientRect(window, &client);
    if (offscreen != nullptr) {
        if (FAILED(state.d2dFactory->CreateWicBitmapRenderTarget(
                offscreen, D2D1::RenderTargetProperties(), &state.renderTarget))) {
            throw std::runtime_error("Nie mozna utworzyc podgladu interfejsu.");
        }
    } else {
        if (FAILED(state.d2dFactory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN), 96.0F, 96.0F),
                D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(client.right, client.bottom)),
                &state.windowRenderTarget))) {
            throw std::runtime_error("Nie mozna utworzyc powierzchni interfejsu.");
        }
        state.renderTarget = state.windowRenderTarget;
    }
    state.renderTarget->CreateSolidColorBrush(white, &state.brush);
    createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Display", 32,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, state.titleFormat);
    createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Text", 18,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, state.headingFormat);
    createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Text", 14,
                     DWRITE_FONT_WEIGHT_NORMAL, state.bodyFormat);
    createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Text", 12,
                     DWRITE_FONT_WEIGHT_NORMAL, state.smallFormat);
    createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Text", 13,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, state.buttonFormat);
    createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Display", 20,
                     DWRITE_FONT_WEIGHT_BOLD, state.brandFormat);
}

[[nodiscard]] ID2D1Bitmap *thumbnailBitmap(AppState &state, const std::wstring &key,
                                           const std::vector<std::uint8_t> &encodedImage) {
    if (encodedImage.empty() || encodedImage.size() > MAXDWORD)
        return nullptr;
    if (const auto existing = state.thumbnailBitmaps.find(key);
        existing != state.thumbnailBitmaps.end()) {
        return existing->second.Get();
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(state.wicFactory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(encodedImage.data()),
            static_cast<DWORD>(encodedImage.size()))) ||
        FAILED(state.wicFactory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(state.wicFactory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut)) ||
        FAILED(state.renderTarget->CreateBitmapFromWicBitmap(
            converter.Get(), nullptr, &bitmap))) {
        return nullptr;
    }
    auto* result = bitmap.Get();
    state.thumbnailBitmaps.emplace(key, std::move(bitmap));
    return result;
}

void fillRounded(AppState& state, const Rect rectangle, const float radius,
                 const D2D1_COLOR_F color) {
    state.brush->SetColor(color);
    state.renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(rectangle.d2d(), radius, radius), state.brush.Get());
}

void strokeRounded(AppState& state, const Rect rectangle, const float radius,
                   const D2D1_COLOR_F color, const float width = 1.0F) {
    state.brush->SetColor(color);
    state.renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(rectangle.d2d(), radius, radius), state.brush.Get(), width);
}

[[nodiscard]] Rect expanded(const Rect rectangle, const float amount) {
    return {
        rectangle.left - amount,
        rectangle.top - amount,
        rectangle.right + amount,
        rectangle.bottom + amount,
    };
}

void drawGlow(AppState& state, const Rect rectangle, const float radius,
              const D2D1_COLOR_F source, const float intensity) {
    if (intensity <= 0.001F) return;
    for (int layer = 5; layer >= 1; --layer) {
        D2D1_COLOR_F glow = source;
        glow.a = intensity * (0.018F + static_cast<float>(6 - layer) * 0.011F);
        strokeRounded(state, expanded(rectangle, static_cast<float>(layer) * 2.2F),
                      radius + static_cast<float>(layer) * 2.2F, glow,
                      static_cast<float>(layer) * 2.0F);
    }
}


[[nodiscard]] float hoverValue(const AppState& state, const HitTarget target) {
    return state.hoverAnimation[static_cast<std::size_t>(target)];
}

void drawText(AppState& state, const std::wstring& text, const Rect rectangle,
              IDWriteTextFormat* format, const D2D1_COLOR_F color) {
    state.brush->SetColor(color);
    state.renderTarget->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()), format, rectangle.d2d(),
        state.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void drawCenteredText(AppState& state, const std::wstring& text, const Rect rectangle,
                      IDWriteTextFormat* format, const D2D1_COLOR_F color) {
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    drawText(state, text, rectangle, format, color);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

[[nodiscard]] float measuredTextWidth(
    AppState& state,
    const std::wstring& text,
    IDWriteTextFormat* format) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(state.writeFactory->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format,
            300.0F, 60.0F, &layout))) {
        return static_cast<float>(text.size()) * 11.0F;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return static_cast<float>(text.size()) * 11.0F;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

void drawNumericValue(
    AppState& state,
    const std::wstring& value,
    const Rect textArea,
    const HitTarget fieldTarget) {
    const bool active = state.activeField == fieldTarget;
    const std::wstring visibleValue = value.empty() ? L"0" : value;
    const float textWidth = measuredTextWidth(state, visibleValue, state.headingFormat.Get());
    const float centerX = (textArea.left + textArea.right) * 0.5F;

    if (active && state.replaceFieldOnInput) {
        const Rect selection{
            centerX - textWidth * 0.5F - 7,
            textArea.top + 6,
            centerX + textWidth * 0.5F + 7,
            textArea.bottom - 5,
        };
        D2D1_COLOR_F selectionColor = primary;
        selectionColor.a = 0.42F;
        fillRounded(state, selection, 6, selectionColor);
        drawGlow(state, selection, 6, primary, 0.20F);
    }

    drawCenteredText(
        state,
        visibleValue,
        textArea,
        state.headingFormat.Get(),
        value.empty() ? muted : white);

    if (active && !state.replaceFieldOnInput) {
        const float phase = static_cast<float>(GetTickCount64() % 1'000) / 1'000.0F;
        if (phase < 0.58F) {
            D2D1_COLOR_F caretColor = accentSecondary;
            caretColor.a = 0.75F + phase * 0.35F;
            state.brush->SetColor(caretColor);
            const float caretX = centerX + textWidth * 0.5F + 4;
            state.renderTarget->DrawLine(
                D2D1::Point2F(caretX, textArea.top + 9),
                D2D1::Point2F(caretX, textArea.bottom - 8),
                state.brush.Get(), 1.5F);
        }
    }
}

void drawToggle(AppState& state, const Rect rectangle, const float position) {
    D2D1_COLOR_F track = border;
    track.r += (primary.r - track.r) * position;
    track.g += (primary.g - track.g) * position;
    track.b += (primary.b - track.b) * position;
    fillRounded(state, rectangle, 11, track);
    const float centerX = rectangle.left + 11 +
        (rectangle.right - rectangle.left - 22) * position;
    state.brush->SetColor(white);
    state.renderTarget->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(centerX, (rectangle.top + rectangle.bottom) / 2), 7, 7),
        state.brush.Get());
}

void drawCheckbox(AppState &state, const Rect r, const bool checked) {
    fillRounded(state, r, 4, checked ? primary : background);
    strokeRounded(state, r, 4, checked ? primary : muted);
    if (!checked)
        return;
    state.brush->SetColor(white);
    state.renderTarget->DrawLine({r.left + 5, r.top + 10}, {r.left + 9, r.top + 14},
                                 state.brush.Get(), 1.6F);
    state.renderTarget->DrawLine({r.left + 9, r.top + 14}, {r.left + 15, r.top + 6},
                                 state.brush.Get(), 1.6F);
}

void drawButton(AppState &state, const Rect rectangle, const std::wstring &label,
                const HitTarget target, const bool emphasized, const bool enabled = true) {
    const float hover = hoverValue(state, target);
    D2D1_COLOR_F color = emphasized ? primary : field;
    if (enabled) {
        const D2D1_COLOR_F destination = emphasized ? primaryHover : border;
        color.r += (destination.r - color.r) * hover;
        color.g += (destination.g - color.g) * hover;
        color.b += (destination.b - color.b) * hover;
    }
    if (!enabled)
        color.a = 0.42F;
    fillRounded(state, rectangle, 6, color);
    if (!emphasized)
        strokeRounded(state, rectangle, 6, border);
    D2D1_COLOR_F textColor = white;
    if (emphasized && enabled &&
        primary.r * 0.2126F + primary.g * 0.7152F + primary.b * 0.0722F > 0.62F) {
        textColor = background;
    }
    if (!enabled)
        textColor.a = 0.42F;
    drawCenteredText(state, label, rectangle, state.buttonFormat.Get(), textColor);
}

enum class Icon {
    record,
    clips,
    settings,
    folder,
    play,
    pause,
    back,
    monitor,
    expand,
    edit,
    copy,
    trash
};
void drawIcon(AppState &state, Icon icon, float x, float y, D2D1_COLOR_F color, float size = 20) {
    state.brush->SetColor(color);
    const float u = size / 20;
    const auto line = [&](float a, float b, float c, float d) {
        state.renderTarget->DrawLine({x + a * u, y + b * u}, {x + c * u, y + d * u},
                                     state.brush.Get(), 1.5F);
    };
    const auto box = [&](float a, float b, float c, float d, float r = 2) {
        state.renderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x + a * u, y + b * u, x + c * u, y + d * u), r, r),
            state.brush.Get(), 1.4F);
    };
    switch (icon) {
    case Icon::record:
        state.renderTarget->DrawEllipse(D2D1::Ellipse({x + 10 * u, y + 10 * u}, 8 * u, 8 * u),
                                        state.brush.Get(), 1.5F);
        state.renderTarget->FillEllipse(D2D1::Ellipse({x + 10 * u, y + 10 * u}, 3 * u, 3 * u),
                                        state.brush.Get());
        break;
    case Icon::clips:
        box(2, 2, 8, 8, 1);
        box(12, 2, 18, 8, 1);
        box(2, 12, 8, 18, 1);
        box(12, 12, 18, 18, 1);
        break;
    case Icon::settings:
        line(3, 5, 17, 5);
        line(3, 15, 17, 15);
        box(6, 2, 10, 8, 1);
        box(11, 12, 15, 18, 1);
        break;
    case Icon::folder:
        box(2, 6, 18, 17);
        line(2, 6, 2, 3);
        line(2, 3, 8, 3);
        line(8, 3, 11, 6);
        break;
    case Icon::play:
        line(6, 3, 16, 10);
        line(16, 10, 6, 17);
        line(6, 17, 6, 3);
        break;
    case Icon::pause:
        line(6, 4, 6, 16);
        line(14, 4, 14, 16);
        break;
    case Icon::back:
        line(15, 10, 4, 10);
        line(4, 10, 9, 5);
        line(4, 10, 9, 15);
        break;
    case Icon::monitor:
        box(2, 3, 18, 14);
        line(10, 14, 10, 18);
        line(6, 18, 14, 18);
        break;
    case Icon::expand:
        line(2, 7, 2, 2);
        line(2, 2, 7, 2);
        line(13, 2, 18, 2);
        line(18, 2, 18, 7);
        line(18, 13, 18, 18);
        line(18, 18, 13, 18);
        line(7, 18, 2, 18);
        line(2, 18, 2, 13);
        break;
    case Icon::edit:
        line(3, 13, 13, 3);
        line(13, 3, 17, 7);
        line(17, 7, 7, 17);
        line(7, 17, 3, 17);
        line(3, 17, 3, 13);
        line(11, 5, 15, 9);
        break;
    case Icon::copy:
        box(6, 6, 17, 18, 1);
        line(3, 14, 3, 2);
        line(3, 2, 14, 2);
        break;
    case Icon::trash:
        line(3, 5, 17, 5);
        line(7, 5, 7, 2);
        line(7, 2, 13, 2);
        line(13, 2, 13, 5);
        line(5, 7, 6, 18);
        line(6, 18, 14, 18);
        line(14, 18, 15, 7);
        line(8, 8, 8, 15);
        line(12, 8, 12, 15);
        break;
    }
}
void rule(AppState &state, float x, float y, float right) {
    state.brush->SetColor(border);
    state.renderTarget->DrawLine({x, y}, {right, y}, state.brush.Get(), 1);
}
void panel(AppState &state, Rect rect) {
    fillRounded(state, rect, 10, card);
    strokeRounded(state, rect, 10, border);
}
void pageHeader(AppState &state, const std::wstring &title, const std::wstring &detail) {
    drawText(state, title, {contentLeft, 80, contentRight - 190, 121}, state.titleFormat.Get(),
             white);
    drawText(state, detail, {contentLeft, 127, contentRight - 160, 149}, state.bodyFormat.Get(),
             muted);
}
void drawSidebar(AppState &state) {
    fillRounded(state, {0, 44, 208, windowHeight}, 0, sidebar);
    state.brush->SetColor(border);
    state.renderTarget->DrawLine({208, 44}, {208, windowHeight}, state.brush.Get());
    drawText(state, L"PRZESTRZEŃ ROBOCZA", {24, 87, 200, 107}, state.smallFormat.Get(), muted);
    const auto nav = [&](Rect r, Page page, HitTarget target, Icon icon, const wchar_t *label) {
        const bool active =
            state.page == page || (page == Page::clips && state.page == Page::editor);
        const float hover = hoverValue(state, target);
        if (active || hover > 0.01F) {
            auto fill = active ? field : card;
            fill.a = active ? 1 : hover;
            fillRounded(state, r, 8, fill);
        }
        drawIcon(state, icon, r.left + 16, r.top + 13, active ? primaryHover : muted);
        drawText(state, label, {r.left + 50, r.top + 14, r.right - 10, r.bottom},
                 state.bodyFormat.Get(), active ? white : muted);
        if (active)
            fillRounded(state, {r.right - 8, r.top + 16, r.right - 5, r.bottom - 16}, 1.5F,
                        primary);
    };
    nav(replayNavRect, Page::replay, HitTarget::replayPage, Icon::record, L"Nagrywanie");
    nav(clipsNavRect, Page::clips, HitTarget::clipsPage, Icon::clips, L"Biblioteka");
    nav(settingsNavRect, Page::settings, HitTarget::settingsPage, Icon::settings, L"Ustawienia");
    rule(state, 20, windowHeight - 178, 188);
    drawText(state, L"SZYBKI DOSTĘP", {24, windowHeight - 158, 194, windowHeight - 138},
             state.smallFormat.Get(), muted);
    const auto shortcut = [&](float y, const std::wstring &key, const wchar_t *label) {
        drawText(state, label, {24, y + 7, 96, y + 30}, state.smallFormat.Get(), muted);
        fillRounded(state, {102, y, 188, y + 30}, 5, field);
        drawCenteredText(state, key, {102, y, 188, y + 30}, state.smallFormat.Get(), white);
    };
    shortcut(windowHeight - 123,
             state.saveHotkeyEnabled ? hotkeyLabel(state.saveHotkeyModifiers, state.saveHotkeyVk)
                                     : L"Wyłączony",
             L"Zapis klipu");
    shortcut(windowHeight - 81,
             state.stopHotkeyEnabled ? hotkeyLabel(state.stopHotkeyModifiers, state.stopHotkeyVk)
                                     : L"Wyłączony",
             L"Zatrzymaj");
}

void drawTitlebar(AppState &state) {
    fillRounded(state, {0, 0, windowWidth, 44}, 0, sidebar);
    rule(state, 0, 44, windowWidth);
    fillRounded(state, {17, 12, 37, 32}, 5, primary);
    drawCenteredText(state, L"N", {17, 11, 37, 32}, state.buttonFormat.Get(), white);
    drawText(state, L"NexPlay", {48, 13, 152, 36}, state.buttonFormat.Get(), white);
    drawText(state, L"/  STUDIO", {148, 14, 280, 35}, state.smallFormat.Get(), muted);
    for (auto target : {HitTarget::minimize, HitTarget::maximize, HitTarget::close}) {
        Rect r = target == HitTarget::minimize
                     ? minimizeRect
                     : (target == HitTarget::maximize ? maximizeRect : closeRect);
        float hover = hoverValue(state, target);
        auto color = target == HitTarget::close ? red : field;
        color.a = hover;
        if (hover > 0.01F)
            fillRounded(state, r, 0, color);
        const float x = (r.left + r.right) * 0.5F, y = 22;
        state.brush->SetColor(hover > 0.4F ? white : muted);
        auto line = [&](float a, float b, float c, float d) {
            state.renderTarget->DrawLine({a, b}, {c, d}, state.brush.Get(), 1);
        };
        if (target == HitTarget::minimize)
            line(x - 5, y, x + 5, y);
        else if (target == HitTarget::close) {
            line(x - 4, y - 4, x + 4, y + 4);
            line(x + 4, y - 4, x - 4, y + 4);
        } else {
            if (IsZoomed(state.mainWindow)) {
                line(x - 2, y - 6, x + 6, y - 6);
                line(x + 6, y - 6, x + 6, y + 2);
                state.renderTarget->DrawRectangle(D2D1::RectF(x - 5, y - 3, x + 3, y + 5),
                                                  state.brush.Get());
            } else
                state.renderTarget->DrawRectangle(D2D1::RectF(x - 5, y - 5, x + 5, y + 5),
                                                  state.brush.Get());
        }
    }
}

void drawStatusChip(AppState &state) {
    const bool running = state.engine.isRunning();
    const Rect r{contentRight - 154, 98, contentRight, 130};
    fillRounded(state, r, 16, card);
    strokeRounded(state, r, 16, border);
    state.brush->SetColor(running ? green : muted);
    state.renderTarget->FillEllipse(D2D1::Ellipse({r.left + 17, 114}, 3, 3), state.brush.Get());
    drawText(state, running ? L"Bufor aktywny" : L"Bufor wyłączony",
             {r.left + 29, 106, r.right - 8, 127}, state.smallFormat.Get(),
             running ? green : muted);
}

void drawAudioGroupSelector(AppState &state, const Rect rectangle, const bool selected,
                            const bool enabled) {
    D2D1_COLOR_F selectorColor = selected ? primary : field;
    if (!enabled)
        selectorColor.a = 0.36F;
    if (selected && enabled)
        drawGlow(state, rectangle, 7, primary, 0.34F);
    fillRounded(state, rectangle, 7, selectorColor);
    strokeRounded(state, rectangle, 7, selected ? primaryHover : border);

    D2D1_COLOR_F iconColor = selected ? white : muted;
    if (!enabled)
        iconColor.a = 0.34F;
    state.brush->SetColor(iconColor);
    state.renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(rectangle.left + 4, rectangle.top + 7, rectangle.left + 12,
                                      rectangle.bottom - 5),
                          4, 4),
        state.brush.Get(), 1.4F);
    state.renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(rectangle.right - 12, rectangle.top + 5, rectangle.right - 4,
                                      rectangle.bottom - 7),
                          4, 4),
        state.brush.Get(), 1.4F);
    state.renderTarget->DrawLine(D2D1::Point2F(rectangle.left + 10, rectangle.bottom - 7),
                                 D2D1::Point2F(rectangle.right - 10, rectangle.top + 7),
                                 state.brush.Get(), 1.4F);
}

void drawAudioGroupDialog(AppState &state) {
    if (!state.audioGroupDialogOpen)
        return;
    fillRounded(state, {0, 44, windowWidth, windowHeight}, 0, {0, 0, 0, 0.65F});
    panel(state, audioGroupDialogRect);
    const float x = audioGroupDialogRect.left + 32, y = audioGroupDialogRect.top;
    drawText(state, L"Nowa grupa audio", {x, y + 24, x + 420, y + 56}, state.headingFormat.Get(),
             white);
    drawText(state, L"Wybrane aplikacje zapiszą się jako jedna nazwana ścieżka.",
             {x, y + 62, x + 448, y + 86}, state.smallFormat.Get(), muted);
    fillRounded(state, audioGroupNameRect, 6, field);
    strokeRounded(state, audioGroupNameRect, 6, primary);
    drawText(
        state, state.audioGroupName.empty() ? L"Nazwa grupy" : state.audioGroupName,
        {audioGroupNameRect.left + 12, audioGroupNameRect.top + 13,
         audioGroupNameRect.right - 12, audioGroupNameRect.bottom},
        state.bodyFormat.Get(), white);
    drawButton(state, cancelAudioGroupRect, L"Anuluj", HitTarget::cancelAudioGroup, false);
    drawButton(state, confirmAudioGroupRect, L"Utwórz grupę", HitTarget::confirmAudioGroup, true,
               validAudioGroupName(state.audioGroupName));
}

void drawReplayPage(AppState &state) {
    const bool running = state.engine.isRunning();
    pageHeader(state, L"Nagrywanie", L"Twoje najlepsze momenty, zawsze pod ręką.");
    drawStatusChip(state);
    panel(state, replayHeroRect);
    panel(state, qualityPanelRect);
    panel(state, audioPanelRect);
    const float x = contentLeft + 24;
    drawIcon(state, Icon::monitor, x, 190, muted);
    drawText(state, L"MONITOR GŁÓWNY", {x + 30, 193, replayHeroRect.right - 24, 217},
             state.smallFormat.Get(), muted);
    drawText(state, running ? L"Jesteś w grze." : L"Gotowy na kolejny moment.",
             {x, 231, replayHeroRect.right - 20, 275}, state.headingFormat.Get(), white);
    drawText(state,
             L"Ostatnie " + state.durationText +
                 (state.saveHotkeyEnabled ? L" s zapiszesz jednym skrótem."
                                          : L" s zapiszesz przyciskiem poniżej."),
             {x, 270, replayHeroRect.right - 20, 297}, state.bodyFormat.Get(), muted);
    drawButton(state, startRect, running ? L"Zatrzymaj bufor" : L"Uruchom bufor",
               HitTarget::startStop, true);
    drawButton(state, saveRect, L"Zapisz klip", HitTarget::save, false, running);
    // Capture settings form: fixed control height, flexible panel position.
    const float q = qualityPanelRect.left + 20;
    drawText(state, L"Jakość nagrania", {q, 188, contentRight - 20, 218}, state.headingFormat.Get(),
             white);
    drawText(state, L"NVENC  /  H.264  /  do 8K", {q, 223, contentRight - 20, 246},
             state.smallFormat.Get(), muted);
    const auto value = [&](Rect r, const wchar_t *label, const std::wstring &text, HitTarget target,
                           const wchar_t *unit) {
        drawText(state, label, {r.left, r.top - 20, r.right, r.top}, state.smallFormat.Get(),
                 muted);
        fillRounded(state, r, 6, field);
        strokeRounded(state, r, 6, state.activeField == target ? primary : border);
        drawNumericValue(state, text, {r.left + 10, r.top + 3, r.right - 38, r.bottom - 3}, target);
        drawText(state, unit, {r.right - 36, r.top + 16, r.right - 5, r.bottom},
                 state.smallFormat.Get(), muted);
    };
    value(durationFieldRect, L"Bufor", state.durationText, HitTarget::durationField, L"s");
    value(fpsFieldRect, L"Klatki / sekundę", state.fpsText, HitTarget::fpsField, L"fps");
    value(bitrateFieldRect, L"Bitrate", state.bitrateText, HitTarget::bitrateField, L"Mb/s");
    drawText(state, L"Rozdzielczość",
             {resolutionFieldRect.left, 326, resolutionFieldRect.right, 346},
             state.smallFormat.Get(), muted);
    drawButton(state, resolutionFieldRect, resolutionPresets[state.resolutionPreset].label,
               HitTarget::resolutionField, false, !running);
    drawText(state, L"Źródła audio", {x, 453, contentRight - 380, 485}, state.headingFormat.Get(),
             white);
    drawText(state, L"Każda aplikacja jako osobna ścieżka. Wybierz ogniwa, aby utworzyć grupę.",
             {x, 495, contentRight - 20, 520}, state.smallFormat.Get(), muted);
    const auto selected = selectedAudioRowCount(state);
    drawButton(state, createAudioGroupRect,
               selectedExistingAudioGroup(state).empty()
                   ? L"Połącz (" + std::to_wstring(selected) + L")"
                   : L"Rozłącz",
               HitTarget::createAudioGroup, false, !running && selected >= 2);
    drawText(state, L"Mikrofon",
             {microphoneRect.left, microphoneRect.top + 10, microphoneRect.right - 50,
              microphoneRect.bottom},
             state.smallFormat.Get(), white);
    drawToggle(state,
               {microphoneRect.right - 40, microphoneRect.top + 7, microphoneRect.right,
                microphoneRect.top + 29},
               state.microphoneAnimation);
    drawButton(state, refreshRect, L"Odśwież", HitTarget::refreshAudio, false, !running);
    state.audioScroll =
        std::clamp(state.audioScroll, 0,
                   std::max(0, static_cast<int>(state.audioRows.size()) - audioVisibleRows));
    for (int v = 0; v < audioVisibleRows; ++v) {
        const int i = state.audioScroll + v;
        if (i >= static_cast<int>(state.audioRows.size()))
            break;
        const auto &row = state.audioRows[static_cast<std::size_t>(i)];
        const float top = audioRowsRect.top + v * audioRowHeight;
        const Rect r{audioRowsRect.left, top, audioRowsRect.right, top + audioRowHeight};
        if (r.contains(state.mouseX, state.mouseY))
            fillRounded(state, r, 5, field);
        drawCheckbox(state, {r.left + 8, top + 10, r.left + 28, top + 30}, row.included);
        drawText(state, row.name, {r.left + 44, top + 12, r.right - 300, top + 36},
                 state.bodyFormat.Get(), row.included ? white : muted);
        drawText(state, row.groupName.empty() ? L"Osobna ścieżka" : L"Grupa: " + row.groupName,
                 {r.right - 286, top + 13, r.right - 64, top + 34}, state.smallFormat.Get(), muted);
        drawAudioGroupSelector(state, {r.right - 38, top + 9, r.right - 14, top + 33},
                               row.groupSelected, !running);
        rule(state, r.left + 44, top + audioRowHeight - 1, r.right - 10);
    }
    if (state.audioRows.empty())
        drawText(state, L"Uruchom dźwięk w aplikacji i odśwież źródła.",
                 {x, 552, contentRight - 24, 590}, state.bodyFormat.Get(), muted);
    if (static_cast<int>(state.audioRows.size()) > audioVisibleRows) {
        const float h = audioVisibleRows * audioRowHeight;
        const float thumb = h * audioVisibleRows / static_cast<float>(state.audioRows.size());
        const float y = audioRowsRect.top + (h - thumb) * state.audioScroll /
                                                (state.audioRows.size() - audioVisibleRows);
        fillRounded(state, {contentRight - 8, y, contentRight - 5, y + thumb}, 1.5F, muted);
    }
}

[[nodiscard]] std::wstring sizeLabel(const std::uintmax_t bytes) {
    wchar_t text[32]{};
    swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

void drawClipsPage(AppState &state) {
    pageHeader(state, L"Biblioteka",
               std::to_wstring(state.clips.size()) +
                   L" klipów  ·  Przesuń kursor po obrazie, aby podejrzeć klatki.");
    drawButton(state, openClipsRect, L"Otwórz folder", HitTarget::openClips, false);
    if (state.clips.empty()) {
        drawIcon(state, Icon::clips, (contentLeft + contentRight) * 0.5F - 20, windowHeight * 0.40F,
                 muted, 40);
        drawCenteredText(
            state, L"Tutaj zaczyna się Twoja kolekcja.",
            {contentLeft, windowHeight * 0.40F + 60, contentRight, windowHeight * 0.40F + 96},
            state.headingFormat.Get(), white);
        drawCenteredText(
            state,
            state.saveHotkeyEnabled
                ? L"Uruchom bufor, a potem naciśnij " +
                      hotkeyLabel(state.saveHotkeyModifiers, state.saveHotkeyVk) +
                      L", aby zapisać pierwszy klip."
                : L"Uruchom bufor i kliknij Zapisz klip w zakładce Nagrywanie.",
            {contentLeft, windowHeight * 0.40F + 106, contentRight, windowHeight * 0.40F + 136},
            state.bodyFormat.Get(), muted);
        return;
    }
    const int totalRows = (static_cast<int>(state.clips.size()) + clipColumns - 1) / clipColumns;
    state.clipScroll = std::clamp(state.clipScroll, 0, std::max(0, totalRows - clipVisibleRows));
    for (int v = 0; v < clipVisibleRows * clipColumns; ++v) {
        const int i = state.clipScroll * clipColumns + v;
        if (i >= static_cast<int>(state.clips.size()))
            break;
        const auto &clip = state.clips[static_cast<std::size_t>(i)];
        const Rect r = clipCardRect(v);
        const Rect thumb{r.left, r.top, r.right, r.top + clipWidth * 9 / 16};
        const bool hover = thumb.contains(state.mouseX, state.mouseY);
        const double fraction = hover ? std::clamp(static_cast<double>((state.mouseX - thumb.left) /
                                                                       (thumb.right - thumb.left)),
                                                   0.0, 0.999999)
                                      : 0;
        const bool known =
            clip.preview && clip.preview->duration > 0 && clip.preview->framesPerSecond > 0;
        const int frames =
            known ? std::max(1, static_cast<int>(std::ceil(clip.preview->duration *
                                                           clip.preview->framesPerSecond)))
                  : 1;
        const int requested = std::clamp(static_cast<int>(fraction * frames), 0, frames - 1);
        if (hover)
            requestThumbnail(state.mainWindow, state, clip.path, requested);
        ID2D1Bitmap *bitmap = nullptr;
        if (clip.preview && !clip.preview->frames.empty()) {
            const int selected = nexplay::playback::thumbnailFrame(
                clip.preview->frames, requested, hover ? clip.preview->lastScrubFrame : -1);
            const auto frame = clip.preview->frames.find(selected);
            bitmap = thumbnailBitmap(
                state, clip.path.wstring() + L"#" + std::to_wstring(frame->first), frame->second);
            if (hover && bitmap != nullptr) clip.preview->lastScrubFrame = selected;
        }
        panel(state, r);
        fillRounded(state, thumb, 8, {0.015F, 0.017F, 0.021F, 1});
        if (bitmap) {
            const auto pixels = bitmap->GetSize();
            const Rect fit = aspectFitRect(state.mainWindow, thumb,
                                           pixels.width / std::max(1.0F, pixels.height));
            state.renderTarget->DrawBitmap(bitmap, fit.d2d(), 1,
                                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else
            drawCenteredText(state,
                             state.thumbnailFailures.contains({clip.path, requested})
                                 ? L"Podgląd niedostępny"
                                 : L"Wczytywanie podglądu…",
                             thumb, state.smallFormat.Get(), muted);
        if (hover) {
            strokeRounded(state, thumb, 8, primary);
            fillRounded(state,
                        {thumb.left, thumb.bottom - 3,
                         thumb.left + static_cast<float>(fraction) * (thumb.right - thumb.left),
                         thumb.bottom},
                        0, primary);
        } else if (bitmap != nullptr) {
            const float cx = (thumb.left + thumb.right) * 0.5F,
                        cy = (thumb.top + thumb.bottom) * 0.5F;
            fillRounded(state, {cx - 20, cy - 20, cx + 20, cy + 20}, 20, {0, 0, 0, 0.55F});
            drawIcon(state, Icon::play, cx - 10, cy - 10, white);
        }
        const std::wstring stamp = hover && known
                                       ? preciseTimeLabel(requested / clip.preview->framesPerSecond)
                                   : known ? timeLabel(clip.preview->duration)
                                           : L"MP4";
        const Rect badge{thumb.right - 96, thumb.bottom - 34, thumb.right - 10, thumb.bottom - 10};
        fillRounded(state, badge, 4, {0, 0, 0, 0.78F});
        drawCenteredText(state, stamp, badge, state.smallFormat.Get(), white);
        drawText(state, clip.path.stem().wstring(),
                 {r.left + 14, thumb.bottom + 13, r.right - 14, thumb.bottom + 36},
                 state.bodyFormat.Get(), white);
        drawText(state, L"MP4  ·  " + sizeLabel(clip.size),
                 {r.left + 14, thumb.bottom + 43, r.right - 14, r.bottom - 6},
                 state.smallFormat.Get(), muted);
    }
    if (totalRows > clipVisibleRows) {
        const float h = clipsListRect.bottom - clipsListRect.top;
        const float thumb = h * clipVisibleRows / totalRows;
        const float y =
            clipsListRect.top + (h - thumb) * state.clipScroll / (totalRows - clipVisibleRows);
        fillRounded(state, {contentRight + 12, y, contentRight + 15, y + thumb}, 1.5F, muted);
    }
}

[[nodiscard]] Rect clipMenuItemRect(const AppState &state, const int item) {
    const auto &menu = state.clipContextMenuRect;
    const float top = item < 4 ? menu.top + 42.0F + item * 38.0F : menu.top + 210.0F;
    return {menu.left + 8, top, menu.right - 8, top + 36};
}

void drawClipContextMenu(AppState &state) {
    if (!state.clipContextMenuOpen || state.clipContextMenuClip.empty())
        return;
    const Rect menu = state.clipContextMenuRect;
    fillRounded(state, expanded(menu, 3), 11, D2D1_COLOR_F{0, 0, 0, 0.40F});
    fillRounded(state, menu, 8, card);
    strokeRounded(state, menu, 8, border);
    drawText(state, state.clipContextMenuClip.stem().wstring(),
             {menu.left + 16, menu.top + 12, menu.right - 16, menu.top + 35},
             state.smallFormat.Get(), muted);

    constexpr std::array labels{
        L"Otwórz w edytorze NexPlay", L"Odtwórz w domyślnej aplikacji", L"Pokaż w folderze",
        L"Kopiuj ścieżkę pliku",      L"Usuń klip do Kosza…",
    };
    constexpr std::array icons{Icon::edit, Icon::play, Icon::folder, Icon::copy, Icon::trash};
    for (int item = 0; item < static_cast<int>(labels.size()); ++item) {
        const Rect row = clipMenuItemRect(state, item);
        const bool hovered = row.contains(state.mouseX, state.mouseY);
        if (hovered) {
            fillRounded(state, row, 5,
                        item == 4 ? D2D1_COLOR_F{0.190F, 0.040F, 0.065F, 1.0F} : field);
        }
        drawIcon(state, icons[static_cast<std::size_t>(item)], row.left + 12, row.top + 9,
                 item == 4 ? red : muted, 18);
        drawText(state, labels[static_cast<std::size_t>(item)],
                 {row.left + 44, row.top + 9, row.right - 8, row.bottom}, state.bodyFormat.Get(),
                 item == 4 ? red : white);
    }
    state.brush->SetColor(border);
    state.renderTarget->DrawLine(D2D1::Point2F(menu.left + 14, menu.top + 202),
                                 D2D1::Point2F(menu.right - 14, menu.top + 202), state.brush.Get());
}

[[nodiscard]] std::wstring timeLabel(const double seconds) {
    const auto totalSeconds = static_cast<unsigned long>(std::max(0.0, seconds));
    const unsigned long minutes = totalSeconds / 60;
    const unsigned long remaining = totalSeconds % 60;
    wchar_t text[32]{};
    swprintf_s(text, L"%02lu:%02lu", minutes, remaining);
    return text;
}

[[nodiscard]] std::wstring preciseTimeLabel(const double seconds) {
    const auto milliseconds = static_cast<unsigned long long>(std::max(0.0, seconds) * 1'000.0);
    const unsigned long long minutes = milliseconds / 60'000;
    const unsigned long long remainingSeconds = (milliseconds / 1'000) % 60;
    const unsigned long long remainingMilliseconds = milliseconds % 1'000;
    wchar_t text[32]{};
    swprintf_s(text, L"%02llu:%02llu.%03llu", minutes, remainingSeconds, remainingMilliseconds);
    return text;
}

void drawEditorName(AppState &state) {
    fillRounded(state, editorNameRect, 10, field);
    strokeRounded(state, editorNameRect, 10,
                  state.activeField == HitTarget::editorName ? primary : border);
    if (state.activeField == HitTarget::editorName) {
        drawGlow(state, editorNameRect, 10, primary, 0.55F);
    }
    const std::wstring shown = state.editorName.empty() ? L"Nazwa klipu" : state.editorName;
    if (state.activeField == HitTarget::editorName && state.replaceFieldOnInput) {
        const float width =
            std::min(measuredTextWidth(state, shown, state.bodyFormat.Get()) + 14.0F,
                     editorNameRect.right - editorNameRect.left - 24.0F);
        D2D1_COLOR_F selectionColor = primary;
        selectionColor.a = 0.38F;
        fillRounded(state,
                    {editorNameRect.left + 12, editorNameRect.top + 9,
                     editorNameRect.left + 12 + width, editorNameRect.bottom - 9},
                    5, selectionColor);
    }
    drawText(state, shown,
             {editorNameRect.left + 14, editorNameRect.top + 12, editorNameRect.right - 14,
              editorNameRect.bottom - 8},
             state.bodyFormat.Get(), state.editorName.empty() ? muted : white);
}

[[nodiscard]] Rect editorAudioTimelineRect(const int visibleRow) {
    const float top = editorAudioRowsRect.top + visibleRow * editorAudioRowHeight;
    return {editorTimelineRect.left, top, editorTimelineRect.right, top + 36};
}
[[nodiscard]] Rect editorAudioDeleteRect(const int visibleRow) {
    const auto row = editorAudioTimelineRect(visibleRow);
    return {editorTimelineRect.left - 34, row.top + 4, editorTimelineRect.left - 6, row.top + 32};
}
[[nodiscard]] nexplay::editing::TimelineEdit editorTimelineEdit(const AppState& state) {
    return {state.trimStart, state.trimEnd, state.editorDuration, state.cutEditorSelection};
}
[[nodiscard]] nexplay::editing::TimeRange editorAudioBounds(
    const AppState& state, const EditorAudioTrack& track) {
    const auto ranges = editorTimelineEdit(state).audioRanges(track.start, track.end);
    if (!ranges.empty()) return {ranges.front().start, ranges.back().end};
    const double anchor = std::clamp(track.start, 0.0, std::max(0.0, state.editorDuration));
    return {anchor, anchor};
}
void drawEditorMergeToggle(AppState &state) {
    drawText(state, L"Jedna ścieżka audio",
             {editorMergeAudioRect.left, editorMergeAudioRect.top + 11,
              editorMergeAudioRect.right - 50, editorMergeAudioRect.bottom},
             state.smallFormat.Get(), white);
    drawToggle(state,
               {editorMergeAudioRect.right - 40, editorMergeAudioRect.top + 8,
                editorMergeAudioRect.right, editorMergeAudioRect.top + 30},
               state.mergeEditorAudio ? 1.0F : 0.0F);
}
[[nodiscard]] double timelineTickInterval(const double duration, const float width) {
    const double ideal = std::max(0.001, duration) / std::max(2.0F, std::floor(width / 100));
    const double magnitude = std::pow(10.0, std::floor(std::log10(ideal)));
    for (const double factor : {1.0, 2.0, 5.0, 10.0}) {
        if (magnitude * factor >= ideal)
            return magnitude * factor;
    }
    return magnitude * 10;
}
void drawUnifiedTimeline(AppState &state) {
    panel(state, timelinePanelRect);
    const float top = timelinePanelRect.top;
    drawText(state, L"Sekwencja", {contentLeft + 18, top + 16, contentLeft + 200, top + 40},
             state.buttonFormat.Get(), white);
    drawText(state, L"V1 + audio: wspólne cięcie  /  Kosz: usuń ścieżkę",
             {contentLeft + 208, top + 17, contentRight - 18, top + 40}, state.smallFormat.Get(),
             muted);
    rule(state, contentLeft, top + 48, contentRight);
    const double duration = std::max(0.001, state.editorDuration);
    const float width = editorTimelineRect.right - editorTimelineRect.left;
    const auto timeX = [&](double t) {
        return editorTimelineRect.left +
               static_cast<float>(std::clamp(t / duration, 0.0, 1.0)) * width;
    };
    const double interval = timelineTickInterval(duration, width);
    for (int i = 0; i * interval <= duration; ++i) {
        const double time = i * interval;
        const float x = timeX(time);
        const std::wstring label = interval < 1 ? preciseTimeLabel(time) : timeLabel(time);
        drawCenteredText(state, label, {x - 42, top + 54, x + 42, top + 74},
                         state.smallFormat.Get(), muted);
        state.brush->SetColor(border);
        state.renderTarget->DrawLine({x, top + 76}, {x, editorAudioRowsRect.bottom},
                                     state.brush.Get());
    }
    drawText(state, L"V1",
             {contentLeft + 20, editorTimelineRect.top + 10, contentLeft + 48,
              editorTimelineRect.bottom},
             state.smallFormat.Get(), primaryHover);
    drawText(state, L"Wideo",
             {contentLeft + 64, editorTimelineRect.top + 9, contentLeft + 190,
              editorTimelineRect.bottom},
             state.bodyFormat.Get(), white);
    fillRounded(state, editorTimelineRect, 4, field);
    const Rect range{timeX(state.trimStart), editorTimelineRect.top, timeX(state.trimEnd),
                     editorTimelineRect.bottom};
    auto fill = state.cutEditorSelection ? red : primary;
    fill.a = 0.30F;
    fillRounded(state, range, 4, fill);
    strokeRounded(state, range, 4, state.cutEditorSelection ? red : primary);
    const auto handle = [&](float x, Rect r) {
        fillRounded(state, {x - 2, r.top + 7, x + 2, r.bottom - 7}, 1, white);
    };
    handle(range.left, range);
    handle(range.right, range);
    state.editorAudioScroll = std::clamp(
        state.editorAudioScroll, 0,
        std::max(0, static_cast<int>(state.editorAudioTracks.size()) - editorVisibleTracks));
    for (int v = 0; v < editorVisibleTracks; ++v) {
        int i = state.editorAudioScroll + v;
        if (i >= static_cast<int>(state.editorAudioTracks.size()))
            break;
        const auto &track = state.editorAudioTracks[static_cast<std::size_t>(i)];
        const Rect r = editorAudioTimelineRect(v);
        drawCheckbox(state, {contentLeft + 18, r.top + 8, contentLeft + 38, r.top + 28},
                     track.included);
        drawText(state, L"A" + std::to_wstring(i + 1),
                 {contentLeft + 50, r.top + 10, contentLeft + 80, r.bottom},
                 state.smallFormat.Get(), muted);
        drawText(state, track.name,
                 {contentLeft + 86, r.top + 9, editorTimelineRect.left - 40, r.bottom},
                 state.smallFormat.Get(), track.included ? white : muted);
        const auto remove = editorAudioDeleteRect(v);
        drawIcon(state, Icon::trash, remove.left + 5, remove.top + 5, muted, 18);
        fillRounded(state, r, 4, field);
        const D2D1_COLOR_F audioColor{0.20F, 0.65F, 0.54F, track.included ? 0.22F : 0.06F};
        for (const auto audibleRange : editorTimelineEdit(state).audioRanges(track.start, track.end)) {
            const Rect audioRange{timeX(audibleRange.start), r.top, timeX(audibleRange.end), r.bottom};
            fillRounded(state, audioRange, 4, audioColor);
            if (track.included) strokeRounded(state, audioRange, 4, {0.24F, 0.58F, 0.50F, 1});
            if (audioRange.right - audioRange.left > 160)
                drawText(state, track.included ? track.name : L"Wyciszona",
                         {audioRange.left + 12, r.top + 10, audioRange.right - 12, r.bottom},
                         state.smallFormat.Get(), track.included ? white : muted);
        }
        if (state.cutEditorSelection)
            fillRounded(state, {range.left, r.top, range.right, r.bottom}, 4,
                        {red.r, red.g, red.b, 0.12F});
        const auto bounds = editorAudioBounds(state, track);
        if (track.included && bounds.duration() > 0) {
            handle(timeX(bounds.start), r);
            handle(timeX(bounds.end), r);
        }
    }
    if (state.editorAudioTracks.empty())
        drawText(state, L"Brak ścieżek audio",
                 {contentLeft + 20, editorAudioRowsRect.top, contentRight - 20,
                  editorAudioRowsRect.top + 28},
                 state.smallFormat.Get(), muted);
    if (static_cast<int>(state.editorAudioTracks.size()) > editorVisibleTracks) {
        const float h = editorVisibleTracks * editorAudioRowHeight,
                    thumb = h * editorVisibleTracks /
                            static_cast<float>(state.editorAudioTracks.size());
        const float y =
            editorAudioRowsRect.top + (h - thumb) * state.editorAudioScroll /
                                          (state.editorAudioTracks.size() - editorVisibleTracks);
        fillRounded(state, {contentRight - 8, y, contentRight - 5, y + thumb}, 1.5F, muted);
    }
    const float playX = timeX(state.playPosition);
    state.brush->SetColor(white);
    state.renderTarget->DrawLine({playX, top + 75}, {playX, editorAudioRowsRect.bottom},
                                 state.brush.Get());
    fillRounded(state, {playX - 4, top + 74, playX + 4, top + 81}, 2, white);
}
void drawEditorPage(AppState &state) {
    drawButton(state, editorBackRect, L"←  Biblioteka", HitTarget::editorBack, false);
    drawText(state, L"Montaż", {editorBackRect.right + 18, 80, contentRight - 200, 114},
             state.headingFormat.Get(), white);
    const auto exportLabel = state.editorExporting
        ? (state.editorExportPercent >= 99 ? std::wstring(L"Finalizuję · 99%")
            : L"Eksport GPU · " + std::to_wstring(state.editorExportPercent) + L"%")
        : std::wstring(L"Eksportuj · GPU");
    drawButton(state, editorSaveRect, exportLabel, HitTarget::editorSave, true,
               state.editorDuration > 0);
    if (state.editorExporting) {
        const Rect bar{editorSaveRect.left, editorSaveRect.bottom + 5,
                       editorSaveRect.right, editorSaveRect.bottom + 9};
        fillRounded(state, bar, 2, field);
        if (state.editorExportPercent > 0)
            fillRounded(state, {bar.left, bar.top,
                bar.left + (bar.right - bar.left) * state.editorExportPercent / 100.0F, bar.bottom},
                2, primaryHover);
    }
    panel(state, previewPanelRect);
    panel(state, inspectorRect);
    drawText(state, L"PODGLĄD", {previewPanelRect.left + 16, 144, previewPanelRect.right - 16, 164},
             state.smallFormat.Get(), muted);
    drawButton(state, editorPlayRect, L"", HitTarget::editorPlay, false);
    drawIcon(state, state.playing ? Icon::pause : Icon::play, editorPlayRect.left + 10,
             editorPlayRect.top + 8, white);
    drawButton(state, editorFullscreenRect, L"", HitTarget::editorFullscreen, false);
    drawIcon(state, Icon::expand, editorFullscreenRect.left + 10, editorFullscreenRect.top + 8,
             white);
    drawCenteredText(state,
                     preciseTimeLabel(state.playPosition) + L" / " +
                         preciseTimeLabel(state.editorDuration),
                     {editorPlayRect.right + 8, editorPlayRect.top, editorFullscreenRect.left - 8,
                      editorPlayRect.bottom},
                     state.smallFormat.Get(), muted);
    const float x = inspectorRect.left + 16;
    drawText(state, L"Eksport · NVENC", {x, 152, contentRight - 16, 178}, state.headingFormat.Get(), white);
    drawText(state, L"Nazwa nowego pliku", {x, 189, contentRight - 16, 213},
             state.smallFormat.Get(), muted);
    drawEditorName(state);
    drawEditorMergeToggle(state);
    drawButton(state, editorCutModeRect,
               state.cutEditorSelection ? L"Wycinanie fragmentu" : L"Przycinanie brzegów",
               HitTarget::editorCutMode, false, state.editorDuration > 0.2);
    if (inspectorRect.bottom > 418)
        drawText(state,
                 state.cutEditorSelection ? L"Czerwony zakres zostanie usunięty."
                                          : L"Uchwyty V1 przycinają też dźwięk.",
                 {x, 378, contentRight - 16, 414}, state.smallFormat.Get(), muted);
    drawUnifiedTimeline(state);
}

void drawSettingsPage(AppState &state) {
    pageHeader(state, L"Ustawienia", L"Dopasuj NexPlay do swojego sposobu pracy.");
    panel(state, startupPanelRect);
    panel(state, hotkeysPanelRect);
    panel(state, colorPanelRect);
    panel(state, notificationsPanelRect);
    drawText(state, L"Uruchamianie", {contentLeft + 20, 188, startupPanelRect.right - 20, 218},
             state.headingFormat.Get(), white);
    const auto startup = [&](Rect r, HitTarget target, const wchar_t *title, const wchar_t *sub,
                             float position) {
        if (hoverValue(state, target) > 0.1F)
            fillRounded(state, r, 6, field);
        drawText(state, title, {r.left, r.top + 6, r.right - 52, r.top + 30},
                 state.bodyFormat.Get(), white);
        drawText(state, sub, {r.left, r.top + 34, r.right - 52, r.bottom}, state.smallFormat.Get(),
                 muted);
        drawToggle(state, {r.right - 40, r.top + 15, r.right, r.top + 37}, position);
    };
    startup(autostartRect, HitTarget::autostartToggle, L"Uruchamiaj z Windows",
            L"Po zalogowaniu, w zasobniku systemowym.", state.autostartAnimation);
    rule(state, autostartRect.left, 310, autostartRect.right);
    startup(autoBufferRect, HitTarget::autoBufferToggle, L"Automatyczny bufor",
            L"Rozpocznij nagrywanie po otwarciu aplikacji.", state.autoBufferAnimation);
    drawText(state, L"Skróty klawiaturowe",
             {contentLeft + 20, 448, hotkeysPanelRect.right - 20, 478}, state.headingFormat.Get(),
             white);
    const auto hotkey = [&](Rect r, HitTarget target, HotkeyCapture capture, const wchar_t* title,
                            UINT key, UINT modifiers, bool enabled, Rect toggle) {
        const bool listening = state.hotkeyCapture == capture;
        fillRounded(state, r, 7, listening ? field : card);
        strokeRounded(state, r, 7, listening ? primary : border);
        drawText(state, title, {r.left + 14, r.top + 17, r.right - 210, r.bottom},
                 state.bodyFormat.Get(), enabled ? white : muted);
        const Rect cap{r.right - 198, r.top + 10, r.right - 64, r.bottom - 10};
        fillRounded(state, cap, 5, field);
        drawCenteredText(state, listening ? L"Naciśnij klawisz…" : hotkeyLabel(modifiers, key), cap,
                         state.smallFormat.Get(),
                         listening ? primaryHover
                         : enabled ? white
                                   : muted);
        drawToggle(state, toggle, enabled ? 1.0F : 0.0F);
        if (hoverValue(state, target) > 0.1F)
            strokeRounded(state, r, 7, muted);
    };
    hotkey(saveHotkeyRect, HitTarget::saveHotkey, HotkeyCapture::save, L"Zapisz klip",
           state.saveHotkeyVk, state.saveHotkeyModifiers, state.saveHotkeyEnabled,
           saveHotkeyToggleRect);
    hotkey(stopHotkeyRect, HitTarget::stopHotkey, HotkeyCapture::stop, L"Zatrzymaj bufor",
           state.stopHotkeyVk, state.stopHotkeyModifiers, state.stopHotkeyEnabled,
           stopHotkeyToggleRect);
    drawText(state, L"Kliknij, aby zmienić. Przełącznik wyłącza skrót. Esc anuluje.",
             {contentLeft + 20, 638, hotkeysPanelRect.right - 20, 670}, state.smallFormat.Get(),
             muted);
    drawText(state, L"Powiadomienia zapisu",
             {notificationsPanelRect.left + 24, notificationsPanelRect.top + 18, contentRight - 24,
              notificationsPanelRect.top + 44},
             state.headingFormat.Get(), white);
    drawText(state, L"Róg głównego ekranu · bez dźwięku",
             {notificationsPanelRect.left + 24, notificationsPanelRect.top + 48, contentRight - 24,
              notificationsPanelRect.top + 70},
             state.smallFormat.Get(), muted);
    for (int i = 0; i < 4; ++i)
        drawButton(state, toastCornerRects[i], toastCornerLabels[i],
                   static_cast<HitTarget>(static_cast<int>(HitTarget::toastTopLeft) + i),
                   static_cast<int>(state.toastCorner) == i);
    drawText(state, L"Wygląd", {colorPanelRect.left + 24, 188, contentRight - 24, 218},
             state.headingFormat.Get(), white);
    drawText(state, L"Kolor akcentu", {colorPanelRect.left + 24, 227, contentRight - 24, 251},
             state.smallFormat.Get(), muted);

    const Rect accentPlane = fixedAspectRect(state.mainWindow, accentPlaneRect);
    const Rect accentHue = fixedAspectRect(state.mainWindow, accentHueRect);
    const Rect accentPreview = fixedAspectRect(state.mainWindow, accentPreviewRect);
    const D2D1_COLOR_F hueColor = hsvColor(state.accentHue, 1.0F, 1.0F);
    fillRounded(state, accentPlane, 8, hueColor);
    const auto overlayGradient = [&](const D2D1_GRADIENT_STOP *stops, const UINT32 count,
                                     const D2D1_POINT_2F start, const D2D1_POINT_2F end,
                                     const Rect rectangle) {
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        if (SUCCEEDED(
                state.renderTarget->CreateGradientStopCollection(stops, count, &collection)) &&
            SUCCEEDED(state.renderTarget->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(start, end), collection.Get(), &gradient))) {
            const float radius = std::min(8.0F, (rectangle.right - rectangle.left) * 0.5F);
            state.renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(rectangle.d2d(), radius, radius), gradient.Get());
        }
    };
    const D2D1_GRADIENT_STOP saturationStops[]{
        {0.0F, D2D1_COLOR_F{1, 1, 1, 1}},
        {1.0F, D2D1_COLOR_F{1, 1, 1, 0}},
    };
    overlayGradient(saturationStops, 2, D2D1::Point2F(accentPlane.left, accentPlane.top),
                    D2D1::Point2F(accentPlane.right, accentPlane.top), accentPlane);
    const D2D1_GRADIENT_STOP valueStops[]{
        {0.0F, D2D1_COLOR_F{0, 0, 0, 0}},
        {1.0F, D2D1_COLOR_F{0, 0, 0, 1}},
    };
    overlayGradient(valueStops, 2, D2D1::Point2F(accentPlane.left, accentPlane.top),
                    D2D1::Point2F(accentPlane.left, accentPlane.bottom), accentPlane);
    strokeRounded(state, accentPlane, 8, border);

    constexpr D2D1_GRADIENT_STOP hueStops[]{
        {0.0F, {1, 0, 0, 1}}, {0.167F, {1, 1, 0, 1}}, {0.333F, {0, 1, 0, 1}},
        {0.5F, {0, 1, 1, 1}}, {0.667F, {0, 0, 1, 1}}, {0.833F, {1, 0, 1, 1}},
        {1.0F, {1, 0, 0, 1}},
    };
    overlayGradient(hueStops, static_cast<UINT32>(std::size(hueStops)),
                    D2D1::Point2F(accentHue.left, accentHue.top),
                    D2D1::Point2F(accentHue.left, accentHue.bottom), accentHue);
    strokeRounded(state, accentHue, 8, border);

    const float pickerX =
        accentPlane.left + state.accentSaturation * (accentPlane.right - accentPlane.left);
    const float pickerY =
        accentPlane.top + (1.0F - state.accentValue) * (accentPlane.bottom - accentPlane.top);
    const Rect pickerGlow =
        fixedAspectRect(state.mainWindow, {pickerX - 7, pickerY - 7, pickerX + 7, pickerY + 7});
    drawGlow(state, pickerGlow, 7, white, 0.45F);
    state.brush->SetColor(white);
    state.renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(pickerX, pickerY), 6, 6),
                                    state.brush.Get(), 2.0F);
    const float hueY = accentHue.top + state.accentHue * (accentHue.bottom - accentHue.top);
    const float hueHandleX = 3.0F;
    state.brush->SetColor(white);
    state.renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(accentHue.left - hueHandleX, hueY - 3,
                                      accentHue.right + hueHandleX, hueY + 3),
                          hueHandleX, 3),
        state.brush.Get(), 2.0F);

    fillRounded(state, accentPreview, 7, field);
    fillRounded(state,
                {accentPreview.left + 12, accentPreview.top + 12, accentPreview.left + 52,
                 accentPreview.top + 52},
                6, primary);
    drawCenteredText(
        state, accentHexLabel(state),
        {accentPreview.left + 56, accentPreview.top, accentPreview.right, accentPreview.bottom},
        state.headingFormat.Get(), white);
}

void drawScene(AppState &state) {
    const ULONGLONG now = GetTickCount64();
    if (state.renderedPage != state.page) {
        state.renderedPage = state.page;
        state.pageTransitionStart = now;
    }
    drawSidebar(state);
    drawTitlebar(state);
    if (state.page == Page::replay)
        drawReplayPage(state);
    else if (state.page == Page::clips)
        drawClipsPage(state);
    else if (state.page == Page::editor)
        drawEditorPage(state);
    else
        drawSettingsPage(state);
    // The native video child must not be faded independently of its player.
    // Navigation fades only affect the painted pages, and respect reduced motion.
    if (state.mainWindow != nullptr && state.clientAnimations && state.page != Page::editor &&
        state.pageTransitionStart != 0 && now - state.pageTransitionStart < 160) {
        const float remaining = 1.0F - static_cast<float>(now - state.pageTransitionStart) / 160;
        auto veil = background;
        veil.a = remaining * remaining * 0.75F;
        fillRounded(state, {contentLeft - 1, 70, contentRight + 16, windowHeight - 50}, 0, veil);
    }
    rule(state, contentLeft, windowHeight - 44, contentRight);
    drawText(state, state.status,
             {contentLeft, windowHeight - 30, contentRight - 200, windowHeight - 8},
             state.smallFormat.Get(), state.status.starts_with(L"Błąd") ? red : muted);
    drawText(state, L"LOKALNIE  /  NVENC",
             {contentRight - 156, windowHeight - 30, contentRight, windowHeight - 8},
             state.smallFormat.Get(), muted);
    drawClipContextMenu(state);
    drawAudioGroupDialog(state);
}
void paint(const HWND window, AppState &state) {
    PAINTSTRUCT paintInfo{};
    BeginPaint(window, &paintInfo);
    try {
        ensureGraphics(window, state);
        state.renderTarget->BeginDraw();
        state.renderTarget->Clear(background);
        drawScene(state);
        if (state.renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
            state.renderTarget.Reset();
            state.windowRenderTarget.Reset();
            state.brush.Reset();
            state.thumbnailBitmaps.clear();
        }
    } catch (...) {
    }
    EndPaint(window, &paintInfo);
}

void updateEditorVisibility(AppState &state) {
    if (state.page == Page::clips || state.page == Page::settings ||
        (state.page == Page::replay && state.engine.isRunning())) {
        state.activeField = HitTarget::none;
    }
    if (state.videoWindow != nullptr) {
        if (state.mainWindow != nullptr) {
            const RECT video = physicalRect(
                state.mainWindow, videoSurfaceRect);
            MoveWindow(state.videoWindow, video.left, video.top,
                       std::max(1L, video.right - video.left),
                       std::max(1L, video.bottom - video.top), TRUE);
            if (state.mediaPlayer != nullptr) state.mediaPlayer->UpdateVideo();
        }
        ShowWindow(state.videoWindow, state.page == Page::editor ? SW_SHOW : SW_HIDE);
        if (state.page == Page::editor) {
            state.embeddedVideoRefreshFrames = std::max(
                state.embeddedVideoRefreshFrames, 18);
            InvalidateRect(state.videoWindow, nullptr, FALSE);
        }
    }
}

void setStatus(const HWND window, AppState& state, std::wstring text) {
    state.status = std::move(text);
    wcsncpy_s(state.tray.szTip, state.status.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &state.tray);
    InvalidateRect(window, nullptr, FALSE);
}

void refreshAudioApplications(const HWND window, AppState& state) {
    std::map<DWORD, AudioRow> previous;
    for (const auto& row : state.audioRows) previous[row.processId] = row;
    state.audioRows.clear();
    for (const auto& application : nexplay::audio::activeAudioApplications()) {
        const auto selection = previous.find(application.processId);
        AudioRow row{.processId = application.processId, .name = application.name};
        if (selection != previous.end()) {
            row.included = selection->second.included;
            row.groupSelected = selection->second.groupSelected;
            row.groupName = selection->second.groupName;
        }
        state.audioRows.push_back(std::move(row));
    }
    state.audioScroll = 0;
    setStatus(window, state,
              L"Wykryto " + std::to_wstring(state.audioRows.size()) + L" aktywnych źródeł audio");
}

void refreshClips(const HWND window, AppState& state) {
    state.clips.clear();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(clipsDirectory(), error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != L".mp4") continue;
        ClipRow clip{
            .path = entry.path(),
            .size = entry.file_size(error),
            .modified = entry.last_write_time(error),
        };
        if (const auto preview = state.thumbnailMemory.find(clip.path);
            preview != state.thumbnailMemory.end()) {
            clip.preview = preview->second;
        } else {
            clip.preview = std::make_shared<ClipPreview>();
            state.thumbnailMemory.emplace(clip.path, clip.preview);
        }
        state.clips.push_back(std::move(clip));
    }
    std::sort(state.clips.begin(), state.clips.end(),
              [](const ClipRow& left, const ClipRow& right) { return left.modified > right.modified; });
    state.clipScroll = 0;
    for (const auto& clip : state.clips) {
        if (clip.preview == nullptr || clip.preview->frames.empty()) {
            requestThumbnail(window, state, clip.path, 0);
        }
    }
    InvalidateRect(window, nullptr, FALSE);
}

[[nodiscard]] double variantSeconds(const PROPVARIANT& value) {
    if (value.vt == VT_I8) return static_cast<double>(value.hVal.QuadPart) / 10'000'000.0;
    if (value.vt == VT_UI8) return static_cast<double>(value.uhVal.QuadPart) / 10'000'000.0;
    return 0.0;
}

void closeEditorPlayer(AppState& state) {
    if (state.fullscreen) exitFullscreen(state);
    state.previewAudio.close();
    if (state.mediaPlayer != nullptr) state.mediaPlayer->Shutdown();
    state.mediaPlayer.Reset();
    if (state.videoWindow != nullptr) ShowWindow(state.videoWindow, SW_HIDE);
    state.playing = false;
    state.embeddedVideoRefreshFrames = 0;
    state.dragHandle = DragHandle::none;
}

void enterFullscreen(AppState& state) {
    if (state.videoWindow == nullptr || state.fullscreen) return;
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const HMONITOR monitor = MonitorFromWindow(
        state.mainWindow, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return;
    const double position = state.playPosition;
    const bool resumePlayback = state.playing;
    if (state.mediaPlayer != nullptr) state.mediaPlayer->Pause();
    state.previewAudio.pause();

    state.fullscreenWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        fullscreenWindowClassName, L"NexPlay — pełny ekran", WS_POPUP | WS_CLIPCHILDREN,
        monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
        monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
        monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
        state.mainWindow, nullptr, GetModuleHandleW(nullptr), &state);
    if (state.fullscreenWindow == nullptr) {
        if (resumePlayback && state.mediaPlayer != nullptr) state.mediaPlayer->Play();
        return;
    }
    RECT client{};
    GetClientRect(state.fullscreenWindow, &client);
    constexpr int controlsHeight = 128;
    state.fullscreenVideoWindow = CreateWindowExW(
        0, videoWindowClassName, L"", WS_CHILD | WS_VISIBLE,
        0, 0, client.right, std::max(1L, client.bottom - controlsHeight),
        state.fullscreenWindow, nullptr, GetModuleHandleW(nullptr), state.mainWindow);
    if (state.fullscreenVideoWindow == nullptr || FAILED(MFPCreateMediaPlayer(
            state.selectedClip.c_str(), FALSE, 0, nullptr,
            state.fullscreenVideoWindow, &state.fullscreenPlayer))) {
        DestroyWindow(state.fullscreenWindow);
        state.fullscreenWindow = nullptr;
        state.fullscreenVideoWindow = nullptr;
        state.fullscreenPlayer.Reset();
        if (resumePlayback && state.mediaPlayer != nullptr) state.mediaPlayer->Play();
        return;
    }
    state.fullscreen = true;
    state.fullscreenPlayer->SetMute(TRUE);
    state.activeField = HitTarget::none;
    seekEditor(state, position);
    if (resumePlayback) state.fullscreenPlayer->Play();
    ShowWindow(state.fullscreenWindow, SW_SHOW);
    SetForegroundWindow(state.fullscreenWindow);
    SetFocus(state.fullscreenWindow);
    state.fullscreenPlayer->UpdateVideo();
}

void exitFullscreen(AppState& state) {
    if (!state.fullscreen) return;
    updateEditorPlayback(state);
    const double position = state.playPosition;
    const bool resumePlayback = state.playing;
    state.previewAudio.pause();
    if (state.fullscreenPlayer != nullptr) state.fullscreenPlayer->Shutdown();
    state.fullscreenPlayer.Reset();
    state.fullscreenRenderTarget.Reset();
    state.fullscreenBrush.Reset();
    const HWND fullscreenWindow = state.fullscreenWindow;
    state.fullscreenWindow = nullptr;
    state.fullscreenVideoWindow = nullptr;
    state.fullscreen = false;
    if (fullscreenWindow != nullptr) DestroyWindow(fullscreenWindow);
    const RECT video = physicalRect(state.mainWindow, videoSurfaceRect);
    MoveWindow(state.videoWindow, video.left, video.top,
               std::max(1L, video.right - video.left),
               std::max(1L, video.bottom - video.top), TRUE);
    ShowWindow(state.videoWindow, SW_SHOW);
    seekEditor(state, position);
    if (resumePlayback && state.mediaPlayer != nullptr) state.mediaPlayer->Play();
    SetFocus(state.mainWindow);
    state.embeddedVideoRefreshFrames = 45;
    InvalidateRect(state.videoWindow, nullptr, FALSE);
    UpdateWindow(state.videoWindow);
}

void openEditor(const HWND window, AppState& state, const std::filesystem::path& clip) {
    closeEditorPlayer(state);
    state.page = Page::editor;
    state.selectedClip = clip;
    state.editorName = clip.stem().wstring() + L"-edit";
    state.editorDuration = 0;
    state.editorVideoWidth = 0;
    state.editorVideoHeight = 0;
    state.playPosition = 0;
    state.trimStart = 0;
    state.trimEnd = 0;
    state.editorAudioTracks = probeEditorAudioTracks(clip);
    state.removedEditorAudioTracks.clear();
    state.editorAudioScroll = 0;
    state.activeEditorAudioTrack = -1;
    state.mergeEditorAudio = false;
    state.cutEditorSelection = false;
    state.activeField = HitTarget::none;
    updateEditorVisibility(state);
    ShowWindow(state.videoWindow, SW_SHOW);
    const HRESULT result = MFPCreateMediaPlayer(
        clip.c_str(), FALSE, 0, nullptr, state.videoWindow, &state.mediaPlayer);
    if (FAILED(result)) {
        closeEditorPlayer(state);
        state.page = Page::clips;
        setStatus(window, state, L"Błąd: nie można otworzyć podglądu klipu");
        return;
    }
    state.mediaPlayer->SetMute(TRUE);
    try {
        std::vector<nexplay::playback::AudioSelection> tracks;
        for (const auto& track : state.editorAudioTracks)
            tracks.push_back({track.included, track.start, track.end, track.trackId});
        state.previewAudio.open(clip, tracks);
    } catch (...) {
        setStatus(window, state, L"Błąd: nie można przygotować odsłuchu ścieżek; podgląd pozostaje wyciszony");
    }
    state.mediaPlayer->Play();
    state.playing = true;
    state.embeddedVideoRefreshFrames = 90;
    InvalidateRect(state.videoWindow, nullptr, FALSE);
    UpdateWindow(state.videoWindow);
    InvalidateRect(window, nullptr, FALSE);
}

void updatePreviewAudio(AppState& state) {
    std::vector<nexplay::playback::AudioSelection> tracks;
    for (const auto& track : state.editorAudioTracks) {
        const auto bounds = editorAudioBounds(state, track);
        tracks.push_back({track.included && editorTimelineEdit(state).contains(state.playPosition),
                          bounds.start, bounds.end, track.trackId});
    }
    state.previewAudio.update(tracks, state.playPosition, state.playing);
}

void removeEditorAudioTrack(AppState& state, const std::size_t index) {
    if (index >= state.editorAudioTracks.size()) return;
    state.removedEditorAudioTracks.push_back({index, state.editorAudioTracks[index]});
    state.editorAudioTracks.erase(state.editorAudioTracks.begin() + index);
    state.activeEditorAudioTrack = -1;
    state.editorAudioScroll = std::min(state.editorAudioScroll,
        std::max(0, static_cast<int>(state.editorAudioTracks.size()) - editorVisibleTracks));
    updatePreviewAudio(state);
}

void restoreEditorAudioTrack(AppState& state) {
    if (state.removedEditorAudioTracks.empty()) return;
    auto removed = std::move(state.removedEditorAudioTracks.back());
    state.removedEditorAudioTracks.pop_back();
    const auto index = std::min(removed.index, state.editorAudioTracks.size());
    state.editorAudioTracks.insert(state.editorAudioTracks.begin() + index, std::move(removed.track));
    updatePreviewAudio(state);
}

void updateEditorPlayback(AppState& state) {
    IMFPMediaPlayer* player = state.fullscreen && state.fullscreenPlayer != nullptr
        ? state.fullscreenPlayer.Get()
        : state.mediaPlayer.Get();
    if (player == nullptr) return;
    if (state.editorVideoWidth == 0 || state.editorVideoHeight == 0) {
        SIZE nativeSize{};
        if (SUCCEEDED(player->GetNativeVideoSize(&nativeSize, nullptr))) {
            state.editorVideoWidth = nativeSize.cx;
            state.editorVideoHeight = nativeSize.cy;
        }
    }
    PROPVARIANT value{};
    PropVariantInit(&value);
    if (state.editorDuration <= 0.0 &&
        SUCCEEDED(player->GetDuration(MFP_POSITIONTYPE_100NS, &value))) {
        state.editorDuration = variantSeconds(value);
        state.trimEnd = state.editorDuration;
        for (auto& track : state.editorAudioTracks) {
            if (track.end <= 0.0) track.end = state.editorDuration;
        }
    }
    PropVariantClear(&value);
    PropVariantInit(&value);
    if (SUCCEEDED(player->GetPosition(MFP_POSITIONTYPE_100NS, &value))) {
        state.playPosition = std::clamp(variantSeconds(value), 0.0, state.editorDuration);
    }
    PropVariantClear(&value);
    MFP_MEDIAPLAYER_STATE playerState{};
    if (SUCCEEDED(player->GetState(&playerState))) {
        state.playing = playerState == MFP_MEDIAPLAYER_STATE_PLAYING;
    }
    updatePreviewAudio(state);
}

void seekEditor(AppState& state, const double seconds) {
    IMFPMediaPlayer* player = state.fullscreen && state.fullscreenPlayer != nullptr
        ? state.fullscreenPlayer.Get()
        : state.mediaPlayer.Get();
    if (player == nullptr) return;
    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = static_cast<LONGLONG>(seconds * 10'000'000.0);
    player->SetPosition(MFP_POSITIONTYPE_100NS, &position);
    state.playPosition = seconds;
    state.previewAudio.seek(seconds);
    updatePreviewAudio(state);
}

[[nodiscard]] IMFPMediaPlayer* activeEditorPlayer(AppState& state) {
    return state.fullscreen && state.fullscreenPlayer != nullptr
        ? state.fullscreenPlayer.Get()
        : state.mediaPlayer.Get();
}

void toggleEditorPlayback(AppState& state) {
    if (IMFPMediaPlayer* player = activeEditorPlayer(state); player != nullptr) {
        if (state.playing) player->Pause();
        else player->Play();
        state.playing = !state.playing;
        updatePreviewAudio(state);
    }
}

void seekEditorBy(AppState& state, const double seconds) {
    seekEditor(state, std::clamp(
        state.playPosition + seconds, 0.0, std::max(0.0, state.editorDuration)));
}

[[nodiscard]] double timelineSeconds(const AppState& state, const float x) {
    const float fraction = std::clamp(
        (x - editorTimelineRect.left) /
            (editorTimelineRect.right - editorTimelineRect.left),
        0.0F, 1.0F);
    return static_cast<double>(fraction) * state.editorDuration;
}

void moveTrimHandle(AppState& state, const float x) {
    if (state.editorDuration <= 0.0) return;
    const double position = timelineSeconds(state, x);
    if (state.dragHandle == DragHandle::start) {
        state.trimStart = std::clamp(position, 0.0, std::max(0.0, state.trimEnd - 0.1));
        seekEditor(state, state.trimStart);
    } else if (state.dragHandle == DragHandle::end) {
        state.trimEnd = std::clamp(position, state.trimStart + 0.1, state.editorDuration);
        seekEditor(state, state.trimEnd);
    } else if (state.dragHandle == DragHandle::playhead) {
        seekEditor(state, std::clamp(position, 0.0, state.editorDuration));
    }
}

void moveEditorAudioHandle(AppState& state, const float x) {
    if (state.editorDuration <= 0.0 || state.activeEditorAudioTrack < 0 ||
        state.activeEditorAudioTrack >= static_cast<int>(state.editorAudioTracks.size())) return;
    const int visible = state.activeEditorAudioTrack - state.editorAudioScroll;
    if (visible < 0 || visible >= editorVisibleTracks) return;
    const Rect timeline = editorAudioTimelineRect(visible);
    const double position = std::clamp(
        static_cast<double>((x - timeline.left) / (timeline.right - timeline.left)) *
            state.editorDuration,
        0.0, state.editorDuration);
    auto& track = state.editorAudioTracks[
        static_cast<std::size_t>(state.activeEditorAudioTrack)];
    const auto bounds = editorAudioBounds(state, track);
    const double minimum = state.cutEditorSelection ? 0.0 : state.trimStart;
    const double maximum = state.cutEditorSelection ? state.editorDuration : state.trimEnd;
    if (state.dragHandle == DragHandle::audioStart) {
        track.start = std::clamp(position, minimum, std::max(minimum, bounds.end - 0.05));
        seekEditor(state, track.start);
    } else if (state.dragHandle == DragHandle::audioEnd) {
        track.end = std::clamp(position, std::min(maximum, bounds.start + 0.05), maximum);
        seekEditor(state, track.end);
    }
}

void drawFullscreenText(
    AppState& state, const std::wstring& text, const Rect rectangle,
    IDWriteTextFormat* format, const D2D1_COLOR_F color,
    const bool centered = false) {
    if (centered) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    state.fullscreenBrush->SetColor(color);
    state.fullscreenRenderTarget->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()), format, rectangle.d2d(),
        state.fullscreenBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (centered) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
}

[[nodiscard]] Rect fullscreenPlayRect(const float height) {
    return {24, height - 78, 80, height - 26};
}

[[nodiscard]] Rect fullscreenTimelineRect(const float width, const float height) {
    return {104, height - 60, std::max(120.0F, width - 272), height - 46};
}

[[nodiscard]] Rect fullscreenExitRect(const float width, const float height) {
    return {width - 148, height - 78, width - 24, height - 26};
}

void paintFullscreen(const HWND window, AppState& state) {
    PAINTSTRUCT paintInfo{};
    BeginPaint(window, &paintInfo);
    if (state.d2dFactory == nullptr || state.bodyFormat == nullptr) {
        EndPaint(window, &paintInfo);
        return;
    }
    RECT client{};
    GetClientRect(window, &client);
    if (state.fullscreenRenderTarget == nullptr) {
        if (FAILED(state.d2dFactory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(
                    window, D2D1::SizeU(client.right, client.bottom)),
                &state.fullscreenRenderTarget))) {
            EndPaint(window, &paintInfo);
            return;
        }
        state.fullscreenRenderTarget->CreateSolidColorBrush(white, &state.fullscreenBrush);
    }
    auto* target = state.fullscreenRenderTarget.Get();
    const float width = static_cast<float>(client.right);
    const float height = static_cast<float>(client.bottom);
    const Rect play = fullscreenPlayRect(height);
    const Rect timeline = fullscreenTimelineRect(width, height);
    const Rect exit = fullscreenExitRect(width, height);

    target->BeginDraw();
    target->Clear(background);
    state.fullscreenBrush->SetColor(D2D1_COLOR_F{0.018F, 0.018F, 0.026F, 1.0F});
    target->FillRectangle(D2D1::RectF(0, height - 128, width, height),
                          state.fullscreenBrush.Get());
    state.fullscreenBrush->SetColor(D2D1_COLOR_F{0.10F, 0.10F, 0.15F, 1.0F});
    target->DrawLine(D2D1::Point2F(0, height - 128),
                     D2D1::Point2F(width, height - 128), state.fullscreenBrush.Get());

    drawFullscreenText(state, state.selectedClip.filename().wstring(),
                       {24, height - 119, width * 0.55F, height - 90},
                       state.headingFormat.Get(), white);
    const std::wstring videoResolution =
        state.editorVideoWidth > 0 && state.editorVideoHeight > 0
        ? std::to_wstring(state.editorVideoWidth) + L" × " +
              std::to_wstring(state.editorVideoHeight)
        : std::wstring(L"pełna rozdzielczość");
    drawFullscreenText(
        state,
        videoResolution + L"  •  " + state.fpsText + L" FPS  •  " +
            state.bitrateText + L" Mb/s  •  wszystkie ścieżki audio",
        {width * 0.55F, height - 114, width - 24, height - 90},
        state.smallFormat.Get(), muted);
    drawFullscreenText(
        state, L"Spacja  play/pause   •   ← →  ±5 s   •   Home / End",
        {104, height - 86, std::max(104.0F, width - 272), height - 64},
        state.smallFormat.Get(), muted);

    state.fullscreenBrush->SetColor(field);
    target->FillRoundedRectangle(D2D1::RoundedRect(play.d2d(), 12, 12),
                                 state.fullscreenBrush.Get());
    drawFullscreenText(state, state.playing ? L"Ⅱ" : L"▶", play,
                       state.headingFormat.Get(), white, true);

    const double duration = std::max(0.001, state.editorDuration);
    const float playX = timeline.left +
        static_cast<float>(std::clamp(state.playPosition / duration, 0.0, 1.0)) *
            (timeline.right - timeline.left);
    state.fullscreenBrush->SetColor(border);
    target->FillRoundedRectangle(
        D2D1::RoundedRect({timeline.left, timeline.top + 4,
                           timeline.right, timeline.bottom - 4}, 3, 3),
        state.fullscreenBrush.Get());
    state.fullscreenBrush->SetColor(primary);
    target->FillRoundedRectangle(
        D2D1::RoundedRect({timeline.left, timeline.top + 4,
                           playX, timeline.bottom - 4}, 3, 3),
        state.fullscreenBrush.Get());
    state.fullscreenBrush->SetColor(white);
    target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(playX, (timeline.top + timeline.bottom) * 0.5F),
                      7, 7),
        state.fullscreenBrush.Get());

    drawFullscreenText(
        state, timeLabel(state.playPosition) + L" / " + timeLabel(state.editorDuration),
        {width - 255, height - 72, width - 158, height - 32},
        state.smallFormat.Get(), muted, true);
    state.fullscreenBrush->SetColor(field);
    target->FillRoundedRectangle(D2D1::RoundedRect(exit.d2d(), 12, 12),
                                 state.fullscreenBrush.Get());
    drawFullscreenText(state, L"ESC  Zamknij", exit, state.buttonFormat.Get(), white, true);

    if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
        state.fullscreenRenderTarget.Reset();
        state.fullscreenBrush.Reset();
    }
    EndPaint(window, &paintInfo);
}

LRESULT CALLBACK fullscreenWindowProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
    case WM_PAINT:
        if (state != nullptr) paintFullscreen(window, *state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (state != nullptr) {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            if (state->fullscreenRenderTarget != nullptr && width > 0 && height > 0) {
                state->fullscreenRenderTarget->Resize(D2D1::SizeU(width, height));
            }
            if (state->fullscreenVideoWindow != nullptr) {
                MoveWindow(state->fullscreenVideoWindow, 0, 0, width,
                           std::max(1, height - 128), TRUE);
                if (state->fullscreenPlayer != nullptr) state->fullscreenPlayer->UpdateVideo();
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (state != nullptr) {
            if (wParam == VK_ESCAPE) {
                PostMessageW(state->mainWindow, exitFullscreenMessage, 0, 0);
                return 0;
            }
            if (wParam == VK_SPACE) {
                toggleEditorPlayback(*state);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                seekEditorBy(*state, wParam == VK_LEFT ? -5.0 : 5.0);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_HOME || wParam == VK_END) {
                seekEditor(*state, wParam == VK_HOME ? 0.0 : state->editorDuration);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        if (state != nullptr) PostMessageW(state->mainWindow, exitFullscreenMessage, 0, 0);
        return 0;
    case WM_LBUTTONDOWN:
        if (state != nullptr) {
            RECT client{};
            GetClientRect(window, &client);
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            const float width = static_cast<float>(client.right);
            const float height = static_cast<float>(client.bottom);
            if (fullscreenPlayRect(height).contains(x, y) && state->fullscreenPlayer != nullptr) {
                if (state->playing) state->fullscreenPlayer->Pause();
                else state->fullscreenPlayer->Play();
                state->playing = !state->playing;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (fullscreenExitRect(width, height).contains(x, y)) {
                PostMessageW(state->mainWindow, exitFullscreenMessage, 0, 0);
                return 0;
            }
            const Rect timeline = fullscreenTimelineRect(width, height);
            if (timeline.contains(x, y) && state->editorDuration > 0.0) {
                state->fullscreenScrubbing = true;
                SetCapture(window);
                seekEditor(*state, std::clamp(
                    static_cast<double>((x - timeline.left) / (timeline.right - timeline.left)) *
                        state->editorDuration,
                    0.0, state->editorDuration));
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (state != nullptr && state->fullscreenScrubbing && state->editorDuration > 0.0) {
            RECT client{};
            GetClientRect(window, &client);
            const Rect timeline = fullscreenTimelineRect(
                static_cast<float>(client.right), static_cast<float>(client.bottom));
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            seekEditor(*state, std::clamp(
                static_cast<double>((x - timeline.left) / (timeline.right - timeline.left)) *
                    state->editorDuration,
                0.0, state->editorDuration));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (state != nullptr && state->fullscreenScrubbing) {
            state->fullscreenScrubbing = false;
            ReleaseCapture();
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

[[nodiscard]] std::uint32_t editorNumber(
    const std::wstring& text, const std::uint32_t minimum, const std::uint32_t maximum) {
    wchar_t* end{};
    const unsigned long value = wcstoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L'\0' || value < minimum || value > maximum) {
        throw std::runtime_error("Jedno z ustawien ma nieprawidlowa wartosc.");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] nexplay::app::RecorderSettings readSettings(const AppState& state) {
    nexplay::app::RecorderSettings settings;
    settings.bufferDuration = std::chrono::seconds(editorNumber(state.durationText, 10, 1'200));
    settings.framesPerSecond = editorNumber(state.fpsText, 1, 1'000);
    settings.bitrate = editorNumber(state.bitrateText, 1, 1'000) * 1'000'000;
    const auto& resolution = resolutionPresets[state.resolutionPreset];
    const bool portrait = GetSystemMetrics(SM_CYSCREEN) > GetSystemMetrics(SM_CXSCREEN);
    settings.outputWidth = portrait ? resolution.height : resolution.width;
    settings.outputHeight = portrait ? resolution.width : resolution.height;
    settings.captureMicrophone = state.microphone;
    for (const auto& row : state.audioRows) {
        if (!row.included) settings.excludedProcessIds.insert(row.processId);
        else if (!row.groupName.empty()) {
            settings.groupedProcessNames.emplace(row.processId, row.groupName);
        }
    }
    return settings;
}

void startRecorder(const HWND window, AppState& state) {
    try {
        const auto settings = readSettings(state);
        savePersistentRecordingSettings(state);
        state.engine.start(
            settings,
            [window](std::wstring message) {
                auto* owned = new std::wstring(std::move(message));
                if (!PostMessageW(window, statusMessage, 0, reinterpret_cast<LPARAM>(owned))) {
                    delete owned;
                }
            },
            [window](nexplay::app::SaveProgress progress) {
                auto* owned = new nexplay::app::SaveProgress(std::move(progress));
                if (!PostMessageW(window, saveProgressMessage, 0, reinterpret_cast<LPARAM>(owned)))
                    delete owned;
            });
        updateEditorVisibility(state);
        setStatus(window, state, L"Uruchamianie NVENC i źródeł audio…");
    } catch (const std::exception& error) {
        MessageBoxA(window, error.what(), "NexPlay", MB_OK | MB_ICONERROR);
    }
}

void stopRecorder(const HWND window, AppState& state) {
    setStatus(window, state, L"Zatrzymywanie bufora…");
    // Let outstanding save jobs report progress while the capture worker shuts down.
    state.engine.requestStop();
    updateEditorVisibility(state);
}

void saveClip(const HWND window, AppState& state) {
    if (!state.engine.isRunning()) return;
    const auto id = state.engine.requestSave();
    if (!id)
        return;
    try {
        state.saveToasts.configure(state.toastCorner, primary);
        state.saveToasts.update({id, nexplay::app::SavePhase::queued, 0, {}});
    } catch (...) {
        // Saving must continue even if the display cannot create an overlay.
    }
    setStatus(window, state, L"Zapisuję klip i zeruję bufor…");
}

void openClipsFolder(const HWND window) {
    try {
        const auto folder = clipsDirectory();
        ShellExecuteW(window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } catch (...) {
    }
}

void exportEditor(const HWND window, AppState& state) {
    if (state.editorExporting) return;
    if (state.editorDuration <= 0.0 || !validClipName(state.editorName)) {
        setStatus(window, state, L"Błąd: wpisz poprawną nazwę klipu");
        return;
    }
    if (state.cutEditorSelection &&
        state.editorDuration - (state.trimEnd - state.trimStart) < 0.1) {
        setStatus(window, state, L"Błąd: nie można wyciąć całego klipu");
        return;
    }
    if (state.mediaPlayer != nullptr) state.mediaPlayer->Pause();
    state.previewAudio.pause();
    state.playing = false;
    setStatus(window, state,
              state.cutEditorSelection
                  ? L"Wycinam fragment i składam jeden klip przez NVENC…"
                  : L"Eksportuję przycięty klip przez NVENC…");
    const auto input = state.selectedClip;
    const auto name = state.editorName;
    const double start = state.trimStart;
    const double end = state.trimEnd;
    const auto audioTracks = state.editorAudioTracks;
    const bool mergeAudio = state.mergeEditorAudio;
    const bool cutSelection = state.cutEditorSelection;
    const double fullDuration = state.editorDuration;
    state.editorExporting = true;
    state.editorExportPercent = 0;
    state.editorExportClip = input;
    const auto exportId = ++state.editorExportId;
    state.editorJobs.emplace_back([window, input, name, start, end,
                                   audioTracks, mergeAudio, cutSelection, fullDuration, exportId] {
        auto result = std::make_unique<EditorResult>();
        try {
            *result = exportEditedClip(input, name, start, end, audioTracks, mergeAudio,
                cutSelection, fullDuration, [window, exportId](int percent) {
                    PostMessageW(window, editorProgressMessage, exportId, percent);
                });
        } catch (const std::exception& error) {
            result->message = utf8ToWide(error.what());
        } catch (...) {
            result->message = L"Nieoczekiwany błąd eksportu GPU.";
        }
        result->exportId = exportId;
        if (PostMessageW(window, editorDoneMessage, 0, reinterpret_cast<LPARAM>(result.get())))
            result.release();
    });
}

[[nodiscard]] HitTarget hitTest(const AppState& state, const float x, const float y) {
    if (state.audioGroupDialogOpen) {
        if (audioGroupNameRect.contains(x, y)) return HitTarget::audioGroupName;
        if (cancelAudioGroupRect.contains(x, y)) return HitTarget::cancelAudioGroup;
        if (confirmAudioGroupRect.contains(x, y) &&
            validAudioGroupName(state.audioGroupName)) {
            return HitTarget::confirmAudioGroup;
        }
        return HitTarget::none;
    }
    if (minimizeRect.contains(x, y)) return HitTarget::minimize;
    if (maximizeRect.contains(x, y)) return HitTarget::maximize;
    if (closeRect.contains(x, y)) return HitTarget::close;
    if (replayNavRect.contains(x, y)) return HitTarget::replayPage;
    if (clipsNavRect.contains(x, y)) return HitTarget::clipsPage;
    if (settingsNavRect.contains(x, y)) return HitTarget::settingsPage;
    if (state.page == Page::replay) {
        if (startRect.contains(x, y)) return HitTarget::startStop;
        if (saveRect.contains(x, y) && state.engine.isRunning()) return HitTarget::save;
        if (refreshRect.contains(x, y) && !state.engine.isRunning()) return HitTarget::refreshAudio;
        if (microphoneRect.contains(x, y) && !state.engine.isRunning()) return HitTarget::microphone;
        if (createAudioGroupRect.contains(x, y) && !state.engine.isRunning() &&
            selectedAudioRowCount(state) >= 2) return HitTarget::createAudioGroup;
        if (!state.engine.isRunning() && durationFieldRect.contains(x, y)) return HitTarget::durationField;
        if (!state.engine.isRunning() && resolutionFieldRect.contains(x, y)) return HitTarget::resolutionField;
        if (!state.engine.isRunning() && fpsFieldRect.contains(x, y)) return HitTarget::fpsField;
        if (!state.engine.isRunning() && bitrateFieldRect.contains(x, y)) return HitTarget::bitrateField;
    } else if (state.page == Page::clips && openClipsRect.contains(x, y)) {
        return HitTarget::openClips;
    } else if (state.page == Page::editor) {
        if (editorBackRect.contains(x, y)) return HitTarget::editorBack;
        if (editorMergeAudioRect.contains(x, y)) {
            return HitTarget::editorMergeAudio;
        }
        if (editorCutModeRect.contains(x, y) && state.editorDuration > 0.2) {
            return HitTarget::editorCutMode;
        }
        if (editorPlayRect.contains(x, y)) return HitTarget::editorPlay;
        if (editorFullscreenRect.contains(x, y)) return HitTarget::editorFullscreen;
        if (editorSaveRect.contains(x, y) && !state.editorExporting) return HitTarget::editorSave;
        if (editorNameRect.contains(x, y)) return HitTarget::editorName;
    } else if (state.page == Page::settings) {
        if (autostartRect.contains(x, y)) return HitTarget::autostartToggle;
        if (autoBufferRect.contains(x, y)) return HitTarget::autoBufferToggle;
        if (saveHotkeyToggleRect.contains(x, y))
            return HitTarget::saveHotkeyToggle;
        if (stopHotkeyToggleRect.contains(x, y))
            return HitTarget::stopHotkeyToggle;
        for (int i = 0; i < 4; ++i)
            if (toastCornerRects[i].contains(x, y))
                return static_cast<HitTarget>(static_cast<int>(HitTarget::toastTopLeft) + i);
        if (saveHotkeyRect.contains(x, y)) return HitTarget::saveHotkey;
        if (stopHotkeyRect.contains(x, y)) return HitTarget::stopHotkey;
        if (fixedAspectRect(state.mainWindow, accentPlaneRect).contains(x, y)) {
            return HitTarget::accentPlane;
        }
        if (fixedAspectRect(state.mainWindow, accentHueRect).contains(x, y)) {
            return HitTarget::accentHue;
        }
    }
    return HitTarget::none;
}

constexpr Rect trayShowRect{12, 68, 298, 112};
constexpr Rect traySaveRect{12, 116, 298, 160};
constexpr Rect trayExitRect{12, 182, 298, 226};

[[nodiscard]] D2D1_COLOR_F blendColor(
    const D2D1_COLOR_F from, const D2D1_COLOR_F to, const float amount) noexcept {
    const float factor = std::clamp(amount, 0.0F, 1.0F);
    return {
        std::lerp(from.r, to.r, factor),
        std::lerp(from.g, to.g, factor),
        std::lerp(from.b, to.b, factor),
        std::lerp(from.a, to.a, factor),
    };
}

void ensureTrayMenuGraphics(const HWND window, AppState& state) {
    if (state.trayMenuRenderTarget != nullptr) return;
    if (state.d2dFactory == nullptr && FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&state.d2dFactory)))) {
        throw std::runtime_error("Nie mozna uruchomic menu zasobnika.");
    }
    if (state.writeFactory == nullptr && FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(state.writeFactory.GetAddressOf())))) {
        throw std::runtime_error("Nie mozna uruchomic tekstu menu zasobnika.");
    }
    RECT client{};
    GetClientRect(window, &client);
    if (FAILED(state.d2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
                96.0F, 96.0F),
            D2D1::HwndRenderTargetProperties(
                window, D2D1::SizeU(client.right, client.bottom)),
            &state.trayMenuRenderTarget)) ||
        FAILED(state.trayMenuRenderTarget->CreateSolidColorBrush(
            white, &state.trayMenuBrush))) {
        state.trayMenuRenderTarget.Reset();
        throw std::runtime_error("Nie mozna utworzyc powierzchni menu zasobnika.");
    }
    if (state.trayMenuTitleFormat == nullptr) {
        createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Display", 16,
                         DWRITE_FONT_WEIGHT_BOLD, state.trayMenuTitleFormat);
        createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Text", 13,
                         DWRITE_FONT_WEIGHT_SEMI_BOLD, state.trayMenuBodyFormat);
        createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Text", 10,
                         DWRITE_FONT_WEIGHT_SEMI_BOLD, state.trayMenuSmallFormat);
    }
}

void trayFillRounded(AppState& state, const Rect rectangle, const float radius,
                     const D2D1_COLOR_F color) {
    state.trayMenuBrush->SetColor(color);
    state.trayMenuRenderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(rectangle.d2d(), radius, radius), state.trayMenuBrush.Get());
}

void trayStrokeRounded(AppState& state, const Rect rectangle, const float radius,
                       const D2D1_COLOR_F color, const float width = 1.0F) {
    state.trayMenuBrush->SetColor(color);
    state.trayMenuRenderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(rectangle.d2d(), radius, radius),
        state.trayMenuBrush.Get(), width);
}

void trayDrawText(AppState& state, const std::wstring& text, const Rect rectangle,
                  IDWriteTextFormat* format, const D2D1_COLOR_F color) {
    state.trayMenuBrush->SetColor(color);
    state.trayMenuRenderTarget->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()), format, rectangle.d2d(),
        state.trayMenuBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void trayDrawCenteredText(AppState& state, const std::wstring& text, const Rect rectangle,
                          IDWriteTextFormat* format, const D2D1_COLOR_F color) {
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    trayDrawText(state, text, rectangle, format, color);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void drawTrayMenuIcon(AppState& state, const int item, const float top,
                      const D2D1_COLOR_F color) {
    auto* target = state.trayMenuRenderTarget.Get();
    auto* brush = state.trayMenuBrush.Get();
    brush->SetColor(color);
    if (item == 0) {
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(25, top + 13, 39, top + 25), 2, 2),
            brush, 1.5F);
        target->DrawLine(D2D1::Point2F(28, top + 28),
                         D2D1::Point2F(36, top + 28), brush, 1.5F);
    } else if (item == 1) {
        target->DrawLine(D2D1::Point2F(32, top + 12),
                         D2D1::Point2F(32, top + 26), brush, 1.7F);
        target->DrawLine(D2D1::Point2F(27, top + 21),
                         D2D1::Point2F(32, top + 26), brush, 1.7F);
        target->DrawLine(D2D1::Point2F(37, top + 21),
                         D2D1::Point2F(32, top + 26), brush, 1.7F);
        target->DrawLine(D2D1::Point2F(25, top + 30),
                         D2D1::Point2F(39, top + 30), brush, 1.7F);
    } else {
        target->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(32, top + 22), 8, 8), brush, 1.6F);
        target->DrawLine(D2D1::Point2F(32, top + 10),
                         D2D1::Point2F(32, top + 21), brush, 1.8F);
    }
}

void drawTrayMenuRow(AppState& state, const int item, const Rect rectangle,
                     const std::wstring& label, const bool enabled,
                     const bool destructive = false) {
    const float hover = state.trayMenuHoverAnimation[static_cast<std::size_t>(item)];
    const D2D1_COLOR_F accent = destructive ? red : primary;
    if (hover > 0.01F && enabled) {
        D2D1_COLOR_F hoverFill = blendColor(field, accent, 0.11F);
        hoverFill.a = 0.40F + hover * 0.50F;
        trayFillRounded(state, rectangle, 6, hoverFill);
        D2D1_COLOR_F hoverBorder = accent;
        hoverBorder.a = hover * 0.34F;
        trayStrokeRounded(state, rectangle, 6, hoverBorder);
    }
    const D2D1_COLOR_F content = !enabled
        ? blendColor(muted, background, 0.40F)
        : (destructive && hover > 0.08F ? red : white);
    drawTrayMenuIcon(state, item, rectangle.top, content);
    trayDrawText(state, label, {52, rectangle.top + 13, 194, rectangle.bottom - 8},
                 state.trayMenuBodyFormat.Get(), content);
}

void paintTrayMenu(const HWND window, AppState& state) {
    PAINTSTRUCT paint{};
    BeginPaint(window, &paint);
    try {
        ensureTrayMenuGraphics(window, state);
        auto* target = state.trayMenuRenderTarget.Get();
        target->BeginDraw();
        target->Clear(background);

        trayStrokeRounded(state, {0.5F, 0.5F, trayMenuWidth - 0.5F,
                                  trayMenuHeight - 0.5F}, 8, border);
        trayFillRounded(state, {18, 16, 48, 46}, 6, primary);
        trayDrawCenteredText(state, L"N", {18, 16, 48, 46},
                             state.trayMenuTitleFormat.Get(), white);
        trayDrawText(state, L"NexPlay", {58, 18, 150, 42},
                     state.trayMenuTitleFormat.Get(), white);

        const bool running = state.engine.isRunning();
        const D2D1_COLOR_F statusColor = running ? green : muted;
        state.trayMenuBrush->SetColor(statusColor);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(224, 31), 3, 3),
                            state.trayMenuBrush.Get());
        trayDrawText(state, running ? L"AKTYWNY" : L"GOTOWY", {234, 24, 292, 42},
                     state.trayMenuSmallFormat.Get(), statusColor);

        drawTrayMenuRow(state, 0, trayShowRect, L"Otwórz NexPlay", true);
        drawTrayMenuRow(state, 1, traySaveRect, L"Zapisz klip", running);
        const std::wstring shortcut =
            state.saveHotkeyEnabled ? hotkeyLabel(state.saveHotkeyModifiers, state.saveHotkeyVk)
                                    : L"Wyłączony";
        const D2D1_COLOR_F shortcutColor = running ? muted : blendColor(muted, background, 0.40F);
        trayFillRounded(state, {200, 127, 284, 150}, 6,
                        running ? field : blendColor(field, background, 0.48F));
        trayStrokeRounded(state, {200, 127, 284, 150}, 6,
                          running ? border : blendColor(border, background, 0.50F));
        trayDrawCenteredText(state, shortcut, {202, 127, 282, 150},
                             state.trayMenuSmallFormat.Get(), shortcutColor);

        state.trayMenuBrush->SetColor(border);
        target->DrawLine(D2D1::Point2F(22, 173), D2D1::Point2F(288, 173),
                         state.trayMenuBrush.Get(), 1.0F);
        drawTrayMenuRow(state, 2, trayExitRect, L"Zakończ NexPlay", true, true);

        if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
            state.trayMenuBrush.Reset();
            state.trayMenuRenderTarget.Reset();
        }
    } catch (...) {
        state.trayMenuBrush.Reset();
        state.trayMenuRenderTarget.Reset();
    }
    EndPaint(window, &paint);
}

[[nodiscard]] int trayMenuItemAt(const AppState& state, const float x, const float y) {
    if (trayShowRect.contains(x, y)) return 0;
    if (traySaveRect.contains(x, y) && state.engine.isRunning()) return 1;
    if (trayExitRect.contains(x, y)) return 2;
    return -1;
}

void executeTrayMenuItem(const HWND menuWindow, AppState& state, const int item) {
    ShowWindow(menuWindow, SW_HIDE);
    if (item == 0) {
        SetTimer(state.mainWindow, 1, 16, nullptr);
        ShowWindow(state.mainWindow, SW_RESTORE);
        SetForegroundWindow(state.mainWindow);
    } else if (item == 1 && state.engine.isRunning()) {
        saveClip(state.mainWindow, state);
    } else if (item == 2) {
        stopRecorder(state.mainWindow, state);
        DestroyWindow(state.mainWindow);
    }
}

LRESULT CALLBACK trayMenuWindowProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
    case WM_CREATE: {
        const int darkMode = TRUE;
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &darkMode, sizeof(darkMode));
        const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &corner, sizeof(corner));
        return 0;
    }
    case WM_SHOWWINDOW:
        if (wParam) SetTimer(window, 1, 16, nullptr);
        else KillTimer(window, 1);
        return 0;
    case WM_PAINT:
        if (state != nullptr) paintTrayMenu(window, *state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (state != nullptr) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            const int hover = trayMenuItemAt(
                *state, static_cast<float>(GET_X_LPARAM(lParam)),
                static_cast<float>(GET_Y_LPARAM(lParam)));
            if (hover != state->trayMenuHover) {
                state->trayMenuHover = hover;
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (state != nullptr) {
            state->trayMenuHover = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state != nullptr) {
            const int item = trayMenuItemAt(
                *state, static_cast<float>(GET_X_LPARAM(lParam)),
                static_cast<float>(GET_Y_LPARAM(lParam)));
            if (item >= 0) executeTrayMenuItem(window, *state, item);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) ShowWindow(window, SW_HIDE);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ShowWindow(window, SW_HIDE);
        } else if (state != nullptr && (wParam == VK_DOWN || wParam == VK_UP)) {
            const int direction = wParam == VK_DOWN ? 1 : -1;
            int next = state->trayMenuHover;
            if (next < 0) next = direction > 0 ? -1 : 0;
            for (int attempt = 0; attempt < 3; ++attempt) {
                next = (next + direction + 3) % 3;
                if (next != 1 || state->engine.isRunning()) break;
            }
            state->trayMenuHover = next;
            InvalidateRect(window, nullptr, FALSE);
        } else if (state != nullptr && wParam == VK_RETURN && state->trayMenuHover >= 0) {
            executeTrayMenuItem(window, *state, state->trayMenuHover);
        }
        return 0;
    case WM_SETCURSOR:
        if (state != nullptr && LOWORD(lParam) == HTCLIENT && state->trayMenuHover >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (state != nullptr) {
            bool changed = false;
            for (std::size_t index = 0; index < state->trayMenuHoverAnimation.size(); ++index) {
                const float target = state->trayMenuHover == static_cast<int>(index) ? 1.0F : 0.0F;
                const float before = state->trayMenuHoverAnimation[index];
                state->trayMenuHoverAnimation[index] +=
                    (target - before) * (state->clientAnimations ? 0.20F : 1.0F);
                if (std::abs(target - state->trayMenuHoverAnimation[index]) < 0.006F) {
                    state->trayMenuHoverAnimation[index] = target;
                }
                changed = changed || before != state->trayMenuHoverAnimation[index];
            }
            if (changed || state->engine.isRunning()) InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_SIZE:
        if (state != nullptr && state->trayMenuRenderTarget != nullptr &&
            LOWORD(lParam) > 0 && HIWORD(lParam) > 0) {
            state->trayMenuRenderTarget->Resize(
                D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        if (state != nullptr) {
            state->trayMenuBrush.Reset();
            state->trayMenuRenderTarget.Reset();
            if (state->trayMenuWindow == window) state->trayMenuWindow = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void showTrayMenu(const HWND window, AppState& state) {
    if (state.trayMenuWindow == nullptr) {
        state.trayMenuWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            trayMenuWindowClassName, L"NexPlay",
            WS_POPUP, 0, 0,
            static_cast<int>(trayMenuWidth), static_cast<int>(trayMenuHeight),
            window, nullptr, GetModuleHandleW(nullptr), &state);
    }
    if (state.trayMenuWindow == nullptr) return;

    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = static_cast<int>(trayMenuWidth);
    const int height = static_cast<int>(trayMenuHeight);
    int x = cursor.x - width + 18;
    int y = cursor.y - height - 10;
    if (y < monitor.rcWork.top) y = cursor.y + 10;
    x = std::clamp(x, static_cast<int>(monitor.rcWork.left) + 8,
                   static_cast<int>(monitor.rcWork.right) - width - 8);
    y = std::clamp(y, static_cast<int>(monitor.rcWork.top) + 8,
                   static_cast<int>(monitor.rcWork.bottom) - height - 8);

    state.trayMenuHover = -1;
    SetWindowPos(state.trayMenuWindow, HWND_TOPMOST, x, y, width, height,
                 SWP_SHOWWINDOW);
    SetForegroundWindow(state.trayMenuWindow);
    SetFocus(state.trayMenuWindow);
    InvalidateRect(state.trayMenuWindow, nullptr, FALSE);
}

[[nodiscard]] int clipIndexAt(const AppState& state, const float x, const float y) {
    if (state.page != Page::clips || !clipsListRect.contains(x, y)) return -1;
    for (int visible = 0; visible < clipVisibleRows*clipColumns; ++visible) {
        const Rect clipCard=clipCardRect(visible);
        if (!clipCard.contains(x, y)) continue;
        const int index = state.clipScroll * clipColumns + visible;
        return index < static_cast<int>(state.clips.size()) ? index : -1;
    }
    return -1;
}

void copyPathToClipboard(const HWND window, const std::filesystem::path& path) {
    if (!OpenClipboard(window)) return;
    EmptyClipboard();
    const std::wstring text = path.wstring();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        if (void* destination = GlobalLock(memory); destination != nullptr) {
            CopyMemory(destination, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void openClipContextMenu(
    AppState& state, const std::filesystem::path& clip, const float x, const float y) {
    const float left = std::clamp(x, 260.0F, windowWidth - 310.0F);
    const float top = std::clamp(y, 72.0F, windowHeight - 266.0F);
    state.clipContextMenuClip = clip;
    state.clipContextMenuRect = {left, top, left + 294, top + 258};
    state.clipContextMenuOpen = true;
}

void executeClipContextMenu(const HWND window, AppState& state, const int item) {
    const auto clip = state.clipContextMenuClip;
    state.clipContextMenuOpen = false;
    state.clipContextMenuClip.clear();
    switch (item) {
    case 0:
        openEditor(window, state, clip);
        break;
    case 1:
        ShellExecuteW(window, L"open", clip.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case 2: {
        const std::wstring parameters = L"/select," + quoteProcessArgument(clip.wstring());
        ShellExecuteW(window, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case 3:
        copyPathToClipboard(window, clip);
        setStatus(window, state, L"Skopiowano ścieżkę klipu");
        break;
    case 4:
        if (MessageBoxW(
                window, L"Przenieść ten klip do Kosza?", L"NexPlay",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            std::wstring fileList = clip.wstring();
            fileList.push_back(L'\0');
            SHFILEOPSTRUCTW operation{};
            operation.hwnd = window;
            operation.wFunc = FO_DELETE;
            operation.pFrom = fileList.c_str();
            operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING;
            if (SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted) {
                state.thumbnailMemory.erase(clip);
                state.thumbnailDesiredFrame.erase(clip);
                std::erase_if(state.thumbnailFailures,
                              [&clip](const auto& failed) { return failed.first == clip; });
                state.thumbnailBitmaps.clear();
                refreshClips(window, state);
                setStatus(window, state, L"Klip przeniesiono do Kosza");
            }
        }
        break;
    default:
        break;
    }
}

void cancelHotkeyCapture(const HWND, AppState& state) {
    state.hotkeyCapture = HotkeyCapture::none;
}

void beginHotkeyCapture(
    const HWND window, AppState& state, const HotkeyCapture capture) {
    cancelHotkeyCapture(window, state);
    state.hotkeyCapture = capture;
    state.activeField = HitTarget::none;
    SetFocus(window);
    setStatus(window, state, L"Naciśnij nowy skrót — Esc anuluje");
}

void completeHotkeyCapture(
    const HWND window, AppState& state, const UINT virtualKey, const UINT modifiers) {
    const HotkeyCapture capture = state.hotkeyCapture;
    if (capture == HotkeyCapture::none) return;

    const bool duplicatesOther = capture == HotkeyCapture::save
        ? virtualKey == state.stopHotkeyVk && modifiers == state.stopHotkeyModifiers
        : virtualKey == state.saveHotkeyVk && modifiers == state.saveHotkeyModifiers;
    if (duplicatesOther) {
        state.hotkeyCapture = HotkeyCapture::none;
        setStatus(window, state, L"Ten skrót jest już używany w NexPlay");
        return;
    }

    if (capture == HotkeyCapture::save) {
        state.saveHotkeyVk = virtualKey;
        state.saveHotkeyModifiers = modifiers;
        writeSettingDword(L"SaveHotkeyVk", virtualKey);
        writeSettingDword(L"SaveHotkeyModifiers", modifiers);
    } else {
        state.stopHotkeyVk = virtualKey;
        state.stopHotkeyModifiers = modifiers;
        writeSettingDword(L"StopHotkeyVk", virtualKey);
        writeSettingDword(L"StopHotkeyModifiers", modifiers);
    }
    state.hotkeyCapture = HotkeyCapture::none;
    setStatus(window, state,
              L"Nowy skrót: " + hotkeyLabel(modifiers, virtualKey));
}

void updateAccentFromPointer(AppState& state, const float x, const float y) {
    const Rect accentPlane = fixedAspectRect(state.mainWindow, accentPlaneRect);
    const Rect accentHue = fixedAspectRect(state.mainWindow, accentHueRect);
    if (state.colorDrag == ColorDrag::plane) {
        state.accentSaturation = std::clamp(
            (x - accentPlane.left) /
                (accentPlane.right - accentPlane.left),
            0.0F, 1.0F);
        state.accentValue = 1.0F - std::clamp(
            (y - accentPlane.top) /
                (accentPlane.bottom - accentPlane.top),
            0.0F, 1.0F);
    } else if (state.colorDrag == ColorDrag::hue) {
        state.accentHue = std::clamp(
            (y - accentHue.top) /
                (accentHue.bottom - accentHue.top),
            0.0F, 0.9999F);
    }
    applyAccentColor(state);
}

void closeAudioGroupDialog(AppState& state) {
    state.audioGroupDialogOpen = false;
    state.audioGroupName.clear();
    state.activeField = HitTarget::none;
    state.replaceFieldOnInput = false;
}

void confirmAudioGroup(const HWND window, AppState& state) {
    if (!validAudioGroupName(state.audioGroupName) ||
        selectedAudioRowCount(state) < 2) return;
    const std::wstring groupName = state.audioGroupName;
    for (auto& row : state.audioRows) {
        if (!row.groupSelected) continue;
        row.groupName = groupName;
        row.groupSelected = false;
    }
    closeAudioGroupDialog(state);
    setStatus(window, state, L"Utworzono ścieżkę audio: " + groupName);
}

void handleClick(const HWND window, AppState& state, const float x, const float y) {
    const HitTarget clicked = hitTest(state, x, y);
    if (state.audioGroupDialogOpen && clicked == HitTarget::none) return;
    if (state.hotkeyCapture != HotkeyCapture::none &&
        clicked != HitTarget::saveHotkey && clicked != HitTarget::stopHotkey) {
        cancelHotkeyCapture(window, state);
    }
    switch (clicked) {
    case HitTarget::minimize:
    case HitTarget::close:
        if (state.page == Page::editor && state.mediaPlayer != nullptr) {
            state.mediaPlayer->Pause();
            state.previewAudio.pause();
            state.playing = false;
        }
        KillTimer(window, 1);
        ShowWindow(window, SW_HIDE);
        setStatus(window, state, L"NexPlay działa w zasobniku systemowym");
        return;
    case HitTarget::maximize:
        SendMessageW(window, WM_SYSCOMMAND,
                     IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE, 0);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::replayPage:
        closeEditorPlayer(state);
        state.page = Page::replay;
        updateEditorVisibility(state);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::clipsPage:
        closeEditorPlayer(state);
        state.page = Page::clips;
        updateEditorVisibility(state);
        refreshClips(window, state);
        return;
    case HitTarget::settingsPage:
        closeEditorPlayer(state);
        state.page = Page::settings;
        updateEditorVisibility(state);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::startStop:
        if (state.engine.isRunning()) stopRecorder(window, state); else startRecorder(window, state);
        return;
    case HitTarget::save:
        saveClip(window, state);
        return;
    case HitTarget::refreshAudio:
        try { refreshAudioApplications(window, state); }
        catch (...) { setStatus(window, state, L"Błąd odświeżania źródeł audio"); }
        return;
    case HitTarget::microphone:
        state.microphone = !state.microphone;
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::createAudioGroup: {
        const std::wstring existingGroup = selectedExistingAudioGroup(state);
        if (!existingGroup.empty()) {
            for (auto& row : state.audioRows) {
                if (row.groupSelected) {
                    row.groupName.clear();
                    row.groupSelected = false;
                }
            }
            setStatus(window, state, L"Rozłączono grupę audio: " + existingGroup);
        } else {
            state.audioGroupDialogOpen = true;
            state.audioGroupName.clear();
            state.activeField = HitTarget::audioGroupName;
            state.replaceFieldOnInput = false;
            SetFocus(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return;
    }
    case HitTarget::audioGroupName:
        state.activeField = HitTarget::audioGroupName;
        state.replaceFieldOnInput = false;
        SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::confirmAudioGroup:
        confirmAudioGroup(window, state);
        return;
    case HitTarget::cancelAudioGroup:
        closeAudioGroupDialog(state);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::durationField:
    case HitTarget::fpsField:
    case HitTarget::bitrateField:
        state.activeField = hitTest(state, x, y);
        state.replaceFieldOnInput = true;
        SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::resolutionField:
        state.resolutionPreset = (state.resolutionPreset + 1) % resolutionPresets.size();
        writeSettingDword(L"ResolutionPreset", static_cast<DWORD>(state.resolutionPreset));
        state.activeField = HitTarget::none;
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::openClips:
        openClipsFolder(window);
        return;
    case HitTarget::editorBack:
        closeEditorPlayer(state);
        state.page = Page::clips;
        state.activeField = HitTarget::none;
        refreshClips(window, state);
        return;
    case HitTarget::editorPlay:
        if (activeEditorPlayer(state) != nullptr) {
            toggleEditorPlayback(state);
            InvalidateRect(window, nullptr, FALSE);
        }
        return;
    case HitTarget::editorFullscreen:
        enterFullscreen(state);
        return;
    case HitTarget::editorMergeAudio:
        state.mergeEditorAudio = !state.mergeEditorAudio;
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::editorCutMode:
        state.cutEditorSelection = !state.cutEditorSelection;
        if (state.cutEditorSelection && state.editorDuration > 0.2 &&
            state.trimStart <= 0.001 &&
            state.trimEnd >= state.editorDuration - 0.001) {
            const double center = std::clamp(
                state.playPosition > 0.0 ? state.playPosition : state.editorDuration * 0.5,
                0.1, state.editorDuration - 0.1);
            const double halfWidth = std::min(1.0, state.editorDuration * 0.12);
            state.trimStart = std::max(0.0, center - halfWidth);
            state.trimEnd = std::min(state.editorDuration, center + halfWidth);
        }
        setStatus(window, state,
                  state.cutEditorSelection
                      ? L"Tryb wycinania: zaznacz fragment do usunięcia"
                      : L"Tryb przycinania początku i końca klipu");
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::editorSave:
        exportEditor(window, state);
        return;
    case HitTarget::editorName:
        state.activeField = HitTarget::editorName;
        state.replaceFieldOnInput = true;
        SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::autostartToggle: {
        const bool requested = !state.autostart;
        if (setAutostartEnabled(requested)) {
            state.autostart = requested;
            setStatus(window, state,
                      requested ? L"Autostart Windows został włączony"
                                : L"Autostart Windows został wyłączony");
        } else {
            setStatus(window, state, L"Błąd: nie udało się zmienić autostartu");
        }
        return;
    }
    case HitTarget::autoBufferToggle:
        state.autoBuffer = !state.autoBuffer;
        writeSettingDword(L"AutoBuffer", state.autoBuffer ? 1 : 0);
        setStatus(window, state,
                  state.autoBuffer ? L"Automatyczny bufor został włączony"
                                   : L"Automatyczny bufor został wyłączony");
        return;
    case HitTarget::saveHotkey:
        beginHotkeyCapture(window, state, HotkeyCapture::save);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::saveHotkeyToggle:
    case HitTarget::stopHotkeyToggle: {
        const bool save = clicked == HitTarget::saveHotkeyToggle;
        bool& enabled = save ? state.saveHotkeyEnabled : state.stopHotkeyEnabled;
        enabled = !enabled;
        writeSettingDword(save ? L"SaveHotkeyEnabled" : L"StopHotkeyEnabled", enabled ? 1 : 0);
        setStatus(window, state,
                  enabled ? L"Skrót włączony"
                          : L"Skrót wyłączony — przyciski aplikacji nadal działają");
        return;
    }
    case HitTarget::toastTopLeft:
    case HitTarget::toastTopRight:
    case HitTarget::toastBottomLeft:
    case HitTarget::toastBottomRight:
        state.toastCorner = static_cast<nexplay::ui::ToastCorner>(
            static_cast<int>(clicked) - static_cast<int>(HitTarget::toastTopLeft));
        writeSettingDword(L"ToastCorner", static_cast<DWORD>(state.toastCorner));
        state.saveToasts.configure(state.toastCorner, primary);
        setStatus(window, state, L"Zmieniono róg powiadomień zapisu");
        return;
    case HitTarget::stopHotkey:
        beginHotkeyCapture(window, state, HotkeyCapture::stop);
        InvalidateRect(window, nullptr, FALSE);
        return;
    case HitTarget::accentPlane:
    case HitTarget::accentHue:
        return;
    default:
        state.activeField = HitTarget::none;
        InvalidateRect(window, nullptr, FALSE);
        break;
    }

    if (state.page == Page::editor &&
        x >= editorAudioRowsRect.left && x < editorTimelineRect.left &&
        y >= editorAudioRowsRect.top && y < editorAudioRowsRect.top+editorVisibleTracks*editorAudioRowHeight) {
        const int index = static_cast<int>((y-editorAudioRowsRect.top)/editorAudioRowHeight) + state.editorAudioScroll;
        if (index >= 0 && index < static_cast<int>(state.editorAudioTracks.size())) {
            auto& track = state.editorAudioTracks[static_cast<std::size_t>(index)];
            if (editorAudioDeleteRect(index - state.editorAudioScroll).contains(x, y)) {
                const auto name = track.name;
                removeEditorAudioTrack(state, static_cast<std::size_t>(index));
                setStatus(window, state, L"Usunięto ścieżkę: " + name + L" · Ctrl+Z: przywróć. Oryginał bez zmian.");
            } else if (x < contentLeft + 44) {
                track.included = !track.included;
            }
            updatePreviewAudio(state);
            InvalidateRect(window, nullptr, FALSE);
        }
    } else if (state.page == Page::replay && !state.engine.isRunning() &&
        audioRowsRect.contains(x,y) && y < audioRowsRect.top+audioVisibleRows*audioRowHeight) {
        const int index = static_cast<int>((y-audioRowsRect.top)/audioRowHeight) + state.audioScroll;
        if (index >= 0 && index < static_cast<int>(state.audioRows.size())) {
            auto& row = state.audioRows[static_cast<std::size_t>(index)];
            if (x >= audioRowsRect.right-54) {
                if (!row.groupName.empty()) {
                    const bool select = !row.groupSelected;
                    for (auto& candidate : state.audioRows) {
                        if (candidate.groupName == row.groupName) {
                            candidate.groupSelected = select;
                        }
                    }
                } else {
                    row.groupSelected = !row.groupSelected;
                }
            } else {
                row.included = !row.included;
            }
            InvalidateRect(window, nullptr, FALSE);
        }
    } else if (const int index = clipIndexAt(state, x, y); index >= 0) {
        openEditor(window, state, state.clips[static_cast<std::size_t>(index)].path);
    }
}

[[nodiscard]] std::wstring* activeFieldText(AppState& state) {
    switch (state.activeField) {
    case HitTarget::durationField: return &state.durationText;
    case HitTarget::fpsField: return &state.fpsText;
    case HitTarget::bitrateField: return &state.bitrateText;
    case HitTarget::editorName: return &state.editorName;
    case HitTarget::audioGroupName: return &state.audioGroupName;
    default: return nullptr;
    }
}

LRESULT CALLBACK windowProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE: {
        state->mainWindow = window;
        updateLayout(window);
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &state->clientAnimations, 0);
        removeLegacyThumbnailCache();
        loadPersistentSettings(*state);
        const int darkMode = TRUE;
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
        const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        state->videoWindow = CreateWindowExW(
            0, videoWindowClassName, L"", WS_CHILD,
            284, 159, 752, 407, window, nullptr, GetModuleHandleW(nullptr), window);
        state->tray.cbSize = sizeof(state->tray);
        state->tray.hWnd = window;
        state->tray.uID = 1;
        state->tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        state->tray.uCallbackMessage = trayMessage;
        state->tray.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_NEXPLAY), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        wcscpy_s(state->tray.szTip, L"NexPlay — gotowy");
        Shell_NotifyIconW(NIM_ADD, &state->tray);
        keyboardHookWindow = window;
        keyboardHookState = state;
        keyboardKeysDown.fill(false);
        keyboardHook = SetWindowsHookExW(
            WH_KEYBOARD_LL, passiveKeyboardProcedure, GetModuleHandleW(nullptr), 0);
        SetTimer(window, 1, 16, nullptr);
        try { refreshAudioApplications(window, *state); }
        catch (...) { state->status = L"Nie udało się odczytać źródeł audio"; }
        if (keyboardHook == nullptr) {
            state->status = L"Nie udało się uruchomić obsługi skrótów klawiaturowych";
        }
        try { refreshClips(window, *state); }
        catch (...) { }
        if (state->autoBuffer) PostMessageW(window, autoStartMessage, 0, 0);
        return 0;
    }
    case WM_PAINT:
        if (state != nullptr) paint(window, *state);
        return 0;
    case WM_SETTINGCHANGE:
        if (state != nullptr) {
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &state->clientAnimations, 0);
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        // All caption controls are client-drawn; do not let the invisible native
        // caption claim clicks meant for our minimize/maximize/close buttons.
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        if (!IsZoomed(window)) {
            RECT client{};
            GetClientRect(window, &client);
            const int edge = GetSystemMetrics(SM_CXSIZEFRAME) +
                GetSystemMetrics(SM_CXPADDEDBORDER);
            const bool left = point.x < edge, right = point.x >= client.right - edge;
            const bool top = point.y < edge, bottom = point.y >= client.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        const auto logical = designPoint(
            window, static_cast<float>(point.x), static_cast<float>(point.y));
        if (logical.y < 44 &&
            !minimizeRect.contains(logical.x, logical.y) &&
            !maximizeRect.contains(logical.x, logical.y) &&
            !closeRect.contains(logical.x, logical.y)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (state != nullptr && LOWORD(lParam) == HTCLIENT) {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(window, &cursor);
            const auto logical = designPoint(
                window, static_cast<float>(cursor.x), static_cast<float>(cursor.y));
            const float x = logical.x;
            const float y = logical.y;
            const bool audioTimeline = state->page == Page::editor &&
                x >= editorTimelineRect.left && x <= editorTimelineRect.right &&
                y >= editorAudioRowsRect.top &&
                y < editorAudioRowsRect.top+editorVisibleTracks*editorAudioRowHeight;
            const bool row =
                (state->page == Page::replay && !state->engine.isRunning() &&
                 audioRowsRect.contains(x,y)) ||
                (state->page == Page::clips && clipsListRect.contains(x, y)) ||
                (state->page == Page::editor &&
                 x >= editorAudioRowsRect.left && x < editorTimelineRect.left &&
                 y >= editorAudioRowsRect.top && y < editorAudioRowsRect.bottom);
            const HitTarget target = hitTest(*state, x, y);
            if (target == HitTarget::durationField || target == HitTarget::fpsField ||
                target == HitTarget::bitrateField ||
                target == HitTarget::audioGroupName) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            if (target == HitTarget::accentPlane || target == HitTarget::accentHue) {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                return TRUE;
            }
            if (audioTimeline) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            if (target != HitTarget::none || row) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (state != nullptr) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            const auto logical = designPoint(
                window, static_cast<float>(GET_X_LPARAM(lParam)),
                static_cast<float>(GET_Y_LPARAM(lParam)));
            state->mouseX = logical.x;
            state->mouseY = logical.y;
            if (state->colorDrag != ColorDrag::none) {
                updateAccentFromPointer(*state, state->mouseX, state->mouseY);
            }
            if (state->dragHandle != DragHandle::none) {
                if (state->dragHandle == DragHandle::audioStart ||
                    state->dragHandle == DragHandle::audioEnd) {
                    moveEditorAudioHandle(*state, state->mouseX);
                } else {
                    moveTrimHandle(*state, state->mouseX);
                }
            }
            const auto target = hitTest(*state, state->mouseX, state->mouseY);
            if (target != state->hover) {
                state->hover = target;
                InvalidateRect(window, nullptr, FALSE);
            }
            if (state->page == Page::clips) InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_RBUTTONUP:
        if (state != nullptr && state->page == Page::clips) {
            const auto logical = designPoint(
                window, static_cast<float>(GET_X_LPARAM(lParam)),
                static_cast<float>(GET_Y_LPARAM(lParam)));
            const float x = logical.x;
            const float y = logical.y;
            if (const int index = clipIndexAt(*state, x, y); index >= 0) {
                openClipContextMenu(
                    *state, state->clips[static_cast<std::size_t>(index)].path, x, y);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            state->clipContextMenuOpen = false;
            state->clipContextMenuClip.clear();
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN: {
        const auto logical = designPoint(
            window, static_cast<float>(GET_X_LPARAM(lParam)),
            static_cast<float>(GET_Y_LPARAM(lParam)));
        if (state != nullptr && state->page == Page::settings) {
            const float x = logical.x;
            const float y = logical.y;
            const Rect accentPlane = fixedAspectRect(window, accentPlaneRect);
            const Rect accentHue = fixedAspectRect(window, accentHueRect);
            if (accentPlane.contains(x, y) || accentHue.contains(x, y)) {
                state->colorDrag = accentPlane.contains(x, y)
                    ? ColorDrag::plane : ColorDrag::hue;
                cancelHotkeyCapture(window, *state);
                SetCapture(window);
                updateAccentFromPointer(*state, x, y);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        if (state != nullptr && state->page == Page::editor &&
            state->editorDuration > 0.0 &&
            logical.x >= editorTimelineRect.left &&
            logical.x <= editorTimelineRect.right &&
            logical.y >= editorAudioRowsRect.top &&
            logical.y < editorAudioRowsRect.top+editorVisibleTracks*editorAudioRowHeight) {
            const int visible = static_cast<int>((logical.y-editorAudioRowsRect.top)/editorAudioRowHeight);
            const int index = visible + state->editorAudioScroll;
            if (index >= 0 && index < static_cast<int>(state->editorAudioTracks.size()) &&
                state->editorAudioTracks[static_cast<std::size_t>(index)].included) {
                const Rect timeline = editorAudioTimelineRect(visible);
                const auto& track = state->editorAudioTracks[static_cast<std::size_t>(index)];
                const auto bounds = editorAudioBounds(*state, track);
                const float startX = timeline.left +
                    static_cast<float>(bounds.start / state->editorDuration) *
                        (timeline.right - timeline.left);
                const float endX = timeline.left +
                    static_cast<float>(bounds.end / state->editorDuration) *
                        (timeline.right - timeline.left);
                const float clickX = logical.x;
                const bool grabStart=bounds.duration()>0 && std::abs(clickX-startX)<=10;
                const bool grabEnd=bounds.duration()>0 && std::abs(clickX-endX)<=10;
                state->activeEditorAudioTrack = grabStart||grabEnd ? index : -1;
                state->dragHandle = grabStart ? DragHandle::audioStart :
                    (grabEnd ? DragHandle::audioEnd : DragHandle::playhead);
                if (IMFPMediaPlayer* player = activeEditorPlayer(*state); player != nullptr) {
                    player->Pause();
                }
                state->playing = false;
                SetCapture(window);
                if (state->dragHandle==DragHandle::playhead) moveTrimHandle(*state, clickX);
                else moveEditorAudioHandle(*state, clickX);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        if (state != nullptr && state->page == Page::editor &&
            state->editorDuration > 0.0 &&
            logical.x >= editorTimelineRect.left - 10 &&
            logical.x <= editorTimelineRect.right + 10 &&
            logical.y >= editorTimelineRect.top - 32 &&
            logical.y <= editorTimelineRect.bottom + 10) {
            const float timelineWidth = editorTimelineRect.right - editorTimelineRect.left;
            const float startX = editorTimelineRect.left +
                static_cast<float>(state->trimStart / state->editorDuration) * timelineWidth;
            const float endX = editorTimelineRect.left +
                static_cast<float>(state->trimEnd / state->editorDuration) * timelineWidth;
            const float clickX = logical.x;
            constexpr float handleGrabRadius = 13.0F;
            const bool onVideoTrack = logical.y >= editorTimelineRect.top &&
                logical.y <= editorTimelineRect.bottom;
            if (onVideoTrack && std::abs(clickX - startX) <= handleGrabRadius) {
                state->dragHandle = DragHandle::start;
            } else if (onVideoTrack && std::abs(clickX - endX) <= handleGrabRadius) {
                state->dragHandle = DragHandle::end;
            } else {
                state->dragHandle = DragHandle::playhead;
            }
            if (state->mediaPlayer != nullptr) state->mediaPlayer->Pause();
            state->playing = false;
            SetCapture(window);
            moveTrimHandle(*state, logical.x);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (state != nullptr) {
            state->hover = HitTarget::none;
            state->mouseX = -1;
            state->mouseY = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (state != nullptr) {
            const auto logical = designPoint(
                window, static_cast<float>(GET_X_LPARAM(lParam)),
                static_cast<float>(GET_Y_LPARAM(lParam)));
            if (state->colorDrag != ColorDrag::none) {
                updateAccentFromPointer(
                    *state, logical.x, logical.y);
                state->colorDrag = ColorDrag::none;
                ReleaseCapture();
                saveAccentColor(*state);
                setStatus(window, *state,
                          L"Kolor akcentu: " + accentHexLabel(*state));
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (state->clipContextMenuOpen) {
                const float x = logical.x;
                const float y = logical.y;
                int selected = -1;
                for (int item = 0; item < 5; ++item) {
                    if (clipMenuItemRect(*state, item).contains(x, y)) {
                        selected = item;
                        break;
                    }
                }
                if (selected >= 0) executeClipContextMenu(window, *state, selected);
                else {
                    state->clipContextMenuOpen = false;
                    state->clipContextMenuClip.clear();
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (state->dragHandle != DragHandle::none) {
                if (state->dragHandle == DragHandle::audioStart ||
                    state->dragHandle == DragHandle::audioEnd) {
                    moveEditorAudioHandle(*state, logical.x);
                } else {
                    moveTrimHandle(*state, logical.x);
                }
                state->dragHandle = DragHandle::none;
                state->activeEditorAudioTrack = -1;
                ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            handleClick(window, *state, logical.x, logical.y);
        }
        return 0;
    }
    case WM_CHAR:
        if (state != nullptr &&
            (!state->engine.isRunning() || state->activeField == HitTarget::editorName)) {
            if (auto* text = activeFieldText(*state); text != nullptr) {
                const bool nameField = state->activeField == HitTarget::editorName ||
                    state->activeField == HitTarget::audioGroupName;
                const bool validNameCharacter = nameField && wParam >= 32 &&
                    std::wstring(L"<>:\"/\\|?*").find(static_cast<wchar_t>(wParam)) == std::wstring::npos;
                if ((nameField && validNameCharacter) ||
                    (!nameField && wParam >= L'0' && wParam <= L'9')) {
                    if (state->replaceFieldOnInput) {
                        text->clear();
                        state->replaceFieldOnInput = false;
                    }
                    const std::size_t maximumLength = state->activeField == HitTarget::audioGroupName
                        ? 48 : (nameField ? 100 : 4);
                    if (text->size() < maximumLength) text->push_back(static_cast<wchar_t>(wParam));
                } else if (wParam == L'\b') {
                    state->replaceFieldOnInput = false;
                    if (!text->empty()) text->pop_back();
                } else if (wParam == L'\r') {
                    if (state->audioGroupDialogOpen) confirmAudioGroup(window, *state);
                    else state->activeField = HitTarget::none;
                } else if (wParam == L'\t') {
                    state->activeField = HitTarget::none;
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        break;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if (state != nullptr) {
            if (state->hotkeyCapture != HotkeyCapture::none) {
                if (wParam == VK_ESCAPE) {
                    cancelHotkeyCapture(window, *state);
                    setStatus(window, *state, L"Zmiana skrótu anulowana");
                } else if (!isModifierKey(wParam)) {
                    completeHotkeyCapture(
                        window, *state, static_cast<UINT>(wParam),
                        pressedHotkeyModifiers());
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (state->audioGroupDialogOpen) {
                if (wParam == VK_ESCAPE) {
                    closeAudioGroupDialog(*state);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (wParam == VK_RETURN) {
                    confirmAudioGroup(window, *state);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
            }
            if (state->page == Page::editor &&
                state->activeField != HitTarget::editorName) {
                if (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    if (!state->removedEditorAudioTracks.empty()) {
                        restoreEditorAudioTrack(*state);
                        setStatus(window, *state, L"Przywrócono ostatnio usuniętą ścieżkę audio");
                        InvalidateRect(window, nullptr, FALSE);
                    }
                    return 0;
                }
                if (wParam == VK_SPACE) {
                    toggleEditorPlayback(*state);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                    seekEditorBy(*state, wParam == VK_LEFT ? -5.0 : 5.0);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (wParam == VK_HOME || wParam == VK_END) {
                    seekEditor(*state, wParam == VK_HOME ? 0.0 : state->editorDuration);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
            }
            if (wParam == VK_ESCAPE) {
                state->clipContextMenuOpen = false;
                state->clipContextMenuClip.clear();
                state->activeField = HitTarget::none;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        break;
    case WM_MOUSEWHEEL:
        if (state != nullptr) {
            const int direction = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1;
            if (state->page == Page::replay) state->audioScroll += direction;
            else if (state->page == Page::editor) {
                state->editorAudioScroll += direction;
            } else state->clipScroll += direction;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (state != nullptr && wParam == 1) {
            const float speed = state->clientAnimations ? 0.22F : 1.0F;
            for (std::size_t index = 1; index < state->hoverAnimation.size(); ++index) {
                const float target = index == static_cast<std::size_t>(state->hover) ? 1.0F : 0.0F;
                state->hoverAnimation[index] +=
                    (target - state->hoverAnimation[index]) * speed;
            }
            const float microphoneTarget = state->microphone ? 1.0F : 0.0F;
            state->microphoneAnimation +=
                (microphoneTarget - state->microphoneAnimation) * speed;
            const float autostartTarget = state->autostart ? 1.0F : 0.0F;
            state->autostartAnimation +=
                (autostartTarget - state->autostartAnimation) * speed;
            const float autoBufferTarget = state->autoBuffer ? 1.0F : 0.0F;
            state->autoBufferAnimation +=
                (autoBufferTarget - state->autoBufferAnimation) * speed;
            if (state->page == Page::editor) {
                updateEditorPlayback(*state);
                if (!state->fullscreen && state->mediaPlayer != nullptr &&
                    state->videoWindow != nullptr &&
                    state->embeddedVideoRefreshFrames > 0) {
                    --state->embeddedVideoRefreshFrames;
                    if (state->embeddedVideoRefreshFrames % 6 == 0 ||
                        state->embeddedVideoRefreshFrames < 4) {
                        InvalidateRect(state->videoWindow, nullptr, FALSE);
                    }
                }
            }
            if (state->fullscreenWindow != nullptr) {
                InvalidateRect(state->fullscreenWindow, nullptr, FALSE);
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case autoStartMessage:
        if (state != nullptr && state->autoBuffer && !state->engine.isRunning()) {
            startRecorder(window, *state);
        }
        return 0;
    case exitFullscreenMessage:
        if (state != nullptr) {
            exitFullscreen(*state);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case thumbnailReadyMessage:
        if (state != nullptr) {
            std::unique_ptr<ThumbnailResult> result(
                reinterpret_cast<ThumbnailResult*>(lParam));
            state->thumbnailsPending.erase({result->clip, result->frameIndex});
            state->thumbnailClipActive.erase(result->clip);
            const bool ready = std::filesystem::exists(result->clip) &&
                result->duration > 0.0 && result->framesPerSecond > 0.0 &&
                !result->frames.empty();
            auto& preview = state->thumbnailMemory[result->clip];
            if (preview == nullptr) preview = std::make_shared<ClipPreview>();
            if (result->duration > 0.0) preview->duration = result->duration;
            if (result->framesPerSecond > 0.0) {
                preview->framesPerSecond = result->framesPerSecond;
            }
            if (ready) {
                for (std::size_t index = 0; index < result->frames.size(); ++index) {
                    const int decodedFrame = result->firstFrameIndex +
                        static_cast<int>(index);
                    if (!result->frames[index].empty()) {
                        preview->frames[decodedFrame] = std::move(result->frames[index]);
                        state->thumbnailFailures.erase({result->clip, decodedFrame});
                    }
                }
            } else {
                state->thumbnailFailures.insert({result->clip, result->frameIndex});
            }
            const auto clip = std::ranges::find_if(
                state->clips,
                [&result](const ClipRow& candidate) {
                    return candidate.path == result->clip;
                });
            if (clip != state->clips.end()) clip->preview = preview;
            if (const auto desired = state->thumbnailDesiredFrame.find(result->clip);
                desired != state->thumbnailDesiredFrame.end() &&
                desired->second != result->frameIndex) {
                requestThumbnail(window, *state, result->clip, desired->second);
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case clearEditorFocusMessage:
        if (state != nullptr) {
            state->activeField = HitTarget::none;
            state->replaceFieldOnInput = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case togglePlaybackMessage:
        if (state != nullptr && state->page == Page::editor &&
            activeEditorPlayer(*state) != nullptr) {
            toggleEditorPlayback(*state);
            InvalidateRect(window, nullptr, FALSE);
            if (state->fullscreenWindow != nullptr) {
                InvalidateRect(state->fullscreenWindow, nullptr, FALSE);
            }
        }
        return 0;
    case WM_HOTKEY:
        if (state != nullptr && state->saveHotkeyEnabled && wParam == saveHotkeyId)
            saveClip(window, *state);
        else if (state != nullptr && state->stopHotkeyEnabled && wParam == stopHotkeyId &&
                 state->engine.isRunning()) {
            stopRecorder(window, *state);
        }
        return 0;
    case saveProgressMessage: {
        std::unique_ptr<nexplay::app::SaveProgress> progress(
            reinterpret_cast<nexplay::app::SaveProgress*>(lParam));
        if (state && progress) {
            try {
                state->saveToasts.configure(state->toastCorner, primary);
                state->saveToasts.update(std::move(*progress));
            } catch (...) {
            }
        }
        return 0;
    }
    case statusMessage:
        if (state != nullptr) {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            const bool clipSaved = text->starts_with(L"Klip zapisany:");
            setStatus(window, *state, std::move(*text));
            updateEditorVisibility(*state);
            if (clipSaved || state->page == Page::clips) refreshClips(window, *state);
        }
        return 0;
    case editorProgressMessage:
        if (state && state->editorExporting && wParam == state->editorExportId) {
            // 100% is reserved for the successful completion/file-finalization event.
            state->editorExportPercent = std::max(state->editorExportPercent,
                std::clamp(static_cast<int>(lParam), 0, 99));
            setStatus(window, *state, L"Eksport GPU · " +
                std::to_wstring(state->editorExportPercent) + L"% · " +
                state->editorExportClip.filename().wstring());
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case editorDoneMessage: {
        std::unique_ptr<EditorResult> result(reinterpret_cast<EditorResult*>(lParam));
        if (state && result && result->exportId == state->editorExportId) {
            state->editorExporting = false;
            state->editorJobs.clear();
            if (result->success) {
                state->editorExportPercent = 100;
                if (state->page == Page::editor && state->selectedClip == state->editorExportClip) {
                    closeEditorPlayer(*state);
                    state->page = Page::clips;
                }
                refreshClips(window, *state);
                setStatus(window, *state, L"Eksport 100% · Zapisano: " + result->output.filename().wstring());
            } else {
                setStatus(window, *state, L"Błąd: " + result->message);
            }
        }
        return 0;
    }
    case trayMessage:
        if (state != nullptr && (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK)) {
            if (state->trayMenuWindow != nullptr) ShowWindow(state->trayMenuWindow, SW_HIDE);
            SetTimer(window, 1, 16, nullptr);
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        } else if (state != nullptr && lParam == WM_RBUTTONUP) {
            showTrayMenu(window, *state);
        }
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        updateLayout(window);
        if (state != nullptr && state->windowRenderTarget != nullptr && LOWORD(lParam) > 0 && HIWORD(lParam) > 0) {
            state->windowRenderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        if (state != nullptr && LOWORD(lParam) > 0 && HIWORD(lParam) > 0) {
            updateEditorVisibility(*state);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (auto* limits = reinterpret_cast<MINMAXINFO*>(lParam); limits != nullptr) {
            limits->ptMinTrackSize.x = 1080;
            limits->ptMinTrackSize.y = 740;
            MONITORINFO monitorInfo{sizeof(monitorInfo)};
            const HMONITOR monitor = MonitorFromWindow(
                window, MONITOR_DEFAULTTONEAREST);
            if (GetMonitorInfoW(monitor, &monitorInfo)) {
                const RECT& monitorArea = monitorInfo.rcMonitor;
                const RECT& workArea = monitorInfo.rcWork;
                limits->ptMaxPosition.x = workArea.left - monitorArea.left;
                limits->ptMaxPosition.y = workArea.top - monitorArea.top;
                limits->ptMaxSize.x = workArea.right - workArea.left;
                limits->ptMaxSize.y = workArea.bottom - workArea.top;
            }
        }
        return 0;
    case WM_NCCALCSIZE:
        // Keep WS_CAPTION for DWM transitions; draw our own client-side titlebar.
        // FALSE is used during initial creation too, not just TRUE during resize.
        return 0;
    case WM_NCACTIVATE:
        // Update activation without asking Windows to repaint a standard caption.
        return DefWindowProcW(window, message, wParam, -1);
    case activateInstanceMessage:
        SetTimer(window, 1, 16, nullptr);
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window);
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
            saveAccentColor(*state);
            closeEditorPlayer(*state);
            state->engine.stop();
            // Finish owned exports before the HWND can be recycled; release queued results.
            for (auto& job : state->editorJobs) if (job.joinable()) job.join();
            state->editorJobs.clear();
            MSG pendingEditor{};
            while (PeekMessageW(&pendingEditor, window, editorDoneMessage, editorDoneMessage, PM_REMOVE))
                delete reinterpret_cast<EditorResult*>(pendingEditor.lParam);
            state->saveToasts.close();
            MSG pendingSave{};
            while (PeekMessageW(&pendingSave, window, saveProgressMessage, saveProgressMessage,
                                PM_REMOVE))
                delete reinterpret_cast<nexplay::app::SaveProgress*>(pendingSave.lParam);
            KillTimer(window, 1);
            if (state->trayMenuWindow != nullptr) {
                DestroyWindow(state->trayMenuWindow);
                state->trayMenuWindow = nullptr;
            }
            if (keyboardHook != nullptr) {
                UnhookWindowsHookEx(keyboardHook);
                keyboardHook = nullptr;
            }
            keyboardHookWindow = nullptr;
            keyboardHookState = nullptr;
            keyboardKeysDown.fill(false);
            Shell_NotifyIconW(NIM_DELETE, &state->tray);
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    // Package smoke test: no UI, settings, hooks, recording or microphone access.
    if (commandLine && wcscmp(commandLine, L"--verify-installation") == 0) {
        const auto directory = nexplay::platform::applicationDirectory();
        if (!FindResourceW(instance, MAKEINTRESOURCEW(IDI_NEXPLAY), RT_GROUP_ICON)) return 2;
        for (const auto* name : {L"ffmpeg.exe", L"ffprobe.exe"}) {
            const auto tool = directory / L"tools" / L"ffmpeg" / L"bin" / name;
            if (!nexplay::platform::isToolFile(tool) ||
                nexplay::platform::resolveMediaTool(name) != tool ||
                runHiddenProcess({name, L"-version"}) != 0) return 3;
        }
        return 0;
    }
    struct InstanceGuard {
        HANDLE mutex{};
        ~InstanceGuard() { if (mutex) CloseHandle(mutex); }
    } guard{CreateMutexW(nullptr, FALSE, L"Local\\NexPlay.Application")};
    const auto instanceError = GetLastError();
    if (!guard.mutex) return 1;
    if (instanceError == ERROR_ALREADY_EXISTS) {
        if (const HWND existing = FindWindowW(windowClassName, nullptr))
            PostMessageW(existing, activateInstanceMessage, 0, 0);
        return 0;
    }
    SetCurrentProcessExplicitAppUserModelID(L"Zimonxx.NexPlay");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_NEXPLAY));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = windowClassName;
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_NEXPLAY),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    RegisterClassExW(&windowClass);

    WNDCLASSEXW videoClass{sizeof(videoClass)};
    videoClass.lpfnWndProc = videoWindowProcedure;
    videoClass.hInstance = instance;
    videoClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    videoClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    videoClass.lpszClassName = videoWindowClassName;
    RegisterClassExW(&videoClass);

    WNDCLASSEXW fullscreenClass{sizeof(fullscreenClass)};
    fullscreenClass.style = CS_HREDRAW | CS_VREDRAW;
    fullscreenClass.lpfnWndProc = fullscreenWindowProcedure;
    fullscreenClass.hInstance = instance;
    fullscreenClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    fullscreenClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    fullscreenClass.lpszClassName = fullscreenWindowClassName;
    RegisterClassExW(&fullscreenClass);

    WNDCLASSEXW trayMenuClass{sizeof(trayMenuClass)};
    trayMenuClass.style = CS_HREDRAW | CS_VREDRAW;
    trayMenuClass.lpfnWndProc = trayMenuWindowProcedure;
    trayMenuClass.hInstance = instance;
    trayMenuClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    trayMenuClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    trayMenuClass.lpszClassName = trayMenuWindowClassName;
    RegisterClassExW(&trayMenuClass);

    AppState state;
    // Our client rectangle is the whole window, so no native caption padding.
    const int initialWidth = static_cast<int>(windowWidth);
    const int initialHeight = static_cast<int>(windowHeight);
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - initialWidth) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - initialHeight) / 2);
    const HWND window = CreateWindowExW(
        mainWindowExStyle, windowClassName, L"NexPlay", mainWindowStyle,
        x, y, initialWidth, initialHeight,
        nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        CoUninitialize();
        return 1;
    }
    initializeCustomFrame(window);
    const bool launchedAtStartup =
        commandLine != nullptr && wcsstr(commandLine, L"--autostart") != nullptr;
    if (launchedAtStartup) {
        KillTimer(window, 1);
        ShowWindow(window, SW_HIDE);
    } else {
        ShowWindow(window, showCommand);
        UpdateWindow(window);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
