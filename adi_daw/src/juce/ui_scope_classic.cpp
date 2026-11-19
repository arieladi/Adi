// SPDX-License-Identifier: GPL-3.0-only
#include "ui_scope_classic.hpp"
#include "ScopeClassicImages.h"
#include "smexoscope/widgets/CustomKnob.h"
#include "smexoscope/widgets/CustomSlider.h"
#include "smexoscope/widgets/MultiStateButton.h"
#include "smexoscope/widgets/TextElement.h"
#include "smexoscope/widgets/WaveDisplay.h"
#include <array>
#include <optional>
#include <cmath>
#include <utility>

namespace adi::ui {
dsp::TriggeredScope::Settings ClassicScopeControls::settings() const noexcept {
    dsp::TriggeredScope::Settings s;
    s.columns = OSC_WIDTH;
    s.trigger = trigger;
    s.removeDc = removeDc;
    dsp::TriggeredScope::mapNormalized({level, amplitude, time, holdoff, speed}, s);
    return s;
}

namespace {
juce::Image image(const char *data, int bytes) {
    return juce::ImageCache::getFromMemory(data, bytes);
}
// The original filmstrip component cycles on press. Publish the change once,
// without polling a host parameter on a second timer.
struct FilmButton final : MultiStateButton {
    using MultiStateButton::MultiStateButton;
    std::function<void()> onChange;
    void mouseDown(const juce::MouseEvent &e) override {
        if (!e.mods.isLeftButtonDown()) return;
        const auto was = getIndex();
        MultiStateButton::mouseDown(e);
        if (was != getIndex() && onChange) onChange();
    }
    void select(int i) {
        if (getIndex() != i) { setIndex(i); repaint(); }
    }
};
}

struct ClassicScopePanel::Face final : juce::Component {
    using Trigger = dsp::TriggeredScope::Trigger;
    using Column = dsp::TriggeredScope::Column;
    explicit Face(ClassicScopePanel &owner) : ownerPanel(owner) {
        setOpaque(true);
        addAndMakeVisible(wave);
        wave.setTitle("Waveform ruler");
        wave.setBounds(38, 16, OSC_WIDTH, OSC_HEIGHT);
        // Exact graph/control coordinates of the 825 x 300 faceplate.
        setup(time, "Time", {697, 31, 33, 33});
        setup(amp, "Amplitude", {762, 31, 33, 33});
        setup(speed, "Internal trigger speed", {700, 134, 33, 33});
        setup(hold, "Retrigger holdoff", {765, 134, 33, 33});
        setup(level, "Trigger level", {11, 13, 12, 275});
        setup(trigger, "Trigger mode", {718, 94, 60, 13});
        setup(sync, "Sync redraw", {696, 221, 41, 17});
        setup(freeze, "Freeze", {754, 221, 41, 17});
        setup(dc, "DC removal", {690, 259, 41, 17});
        setup(channel, "Left or right channel", {748, 259, 41, 17});
        setup(timeText, "Time readout", {696, 71, 34, 12});
        setup(ampText, "Amplitude readout", {760, 71, 34, 12});
        setup(speedText, "Trigger frequency", {698, 174, 34, 12});
        setup(holdText, "Holdoff in samples", {765, 174, 34, 12});
        trigger.setNumStates(4);
        for (juce::Slider *s : std::array<juce::Slider *, 5>{&time, &amp, &speed, &hold, &level})
            s->onValueChange = [this] { changed(); };
        for (auto *b : {&trigger, &sync, &dc, &channel}) b->onChange = [this] { changed(); };
        freeze.onChange = [this] { if (ownerPanel.onFreeze) ownerPanel.onFreeze(freeze.getIndex() != 0); };
    }

    void setup(juce::Component &c, const char *name, juce::Rectangle<int> r) {
        c.setTitle(name); addAndMakeVisible(c); c.setBounds(r);
    }
    void changed() {
        ClassicScopeControls v;
        v.time = static_cast<float>(time.getValue()); v.amplitude = static_cast<float>(amp.getValue());
        v.speed = static_cast<float>(speed.getValue()); v.holdoff = static_cast<float>(hold.getValue());
        v.level = static_cast<float>(level.getValue()); v.trigger = static_cast<Trigger>(trigger.getIndex());
        v.right = channel.getIndex() != 0; v.removeDc = dc.getIndex() != 0; v.sync = sync.getIndex() != 0;
        if (ownerPanel.onChange) ownerPanel.onChange(v);
    }
    void paint(juce::Graphics &g) override {
        g.fillAll(juce::Colours::white);
        g.drawImageAt(body, 0, 0);
    }
    void updateControls(const ClassicScopeControls &v, bool frozen, double rate) {
        freeze.select(frozen ? 1 : 0);
        if (last && *last == v && model.sampleRate == rate) return;
        last = v;
        model.parameters = {static_cast<float>(v.trigger) / 4.f, v.level, v.sync ? 1.f : 0.f, v.time, v.amplitude};
        model.sampleRate = rate;
        wave.repaint();
        time.setValue(v.time, juce::dontSendNotification); amp.setValue(v.amplitude, juce::dontSendNotification);
        speed.setValue(v.speed, juce::dontSendNotification); hold.setValue(v.holdoff, juce::dontSendNotification);
        level.setValue(v.level, juce::dontSendNotification);
        trigger.select(static_cast<int>(v.trigger)); channel.select(v.right ? 1 : 0);
        dc.select(v.removeDc ? 1 : 0); sync.select(v.sync ? 1 : 0);
        const auto s = v.settings();
        timeText.setValue(static_cast<float>(s.columnsPerSample)); ampText.setValue(s.gain);
        speedText.setValue(static_cast<float>(s.internalCyclesPerSample * rate));
        holdText.setValue(std::pow(10.f, v.holdoff * 4.f));
    }
    void updateTrace(const std::shared_ptr<const SharedAnalysis::CaptureResult> &next, std::int64_t fence) {
        if (picture == next && firstAllowed == fence && begun) return;
        begun = true; picture = next; firstAllowed = fence;
        const bool synchronous = model.getParameter(Smexoscope::kSyncDraw) > .5f;
        bool changed = false;
        for (size_t x = 0; x < OSC_WIDTH; ++x) {
            const Column *prev = nullptr, *now = nullptr;
            if (next && x < next->previous.size() && next->previous[x].firstSequence >= fence) prev = &next->previous[x];
            if (next && x < next->current.size()) {
                if (next->current[x].firstSequence >= fence) now = &next->current[x];
            } else now = prev;
            const bool previousChanged = put(x, prev, model.previous, model.previousValid);
            const bool currentChanged = put(x, now, model.current, model.currentValid);
            changed |= synchronous ? previousChanged : currentChanged;
        }
        // Parked Free/Internal acquisition still advances sweep identity and
        // sample ranges. Only a change in the painted projection needs a frame.
        if (changed) wave.repaint();
    }
    static bool put(size_t x, const Column *value, Smexoscope::Trace &trace, std::array<bool, OSC_WIDTH> &valid) {
        if (!value) return std::exchange(valid[x], false);
        const auto coordinate = [](float y) { return static_cast<int>(OSC_CENTER - y * OSC_CENTER); };
        const auto first = coordinate(value->first), second = coordinate(value->second);
        if (valid[x] && trace[x * 2].y == first && trace[x * 2 + 1].y == second) return false;
        valid[x] = true;
        trace[x * 2] = {static_cast<int>(x), first};
        trace[x * 2 + 1] = {static_cast<int>(x), second};
        return true;
    }
    ClassicScopePanel &ownerPanel;
    Smexoscope model;
    juce::Image body = image(ScopeClassicImages::body_png, ScopeClassicImages::body_pngSize);
    juce::Image knob = image(ScopeClassicImages::blue_knob1_4_png, ScopeClassicImages::blue_knob1_4_pngSize);
    juce::Image onOff = image(ScopeClassicImages::off_on_png, ScopeClassicImages::off_on_pngSize);
    WaveDisplay wave{model, image(ScopeClassicImages::heads_png, ScopeClassicImages::heads_pngSize),
                     image(ScopeClassicImages::readout_png, ScopeClassicImages::readout_pngSize)};
    CustomKnob time{knob, true, .75}, amp{knob, true, .5}, speed{knob, true, .5}, hold{knob, true, .5};
    CustomSlider level{image(ScopeClassicImages::slider_new_png, ScopeClassicImages::slider_new_pngSize)};
    FilmButton trigger{image(ScopeClassicImages::free_etc_png, ScopeClassicImages::free_etc_pngSize), true, 5};
    FilmButton channel{image(ScopeClassicImages::lefr_right_png, ScopeClassicImages::lefr_right_pngSize), true, 2};
    FilmButton sync{onOff, true, 2}, freeze{onOff, true, 2}, dc{onOff, true, 2};
    TextElement timeText, ampText, speedText, holdText;
    std::optional<ClassicScopeControls> last;
    std::shared_ptr<const SharedAnalysis::CaptureResult> picture;
    std::int64_t firstAllowed = 0;
    bool begun = false;
};
ClassicScopePanel::ClassicScopePanel() : face_(std::make_unique<Face>(*this)) {
    setTitle("Classic Scope");
    setInterceptsMouseClicks(false, true);
    addAndMakeVisible(*face_);
}
ClassicScopePanel::~ClassicScopePanel() = default;
void ClassicScopePanel::present(const ClassicScopeControls &v, bool frozen,
                                const std::shared_ptr<const SharedAnalysis::CaptureResult> &picture, std::int64_t fence) {
    const auto rate = picture ? picture->sampleRate : 0.;
    face_->updateControls(v, frozen, rate);
    face_->updateTrace(picture, fence);
}
void ClassicScopePanel::resized() {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    face_->setBounds(0, 0, BACK_WIDTH, BACK_HEIGHT);
    const auto scale = std::min(static_cast<float>(getWidth()) / BACK_WIDTH, static_cast<float>(getHeight()) / BACK_HEIGHT);
    const auto x = (static_cast<float>(getWidth()) - BACK_WIDTH * scale) * .5f;
    const auto y = (static_cast<float>(getHeight()) - BACK_HEIGHT * scale) * .5f;
    face_->setTransform(juce::AffineTransform::scale(scale).translated(x, y));
}
} // namespace adi::ui
