#include "audio/AudioSessionScanner.h"
#include "audio/PcmAudioCapture.h"
#include "capture/DesktopDuplicator.h"
#include "encoding/NvencEncoder.h"
#include "storage/PcmSegmentBuffer.h"
#include "storage/ReplaySegmentBuffer.h"
#include "storage/ReplayStorage.h"

#include <Windows.h>
#include <ShlObj.h>
#include <process.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int saveHotkeyId = 1;
constexpr int stopHotkeyId = 2;

struct ComApartment final {
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result)) {
            throw std::runtime_error("Nie mozna uruchomic obslugi audio Windows.");
        }
    }
    ~ComApartment() {
        CoUninitialize();
    }
};

struct ActiveAudioTrack final {
    std::wstring name;
    std::unique_ptr<nexplay::storage::PcmSegmentBuffer> buffer;
    std::unique_ptr<nexplay::audio::PcmAudioCapture> capture;
};

struct StagedAvReplay final {
    nexplay::storage::StagedReplay video;
    std::vector<nexplay::storage::StagedPcmTrack> audioTracks;
};

[[nodiscard]] std::filesystem::path clipsDirectory() {
    PWSTR videosPath{};
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videosPath);
    if (FAILED(result) || videosPath == nullptr) {
        throw std::runtime_error("Nie mozna odnalezc systemowego folderu Wideo.");
    }
    const std::filesystem::path directory =
        std::filesystem::path(videosPath) / L"NexPlay" / L"Clips";
    CoTaskMemFree(videosPath);

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::runtime_error("Nie mozna utworzyc folderu na zapisane klipy.");
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

[[nodiscard]] std::chrono::seconds requestedDuration(
    const int argumentCount,
    wchar_t** arguments) {
    if (argumentCount < 2) {
        return std::chrono::seconds(10);
    }
    const long seconds = std::wcstol(arguments[1], nullptr, 10);
    if (seconds < 10 || seconds > 1'200) {
        throw std::invalid_argument("Czas bufora musi wynosic od 10 do 1200 sekund.");
    }
    return std::chrono::seconds(seconds);
}

void concatenateFiles(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& destination) {
    std::ofstream combined(destination, std::ios::binary | std::ios::trunc);
    if (!combined) {
        throw std::runtime_error("Nie mozna utworzyc pliku posredniego klipu.");
    }
    for (const auto& source : sources) {
        std::ifstream input(source, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Nie mozna odczytac segmentu klipu.");
        }
        combined << input.rdbuf();
    }
    if (!combined) {
        throw std::runtime_error("Nie mozna polaczyc segmentow klipu.");
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

[[nodiscard]] std::wstring quoteSpawnArgument(const std::wstring_view argument) {
    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');

    std::size_t backslashes{};
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

void saveStagedReplay(
    StagedAvReplay staged,
    const std::filesystem::path destinationDirectory,
    const std::uint32_t framesPerSecond) {
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
                L"-f", L"s16le",
                L"-ar", std::to_wstring(format.sampleRate),
                L"-ac", std::to_wstring(format.channels),
                L"-i", quoteSpawnArgument(rawAudioPaths[index].wstring()),
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

        std::vector<const wchar_t*> argumentPointers;
        argumentPointers.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argumentPointers.push_back(argument.c_str());
        }
        argumentPointers.push_back(nullptr);
        const intptr_t ffmpegResult = _wspawnvp(
            _P_WAIT, arguments.front().c_str(), argumentPointers.data());
        if (ffmpegResult != 0) {
            throw std::runtime_error("FFmpeg nie utworzyl pliku MP4 ze sciezkami audio.");
        }

        std::wcout << L"\nKlip zapisany: " << outputPath.wstring()
                   << L" (sciezki audio: " << staged.audioTracks.size() << L")\n";
        MessageBeep(MB_OK);
        std::error_code error;
        std::filesystem::remove_all(staged.video.directory, error);
    } catch (const std::exception& error) {
        std::cerr << "\nBlad zapisu klipu: " << error.what() << '\n';
        MessageBeep(MB_ICONERROR);
    }
}

[[nodiscard]] std::vector<std::unique_ptr<ActiveAudioTrack>> startAudioTracks(
    const std::filesystem::path& sessionDirectory,
    const std::chrono::seconds duration) {
    std::vector<std::unique_ptr<ActiveAudioTrack>> tracks;
    const auto applications = nexplay::audio::activeAudioApplications();
    for (const auto& application : applications) {
        try {
            auto track = std::make_unique<ActiveAudioTrack>();
            track->name = application.name;
            track->buffer = std::make_unique<nexplay::storage::PcmSegmentBuffer>(
                sessionDirectory, L"process-" + std::to_wstring(application.processId), duration);
            track->capture = std::make_unique<nexplay::audio::PcmAudioCapture>();
            auto* buffer = track->buffer.get();
            track->capture->startProcessToSink(
                application.processId,
                [buffer](const nexplay::audio::PcmFormat format,
                         const std::span<const std::byte> samples,
                         const std::uint32_t frames) {
                    buffer->append(format, samples, frames);
                });
            std::wcout << L"Audio aplikacji: " << track->name << L"\n";
            tracks.push_back(std::move(track));
        } catch (const std::exception& error) {
            std::wcerr << L"Pominieto audio procesu " << application.name << L": "
                       << error.what() << L"\n";
        }
    }

    try {
        auto microphone = std::make_unique<ActiveAudioTrack>();
        microphone->name = L"Microphone";
        microphone->buffer = std::make_unique<nexplay::storage::PcmSegmentBuffer>(
            sessionDirectory, L"microphone", duration);
        microphone->capture = std::make_unique<nexplay::audio::PcmAudioCapture>();
        auto* buffer = microphone->buffer.get();
        microphone->capture->startDefaultMicrophoneToSink(
            [buffer](const nexplay::audio::PcmFormat format,
                     const std::span<const std::byte> samples,
                     const std::uint32_t frames) {
                buffer->append(format, samples, frames);
            });
        std::wcout << L"Audio: Microphone\n";
        tracks.push_back(std::move(microphone));
    } catch (const std::exception& error) {
        std::cerr << "Pominieto mikrofon: " << error.what() << '\n';
    }
    return tracks;
}

void stopAudioTracks(std::vector<std::unique_ptr<ActiveAudioTrack>>& tracks) noexcept {
    for (auto& track : tracks) {
        try {
            track->capture->stop();
        } catch (const std::exception& error) {
            std::cerr << "Blad zatrzymywania audio: " << error.what() << '\n';
        }
        try {
            track->buffer->finishSegment();
        } catch (const std::exception& error) {
            std::cerr << "Blad zamykania bufora audio: " << error.what() << '\n';
        }
    }
}

} // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    constexpr std::uint32_t framesPerSecond = 60;
    constexpr std::uint32_t bitrate = 25'000'000;
    constexpr std::uint32_t framesPerSegment = framesPerSecond;

    try {
        ComApartment apartment;
        const std::chrono::seconds duration = requestedDuration(argumentCount, arguments);
        nexplay::storage::ReplayStorage storage;
        storage.ensureExists();
        nexplay::storage::ReplaySegmentBuffer replay(
            storage.root(), framesPerSecond, duration);

        nexplay::capture::DesktopDuplicator capture;
        if (capture.adapterVendorId() != 0x10DE) {
            throw std::runtime_error("Glowny monitor nie jest obslugiwany przez karte NVIDIA.");
        }
        nexplay::encoding::NvencEncoder encoder(
            capture.device(),
            capture.frameTexture(),
            {
                .width = capture.width(),
                .height = capture.height(),
                .framesPerSecond = framesPerSecond,
                .bitrate = bitrate,
            });
        if (!capture.acquireLatestFrame(2'000)) {
            throw std::runtime_error("Pulpit nie dostarczyl pierwszej klatki.");
        }
        if (!RegisterHotKey(nullptr, saveHotkeyId, MOD_NOREPEAT, VK_F8)) {
            throw std::runtime_error("Nie mozna zarejestrowac klawisza F8.");
        }
        if (!RegisterHotKey(nullptr, stopHotkeyId, MOD_NOREPEAT, VK_F9)) {
            UnregisterHotKey(nullptr, saveHotkeyId);
            throw std::runtime_error("Nie mozna zarejestrowac klawisza F9.");
        }

        auto audioTracks = startAudioTracks(replay.sessionDirectory(), duration);
        const auto clipFolder = clipsDirectory();
        std::vector<std::jthread> saveJobs;
        std::ostream* segmentOutput = &replay.beginSegment();
        std::uint32_t segmentFrames{};
        std::uint64_t frameIndex{};
        bool running = true;

        std::wcout << L"\nNexPlay A/V - bufor powtorek dziala.\n";
        std::wcout << L"Bufor: " << duration.count() << L" sekund, audio: "
                   << audioTracks.size() << L" osobnych sciezek.\n";
        std::wcout << L"F8 - zapisz i wyzeruj bufor, F9 - zakoncz.\n";
        std::wcout << L"Pliki: " << clipFolder.wstring() << L"\n";

        auto nextFrame = std::chrono::steady_clock::now();
        const auto frameDuration = std::chrono::nanoseconds(1'000'000'000 / framesPerSecond);
        while (running) {
            MSG message{};
            bool saveRequested = false;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_HOTKEY && message.wParam == saveHotkeyId) {
                    saveRequested = true;
                } else if (message.message == WM_HOTKEY && message.wParam == stopHotkeyId) {
                    running = false;
                }
            }
            if (!running) {
                break;
            }

            if (saveRequested) {
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
                        } catch (const std::exception& error) {
                            std::cerr << "Pominieto sciezke przy zapisie: " << error.what() << '\n';
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
                        framesPerSecond);
                }

                audioTracks.clear();
                audioTracks = startAudioTracks(replay.sessionDirectory(), duration);
                segmentOutput = &replay.beginSegment();
                nextFrame = std::chrono::steady_clock::now();
                std::wcout << L"Bufor wyzerowany; nowe audio: "
                           << audioTracks.size() << L" sciezek.\n";
            }

            const bool receivedFrame = capture.acquireLatestFrame(0);
            (void)receivedFrame;
            encoder.encodeFrame(*segmentOutput, frameIndex++, segmentFrames == 0);
            ++segmentFrames;
            if (segmentFrames >= framesPerSegment) {
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
        UnregisterHotKey(nullptr, saveHotkeyId);
        UnregisterHotKey(nullptr, stopHotkeyId);
        std::wcout << L"Oczekiwanie na zakonczenie zapisow...\n";
        saveJobs.clear();
        std::wcout << L"NexPlay zakonczony.\n";
        return 0;
    } catch (const std::exception& error) {
        UnregisterHotKey(nullptr, saveHotkeyId);
        UnregisterHotKey(nullptr, stopHotkeyId);
        std::cerr << "Blad: " << error.what() << '\n';
        MessageBoxA(nullptr, error.what(), "NexPlay - blad", MB_OK | MB_ICONERROR);
        return 1;
    }
}
