#include "capture/DesktopDuplicator.h"
#include "encoding/NvencEncoder.h"
#include "storage/ReplayStorage.h"

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <process.h>

namespace {

[[nodiscard]] std::filesystem::path createOutputPath(
    const std::filesystem::path& directory) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    std::wostringstream name;
    name << L"capture-test-" << std::put_time(&localTime, L"%Y%m%d-%H%M%S") << L".h264";
    return directory / name.str();
}

[[nodiscard]] std::filesystem::path remuxToMp4(
    const std::filesystem::path& rawPath,
    const std::uint32_t framesPerSecond) {
    std::filesystem::path mp4Path = rawPath;
    mp4Path.replace_extension(L".mp4");
    const std::wstring frameRate = std::to_wstring(framesPerSecond);

    const intptr_t result = _wspawnlp(
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
        mp4Path.c_str(),
        static_cast<wchar_t*>(nullptr));
    if (result != 0) {
        throw std::runtime_error(
            "Nagranie H.264 powstalo, ale nie udalo sie utworzyc MP4. "
            "Sprawdz, czy ffmpeg.exe jest dostepny w PATH.");
    }

    std::error_code removeError;
    std::filesystem::remove(rawPath, removeError);
    return mp4Path;
}

} // namespace

int wmain() {
    constexpr std::uint32_t framesPerSecond = 60;
    constexpr std::uint32_t recordingSeconds = 10;
    constexpr std::uint32_t bitrate = 25'000'000;
    constexpr std::uint32_t nvidiaVendorId = 0x10DE;

    try {
        nexplay::storage::ReplayStorage storage;
        storage.ensureExists();
        const std::filesystem::path rawOutputPath = createOutputPath(storage.root());

        std::wcout << L"NexPlay - test nagrywania NVENC\n";
        std::wcout << L"Inicjalizacja glownego monitora...\n";
        nexplay::capture::DesktopDuplicator capture;

        std::wcout << L"Karta obslugujaca glowny monitor: " << capture.adapterName() << L'\n';
        std::wcout << L"Rozdzielczosc: " << capture.width() << L'x' << capture.height() << L'\n';
        if (capture.adapterVendorId() != nvidiaVendorId) {
            throw std::runtime_error(
                "Glowny monitor nie jest podlaczony do karty NVIDIA. "
                "Obsluga kopiowania miedzy kartami zostanie dodana pozniej.");
        }

        std::ofstream output(rawOutputPath, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Nie mozna utworzyc pliku testowego H.264.");
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

        std::wcout << L"Oczekiwanie na pierwsza klatke pulpitu...\n";
        if (!capture.acquireLatestFrame(2'000)) {
            throw std::runtime_error("Pulpit nie dostarczyl pierwszej klatki w ciagu 2 sekund.");
        }

        std::wcout << L"Nagrywanie 10 sekund, 60 FPS, 25 Mb/s...\n";
        const auto start = std::chrono::steady_clock::now();
        const auto frameDuration = std::chrono::nanoseconds(1'000'000'000 / framesPerSecond);
        const std::uint64_t frameCount = recordingSeconds * framesPerSecond;

        for (std::uint64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            const bool receivedNewFrame = capture.acquireLatestFrame(0);
            (void)receivedNewFrame;
            encoder.encodeFrame(output, frameIndex);

            if ((frameIndex + 1) % framesPerSecond == 0) {
                std::wcout << L"  " << ((frameIndex + 1) / framesPerSecond)
                           << L"/" << recordingSeconds << L" s\n";
            }

            std::this_thread::sleep_until(start + frameDuration * (frameIndex + 1));
        }

        encoder.finish();
        output.close();
        const std::filesystem::path outputPath = remuxToMp4(rawOutputPath, framesPerSecond);
        const auto fileSize = std::filesystem::file_size(outputPath);

        std::wcout << L"Gotowe. Plik testowy:\n" << outputPath.wstring() << L'\n';
        std::wcout << L"Rozmiar: " << (fileSize / 1'000'000.0) << L" MB\n";
        const std::wstring successMessage =
            L"Nagranie testowe zostalo zapisane tutaj:\n\n" + outputPath.wstring();
        MessageBoxW(
            nullptr,
            successMessage.c_str(),
            L"NexPlay - test zakonczony",
            MB_OK | MB_ICONINFORMATION);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Blad: " << error.what() << '\n';
        MessageBoxA(
            nullptr,
            error.what(),
            "NexPlay - blad testu",
            MB_OK | MB_ICONERROR);
        return 1;
    }
}
