#pragma once
#include <Windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nexplay::platform {
inline std::filesystem::path applicationDirectory() {
    std::wstring name(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (!length || length >= name.size())
        return {};
    name.resize(length);
    return std::filesystem::path(name).parent_path();
}

inline bool isToolFile(const std::filesystem::path &file) {
    std::error_code error;
    return file.is_absolute() && std::filesystem::is_regular_file(file, error);
}

inline std::filesystem::path resolveMediaTool(std::wstring argument) {
    if (argument.size() >= 2 && argument.front() == L'"' && argument.back() == L'"')
        argument = argument.substr(1, argument.size() - 2);
    const std::filesystem::path requested(argument);
    if (requested.is_absolute())
        return isToolFile(requested) ? requested : std::filesystem::path{};
    if (requested.has_parent_path() || (argument != L"ffmpeg.exe" && argument != L"ffprobe.exe"))
        return {};
    const auto bundled = applicationDirectory() / L"tools" / L"ffmpeg" / L"bin" / requested;
    if (isToolFile(bundled))
        return bundled;
    // Source builds may use PATH. Never search the current working directory implicitly.
    const auto length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (!length)
        return {};
    std::wstring paths(length, L'\0');
    const auto written = GetEnvironmentVariableW(L"PATH", paths.data(), length);
    if (!written || written >= length)
        return {};
    paths.resize(written);
    for (size_t start = 0; start < paths.size();) {
        const auto end = paths.find(L';', start);
        auto directory = paths.substr(start, end == std::wstring::npos ? end : end - start);
        if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"')
            directory = directory.substr(1, directory.size() - 2);
        if (const auto candidate = std::filesystem::path(directory) / requested;
            !directory.empty() && isToolFile(candidate))
            return candidate;
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    return {};
}

struct MediaCommand {
    std::wstring executable, line;
};
inline std::optional<MediaCommand> prepareMediaCommand(const std::vector<std::wstring> &arguments) {
    if (arguments.empty())
        return {};
    const auto executable = resolveMediaTool(arguments.front());
    if (executable.empty())
        return {};
    MediaCommand result{executable.wstring(), L"\"" + executable.wstring() + L"\""};
    for (size_t i = 1; i < arguments.size(); ++i)
        result.line += L" " + arguments[i];
    return result;
}
} // namespace nexplay::platform
