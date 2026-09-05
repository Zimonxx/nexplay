#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace nexplay::audio {

struct AudioApplication {
    DWORD processId{};
    std::wstring name;
};

[[nodiscard]] std::vector<AudioApplication> activeAudioApplications();

} // namespace nexplay::audio
