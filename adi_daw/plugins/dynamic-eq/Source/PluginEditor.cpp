// SPDX-License-Identifier: AGPL-3.0-only
#include "PluginEditor.hpp"
using namespace adi::eq;
namespace {
Modifiers modifiers(const juce::ModifierKeys &m) {
    return {m.isCtrlDown() || m.isCommandDown(), m.isAltDown(), m.isShiftDown()};
}
// ADI graph scale: 10 Hz..30 kHz, +/-30 dB; no constants are taken from drone prose.
constexpr double low = 10, high = 30000, extent = 30;
} // namespace
AdiEqEditor::AdiEqEditor(AdiEqProcessor &processor) : AudioProcessorEditor(processor), p(processor) {
    setSize(900, 560);
    setResizable(true, true);
    setResizeLimits(600, 360, 1800, 1120);
    startTimerHz(30); // UI refresh only; no audio callbacks or device opening.
}
AdiEqEditor::~AdiEqEditor() { endDrag(); }
juce::Rectangle<float> AdiEqEditor::plot() const { return getLocalBounds().toFloat().reduced(38, 62); }
juce::Point<float> AdiEqEditor::point(Band b) const {
    const auto r = plot();
    return {r.getX() + r.getWidth() * static_cast<float>(std::log(b.frequency / low) / std::log(high / low)),
            r.getCentreY() - r.getHeight() * static_cast<float>(b.gain / (2 * extent))};
}
int AdiEqEditor::hit(juce::Point<float> pos) const {
    for (int i = 0; i < static_cast<int>(zlp::kBandNum); ++i)
        if (value(p, zlp::PFilterStatus::kID, i) != 0 && point(readBand(p, i)).getDistanceFrom(pos) < 12)
            return i;
    return -1;
}
void AdiEqEditor::resized() {}
void AdiEqEditor::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff151b25));
    g.setColour(juce::Colours::white);
    g.setFont(22);
    g.drawText("ADI Dynamic EQ", 24, 12, 420, 32, juce::Justification::centredLeft);
    const auto r = plot();
    g.setFont(12);
    for (double f : {20., 100., 1000., 10000.}) {
        Band b;
        b.frequency = f;
        const auto x = point(b).x;
        g.setColour(juce::Colour(0xff344052));
        g.drawVerticalLine(static_cast<int>(x), r.getY(), r.getBottom());
        g.setColour(juce::Colours::lightgrey);
        g.drawText(juce::String(f, 0), static_cast<int>(x) - 25, static_cast<int>(r.getBottom()) + 3, 50, 18,
                   juce::Justification::centred);
    }
    for (int db : {-24, -12, 0, 12, 24}) {
        Band b;
        b.gain = db;
        const auto y = point(b).y;
        g.setColour(juce::Colour(0xff344052));
        g.drawHorizontalLine(static_cast<int>(y), r.getX(), r.getRight());
        g.setColour(juce::Colours::lightgrey);
        g.drawText(juce::String(db), 2, static_cast<int>(y) - 8, 32, 16, juce::Justification::centredRight);
    }
    juce::Path curve;
    std::array<Response, zlp::kBandNum> curves;
    size_t count = 0;
    for (int i = 0; i < static_cast<int>(zlp::kBandNum); ++i)
        if (value(p, zlp::PFilterStatus::kID, i) == 2)
            curves[count++] = responseCurve(readBand(p, i), p.getAtomicSampleRate());
    // UI drawing grid, 256 log-frequency points. Never executed on the audio thread.
    for (int n = 0; n < 256; ++n) {
        const auto f = low * std::pow(high / low, static_cast<double>(n) / 255);
        if (f >= p.getAtomicSampleRate() * .5)
            break; // No digital response above Nyquist.
        double db = 0;
        for (size_t i = 0; i < count; ++i)
            db += 20 * std::log10(std::max(1e-12, curves[i].magnitude(f)));
        Band b;
        b.frequency = f;
        b.gain = std::clamp(db, -extent, extent);
        const auto xy = point(b);
        if (n == 0)
            curve.startNewSubPath(xy);
        else
            curve.lineTo(xy);
    }
    g.setColour(juce::Colour(0xff5ed9c3));
    g.strokePath(curve, juce::PathStrokeType(2));
    g.setFont(12);
    g.setColour(juce::Colours::lightgrey);
    g.drawText("Static minimum-phase response", getWidth() - 270, 18, 245, 22,
               juce::Justification::centredRight);
    for (int i = 0; i < static_cast<int>(zlp::kBandNum); ++i)
        if (value(p, zlp::PFilterStatus::kID, i) != 0) {
            const auto b = readBand(p, i);
            const auto xy = point(b);
            g.setColour(b.bypass ? juce::Colours::grey : juce::Colour(0xfff4bb67));
            g.fillEllipse(xy.x - 6, xy.y - 6, 12, 12);
            if (b.dynamic) {
                auto end = b;
                end.gain += b.range;
                g.drawLine(xy.x, xy.y, xy.x, point(end).y, 2);
            }
        }
    g.setColour(juce::Colours::lightgrey);
    g.setFont(13);
    auto info = juce::String(
        "Drag curve: add band   |   Wheel: Q   |   Double-click node: values   |   Right-click: menu");
    if (selected >= 0) {
        const auto b = readBand(p, selected);
        info = "Band " + juce::String(selected + 1) + "   " + juce::String(b.frequency, 1) + " Hz   " +
               juce::String(b.gain, 2) + " dB   Q " + juce::String(b.q, 3) + "   Range " +
               juce::String(b.range, 2) + " dB";
    }
    g.drawText(info, 24, getHeight() - 30, getWidth() - 48, 22, juce::Justification::centredLeft);
}
void AdiEqEditor::endDrag() {
    if (dragging && selected >= 0)
        endChanges(p, selected, changes);
    dragging = false;
}
void AdiEqEditor::apply(const Input &in, bool oneShot) {
    if (selected < 0)
        return;
    const auto result = gesture(oneShot ? readBand(p, selected) : start, in);
    axis = result.axis;
    if (result.action == Action::EditValues) {
        editValues();
        return;
    }
    if (result.action == Action::Menu) {
        menu();
        return;
    }
    writeBand(p, selected, result.band, &changes);
    if (oneShot)
        endChanges(p, selected, changes);
    repaint();
}
void AdiEqEditor::mouseDown(const juce::MouseEvent &e) {
    endDrag();
    selected = hit(e.position);
    origin = e.position;
    axis = Axis::None;
    creating = false;
    newBand = false;
    if (selected >= 0 && e.mods.isPopupMenu()) {
        apply({Kind::RightClick}, true);
        return;
    }
    if (selected < 0) {
        if (!plot().contains(e.position) || e.mods.isPopupMenu())
            return;
        Band at;
        at.frequency = low * std::pow(high / low, (e.position.x - plot().getX()) / plot().getWidth());
        if (at.frequency >= p.getAtomicSampleRate() * .5)
            return;
        for (int i = 0; i < static_cast<int>(zlp::kBandNum); ++i)
            if (value(p, zlp::PFilterStatus::kID, i) == 2)
                at.gain += 20 * std::log10(std::max(
                                    1e-12, response(readBand(p, i), p.getAtomicSampleRate(), at.frequency)));
        at.gain = std::clamp(at.gain, -extent, extent);
        if (std::abs(point(at).y - e.position.y) > 12)
            return;
        // Defer creation until movement, so clicking empty space doesn't add a band.
        creating = true;
        return;
    }
    start = readBand(p, selected);
}
void AdiEqEditor::mouseDrag(const juce::MouseEvent &e) {
    if (creating) {
        if (e.getDistanceFromDragStart() < 3)
            return; // ADI click/drag threshold in pixels.
        for (int i = 0; i < static_cast<int>(zlp::kBandNum); ++i)
            if (value(p, zlp::PFilterStatus::kID, i) == 0) {
                selected = i;
                break;
            }
        if (selected < 0)
            return;
        start = Band{};
        start.frequency = low * std::pow(high / low, (origin.x - plot().getX()) / plot().getWidth());
        creating = false;
        newBand = true;
    }
    if (selected < 0)
        return;
    dragging = true;
    const auto delta = e.position - origin;
    const auto mods = modifiers(e.mods);
    if (mods.alt && axis == Axis::None && !newBand)
        axis = std::abs(delta.x) > std::abs(delta.y) ? Axis::Frequency : Axis::Vertical;
    Input in{newBand ? Kind::Create : Kind::Drag,
             delta.x / plot().getWidth() * std::log2(high / low),
             -delta.y / plot().getHeight() * (mods.control || !hasGain(start.shape) ? 6 : 2 * extent),
             0,
             mods,
             axis};
    apply(in, false);
}
void AdiEqEditor::mouseUp(const juce::MouseEvent &e) {
    if (!dragging && selected >= 0 && !e.mods.isPopupMenu() && e.getNumberOfClicks() == 1)
        apply({Kind::Click, 0, 0, 0, modifiers(e.mods)}, true);
    endDrag();
    creating = false;
}
void AdiEqEditor::mouseDoubleClick(const juce::MouseEvent &e) {
    selected = hit(e.position);
    apply({Kind::DoubleClick}, true);
}
void AdiEqEditor::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    if (!dragging)
        selected = hit(e.position);
    if (selected < 0)
        return;
    // Wheel Q: quarter-octave per detent; gain/range: 1 dB. JUCE detent ~0.1.
    const auto mods = modifiers(e.mods);
    if (dragging)
        start = readBand(p, selected);
    apply({Kind::Wheel, 0, 0, w.deltaY * 10 * (mods.alt || mods.control ? 1 : .25), mods}, !dragging);
    if (dragging) {
        start = readBand(p, selected);
        origin = e.position;
        newBand = false;
    }
}
void AdiEqEditor::menu() {
    if (selected < 0)
        return;
    const int band = selected;
    juce::PopupMenu m;
    m.addItem(1, "Edit values");
    m.addItem(2, "Bypass", true, readBand(p, band).bypass);
    m.addItem(3, "Dynamic", true, readBand(p, band).dynamic);
    m.addItem(4, "Remove band");
    for (int i = 0; i < zlp::PFilterType::kChoices.size(); ++i)
        m.addItem(100 + i, zlp::PFilterType::kChoices[i]);
    juce::Component::SafePointer<AdiEqEditor> self(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [self, band](int id) {
        if (!self || id == 0)
            return;
        self->selected = band;
        if (id == 1) {
            self->editValues();
            return;
        }
        auto b = readBand(self->p, band);
        if (id == 2)
            b.bypass = !b.bypass;
        if (id == 3)
            b.dynamic = !b.dynamic;
        if (id >= 100)
            b.shape = static_cast<Shape>(id - 100);
        Changes changed{};
        writeBand(self->p, band, b, &changed);
        endChanges(self->p, band, changed);
        if (id == 4) {
            auto *param = self->p.parameters_.getParameter("filter_status" + juce::String(band));
            param->beginChangeGesture();
            param->setValueNotifyingHost(0);
            param->endChangeGesture();
        }
    });
}
void AdiEqEditor::editValues() {
    if (selected < 0)
        return;
    // Selected band first; the second tab retains every advanced ZL parameter.
    class ExactValues final : public juce::Component {
      public:
        ExactValues(AdiEqProcessor &processor, int band) : target(processor), index(band) {
            const auto b = readBand(target, index);
            const std::array<double, 4> numbers{b.frequency, b.gain, b.q, b.range};
            const std::array<const char *, 4> names{"Frequency (Hz)", "Gain (dB)", "Q", "Dynamic range (dB)"};
            for (size_t i = 0; i < fields.size(); ++i) {
                labels[i].setText(names[i], juce::dontSendNotification);
                addAndMakeVisible(labels[i]);
                fields[i].setText(juce::String(numbers[i], 3));
                fields[i].setInputRestrictions(0, "0123456789.-+eE");
                fields[i].setExplicitFocusOrder(static_cast<int>(i) + 1);
                addAndMakeVisible(fields[i]);
                fields[i].onReturnKey = [this] { commit(); };
            }
            apply.setButtonText("Apply");
            apply.onClick = [this] { commit(); };
            addAndMakeVisible(apply);
            addAndMakeVisible(status);
        }
        void resized() override {
            for (size_t i = 0; i < fields.size(); ++i) {
                const int y = 24 + static_cast<int>(i) * 48;
                labels[i].setBounds(20, y, 180, 30);
                fields[i].setBounds(200, y, 180, 30);
            }
            apply.setBounds(200, 224, 100, 30);
            status.setBounds(20, 270, getWidth() - 40, 40);
        }

      private:
        AdiEqProcessor &target;
        int index;
        std::array<juce::TextEditor, 4> fields;
        std::array<juce::Label, 4> labels;
        juce::TextButton apply;
        juce::Label status;
        void commit() {
            std::array<double, 4> values{};
            for (size_t i = 0; i < fields.size(); ++i) {
                const auto text = fields[i].getText().trim().toStdString();
                char *end = nullptr;
                values[i] = std::strtod(text.c_str(), &end);
                if (end == text.c_str() || *end != '\0' || !std::isfinite(values[i])) {
                    status.setText("Enter a finite number in each field.", juce::dontSendNotification);
                    return;
                }
            }
            auto b = readBand(target, index);
            b.frequency = values[0];
            b.gain = values[1];
            b.q = values[2];
            b.range = values[3];
            Changes changed{};
            writeBand(target, index, b, &changed);
            endChanges(target, index, changed);
            const auto saved = readBand(target, index);
            const std::array<double, 4> actual{saved.frequency, saved.gain, saved.q, saved.range};
            for (size_t i = 0; i < fields.size(); ++i)
                fields[i].setText(juce::String(actual[i], 3));
            status.setText("Applied within the parameter limits.", juce::dontSendNotification);
        }
    };
    class Inspector final : public juce::DialogWindow {
      public:
        explicit Inspector(AdiEqProcessor &processor, int band)
            : DialogWindow("ADI Dynamic EQ — Band " + juce::String(band + 1), juce::Colour(0xff151b25),
                           true) {
            auto *tabs = new juce::TabbedComponent(juce::TabbedButtonBar::TabsAtTop);
            tabs->addTab("Band values", juce::Colour(0xff151b25), new ExactValues(processor, band), true);
            tabs->addTab("All parameters", juce::Colour(0xff151b25),
                         new juce::GenericAudioProcessorEditor(processor), true);
            tabs->setSize(600, 500);
            setUsingNativeTitleBar(true);
            setContentOwned(tabs, true);
            setResizable(true, false);
            centreWithSize(600, 500);
        }
        void closeButtonPressed() override { setVisible(false); }
    };
    inspector = std::make_unique<Inspector>(p, selected);
    inspector->setVisible(true);
}
