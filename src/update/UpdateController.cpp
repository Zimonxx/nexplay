#include "UpdateController.h"
#include "platform/windows/MediaTools.h"
#include <ShlObj.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace nexplay::update {
namespace {
std::wstring quote(const std::wstring& value) {
    // These arguments are local filenames/version strings, never shell source.
    if (value.find(L'"') != std::wstring::npos) throw std::runtime_error("Invalid update path");
    return L"\"" + value + L"\"";
}
}
std::wstring Controller::currentVersion() {
    static const std::wstring version = []() -> std::wstring {
    const auto exe = platform::applicationDirectory() / L"nexplay.exe";
    DWORD ignored{};
    std::vector<BYTE> bytes(GetFileVersionInfoSizeW(exe.c_str(), &ignored));
    if (bytes.empty() || !GetFileVersionInfoW(exe.c_str(), 0, static_cast<DWORD>(bytes.size()), bytes.data())) return L"0.0.0";
    VS_FIXEDFILEINFO* info{}; UINT size{};
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &size) || size < sizeof(*info)) return L"0.0.0";
    return std::to_wstring(HIWORD(info->dwProductVersionMS)) + L"." +
        std::to_wstring(LOWORD(info->dwProductVersionMS)) + L"." +
        std::to_wstring(HIWORD(info->dwProductVersionLS));
    }();
    return version;
}
Controller::~Controller() {
    if (process_) {
        // A staged apply helper must survive the parent's graceful shutdown.
        if (!applying_) TerminateProcess(process_, ERROR_CANCELLED);
        CloseHandle(process_);
    }
}
void Controller::check() {
    if (busy()) return;
    try {
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot create update job");
        wchar_t guid[40]{}; StringFromGUID2(id, guid, 40);
        if (cacheRoot_.empty()) {
            PWSTR local{};
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) throw std::runtime_error("Cannot locate update cache");
            cacheRoot_ = std::filesystem::path(local) / L"NexPlay" / L"Updates";
            CoTaskMemFree(local);
        }
        job_ = std::filesystem::absolute(cacheRoot_) / guid;
        std::filesystem::create_directories(job_);
        const auto resource = FindResourceW(nullptr, L"UPDATER_SCRIPT", RT_RCDATA);
        const auto loaded = resource ? LoadResource(nullptr, resource) : nullptr;
        const auto bytes = loaded ? LockResource(loaded) : nullptr;
        if (!bytes) throw std::runtime_error("Missing updater resource");
        std::ofstream file(job_ / L"update.ps1", std::ios::binary);
        file.write(static_cast<const char*>(bytes), SizeofResource(nullptr, resource));
        file.close();
        if (!file) throw std::runtime_error("Cannot write update helper");
        status_ = {L"checking"}; launch(L"Check");
    } catch (...) { status_ = {L"error", 0, {}, L"Nie można uruchomić sprawdzania aktualizacji."}; }
}
void Controller::download() {
    if (!busy() && status_.phase == L"available") {
        status_.phase = L"downloading"; launch(L"Stage");
    }
}
void Controller::apply() {
    if (!busy() && status_.phase == L"ready") {
        status_.phase = L"preparing"; launch(L"Apply");
    }
}
void Controller::launch(const wchar_t* mode) {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const auto shell = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
    auto command = quote(shell.wstring()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
        quote((job_ / L"update.ps1").wstring()) + L" -Mode " + mode + L" -Job " + quote(job_.wstring()) +
        L" -Target " + quote(platform::applicationDirectory().wstring()) + L" -CurrentVersion " +
        quote(currentVersion()) + L" -ParentId " + std::to_wstring(GetCurrentProcessId());
    // Discard the previous phase so an Apply launch cannot read stale 'ready'.
    std::error_code ignored;
    std::filesystem::remove(job_ / L"status", ignored);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, job_.c_str(), &startup, &process)) {
        status_ = {L"error", 0, {}, L"Nie można uruchomić pomocnika aktualizacji."}; return;
    }
    CloseHandle(process.hThread); process_ = process.hProcess;
    applying_ = wcscmp(mode, L"Apply") == 0;
}
void Controller::poll() {
    if (!process_) return;
    // ReadFile with no writer sharing prevents torn progress records.
    const HANDLE file = CreateFileW((job_ / L"status").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char buffer[4096]{}; DWORD count{};
        if (ReadFile(file, buffer, sizeof(buffer)-1, &count, nullptr) && count) {
            std::wstring text(count, L'\0');
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buffer, count, text.data(), count);
            if (length > 0) {
                text.resize(length); std::wistringstream input(text);
                Status next; std::wstring percent;
                if (std::getline(input, next.phase, L'|') && std::getline(input, percent, L'|') &&
                    std::getline(input, next.version, L'|')) {
                    std::getline(input, next.detail);
                    next.percent = std::clamp(_wtoi(percent.c_str()), 0, 100);
                    status_ = std::move(next);
                }
            }
        }
        CloseHandle(file);
    }
    if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
        DWORD code{}; GetExitCodeProcess(process_, &code);
        CloseHandle(process_); process_ = nullptr;
        if (code != 0 && status_.phase != L"error") status_ = {L"error", 0, {}, L"Pomocnik aktualizacji zakończył się błędem."};
    }
}
}
