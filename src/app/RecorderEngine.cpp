#include "app/RecorderEngine.h"

#include "audio/AudioSessionScanner.h"
#include "audio/PcmAudioCapture.h"
#include "capture/DesktopDuplicator.h"
#include "encoding/NvencEncoder.h"
#include "storage/PcmSegmentBuffer.h"
#include "storage/ReplaySegmentBuffer.h"
#include "storage/ReplayStorage.h"

#include <ShlObj.h>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace nexplay::app {
namespace {

struct ComApartment final {
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result)) {
            throw std::runtime_error("Nie można uruchomić obsługi audio Windows.");
        }
    }
    ~ComApartment() {
        CoUninitialize();
    }
};

struct ActiveAudioTrack final {
    std::wstring name;
    std::unique_ptr<storage::PcmSegmentBuffer> buffer;
    std::unique_ptr<audio::PcmAudioCapture> capture;
};

struct StagedAvReplay final {
    storage::StagedReplay video;
    std::vector<storage::StagedPcmTrack> audioTracks;
};

[[nodiscard]] std::filesystem::path clipsDirectory() {
    PWSTR videosPath{};
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videosPath);
    if (FAILED(result) || videosPath == nullptr) {
        throw std::runtime_error("Nie można odnaleźć systemowego folderu Wideo.");
    }
    const std::filesystem::path directory =
        std::filesystem::path(videosPath) / L"NexPlay" / L"Clips";
    CoTaskMemFree(videosPath);

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::runtime_error("Nie można utworzyć folderu na zapisane klipy.");
    }
    return directory;
}

[[nodiscard]] std::wstring timestampName() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &time);
    wchar_t text[32]{};
    wcsftime(text, std::size(text), L"NexPlay-%Y%m%d-%H%M%S", &localTime);
    return text;
}

[[nodiscard]] std::wstring quoteSpawnArgument(const std::wstring_view argument) {
    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');
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

[[nodiscard]] std::wstring utf8ToWide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return L"Nieznany błąd";
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

void concatenateFiles(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& destination) {
    std::ofstream combined(destination, std::ios::binary | std::ios::trunc);
    if (!combined) {
        throw std::runtime_error("Nie można utworzyć pliku pośredniego klipu.");
    }
    for (const auto& source : sources) {
        std::ifstream input(source, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Nie można odczytać segmentu klipu.");
        }
        combined << input.rdbuf();
    }
    if (!combined) {
        throw std::runtime_error("Nie można połączyć segmentów klipu.");
    }
}

[[nodiscard]] DWORD runHiddenProcess(const std::vector<std::wstring>& arguments) {
    std::wstring commandLine;
    for (const auto& argument : arguments) {
        if (!commandLine.empty()) {
            commandLine.push_back(L' ');
        }
        commandLine.append(argument);
    }

    STARTUPINFOW startupInfo{sizeof(startupInfo)};
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo)) {
        throw std::runtime_error("Nie można uruchomić programu FFmpeg.");
    }
    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return exitCode;
}

[[nodiscard]] std::wstring decimalSeconds(
    const std::uint64_t frameCount,
    const std::uint32_t framesPerSecond) {
    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(6)
         << static_cast<double>(frameCount) / framesPerSecond;
    return text.str();
}

void saveStagedReplay(
    StagedAvReplay staged,
    const std::filesystem::path destinationDirectory,
    const std::uint32_t framesPerSecond,
    const StatusCallback& statusCallback) {
    try {
        const auto rawVideoPath = staged.video.directory / L"combined.h264";
        concatenateFiles(staged.video.segments, rawVideoPath);

        std::vector<std::filesystem::path> rawAudioPaths;
        rawAudioPaths.reserve(staged.audioTracks.size());
        for (std::size_t index = 0; index < staged.audioTracks.size(); ++index) {
            const auto path = staged.video.directory /
                (L"combined-audio-" + std::to_wstring(index) + L".pcm");
            concatenateFiles(staged.audioTracks[index].segments, path);
            rawAudioPaths.push_back(path);
        }

        const auto outputPath = destinationDirectory /
            (timestampName() + L"-" + staged.video.directory.filename().wstring() + L".mp4");
        std::vector<std::wstring> arguments{
            L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"error", L"-y",
            L"-r", std::to_wstring(framesPerSecond), L"-i",
            quoteSpawnArgument(rawVideoPath.wstring()),
        };
        for (std::size_t index = 0; index < staged.audioTracks.size(); ++index) {
            const auto& format = staged.audioTracks[index].format;
            arguments.insert(arguments.end(), {
                L"-f", L"s16le", L"-ar", std::to_wstring(format.sampleRate),
                L"-ac", std::to_wstring(format.channels), L"-i",
                quoteSpawnArgument(rawAudioPaths[index].wstring()),
            });
        }
        arguments.insert(arguments.end(), {L"-map", L"0:v:0"});
        for (std::size_t index = 0; index < staged.audioTracks.size(); ++index) {
            arguments.insert(arguments.end(), {
                L"-map", std::to_wstring(index + 1) + L":a:0",
            });
        }
        arguments.insert(arguments.end(), {L"-c:v", L"copy"});
        if (!staged.audioTracks.empty()) {
            arguments.insert(arguments.end(), {L"-c:a", L"aac", L"-b:a", L"192k"});
            for (std::size_t index = 0; index < staged.audioTracks.size(); ++index) {
                arguments.push_back(L"-metadata:s:a:" + std::to_wstring(index));
                arguments.push_back(quoteSpawnArgument(
                    L"handler_name=" + staged.audioTracks[index].name));
            }
        }
        arguments.insert(arguments.end(), {
            L"-t", decimalSeconds(staged.video.frameCount, framesPerSecond),
            L"-movflags", L"+faststart", quoteSpawnArgument(outputPath.wstring()),
        });

        if (runHiddenProcess(arguments) != 0) {
            throw std::runtime_error("FFmpeg nie utworzył pliku MP4 ze ścieżkami audio.");
        }

        std::error_code error;
        std::filesystem::remove_all(staged.video.directory, error);
        if (statusCallback) {
            statusCallback(L"Klip zapisany: " + outputPath.wstring());
        }
        MessageBeep(MB_OK);
    } catch (const std::exception& error) {
        if (statusCallback) {
            statusCallback(L"Błąd zapisu klipu: " + utf8ToWide(error.what()));
        }
        MessageBeep(MB_ICONERROR);
    }
}

[[nodiscard]] std::vector<std::unique_ptr<ActiveAudioTrack>> startAudioTracks(
    const std::filesystem::path& sessionDirectory,
    const RecorderSettings& settings) {
    std::vector<std::unique_ptr<ActiveAudioTrack>> tracks;
    for (const auto& application : audio::activeAudioApplications()) {
        if (settings.excludedProcessIds.contains(application.processId)) {
            continue;
        }
        try {
            auto track = std::make_unique<ActiveAudioTrack>();
            track->name = application.name;
            track->buffer = std::make_unique<storage::PcmSegmentBuffer>(
                sessionDirectory,
                L"process-" + std::to_wstring(application.processId),
                settings.bufferDuration);
            track->capture = std::make_unique<audio::PcmAudioCapture>();
            auto* buffer = track->buffer.get();
            track->capture->startProcessToSink(
                application.processId,
                [buffer](const audio::PcmFormat format,
                         const std::span<const std::byte> samples,
                         const std::uint32_t frames) {
                    buffer->append(format, samples, frames);
                });
            tracks.push_back(std::move(track));
        } catch (...) {
        }
    }

    if (settings.captureMicrophone) {
        try {
            auto microphone = std::make_unique<ActiveAudioTrack>();
            microphone->name = L"Microphone";
            microphone->buffer = std::make_unique<storage::PcmSegmentBuffer>(
                sessionDirectory, L"microphone", settings.bufferDuration);
            microphone->capture = std::make_unique<audio::PcmAudioCapture>();
            auto* buffer = microphone->buffer.get();
            microphone->capture->startDefaultMicrophoneToSink(
                [buffer](const audio::PcmFormat format,
                         const std::span<const std::byte> samples,
                         const std::uint32_t frames) {
                    buffer->append(format, samples, frames);
                });
            tracks.push_back(std::move(microphone));
        } catch (...) {
        }
    }
    return tracks;
}

void stopAudioTracks(std::vector<std::unique_ptr<ActiveAudioTrack>>& tracks) noexcept {
    for (auto& track : tracks) {
        try {
            track->capture->stop();
        } catch (...) {
        }
        try {
            track->buffer->finishSegment();
        } catch (...) {
        }
    }
}

} // namespace

RecorderEngine::~RecorderEngine() {
    stop();
}

void RecorderEngine::start(RecorderSettings settings, StatusCallback statusCallback) {
    if (running_.exchange(true)) {
        throw std::logic_error("Bufor jest już uruchomiony.");
    }
    if (settings.bufferDuration < std::chrono::seconds(10) ||
        settings.bufferDuration > std::chrono::minutes(20) ||
        settings.framesPerSecond == 0 || settings.bitrate == 0) {
        running_ = false;
        throw std::invalid_argument("Nieprawidłowe ustawienia nagrywania.");
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    saveRequested_ = false;
    worker_ = std::jthread(
        [this, settings = std::move(settings), statusCallback = std::move(statusCallback)](
            const std::stop_token stopToken) mutable {
            run(stopToken, std::move(settings), std::move(statusCallback));
        });
}

void RecorderEngine::requestSave() noexcept {
    if (running_) {
        saveRequested_ = true;
    }
}

void RecorderEngine::stop() noexcept {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    running_ = false;
    saveRequested_ = false;
}

bool RecorderEngine::isRunning() const noexcept {
    return running_;
}

void RecorderEngine::run(
    const std::stop_token stopToken,
    RecorderSettings settings,
    StatusCallback statusCallback) {
    try {
        ComApartment apartment;
        storage::ReplayStorage storageRoot;
        storageRoot.ensureExists();
        storage::ReplaySegmentBuffer replay(
            storageRoot.root(), settings.framesPerSecond, settings.bufferDuration);
        capture::DesktopDuplicator capture(settings.outputWidth, settings.outputHeight);
        if (capture.adapterVendorId() != 0x10DE) {
            throw std::runtime_error("Główny monitor nie jest obsługiwany przez kartę NVIDIA.");
        }
        encoding::NvencEncoder encoder(
            capture.device(), capture.frameTexture(), {
                .width = capture.width(),
                .height = capture.height(),
                .framesPerSecond = settings.framesPerSecond,
                .bitrate = settings.bitrate,
            });
        if (!capture.acquireLatestFrame(2'000)) {
            throw std::runtime_error("Pulpit nie dostarczył pierwszej klatki.");
        }

        auto audioTracks = startAudioTracks(replay.sessionDirectory(), settings);
        const auto clipFolder = clipsDirectory();
        std::vector<std::jthread> saveJobs;
        std::ostream* segmentOutput = &replay.beginSegment();
        std::uint32_t segmentFrames{};
        std::uint64_t frameIndex{};
        if (statusCallback) {
            statusCallback(
                L"Bufor działa — użyj skrótu zapisu klipu. Ścieżki audio: " +
                std::to_wstring(audioTracks.size()));
        }

        auto nextFrame = std::chrono::steady_clock::now();
        const auto frameDuration =
            std::chrono::nanoseconds(1'000'000'000 / settings.framesPerSecond);
        while (!stopToken.stop_requested()) {
            if (saveRequested_.exchange(false)) {
                replay.finishSegment(segmentFrames);
                segmentFrames = 0;
                stopAudioTracks(audioTracks);

                if (replay.bufferedFrames() > 0) {
                    StagedAvReplay staged{.video = replay.stageLatest()};
                    for (std::uint32_t index = 0; index < audioTracks.size(); ++index) {
                        auto& track = audioTracks[index];
                        if (track->buffer->bufferedFrames() == 0) {
                            continue;
                        }
                        try {
                            staged.audioTracks.push_back(track->buffer->stageLatest(
                                staged.video.directory, track->name, index));
                        } catch (...) {
                        }
                    }
                    replay.reset();
                    for (auto& track : audioTracks) {
                        track->buffer->reset();
                    }
                    saveJobs.emplace_back(
                        saveStagedReplay,
                        std::move(staged),
                        clipFolder,
                        settings.framesPerSecond,
                        statusCallback);
                }

                audioTracks.clear();
                audioTracks = startAudioTracks(replay.sessionDirectory(), settings);
                segmentOutput = &replay.beginSegment();
                nextFrame = std::chrono::steady_clock::now();
                if (statusCallback) {
                    statusCallback(
                        L"Bufor wyzerowany. Nowe ścieżki audio: " +
                        std::to_wstring(audioTracks.size()));
                }
            }

            const bool receivedFrame = capture.acquireLatestFrame(0);
            (void)receivedFrame;
            encoder.encodeFrame(*segmentOutput, frameIndex++, segmentFrames == 0);
            ++segmentFrames;
            if (segmentFrames >= settings.framesPerSecond) {
                replay.finishSegment(segmentFrames);
                segmentFrames = 0;
                segmentOutput = &replay.beginSegment();
            }

            nextFrame += frameDuration;
            std::this_thread::sleep_until(nextFrame);
            if (std::chrono::steady_clock::now() - nextFrame > std::chrono::seconds(1)) {
                nextFrame = std::chrono::steady_clock::now();
            }
        }

        stopAudioTracks(audioTracks);
        audioTracks.clear();
        replay.finishSegment(segmentFrames);
        encoder.finish();
        saveJobs.clear();
        if (statusCallback) {
            statusCallback(L"Bufor zatrzymany.");
        }
    } catch (const std::exception& error) {
        if (statusCallback) {
            statusCallback(L"Błąd: " + utf8ToWide(error.what()));
        }
        MessageBeep(MB_ICONERROR);
    }
    running_ = false;
}

} // namespace nexplay::app
