// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_devices.hpp"
#include "ui_shell.hpp"
namespace adi::ui {
ParameterControl::ParameterControl(DeviceChainStrip &s, std::int64_t d, const panel::Record &r)
    : device(d), parameter(r.id), strip(s) {
    addAndMakeVisible(name);
    addAndMakeVisible(value);
    name.setFont(juce::FontOptions(12.f));
    value.setFont(juce::FontOptions(11.f));
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setRange(0, 1, 0);
    slider.onDragStart = [this] { gesture(engine::ParamEventKind::Begin, slider.getValue()); };
    slider.onValueChange = [this] { gesture(engine::ParamEventKind::Value, slider.getValue()); };
    slider.onDragEnd = [this] { gesture(engine::ParamEventKind::End, slider.getValue()); };
    // Wheel/keyboard changes also need explicit bounded gestures.
    slider.setScrollWheelEnabled(false);
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
    for (int i = 0; r.shape == panel::Shape::Menu && i < std::max(1, r.stepCount); ++i)
        menu.addItem(juce::String(i + 1), i + 1);
    addChildComponent(slider);
    addChildComponent(toggle);
    addChildComponent(menu);
    present(r);
}
void ParameterControl::gesture(engine::ParamEventKind kind, double v) {
    strip.edit(device, parameter, kind, v);
}
void ParameterControl::present(const panel::Record &r) {
    record = &r;
    setEnabled(!r.missing);
    const juce::String label = r.name + (r.missing ? " (missing)" : "");
    if (name.getText() != label)
        name.setText(label, juce::dontSendNotification);
    const juce::String text = r.text.empty() ? juce::String(r.playing, 3) : juce::String(r.text);
    if (value.getText() != text)
        value.setText(text, juce::dontSendNotification);
    slider.setVisible(r.shape == panel::Shape::Continuous);
    toggle.setVisible(r.shape == panel::Shape::Switch);
    menu.setVisible(r.shape == panel::Shape::Menu);
    if (!slider.isMouseButtonDown())
        slider.setValue(r.playing, juce::dontSendNotification);
    toggle.setToggleState(r.playing >= .5, juce::dontSendNotification);
    menu.setSelectedId(1 + static_cast<int>(std::round(r.playing * std::max(1, r.stepCount - 1))),
                       juce::dontSendNotification);
}
void ParameterControl::resized() {
    name.setBounds(4, 0, getWidth() - 8, 20);
    slider.setBounds(4, 20, getWidth() - 8, 22);
    toggle.setBounds(4, 20, getWidth() - 8, 22);
    menu.setBounds(4, 20, getWidth() - 8, 22);
    value.setBounds(4, 42, getWidth() - 8, 18);
}
void ParameterControl::paint(juce::Graphics &g) {
    if (!record)
        return;
    g.setColour(record->overridden  ? juce::Colours::orange
                : record->automated ? juce::Colours::red
                                    : juce::Colours::grey);
    g.fillEllipse(static_cast<float>(getWidth() - 8), 3.f, 5.f, 5.f);
    if (record->driven) {
        g.setColour(juce::Colours::cyan);
        g.fillRect(4, 62, static_cast<int>((getWidth() - 8) * record->playing), 2);
    }
}
DevicePanelView::DevicePanelView(DeviceChainStrip &s, const DevicePanelData &d)
    : id(d.id), strip(s) {
    setWantsKeyboardFocus(true);
    addAndMakeVisible(title);
    addAndMakeVisible(enabled);
    addAndMakeVisible(configureButton);
    addAndMakeVisible(search);
    search.setTextToShowWhenEmpty("Find parameter", juce::Colours::grey);
    search.setTitle("Find parameter");
    title.addMouseListener(this, false);
    title.onClick = [this] { grabKeyboardFocus(); };
    enabled.onClick = [this] {
        strip.action("device.setEnabled", {{"id", id}, {"enabled", enabled.getToggleState()}});
    };
    configureButton.onClick = [this] { configure(); };
    present(d);
}
void DevicePanelView::present(const DevicePanelData &d) {
    data = &d;
    title.setButtonText(d.name);
    enabled.setToggleState(d.enabled, juce::dontSendNotification);
    bool same = controls.size() == d.resolved.entries.size();
    if (same)
        for (std::size_t i = 0; i < controls.size(); ++i)
            if (controls[i]->parameter != d.resolved.entries[i].id) {
                same = false;
                break;
            }
    if (!same) {
        controls.clear();
        for (const auto &e : d.resolved.entries)
            for (const auto &r : d.records)
                if (r.id == e.id) {
                    auto c = std::make_unique<ParameterControl>(strip, id, r);
                    addAndMakeVisible(*c);
                    controls.push_back(std::move(c));
                    break;
                }
        resized();
    }
    for (auto &c : controls)
        for (const auto &r : d.records)
            if (c->parameter == r.id) {
                c->present(r);
                break;
            }
}
void DevicePanelView::resized() {
    enabled.setBounds(2, 2, 24, 24);
    title.setBounds(28, 2, std::max(0, getWidth() - 32), 24);
    configureButton.setBounds(4, 29, std::max(0, getWidth() - 8), 24);
    search.setBounds(4, 56, std::max(0, getWidth() - 8), 24);
    search.setVisible(!strip.folded(id));
    int y = 84;
    for (auto &c : controls) {
        c->setVisible(!strip.folded(id));
        c->setBounds(4, y, std::max(0, getWidth() - 8), 66);
        y += 66;
    }
}
void DevicePanelView::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff2a303a));
    g.setColour(juce::Colour(0xff7e8c9f));
    g.drawRect(getLocalBounds());
    if (data && data->resolved.showsConfigureHint && !strip.folded(id)) {
        g.setColour(juce::Colours::white);
        g.drawFittedText("Choose parameters with Configure", 8, 86, getWidth() - 16, 48,
                         juce::Justification::centred, 3);
    }
}
bool DevicePanelView::keyPressed(const juce::KeyPress &k) {
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        return strip.action("device.remove", {{"id", id}});
    return false;
}
void DevicePanelView::mouseDoubleClick(const juce::MouseEvent &e) {
    if (e.eventComponent == &title)
        strip.fold(id);
}
void DevicePanelView::configure() {
    if (!data)
        return;
    juce::PopupMenu m;
    m.addItem(1, "Use default panel");
    m.addItem(2, "Clear panel");
    auto choices = panel::search(data->declared, search.getText().toStdString());
    std::vector<std::string> ids;
    for (const auto &e : data->resolved.entries)
        ids.push_back(e.id);
    std::vector<std::string> candidates;
    for (auto match : choices) {
        const auto &d = data->declared[match.index];
        if (!d.modifiable)
            continue;
        candidates.push_back(d.id);
        m.addItem(static_cast<int>(candidates.size()) + 2, d.name, true,
                  std::find(ids.begin(), ids.end(), d.id) != ids.end());
    }
    auto safe = juce::Component::SafePointer<DevicePanelView>(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&configureButton),
                    [safe, ids, candidates](int chosen) mutable {
                        if (!safe || chosen == 0)
                            return;
                        if (chosen == 1) {
                            safe->strip.action("device.setPanel",
                                               {{"dev", safe->id}, {"params", nullptr}});
                            return;
                        }
                        if (chosen == 2)
                            ids.clear();
                        else {
                            const auto index = static_cast<std::size_t>(chosen - 3);
                            if (index >= candidates.size())
                                return;
                            const auto &id = candidates[index];
                            auto it = std::find(ids.begin(), ids.end(), id);
                            if (it == ids.end())
                                ids.push_back(id);
                            else
                                ids.erase(it);
                        }
                        safe->strip.action("device.setPanel", {{"dev", safe->id}, {"params", ids}});
                    });
}
DeviceChainStrip::DeviceChainStrip(AdiRootComponent &r) : root(r) {
    addAndMakeVisible(scroll);
    scroll.setViewedComponent(&content, false);
    scroll.setScrollBarsShown(true, true);
}
void DeviceChainStrip::frame(std::int64_t track) {
    auto next = publication ? publication() : nullptr;
    if (next == presented && track == selectedTrack)
        return;
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
            if (panels[i]->id != visible[i]->id) {
                same = false;
                break;
            }
    if (!same) {
        panels.clear();
        for (auto *d : visible) {
            auto p = std::make_unique<DevicePanelView>(*this, *d);
            content.addAndMakeVisible(*p);
            panels.push_back(std::move(p));
        }
    }
    for (std::size_t i = 0; i < panels.size(); ++i)
        panels[i]->present(*visible[i]);
    presented = std::move(next);
    resized();
    repaint();
}
void DeviceChainStrip::resized() {
    scroll.setBounds(getLocalBounds());
    int x = 0, h = 80;
    for (auto &p : panels) {
        int width = folded(p->id) ? 100 : 220;
        int height = folded(p->id) ? 56 : 84 + static_cast<int>(p->controls.size()) * 66;
        p->setBounds(x, 0, width, height);
        x += width + 6;
        h = std::max(h, height);
    }
    content.setSize(std::max(x, getWidth() - 16), std::max(h, getHeight() - 16));
}
void DeviceChainStrip::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff20232b));
    if (panels.empty()) {
        g.setColour(juce::Colours::lightgrey);
        g.drawText("Devices — select a track", 12, 12, 250, 24, juce::Justification::centredLeft);
    }
}
void DeviceChainStrip::fold(std::int64_t id) {
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
bool DeviceChainStrip::action(const std::string &op, Payload p) {
    return submit && submit(op, std::move(p));
}
DockResizer::DockResizer(AdiRootComponent &r) : root(r) {
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}
void DockResizer::mouseDown(const juce::MouseEvent &) { start = root.state().deviceHeight; }
void DockResizer::mouseDrag(const juce::MouseEvent &e) {
    root.resizeDevices(start - e.getDistanceFromDragStartY());
}
void DockResizer::mouseUp(const juce::MouseEvent &) { root.persist(); }
} // namespace adi::ui
