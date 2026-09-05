#include "app/RecorderEngine.h"
#include "audio/AudioSessionScanner.h"

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
constexpr UINT_PTR videoClickTimerId = 1;
constexpr int saveHotkeyId = 1;
constexpr int stopHotkeyId = 2;
constexpr float windowWidth = 1120.0F;
constexpr float windowHeight = 800.0F;
constexpr float trayMenuWidth = 310.0F;
constexpr float trayMenuHeight = 238.0F;

constexpr D2D1_COLOR_F background = {0.0F, 0.0F, 0.0F, 1.0F};
constexpr D2D1_COLOR_F sidebar = {0.012F, 0.012F, 0.018F, 1.0F};
constexpr D2D1_COLOR_F card = {0.026F, 0.027F, 0.039F, 1.0F};
constexpr D2D1_COLOR_F field = {0.043F, 0.045F, 0.063F, 1.0F};
constexpr D2D1_COLOR_F border = {0.100F, 0.106F, 0.145F, 1.0F};
D2D1_COLOR_F primary = {0.435F, 0.259F, 1.0F, 1.0F};
D2D1_COLOR_F primaryHover = {0.565F, 0.400F, 1.0F, 1.0F};
D2D1_COLOR_F accentSecondary = {0.176F, 0.651F, 1.0F, 1.0F};
constexpr D2D1_COLOR_F white = {0.965F, 0.969F, 0.988F, 1.0F};
constexpr D2D1_COLOR_F muted = {0.488F, 0.506F, 0.588F, 1.0F};
constexpr D2D1_COLOR_F green = {0.173F, 0.949F, 0.616F, 1.0F};
constexpr D2D1_COLOR_F red = {1.0F, 0.286F, 0.376F, 1.0F};

enum class Page { replay, clips, editor, settings };
enum class HitTarget {
    none,
    minimize,
    close,
    replayPage,
    clipsPage,
    startStop,
    save,
    refreshAudio,
    microphone,
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
    settingsPage,
    autostartToggle,
    autoBufferToggle,
    saveHotkey,
    stopHotkey,
    accentPlane,
    accentHue,
    count,
};

enum class DragHandle { none, start, end, playhead };
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

constexpr Rect minimizeRect{1032, 15, 1066, 49};
constexpr Rect closeRect{1070, 15, 1104, 49};
constexpr Rect replayNavRect{16, 112, 204, 160};
constexpr Rect clipsNavRect{16, 168, 204, 216};
constexpr Rect settingsNavRect{16, 224, 204, 272};
constexpr Rect startRect{284, 222, 458, 262};
constexpr Rect saveRect{470, 222, 644, 262};
constexpr Rect refreshRect{967, 508, 1050, 542};
constexpr Rect microphoneRect{760, 508, 946, 542};
constexpr Rect openClipsRect{900, 91, 1060, 129};
constexpr Rect clipsListRect{260, 162, 1060, 746};
constexpr Rect durationFieldRect{284, 405, 464, 455};
constexpr Rect resolutionFieldRect{478, 405, 680, 455};
constexpr Rect fpsFieldRect{694, 405, 824, 455};
constexpr Rect bitrateFieldRect{838, 405, 1036, 455};
constexpr Rect editorBackRect{260, 91, 344, 129};
constexpr Rect editorPlayRect{284, 721, 334, 761};
constexpr Rect editorFullscreenRect{346, 721, 486, 761};
constexpr Rect editorSaveRect{846, 721, 1036, 761};
constexpr Rect editorNameRect{284, 604, 654, 648};
constexpr Rect editorTimelineRect{284, 682, 1036, 700};
constexpr Rect autostartRect{280, 210, 1040, 264};
constexpr Rect autoBufferRect{280, 270, 1040, 324};
constexpr Rect saveHotkeyRect{280, 410, 650, 466};
constexpr Rect stopHotkeyRect{670, 410, 1040, 466};
constexpr Rect accentPlaneRect{292, 590, 800, 684};
constexpr Rect accentHueRect{818, 590, 846, 684};
constexpr Rect accentPreviewRect{870, 590, 1028, 684};

struct AudioRow final {
    DWORD processId{};
    std::wstring name;
    bool included{true};
};

struct ClipPreview final {
    double duration{};
    double framesPerSecond{};
    std::map<int, std::vector<std::uint8_t>> frames;
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
    DragHandle dragHandle{DragHandle::none};
    bool playing{};
    bool fullscreen{};
    bool fullscreenScrubbing{};
    bool clipContextMenuOpen{};
    std::filesystem::path clipContextMenuClip;
    Rect clipContextMenuRect{};
    bool autostart{};
    bool autoBuffer{};
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
    ComPtr<ID2D1HwndRenderTarget> renderTarget;
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
        if (RemovePropW(window, L"NexPlaySuppressSingleClick") != nullptr) {
            return 0;
        }
        SetTimer(window, videoClickTimerId, GetDoubleClickTime(), nullptr);
        return 0;
    }
    if (message == WM_TIMER && wParam == videoClickTimerId) {
        KillTimer(window, videoClickTimerId);
        if (owner != nullptr) PostMessageW(owner, togglePlaybackMessage, 0, 0);
        return 0;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        if (owner != nullptr) PostMessageW(owner, exitFullscreenMessage, 0, 0);
        return 0;
    }
    if (message == WM_LBUTTONDBLCLK) {
        KillTimer(window, videoClickTimerId);
        SetPropW(window, L"NexPlaySuppressSingleClick", reinterpret_cast<HANDLE>(1));
        if (owner != nullptr) PostMessageW(owner, exitFullscreenMessage, 1, 0);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        KillTimer(window, videoClickTimerId);
        RemovePropW(window, L"NexPlaySuppressSingleClick");
    }
    if (message == WM_ERASEBKGND) {
        const RECT area = [] (const HWND target) {
            RECT rectangle{};
            GetClientRect(target, &rectangle);
            return rectangle;
        }(window);
        FillRect(reinterpret_cast<HDC>(wParam), &area,
                 reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
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
                    if (key->vkCode == keyboardHookState->saveHotkeyVk &&
                        modifiers == keyboardHookState->saveHotkeyModifiers) {
                        PostMessageW(keyboardHookWindow, WM_HOTKEY, saveHotkeyId, 0);
                    } else if (key->vkCode == keyboardHookState->stopHotkeyVk &&
                               modifiers == keyboardHookState->stopHotkeyModifiers) {
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
    std::wstring commandLine;
    for (const auto& argument : arguments) {
        if (!commandLine.empty()) commandLine.push_back(L' ');
        commandLine.append(argument);
    }
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
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
    std::wstring commandLine;
    for (const auto& argument : arguments) {
        if (!commandLine.empty()) commandLine.push_back(L' ');
        commandLine.append(argument);
    }
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
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
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
        const double timestamp = std::clamp(
            (static_cast<double>(result.firstFrameIndex) + 0.5) / framesPerSecond,
            0.0, std::max(0.0, duration - 0.5 / framesPerSecond));
        const std::vector<std::wstring> arguments{
            L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"quiet",
            L"-ss", secondsArgument(timestamp), L"-i", quoteProcessArgument(clip.wstring()),
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

struct EditorResult final {
    bool success{};
    std::filesystem::path output;
    std::wstring message;
};

[[nodiscard]] EditorResult exportEditedClip(
    const std::filesystem::path& input,
    std::wstring outputName,
    const double start,
    const double end) {
    if (!validClipName(outputName) || end - start < 0.1) {
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
    const std::vector<std::wstring> arguments{
        L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"error", L"-y",
        L"-ss", secondsArgument(start), L"-i", quoteProcessArgument(input.wstring()),
        L"-t", secondsArgument(end - start), L"-map", L"0",
        L"-map_metadata", L"0", L"-c:v", L"h264_nvenc", L"-preset", L"p5",
        L"-cq", L"18", L"-c:a", L"aac", L"-b:a", L"192k",
        L"-movflags", L"+faststart", quoteProcessArgument(temporary.wstring()),
    };
    if (runHiddenProcess(arguments) != 0) {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return {false, {}, L"FFmpeg nie mógł wyeksportować klipu."};
    }
    std::error_code error;
    std::filesystem::rename(temporary, output, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return {false, {}, L"Nie można zapisać pliku wynikowego."};
    }
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

void ensureGraphics(const HWND window, AppState& state) {
    if (state.renderTarget != nullptr) return;
    if (state.d2dFactory == nullptr && FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&state.d2dFactory)))) {
        throw std::runtime_error("Nie mozna uruchomic renderowania interfejsu.");
    }
    if (state.writeFactory == nullptr && FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(state.writeFactory.GetAddressOf())))) {
        throw std::runtime_error("Nie mozna uruchomic tekstu interfejsu.");
    }
    if (state.wicFactory == nullptr && FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&state.wicFactory)))) {
        throw std::runtime_error("Nie mozna uruchomic obslugi miniatur.");
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
            &state.renderTarget))) {
        throw std::runtime_error("Nie mozna utworzyc powierzchni interfejsu.");
    }
    state.renderTarget->CreateSolidColorBrush(white, &state.brush);
    createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Display", 28,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, state.titleFormat);
    createTextFormat(state.writeFactory.Get(), L"Segoe UI Variable Text", 19,
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

[[nodiscard]] ID2D1Bitmap* thumbnailBitmap(
    AppState& state, const std::wstring& key,
    const std::vector<std::uint8_t>& encodedImage) {
    if (encodedImage.empty() || encodedImage.size() > MAXDWORD) return nullptr;
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

void fillAccentGradient(AppState& state, const Rect rectangle, const float radius) {
    const D2D1_GRADIENT_STOP stops[] = {
        {0.0F, primary},
        {1.0F, accentSecondary},
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (SUCCEEDED(state.renderTarget->CreateGradientStopCollection(
            stops, static_cast<UINT32>(std::size(stops)), &collection)) &&
        SUCCEEDED(state.renderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(rectangle.left, rectangle.top),
                D2D1::Point2F(rectangle.right, rectangle.bottom)),
            collection.Get(), &gradient))) {
        state.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(rectangle.d2d(), radius, radius), gradient.Get());
    } else {
        fillRounded(state, rectangle, radius, primary);
    }
}

void drawAmbientGlow(AppState& state) {
    const float time = static_cast<float>(GetTickCount64() % 20'000) / 1000.0F;
    const float driftX = std::sin(time * 0.34F) * 24.0F;
    const float driftY = std::cos(time * 0.27F) * 18.0F;
    D2D1_COLOR_F ambientPrimary = primary;
    ambientPrimary.a = 0.115F;
    D2D1_COLOR_F ambientSecondary = accentSecondary;
    ambientSecondary.a = 0.035F;
    const D2D1_GRADIENT_STOP stops[] = {
        {0.0F, ambientPrimary},
        {0.52F, ambientSecondary},
        {1.0F, D2D1_COLOR_F{0.0F, 0.0F, 0.0F, 0.0F}},
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1RadialGradientBrush> gradient;
    if (SUCCEEDED(state.renderTarget->CreateGradientStopCollection(
            stops, static_cast<UINT32>(std::size(stops)), &collection)) &&
        SUCCEEDED(state.renderTarget->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(
                D2D1::Point2F(920 + driftX, 100 + driftY),
                D2D1::Point2F(0, 0), 330, 260),
            collection.Get(), &gradient))) {
        state.renderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(920 + driftX, 100 + driftY), 330, 260),
            gradient.Get());
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
    if (position > 0.02F) drawGlow(state, rectangle, 11, primary, position * 0.55F);
    fillRounded(state, rectangle, 11, track);
    const float centerX = rectangle.left + 11 +
        (rectangle.right - rectangle.left - 22) * position;
    state.brush->SetColor(white);
    state.renderTarget->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(centerX, (rectangle.top + rectangle.bottom) / 2), 7, 7),
        state.brush.Get());
}

void drawCheckbox(AppState& state, const Rect rectangle, const bool checked) {
    if (checked) drawGlow(state, rectangle, 5, primary, 0.22F);
    fillRounded(state, rectangle, 5, checked ? primary : field);
    if (!checked) {
        strokeRounded(state, rectangle, 5, border);
        return;
    }
    state.brush->SetColor(white);
    state.renderTarget->DrawLine(
        D2D1::Point2F(rectangle.left + 5, rectangle.top + 10),
        D2D1::Point2F(rectangle.left + 9, rectangle.top + 14), state.brush.Get(), 2.0F);
    state.renderTarget->DrawLine(
        D2D1::Point2F(rectangle.left + 9, rectangle.top + 14),
        D2D1::Point2F(rectangle.left + 16, rectangle.top + 6), state.brush.Get(), 2.0F);
}

void drawButton(AppState& state, const Rect rectangle, const std::wstring& label,
                const HitTarget target, const bool emphasized, const bool enabled = true) {
    const float hover = hoverValue(state, target);
    D2D1_COLOR_F color = emphasized ? primary : field;
    if (enabled) {
        const D2D1_COLOR_F destination = emphasized ? primaryHover : border;
        color.r += (destination.r - color.r) * hover;
        color.g += (destination.g - color.g) * hover;
        color.b += (destination.b - color.b) * hover;
    }
    if (!enabled) color.a = 0.42F;
    if (enabled && (emphasized || hover > 0.01F)) {
        drawGlow(state, rectangle, 10, emphasized ? primary : accentSecondary,
                 (emphasized ? 0.30F : 0.0F) + hover * 0.75F);
    }
    if (emphasized && enabled) fillAccentGradient(state, rectangle, 10);
    else fillRounded(state, rectangle, 10, color);
    if (!emphasized) strokeRounded(state, rectangle, 10, border);
    D2D1_COLOR_F textColor = white;
    if (!enabled) textColor.a = 0.42F;
    drawCenteredText(state, label, rectangle, state.buttonFormat.Get(), textColor);
}

void drawSidebar(AppState& state) {
    state.brush->SetColor(sidebar);
    state.renderTarget->FillRectangle(D2D1::RectF(0, 0, 220, windowHeight), state.brush.Get());
    drawGlow(state, {20, 20, 58, 58}, 11, primary, 0.5F);
    fillAccentGradient(state, {20, 20, 58, 58}, 11);
    drawCenteredText(state, L"N", {20, 19, 58, 58}, state.brandFormat.Get(), white);
    drawText(state, L"NexPlay", {70, 25, 190, 54}, state.brandFormat.Get(), white);
    drawText(state, L"REPLAY ENGINE", {20, 83, 190, 104}, state.smallFormat.Get(), muted);

    const auto drawNav = [&](const Rect rectangle, const Page page,
                             const wchar_t* icon, const wchar_t* label) {
        const bool active = state.page == page ||
            (page == Page::clips && state.page == Page::editor);
        const HitTarget target = page == Page::replay ? HitTarget::replayPage :
            (page == Page::clips ? HitTarget::clipsPage : HitTarget::settingsPage);
        const float hover = hoverValue(state, target);
        if (active || hover > 0.01F) {
            D2D1_COLOR_F navColor = active ? primary : field;
            if (active) {
                navColor.r *= 0.20F;
                navColor.g *= 0.20F;
                navColor.b *= 0.20F;
            }
            if (!active) navColor.a = hover;
            fillRounded(state, rectangle, 10,
                        navColor);
        }
        if (active) {
            drawGlow(state,
                      {rectangle.left, rectangle.top + 11, rectangle.left + 3, rectangle.bottom - 11},
                      2, primary, 0.8F);
            fillRounded(state,
                        {rectangle.left, rectangle.top + 11, rectangle.left + 3, rectangle.bottom - 11},
                        2, primary);
        }
        drawText(state, icon,
                 {rectangle.left + 18, rectangle.top + 13, rectangle.left + 42, rectangle.bottom},
                 state.bodyFormat.Get(), active ? primaryHover : muted);
        drawText(state, label,
                 {rectangle.left + 50, rectangle.top + 13, rectangle.right, rectangle.bottom},
                 state.bodyFormat.Get(), active ? white : muted);
    };
    drawNav(replayNavRect, Page::replay, L"●", L"Nagrywanie");
    drawNav(clipsNavRect, Page::clips, L"▰", L"Biblioteka klipów");
    drawNav(settingsNavRect, Page::settings, L"⚙", L"Ustawienia");

    drawText(state, L"SKRÓTY", {20, 644, 190, 666}, state.smallFormat.Get(), muted);
    fillRounded(state, {20, 680, 122, 710}, 7, field);
    drawCenteredText(state, hotkeyLabel(state.saveHotkeyModifiers, state.saveHotkeyVk),
                     {20, 680, 122, 710}, state.smallFormat.Get(), white);
    drawText(state, L"Zapisz", {132, 686, 204, 710}, state.smallFormat.Get(), muted);
    fillRounded(state, {20, 722, 122, 752}, 7, field);
    drawCenteredText(state, hotkeyLabel(state.stopHotkeyModifiers, state.stopHotkeyVk),
                     {20, 722, 122, 752}, state.smallFormat.Get(), white);
    drawText(state, L"Stop", {132, 728, 204, 752}, state.smallFormat.Get(), muted);
}

void drawTitlebar(AppState& state) {
    const float minimizeHover = hoverValue(state, HitTarget::minimize);
    const float closeHover = hoverValue(state, HitTarget::close);
    if (minimizeHover > 0.01F) {
        D2D1_COLOR_F hoverColor = field;
        hoverColor.a = minimizeHover;
        fillRounded(state, minimizeRect, 8, hoverColor);
    }
    drawCenteredText(state, L"—", minimizeRect, state.headingFormat.Get(),
                     minimizeHover > 0.4F ? white : muted);
    if (closeHover > 0.01F) {
        D2D1_COLOR_F closeColor = red;
        closeColor.a = closeHover;
        fillRounded(state, closeRect, 8, closeColor);
    }
    drawCenteredText(state, L"×", closeRect, state.headingFormat.Get(),
                     closeHover > 0.4F ? white : muted);
}

void drawStatusChip(AppState& state) {
    const bool running = state.engine.isRunning();
    const Rect chip{914, 92, 1060, 126};
    const float pulse = running
        ? 0.5F + 0.5F * std::sin(static_cast<float>(GetTickCount64() % 4'000) / 4'000.0F * 6.283185F)
        : 0.0F;
    if (running) drawGlow(state, chip, 17, green, 0.15F + pulse * 0.18F);
    fillRounded(state, chip, 17,
                running ? D2D1_COLOR_F{0.018F, 0.100F, 0.073F, 1} : field);
    state.brush->SetColor(running ? green : muted);
    if (running) {
        D2D1_COLOR_F dotGlow = green;
        dotGlow.a = 0.16F + pulse * 0.18F;
        state.brush->SetColor(dotGlow);
        state.renderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(935, 109), 8 + pulse * 2, 8 + pulse * 2),
            state.brush.Get());
        state.brush->SetColor(green);
    }
    state.renderTarget->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(935, 109), 3.5F, 3.5F), state.brush.Get());
    drawText(state, running ? L"BUFOR AKTYWNY" : L"GOTOWY",
             {947, 100, 1052, 124}, state.smallFormat.Get(), running ? green : muted);
}

void drawReplayPage(AppState& state) {
    const bool running = state.engine.isRunning();
    const auto& resolution = resolutionPresets[state.resolutionPreset];
    const std::wstring resolutionDescription = resolution.width == 0
        ? L"Natywna (" + std::to_wstring(GetSystemMetrics(SM_CXSCREEN)) + L" × " +
              std::to_wstring(GetSystemMetrics(SM_CYSCREEN)) + L")"
        : std::wstring(resolution.label);
    drawText(state, L"Instant Replay", {260, 88, 600, 128}, state.titleFormat.Get(), white);
    drawText(state, L"Nagrywaj w tle i zachowuj najlepsze momenty.",
             {260, 126, 730, 150}, state.bodyFormat.Get(), muted);
    drawStatusChip(state);

    fillRounded(state, {260, 166, 1060, 282}, 16, card);
    strokeRounded(state, {260, 166, 1060, 282}, 16, D2D1_COLOR_F{0.080F, 0.082F, 0.115F, 1});
    drawText(state, L"Sterowanie buforem", {284, 184, 520, 214}, state.headingFormat.Get(), white);
    drawButton(state, startRect, running ? L"Zatrzymaj bufor" : L"Uruchom bufor",
               HitTarget::startStop, true);
    drawButton(state, saveRect,
               L"Zapisz klip  ·  " + hotkeyLabel(state.saveHotkeyModifiers, state.saveHotkeyVk),
               HitTarget::save, false, running);
    drawText(state, state.status, {674, 231, 1034, 254}, state.smallFormat.Get(),
             state.status.starts_with(L"Błąd") ? red : muted);

    fillRounded(state, {260, 298, 1060, 474}, 16, card);
    strokeRounded(state, {260, 298, 1060, 474}, 16, D2D1_COLOR_F{0.080F, 0.082F, 0.115F, 1});
    drawText(state, L"Jakość nagrywania", {284, 318, 540, 346}, state.headingFormat.Get(), white);
    drawText(state, L"Główny monitor  •  NVENC H.264  •  skalowanie na GPU  •  do 8K",
             {284, 348, 830, 370}, state.smallFormat.Get(), muted);
    fillRounded(state, durationFieldRect, 10, field);
    fillRounded(state, resolutionFieldRect, 10, field);
    fillRounded(state, fpsFieldRect, 10, field);
    fillRounded(state, bitrateFieldRect, 10, field);
    strokeRounded(state, durationFieldRect, 10,
                  state.activeField == HitTarget::durationField ? primary : border);
    strokeRounded(state, resolutionFieldRect, 10,
                  state.hover == HitTarget::resolutionField ? primary : border);
    strokeRounded(state, fpsFieldRect, 10,
                  state.activeField == HitTarget::fpsField ? primary : border);
    strokeRounded(state, bitrateFieldRect, 10,
                  state.activeField == HitTarget::bitrateField ? primary : border);
    if (state.activeField == HitTarget::durationField) drawGlow(state, durationFieldRect, 10, primary, 0.75F);
    if (state.hover == HitTarget::resolutionField) drawGlow(state, resolutionFieldRect, 10, primary, 0.35F);
    if (state.activeField == HitTarget::fpsField) drawGlow(state, fpsFieldRect, 10, primary, 0.75F);
    if (state.activeField == HitTarget::bitrateField) drawGlow(state, bitrateFieldRect, 10, primary, 0.75F);
    drawText(state, L"DŁUGOŚĆ BUFORA", {298, 380, 464, 402}, state.smallFormat.Get(), muted);
    drawText(state, L"ROZDZIELCZOŚĆ", {492, 380, 680, 402}, state.smallFormat.Get(), muted);
    drawText(state, L"FPS", {708, 380, 824, 402}, state.smallFormat.Get(), muted);
    drawText(state, L"BITRATE (Mb/s)", {852, 380, 1036, 402}, state.smallFormat.Get(), muted);
    drawNumericValue(state, state.durationText, {302, 411, 420, 450}, HitTarget::durationField);
    drawCenteredText(state, resolutionDescription, {486, 407, 672, 453},
                     state.buttonFormat.Get(), white);
    drawNumericValue(state, state.fpsText, {706, 411, 780, 450}, HitTarget::fpsField);
    drawNumericValue(state, state.bitrateText, {850, 411, 990, 450}, HitTarget::bitrateField);
    drawText(state, L"sek.", {420, 422, 458, 448}, state.smallFormat.Get(), muted);
    drawText(state, L"kl./s", {780, 422, 818, 448}, state.smallFormat.Get(), muted);

    fillRounded(state, {260, 490, 1060, 770}, 16, card);
    strokeRounded(state, {260, 490, 1060, 770}, 16, D2D1_COLOR_F{0.080F, 0.082F, 0.115F, 1});
    drawText(state, L"Źródła audio", {284, 509, 520, 537}, state.headingFormat.Get(), white);
    drawText(state, L"Każde źródło zostanie zapisane jako osobna ścieżka.",
             {284, 539, 690, 561}, state.smallFormat.Get(), muted);
    drawText(state, L"Mikrofon", {774, 516, 846, 538}, state.smallFormat.Get(),
             running ? muted : white);
    drawToggle(state, {898, 514, 938, 536}, state.microphoneAnimation);
    drawButton(state, refreshRect, L"Odśwież", HitTarget::refreshAudio, false, !running);

    constexpr int visibleRows = 5;
    constexpr float rowHeight = 36.0F;
    state.audioScroll = std::clamp(
        state.audioScroll, 0,
        std::max(0, static_cast<int>(state.audioRows.size()) - visibleRows));
    for (int visible = 0; visible < visibleRows; ++visible) {
        const int index = visible + state.audioScroll;
        if (index >= static_cast<int>(state.audioRows.size())) break;
        const float top = 574 + visible * rowHeight;
        const auto& row = state.audioRows[static_cast<std::size_t>(index)];
        const bool rowHovered = state.mouseX >= 280 && state.mouseX <= 1038 &&
            state.mouseY >= top && state.mouseY <= top + 32 && !running;
        if (visible % 2 == 0 || rowHovered) {
            fillRounded(state, {280, top, 1038, top + 32}, 7,
                        rowHovered ? D2D1_COLOR_F{0.070F, 0.058F, 0.125F, 1}
                                   : D2D1_COLOR_F{0.036F, 0.038F, 0.053F, 1});
        }
        drawCheckbox(state, {294, top + 6, 314, top + 26}, row.included);
        drawText(state, row.name, {330, top + 7, 840, top + 29}, state.bodyFormat.Get(),
                 row.included ? white : muted);
        drawText(state, L"PID " + std::to_wstring(row.processId),
                 {875, top + 8, 1018, top + 29}, state.smallFormat.Get(), muted);
    }
    if (state.audioRows.size() > visibleRows) {
        fillRounded(state, {1044, 574, 1047, 754}, 2, border);
        const float thumbHeight = 180.0F * visibleRows /
            static_cast<float>(state.audioRows.size());
        const float travel = 180.0F - thumbHeight;
        const float position = static_cast<float>(state.audioScroll) /
            static_cast<float>(state.audioRows.size() - visibleRows);
        fillRounded(state, {1043, 574 + position * travel, 1048,
                            574 + position * travel + thumbHeight}, 3, primary);
    }
    if (state.audioRows.empty()) {
        drawText(state, L"Brak aktywnych aplikacji audio. Uruchom dźwięk i kliknij Odśwież.",
                 {294, 590, 900, 620}, state.bodyFormat.Get(), muted);
    }
}

[[nodiscard]] std::wstring sizeLabel(const std::uintmax_t bytes) {
    wchar_t text[32]{};
    swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

void drawClipsPage(AppState& state) {
    drawText(state, L"Biblioteka klipów", {260, 88, 650, 128}, state.titleFormat.Get(), white);
    drawText(state, L"Najedź na miniaturę i przesuń kursor, aby podejrzeć timeline.",
             {260, 126, 790, 150}, state.bodyFormat.Get(), muted);
    drawButton(state, openClipsRect, L"Otwórz folder", HitTarget::openClips, false);
    fillRounded(state, clipsListRect, 16, card);
    strokeRounded(state, clipsListRect, 16, D2D1_COLOR_F{0.080F, 0.082F, 0.115F, 1});
    if (state.clips.empty()) {
        drawCenteredText(state, L"Nie ma jeszcze zapisanych klipów",
                         {300, 350, 1020, 390}, state.headingFormat.Get(), white);
        drawCenteredText(state,
                         L"Uruchom bufor i użyj skrótu " +
                             hotkeyLabel(state.saveHotkeyModifiers, state.saveHotkeyVk) +
                             L", aby zapisać pierwszy klip.",
                         {300, 394, 1020, 430}, state.bodyFormat.Get(), muted);
        return;
    }
    constexpr int visibleRows = 2;
    const int totalRows = static_cast<int>((state.clips.size() + 1) / 2);
    state.clipScroll = std::clamp(
        state.clipScroll, 0, std::max(0, totalRows - visibleRows));
    for (int visible = 0; visible < visibleRows * 2; ++visible) {
        const int index = state.clipScroll * 2 + visible;
        if (index >= static_cast<int>(state.clips.size())) break;
        const int column = visible % 2;
        const int row = visible / 2;
        const float left = 280.0F + column * 380.0F;
        const float top = 176.0F + row * 276.0F;
        const Rect clipCard{left, top, left + 370, top + 258};
        const Rect imageArea{left + 10, top + 10, left + 360, top + 207};
        const auto& clip = state.clips[static_cast<std::size_t>(index)];
        const bool hovered = clipCard.contains(state.mouseX, state.mouseY);
        const bool previewHovered = imageArea.contains(state.mouseX, state.mouseY);
        const float previewPosition = previewHovered
            ? std::clamp((state.mouseX - imageArea.left) /
                         (imageArea.right - imageArea.left), 0.0F, 0.999F)
            : 0.0F;
        const bool timelineKnown = clip.preview != nullptr &&
            clip.preview->duration > 0.0 && clip.preview->framesPerSecond > 0.0;
        const int totalFrames = timelineKnown
            ? std::max(1, static_cast<int>(std::ceil(
                  clip.preview->duration * clip.preview->framesPerSecond)))
            : 1;
        const int requestedFrame = timelineKnown
            ? std::clamp(
                  static_cast<int>(std::floor(previewPosition * totalFrames)),
                  0, totalFrames - 1)
            : 0;
        if (previewHovered) {
            requestThumbnail(state.mainWindow, state, clip.path, requestedFrame);
        }
        int displayedFrame = requestedFrame;
        const std::vector<std::uint8_t>* thumbnail{};
        if (clip.preview != nullptr) {
            auto frame = clip.preview->frames.find(requestedFrame);
            if (frame == clip.preview->frames.end() && !clip.preview->frames.empty()) {
                frame = clip.preview->frames.begin();
            }
            if (frame != clip.preview->frames.end()) {
                displayedFrame = frame->first;
                thumbnail = &frame->second;
            }
        }

        D2D1_COLOR_F hoveredCard = primary;
        hoveredCard.r *= 0.16F;
        hoveredCard.g *= 0.16F;
        hoveredCard.b *= 0.16F;
        fillRounded(state, clipCard, 14,
                    hovered ? hoveredCard : field);
        if (hovered) {
            const float pulse = 0.72F + 0.28F * std::sin(
                static_cast<float>(GetTickCount64() % 2'400) / 2'400.0F * 6.283185F);
            drawGlow(state, clipCard, 14, primary, 0.18F + pulse * 0.10F);
        }
        fillRounded(state, imageArea, 10, D2D1_COLOR_F{0.008F, 0.008F, 0.012F, 1});
        const std::wstring bitmapKey = clip.path.wstring() + L"#" +
            std::to_wstring(displayedFrame);
        if (ID2D1Bitmap* bitmap = thumbnail == nullptr
                ? nullptr
                : thumbnailBitmap(state, bitmapKey, *thumbnail);
            bitmap != nullptr) {
            state.renderTarget->DrawBitmap(
                bitmap, imageArea.d2d(), 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            const bool failed = state.thumbnailFailures.contains(
                {clip.path, requestedFrame}) ||
                (requestedFrame != 0 &&
                 state.thumbnailFailures.contains({clip.path, 0}));
            drawCenteredText(state,
                             failed ? L"Podgląd niedostępny" : L"Generowanie miniatury…",
                             imageArea,
                             state.smallFormat.Get(), muted);
        }
        strokeRounded(state, imageArea, 10, hovered ? primary : border);
        if (!previewHovered) {
            fillRounded(state, {left + 161, top + 82, left + 209, top + 130}, 24,
                        D2D1_COLOR_F{0.025F, 0.020F, 0.050F, 0.88F});
            drawCenteredText(state, L"▶", {left + 161, top + 82, left + 209, top + 130},
                             state.headingFormat.Get(), white);
        } else {
            fillRounded(state,
                        {imageArea.left, imageArea.bottom - 5,
                         imageArea.left + previewPosition *
                             (imageArea.right - imageArea.left), imageArea.bottom},
                        2, primary);
            if (timelineKnown) {
                const Rect timeBadge{imageArea.right - 120, imageArea.bottom - 34,
                                     imageArea.right - 10, imageArea.bottom - 10};
                fillRounded(state, timeBadge, 7,
                            D2D1_COLOR_F{0.010F, 0.010F, 0.016F, 0.88F});
                const double frameTime = static_cast<double>(requestedFrame) /
                    clip.preview->framesPerSecond;
                drawCenteredText(state,
                                 preciseTimeLabel(frameTime) + L"  •  #" +
                                     std::to_wstring(requestedFrame),
                                 timeBadge, state.smallFormat.Get(), white);
            }
        }
        drawText(state, clip.path.stem().wstring(),
                 {left + 14, top + 216, left + 268, top + 239},
                  state.bodyFormat.Get(), white);
        const std::wstring details = clip.preview != nullptr && clip.preview->duration > 0.0
            ? timeLabel(clip.preview->duration) + L"  •  MP4  •  osobne audio"
            : L"MP4  •  osobne ścieżki audio";
        drawText(state, details, {left + 14, top + 238, left + 280, top + 256},
                  state.smallFormat.Get(), muted);
        drawText(state, sizeLabel(clip.size),
                 {left + 290, top + 220, left + 354, top + 250},
                  state.smallFormat.Get(), muted);
    }
    if (totalRows > visibleRows) {
        fillRounded(state, {1046, 176, 1049, 710}, 2, border);
        const float thumbHeight = 534.0F * visibleRows / static_cast<float>(totalRows);
        const float travel = 534.0F - thumbHeight;
        const float position = static_cast<float>(state.clipScroll) /
            static_cast<float>(totalRows - visibleRows);
        fillRounded(state, {1045, 176 + position * travel, 1050,
                            176 + position * travel + thumbHeight}, 3, primary);
    }
}

[[nodiscard]] Rect clipMenuItemRect(const AppState& state, const int item) {
    const auto& menu = state.clipContextMenuRect;
    const float top = item < 4
        ? menu.top + 42.0F + item * 38.0F
        : menu.top + 210.0F;
    return {menu.left + 8, top, menu.right - 8, top + 36};
}

void drawClipContextMenu(AppState& state) {
    if (!state.clipContextMenuOpen || state.clipContextMenuClip.empty()) return;
    const Rect menu = state.clipContextMenuRect;
    drawGlow(state, menu, 14, primary, 0.50F);
    fillRounded(state, menu, 14, D2D1_COLOR_F{0.018F, 0.018F, 0.028F, 0.985F});
    strokeRounded(state, menu, 14, D2D1_COLOR_F{0.170F, 0.130F, 0.330F, 1.0F});
    drawText(state, state.clipContextMenuClip.stem().wstring(),
             {menu.left + 16, menu.top + 12, menu.right - 16, menu.top + 35},
             state.smallFormat.Get(), muted);

    constexpr std::array labels{
        L"Otwórz w edytorze NexPlay",
        L"Odtwórz w domyślnej aplikacji",
        L"Pokaż w folderze",
        L"Kopiuj ścieżkę pliku",
        L"Usuń klip do Kosza…",
    };
    constexpr std::array glyphs{L"✦", L"▶", L"⌕", L"⌘", L"×"};
    for (int item = 0; item < static_cast<int>(labels.size()); ++item) {
        const Rect row = clipMenuItemRect(state, item);
        const bool hovered = row.contains(state.mouseX, state.mouseY);
        if (hovered) {
            fillRounded(state, row, 9,
                        item == 4 ? D2D1_COLOR_F{0.190F, 0.040F, 0.065F, 1.0F}
                                  : D2D1_COLOR_F{0.075F, 0.052F, 0.150F, 1.0F});
        }
        drawCenteredText(state, glyphs[static_cast<std::size_t>(item)],
                         {row.left + 4, row.top, row.left + 38, row.bottom},
                         state.bodyFormat.Get(), item == 4 ? red : primaryHover);
        drawText(state, labels[static_cast<std::size_t>(item)],
                 {row.left + 44, row.top + 9, row.right - 8, row.bottom},
                 state.bodyFormat.Get(), item == 4 ? red : white);
    }
    state.brush->SetColor(border);
    state.renderTarget->DrawLine(
        D2D1::Point2F(menu.left + 14, menu.top + 202),
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
    const auto milliseconds = static_cast<unsigned long long>(
        std::max(0.0, seconds) * 1'000.0);
    const unsigned long long minutes = milliseconds / 60'000;
    const unsigned long long remainingSeconds = (milliseconds / 1'000) % 60;
    const unsigned long long remainingMilliseconds = milliseconds % 1'000;
    wchar_t text[32]{};
    swprintf_s(text, L"%02llu:%02llu.%03llu",
               minutes, remainingSeconds, remainingMilliseconds);
    return text;
}

void drawEditorName(AppState& state) {
    fillRounded(state, editorNameRect, 10, field);
    strokeRounded(state, editorNameRect, 10,
                  state.activeField == HitTarget::editorName ? primary : border);
    if (state.activeField == HitTarget::editorName) {
        drawGlow(state, editorNameRect, 10, primary, 0.55F);
    }
    const std::wstring shown = state.editorName.empty() ? L"Nazwa klipu" : state.editorName;
    if (state.activeField == HitTarget::editorName && state.replaceFieldOnInput) {
        const float width = std::min(
            measuredTextWidth(state, shown, state.bodyFormat.Get()) + 14.0F,
            editorNameRect.right - editorNameRect.left - 24.0F);
        D2D1_COLOR_F selectionColor = primary;
        selectionColor.a = 0.38F;
        fillRounded(state,
                    {editorNameRect.left + 12, editorNameRect.top + 9,
                     editorNameRect.left + 12 + width, editorNameRect.bottom - 9},
                    5, selectionColor);
    }
    drawText(state, shown,
             {editorNameRect.left + 14, editorNameRect.top + 12,
              editorNameRect.right - 14, editorNameRect.bottom - 8},
             state.bodyFormat.Get(), state.editorName.empty() ? muted : white);
}

void drawEditorPage(AppState& state) {
    drawButton(state, editorBackRect, L"←  Wróć", HitTarget::editorBack, false);
    drawText(state, L"Edytor klipu", {366, 88, 650, 128}, state.titleFormat.Get(), white);
    drawText(state, L"Przytnij materiał i zapisz nową wersję bez naruszania oryginału.",
             {366, 126, 850, 150}, state.bodyFormat.Get(), muted);

    drawGlow(state, {276, 151, 1044, 574}, 16, primary, 0.16F);
    fillRounded(state, {276, 151, 1044, 574}, 16, card);
    strokeRounded(state, {276, 151, 1044, 574}, 16, border);

    fillRounded(state, {260, 586, 1060, 778}, 16, card);
    strokeRounded(state, {260, 586, 1060, 778}, 16,
                  D2D1_COLOR_F{0.080F, 0.082F, 0.115F, 1});
    drawText(state, L"NAZWA NOWEGO PLIKU", {284, 580, 650, 604}, state.smallFormat.Get(), muted);
    drawEditorName(state);
    drawText(state, L".mp4", {664, 617, 714, 644}, state.bodyFormat.Get(), muted);
    drawText(state, L"Wszystkie osobne ścieżki audio zostaną zachowane.",
             {730, 617, 1036, 642}, state.smallFormat.Get(), muted);
    drawText(state,
             L"Kliknij pasek, aby przewinąć  •  przeciągnij biały uchwyt, aby przyciąć",
             {284, 653, 850, 675}, state.smallFormat.Get(), muted);

    const double duration = std::max(0.001, state.editorDuration);
    const float startX = editorTimelineRect.left +
        static_cast<float>(state.trimStart / duration) *
        (editorTimelineRect.right - editorTimelineRect.left);
    const float endX = editorTimelineRect.left +
        static_cast<float>(state.trimEnd / duration) *
        (editorTimelineRect.right - editorTimelineRect.left);
    const float playX = editorTimelineRect.left +
        static_cast<float>(state.playPosition / duration) *
        (editorTimelineRect.right - editorTimelineRect.left);

    fillRounded(state, {editorTimelineRect.left, 682, editorTimelineRect.right, 690}, 4, border);
    drawGlow(state, {startX, 681, endX, 691}, 5, primary, 0.30F);
    fillRounded(state, {startX, 681, endX, 691}, 5, primary);
    state.brush->SetColor(accentSecondary);
    state.renderTarget->DrawLine(
        D2D1::Point2F(playX, 673), D2D1::Point2F(playX, 703), state.brush.Get(), 1.5F);
    state.brush->SetColor(white);
    state.renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(startX, 686), 7, 7), state.brush.Get());
    state.renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(endX, 686), 7, 7), state.brush.Get());
    drawText(state, timeLabel(state.trimStart), {284, 702, 350, 722}, state.smallFormat.Get(), muted);
    drawText(state, timeLabel(state.trimEnd), {976, 702, 1036, 722}, state.smallFormat.Get(), muted);
    drawCenteredText(state, timeLabel(state.playPosition) + L" / " + timeLabel(state.editorDuration),
                     {514, 700, 806, 722}, state.smallFormat.Get(), muted);

    drawButton(state, editorPlayRect, state.playing ? L"Ⅱ" : L"▶",
               HitTarget::editorPlay, false);
    drawButton(state, editorFullscreenRect, L"Pełny ekran",
               HitTarget::editorFullscreen, false);
    drawText(state, L"Spacja  play/pause  •  ← →  ±5 s",
             {504, 731, 830, 753}, state.smallFormat.Get(), muted);
    drawButton(state, editorSaveRect, L"Eksportuj nowy klip",
               HitTarget::editorSave, true, state.editorDuration > 0.0);
}

void drawSettingsPage(AppState& state) {
    drawText(state, L"Ustawienia", {260, 88, 650, 128}, state.titleFormat.Get(), white);
    drawText(state, L"Dopasuj zachowanie, skróty i wygląd NexPlay.",
             {260, 126, 780, 150}, state.bodyFormat.Get(), muted);

    fillRounded(state, {260, 158, 1060, 338}, 16, card);
    strokeRounded(state, {260, 158, 1060, 338}, 16,
                  D2D1_COLOR_F{0.080F, 0.082F, 0.115F, 1});
    drawText(state, L"Uruchamianie", {284, 174, 620, 202}, state.headingFormat.Get(), white);

    const auto drawStartupRow = [&](const Rect rectangle, const HitTarget target,
                                    const wchar_t* title, const wchar_t* description,
                                    const float togglePosition) {
        const float hover = hoverValue(state, target);
        D2D1_COLOR_F rowColor = field;
        rowColor.r += primary.r * hover * 0.055F;
        rowColor.g += primary.g * hover * 0.055F;
        rowColor.b += primary.b * hover * 0.055F;
        if (hover > 0.01F || togglePosition > 0.02F) {
            const float pulse = 0.65F + 0.35F * std::sin(
                static_cast<float>(GetTickCount64() % 2'800) / 2'800.0F * 6.283185F);
            drawGlow(state, rectangle, 12, primary,
                     hover * 0.22F + togglePosition * pulse * 0.12F);
        }
        fillRounded(state, rectangle, 12, rowColor);
        D2D1_COLOR_F rowBorder = border;
        if (hover > 0.01F) {
            rowBorder = primary;
            rowBorder.a = 0.30F + hover * 0.35F;
        }
        strokeRounded(state, rectangle, 12, rowBorder);
        drawText(state, title,
                 {rectangle.left + 22, rectangle.top + 10,
                  rectangle.right - 100, rectangle.top + 32},
                 state.bodyFormat.Get(), white);
        drawText(state, description,
                 {rectangle.left + 22, rectangle.top + 32,
                  rectangle.right - 100, rectangle.bottom - 5},
                 state.smallFormat.Get(), muted);
        drawToggle(state,
                   {rectangle.right - 70, rectangle.top + 15,
                    rectangle.right - 26, rectangle.top + 39},
                   togglePosition);
    };
    drawStartupRow(
        autostartRect, HitTarget::autostartToggle,
        L"Uruchamiaj NexPlay razem z Windows",
        L"Program otworzy się cicho w zasobniku po zalogowaniu użytkownika.",
        state.autostartAnimation);
    drawStartupRow(
        autoBufferRect, HitTarget::autoBufferToggle,
        L"Automatycznie uruchamiaj bufor",
        L"Nagrywanie ruszy z zapisanymi ustawieniami zaraz po starcie aplikacji.",
        state.autoBufferAnimation);

    fillRounded(state, {260, 354, 1060, 516}, 16, card);
    strokeRounded(state, {260, 354, 1060, 516}, 16,
                  D2D1_COLOR_F{0.080F, 0.082F, 0.115F, 1});
    drawText(state, L"Skróty globalne", {284, 370, 620, 398},
             state.headingFormat.Get(), white);

    const auto drawHotkey = [&](const Rect rectangle, const HitTarget target,
                                const HotkeyCapture capture, const wchar_t* title,
                                const UINT modifiers, const UINT virtualKey) {
        const bool listening = state.hotkeyCapture == capture;
        const float hover = hoverValue(state, target);
        const float pulse = 0.60F + 0.40F * std::sin(
            static_cast<float>(GetTickCount64() % 1'900) / 1'900.0F * 6.283185F);
        D2D1_COLOR_F rowColor = field;
        rowColor.r += primary.r * (hover * 0.05F + (listening ? 0.05F : 0.0F));
        rowColor.g += primary.g * (hover * 0.05F + (listening ? 0.05F : 0.0F));
        rowColor.b += primary.b * (hover * 0.05F + (listening ? 0.05F : 0.0F));
        if (listening || hover > 0.01F) {
            drawGlow(state, rectangle, 12, primary,
                     (listening ? 0.30F + pulse * 0.26F : hover * 0.24F));
        }
        fillRounded(state, rectangle, 12, rowColor);
        strokeRounded(state, rectangle, 12, listening ? primary : border);
        drawText(state, title, {rectangle.left + 18, rectangle.top + 18,
                                rectangle.right - 150, rectangle.bottom - 8},
                 state.bodyFormat.Get(), white);
        const std::wstring value = listening
            ? L"Wciśnij klawisz…" : hotkeyLabel(modifiers, virtualKey);
        const Rect keycap{rectangle.right - 142, rectangle.top + 10,
                          rectangle.right - 14, rectangle.bottom - 10};
        if (listening) drawGlow(state, keycap, 9, primary, 0.28F + pulse * 0.30F);
        D2D1_COLOR_F keycapColor = primary;
        keycapColor.r *= 0.13F;
        keycapColor.g *= 0.13F;
        keycapColor.b *= 0.13F;
        fillRounded(state, keycap, 9, listening ? keycapColor : sidebar);
        strokeRounded(state, keycap, 9, listening ? primary : border);
        drawCenteredText(state, value,
                         keycap,
                         state.buttonFormat.Get(), listening ? primaryHover : muted);
    };
    drawHotkey(saveHotkeyRect, HitTarget::saveHotkey, HotkeyCapture::save,
               L"Zapis klipu", state.saveHotkeyModifiers, state.saveHotkeyVk);
    drawHotkey(stopHotkeyRect, HitTarget::stopHotkey, HotkeyCapture::stop,
               L"Zatrzymaj bufor", state.stopHotkeyModifiers, state.stopHotkeyVk);
    drawText(state, L"Kliknij pole i wciśnij nowy klawisz lub kombinację. Esc anuluje zmianę.",
             {284, 480, 960, 502}, state.smallFormat.Get(), muted);

    fillRounded(state, {260, 532, 1060, 716}, 16, card);
    strokeRounded(state, {260, 532, 1060, 716}, 16,
                  D2D1_COLOR_F{0.080F, 0.082F, 0.115F, 1});
    drawText(state, L"Kolor akcentu", {284, 548, 620, 576},
             state.headingFormat.Get(), white);
    drawText(state, L"Kliknij lub przeciągnij, aby wybrać dowolny kolor.",
             {660, 552, 1028, 574}, state.smallFormat.Get(), muted);

    const D2D1_COLOR_F hueColor = hsvColor(state.accentHue, 1.0F, 1.0F);
    fillRounded(state, accentPlaneRect, 10, hueColor);
    const auto overlayGradient = [&](const D2D1_GRADIENT_STOP* stops,
                                     const UINT32 count,
                                     const D2D1_POINT_2F start,
                                     const D2D1_POINT_2F end,
                                     const Rect rectangle) {
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        if (SUCCEEDED(state.renderTarget->CreateGradientStopCollection(
                stops, count, &collection)) &&
            SUCCEEDED(state.renderTarget->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(start, end),
                collection.Get(), &gradient))) {
            state.renderTarget->FillRectangle(rectangle.d2d(), gradient.Get());
        }
    };
    const D2D1_GRADIENT_STOP saturationStops[]{
        {0.0F, D2D1_COLOR_F{1, 1, 1, 1}}, {1.0F, D2D1_COLOR_F{1, 1, 1, 0}},
    };
    overlayGradient(saturationStops, 2,
                    D2D1::Point2F(accentPlaneRect.left, accentPlaneRect.top),
                    D2D1::Point2F(accentPlaneRect.right, accentPlaneRect.top),
                    accentPlaneRect);
    const D2D1_GRADIENT_STOP valueStops[]{
        {0.0F, D2D1_COLOR_F{0, 0, 0, 0}}, {1.0F, D2D1_COLOR_F{0, 0, 0, 1}},
    };
    overlayGradient(valueStops, 2,
                    D2D1::Point2F(accentPlaneRect.left, accentPlaneRect.top),
                    D2D1::Point2F(accentPlaneRect.left, accentPlaneRect.bottom),
                    accentPlaneRect);
    strokeRounded(state, accentPlaneRect, 10, border);

    constexpr D2D1_GRADIENT_STOP hueStops[]{
        {0.0F, {1, 0, 0, 1}}, {0.167F, {1, 1, 0, 1}},
        {0.333F, {0, 1, 0, 1}}, {0.5F, {0, 1, 1, 1}},
        {0.667F, {0, 0, 1, 1}}, {0.833F, {1, 0, 1, 1}},
        {1.0F, {1, 0, 0, 1}},
    };
    overlayGradient(hueStops, static_cast<UINT32>(std::size(hueStops)),
                    D2D1::Point2F(accentHueRect.left, accentHueRect.top),
                    D2D1::Point2F(accentHueRect.left, accentHueRect.bottom),
                    accentHueRect);
    strokeRounded(state, accentHueRect, 10, border);

    const float pickerX = accentPlaneRect.left +
        state.accentSaturation * (accentPlaneRect.right - accentPlaneRect.left);
    const float pickerY = accentPlaneRect.top +
        (1.0F - state.accentValue) * (accentPlaneRect.bottom - accentPlaneRect.top);
    drawGlow(state, {pickerX - 7, pickerY - 7, pickerX + 7, pickerY + 7}, 7,
             white, 0.45F);
    state.brush->SetColor(white);
    state.renderTarget->DrawEllipse(
        D2D1::Ellipse(D2D1::Point2F(pickerX, pickerY), 6, 6), state.brush.Get(), 2.0F);
    const float hueY = accentHueRect.top +
        state.accentHue * (accentHueRect.bottom - accentHueRect.top);
    state.brush->SetColor(white);
    state.renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(accentHueRect.left - 3, hueY - 3,
                                     accentHueRect.right + 3, hueY + 3), 3, 3),
        state.brush.Get(), 2.0F);

    const float pulse = 0.62F + 0.38F * std::sin(
        static_cast<float>(GetTickCount64() % 2'600) / 2'600.0F * 6.283185F);
    drawGlow(state, accentPreviewRect, 12, primary, 0.30F + pulse * 0.26F);
    fillRounded(state, accentPreviewRect, 12, primary);
    drawCenteredText(state, accentHexLabel(state),
                     {accentPreviewRect.left, accentPreviewRect.top + 10,
                      accentPreviewRect.right, accentPreviewRect.top + 42},
                     state.headingFormat.Get(), white);
    drawCenteredText(state, L"AKCENT",
                     {accentPreviewRect.left, accentPreviewRect.top + 49,
                      accentPreviewRect.right, accentPreviewRect.bottom - 4},
                     state.smallFormat.Get(), white);
}

void paint(const HWND window, AppState& state) {
    PAINTSTRUCT paintInfo{};
    BeginPaint(window, &paintInfo);
    try {
        ensureGraphics(window, state);
        state.renderTarget->BeginDraw();
        state.renderTarget->Clear(background);
        drawAmbientGlow(state);
        drawSidebar(state);
        drawTitlebar(state);
        if (state.page == Page::replay) drawReplayPage(state);
        else if (state.page == Page::clips) drawClipsPage(state);
        else if (state.page == Page::editor) drawEditorPage(state);
        else drawSettingsPage(state);
        drawClipContextMenu(state);
        if (state.renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
            state.renderTarget.Reset();
            state.brush.Reset();
            state.thumbnailBitmaps.clear();
        }
    } catch (...) {
    }
    EndPaint(window, &paintInfo);
}

void updateEditorVisibility(AppState& state) {
    if (state.page == Page::clips || state.page == Page::settings ||
        (state.page == Page::replay && state.engine.isRunning())) {
        state.activeField = HitTarget::none;
    }
    if (state.videoWindow != nullptr) {
        ShowWindow(state.videoWindow, state.page == Page::editor ? SW_SHOW : SW_HIDE);
    }
}

void setStatus(const HWND window, AppState& state, std::wstring text) {
    state.status = std::move(text);
    wcsncpy_s(state.tray.szTip, state.status.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &state.tray);
    InvalidateRect(window, nullptr, FALSE);
}

void refreshAudioApplications(const HWND window, AppState& state) {
    std::map<DWORD, bool> previous;
    for (const auto& row : state.audioRows) previous[row.processId] = row.included;
    state.audioRows.clear();
    for (const auto& application : nexplay::audio::activeAudioApplications()) {
        const auto selection = previous.find(application.processId);
        state.audioRows.push_back({application.processId, application.name,
            selection == previous.end() || selection->second});
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
    if (state.mediaPlayer != nullptr) state.mediaPlayer->Pause();
    state.mediaPlayer.Reset();
    if (state.videoWindow != nullptr) ShowWindow(state.videoWindow, SW_HIDE);
    state.playing = false;
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
    if (state.fullscreenPlayer != nullptr) state.fullscreenPlayer->Pause();
    state.fullscreenPlayer.Reset();
    state.fullscreenRenderTarget.Reset();
    state.fullscreenBrush.Reset();
    const HWND fullscreenWindow = state.fullscreenWindow;
    state.fullscreenWindow = nullptr;
    state.fullscreenVideoWindow = nullptr;
    state.fullscreen = false;
    if (fullscreenWindow != nullptr) DestroyWindow(fullscreenWindow);
    MoveWindow(state.videoWindow, 284, 159, 752, 407, TRUE);
    ShowWindow(state.videoWindow, SW_SHOW);
    seekEditor(state, position);
    if (resumePlayback && state.mediaPlayer != nullptr) state.mediaPlayer->Play();
    SetFocus(state.mainWindow);
    if (state.mediaPlayer != nullptr) state.mediaPlayer->UpdateVideo();
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
    state.activeField = HitTarget::none;
    updateEditorVisibility(state);
    ShowWindow(state.videoWindow, SW_SHOW);
    const HRESULT result = MFPCreateMediaPlayer(
        clip.c_str(), TRUE, 0, nullptr, state.videoWindow, &state.mediaPlayer);
    if (FAILED(result)) {
        closeEditorPlayer(state);
        state.page = Page::clips;
        setStatus(window, state, L"Błąd: nie można otworzyć podglądu klipu");
        return;
    }
    state.playing = true;
    state.mediaPlayer->UpdateVideo();
    InvalidateRect(window, nullptr, FALSE);
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
            });
        updateEditorVisibility(state);
        setStatus(window, state, L"Uruchamianie NVENC i źródeł audio…");
    } catch (const std::exception& error) {
        MessageBoxA(window, error.what(), "NexPlay", MB_OK | MB_ICONERROR);
    }
}

void stopRecorder(const HWND window, AppState& state) {
    setStatus(window, state, L"Zatrzymywanie bufora…");
    state.engine.stop();
    updateEditorVisibility(state);
    setStatus(window, state, L"Bufor zatrzymany");
}

void saveClip(const HWND window, AppState& state) {
    if (!state.engine.isRunning()) return;
    state.engine.requestSave();
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
    if (state.editorDuration <= 0.0 || !validClipName(state.editorName)) {
        setStatus(window, state, L"Błąd: wpisz poprawną nazwę klipu");
        return;
    }
    if (state.mediaPlayer != nullptr) state.mediaPlayer->Pause();
    state.playing = false;
    setStatus(window, state, L"Eksportuję przycięty klip przez NVENC…");
    const auto input = state.selectedClip;
    const auto name = state.editorName;
    const double start = state.trimStart;
    const double end = state.trimEnd;
    state.editorJobs.emplace_back([window, input, name, start, end] {
        auto* result = new EditorResult(exportEditedClip(input, name, start, end));
        if (!PostMessageW(window, editorDoneMessage, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    });
}

[[nodiscard]] HitTarget hitTest(const AppState& state, const float x, const float y) {
    if (minimizeRect.contains(x, y)) return HitTarget::minimize;
    if (closeRect.contains(x, y)) return HitTarget::close;
    if (replayNavRect.contains(x, y)) return HitTarget::replayPage;
    if (clipsNavRect.contains(x, y)) return HitTarget::clipsPage;
    if (settingsNavRect.contains(x, y)) return HitTarget::settingsPage;
    if (state.page == Page::replay) {
        if (startRect.contains(x, y)) return HitTarget::startStop;
        if (saveRect.contains(x, y) && state.engine.isRunning()) return HitTarget::save;
        if (refreshRect.contains(x, y) && !state.engine.isRunning()) return HitTarget::refreshAudio;
        if (microphoneRect.contains(x, y) && !state.engine.isRunning()) return HitTarget::microphone;
        if (!state.engine.isRunning() && durationFieldRect.contains(x, y)) return HitTarget::durationField;
        if (!state.engine.isRunning() && resolutionFieldRect.contains(x, y)) return HitTarget::resolutionField;
        if (!state.engine.isRunning() && fpsFieldRect.contains(x, y)) return HitTarget::fpsField;
        if (!state.engine.isRunning() && bitrateFieldRect.contains(x, y)) return HitTarget::bitrateField;
    } else if (state.page == Page::clips && openClipsRect.contains(x, y)) {
        return HitTarget::openClips;
    } else if (state.page == Page::editor) {
        if (editorBackRect.contains(x, y)) return HitTarget::editorBack;
        if (editorPlayRect.contains(x, y)) return HitTarget::editorPlay;
        if (editorFullscreenRect.contains(x, y)) return HitTarget::editorFullscreen;
        if (editorSaveRect.contains(x, y)) return HitTarget::editorSave;
        if (editorNameRect.contains(x, y)) return HitTarget::editorName;
    } else if (state.page == Page::settings) {
        if (autostartRect.contains(x, y)) return HitTarget::autostartToggle;
        if (autoBufferRect.contains(x, y)) return HitTarget::autoBufferToggle;
        if (saveHotkeyRect.contains(x, y)) return HitTarget::saveHotkey;
        if (stopHotkeyRect.contains(x, y)) return HitTarget::stopHotkey;
        if (accentPlaneRect.contains(x, y)) return HitTarget::accentPlane;
        if (accentHueRect.contains(x, y)) return HitTarget::accentHue;
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

void trayDrawGlow(AppState& state, const Rect rectangle, const float radius,
                  const D2D1_COLOR_F source, const float intensity) {
    for (int layer = 5; layer >= 1; --layer) {
        D2D1_COLOR_F glow = source;
        glow.a = intensity * (0.018F + static_cast<float>(6 - layer) * 0.012F);
        trayStrokeRounded(
            state, expanded(rectangle, static_cast<float>(layer) * 1.7F),
            radius + static_cast<float>(layer) * 1.7F, glow,
            static_cast<float>(layer) * 1.4F);
    }
}

void trayFillAccentGradient(AppState& state, const Rect rectangle, const float radius) {
    const D2D1_GRADIENT_STOP stops[] = {{0.0F, primary}, {1.0F, accentSecondary}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (SUCCEEDED(state.trayMenuRenderTarget->CreateGradientStopCollection(
            stops, static_cast<UINT32>(std::size(stops)), &collection)) &&
        SUCCEEDED(state.trayMenuRenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(rectangle.left, rectangle.top),
                D2D1::Point2F(rectangle.right, rectangle.bottom)),
            collection.Get(), &gradient))) {
        state.trayMenuRenderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(rectangle.d2d(), radius, radius), gradient.Get());
    } else {
        trayFillRounded(state, rectangle, radius, primary);
    }
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
        trayDrawGlow(state, rectangle, 10, accent, hover * 0.45F);
        trayFillRounded(state, rectangle, 10, hoverFill);
        D2D1_COLOR_F hoverBorder = accent;
        hoverBorder.a = hover * 0.34F;
        trayStrokeRounded(state, rectangle, 10, hoverBorder);
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

        const float phase = static_cast<float>(GetTickCount64() % 4000) / 4000.0F;
        D2D1_COLOR_F ambient = primary;
        ambient.a = 0.08F + std::sin(phase * 6.2831853F) * 0.02F;
        const D2D1_GRADIENT_STOP ambientStops[] = {
            {0.0F, ambient},
            {1.0F, D2D1_COLOR_F{0, 0, 0, 0}},
        };
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1RadialGradientBrush> gradient;
        if (SUCCEEDED(target->CreateGradientStopCollection(
                ambientStops, static_cast<UINT32>(std::size(ambientStops)), &collection)) &&
            SUCCEEDED(target->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(
                    D2D1::Point2F(264, 22), D2D1::Point2F(), 142, 104),
                collection.Get(), &gradient))) {
            target->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(264, 22), 142, 104), gradient.Get());
        }

        trayStrokeRounded(state, {0.5F, 0.5F, trayMenuWidth - 0.5F,
                                  trayMenuHeight - 0.5F}, 16,
                          blendColor(border, primary, 0.16F));
        trayDrawGlow(state, {18, 16, 48, 46}, 9, primary, 0.48F);
        trayFillAccentGradient(state, {18, 16, 48, 46}, 9);
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
            hotkeyLabel(state.saveHotkeyModifiers, state.saveHotkeyVk);
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
                state->trayMenuHoverAnimation[index] += (target - before) * 0.20F;
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
    for (int visible = 0; visible < 4; ++visible) {
        const int column = visible % 2;
        const int row = visible / 2;
        const Rect clipCard{
            280.0F + column * 380.0F,
            176.0F + row * 276.0F,
            650.0F + column * 380.0F,
            434.0F + row * 276.0F,
        };
        if (!clipCard.contains(x, y)) continue;
        const int index = state.clipScroll * 2 + visible;
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
    if (state.colorDrag == ColorDrag::plane) {
        state.accentSaturation = std::clamp(
            (x - accentPlaneRect.left) /
                (accentPlaneRect.right - accentPlaneRect.left),
            0.0F, 1.0F);
        state.accentValue = 1.0F - std::clamp(
            (y - accentPlaneRect.top) /
                (accentPlaneRect.bottom - accentPlaneRect.top),
            0.0F, 1.0F);
    } else if (state.colorDrag == ColorDrag::hue) {
        state.accentHue = std::clamp(
            (y - accentHueRect.top) /
                (accentHueRect.bottom - accentHueRect.top),
            0.0F, 0.9999F);
    }
    applyAccentColor(state);
}

void handleClick(const HWND window, AppState& state, const float x, const float y) {
    const HitTarget clicked = hitTest(state, x, y);
    if (state.hotkeyCapture != HotkeyCapture::none &&
        clicked != HitTarget::saveHotkey && clicked != HitTarget::stopHotkey) {
        cancelHotkeyCapture(window, state);
    }
    switch (clicked) {
    case HitTarget::minimize:
    case HitTarget::close:
        KillTimer(window, 1);
        ShowWindow(window, SW_HIDE);
        setStatus(window, state, L"NexPlay działa w zasobniku systemowym");
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

    if (state.page == Page::replay && !state.engine.isRunning() &&
        x >= 280 && x <= 1038 && y >= 574 && y < 754) {
        const int index = static_cast<int>((y - 574) / 36) + state.audioScroll;
        if (index >= 0 && index < static_cast<int>(state.audioRows.size())) {
            auto& row = state.audioRows[static_cast<std::size_t>(index)];
            row.included = !row.included;
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
        state->tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        const LRESULT standard = DefWindowProcW(window, message, wParam, lParam);
        if (standard != HTCLIENT) return standard;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        if (point.y < 64 &&
            !minimizeRect.contains(static_cast<float>(point.x), static_cast<float>(point.y)) &&
            !closeRect.contains(static_cast<float>(point.x), static_cast<float>(point.y))) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (state != nullptr && LOWORD(lParam) == HTCLIENT) {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(window, &cursor);
            const float x = static_cast<float>(cursor.x);
            const float y = static_cast<float>(cursor.y);
            const bool row =
                (state->page == Page::replay && !state->engine.isRunning() &&
                 x >= 280 && x <= 1038 && y >= 574 && y < 754) ||
                (state->page == Page::clips && clipsListRect.contains(x, y));
            const HitTarget target = hitTest(*state, x, y);
            if (target == HitTarget::durationField || target == HitTarget::fpsField ||
                target == HitTarget::bitrateField) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            if (target == HitTarget::accentPlane || target == HitTarget::accentHue) {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
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
            state->mouseX = static_cast<float>(GET_X_LPARAM(lParam));
            state->mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
            if (state->colorDrag != ColorDrag::none) {
                updateAccentFromPointer(*state, state->mouseX, state->mouseY);
            }
            if (state->dragHandle != DragHandle::none) {
                moveTrimHandle(*state, state->mouseX);
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
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
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
    case WM_LBUTTONDOWN:
        if (state != nullptr && state->page == Page::settings) {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            if (accentPlaneRect.contains(x, y) || accentHueRect.contains(x, y)) {
                state->colorDrag = accentPlaneRect.contains(x, y)
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
            GET_X_LPARAM(lParam) >= static_cast<int>(editorTimelineRect.left - 10) &&
            GET_X_LPARAM(lParam) <= static_cast<int>(editorTimelineRect.right + 10) &&
            GET_Y_LPARAM(lParam) >= 667 && GET_Y_LPARAM(lParam) <= 707) {
            const float timelineWidth = editorTimelineRect.right - editorTimelineRect.left;
            const float startX = editorTimelineRect.left +
                static_cast<float>(state->trimStart / state->editorDuration) * timelineWidth;
            const float endX = editorTimelineRect.left +
                static_cast<float>(state->trimEnd / state->editorDuration) * timelineWidth;
            const float clickX = static_cast<float>(GET_X_LPARAM(lParam));
            constexpr float handleGrabRadius = 13.0F;
            if (std::abs(clickX - startX) <= handleGrabRadius) {
                state->dragHandle = DragHandle::start;
            } else if (std::abs(clickX - endX) <= handleGrabRadius) {
                state->dragHandle = DragHandle::end;
            } else {
                state->dragHandle = DragHandle::playhead;
            }
            if (state->mediaPlayer != nullptr) state->mediaPlayer->Pause();
            state->playing = false;
            SetCapture(window);
            moveTrimHandle(*state, static_cast<float>(GET_X_LPARAM(lParam)));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        if (state != nullptr) {
            state->hover = HitTarget::none;
            state->mouseX = -1;
            state->mouseY = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state != nullptr) {
            if (state->colorDrag != ColorDrag::none) {
                updateAccentFromPointer(
                    *state, static_cast<float>(GET_X_LPARAM(lParam)),
                    static_cast<float>(GET_Y_LPARAM(lParam)));
                state->colorDrag = ColorDrag::none;
                ReleaseCapture();
                saveAccentColor(*state);
                setStatus(window, *state,
                          L"Kolor akcentu: " + accentHexLabel(*state));
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (state->clipContextMenuOpen) {
                const float x = static_cast<float>(GET_X_LPARAM(lParam));
                const float y = static_cast<float>(GET_Y_LPARAM(lParam));
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
                moveTrimHandle(*state, static_cast<float>(GET_X_LPARAM(lParam)));
                state->dragHandle = DragHandle::none;
                ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            handleClick(window, *state, static_cast<float>(GET_X_LPARAM(lParam)),
                        static_cast<float>(GET_Y_LPARAM(lParam)));
        }
        return 0;
    case WM_CHAR:
        if (state != nullptr &&
            (!state->engine.isRunning() || state->activeField == HitTarget::editorName)) {
            if (auto* text = activeFieldText(*state); text != nullptr) {
                const bool nameField = state->activeField == HitTarget::editorName;
                const bool validNameCharacter = nameField && wParam >= 32 &&
                    std::wstring(L"<>:\"/\\|?*").find(static_cast<wchar_t>(wParam)) == std::wstring::npos;
                if ((nameField && validNameCharacter) ||
                    (!nameField && wParam >= L'0' && wParam <= L'9')) {
                    if (state->replaceFieldOnInput) {
                        text->clear();
                        state->replaceFieldOnInput = false;
                    }
                    const std::size_t maximumLength = nameField ? 100 : 4;
                    if (text->size() < maximumLength) text->push_back(static_cast<wchar_t>(wParam));
                } else if (wParam == L'\b') {
                    state->replaceFieldOnInput = false;
                    if (!text->empty()) text->pop_back();
                } else if (wParam == L'\r' || wParam == L'\t') {
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
            if (state->page == Page::editor &&
                state->activeField != HitTarget::editorName) {
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
            else state->clipScroll += direction;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (state != nullptr && wParam == 1) {
            for (std::size_t index = 1; index < state->hoverAnimation.size(); ++index) {
                const float target = index == static_cast<std::size_t>(state->hover) ? 1.0F : 0.0F;
                state->hoverAnimation[index] +=
                    (target - state->hoverAnimation[index]) * 0.22F;
            }
            const float microphoneTarget = state->microphone ? 1.0F : 0.0F;
            state->microphoneAnimation +=
                (microphoneTarget - state->microphoneAnimation) * 0.20F;
            const float autostartTarget = state->autostart ? 1.0F : 0.0F;
            state->autostartAnimation +=
                (autostartTarget - state->autostartAnimation) * 0.20F;
            const float autoBufferTarget = state->autoBuffer ? 1.0F : 0.0F;
            state->autoBufferAnimation +=
                (autoBufferTarget - state->autoBufferAnimation) * 0.20F;
            if (state->page == Page::editor) updateEditorPlayback(*state);
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
            if (wParam != 0 && !state->fullscreen) enterFullscreen(*state);
            else exitFullscreen(*state);
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
        if (state != nullptr && wParam == saveHotkeyId) saveClip(window, *state);
        else if (state != nullptr && wParam == stopHotkeyId && state->engine.isRunning()) {
            stopRecorder(window, *state);
        }
        return 0;
    case statusMessage:
        if (state != nullptr) {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            const bool clipSaved = text->starts_with(L"Klip zapisany:");
            setStatus(window, *state, std::move(*text));
            updateEditorVisibility(*state);
            if (clipSaved || state->page == Page::clips) refreshClips(window, *state);
        }
        return 0;
    case editorDoneMessage:
        if (state != nullptr) {
            std::unique_ptr<EditorResult> result(reinterpret_cast<EditorResult*>(lParam));
            if (result->success) {
                closeEditorPlayer(*state);
                state->page = Page::clips;
                refreshClips(window, *state);
                setStatus(window, *state, L"Zapisano: " + result->output.filename().wstring());
            } else {
                setStatus(window, *state, L"Błąd: " + result->message);
            }
        }
        return 0;
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
        if (state != nullptr && state->renderTarget != nullptr && LOWORD(lParam) > 0 && HIWORD(lParam) > 0) {
            state->renderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
            saveAccentColor(*state);
            closeEditorPlayer(*state);
            state->engine.stop();
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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = windowClassName;
    windowClass.hIconSm = windowClass.hIcon;
    RegisterClassExW(&windowClass);

    WNDCLASSEXW videoClass{sizeof(videoClass)};
    videoClass.style = CS_DBLCLKS;
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
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - static_cast<int>(windowWidth)) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - static_cast<int>(windowHeight)) / 2);
    const HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, windowClassName, L"NexPlay",
        WS_POPUP | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        x, y, static_cast<int>(windowWidth), static_cast<int>(windowHeight),
        nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        CoUninitialize();
        return 1;
    }
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
