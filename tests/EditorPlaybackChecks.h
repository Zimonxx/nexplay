#pragma once
#include <chrono>
#include <thread>

// Exercise the real editor startup and timer updates with silent synthetic audio.
// The parent stays hidden: no capture, hotkeys, settings or user clips are involved.
inline void validateEditorPlayback(const std::filesystem::path& folder,
    const std::filesystem::path& existing = {}) {
    std::filesystem::create_directories(folder);
    const auto clip = existing.empty() ? folder / L"Silent playback fixture.mp4" : existing;
    if (existing.empty()) editorRequire(runHiddenProcess({L"ffmpeg.exe", L"-v", L"error", L"-y",
        L"-f", L"lavfi", L"-i", L"testsrc2=size=160x90:rate=30:duration=6",
        L"-f", L"lavfi", L"-i", L"sine=frequency=440:sample_rate=48000:duration=6",
        L"-map", L"0:v", L"-map", L"1:a", L"-map", L"1:a",
        L"-c:v", L"mpeg4", L"-c:a", L"aac", L"-af", L"volume=0", L"-t", L"6",
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
    std::cout << "Startup, real audio clocks, pause, seek/resume and edit mutes passed.\n";
}
