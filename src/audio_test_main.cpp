#include "audio/AudioSessionScanner.h"
#include "audio/PcmAudioCapture.h"
#include "storage/ReplayStorage.h"

#include <Windows.h>
#include <objbase.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

[[nodiscard]] std::wstring safeName(std::wstring name) {
    constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
    for (wchar_t& character : name) {
        if (invalid.find(character) != std::wstring_view::npos) {
            character = L'_';
        }
    }
    return name;
}

} // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    const bool showDialog = argumentCount < 2 || std::wstring_view(arguments[1]) != L"--no-dialog";
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        MessageBoxW(nullptr, L"Nie mozna uruchomic COM.", L"NexPlay - audio", MB_OK | MB_ICONERROR);
        return 1;
    }

    try {
        nexplay::storage::ReplayStorage storage;
        storage.ensureExists();
        const std::filesystem::path outputDirectory = storage.root() /
            (L"audio-test-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(outputDirectory);

        const auto applications = nexplay::audio::activeAudioApplications();
        std::wcout << L"Aktywne aplikacje audio: " << applications.size() << L"\n";

        std::vector<std::unique_ptr<nexplay::audio::PcmAudioCapture>> captures;
        for (const auto& application : applications) {
            try {
                auto capture = std::make_unique<nexplay::audio::PcmAudioCapture>();
                const std::filesystem::path path = outputDirectory /
                    (safeName(application.name) + L"-" +
                     std::to_wstring(application.processId) + L".wav");
                capture->startProcess(application.processId, path);
                std::wcout << L"  " << application.name << L" (PID "
                           << application.processId << L")\n";
                captures.push_back(std::move(capture));
            } catch (const std::exception& error) {
                std::cerr << "Pominieto proces audio: " << error.what() << '\n';
            }
        }

        try {
            auto microphone = std::make_unique<nexplay::audio::PcmAudioCapture>();
            microphone->startDefaultMicrophone(outputDirectory / L"Microphone.wav");
            captures.push_back(std::move(microphone));
            std::wcout << L"  Domyslny mikrofon\n";
        } catch (const std::exception& error) {
            std::cerr << "Pominieto mikrofon: " << error.what() << '\n';
        }

        if (captures.empty()) {
            throw std::runtime_error("Nie udalo sie uruchomic zadnego zrodla audio.");
        }

        std::wcout << L"Nagrywanie audio przez 10 sekund...\n";
        std::this_thread::sleep_for(std::chrono::seconds(10));
        for (auto& capture : captures) {
            capture->stop();
        }
        captures.clear();

        const std::wstring message =
            L"Test audio zakonczony. Osobne sciezki WAV zapisano tutaj:\n\n" +
            outputDirectory.wstring();
        if (showDialog) {
            MessageBoxW(nullptr, message.c_str(), L"NexPlay - audio gotowe", MB_OK | MB_ICONINFORMATION);
        } else {
            std::wcout << message << L'\n';
        }
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "NexPlay - blad audio", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
}
