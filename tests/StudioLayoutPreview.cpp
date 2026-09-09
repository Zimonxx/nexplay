// Render the actual UI offscreen, without capture, hooks, registry writes or a window.
#include "../src/gui_main.cpp"
#include <iostream>
#include "EditorExportChecks.h"
#include "EditorPlaybackChecks.h"

namespace {
// An isolated, never-shown test window. Route only non-client messages through
// the production procedure: no app startup, capture, settings, tray or hotkeys.
LRESULT CALLBACK frameTestProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCALCSIZE || message == WM_NCHITTEST || message == WM_NCACTIVATE)
        return windowProcedure(window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}
void validateNativeFrame() {
    WNDCLASSW type{};
    type.lpfnWndProc = frameTestProcedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"NexPlayIsolatedFrameTest";
    if (!RegisterClassW(&type))
        throw std::runtime_error("Cannot register frame test");
    computeLayout(1240, 820);
    HWND window = CreateWindowExW(mainWindowExStyle, type.lpszClassName, L"Frame geometry test",
                                  mainWindowStyle, 100, 100, 1240, 820, nullptr, nullptr,
                                  type.hInstance, nullptr);
    if (!window)
        throw std::runtime_error("Cannot create hidden frame test");
    const auto coversWholeWindow = [&] {
        RECT outer{}, client{};
        GetWindowRect(window, &outer);
        GetClientRect(window, &client);
        POINT origin{};
        ClientToScreen(window, &origin);
        return origin.x == outer.left && origin.y == outer.top &&
               client.right == outer.right - outer.left &&
               client.bottom == outer.bottom - outer.top;
    };
    const auto hit = [&](int x, int y) {
        POINT point{x, y};
        ClientToScreen(window, &point);
        return SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
    };
    bool valid = coversWholeWindow();
    initializeCustomFrame(window);
    valid = valid && coversWholeWindow() && hit(1170, 22) == HTCLIENT &&
            hit(500, 22) == HTCAPTION && hit(2, 400) == HTLEFT;
    SetWindowPos(window, nullptr, 0, 0, 1400, 900, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    valid = valid && coversWholeWindow();
    DestroyWindow(window);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    if (!valid)
        throw std::runtime_error("Native title bar space or hit targets leaked into custom frame");
    std::cout << "Native frame geometry passed at creation and resize; custom buttons remain "
                 "client controls.\n";
}
// A lossless in-memory test bitmap: its circle exposes stretched thumbnails.
// This is fixture data, not a bundled application asset or a user's recording.
std::vector<std::uint8_t> previewFixture(const int width, const int height) {
    const int stride = (width * 3 + 3) & ~3;
    std::vector<std::uint8_t> bytes(54 + stride * height);
    const auto put = [&](const int offset, const std::uint32_t value, const int count) {
        for (int i = 0; i < count; ++i)
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    };
    bytes[0] = 'B';
    bytes[1] = 'M';
    put(2, static_cast<std::uint32_t>(bytes.size()), 4);
    put(10, 54, 4);
    put(14, 40, 4);
    put(18, width, 4);
    put(22, height, 4);
    put(26, 1, 2);
    put(28, 24, 2);
    const int radius = std::min(width, height) / 3;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool circle =
                (x - width / 2) * (x - width / 2) + (y - height / 2) * (y - height / 2) <
                radius * radius;
            const std::size_t pixel = 54 + y * stride + x * 3;
            bytes[pixel] = circle ? 152 : 35;
            bytes[pixel + 1] = circle ? 178 : 31;
            bytes[pixel + 2] = circle ? 88 : 27;
        }
    }
    return bytes;
}
void check(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("UI preview render failed");
}
std::vector<BYTE> decodedPixels(IWICImagingFactory *factory,
                                const std::vector<std::uint8_t> &encoded) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateStream(&stream));
    check(stream->InitializeFromMemory(const_cast<BYTE *>(encoded.data()),
                                       static_cast<DWORD>(encoded.size())));
    check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                           &decoder));
    check(decoder->GetFrame(0, &frame));
    check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom));
    UINT width{}, height{};
    check(converter->GetSize(&width, &height));
    std::vector<BYTE> pixels(width * height * 4);
    check(
        converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    return pixels;
}
void validateFrameDecoder(const std::filesystem::path &clip) {
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)));
    const auto metadata = generateThumbnail(clip, 0, 0, 0);
    if (metadata.duration <= 0 || metadata.framesPerSecond <= 0)
        throw std::runtime_error("No fixture metadata");
    const int last = static_cast<int>(metadata.duration * metadata.framesPerSecond) - 1;
    for (int requested : {0, 1, 29, 60, last - 1, last}) {
        const auto actual =
            generateThumbnail(clip, requested, metadata.duration, metadata.framesPerSecond);
        const int offset = requested - actual.firstFrameIndex;
        if (offset < 0 || offset >= static_cast<int>(actual.frames.size()))
            throw std::runtime_error("Requested preview frame was not decoded");
        // Full linear decoding with an explicit frame index is the independent reference.
        const auto reference = splitMjpegFrames(runHiddenProcessCapture(
            {L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"quiet", L"-i",
             quoteProcessArgument(clip.wstring()), L"-vf",
             quoteProcessArgument(L"select=eq(n\\," + std::to_wstring(requested) +
                                  L"),"
                                  L"scale=352:198:force_original_aspect_ratio=decrease:force_"
                                  L"divisible_by=2,pad=352:198:(ow-iw)/2:(oh-ih)/2"),
             L"-frames:v", L"1", L"-an", L"-q:v", L"3", L"-f", L"image2pipe", L"-vcodec", L"mjpeg",
             L"pipe:1"}));
        if (reference.empty() || decodedPixels(factory.Get(), actual.frames[offset]) !=
                                     decodedPixels(factory.Get(), reference.front()))
            throw std::runtime_error("Preview frame differs from linear decode: " +
                                     std::to_string(requested));
    }
    std::cout << "Fast-seek preview frames match linear decoding, including the last frame.\n";
}
void savePng(AppState &state, IWICBitmap *bitmap, const std::filesystem::path &path) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    check(state.wicFactory->CreateStream(&stream));
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    check(state.wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    check(encoder->CreateNewFrame(&frame, nullptr));
    check(frame->Initialize(nullptr));
    check(frame->WriteSource(bitmap, nullptr));
    check(frame->Commit());
    check(encoder->Commit());
}
bool containsRect(Rect parent, Rect child) {
    return parent.contains(child.left, child.top) && parent.contains(child.right, child.bottom) &&
           child.right > child.left && child.bottom > child.top;
}
void validateLayout() {
    for (const auto& corner : toastCornerRects)
        if (!containsRect(notificationsPanelRect, corner))
            throw std::runtime_error("Toast corner selector outside settings panel");
    if (!containsRect(saveHotkeyRect, saveHotkeyToggleRect) ||
        !containsRect(stopHotkeyRect, stopHotkeyToggleRect) ||
        colorPanelRect.bottom >= notificationsPanelRect.top)
        throw std::runtime_error("Shortcut toggles or notification panel overlap");
    if (!containsRect(replayHeroRect, startRect) || !containsRect(replayHeroRect, saveRect) ||
        !containsRect(qualityPanelRect, durationFieldRect) ||
        !containsRect(qualityPanelRect, resolutionFieldRect) ||
        !containsRect(qualityPanelRect, fpsFieldRect) ||
        !containsRect(qualityPanelRect, bitrateFieldRect) ||
        !containsRect(colorPanelRect, accentPlaneRect) ||
        !containsRect(colorPanelRect, accentPreviewRect) ||
        !containsRect(colorPanelRect, accentHueRect) ||
        !containsRect(hotkeysPanelRect, stopHotkeyRect) ||
        !containsRect(inspectorRect, editorNameRect) ||
        !containsRect(inspectorRect, editorCutModeRect) ||
        !containsRect(previewPanelRect, videoSurfaceRect) ||
        !containsRect(timelinePanelRect, editorTimelineRect)) {
        throw std::runtime_error("A control is outside its panel");
    }
    for (int i = 0; i < clipColumns * clipVisibleRows; ++i) {
        if (!containsRect(clipsListRect, clipCardRect(i)))
            throw std::runtime_error("Clip grid overflow");
    }
    for (int i = 0; i < editorVisibleTracks; ++i) {
        const auto r = editorAudioTimelineRect(i);
        if (!containsRect(editorAudioRowsRect, r) || r.left != editorTimelineRect.left ||
            r.right != editorTimelineRect.right)
            throw std::runtime_error("Audio and video time axes are misaligned");
        const auto remove = editorAudioDeleteRect(i);
        if (!containsRect(editorAudioRowsRect, remove) || remove.right >= r.left ||
            remove.right - remove.left != 28 || remove.bottom - remove.top != 28)
            throw std::runtime_error("Audio delete button overlaps or stretches");
    }
    if (std::abs((accentPlaneRect.right - accentPlaneRect.left) -
                 (accentPlaneRect.bottom - accentPlaneRect.top)) > 0.01F)
        throw std::runtime_error("Color picker was stretched");
    for (float ratio : {16.0F / 9, 9.0F / 16, 21.0F / 9, 1.0F}) {
        const Rect fitted = aspectFitRect(nullptr, clipCardRect(0), ratio);
        if (!containsRect(clipCardRect(0), fitted) ||
            std::abs((fitted.right - fitted.left) / (fitted.bottom - fitted.top) - ratio) > 0.001F)
            throw std::runtime_error("Thumbnail aspect ratio was not preserved");
    }
    for (double duration : {0.3, 2.0, 10.0, 59.94, 1200.0}) {
        const double interval =
            timelineTickInterval(duration, editorTimelineRect.right - editorTimelineRect.left);
        if (!std::isfinite(interval) || interval <= 0 || duration / interval > 20)
            throw std::runtime_error("Invalid timeline ruler spacing");
    }
}
void validateHitTargets(AppState &state) {
    const auto checkTarget = [&](Rect rectangle, HitTarget expected) {
        if (hitTest(state, (rectangle.left + rectangle.right) / 2,
                    (rectangle.top + rectangle.bottom) / 2) != expected)
            throw std::runtime_error("Painted control and pointer target do not match");
    };
    checkTarget(maximizeRect, HitTarget::maximize);
    if (state.page == Page::replay) {
        checkTarget(startRect, HitTarget::startStop);
        checkTarget(fpsFieldRect, HitTarget::fpsField);
        checkTarget(resolutionFieldRect, HitTarget::resolutionField);
    } else if (state.page == Page::settings) {
        checkTarget(autostartRect, HitTarget::autostartToggle);
        checkTarget(saveHotkeyRect, HitTarget::saveHotkey);
        checkTarget(saveHotkeyToggleRect, HitTarget::saveHotkeyToggle);
        checkTarget(stopHotkeyToggleRect, HitTarget::stopHotkeyToggle);
        for (int i = 0; i < 4; ++i)
            checkTarget(toastCornerRects[i],
                        static_cast<HitTarget>(static_cast<int>(HitTarget::toastTopLeft) + i));
        checkTarget(accentPlaneRect, HitTarget::accentPlane);
        checkTarget(accentHueRect, HitTarget::accentHue);
    } else if (state.page == Page::editor) {
        checkTarget(editorNameRect, HitTarget::editorName);
        checkTarget(editorFullscreenRect, HitTarget::editorFullscreen);
        checkTarget(editorSaveRect, HitTarget::editorSave);
        state.editorExporting = true;
        if (hitTest(state, editorSaveRect.left + 30, editorSaveRect.top + 16) == HitTarget::editorSave)
            throw std::runtime_error("Export button allowed a duplicate job");
        state.editorExporting = false;
    } else {
        checkTarget(openClipsRect, HitTarget::openClips);
        for (int i = 0;
             i < std::min(static_cast<int>(state.clips.size()), clipColumns * clipVisibleRows);
             ++i) {
            const Rect r = clipCardRect(i);
            if (clipIndexAt(state, (r.left + r.right) / 2, (r.top + r.bottom) / 2) != i)
                throw std::runtime_error("Clip grid hit target does not match the card");
        }
    }
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 2 && argc != 3)
        return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 3;
    int result = 0;
    try {
        validateEditorTrackEdits();
        if (argc == 3) {
            if (std::wstring(argv[2]) == L"--export-tests")
                validateEditorExports(std::filesystem::path(argv[1]) / L"export-fixtures");
            else if (std::wstring(argv[2]) == L"--playback-tests")
                validateEditorPlayback(std::filesystem::path(argv[1]) / L"playback-fixtures");
            else if (std::wstring(argv[2]).starts_with(L"--muted-playback="))
                validateEditorPlayback(std::filesystem::path(argv[1]) / L"playback-fixtures",
                    std::wstring(argv[2]).substr(17));
            else if (std::wstring(argv[2]) == L"--window-frame")
                validateNativeFrame();
            else
                validateFrameDecoder(argv[2]);
        }
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        for (float width : {1080.0F, 1240.0F, 1920.0F, 2560.0F}) {
            for (float height : {740.0F, 820.0F, 1080.0F, 1440.0F}) {
                computeLayout(width, height);
                validateLayout();
            }
        }
        for (auto size : {SIZE{1080, 740}, SIZE{1240, 820}, SIZE{1920, 1080}}) {
            computeLayout(static_cast<float>(size.cx), static_cast<float>(size.cy));
            validateLayout();
            AppState state;
            state.stopHotkeyEnabled = false;
            state.status = L"Podgląd interfejsu · dane testowe";
            for (int i = 0; i < 8; ++i) {
                state.audioRows.push_back(
                    {.processId = static_cast<DWORD>(100 + i),
                     .name = std::array{L"FiveM", L"Discord", L"Spotify", L"Przeglądarka"}[i % 4]});
                state.editorAudioTracks.push_back({.streamIndex = i,
                                                   .name = state.audioRows.back().name,
                                                   .start = i == 1 ? 2.0 : 0.0,
                                                   .end = 10});
                ClipRow clip;
                clip.path = L"Sesja " + std::to_wstring(i + 1) + L" — wieczorna rozgrywka.mp4";
                clip.size = 34'000'000;
                if (i < 6) {
                    clip.preview = std::make_shared<ClipPreview>();
                    clip.preview->duration = 10;
                    clip.preview->framesPerSecond = 60;
                    clip.preview->frames.emplace(0, i % 2 == 0 ? previewFixture(320, 180)
                                                               : previewFixture(180, 320));
                } else {
                    state.thumbnailFailures.insert({clip.path, 0});
                }
                state.clips.push_back(std::move(clip));
            }
            state.editorDuration = 10;
            state.trimEnd = 10;
            state.playPosition = 3.2;
            state.editorName = L"Najlepszy moment-edit";
            check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&state.wicFactory)));
            ComPtr<IWICBitmap> bitmap;
            check(state.wicFactory->CreateBitmap(
                static_cast<UINT>(size.cx), static_cast<UINT>(size.cy),
                GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap));
            ensureGraphics(nullptr, state, bitmap.Get());
            for (auto page : {Page::replay, Page::clips, Page::settings, Page::editor}) {
                state.page = page;
                validateHitTargets(state);
                state.renderTarget->BeginDraw();
                state.renderTarget->Clear(background);
                drawScene(state);
                if (page == Page::editor) {
                    fillRounded(state, videoSurfaceRect, 0, {0.016F, 0.018F, 0.022F, 1});
                    drawCenteredText(state, L"Podgląd wideo",
                                     {videoSurfaceRect.left, videoSurfaceRect.top,
                                      videoSurfaceRect.right, videoSurfaceRect.bottom},
                                     state.bodyFormat.Get(), muted);
                }
                check(state.renderTarget->EndDraw());
                const std::wstring name = std::to_wstring(size.cx) + L"-" +
                                          std::array{L"replay", L"library", L"editor",
                                                     L"settings"}[static_cast<int>(page)] +
                                          L".png";
                savePng(state, bitmap.Get(), folder / name);
            }
            if (size.cx == 1240) {
                state.page = Page::editor;
                state.trimStart = 2;
                state.trimEnd = 8;
                state.editorExporting = true;
                state.editorExportPercent = 42;
                for (bool cut : {false, true}) {
                    state.cutEditorSelection = cut;
                    state.renderTarget->BeginDraw();
                    state.renderTarget->Clear(background);
                    drawScene(state);
                    check(state.renderTarget->EndDraw());
                    savePng(state, bitmap.Get(), folder / (cut ? L"editor-cut-export.png" : L"editor-trim-export.png"));
                }
                state.editorExporting = false;
                for (bool audioDialog : {false, true}) {
                    state.page = audioDialog ? Page::replay : Page::clips;
                    state.clipContextMenuOpen = !audioDialog;
                    state.clipContextMenuClip = state.clips.front().path;
                    state.clipContextMenuRect = {560, 270, 890, 528};
                    state.audioGroupDialogOpen = audioDialog;
                    state.audioGroupName = L"Dźwięk gry";
                    state.activeField = HitTarget::audioGroupName;
                    state.renderTarget->BeginDraw();
                    state.renderTarget->Clear(background);
                    drawScene(state);
                    check(state.renderTarget->EndDraw());
                    savePng(state, bitmap.Get(),
                            folder /
                                (audioDialog ? L"1240-audio-group.png" : L"1240-context-menu.png"));
                }
            }
        }
        {
            AppState state;
            check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&state.wicFactory)));
            ComPtr<IWICBitmap> bitmap;
            check(state.wicFactory->CreateBitmap(400, 396, GUID_WICPixelFormat32bppPBGRA,
                                                 WICBitmapCacheOnLoad, &bitmap));
            ensureGraphics(nullptr, state, bitmap.Get());
            state.renderTarget->BeginDraw();
            state.renderTarget->Clear(background);
            const std::array<nexplay::ui::ToastEntry, 3> entries{
                {{{1, nexplay::app::SavePhase::encoding, 37, L"NexPlay-20260906-rozgrywka.mp4"}},
                 {{2, nexplay::app::SavePhase::saved, 100, L"Najlepszy moment.mp4"}},
                 {{3, nexplay::app::SavePhase::failed, 82, L"Brak miejsca na dysku."}}}};
            for (int i = 0; i < 3; ++i) {
                state.renderTarget->SetTransform(
                    D2D1::Matrix3x2F::Translation(20, 20.0F + i * 122));
                nexplay::ui::drawSaveToast(state.renderTarget.Get(), state.writeFactory.Get(),
                                           entries[i], primary);
            }
            state.renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
            check(state.renderTarget->EndDraw());
            savePng(state, bitmap.Get(), folder / L"save-toasts.png");
        }
        std::cout << "Layout and aspect checks passed at 16 sizes. Pointer checks passed on all "
                     "pages. Rendered 12 page previews and 2 dialog previews.\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        result = 1;
    }
    CoUninitialize();
    return result;
}
