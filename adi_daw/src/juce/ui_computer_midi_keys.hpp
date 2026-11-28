// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "juce/ui_input_clock.hpp"
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#endif

namespace adi::ui {
#if JUCE_WINDOWS
// Standard PC scan codes for the physical A–Z positions on a US keyboard.
inline constexpr std::array<UINT, 26> computerMidiScans{
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
};
#endif

inline int computerMidiKeyCode(const juce::KeyPress &key) noexcept {
#if JUCE_WINDOWS
    const auto character = key.getTextCharacter();
    if (character > 0 && character <= 0xffff) {
        const auto layout = GetKeyboardLayout(0);
        const auto translated = VkKeyScanExW(static_cast<wchar_t>(character), layout);
        if (translated != -1) {
            const auto scan = MapVirtualKeyExW(static_cast<UINT>(translated) & 0xffu, MAPVK_VK_TO_VSC, layout);
            const auto physical = std::find(computerMidiScans.begin(), computerMidiScans.end(), scan);
            return physical == computerMidiScans.end() ? 0 : 'a' + static_cast<int>(physical - computerMidiScans.begin());
        }
    }
#endif
    return key.getKeyCode();
}

inline bool computerMidiKeyDown(const InputClock &input, int letter) {
#if JUCE_WINDOWS
    if (!input.keyDown && letter >= 'a' && letter <= 'z') {
        const auto scan = computerMidiScans[static_cast<std::size_t>(letter - 'a')];
        const auto virtualKey = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
        return (GetAsyncKeyState(static_cast<int>(virtualKey)) & 0x8000) != 0;
    }
#endif
    return input.down(letter);
}
} // namespace adi::ui
