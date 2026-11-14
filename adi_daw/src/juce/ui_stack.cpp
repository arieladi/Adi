// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_stack.hpp"
#include "ui_devices.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include "adi/mix_functions.hpp"
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
    std::vector<std::int64_t> demanded;
    std::shared_ptr<const ParameterPublication> publication, structure, editPublication;
    std::uint64_t generation = std::numeric_limits<std::uint64_t>::max(), revision = 1;
    const engine::Snapshot *snapshot = nullptr; // identity only, never dereferenced after the frame
    const MixViewProvider *provider = nullptr;
    juce::Component canvas;
    juce::ScrollBar scrollbar{true};
    RenameEditor rename;
    bool syncing = false, ending = false, destroying = false, began = false, parametersSuspended = false;
    std::uint64_t frame = 0, layout = 0;
    std::optional<stack::CaptureRef> interaction;
    const DevicePanelData *editData = nullptr;
    std::string parameter;
    double lastValue = 0;
};
struct StackRow::State {
    stack::RowGeo geo;
    stack::RowGeo presentedRow;
    stack::ChipGeo presentedChip;
    stack::LaneGeo presentedLane;
    std::vector<stack::ItemGeo> presentedItems, pendingItems;
    std::uint64_t presentation = 0;
    bool destroying = false;
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
StackRow::~StackRow() { state_->destroying = true; }
bool StackRow::zoomed() const noexcept { return state_->geo.enlarged || state_->geo.maximised; }
juce::Rectangle<int> StackRow::eqArea() const noexcept { return {}; }
juce::Rectangle<int> StackRow::sidechainArea() const noexcept { return {}; }
juce::Rectangle<int> StackRow::limiterArea() const noexcept { return {}; }
const char *StackRow::mode() const noexcept { return "Actual devices"; }
void StackRow::present(const stack::RowGeo &input) {
    const auto geo = input;
    const juce::Component::SafePointer<StackRow> alive(this);
    const auto presentation = ++state_->presentation;
    const auto publication = stack_.state_->publication;
    (void)publication; // callbacks may publish a replacement while this row adopts these records
    state_->geo = geo;
    const auto lane = stack_.model().lane(geo.shown);
    const auto chip = stack_.model().chip(geo.shown);
    state_->pendingItems.clear();
    state_->presentedItems.reserve(lane.count);
    state_->pendingItems.reserve(lane.count);
    state_->lane.setBounds(static_cast<int>(std::round(lane.x)), 0,
                           std::max(0, static_cast<int>(std::round(lane.width))), getHeight());
    if (!alive || state_->presentation != presentation) return;
    for (auto &tile : state_->tiles) tile.wanted = false;
    // Complete the pure model walk before any control callback can replace the
    // row/model. Callback-bearing presentation only visits local item copies.
    stack_.model().forEachChainItem(geo.shown, [&](const stack::ItemGeo &item) {
        state_->pendingItems.push_back(item);
    });
    const auto presentItem = [&](const stack::ItemGeo &item) {
        if (!alive || state_->presentation != presentation) return;
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
            const juce::Component::SafePointer<StackRow> owner(this);
            tile.view->targetValid = [owner, target] { return owner && !owner->state_->destroying && owner->stack_.validTarget(target); };
            tile.view->parameterGesture = [owner, target](const std::string &p, engine::ParamEventKind k, double v) {
                return owner && !owner->state_->destroying && owner->stack_.editParameter(target, p, k, v);
            };
            tile.view->interactionBegin = [owner, target](DevicePanelView::Interaction k) {
                return owner && !owner->state_->destroying && owner->stack_.beginInteraction(target, k == DevicePanelView::Interaction::Text
                                                         ? stack::CaptureKind::Text : stack::CaptureKind::Menu);
            };
            tile.view->interactionEnd = [owner, target](DevicePanelView::Interaction) {
                if (!owner || owner->state_->destroying) return;
                if (const auto cap = owner->stack_.model().capture(); cap && cap->target == target)
                    owner->stack_.endInteraction();
            };
            tile.view->navigate = [owner, target](bool forward) {
                if (!owner || owner->state_->destroying || !owner->stack_.validTarget(target)) return false;
                const juce::Component::SafePointer<StackTab> surface(&owner->stack_);
                surface->effects(surface->model().focusItem(target, clockNow()), false);
                if (!surface) return false;
                const auto fx = surface->model().key(forward ? stack::Key::Right : stack::Key::Left, {}, clockNow());
                surface->effects(fx);
                if (!surface) return fx.consumed;
                surface->layoutRows();
                if (surface) surface->focusTile(surface->model().focus().item);
                return fx.consumed;
            };
            state_->lane.addAndMakeVisible(*tile.view);
            if (!alive || state_->presentation != presentation) return;
            state_->tiles.push_back(std::move(tile));
            found = std::prev(state_->tiles.end());
        }
        found->wanted = true;
        found->view->setExplicitFocusOrder(static_cast<int>(item.index + 1));
        if (const auto cap = stack_.model().capture(); cap && cap->target == item.ref &&
            cap->kind == stack::CaptureKind::Drag) {
            const bool sameControls = found->view->controlsMatch(*data);
            const bool parameterLive = data->available && std::any_of(data->parameters().begin(), data->parameters().end(),
                [&](const auto &record) { return record.id == stack_.state_->parameter && !record.missing; });
            if (!sameControls || !parameterLive) stack_.endInteraction();
            if (!alive || state_->presentation != presentation) return;
            found = std::find_if(state_->tiles.begin(), state_->tiles.end(),
                                 [&](const State::Tile &t) { return t.target == item.ref; });
            if (found == state_->tiles.end()) return;
        }
        const juce::Component::SafePointer<DevicePanelView> panel(found->view.get());
        panel->present(*data);
        if (!alive || !panel || state_->presentation != presentation) return;
        if (panel->wrench.isVisible())
            panel->wrench.setToggleState(stack_.root.pluginWindowOpen(data->id), juce::dontSendNotification);
        if (panel->sample) {
            const engine::SampleSlotNode *slot = nullptr;
            if (const auto *snapshot = stack_.root.reader().get())
                for (const auto &candidate : snapshot->sampleSlots)
                    if (candidate && candidate->deviceId == data->id &&
                        candidate->slot == panel->sample->slot) slot = candidate.get();
            panel->sample->show(slot);
        }
        if (!alive || !panel || state_->presentation != presentation) return;
        panel->setBounds(static_cast<int>(std::round(item.x - lane.x)),
                                static_cast<int>(std::round(item.top - geo.top)) + 2,
                                std::max(0, static_cast<int>(std::round(item.width))),
                                std::max(0, static_cast<int>(std::round(item.height)) - 4));
    };
    const auto count = state_->pendingItems.size();
    for (std::size_t j = 0; j < count; ++j) {
        if (!alive || state_->presentation != presentation) return;
        const auto item = state_->pendingItems[j];
        presentItem(item);
    }
    if (!alive || state_->presentation != presentation) return;
    for (std::size_t j = state_->tiles.size(); j-- > 0;) {
        if (state_->tiles[j].wanted) continue;
        auto retired = std::move(state_->tiles[j]);
        state_->tiles.erase(state_->tiles.begin() + static_cast<std::ptrdiff_t>(j));
        retired.view.reset();
        if (!alive || state_->presentation != presentation) return;
    }
    if (!alive || state_->presentation != presentation) return;
    state_->presentedRow = geo;
    state_->presentedChip = chip;
    state_->presentedLane = lane;
    state_->presentedItems.swap(state_->pendingItems);
}
void StackRow::paint(juce::Graphics &g) {
    const auto &row = state_->presentedRow;
    const auto &m = stack_.model();
    const auto local = [&](stack::Rect r) { r.y -= row.top; return rect(r); };
    g.fillAll(row.selected ? look::laneSelectedAlt : look::panel);
    g.setColour(row.colour >= 0 ? look::paletteColour(row.colour) : look::trackColour(row.shown));
    g.fillRect(2, 2, 3, std::max(0, getHeight() - 4));
    const auto chip = state_->presentedChip;
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
    const auto lane = state_->presentedLane;
    {
        juce::Graphics::ScopedSaveState saved(g);
        g.reduceClipRegion(juce::Rectangle<int>(static_cast<int>(lane.x), 0,
                                               static_cast<int>(lane.width), getHeight()));
        for (const auto &item : state_->presentedItems) {
            const auto *data = stack_.device(item.ref.device);
            const auto resolved = m.resolve(item.ref);
            const bool current = data && resolved && resolved->identity == data->instance;
            if (!item.pill && current) continue;
            const juce::Rectangle<float> box(static_cast<float>(item.x), static_cast<float>(item.top - row.top) + 3.f,
                                              static_cast<float>(item.width),
                                              static_cast<float>(std::max(0.0, item.height - 6)));
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
                const juce::Rectangle<float> labelArea(box.getCentreX() - (box.getHeight() - 18) / 2,
                                                  box.getCentreY() - box.getWidth() / 2,
                                                  box.getHeight() - 18, box.getWidth());
                g.drawText(label, labelArea.reduced(2, 0), juce::Justification::centred, true);
            } else g.drawFittedText(label, box.withTrimmedTop(13).reduced(2, 1).toNearestInt(),
                                   juce::Justification::centred, 1);
        }
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
    channelScope.setTitle("Channels shown");
    channelScope.setTooltip("Choose channels containing native Mix View tools, or show every channel. This only changes the list.");
    channelScope.addItem("With tools", 1);
    channelScope.addItem("All channels", 2);
    channelScope.setSelectedId(1, juce::dontSendNotification);
    channelScope.onChange = [this] {
        const auto scope = channelScope.getSelectedId() == 2 ? stack::ChannelScope::AllChannels
                                                           : stack::ChannelScope::WithTools;
        effects(model().setChannelScope(scope, clockNow()), false);
        layoutRows();
    };
    addAndMakeVisible(channelScope);
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
    if (root.devices.parameterDemand) root.devices.parameterDemand(this, {});
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
int StackTab::toolbarHeight() const noexcept {
    return kHeaderHeight * (getWidth() >= 720 ? 1 : getWidth() >= 370 ? 2 : 3);
}
juce::Rectangle<int> StackTab::display() const noexcept {
    return {look::gap, toolbarHeight(), std::max(0, getWidth() - 2 * look::gap), kDisplayHeight};
}
juce::Rectangle<int> StackTab::listArea() const noexcept {
    const int top = toolbarHeight() + kDisplayHeight + look::gap;
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
    const juce::Component::SafePointer<StackTab> alive(this);
    const auto frame = state_->frame, layout = state_->layout;
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
                    d.plugin ? stack::ItemClass::Plugin : stack::ItemClass::View, d.name, d.available, d.enabled,
                    !d.plugin && mix::functionForUid(d.uid) != nullptr});
        stack::tracksFromSnapshot(reader, root.arrangement.geometry, provider(), state_->chain, state_->input);
        effects(model().setRows(state_->input.input(generation, state_->revision), time), false);
        if (!alive || state_->frame != frame || state_->layout != layout) return;
        state_->generation = generation;
        state_->snapshot = reader.get();
        state_->provider = &provider();
        state_->hidden = hidden;
    }
    state_->structure = state_->publication;
}
void StackTab::frame(const SnapshotReader &reader) { frameAt(reader, clockNow()); }
void StackTab::frameAt(const SnapshotReader &reader, stack::TimeMs time) {
    if (root.devices.parameterDemand && (state_->parametersSuspended || !isShowing())) {
        updateParameterDemand({});
        return;
    }
    const juce::Component::SafePointer<StackTab> alive(this);
    const auto frame = ++state_->frame, layout = state_->layout;
    const auto current = [&] { return alive && state_->frame == frame && state_->layout == layout; };
    // Keep the prior record storage alive until every component has adopted the new feed.
    const auto old = state_->publication;
    (void)old; // its ownership deliberately lasts through every present() below
    if (root.devices.publication) {
        // A getter may destroy this surface or synchronously present a newer pose.
        // Validate before touching state, including the publication assignment itself.
        auto next = root.devices.publication();
        if (!current()) return;
        state_->publication = std::move(next);
    }
    if (!current()) return;
    refreshRows(reader, time);
    if (!current()) return;
    effects(model().setSelection(root.arrangement.geometry.selection.track, time), false);
    if (!current()) return;
    effects(model().advance(time), false);
    if (!current()) return;
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
    if (root.devices.parameterDemand && (state_->parametersSuspended || !isShowing())) {
        updateParameterDemand({});
        return;
    }
    const juce::Component::SafePointer<StackTab> alive(this);
    const auto frame = state_->frame, layout = ++state_->layout;
    const auto current = [&] { return alive && state_->frame == frame && state_->layout == layout; };
    state_->wanted.clear();
    const auto realised = model().realised();
    model().forEachRow(realised, [this](const stack::RowGeo &g) { state_->wanted.push_back(g); });
    for (const auto ref : model().retained())
        if (ref.valid())
            if (const auto at = model().shownIndex(ref.track); at && !realised.contains(*at))
                if (std::none_of(state_->wanted.begin(), state_->wanted.end(), [&](const auto &g) { return g.ref == ref; }))
                    state_->wanted.push_back(model().row(*at));
    const auto priorPublication = state_->publication;
    (void)priorPublication;
    if (root.devices.parameterDemand) {
        std::vector<std::int64_t> devices;
        for (const auto &geo : state_->wanted)
            model().forEachChainItem(geo.shown, [&](const stack::ItemGeo &item) {
                if (!item.pill && !root.devices.folded(item.ref.device)) devices.push_back(item.ref.device);
            });
        updateParameterDemand(std::move(devices));
        if (!current()) return;
        if (root.devices.publication) {
            auto next = root.devices.publication();
            if (!current()) return;
            state_->publication = std::move(next);
        }
    }
    for (std::size_t j = rows_.size(); j-- > 0;) {
        if (std::any_of(state_->wanted.begin(), state_->wanted.end(),
                       [&](const auto &g) { return g.ref == rows_[j]->state_->geo.ref; })) continue;
        auto retired = std::move(rows_[j]);
        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(j));
        retired.reset();
        if (!current()) return;
    }
    const auto count = state_->wanted.size();
    for (std::size_t j = 0; j < count; ++j) {
        if (!current()) return;
        const auto geo = state_->wanted[j];
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
        if (!current()) return;
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
    const juce::Component::SafePointer<StackTab> alive(this);
    if (fx.captureLost != stack::CaptureStatus::Live) cancelInteractions();
    if (!alive) return;
    if (fx.captureEnded != 0 && state_->rename.isVisible()) endRename(true);
    if (!alive) return;
    if (fx.select != 0) {
        root.arrangement.geometry.selection.track = fx.select;
        root.publishSelection();
        if (!alive) return;
    }
    if (fx.showDevices.valid()) {
        root.arrangement.geometry.selection.track = fx.showDevices.track;
        root.publishSelection();
        if (!alive) return;
        root.showDeviceView();
        if (!alive) return;
    }
    if (fx.takeFocus && takeFocus && isShowing()) grabKeyboardFocus();
    if (!alive) return;
    if (fx.escape == stack::EscapeStep::Capture) cancelInteractions();
    if (!alive) return;
    if (fx.escape == stack::EscapeStep::CloseMixView) root.setMixView(false, root.state().mixViewDetached);
    if (!alive) return;
    if (fx.rename.valid()) beginRename(fx.rename);
    if (fx.repaint) { state_->canvas.repaint(); repaint(); }
    if (fx.windowViewChanged) {
        rowSize.setValue(model().windowView().baseHeight, juce::dontSendNotification);
        autoZoom.setToggleState(model().windowView().autoZoom, juce::dontSendNotification);
        pinsOnly.setToggleState(model().windowView().filterPinned, juce::dontSendNotification);
        navigator.setToggleState(model().windowView().navigator, juce::dontSendNotification);
        channelScope.setSelectedId(model().windowView().channelScope == stack::ChannelScope::AllChannels ? 2 : 1,
                                   juce::dontSendNotification);
        if (search.getText().toStdString() != model().windowView().search) search.setText(model().windowView().search, false);
    }
}
void StackTab::resized() {
    const int available = std::max(0, getWidth() - 2 * look::gap);
    const bool oneRow = toolbarHeight() == kHeaderHeight;
    const bool compact = available < 476;
    channelScope.setBounds(look::gap, 4, 120, 22);
    search.setBounds(channelScope.getRight() + 6, 4,
                     std::max(50, std::min(230, available - (oneRow ? 610 : 126))), 22);
    const int secondY = oneRow ? 4 : kHeaderHeight + 4;
    rowSize.setBounds(oneRow ? search.getRight() + 8 : look::gap, secondY, compact ? 60 : 90, 22);
    autoZoom.setBounds(rowSize.getRight() + 6, secondY, compact ? 78 : 94, 22);
    const bool thirdRow = toolbarHeight() == kHeaderHeight * 3;
    const int lastY = thirdRow ? kHeaderHeight * 2 + 4 : secondY;
    pinsOnly.setBounds(thirdRow ? look::gap : autoZoom.getRight() + 4, lastY, compact ? 62 : 78, 22);
    navigator.setBounds(pinsOnly.getRight() + 4, lastY, compact ? 84 : 100, 22);
    state_->canvas.setBounds(listArea());
    state_->scrollbar.setBounds(listArea().getRight(), listArea().getY(), 8, listArea().getHeight());
    effects(model().setViewport(listArea().getWidth(), listArea().getHeight(), clockNow()), false);
    layoutRows();
}
juce::String StackTab::emptyMessage() const {
    if (!search.getText().isEmpty())
        return "No matching channels or devices.";
    if (model().windowView().filterPinned)
        return "No pinned channels in this view.";
    if (model().windowView().channelScope == stack::ChannelScope::WithTools)
        return "No channels with Mix View tools. Choose All channels, select a channel, then add a tool with + in Device View.";
    return "No channels in this view.";
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
        g.drawFittedText(emptyMessage(), listArea().reduced(16), juce::Justification::centred, 3);
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
stack::Hit StackTab::presentedHit(double x, double y) const {
    if (x < 0 || y < 0 || x >= listArea().getWidth() || y >= listArea().getHeight()) return {};
    // Navigator is painted by the model; actual chain controls and pills are
    // painted from the last completed row presentation below.
    if (model().windowView().navigator && x >= model().plan().navX) return model().hitTest(x, y);
    for (const auto &row : rows_) {
        const auto &pose = row->state_->presentedRow;
        if (!pose.ref.valid() || y < pose.top || y >= pose.top + pose.height) continue;
        stack::Hit hit;
        hit.row = pose.ref;
        hit.item = {pose.ref, stack::ItemPlace::None, 0, 0, pose.ref.gen};
        hit.shown = pose.shown;
        hit.tier = pose.tier;
        hit.part = stack::HitPart::RowSpace;
        hit.rect = {0, pose.top, static_cast<double>(row->getWidth()), pose.height};
        if (x < model().constants().chipColumn) {
            const auto &chip = row->state_->presentedChip;
            if (chip.pin.contains(x, y)) { hit.part = stack::HitPart::ChipPin; hit.rect = chip.pin; }
            else if (chip.collision.contains(x, y)) { hit.part = stack::HitPart::ChipCollision; hit.rect = chip.collision; }
            else if (chip.kick.contains(x, y)) { hit.part = stack::HitPart::ChipKick; hit.rect = chip.kick; }
            else if (pose.group && chip.fold.contains(x, y)) { hit.part = stack::HitPart::ChipFold; hit.rect = chip.fold; }
            else if (chip.chip.contains(x, y)) { hit.part = stack::HitPart::Chip; hit.rect = chip.chip; }
            return hit;
        }
        const auto &lane = row->state_->presentedLane;
        if (lane.right && lane.rightChevron.contains(x, y)) {
            hit.part = stack::HitPart::ChevronRight; hit.rect = lane.rightChevron; return hit;
        }
        if (lane.left && lane.leftChevron.contains(x, y)) {
            hit.part = stack::HitPart::ChevronLeft; hit.rect = lane.leftChevron; return hit;
        }
        if (x >= lane.x && x < lane.x + lane.width)
            for (const auto &item : row->state_->presentedItems) {
                const stack::Rect box{item.x, item.top, item.width, item.height};
                if (!box.contains(x, y)) continue;
                hit.item = item.ref; hit.rect = box; hit.tier = item.tier;
                hit.part = item.pill ? stack::HitPart::ChainPill : stack::HitPart::ChainItem;
                return hit;
            }
        return hit;
    }
    return {};
}
void StackTab::mouseMove(const juce::MouseEvent &e) {
    if (!fromList(e)) return;
    const auto p = modelPoint(e);
    effects(model().pointerMove(p.x, p.y, e.mods.isAnyMouseButtonDown(), clockNow()), false);
}
void StackTab::mouseEnter(const juce::MouseEvent &e) { mouseMove(e); }
void StackTab::mouseExit(const juce::MouseEvent &e) {
    const auto p = modelPoint(e);
    if (p.x < 0 || p.y < 0 || p.x >= static_cast<float>(listArea().getWidth()) ||
        p.y >= static_cast<float>(listArea().getHeight()))
        effects(model().pointerExit(clockNow()), false);
}
void StackTab::mouseDown(const juce::MouseEvent &e) {
    const juce::Component::SafePointer<StackTab> alive(this);
    if (!fromList(e)) return;
    const auto p = modelPoint(e);
    // Hit the geometry that supplied the visible components. Reflowing a newer document
    // here would hit an unseen row at this screen point. Validate the seen identity against
    // current state instead; the next frame adopts its new position.
    const auto presented = presentedHit(p.x, p.y);
    const auto current = root.current();
    if (presented.row.valid() && (!current || !current->findTrack(presented.row.track))) return;
    if (presented.item.place == stack::ItemPlace::Chain && !validTarget(presented.item)) return;
    const auto button = e.mods.isPopupMenu() ? stack::Button::Right
                      : e.mods.isMiddleButtonDown() ? stack::Button::Middle : stack::Button::Left;
    const auto fx = model().pointerDownPresented(p.x, p.y, button, mods(e.mods), e.getNumberOfClicks(), presented, clockNow());
    effects(fx, e.eventComponent == &state_->canvas);
    if (!alive) return;
    if (fx.hit.part == stack::HitPart::ChainPill && button == stack::Button::Left &&
        p.y >= fx.hit.rect.y && p.y < fx.hit.rect.y + 16 && validTarget(fx.hit.item)) {
        if (const auto *data = device(fx.hit.item.device); data && data->available)
            root.devices.action("device.setEnabled", {{"id", data->id}, {"enabled", !data->enabled}});
        if (!alive) return;
        layoutRows();
        return;
    }
    if ((fx.hit.part == stack::HitPart::ChainPill || fx.hit.part == stack::HitPart::ChainItem) &&
        e.getNumberOfClicks() > 1 && e.eventComponent == &state_->canvas && validTarget(fx.hit.item)) {
        root.devices.chose(fx.hit.item.device);
        if (!alive) return;
        root.showDeviceView();
        if (!alive) return;
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
bool StackTab::hasToolbarFocus() const {
    return channelScope.hasKeyboardFocus(true) || search.hasKeyboardFocus(true) || rowSize.hasKeyboardFocus(true) ||
           autoZoom.hasKeyboardFocus(true) || pinsOnly.hasKeyboardFocus(true) || navigator.hasKeyboardFocus(true);
}
bool StackTab::hasDeviceContext() const {
    if (!hasKeyboardFocus(true) || hasToolbarFocus() || state_->rename.isVisible()) return false;
    const auto target = model().focus().item;
    if (target.place != stack::ItemPlace::Chain) return false;
    for (const auto &row : rows_)
        for (const auto &tile : row->state_->tiles)
            if (tile.target == target && (tile.view->rename.isVisible() || tile.view->search.hasKeyboardFocus(true)))
                return false;
    return true;
}
bool StackTab::deviceCommand(int command) {
    if (hasToolbarFocus()) return false;
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
    if (hasToolbarFocus()) {
        const int code = key.getKeyCode();
        const bool editShortcut = key.getModifiers().isCommandDown() &&
            (code == 'x' || code == 'X' || code == 'c' || code == 'C' || code == 'v' || code == 'V' ||
             code == 'd' || code == 'D' || code == 'r' || code == 'R');
        // A toolbar's unhandled edit key must not reach a previously focused
        // insert or fall through to an unrelated track-level edit.
        return editShortcut || code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey;
    }
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
        updateParameterDemand({});
    }
}
void StackTab::parentHierarchyChanged() { visibilityChanged(); }
void StackTab::updateParameterDemand(std::vector<std::int64_t> devices) {
    std::sort(devices.begin(), devices.end());
    devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
    if (devices == state_->demanded) return;
    state_->demanded = std::move(devices);
    if (root.devices.parameterDemand) root.devices.parameterDemand(this, state_->demanded);
}
void StackTab::suspendParameters(bool suspended) {
    state_->parametersSuspended = suspended;
    if (suspended) {
        const auto safe = SafePointer<StackTab>(this);
        cancelInteractions();
        if (safe) updateParameterDemand({});
    }
}
void StackTab::focus(std::int64_t track) {
    const juce::Component::SafePointer<StackTab> alive(this);
    const auto time = clockNow();
    // The channel's explicit shortcut must still reach a chain before its first
    // tool is added. Change the visible scope instead of inventing membership or
    // creating a processor; other deliberate filters remain in force.
    if (model().hiddenOnlyByToolScope(track)) {
        effects(model().setChannelScope(stack::ChannelScope::AllChannels, time), false);
        if (!alive) return;
    }
    effects(model().openFromChip(track, {}, time));
    if (!alive) return;
    layoutRows();
}
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
    const juce::Component::SafePointer<StackTab> alive(this);
    if (state_->ending || state_->destroying) return false;
    if (!validTarget(target)) return false;
    if (state_->interaction) return state_->interaction->target == target && state_->interaction->kind == kind;
    // Child controls receive their own mouseDown before the canvas listener. Select their
    // stable target here as well, without stealing keyboard focus from the control.
    effects(model().focusItem(target, clockNow()), false);
    if (!alive) return false;
    stack::CaptureResult capture;
    capture.refusal = stack::CaptureRefusal::NotShown;
    for (const auto &row : rows_)
        if (row->state_->geo.ref == target.row) {
            capture = model().beginCapturePresented(kind, target, row->state_->presentedRow,
                row->state_->presentedLane, row->state_->presentedItems, clockNow());
            break;
        }
    if (capture.refusal != stack::CaptureRefusal::None) return false;
    state_->interaction = capture.ref;
    return true;
}
void StackTab::endInteraction() {
    if (state_->ending) return;
    state_->ending = true;
    const juce::Component::SafePointer<StackTab> alive(this);
    if (state_->began && state_->editData && !state_->parameter.empty())
        root.devices.editExpected(*state_->editData, state_->parameter, engine::ParamEventKind::End, state_->lastValue);
    if (!alive) return;
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
    const juce::Component::SafePointer<StackTab> alive(this);
    if (state_->ending) return false;
    if (kind == engine::ParamEventKind::Begin) {
        // A reset/new control gesture during a held drag is a new gesture, not a new
        // owner installed over an unclosed old one. End the exact original target first.
        if (state_->editData) endInteraction();
        if (!alive) return false;
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
    const auto interaction = state_->interaction;
    const bool accepted = root.devices.editExpected(*state_->editData, parameter, kind, value);
    if (!alive || state_->interaction != interaction) return accepted;
    if (accepted) state_->lastValue = value;
    if (kind == engine::ParamEventKind::Begin) state_->began = accepted;
    if (!accepted) endInteraction();
    return accepted;
}
void StackTab::cancelInteractions() {
    if (!state_ || state_->ending) return;
    const juce::Component::SafePointer<StackTab> alive(this);
    endRename(false);
    if (!alive) return;
    endInteraction();
    if (!alive) return;
    const auto layout = state_->layout;
    for (std::size_t r = 0; r < rows_.size(); ++r)
        for (std::size_t t = 0; t < rows_[r]->state_->tiles.size(); ++t) {
            const juce::Component::SafePointer<DevicePanelView> panel(rows_[r]->state_->tiles[t].view.get());
            panel->cancelInteractions();
            if (!alive || state_->layout != layout) return;
        }
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
