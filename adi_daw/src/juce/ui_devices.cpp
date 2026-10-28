// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_devices.hpp"
#include "ui_shell.hpp"
namespace adi::ui {
ParameterControl::ParameterControl(DeviceChainStrip &s, std::int64_t d, const panel::Record &r)
    : device(d), parameter(r.id), strip(s) {
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
    // Automation's state, as Live marks it: red when automated, orange when overridden.
    if (record->overridden || record->automated) {
        g.setColour(record->overridden ? juce::Colours::orange : juce::Colours::red);
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
    addChildComponent(search);
    enabled.setTitle("Device Activator");
    enabled.setTooltip("Switches the device on or off.");
    configureButton.setTooltip("Choose which of the plug-in's parameters the panel shows.");
    search.setTextToShowWhenEmpty("Find parameter", juce::Colours::grey);
    search.setTitle("Find parameter");
    title.addMouseListener(this, false);
    title.onClick = [this] { grabKeyboardFocus(); };
    enabled.onClick = [this] {
        strip.action("device.setEnabled", {{"id", id}, {"enabled", enabled.getToggleState()}});
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
void DevicePanelView::present(const DevicePanelData &d) {
    data = &d;
    if (title.getButtonText() != juce::String(d.name)) {
        title.setButtonText(d.name);
        title.setTooltip("Double-click to fold the device; right-click for its commands.");
    }
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
int DevicePanelView::widthFor(int height) const noexcept {
    if (strip.folded(id))
        return kFoldedWidth;
    const int top = kTitleHeight + (configuring ? 26 : 2);
    const int rows = std::max(1, (height - top - 2) / ParameterControl::kHeight);
    const int columns = static_cast<int>((controls.size() + static_cast<std::size_t>(rows) - 1) /
                                         static_cast<std::size_t>(rows));
    // Wide enough for the title bar's switches, a name and Configure.
    return std::max(150, columns * ParameterControl::kWidth + 12);
}
void DevicePanelView::resized() {
    const bool isFolded = strip.folded(id);
    enabled.setBounds(2, 1, 16, 15);
    title.setVisible(!isFolded);
    configureButton.setVisible(!isFolded);
    title.setBounds(32, 1, std::max(0, getWidth() - 32 - 58), 15);
    configureButton.setBounds(getWidth() - 56, 2, 52, 13);
    search.setVisible(!isFolded && configuring);
    search.setBounds(4, kTitleHeight + 2, std::max(0, getWidth() - 8), 22);
    const int top = kTitleHeight + (configuring ? 26 : 2);
    const int rows = std::max(1, (getHeight() - top - 2) / ParameterControl::kHeight);
    for (std::size_t i = 0; i < controls.size(); ++i) {
        const int column = static_cast<int>(i) / rows, row = static_cast<int>(i) % rows;
        controls[i]->setVisible(!isFolded);
        controls[i]->setBounds(6 + column * ParameterControl::kWidth,
                               top + row * ParameterControl::kHeight, ParameterControl::kWidth - 4,
                               ParameterControl::kHeight);
    }
}
void DevicePanelView::paint(juce::Graphics &g) {
    // Live's device: grey, its title bar and a 2 pt edge lighter; blue-grey when the
    // device has the focus, as Live marks the selected device.
    const auto edge = hasKeyboardFocus(true) ? juce::Colour(0xffa9b8c5) : look::field;
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
        g.drawText(juce::String(data->name), kTitleHeight + 2, getHeight() + 4,
                   std::max(0, getHeight() - kTitleHeight - 4), kFoldedWidth - 8,
                   juce::Justification::centredLeft);
    } else if (data && data->resolved.showsConfigureHint) {
        g.setColour(look::text);
        g.drawFittedText("Choose parameters with Configure", 8, kTitleHeight + 6, getWidth() - 16,
                         36, juce::Justification::centred, 2);
    }
}
bool DevicePanelView::keyPressed(const juce::KeyPress &k) {
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        return strip.action("device.remove", {{"id", id}});
    return false;
}
void DevicePanelView::mouseDown(const juce::MouseEvent &e) {
    if (e.mods.isPopupMenu() && (e.eventComponent == &title || e.eventComponent == this))
        contextMenu(e.getScreenPosition());
    else if (e.eventComponent == this && foldSwitch().contains(e.getPosition()))
        strip.fold(id);
}
juce::Rectangle<int> DevicePanelView::foldSwitch() const noexcept { return {19, 3, 11, 11}; }
void DevicePanelView::mouseDoubleClick(const juce::MouseEvent &e) {
    if (e.eventComponent == &title || e.y < kTitleHeight)
        strip.fold(id);
}
juce::PopupMenu DevicePanelView::commands() {
    // Live's order (manual 23.2.1): Cut, Copy, Duplicate and Delete, then Rename, then the
    // device's own. Cut, Copy and Duplicate wait for a device's copy (window log, N18).
    juce::PopupMenu m;
    const auto safe = SafePointer<DevicePanelView>(this);
    m.addItem("Delete", [safe] {
        if (safe)
            safe->strip.action("device.remove", {{"id", safe->id}});
    });
    m.addSeparator();
    juce::PopupMenu::Item renameItem("Rename");
    renameItem.shortcutKeyDescription =
        juce::KeyPress('r', juce::ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons();
    renameItem.isEnabled = !strip.folded(id);
    renameItem.action = [safe] {
        if (safe)
            safe->beginRename();
    };
    m.addItem(std::move(renameItem));
    m.addSeparator();
    m.addItem(strip.folded(id) ? "Unfold" : "Fold", [safe] {
        if (safe)
            safe->strip.fold(safe->id);
    });
    m.addItem("Configure", [safe] {
        if (safe)
            safe->configure();
    });
    return m;
}
void DevicePanelView::contextMenu(juce::Point<int> where) {
    commands().showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({where.x, where.y, 1, 1}));
}
bool DevicePanelView::beginRename() {
    // A folded device has no title bar to type in.
    if (data == nullptr || strip.folded(id))
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
    if (commit && data != nullptr && !name.empty() && name != data->name)
        strip.action("device.rename", {{"id", id}, {"name", name}});
    if (isShowing())
        grabKeyboardFocus();
}
void DevicePanelView::configure() {
    if (!data)
        return;
    if (!configuring) {
        // The first press shows the search; the menu lists what it finds.
        configuring = true;
        strip.resized();
        return;
    }
    juce::PopupMenu m;
    m.addItem(1, "Use default panel");
    m.addItem(2, "Clear panel");
    m.addItem(3, "Done");
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
        m.addItem(static_cast<int>(candidates.size()) + 3, d.name, true,
                  std::find(ids.begin(), ids.end(), d.id) != ids.end());
    }
    auto safe = juce::Component::SafePointer<DevicePanelView>(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&configureButton),
                    [safe, ids, candidates](int chosen) mutable {
                        if (!safe || chosen == 0)
                            return;
                        if (chosen == 3) {
                            safe->configuring = false;
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
    const int h = getHeight();
    // Live's chain: devices 11 pt apart, a margin before the first.
    int x = 10;
    for (auto &p : panels) {
        const int width = p->widthFor(h);
        p->setBounds(x, 0, width, h);
        x += width + 11;
    }
    end_ = x;
    content.setSize(std::max(x, getWidth()), h);
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
    const int from = std::max(0, end_ - scroll.getViewPositionX());
    g.setColour(look::text);
    g.setFont(look::font(11.5f));
    g.drawText("Drop Audio Effects Here", from, 0, std::max(0, getWidth() - from), getHeight(),
               juce::Justification::centred);
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
    setTitle("Device View height");
}
void DockResizer::mouseDown(const juce::MouseEvent &) { start = root.state().deviceHeight; }
void DockResizer::mouseDrag(const juce::MouseEvent &e) {
    root.resizeDevices(start - e.getDistanceFromDragStartY());
}
void DockResizer::mouseUp(const juce::MouseEvent &) { root.persist(); }
} // namespace adi::ui
