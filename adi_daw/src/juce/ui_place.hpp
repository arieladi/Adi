// SPDX-License-Identifier: GPL-3.0-or-later
//
// WIN-02's placement rule (adi/ui/window_place.hpp) on JUCE's windows: the desktop's displays, a window
// put at its kept place, its place kept in the App record window.places, and its moves debounced into
// one save.
#pragma once
#include "adi/settings/store.hpp"
#include "adi/ui/window_place.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace adi::ui {
std::string displayId(const juce::Displays::Display &);
inline place::Rect toPlace(juce::Rectangle<int> r) noexcept { return {r.getX(), r.getY(), r.getWidth(), r.getHeight()}; }
inline juce::Rectangle<int> fromPlace(const place::Rect &r) noexcept { return {r.x, r.y, r.w, r.h}; }
// The desktop's displays as the rule reads them.
inline std::vector<place::Monitor> desktopMonitors() {
    std::vector<place::Monitor> out;
    for (const auto &d : juce::Desktop::getInstance().getDisplays().displays)
        out.push_back({displayId(d), toPlace(d.totalArea), toPlace(d.userArea), static_cast<int>(d.dpi), d.isMain});
    return out;
}
// Where a window stands for its record: its bounds out of full screen, and whether it is in it.
inline place::Rect standing(juce::ResizableWindow &w, bool &fullScreen) {
    fullScreen = w.isFullScreen();
    if (!fullScreen)
        return toPlace(w.getBounds());
    // Out of full screen, the bounds it goes back to (JUCE keeps them in its state string).
    juce::StringArray t;
    t.addTokens(w.getWindowStateAsString(), false);
    t.removeEmptyStrings();
    const int at = t[0].startsWithIgnoreCase("fs") ? 1 : 0;
    if (t.size() < at + 4)
        return toPlace(w.getBounds());
    return {t[at].getIntValue(), t[at + 1].getIntValue(), t[at + 2].getIntValue(), t[at + 3].getIntValue()};
}
// The window at its kept place, window.places[name] by the one rule, full screen again when it was;
// with none, or one the rule cannot read, `width` by `height` centred on the main display.
inline void restorePlace(juce::ResizableWindow &w, const settings::AppSettings &s, const std::string &name, int width,
                         int height, const std::vector<place::Monitor> &monitors) {
    const auto *limits = w.getConstrainer();
    const int minW = limits != nullptr ? limits->getMinimumWidth() : 1;
    const int minH = limits != nullptr ? limits->getMinimumHeight() : 1;
    const auto places = s.get("window.places");
    std::optional<place::Place> kept;
    if (places.is_object())
        if (const auto it = places.find(name); it != places.end())
            kept = place::fromJson(*it);
    if (!kept) {
        const int main = place::mainOf(monitors);
        if (main < 0) {
            w.centreWithSize(width, height);
            return;
        }
        const auto &usable = monitors[static_cast<std::size_t>(main)].usable;
        place::Rect r{0, 0, std::min(width, usable.w), std::min(height, usable.h)};
        r.x = usable.x + (usable.w - r.w) / 2;
        r.y = usable.y + (usable.h - r.h) / 2;
        w.setBounds(fromPlace(r));
        return;
    }
    w.setBounds(fromPlace(place::restore(*kept, monitors, minW, minH)));
    if (kept->fullScreen && !w.isFullScreen())
        w.setFullScreen(true);
}
inline void restorePlace(juce::ResizableWindow &w, const settings::AppSettings &s, const std::string &name, int width,
                         int height) {
    restorePlace(w, s, name, width, height, desktopMonitors());
}
// window.places[name] brought up to date with where the window stands; written only when it moved.
inline bool keepPlace(juce::ResizableWindow &w, settings::AppSettings &s, const std::string &name, std::string &why,
                      const std::vector<place::Monitor> &monitors) {
    bool full = false;
    const auto r = standing(w, full);
    auto places = s.get("window.places");
    if (!places.is_object())
        places = settings::Value::object();
    const auto entry = place::toJson(place::placeOf(r, full, monitors));
    if (places.contains(name) && places[name] == entry)
        return true;
    places[name] = entry;
    return s.set("window.places", places, {}, why) && s.save(why);
}
inline bool keepPlace(juce::ResizableWindow &w, settings::AppSettings &s, const std::string &name, std::string &why) {
    return keepPlace(w, s, name, why, desktopMonitors());
}
// After the displays change: a window whose title bar no screen holds now is moved the least that
// gives it one again, on the monitor showing most of it (else the main display). True when it moved.
inline bool keepReachable(juce::Component &w, const std::vector<place::Monitor> &monitors) {
    const auto r = toPlace(w.getBounds());
    if (monitors.empty() || place::reachable(r, monitors))
        return false;
    const auto &m = monitors[static_cast<std::size_t>(place::monitorOf(r, monitors))];
    auto moved = r;
    moved.w = std::min(moved.w, m.usable.w);
    moved.h = std::min(moved.h, m.usable.h);
    w.setBounds(fromPlace(place::keptReachable(moved, m.usable)));
    return true;
}
inline bool keepReachable(juce::Component &w) { return keepReachable(w, desktopMonitors()); }
// A window's moves and sizes, debounced into one save once it has stood still (WIN-02: places are
// saved on move, so a window keeps its place after a kill).
class PlaceWatch final : private juce::Timer {
  public:
    std::function<void()> save;
    void moved() { startTimer(kDebounceMs); }
    bool pending() const noexcept { return isTimerRunning(); }
    // Saves now what is pending (at close, and for tests).
    void flush() {
        if (!isTimerRunning())
            return;
        stopTimer();
        if (save)
            save();
    }
    static constexpr int kDebounceMs = 400;

  private:
    void timerCallback() override { flush(); }
};
} // namespace adi::ui
