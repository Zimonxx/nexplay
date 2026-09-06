#pragma once
#include <cstdint>
namespace nexplay::app {
constexpr bool shortcutMatches(bool enabled, std::uint32_t key, std::uint32_t modifiers,
                               std::uint32_t pressedKey, std::uint32_t pressedModifiers) noexcept {
    return enabled && key == pressedKey && modifiers == pressedModifiers;
}
} // namespace nexplay::app
