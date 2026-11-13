// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_devices.hpp"
#include "ui_shell.hpp"
#include "ui_browser.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
namespace adi::ui {
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
        if (!record)
            return;
        const double v =
            static_cast<double>(menu.getSelectedId() - 1) / std::max(1, record->stepCount - 1);
        gesture(engine::ParamEventKind::Begin, v);
        gesture(engine::ParamEventKind::Value, v);
        gesture(engine::ParamEventKind::End, v);
    };
    addChildComponent(slider);
    addChildComponent(toggle);
    addChildComponent(menu);
    present(r, defaultNormalized);
}
bool ParameterControl::gesture(engine::ParamEventKind kind, double v) {
    if (kind != engine::ParamEventKind::End && !isEnabled()) return false;
    return parameterGesture ? parameterGesture(parameter, kind, v) : strip.edit(device, parameter, kind, v);
}
bool ParameterControl::keyPressed(const juce::KeyPress &k) {
    // Live 12 manual 41.6: Return to Default is Delete. Backspace with it, as the device's own
    // Delete takes both. Handled here, a Delete on a clicked knob never removes the device.
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) {
        returnToDefault();
        return true; // A refused reset must not bubble into deleting the device or its track.
    }
    return false;
}
bool ParameterControl::returnToDefault() {
    if (!record || record->missing)
        return false;
    // One bounded gesture from the value playing to the plug-in's default: one undo step, its
    // opener in front when the parameter was never touched (ADR-0124 d4).
    if (!gesture(engine::ParamEventKind::Begin, record->playing))
        return false;
    gesture(engine::ParamEventKind::Value, defaultValue);
    return gesture(engine::ParamEventKind::End, defaultValue);
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
        for (std::int32_t i = 0; i < menuSteps_; ++i) {
            const auto at = static_cast<std::size_t>(i);
            menu.addItem(at < menuLabels_.size() && !menuLabels_[at].empty() ? juce::String(menuLabels_[at])
                                                                           : juce::String(i + 1), i + 1);
        }
    }
    if (!slider.isMouseButtonDown())
        slider.setValue(r.playing, juce::dontSendNotification);
    toggle.setToggleState(r.playing >= .5, juce::dontSendNotification);
    menu.setSelectedId(1 + static_cast<int>(std::round(r.playing * std::max(1, r.stepCount - 1))),
                       juce::dontSendNotification);
}
void ParameterControl::resized() {
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
SampleTile::SampleTile(DeviceChainStrip &s, std::int64_t d, std::int32_t sl, juce::String n)
    : device(d), slot(sl), slotName(std::move(n)), strip_(s) {
    setTitle(slotName + " sample");
    setTooltip("Drop a sample file here, from the desktop or the browser.");
    clear.getProperties().set("live.small", true);
    clear.setTooltip("Empties the " + slotName + " slot: one undo step.");
    clear.onClick = [this] {
        if (targetValid && !targetValid()) return;
        if (strip_.clearSample)
            strip_.clearSample(device, slot);
    };
    addChildComponent(clear);
}
void SampleTile::resized() { clear.setBounds(getWidth() - 40, getHeight() - 17, 36, 13); }
void SampleTile::show(const engine::SampleSlotNode *node) {
    const auto next = node == nullptr || node->empty ? State::Empty : node->missing ? State::Missing : State::Loaded;
    const auto problem = node != nullptr ? node->problem : std::string();
    if (next == state_ && problem == problem_)
        return;
    state_ = next;
    problem_ = problem;
    clear.setVisible(state_ != State::Empty);
    setTooltip(problem_.empty() ? "Drop a sample file here, from the desktop or the browser." : juce::String(problem_));
    repaint();
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
    if (state_ == State::Empty) {
        juce::Path dashed;
        juce::PathStrokeType(1.f).createDashedStroke(dashed, edge, dash, 2);
        g.fillPath(dashed);
    } else {
        g.strokePath(edge, juce::PathStrokeType(1.f));
    }
    // Light text on the dark display, as a device's display draws it.
    g.setColour(look::textOnBand);
    g.setFont(look::font(11.5f, true));
    g.drawText(slotName, 6, 3, getWidth() - 12, 14, juce::Justification::centredLeft);
    g.setFont(look::font(11.f));
    const char *what = state_ == State::Empty ? "Drop a sample" : state_ == State::Missing ? "File missing" : "Sample loaded";
    g.setColour(state_ == State::Empty     ? juce::Colour(0xff8a8a8a)
                : state_ == State::Missing ? juce::Colour(0xffe5352b)
                                           : look::textOnBand);
    g.drawFittedText(what, 6, 18, getWidth() - 12, std::max(14, getHeight() - 38), juce::Justification::centred, 2);
}
bool SampleTile::drop(const juce::String &path) {
    if (targetValid && !targetValid()) return false;
    if (path.isEmpty())
        return false;
    if (!strip_.assignSample) {
        strip_.root.say("Samples load in the application.");
        return false;
    }
    return strip_.assignSample(device, slot, path.toStdString());
}
void SampleTile::filesDropped(const juce::StringArray &files, int, int) {
    if (!files.isEmpty())
        drop(files[0]);
}
bool SampleTile::isInterestedInDragSource(const SourceDetails &details) {
    return !BrowserPanel::droppedFiles(details.description).isEmpty();
}
void SampleTile::itemDropped(const SourceDetails &details) {
    if (const auto files = BrowserPanel::droppedFiles(details.description); !files.isEmpty())
        drop(files[0]);
}
DevicePanelView::DevicePanelView(DeviceChainStrip &s, const DevicePanelData &d)
    : id(d.id), strip(s) {
    // The title is the bar's text, not a box (Live's device title).
    title.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    title.getProperties().set("live.left", true);
    configureButton.setColour(juce::TextButton::buttonColourId, look::button);
    configureButton.getProperties().set("live.small", true);
    setWantsKeyboardFocus(true);
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
    title.addMouseListener(this, false);
    title.onClick = [this] { grabKeyboardFocus(); };
    enabled.onClick = [this] {
        if (!validTarget()) return;
        if (nativeFunction()) {
            const auto safe = SafePointer<DevicePanelView>(this);
            cancelInteractions();
            if (safe && safe->validTarget()) safe->strip.action("device.remove", {{"id", safe->id}});
        } else strip.action("device.setEnabled", {{"id", id}, {"enabled", enabled.getToggleState()}});
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
    const auto safe = SafePointer<DevicePanelView>(this);
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
    return true;
}
const DevicePanelData *DevicePanelView::functionDragTarget() const noexcept {
    return dragTarget_ ? &*dragTarget_ : nullptr;
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
        if (!metadata.sectionId.empty() && std::none_of(sections.begin(), sections.end(), [&](const auto &section) {
                return section.first == metadata.sectionId;
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
    if (!parameterPages.empty() && !isShowing()) cancelInteractions();
}
void DevicePanelView::present(const DevicePanelData &d) {
    if (!presentParameterPages(d)) return;
    presentFunctionSelection(d);
    data = &d;
    auto hint = juce::String("Double-click to fold; right-click for device commands.\n") +
                      (d.available ? "Latency: " + juce::String(d.latency) + " samples."
                                   : juce::String("This device is unavailable."));
    if (nativeFunction()) hint += "\nNative Mix View function; drag the header to move it within this channel.";
    if (!d.processingIssue.empty()) {
        hint += "\nConfiguration failed: " + juce::String(d.processingIssue);
        hint += "\nReload the device to retry.";
    }
    if (title.getTooltip() != hint) title.setTooltip(hint);
    const auto activatorHint = juce::String(nativeFunction() ? "Remove this Mix View function. Undo restores its settings."
                                                          : "Switches the device on or off.");
    if (enabled.getTooltip() != activatorHint) enabled.setTooltip(activatorHint);
    const auto caption = d.processingIssue.empty() ? juce::String(d.name)
                                                  : "Configuration failed: " + juce::String(d.name);
    if (title.getButtonText() != caption) {
        title.setButtonText(caption);
    }
    enabled.setToggleState(d.enabled, juce::dontSendNotification);
    // Color Cab's Impulse slot, its one declared sample slot (ENGINE-SAMPLE-SLOTS.md).
    if (!sample && d.uid == "org.adidaw.builtin.color-cab") {
        sample = std::make_unique<SampleTile>(strip, id, 1, "Impulse");
        sample->targetValid = [this] { return validTarget(); };
        addAndMakeVisible(*sample);
        resized();
    }
    // Only a plug-in has a window of its own (parity-7.5 row 1).
    if (wrench.isVisible() != (d.plugin && !strip.folded(id))) {
        wrench.setVisible(d.plugin && !strip.folded(id));
        resized();
    }
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
    if (!parameterPages.empty()) resized();
}
int DevicePanelView::widthFor(int height) const noexcept {
    if (strip.folded(id))
        return kFoldedWidth;
    const int top = kTitleHeight + (configuring ? 26 : 2) + (parameterPages.empty() ? 0 : kPageHeight) +
                    (eqPresentation() && parameterPageUnavailableReason().empty() &&
                     (!functionBands_.empty() || !functionSections_.empty()) ? kPageHeight : 0);
    const int rows = std::max(1, (height - top - 2) / ParameterControl::kHeight);
    const auto count = static_cast<std::size_t>(std::count_if(controls.begin(), controls.end(), [&](const auto &c) { return parameterShown(c->parameter); }));
    const int columns = static_cast<int>((count + static_cast<std::size_t>(rows) - 1) /
                                         static_cast<std::size_t>(rows));
    // Wide enough for the title bar's switches, a name and Configure; and a sample tile beside.
    int pageWidth = 12;
    for (const auto &page : parameterPages) pageWidth += pageSelectorWidth(page) + (page.enableParameter.empty() ? 0 : 18) + 3;
    return std::max({eqPresentation() ? 270 : 150, pageWidth,
                     columns * ParameterControl::kWidth + 12 + (sample ? kTileWidth : 0)});
}
void DevicePanelView::resized() {
    const bool isFolded = strip.folded(id);
    enabled.setBounds(2, 1, 16, 15);
    title.setVisible(!isFolded);
    configureButton.setVisible(!isFolded && !eqPresentation());
    wrench.setVisible(!isFolded && data != nullptr && data->plugin);
    // Live's order (manual 23.3's figure): the activator, the fold switch, the wrench, the name.
    const int nameLeft = wrench.isVisible() ? 50 : 32;
    wrench.setBounds(32, 1, 16, 15);
    title.setBounds(nameLeft, 1, std::max(0, getWidth() - nameLeft - (eqPresentation() ? 4 : 58)), 15);
    configureButton.setBounds(getWidth() - 56, 2, 52, 13);
    if (sample) {
        sample->setVisible(!isFolded);
        const int tileTop = kTitleHeight + (configuring ? 26 : 4) + (parameterPages.empty() ? 0 : kPageHeight);
        sample->setBounds(getWidth() - kTileWidth, tileTop, kTileWidth - 6, std::min(84, std::max(0, getHeight() - tileTop - 4)));
    }
    search.setVisible(!isFolded && configuring);
    search.setBounds(4, kTitleHeight + 2, std::max(0, getWidth() - 8), 22);
    int top = kTitleHeight + (configuring ? 26 : 2);
    int pageX = 6;
    for (auto &page : parameterPages) {
        page.selector->setVisible(!isFolded);
        page.enabled->setVisible(!isFolded && !page.enableParameter.empty());
        if (!page.enableParameter.empty()) { page.enabled->setBounds(pageX, top + 2, 16, 16); pageX += 18; }
        const auto width = pageSelectorWidth(page);
        page.selector->setBounds(pageX, top + 1, width, kPageHeight - 3); pageX += width + 3;
    }
    if (!parameterPages.empty()) top += kPageHeight;
    const bool functionControls = !isFolded && eqPresentation() && parameterPageUnavailableReason().empty() &&
                                  (!functionBands_.empty() || !functionSections_.empty());
    functionBand.setVisible(functionControls && !functionBands_.empty());
    functionSection.setVisible(functionControls && !functionSections_.empty());
    const bool bandApplies = std::find(functionBandSections_.begin(), functionBandSections_.end(), std::string()) != functionBandSections_.end() ||
                            std::find(functionBandSections_.begin(), functionBandSections_.end(), selectedSection_) != functionBandSections_.end();
    functionBand.setEnabled(bandApplies && validTarget());
    functionSection.setEnabled(validTarget());
    functionBand.setBounds(6, top + 1, 98, kPageHeight - 3);
    functionSection.setBounds(108, top + 1, std::max(0, std::min(150, getWidth() - 114)), kPageHeight - 3);
    if (functionControls) top += kPageHeight;
    const int rows = std::max(1, (getHeight() - top - 2) / ParameterControl::kHeight);
    int visible = 0;
    for (auto &control : controls) {
        const bool shown = !isFolded && parameterShown(control->parameter);
        control->setVisible(shown);
        if (!shown) continue;
        const int column = visible / rows, row = visible++ % rows;
        control->setBounds(6 + column * ParameterControl::kWidth,
                               top + row * ParameterControl::kHeight, ParameterControl::kWidth - 4,
                               ParameterControl::kHeight);
    }
}
void DevicePanelView::paint(juce::Graphics &g) {
    // Live's device: grey, its title bar and a 2 pt edge lighter; blue-grey when the
    // device has the focus, as Live marks the selected device.
    const auto edge = nativeFunction() ? look::accent : hasKeyboardFocus(true) ? juce::Colour(0xffa9b8c5) : look::field;
    g.fillAll(look::panel);
    g.setColour(edge);
    g.fillRect(0, 0, getWidth(), kTitleHeight);
    g.drawRect(getLocalBounds(), 2);
    // Live's fold switch beside the activator.
    const auto fold = foldSwitch().toFloat();
    g.setColour(look::button);
    g.fillEllipse(fold);
    g.setColour(look::text);
    g.drawEllipse(fold.reduced(0.5f), 1.f);
    look::triangle(g, fold.reduced(3.f).translated(0.5f, 0),
                   strip.folded(id) ? 0.f : juce::MathConstants<float>::halfPi, look::text);
    if (strip.folded(id) && data) {
        // A folded device is a narrow bar with its name running up it, as Live's.
        g.setColour(look::text);
        g.setFont(look::font(11.5f));
        juce::Graphics::ScopedSaveState saved(g);
        g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, 0,
                                                       static_cast<float>(getHeight())));
        g.drawText(title.getButtonText(), kTitleHeight + 2, getHeight() + 4,
                   std::max(0, getHeight() - kTitleHeight - 4), kFoldedWidth - 8,
                   juce::Justification::centredLeft);
    } else if (data && !parameterPageUnavailableReason().empty()) {
        g.setColour(look::text);
        g.setFont(look::font(12.f));
        g.drawFittedText(juce::String(parameterPageUnavailableReason().data(), parameterPageUnavailableReason().size()),
                        10, kTitleHeight + kPageHeight + 8, std::max(0, getWidth() - 20),
                        std::max(0, getHeight() - kTitleHeight - kPageHeight - 16), juce::Justification::centred, 5);
    } else if (data && data->resolved.showsConfigureHint && !eqPresentation()) {
        g.setColour(look::text);
        g.drawFittedText("Choose parameters with Configure", 8, kTitleHeight + 6, getWidth() - 16,
                         36, juce::Justification::centred, 2);
    }
}
bool DevicePanelView::keyPressed(const juce::KeyPress &k) {
    if (!validTarget())
        return k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey;
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        return strip.action("device.remove", {{"id", id}});
    // Live moves between devices with the left and right arrows (§41.4.1.5).
    if (k == juce::KeyPress::leftKey || k == juce::KeyPress::rightKey)
        return navigate ? navigate(k == juce::KeyPress::rightKey) : strip.focusNext(id, k == juce::KeyPress::rightKey);
    return false;
}
void DevicePanelView::focusGained(FocusChangeType) {
    strip.chose(id);
    repaint();
}
void DevicePanelView::focusOfChildComponentChanged(FocusChangeType) {
    if (search.hasKeyboardFocus(false))
        beginInteraction(Interaction::Text);
    else if (!rename.isVisible())
        endInteraction(Interaction::Text);
    if (hasKeyboardFocus(true))
        strip.chose(id);
    repaint();
}
void DevicePanelView::mouseDown(const juce::MouseEvent &e) {
    dragTarget_.reset();
    if (!validTarget()) return;
    if (e.mods.isPopupMenu() && (e.eventComponent == &title || e.eventComponent == this))
        contextMenu(e.getScreenPosition());
    else if (e.eventComponent == this && foldSwitch().contains(e.getPosition()))
        strip.fold(id);
}
juce::Rectangle<int> DevicePanelView::foldSwitch() const noexcept { return {19, 3, 11, 11}; }
void DevicePanelView::mouseDrag(const juce::MouseEvent &e) {
    if (!nativeFunction() || dragTarget_ || e.getDistanceFromDragStart() < 4 || !e.mods.isLeftButtonDown() ||
        !(e.eventComponent == &title || (e.eventComponent == this && e.y < kTitleHeight))) return;
    auto *container = juce::DragAndDropContainer::findParentDragContainerFor(this);
    if (container && beginFunctionDrag())
        container->startDragging(juce::String("adi-mix-function"), this, juce::ScaledImage{}, true, nullptr, &e.source);
}
void DevicePanelView::mouseDoubleClick(const juce::MouseEvent &e) {
    if (!validTarget()) return;
    if (e.eventComponent == &title || e.y < kTitleHeight)
        strip.fold(id);
}
juce::PopupMenu DevicePanelView::commands() {
    // Live 12's order (manual 23.2.1, its figure of a device's menu): Cut, Copy and Duplicate; Rename;
    // Delete and Fold; then the device's own. Paste is not in it: the Edit menu's and Ctrl/Cmd+V.
    juce::PopupMenu m;
    const auto safe = SafePointer<DevicePanelView>(this);
    const auto epoch = menuEpoch_;
    using Clip = DeviceChainStrip::Clip;
    const auto clipItem = [&](const char *name, char key, Clip c) {
        juce::PopupMenu::Item item(name);
        item.isEnabled = c != Clip::Duplicate || !nativeFunction();
        item.shortcutKeyDescription =
            juce::KeyPress(key, juce::ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons();
        item.action = [safe, c, epoch] {
            if (!safe || safe->menuEpoch_ != epoch) return;
            if (safe) safe->endInteraction(Interaction::Menu);
            if (safe && safe->validTarget())
                safe->strip.clipboard(c, safe->id);
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
    m.addItem("Delete", [safe, epoch] {
        if (!safe || safe->menuEpoch_ != epoch) return;
        if (safe) safe->endInteraction(Interaction::Menu);
        if (safe && safe->validTarget())
            safe->strip.action("device.remove", {{"id", safe->id}});
    });
    m.addItem(strip.folded(id) ? "Unfold" : "Fold", [safe, epoch] {
        if (!safe || safe->menuEpoch_ != epoch) return;
        if (safe) safe->endInteraction(Interaction::Menu);
        if (safe && safe->validTarget())
            safe->strip.fold(safe->id);
    });
    if (!eqPresentation()) {
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
        rename.grabKeyboardFocus();
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
        grabKeyboardFocus();
    endInteraction(Interaction::Text);
}
void DevicePanelView::configure() {
    if (!data || !validTarget() || eqPresentation())
        return;
    if (!configuring) {
        // The first press shows the search; the menu lists what it finds.
        configuring = true;
        resized();
        strip.resized();
        return;
    }
    endInteraction(Interaction::Text);
    if (!beginInteraction(Interaction::Menu)) return;
    juce::PopupMenu m;
    m.addItem(1, "Use default panel");
    m.addItem(2, "Clear panel");
    m.addItem(3, "Done");
    auto choices = panel::search(data->declarations(), search.getText().toStdString());
    std::vector<std::string> ids;
    for (const auto &e : data->resolved.entries)
        ids.push_back(e.id);
    std::vector<std::string> candidates;
    for (auto match : choices) {
        const auto &d = data->declarations()[match.index];
        if (!d.modifiable)
            continue;
        candidates.push_back(d.id);
        m.addItem(static_cast<int>(candidates.size()) + 3, d.name, true,
                  std::find(ids.begin(), ids.end(), d.id) != ids.end());
    }
    auto safe = juce::Component::SafePointer<DevicePanelView>(this);
    const auto epoch = menuEpoch_;
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&configureButton),
                    [safe, ids, candidates, epoch](int chosen) mutable {
                        if (!safe || safe->menuEpoch_ != epoch) return;
                        if (safe) safe->endInteraction(Interaction::Menu);
                        if (!safe || !safe->validTarget() || chosen == 0)
                            return;
                        if (chosen == 3) {
                            safe->configuring = false;
                            safe->resized();
                            safe->strip.resized();
                            return;
                        }
                        if (chosen == 1) {
                            safe->strip.action("device.setPanel",
                                               {{"dev", safe->id}, {"params", nullptr}});
                            return;
                        }
                        if (chosen == 2)
                            ids.clear();
                        else {
                            const auto index = static_cast<std::size_t>(chosen - 4);
                            if (index >= candidates.size())
                                return;
                            const auto &chosenId = candidates[index];
                            auto it = std::find(ids.begin(), ids.end(), chosenId);
                            if (it == ids.end())
                                ids.push_back(chosenId);
                            else
                                ids.erase(it);
                        }
                        safe->strip.action("device.setPanel", {{"dev", safe->id}, {"params", ids}});
                    });
}
DeviceChainStrip::DeviceChainStrip(AdiRootComponent &r) : root(r) {
    addAndMakeVisible(scroll);
    scroll.setViewedComponent(&content, false);
    // Devices run left to right; the strip scrolls sideways, never down, and shows no
    // scroll bar (Live's Device View).
    scroll.setScrollBarsShown(false, false, false, true);
    // A click past the last device chooses the chain's end, where a paste goes (manual 23.2.1).
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
    if (e.eventComponent == &content && e.x >= end_ - 11 && !e.mods.isPopupMenu()) {
        chosen_.reset();
        grabKeyboardFocus();
    }
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
    return !BrowserPanel::droppedDevice(details.description).empty();
}
std::optional<std::int64_t> DeviceChainStrip::dropBefore(int x) const {
    const int at = x + scroll.getViewPositionX();
    for (const auto &p : panels)
        if (at < p->getBounds().getCentreX())
            return p->id;
    return std::nullopt;
}
void DeviceChainStrip::itemDropped(const SourceDetails &details) {
    if (details.description.toString() == "adi-mix-function") {
        if (auto *source = dynamic_cast<DevicePanelView *>(details.sourceComponent.get()))
            dropFunction(*source, dropBefore(details.localPosition.x));
        return;
    }
    const auto uid = BrowserPanel::droppedDevice(details.description);
    if (!uid.empty())
        root.insertDevice(uid, selectedTrack, dropBefore(details.localPosition.x));
}
bool DeviceChainStrip::hasFunction(mix::Function kind) const {
    if (presented) for (const auto &d : *presented)
        if (d.track == selectedTrack && !d.plugin)
            if (const auto *info = mix::functionForUid(d.uid); info && info->kind == kind) return true;
    return false;
}
juce::PopupMenu DeviceChainStrip::functionMenu() {
    juce::PopupMenu menu;
    const auto safe = SafePointer<DeviceChainStrip>(this);
    const auto track = selectedTrack;
    const auto epoch = ++addMenuEpoch_;
    for (const auto &info : mix::functionCatalog()) {
        const bool present = hasFunction(info.kind);
        const auto availability = functionAvailability ? functionAvailability(info.kind)
            : mix::Availability{false, "Engine unavailable"};
        auto label = juce::String::fromUTF8(info.name.data(), static_cast<int>(info.name.size()));
        if (!availability.ready && !present && !availability.reason.empty()) label += " (" + juce::String(availability.reason) + ")";
        juce::PopupMenu::Item item(label);
        item.isTicked = present;
        item.isEnabled = !present && availability.ready && addFunction && selectedTrack > 0;
        item.action = [safe, track, epoch, kind = info.kind] {
            if (!safe || safe->addMenuEpoch_ != epoch || safe->selectedTrack != track || !safe->isShowing() ||
                safe->hasFunction(kind) || !safe->addFunction || !safe->functionAvailability ||
                !safe->functionAvailability(kind).ready) return;
            safe->addFunction(kind, track);
        };
        menu.addItem(std::move(item));
    }
    return menu;
}
bool DeviceChainStrip::dropFunction(DevicePanelView &source, std::optional<std::int64_t> before) {
    const auto *expected = source.functionDragTarget();
    if (!expected || &source.strip.root != &root || expected->track != selectedTrack || !source.validTarget() ||
        !validExpected(*expected) || !moveFunction) return false;
    const auto captured = *expected;
    source.dragTarget_.reset();
    return moveFunction(captured, selectedTrack, before);
}
void DeviceChainStrip::cancelFunctionInteractions() {
    ++addMenuEpoch_;
    for (auto &panel : panels) if (panel->nativeFunction()) panel->cancelInteractions();
}
DeviceChainStrip::~DeviceChainStrip() { if (parameterDemand) parameterDemand(this, {}); }
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
        cancelFunctionInteractions();
        if (safe) updateParameterDemand({});
    }
}
void DeviceChainStrip::visibilityChanged() {
    if (!isShowing()) {
        const auto safe = SafePointer<DeviceChainStrip>(this);
        cancelFunctionInteractions();
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
std::optional<std::int64_t> DeviceChainStrip::focusedDevice() const {
    // While its name is being edited the keys are the editor's: no device is chosen.
    for (const auto &p : panels)
        if (p->hasKeyboardFocus(true) && !p->rename.isVisible())
            return p->id;
    return std::nullopt;
}
bool DeviceChainStrip::canClipboard(Clip c) const {
    const auto device = focusedDevice();
    if (c == Clip::Paste)
        return holdsCopy && holdsCopy() && (device || (endChosen() && selectedTrack >= 0));
    if (c == Clip::Duplicate && device)
        if (const auto *data = dataOf(*device); data && !data->plugin && mix::functionForUid(data->uid)) return false;
    return device.has_value();
}
bool DeviceChainStrip::clipboard(Clip c, std::optional<std::int64_t> device) {
    if (!device)
        device = focusedDevice();
    const auto *data = device ? dataOf(*device) : nullptr;
    switch (c) {
    case Clip::Cut:
    case Clip::Copy:
        return data != nullptr && copy && copy(*device, c == Clip::Cut);
    case Clip::Paste:
        if (!paste || !holdsCopy || !holdsCopy())
            return false;
        if (data != nullptr)
            return paste(data->track, *device);
        return endChosen() && selectedTrack >= 0 && paste(selectedTrack, std::nullopt);
    case Clip::Duplicate: {
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
    // A sample tile shows its slot as the snapshot holds it: empty, loaded or missing.
    const auto *snapshot = root.reader().get();
    if (snapshot == nullptr)
        return;
    for (auto &p : panels)
        if (p->sample) {
            const engine::SampleSlotNode *node = nullptr;
            for (const auto &slot : snapshot->sampleSlots)
                if (slot && slot->deviceId == p->id && slot->slot == p->sample->slot)
                    node = slot.get();
            p->sample->show(node);
        }
}
void DeviceChainStrip::frame(std::int64_t track) {
    const auto safe = SafePointer<DeviceChainStrip>(this);
    const auto epoch = ++frameEpoch_;
    const auto current = [&safe, epoch] { return safe && safe->frameEpoch_ == epoch; };
    if (parameterDemand && (parametersSuspended_ || !isShowing())) { updateParameterDemand({}); return; }
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
                    if (track == selectedTrack && panel != panels.end() && !scroll.getViewArea().isEmpty() &&
                        !(*panel)->getBounds().intersects(scroll.getViewArea())) continue;
                    demanded.push_back(device.id);
                }
        updateParameterDemand(std::move(demanded));
        if (!current()) return;
        next = publication ? publication() : nullptr;
        if (!current()) return;
    }
    if (next == presented && track == selectedTrack)
        return;
    if (track != selectedTrack) {
        ++addMenuEpoch_;
        chosen_.reset();
    }
    selectedTrack = track;
    // Keep the old publication alive until every control has adopted the new one.
    std::vector<const DevicePanelData *> visible;
    if (next)
        for (const auto &d : *next)
            if (d.track == track)
                visible.push_back(&d);
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
void DeviceChainStrip::paint(juce::Graphics &g) {
    g.fillAll(look::panel);
    // Between devices, Live's two dark marks; after the last, where the next one goes.
    g.setColour(look::display);
    for (std::size_t i = 1; i < panels.size(); ++i) {
        const int x = panels[i]->getX() - scroll.getViewPositionX() - 8;
        const int y = getHeight() / 2 - 12;
        g.fillRect(x, y, 2, 24);
        g.fillRect(x + 3, y, 2, 24);
    }
    const int from = std::max(0, end_ + (addTab.isVisible() ? 36 : 0) - scroll.getViewPositionX());
    g.setColour(look::text);
    g.setFont(look::font(11.5f));
    g.drawText("Drop Audio Effects Here", from, 0, std::max(0, getWidth() - from), getHeight(),
               juce::Justification::centred);
    // The chosen end: where a paste goes, marked as Live marks an insert point.
    if (endChosen()) {
        g.setColour(look::accent);
        g.fillRect(std::max(0, end_ - scroll.getViewPositionX() - 7), 4, 2, std::max(0, getHeight() - 8));
    }
}
void DeviceChainStrip::fold(std::int64_t id) {
    for (auto &panel : panels)
        if (panel->id == id && !panel->parameterPages.empty()) panel->cancelInteractions();
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
bool DeviceChainStrip::focusNext(std::int64_t from, bool forward) {
    for (std::size_t i = 0; i < panels.size(); ++i)
        if (panels[i]->id == from) {
            if (forward && i + 1 < panels.size())
                panels[i + 1]->grabKeyboardFocus();
            else if (!forward && i > 0)
                panels[i - 1]->grabKeyboardFocus();
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
