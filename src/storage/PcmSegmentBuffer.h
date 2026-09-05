#pragma once

#include "audio/PcmAudioCapture.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace nexplay::storage {

struct StagedPcmTrack {
    std::wstring name;
    audio::PcmFormat format;
    std::vector<std::filesystem::path> segments;
    std::uint64_t frameCount{};
};

class PcmSegmentBuffer final {
public:
    PcmSegmentBuffer(
        std::filesystem::path root,
        std::wstring trackId,
        std::chrono::seconds duration);
    ~PcmSegmentBuffer();

    PcmSegmentBuffer(const PcmSegmentBuffer&) = delete;
    PcmSegmentBuffer& operator=(const PcmSegmentBuffer&) = delete;

    void append(
        audio::PcmFormat format,
        std::span<const std::byte> samples,
        std::uint32_t frameCount);
    void finishSegment();
    [[nodiscard]] StagedPcmTrack stageLatest(
        const std::filesystem::path& saveDirectory,
        std::wstring trackName,
        std::uint32_t trackIndex);
    void reset();

    [[nodiscard]] std::uint64_t bufferedFrames() const noexcept;

private:
    struct Segment {
        std::filesystem::path path;
        std::uint64_t frameCount{};
    };

    void beginSegment();
    void finishSegmentUnlocked();
    void prune();
    void cleanSessionDirectory() noexcept;

    std::filesystem::path root_;
    std::filesystem::path sessionDirectory_;
    std::chrono::seconds duration_;
    audio::PcmFormat format_{};
    bool hasFormat_{};
    std::uint64_t maximumFrames_{};
    std::uint64_t bufferedFrames_{};
    std::uint64_t activeFrames_{};
    std::uint64_t nextSegmentNumber_{};
    std::filesystem::path activePath_;
    std::ofstream activeStream_;
    std::deque<Segment> segments_;
    mutable std::mutex mutex_;
};

} // namespace nexplay::storage
