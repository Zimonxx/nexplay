#include "capture/DesktopDuplicator.h"
#include "encoding/NvencEncoder.h"
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
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int saveHotkeyId = 1;
constexpr int stopHotkeyId = 2;

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

void saveStagedReplay(
    nexplay::storage::StagedReplay staged,
    const std::filesystem::path destinationDirectory,
    const std::uint32_t framesPerSecond) {
    try {
        const std::filesystem::path rawPath = staged.directory / L"combined.h264";
        std::ofstream combined(rawPath, std::ios::binary | std::ios::trunc);
        if (!combined) {
            throw std::runtime_error("Nie mozna utworzyc pliku posredniego klipu.");
        }
        for (const auto& segment : staged.segments) {
            std::ifstream input(segment, std::ios::binary);
            if (!input) {
                throw std::runtime_error("Nie mozna odczytac segmentu klipu.");
            }
            combined << input.rdbuf();
        }
        combined.close();

        const std::filesystem::path outputPath =
            destinationDirectory /
            (timestampName() + L"-" + staged.directory.filename().wstring() + L".mp4");
        const std::wstring frameRate = std::to_wstring(framesPerSecond);
        const intptr_t ffmpegResult = _wspawnlp(
            _P_WAIT,
            L"ffmpeg.exe",
            L"ffmpeg.exe",
            L"-hide_banner",
            L"-loglevel",
            L"error",
            L"-y",
            L"-r",
            frameRate.c_str(),
            L"-i",
            rawPath.c_str(),
            L"-c:v",
            L"copy",
            L"-movflags",
            L"+faststart",
            outputPath.c_str(),
            static_cast<wchar_t*>(nullptr));
        if (ffmpegResult != 0) {
            throw std::runtime_error("FFmpeg nie utworzyl pliku MP4.");
        }

        std::wcout << L"\nKlip zapisany: " << outputPath.wstring() << L"\n";
        MessageBeep(MB_OK);

        std::error_code error;
        std::filesystem::remove_all(staged.directory, error);
    } catch (const std::exception& error) {
        std::cerr << "\nBlad zapisu klipu: " << error.what() << '\n';
        MessageBeep(MB_ICONERROR);
    }
}

[[nodiscard]] std::chrono::seconds requestedDuration(const int argumentCount, wchar_t** arguments) {
    if (argumentCount < 2) {
        return std::chrono::seconds(10);
    }
    const long seconds = std::wcstol(arguments[1], nullptr, 10);
    if (seconds < 10 || seconds > 1'200) {
        throw std::invalid_argument("Czas bufora musi wynosic od 10 do 1200 sekund.");
    }
    return std::chrono::seconds(seconds);
}

} // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    constexpr std::uint32_t framesPerSecond = 60;
    constexpr std::uint32_t bitrate = 25'000'000;
    constexpr std::uint32_t framesPerSegment = framesPerSecond;

    try {
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

        std::vector<std::jthread> saveJobs;
        const std::filesystem::path clipFolder = clipsDirectory();
        std::ostream* segmentOutput = &replay.beginSegment();
        std::uint32_t segmentFrames{};
        std::uint64_t frameIndex{};
        bool running = true;

        std::wcout << L"NexPlay - bufor powtorek dziala.\n";
        std::wcout << L"Bufor: " << duration.count() << L" sekund.\n";
        std::wcout << L"F8 - zapisz klip, F9 - zakoncz.\n";
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
                if (replay.bufferedFrames() > 0) {
                    auto staged = replay.stageLatest();
                    replay.reset();
                    saveJobs.emplace_back(
                        saveStagedReplay,
                        std::move(staged),
                        clipFolder,
                        framesPerSecond);
                }
                segmentOutput = &replay.beginSegment();
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
