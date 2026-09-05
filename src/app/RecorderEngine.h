#pragma once

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <thread>

namespace nexplay::app {

struct RecorderSettings {
    std::chrono::seconds bufferDuration{10};
    std::uint32_t framesPerSecond{60};
    std::uint32_t bitrate{25'000'000};
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    bool captureMicrophone{true};
    std::set<DWORD> excludedProcessIds;
};

using StatusCallback = std::function<void(std::wstring)>;

class RecorderEngine final {
public:
    RecorderEngine() = default;
    ~RecorderEngine();

    RecorderEngine(const RecorderEngine&) = delete;
    RecorderEngine& operator=(const RecorderEngine&) = delete;

    void start(RecorderSettings settings, StatusCallback statusCallback);
    void requestSave() noexcept;
    void stop() noexcept;

    [[nodiscard]] bool isRunning() const noexcept;

private:
    void run(std::stop_token stopToken, RecorderSettings settings, StatusCallback statusCallback);

    std::jthread worker_;
    std::atomic_bool running_{};
    std::atomic_bool saveRequested_{};
};

} // namespace nexplay::app
