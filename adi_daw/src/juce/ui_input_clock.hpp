// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The window's input clock (ADR-0231 d2, WTS-02; DF-W18): where the window reads the time in
// milliseconds and whether a key is held, for the latching keys (Live 42.25), the ★ Reference's [REF]
// and Shift+R, Tap Tempo and the fade handles' F. The system's by default; a test gives it its own
// time and keys, so a check that times a tap or a hold never measures the machine it runs on.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <cctype>
#include <cstdint>
#include <functional>
namespace adi::ui {
struct InputClock {
    // Unset: the system's millisecond counter and key state.
    std::function<std::int64_t()> now;
    std::function<bool(int keyCode)> keyDown;
    std::int64_t ms() const { return now ? now() : static_cast<std::int64_t>(juce::Time::getMillisecondCounter()); }
    // A letter's key is down whichever case its code names.
    bool down(int code) const {
        const auto held = [this](int c) { return keyDown ? keyDown(c) : juce::KeyPress::isKeyCurrentlyDown(c); };
        return held(code) || (code > 0 && code < 128 && std::isalpha(code) &&
                              (held(std::toupper(code)) || held(std::tolower(code))));
    }
};
} // namespace adi::ui
