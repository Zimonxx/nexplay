#include "app/RecorderEngine.h"
#include "app/FfmpegProgress.h"

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
#include <map>
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
    std::wstring outputId;
    std::unique_ptr<storage::PcmSegmentBuffer> buffer;
    std::unique_ptr<audio::PcmAudioCapture> capture;
};

struct StagedAudioInput final {
    storage::StagedPcmTrack track;
    std::wstring outputId;
};

struct StagedAvReplay final {
    storage::StagedReplay video;
    std::vector<StagedAudioInput> audioInputs;
};

struct MuxedAudioTrack final {
    std::wstring name;
    std::vector<std::size_t> inputs;
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

void concatenateFiles(const std::vector<std::filesystem::path>& sources,
                      const std::filesystem::path& destination,
                      const std::function<void(std::uintmax_t)>& copied) {
    std::ofstream combined(destination, std::ios::binary | std::ios::trunc);
    if (!combined) {
        throw std::runtime_error("Nie można utworzyć pliku pośredniego klipu.");
    }
    for (const auto& source : sources) {
        std::ifstream input(source, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Nie można odczytać segmentu klipu.");
        }
        std::vector<char> chunk(256 * 1024);
        while (input) {
            input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = input.gcount();
            combined.write(chunk.data(), count);
            if (!combined)
                throw std::runtime_error("Nie można zapisać danych klipu.");
            if (copied)
                copied(static_cast<std::uintmax_t>(count));
        }
        if (!input.eof())
            throw std::runtime_error("Nie można odczytać danych klipu.");
    }
    combined.close();
    if (!combined) {
        throw std::runtime_error("Nie można połączyć segmentów klipu.");
    }
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

void saveStagedReplay(StagedAvReplay staged, const std::filesystem::path destinationDirectory,
                      const std::uint32_t framesPerSecond, const StatusCallback& statusCallback,
                      const SaveId saveId, const SaveCallback& saveCallback) {
    const auto report = [&](SavePhase phase, int percent, std::wstring detail = {}) {
        if (saveCallback)
            saveCallback({saveId, phase, percent, std::move(detail)});
    };
    try {
        std::uintmax_t totalBytes{}, copiedBytes{};
        for (const auto& file : staged.video.segments)
            totalBytes += std::filesystem::file_size(file);
        for (const auto& input : staged.audioInputs)
            for (const auto& file : input.track.segments)
                totalBytes += std::filesystem::file_size(file);
        int lastPercent = -1;
        const auto copied = [&](std::uintmax_t bytes) {
            copiedBytes += bytes;
            const int percent =
                5 + static_cast<int>(15.0 * copiedBytes / std::max<std::uintmax_t>(1, totalBytes));
            if (percent != lastPercent) {
                lastPercent = percent;
                report(SavePhase::preparing, percent);
            }
        };
        report(SavePhase::preparing, 5);
        const auto rawVideoPath = staged.video.directory / L"combined.h264";
        concatenateFiles(staged.video.segments, rawVideoPath, copied);

        std::vector<std::filesystem::path> rawAudioPaths;
        rawAudioPaths.reserve(staged.audioInputs.size());
        for (std::size_t index = 0; index < staged.audioInputs.size(); ++index) {
            const auto path = staged.video.directory /
                (L"combined-audio-" + std::to_wstring(index) + L".pcm");
            concatenateFiles(staged.audioInputs[index].track.segments, path, copied);
            rawAudioPaths.push_back(path);
        }

        std::vector<MuxedAudioTrack> outputTracks;
        std::map<std::wstring, std::size_t> outputTrackIndices;
        for (std::size_t index = 0; index < staged.audioInputs.size(); ++index) {
            const auto& input = staged.audioInputs[index];
            const auto [position, inserted] = outputTrackIndices.emplace(
                input.outputId, outputTracks.size());
            if (inserted) {
                outputTracks.push_back({.name = input.track.name});
            }
            outputTracks[position->second].inputs.push_back(index);
        }

        const auto outputPath = destinationDirectory /
            (timestampName() + L"-" + staged.video.directory.filename().wstring() + L".mp4");
        std::vector<std::wstring> arguments{
            L"ffmpeg.exe",
            L"-hide_banner",
            L"-loglevel",
            L"error",
            L"-y",
            L"-nostats",
            L"-stats_period",
            L"0.1",
            L"-progress",
            L"pipe:1",
            L"-r",
            std::to_wstring(framesPerSecond),
            L"-i",
            quoteSpawnArgument(rawVideoPath.wstring()),
        };
        for (std::size_t index = 0; index < staged.audioInputs.size(); ++index) {
            const auto& format = staged.audioInputs[index].track.format;
            arguments.insert(arguments.end(), {
                L"-f", L"s16le", L"-ar", std::to_wstring(format.sampleRate),
                L"-ac", std::to_wstring(format.channels), L"-i",
                quoteSpawnArgument(rawAudioPaths[index].wstring()),
            });
        }

        std::wstring audioFilters;
        for (std::size_t outputIndex = 0; outputIndex < outputTracks.size(); ++outputIndex) {
            const auto& output = outputTracks[outputIndex];
            if (output.inputs.size() < 2) continue;
            if (!audioFilters.empty()) audioFilters.push_back(L';');
            for (const std::size_t input : output.inputs) {
                audioFilters += L"[" + std::to_wstring(input + 1) + L":a:0]";
            }
            audioFilters += L"amix=inputs=" + std::to_wstring(output.inputs.size()) +
                L":duration=longest:dropout_transition=0:normalize=1[aout" +
                std::to_wstring(outputIndex) + L"]";
        }
        if (!audioFilters.empty()) {
            arguments.insert(arguments.end(), {L"-filter_complex", audioFilters});
        }
        arguments.insert(arguments.end(), {L"-map", L"0:v:0"});
        for (std::size_t index = 0; index < outputTracks.size(); ++index) {
            const auto& output = outputTracks[index];
            const std::wstring source = output.inputs.size() == 1
                ? std::to_wstring(output.inputs.front() + 1) + L":a:0"
                : L"[aout" + std::to_wstring(index) + L"]";
            arguments.insert(arguments.end(), {L"-map", source});
        }
        arguments.insert(arguments.end(), {L"-c:v", L"copy"});
        if (!outputTracks.empty()) {
            arguments.insert(arguments.end(), {L"-c:a", L"aac", L"-b:a", L"192k"});
            for (std::size_t index = 0; index < outputTracks.size(); ++index) {
                arguments.push_back(L"-metadata:s:a:" + std::to_wstring(index));
                arguments.push_back(quoteSpawnArgument(
                    L"handler_name=" + outputTracks[index].name));
            }
        }
        arguments.insert(arguments.end(), {
            L"-t", decimalSeconds(staged.video.frameCount, framesPerSecond),
            L"-movflags", L"+faststart", quoteSpawnArgument(outputPath.wstring()),
        });

        report(SavePhase::encoding, 20, outputPath.filename().wstring());
        if (runFfmpegProgress(arguments,
                              static_cast<double>(staged.video.frameCount) / framesPerSecond,
                              [&](int percent) {
                                  report(SavePhase::encoding, 20 + percent * 79 / 100,
                                         outputPath.filename().wstring());
                              }) != 0) {
            throw std::runtime_error("FFmpeg nie utworzył pliku MP4 ze ścieżkami audio.");
        }
        report(SavePhase::finalizing, 99, outputPath.filename().wstring());
        if (!std::filesystem::exists(outputPath) || std::filesystem::file_size(outputPath) == 0)
            throw std::runtime_error("Zapisany plik klipu jest pusty.");
        std::error_code error;
        std::filesystem::remove_all(staged.video.directory, error);
        report(SavePhase::saved, 100, outputPath.filename().wstring());
        if (statusCallback) {
            statusCallback(L"Klip zapisany: " + outputPath.wstring());
        }
    } catch (const std::exception& error) {
        report(SavePhase::failed, 0, utf8ToWide(error.what()));
        if (statusCallback) {
            statusCallback(L"Błąd zapisu klipu: " + utf8ToWide(error.what()));
        }
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
            if (const auto group = settings.groupedProcessNames.find(application.processId);
                group != settings.groupedProcessNames.end() && !group->second.empty()) {
                track->name = group->second;
                track->outputId = L"group:" + group->second;
            } else {
                track->name = application.name;
                track->outputId = L"process:" + std::to_wstring(application.processId);
            }
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
            microphone->outputId = L"microphone";
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

[[nodiscard]] std::size_t outputAudioTrackCount(
    const std::vector<std::unique_ptr<ActiveAudioTrack>>& tracks) {
    std::set<std::wstring> outputs;
    for (const auto& track : tracks) outputs.insert(track->outputId);
    return outputs.size();
}

} // namespace

RecorderEngine::~RecorderEngine() {
    stop();
}

void RecorderEngine::start(RecorderSettings settings, StatusCallback statusCallback,
                           SaveCallback saveCallback) {
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
    {
        std::lock_guard lock(saveMutex_);
        pendingSaves_.clear();
    }
    worker_ = std::jthread(
        [this, settings = std::move(settings), statusCallback = std::move(statusCallback),
         saveCallback = std::move(saveCallback)](const std::stop_token stopToken) mutable {
            run(stopToken, std::move(settings), std::move(statusCallback), std::move(saveCallback));
        });
}

SaveId RecorderEngine::requestSave() {
    std::lock_guard lock(saveMutex_);
    if (!running_ || worker_.get_stop_token().stop_requested())
        return 0;
    const auto id = nextSaveId_++;
    pendingSaves_.push_back(id);
    return id;
}

void RecorderEngine::requestStop() noexcept {
    if (worker_.joinable())
        worker_.request_stop();
}

void RecorderEngine::stop() noexcept {
    {
        std::lock_guard lock(saveMutex_);
        running_ = false;
    }
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    running_ = false;
}

bool RecorderEngine::isRunning() const noexcept {
    return running_;
}

void RecorderEngine::run(const std::stop_token stopToken, RecorderSettings settings,
                         StatusCallback statusCallback, SaveCallback saveCallback) {
    SaveId stagingId{};
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
                std::to_wstring(outputAudioTrackCount(audioTracks)));
        }

        auto nextFrame = std::chrono::steady_clock::now();
        const auto frameDuration =
            std::chrono::nanoseconds(1'000'000'000 / settings.framesPerSecond);
        while (!stopToken.stop_requested()) {
            {
                std::lock_guard lock(saveMutex_);
                if (!pendingSaves_.empty()) {
                    stagingId = pendingSaves_.front();
                    pendingSaves_.pop_front();
                }
            }
            if (stagingId) {
                if (saveCallback)
                    saveCallback({stagingId, SavePhase::preparing, 1, {}});
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
                            staged.audioInputs.push_back({
                                .track = track->buffer->stageLatest(
                                    staged.video.directory, track->name, index),
                                .outputId = track->outputId,
                            });
                        } catch (...) {
                        }
                    }
                    replay.reset();
                    for (auto& track : audioTracks) {
                        track->buffer->reset();
                    }
                    saveJobs.emplace_back(saveStagedReplay, std::move(staged), clipFolder,
                                          settings.framesPerSecond, statusCallback, stagingId,
                                          saveCallback);
                } else if (saveCallback) {
                    saveCallback(
                        {stagingId, SavePhase::failed, 0, L"Bufor nie zawiera jeszcze klatek."});
                }
                stagingId = 0;

                audioTracks.clear();
                audioTracks = startAudioTracks(replay.sessionDirectory(), settings);
                segmentOutput = &replay.beginSegment();
                nextFrame = std::chrono::steady_clock::now();
                if (statusCallback) {
                    statusCallback(
                        L"Bufor wyzerowany. Nowe ścieżki audio: " +
                        std::to_wstring(outputAudioTrackCount(audioTracks)));
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
        if (stagingId && saveCallback)
            saveCallback({stagingId, SavePhase::failed, 0, utf8ToWide(error.what())});
        if (statusCallback) {
            statusCallback(L"Błąd: " + utf8ToWide(error.what()));
        }
    }
    std::deque<SaveId> cancelled;
    {
        std::lock_guard lock(saveMutex_);
        running_ = false;
        cancelled.swap(pendingSaves_);
    }
    if (saveCallback)
        for (const auto id : cancelled)
            saveCallback({id, SavePhase::failed, 0, L"Bufor został zatrzymany przed zapisem."});
}

} // namespace nexplay::app
