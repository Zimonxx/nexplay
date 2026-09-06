#pragma once
#include <map>

namespace nexplay::playback {
// A cache miss must hold the last displayed scrub frame, never flash the poster.
// With no previous frame, use the closest cached frame while decoding catches up.
template <class Value>
[[nodiscard]] int thumbnailFrame(const std::map<int, Value> &frames, int requested, int previous) {
    if (frames.empty())
        return -1;
    if (frames.contains(requested))
        return requested;
    if (frames.contains(previous))
        return previous;
    auto next = frames.lower_bound(requested);
    if (next == frames.begin())
        return next->first;
    if (next == frames.end())
        return std::prev(next)->first;
    const auto before = std::prev(next);
    return requested - before->first <= next->first - requested ? before->first : next->first;
}
} // namespace nexplay::playback
