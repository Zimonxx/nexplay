#pragma once
#include "app/SaveProgress.h"
#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <vector>
#include <utility>

namespace nexplay::ui {
enum class ToastCorner { topLeft, topRight, bottomLeft, bottomRight };
struct ToastEntry {
    app::SaveProgress progress;
    bool visible{};
    std::uint64_t entered{};
    std::optional<std::uint64_t> finishedVisible;
    bool terminal() const {
        return progress.phase == app::SavePhase::saved || progress.phase == app::SavePhase::failed;
    }
    std::uint64_t holdMs() const { return progress.phase == app::SavePhase::failed ? 5000 : 2000; }
    float opacity(std::uint64_t now) const {
        const float appear = std::clamp((now - entered) / 160.0F, 0.0F, 1.0F);
        if (!finishedVisible || now < *finishedVisible + holdMs())
            return appear;
        return std::min(
            appear, 1.0F - std::clamp((now - *finishedVisible - holdMs()) / 180.0F, 0.0F, 1.0F));
    }
};
class SaveToastModel {
  public:
    void apply(app::SaveProgress progress) {
        if (!progress.id || retired_.contains(progress.id))
            return;
        auto found = std::find_if(entries.begin(), entries.end(), [&](const auto &item) {
            return item.progress.id == progress.id;
        });
        if (found == entries.end()) {
            entries.push_back({std::move(progress)});
            found = std::prev(entries.end());
        } else {
            if (found->terminal())
                return;
            progress.percent = std::max(progress.percent, found->progress.percent);
            if (progress.phase < found->progress.phase)
                return;
            if (progress.detail.empty())
                progress.detail = found->progress.detail;
            found->progress = std::move(progress);
        }
        found->progress.percent = found->progress.phase == app::SavePhase::saved
                                      ? 100
                                      : std::clamp(found->progress.percent, 0, 99);
    }
    void advance(std::uint64_t now, std::size_t capacity) {
        std::erase_if(entries, [&](const auto &item) {
            if (item.finishedVisible && now >= *item.finishedVisible + item.holdMs() + 180) {
                retired_.insert(item.progress.id);
                return true;
            }
            return false;
        });
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto &item = entries[i];
            const bool visible = i < capacity;
            if (visible && !item.visible)
                item.entered = now;
            if (visible && item.terminal() && !item.finishedVisible)
                item.finishedVisible = std::max(now, item.entered + 160);
            // If the display shrinks, retain the full reading period when shown again.
            if (!visible)
                item.finishedVisible.reset();
            item.visible = visible;
        }
    }
    std::vector<ToastEntry> entries;

  private:
    std::set<app::SaveId> retired_;
};
struct ToastPlacement {
    int x{}, y{};
};
inline ToastPlacement toastPlacement(ToastCorner corner, int left, int top, int right, int bottom,
                                     int width, int height, int gap, int margin, int index) {
    const bool onRight = corner == ToastCorner::topRight || corner == ToastCorner::bottomRight;
    const bool onBottom = corner == ToastCorner::bottomLeft || corner == ToastCorner::bottomRight;
    return {onRight ? right - margin - width : left + margin,
            onBottom ? bottom - margin - height - index * (height + gap)
                     : top + margin + index * (height + gap)};
}
} // namespace nexplay::ui
