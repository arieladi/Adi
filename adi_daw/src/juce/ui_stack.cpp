// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_stack.hpp"
#include "ui_devices.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace adi::ui {
namespace {
stack::TimeMs clockNow() noexcept {
    return static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes());
}
juce::Rectangle<float> rect(stack::Rect r) {
    return {static_cast<float>(r.x), static_cast<float>(r.y),
            static_cast<float>(r.w), static_cast<float>(r.h)};
}
stack::Mods mods(const juce::ModifierKeys &m) {
    return {m.isShiftDown(), m.isCommandDown() || m.isCtrlDown(), m.isAltDown()};
}
std::optional<stack::Key> modelKey(const juce::KeyPress &key) {
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::upKey) return stack::Key::Up;
    if (code == juce::KeyPress::downKey) return stack::Key::Down;
    if (code == juce::KeyPress::leftKey) return stack::Key::Left;
    if (code == juce::KeyPress::rightKey) return stack::Key::Right;
    if (code == juce::KeyPress::returnKey) return stack::Key::Enter;
    if (code == juce::KeyPress::escapeKey) return stack::Key::Escape;
    if (code == juce::KeyPress::homeKey) return stack::Key::Home;
    if (code == juce::KeyPress::endKey) return stack::Key::End;
    if (code == juce::KeyPress::pageUpKey) return stack::Key::PageUp;
    if (code == juce::KeyPress::pageDownKey) return stack::Key::PageDown;
    if (code == juce::KeyPress::tabKey) return stack::Key::Tab;
    if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'r' || key.getKeyCode() == 'R'))
        return stack::Key::Rename;
    return {};
}
bool sameStructure(const ParameterPublication *a, const ParameterPublication *b) {
    if (a == b) return true;
    if (!a || !b || a->size() != b->size()) return false;
    for (std::size_t i = 0; i < a->size(); ++i) {
        const auto &x = (*a)[i], &y = (*b)[i];
        if (x.id != y.id || x.track != y.track || x.chain != y.chain || x.instance != y.instance ||
            x.name != y.name || x.uid != y.uid || x.plugin != y.plugin ||
            x.available != y.available || x.enabled != y.enabled || x.instrument != y.instrument)
            return false;
    }
    return true;
}
} // namespace
struct StackTab::State : juce::ScrollBar::Listener {
    explicit State(StackTab &s) : owner(s) { scrollbar.addListener(this); }
    ~State() override { scrollbar.removeListener(this); }
    void scrollBarMoved(juce::ScrollBar *, double start) override {
        if (!syncing) owner.effects(model.scrollTo(start, clockNow()), false);
    }
    StackTab &owner;
    StackModel model;
    stack::SnapshotRows input;
    std::vector<stack::ChainDeviceInput> chain;
    std::vector<std::pair<std::int64_t, std::size_t>> deviceIndex;
    std::vector<stack::RowGeo> wanted;
    std::vector<std::int64_t> hidden;
    std::shared_ptr<const ParameterPublication> publication, structure, editPublication;
    std::uint64_t generation = std::numeric_limits<std::uint64_t>::max(), revision = 1;
    const engine::Snapshot *snapshot = nullptr; // identity only, never dereferenced after the frame
    const MixViewProvider *provider = nullptr;
    juce::Component canvas;
    juce::ScrollBar scrollbar{true};
    RenameEditor rename;
    bool syncing = false, ending = false, destroying = false, began = false;
    std::optional<stack::CaptureRef> interaction;
    const DevicePanelData *editData = nullptr;
    std::string parameter;
    double lastValue = 0;
};
struct StackRow::State {
    stack::RowGeo geo;
    juce::Component lane;
    struct Tile {
        stack::ItemRef target;
        std::unique_ptr<DevicePanelView> view;
        bool wanted = false;
    };
    std::vector<Tile> tiles;
};
StackRow::StackRow(StackTab &s, std::int64_t track)
    : stack_(s), track_(track), state_(std::make_unique<State>()) {
    setInterceptsMouseClicks(false, true);
    state_->lane.setInterceptsMouseClicks(false, true);
    addAndMakeVisible(state_->lane);
    setTitle("Channel");
    setTooltip("The channel's actual devices. Hover to enlarge; pin to retain a channel.");
}
StackRow::~StackRow() = default;
bool StackRow::zoomed() const noexcept { return state_->geo.enlarged || state_->geo.maximised; }
juce::Rectangle<int> StackRow::eqArea() const noexcept { return {}; }
juce::Rectangle<int> StackRow::sidechainArea() const noexcept { return {}; }
juce::Rectangle<int> StackRow::limiterArea() const noexcept { return {}; }
const char *StackRow::mode() const noexcept { return "Actual devices"; }
void StackRow::present(const stack::RowGeo &geo) {
    state_->geo = geo;
    const auto lane = stack_.model().lane(geo.shown);
    state_->lane.setBounds(static_cast<int>(std::round(lane.x)), 0,
                           std::max(0, static_cast<int>(std::round(lane.width))), getHeight());
    for (auto &tile : state_->tiles) tile.wanted = false;
    stack_.model().forEachChainItem(geo.shown, [&](const stack::ItemGeo &item) {
        const auto *data = stack_.device(item.ref.device);
        const auto resolved = stack_.model().resolve(item.ref);
        if (item.pill || !data || !resolved || resolved->identity != data->instance) return;
        auto found = std::find_if(state_->tiles.begin(), state_->tiles.end(),
                                 [&](const State::Tile &t) { return t.target == item.ref; });
        if (found == state_->tiles.end()) {
            State::Tile tile;
            tile.target = item.ref;
            tile.view = std::make_unique<DevicePanelView>(stack_.root.devices, *data);
            const auto target = item.ref;
            tile.view->targetValid = [this, target] { return stack_.validTarget(target); };
            tile.view->parameterGesture = [this, target](const std::string &p, engine::ParamEventKind k, double v) {
                return stack_.editParameter(target, p, k, v);
            };
            tile.view->interactionBegin = [this, target](DevicePanelView::Interaction k) {
                return stack_.beginInteraction(target, k == DevicePanelView::Interaction::Text
                                                         ? stack::CaptureKind::Text : stack::CaptureKind::Menu);
            };
            tile.view->interactionEnd = [this, target](DevicePanelView::Interaction) {
                if (const auto cap = stack_.model().capture(); cap && cap->target == target)
                    stack_.endInteraction();
            };
            tile.view->navigate = [this, target](bool forward) {
                if (!stack_.validTarget(target)) return false;
                stack_.effects(stack_.model().focusItem(target, clockNow()), false);
                const auto fx = stack_.model().key(forward ? stack::Key::Right : stack::Key::Left, {}, clockNow());
                stack_.effects(fx);
                stack_.layoutRows();
                stack_.focusTile(stack_.model().focus().item);
                return fx.consumed;
            };
            state_->lane.addAndMakeVisible(*tile.view);
            state_->tiles.push_back(std::move(tile));
            found = std::prev(state_->tiles.end());
        }
        found->wanted = true;
        found->view->setExplicitFocusOrder(static_cast<int>(item.index + 1));
        if (const auto cap = stack_.model().capture(); cap && cap->target == item.ref &&
            cap->kind == stack::CaptureKind::Drag) {
            bool sameControls = found->view->controls.size() == data->resolved.entries.size();
            for (std::size_t i = 0; sameControls && i < found->view->controls.size(); ++i)
                sameControls = found->view->controls[i]->parameter == data->resolved.entries[i].id;
            const bool parameterLive = data->available && std::any_of(data->records.begin(), data->records.end(),
                [&](const auto &record) { return record.id == stack_.state_->parameter && !record.missing; });
            if (!sameControls || !parameterLive) stack_.endInteraction();
        }
        found->view->present(*data);
        if (found->view->wrench.isVisible())
            found->view->wrench.setToggleState(stack_.root.pluginWindowOpen(data->id), juce::dontSendNotification);
        if (found->view->sample) {
            const engine::SampleSlotNode *slot = nullptr;
            if (const auto *snapshot = stack_.root.reader().get())
                for (const auto &candidate : snapshot->sampleSlots)
                    if (candidate && candidate->deviceId == data->id &&
                        candidate->slot == found->view->sample->slot) slot = candidate.get();
            found->view->sample->show(slot);
        }
        found->view->setBounds(static_cast<int>(std::round(item.x - lane.x)), 2,
                                std::max(0, static_cast<int>(std::round(item.width))),
                                std::max(0, getHeight() - 4));
    });
    std::erase_if(state_->tiles, [](const State::Tile &t) { return !t.wanted; });
}
void StackRow::paint(juce::Graphics &g) {
    const auto &row = state_->geo;
    const auto &m = stack_.model();
    const auto local = [&](stack::Rect r) { r.y -= row.top; return rect(r); };
    g.fillAll(row.selected ? look::laneSelectedAlt : look::panel);
    g.setColour(row.colour >= 0 ? look::paletteColour(row.colour) : look::trackColour(row.shown));
    g.fillRect(2, 2, 3, std::max(0, getHeight() - 4));
    const auto chip = m.chip(row.shown);
    const auto name = local(chip.chip);
    g.setColour(row.focused ? look::field : look::panel);
    g.fillRoundedRectangle(name, 2.f);
    g.setColour(look::text);
    g.setFont(look::font(12.f, true));
    const auto text = m.trackName(track_);
    g.drawFittedText(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())),
                    name.reduced(5, 1).toNearestInt(), juce::Justification::centredLeft, 2);
    if (row.group)
        look::triangle(g, local(chip.fold), row.folded ? 0.f : juce::MathConstants<float>::halfPi, look::text);
    g.setFont(look::font(11.f, true));
    g.setColour(row.pinned ? look::accent : look::textDim);
    g.drawText("P", local(chip.pin), juce::Justification::centred);
    g.setColour(row.pairA || row.pairB ? look::accent : look::textDim);
    g.drawText(row.pairA ? "A" : row.pairB ? "B" : "C", local(chip.collision), juce::Justification::centred);
    const auto lane = m.lane(row.shown);
    {
        juce::Graphics::ScopedSaveState saved(g);
        g.reduceClipRegion(juce::Rectangle<int>(static_cast<int>(lane.x), 0,
                                               static_cast<int>(lane.width), getHeight()));
        m.forEachChainItem(row.shown, [&](const stack::ItemGeo &item) {
            const auto *data = stack_.device(item.ref.device);
            const auto resolved = m.resolve(item.ref);
            const bool current = data && resolved && resolved->identity == data->instance;
            if (!item.pill && current) return;
            const juce::Rectangle<float> box(static_cast<float>(item.x), 3.f,
                                              static_cast<float>(item.width),
                                              static_cast<float>(std::max(0, getHeight() - 6)));
            look::box(g, box, item.focused ? look::field : look::button);
            g.setColour(current && item.enabled && item.available ? look::accent : look::textDim);
            g.fillEllipse(box.getCentreX() - 4, box.getY() + 3, 8.f, 8.f);
            g.setColour(current && item.available ? look::text : look::textDim);
            g.setFont(look::font(11.5f));
            const juce::String label = current ? juce::String(data->name) : "Device changed";
            if (box.getHeight() > box.getWidth() * 2) {
                juce::Graphics::ScopedSaveState rotated(g);
                g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi,
                                                               box.getCentreX(), box.getCentreY()));
                const juce::Rectangle<float> text(box.getCentreX() - (box.getHeight() - 18) / 2,
                                                  box.getCentreY() - box.getWidth() / 2,
                                                  box.getHeight() - 18, box.getWidth());
                g.drawText(label, text.reduced(2, 0), juce::Justification::centred, true);
            } else g.drawFittedText(label, box.withTrimmedTop(13).reduced(2, 1).toNearestInt(),
                                   juce::Justification::centred, 1);
        });
        if (lane.count == 0) {
            g.setColour(look::textDim);
            g.setFont(look::font(11.5f));
            g.drawText("No devices", juce::Rectangle<float>(static_cast<float>(lane.x + 8), 0,
                            static_cast<float>(std::max(0.0, lane.width - 16)), static_cast<float>(getHeight())),
                       juce::Justification::centredLeft);
        }
    }
    if (lane.left) look::triangle(g, local(lane.leftChevron).reduced(5.f), juce::MathConstants<float>::pi, look::text);
    if (lane.right) look::triangle(g, local(lane.rightChevron).reduced(5.f), 0.f, look::text);
    g.setColour(row.captured ? look::accent : look::separator);
    g.drawRect(getLocalBounds(), row.captured ? 2 : 1);
}
void StackRow::mouseEnter(const juce::MouseEvent &) { stack_.hover(track_); }
void StackRow::mouseExit(const juce::MouseEvent &) { stack_.hover(0); }
void StackRow::mouseDown(const juce::MouseEvent &e) { stack_.mouseDown(e); }

StackTab::StackTab(AdiRootComponent &r) : root(r), state_(std::make_unique<State>(*this)) {
    setTitle("STACK");
    setWantsKeyboardFocus(true);
    state_->canvas.addMouseListener(this, true);
    addAndMakeVisible(state_->canvas);
    addAndMakeVisible(state_->scrollbar);
    search.setTitle("Find channels or devices");
    search.setTextToShowWhenEmpty("Find channel or device", look::textDim);
    search.onTextChange = [this] {
        effects(model().setSearch(search.getText().toStdString(), clockNow()), false);
        layoutRows();
    };
    search.onEscapeKey = [this] { search.clear(); };
    search.onReturnKey = [this] { effects(model().searchKey(stack::Key::Enter, clockNow())); layoutRows(); };
    addAndMakeVisible(search);
    rowSize.setTitle("Channel height");
    rowSize.setSliderStyle(juce::Slider::LinearHorizontal);
    rowSize.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    rowSize.setRange(model().constants().baseMin, model().constants().baseMax, 1);
    rowSize.setValue(model().windowView().baseHeight, juce::dontSendNotification);
    rowSize.onValueChange = [this] {
        effects(model().setBaseHeight(static_cast<int>(rowSize.getValue()), clockNow()), false);
    };
    addAndMakeVisible(rowSize);
    for (auto *button : {&autoZoom, &pinsOnly, &navigator}) {
        button->setMouseClickGrabsKeyboardFocus(false);
        addAndMakeVisible(*button);
    }
    autoZoom.setToggleState(true, juce::dontSendNotification);
    navigator.setToggleState(true, juce::dontSendNotification);
    autoZoom.onClick = [this] { effects(model().setAutoZoom(autoZoom.getToggleState(), clockNow()), false); };
    pinsOnly.onClick = [this] { effects(model().setFilterPinned(pinsOnly.getToggleState(), clockNow()), false); layoutRows(); };
    navigator.onClick = [this] { effects(model().setNavigator(navigator.getToggleState(), false, clockNow()), false); layoutRows(); };
    state_->rename.setTitle("Channel name");
    state_->rename.onCommit = [this] { endRename(true); };
    state_->rename.onCancel = [this] { endRename(false); };
    state_->rename.onFocusLost = [this] { if (!state_->ending) endRename(true); };
    state_->rename.onTab = [this](bool forward) {
        effects(model().key(stack::Key::Tab, {!forward, false, false}, clockNow()));
        layoutRows();
    };
    state_->canvas.addChildComponent(state_->rename);
}
StackTab::~StackTab() {
    state_->destroying = true;
    cancelInteractions();
    giveAwayKeyboardFocus();
    state_->canvas.removeMouseListener(this);
    rows_.clear();
    // Public toolbar children outlive state_ by declaration order. Detach them while
    // state_ is still alive so their destructors cannot send focus events into it.
    removeAllChildren();
}
StackModel &StackTab::model() noexcept { return state_->model; }
const StackModel &StackTab::model() const noexcept { return state_->model; }
const MixViewProvider &StackTab::provider() const noexcept { return root.mixProvider(); }
juce::Rectangle<int> StackTab::display() const noexcept {
    return {look::gap, kHeaderHeight, std::max(0, getWidth() - 2 * look::gap), kDisplayHeight};
}
juce::Rectangle<int> StackTab::listArea() const noexcept {
    const int top = kHeaderHeight + kDisplayHeight + look::gap;
    return {look::gap, top, std::max(0, getWidth() - 2 * look::gap - 8),
             std::max(0, getHeight() - top - look::gap)};
}
juce::String StackTab::scope() const {
    if (const auto id = root.state().viewGroup; id != 0)
        if (const auto *track = root.reader().findTrack(id)) return juce::String(track->name);
    return "Project";
}
const DevicePanelData *StackTab::device(std::int64_t id) const noexcept {
    const auto &index = state_->deviceIndex;
    const auto at = std::lower_bound(index.begin(), index.end(), id,
                                    [](const auto &entry, std::int64_t key) { return entry.first < key; });
    if (state_->publication && at != index.end() && at->first == id && at->second < state_->publication->size())
        return &(*state_->publication)[at->second];
    return nullptr;
}
void StackTab::refreshRows(const SnapshotReader &reader, stack::TimeMs time) {
    const auto &hidden = root.arrangement.geometry.hidden;
    // ProjectView owns the UI publication, not SnapshotPublisher: Snapshot::seq is unstamped
    // here. The root's generation is its presented ProjectView generation. Pointer identity
    // additionally notices a fresh current() read by an input event before the next frame.
    const auto generation = root.generation();
    const bool changed = !sameStructure(state_->structure.get(), state_->publication.get());
    if (changed) {
        state_->deviceIndex.clear();
        if (state_->publication)
            for (std::size_t i = 0; i < state_->publication->size(); ++i)
                state_->deviceIndex.emplace_back((*state_->publication)[i].id, i);
        std::sort(state_->deviceIndex.begin(), state_->deviceIndex.end());
    }
    if (state_->generation != generation || state_->snapshot != reader.get() ||
        state_->provider != &provider() || state_->hidden != hidden || changed) {
        ++state_->revision;
        state_->chain.clear();
        if (state_->publication)
            for (const auto &d : *state_->publication)
                state_->chain.push_back({d.track, d.id, d.instance,
                    d.plugin ? stack::ItemClass::Plugin : stack::ItemClass::View, d.name, d.available, d.enabled});
        stack::tracksFromSnapshot(reader, root.arrangement.geometry, provider(), state_->chain, state_->input);
        effects(model().setRows(state_->input.input(generation, state_->revision), time), false);
        state_->generation = generation;
        state_->snapshot = reader.get();
        state_->provider = &provider();
        state_->hidden = hidden;
    }
    state_->structure = state_->publication;
}
void StackTab::frame(const SnapshotReader &reader) { frameAt(reader, clockNow()); }
void StackTab::frameAt(const SnapshotReader &reader, stack::TimeMs time) {
    // Keep the prior record storage alive until every component has adopted the new feed.
    const auto old = state_->publication;
    (void)old; // its ownership deliberately lasts through every present() below
    if (root.devices.publication) state_->publication = root.devices.publication();
    refreshRows(reader, time);
    effects(model().setSelection(root.arrangement.geometry.selection.track, time), false);
    effects(model().advance(time), false);
    layoutRows();
}
void StackTab::syncScroll() {
    state_->syncing = true;
    state_->scrollbar.setRangeLimits(0, std::max(1.0, model().contentHeight()), juce::dontSendNotification);
    state_->scrollbar.setCurrentRange(model().scroll(), listArea().getHeight(), juce::dontSendNotification);
    state_->scrollbar.setSingleStepSize(model().windowView().baseHeight);
    state_->scrollbar.setVisible(model().maxScroll() > 0);
    state_->syncing = false;
}
void StackTab::focusTile(const stack::ItemRef &target) {
    if (target.place != stack::ItemPlace::Chain) return;
    for (const auto &row : rows_)
        for (const auto &tile : row->state_->tiles)
            if (tile.target == target && tile.view->isShowing()) {
                tile.view->grabKeyboardFocus();
                return;
            }
}
void StackTab::layoutRows() {
    state_->wanted.clear();
    const auto realised = model().realised();
    model().forEachRow(realised, [this](const stack::RowGeo &g) { state_->wanted.push_back(g); });
    for (const auto ref : model().retained())
        if (ref.valid())
            if (const auto at = model().shownIndex(ref.track); at && !realised.contains(*at))
                if (std::none_of(state_->wanted.begin(), state_->wanted.end(), [&](const auto &g) { return g.ref == ref; }))
                    state_->wanted.push_back(model().row(*at));
    std::erase_if(rows_, [this](const auto &row) {
        return std::none_of(state_->wanted.begin(), state_->wanted.end(),
                            [&](const auto &g) { return g.ref == row->state_->geo.ref; });
    });
    for (const auto &geo : state_->wanted) {
        auto found = std::find_if(rows_.begin(), rows_.end(), [&](const auto &r) { return r->track() == geo.ref.track; });
        if (found == rows_.end()) {
            auto row = std::make_unique<StackRow>(*this, geo.ref.track);
            state_->canvas.addAndMakeVisible(*row);
            rows_.push_back(std::move(row));
            found = std::prev(rows_.end());
        }
        (*found)->setBounds(0, static_cast<int>(std::round(geo.top)),
                            std::max(0, static_cast<int>(std::round(model().plan().rowsRight))),
                            std::max(0, static_cast<int>(std::round(geo.height))));
        (*found)->setExplicitFocusOrder(static_cast<int>(std::min<std::size_t>(geo.shown + 1,
                                                      static_cast<std::size_t>(std::numeric_limits<int>::max()))));
        (*found)->present(geo);
    }
    if (state_->rename.isVisible())
        if (const auto cap = model().capture(); cap && cap->target.place == stack::ItemPlace::None)
            if (const auto at = model().shownIndex(cap->target.row.track))
                state_->rename.setBounds(rect(model().chip(*at).chip).toNearestInt());
    state_->rename.toFront(false);
    syncScroll();
}
void StackTab::effects(const stack::Effects &fx, bool takeFocus) {
    if (state_->destroying) return;
    if (fx.captureLost != stack::CaptureStatus::Live) cancelInteractions();
    if (fx.captureEnded != 0 && state_->rename.isVisible()) endRename(true);
    if (fx.select != 0) {
        root.arrangement.geometry.selection.track = fx.select;
        root.publishSelection();
    }
    if (fx.showDevices.valid()) {
        root.arrangement.geometry.selection.track = fx.showDevices.track;
        root.publishSelection();
        root.showDeviceView();
    }
    if (fx.takeFocus && takeFocus && isShowing()) grabKeyboardFocus();
    if (fx.escape == stack::EscapeStep::Capture) cancelInteractions();
    if (fx.escape == stack::EscapeStep::CloseMixView) root.setMixView(false, root.state().mixViewDetached);
    if (fx.rename.valid()) beginRename(fx.rename);
    if (fx.repaint) { state_->canvas.repaint(); repaint(); }
    if (fx.windowViewChanged) {
        rowSize.setValue(model().windowView().baseHeight, juce::dontSendNotification);
        autoZoom.setToggleState(model().windowView().autoZoom, juce::dontSendNotification);
        pinsOnly.setToggleState(model().windowView().filterPinned, juce::dontSendNotification);
        navigator.setToggleState(model().windowView().navigator, juce::dontSendNotification);
        if (search.getText().toStdString() != model().windowView().search) search.setText(model().windowView().search, false);
    }
}
void StackTab::resized() {
    const int available = std::max(0, getWidth() - 2 * look::gap);
    search.setBounds(look::gap, 4, std::max(50, std::min(230, available / 3)), 22);
    rowSize.setBounds(search.getRight() + 8, 4, 90, 22);
    autoZoom.setBounds(rowSize.getRight() + 6, 4, 94, 22);
    pinsOnly.setBounds(autoZoom.getRight() + 4, 4, 78, 22);
    navigator.setBounds(pinsOnly.getRight() + 4, 4, 100, 22);
    state_->canvas.setBounds(listArea());
    state_->scrollbar.setBounds(listArea().getRight(), listArea().getY(), 8, listArea().getHeight());
    effects(model().setViewport(listArea().getWidth(), listArea().getHeight(), clockNow()), false);
    layoutRows();
}
void StackTab::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    const auto area = display();
    g.setColour(look::display);
    g.fillRect(area);
    g.setColour(look::textOnBand);
    g.setFont(look::font(12.f, true));
    g.drawText("Channels: " + scope(), area.reduced(8, 5).withHeight(18), juce::Justification::centredLeft);
    g.setFont(look::font(11.5f));
    g.drawText("Actual device chains  |  " + juce::String(static_cast<int>(model().shownCount())) + " channels",
               area.reduced(8, 5).withTrimmedTop(20), juce::Justification::centredLeft);
    if (model().shownCount() == 0) {
        g.setColour(look::textDim);
        g.drawText(search.getText().isEmpty() ? "No channels in this view." : "No matching channels or devices.",
                   listArea(), juce::Justification::centred);
    }
    const auto nav = model().navigator();
    if (nav.shown) {
        juce::Graphics::ScopedSaveState saved(g);
        g.addTransform(juce::AffineTransform::translation(static_cast<float>(listArea().getX()),
                                                          static_cast<float>(listArea().getY())));
        g.setColour(look::band); g.fillRect(rect(nav.area));
        g.setColour(look::textOnBand);
        g.drawText("Channels", rect(nav.header).reduced(4, 0), juce::Justification::centredLeft);
        g.reduceClipRegion(rect(nav.lines).toNearestInt());
        model().forEachNavigatorLine([&](const stack::NavLine &line) {
            const auto colour = line.colour >= 0 ? look::paletteColour(line.colour) : look::trackColour(0);
            g.setColour(line.onScreen ? colour : colour.withAlpha(.45f));
            g.fillRect(static_cast<float>(nav.lines.x + line.indent + 3), static_cast<float>(line.y + 2), 3.f, 10.f);
            if (!nav.collapsed) {
                g.setColour(line.ref.track == model().selection() ? look::accent : look::textOnBand);
                const auto name = model().trackName(line.ref.track);
                g.drawText(juce::String::fromUTF8(name.data(), static_cast<int>(name.size())),
                    juce::Rectangle<float>(static_cast<float>(nav.lines.x + line.indent + 10),
                        static_cast<float>(line.y), static_cast<float>(std::max(0.0, nav.lines.w - line.indent - 12)), 14.f),
                    juce::Justification::centredLeft, true);
            }
        });
        g.setColour(look::accent.withAlpha(.8f)); g.drawRect(rect(nav.frame), 1.f);
    }
}
juce::Point<float> StackTab::modelPoint(const juce::MouseEvent &e) const {
    return e.getEventRelativeTo(const_cast<StackTab *>(this)).position - listArea().getPosition().toFloat();
}
bool StackTab::fromList(const juce::MouseEvent &e) const {
    return e.eventComponent == &state_->canvas || state_->canvas.isParentOf(e.eventComponent) ||
           listArea().contains(e.getEventRelativeTo(const_cast<StackTab *>(this)).getPosition());
}
void StackTab::mouseMove(const juce::MouseEvent &e) {
    if (!fromList(e)) return;
    const auto p = modelPoint(e);
    effects(model().pointerMove(p.x, p.y, e.mods.isAnyMouseButtonDown(), clockNow()), false);
}
void StackTab::mouseEnter(const juce::MouseEvent &e) { mouseMove(e); }
void StackTab::mouseExit(const juce::MouseEvent &e) {
    const auto p = modelPoint(e);
    if (p.x < 0 || p.y < 0 || p.x >= listArea().getWidth() || p.y >= listArea().getHeight())
        effects(model().pointerExit(clockNow()), false);
}
void StackTab::mouseDown(const juce::MouseEvent &e) {
    if (!fromList(e)) return;
    const auto p = modelPoint(e);
    // Hit the geometry that supplied the visible components. Reflowing a newer document
    // here would hit an unseen row at this screen point. Validate the seen identity against
    // current state instead; the next frame adopts its new position.
    const auto presented = model().hitTest(p.x, p.y);
    const auto current = root.current();
    if (presented.row.valid() && (!current || !current->findTrack(presented.row.track))) return;
    if (presented.item.place == stack::ItemPlace::Chain && !validTarget(presented.item)) return;
    const auto button = e.mods.isPopupMenu() ? stack::Button::Right
                      : e.mods.isMiddleButtonDown() ? stack::Button::Middle : stack::Button::Left;
    const auto fx = model().pointerDown(p.x, p.y, button, mods(e.mods), e.getNumberOfClicks(), clockNow());
    effects(fx, e.eventComponent == &state_->canvas);
    if (fx.hit.part == stack::HitPart::ChainPill && button == stack::Button::Left &&
        p.y >= fx.hit.rect.y && p.y < fx.hit.rect.y + 16 && validTarget(fx.hit.item)) {
        if (const auto *data = device(fx.hit.item.device); data && data->available)
            root.devices.action("device.setEnabled", {{"id", data->id}, {"enabled", !data->enabled}});
        layoutRows();
        return;
    }
    if ((fx.hit.part == stack::HitPart::ChainPill || fx.hit.part == stack::HitPart::ChainItem) &&
        e.getNumberOfClicks() > 1 && e.eventComponent == &state_->canvas) {
        root.devices.chose(fx.hit.item.device);
        root.showDeviceView();
    }
    layoutRows();
}
void StackTab::mouseDrag(const juce::MouseEvent &e) { mouseMove(e); }
void StackTab::mouseUp(const juce::MouseEvent &e) {
    if (!fromList(e)) return;
    const auto p = modelPoint(e);
    effects(model().pointerUp(p.x, p.y, clockNow()), false);
}
void StackTab::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    if (!fromList(e)) return;
    const auto p = modelPoint(e);
    stack::WheelDelta wheel;
    if (w.isSmooth) { wheel.pixelsX = -w.deltaX * 120; wheel.pixelsY = -w.deltaY * 120; }
    else wheel.notches = -w.deltaY * 10;
    effects(model().wheel(p.x, p.y, wheel, mods(e.mods), clockNow()), false);
    layoutRows();
}
bool StackTab::hasDeviceContext() const {
    if (!hasKeyboardFocus(true) || search.hasKeyboardFocus(true) || state_->rename.isVisible()) return false;
    const auto target = model().focus().item;
    if (target.place != stack::ItemPlace::Chain) return false;
    for (const auto &row : rows_)
        for (const auto &tile : row->state_->tiles)
            if (tile.target == target && (tile.view->rename.isVisible() || tile.view->search.hasKeyboardFocus(true)))
                return false;
    return true;
}
bool StackTab::deviceCommand(int command) {
    const auto target = model().focus().item;
    if (target.place != stack::ItemPlace::Chain || !validTarget(target)) return false;
    using Clip = DeviceChainStrip::Clip;
    if (command == AppCommands::Cut) return root.devices.clipboard(Clip::Cut, target.device);
    if (command == AppCommands::Copy) return root.devices.clipboard(Clip::Copy, target.device);
    if (command == AppCommands::Paste) return root.devices.clipboard(Clip::Paste, target.device);
    if (command == AppCommands::Duplicate) return root.devices.clipboard(Clip::Duplicate, target.device);
    if (command == AppCommands::DeleteTrack) return root.devices.action("device.remove", {{"id", target.device}});
    if (command == AppCommands::RenameTrack) {
        for (const auto &row : rows_)
            for (const auto &tile : row->state_->tiles)
                if (tile.target == target) return tile.view->beginRename();
        root.say("Enlarge the device to rename it, or open its Device View.");
    }
    return false;
}
bool StackTab::keyPressed(const juce::KeyPress &key) {
    const auto target = model().focus().item;
    if (target.place == stack::ItemPlace::Chain) {
        const auto code = key.getKeyCode();
        if (key.getModifiers().isCommandDown()) {
            std::optional<DeviceChainStrip::Clip> clip;
            if (code == 'x' || code == 'X') clip = DeviceChainStrip::Clip::Cut;
            if (code == 'c' || code == 'C') clip = DeviceChainStrip::Clip::Copy;
            if (code == 'v' || code == 'V') clip = DeviceChainStrip::Clip::Paste;
            if (code == 'd' || code == 'D') clip = DeviceChainStrip::Clip::Duplicate;
            if (clip) {
                const auto command = *clip == DeviceChainStrip::Clip::Cut ? AppCommands::Cut
                                   : *clip == DeviceChainStrip::Clip::Copy ? AppCommands::Copy
                                   : *clip == DeviceChainStrip::Clip::Paste ? AppCommands::Paste : AppCommands::Duplicate;
                deviceCommand(command);
                return true;
            }
            if (code == 'r' || code == 'R') {
                deviceCommand(AppCommands::RenameTrack);
                return true;
            }
        }
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) {
            if (validTarget(target)) root.devices.action("device.remove", {{"id", target.device}});
            return true;
        }
    }
    if (const auto k = modelKey(key)) {
        const auto fx = model().key(*k, mods(key.getModifiers()), clockNow());
        effects(fx); layoutRows();
        if (fx.takeFocus) focusTile(model().focus().item);
        return fx.consumed;
    }
    return false;
}
void StackTab::focusGained(FocusChangeType) { effects(model().setKeyboardFocus(true, clockNow()), false); }
void StackTab::focusLost(FocusChangeType) {
    effects(model().setKeyboardFocus(hasKeyboardFocus(true), clockNow()), false);
    if (!hasKeyboardFocus(true) && state_->interaction && state_->interaction->kind == stack::CaptureKind::Drag)
        cancelInteractions();
}
void StackTab::focusOfChildComponentChanged(FocusChangeType type) {
    if (state_->destroying) return;
    if (hasKeyboardFocus(true)) {
        for (const auto &row : rows_)
            for (const auto &tile : row->state_->tiles)
                if (tile.view->hasKeyboardFocus(true) && model().focus().item != tile.target && validTarget(tile.target))
                    effects(model().focusItem(tile.target, clockNow()), false);
        focusGained(type);
    } else focusLost(type);
}
void StackTab::visibilityChanged() {
    if (!isShowing()) {
        cancelInteractions();
        effects(model().pointerExit(clockNow()), false);
        effects(model().setKeyboardFocus(false, clockNow()), false);
    }
}
void StackTab::parentHierarchyChanged() { if (!isShowing()) cancelInteractions(); }
void StackTab::focus(std::int64_t track) { effects(model().openFromChip(track, {}, clockNow())); layoutRows(); }
std::int64_t StackTab::focused() const noexcept { return model().focus().row.track; }
void StackTab::hover(std::int64_t track) {
    const auto time = clockNow();
    if (const auto at = model().shownIndex(track)) {
        const auto row = model().row(*at);
        effects(model().pointerMove(8, row.top + row.height / 2, false, time), false);
    } else effects(model().pointerExit(time), false);
}
std::int64_t StackTab::hovered() const noexcept { return model().enlarged().track; }
bool StackTab::validTarget(const stack::ItemRef &target) const {
    if (!isShowing() || !root.state().mixView || root.state().mixViewTab != 1)
        return false; // a late menu/key must not edit through a closed surface
    const auto resolved = model().resolve(target);
    const auto *data = device(target.device);
    if (!resolved || !data || resolved->identity != data->instance) return false;
    const auto current = root.current();
    return current && current->findTrack(target.row.track) != nullptr && root.devices.validExpected(*data);
}
bool StackTab::beginInteraction(const stack::ItemRef &target, stack::CaptureKind kind) {
    if (!validTarget(target)) return false;
    if (state_->interaction) return state_->interaction->target == target && state_->interaction->kind == kind;
    // Child controls receive their own mouseDown before the canvas listener. Select their
    // stable target here as well, without stealing keyboard focus from the control.
    effects(model().focusItem(target, clockNow()), false);
    const auto capture = model().beginCapture(kind, target, clockNow());
    if (capture.refusal != stack::CaptureRefusal::None) return false;
    state_->interaction = capture.ref;
    return true;
}
void StackTab::endInteraction() {
    if (state_->ending) return;
    state_->ending = true;
    if (state_->began && state_->editData && !state_->parameter.empty())
        root.devices.editExpected(*state_->editData, state_->parameter, engine::ParamEventKind::End, state_->lastValue);
    state_->began = false;
    state_->editData = nullptr;
    state_->editPublication.reset();
    state_->parameter.clear();
    if (state_->interaction) {
        const auto capture = *state_->interaction;
        state_->interaction.reset();
        effects(model().endCapture(capture, clockNow()), false);
    }
    state_->ending = false;
}
bool StackTab::editParameter(const stack::ItemRef &target, const std::string &parameter,
                             engine::ParamEventKind kind, double value) {
    if (kind == engine::ParamEventKind::Begin) {
        // A reset/new control gesture during a held drag is a new gesture, not a new
        // owner installed over an unclosed old one. End the exact original target first.
        if (state_->editData) endInteraction();
        if (!beginInteraction(target, stack::CaptureKind::Drag)) return false;
        state_->editPublication = state_->publication;
        state_->editData = device(target.device);
        state_->parameter = parameter;
        state_->lastValue = value;
        state_->began = false;
    }
    if (!state_->interaction || state_->interaction->target != target ||
        state_->parameter != parameter || !state_->editData) return false;
    if (kind == engine::ParamEventKind::End) {
        endInteraction();
        return true;
    }
    if (!validTarget(target) || model().captureStatus(*state_->interaction) != stack::CaptureStatus::Live) {
        endInteraction();
        return false;
    }
    const bool accepted = root.devices.editExpected(*state_->editData, parameter, kind, value);
    if (accepted) state_->lastValue = value;
    if (kind == engine::ParamEventKind::Begin) state_->began = accepted;
    if (!accepted) endInteraction();
    return accepted;
}
void StackTab::cancelInteractions() {
    if (!state_ || state_->ending) return;
    endRename(false);
    endInteraction();
    for (auto &row : rows_)
        for (auto &tile : row->state_->tiles)
            tile.view->cancelInteractions();
}
void StackTab::beginRename(stack::RowRef row) {
    const stack::ItemRef target{row, stack::ItemPlace::None, 0, 0, 0};
    const auto capture = model().beginCapture(stack::CaptureKind::Text, target, clockNow());
    if (capture.refusal != stack::CaptureRefusal::None) return;
    state_->interaction = capture.ref;
    const auto at = model().shownIndex(row.track);
    if (!at) { endInteraction(); return; }
    const auto name = model().trackName(row.track);
    state_->rename.setText(juce::String::fromUTF8(name.data(), static_cast<int>(name.size())), false);
    state_->rename.setBounds(rect(model().chip(*at).chip).toNearestInt());
    state_->rename.setVisible(true);
    state_->rename.toFront(true);
    state_->rename.selectAll();
    if (state_->rename.isShowing()) state_->rename.grabKeyboardFocus();
}
void StackTab::endRename(bool commit) {
    if (!state_->rename.isVisible() || state_->ending) return;
    state_->ending = true;
    const auto capture = state_->interaction;
    const auto text = state_->rename.getText().toStdString();
    state_->rename.setVisible(false);
    state_->ending = false;
    if (commit && capture && model().live(capture->target.row)) {
        const auto current = root.current();
        const auto *track = current ? current->findTrack(capture->target.row.track) : nullptr;
        if (track && track->name != text)
            root.submitEdit("track.rename", {{"id", track->id}, {"name", text}}, "Rename Track");
    }
    endInteraction();
}
} // namespace adi::ui
