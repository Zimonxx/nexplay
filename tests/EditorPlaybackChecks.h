#pragma once
#include <chrono>
#include <thread>

// Exercise the real editor startup and timer updates with silent synthetic audio.
// The parent stays hidden: no capture, hotkeys, settings or user clips are involved.
inline void validateEditorPlayback(const std::filesystem::path& folder,
    const std::filesystem::path& existing = {}, const bool nvenc = false) {
    std::filesystem::create_directories(folder);
    const auto clip = existing.empty() ? folder / L"Silent playback fixture.mp4" : existing;
    if (existing.empty()) editorRequire(runHiddenProcess({L"ffmpeg.exe", L"-v", L"error", L"-y",
        L"-f", L"lavfi", L"-i", nvenc ? L"testsrc2=size=160x90:rate=120:duration=6" : L"testsrc2=size=160x90:rate=30:duration=6",
        L"-f", L"lavfi", L"-i", L"sine=frequency=440:sample_rate=48000:duration=6",
        L"-map", L"0:v", L"-map", L"1:a", L"-map", L"1:a",
        L"-c:v", nvenc ? L"h264_nvenc" : L"mpeg4", L"-g", nvenc ? L"240" : L"30",
        L"-c:a", L"aac", L"-af", L"volume=0", L"-t", L"6",
        L"-movflags", L"+faststart",
        quoteProcessArgument(clip.wstring())}) == 0, "Cannot generate playback fixture");
    IO_COUNTERS initialIo{};
    GetProcessIoCounters(GetCurrentProcess(), &initialIo);
    computeLayout(1240, 820);
    AppState state;
    state.mainWindow = CreateWindowExW(0, L"STATIC", L"NexPlay playback test", WS_OVERLAPPED,
        0, 0, 1240, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    editorRequire(state.mainWindow != nullptr, "Cannot create isolated playback window");
    struct Cleanup {
        AppState& state;
        ~Cleanup() { closeEditorPlayer(state); DestroyWindow(state.mainWindow); }
    } cleanup{state};
    state.videoWindow = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
        0, 0, 160, 90, state.mainWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
    openEditor(state.mainWindow, state, clip);
    if (!existing.empty()) for (auto& track : state.editorAudioTracks) track.included = false;
    const auto pump = [&](int milliseconds) {
        const auto until = GetTickCount64() + milliseconds;
        while (GetTickCount64() < until) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            updateEditorPlayback(state);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    };
    pump(1500);
    IO_COUNTERS currentIo{};
    GetProcessIoCounters(GetCurrentProcess(), &currentIo);
    const auto readBytes = currentIo.ReadTransferCount - initialIo.ReadTransferCount;
    std::cout << "Preview bytes read: " << readBytes << '\n';
    std::cout << "Editor startup: duration=" << state.editorDuration
              << " position=" << state.playPosition << " playing=" << state.playing
              << " tracks=" << state.previewAudio.size() << " muted="
              << state.previewAudio.muted(0) << ',' << state.previewAudio.muted(1)
              << '\n';
    std::wcout << L"Editor status: " << state.status << L'\n';
    editorRequire(state.playing && state.playPosition > 0.2 && state.editorDuration > 5,
        "Editor playback did not start");
    if (existing.empty()) editorRequire(state.previewAudio.size() == 2 && state.previewAudio.ready() &&
        !state.previewAudio.muted(0) && !state.previewAudio.muted(1),
        "Freshly opened editor has silent audio players");
    for (std::size_t index = 0; index < state.previewAudio.size(); ++index) {
        std::cout << "Audio " << index << " playing=" << state.previewAudio.playing(index)
                  << " position=" << state.previewAudio.position(index) << '\n';
    }
    editorRequire(state.previewAudio.size() == state.editorAudioTracks.size() &&
        !state.editorAudioTracks.empty(), "Audio tracks were not initialized");
    if (!existing.empty()) editorRequire(readBytes < std::filesystem::file_size(clip) / 2,
        "Audio preview scanned a large portion of the original MP4 before playback");
    for (std::size_t index = 0; index < state.previewAudio.size(); ++index)
        editorRequire(state.previewAudio.playing(index) && state.previewAudio.position(index) > 0.2,
            "Unmuted audio player is not actually playing");
    toggleEditorPlayback(state);
    pump(180);
    for (std::size_t i = 0; i < state.previewAudio.size(); ++i)
        editorRequire(!state.previewAudio.playing(i), "Pause left an audio player running");
    // Regression: seek to EOF while paused, then scrub back rapidly. The timer
    // must never overwrite the user's new position with the stale EOF clock.
    seekEditor(state, state.editorDuration);
    pump(250);
    for (const double destination : {2.1, 4.1, 1.2, 3.4, 2.5}) {
        seekEditor(state, destination);
        updateEditorPlayback(state);
        editorRequire(std::abs(state.playPosition - destination) < 0.05,
            "Timeline jumped away from the latest seek before the decoder completed it");
    }
    pump(500);
    editorRequire(std::abs(state.playPosition - 2.5) < 0.05,
        "Paused scrub did not settle on its latest requested position");
    const auto actualVideoPosition = [&] {
        PROPVARIANT value{};
        const auto hr = state.mediaPlayer->GetPosition(MFP_POSITIONTYPE_100NS, &value);
        const double result = SUCCEEDED(hr) ? variantSeconds(value) : -1;
        PropVariantClear(&value);
        return result;
    };
    editorRequire(!state.embeddedSeek.busy() && std::abs(actualVideoPosition() - 2.5) < 0.05,
        "Timeline looked correct but the actual video decoder did not seek");
    seekEditor(state, state.editorDuration);
    pump(300);
    editorRequire(!state.embeddedSeek.busy() && actualVideoPosition() < state.editorDuration &&
        actualVideoPosition() > state.editorDuration - 0.1,
        "End seek did not display the last video frame");
    seekEditor(state, 1.5);
    toggleEditorPlayback(state); // Play while POSITION_SET has not arrived yet.
    pump(450);
    editorRequire(state.playing && actualVideoPosition() > 1.6 && actualVideoPosition() < 2.2,
        "Play during an asynchronous seek was lost");
    toggleEditorPlayback(state);
    pump(120);
    std::cout << "EOF, rapid paused scrubbing, actual decoder positions and play-during-seek passed.\n";
    seekEditor(state, 2.5);
    pump(100);
    toggleEditorPlayback(state);
    pump(350);
    for (std::size_t i = 0; i < state.previewAudio.size(); ++i)
        editorRequire(state.previewAudio.playing(i) &&
            std::abs(state.previewAudio.position(i) - state.playPosition) < 0.2,
            "Seek/resume lost audio synchronization");
    if (existing.empty()) {
        state.trimStart = 2;
        state.trimEnd = 4;
        state.editorAudioTracks[0].included = false;
        updatePreviewAudio(state);
        editorRequire(state.previewAudio.muted(0) && !state.previewAudio.muted(1),
            "Muting one track muted the other");
        removeEditorAudioTrack(state, 0);
        editorRequire(state.previewAudio.muted(0) && !state.previewAudio.muted(1),
            "Deleting one track muted the survivor");
        restoreEditorAudioTrack(state);
        state.cutEditorSelection = true;
        seekEditor(state, 3);
        pump(150);
        editorRequire(state.previewAudio.muted(0) && state.previewAudio.muted(1),
            "Audio remained audible inside the video cut");
        seekEditor(state, 4.5);
        pump(150);
        editorRequire(state.previewAudio.muted(0) && !state.previewAudio.muted(1),
            "Audio failed to return beyond the cut");
    }
    if (state.playing) toggleEditorPlayback(state);
    pump(100);
    seekEditor(state, state.editorDuration);
    // Simulate changing video surfaces before the old player's seek completes.
    // Both windows stay hidden, and the real fullscreen transport path is used.
    state.fullscreenSeek.reset();
    const HWND fullscreenSurface = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
        0, 0, 160, 90, state.mainWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
    editorRequire(SUCCEEDED(MFPCreateMediaPlayer(clip.c_str(), FALSE,
        MFP_OPTION_FREE_THREADED_CALLBACK, state.fullscreenSeek.callback(),
        fullscreenSurface, &state.fullscreenPlayer)), "Cannot prepare fullscreen transport fixture");
    state.fullscreenPlayer->SetMute(TRUE);
    state.fullscreen = true;
    seekEditor(state, 1.75);
    pump(350);
    PROPVARIANT fullscreenPosition{};
    MFP_MEDIAPLAYER_STATE fullscreenState{};
    state.fullscreenPlayer->GetState(&fullscreenState);
    std::cout << "Fullscreen seek: busy=" << state.fullscreenSeek.busy() << " failure="
              << std::hex << state.fullscreenSeek.failure() << std::dec << " state=" << fullscreenState << '\n';
    editorRequire(SUCCEEDED(state.fullscreenPlayer->GetPosition(MFP_POSITIONTYPE_100NS, &fullscreenPosition)),
        "Cannot read fullscreen position");
    const auto fullscreenSeconds = variantSeconds(fullscreenPosition);
    PropVariantClear(&fullscreenPosition);
    editorRequire(!state.fullscreenSeek.busy() && std::abs(fullscreenSeconds - 1.75) < 0.05 &&
        std::abs(state.playPosition - 1.75) < 0.05, "Old embedded seek interfered with fullscreen playback");
    state.fullscreenPlayer->Shutdown();
    state.fullscreenPlayer.Reset();
    DestroyWindow(fullscreenSurface);
    state.fullscreen = false;
    seekEditor(state, 2.25);
    pump(350);
    editorRequire(!state.embeddedSeek.busy() && std::abs(actualVideoPosition() - 2.25) < 0.05,
        "Return from fullscreen reused a stale seek completion");
    seekEditor(state, 3.25);
    closeEditorPlayer(state);
    editorRequire(!state.embeddedSeek.busy(), "Closing the clip left a pending seek behind");
    std::cout << "Startup, audio clocks, pause, seek/resume, edit mutes and fullscreen transport transitions passed.\n";
}
