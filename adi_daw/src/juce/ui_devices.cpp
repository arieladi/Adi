// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_devices.hpp"
#include "ui_shell.hpp"
#include "ui_browser.hpp"
#include "ui_value_entry.hpp"
#include "adi/ui/automation_override.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/media/media_ops.hpp"
#include "juce/pd_declarations.hpp"
#include "mix_eq_host.hpp"
#include "adi/mix/eq/Preview.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
namespace adi::ui {
namespace {
constexpr int kLoadIssueHeight = 48;
constexpr const char *kPluginUnavailable = "Plug-in unavailable";
constexpr const char *kPlaybackProblemDescription =
    "This device has a playback problem. See Playback Problems in the Status Bar.";
}
ParameterControl::ParameterControl(DeviceChainStrip &s, std::int64_t d, const panel::Record &r,
                                   double defaultNormalized)
    : device(d), parameter(r.id), strip(s) {
    // A click on the knob lands the focus here (the slider declines it and JUCE walks to the
    // parent), so Live's value keys reach the control that was clicked (manual 41.6).
    setWantsKeyboardFocus(true);
    addAndMakeVisible(name);
    addAndMakeVisible(value);
    name.setFont(look::font(12.5f));
    value.setFont(look::font(12.5f));
    name.setJustificationType(juce::Justification::centred);
    value.setJustificationType(juce::Justification::centred);
    name.setInterceptsMouseClicks(false, false);
    value.setInterceptsMouseClicks(false, false);
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setRange(0, 1, 0);
    slider.onDragStart = [this] { gesture(engine::ParamEventKind::Begin, slider.getValue()); };
    slider.onValueChange = [this] { gesture(engine::ParamEventKind::Value, slider.getValue()); };
    slider.onDragEnd = [this] { gesture(engine::ParamEventKind::End, slider.getValue()); };
    // The wheel only scrolls, as Live (lookfeel.wheelChangesParameters, ADR-0203 d11).
    slider.setScrollWheelEnabled(false);
    slider.addMouseListener(this, false); // its right-click opens the parameter's menu (showMenu)
    toggle.addMouseListener(this, false);
    menu.addMouseListener(this, true); // includes the ComboBox's label
    // Shift while dragging is Live's finer resolution (manual 41.6). JUCE's way is its
    // velocity mode under the swap key: slow movement under Shift moves the value by a
    // fraction of the plain drag's pixel, speeding up as the mouse does; the plain drag stays
    // absolute. JUCE's own swap key is Ctrl/Alt/Cmd; Live's is Shift.
    slider.setVelocityModeParameters(1.0, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
    toggle.onClick = [this] {
        gesture(engine::ParamEventKind::Begin, toggle.getToggleState() ? 0 : 1);
        gesture(engine::ParamEventKind::Value, toggle.getToggleState() ? 1 : 0);
        gesture(engine::ParamEventKind::End, toggle.getToggleState() ? 1 : 0);
    };
    menu.onChange = [this] {
        if (!record || record->shape != panel::Shape::Menu) return;
        const auto chosen = menu.getSelectedId();
        if (chosen <= 0 || chosen > menuSteps_) return;
        const auto previous = record->playing;
        const double v = static_cast<double>(chosen - 1) / std::max(1, menuSteps_ - 1);
        presentedMenuId_ = chosen; // consume before a gesture can synchronously present
        if (v == previous) return;
        const SafePointer<ParameterControl> safe(this);
        if (!gesture(engine::ParamEventKind::Begin, previous)) {
            if (safe) {
                presentedMenuId_ = 1 + static_cast<int>(std::round(previous * std::max(1, menuSteps_ - 1)));
                menu.setSelectedId(presentedMenuId_, juce::dontSendNotification);
            }
            return;
        }
        if (!safe) return;
        gesture(engine::ParamEventKind::Value, v);
        if (safe) gesture(engine::ParamEventKind::End, v);
    };
    addChildComponent(slider);
    addChildComponent(toggle);
    addChildComponent(menu);
    present(r, defaultNormalized);
}
ParameterControl::~ParameterControl() = default;
bool ParameterControl::gesture(engine::ParamEventKind kind, double v) {
    if (kind != engine::ParamEventKind::End && !isEnabled()) return false;
    const SafePointer<ParameterControl> alive(this);
    const bool accepted = parameterGesture ? parameterGesture(parameter, kind, v) : strip.edit(device, parameter, kind, v);
    if (alive && accepted && kind == engine::ParamEventKind::Begin)
        strip.root.followDeviceAutomation(device, parameter);
    return accepted;
}
bool ParameterControl::keyPressed(const juce::KeyPress &k) {
    // Live's Configure edits the panel. Outside Configure these are the knob's Return to Default.
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) {
        auto *owner = findParentComponentOfClass<DevicePanelView>();
        if (!owner || !owner->deleteConfigureParameter(parameter))
            returnToDefault();
        return true; // A refused reset must not bubble into deleting the device or its track.
    }
    if (!record || record->shape != panel::Shape::Continuous || k.getModifiers().isAnyModifierKeyDown())
        return false;
    if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey) {
        const auto step = record->stepCount > 1 ? 1.0 / static_cast<double>(record->stepCount - 1) : .01;
        setOnce(std::clamp(record->playing + (k == juce::KeyPress::upKey ? step : -step), 0.0, 1.0));
        return true;
    }
    const auto first = valueEntryStart(k);
    if (k != juce::KeyPress::returnKey && first == 0)
        return false;
    if (!valueEntry_)
        valueEntry_ = std::make_unique<ValueEntry>("Normalized parameter value (0 to 1)", [this](const juce::String &text) {
            const auto normalized = valueNumber(text);
            if (normalized && *normalized >= 0 && *normalized <= 1)
                setOnce(*normalized);
            else
                strip.root.say("Enter a parameter value from 0 to 1.");
        });
    valueEntry_->open(*this, value.getBounds().withHeight(std::max(18, value.getHeight())).constrainedWithin(getLocalBounds()),
                      juce::String(record->playing, 6), first);
    return true;
}
bool ParameterControl::returnToDefault() {
    return setOnce(defaultValue);
}
bool ParameterControl::setOnce(double normalized) {
    if (!record || record->missing)
        return false;
    const auto previous = record->playing;
    if (normalized == previous)
        return true;
    // One gesture is one undo step. A callback may rebuild the control.
    const SafePointer<ParameterControl> alive(this);
    if (!gesture(engine::ParamEventKind::Begin, previous) || !alive)
        return false;
    const bool applied = gesture(engine::ParamEventKind::Value, normalized);
    return alive && gesture(engine::ParamEventKind::End, normalized) && applied;
}
void ParameterControl::present(const panel::Record &r, double defaultNormalized) {
    record = &r;
    defaultValue = std::isfinite(defaultNormalized) ? std::clamp(defaultNormalized, 0.0, 1.0) : 0.0;
    setEnabled(!r.missing);
    const juce::String label = r.name + (r.missing ? " (missing)" : "");
    if (name.getText() != label) {
        name.setText(label, juce::dontSendNotification);
        setTitle(label);
        setTooltip(r.missing ? "This parameter is missing from the plug-in."
                             : "A parameter of the plug-in, in its own words.");
    }
    const juce::String text = r.text.empty() ? juce::String(r.playing, 3) : juce::String(r.text);
    if (value.getText() != text)
        value.setText(text, juce::dontSendNotification);
    slider.setVisible(r.shape == panel::Shape::Continuous);
    toggle.setVisible(r.shape == panel::Shape::Switch);
    menu.setVisible(r.shape == panel::Shape::Menu);
    if (r.shape == panel::Shape::Menu && (menuSteps_ != std::max(1, r.stepCount) || menuLabels_ != r.choiceLabels)) {
        menuSteps_ = std::max(1, r.stepCount); menuLabels_ = r.choiceLabels;
        menu.clear(juce::dontSendNotification);
        presentedMenuId_ = 0;
        for (std::int32_t i = 0; i < menuSteps_; ++i) {
            const auto at = static_cast<std::size_t>(i);
            menu.addItem(at < menuLabels_.size() && !menuLabels_[at].empty() ? juce::String(menuLabels_[at])
                                                                           : juce::String(i + 1), i + 1);
        }
    }
    if (!slider.isMouseButtonDown())
        slider.setValue(r.playing, juce::dontSendNotification);
    toggle.setToggleState(r.playing >= .5, juce::dontSendNotification);
    // ComboBox sends its choice asynchronously. A differing ID is a choice
    // not delivered yet; a frame must not replace it with the old playing value.
    if (r.shape == panel::Shape::Menu && menu.getSelectedId() == presentedMenuId_) {
        presentedMenuId_ = 1 + static_cast<int>(std::round(r.playing * std::max(1, r.stepCount - 1)));
        menu.setSelectedId(presentedMenuId_, juce::dontSendNotification);
    }
}
void ParameterControl::mouseDown(const juce::MouseEvent &e) {
    if (e.eventComponent == this) {
        if (auto *panel = dynamic_cast<DevicePanelView *>(getParentComponent())) {
            panel->parameterReorder_.reset();
            if (e.mods.isLeftButtonDown() && !e.mods.isPopupMenu() && name.getBounds().contains(e.getPosition())) {
                const auto safe = SafePointer<ParameterControl>(this);
                if (panel->beginParameterReorder(parameter) || !safe)
                    return;
            }
        }
    }
    if (e.mods.isPopupMenu()) {
        if (e.eventComponent == this || e.eventComponent == &slider || e.eventComponent == &toggle ||
            e.eventComponent == &menu || menu.isParentOf(e.eventComponent))
            showMenu();
    } else if (e.mods.isLeftButtonDown() && isEnabled() && record && !record->missing)
        strip.root.followDeviceAutomation(device, parameter);
}
void ParameterControl::mouseUp(const juce::MouseEvent &e) {
    if (e.eventComponent == this) {
        if (auto *panel = dynamic_cast<DevicePanelView *>(getParentComponent())) {
            const bool labelClick = name.getBounds().contains(e.getPosition()) &&
                                    !e.source.hasMovedSignificantlySincePressed();
            panel->endParameterReorder(parameter, e.getEventRelativeTo(panel).getPosition(),
                                       e.getDistanceFromDragStart(), labelClick);
        }
    }
}
void ParameterControl::mouseDoubleClick(const juce::MouseEvent &e) {
    if (e.eventComponent == this && name.getBounds().contains(e.getPosition()))
        if (const auto *panel = dynamic_cast<DevicePanelView *>(getParentComponent()); panel && panel->configureSession_)
            return;
    if (!e.mods.isPopupMenu() && record && record->shape == panel::Shape::Continuous)
        returnToDefault();
}
juce::PopupMenu ParameterControl::menuItems() {
    juce::PopupMenu m;
    const auto current = strip.root.current();
    if (!current)
        return m;
    // Live's Show Automation (manual 25.2; DF-W37): the track's lane shows this parameter's envelope.
    const auto target = strip.root.deviceAutomationTarget(device, parameter);
    if (target) {
        if (!record || !record->missing)
            m.addItem("Show Automation", [safe = SafePointer<ParameterControl>(this)] {
                if (safe)
                    safe->strip.root.showDeviceAutomation(safe->device, safe->parameter);
            });
        strip.root.addAutomationDeleteItem(m, target->second);
    }
    strip.root.addReenableAutomation(m, deviceLaneOf(*current, device, format, parameter));
    return m;
}
void ParameterControl::showMenu() {
    auto m = menuItems();
    if (m.getNumItems() > 0)
        m.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this));
}
void ParameterControl::resized() {
    if (getHeight() < 48) {
        const int half = getHeight() / 2;
        const int side = std::min(24, getHeight());
        if (slider.isVisible()) {
            slider.setBounds(0, (getHeight() - side) / 2, side, side);
            name.setBounds(side, 0, getWidth() - side, half);
            value.setBounds(side, half, getWidth() - side, getHeight() - half);
        } else {
            name.setBounds(0, 0, getWidth(), half);
            value.setBounds({});
            toggle.setBounds(getWidth() / 2 - 12, half, 24, getHeight() - half);
            menu.setBounds(2, half, getWidth() - 4, getHeight() - half);
        }
        return;
    }
    // Live's knob column: the name, the knob, the value under it.
    name.setBounds(0, 0, getWidth(), 14);
    slider.setBounds(getWidth() / 2 - 14, 13, 28, 28);
    toggle.setBounds(getWidth() / 2 - 14, 19, 28, 14);
    menu.setBounds(2, 19, getWidth() - 4, 14);
    value.setBounds(0, 39, getWidth(), 14);
}
void ParameterControl::paint(juce::Graphics &g) {
    if (!record)
        return;
    // The automation LED (Live 12 manual 25.4): lit red while the control follows its lane, and off
    // when its automation is overridden, so an unlit LED is what says "overridden" until the
    // side-by-side measurement (parity-7.4 row 11). Dim, not absent, so the control still reads as
    // an automated one; red and the dim grey are ADI's until measured.
    if (record->overridden || record->automated) {
        g.setColour(record->overridden ? look::textDim : juce::Colours::red);
        g.fillEllipse(static_cast<float>(getWidth() - 7), 4.f, 5.f, 5.f);
    }
    if (record->driven) {
        g.setColour(look::value);
        g.fillRect(2, getHeight() - 2, static_cast<int>((getWidth() - 4) * record->playing), 2);
    }
}
void ActivatorButton::paintButton(juce::Graphics &g, bool over, bool) {
    const auto r = getLocalBounds().toFloat().withSizeKeepingCentre(10, 10);
    g.setColour(getToggleState() ? look::accent : look::button);
    g.fillEllipse(r);
    g.setColour(over ? look::text : look::outline);
    g.drawEllipse(r.reduced(0.5f), 1.f);
}
void WrenchButton::paintButton(juce::Graphics &g, bool over, bool) {
    // A wrench: a handle across the box, an open jaw at its head.
    const auto r = getLocalBounds().toFloat().reduced(2.f);
    g.setColour(getToggleState() ? look::accent : over ? look::text : look::outline);
    juce::Path handle;
    handle.startNewSubPath(r.getX() + 1.f, r.getBottom() - 1.f);
    handle.lineTo(r.getCentreX() + 1.f, r.getCentreY() - 1.f);
    g.strokePath(handle, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const auto head = juce::Rectangle<float>(r.getRight() - 6.5f, r.getY(), 6.5f, 6.5f);
    g.drawEllipse(head.reduced(0.6f), 1.8f);
}
void HotSwapButton::paintButton(juce::Graphics &g, bool over, bool down) {
    const auto centre = getLocalBounds().toFloat().getCentre();
    const float x = centre.x, y = centre.y;
    juce::Path arrows;
    arrows.startNewSubPath(x - 4.f, y - 1.f);
    arrows.quadraticTo(x - 1.f, y - 6.f, x + 4.f, y - 2.f);
    arrows.startNewSubPath(x + 1.f, y - 2.f);
    arrows.lineTo(x + 4.f, y - 2.f);
    arrows.lineTo(x + 4.f, y - 5.f);
    arrows.startNewSubPath(x + 4.f, y + 1.f);
    arrows.quadraticTo(x + 1.f, y + 6.f, x - 4.f, y + 2.f);
    arrows.startNewSubPath(x - 1.f, y + 2.f);
    arrows.lineTo(x - 4.f, y + 2.f);
    arrows.lineTo(x - 4.f, y + 5.f);
    g.setColour(down ? look::accent : over ? look::text : look::outline);
    g.strokePath(arrows, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}
void RevealSampleButton::paintButton(juce::Graphics &g, bool over, bool down) {
    const auto bounds = getLocalBounds().toFloat();
    const auto state = controls::stateOf(false, over, down, isEnabled());
    controls::button(g, bounds, state);
    const auto folder = bounds.withSizeKeepingCentre(11.f, 8.f);
    juce::Path shape;
    shape.startNewSubPath(folder.getX(), folder.getBottom());
    shape.lineTo(folder.getX(), folder.getY());
    shape.lineTo(folder.getX() + 4.f, folder.getY());
    shape.lineTo(folder.getX() + 6.f, folder.getY() + 2.f);
    shape.lineTo(folder.getRight(), folder.getY() + 2.f);
    shape.lineTo(folder.getRight(), folder.getBottom());
    shape.closeSubPath();
    g.setColour(controls::labelColour(state));
    g.strokePath(shape, juce::PathStrokeType(1.f));
}
namespace {
std::shared_ptr<const engine::SampleSlotNode> sampleSlot(const engine::Snapshot *snapshot, std::int64_t device,
                                                         std::int32_t slot) {
    if (snapshot)
        for (const auto &node : snapshot->sampleSlots)
            if (node && node->deviceId == device && node->slot == slot)
                return node;
    return {};
}
} // namespace
SampleTile::SampleTile(DeviceChainStrip &s, std::int64_t d, std::vector<DeclaredSlot> slots)
    : device(d), slot(slots.front().id), strip_(s), slots_(std::move(slots)) {
    setWantsKeyboardFocus(true);
    setTooltip("Drop a sample file here, from the desktop or the browser.");
    clear.getProperties().set("live.small", true);
    clear.onClick = [this] {
        if (targetValid && !targetValid()) return;
        if (strip_.clearSample)
            strip_.clearSample(device, slot);
    };
    addChildComponent(clear);
    addChildComponent(hotSwapButton);
    hotSwapButton.setTitle("Hot-Swap Sample");
    hotSwapButton.setTooltip("Browse samples for this slot.");
    addChildComponent(reveal);
    reveal.setTitle("Show Sample in Folder");
    reveal.onClick = [this] {
        if (targetValid && !targetValid()) return;
        const auto current = strip_.root.current();
        const auto selected = sampleSlot(current.get(), device, slot);
        if (!selected || selected != presented_ || !selected->filePresent || !selected->mediaId)
            return;
        try {
            auto &store = strip_.root.editStore();
            std::string error;
            const auto row = media::mediaRow(store.db(), *selected->mediaId);
            const auto file = media::resolveMedia(media::projectFolder(store), row, error);
            if (file.empty()) {
                strip_.root.say(error.empty() ? "The sample file is unavailable." : error);
                return;
            }
            if (strip_.revealSample)
                strip_.revealSample(file);
            else
                juce::File(juce::String::fromUTF8(media::pathUtf8(file).c_str())).revealToUser();
        } catch (const std::exception &failure) {
            strip_.root.say(failure.what());
        }
    };
    addChildComponent(slotChooser);
    if (slots_.size() > 1) {
        slotChooser.setTitle("Sample Slot");
        slotChooser.setTooltip("Choose which sample slot to show and edit.");
        for (std::size_t i = 0; i < slots_.size(); ++i)
            slotChooser.addItem(slots_[i].name, static_cast<int>(i + 1));
        slotChooser.setSelectedId(1, juce::dontSendNotification);
        slotChooser.onChange = [this] {
            const auto index = slotChooser.getSelectedItemIndex();
            if (index >= 0 && static_cast<std::size_t>(index) < slots_.size())
                choose(slots_[static_cast<std::size_t>(index)]);
        };
        slotChooser.setVisible(true);
    }
    choose(slots_.front());
}
void SampleTile::choose(const DeclaredSlot &choice) {
    slot = choice.id;
    slotName = choice.name;
    setTitle(slotName + " sample");
    clear.setTooltip("Empties the " + slotName + " slot: one undo step.");
    refresh();
    repaint();
}
void SampleTile::refresh() {
    const auto next = sampleSlot(strip_.root.reader().get(), device, slot);
    const bool changed = next != presented_;
    if (changed) {
        presented_ = next;
        state_ = !next || next->empty ? State::Empty : next->missing ? State::Missing : State::Loaded;
        const bool loaded = state_ == State::Loaded;
        fileName_ = juce::String{};
        if (loaded) {
            fileName_ = juce::String::fromUTF8(next->fileName.c_str());
            if (fileName_.isEmpty()) fileName_ = next->filePresent ? "Sample" : "Embedded sample";
        }
        clear.setVisible(state_ != State::Empty);
        reveal.setVisible(loaded);
        const bool canReveal = loaded && next->filePresent && next->mediaId;
        reveal.setEnabled(canReveal);
        reveal.setTooltip(canReveal ? "Show the sample file in its folder." : "This sample is embedded in the device preset.");
        if (next && !next->problem.empty())
            setTooltip(juce::String::fromUTF8(next->problem.c_str()));
        else
            setTooltip(fileName_.isNotEmpty() ? fileName_ : "Drop a sample file here, from the desktop or the browser.");
        repaint();
    }
    auto *cache = strip_.root.arrangement.waveforms;
    const auto delivered = cache ? cache->delivered() : 0;
    if (!changed && waveformsSeen_ == cache && peaksSeen_ == delivered) return;
    waveformsSeen_ = cache;
    peaksSeen_ = delivered;
    auto peaks = cache && state_ == State::Loaded ? cache->peaks(*next) : nullptr;
    if (peaks_ != peaks) {
        peaks_ = std::move(peaks);
        repaint(waveformBounds());
    }
}
void SampleTile::resized() {
    slotChooser.setBounds(3, 2, getWidth() - 6, 17);
    hotSwapButton.setBounds(4, getHeight() - 18, 17, 15);
    reveal.setBounds(26, getHeight() - 18, 17, 15);
    clear.setBounds(getWidth() - 40, getHeight() - 17, 36, 13);
}
juce::Rectangle<int> SampleTile::fileNameBounds() const noexcept {
    return getLocalBounds().reduced(6, 0).withTrimmedTop(20).withHeight(14);
}
juce::Rectangle<int> SampleTile::waveformBounds() const noexcept {
    return getLocalBounds().reduced(6, 0).withTrimmedTop(35).withTrimmedBottom(20);
}
void SampleTile::mouseDown(const juce::MouseEvent &event) {
    if (event.mods.isLeftButtonDown())
        focusIfShowing(*this);
}
void SampleTile::paint(juce::Graphics &g) {
    // A drop well, as Live's sample tiles: the slot's name, then its state.
    const auto r = getLocalBounds().toFloat().reduced(1.f);
    g.setColour(look::display);
    g.fillRoundedRectangle(r, 3.f);
    g.setColour(state_ == State::Missing ? juce::Colour(0xffe5352b) : look::outline);
    const float dash[] = {4.f, 3.f};
    juce::Path edge;
    edge.addRoundedRectangle(r.reduced(0.5f), 3.f);
    if (dropPreview_) {
        g.setColour(look::dropTarget);
        g.strokePath(edge, juce::PathStrokeType(2.f));
    } else if (state_ == State::Empty) {
        juce::Path dashed;
        juce::PathStrokeType(1.f).createDashedStroke(dashed, edge, dash, 2);
        g.fillPath(dashed);
    } else {
        g.strokePath(edge, juce::PathStrokeType(1.f));
    }
    // Light text on the dark display, as a device's display draws it.
    g.setColour(look::textOnBand);
    g.setFont(look::font(11.5f, true));
    if (!slotChooser.isVisible())
        g.drawText(slotName, 6, 3, getWidth() - 12, 14, juce::Justification::centredLeft);
    g.setFont(look::font(11.f));
    if (state_ != State::Loaded) {
        const bool missing = state_ == State::Missing;
        g.setColour(missing ? juce::Colour(0xffe5352b) : juce::Colour(0xff8a8a8a));
        g.drawFittedText(missing ? "File missing" : "Drop a sample", 6, 18, getWidth() - 12,
                         std::max(14, getHeight() - 38), juce::Justification::centred, 2);
        return;
    }
    g.drawText(fileName_, fileNameBounds(), juce::Justification::centredLeft, true);
    const auto area = waveformBounds();
    if (!peaks_ || peaks_->channels == 0 || area.isEmpty()) return;
    const auto channels = std::min(peaks_->channels, 2u);
    const auto framesPerPixel = static_cast<double>(peaks_->frames) / area.getWidth();
    const float band = static_cast<float>(area.getHeight()) / static_cast<float>(channels);
    g.setColour(look::value);
    for (int x = 0; x < area.getWidth(); ++x) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const auto peak = peakSpan(*peaks_, x * framesPerPixel, (x + 1) * framesPerPixel, ch);
            if (!peak) continue;
            const float centre = static_cast<float>(area.getY()) + band * (static_cast<float>(ch) + .5f);
            const float top = centre - std::clamp(peak->maximum, -1.f, 1.f) * band * .5f;
            const float bottom = centre - std::clamp(peak->minimum, -1.f, 1.f) * band * .5f;
            g.fillRect(static_cast<float>(area.getX() + x), top, 1.f, std::max(1.f, bottom - top));
        }
    }
}
bool SampleTile::drop(const juce::String &path) {
    setDropPreview(false);
    if (targetValid && !targetValid()) return false;
    if (path.isEmpty())
        return false;
    if (!strip_.assignSample) {
        strip_.root.say("Samples load in the application.");
        return false;
    }
    return strip_.assignSample(device, slot, path.toStdString());
}
void SampleTile::setDropPreview(bool on) {
    if (dropPreview_ == on)
        return;
    dropPreview_ = on;
    repaint();
}
void SampleTile::fileDragEnter(const juce::StringArray &files, int, int) {
    setDropPreview(!files.isEmpty() && strip_.assignSample && (!targetValid || targetValid()));
}
void SampleTile::fileDragExit(const juce::StringArray &) { setDropPreview(false); }
void SampleTile::filesDropped(const juce::StringArray &files, int, int) {
    setDropPreview(false);
    if (!files.isEmpty())
        drop(files[0]);
}
bool SampleTile::isInterestedInDragSource(const SourceDetails &details) {
    return !BrowserPanel::droppedFiles(details.description).isEmpty();
}
void SampleTile::itemDragEnter(const SourceDetails &details) {
    fileDragEnter(BrowserPanel::droppedFiles(details.description), 0, 0);
}
void SampleTile::itemDragExit(const SourceDetails &) { setDropPreview(false); }
void SampleTile::itemDropped(const SourceDetails &details) {
    filesDropped(BrowserPanel::droppedFiles(details.description), 0, 0);
}
namespace {
using NodeAxis = device::EqNodeAxis;
bool nodeHasGain(mix::eq::Shape shape) {
    using S = mix::eq::Shape;
    return shape == S::Bell || shape == S::LowShelf || shape == S::HighShelf ||
           shape == S::TiltShelf || shape == S::FlatTilt || shape == S::FlatGain;
}
constexpr float eqDbSpan = 36.f;
float eqFrequencyPosition(float hz) {
    return std::clamp(std::log(hz / 10.f) / std::log(3000.f), 0.f, 1.f);
}
void drawEqGrid(juce::Graphics &g, juce::Rectangle<float> graph) {
    struct Tick { float fraction; bool decade; };
    static const auto ticks = [] {
        std::array<Tick, 29> values{};
        auto out = values.begin();
        for (float decade : {10.f, 100.f, 1000.f}) {
            for (int tick = 1; tick <= 9; ++tick)
                *out++ = {eqFrequencyPosition(decade * static_cast<float>(tick)), tick == 1};
        }
        *out++ = {eqFrequencyPosition(10000.f), true};
        *out = {eqFrequencyPosition(20000.f), false};
        return values;
    }();
    g.setColour(look::display);
    g.fillRect(graph);
    for (const auto &tick : ticks) {
        g.setColour(look::gridLine.withMultipliedAlpha(tick.decade ? 1.f : .46f));
        const float x = graph.getX() + tick.fraction * graph.getWidth();
        g.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
    }
    g.setColour(look::outline);
    g.drawHorizontalLine(juce::roundToInt(graph.getCentreY()), graph.getX(), graph.getRight());
    if (graph.getHeight() >= static_cast<float>(2 * look::fieldHeight)) {
        g.setColour(look::gridLine);
        for (float offset : {-1.f / 3.f, 1.f / 3.f}) {
            const auto y = graph.getCentreY() + offset * graph.getHeight();
            g.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
        }
    }
}
juce::Point<float> nodePosition(const mix::eq::BandSettings &band, juce::Rectangle<float> graph) {
    const auto fraction = eqFrequencyPosition(band.frequencyHz);
    const auto gain = nodeHasGain(band.shape) ? band.gainDb : 0.f;
    return {graph.getX() + fraction * graph.getWidth(),
            std::clamp(graph.getCentreY() - gain * graph.getHeight() / eqDbSpan, graph.getY(), graph.getBottom())};
}
juce::Rectangle<float> nodeHandle(juce::Point<float> node, juce::Rectangle<float> graph, NodeAxis axis) {
    constexpr float w = 23.f, h = 13.f;
    const float x = node.x + ((node.x + w + 8.f <= graph.getRight()) ? 7.f : -w - 7.f);
    const float y = std::clamp(node.y - h, graph.getY(), graph.getBottom() - 2.f * h);
    return {x, y + (axis == NodeAxis::Gain ? h : 0.f), w, h};
}
}

struct DevicePanelView::EqPreview {
    std::shared_ptr<const std::vector<panel::Record>> records;
    std::optional<mix::eq::Settings> settings;
    double sampleRate = 0;
    std::array<juce::Path, 5> paths;
    std::weak_ptr<SharedAnalysis> spectrumBroker;
    SharedAnalysis::Lease spectrumLease;
    SharedAnalysis::Lease::SourcePosition freshSpectrum;
    std::shared_ptr<const SharedAnalysis::Result> spectrumResult;
    std::vector<float> spectrumPositions;
    double spectrumRate = 0;
    int spectrumFftSize = 0;
    int spectrumWidth = 0;
    juce::Path spectrumPath;
    int hovered = -1;
    struct Edit {
        std::size_t band;
        std::uint64_t instance;
        NodeAxis axis;
        juce::Point<float> start;
        juce::Rectangle<float> graph;
        juce::Point<float> origin, current; // Hz and dB, respectively
        std::optional<engine::ParameterPair> pair;
    };
    std::optional<Edit> edit;
    mix::eq::BandSettings displayedBand(std::size_t i) const {
        auto band = settings->bands[i];
        if (edit && edit->band == i) {
            if (edit->pair || edit->axis == NodeAxis::Frequency) band.frequencyHz = edit->current.x;
            if (edit->pair || edit->axis == NodeAxis::Gain) band.gainDb = edit->current.y;
        }
        return band;
    }
};
void DevicePanelView::updateEqPreview(const DevicePanelData &d) {
    if (!mixCard_ || !eqPresentation(d)) return;
    if (!eqPreview_) eqPreview_ = std::make_unique<EqPreview>();
    auto &preview = *eqPreview_;
    const auto rate = strip.root.editRate();
    if (d.cachedRecords && preview.records == d.cachedRecords && preview.sampleRate == rate) return;
    preview.records = d.cachedRecords;
    const auto settings = device::publishedMixEqSettings(d.parameters());
    if (settings == preview.settings && preview.sampleRate == rate) return;
    preview.sampleRate = rate;
    preview.settings = settings;
    for (auto &path : preview.paths) path.clear();
    if (settings) {
        const auto graph = mix::eq::designPreview(*settings, rate);
        for (std::size_t route = 0; route < graph.visible.size(); ++route) {
            if (!graph.visible[route]) continue;
            auto &path = preview.paths[route];
            const auto &db = graph.decibels[route];
            path.preallocateSpace(static_cast<int>(db.size() * 3));
            path.startNewSubPath(0.f, db.front());
            for (std::size_t i = 1; i < db.size(); ++i)
                path.lineTo(static_cast<float>(i) / static_cast<float>(db.size() - 1), db[i]);
        }
    }
    repaint();
}

void DevicePanelView::releaseEqSpectrum() {
    if (!eqPreview_) return;
    auto &preview = *eqPreview_;
    // The broker's release hook can change the owning document. Leave the
    // picture and lease state clear before calling it.
    auto lease = std::move(preview.spectrumLease);
    preview.freshSpectrum = {};
    preview.spectrumResult.reset();
    if (preview.spectrumPath.isEmpty()) return;
    preview.spectrumPath.clear();
    repaint(eqPreviewBounds().toNearestInt());
}

bool DevicePanelView::previewVisible(juce::Rectangle<int> visible) const {
    if (!isShowing()) return false;
    for (auto *p = getParentComponent(); p != nullptr && !visible.isEmpty(); p = p->getParentComponent())
        visible = visible.getIntersection(getLocalArea(p, p->getLocalBounds()));
    return !visible.isEmpty();
}

bool DevicePanelView::frameEqSpectrum(const std::shared_ptr<SharedAnalysis> &broker) {
    if (!eqPreview_) return false;
    if (!broker || !isShowing() || !data || !data->available || strip.folded(id)) {
        releaseEqSpectrum();
        return false;
    }
    const auto graph = eqPreviewBounds().toNearestInt();
    if (!previewVisible(graph)) {
        releaseEqSpectrum();
        return false;
    }
    const auto safe = SafePointer<DevicePanelView>(this);
    auto &preview = *eqPreview_;
    if (preview.spectrumBroker.lock() != broker) {
        releaseEqSpectrum();
        if (!safe) return false;
        preview.spectrumBroker = broker;
    }
    if (!preview.spectrumLease) {
        const auto track = data->track;
        const auto instance = presentedInstance();
        SharedAnalysis::Request request{track, engine::ScopePoint::DeviceOutput};
        request.device = id;
        request.channel = SharedAnalysis::Channel::StereoPower;
        SharedAnalysis::Lease acquired;
        try { acquired = broker->acquire(request); }
        catch (...) { return false; }
        if (!safe || !data || !data->available || data->track != track || presentedInstance() != instance ||
            preview.spectrumBroker.lock() != broker || !isShowing() || strip.folded(id) || eqPreviewBounds().isEmpty()) return false;
        if (!preview.spectrumLease) {
            preview.freshSpectrum = acquired.sourcePosition();
            preview.spectrumLease = std::move(acquired);
        }
    }
    auto next = preview.spectrumLease.read();
    // The broker fences an adopted tap before it publishes its first window.
    if (next && next->generation != preview.freshSpectrum.generation)
        preview.freshSpectrum = {next->generation, 0};
    if (next && next->firstSequence + next->request.fftSize <= preview.freshSpectrum.written)
        next.reset();
    // The graph stamps the output at the time it is heard. A result built
    // before a driver callback finishes waits for its last audible sample.
    if (next && next->advancing && next->firstStamp + next->request.fftSize > strip.root.renderedSamplePosition()) {
        next = preview.spectrumResult && preview.spectrumResult->sourceEpoch == next->sourceEpoch
            ? preview.spectrumResult : nullptr;
    }
    if (next == preview.spectrumResult && graph.getWidth() == preview.spectrumWidth)
        return static_cast<bool>(preview.spectrumLease);
    preview.spectrumPath.clear();
    if (next && std::max(next->peak[0], next->peak[1]) > 0.f) {
        if (preview.spectrumPositions.empty() || preview.spectrumRate != next->sampleRate ||
            preview.spectrumFftSize != next->request.fftSize) {
            preview.spectrumPositions.clear();
            preview.spectrumRate = next->sampleRate;
            preview.spectrumFftSize = next->request.fftSize;
            const auto step = static_cast<float>(next->sampleRate / next->request.fftSize);
            for (std::size_t bin = 1; bin < next->binsDbfs.size(); ++bin) {
                const auto hz = static_cast<float>(bin) * step;
                if (hz > 30000.f) break;
                preview.spectrumPositions.push_back(eqFrequencyPosition(hz));
            }
        }
        // Retain the loudest bin in each on-screen column. Use its actual Hz
        // to align it to the nodes, and stop at Nyquist rather than extending it.
        constexpr float floorDb = -96.f;
        const auto width = graph.getWidth();
        int column = -1;
        float x = 0.f, peak = floorDb;
        bool firstPoint = true;
        const auto append = [&] {
            const auto y = std::clamp(peak / floorDb, 0.f, 1.f);
            if (firstPoint) preview.spectrumPath.startNewSubPath(x, y);
            else preview.spectrumPath.lineTo(x, y);
            firstPoint = false;
        };
        preview.spectrumPath.preallocateSpace(3 * std::min(width, static_cast<int>(preview.spectrumPositions.size())));
        for (std::size_t i = 0; i < preview.spectrumPositions.size(); ++i) {
            const auto position = preview.spectrumPositions[i];
            const int pixel = static_cast<int>(position * static_cast<float>(width));
            const auto value = next->binsDbfs[i + 1];
            if (pixel != column) {
                if (column >= 0) append();
                column = pixel; x = position; peak = value;
            } else if (value > peak) {
                x = position; peak = value;
            }
        }
        if (column >= 0) append();
    }
    preview.spectrumWidth = graph.getWidth();
    preview.spectrumResult = std::move(next);
    repaint(graph);
    return static_cast<bool>(preview.spectrumLease);
}

struct DevicePanelView::LimiterPreview {
    static constexpr float gap = -1.f;
    static constexpr float displayRangeDb = 24.f;
    std::array<float, 64> peaks;
    mix::limiter::ReductionMailbox::Lease lease;
    std::optional<mix::limiter::ReductionFrame> previous;
    std::uint64_t instance = 0;
    juce::String readout{"-- dB"};

    LimiterPreview() { peaks.fill(gap); }

    bool present(const mix::limiter::ReductionFrame &next) {
        const auto oldPeak = peaks.back();
        const bool samePeak = previous && next.reduction.available == previous->reduction.available &&
            next.reduction.peakLoss == previous->reduction.peakLoss;
        const auto peak = samePeak ? oldPeak : next.reduction.available
            ? -20.f * std::log10(std::max(1.e-8f, 1.f - next.reduction.peakLoss)) : gap;
        const bool discontinuity = previous &&
            (next.transportRevision != previous->transportRevision || next.advancing != previous->advancing ||
             next.renderBlock < previous->renderBlock);
        if (discontinuity) peaks.fill(gap);
        const bool continuation = previous && !discontinuity;
        const bool sameBlock = continuation && previous->renderBlock == next.renderBlock && next.renderBlock != 0;
        const auto blockGap = continuation && next.advancing && previous->renderBlock != 0 &&
            next.renderBlock > previous->renderBlock ? next.renderBlock - previous->renderBlock : 1;
        previous = next;
        // Parked callbacks with steady reduction leave the same view. A split
        // callback overwrites its own column as more segments become audible.
        if ((sameBlock || (!next.advancing && !discontinuity)) && peaks.back() == peak) return false;
        if (!sameBlock) {
            const auto shift = static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(blockGap, peaks.size()));
            std::move(peaks.begin() + shift, peaks.end(), peaks.begin());
            std::fill(peaks.end() - shift, peaks.end(), gap);
        }
        peaks.back() = peak;
        if (peak != oldPeak)
            readout = peak == gap ? juce::String("-- dB") : juce::String(peak == 0 ? 0.f : -peak, 1) + " dB";
        return true;
    }
};

juce::Rectangle<int> DevicePanelView::limiterPreviewBounds() const {
    if (!mixCard_ || getHeight() < (look::mixStackNearRow + look::fieldHeight) || selectedPage_ != "limiter" ||
        configuring || strip.folded(id)) return {};
    const auto *function = nativeFunction();
    if (!function || function->kind != mix::Function::Limiter) return {};
    const int inset = look::gap;
    const int top = kTitleHeight + kPageHeight + 2 * inset;
    const int free = getHeight() - top - ParameterControl::kHeight - 2 * inset;
    const int reading = look::fieldHeight;
    if (free < 2 * reading || getWidth() < 2 * ParameterControl::kWidth) return {};
    const int height = std::min(free, juce::roundToInt(static_cast<float>(getHeight()) * .36f));
    const int left = inset + look::fieldHeight;
    return {left, top, std::max(0, getWidth() - left - inset), height};
}

void DevicePanelView::releaseLimiterReduction() {
    if (!limiterPreview_) return;
    limiterPreview_.reset();
    repaint(limiterPreviewBounds());
}

void DevicePanelView::frameLimiterReduction(
    const std::function<mix::limiter::ReductionMailbox::Lease(const DevicePanelData &)>* observer) {
    const auto area = limiterPreviewBounds();
    if (!observer || !*observer || !data || !data->available || !data->enabled ||
        !previewVisible(area)) {
        releaseLimiterReduction();
        return;
    }
    if (limiterPreview_ && limiterPreview_->instance != data->instance) releaseLimiterReduction();
    if (!limiterPreview_ || !limiterPreview_->lease) {
        const auto expected = data->runtime;
        const auto stamp = data->instance;
        const auto safe = SafePointer<DevicePanelView>(this);
        const auto acquire = *observer;
        auto acquired = acquire(*data);
        if (!safe || !data || !data->available || !data->enabled || data->instance != stamp ||
            !expected.sameInstance(data->runtime) || !previewVisible(limiterPreviewBounds())) return;
        if (!acquired) {
            releaseLimiterReduction();
            return;
        }
        if (!limiterPreview_ || !limiterPreview_->lease) {
            limiterPreview_ = std::make_unique<LimiterPreview>();
            limiterPreview_->instance = stamp;
            limiterPreview_->lease = std::move(acquired);
            repaint(area);
        }
    }
    const auto snapshot = limiterPreview_->lease.read();
    if (snapshot && limiterPreview_->present(*snapshot)) repaint(area);
}

void DevicePanelView::paintLimiterReduction(juce::Graphics &g, juce::Rectangle<int> area) const {
    using Preview = LimiterPreview;
    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(area);
    g.setColour(look::display);
    g.fillRoundedRectangle(area.toFloat(), look::radiusBox);
    auto inner = area.reduced(look::gap);
    const auto captions = inner.removeFromTop(look::fieldHeight);
    g.setFont(look::font(look::fontTiny));
    g.setColour(look::textDim);
    g.drawText("Reduction", captions, juce::Justification::centredLeft);
    const auto* preview = limiterPreview_ && data && data->available && data->enabled &&
        limiterPreview_->instance == data->instance && limiterPreview_->lease ? limiterPreview_.get() : nullptr;
    const auto latest = preview ? preview->peaks.back() : Preview::gap;
    g.setColour(latest == Preview::gap ? look::textDim : look::text);
    g.drawText(preview ? preview->readout : juce::String("-- dB"),
        captions, juce::Justification::centredRight);
    if (inner.isEmpty()) return;

    const auto plot = inner.toFloat();
    g.setColour(look::gridLine);
    g.drawHorizontalLine(inner.getY(), plot.getX(), plot.getRight());
    g.drawHorizontalLine(inner.getCentreY(), plot.getX(), plot.getRight());
    if (!preview) return;
    const auto &peaks = preview->peaks;
    const auto width = plot.getWidth() / static_cast<float>(peaks.size());
    g.setColour(look::meterPeak);
    for (std::size_t i = 0; i < peaks.size(); ++i) {
        if (peaks[i] <= 0) continue;
        const auto depth = std::min(peaks[i] / Preview::displayRangeDb, 1.f) * plot.getHeight();
        g.fillRect(plot.getX() + static_cast<float>(i) * width, plot.getY(), width, depth);
    }
}

juce::Rectangle<float> DevicePanelView::eqPreviewBounds() const {
    if (!eqPreview_ || !eqPreview_->settings || (!selectedPage_.empty() && selectedPage_ != "eq")) return {};
    auto area = getLocalBounds().reduced(look::border);
    area.removeFromLeft(look::fieldHeight);
    if (getHeight() >= look::mixStackNearRow + look::fieldHeight)
        area.setHeight(std::min(look::mixStackOuterRow, area.getHeight() / 2));
    return area.toFloat();
}

int DevicePanelView::eqBandAt(juce::Point<float> p) const {
    const auto graph = eqPreviewBounds();
    if (getHeight() < look::mixStackNearRow + look::fieldHeight || graph.isEmpty() || !graph.expanded(4.f).contains(p) || strip.folded(id)) return -1;
    int hit = -1;
    float distance = 7.f;
    const auto &bands = eqPreview_->settings->bands;
    for (std::size_t i = 0; i < bands.size(); ++i) {
        if (bands[i].status == mix::eq::Status::Off) continue;
        const auto d = nodePosition(bands[i], graph).getDistanceFrom(p);
        if (d < distance || (d == distance && static_cast<int>(i) == selectedBand_)) {
            distance = d;
            hit = static_cast<int>(i);
        }
    }
    if (hit < 0 && selectedBand_ >= 0 && static_cast<std::size_t>(selectedBand_) < bands.size()) {
        const auto &b = bands[static_cast<std::size_t>(selectedBand_)];
        const auto point = nodePosition(b, graph);
        if (b.status != mix::eq::Status::Off &&
            ((b.shape != mix::eq::Shape::FlatGain && nodeHandle(point, graph, NodeAxis::Frequency).contains(p)) ||
             (nodeHasGain(b.shape) && nodeHandle(point, graph, NodeAxis::Gain).contains(p)))) return selectedBand_;
    }
    return hit;
}
void DevicePanelView::drawEqNodes(juce::Graphics &g, juce::Rectangle<float> graph) const {
    if (getHeight() < look::mixStackNearRow + look::fieldHeight || strip.folded(id)) return;
    const auto &preview = *eqPreview_;
    const auto &bands = preview.settings->bands;
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const auto band = preview.displayedBand(i);
        if (band.status == mix::eq::Status::Off) continue;
        const auto p = nodePosition(band, graph);
        const auto route = static_cast<std::size_t>(band.route);
        const auto colour = route == 0 ? look::accent : look::trackColour(route);
        auto dot = juce::Rectangle<float>{8.f, 8.f}.withCentre(p);
        g.setColour(band.status == mix::eq::Status::Bypass ? look::panel : colour);
        g.fillEllipse(dot);
        g.setColour(static_cast<int>(i) == selectedBand_ ? look::text : colour);
        g.drawEllipse(dot, 1.f);
    }
    if (selectedBand_ >= 0 && static_cast<std::size_t>(selectedBand_) < bands.size()) {
        const auto &selected = bands[static_cast<std::size_t>(selectedBand_)];
        if (selected.status != mix::eq::Status::Off) {
            const auto node = nodePosition(preview.displayedBand(static_cast<std::size_t>(selectedBand_)), graph);
            for (const auto axis : {NodeAxis::Frequency, NodeAxis::Gain}) {
                if ((axis == NodeAxis::Gain && !nodeHasGain(selected.shape)) ||
                    (axis == NodeAxis::Frequency && selected.shape == mix::eq::Shape::FlatGain)) continue;
                const auto box = nodeHandle(node, graph, axis);
                g.setColour(look::button); g.fillRoundedRectangle(box.reduced(.4f), 2.f);
                g.setColour(look::text); g.setFont(look::font(10.f));
                g.drawText(axis == NodeAxis::Frequency ? "Hz" : "dB", box, juce::Justification::centred);
            }
        }
    }
    const int read = preview.edit ? static_cast<int>(preview.edit->band) : preview.hovered;
    if (read < 0 || static_cast<std::size_t>(read) >= bands.size() || bands[static_cast<std::size_t>(read)].status == mix::eq::Status::Off) return;
    const auto band = preview.displayedBand(static_cast<std::size_t>(read));
    const auto p = nodePosition(band, graph);
    juce::String label = juce::String(band.frequencyHz, band.frequencyHz < 100.f ? 2 : 1) + " Hz";
    if (nodeHasGain(band.shape)) label += "   " + juce::String(band.gainDb, 2) + " dB";
    label += "   Q " + juce::String(band.q, 2);
    const auto font = look::font(look::fontSmall);
    const auto gap = static_cast<float>(look::gap);
    const auto height = static_cast<float>(look::fieldHeight);
    const auto width = std::min(juce::GlyphArrangement::getStringWidth(font, label) + 2.f * gap,
                                graph.getWidth());
    const float left = std::clamp(p.x - width * .5f, graph.getX(), graph.getRight() - width);
    const float above = p.y - gap - height, below = p.y + gap;
    const float preferred = band.gainDb >= 0.f || !nodeHasGain(band.shape) ? above : below;
    const float alternative = preferred == above ? below : above;
    const float last = graph.getBottom() - height;
    const auto fits = [&](float y) { return y >= graph.getY() && y <= last; };
    const float top = fits(preferred) ? preferred :
                      fits(alternative) ? alternative : std::clamp(preferred, graph.getY(), last);
    const juce::Rectangle<float> rect{left, top, width, height};
    g.setColour(look::tooltip);
    g.fillRoundedRectangle(rect, look::radiusBox);
    g.setColour(look::text);
    g.setFont(font);
    g.drawText(label, rect, juce::Justification::centred);
}

bool DevicePanelView::pressEqGraph(juce::Point<float> point) {
    const auto graph = eqPreviewBounds();
    if (getHeight() < look::mixStackNearRow + look::fieldHeight || graph.isEmpty() || !graph.expanded(4.f).contains(point) ||
        strip.folded(id) || !validTarget()) return false;
    const auto safe = SafePointer<DevicePanelView>(this);
    const auto beginSingle = [&](std::size_t band, NodeAxis axis, juce::Point<float> origin) {
        const auto mapped = device::mixEqNodeValue(band, axis, axis == NodeAxis::Frequency ? origin.x : origin.y);
        if (!mapped || !editFunctionParameter(mapped->id, engine::ParamEventKind::Begin, mapped->value.normalized) ||
            !safe || !editingBegan_) return false;
        eqPreview_->edit.emplace(EqPreview::Edit{band, presentedInstance(), axis, point, graph, origin, origin, {}});
        repaint(graph.toNearestInt());
        return true;
    };
    if (selectedBand_ >= 0 && static_cast<std::size_t>(selectedBand_) < mix::eq::bandCount) {
        const auto bandIndex = static_cast<std::size_t>(selectedBand_);
        const auto band = eqPreview_->settings->bands[bandIndex];
        const auto node = nodePosition(band, graph);
        if (band.status != mix::eq::Status::Off) {
            if (band.shape != mix::eq::Shape::FlatGain && nodeHandle(node, graph, NodeAxis::Frequency).contains(point))
                return beginSingle(bandIndex, NodeAxis::Frequency, {band.frequencyHz, band.gainDb});
            if (nodeHasGain(band.shape) && nodeHandle(node, graph, NodeAxis::Gain).contains(point))
                return beginSingle(bandIndex, NodeAxis::Gain, {band.frequencyHz, band.gainDb});
        }
    }

    const auto hit = eqBandAt(point);
    if (hit < 0 || !chooseFunctionBand(hit) || !safe) return false;
    eqPreview_->hovered = hit;
    repaint(graph.toNearestInt());
    if (!parameterPairGesture) return true;

    const auto bandIndex = static_cast<std::size_t>(hit);
    const auto band = eqPreview_->settings->bands[bandIndex];
    const juce::Point<float> origin{band.frequencyHz, band.gainDb};
    if (band.shape == mix::eq::Shape::FlatGain) return beginSingle(bandIndex, NodeAxis::Gain, origin);
    if (!nodeHasGain(band.shape)) return beginSingle(bandIndex, NodeAxis::Frequency, origin);
    const auto frequency = device::mixEqNodeValue(bandIndex, NodeAxis::Frequency, origin.x);
    const auto gain = device::mixEqNodeValue(bandIndex, NodeAxis::Gain, origin.y);
    if (!frequency || !gain) return false;
    engine::ParameterPair pair{{{frequency->id, frequency->value.normalized}, {gain->id, gain->value.normalized}}};
    const auto instance = presentedInstance();
    if (!parameterPairGesture(pair, engine::ParameterPairEvent::Begin) || !safe) return false;
    eqPreview_->edit.emplace(EqPreview::Edit{bandIndex, instance, NodeAxis::Frequency, point, graph, origin, origin, std::move(pair)});
    return true;
}
bool DevicePanelView::dragEqHandle(juce::Point<float> point) {
    if (!eqPreview_ || !eqPreview_->edit) return false;
    const auto e = *eqPreview_->edit;
    if (presentedInstance() != e.instance || !validTarget() || getHeight() < look::mixStackNearRow + look::fieldHeight || eqPreviewBounds().isEmpty() ||
        strip.folded(id) || eqPreview_->settings->bands[e.band].status == mix::eq::Status::Off) {
        finishEqEdit(true);
        return false;
    }
    const float dx = (point.x - e.start.x) / e.graph.getWidth();
    const float dy = (point.y - e.start.y) / e.graph.getHeight();
    const auto frequency = std::clamp(e.origin.x * std::pow(3000.f, dx), 10.f, std::max(30000.f, e.origin.x));
    const auto gain = std::clamp(e.origin.y - dy * eqDbSpan, -30.f, 30.f);
    const auto safe = SafePointer<DevicePanelView>(this);
    bool accepted = false;
    auto pair = e.pair;
    if (pair) {
        const auto horizontal = device::mixEqNodeValue(e.band, NodeAxis::Frequency, frequency);
        const auto vertical = device::mixEqNodeValue(e.band, NodeAxis::Gain, gain);
        if (horizontal && vertical && parameterPairGesture) {
            (*pair)[0].second = horizontal->value.normalized;
            (*pair)[1].second = vertical->value.normalized;
            accepted = parameterPairGesture(*pair, engine::ParameterPairEvent::Value);
        }
    } else {
        const auto mapped = device::mixEqNodeValue(e.band, e.axis, e.axis == NodeAxis::Frequency ? frequency : gain);
        accepted = mapped && editFunctionParameter(mapped->id, engine::ParamEventKind::Value, mapped->value.normalized);
    }
    if (!safe || !eqPreview_->edit) return false;
    if (!accepted) {
        finishEqEdit(true);
        return false;
    }
    eqPreview_->edit->current = {frequency, gain};
    eqPreview_->edit->pair = std::move(pair);
    repaint(e.graph.toNearestInt());
    return true;
}
void DevicePanelView::releaseEqHandle() {
    finishEqEdit(false);
}
void DevicePanelView::finishEqEdit(bool cancel) {
    if (!eqPreview_ || !eqPreview_->edit) return;
    auto pair = std::move(eqPreview_->edit->pair);
    eqPreview_->edit.reset();
    repaint(eqPreviewBounds().toNearestInt());
    if (pair) {
        if (parameterPairGesture)
            parameterPairGesture(*pair, cancel ? engine::ParameterPairEvent::Cancel : engine::ParameterPairEvent::End);
    } else {
        endFunctionParameter();
    }
}

DevicePanelView::DevicePanelView(DeviceChainStrip &s, const DevicePanelData &d, bool mixCard)
    : id(d.id), mixCard_(mixCard), strip(s) {
    // The title is the bar's text, not a box (Live's device title).
    title.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    title.getProperties().set("live.left", true);
    configureButton.setColour(juce::TextButton::buttonColourId, look::button);
    configureButton.getProperties().set("live.small", true);
    setWantsKeyboardFocus(true);
    if (!mixCard_) {
        // Take focus after handling the title press. A Shift click needs the old anchor.
        setMouseClickGrabsKeyboardFocus(false);
        title.setMouseClickGrabsKeyboardFocus(false);
    }
    addAndMakeVisible(title);
    addAndMakeVisible(enabled);
    addAndMakeVisible(configureButton);
    addChildComponent(wrench);
    wrench.setTitle("Plug-In Window");
    wrench.setTooltip("Opens or closes the plug-in's own window; Ctrl/Cmd keeps the other plug-in windows open.");
    wrench.onClick = [this] {
        if (!validTarget()) return;
        strip.root.togglePluginWindow(id, juce::ModifierKeys::currentModifiers.isCommandDown());
        wrench.setToggleState(strip.root.pluginWindowOpen(id), juce::dontSendNotification);
    };
    addChildComponent(hotSwapButton);
    hotSwapButton.setTitle("Hot-Swap Device");
    hotSwapButton.setTooltip("Browse replacements for this device.");
    hotSwapButton.onClick = [this] {
        if (data) requestHotSwap(*data, std::nullopt, menuEpoch_);
    };
    addChildComponent(search);
    addChildComponent(functionBand);
    addChildComponent(functionSection);
    functionBand.setTitle("EQ band");
    functionBand.setTooltip("Selects the band whose controls are shown. Selection does not change processing.");
    functionSection.setTitle("EQ control section");
    functionSection.setTooltip("Shows a section of the selected band's controls, or the global controls.");
    functionBand.onChange = [this] {
        const auto at = functionBand.getSelectedId() - 1;
        if (at >= 0 && static_cast<std::size_t>(at) < functionBands_.size())
            chooseFunctionBand(functionBands_[static_cast<std::size_t>(at)]);
    };
    functionSection.onChange = [this] {
        const auto at = functionSection.getSelectedId() - 1;
        if (at >= 0 && static_cast<std::size_t>(at) < functionSections_.size())
            chooseFunctionSection(functionSections_[static_cast<std::size_t>(at)].first);
    };
    enabled.setTitle("Device Activator");
    enabled.setTooltip("Switches the device on or off.");
    configureButton.setTooltip("Choose which of the plug-in's parameters the panel shows.");
    search.setTextToShowWhenEmpty("Find parameter", juce::Colours::grey);
    search.setTitle("Find parameter");
    addChildComponent(parameterList);
    parameterList.getProperties().set("live.small", true);
    parameterList.setTooltip("Lists parameters declared by the plug-in, including those it does not report when touched.");
    parameterList.onClick = [this] { showParameterList(); };
    title.addMouseListener(this, false);
    title.onClick = [this] { focusIfShowing(*this); };
    enabled.onClick = [this] {
        const auto safe = SafePointer<DevicePanelView>(this);
        if (!validTarget()) return;
        if (nativeFunction()) cancelInteractions();
        if (!safe || !safe->validTarget() || !safe->data) return;
        safe->strip.root.followDeviceAutomation(safe->id, std::string(kDeviceActivatorParamRef));
        if (!safe || !safe->data) return;
        const bool requested = safe->nativeFunction() ? !safe->data->enabled : safe->enabled.getToggleState();
        if (!safe->strip.setEnabledExpected(*safe->data, requested) && safe && safe->data)
            safe->enabled.setToggleState(safe->data->enabledNow(), juce::dontSendNotification);
    };
    configureButton.onClick = [this] { configure(); };
    addChildComponent(rename);
    rename.setTitle("Device name");
    rename.setFont(look::font(12.f));
    rename.onCommit = [this] { endRename(true); };
    rename.onCancel = [this] { endRename(false); };
    rename.onTab = [this](bool) { endRename(true); };
    rename.onFocusLost = [this] { endRename(true); };
    present(d);
}
DevicePanelView::~DevicePanelView() {
    finishEqEdit(true);
    endFunctionParameter();
    endInteraction(Interaction::Text);
    endInteraction(Interaction::Menu);
    // Slider/menu teardown may finish native interaction after this destructor body. Do not
    // leave callbacks reaching the retained owner once its identity/gesture members are gone.
    for (auto &control : controls) {
        control->slider.onDragStart = {}; control->slider.onValueChange = {}; control->slider.onDragEnd = {};
        control->toggle.onClick = {}; control->menu.onChange = {};
    }
}
bool DevicePanelView::beginInteraction(Interaction kind) {
    auto &active = kind == Interaction::Text ? textInteraction_ : menuInteraction_;
    if (active) return true;
    if (!validTarget() || (interactionBegin && !interactionBegin(kind))) return false;
    active = true;
    if (kind == Interaction::Menu) ++menuEpoch_;
    return true;
}
void DevicePanelView::endInteraction(Interaction kind) {
    auto &active = kind == Interaction::Text ? textInteraction_ : menuInteraction_;
    if (!active) return;
    active = false;
    if (interactionEnd) interactionEnd(kind);
}
void DevicePanelView::cancelInteractions() {
    // A native menu may complete after this panel was hidden and shown again. Its old action
    // and completion must neither mutate this target nor end a newly acquired capture.
    ++menuEpoch_;
    dragTarget_.reset();
    draggedDevices_.clear();
    selectOnRelease_ = false;
    startedDeviceDrag_ = false;
    const auto safe = SafePointer<DevicePanelView>(this);
    if (configuring && data && data->plugin) closeConfigure();
    if (!safe) return;
    if (eqPreview_) eqPreview_->hovered = -1;
    finishEqEdit(true);
    if (!safe) return;
    endFunctionParameter();
    if (!safe) return;
    endRename(false);
    if (!safe) return;
    endInteraction(Interaction::Text);
    if (!safe) return;
    endInteraction(Interaction::Menu);
}
const mix::FunctionInfo *DevicePanelView::nativeFunction() const noexcept {
    return data && !data->plugin ? mix::functionForUid(data->uid) : nullptr;
}
std::uint64_t DevicePanelView::presentedInstance() const noexcept { return data ? data->instance : 0; }
bool DevicePanelView::beginFunctionDrag() {
    if (!nativeFunction() || !validTarget() || !data) return false;
    auto expected = *data;
    const auto safe = SafePointer<DevicePanelView>(this);
    endFunctionParameter();
    if (!safe || !safe->validTarget()) return false;
    dragTarget_ = std::move(expected);
    ++dragEpoch_;
    return true;
}
const DevicePanelData *DevicePanelView::functionDragTarget() const noexcept {
    return nativeFunction() && dragTarget_ ? &*dragTarget_ : nullptr;
}
bool DevicePanelView::beginDeviceDrag() {
    if (mixCard_ || nativeFunction() || !data || !validTarget()) return false;
    if (strip.selectionActsOn(id)) {
        draggedDevices_ = strip.selectedTargets();
        if (draggedDevices_.empty()) return false;
    } else {
        draggedDevices_ = {*data};
    }
    startedDeviceDrag_ = true;
    return true;
}
void DevicePanelView::endFunctionParameter() {
    if (!editingBegan_) return;
    const auto parameter = std::move(editingParameter_);
    const auto value = editingValue_;
    editingBegan_ = false;
    if (parameterGesture) parameterGesture(parameter, engine::ParamEventKind::End, value);
    else strip.edit(id, parameter, engine::ParamEventKind::End, value);
}
bool DevicePanelView::editFunctionParameter(const std::string &parameter, engine::ParamEventKind kind, double value) {
    if (kind == engine::ParamEventKind::End) {
        if (!editingBegan_ || editingParameter_ != parameter) return false;
        endFunctionParameter(); return true;
    }
    const auto safe = SafePointer<DevicePanelView>(this);
    if (!parameterPages.empty() && kind == engine::ParamEventKind::Value &&
        (!editingBegan_ || editingParameter_ != parameter)) return false;
    if (kind == engine::ParamEventKind::Begin) {
        endFunctionParameter();
        if (!safe) return false;
    }
    if (!validTarget()) { endFunctionParameter(); return false; }
    const bool accepted = parameterGesture ? parameterGesture(parameter, kind, value) : strip.edit(id, parameter, kind, value);
    if (!safe) return false;
    if (kind == engine::ParamEventKind::Begin && accepted) {
        editingBegan_ = true; editingParameter_ = parameter; editingValue_ = value;
    } else if (kind == engine::ParamEventKind::Value && editingBegan_ && editingParameter_ == parameter) {
        if (accepted) editingValue_ = value;
        else endFunctionParameter();
    }
    return accepted;
}
int DevicePanelView::pageSelectorWidth(const ParameterPageControl &page) noexcept {
    return std::clamp(14 + 6 * static_cast<int>(std::min<std::size_t>(page.name.size(), 16)), 36, 110);
}
bool DevicePanelView::eqPresentation(const DevicePanelData &value) noexcept {
    const auto *function = !value.plugin ? mix::functionForUid(value.uid) : nullptr;
    return function && function->kind == mix::Function::Eq;
}
void DevicePanelView::presentFunctionSelection(const DevicePanelData &next) {
    if (!eqPresentation(next)) return;
    // A folded or scrolled-out identity row carries no parameter payload.
    // Keep this runtime's presentation selection until demand resumes.
    if (next.groups().empty()) return;
    const bool pageChanged = functionSelectionPage_ != selectedPage_;
    if (!pageChanged && data && eqPresentation(*data) &&
        (&data->groups() == &next.groups() || data->groups() == next.groups())) return;
    if (pageChanged) {
        if (!functionSelectionPage_.empty())
            functionSelectionByPage_[functionSelectionPage_] = {selectedBand_, selectedSection_};
        const auto saved = functionSelectionByPage_.find(selectedPage_);
        selectedBand_ = saved == functionSelectionByPage_.end() ? 0 : saved->second.band;
        selectedSection_ = saved == functionSelectionByPage_.end() ? std::string() : saved->second.section;
        functionSelectionPage_ = selectedPage_;
    }
    std::vector<std::int32_t> bands;
    std::vector<std::pair<std::string, std::string>> sections;
    std::vector<std::string> bandSections;
    bool groupPresent = false;
    for (const auto &[parameter, metadata] : next.groups()) {
        (void)parameter;
        if (metadata.groupId != selectedPage_) continue;
        groupPresent = true;
        if (metadata.bandIndex >= 0) {
            if (std::find(bands.begin(), bands.end(), metadata.bandIndex) == bands.end()) bands.push_back(metadata.bandIndex);
            if (std::find(bandSections.begin(), bandSections.end(), metadata.sectionId) == bandSections.end())
                bandSections.push_back(metadata.sectionId);
        }
        if (!metadata.sectionId.empty() && std::none_of(sections.begin(), sections.end(), [&sectionId = metadata.sectionId](const auto &section) {
                return section.first == sectionId;
            })) sections.emplace_back(metadata.sectionId, metadata.sectionName.empty() ? metadata.sectionId : metadata.sectionName);
    }
    std::sort(bands.begin(), bands.end());
    const auto sectionOrder = [](const std::string &section) {
        return section == "basic" ? 0 : section == "dynamics" ? 1 : section == "detector" ? 2 :
               section == "spectral" ? 3 : section == "global" ? 4 : 5;
    };
    std::stable_sort(sections.begin(), sections.end(), [&](const auto &a, const auto &b) {
        const auto ao = sectionOrder(a.first), bo = sectionOrder(b.first);
        return ao != bo ? ao < bo : a.first < b.first;
    });
    if (groupPresent) {
        if (!bands.empty() && std::find(bands.begin(), bands.end(), selectedBand_) == bands.end()) selectedBand_ = bands.front();
        if (std::none_of(sections.begin(), sections.end(), [&](const auto &section) { return section.first == selectedSection_; }))
            selectedSection_ = sections.empty() ? std::string() : sections.front().first;
        functionSelectionByPage_[selectedPage_] = {selectedBand_, selectedSection_};
    }
    // A temporarily missing group keeps its last selection but no active controls.
    // The same runtime may later publish that group's real metadata again.
    const bool nodes = selectedPage_ == "focus";
    const auto page = std::find_if(parameterPages.begin(), parameterPages.end(), [&](const auto &p) { return p.id == selectedPage_; });
    const auto pageName = page == parameterPages.end() ? selectedPage_ : page->name;
    functionBand.setTitle(pageName + (nodes ? " node" : " band"));
    functionBand.setTooltip(nodes ? "Selects the node whose controls are shown. Selection does not change processing."
                                  : "Selects the band whose controls are shown. Selection does not change processing.");
    functionSection.setTitle(pageName + " control section");
    if (pageChanged || bands != functionBands_) {
        functionBands_ = std::move(bands);
        functionBand.clear(juce::dontSendNotification);
        for (std::size_t i = 0; i < functionBands_.size(); ++i)
            functionBand.addItem(juce::String(nodes ? "Node " : "Band ") + juce::String(functionBands_[i] + 1), static_cast<int>(i + 1));
    }
    functionBandSections_ = std::move(bandSections);
    if (pageChanged || sections != functionSections_) {
        functionSections_ = std::move(sections);
        functionSection.clear(juce::dontSendNotification);
        for (std::size_t i = 0; i < functionSections_.size(); ++i)
            functionSection.addItem(functionSections_[i].second, static_cast<int>(i + 1));
    }
    const auto band = std::find(functionBands_.begin(), functionBands_.end(), selectedBand_);
    functionBand.setSelectedId(band == functionBands_.end() ? 0 : static_cast<int>(band - functionBands_.begin() + 1), juce::dontSendNotification);
    const auto section = std::find_if(functionSections_.begin(), functionSections_.end(), [&](const auto &s) { return s.first == selectedSection_; });
    functionSection.setSelectedId(section == functionSections_.end() ? 0 : static_cast<int>(section - functionSections_.begin() + 1), juce::dontSendNotification);
}
bool DevicePanelView::chooseFunctionBand(std::int32_t band) {
    if (!eqPresentation() || !validTarget() ||
        std::find(functionBands_.begin(), functionBands_.end(), band) == functionBands_.end()) return false;
    if (selectedBand_ == band) return true;
    const auto page = selectedPage_;
    const auto safe = SafePointer<DevicePanelView>(this);
    cancelInteractions();
    if (!safe || !safe->validTarget() || safe->selectedPage_ != page) return false;
    const auto selected = std::find(functionBands_.begin(), functionBands_.end(), band);
    if (selected == functionBands_.end()) return false;
    selectedBand_ = band;
    functionSelectionByPage_[selectedPage_] = {selectedBand_, selectedSection_};
    functionBand.setSelectedId(static_cast<int>(selected - functionBands_.begin() + 1), juce::dontSendNotification);
    present(*data);
    if (!safe) return false;
    strip.resized(); repaint();
    return true;
}
bool DevicePanelView::chooseFunctionSection(const std::string &section) {
    const auto requested = section;
    if (!eqPresentation() || !validTarget() || std::none_of(functionSections_.begin(), functionSections_.end(),
        [&](const auto &s) { return s.first == requested; })) return false;
    if (selectedSection_ == requested) return true;
    const auto page = selectedPage_;
    const auto safe = SafePointer<DevicePanelView>(this);
    cancelInteractions();
    if (!safe || !safe->validTarget() || safe->selectedPage_ != page) return false;
    const auto selected = std::find_if(functionSections_.begin(), functionSections_.end(), [&](const auto &s) { return s.first == requested; });
    if (selected == functionSections_.end()) return false;
    selectedSection_ = requested;
    functionSelectionByPage_[selectedPage_] = {selectedBand_, selectedSection_};
    functionSection.setSelectedId(static_cast<int>(selected - functionSections_.begin() + 1), juce::dontSendNotification);
    present(*data);
    if (!safe) return false;
    strip.resized(); repaint();
    return true;
}
std::string_view DevicePanelView::parameterPageUnavailableReason() const {
    const auto page = std::find_if(parameterPages.begin(), parameterPages.end(), [&](const auto &p) { return p.id == selectedPage_; });
    return page == parameterPages.end() ? std::string_view() : std::string_view(page->unavailableReason);
}
const panel::Record *DevicePanelView::pageEnableRecord(const ParameterPageControl &page) const {
    if (!data || page.enableParameter.empty()) return nullptr;
    const auto declared = std::find_if(data->declarations().begin(), data->declarations().end(), [&](const auto &p) {
        return p.id == page.enableParameter && p.modifiable;
    });
    if (declared == data->declarations().end()) return nullptr;
    const auto at = std::find_if(data->parameters().begin(), data->parameters().end(), [&](const auto &r) { return r.id == page.enableParameter; });
    return at != data->parameters().end() && !at->missing ? &*at : nullptr;
}
bool DevicePanelView::parameterShown(const std::string &parameter) const {
    return !data || parameterShown(*data, parameter);
}
bool DevicePanelView::parameterShown(const DevicePanelData &next, const std::string &parameter) const {
    if (parameterPages.empty()) return true;
    if (!parameterPageUnavailableReason().empty()) return false;
    const auto group = next.groups().find(parameter);
    if (group == next.groups().end() || group->second.groupId.empty()) return true;
    const auto &metadata = group->second;
    if (metadata.groupEnableControl || metadata.groupId != selectedPage_) return false;
    if (!eqPresentation(next)) return true;
    if (!metadata.sectionId.empty() && metadata.sectionId != selectedSection_) return false;
    return metadata.bandIndex < 0 || metadata.bandIndex == selectedBand_;
}
bool DevicePanelView::controlsMatch(const DevicePanelData &next) const {
    if (!eqPresentation(next)) {
        if (controls.size() != next.resolved.entries.size()) return false;
        for (std::size_t i = 0; i < controls.size(); ++i)
            if (controls[i]->parameter != next.resolved.entries[i].id) return false;
        return true;
    }
    std::size_t index = 0;
    for (const auto &record : next.parameters()) {
        if (!parameterShown(next, record.id)) continue;
        if (index >= controls.size() || controls[index]->parameter != record.id) return false;
        ++index;
    }
    return index == controls.size();
}
bool DevicePanelView::chooseParameterPage(const std::string &page) {
    const auto requested = page;
    if (!validTarget() || std::none_of(parameterPages.begin(), parameterPages.end(), [&](const auto &p) { return p.id == requested; })) return false;
    if (selectedPage_ == requested) return true;
    const auto safe = SafePointer<DevicePanelView>(this);
    cancelInteractions();
    if (!safe || !safe->validTarget()) return false;
    if (std::none_of(parameterPages.begin(), parameterPages.end(), [&](const auto &p) { return p.id == requested; })) return false;
    selectedPage_ = requested;
    if (selectedPage_ != "eq") releaseEqSpectrum();
    if (!safe) return false;
    if (selectedPage_ != "limiter") releaseLimiterReduction();
    for (auto &p : parameterPages) p.selector->setToggleState(p.id == selectedPage_, juce::dontSendNotification);
    if (eqPresentation()) present(*data);
    if (!safe) return false;
    resized(); strip.resized(); repaint();
    return true;
}
bool DevicePanelView::setParameterPageEnabled(const std::string &page, bool on) {
    const auto found = std::find_if(parameterPages.begin(), parameterPages.end(), [&](const auto &p) { return p.id == page; });
    if (found == parameterPages.end()) return false;
    const auto *record = pageEnableRecord(*found);
    if (!record || !validTarget()) { endFunctionParameter(); return false; }
    const auto parameter = found->enableParameter;
    const double before = record->playing, desired = on ? 1. : 0.;
    const auto safe = SafePointer<DevicePanelView>(this);
    if (!editFunctionParameter(parameter, engine::ParamEventKind::Begin, before) || !safe) return false;
    safe->strip.root.followDeviceAutomation(safe->id, parameter);
    if (!safe) return false;
    const bool accepted = editFunctionParameter(parameter, engine::ParamEventKind::Value, desired);
    if (!safe) return accepted;
    editFunctionParameter(parameter, engine::ParamEventKind::End, desired);
    if (!safe) return accepted;
    const auto current = std::find_if(parameterPages.begin(), parameterPages.end(), [&](const auto &p) { return p.id == page; });
    if (current != parameterPages.end()) current->enabled->setToggleState(accepted ? on : before >= .5, juce::dontSendNotification);
    return accepted;
}
bool DevicePanelView::presentParameterPages(const DevicePanelData &next) {
    // No demand means an identity-only row, not removal of a configured page.
    if (next.groups().empty() && data && eqPresentation(*data) && eqPresentation(next)) return true;
    if (data && eqPresentation(*data) == eqPresentation(next) &&
        (&data->groups() == &next.groups() || data->groups() == next.groups())) return true;
    struct Page { std::string id, name, enable; std::string unavailable{}; };
    std::vector<Page> pages;
    for (const auto &r : next.parameters()) {
        const auto metadata = next.groups().find(r.id);
        if (metadata == next.groups().end() || metadata->second.groupId.empty()) continue;
        const auto &m = metadata->second;
        auto at = std::find_if(pages.begin(), pages.end(), [&](const auto &p) { return p.id == m.groupId; });
        if (at == pages.end()) {
            pages.push_back({m.groupId, m.groupName.empty() ? m.groupId : m.groupName, {}}); at = std::prev(pages.end());
        }
        if (m.groupEnableControl && at->enable.empty()) at->enable = r.id;
    }
    if (eqPresentation(next)) {
        if (std::none_of(pages.begin(), pages.end(), [](const auto &page) { return page.id == "eq"; }))
            pages.push_back({"eq", "EQ", {}});
    }
    const bool eqFamily = eqPresentation(next);
    const auto order = [eqFamily](const std::string &id) {
        return eqFamily ? (id == "eq" ? 0 : id == "carve" ? 1 : id == "focus" ? 2 : 3)
                        : (id == "clip" ? 0 : id == "limiter" ? 1 : 2);
    };
    std::stable_sort(pages.begin(), pages.end(), [&](const auto &a, const auto &b) { return order(a.id) < order(b.id); });
    bool same = pages.size() == parameterPages.size();
    if (same) for (std::size_t i = 0; i < pages.size(); ++i)
        if (pages[i].id != parameterPages[i].id || pages[i].name != parameterPages[i].name || pages[i].enable != parameterPages[i].enableParameter ||
            pages[i].unavailable != parameterPages[i].unavailableReason) { same = false; break; }
    if (same) return true;
    const auto safe = SafePointer<DevicePanelView>(this);
    cancelInteractions();
    if (!safe) return false;
    ++pageEpoch_;
    // Retain the bounded native page selections if a configured group temporarily
    // disappears. Its selector still disappears; a future real group can restore
    // that page's band/section without inventing an unavailable placeholder.
    parameterPages.clear();
    if (std::none_of(pages.begin(), pages.end(), [&](const auto &p) { return p.id == selectedPage_; })) {
        const auto limiter = std::find_if(pages.begin(), pages.end(), [](const auto &p) { return p.id == "limiter"; });
        selectedPage_ = limiter != pages.end() ? limiter->id : pages.empty() ? std::string() : pages.front().id;
    }
    for (const auto &page : pages) {
        ParameterPageControl p;
        p.id = page.id; p.name = page.name; p.enableParameter = page.enable;
        p.unavailableReason = page.unavailable;
        p.selector = std::make_unique<juce::TextButton>(page.name);
        p.selector->setTitle(page.name + " parameters");
        p.selector->setTooltip(page.unavailable.empty() ? "Shows " + juce::String(page.name) + " controls. Page selection does not change processing."
                                                      : juce::String(page.unavailable));
        p.selector->setColour(juce::TextButton::buttonColourId, look::button);
        p.selector->setColour(juce::TextButton::buttonOnColourId, look::accent);
        p.selector->setToggleState(p.id == selectedPage_, juce::dontSendNotification);
        p.enabled = std::make_unique<ActivatorButton>();
        p.enabled->setTitle(page.name + " processing enabled");
        p.enabled->setTooltip("Enables " + juce::String(page.name) + " processing independently; the other page and saved settings are unchanged.");
        const auto epoch = pageEpoch_;
        p.selector->onClick = [safe, epoch, group = page.id] {
            if (safe && safe->pageEpoch_ == epoch) safe->chooseParameterPage(group);
        };
        p.enabled->onClick = [safe, epoch, group = page.id] {
            if (!safe || safe->pageEpoch_ != epoch || !safe->validTarget()) return;
            const auto at = std::find_if(safe->parameterPages.begin(), safe->parameterPages.end(), [&](const auto &current) { return current.id == group; });
            if (at != safe->parameterPages.end()) safe->setParameterPageEnabled(group, at->enabled->getToggleState());
        };
        addAndMakeVisible(*p.selector); addAndMakeVisible(*p.enabled);
        parameterPages.push_back(std::move(p));
    }
    return true;
}
void DevicePanelView::visibilityChanged() {
    if (isShowing()) return;
    const auto safe = SafePointer<DevicePanelView>(this);
    if (configuring && data && data->plugin) closeConfigure();
    if (!safe) return;
    releaseEqSpectrum();
    if (!safe) return;
    releaseLimiterReduction();
    if (!parameterPages.empty()) cancelInteractions();
}
bool DevicePanelView::hasPlaybackProblem() const noexcept {
    return strip.hasPlaybackProblem(id);
}
juce::String DevicePanelView::getTooltip() {
    return hasPlaybackProblem() ? juce::String(kPlaybackProblemDescription) : juce::String{};
}
bool DevicePanelView::refreshPlaybackProblem() {
    const bool next = hasPlaybackProblem();
    if (next == playbackProblemPresented_)
        return false;
    playbackProblemPresented_ = next;
    const juce::String description(next ? kPlaybackProblemDescription : "");
    setDescription(description);
    title.setDescription(description);
    enabled.setDescription(description);
    updateTitleTooltip();
    layoutTitle(strip.folded(id));
    repaint();
    return true;
}
void DevicePanelView::updateTitleTooltip() {
    if (!data)
        return;
    const auto &d = *data;
    auto hint = juce::String("Double-click to fold; right-click for device commands.\n") +
                      (d.available ? "Latency: " + juce::String(d.latency) + " samples."
                                   : juce::String("This device is unavailable."));
    if (nativeFunction()) hint += "\nNative Mix View function; drag the name to move it within this channel.";
    if (mixCard_ && eqPresentation(d))
        hint += "\nSelect a band, then drag its frequency or gain handle.";
    if (!d.processingIssue.empty()) {
        hint += "\nConfiguration failed: " + juce::String(d.processingIssue);
        hint += "\nTo retry, reopen this project using File > Open Project.";
    }
    if (!d.loadIssue.empty())
        hint += "\n" + juce::String(kPluginUnavailable) + ": " + juce::String(d.loadIssue);
    if (playbackProblemPresented_)
        hint += "\n" + juce::String(kPlaybackProblemDescription);
    if (title.getTooltip() != hint) title.setTooltip(hint);
}
void DevicePanelView::present(const DevicePanelData &d) {
    if (!presentParameterPages(d)) return;
    presentFunctionSelection(d);
    updateEqPreview(d);
    const bool bypassChanged = data != nullptr && data->enabledNow() != d.enabledNow();
    data = &d;
    if (configuring) {
        const auto safe = SafePointer<DevicePanelView>(this);
        if (!allowsConfigure())
            closeConfigure();
        else if (configureSession_)
            queueConfigureTouch(d);
        if (!safe) return;
    }
    if (bypassChanged)
        repaint();
    const bool loadFailed = !d.loadIssue.empty();
    const bool loadStateChanged = wrench.isEnabled() == loadFailed;
    if (!refreshPlaybackProblem())
        updateTitleTooltip();
    const auto activatorHint = juce::String(nativeFunction() ? "Remove this Mix View function. Undo restores its settings."
                                           : loadFailed ? "This plug-in is unavailable."
                                                        : "Switches the device on or off.");
    if (enabled.getTooltip() != activatorHint) enabled.setTooltip(activatorHint);
    const auto caption = d.processingIssue.empty() ? juce::String(d.name)
                                                  : "Configuration failed: " + juce::String(d.name);
    if (title.getButtonText() != caption) {
        title.setButtonText(caption);
    }
    enabled.setEnabled(!loadFailed);
    enabled.setToggleState(d.enabledNow() && !loadFailed, juce::dontSendNotification);
    wrench.setEnabled(!loadFailed);
    // The declarations ProjectDocument::assignSample checks also name the sample slots on the panel.
    // With Pd off Color Cab still keeps its Impulse tile so a drop can explain why it cannot load.
    if (!sample && !sampleSlotsRead_ && d.format == "internal" && !d.uid.empty()) {
        sampleSlotsRead_ = true;
        std::vector<SampleTile::DeclaredSlot> slots;
        for (const auto &declared : device::parsePdDeclarations(builtinDeclarations(d.uid)).samples)
            slots.push_back({declared.id, juce::String(declared.name)});
        if (slots.empty() && d.uid == "org.adidaw.builtin.color-cab")
            slots.push_back({1, "Impulse"});
        if (!slots.empty()) {
            sample = std::make_unique<SampleTile>(strip, id, std::move(slots));
            sample->targetValid = [this] { return validTarget(); };
            sample->hotSwapButton.onClick = [this] {
                if (data && sample) requestHotSwap(*data, sample->slot, menuEpoch_);
            };
            addAndMakeVisible(*sample);
            resized();
        }
    }
    // Only a plug-in has a window of its own (parity-7.5 row 1).
    const bool isFolded = strip.folded(id);
    if (hotSwapButton.isVisible() != (!isFolded && canHotSwap()) ||
        wrench.isVisible() != (!isFolded && d.plugin) ||
        configureButton.isVisible() != (!isFolded && !mixCard_ && allowsConfigure()))
        resized();
    // Native EQ uses its declared stable band/section metadata, not the ordinary
    // plug-in Configure cap. Construct only this small visible control set.
    std::vector<panel::Entry> eqEntries;
    if (eqPresentation()) {
        for (const auto &record : d.parameters())
            if (parameterShown(record.id)) eqEntries.push_back({record.id, record.missing});
    }
    const auto &entries = eqPresentation() ? eqEntries : d.resolved.entries;
    bool same = controls.size() == entries.size();
    if (same)
        for (std::size_t i = 0; i < controls.size(); ++i)
            if (controls[i]->parameter != entries[i].id) {
                same = false;
                break;
            }
    // The plug-in's default for a parameter, where Delete returns it (manual 41.6); 0 when the
    // feed knows none.
    auto defaultOf = [&d](const std::string &param) {
        const auto found = d.defaultValues().find(param);
        return found == d.defaultValues().end() ? 0.0 : found->second;
    };
    if (!same) {
        parameterReorder_.reset();
        const auto safe = SafePointer<DevicePanelView>(this);
        endFunctionParameter();
        if (!safe) return;
        ++controlsEpoch_;
        for (auto &control : controls) {
            control->slider.onDragStart = {}; control->slider.onValueChange = {}; control->slider.onDragEnd = {};
            control->toggle.onClick = {}; control->menu.onChange = {};
        }
        controls.clear();
        const auto epoch = controlsEpoch_;
        for (const auto &e : entries)
            for (const auto &r : d.parameters())
                if (r.id == e.id) {
                    auto c = std::make_unique<ParameterControl>(strip, id, r, defaultOf(r.id));
                    c->format = d.format;
                    c->parameterGesture = [this, epoch](const std::string &param, engine::ParamEventKind kind, double v) {
                        if (epoch != controlsEpoch_) return false;
                        if (kind != engine::ParamEventKind::End && !parameterShown(param)) return false;
                        if (nativeFunction() || !parameterPages.empty()) return editFunctionParameter(param, kind, v);
                        if (kind == engine::ParamEventKind::End && parameterGesture)
                            return parameterGesture(param, kind, v);
                        if (!validTarget()) return false;
                        return parameterGesture ? parameterGesture(param, kind, v) : strip.edit(id, param, kind, v);
                    };
                    addAndMakeVisible(*c);
                    controls.push_back(std::move(c));
                    break;
                }
        resized();
    }
    for (auto &c : controls)
        for (const auto &r : d.parameters())
            if (c->parameter == r.id) {
                c->present(r, defaultOf(r.id));
                const auto declared = std::find_if(d.declarations().begin(), d.declarations().end(), [&](const auto &p) {
                    return p.id == r.id;
                });
                c->setEnabled(!r.missing && declared != d.declarations().end() && declared->modifiable);
                if (nativeFunction() && !r.missing && declared != d.declarations().end() && !declared->modifiable)
                    c->setTooltip("Read-only in this device tier. The current setting is retained; editing is not connected.");
                break;
            }
    for (auto &page : parameterPages) {
        const auto *record = pageEnableRecord(page);
        page.enabled->setEnabled(record && validTarget());
        page.enabled->setToggleState(record && record->playing >= .5, juce::dontSendNotification);
    }
    if (loadStateChanged || !parameterPages.empty() || mixCard_) resized();
}
int DevicePanelView::widthFor(int height) const noexcept {
    if (strip.folded(id))
        return kFoldedWidth;
    const int top = kTitleHeight + (configuring ? 26 : 2) + (parameterPages.empty() ? 0 : kPageHeight) +
                    (data && !data->loadIssue.empty() ? kLoadIssueHeight : 0) +
                    (eqPresentation() && parameterPageUnavailableReason().empty() &&
                     (!functionBands_.empty() || !functionSections_.empty()) ? kPageHeight : 0);
    const int rows = std::max(1, (height - top - 2) / ParameterControl::kHeight);
    const auto count = static_cast<std::size_t>(std::count_if(controls.begin(), controls.end(), [&](const auto &c) { return parameterShown(c->parameter); }));
    const int columns = static_cast<int>((count + static_cast<std::size_t>(rows) - 1) /
                                         static_cast<std::size_t>(rows));
    // Wide enough for the title bar's switches, a name and Configure; and a sample tile beside.
    int pageWidth = 12;
    for (const auto &page : parameterPages) pageWidth += pageSelectorWidth(page) + (page.enableParameter.empty() ? 0 : 18) + 3;
    return std::max({eqPresentation() ? 270 : hotSwapButton.isVisible() ? 168 : 150, pageWidth,
                     columns * ParameterControl::kWidth + 12 + (sample ? kTileWidth : 0)});
}
void DevicePanelView::layoutCompactControls(int top) {
    const int h = std::min(ParameterControl::kHeight, getHeight() - top - 2);
    const auto left = look::fieldHeight + look::border;
    const auto width = std::max(0, getWidth() - left - look::border);
    const auto columns = h >= look::fieldHeight ? std::clamp(width / (4 * look::fieldHeight), 1, 4) : 0;
    const int w = width / std::max(1, columns);
    std::array<ParameterControl *, 4> selected{};
    int placed = 0;
    const auto take = [&](ParameterControl &control) {
        if (placed >= columns || std::find(selected.begin(), selected.end(), &control) != selected.end()
            || !parameterShown(control.parameter)) return;
        selected[static_cast<std::size_t>(placed++)] = &control;
    };
    for (const char *key : {"Threshold", "Ratio", "Input Gain", "Output Level", "Amount", "Attack", "Release"}) {
        if (placed == columns) break;
        for (auto &control : controls)
            if (control->getTitle().endsWithIgnoreCase(key)) take(*control);
    }
    for (auto &control : controls) take(*control);
    for (auto &control : controls) {
        const auto at = std::find(selected.begin(), selected.end(), control.get());
        control->setVisible(at != selected.end());
        if (at != selected.end()) control->setBounds(left + static_cast<int>(at - selected.begin()) * w, top,
                                                     std::max(0, w - look::border), h);
    }
}
void DevicePanelView::layoutTitle(bool isFolded) {
    const int markSpace = playbackProblemPresented_ ? static_cast<int>(kPlaybackProblemMarkSize) + 2 : 0;
    enabled.setBounds(2, 1, 16, 15);
    title.setVisible(!isFolded);
    configureButton.setVisible(!mixCard_ && !isFolded && allowsConfigure());
    wrench.setVisible(!isFolded && data != nullptr && data->plugin);
    hotSwapButton.setVisible(!isFolded && canHotSwap());
    // Live's order (manual 23.3's figure): the activator, the fold switch, the wrench, the name.
    const int nameLeft = wrench.isVisible() ? 50 : 32;
    wrench.setBounds(32, 1, 16, 15);
    const int titleRight = getWidth() - (configureButton.isVisible() ? 58 : 4);
    const int swapWidth = hotSwapButton.isVisible() ? 17 : 0;
    hotSwapButton.setBounds(titleRight - swapWidth, 1, swapWidth, 15);
    title.setBounds(nameLeft, 1, std::max(0, titleRight - swapWidth - nameLeft - markSpace), 15);
    configureButton.setBounds(getWidth() - 56, 2, 52, 13);
    if (mixCard_ && !isFolded) {
        const auto rail = look::fieldHeight;
        enabled.setBounds(0, 0, rail, rail);
        title.setBounds(0, 0, std::max(0, getHeight() - rail - markSpace), rail);
        title.setTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi)
                               .translated(0.f, static_cast<float>(getHeight())));
    }
}
void DevicePanelView::resized() {
    const bool isFolded = strip.folded(id);
    layoutTitle(isFolded);
    if (sample) {
        sample->setVisible(!isFolded);
        sample->hotSwapButton.setVisible(hotSwapButton.isVisible());
        const int tileTop = kTitleHeight + (configuring ? 26 : 4) + (parameterPages.empty() ? 0 : kPageHeight);
        sample->setBounds(getWidth() - kTileWidth, tileTop, kTileWidth - 6, std::min(84, std::max(0, getHeight() - tileTop - 4)));
    }
    search.setVisible(!isFolded && configuring);
    parameterList.setVisible(search.isVisible() && data && data->plugin && !mixCard_);
    auto searchRow = juce::Rectangle<int>{4, kTitleHeight + 2, std::max(0, getWidth() - 8), 22};
    if (parameterList.isVisible()) {
        parameterList.setBounds(searchRow.removeFromRight(83).reduced(0, 1));
        searchRow.removeFromRight(4);
    }
    search.setBounds(searchRow);
    const int rail = mixCard_ ? look::fieldHeight : 0;
    int top = mixCard_ ? look::border : kTitleHeight + (configuring ? 26 : 2);
    if (data && !data->loadIssue.empty())
        top += kLoadIssueHeight;
    if (mixCard_ && !isFolded) {
        const auto graph = eqPreviewBounds();
        if (!graph.isEmpty()) top = static_cast<int>(graph.getBottom()) + 2;
        if (getHeight() < look::mixStackNearRow + look::fieldHeight) {
            releaseLimiterReduction();
            for (auto &page : parameterPages) {
                page.selector->setVisible(false);
                page.enabled->setVisible(false);
            }
            functionBand.setVisible(false);
            functionSection.setVisible(false);
            layoutCompactControls(top);
            return;
        }
    }
    int pageX = 6 + rail;
    for (auto &page : parameterPages) {
        page.selector->setVisible(!isFolded);
        page.enabled->setVisible(!isFolded && !page.enableParameter.empty());
        if (!page.enableParameter.empty()) { page.enabled->setBounds(pageX, top + 2, 16, 16); pageX += 18; }
        const auto width = pageSelectorWidth(page);
        page.selector->setBounds(pageX, top + 1, width, kPageHeight - 3); pageX += width + 3;
    }
    if (!parameterPages.empty()) top += kPageHeight;
    const auto reduction = limiterPreviewBounds();
    if (reduction.isEmpty()) releaseLimiterReduction();
    else top = std::max(top, reduction.getBottom() + static_cast<int>(look::gap));
    const bool functionControls = !isFolded && eqPresentation() && parameterPageUnavailableReason().empty() &&
                                  (!functionBands_.empty() || !functionSections_.empty());
    functionBand.setVisible(functionControls && !functionBands_.empty());
    functionSection.setVisible(functionControls && !functionSections_.empty());
    const bool bandApplies = std::find(functionBandSections_.begin(), functionBandSections_.end(), std::string()) != functionBandSections_.end() ||
                            std::find(functionBandSections_.begin(), functionBandSections_.end(), selectedSection_) != functionBandSections_.end();
    functionBand.setEnabled(bandApplies && validTarget());
    functionSection.setEnabled(validTarget());
    functionBand.setBounds(6 + rail, top + 1, 98, kPageHeight - 3);
    functionSection.setBounds(108 + rail, top + 1, std::max(0, std::min(150, getWidth() - 114 - rail)), kPageHeight - 3);
    if (functionControls) top += kPageHeight;
    const int rows = std::max(1, (getHeight() - top - 2) / ParameterControl::kHeight);
    int visible = 0;
    for (auto &control : controls) {
        const bool shown = !isFolded && parameterShown(control->parameter);
        control->setVisible(shown);
        if (!shown) continue;
        const int column = visible / rows, row = visible++ % rows;
        control->setBounds(6 + rail + column * ParameterControl::kWidth,
                               top + row * ParameterControl::kHeight, ParameterControl::kWidth - 4,
                               ParameterControl::kHeight);
    }
}
void DevicePanelView::paint(juce::Graphics &g) {
    // Live's device: grey, its title bar and a 2 pt edge lighter; blue-grey when the
    // device has the focus, as Live marks the selected device.
    const bool range = !mixCard_ && strip.selectionActsOn(id);
    auto edge = range || hasKeyboardFocus(true) ? look::focus : look::field;
    if (nativeFunction() && !range)
        edge = look::accent;
    const bool loadFailed = data && !data->loadIssue.empty();
    g.fillAll(look::panel);
    g.setColour(edge);
    if (!mixCard_) g.fillRect(0, 0, getWidth(), kTitleHeight);
    if (loadFailed) g.setColour(look::error);
    g.drawRect(getLocalBounds().toFloat(), mixCard_ ? look::hairline : look::heavyLine);
    if (mixCard_ && eqPreview_) {
        const auto graph = eqPreviewBounds();
        if (!graph.isEmpty()) {
            juce::Graphics::ScopedSaveState saved(g);
            g.reduceClipRegion(graph.toNearestInt());
            drawEqGrid(g, graph);
            if (!eqPreview_->spectrumPath.isEmpty()) {
                g.setColour(look::textDim);
                g.strokePath(eqPreview_->spectrumPath, juce::PathStrokeType(look::hairline),
                    juce::AffineTransform{graph.getWidth(), 0.f, graph.getX(), 0.f, graph.getHeight(), graph.getY()});
            }
            const juce::AffineTransform transform{graph.getWidth(), 0.f, graph.getX(), 0.f,
                                                  -graph.getHeight() / eqDbSpan, graph.getCentreY()};
            for (std::size_t r = 0; r < eqPreview_->paths.size(); ++r) {
                const auto &path = eqPreview_->paths[r];
                if (path.isEmpty()) continue;
                const auto colour = r == 0 ? static_cast<juce::Colour>(look::accent) : look::trackColour(r);
                if (r == 0 && eqPreview_->hovered >= 0) {
                    g.setColour(colour.withMultipliedAlpha(.18f));
                    g.strokePath(path, juce::PathStrokeType(look::line + look::heavyLine), transform);
                }
                g.setColour(colour);
                g.strokePath(path, juce::PathStrokeType(look::line), transform);
            }
            drawEqNodes(g, graph);
        }
    }
    const auto reduction = limiterPreviewBounds();
    if (!reduction.isEmpty()) paintLimiterReduction(g, reduction);
    // Live's fold switch beside the activator.
    const auto fold = foldSwitch().toFloat();
    if (!fold.isEmpty()) {
        g.setColour(look::button);
        g.fillEllipse(fold);
        g.setColour(look::text);
        g.drawEllipse(fold.reduced(0.5f), 1.f);
        look::triangle(g, fold.reduced(3.f).translated(0.5f, 0),
                       strip.folded(id) ? 0.f : juce::MathConstants<float>::halfPi, look::text);
    }
    if (strip.folded(id) && data) {
        // A folded device is a narrow bar with its name running up it, as Live's.
        g.setColour(loadFailed ? look::error : look::text);
        g.setFont(look::font(11.5f));
        juce::Graphics::ScopedSaveState saved(g);
        g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, 0,
                                                       static_cast<float>(getHeight())));
        g.drawText(title.getButtonText(), kTitleHeight + 2, getHeight() + 4,
                   std::max(0, getHeight() - kTitleHeight - 4), kFoldedWidth - 8,
                   juce::Justification::centredLeft);
    } else if (loadFailed) {
        const int statusTop = kTitleHeight + (configuring ? 28 : 4);
        g.setColour(look::error);
        g.setFont(look::font(12.f, true));
        g.drawText(kPluginUnavailable, 8, statusTop, getWidth() - 16, 16,
                   juce::Justification::centredLeft);
        g.setColour(look::text);
        g.setFont(look::font(11.5f));
        g.drawFittedText(juce::String(data->loadIssue), 8, statusTop + 18, getWidth() - 16, 24,
                         juce::Justification::topLeft, 2);
    } else if (data && !parameterPageUnavailableReason().empty()) {
        g.setColour(look::text);
        g.setFont(look::font(12.f));
        g.drawFittedText(juce::String(parameterPageUnavailableReason().data(), parameterPageUnavailableReason().size()),
                        10, kTitleHeight + kPageHeight + 8, std::max(0, getWidth() - 20),
                        std::max(0, getHeight() - kTitleHeight - kPageHeight - 16), juce::Justification::centred, 5);
    } else if (allowsConfigure() && data->resolved.showsConfigureHint) {
        g.setColour(look::text);
        g.drawFittedText("Choose parameters with Configure", 8, kTitleHeight + 6, getWidth() - 16,
                         36, juce::Justification::centred, 2);
    }
}
void DevicePanelView::paintOverChildren(juce::Graphics &g) {
    // Wash the whole device, including its controls, while keeping them editable.
    if (data != nullptr && !data->enabledNow())
        g.fillAll(look::frame.withAlpha(0.55f));
    if (!hasPlaybackProblem())
        return;
    constexpr auto size = kPlaybackProblemMarkSize;
    if (strip.folded(id)) {
        paintPlaybackProblemMark(g, {(static_cast<float>(getWidth()) - size) * .5f,
                                    static_cast<float>(getHeight()) - size - 3.f, size, size});
    } else if (mixCard_) {
        paintPlaybackProblemMark(g, {(static_cast<float>(look::fieldHeight) - size) * .5f,
                                    static_cast<float>(look::fieldHeight) + 1.f, size, size});
    } else {
        paintPlaybackProblemMark(g, {static_cast<float>(title.getRight()) + 1.f, 4.f, size, size});
    }
}
bool DevicePanelView::canHotSwap() const {
    if (!strip.hotSwap || mixCard_ || !data || !data->available || data->uid.empty() || nativeFunction())
        return false;
    if (data->format != "internal" && data->format != "clap" && data->format != "vst3")
        return false;
    const auto current = strip.root.current();
    const auto *track = current ? current->findTrack(data->track) : nullptr;
    return track && !track->frozen && !track->frozenByGroup;
}
bool DevicePanelView::requestHotSwap(DevicePanelData expected, std::optional<std::int32_t> slot,
                                     std::uint64_t epoch) {
    const auto safe = SafePointer<DevicePanelView>(this);
    if (menuEpoch_ != epoch || !canHotSwap() || !validTarget() || !safe)
        return false;
    if (!safe->strip.validExpected(expected) || !safe || safe->menuEpoch_ != epoch || !safe->strip.hotSwap)
        return false;
    safe->strip.hotSwap(expected, slot);
    return true;
}
bool DevicePanelView::keyPressed(const juce::KeyPress &k) {
    if (k == juce::KeyPress::escapeKey && parameterReorder_) {
        parameterReorder_.reset();
        return true;
    }
    if (!validTarget())
        return k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey;
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        return mixCard_ ? strip.action("device.remove", {{"id", id}}) : strip.deleteDevices(id);
    // Live moves between devices with the left and right arrows (§41.4.1.5).
    if (k == juce::KeyPress::leftKey || k == juce::KeyPress::rightKey)
        return navigate ? navigate(k == juce::KeyPress::rightKey) : strip.focusNext(id, k == juce::KeyPress::rightKey);
    return false;
}
void DevicePanelView::focusGained(FocusChangeType) {
    strip.chose(id);
    if (!mixCard_ && !strip.isSelected(id))
        strip.selectTitle(*this, false);
    repaint();
}
void DevicePanelView::focusOfChildComponentChanged(FocusChangeType) {
    if (search.hasKeyboardFocus(false))
        beginInteraction(Interaction::Text);
    else if (!rename.isVisible())
        endInteraction(Interaction::Text);
    if (hasKeyboardFocus(true)) {
        strip.chose(id);
        if (!mixCard_ && !strip.isSelected(id))
            strip.selectTitle(*this, false);
    }
    repaint();
}
void DevicePanelView::mouseDown(const juce::MouseEvent &e) {
    parameterReorder_.reset();
    dragTarget_.reset();
    draggedDevices_.clear();
    selectOnRelease_ = false;
    startedDeviceDrag_ = false;
    if (!validTarget()) return;
    const bool titleHit = e.eventComponent == &title ||
                          (e.eventComponent == this && !foldSwitch().contains(e.getPosition()) &&
                           (e.y < kTitleHeight || strip.folded(id)));
    if (e.mods.isPopupMenu() && (e.eventComponent == &title || e.eventComponent == this)) {
        if (!mixCard_) {
            if (!strip.isSelected(id)) strip.selectTitle(*this, false);
            focusIfShowing(*this);
        }
        contextMenu(e.getScreenPosition());
    } else if (!mixCard_ && titleHit && e.mods.isLeftButtonDown()) {
        selectOnRelease_ = !e.mods.isShiftDown() && strip.selectionActsOn(id);
        if (!selectOnRelease_) strip.selectTitle(*this, e.mods.isShiftDown());
        focusIfShowing(*this);
    } else if (e.eventComponent == this && foldSwitch().contains(e.getPosition())) {
        focusIfShowing(*this);
        strip.fold(id);
    } else if (e.eventComponent == this && e.mods.isLeftButtonDown()) {
        focusIfShowing(*this);
        pressEqGraph(e.position);
    }
}
juce::Rectangle<int> DevicePanelView::foldSwitch() const noexcept {
    return mixCard_ ? juce::Rectangle<int>{} : juce::Rectangle<int>{19, 3, 11, 11};
}
void DevicePanelView::mouseMove(const juce::MouseEvent &e) {
    if (!eqPreview_ || eqPreview_->edit) return;
    const auto hovered = eqBandAt(e.position);
    if (eqPreview_->hovered != hovered) {
        eqPreview_->hovered = hovered;
        repaint(eqPreviewBounds().toNearestInt());
    }
}
void DevicePanelView::mouseExit(const juce::MouseEvent &) {
    if (eqPreview_ && eqPreview_->hovered >= 0) {
        eqPreview_->hovered = -1;
        repaint(eqPreviewBounds().toNearestInt());
    }
}
void DevicePanelView::mouseUp(const juce::MouseEvent &event) {
    if (std::exchange(selectOnRelease_, false) && !startedDeviceDrag_ && event.getDistanceFromDragStart() < 4)
        strip.selectTitle(*this, false);
    releaseEqHandle();
}
void DevicePanelView::mouseDrag(const juce::MouseEvent &e) {
    if (eqPreview_ && eqPreview_->edit) { dragEqHandle(e.position); return; }
    const auto pressed = e.getMouseDownPosition();
    const bool panelTitle = e.eventComponent == this && !foldSwitch().contains(pressed) &&
                            (pressed.y < kTitleHeight || strip.folded(id));
    if (dragTarget_ || startedDeviceDrag_ || e.getDistanceFromDragStart() < 4 || !e.mods.isLeftButtonDown() ||
        !(e.eventComponent == &title || panelTitle)) return;
    auto *container = juce::DragAndDropContainer::findParentDragContainerFor(this);
    if (!container) return;
    const bool function = nativeFunction() != nullptr;
    if (function && !mixCard_ && strip.selectionActsOn(id)) {
        strip.root.say("Move native Mix functions one at a time.");
        return;
    }
    if (function ? beginFunctionDrag() : beginDeviceDrag())
        container->startDragging(juce::String(function ? "adi-mix-function" : kDeviceMoveDrag), this,
                                 juce::ScaledImage{}, function, nullptr, &e.source);
}
void DevicePanelView::mouseDoubleClick(const juce::MouseEvent &e) {
    if (!validTarget()) return;
    if (e.eventComponent == &title || e.y < kTitleHeight)
        strip.fold(id);
}
namespace {
bool samePanelDevice(const DevicePanelData &a, const DevicePanelData &b) {
    return a.id == b.id && a.track == b.track && a.chain == b.chain && a.instance == b.instance;
}
const DevicePanelData *findPanelDevice(const std::shared_ptr<const ParameterPublication> &feed,
                                      const DevicePanelData &expected) {
    if (feed)
        for (const auto &device : *feed)
            if (samePanelDevice(device, expected))
                return &device;
    return nullptr;
}
const panel::Declared *declaredParameter(const DevicePanelData &device, const std::string &id) {
    const auto &declarations = device.declarations();
    const auto found = std::find_if(declarations.begin(), declarations.end(), [&](const auto &entry) {
        return entry.id == id;
    });
    return found == declarations.end() ? nullptr : &*found;
}
bool configurableParameter(const DevicePanelData &device, const std::string &id) {
    const auto *parameter = declaredParameter(device, id);
    return parameter && parameter->modifiable;
}
bool showsParameter(const DevicePanelData &device, const std::string &id) {
    return std::any_of(device.resolved.entries.begin(), device.resolved.entries.end(),
                       [&](const auto &entry) { return entry.id == id; });
}
std::vector<std::string> panelParameterIds(const panel::Resolved &resolved) {
    std::vector<std::string> ids;
    ids.reserve(resolved.entries.size());
    for (const auto &entry : resolved.entries)
        ids.push_back(entry.id);
    return ids;
}
bool samePanelLayout(const panel::Resolved &a, const panel::Resolved &b) {
    return a.configured == b.configured &&
           std::equal(a.entries.begin(), a.entries.end(), b.entries.begin(), b.entries.end(),
                      [](const auto &left, const auto &right) { return left.id == right.id; });
}
bool hasAuthoredParameterAutomation(const engine::Snapshot &snapshot, const DevicePanelData &device,
                                    const std::string &parameter) {
    const auto ref = engine::automationParamRef(device.format, parameter);
    if (!ref) return false;
    for (const auto &lane : snapshot.automationLanes)
        if (lane && lane->ownerKind == "device" && lane->ownerId == device.id &&
            automation_override_detail::sameParam(lane->ownerKind, lane->paramRef, *ref) &&
            std::any_of(lane->envelopes.begin(), lane->envelopes.end(), [](const auto &envelope) {
                return envelope && !envelope->points.empty();
            }))
            return true;
    return false;
}
bool addPanelParameters(DeviceChainStrip &strip, const DevicePanelData &device,
                        std::span<const std::string> additions) {
    auto ids = panelParameterIds(device.resolved);
    const auto before = ids.size();
    for (const auto &parameter : additions)
        if (configurableParameter(device, parameter) && std::find(ids.begin(), ids.end(), parameter) == ids.end())
            ids.push_back(parameter);
    return ids.size() == before || strip.action("device.setPanel", {{"dev", device.id}, {"params", ids}});
}
} // namespace
bool DevicePanelView::beginParameterReorder(const std::string &parameter) {
    if (!configureSession_ || !configuring || mixCard_ || menuInteraction_ || strip.folded(id))
        return false;
    const auto expected = configureSession_->expected;
    const auto menuEpoch = menuEpoch_, controlsEpoch = controlsEpoch_;
    const auto source = parameter;
    const auto safe = SafePointer<DevicePanelView>(this);
    if (!panelTargetCurrent(expected) || !safe)
        return false;
    const auto feed = strip.latestPublication();
    if (!safe || !configureSession_ || menuEpoch_ != menuEpoch || controlsEpoch_ != controlsEpoch)
        return false;
    const auto *current = findPanelDevice(feed, expected);
    if (!current || !current->plugin || !current->available || !controlsMatch(*current) ||
        !configurableParameter(*current, source))
        return false;
    parameterReorder_ = ParameterReorder{source, panelParameterIds(current->resolved), current->resolved.configured,
                                         menuEpoch, controlsEpoch};
    return true;
}
void DevicePanelView::endParameterReorder(const std::string &parameter, juce::Point<int> position, int distance,
                                          bool labelClick) {
    auto drag = std::exchange(parameterReorder_, std::nullopt);
    const bool click = distance < 4;
    if (!drag || (click && !labelClick) || drag->parameter != parameter || !configureSession_ || !configuring ||
        menuInteraction_ || menuEpoch_ != drag->menuEpoch || controlsEpoch_ != drag->controlsEpoch ||
        strip.folded(id) || !reallyContains(position, true))
        return;
    const auto target = std::find_if(controls.begin(), controls.end(), [&](const auto &control) {
        return control->isShowing() && control->getBounds().contains(position);
    });
    if (target == controls.end())
        return;
    const auto destination = (*target)->parameter;
    if (click ? destination != drag->parameter : destination == drag->parameter)
        return;
    const auto expected = configureSession_->expected;
    const auto safe = SafePointer<DevicePanelView>(this);
    if (!panelTargetCurrent(expected) || !safe)
        return;
    const auto feed = strip.latestPublication();
    if (!safe || !configureSession_ || menuEpoch_ != drag->menuEpoch || controlsEpoch_ != drag->controlsEpoch)
        return;
    const auto *current = findPanelDevice(feed, expected);
    if (!current || !current->plugin || !current->available || !configurableParameter(*current, drag->parameter) ||
        current->resolved.configured != drag->configured ||
        !std::equal(current->resolved.entries.begin(), current->resolved.entries.end(),
                    drag->order.begin(), drag->order.end(),
                    [](const panel::Entry &entry, const auto &parameterId) { return entry.id == parameterId; }))
        return;
    if (click) {
        if (strip.root.automationMode())
            if (const auto lane = strip.root.deviceAutomationTarget(id, drag->parameter))
                strip.root.showAutomation(lane->first, lane->second);
        return;
    }
    auto &order = drag->order;
    const auto from = std::find(order.begin(), order.end(), drag->parameter);
    const auto to = std::find(order.begin(), order.end(), destination);
    if (from == order.end() || to == order.end())
        return;
    if (from < to)
        std::rotate(from, std::next(from), std::next(to));
    else
        std::rotate(to, from, std::next(from));
    strip.action("device.setPanel", {{"dev", id}, {"params", order}});
}
juce::PopupMenu DevicePanelView::commands() {
    // Live 12's order (manual 23.2.1, its figure of a device's menu): Cut, Copy and Duplicate; Rename;
    // Delete and Fold; then the device's own. Paste is not in it: the Edit menu's and Ctrl/Cmd+V.
    juce::PopupMenu m;
    const auto safe = SafePointer<DevicePanelView>(this);
    const auto epoch = menuEpoch_;
    const auto selectionEpoch = strip.selectionEpoch_;
    using Clip = DeviceChainStrip::Clip;
    const auto clipItem = [&](const char *name, char key, Clip c) {
        juce::PopupMenu::Item item(name);
        item.isEnabled = c != Clip::Duplicate || !nativeFunction();
        item.shortcutKeyDescription =
            juce::KeyPress(key, juce::ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons();
        item.action = [safe, c, epoch, selectionEpoch] {
            if (!safe || safe->menuEpoch_ != epoch) return;
            if (safe) safe->endInteraction(Interaction::Menu);
            if (safe && (safe->mixCard_ || safe->strip.selectionEpoch_ == selectionEpoch) && safe->validTarget())
                safe->strip.clipboard(c, safe->id, !safe->mixCard_);
        };
        m.addItem(std::move(item));
    };
    clipItem("Cut", 'x', Clip::Cut);
    clipItem("Copy", 'c', Clip::Copy);
    clipItem("Duplicate", 'd', Clip::Duplicate);
    m.addSeparator();
    juce::PopupMenu::Item renameItem("Rename");
    renameItem.shortcutKeyDescription =
        juce::KeyPress('r', juce::ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons();
    renameItem.isEnabled = !strip.folded(id);
    renameItem.action = [safe, epoch] {
        if (!safe || safe->menuEpoch_ != epoch) return;
        if (safe) safe->endInteraction(Interaction::Menu);
        if (safe && safe->validTarget())
            safe->beginRename();
    };
    m.addItem(std::move(renameItem));
    m.addSeparator();
    m.addItem("Delete", [safe, epoch, selectionEpoch] {
        if (!safe || safe->menuEpoch_ != epoch) return;
        if (safe) safe->endInteraction(Interaction::Menu);
        if (!safe || (!safe->mixCard_ && safe->strip.selectionEpoch_ != selectionEpoch) || !safe->validTarget())
            return;
        if (safe->mixCard_) safe->strip.action("device.remove", {{"id", safe->id}});
        else safe->strip.deleteDevices(safe->id);
    });
    m.addItem(strip.folded(id) ? "Unfold" : "Fold", [safe, epoch] {
        if (!safe || safe->menuEpoch_ != epoch) return;
        if (safe) safe->endInteraction(Interaction::Menu);
        if (safe && safe->validTarget())
            safe->strip.fold(safe->id);
    });
    if (strip.hotSwap && !mixCard_) {
        juce::PopupMenu::Item item("Hot-Swap");
        const auto keys = strip.root.commands().manager.getKeyMappings()->getKeyPressesAssignedToCommand(AppCommands::HotSwap);
        if (!keys.isEmpty())
            item.shortcutKeyDescription = keys[0].getTextDescriptionWithIcons();
        item.isEnabled = canHotSwap();
        if (item.isEnabled) {
            item.action = [safe, epoch, expected = *data] {
                if (!safe || safe->menuEpoch_ != epoch)
                    return;
                safe->endInteraction(Interaction::Menu);
                if (safe) safe->requestHotSwap(expected, std::nullopt, epoch);
            };
        }
        m.addSeparator();
        m.addItem(std::move(item));
    }
    if (strip.savePreset || strip.saveDefaultPreset) {
        const auto current = strip.root.current();
        const auto *track = current && data ? current->findTrack(data->track) : nullptr;
        const bool supported = data && !data->uid.empty() && !nativeFunction() &&
                               (data->format == "internal" || data->format == "clap" || data->format == "vst3");
        const bool eligible = supported && data->available && track && !track->frozen && !track->frozenByGroup;
        const auto addPresetItem = [&](juce::String label, decltype(&DeviceChainStrip::savePreset) handler) {
            if (!(strip.*handler)) return;
            juce::PopupMenu::Item item(std::move(label));
            item.isEnabled = eligible;
            if (eligible) {
                item.action = [safe, epoch, expected = *data, handler] {
                    if (!safe || safe->menuEpoch_ != epoch)
                        return;
                    safe->endInteraction(Interaction::Menu);
                    if (!safe || safe->menuEpoch_ != epoch || !safe->validTarget())
                        return;
                    if (!safe || safe->menuEpoch_ != epoch || !safe->strip.validExpected(expected))
                        return;
                    if (safe && safe->menuEpoch_ == epoch)
                        if (const auto callback = safe->strip.*handler)
                            callback(expected);
                };
            }
            m.addItem(std::move(item));
        };
        m.addSeparator();
        addPresetItem(juce::String::fromUTF8("Save Preset\xe2\x80\xa6"), &DeviceChainStrip::savePreset);
        addPresetItem("Save as Default Preset", &DeviceChainStrip::saveDefaultPreset);
    }
    if (!mixCard_ && !configuring && data && data->pluginTouch) {
        const auto &paramId = data->pluginTouch->parameter;
        if (const auto *parameter = declaredParameter(*data, paramId)) {
            juce::PopupMenu touch;
            const auto choose = [safe, epoch, expected = *data](bool showAutomation) {
                if (!safe || safe->menuEpoch_ != epoch) return;
                safe->endInteraction(Interaction::Menu);
                if (safe && safe->menuEpoch_ == epoch)
                    safe->chooseLastTouch(expected, showAutomation);
            };
            touch.addItem("Show on Device Panel", parameter->modifiable && !showsParameter(*data, paramId), false,
                          [choose] { choose(false); });
            touch.addItem("Show Automation", deviceTarget(*data, *parameter).has_value(), false,
                          [choose] { choose(true); });
            m.addSeparator();
            m.addSubMenu("Last Touched: " + juce::String(parameter->name.empty() ? parameter->id : parameter->name), touch);
        }
    }
    juce::PopupMenu automation;
    if (data && data->activatorAvailable) {
        DevicePanelData expected;
        expected.id = data->id;
        expected.runtime = data->runtime;
        expected.instance = data->instance;
        automation.addItem("Show Automation", [safe, expected] {
            if (safe && safe->strip.validExpected(expected) && safe)
                safe->strip.root.showDeviceAutomation(expected.id, std::string(kDeviceActivatorParamRef));
        });
        if (const auto current = strip.root.current())
            strip.root.addReenableAutomation(automation, envelopeOf(*current, deviceActivatorTarget(*data)).lane);
    }
    strip.root.addDeviceAutomationDeleteItem(automation, id);
    if (automation.getNumItems() > 0) {
        m.addSeparator();
        for (juce::PopupMenu::MenuItemIterator iter(automation); iter.next();) {
            auto item = iter.getItem();
            if (item.action)
                item.action = [safe, epoch, action = item.action] {
                    if (!safe || safe->menuEpoch_ != epoch)
                        return;
                    safe->endInteraction(Interaction::Menu);
                    if (safe && safe->validTarget())
                        action();
                };
            m.addItem(std::move(item));
        }
    }
    if (data && strip.keyChoices && strip.setKey) {
        const auto expected=*data;juce::PopupMenu keys;
        const auto choices=strip.keyChoices(expected);
        std::map<std::int64_t,std::pair<juce::String,juce::PopupMenu>> sources;
        for(const auto& choice:choices){
            juce::PopupMenu::Item item(choice.source?(choice.tap=="pre_fx"?"Pre FX":choice.tap=="post_fx"?"Post FX":"Post Mixer"):"None");
            item.isTicked=choice.current;
            item.action=[safe,epoch,expected,choice]{
                if(!safe || safe->menuEpoch_!=epoch)return;
                safe->endInteraction(Interaction::Menu);
                if(safe && safe->validTarget() && safe->strip.validExpected(expected) && safe->strip.setKey)
                    safe->strip.setKey(expected,choice);
            };
            if(choice.source){auto& entry=sources[*choice.source];entry.first=juce::String::fromUTF8(choice.name.c_str());entry.second.addItem(std::move(item));}
            else keys.addItem(std::move(item));
        }
        for(auto& [sourceId,entry]:sources){(void)sourceId;keys.addSubMenu(entry.first,entry.second);}
        if(!choices.empty()){m.addSeparator();m.addSubMenu("Sidechain Input",keys);}
    }
    if (data && data->plugin) {
        // SC-325 (ADR-0134 d7): how the plug-in receives per-note expression, from its device menu as
        // Live's MPE Mode is. The project keeps the choice (one undo step), and it is remembered for the
        // plug-in, so the next instance of it starts with it.
        juce::PopupMenu routes;
        const auto expected = *data;
        const bool clap = data->format == "clap";
        const std::array<std::pair<engine::RouteChoice, const char *>, 4> choices{{
            {engine::RouteChoice::Auto, "Auto"},
            {engine::RouteChoice::NoteExpression, clap ? "CLAP Note Expression" : "VST3 Note Expression"},
            {engine::RouteChoice::MpeMidi, "MIDI-MPE"},
            {engine::RouteChoice::Plain, "Poly-AT (no MPE)"}}};
        for (const auto &[choice, label] : choices) {
            juce::PopupMenu::Item item(label);
            item.isTicked = data->route == engine::routeChoiceName(choice);
            item.action = [safe, epoch, expected, choice = choice] {
                if (!safe || safe->menuEpoch_ != epoch) return;
                safe->endInteraction(Interaction::Menu);
                if (!safe || !safe->validTarget() || !safe->strip.validExpected(expected)) return;
                const std::string name = engine::routeChoiceName(choice);
                Payload payload{{"dev", expected.id}};
                payload["route"] = name.empty() ? Payload(nullptr) : Payload(name);
                if (safe->strip.action("device.setExpressionRoute", std::move(payload)) && safe &&
                    safe->strip.rememberRoute)
                    safe->strip.rememberRoute(expected, choice);
            };
            routes.addItem(std::move(item));
        }
        m.addSeparator();
        m.addSubMenu("MPE", routes);
    }
    if (allowsConfigure()) {
        m.addSeparator();
        m.addItem("Configure", [safe, epoch] {
            if (!safe || safe->menuEpoch_ != epoch) return;
            if (safe) safe->endInteraction(Interaction::Menu);
            if (safe && safe->validTarget())
                safe->configure();
        });
    }
    return m;
}
void DevicePanelView::contextMenu(juce::Point<int> where) {
    if (!beginInteraction(Interaction::Menu)) return;
    auto safe = SafePointer<DevicePanelView>(this);
    const auto epoch = menuEpoch_;
    commands().showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({where.x, where.y, 1, 1}),
                             [safe, epoch](int) {
                                 if (safe && safe->menuEpoch_ == epoch) safe->endInteraction(Interaction::Menu);
                             });
}
bool DevicePanelView::beginRename() {
    // A folded device has no title bar to type in.
    if (data == nullptr || strip.folded(id) || !beginInteraction(Interaction::Text))
        return false;
    rename.setBounds(title.getBounds());
    rename.setText(juce::String::fromUTF8(data->name.c_str()), false);
    rename.selectAll();
    rename.setVisible(true);
    if (rename.isShowing())
        focusIfShowing(rename);
    return true;
}
void DevicePanelView::endRename(bool commit) {
    if (!rename.isVisible())
        return;
    rename.setVisible(false);
    // An empty name keeps the one the device has.
    const auto name = rename.getText().trim().toStdString();
    if (commit && validTarget() && data != nullptr && !name.empty() && name != data->name)
        strip.action("device.rename", {{"id", id}, {"name", name}});
    if (isShowing())
        focusIfShowing(*this);
    endInteraction(Interaction::Text);
}
bool DevicePanelView::panelTargetCurrent(const DevicePanelData &expected) {
    if (!isShowing() || !data || !samePanelDevice(*data, expected) ||
        (!mixCard_ && strip.root.arrangement.geometry.selection.track != expected.track))
        return false;
    const auto safe = SafePointer<DevicePanelView>(this);
    return validTarget() && safe && safe->strip.validExpected(expected) && safe;
}
void DevicePanelView::queueConfigureTouch(const DevicePanelData &next) {
    if (!configureSession_) return;
    if (!next.plugin || !next.available || !samePanelDevice(configureSession_->expected, next)) {
        closeConfigure();
        return;
    }
    auto &session = *configureSession_;
    const auto &touches = next.recentPluginTouches;
    if (!touches || touches->empty() || touches->back().sequence <= session.observed)
        return;
    auto &pending = session.pending;
    const auto pendingBefore = pending.size();
    for (const auto &touch : *touches) {
        if (touch.sequence <= session.observed) continue;
        session.observed = touch.sequence;
        const auto &paramId = touch.parameter;
        if (configurableParameter(next, paramId) && !showsParameter(next, paramId) &&
            std::find(pending.begin(), pending.end(), paramId) == pending.end())
            pending.push_back(paramId);
    }
    if (pending.size() != pendingBefore)
        strip.triggerAsyncUpdate();
}
void DevicePanelView::applyConfigureTouches() {
    if (!configureSession_ || configureSession_->pending.empty()) return;
    auto pending = std::exchange(configureSession_->pending, {});
    const auto expected = configureSession_->expected;
    const auto safe = SafePointer<DevicePanelView>(this);
    if (const auto poll = strip.pollParameterEdits) poll();
    if (!safe || !safe->configureSession_ || safe->strip.folded(expected.id) || !safe->panelTargetCurrent(expected) || !safe)
        return;
    const auto feed = safe->strip.latestPublication();
    if (safe)
        if (const auto *current = findPanelDevice(feed, expected))
            addPanelParameters(safe->strip, *current, pending);
}
void DevicePanelView::chooseLastTouch(const DevicePanelData &expected, bool showAutomation) {
    const auto safe = SafePointer<DevicePanelView>(this);
    if (configuring || !expected.pluginTouch || !panelTargetCurrent(expected) || !safe)
        return;
    const auto &paramId = expected.pluginTouch->parameter;
    if (showAutomation && !strip.root.deviceAutomationTarget(expected.id, paramId))
        return;
    if (!strip.addLastTouchedParameter(expected))
        return;
    if (showAutomation && safe && safe->panelTargetCurrent(expected) && safe)
        safe->strip.root.showDeviceAutomation(expected.id, paramId);
}
void DevicePanelView::closeConfigure() {
    parameterReorder_.reset();
    configureSession_.reset();
    if (!std::exchange(configuring, false)) return;
    ++menuEpoch_;
    const auto safe = SafePointer<DevicePanelView>(this);
    panelRemovalWarning_.close();
    if (!safe) return;
    endInteraction(Interaction::Menu);
    if (!safe) return;
    configureButton.setToggleState(false, juce::dontSendNotification);
    resized();
}
bool DevicePanelView::allowsConfigure() const noexcept {
    return data && !(data->format == "internal" && findBuiltin(data->uid)) && !eqPresentation();
}
void DevicePanelView::configure() {
    if (!allowsConfigure() || !validTarget() || strip.folded(id))
        return;
    const bool plugin = !mixCard_ && data->plugin;
    const auto safe = SafePointer<DevicePanelView>(this);
    if (configuring) {
        if (plugin) {
            closeConfigure();
            if (safe) strip.resized();
        } else {
            showParameterList();
        }
        return;
    }

    if (plugin && data->available) {
        const auto expected = *data;
        if (const auto poll = strip.pollParameterEdits) poll();
        if (!safe || safe->configuring || !safe->panelTargetCurrent(expected) || !safe)
            return;
        const auto feed = strip.latestPublication();
        if (!safe) return;
        const auto *latest = findPanelDevice(feed, expected);
        if (!latest || !latest->available || !latest->plugin) return;
        const auto &touches = latest->recentPluginTouches;
        const auto observed = touches && !touches->empty() ? touches->back().sequence : 0;
        configureSession_ = ConfigureSession{expected, observed, {}};
    }
    configuring = true;
    configureButton.setToggleState(true, juce::dontSendNotification);
    resized();
    if (!safe) return;
    strip.resized();
    if (safe && configureSession_) {
        const auto epoch = menuEpoch_;
        const bool opened = strip.root.openPluginWindow(id);
        if (opened && safe && safe->configureSession_ && safe->menuEpoch_ == epoch)
            safe->configureSession_->openedEditor = true;
    }
}
bool DevicePanelView::deleteConfigureParameter(std::string parameter) {
    if (mixCard_ || !configuring || !data || !data->plugin)
        return false;
    if (menuInteraction_)
        return true;
    const auto safe = SafePointer<DevicePanelView>(this);
    const auto expected = *data;
    const auto epoch = ++menuEpoch_;
    if (!panelTargetCurrent(expected) || !safe || safe->menuEpoch_ != epoch)
        return true;
    const auto feed = strip.latestPublication();
    if (!safe || safe->menuEpoch_ != epoch)
        return true;
    const auto *current = findPanelDevice(feed, expected);
    if (!current || !samePanelLayout(current->resolved, expected.resolved))
        return true;
    auto ids = panelParameterIds(current->resolved);
    const auto found = std::find(ids.begin(), ids.end(), parameter);
    if (found != ids.end()) {
        ids.erase(found);
        safe->changeParameterPanel(*current, epoch, std::move(ids));
    }
    return true;
}
void DevicePanelView::showParameterList() {
    const auto safe = SafePointer<DevicePanelView>(this);
    if (!data || !configuring || menuInteraction_ || !validTarget() || !safe) return;

    endInteraction(Interaction::Text);
    if (!safe || !data || !configuring || !beginInteraction(Interaction::Menu) || !safe) return;
    juce::PopupMenu m;
    m.addItem(1, "Use default panel");
    m.addItem(2, "Clear panel");
    m.addItem(3, "Done");
    const auto choices = panel::search(data->declarations(), search.getText().toStdString());
    std::vector<std::string> candidates;
    for (auto match : choices) {
        const auto &parameter = data->declarations()[match.index];
        if (!parameter.modifiable)
            continue;
        candidates.push_back(parameter.id);
        m.addItem(static_cast<int>(candidates.size()) + 3, parameter.name, true,
                  showsParameter(*data, parameter.id));
    }
    const auto epoch = menuEpoch_;
    const auto expected = *data;
    auto choose = [safe, expected, candidates = std::move(candidates), epoch](int chosen) {
        if (!safe || safe->menuEpoch_ != epoch || !safe->menuInteraction_) return;
        safe->endInteraction(Interaction::Menu);
        if (!safe || chosen == 0 || safe->menuEpoch_ != epoch || !safe->panelTargetCurrent(expected) || !safe)
            return;
        if (chosen == 3) {
            safe->closeConfigure();
            if (safe) safe->strip.resized();
            return;
        }
        const auto feed = safe->strip.latestPublication();
        if (!safe || safe->menuEpoch_ != epoch) return;
        const auto *current = findPanelDevice(feed, expected);
        if (!current || (expected.plugin && !samePanelLayout(current->resolved, expected.resolved)))
            return;
        if (chosen == 1) {
            safe->changeParameterPanel(*current, epoch, std::nullopt);
            return;
        }
        std::vector<std::string> ids;
        if (chosen != 2) {
            const auto index = static_cast<std::size_t>(chosen - 4);
            if (index >= candidates.size() || !configurableParameter(*current, candidates[index]))
                return;
            const auto &paramId = candidates[index];
            ids = panelParameterIds(current->resolved);
            if (const auto found = std::find(ids.begin(), ids.end(), paramId); found != ids.end())
                ids.erase(found);
            else
                ids.push_back(paramId);
        }
        safe->changeParameterPanel(*current, epoch, std::move(ids));
    };
    if (onParameterListMenu) {
        onParameterListMenu(std::move(m), std::move(choose));
    } else {
        auto &button = parameterList.isVisible() ? parameterList : configureButton;
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&button), std::move(choose));
    }
}
void DevicePanelView::changeParameterPanel(const DevicePanelData &current, std::uint64_t epoch,
                                          std::optional<std::vector<std::string>> params) {
    const auto safe = SafePointer<DevicePanelView>(this);
    const Payload edit{{"dev", current.id}, {"params", params ? Payload(*params) : Payload(nullptr)}};
    if (!current.plugin) {
        strip.action("device.setPanel", edit);
        return;
    }
    const auto before = panelParameterIds(current.resolved);
    // An explicit empty panel persists even when the automatic panel is already empty.
    if (params ? current.resolved.configured && *params == before : !current.resolved.configured)
        return;
    const auto after = params ? *params : panelParameterIds(panel::resolve({}, current.id, current.declarations()));
    const auto hides = [&](const auto &parameter) {
        return std::find(after.begin(), after.end(), parameter) == after.end();
    };
    const bool reduces = std::any_of(before.begin(), before.end(), hides);
    if (reduces) {
        // ADR-0203 d8: a hosted plug-in needs its own window to remove a panel control.
        if (configureSession_ && strip.root.pluginWindowOpen(current.id))
            configureSession_->openedEditor = true;
        if (!configureSession_ || !configureSession_->openedEditor) {
            strip.root.say("This plug-in needs its own editor to remove panel controls.");
            return;
        }
    }
    const auto snapshot = reduces ? strip.root.current() : nullptr;
    const bool hidesAutomation = snapshot && std::any_of(before.begin(), before.end(), [&](const auto &parameter) {
        return hides(parameter) && hasAuthoredParameterAutomation(*snapshot, current, parameter);
    });
    if (!hidesAutomation) {
        strip.action("device.setPanel", edit);
        return;
    }

    // AWAITING #11: the provisional Configure choice hides controls and keeps their automation.
    const auto options = juce::MessageBoxOptions{}
                             .withIconType(juce::MessageBoxIconType::WarningIcon)
                             .withTitle("Hide automated controls?")
                             .withMessage("This will hide controls with automation. Their automation will stay in the project.")
                             .withButton(juce::MessageBoxButtonOptions("Cancel", 0)
                                             .withRole(juce::MessageBoxButtonRole::cancel)
                                             .withPreferredAction())
                             .withButton("Hide Controls", 1)
                             .withAssociatedComponent(this);
    auto answer = [safe, expected = current, epoch, edit, after](bool confirm) {
        if (!safe || safe->menuEpoch_ != epoch)
            return;
        const auto answeredEpoch = ++safe->menuEpoch_;
        if (!confirm || !safe->panelTargetCurrent(expected) || !safe)
            return;
        const auto feed = safe->strip.latestPublication();
        if (!safe || safe->menuEpoch_ != answeredEpoch) return;
        const auto *latest = findPanelDevice(feed, expected);
        if (!latest || !samePanelLayout(latest->resolved, expected.resolved))
            return;
        if (edit.at("params").is_null() &&
            panelParameterIds(panel::resolve({}, latest->id, latest->declarations())) != after)
            return;
        safe->strip.action("device.setPanel", edit);
    };
    if (onPanelRemovalWarning) {
        onPanelRemovalWarning(options, std::move(answer));
    } else {
        panelRemovalWarning_ = juce::AlertWindow::showScopedAsync(options, [answer](int result) {
            answer(result == 1);
        });
    }
}
struct DeviceChainStrip::OutputMeter {
    engine::ScopeSource source;
    std::uint64_t instance = 0;
    SharedAnalysis::Lease lease;
    SharedAnalysis::Lease::SourcePosition first;
    std::uint64_t revision = 0;
    std::int64_t lastUpdate = 0;
    std::array<int, 2> levels{};
    std::array<bool, 2> clipping{};
};
DeviceChainStrip::DeviceChainStrip(AdiRootComponent &r) : root(r) {
    addAndMakeVisible(scroll);
    scroll.setViewedComponent(&content, false);
    // Devices run left to right; the strip scrolls sideways, never down, and shows no
    // scroll bar (Live's Device View).
    scroll.setScrollBarsShown(false, false, false, true);
    // The strip takes focus at the chain's end, where a paste goes (manual 23.2.1).
    setWantsKeyboardFocus(true);
    content.addMouseListener(this, false);
    content.addChildComponent(addTab);
    addTab.setTitle("Add Mix Function");
    addTab.setTooltip("Add one native Mix View function to this channel. Nothing is enabled by default.");
    addTab.onClick = [this] {
        functionMenu().showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&addTab));
    };
}
void DeviceChainStrip::mouseDown(const juce::MouseEvent &e) {
    if (e.eventComponent != &content || e.x < end_ - 11)
        return;
    const auto safe = SafePointer<DeviceChainStrip>(this);
    const auto track = selectedTrack;
    const auto epoch = addMenuEpoch_;
    const bool popup = e.mods.isPopupMenu();
    const auto where = e.getScreenPosition();
    chooseEnd();
    if (!safe || !popup)
        return;

    juce::PopupMenu menu;
    juce::PopupMenu::Item item("Paste");
    item.isEnabled = track > 0 && paste && holdsCopy && holdsCopy();
    item.shortcutKeyDescription =
        juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons();
    item.action = [safe, track, epoch] {
        if (safe && track > 0 && safe->selectedTrack == track && safe->addMenuEpoch_ == epoch &&
            safe->root.arrangement.geometry.selection.track == track)
            safe->pasteAt(track, std::nullopt);
    };
    menu.addItem(std::move(item));
    if (onEndMenu)
        onEndMenu(std::move(menu));
    else
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({where.x, where.y, 1, 1}));
}
bool DeviceChainStrip::keyPressed(const juce::KeyPress &key) {
    if (key != juce::KeyPress::leftKey || !endChosen() || panels.empty())
        return false;
    focusIfShowing(*panels.back());
    return true;
}
void DeviceChainStrip::chooseEnd() {
    chosen_.reset();
    clearSelection();
    const auto view = scroll.getViewArea();
    if (end_ > view.getRight())
        scroll.setViewPosition(end_ - view.getWidth(), view.getY());
    focusIfShowing(*this);
}
std::optional<std::int64_t> DeviceChainStrip::insertPoint() const {
    if (const auto focused = focusedDevice())
        return focused;
    if (endChosen() || !chosen_)
        return std::nullopt;
    for (const auto &p : panels)
        if (p->id == *chosen_)
            return chosen_;
    return std::nullopt;
}
bool DeviceChainStrip::isInterestedInDragSource(const SourceDetails &details) {
    if (details.description.toString() == "adi-mix-function") {
        const auto *source = dynamic_cast<DevicePanelView *>(details.sourceComponent.get());
        return source && source->functionDragTarget() && &source->strip.root == &root &&
               source->functionDragTarget()->track == selectedTrack;
    }
    return isMovingDevice(details) || (selectedTrack != 0 && root.browser.acceptsDeviceDrop(details)) ||
           isInterestedInFileDrag(BrowserPanel::droppedFiles(details.description));
}
bool DeviceChainStrip::acceptsSample() const {
    return panels.empty() && root.sampleInstrumentTarget(selectedTrack);
}
bool DeviceChainStrip::isInterestedInFileDrag(const juce::StringArray &files) {
    return !files.isEmpty() && acceptsSample();
}
void DeviceChainStrip::fileDragEnter(const juce::StringArray &files, int x, int) {
    setDropPreview(files.size() == 1 && acceptsSample() && root.loadSampleInstrument ? std::optional{x} : std::nullopt);
}
void DeviceChainStrip::fileDragExit(const juce::StringArray &) { setDropPreview({}); }
void DeviceChainStrip::filesDropped(const juce::StringArray &files, int, int) {
    setDropPreview({});
    if (!isInterestedInFileDrag(files))
        return;
    if (files.size() != 1) {
        root.say("Drop one sample at a time to make an instrument.");
        return;
    }
    root.loadSampleAsInstrument(selectedTrack, media::pathFromUtf8(files[0].toStdString()));
}
std::optional<std::int64_t> DeviceChainStrip::dropBefore(int x) const {
    const int at = x + scroll.getViewPositionX();
    for (const auto &p : panels)
        if (at < p->getBounds().getCentreX())
            return p->id;
    return std::nullopt;
}
void DeviceChainStrip::setDropPreview(std::optional<int> x) {
    if (dropPreviewX_ == x)
        return;
    dropPreviewX_ = x;
    repaint();
}
void DeviceChainStrip::itemDragEnter(const SourceDetails &details) { itemDragMove(details); }
void DeviceChainStrip::itemDragMove(const SourceDetails &details) {
    if (const auto files = BrowserPanel::droppedFiles(details.description); !files.isEmpty()) {
        fileDragEnter(files, details.localPosition.x, details.localPosition.y);
        return;
    }
    setDropPreview(isInterestedInDragSource(details) ? std::optional{details.localPosition.x} : std::nullopt);
}
void DeviceChainStrip::itemDragExit(const SourceDetails &) { setDropPreview({}); }
void DeviceChainStrip::itemDropped(const SourceDetails &details) {
    setDropPreview({});
    if (details.description.toString() == "adi-mix-function") {
        if (auto *source = dynamic_cast<DevicePanelView *>(details.sourceComponent.get()))
            dropFunction(*source, dropBefore(details.localPosition.x));
        return;
    }
    if (details.description.toString() == DevicePanelView::kDeviceMoveDrag) {
        moveDroppedDevice(details, selectedTrack, dropBefore(details.localPosition.x));
        return;
    }
    if (selectedTrack != 0 && root.browser.acceptsDeviceDrop(details)) {
        root.browser.loadDroppedDevice(details, selectedTrack, dropBefore(details.localPosition.x));
        return;
    }
    filesDropped(BrowserPanel::droppedFiles(details.description), details.localPosition.x, details.localPosition.y);
}
DevicePanelView *DeviceChainStrip::moveSource(const SourceDetails &details) const {
    if (details.description.toString() != DevicePanelView::kDeviceMoveDrag)
        return nullptr;
    auto *source = dynamic_cast<DevicePanelView *>(details.sourceComponent.get());
    if (!source || &source->strip.root != &root || source->nativeFunction() || source->draggedDevices_.empty())
        return nullptr;
    return source;
}
bool DeviceChainStrip::isMovingDevice(const SourceDetails &details) const {
    return moveSource(details) != nullptr;
}
bool DeviceChainStrip::moveDroppedDevice(const SourceDetails &details, std::int64_t track,
                                        std::optional<std::int64_t> before) {
    auto *source = moveSource(details);
    if (!source) {
        root.say("That device changed before it could be moved.");
        return false;
    }
    auto expected = std::exchange(source->draggedDevices_, {});
    if (expected.size() > 1 && moveSelection)
        return moveSelection(expected, track, before);
    if (expected.size() == 1 && moveDevice)
        return moveDevice(expected.front(), track, before);
    root.say("Devices move in the application.");
    return false;
}
bool DeviceChainStrip::hasFunction(mix::Function kind) const {
    if (presented) for (const auto &d : *presented)
        if (d.track == selectedTrack && !d.plugin)
            if (const auto *info = mix::functionForUid(d.uid); info && info->kind == kind) return true;
    return false;
}
juce::PopupMenu DeviceChainStrip::functionMenu() {
    const auto safe = SafePointer<DeviceChainStrip>(this);
    const auto track = selectedTrack;
    const auto epoch = ++addMenuEpoch_;
    return functionMenuFor(track, [safe, track, epoch] {
        return safe && safe->addMenuEpoch_ == epoch && safe->selectedTrack == track &&
               safe->isShowing();
    });
}
juce::PopupMenu DeviceChainStrip::functionMenuFor(std::int64_t track,
                                                  std::function<bool()> ownerCurrent) {
    juce::PopupMenu menu;
    const auto safe = SafePointer<DeviceChainStrip>(this);
    const auto shown = publication ? publication() : presented;
    if (!safe)
        return menu;
    for (const auto &info : mix::functionCatalog()) {
        const auto present = [track, kind = info.kind](const auto &feed) {
            return feed && std::any_of(feed->begin(), feed->end(), [&](const auto &d) {
                       const auto *function = !d.plugin ? mix::functionForUid(d.uid) : nullptr;
                       return d.track == track && function && function->kind == kind;
                   });
        };
        const bool exists = present(shown);
        const auto availability = functionAvailability
                                      ? functionAvailability(info.kind)
                                      : mix::Availability{false, "Engine unavailable"};
        if (!safe)
            return menu;
        auto label = juce::String::fromUTF8(info.name.data(), static_cast<int>(info.name.size()));
        if (!availability.ready && !exists && !availability.reason.empty())
            label += " (" + juce::String(availability.reason) + ")";
        juce::PopupMenu::Item item(label);
        item.isTicked = exists;
        item.isEnabled = !exists && availability.ready && addFunction && ownerCurrent && track > 0;
        if (item.isEnabled) {
            item.action = [safe, track, ownerCurrent, present, kind = info.kind] {
                const auto current = [&] {
                    return safe && ownerCurrent() && safe && safe->addFunction && safe->functionAvailability;
                };
                const auto refuse = [&] { if (safe) safe->root.sayStaleAddMenu(); };
                if (!current()) { refuse(); return; }
                const auto latest = safe->publication ? safe->publication() : safe->presented;
                if (!safe || present(latest) || !current()) { refuse(); return; }
                const auto availability = safe->functionAvailability(kind);
                if (!safe || !availability.ready || !current()) { refuse(); return; }
                safe->addFunction(kind, track);
            };
        }
        menu.addItem(std::move(item));
    }
    return menu;
}
bool DeviceChainStrip::dropFunction(DevicePanelView &source, std::optional<std::int64_t> before) {
    const auto *expected = source.functionDragTarget();
    const auto track = selectedTrack;
    if (!expected || &source.strip.root != &root || expected->track != track)
        return false;
    const auto captured = *expected;
    const auto epoch = source.dragEpoch_;
    const auto origin = SafePointer<DevicePanelView>(&source);
    const auto destination = SafePointer<DeviceChainStrip>(this);
    const auto current = [&] {
        return destination && origin && destination->selectedTrack == track &&
               origin->dragEpoch_ == epoch && origin->functionDragTarget();
    };
    if (!source.validTarget() || !current() || !destination->validExpected(captured) || !current() ||
        !destination->moveFunction)
        return false;
    origin->dragTarget_.reset();
    return destination->moveFunction(captured, track, before);
}
void DeviceChainStrip::cancelFunctionInteractions() {
    ++addMenuEpoch_;
    for (auto &panel : panels) if (panel->nativeFunction()) panel->cancelInteractions();
}
DeviceChainStrip::~DeviceChainStrip() {
    cancelPendingUpdate();
    if (parameterDemand)
        parameterDemand(this, {});
}
std::shared_ptr<const ParameterPublication> DeviceChainStrip::latestPublication() const {
    return publication ? publication() : presented;
}
bool DeviceChainStrip::addLastTouchedParameter(const DevicePanelData &expected) {
    if (!expected.pluginTouch) return false;
    DevicePanelData identity;
    identity.id = expected.id;
    identity.track = expected.track;
    identity.chain = expected.chain;
    identity.instance = expected.instance;
    identity.runtime = expected.runtime;
    identity.pluginTouch = expected.pluginTouch;

    const auto safe = SafePointer<DeviceChainStrip>(this);
    if (!validExpected(identity) || !safe) return false;
    const auto touchedDevice = [&](const auto &feed) -> const DevicePanelData * {
        const auto *row = findPanelDevice(feed, identity);
        return row && row->available && row->plugin && row->pluginTouch == identity.pluginTouch ? row : nullptr;
    };
    const auto &paramId = identity.pluginTouch->parameter;
    const auto feed = latestPublication();
    if (!safe) return false;
    const auto *current = touchedDevice(feed);
    if (!current) return false;
    std::shared_ptr<const ParameterPublication> described;
    if (!declaredParameter(*current, paramId) && root.automationParameters) {
        // Read descriptors without opening a live parameter subscription.
        const auto describe = root.automationParameters;
        const auto generation = root.projectViewGeneration();
        described = describe(identity.track);
        if (!safe || !safe->validExpected(identity) || !safe) return false;
        const auto latest = safe->latestPublication();
        if (!safe || generation != safe->root.projectViewGeneration() || !touchedDevice(latest)) return false;
        current = touchedDevice(described);
        if (!current) return false;
    }
    const auto *parameter = declaredParameter(*current, paramId);
    if (!parameter) return false;
    if (!parameter->modifiable) return deviceTarget(*current, *parameter).has_value();
    return addPanelParameters(*this, *current, {&paramId, 1});
}
void DeviceChainStrip::cancelConfigureTouches() {
    cancelPendingUpdate();
    const auto safe = SafePointer<DeviceChainStrip>(this);
    const auto epoch = frameEpoch_;
    for (std::size_t i = 0; i < panels.size(); ++i) {
        if (panels[i]->configureSession_) panels[i]->closeConfigure();
        if (!safe || frameEpoch_ != epoch) return;
    }
}
void DeviceChainStrip::handleAsyncUpdate() {
    const auto safe = SafePointer<DeviceChainStrip>(this);
    const auto epoch = frameEpoch_;
    for (std::size_t i = 0; i < panels.size(); ++i) {
        panels[i]->applyConfigureTouches();
        if (!safe) return;
        if (frameEpoch_ != epoch) {
            triggerAsyncUpdate();
            return;
        }
    }
}
void DeviceChainStrip::updateParameterDemand(std::vector<std::int64_t> devices) {
    std::sort(devices.begin(), devices.end());
    devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
    if (devices == demandedDevices_) return;
    demandedDevices_ = std::move(devices);
    if (parameterDemand) parameterDemand(this, demandedDevices_);
}
void DeviceChainStrip::suspendParameters(bool suspended) {
    parametersSuspended_ = suspended;
    if (suspended) {
        const auto safe = SafePointer<DeviceChainStrip>(this);
        cancelConfigureTouches();
        if (!safe) return;
        cancelFunctionInteractions();
        if (safe) releaseOutputMeters();
        if (safe) updateParameterDemand({});
    }
}
void DeviceChainStrip::visibilityChanged() {
    if (!isShowing()) {
        const auto safe = SafePointer<DeviceChainStrip>(this);
        cancelConfigureTouches();
        if (!safe) return;
        cancelFunctionInteractions();
        if (safe) releaseOutputMeters();
        if (safe) updateParameterDemand({});
    }
}
void DeviceChainStrip::parentHierarchyChanged() { visibilityChanged(); }
const DevicePanelData *DeviceChainStrip::dataOf(std::int64_t device) const {
    if (presented)
        for (const auto &d : *presented)
            if (d.id == device)
                return &d;
    return nullptr;
}
bool DeviceChainStrip::PickedDevice::matches(const DevicePanelData &data) const {
    return data.id == id && data.track == track && data.chain == chain && data.instance == instance;
}
bool DeviceChainStrip::isSelected(std::int64_t id) const {
    return std::any_of(selected_.begin(), selected_.end(), [id](const auto &device) { return device.id == id; });
}
bool DeviceChainStrip::selectionActsOn(std::int64_t id) const {
    return selected_.size() > 1 && isSelected(id);
}
void DeviceChainStrip::clearSelection() {
    if (selected_.empty()) return;
    selected_.clear();
    selectionAnchor_.reset();
    ++selectionEpoch_;
    for (const auto &p : panels) p->repaint();
}
void DeviceChainStrip::selectTitle(const DevicePanelView &panel, bool extend) {
    const auto *clicked = panel.data;
    if (!clicked) return;
    chose(clicked->id);
    auto first = std::find_if(panels.begin(), panels.end(), [&](const auto &p) { return p.get() == &panel; });
    if (first == panels.end()) return;
    auto last = first;
    if (extend && selectionAnchor_ && selectionAnchor_->chain == clicked->chain) {
        const auto anchor = std::find_if(panels.begin(), panels.end(), [&](const auto &p) {
            return p->data && selectionAnchor_->matches(*p->data);
        });
        if (anchor != panels.end()) {
            first = std::min(anchor, last);
            last = std::max(anchor, last);
        }
    }
    const PickedDevice target{clicked->id, clicked->track, clicked->chain, clicked->instance};
    if (first == last)
        selectionAnchor_ = target;
    selected_.clear();
    for (auto i = first; i <= last; ++i)
        if (const auto *data = (*i)->data; data && data->chain == clicked->chain)
            selected_.push_back({data->id, data->track, data->chain, data->instance});
    ++selectionEpoch_;
    for (const auto &p : panels) p->repaint();
}
std::vector<DevicePanelData> DeviceChainStrip::selectedTargets() const {
    std::vector<DevicePanelData> targets;
    targets.reserve(selected_.size());
    for (const auto &panel : panels) {
        const auto *data = panel->data;
        if (data && std::any_of(selected_.begin(), selected_.end(), [&](const auto &picked) { return picked.matches(*data); }))
            targets.push_back(*data);
    }
    if (targets.size() != selected_.size()) return {};
    return targets;
}
bool DeviceChainStrip::deleteDevices(std::optional<std::int64_t> device) {
    if (!device) device = focusedDevice();
    if (!device) return false;
    if (!selectionActsOn(*device)) return action("device.remove", {{"id", *device}});
    const auto targets = selectedTargets();
    return targets.size() > 1 && removeSelection && removeSelection(targets);
}
std::optional<std::int64_t> DeviceChainStrip::focusedDevice() const {
    // While its name is being edited the keys are the editor's: no device is chosen.
    for (const auto &p : panels)
        if (p->hasKeyboardFocus(true) && !p->rename.isVisible())
            return p->id;
    return std::nullopt;
}
DevicePanelView *DeviceChainStrip::focusedHotSwapPanel() const {
    auto *focused = juce::Component::getCurrentlyFocusedComponent();
    if (dynamic_cast<const juce::TextEditor *>(focused) != nullptr)
        return nullptr;
    // Ordinary Stack tiles share the strip's authority, including when Mix View is detached.
    for (auto *component = focused; component != nullptr; component = component->getParentComponent())
        if (auto *panel = dynamic_cast<DevicePanelView *>(component))
            return &panel->strip == this && !panel->rename.isVisible() && panel->canHotSwap() ? panel : nullptr;
    return nullptr;
}
bool DeviceChainStrip::hasHotSwapContext() const { return focusedHotSwapPanel() != nullptr; }
bool DeviceChainStrip::hotSwapFocused() {
    if (auto *panel = focusedHotSwapPanel()) {
        const auto slot = panel->sample && panel->sample->hasKeyboardFocus(true) ? std::optional{panel->sample->slot}
                                                                                : std::nullopt;
        return panel->requestHotSwap(*panel->data, slot, panel->menuEpoch_);
    }
    return false;
}
bool DeviceChainStrip::canClipboard(Clip c) const {
    const auto device = focusedDevice();
    if (c == Clip::Paste)
        return holdsCopy && holdsCopy() && (device || (endChosen() && selectedTrack >= 0));
    if (device && selectionActsOn(*device))
        return true;
    if (c == Clip::Duplicate && device)
        if (const auto *data = dataOf(*device); data && !data->plugin && mix::functionForUid(data->uid)) return false;
    return device.has_value();
}
bool DeviceChainStrip::clipboardSelection(Clip c) {
    const auto targets = selectedTargets();
    if (targets.size() < 2) return false;
    if (c == Clip::Cut || c == Clip::Copy)
        return copySelection && copySelection(targets, c == Clip::Cut);
    if (c != Clip::Duplicate || !duplicateSelection) return false;
    const auto &last = targets.back();
    const auto after = std::find_if(panels.begin(), panels.end(), [&](const auto &p) { return p->id == last.id; });
    if (after == panels.end()) return false;
    std::optional<std::int64_t> before;
    for (auto i = std::next(after); i != panels.end(); ++i)
        if ((*i)->data && (*i)->data->chain == last.chain) {
            before = (*i)->id;
            break;
        }
    return duplicateSelection(targets, last.track, before);
}
bool DeviceChainStrip::pasteAt(std::int64_t track, std::optional<std::int64_t> before) {
    if (!paste || !holdsCopy || !holdsCopy())
        return false;
    const SafePointer<DeviceChainStrip> alive(this);
    const bool pasted = paste(track, before);
    if (pasted && alive) root.say({});
    return pasted;
}
bool DeviceChainStrip::clipboard(Clip c, std::optional<std::int64_t> device, bool includeSelection) {
    includeSelection = includeSelection || !device;
    if (!device)
        device = focusedDevice();
    const bool useSelection = includeSelection && device && selectionActsOn(*device);
    const auto *data = device ? dataOf(*device) : nullptr;
    switch (c) {
    case Clip::Cut:
    case Clip::Copy: {
        const SafePointer<DeviceChainStrip> alive(this);
        const bool copied = useSelection ? clipboardSelection(c) : data && copy && copy(*device, c == Clip::Cut);
        if (copied && alive) {
            auto &commands = alive->root.commands();
            commands.clipboardWritten(AppCommands::ClipboardSource::Device);
            commands.manager.commandStatusChanged();
        }
        return copied;
    }
    case Clip::Paste:
        return data ? pasteAt(data->track, *device)
                    : endChosen() && selectedTrack >= 0 && pasteAt(selectedTrack, std::nullopt);
    case Clip::Duplicate: {
        if (useSelection)
            return clipboardSelection(c);
        if (data == nullptr || !duplicate || (!data->plugin && mix::functionForUid(data->uid)))
            return false;
        // In front of the next device in its chain, else at the chain's end.
        std::optional<std::int64_t> next;
        bool after = false;
        if (presented)
            for (const auto &d : *presented) {
                if (after && d.chain == data->chain) {
                    next = d.id;
                    break;
                }
                after = after || d.id == data->id;
            }
        return duplicate(*device, data->track, next);
    }
    }
    return false;
}
void DeviceChainStrip::showSamples() {
    for (auto &p : panels)
        if (p->sample) p->sample->refresh();
}
void DeviceChainStrip::setPlaybackProblemIds(std::vector<std::int64_t> ids) {
    if (playbackProblemIds_ == ids)
        return;
    playbackProblemIds_ = std::move(ids);
    for (const auto &panel : panels)
        panel->refreshPlaybackProblem();
}
bool DeviceChainStrip::hasPlaybackProblem(std::int64_t id) const noexcept {
    return std::binary_search(playbackProblemIds_.begin(), playbackProblemIds_.end(), id);
}
void DeviceChainStrip::frame(std::int64_t track) {
    const auto safe = SafePointer<DeviceChainStrip>(this);
    const auto epoch = ++frameEpoch_;
    const auto current = [&safe, epoch] { return safe && safe->frameEpoch_ == epoch; };
    if (parameterDemand && (parametersSuspended_ || !isShowing())) {
        releaseOutputMeters();
        if (current()) updateParameterDemand({});
        return;
    }
    // A wrench is lit while its window is open, however it closed (its own button, the host).
    for (auto &p : panels)
        if (p->wrench.isVisible() && p->wrench.getToggleState() != root.pluginWindowOpen(p->id))
            p->wrench.setToggleState(root.pluginWindowOpen(p->id), juce::dontSendNotification);
    showSamples();
    // Do not copy this std::function on every frame: a heap-stored target in
    // the standard library would allocate even for an unchanged
    // publication. No strip member is touched after the callback until checked.
    auto next = publication ? publication() : nullptr;
    if (!current()) return;
    if (parameterDemand) {
        std::vector<std::int64_t> demanded;
        if (next)
            for (const auto &device : *next)
                if (device.track == track && !folded(device.id)) {
                    const auto panel = std::find_if(panels.begin(), panels.end(), [&](const auto &candidate) {
                        return candidate->id == device.id;
                    });
                    if (track == selectedTrack && panel != panels.end() && !(*panel)->configureSession_ &&
                        !scroll.getViewArea().isEmpty() &&
                        !(*panel)->getBounds().intersects(scroll.getViewArea())) continue;
                    demanded.push_back(device.id);
                }
        updateParameterDemand(std::move(demanded));
        if (!current()) return;
        next = publication ? publication() : nullptr;
        if (!current()) return;
    }
    if (next == presented && track == selectedTrack) {
        frameOutputMeters();
        return;
    }
    if (track != selectedTrack) {
        ++addMenuEpoch_;
        chosen_.reset();
        clearSelection();
    }
    selectedTrack = track;
    // Keep the old publication alive until every control has adopted the new one.
    std::vector<const DevicePanelData *> visible;
    if (next)
        for (const auto &d : *next)
            if (d.track == track)
                visible.push_back(&d);
    // A selected ID reused for another runtime must never join a later edit.
    if (std::any_of(selected_.begin(), selected_.end(), [&](const auto &picked) {
            return std::none_of(visible.begin(), visible.end(), [&](const auto *d) { return picked.matches(*d); });
        }))
        clearSelection();
    bool same = panels.size() == visible.size();
    if (same)
        for (std::size_t i = 0; i < panels.size(); ++i)
            if (panels[i]->id != visible[i]->id ||
                (panels[i]->nativeFunction() && panels[i]->presentedInstance() != visible[i]->instance)) {
                same = false;
                break;
            }
    if (!same) {
        // End can synchronously close this root or reconcile a newer publication.
        // Keep panels under strip ownership during cancellation: if the strip
        // dies, its normal teardown owns every panel and no local survivor has
        // a dangling strip reference.
        for (std::size_t i = 0; i < panels.size(); ++i) {
            panels[i]->cancelInteractions();
            if (!current()) return;
        }
        // A native EQ's chosen band/section belongs to its stable authored runtime,
        // not its chain ordinal. Reuse that presentation when adjacent devices move.
        // Ordinary plug-in rebuilding retains its existing behavior.
        auto previous = std::move(panels);
        panels.clear();
        for (auto *d : visible) {
            const auto retained = std::find_if(previous.begin(), previous.end(), [&](const auto &p) {
                return p && p->eqPresentation() && p->id == d->id && p->presentedInstance() == d->instance &&
                    p->data && p->data->track == d->track && p->data->chain == d->chain &&
                    p->data->uid == d->uid && !d->plugin;
            });
            auto p = retained == previous.end() ? std::make_unique<DevicePanelView>(*this, *d) : std::move(*retained);
            if (p->nativeFunction()) {
                const auto expected = *d;
                p->targetValid = [this, expected] { return validExpected(expected); };
                p->parameterGesture = [this, expected](const std::string &parameter, engine::ParamEventKind kind, double value) {
                    return editExpected(expected, parameter, kind, value);
                };
            }
            content.addAndMakeVisible(*p);
            panels.push_back(std::move(p));
        }
    }
    for (std::size_t i = 0; i < panels.size(); ++i) {
        panels[i]->present(*visible[i]);
        if (!current()) return;
    }
    presented = std::move(next);
    addTab.setVisible(addFunction && selectedTrack > 0);
    // The panels just made show their sample slots in this frame, not the next.
    showSamples();
    resized();
    repaint();
    frameOutputMeters();
}
juce::Rectangle<int> DeviceChainStrip::outputMeterBounds(std::size_t gap) const noexcept {
    return {panels[gap + 1]->getX() - scroll.getViewPositionX() - 8, getHeight() / 2 - 12, 5, 24};
}
void DeviceChainStrip::releaseOutputMeters() {
    if (outputMeters_.empty())
        return;
    auto released = std::move(outputMeters_);
    outputMeterBroker_.reset();
    repaint();
}
void DeviceChainStrip::frameOutputMeters() {
    const auto broker = root.sharedAnalysis;
    if (!broker || parametersSuspended_ || !isShowing() || panels.size() < 2) {
        releaseOutputMeters();
        return;
    }
    if (outputMeterBroker_.lock() != broker) {
        releaseOutputMeters();
        outputMeterBroker_ = broker;
    }
    outputMeters_.resize(panels.size() - 1);
    bool active = false;
    for (std::size_t gap = 0; gap < outputMeters_.size(); ++gap) {
        auto &meter = outputMeters_[gap];
        const auto bounds = outputMeterBounds(gap);
        const auto *before = panels[gap]->data;
        const auto *after = panels[gap + 1]->data;
        const auto previousLevels = meter.levels;
        const auto previousClipping = meter.clipping;
        if (!getLocalBounds().intersects(bounds) || !before || !before->available || !after ||
            before->chain != after->chain) {
            meter = {};
        } else {
            const engine::ScopeSource source{before->track, engine::ScopePoint::DeviceOutput, before->id};
            if (meter.source != source || meter.instance != before->instance) {
                meter = {};
                meter.source = source;
                meter.instance = before->instance;
            }
            if (!meter.lease) {
                SharedAnalysis::Request request{source.track, source.point};
                request.device = source.device;
                request.outputs = SharedAnalysis::Meters;
                request.hopSize = request.fftSize;
                try { meter.lease = broker->acquire(request); }
                catch (...) {}
                meter.first = meter.lease.sourcePosition();
            }
            active = active || static_cast<bool>(meter.lease);
            const auto now = root.input.ms();
            const auto result = meter.lease.read();
            if (result && meter.first.generation != result->generation)
                meter.first = {result->generation, 0};
            if (result && meter.revision != result->revision &&
                result->firstSequence + result->request.fftSize > meter.first.written) {
                meter.revision = result->revision;
                meter.lastUpdate = now;
                for (std::size_t channel = 0; channel < meter.levels.size(); ++channel) {
                    const auto peak = result->peak[channel];
                    const auto db = 20. * std::log10(std::max(peak, 1.e-6f));
                    meter.levels[channel] = juce::roundToInt(bounds.getHeight() * (1. - meterDepth(db)));
                    meter.clipping[channel] = peak >= 1.f;
                }
            }
            // A stopped audio device cannot publish the silence that a running one would send.
            constexpr auto staleMilliseconds = 250;
            if (!result || now < meter.lastUpdate || now - meter.lastUpdate >= staleMilliseconds) {
                meter.levels = {};
                meter.clipping = {};
            }
        }
        if (meter.levels != previousLevels || meter.clipping != previousClipping)
            repaint(bounds);
    }
    if (active)
        broker->requestFrame();
}
void DeviceChainStrip::resized() {
    scroll.setBounds(getLocalBounds());
    const int h = getHeight();
    // Live's chain: devices 11 pt apart, a margin before the first.
    int x = 10;
    for (auto &p : panels) {
        const int width = p->widthFor(h);
        p->setBounds(x, 0, width, h);
        x += width + 11;
    }
    end_ = x;
    addTab.setBounds(x + 2, 2, 28, 22);
    content.setSize(std::max(x + (addTab.isVisible() ? 34 : 0), getWidth()), h);
}
const char *DeviceChainStrip::dropInstruction() const {
    const auto *track = root.reader().findTrack(selectedTrack);
    if (track == nullptr)
        return "Select a Track to Add Devices";
    if (track->kind == "master")
        return "Drop Audio Effects on Main";
    if (track->kind == "vca")
        return "VCA Tracks Do Not Take Devices";
    if (!hasAudioMixer(*track, presented.get()))
        return panels.empty() ? "Drop a MIDI Effect, Instrument or Sample Here"
                              : "Drop a MIDI Effect or Instrument Here";
    return "Drop Audio Effects Here";
}
juce::Rectangle<int> DeviceChainStrip::dropInstructionArea() const noexcept {
    const int from = std::max(0, end_ + (addTab.isVisible() ? 36 : 0) - scroll.getViewPositionX());
    return {from, 0, std::max(0, getWidth() - from), getHeight()};
}
void DeviceChainStrip::paint(juce::Graphics &g) {
    g.fillAll(look::panel);
    // Each gap shows the two channels leaving the device on its left.
    for (std::size_t gap = 0; gap + 1 < panels.size(); ++gap) {
        const auto bounds = outputMeterBounds(gap);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto well = bounds.withX(bounds.getX() + static_cast<int>(channel) * 3).withWidth(2);
            g.setColour(look::display);
            g.fillRect(well);
            if (gap < outputMeters_.size()) {
                const auto &meter = outputMeters_[gap];
                g.setColour(meter.clipping[channel] ? look::record : look::meterLevel);
                g.fillRect(well.withTop(well.getBottom() - meter.levels[channel]));
            }
        }
    }
    g.setColour(look::text);
    g.setFont(look::font(11.5f));
    g.drawText(dropInstruction(), dropInstructionArea(), juce::Justification::centred);
    // The chosen end: where a paste goes, marked as Live marks an insert point.
    if (endChosen()) {
        g.setColour(look::accent);
        g.fillRect(std::max(0, end_ - scroll.getViewPositionX() - 7), 4, 2, std::max(0, getHeight() - 8));
    }
}
void DeviceChainStrip::paintOverChildren(juce::Graphics &g) {
    if (!dropPreviewX_)
        return;
    const auto before = dropBefore(*dropPreviewX_);
    int insertX = end_ - 7;
    if (before)
        for (const auto &device : panels)
            if (device->id == *before) {
                insertX = device->getX() - 7;
                break;
            }
    const int x = std::clamp(insertX - scroll.getViewPositionX(), 0, std::max(0, getWidth() - 2));
    g.setColour(look::dropTarget);
    g.fillRect(x, 4, 2, std::max(0, getHeight() - 8));
}
void DeviceChainStrip::fold(std::int64_t id) {
    for (auto &panel : panels)
        if (panel->id == id && (!panel->parameterPages.empty() ||
                               (panel->configuring && panel->data && panel->data->plugin)))
            panel->cancelInteractions();
    auto &set = root.state().foldedDevices;
    if (!set.erase(id))
        set.insert(id);
    root.persist();
    resized();
}
bool DeviceChainStrip::folded(std::int64_t id) const {
    return root.state().foldedDevices.contains(id);
}
bool DeviceChainStrip::edit(std::int64_t id, const std::string &p, engine::ParamEventKind k,
                            double v) {
    return gesture && gesture(id, p, k, v);
}
bool DeviceChainStrip::editPairExpected(const DevicePanelData &expected, const engine::ParameterPair &pair,
                                        engine::ParameterPairEvent event, std::shared_ptr<const gesture::OwnerToken> &owner) {
    using Event = engine::ParameterPairEvent;
    if ((event == Event::Begin || event == Event::Value) && !validExpected(expected)) return false;
    return guardedPairGesture && guardedPairGesture(expected.id, pair, event, expected.runtime, expected.instance, owner);
}
bool DeviceChainStrip::editExpected(const DevicePanelData &expected, const std::string &parameter,
                                   engine::ParamEventKind kind, double value) {
    // End must reach the document even after this publication has been superseded. It owns
    // the exact retained gesture and can cancel that owner without touching a new instance.
    if (kind == engine::ParamEventKind::End && guardedGesture)
        return guardedGesture(expected.id, parameter, kind, value, expected.runtime, expected.instance);
    const auto latest = publication ? publication() : nullptr;
    if (!latest) return false;
    const auto found = std::find_if(latest->begin(), latest->end(), [&](const auto &d) {
        return d.id == expected.id && d.instance == expected.instance && d.available;
    });
    if (found == latest->end()) return false;
    if (guardedGesture)
        return guardedGesture(expected.id, parameter, kind, value, expected.runtime, expected.instance);
    // Component/fake-host clients may not expose a Session token. Their publication stamp is
    // still checked; production always supplies guardedGesture and validates in the document.
    return edit(expected.id, parameter, kind, value);
}
bool DeviceChainStrip::validExpected(const DevicePanelData &expected) const {
    if (targetCurrent) return targetCurrent(expected.id, expected.runtime, expected.instance);
    const auto latest = publication ? publication() : nullptr;
    return latest && std::any_of(latest->begin(), latest->end(), [&](const auto &d) {
        return d.id == expected.id && d.instance == expected.instance;
    });
}
bool DeviceChainStrip::action(const std::string &op, Payload p) {
    return submit && submit(op, std::move(p));
}
bool DeviceChainStrip::setEnabledExpected(const DevicePanelData &expected, bool enabled) {
    const auto safe = SafePointer<DeviceChainStrip>(this);
    const auto id = expected.id;
    const bool removeNative = expected.enabled && !expected.plugin && mix::functionForUid(expected.uid);
    // Retain the click's operation and identity before revalidation can
    // replace the publication or destroy its panel.
    DevicePanelData identity;
    identity.id = id;
    identity.runtime = expected.runtime;
    identity.instance = expected.instance;
    if (!validExpected(identity) || !safe)
        return false;
    // Compatibility: older projects, control clients and Bounce can leave a
    // native function bypassed. Its first activation recovers it; deactivating
    // an active native function still means remove plus Undo.
    if (removeNative)
        return safe->action("device.remove", {{"id", id}});
    return safe->action("device.setEnabled", {{"id", id}, {"enabled", enabled}});
}
bool DeviceChainStrip::focusNext(std::int64_t from, bool forward) {
    for (std::size_t i = 0; i < panels.size(); ++i)
        if (panels[i]->id == from) {
            if (forward && i + 1 < panels.size())
                focusIfShowing(*panels[i + 1]);
            else if (forward)
                chooseEnd();
            else if (i > 0)
                focusIfShowing(*panels[i - 1]);
            return true;
        }
    return false;
}
DockResizer::DockResizer(AdiRootComponent &r) : root(r) {
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    setTitle("Device View height");
}
void DockResizer::mouseDown(const juce::MouseEvent &) { start = root.state().deviceHeight; }
void DockResizer::mouseDrag(const juce::MouseEvent &e) {
    root.resizeDevices(start - e.getDistanceFromDragStartY());
}
void DockResizer::mouseUp(const juce::MouseEvent &) { root.persist(); }
} // namespace adi::ui
