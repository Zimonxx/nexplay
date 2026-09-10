#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace nexplay::audio {

struct AudioApplication {
    DWORD processId{};
    std::wstring name;
    std::wstring applicationId; // Executable filename, independent of PID/install folder.
};

[[nodiscard]] std::vector<AudioApplication> activeAudioApplications();

} // namespace nexplay::audio
