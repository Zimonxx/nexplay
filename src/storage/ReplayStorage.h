#pragma once

#include <chrono>
#include <filesystem>

namespace nexplay::storage {

struct ReplayBufferSettings {
    std::chrono::seconds duration{10};

    static constexpr std::chrono::seconds minimumDuration{10};
    static constexpr std::chrono::minutes maximumDuration{20};
};

class ReplayStorage final {
public:
    ReplayStorage();

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    void ensureExists() const;

private:
    std::filesystem::path root_;
};

} // namespace nexplay::storage
