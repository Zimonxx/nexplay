#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace nexplay::app {
using SaveId = std::uint64_t;
enum class SavePhase { queued, preparing, encoding, finalizing, saved, failed };
struct SaveProgress {
    SaveId id{};
    SavePhase phase{SavePhase::queued};
    int percent{};
    std::wstring detail;
};
using SaveCallback = std::function<void(SaveProgress)>;

// FFmpeg reports encoded media time in microseconds, not wall-clock time.
class FfmpegProgressParser {
  public:
    explicit FfmpegProgressParser(double durationSeconds) : duration_(durationSeconds) {}
    int line(std::string_view text) noexcept {
        constexpr std::string_view prefix = "out_time_us=";
        if (text.starts_with(prefix) && duration_ > 0) {
            try {
                const auto microseconds = std::stoll(std::string(text.substr(prefix.size())));
                const double ratio = std::clamp(microseconds / 1'000'000.0 / duration_, 0.0, 1.0);
                percent_ = std::max(percent_, static_cast<int>(ratio * 99));
            } catch (...) {
            }
        }
        // Never claim success until the process exits successfully and the file exists.
        return percent_;
    }

  private:
    double duration_{};
    int percent_{};
};
} // namespace nexplay::app
