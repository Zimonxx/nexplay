#include "ui/SaveToasts.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
BOOL CALLBACK enumerate(HWND window, LPARAM parameter) {
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    wchar_t type[128]{};
    GetClassNameW(window, type, 128);
    if (process == GetCurrentProcessId() && std::wstring(type) == L"NexPlaySaveToast")
        reinterpret_cast<std::vector<HWND> *>(parameter)->push_back(window);
    return TRUE;
}
std::vector<HWND> overlays() {
    std::vector<HWND> windows;
    EnumWindows(enumerate, reinterpret_cast<LPARAM>(&windows));
    return windows;
}
void pump(unsigned milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (GetTickCount64() < end);
}
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        nexplay::ui::SaveToasts toasts;
        toasts.configure(nexplay::ui::ToastCorner::bottomRight, {0.435F, 0.259F, 1, 1});
        const HWND foreground = GetForegroundWindow();
        toasts.update(
            {1, nexplay::app::SavePhase::encoding, 37, L"Test powiadomienia — bez nagrywania"});
        toasts.update(
            {2, nexplay::app::SavePhase::encoding, 72, L"Drugi zapis testowy — bez dźwięku"});
        require(GetForegroundWindow() == foreground, "Toast stole foreground focus");
        pump(200);
        auto windows = overlays();
        require(windows.size() == 2, "Native toast overlay missing");
        std::vector<RECT> rects;
        for (auto window : windows) {
            const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
            require((style & (WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                              WS_EX_TOOLWINDOW)) ==
                        (WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                         WS_EX_TOOLWINDOW),
                    "Unsafe overlay window styles");
            require(IsWindowVisible(window) != FALSE, "Overlay is not visible");
            RECT rectangle{};
            GetWindowRect(window, &rectangle);
            rects.push_back(rectangle);
            require(rectangle.right > rectangle.left && rectangle.bottom > rectangle.top,
                    "Empty overlay");
            DWORD affinity{};
            require(GetWindowDisplayAffinity(window, &affinity) &&
                        affinity == WDA_EXCLUDEFROMCAPTURE,
                    "Toast not excluded from recording");
        }
        RECT overlap{};
        require(!IntersectRect(&overlap, &rects[0], &rects[1]), "Native toasts overlap");
        toasts.update(
            {1, nexplay::app::SavePhase::saved, 100, L"Test zakończony — nie utworzono pliku"});
        toasts.update(
            {2, nexplay::app::SavePhase::saved, 100, L"Test zakończony — nie utworzono pliku"});
        pump(1900);
        require(overlays().size() == 2, "Saved text disappeared before two seconds");
        pump(400);
        require(overlays().empty(), "Completed native toasts did not disappear");
        toasts.close();
        std::cout
            << "Native toast rendering, focus, stacking, capture exclusion and expiry passed.\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    return 0;
}
