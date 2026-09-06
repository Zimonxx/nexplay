#include "SaveToasts.h"
#include <wrl/client.h>
#include <cmath>
#include <map>
#include <stdexcept>

namespace nexplay::ui {
using Microsoft::WRL::ComPtr;
namespace {
constexpr float toastWidth = 360, toastHeight = 112;
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Cannot create save notification");
}
LRESULT CALLBACK overlayProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(window, message, wParam, lParam);
}
struct Overlay {
    HWND window{};
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ original{};
    int width{}, height{};
    double x{}, y{};
    bool positioned{};
    ~Overlay() {
        if (window)
            DestroyWindow(window);
        releaseBitmap();
        if (dc)
            DeleteDC(dc);
    }
    void releaseBitmap() {
        if (bitmap) {
            SelectObject(dc, original);
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
    }
    void resize(int w, int h) {
        if (bitmap && w == width && h == height)
            return;
        releaseBitmap();
        if (!dc)
            dc = CreateCompatibleDC(nullptr);
        if (!dc)
            throw std::runtime_error("Cannot create toast surface");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *pixels{};
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap)
            throw std::runtime_error("Cannot allocate toast surface");
        original = SelectObject(dc, bitmap);
        width = w;
        height = h;
    }
};
} // namespace
void drawSaveToast(ID2D1RenderTarget *target, IDWriteFactory *textFactory, const ToastEntry &entry,
                   D2D1_COLOR_F accent) {
    ComPtr<ID2D1SolidColorBrush> brush;
    check(target->CreateSolidColorBrush(D2D1::ColorF(0), &brush));
    const auto text = [&](const std::wstring &value, D2D1_RECT_F rect, float size,
                          DWRITE_FONT_WEIGHT weight, D2D1_COLOR_F color,
                          DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        ComPtr<IDWriteTextFormat> format;
        check(textFactory->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L"pl-PL", &format));
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(textFactory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)))
            format->SetTrimming(&trimming, ellipsis.Get());
        brush->SetColor(color);
        target->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format.Get(), rect,
                          brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const bool saved = entry.progress.phase == app::SavePhase::saved;
    const bool failed = entry.progress.phase == app::SavePhase::failed;
    const D2D1_COLOR_F foreground{0.96F, 0.97F, 0.99F, 1}, secondary{0.57F, 0.61F, 0.68F, 1};
    if (failed)
        accent = {1, 0.32F, 0.38F, 1};
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    brush->SetColor({0.045F, 0.050F, 0.063F, 1});
    target->FillRoundedRectangle(D2D1::RoundedRect({0.5F, 0.5F, 359.5F, 111.5F}, 14, 14),
                                 brush.Get());
    brush->SetColor({0.18F, 0.20F, 0.24F, 1});
    target->DrawRoundedRectangle(D2D1::RoundedRect({0.5F, 0.5F, 359.5F, 111.5F}, 14, 14),
                                 brush.Get());
    text(L"NEXPLAY  /  KLIP #" + std::to_wstring(entry.progress.id), {18, 11, 284, 29}, 10,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, secondary);
    text(std::to_wstring(entry.progress.percent) + L"%", {285, 14, 342, 37}, 15,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, failed ? secondary : accent, DWRITE_TEXT_ALIGNMENT_TRAILING);
    const wchar_t *title = saved    ? L"Zapisano klip"
                           : failed ? L"Nie udało się zapisać"
                                    : L"Zapisuję klip";
    text(title, {18, 34, 342, 60}, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD, foreground);
    std::wstring detail = entry.progress.detail;
    if (entry.progress.phase == app::SavePhase::queued)
        detail = L"Oczekiwanie na zapis…";
    else if (entry.progress.phase == app::SavePhase::preparing)
        detail = L"Przygotowywanie obrazu i dźwięku…";
    else if (entry.progress.phase == app::SavePhase::finalizing)
        detail = L"Finalizowanie pliku…";
    text(detail, {18, 61, 342, 80}, 11, DWRITE_FONT_WEIGHT_NORMAL, secondary);
    brush->SetColor({0.12F, 0.14F, 0.18F, 1});
    target->FillRoundedRectangle(D2D1::RoundedRect({18, 92, 342, 96}, 2, 2), brush.Get());
    if (entry.progress.percent > 0) {
        brush->SetColor(accent);
        target->FillRoundedRectangle(
            D2D1::RoundedRect({18, 92, 18 + 324 * entry.progress.percent / 100.0F, 96}, 2, 2),
            brush.Get());
    }
}

struct SaveToasts::Impl {
    SaveToastModel model;
    ToastCorner corner{ToastCorner::bottomRight};
    D2D1_COLOR_F accent{0.435F, 0.259F, 1, 1};
    HWND timer{};
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<IDWriteFactory> textFactory;
    std::map<app::SaveId, std::unique_ptr<Overlay>> windows;
    ULONGLONG previousTick{};
    ~Impl() {
        stop();
        if (timer)
            DestroyWindow(timer);
    }
    void stop() {
        if (timer)
            KillTimer(timer, 1);
        windows.clear();
        model = SaveToastModel{};
    }
    static LRESULT CALLBACK timerProcedure(HWND window, UINT message, WPARAM wParam,
                                           LPARAM lParam) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_TIMER && self) {
            try {
                self->tick();
            } catch (...) {
                self->stop();
            }
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    void initialize() {
        if (timer)
            return;
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown **>(textFactory.GetAddressOf())));
        auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        check(factory->CreateDCRenderTarget(&properties, &target));
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = timerProcedure;
        type.lpszClassName = L"NexPlaySaveToastTimer";
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Cannot register toast timer");
        type.lpfnWndProc = overlayProcedure;
        type.lpszClassName = L"NexPlaySaveToast";
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Cannot register toast overlay");
        timer = CreateWindowExW(0, L"NexPlaySaveToastTimer", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                nullptr, type.hInstance, this);
        if (!timer)
            throw std::runtime_error("Cannot create toast timer");
    }
    void tick() {
        const auto now = GetTickCount64();
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor))
            return;
        UINT dpi =
            windows.empty() ? GetDpiForSystem() : GetDpiForWindow(windows.begin()->second->window);
        dpi = std::max(96U, dpi);
        const int width = MulDiv(360, static_cast<int>(dpi), 96),
                  height = MulDiv(112, static_cast<int>(dpi), 96);
        const int margin = MulDiv(20, static_cast<int>(dpi), 96),
                  gap = MulDiv(10, static_cast<int>(dpi), 96);
        const auto &area = monitor.rcWork;
        const auto capacity =
            std::max(1L, (area.bottom - area.top - 2 * margin + gap) / (height + gap));
        model.advance(now, static_cast<std::size_t>(capacity));
        std::erase_if(windows, [&](const auto &pair) {
            return std::none_of(model.entries.begin(), model.entries.end(), [&](const auto &item) {
                return item.progress.id == pair.first && item.visible;
            });
        });
        const double smoothing =
            previousTick
                ? 1 - std::exp(-static_cast<double>(std::min<ULONGLONG>(now - previousTick, 100)) /
                               70.0)
                : 1;
        previousTick = now;
        int index = 0;
        for (const auto &entry : model.entries) {
            if (!entry.visible)
                continue;
            const auto pos = toastPlacement(corner, area.left, area.top, area.right, area.bottom,
                                            width, height, gap, margin, index++);
            auto &ptr = windows[entry.progress.id];
            if (!ptr) {
                ptr = std::make_unique<Overlay>();
                ptr->window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                                  WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                              L"NexPlaySaveToast", L"NexPlay — zapis klipu",
                                              WS_POPUP, pos.x, pos.y, width, height, nullptr,
                                              nullptr, GetModuleHandleW(nullptr), nullptr);
                if (!ptr->window)
                    throw std::runtime_error("Cannot create toast window");
                // The overlay is feedback for the user, not part of the captured recording.
                SetWindowDisplayAffinity(ptr->window, WDA_EXCLUDEFROMCAPTURE);
            }
            auto &overlay = *ptr;
            if (!overlay.positioned) {
                overlay.x = pos.x;
                overlay.y = pos.y;
                overlay.positioned = true;
            }
            overlay.x += (pos.x - overlay.x) * smoothing;
            overlay.y += (pos.y - overlay.y) * smoothing;
            overlay.resize(width, height);
            RECT bounds{0, 0, width, height};
            check(target->BindDC(overlay.dc, &bounds));
            target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0.0F));
            drawSaveToast(target.Get(), textFactory.Get(), entry, accent);
            check(target->EndDraw());
            const float opacity = entry.opacity(now);
            const bool right =
                corner == ToastCorner::topRight || corner == ToastCorner::bottomRight;
            const float offset = (right ? 1 : -1) *
                                 (1 - std::clamp((now - entry.entered) / 160.0F, 0.0F, 1.0F)) * 12;
            POINT destination{static_cast<LONG>(std::lround(overlay.x + offset)),
                              static_cast<LONG>(std::lround(overlay.y))};
            POINT origin{};
            SIZE size{width, height};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(opacity * 255)),
                                AC_SRC_ALPHA};
            if (!UpdateLayeredWindow(overlay.window, nullptr, &destination, &size, overlay.dc,
                                     &origin, 0, &blend, ULW_ALPHA))
                throw std::runtime_error("Cannot present toast");
            SetWindowPos(overlay.window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        if (model.entries.empty())
            KillTimer(timer, 1);
    }
};
SaveToasts::SaveToasts() = default;
SaveToasts::~SaveToasts() = default;
void SaveToasts::configure(ToastCorner corner, D2D1_COLOR_F accent) {
    if (!impl_)
        impl_ = std::make_unique<Impl>();
    impl_->corner = corner;
    impl_->accent = accent;
}
void SaveToasts::update(app::SaveProgress progress) {
    if (!impl_)
        impl_ = std::make_unique<Impl>();
    impl_->initialize();
    impl_->model.apply(std::move(progress));
    impl_->tick();
    if (!impl_->model.entries.empty())
        SetTimer(impl_->timer, 1, 16, nullptr);
}
void SaveToasts::close() { impl_.reset(); }
} // namespace nexplay::ui
