#include "audio/AudioSourcePreferences.h"
#include <iostream>
#include <stdexcept>

using namespace nexplay::audio;
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
int main(int argc, char**) {
    try {
        ExcludedApplications excluded{L"Zen.exe", L"Discord.exe", L"Adobe Premiere Pro.exe"};
        const auto restored = decodeExclusions(encodeExclusions(excluded));
        require(restored == excluded, "Exclusions round-trip failed");
        require(isApplicationExcluded(restored, L"ZEN.EXE"), "Executable matching is case-sensitive");
        require(!isApplicationExcluded(restored, L""), "Unknown process matched");
        require(!isApplicationExcluded(restored, L"zen-helper.exe"), "Partial executable name matched");
        require(decodeExclusions(encodeExclusions({})).empty(), "Empty exclusions failed");
        require(decodeExclusions({L'a', 0}).empty(), "Unterminated list accepted");
        require(decodeExclusions({}).empty(), "Missing list accepted");
        excluded.erase(L"ZEN.EXE");
        require(!isApplicationExcluded(decodeExclusions(encodeExclusions(excluded)), L"zen.exe"), "Re-enable not persisted");
        struct Row { int pid; bool included; std::wstring group; };
        std::vector<Row> rows{{1, false, L"group"}, {2, true, L""}, {3, true, L"group"}, {4, false, L""}};
        sortAudioSources(rows);
        require(rows[0].pid == 2 && rows[1].pid == 3 && rows[2].pid == 1 && rows[3].pid == 4,
            "Enabled-first ordering is not stable");
        require(rows[2].group == L"group", "Sorting lost group metadata");
        rows[2].included = true;
        sortAudioSources(rows);
        require(rows[2].pid == 1 && rows.back().pid == 4, "Re-enabled source stayed below disabled ones");
        if (argc > 1) {
            // Optional integration test: a unique volatile key, never user settings.
            const auto keyPath = L"Software\\NexPlay.AudioPreferencesTest-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64());
            HKEY key{};
            require(RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                KEY_READ | KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS, "Cannot create isolated test key");
            struct Cleanup {
                HKEY key; std::wstring path;
                ~Cleanup() { RegCloseKey(key); RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()); }
            } cleanup{key, keyPath};
            require(loadAudioExclusions(key, nullptr).empty(), "Missing registry value failed");
            require(saveAudioExclusions(key, nullptr, restored), "Registry save failed");
            require(loadAudioExclusions(key, nullptr) == restored, "Registry reload failed");
            require(saveAudioExclusions(key, nullptr, excluded), "Registry re-enable save failed");
            require(loadAudioExclusions(key, nullptr) == excluded, "Registry re-enable reload failed");
            require(saveAudioExclusions(key, nullptr, {}), "Empty registry save failed");
            require(loadAudioExclusions(key, nullptr).empty(), "Cleared registry exclusions remained");
        }
        std::cout << "PASS: audio source persistence, case-insensitive identities, re-enable and stable ordering.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
