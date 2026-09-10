#include "app/FfmpegProgress.h"
#include "app/SaveProgress.h"
#include "app/Shortcut.h"
#include "ui/SaveToastModel.h"
#include <future>
#include <iostream>
#include <stdexcept>

using namespace nexplay;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void tests() {
    require(app::shortcutMatches(true, 119, 0, 119, 0), "Enabled shortcut did not match");
    require(!app::shortcutMatches(false, 119, 0, 119, 0), "Disabled shortcut fired");
    require(!app::shortcutMatches(true, 119, 2, 119, 0), "Wrong modifiers fired");
    constexpr auto rightShiftPageDown = app::shortcutModifiers(false, false, false, false, false, true);
    constexpr auto leftShiftPageDown = app::shortcutModifiers(false, false, false, false, true, false);
    require(app::shortcutMatches(true, 0x22, rightShiftPageDown, 0x22, rightShiftPageDown), "RShift+PageDown did not fire");
    require(!app::shortcutMatches(true, 0x22, rightShiftPageDown, 0x22, 0), "PageDown alone fired a chord");
    require(!app::shortcutMatches(true, 0x22, rightShiftPageDown, 0xa1, rightShiftPageDown), "Modifier alone fired a chord");
    require(!app::shortcutMatches(true, 0x22, rightShiftPageDown, 0x22, leftShiftPageDown), "LShift activated RShift binding");
    require(!app::shortcutMatches(true, 0x22, rightShiftPageDown, 0x22, rightShiftPageDown | 2 | app::leftControl), "Extra Ctrl activated a chord");
    require(!app::shortcutMatches(true, 0x22, rightShiftPageDown, 0x22, rightShiftPageDown | 8), "Windows modifier was ignored");
    require(app::shortcutMatches(true, 0x22, 4, 0x22, rightShiftPageDown) &&
        app::shortcutMatches(true, 0x22, 4, 0x22, leftShiftPageDown), "Legacy generic Shift binding stopped working");
    require(app::shortcutsOverlap(0x22, 4, 0x22, rightShiftPageDown), "Generic/sided conflict not detected");
    require(!app::shortcutsOverlap(0x22, leftShiftPageDown, 0x22, rightShiftPageDown), "Distinct sides treated as duplicates");
    require(app::physicalShortcutKey(0x10, 0x36, false) == 0xa1 &&
        app::physicalShortcutKey(0x10, 0x2a, false) == 0xa0 &&
        app::physicalShortcutKey(0x11, 0, true) == 0xa3 &&
        app::physicalShortcutKey(0x12, 0, true) == 0xa5, "Physical modifier decoding failed");
    app::FfmpegProgressParser parser(10);
    require(parser.line("out_time_us=5000000") == 49, "Incorrect media-time percentage");
    require(parser.line("out_time_us=N/A") == 49, "Invalid progress regressed");
    require(parser.line("out_time_us=1000000") == 49, "Progress moved backwards");
    require(parser.line("out_time_us=20000000") == 99, "Process must not report success early");
    require(parser.line("progress=end") == 99, "End record is not a successful process exit");
    app::FfmpegProgressParser empty(0);
    require(empty.line("out_time_us=99999999999") == 0, "Zero duration division");
    ui::SaveToastModel model;
    model.apply({1, app::SavePhase::queued, 0, {}});
    model.apply({2, app::SavePhase::queued, 0, {}});
    model.apply({3, app::SavePhase::queued, 0, {}});
    model.advance(1000, 2);
    require(model.entries.size() == 3 && model.entries[0].visible && model.entries[1].visible &&
                !model.entries[2].visible,
            "Concurrent notifications overwrite or overflow");
    model.apply({2, app::SavePhase::encoding, 65, {}});
    model.apply({1, app::SavePhase::encoding, 20, {}});
    require(model.entries[0].progress.percent == 20 && model.entries[1].progress.percent == 65,
            "Jobs mixed percentages");
    model.apply({1, app::SavePhase::encoding, 10, {}});
    require(model.entries[0].progress.percent == 20, "Toast progress regressed");
    model.apply({1, app::SavePhase::saved, 100, {}});
    model.apply({3, app::SavePhase::saved, 100, {}});
    model.advance(1500, 2);
    model.apply({1, app::SavePhase::encoding, 12, {}});
    require(model.entries[0].progress.phase == app::SavePhase::saved,
            "Late progress reopened completed toast");
    model.advance(3499, 2);
    require(model.entries[0].opacity(3499) == 1,
            "Saved text did not remain readable for two seconds");
    model.advance(3590, 2);
    require(model.entries[0].opacity(3590) > 0 && model.entries[0].opacity(3590) < 1,
            "No exit animation");
    model.advance(3680, 2);
    require(model.entries.size() == 2 && model.entries[0].progress.id == 2 &&
                model.entries[1].visible,
            "Toast stack did not close gaps");
    require(model.entries[1].finishedVisible == 3840,
            "Queued completion expired before it was visible");
    model.apply({1, app::SavePhase::saved, 100, {}});
    require(model.entries.size() == 2, "Late completion resurrected a toast");
    for (int corner = 0; corner < 4; ++corner) {
        auto first = ui::toastPlacement(static_cast<ui::ToastCorner>(corner), -1920, 0, 0, 1040,
                                        360, 112, 10, 20, 0);
        auto next = ui::toastPlacement(static_cast<ui::ToastCorner>(corner), -1920, 0, 0, 1040, 360,
                                       112, 10, 20, 1);
        require(first.x >= -1920 && first.x + 360 <= 0 && first.y >= 0 && first.y + 112 <= 1040,
                "Toast outside monitor");
        require(first.x == next.x && std::abs(first.y - next.y) == 122, "Toast stack overlaps");
    }
}
void processTest(const std::wstring &executable) {
    // Synthetic audio sent to FFmpeg's null muxer: no files, sound, or screen capture.
    const auto run = [&] {
        std::vector<int> progress;
        const auto exitCode = app::runFfmpegProgress(
            {L"\"" + executable + L"\"", L"-hide_banner", L"-loglevel", L"error", L"-nostats",
             L"-stats_period", L"0.1", L"-progress", L"pipe:1", L"-re", L"-f", L"lavfi", L"-i",
             L"\"sine=frequency=440:duration=0.8\"", L"-f", L"null", L"-"},
            0.8, [&](int value) { progress.push_back(value); });
        require(exitCode == 0 && progress.size() >= 3 && progress.back() > 80,
                "Missing live FFmpeg progress");
        require(std::is_sorted(progress.begin(), progress.end()), "Progress was not monotonic");
    };
    auto first = std::async(std::launch::async, run);
    auto second = std::async(std::launch::async, run);
    first.get();
    second.get();
    const auto failure =
        app::runFfmpegProgress({L"\"" + executable + L"\"", L"-nexplay-invalid-option"}, 1, {});
    require(failure != 0, "Failed process was reported as success");
}
int wmain(int argc, wchar_t **argv) {
    try {
        tests();
        if (argc == 2)
            processTest(argv[1]);
        std::cout << "Shortcut, toast lifecycle, stacking and save progress tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
