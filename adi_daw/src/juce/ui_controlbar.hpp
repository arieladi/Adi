// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/meters.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <string>
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
    // Live's automation LED in a field's upper left corner (manual 6.5: the time signature fields
    // show one "when the Arrangement contains time signature changes").
    bool automationLed = false;

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
    // Live's Tempo and Time Signature fields (manual 9.1.1, 6.5): a click opens a field over one,
    // holding the value shown; Enter sets it at the play location (a value refused keeps the field
    // open, the Status Bar saying why), Esc or a click elsewhere closes it.
    void beginTempoEdit();
    void beginSignatureEdit();
    // The field being typed in: the tempo's or the signature's, or none.
    const BarItem *editing() const noexcept { return fieldEditor.isVisible() ? editing_ : nullptr; }
    juce::TextEditor fieldEditor;
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
        midi{"MIDI"}, cpu{"0 %", BarItem::Glyph::None, true, true}, indicators{{}},
        // ADI's: the Mix View's button (ADR-0209 d9), beside Live's switches; F4 too.
        mix{"Mix"};

  private:
    std::vector<BarItem *> items();
    AdiRootComponent &root_;
    BarItem *lastFocused_ = &link;
    void beginFieldEdit(BarItem &, const juce::String &text, std::function<bool(const std::string &)> commit);
    BarItem *editing_ = nullptr;
    std::function<bool(const std::string &)> commit_;
    // What the fields last showed, so a frame that changes nothing formats nothing.
    double shownBpm_ = -1;
    BarPosition shownPosition_{0, 0, 0};
    std::uint32_t shownSignature_ = 0; // numerator << 16 | denominator
};
} // namespace adi::ui
