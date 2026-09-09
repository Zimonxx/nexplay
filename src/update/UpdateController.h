#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>

namespace nexplay::update {
struct Status {
    std::wstring phase{L"idle"};
    int percent{};
    std::wstring version, detail;
};
// A hidden, low-priority helper does all network/disk work. The UI only polls a
// tiny status file; it never waits for downloads or modifies running binaries.
class Controller final {
public:
    // An explicit cache root lets integration tests stay inside their workspace.
    explicit Controller(std::filesystem::path cacheRoot = {}) : cacheRoot_(std::move(cacheRoot)) {}
    ~Controller();
    void check();
    void download();
    void apply();
    void poll();
    const Status& status() const noexcept { return status_; }
    bool busy() const noexcept { return process_ != nullptr; }
    static std::wstring currentVersion();
private:
    void launch(const wchar_t* mode);
    HANDLE process_{};
    bool applying_{};
    std::filesystem::path job_;
    std::filesystem::path cacheRoot_;
    Status status_;
};
}
