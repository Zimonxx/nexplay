#include "storage/ReplaySegmentBuffer.h"

#include <Windows.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace nexplay::storage {
namespace {

[[nodiscard]] std::wstring paddedNumber(const std::uint64_t value) {
    std::wostringstream text;
    text << std::setw(8) << std::setfill(L'0') << value;
    return text.str();
}

} // namespace

ReplaySegmentBuffer::ReplaySegmentBuffer(
    std::filesystem::path root,
    const std::uint32_t framesPerSecond,
    const std::chrono::seconds duration)
    : root_(std::move(root)),
      framesPerSecond_(framesPerSecond),
      maximumFrames_(static_cast<std::uint64_t>(framesPerSecond) * duration.count()) {
    if (framesPerSecond_ == 0 || duration < std::chrono::seconds(10) ||
        duration > std::chrono::minutes(20)) {
        throw std::invalid_argument("Bufor musi miec od 10 sekund do 20 minut.");
    }

    sessionDirectory_ = root_ /
        (L"session-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::create_directories(sessionDirectory_, error);
    if (error) {
        throw std::runtime_error("Nie mozna utworzyc sesji bufora w katalogu TEMP.");
    }
}

ReplaySegmentBuffer::~ReplaySegmentBuffer() {
    if (activeStream_.is_open()) {
        activeStream_.close();
    }
    cleanSessionDirectory();
}

std::ostream& ReplaySegmentBuffer::beginSegment() {
    if (activeStream_.is_open()) {
        throw std::logic_error("Segment bufora jest juz otwarty.");
    }

    activePath_ = sessionDirectory_ /
        (L"segment-" + paddedNumber(nextSegmentNumber_++) + L".h264");
    activeStream_.open(activePath_, std::ios::binary | std::ios::trunc);
    if (!activeStream_) {
        throw std::runtime_error("Nie mozna utworzyc segmentu bufora w TEMP.");
    }
    return activeStream_;
}

void ReplaySegmentBuffer::finishSegment(const std::uint32_t frameCount) {
    if (!activeStream_.is_open()) {
        return;
    }

    activeStream_.close();
    if (frameCount == 0) {
        std::error_code error;
        std::filesystem::remove(activePath_, error);
    } else {
        segments_.push_back({activePath_, frameCount});
        bufferedFrames_ += frameCount;
        prune();
    }
    activePath_.clear();
}

StagedReplay ReplaySegmentBuffer::stageLatest() {
    if (segments_.empty()) {
        throw std::runtime_error("Bufor nie zawiera jeszcze zadnych klatek.");
    }

    StagedReplay result;
    result.directory = sessionDirectory_ /
        (L"save-" + paddedNumber(nextSaveNumber_++));
    std::error_code error;
    std::filesystem::create_directories(result.directory, error);
    if (error) {
        throw std::runtime_error("Nie mozna przygotowac zapisu klipu.");
    }

    std::vector<const Segment*> selected;
    std::uint64_t selectedFrames{};
    for (auto segment = segments_.rbegin(); segment != segments_.rend(); ++segment) {
        selected.push_back(&*segment);
        selectedFrames += segment->frameCount;
        if (selectedFrames >= maximumFrames_) {
            break;
        }
    }
    std::reverse(selected.begin(), selected.end());

    for (std::size_t index = 0; index < selected.size(); ++index) {
        const std::filesystem::path stagedPath = result.directory /
            (L"part-" + paddedNumber(index) + L".h264");
        std::filesystem::create_hard_link(selected[index]->path, stagedPath, error);
        if (error) {
            error.clear();
            std::filesystem::copy_file(
                selected[index]->path,
                stagedPath,
                std::filesystem::copy_options::overwrite_existing,
                error);
        }
        if (error) {
            throw std::runtime_error("Nie mozna przygotowac segmentow do zapisu klipu.");
        }
        result.segments.push_back(stagedPath);
        result.frameCount += selected[index]->frameCount;
    }
    return result;
}

void ReplaySegmentBuffer::reset() {
    if (activeStream_.is_open()) {
        throw std::logic_error("Nie mozna wyzerowac bufora z otwartym segmentem.");
    }

    for (const auto& segment : segments_) {
        std::error_code error;
        std::filesystem::remove(segment.path, error);
    }
    segments_.clear();
    bufferedFrames_ = 0;
}

const std::filesystem::path& ReplaySegmentBuffer::sessionDirectory() const noexcept {
    return sessionDirectory_;
}

std::uint64_t ReplaySegmentBuffer::bufferedFrames() const noexcept {
    return bufferedFrames_;
}

void ReplaySegmentBuffer::prune() {
    while (segments_.size() > 1 &&
           bufferedFrames_ - segments_.front().frameCount >= maximumFrames_) {
        const Segment oldest = std::move(segments_.front());
        segments_.pop_front();
        bufferedFrames_ -= oldest.frameCount;
        std::error_code error;
        std::filesystem::remove(oldest.path, error);
    }
}

void ReplaySegmentBuffer::cleanSessionDirectory() noexcept {
    std::error_code error;
    const auto normalizedRoot = std::filesystem::weakly_canonical(root_, error);
    if (error) {
        return;
    }
    const auto normalizedSession = std::filesystem::weakly_canonical(sessionDirectory_, error);
    if (error || normalizedSession.parent_path() != normalizedRoot) {
        return;
    }
    std::filesystem::remove_all(normalizedSession, error);
}

} // namespace nexplay::storage
