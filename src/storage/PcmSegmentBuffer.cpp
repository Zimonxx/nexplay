#include "storage/PcmSegmentBuffer.h"

#include <Windows.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace nexplay::storage {
namespace {

[[nodiscard]] std::wstring paddedNumber(const std::uint64_t value) {
    std::wostringstream text;
    text << std::setw(8) << std::setfill(L'0') << value;
    return text.str();
}

[[nodiscard]] bool sameFormat(const audio::PcmFormat left, const audio::PcmFormat right) {
    return left.sampleRate == right.sampleRate &&
        left.channels == right.channels &&
        left.bitsPerSample == right.bitsPerSample &&
        left.blockAlign == right.blockAlign;
}

} // namespace

PcmSegmentBuffer::PcmSegmentBuffer(
    std::filesystem::path root,
    std::wstring trackId,
    const std::chrono::seconds duration)
    : root_(std::move(root)), duration_(duration) {
    if (duration < std::chrono::seconds(10) || duration > std::chrono::minutes(20)) {
        throw std::invalid_argument("Bufor audio musi miec od 10 sekund do 20 minut.");
    }
    if (trackId.empty() || trackId.find_first_of(L"\\/:") != std::wstring::npos) {
        throw std::invalid_argument("Nieprawidlowy identyfikator sciezki audio.");
    }

    sessionDirectory_ = root_ / (L"audio-" + trackId);
    std::error_code error;
    std::filesystem::create_directories(sessionDirectory_, error);
    if (error) {
        throw std::runtime_error("Nie mozna utworzyc bufora audio w TEMP.");
    }
}

PcmSegmentBuffer::~PcmSegmentBuffer() {
    {
        std::scoped_lock lock(mutex_);
        if (activeStream_.is_open()) {
            activeStream_.close();
        }
    }
    cleanSessionDirectory();
}

void PcmSegmentBuffer::append(
    const audio::PcmFormat format,
    const std::span<const std::byte> samples,
    const std::uint32_t frameCount) {
    std::scoped_lock lock(mutex_);
    if (format.sampleRate == 0 || format.channels == 0 || format.bitsPerSample != 16 ||
        format.blockAlign == 0) {
        throw std::runtime_error("Nieobslugiwany format probek audio.");
    }
    if (samples.size() != static_cast<std::size_t>(frameCount) * format.blockAlign) {
        throw std::runtime_error("Nieprawidlowy rozmiar pakietu audio.");
    }
    if (!hasFormat_) {
        format_ = format;
        hasFormat_ = true;
        maximumFrames_ = static_cast<std::uint64_t>(format.sampleRate) * duration_.count();
    } else if (!sameFormat(format_, format)) {
        throw std::runtime_error("Format sciezki audio zmienil sie podczas nagrywania.");
    }

    std::uint64_t remaining = frameCount;
    std::size_t byteOffset{};
    while (remaining > 0) {
        if (!activeStream_.is_open()) {
            beginSegment();
        }
        const std::uint64_t available = format_.sampleRate - activeFrames_;
        const std::uint64_t toWrite = std::min(remaining, available);
        const std::size_t byteCount = static_cast<std::size_t>(toWrite) * format_.blockAlign;
        activeStream_.write(
            reinterpret_cast<const char*>(samples.data() + byteOffset),
            static_cast<std::streamsize>(byteCount));
        if (!activeStream_) {
            throw std::runtime_error("Nie mozna zapisac segmentu audio w TEMP.");
        }
        activeFrames_ += toWrite;
        remaining -= toWrite;
        byteOffset += byteCount;
        if (activeFrames_ == format_.sampleRate) {
            finishSegmentUnlocked();
        }
    }
}

void PcmSegmentBuffer::finishSegment() {
    std::scoped_lock lock(mutex_);
    finishSegmentUnlocked();
}

StagedPcmTrack PcmSegmentBuffer::stageLatest(
    const std::filesystem::path& saveDirectory,
    std::wstring trackName,
    const std::uint32_t trackIndex) {
    std::scoped_lock lock(mutex_);
    if (activeStream_.is_open()) {
        throw std::logic_error("Nie mozna zapisac aktywnego segmentu audio.");
    }
    if (!hasFormat_ || segments_.empty()) {
        throw std::runtime_error("Sciezka audio nie zawiera jeszcze probek.");
    }

    StagedPcmTrack result{
        .name = std::move(trackName),
        .format = format_,
    };
    const std::filesystem::path trackDirectory =
        saveDirectory / (L"audio-" + paddedNumber(trackIndex));
    std::error_code error;
    std::filesystem::create_directories(trackDirectory, error);
    if (error) {
        throw std::runtime_error("Nie mozna przygotowac zapisu sciezki audio.");
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
        const auto stagedPath = trackDirectory /
            (L"part-" + paddedNumber(index) + L".pcm");
        error.clear();
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
            throw std::runtime_error("Nie mozna przygotowac segmentow audio do zapisu.");
        }
        result.segments.push_back(stagedPath);
        result.frameCount += selected[index]->frameCount;
    }
    return result;
}

void PcmSegmentBuffer::reset() {
    std::scoped_lock lock(mutex_);
    if (activeStream_.is_open()) {
        throw std::logic_error("Nie mozna wyzerowac aktywnego bufora audio.");
    }
    for (const auto& segment : segments_) {
        std::error_code error;
        std::filesystem::remove(segment.path, error);
    }
    segments_.clear();
    bufferedFrames_ = 0;
}

std::uint64_t PcmSegmentBuffer::bufferedFrames() const noexcept {
    std::scoped_lock lock(mutex_);
    return bufferedFrames_ + activeFrames_;
}

void PcmSegmentBuffer::beginSegment() {
    activePath_ = sessionDirectory_ /
        (L"segment-" + paddedNumber(nextSegmentNumber_++) + L".pcm");
    activeStream_.open(activePath_, std::ios::binary | std::ios::trunc);
    if (!activeStream_) {
        throw std::runtime_error("Nie mozna utworzyc segmentu audio w TEMP.");
    }
    activeFrames_ = 0;
}

void PcmSegmentBuffer::finishSegmentUnlocked() {
    if (!activeStream_.is_open()) {
        return;
    }
    activeStream_.close();
    if (activeFrames_ == 0) {
        std::error_code error;
        std::filesystem::remove(activePath_, error);
    } else {
        segments_.push_back({activePath_, activeFrames_});
        bufferedFrames_ += activeFrames_;
        prune();
    }
    activePath_.clear();
    activeFrames_ = 0;
}

void PcmSegmentBuffer::prune() {
    while (segments_.size() > 1 &&
           bufferedFrames_ - segments_.front().frameCount >= maximumFrames_) {
        const Segment oldest = std::move(segments_.front());
        segments_.pop_front();
        bufferedFrames_ -= oldest.frameCount;
        std::error_code error;
        std::filesystem::remove(oldest.path, error);
    }
}

void PcmSegmentBuffer::cleanSessionDirectory() noexcept {
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
