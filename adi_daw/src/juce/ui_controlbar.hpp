// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
// One item of Live's control bar: a field with text, or a glyph, flat as Live's.
class BarItem final : public juce::Button {
  public:
    enum class Glyph {
        None,
        Play,
        Stop,
        Record,
        Overdub,
        Follow,
        Metronome,
        NudgeDown,
        NudgeUp,
        AutomationArm,
        ReEnable,
        Capture,
        SessionRecord,
        PunchIn,
        Loop,
        PunchOut,
        Draw,
        Keyboard
    };
    BarItem(const juce::String &text, Glyph glyph = Glyph::None, bool boxed = true,
            bool dropdown = false);
    void paintButton(juce::Graphics &, bool over, bool down) override;
    void focusGained(FocusChangeType) override;
    void focusLost(FocusChangeType) override;
    void setText(const juce::String &t);
    // Live 12's focus ring: shown where the keyboard put the focus, never after a click.
    bool focusRing() const noexcept { return ring_; }
    std::function<void()> onFocus;
    juce::String text;
    Glyph glyph;
    bool boxed, dropdown;

  private:
    bool ring_ = false;
};
// Live's control bar (manual 3.1): tempo and the metronome on the left, the
// transport and the loop in the middle, the global switches on the right. What the
// engine has is live; the rest is drawn as Live draws it and says, when clicked, that
// it is not built yet.
class TransportBar final : public juce::Component {
  public:
    explicit TransportBar(AdiRootComponent &);
    void paint(juce::Graphics &) override;
    void resized() override;
    // Refreshes the live fields: the tempo, the position, the transport's state.
    void refresh();
    // Navigate's Control Bar (§41.4.1.1): the focus returns to the control that last had it.
    void focus();
    BarItem link{"Link"}, tap{"Tap"}, tempo{"120.00"},
        nudgeDown{{}, BarItem::Glyph::NudgeDown}, nudgeUp{{}, BarItem::Glyph::NudgeUp},
        signature{"4 / 4"}, metronome{{}, BarItem::Glyph::Metronome, true, true},
        quantize{"1 Bar", BarItem::Glyph::None, true, true},
        follow{{}, BarItem::Glyph::Follow}, position{"1.  1.  1"},
        play{{}, BarItem::Glyph::Play, false}, stop{{}, BarItem::Glyph::Stop, false},
        record{{}, BarItem::Glyph::Record, false}, overdub{{}, BarItem::Glyph::Overdub},
        automationArm{{}, BarItem::Glyph::AutomationArm},
        reEnable{{}, BarItem::Glyph::ReEnable}, capture{{}, BarItem::Glyph::Capture},
        sessionRecord{{}, BarItem::Glyph::SessionRecord}, loopStart{"1.  1.  1"},
        punchIn{{}, BarItem::Glyph::PunchIn}, loop{{}, BarItem::Glyph::Loop},
        punchOut{{}, BarItem::Glyph::PunchOut}, loopLength{"4.  0.  0"},
        draw{{}, BarItem::Glyph::Draw}, keyboard{{}, BarItem::Glyph::Keyboard}, key{"Key"},
        midi{"MIDI"}, cpu{"0 %", BarItem::Glyph::None, true, true}, indicators{{}};

  private:
    std::vector<BarItem *> items();
    AdiRootComponent &root_;
    BarItem *lastFocused_ = &link;
    // What the fields last showed, so a frame that changes nothing formats nothing.
    double shownBpm_ = -1;
    std::int64_t shownSixteenths_ = -1;
};
} // namespace adi::ui
