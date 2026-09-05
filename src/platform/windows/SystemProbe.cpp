#include "platform/windows/SystemProbe.h"

#include <Windows.h>
#include <winternl.h>

namespace nexplay::platform {
namespace {

using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
using NvEncodeApiGetMaxSupportedVersionFunction = int(__stdcall*)(std::uint32_t*);

} // namespace

WindowsVersion queryWindowsVersion() {
    WindowsVersion result{};

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return result;
    }

    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtlGetVersion == nullptr) {
        return result;
    }

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion(&version) != 0) {
        return result;
    }

    result.major = version.dwMajorVersion;
    result.minor = version.dwMinorVersion;
    result.build = version.dwBuildNumber;
    return result;
}

PrimaryMonitor queryPrimaryMonitor() {
    PrimaryMonitor result{};

    POINT origin{};
    const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    if (monitor == nullptr) {
        return result;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return result;
    }

    result.deviceName = info.szDevice;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) != FALSE) {
        result.width = mode.dmPelsWidth;
        result.height = mode.dmPelsHeight;
        result.refreshRate = mode.dmDisplayFrequency;
    } else {
        result.width = static_cast<std::uint32_t>(info.rcMonitor.right - info.rcMonitor.left);
        result.height = static_cast<std::uint32_t>(info.rcMonitor.bottom - info.rcMonitor.top);
    }

    return result;
}

NvencDriver queryNvencDriver() {
    NvencDriver result{};

    const HMODULE nvenc = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (nvenc == nullptr) {
        return result;
    }

    result.libraryAvailable = true;
    const auto getMaximumVersion = reinterpret_cast<NvEncodeApiGetMaxSupportedVersionFunction>(
        GetProcAddress(nvenc, "NvEncodeAPIGetMaxSupportedVersion"));

    if (getMaximumVersion != nullptr) {
        std::uint32_t version{};
        if (getMaximumVersion(&version) == 0) {
            result.apiAvailable = true;
            result.maximumApiVersion = version;
            // NvEncodeAPIGetMaxSupportedVersion uses (major << 4) | minor.
            result.maximumApiMajor = version >> 4U;
            result.maximumApiMinor = version & 0x0FU;
        }
    }

    FreeLibrary(nvenc);
    return result;
}

} // namespace nexplay::platform
