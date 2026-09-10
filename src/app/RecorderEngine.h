#pragma once

#include <Windows.h>
#include "SaveProgress.h"
#include "audio/AudioSourcePreferences.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <deque>
#include <mutex>

namespace nexplay::app {

struct RecorderSettings {
    std::chrono::seconds bufferDuration{10};
    std::uint32_t framesPerSecond{60};
    std::uint32_t bitrate{25'000'000};
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    bool captureMicrophone{true};
    std::set<DWORD> excludedProcessIds;
    audio::ExcludedApplications excludedApplications;
    std::map<DWORD, std::wstring> groupedProcessNames;
};

using StatusCallback = std::function<void(std::wstring)>;

class RecorderEngine final {
public:
    RecorderEngine() = default;
    ~RecorderEngine();

    RecorderEngine(const RecorderEngine&) = delete;
    RecorderEngine& operator=(const RecorderEngine&) = delete;

    void start(RecorderSettings settings, StatusCallback statusCallback,
               SaveCallback saveCallback = {});
    SaveId requestSave();
    void requestStop() noexcept;
    void stop() noexcept;

    [[nodiscard]] bool isRunning() const noexcept;

private:
    void run(std::stop_token stopToken, RecorderSettings settings, StatusCallback statusCallback,
             SaveCallback saveCallback);

    std::jthread worker_;
    std::atomic_bool running_{};
    std::mutex saveMutex_;
    std::deque<SaveId> pendingSaves_;
    SaveId nextSaveId_{1};
};

} // namespace nexplay::app
