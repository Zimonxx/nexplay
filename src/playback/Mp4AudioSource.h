#pragma once
#include <Windows.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <vector>

namespace nexplay::playback {
struct Mp4Track {
    std::uint32_t id{};
    std::uint64_t typeOffset{};
    bool audio{};
};
// Bounded metadata-only parsing. No payload decoding or writes to the recording.
std::vector<Mp4Track> readMp4Tracks(std::istream &input, std::uint64_t length);
Microsoft::WRL::ComPtr<IMFMediaSource> audioTrackSource(const std::filesystem::path &file,
                                                        std::uint32_t mp4TrackId);
} // namespace nexplay::playback
