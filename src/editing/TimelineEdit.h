#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace nexplay::editing {
struct TimeRange {
    double start{}, end{};
    double duration() const noexcept { return std::max(0.0, end - start); }
    bool contains(double time) const noexcept { return time >= start && time < end; }
};
struct TimelineEdit {
    double start{}, end{}, duration{};
    bool cut{};
    std::vector<TimeRange> keptRanges() const {
        if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(duration) ||
            duration <= 0 || start < 0 || end > duration || start >= end)
            return {};
        if (!cut)
            return {{start, end}};
        std::vector<TimeRange> ranges;
        if (start > 0)
            ranges.push_back({0, start});
        if (end < duration)
            ranges.push_back({end, duration});
        return ranges;
    }
    std::vector<TimeRange> audioRanges(double audioStart, double audioEnd) const {
        std::vector<TimeRange> ranges;
        for (const auto kept : keptRanges()) {
            const TimeRange range{std::max(kept.start, audioStart), std::min(kept.end, audioEnd)};
            if (range.duration() > 0)
                ranges.push_back(range);
        }
        return ranges;
    }
    bool contains(double time) const {
        const auto ranges = keptRanges();
        return std::any_of(ranges.begin(), ranges.end(),
                           [time](const TimeRange &range) { return range.contains(time); });
    }
    double outputDuration() const {
        double result{};
        for (const auto range : keptRanges())
            result += range.duration();
        return result;
    }
};
} // namespace nexplay::editing
