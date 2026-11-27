// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/transport.hpp"
#include "adi/ui/meters.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <string>
namespace adi::ui {
class AdiRootComponent;
// One item of Live's control bar: a field with text, or a glyph, flat as Live's.
class BarItem : public juce::Button {
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
    void mouseDown(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void clicked(const juce::ModifierKeys &) override;
    std::function<void(const juce::ModifierKeys &)> onModifiedClick;
    void focusGained(FocusChangeType) override;
    void focusLost(FocusChangeType) override;
    void setText(const juce::String &t);
    void setCountInPosition(engine::CountInPosition) noexcept;
    // Live 12's focus ring: shown where the keyboard put the focus, never after a click.
    bool focusRing() const noexcept { return ring_; }
    std::function<void()> onFocus;
    std::function<void()> onDropdown;
    juce::String text;
    Glyph glyph;
    bool boxed, dropdown;
    bool workspaceSegment = false;
    // Live's automation LED in a field's upper left corner (manual 6.5: the time signature fields
    // show one "when the Arrangement contains time signature changes").
    bool automationLed = false;
    bool overloadLed = false;

  protected:
    virtual const juce::String &displayText() const noexcept { return text; }

  private:
    bool ring_ = false;
    bool dropdownPress_ = false;
    engine::CountInPosition countIn_;
};
// A numeric field keeps its typed entry and lets the digits be adjusted independently.
class BarValue final : public BarItem {
  public:
    BarValue(const juce::String &text, char separator, int parts);
    bool keyPressed(const juce::KeyPress &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    void paintButton(juce::Graphics &, bool over, bool down) override;
    bool stepKey(const juce::KeyPress &);
    bool wheelAt(float x, const juce::MouseWheelDetails &);
    std::function<juce::String(int part, int steps, bool commit)> adjust;

  private:
    const juce::String &displayText() const noexcept override { return preview_.isEmpty() ? text : preview_; }
    float partBoundary(int part) const;
    int partAt(float x) const;
    juce::String preview_;
    char separator_;
    int parts_, part_ = 0, dragSteps_ = 0;
    bool pressed_ = false, dragged_ = false;
};
// Live's control bar (manual 3.1): tempo and the metronome on the left, the
// transport and the loop in the middle, the global switches on the right.
class TransportBar final : public juce::Component {
  public:
    explicit TransportBar(AdiRootComponent &);
    void paint(juce::Graphics &) override;
    void resized() override;
    // Refreshes the live fields: the tempo, the position, the transport's state.
    void refresh();
    juce::PopupMenu metronomeMenu() const;
    struct CpuLoad {
        float current, average;
        bool missedDeadline = false, deviceReset = false;
    };
    // The application supplies a sample at its message-thread timer rate.
    void setCpuLoad(std::optional<CpuLoad>);
    std::function<void(juce::PopupMenu)> onCpuMenu;
    // Navigate's Control Bar (§41.4.1.1): the focus returns to the control that last had it.
    void focus();
    // Live's Tempo and Time Signature fields (manual 9.1.1, 6.5): a click opens a field over one,
    // holding the value shown; Enter sets it at the play location (a value refused keeps the field
    // open, the Status Bar saying why), Esc or a click elsewhere closes it.
    void beginTempoEdit();
    void beginSignatureEdit();
    // Live's Arrangement Position fields (manual 6.1): typed, or up and down, a bar at a time.
    void beginPositionEdit();
    // Live's Loop Start and Loop Length fields (manual 6.6): typed, the start keeping the length, the
    // length keeping the start.
    void beginLoopStartEdit();
    void beginLoopLengthEdit();
    // The field being typed in: the tempo's or the signature's, or none.
    const BarItem *editing() const noexcept { return fieldEditor.isVisible() ? editing_ : nullptr; }
    class FieldEditor final : public juce::TextEditor {
      public:
        void bind(BarValue &, const juce::String &);
        bool keyPressed(const juce::KeyPress &) override;
        void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

      private:
        void finishAdjustment();
        BarValue *field_ = nullptr;
        juce::String initial_;
    } fieldEditor;
    BarValue tempo{"120.00", '.', 2}, signature{"4 / 4", '/', 2}, position{"1.  1.  1", '.', 3},
        loopStart{"1.  1.  1", '.', 3}, loopLength{"4.  0.  0", '.', 3};
    BarItem link{"Link"}, tap{"Tap"},
        nudgeDown{{}, BarItem::Glyph::NudgeDown}, nudgeUp{{}, BarItem::Glyph::NudgeUp},
        metronome{{}, BarItem::Glyph::Metronome, true, true},
        quantize{juce::String::charToString(0x2014), BarItem::Glyph::None, true, true},
        follow{{}, BarItem::Glyph::Follow},
        play{{}, BarItem::Glyph::Play, false}, stop{{}, BarItem::Glyph::Stop, false},
        record{{}, BarItem::Glyph::Record, false}, overdub{{}, BarItem::Glyph::Overdub},
        automationArm{{}, BarItem::Glyph::AutomationArm},
        reEnable{{}, BarItem::Glyph::ReEnable}, capture{{}, BarItem::Glyph::Capture},
        sessionRecord{{}, BarItem::Glyph::SessionRecord},
        punchIn{{}, BarItem::Glyph::PunchIn}, loop{{}, BarItem::Glyph::Loop},
        punchOut{{}, BarItem::Glyph::PunchOut},
        draw{{}, BarItem::Glyph::Draw}, keyboard{{}, BarItem::Glyph::Keyboard}, key{"Key"},
        midi{"MIDI"}, cpu{{}, BarItem::Glyph::None, true, true}, indicators{"In"}, midiOutIndicator{"Out"},
        arrangementPage{"Arrangement"}, mix{"Mix View"};

  private:
    AdiRootComponent &root_;
    BarItem *lastFocused_ = nullptr;
    void beginFieldEdit(BarValue &, const juce::String &text, std::function<bool(const std::string &)> commit);
    enum class TimeField { Position, LoopStart, LoopLength };
    juce::String adjustTime(TimeField, int part, int steps, bool commit);
    BarItem *editing_ = nullptr;
    std::function<bool(const std::string &)> commit_;
    enum class CpuMode { Average, Current, Off };
    CpuMode cpuMode_ = CpuMode::Average;
    std::optional<CpuLoad> cpuLoad_;
    int shownCpuPercent_ = -1;
    void refreshCpuMeter();
    void setCpuOverload(bool);
    juce::PopupMenu cpuMenu();
    // What the fields last showed, so a frame that changes nothing formats nothing.
    double shownBpm_ = -1;
    BarPosition shownPosition_{0, 0, 0};
    std::uint32_t shownSignature_ = 0; // numerator << 16 | denominator
    BarPosition shownLoopStart_{0, 0, 0}, shownLoopLength_{-1, 0, 0};
};
} // namespace adi::ui
