#include "FfmpegProgress.h"
#include "SaveProgress.h"
#include "platform/windows/MediaTools.h"
#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace nexplay::app {
namespace {
struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    void close() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = nullptr;
    }
};
struct Attributes {
    std::unique_ptr<std::byte[]> bytes;
    LPPROC_THREAD_ATTRIBUTE_LIST value{};
    ~Attributes() {
        if (value)
            DeleteProcThreadAttributeList(value);
    }
};
} // namespace
DWORD runFfmpegProgress(const std::vector<std::wstring> &arguments, double durationSeconds,
                        const std::function<void(int)> &progress) {
    auto command = platform::prepareMediaCommand(arguments);
    if (!command)
        throw std::runtime_error("Brakuje FFmpeg. Zainstaluj ponownie pełną paczkę NexPlay.");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle reader, writer, input;
    if (!CreatePipe(&reader.value, &writer.value, &security, 0) ||
        !SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("Nie można odczytać postępu zapisu.");
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                              OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Nie można uruchomić FFmpeg.");
    SIZE_T bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    Attributes attributes;
    attributes.bytes = std::make_unique<std::byte[]>(bytes);
    auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.bytes.get());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes))
        throw std::runtime_error("Nie można przygotować procesu zapisu.");
    attributes.value = list;
    // Concurrent saves must not inherit one another's pipes (which prevents EOF).
    const HANDLE inherited[]{writer.value, input.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   const_cast<HANDLE *>(inherited), sizeof(inherited), nullptr,
                                   nullptr))
        throw std::runtime_error("Nie można przygotować uchwytów zapisu.");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = writer.value;
    startup.StartupInfo.hStdError = writer.value;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(command->executable.c_str(), command->line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &startup.StartupInfo, &process))
        throw std::runtime_error("Nie można uruchomić programu FFmpeg.");
    Handle processHandle{process.hProcess}, threadHandle{process.hThread};
    writer.close();
    FfmpegProgressParser parser(durationSeconds);
    std::string line;
    std::array<char, 4096> buffer{};
    DWORD count{};
    int last = -1;
    while (
        ReadFile(reader.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) &&
        count) {
        for (DWORD i = 0; i < count; ++i) {
            if (buffer[i] == '\n' || buffer[i] == '\r') {
                const int percent = parser.line(line);
                line.clear();
                if (percent != last) {
                    last = percent;
                    if (progress)
                        progress(percent);
                }
            } else if (line.size() < 8192)
                line += buffer[i];
        }
    }
    WaitForSingleObject(processHandle.value, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(processHandle.value, &exitCode);
    return exitCode;
}
} // namespace nexplay::app
