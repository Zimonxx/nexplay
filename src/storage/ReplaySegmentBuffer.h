#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <vector>

namespace nexplay::storage {

struct StagedReplay {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> segments;
    std::uint64_t frameCount{};
};

class ReplaySegmentBuffer final {
public:
    ReplaySegmentBuffer(
        std::filesystem::path root,
        std::uint32_t framesPerSecond,
        std::chrono::seconds duration);
    ~ReplaySegmentBuffer();

    ReplaySegmentBuffer(const ReplaySegmentBuffer&) = delete;
    ReplaySegmentBuffer& operator=(const ReplaySegmentBuffer&) = delete;

    [[nodiscard]] std::ostream& beginSegment();
    void finishSegment(std::uint32_t frameCount);
    [[nodiscard]] StagedReplay stageLatest();
    void reset();

    [[nodiscard]] const std::filesystem::path& sessionDirectory() const noexcept;
    [[nodiscard]] std::uint64_t bufferedFrames() const noexcept;

private:
    struct Segment {
        std::filesystem::path path;
        std::uint32_t frameCount{};
    };

    void prune();
    void cleanSessionDirectory() noexcept;

    std::filesystem::path root_;
    std::filesystem::path sessionDirectory_;
    std::uint32_t framesPerSecond_{};
    std::uint64_t maximumFrames_{};
    std::uint64_t bufferedFrames_{};
    std::uint64_t nextSegmentNumber_{};
    std::uint64_t nextSaveNumber_{};
    std::filesystem::path activePath_;
    std::ofstream activeStream_;
    std::deque<Segment> segments_;
};

} // namespace nexplay::storage
