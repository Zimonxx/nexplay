#pragma once

#include <cstdint>
#include <string>

namespace nexplay::platform {

struct WindowsVersion {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t build{};
};

struct PrimaryMonitor {
    std::wstring deviceName;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t refreshRate{};
};

struct NvencDriver {
    bool libraryAvailable{};
    bool apiAvailable{};
    std::uint32_t maximumApiVersion{};
    std::uint32_t maximumApiMajor{};
    std::uint32_t maximumApiMinor{};
};

[[nodiscard]] WindowsVersion queryWindowsVersion();
[[nodiscard]] PrimaryMonitor queryPrimaryMonitor();
[[nodiscard]] NvencDriver queryNvencDriver();

} // namespace nexplay::platform
