#pragma once
#include <cstdint>
namespace nexplay::app {
// Low bits retain the old MOD_ALT/CONTROL/SHIFT/WIN registry representation.
// High bits optionally constrain a modifier to its physical left/right keys.
constexpr std::uint32_t leftControl = 0x100, rightControl = 0x200;
constexpr std::uint32_t leftAlt = 0x400, rightAlt = 0x800;
constexpr std::uint32_t leftShift = 0x1000, rightShift = 0x2000;
constexpr std::uint32_t shortcutModifierMask = 0x3f0f;
constexpr std::uint32_t shortcutModifiers(bool lc, bool rc, bool la, bool ra,
                                           bool ls, bool rs, bool win = false) noexcept {
    return (lc || rc ? 2u : 0u) | (la || ra ? 1u : 0u) | (ls || rs ? 4u : 0u) |
        (win ? 8u : 0u) | (lc ? leftControl : 0u) | (rc ? rightControl : 0u) |
        (la ? leftAlt : 0u) | (ra ? rightAlt : 0u) | (ls ? leftShift : 0u) | (rs ? rightShift : 0u);
}
constexpr bool modifierSidesMatch(std::uint32_t required, std::uint32_t pressed,
                                   std::uint32_t sides) noexcept {
    return !(required & sides) || (required & sides) == (pressed & sides);
}
constexpr bool shortcutMatches(bool enabled, std::uint32_t key, std::uint32_t modifiers,
                               std::uint32_t pressedKey, std::uint32_t pressedModifiers) noexcept {
    return enabled && key == pressedKey && (modifiers & 15) == (pressedModifiers & 15) &&
        modifierSidesMatch(modifiers, pressedModifiers, leftControl | rightControl) &&
        modifierSidesMatch(modifiers, pressedModifiers, leftAlt | rightAlt) &&
        modifierSidesMatch(modifiers, pressedModifiers, leftShift | rightShift);
}
constexpr bool modifierSidesOverlap(std::uint32_t a, std::uint32_t b, std::uint32_t sides) noexcept {
    return !(a & sides) || !(b & sides) || (a & sides) == (b & sides);
}
constexpr bool shortcutsOverlap(std::uint32_t keyA, std::uint32_t a,
                                 std::uint32_t keyB, std::uint32_t b) noexcept {
    return keyA == keyB && (a & 15) == (b & 15) &&
        modifierSidesOverlap(a, b, leftControl | rightControl) &&
        modifierSidesOverlap(a, b, leftAlt | rightAlt) &&
        modifierSidesOverlap(a, b, leftShift | rightShift);
}
constexpr std::uint32_t physicalShortcutKey(std::uint32_t key, std::uint32_t scan,
                                           bool extended) noexcept {
    if (key == 0x10) return scan == 0x36 ? 0xa1 : 0xa0; // Shift
    if (key == 0x11) return extended ? 0xa3 : 0xa2;      // Ctrl
    if (key == 0x12) return extended ? 0xa5 : 0xa4;      // Alt
    return key;
}
} // namespace nexplay::app
