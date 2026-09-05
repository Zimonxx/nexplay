#include "platform/windows/SystemProbe.h"
#include "storage/ReplayStorage.h"

#include <exception>
#include <iomanip>
#include <iostream>

int wmain() {
    try {
        const auto windows = nexplay::platform::queryWindowsVersion();
        const auto monitor = nexplay::platform::queryPrimaryMonitor();
        const auto nvenc = nexplay::platform::queryNvencDriver();

        nexplay::storage::ReplayStorage replayStorage;
        replayStorage.ensureExists();

        std::wcout << L"NexPlay - diagnostyka srodowiska\n\n";
        std::wcout << L"Windows: " << windows.major << L'.' << windows.minor
                   << L" (build " << windows.build << L")\n";
        std::wcout << L"Glowny monitor: " << monitor.deviceName << L' '
                   << monitor.width << L'x' << monitor.height << L" @ "
                   << monitor.refreshRate << L" Hz\n";
        std::wcout << L"NVENC: " << (nvenc.apiAvailable ? L"dostepny" : L"niedostepny") << L'\n';

        if (nvenc.apiAvailable) {
            std::wcout << L"Maksymalna wersja API NVENC sterownika: "
                       << nvenc.maximumApiMajor << L'.' << nvenc.maximumApiMinor
                       << L" (0x" << std::hex << nvenc.maximumApiVersion << std::dec << L")\n";
        } else if (!nvenc.libraryAvailable) {
            std::wcout << L"Nie znaleziono nvEncodeAPI64.dll. Sprawdz sterownik NVIDIA.\n";
        }

        std::wcout << L"Katalog bufora: " << replayStorage.root().wstring() << L'\n';
        std::wcout << L"Domyslny klip: 10 sekund; maksymalny klip: 20 minut.\n";
        return nvenc.apiAvailable ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "Blad: " << error.what() << '\n';
        return 1;
    }
}
