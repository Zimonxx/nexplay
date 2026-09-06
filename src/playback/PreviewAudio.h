#pragma once

#include <Windows.h>
#include <mfplay.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdint>
#include <vector>

namespace nexplay::playback {

struct AudioSelection {
    bool included{true};
    double start{};
    double end{};
    std::uint32_t trackId{}; // Stable MP4 tkhd.track_ID, as reported by ffprobe stream.id.
};

[[nodiscard]] inline bool audible(const AudioSelection &track, double seconds) noexcept {
    return track.included && seconds >= track.start && seconds < track.end;
}

// Video stays muted. Each original audio stream has an independently muted player;
// switching an edit does not rebuild the media topology or re-encode the recording.
class PreviewAudio final {
  public:
    ~PreviewAudio();
    void open(const std::filesystem::path &file, const std::vector<AudioSelection> &tracks);
    void close() noexcept;
    void pause() noexcept;
    void seek(double seconds) noexcept;
    void update(const std::vector<AudioSelection> &tracks, double seconds, bool playing) noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return players_.size(); }
    [[nodiscard]] bool muted(std::size_t track) const noexcept;
    [[nodiscard]] bool ready() const noexcept;

  private:
    struct TrackPlayer {
        Microsoft::WRL::ComPtr<IMFPMediaPlayer> player;
        bool muted{true};
        std::uint32_t trackId{};
    };
    std::vector<TrackPlayer> players_;
    bool playing_{};
    ULONGLONG lastSync_{};
};
} // namespace nexplay::playback
