#pragma once
#include <fstream>

// Included by the offscreen tool to exercise the actual editor implementation.
inline void editorRequire(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
inline void validateEditorTrackEdits() {
    AppState state;
    state.editorDuration = 10;
    state.trimStart = 2;
    state.trimEnd = 8;
    state.editorAudioTracks = {{1, L"Game", true, 0, 10, 42},
                               {3, L"Browser", true, 3, 7, 13},
                               {2, L"Microphone", false, 0, 10, 7}};
    const auto full = editorAudioBounds(state, state.editorAudioTracks[0]);
    editorRequire(full.start == 2 && full.end == 8, "Visible audio trim differs from video");
    const auto individual = editorAudioBounds(state, state.editorAudioTracks[1]);
    editorRequire(individual.start == 3 && individual.end == 7, "Individual trim not retained");
    state.cutEditorSelection = true;
    editorRequire(editorAudioBounds(state, state.editorAudioTracks[1]).duration() == 0,
                  "Invisible audio still exposes trim handles inside the removed span");
    state.cutEditorSelection = false;
    removeEditorAudioTrack(state, 0);
    editorRequire(state.editorAudioTracks.size() == 2 && state.editorAudioTracks[0].trackId == 13 &&
                      state.editorAudioTracks[0].streamIndex == 3,
                  "Delete renumbered source identity");
    removeEditorAudioTrack(state, 1);
    restoreEditorAudioTrack(state);
    restoreEditorAudioTrack(state);
    editorRequire(state.editorAudioTracks.size() == 3 && state.editorAudioTracks[0].trackId == 42 &&
                      state.editorAudioTracks[2].trackId == 7 &&
                      !state.editorAudioTracks[2].included,
                  "Undo did not restore order and mute state");
    state.trimStart = 0;
    state.trimEnd = 10;
    const auto expanded = editorAudioBounds(state, state.editorAudioTracks[0]);
    editorRequire(expanded.start == 0 && expanded.end == 10, "Video trim destroyed source range");
    while (!state.editorAudioTracks.empty())
        removeEditorAudioTrack(state, 0);
    editorRequire(state.editorAudioScroll == 0, "Deleting all tracks left an invalid scroll");
    std::cout << "Editor linked bounds, delete/undo and stable identities passed.\n";
}

inline void checkExportTone(const std::filesystem::path &clip, int stream, double seconds,
                            double frequency) {
    const auto pcm = runHiddenProcessCapture(
        {L"ffmpeg.exe", L"-v", L"error", L"-ss", secondsArgument(seconds), L"-i",
         quoteProcessArgument(clip.wstring()), L"-map", L"0:a:" + std::to_wstring(stream), L"-t",
         L"0.2", L"-ac", L"1", L"-ar", L"48000", L"-f", L"s16le", L"pipe:1"});
    editorRequire(pcm.size() >= 15000, "Audio stream is missing or shorter than video");
    int crossings = 0, maximum = 0, previous = 0;
    for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
        const auto sample = static_cast<std::int16_t>(
            static_cast<unsigned char>(pcm[i]) | (static_cast<unsigned char>(pcm[i + 1]) << 8));
        maximum = std::max(maximum, std::abs(static_cast<int>(sample)));
        if (previous <= 0 && sample > 0)
            ++crossings;
        previous = sample;
    }
    if (frequency == 0)
        editorRequire(maximum < 8, "Muted/trimmed audio is not silent");
    else {
        const auto measured = crossings * 48000.0 / (pcm.size() / 2);
        editorRequire(maximum > 100 && std::abs(measured - frequency) < 25,
                      "Export audio has the wrong source or timeline position");
    }
}

inline void validateEditorExports(const std::filesystem::path &folder) {
    std::filesystem::create_directories(folder);
    const auto source = folder / L"GPU source with spaces.mp4";
    std::vector<std::wstring> arguments{
        L"ffmpeg.exe", L"-v",    L"error", L"-y",
        L"-f",         L"lavfi", L"-i",    L"testsrc2=size=640x360:rate=60:duration=4"};
    for (int frequency : {440, 660, 880, 1100, 1760})
        arguments.insert(arguments.end(),
                         {L"-f", L"lavfi", L"-i",
                          L"sine=frequency=" + std::to_wstring(frequency) +
                              (frequency == 1760 ? L":duration=4" : L":duration=1")});
    arguments.insert(arguments.end(),
                     {L"-filter_complex", L"[1:a][2:a][3:a][4:a]concat=n=4:v=0:a=1[tone]", L"-map",
                      L"0:v", L"-map", L"[tone]", L"-map", L"5:a", L"-c:v", L"h264_nvenc", L"-c:a",
                      L"aac", L"-metadata:s:a:0", quoteProcessArgument(L"handler_name=Game track"),
                      L"-metadata:s:a:1", quoteProcessArgument(L"handler_name=Browser track"),
                      quoteProcessArgument(source.wstring())});
    editorRequire(runHiddenProcess(arguments) == 0, "Cannot generate synthetic NVENC fixture");
    const auto readFile = [](const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto original = readFile(source);
    auto tracks = probeEditorAudioTracks(source);
    editorRequire(tracks.size() == 2, "Fixture audio probe failed");
    for (auto &track : tracks)
        track.end = 4;
    const auto run = [&](const wchar_t *name, const std::vector<EditorAudioTrack> &edits,
                         bool cut = false, bool merge = false) {
        std::vector<int> progress;
        const auto result = exportEditedClip(source, name, 1, 3, edits, merge, cut, 4,
                                             [&](int value) { progress.push_back(value); });
        editorRequire(result.success, "GPU editor export failed");
        editorRequire(!progress.empty() && progress.back() == 100 &&
                          std::is_sorted(progress.begin(), progress.end()) &&
                          std::any_of(progress.begin(), progress.end(),
                                      [](int value) { return value > 0 && value < 100; }),
                      "Missing real, monotonic export progress");
        const auto info = runHiddenProcessCapture(
            {L"ffprobe.exe", L"-v", L"error", L"-select_streams", L"v:0", L"-count_frames",
             L"-show_entries", L"stream=width,height,nb_read_frames,r_frame_rate:format=duration",
             L"-of", L"default=noprint_wrappers=1", quoteProcessArgument(result.output.wstring())});
        editorRequire(info.find("nb_read_frames=120") != std::string::npos &&
                          info.find("duration=2.000000") != std::string::npos &&
                          info.find("r_frame_rate=60/1") != std::string::npos &&
                          info.find("width=640") != std::string::npos,
                      "Video duration, frame count, resolution or FPS changed");
        return result.output;
    };
    auto trimmed = run(L"trim", tracks);
    checkExportTone(trimmed, 0, 0.2, 660);
    checkExportTone(trimmed, 0, 1.2, 880);
    auto cut = run(L"cut", tracks, true);
    checkExportTone(cut, 0, 0.2, 440);
    checkExportTone(cut, 0, 1.2, 1100);
    auto removed = run(L"removed", {tracks[1]});
    const auto survivingTracks = probeEditorAudioTracks(removed);
    editorRequire(survivingTracks.size() == 1 && survivingTracks[0].name == tracks[1].name,
                  "Deleted stream remains in MP4 or surviving label is wrong");
    checkExportTone(removed, 0, 0.2, 1760);
    auto mutedTracks = tracks;
    mutedTracks[0].included = false;
    const auto silent = run(L"muted", mutedTracks);
    editorRequire(probeEditorAudioTracks(silent).size() == 2, "Mute incorrectly deletes a stream");
    checkExportTone(silent, 0, 0.2, 0);
    checkExportTone(silent, 1, 0.2, 1760);
    auto ranged = tracks;
    ranged[0].start = 1.5;
    ranged[0].end = 2.5;
    const auto audioTrim = run(L"audio-range", ranged);
    checkExportTone(audioTrim, 0, 0.1, 0);
    checkExportTone(audioTrim, 0, 0.7, 660);
    checkExportTone(audioTrim, 0, 1.2, 880);
    checkExportTone(audioTrim, 0, 1.7, 0);
    editorRequire(probeEditorAudioTracks(run(L"merged", tracks, false, true)).size() == 1,
                  "Mix did not produce exactly one track");
    editorRequire(probeEditorAudioTracks(run(L"no-audio", {})).empty(),
                  "All-deleted export has audio");
    editorRequire(readFile(source) == original, "Export modified the source recording");
    std::vector<int> failedProgress;
    const auto failed = exportEditedClip(folder / L"missing-fixture.mp4", L"failed", 1, 3,
        tracks, false, false, 4, [&](int value) { failedProgress.push_back(value); });
    editorRequire(!failed.success &&
        std::find(failedProgress.begin(), failedProgress.end(), 100) == failedProgress.end(),
        "A failed GPU export reported 100 percent");
    for (const auto& file : std::filesystem::directory_iterator(folder))
        editorRequire(!file.path().filename().wstring().starts_with(L".nexplay-edit-"),
                      "Export left an unfinished temporary file");
    std::cout << "GPU editor exports passed: A/V trim, middle cut, delete, mute, mix, silence "
                 "ranges and live progress.\n";
}
