// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mix_focus.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include "adi/ui/note_geometry.hpp"
#include "adi/ui/track_order.hpp"
#include "adi/ui/arrangement_geometry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
namespace adi::ui {
namespace {
constexpr float matchDisplayDb = 24.f;
bool focusable(const engine::TrackNode &t) {
    return t.kind != "master" && t.kind != "main" && t.kind != "reference" &&
           (!t.role || *t.role != "reference");
}
} // namespace
MixFocus::MixFocus(AdiRootComponent &root) : root_(root) {
    setTitle("Focus");
    available.setTitle("Choose a channel for Focus");
    available.setTextWhenNothingSelected("Choose channel");
    add.setTitle("Add channel to Focus");
    add.onClick = [this] {
        const auto at = available.getSelectedId() - 1;
        if (at < 0 || static_cast<size_t>(at) >= choices_.size())
            return;
        const auto id = choices_[static_cast<size_t>(at)];
        std::vector<std::int64_t> ids;
        for (const auto &m : members_)
            ids.push_back(m.id);
        ids.push_back(id);
        setMembers(ids);
        select(id);
    };
    star.onClick = [this] {
        main_ = active_;
        refreshControls();
        redraw_ = true;
        repaint();
        if (onMembersChanged) onMembersChanged();
    };
    mute.onClick = [this] {
        if (active_)
            root_.activateTrack(active_, false);
    };
    solo.onClick = [this] { toggleMemberSolo(active_); };
    hearOne.onClick = [this] { hear(false); };
    hearAll.onClick = [this] { hear(true); };
    edit.onClick = [this] {
        if (onEdit && contains(active_) && SnapshotReader(root_.current()).findTrack(active_))
            onEdit(active_);
    };
    scope.setTitle("Synchronous Focus scope");
    scope.setTooltip("Show the Focus channels' post-fader waveforms and their sum when aligned. "
                     "Ctrl/Cmd+wheel zooms time. Shift+wheel pans time. Alt+wheel and Alt+Shift+wheel adjust the level axis.");
    scope.onClick = [this] {
        if (!scope.getToggleState()) thawScope();
        auto retired = scope.getToggleState() ? SharedAnalysis::BarScopeLease{} : std::move(barScope_);
        if (!scope.getToggleState()) clearBarScopeData();
        updateMatchControls();
        updateScopeChannelDemand();
        resized();
        redraw_ = true;
        repaint();
    };
    scopeDisplay_.setTitle("Focus Scope display");
    scopeDisplay_.setTooltip("Difference shows starred Main minus the selected channel.");
    scopeDisplay_.addItem("Overlay", static_cast<int>(ScopeDisplay::Overlay));
    scopeDisplay_.addItem("Layers", static_cast<int>(ScopeDisplay::Layers));
    scopeDisplay_.addItem("Stack", static_cast<int>(ScopeDisplay::Stack));
    scopeDisplay_.addItem("Sum", static_cast<int>(ScopeDisplay::Sum));
    scopeDisplay_.addItem("Difference", static_cast<int>(ScopeDisplay::Difference));
    scopeDisplay_.setSelectedId(static_cast<int>(ScopeDisplay::Overlay), juce::dontSendNotification);
    scopeDisplay_.onChange = [this] {
        thawScope();
        clearScopeRegion();
        scopeCursor_.setVisible(false);
        updateScopeDifference();
        updateScopeTruePeakControls();
        updateScopeChannelDemand();
        if (phaseBar_.isEmpty() != showsScopeDifference()) resized();
        redraw_ = true;
        repaint();
    };
    addChildComponent(scopeDisplay_);
    scopeChannels_.setTitle("Focus Scope channels");
    scopeChannels_.setTooltip("Show Stereo, one channel, Mid (L + R) / 2 or Side (L - R) / 2.");
    scopeChannels_.addItem("Stereo", static_cast<int>(FocusScopeChannels::Stereo));
    scopeChannels_.addItem("Left", static_cast<int>(FocusScopeChannels::Left));
    scopeChannels_.addItem("Right", static_cast<int>(FocusScopeChannels::Right));
    scopeChannels_.addItem("Mid", static_cast<int>(FocusScopeChannels::Mid));
    scopeChannels_.addItem("Side", static_cast<int>(FocusScopeChannels::Side));
    scopeChannels_.setSelectedId(static_cast<int>(FocusScopeChannels::Stereo), juce::dontSendNotification);
    scopeChannels_.onChange = [this] {
        thawScope();
        clearScopeRegion();
        scopeCursor_.setVisible(false);
        updateScopeDifference();
        updateScopeTruePeakControls();
        updateScopeChannelDemand();
        redraw_ = true;
    };
    addChildComponent(scopeChannels_);
    scopeWindow_.setTitle("Scope window");
    scopeWindow_.setTooltip("Choose a musical window or a fixed-time window that free-runs from the heard sample.");
    scopeWindow_.addItem("1/16 Note", static_cast<int>(FocusScopeWindow::Sixteenth));
    scopeWindow_.addItem("One Beat", static_cast<int>(FocusScopeWindow::Beat));
    scopeWindow_.addItem("One Bar", static_cast<int>(FocusScopeWindow::Bar));
    scopeWindow_.addItem("Two Bars", static_cast<int>(FocusScopeWindow::TwoBars));
    scopeWindow_.addItem("Four Bars", static_cast<int>(FocusScopeWindow::FourBars));
    scopeWindow_.addSeparator();
    for (const auto window : {FocusScopeWindow::Milliseconds85, FocusScopeWindow::Milliseconds170,
                              FocusScopeWindow::Milliseconds340})
        scopeWindow_.addItem(juce::String(focusScopeWindowMilliseconds(window)) + " ms", static_cast<int>(window));
    scopeWindow_.setSelectedId(static_cast<int>(FocusScopeWindow::Bar), juce::dontSendNotification);
    scopeWindow_.onChange = [this] {
        const SafePointer<MixFocus> alive(this);
        thawScope();
        if (!alive) return;
        shownBar_.reset();
        requestedScope_.reset();
        scopeWindowAwaiting_ = true;
        clearScopeRegion();
        scopeCursor_.setVisible(false);
        rebuildScope();
        redraw_ = true;
        repaint();
    };
    addChildComponent(scopeWindow_);
    scopeLayer_.setTitle("Selected scope layer");
    scopeLayer_.setTooltip("Choose the selected layer's channel view, polarity and visual timing.");
    scopeLayer_.onClick = [this] { showScopeLayerMenu(); };
    addChildComponent(scopeLayer_);
    differenceSpectrum_.setTitle("Difference spectrum");
    differenceSpectrum_.setTooltip("Show the spectrum of the two aligned scope channels after subtraction.");
    differenceSpectrum_.onClick = [this] { updateScopeDifference(); repaint(); };
    addChildComponent(differenceSpectrum_);
    scopeFreeze_.setTitle("Freeze Focus Scope");
    scopeFreeze_.setTooltip("Hold the displayed waveform and measurement.");
    scopeFreeze_.setEnabled(false);
    scopeFreeze_.onClick = [this] {
        if (scopeFreeze_.getToggleState()) freezeScope();
        else thawScope();
    };
    addChildComponent(scopeFreeze_);
    learnMatch.setTooltip("Capture the selected channel's spectrum difference toward the starred channel. Plays unchanged.");
    learnMatch.onClick = [this] { learnMatchPreview(); };
    clearMatch.onClick = [this] { clearMatchPreview(); updateMatchControls(); };
    alignMeasure_.setTitle("Measure Focus alignment");
    alignMeasure_.setTooltip("Measure the selected channel's timing against Main. Playback stays unchanged.");
    alignMeasure_.onClick = [this] { measureAlign(); };
    alignReadout_.setTitle("Focus alignment preview");
    alignReadout_.setFont(look::font());
    alignReadout_.setJustificationType(juce::Justification::centredRight);
    alignReadout_.setColour(juce::Label::textColourId, look::textDim);
    addChildComponent(alignReadout_);
    scopeCursor_.setTitle("Scope cursor");
    scopeCursor_.setJustificationType(juce::Justification::centredRight);
    scopeCursor_.setColour(juce::Label::textColourId, look::text);
    scopeCursor_.setColour(juce::Label::backgroundColourId, look::panel);
    scopeCursor_.setInterceptsMouseClicks(false, false);
    addChildComponent(scopeCursor_);
    scopeRegionReadout_.setTitle("Scope region");
    scopeRegionReadout_.setJustificationType(juce::Justification::centredRight);
    scopeRegionReadout_.setColour(juce::Label::textColourId, look::text);
    scopeRegionReadout_.setColour(juce::Label::backgroundColourId, look::panel);
    scopeRegionReadout_.setInterceptsMouseClicks(false, false);
    addChildComponent(scopeRegionReadout_);
    differenceRmsReadout_.setTitle("Scope Difference RMS");
    differenceRmsReadout_.setJustificationType(juce::Justification::centredRight);
    differenceRmsReadout_.setColour(juce::Label::textColourId, look::text);
    differenceRmsReadout_.setColour(juce::Label::backgroundColourId, look::panel);
    differenceRmsReadout_.setInterceptsMouseClicks(false, false);
    addChildComponent(differenceRmsReadout_);
    scopeTruePeak_.setTitle("Focus Scope true peak");
    scopeTruePeak_.setTooltip("Measure the captured stereo sum with silence outside the displayed interval.");
    scopeTruePeak_.onClick = [this] {
        updateScopeTruePeakControls();
        rebuildScope();
        redraw_ = true;
        repaint();
    };
    addChildComponent(scopeTruePeak_);
    scopeTruePeakReadout_.setTitle("Scope captured true peak");
    scopeTruePeakReadout_.setJustificationType(juce::Justification::centredRight);
    scopeTruePeakReadout_.setColour(juce::Label::textColourId, look::text);
    scopeTruePeakReadout_.setColour(juce::Label::backgroundColourId, look::panel);
    scopeTruePeakReadout_.setInterceptsMouseClicks(false, false);
    addChildComponent(scopeTruePeakReadout_);
    scroll_.setViewedComponent(&strip_, false);
    scroll_.setScrollBarsShown(false, true);
    for (auto *c : std::array<juce::Component *, 13>{&available, &add, &star, &mute, &solo, &hearOne, &hearAll,
                                                     &edit, &scope, &learnMatch, &clearMatch, &alignMeasure_, &scroll_})
        addAndMakeVisible(*c);
    refreshControls();
}
MixFocus::~MixFocus() {
    unwatchAncestors();
    scroll_.setViewedComponent(nullptr);
}
bool MixFocus::contains(std::int64_t id) const noexcept {
    return std::any_of(members_.begin(), members_.end(), [id](const auto &member) { return member.id == id; });
}
void MixFocus::toggleMemberSolo(std::int64_t id) {
    if (!isShowing() || suspended_ || !contains(id)) return;
    const SnapshotReader snapshot(root_.current());
    const auto *track = snapshot.findTrack(id);
    if (!track || !focusable(*track)) return;
    const bool removing = track->soloed;
    const SafePointer<MixFocus> alive(this);
    if (!root_.submitEdit("track.setSolo", {{"id", id}, {"soloed", !removing}}, "Focus solo") || !alive) return;
    if (removing) {
        auto kept = channelIds();
        std::erase(kept, id);
        setMembers(kept, true);
    } else {
        refreshControls();
    }
}
std::vector<std::int64_t> MixFocus::soloedChannels(const SnapshotReader &reader) {
    std::vector<TrackRow> rows;
    rowsFromSnapshot(reader, rows);
    TrackOrder order;
    order.build(reader, rows);
    std::vector<std::int64_t> ids;
    for (const auto &place : order.places()) {
        const auto &track = *reader.tracks()[place.at];
        if (track.soloed && focusable(track))
            ids.push_back(track.id);
    }
    return ids;
}
void MixFocus::setMembers(const std::vector<std::int64_t> &ids) {
    setMembers(ids, false);
}
void MixFocus::setMembers(const std::vector<std::int64_t> &ids, bool promoteSoloed) {
    thawScope();
    const SnapshotReader snapshot(root_.current());
    std::vector<Member> next;
    for (auto id : ids) {
        const auto *t = snapshot.findTrack(id);
        if (!t || !focusable(*t) ||
            std::any_of(next.begin(), next.end(), [id](auto &m) { return m.id == id; }))
            continue;
        const auto found =
            std::find_if(members_.begin(), members_.end(), [id](auto &m) { return m.id == id; });
        if (found != members_.end()) {
            next.push_back(std::move(*found));
            continue;
        }
        Member m;
        m.id = id;
        m.chip = std::make_unique<juce::TextButton>();
        m.chip->onClick = [this, id] { select(id); };
        strip_.addAndMakeVisible(*m.chip);
        next.push_back(std::move(m));
    }
    // Retired leases may run a source-detach callback. Publish the new membership
    // and finish its guarded owner callbacks before allowing those releases.
    auto retired = std::move(members_);
    auto retiredBar = std::move(barScope_);
    members_ = std::move(next);
    ++membershipPose_;
    updateScopeLayerAdjustments();
    clearBarScopeData();
    clearScopeSum();
    const auto exists = [this](auto id) {
        return std::any_of(members_.begin(), members_.end(), [id](auto &m) { return m.id == id; });
    };
    if (!exists(main_)) {
        const auto first = std::find_if(members_.begin(), members_.end(), [&](const auto &member) {
            return !promoteSoloed || snapshot.findTrack(member.id)->soloed;
        });
        main_ = first == members_.end() ? 0 : first->id;
    }
    if (!exists(active_)) {
        active_ = main_;
        if (active_ == 0 && !members_.empty())
            active_ = members_.front().id;
    }
    refreshControls();
    resized();
    redraw_ = true;
    repaint();
    if (onMembersChanged) onMembersChanged();
}
std::vector<std::int64_t> MixFocus::channelIds() const {
    std::vector<std::int64_t> ids;
    ids.reserve(members_.size());
    for (const auto& channel : members_) ids.push_back(channel.id);
    return ids;
}
void MixFocus::select(std::int64_t id) {
    if (!contains(id) || !SnapshotReader(root_.current()).findTrack(id)) return;
    active_ = id;
    refreshControls();
}
void MixFocus::refreshControls() {
    // The initial refresh runs while AdiRootComponent is still constructing its panel.
    const SnapshotReader snapshot(members_.empty() ? SnapshotPtr{} : root_.current());
    // Each chip names its track as the track's header does (walk-B B13).
    const auto shown = members_.empty() ? std::map<std::int64_t, std::string>{} : shownTrackNames(snapshot);
    for (auto &m : members_)
        if (const auto *t = snapshot.findTrack(m.id)) {
            const auto named = shown.find(m.id);
            const auto &name = named != shown.end() ? named->second : t->name;
            m.colour =
                t->color ? look::paletteColour(*t->color) : look::trackColour(static_cast<std::size_t>(static_cast<int>(m.id % 70)));
            m.chip->setButtonText((main_ == m.id ? juce::String::fromUTF8("\xe2\x98\x85 ") : juce::String()) +
                                  juce::String::fromUTF8(name.c_str()));
            m.chip->setColour(juce::TextButton::textColourOffId, m.colour.brighter());
            m.chip->setToggleState(m.id == active_, juce::dontSendNotification);
        }
    const auto *t = snapshot.findTrack(active_);
    for (auto *button : {&star, &mute, &solo, &hearOne, &hearAll, &edit})
        button->setEnabled(t != nullptr);
    mute.setToggleState(t && t->muted, juce::dontSendNotification);
    solo.setToggleState(t && t->soloed, juce::dontSendNotification);
    updateMatchControls();
}
std::array<const MixFocus::Member *, 2> MixFocus::matchMembers() const noexcept {
    std::array<const Member *, 2> pair{};
    if (!main_ || main_ == active_) return pair;
    for (const auto &member : members_) {
        if (member.id == main_) pair[0] = &member;
        if (member.id == active_) pair[1] = &member;
        if (pair[0] && pair[1]) break;
    }
    return pair;
}
bool MixFocus::canLearnMatch(const std::array<const Member *, 2> &pair) const noexcept {
    return isShowing() && !suspended_ && !scope.getToggleState() && scopePlaying_ &&
           alignment_ == Alignment::Ready && pair[0] && pair[1] && pair[0]->data && pair[1]->data &&
           focusMatchCompatible(*pair[0]->data, *pair[1]->data) && matchCaptureIsCurrent(*pair[0]->data);
}
bool MixFocus::matchCaptureIsCurrent(const SharedAnalysis::Result &capture) const noexcept {
    const auto locateRevision = root_.settledLocateRevision();
    return locateRevision && capture.transportRevision >= *locateRevision;
}
void MixFocus::clearMatchPreview() {
    if (!match_) return;
    match_.reset();
    matchBroker_.reset();
    matchPath_.clear();
    matchCaption_.clear();
    repaint();
}
void MixFocus::updateMatchControls() {
    const auto pair = matchMembers();
    if (match_) {
        const auto replaced = [](const Member &member, std::uint64_t generation) {
            if (member.data) return member.data->generation != generation;
            return member.lease.available() && member.lease.sourcePosition().generation != generation;
        };
        if (!pair[0] || !pair[1] || match_->main != pair[0]->id || match_->target != pair[1]->id ||
            replaced(*pair[0], match_->mainGeneration) || replaced(*pair[1], match_->targetGeneration))
            clearMatchPreview();
    }
    const bool together = !scope.getToggleState();
    const bool hasPair = members_.size() >= 2;
    const auto difference = static_cast<int>(ScopeDisplay::Difference);
    scopeDisplay_.setItemEnabled(difference, hasPair);
    if (!hasPair && scopeDisplay_.getSelectedId() == difference)
        scopeDisplay_.setSelectedId(static_cast<int>(ScopeDisplay::Overlay), juce::dontSendNotification);
    scopeDisplay_.setVisible(!together);
    scopeChannels_.setVisible(!together);
    scopeWindow_.setVisible(!together);
    scopeWindow_.setEnabled(!members_.empty());
    scopeLayer_.setVisible(!together);
    scopeFreeze_.setVisible(!together);
    updateScopeLayerControl();
    learnMatch.setVisible(together);
    clearMatch.setVisible(together && match_.has_value());
    learnMatch.setEnabled(canLearnMatch(pair));
    refreshAlign(pair);
    updateScopeDifference();
    updateScopeTruePeakControls();
}
MixFocus::Member *MixFocus::selectedScopeLayer() noexcept {
    const auto found = std::ranges::find(members_, active_, &Member::id);
    return found == members_.end() ? nullptr : &*found;
}

juce::PopupMenu MixFocus::scopeLayerMenu(const Member &layer) const {
    const double sampleRate = shownBar_ ? shownBar_->sampleRate : layer.data ? layer.data->sampleRate : 0.;
    const auto offset = layer.scopeAdjustment.offsetSamples;
    const auto format = [&](int samples) {
        if (scopeOffsetsInMilliseconds_ && sampleRate > 0.)
            return juce::String(static_cast<double>(samples) * 1000. / sampleRate, 3) + " ms";
        return juce::String(samples) + (samples == 1 || samples == -1 ? " sample" : " samples");
    };
    juce::PopupMenu menu;
    menu.addSectionHeader(layer.chip->getButtonText() + "  " + (offset > 0 ? "+" : "") + format(offset));
    const auto item = [&](ScopeLayerAction action, const juce::String &text, bool enabled = true, bool checked = false) {
        menu.addItem(static_cast<int>(action), text, enabled, checked);
    };
    item(ScopeLayerAction::Invert, "Invert polarity", true, layer.scopeAdjustment.inverted);
    item(ScopeLayerAction::EarlierCoarse, "Earlier by " + format(10));
    item(ScopeLayerAction::EarlierFine, "Earlier by " + format(1));
    item(ScopeLayerAction::LaterFine, "Later by " + format(1));
    item(ScopeLayerAction::LaterCoarse, "Later by " + format(10));
    menu.addSeparator();
    juce::PopupMenu channels;
    const auto channel = [&](ScopeLayerAction action, const char *label, std::optional<FocusScopeChannels> view) {
        channels.addItem(static_cast<int>(action), label, true, layer.scopeChannel == view);
    };
    channel(ScopeLayerAction::FollowWindow, "Follow window", std::nullopt);
    channel(ScopeLayerAction::Stereo, "Stereo", FocusScopeChannels::Stereo);
    channel(ScopeLayerAction::Left, "Left", FocusScopeChannels::Left);
    channel(ScopeLayerAction::Right, "Right", FocusScopeChannels::Right);
    channel(ScopeLayerAction::Mid, "Mid", FocusScopeChannels::Mid);
    channel(ScopeLayerAction::Side, "Side", FocusScopeChannels::Side);
    menu.addSubMenu("Channels", std::move(channels));
    item(ScopeLayerAction::Samples, "Show samples", true, !scopeOffsetsInMilliseconds_);
    item(ScopeLayerAction::Milliseconds, "Show milliseconds", true, scopeOffsetsInMilliseconds_);
    item(ScopeLayerAction::Reset, "Reset layer", layer.scopeAdjustment != FocusScopeLayerAdjustment{} || layer.scopeChannel);
    return menu;
}

void MixFocus::updateScopeLayerControl() {
    if (!scope.getToggleState()) return;
    const auto *selected = selectedScopeLayer();
    scopeLayer_.setEnabled(selected != nullptr);
    scopeLayer_.setButtonText(selected && (selected->scopeAdjustment != FocusScopeLayerAdjustment{} || selected->scopeChannel)
        ? "Layer*" : "Layer");
}

void MixFocus::showScopeLayerMenu() {
    const auto *selected = selectedScopeLayer();
    if (!scope.getToggleState() || !selected) return;
    auto menu = scopeLayerMenu(*selected);
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&scopeLayer_),
        [safe = SafePointer<MixFocus>(this), track = selected->id, membership = membershipPose_](int choice) {
            if (safe && choice != 0)
                safe->applyScopeLayerAction(track, membership, static_cast<ScopeLayerAction>(choice));
        });
}

void MixFocus::applyScopeLayerAction(std::int64_t track, std::uint64_t membership, ScopeLayerAction action) {
    if (membership != membershipPose_ || track != active_ || !scope.getToggleState()) return;
    auto *layer = selectedScopeLayer();
    if (!layer) return;
    auto adjustment = layer->scopeAdjustment;
    auto channel = layer->scopeChannel;
    switch (action) {
    case ScopeLayerAction::Invert: adjustment.inverted = !adjustment.inverted; break;
    case ScopeLayerAction::EarlierCoarse: adjustment.offsetSamples -= 10; break;
    case ScopeLayerAction::EarlierFine: --adjustment.offsetSamples; break;
    case ScopeLayerAction::LaterFine: ++adjustment.offsetSamples; break;
    case ScopeLayerAction::LaterCoarse: adjustment.offsetSamples += 10; break;
    case ScopeLayerAction::Samples: scopeOffsetsInMilliseconds_ = false; return;
    case ScopeLayerAction::Milliseconds: scopeOffsetsInMilliseconds_ = true; return;
    case ScopeLayerAction::Reset: adjustment = {}; channel.reset(); break;
    case ScopeLayerAction::FollowWindow: channel.reset(); break;
    case ScopeLayerAction::Stereo: channel = FocusScopeChannels::Stereo; break;
    case ScopeLayerAction::Left: channel = FocusScopeChannels::Left; break;
    case ScopeLayerAction::Right: channel = FocusScopeChannels::Right; break;
    case ScopeLayerAction::Mid: channel = FocusScopeChannels::Mid; break;
    case ScopeLayerAction::Side: channel = FocusScopeChannels::Side; break;
    }
    const int limit = SharedAnalysis::kMaxFftSize;
    adjustment.offsetSamples = std::clamp(adjustment.offsetSamples, -limit, limit);
    const bool adjusted = adjustment != layer->scopeAdjustment;
    if (!adjusted && channel == layer->scopeChannel) return;
    const auto view = scopeChannelView();
    const auto previous = layer->scopeChannel.value_or(view);
    const auto next = channel.value_or(view);
    layer->scopeAdjustment = adjustment;
    layer->scopeChannel = channel;
    const auto display = static_cast<ScopeDisplay>(scopeDisplay_.getSelectedId());
    if (!adjusted && (display == ScopeDisplay::Sum || display == ScopeDisplay::Difference)) {
        updateScopeLayerControl();
        return;
    }
    const auto dirty = plot_.toNearestInt().withTop(scroll_.getBottom()).getUnion(sumLabel_).getUnion(phaseBar_);
    if (!adjusted && (previous == next || (!focusScopeIsFolded(previous) && !focusScopeIsFolded(next)))) {
        updateScopeLayerControl();
        updateScopeChannelDemand();
        if (previous != next) rebuildScope();
        repaint(dirty);
        return;
    }
    const SafePointer<MixFocus> alive(this);
    const auto thawedPose = membership + (frozenScopeRevision_ || stoppedScopeRevision_ ? 1 : 0);
    thawScope();
    if (!alive || thawedPose != membershipPose_) return;
    updateScopeLayerControl();
    updateScopeLayerAdjustments();
    updateScopeChannelDemand();
    shownBar_.reset();
    redraw_ = true;
    if (scopePlaying_) rebuildScope();
    repaint(dirty);
}

void MixFocus::updateScopeLayerAdjustments() {
    scopeAdjustments_.clear();
    scopeAdjustments_.reserve(members_.size());
    for (const auto &layer : members_) scopeAdjustments_.push_back(layer.scopeAdjustment);
    barScope_.setLayerAdjustments(scopeAdjustments_);
}

void MixFocus::updateScopeChannelDemand() {
    if (!barScope_) return;
    const auto display = static_cast<ScopeDisplay>(scopeDisplay_.getSelectedId());
    barScope_.setMonoRequested(display == ScopeDisplay::Difference ? FocusScopeChannels::Stereo : scopeChannelView());
    scopeLayerViews_.clear();
    const bool showLayers = scope.getToggleState() && display != ScopeDisplay::Sum && display != ScopeDisplay::Difference;
    if (showLayers && std::ranges::any_of(members_, [](const auto &member) { return member.scopeChannel.has_value(); })) {
        scopeLayerViews_.reserve(members_.size());
        for (const auto &member : members_) scopeLayerViews_.push_back(member.scopeChannel);
    }
    barScope_.setLayerChannelViews(scopeLayerViews_);
}

bool MixFocus::canMeasureAlign(const std::array<const Member *, 2> &pair) const noexcept {
    if (!scopePlaying_ || suspended_ || scope.getToggleState() || alignment_ != Alignment::Ready ||
        !isShowing() || plot_.isEmpty() || !pair[0] || !pair[1] || !pair[0]->data || !pair[1]->data)
        return false;
    const auto floor = root_.settledLocateRevision();
    const auto &main = *pair[0]->data;
    const auto &recipient = *pair[1]->data;
    return floor && main.transportRevision && main.transportRevision >= *floor &&
           main.transportRevision == recipient.transportRevision && main.advancing && recipient.advancing;
}
void MixFocus::showAlign(const juce::String &message) {
    alignReadout_.setText(message, juce::dontSendNotification);
    alignReadout_.setTooltip(message);
    alignReadout_.setVisible(message.isNotEmpty());
    repaint(titleRow_);
}
void MixFocus::clearAlign() {
    alignJob_.reset();
    alignCapture_.reset();
    alignPending_ = false;
    if (alignReadout_.isVisible()) showAlign({});
}
void MixFocus::refreshAlign(const std::array<const Member *, 2> &pair) {
    const auto eligible = canMeasureAlign(pair);
    if (!eligible) {
        clearAlign();
    } else if (alignCapture_) {
        const auto &request = *alignCapture_;
        const auto matches = [&request](const Member &member, std::size_t side) {
            const auto &data = *member.data;
            return request.transportRevision == data.transportRevision &&
                   request.sources[side] == FocusScopeBarSource{data.request.source(), data.generation};
        };
        if (!matches(*pair[0], 0) || !matches(*pair[1], 1) ||
            (alignJob_ && !alignPending_ && alignJob_.status() != SharedAnalysis::FocusLagStatus::Ready))
            clearAlign();
    }
    alignMeasure_.setVisible(!scope.getToggleState());
    alignMeasure_.setEnabled(eligible && !alignPending_);
}
void MixFocus::measureAlign() {
    const auto pair = matchMembers();
    if (alignPending_ || !canMeasureAlign(pair) || !root_.playing() || !plotShowing()) return;
    const auto broker = broker_.lock();
    const SnapshotReader reader(root_.current());
    const auto &transport = reader.transport();
    if (!broker || !broker->active() || root_.sharedAnalysis != broker ||
        (transport.loopEnabled && transport.hasLoop)) return;

    clearAlign();
    const auto revision = pair[0]->data->transportRevision;
    const auto request = focusLagPreviewRequest({pair[0]->data.get(), pair[1]->data.get()},
        {pair[0]->lease.sourcePosition(), pair[1]->lease.sourcePosition()}, revision,
        root_.renderedSamplePosition());
    if (!request || !(alignJob_ = broker->requestFocusLag(*request))) {
        showAlign("No current timing measurement");
        return;
    }
    alignCapture_ = request;
    alignRate_ = pair[0]->data->sampleRate;
    alignPending_ = true;
    alignMeasure_.setEnabled(false);
    showAlign("Measuring alignment...");
}
void MixFocus::pollAlign() {
    if (!alignPending_ || alignJob_.status() == SharedAnalysis::FocusLagStatus::Pending) return;
    const auto result = alignJob_.read();
    alignPending_ = false;
    if (result) {
        const auto signedCorrelation = [](double score) {
            return (score < 0. ? juce::String{} : juce::String("+")) + juce::String(score, 2);
        };
        const auto lag = std::abs(result->lagSamples);
        const auto timing = lag == 0 ? juce::String("Aligned") :
            juce::String(result->lagSamples > 0 ? "Recipient late " : "Recipient early ") +
                juce::String(lag) + " samples (" + juce::String(1000. * lag / alignRate_, 3) + " ms)";
        showAlign(timing + "   Mid corr. " + signedCorrelation(result->unalignedCorrelation) + " to " +
                  signedCorrelation(result->correlation));
    } else {
        alignJob_.reset();
        showAlign("No clear timing match");
    }
    refreshAlign(matchMembers());
}
void MixFocus::learnMatchPreview() {
    const auto pair = matchMembers();
    if (!canLearnMatch(pair)) return;
    auto owner = broker_.lock();
    if (!owner || !owner->active() || root_.sharedAnalysis != owner || !root_.playing()) return;
    const SnapshotReader reader(root_.current());
    const auto &transport = reader.transport();
    if (transport.loopEnabled && transport.hasLoop) return;
    for (const auto *member : pair)
        if (member->lease.sourcePosition().generation != member->data->generation) return;
    auto learned = focusReadMatch(*pair[0]->data, *pair[1]->data);
    if (!learned) return;
    const auto *main = reader.findTrack(learned->main);
    const auto *target = reader.findTrack(learned->target);
    if (!main || !target) return;
    const auto shown = shownTrackNames(reader); // as the headers show them (walk-B B13)
    const auto nameOf = [&shown](const engine::TrackNode &t) {
        const auto at = shown.find(t.id);
        return juce::String::fromUTF8((at != shown.end() ? at->second : t.name).c_str());
    };
    auto caption = " / Match " + nameOf(*target) + " toward " + nameOf(*main) + " (+/-" +
                   juce::String(static_cast<int>(matchDisplayDb)) + " dB)";
    if (!matchCaptureIsCurrent(*pair[0]->data)) return;
    matchCaption_ = std::move(caption);
    match_ = std::move(learned);
    matchBroker_ = owner;
    updateMatchControls();
    rebuildMatchPath();
    repaint();
}
void MixFocus::rebuildMatchPath() {
    matchPath_.clear();
    if (!match_ || plot_.isEmpty()) return;
    bool drewPoint = false;
    for (std::size_t bin = 1; bin < match_->deltaDb.size(); ++bin) {
        const auto db = match_->deltaDb[bin];
        if (!db) {
            drewPoint = false;
            continue;
        }
        const double hz = static_cast<double>(bin) * match_->sampleRate / match_->fftSize;
        const float x = plot_.getX() + static_cast<float>(std::log(hz / 20.) / std::log(1000.)) * plot_.getWidth();
        const float y = plot_.getCentreY() - std::clamp(*db / matchDisplayDb, -1.f, 1.f) * plot_.getHeight() / 2.f;
        if (drewPoint) {
            matchPath_.lineTo(x, y);
        } else {
            // A single measurable bin can occur with narrow tones.
            matchPath_.startNewSubPath(x, y);
            matchPath_.lineTo(std::min(x + look::hairline, plot_.getRight()), y);
        }
        drewPoint = true;
    }
}
void MixFocus::hear(bool group) {
    if (!active_)
        return;
    const auto current = root_.current();
    if (!current)
        return;
    std::vector<OpRequest> ops;
    for (const auto &t : current->tracks)
        if (focusable(*t)) {
            const bool hearTrack = group ? std::any_of(members_.begin(), members_.end(),
                                                       [&](const auto &m) { return m.id == t->id; })
                                         : t->id == active_;
            if (t->soloed == hearTrack)
                continue;
            OpRequest req;
            req.opType = "track.setSolo";
            req.label = group ? "Hear Focus group" : "Hear Focus channel";
            req.payload = {{"id", t->id}, {"soloed", hearTrack}};
            ops.push_back(std::move(req));
        }
    if (!ops.empty())
        root_.submitEdits(std::move(ops));
}
void MixFocus::drop() {
    ++membershipPose_;
    if (frozenScopeRevision_ || stoppedScopeRevision_) alignment_ = Alignment::WaitForFresh;
    frozenScopeRevision_.reset();
    stoppedScopeRevision_.reset();
    scopeLocate_.reset();
    scopeCommandPending_ = false;
    scopeFreeze_.setToggleState(false, juce::dontSendNotification);
    scopeFreeze_.setEnabled(false);
    clearAlign();
    auto retiredBar = std::move(barScope_);
    std::vector<SharedAnalysis::Lease> released;
    released.reserve(members_.size());
    for (auto &m : members_) {
        released.push_back(std::move(m.lease));
        m.outputs = 0;
        m.data.reset();
        m.energy.clear();
        m.spectrumChanged = true;
        m.path.clear();
        for (auto &path : m.scopePaths) path.clear();
    }
    scopeSources_.clear();
    clearBarScopeData();
    scopeGridStamp_.reset();
    clearScopeSum();
    clearScopeRegion();
    scopeCursorPoint_.reset();
    scopeCursor_.setVisible(false);
    scopeHasTrace_ = false;
    masked_.clear();
    broker_.reset();
    updateMatchControls();
    redraw_ = true;
}
void MixFocus::dropIfActive() {
    if (frozenScopeRevision_ || stoppedScopeRevision_ || barScope_ || shownBar_ || !broker_.expired() ||
        std::any_of(members_.begin(), members_.end(), [](const auto &member) { return member.outputs != 0; }))
        drop();
}
void MixFocus::suspend(bool v) {
    if (v == suspended_) return;
    suspended_ = v;
    if (v) dropIfActive();
}
void MixFocus::visibilityChanged() {
    if (!plotShowing())
        dropIfActive();
}
bool MixFocus::plotShowing() const noexcept {
    if (!isShowing() || plot_.isEmpty()) return false;
    auto area = plot_.toNearestInt();
    for (auto *parent = getParentComponent(); parent; parent = parent->getParentComponent()) {
        area = area.getIntersection(getLocalArea(parent, parent->getLocalBounds()));
        if (area.isEmpty()) return false;
    }
    return !area.isEmpty();
}
void MixFocus::parentHierarchyChanged() {
    watchAncestors();
    visibilityChanged();
}
void MixFocus::watchAncestors() {
    unwatchAncestors();
    for (auto *parent = getParentComponent(); parent != nullptr; parent = parent->getParentComponent()) {
        ancestors_.emplace_back(parent);
        parent->addComponentListener(this);
    }
}
void MixFocus::unwatchAncestors() noexcept {
    for (auto &parent : ancestors_)
        if (parent != nullptr) parent->removeComponentListener(this);
    ancestors_.clear();
}
void MixFocus::componentVisibilityChanged(juce::Component &) { visibilityChanged(); }
void MixFocus::componentMovedOrResized(juce::Component &, bool, bool) { visibilityChanged(); }
void MixFocus::moved() { visibilityChanged(); }
void MixFocus::componentParentHierarchyChanged(juce::Component &) { parentHierarchyChanged(); }
void MixFocus::componentBeingDeleted(juce::Component &) {
    unwatchAncestors();
    dropIfActive();
}
void MixFocus::frame(const SnapshotReader &reader) {
    if (!isShowing() || suspended_) {
        dropIfActive();
        return;
    }
    const SafePointer<MixFocus> alive(this);
    if (density_ != look::densityId()) {
        density_ = look::densityId();
        resized();
        if (!alive) return;
    }
    if (plot_.isEmpty()) {
        dropIfActive();
        return;
    }
    if (generation_ != root_.generation() || snapshot_ != reader.get()) {
        const auto last = available.getSelectedId() - 1;
        const auto oldChoice = last >= 0 && static_cast<size_t>(last) < choices_.size()
                                   ? choices_[static_cast<size_t>(last)]
                                   : 0;
        choices_.clear();
        available.clear(juce::dontSendNotification);
        const auto shown = shownTrackNames(reader); // as the headers show them (walk-B B13)
        for (const auto &t : reader.tracks())
            if (focusable(*t)) {
                choices_.push_back(t->id);
                const int item = static_cast<int>(choices_.size());
                const auto named = shown.find(t->id);
                const auto name = juce::String::fromUTF8((named != shown.end() ? named->second : t->name).c_str());
                available.addItem(name.isEmpty() ? "Channel " + juce::String(item) : name, item);
                if (oldChoice == t->id)
                    available.setSelectedId(item, juce::dontSendNotification);
            }
        if (!available.getSelectedId() && !choices_.empty())
            available.setSelectedId(1, juce::dontSendNotification);
        std::vector<std::int64_t> keep;
        for (const auto &m : members_)
            if (const auto *track = reader.findTrack(m.id); track && focusable(*track))
                keep.push_back(m.id);
        if (keep.size() != members_.size())
            setMembers(keep);
        else
            refreshControls();
        if (!alive || !isShowing() || suspended_) return;
        generation_ = root_.generation();
        snapshot_ = reader.get();
    }
    if (frozenScopeRevision_ || stoppedScopeRevision_) {
        const auto heldBroker = broker_.lock();
        if (scope.getToggleState() && plotShowing() &&
            heldBroker && heldBroker == root_.sharedAnalysis && heldBroker->active() &&
            (!stoppedScopeRevision_ || (shownBar_ && scopeTimingCurrent()))) return;
        thawScope();
        if (!alive || !plotShowing() || suspended_) return;
    }
    const bool playing = root_.playing();
    const bool stoppedNow = scopePlaying_ && !playing;
    if (stoppedNow && scope.getToggleState() && shownBar_ && !root_.settledLocateRevision()) return;
    const auto &transport = reader.transport();
    const bool looping = playing && transport.loopEnabled && transport.hasLoop;
    if (looping && alignment_ != Alignment::WaitForStop) {
        alignment_ = Alignment::WaitForStop;
        for (auto &member : members_) member.afterLoop.reset();
        redraw_ = true;
    } else if (!playing && alignment_ == Alignment::WaitForStop) {
        // Turning off the authored loop may lead the audio. A visible
        // stop/start gives the next frame a fresh boundary on every tap.
        alignment_ = Alignment::WaitForFresh;
        redraw_ = true;
    }
    if (scopePlaying_ != playing) {
        scopePlaying_ = playing;
        redraw_ = true;
    }
    if (!plotShowing()) {
        dropIfActive();
        return;
    }
    const bool scopeShown = scope.getToggleState();
    if (scopeShown != scopeDisplay_.isVisible()) {
        updateMatchControls();
        resized();
    }
    if (scopeShown && !playing) {
        const auto scopeBroker = broker_.lock();
        if (stoppedNow && alignment_ == Alignment::Ready && shownBar_ && scopeTimingCurrent() &&
            shownBar_->transportRevision >= *scopeLocate_ && scopeBroker &&
            scopeBroker == root_.sharedAnalysis && scopeBroker->active()) {
            if (scopeRegion_ && scopeRegion_->dragging) clearScopeRegion();
            stoppedScopeRevision_ = shownBar_->transportRevision;
            rebuildScope();
            if (scopeCursorPoint_) refreshScopeCursor(reader);
            redraw_ = false;
            scopeFreeze_.setEnabled(scopeTraceRevision().has_value());
            repaint();
            releaseScopeLeases();
            return;
        }
        dropIfActive();
        if (!alive) return;
        if (redraw_) {
            rebuildScope();
            redraw_ = false;
            repaint(plot_.toNearestInt().withTop(scroll_.getBottom()).getUnion(sumLabel_).getUnion(phaseBar_));
        }
        return;
    }
    const auto broker = root_.sharedAnalysis;
    if (match_ && (matchBroker_.lock() != broker || !broker || !broker->active())) {
        clearMatchPreview();
        updateMatchControls();
    }
    if (broker != broker_.lock()) {
        const auto nextPose = membershipPose_ + 1;
        drop();
        if (!alive || membershipPose_ != nextPose || !isShowing() || suspended_ || root_.sharedAnalysis != broker)
            return;
        broker_ = broker;
    }
    if (broker && !broker->active()) {
        if (std::any_of(members_.begin(), members_.end(), [](const auto &member) { return member.data || member.lease; }))
            drop();
        if (!alive) return;
    }
    if (scopeShown) {
        const auto locate = root_.settledLocateRevision();
        if (locate && locate != scopeLocate_) {
            scopeLocate_ = locate;
            shownBar_.reset();
            requestedScope_.reset();
            redraw_ = true;
        }
        const bool pending = !locate;
        if (pending != scopeCommandPending_) {
            scopeCommandPending_ = pending;
            requestedScope_.reset();
            redraw_ = true;
        }
    }
    const bool groupedOnly = waitsForCompleteScopeWindow() || (scopeShown && alignment_ == Alignment::Ready && shownBar_);
    if (broker && broker->active() && !groupedOnly) {
        const auto pose = membershipPose_;
        auto outputs = static_cast<std::uint32_t>(scopeShown ? SharedAnalysis::Scope : SharedAnalysis::Spectrum);
        if (playing && alignment_ != Alignment::WaitForStop && !sumLabel_.isEmpty() && members_.size() >= 2)
            outputs |= SharedAnalysis::Scope;
        for (std::size_t i = 0; i < members_.size(); ++i) {
            if (!members_[i].lease || members_[i].outputs != outputs) {
                SharedAnalysis::Request req;
                req.track = members_[i].id;
                req.channel = SharedAnalysis::Channel::StereoPower;
                req.outputs = outputs;
                SharedAnalysis::Lease acquired;
                try { acquired = broker->acquire(req); }
                catch (...) {}
                if (!alive || membershipPose_ != pose || !isShowing() || suspended_ || root_.sharedAnalysis != broker)
                    return;
                if (acquired) {
                    members_[i].lease = std::move(acquired);
                    members_[i].outputs = outputs;
                }
            }
            auto &member = members_[i];
            if (playing && alignment_ == Alignment::WaitForFresh && !member.afterLoop && member.lease.available()) {
                const auto boundary = member.lease.sourcePosition();
                if (boundary.generation != 0) member.afterLoop = boundary;
            }
            auto next = member.lease.read();
            if (next && next->advancing) {
                const auto end = focusWindowEnd(next->firstStamp, next->request.fftSize);
                if (!end) next.reset();
                else if (*end > root_.renderedSamplePosition())
                    next = member.data && member.data->sourceEpoch == next->sourceEpoch ? member.data : nullptr;
            }
            if (next && playing && alignment_ == Alignment::WaitForFresh) {
                const auto &boundary = member.afterLoop;
                if (!boundary || !next->advancing || next->generation < boundary->generation ||
                    (next->generation == boundary->generation && next->firstSequence < boundary->written))
                    next.reset();
            }
            if (next && scopeShown && (scopeCommandPending_ || !scopeLocate_ || next->transportRevision < *scopeLocate_)) next.reset();
            if (next != member.data) {
                if (!scopeShown || !shownBar_) redraw_ = true;
                member.data = std::move(next);
                member.spectrumChanged = true;
            }
        }
    }
    if (playing && alignment_ == Alignment::WaitForFresh && !members_.empty() &&
        std::all_of(members_.begin(), members_.end(), [](const auto &member) {
            return member.afterLoop && member.data;
        })) {
        alignment_ = Alignment::Ready;
        redraw_ = true;
    }
    updateBarScope(reader, broker);
    if (!alive || !isShowing() || suspended_) return;
    std::vector<SharedAnalysis::Lease> shortPreview;
    if (barScope_ && (groupedOnly || shownBar_) &&
        std::ranges::any_of(members_, [](const auto &member) { return static_cast<bool>(member.lease); })) {
        // Keep the shared taps alive when retiring the short previews. Only
        // the grouped reader needs source audio while a full window is shown or required.
        shortPreview.reserve(members_.size());
        for (auto &member : members_) {
            shortPreview.push_back(std::move(member.lease));
            member.outputs = 0;
            member.data.reset();
            member.spectrumChanged = true;
        }
    }
    if (broker && !members_.empty()) broker->requestFrame();
    const auto locateRevision = scopeShown ? std::nullopt : root_.settledLocateRevision();
    if (redraw_ || matchLocateRevision_ != locateRevision)
        updateMatchControls();
    matchLocateRevision_ = locateRevision;
    pollAlign();
    if (redraw_) {
        if (!scope.getToggleState()) rebuildPlot();
        rebuildScope();
        if (scopeRegion_ && !scopeRegionCurrent()) clearScopeRegion();
        if (scopeCursorPoint_) refreshScopeCursor(reader);
        redraw_ = false;
        repaint(plot_.toNearestInt().withTop(scroll_.getBottom()).getUnion(sumLabel_).getUnion(phaseBar_));
    } else {
        if (scopeRegion_ && !scopeRegionCurrent()) clearScopeRegion();
        if (scopeCursor_.isVisible() && !scopeCursorCurrent()) scopeCursor_.setVisible(false);
        if (scopeTruePeakReadout_.isVisible() && !scopeTraceRevision()) scopeTruePeakReadout_.setVisible(false);
    }
    scopeFreeze_.setEnabled(scopeTraceRevision().has_value());
}
void MixFocus::clearBarScopeData() {
    redraw_ |= static_cast<bool>(shownBar_);
    shownBar_.reset();
    requestedScope_.reset();
    if (barTempo_) scopeGridStamp_.reset();
    barTempo_.reset();
    barMeters_.clear();
    scopeCursorBeat_.reset();
    scopeTruePeakReadout_.setVisible(false);
    differenceRmsReadout_.setVisible(false);
}
void MixFocus::updateBarScope(const SnapshotReader &reader, const std::shared_ptr<SharedAnalysis> &broker) {
    const auto *tempo = reader.tempo();
    const auto choice = scopeWindowChoice();
    const auto milliseconds = focusScopeWindowMilliseconds(choice);
    if (!scopePlaying_ || !scope.getToggleState() || alignment_ != Alignment::Ready || plot_.isEmpty() ||
        members_.empty() || (!tempo && !milliseconds) || !broker || !broker->active()) {
        clearBarScopeData();
        barScope_.reset();
        return;
    }
    if (scopeCommandPending_ || !scopeLocate_) return;
    const SafePointer<MixFocus> alive(this);
    const auto pose = membershipPose_;
    const auto sameMeter = [](const auto &a, const auto &b) {
        return a.start_ticks == b.start_ticks && a.numerator == b.numerator && a.denominator == b.denominator;
    };
    if (barTempo_.get() != tempo || !std::ranges::equal(barMeters_, reader.meters(), sameMeter)) {
        if (milliseconds) {
            scopeGridStamp_.reset();
            scopeCursorBeat_.reset();
            redraw_ = true;
        } else {
            clearBarScopeData();
            barScope_.reset();
            if (!alive || pose != membershipPose_ || !isShowing() || suspended_ || root_.sharedAnalysis != broker)
                return;
        }
        barTempo_ = reader.get() ? reader.get()->tempo : nullptr;
        barMeters_ = reader.meters();
    }

    if (!barScope_) {
        std::vector<engine::ScopeSource> sources;
        sources.reserve(members_.size());
        for (const auto &member : members_)
            sources.push_back({member.id, engine::ScopePoint::PostFader, 0});
        SharedAnalysis::BarScopeLease acquired;
        try { acquired = broker->acquireBarScope(sources, static_cast<std::size_t>(plot_.getWidth())); }
        catch (...) {}
        if (!alive || pose != membershipPose_ || !isShowing() || suspended_ || root_.sharedAnalysis != broker)
            return;
        barScope_ = std::move(acquired);
        barScope_.setLayerAdjustments(scopeAdjustments_);
        barScope_.setPhaseRequested(!phaseBar_.isEmpty());
        updateScopeChannelDemand();
        barScope_.setDifferenceRequested(differenceRequest_);
        barScope_.setSumTruePeakRequested(scopeTruePeak_.isVisible() && scopeTruePeak_.getToggleState());
    }
    const auto rate = barScope_.sampleRate();
    if (!(rate > 0.) || (requestedScope_ && requestedScope_->range.sampleRate != rate)) {
        if (shownBar_) redraw_ = true;
        shownBar_.reset();
        requestedScope_.reset();
        return;
    }

    const auto heard = root_.renderedSamplePosition();
    std::uint64_t heardRevision = 0;
    for (const auto &member : members_)
        if (member.data && member.data->advancing)
            heardRevision = std::max(heardRevision, member.data->transportRevision);
    const auto requiredRevision = std::max(heardRevision, *scopeLocate_);
    const auto barEnd = [](const auto &bar) {
        return focusWindowEnd(bar.firstStamp, static_cast<std::int64_t>(bar.frames));
    };
    if (shownBar_ && (requiredRevision > shownBar_->transportRevision ||
        heard < barEnd(*shownBar_).value_or(heard))) {
        shownBar_.reset();
        redraw_ = true;
    }
    std::optional<std::int64_t> tick;
    std::optional<std::int64_t> nextSample;
    bool missed = false;
    if (requestedScope_) {
        using Status = SharedAnalysis::BarScopeStatus;
        const auto status = barScope_.status();
        if (status == Status::Pending) return;
        if (status == Status::Ready) {
            if (const auto current = barScope_.read(); current && current->transportRevision >= requiredRevision) {
                const auto end = barEnd(*current);
                if (!end || *end > heard) return;
                if (shownBar_ != current) {
                    shownBar_ = current;
                    scopeWindowAwaiting_ = false;
                    redraw_ = true;
                }
                tick = requestedScope_->nextTick;
                nextSample = *end;
                heardRevision = current->transportRevision;
            }
        } else if (status == Status::Missed) {
            if (shownBar_) redraw_ = true;
            shownBar_.reset();
            missed = true;
        }
    }
    if (milliseconds) {
        if (const auto next = focusScopeTimedWindow(nextSample.value_or(heard), rate, milliseconds);
            next && barScope_.request(*next, heardRevision, *scopeLocate_))
            requestedScope_ = ScopeRequest{*next, std::nullopt};
        return;
    }
    auto next = tick ? focusScopeWindowAtTick(*tick, rate, *tempo, reader.meters(), choice)
                     : focusScopeWindowAtSample(heard, rate, *tempo, reader.meters(), choice);
    if (missed && next && next->range == requestedScope_->range)
        next = focusScopeWindowAtTick(next->nextTick, rate, *tempo, reader.meters(), choice);
    if (next && barScope_.request(next->range, heardRevision, *scopeLocate_))
        requestedScope_ = ScopeRequest{next->range, next->nextTick};
}
void MixFocus::rebuildPlot() {
    masked_.clear();
    const auto xAt = [&](double hz) {
        return plot_.getX() +
               static_cast<float>(std::clamp(std::log(std::max(20., hz) / 20.) / std::log(1000.), 0., 1.)) *
                   plot_.getWidth();
    };
    const SharedAnalysis::Result *calibration = nullptr;
    for (const auto &member : members_)
        if (member.data && member.data->sampleRate > 0. && member.data->binsDbfs.size() >= 2) {
            calibration = member.data.get();
            if (member.id == main_)
                break;
        }
    if (!calibration) {
        for (auto &m : members_)
            m.path.clear();
        return;
    }
    const bool formatChanged =
        !bands_ || rate_ != calibration->sampleRate || bins_ != calibration->binsDbfs.size();
    if (formatChanged) {
        rate_ = calibration->sampleRate;
        bins_ = calibration->binsDbfs.size();
        bands_ = std::make_unique<dsp::MaskingBands>(static_cast<int>(bins_), rate_);
        magnitudes_.resize(bins_);
        maskers_.resize(static_cast<size_t>(bands_->count()));
        normalizedBinX_.clear();
        const double binHz = rate_ / (2. * static_cast<double>(bins_ - 1));
        for (size_t i = 0; i < bins_ && static_cast<double>(i) * binHz <= 20000.; ++i)
            normalizedBinX_.push_back(static_cast<float>(
                std::max(0., std::log(std::max(20., static_cast<double>(i) * binHz) / 20.) / std::log(1000.))));
    }
    const Member *primary = nullptr;
    for (auto &m : members_) {
        if (!m.data || m.data->sampleRate != rate_ || m.data->binsDbfs.size() != bins_) {
            m.path.clear();
            m.energy.clear();
            continue;
        }
        if (m.spectrumChanged || formatChanged) {
            m.path.clear();
            for (size_t i = 1; i < normalizedBinX_.size(); ++i) {
                const auto x = plot_.getX() + normalizedBinX_[i] * plot_.getWidth();
                const auto y = plot_.getBottom() -
                               plot_.getHeight() * std::clamp((m.data->binsDbfs[i] + 90.f) / 90.f, 0.f, 1.f);
                if (i == 1)
                    m.path.startNewSubPath(x, y);
                else
                    m.path.lineTo(x, y);
            }
            std::transform(m.data->binsDbfs.begin(), m.data->binsDbfs.end(), magnitudes_.begin(),
                           [](float db) { return std::pow(10.f, db * 0.05f); });
            bands_->accumulate(magnitudes_, m.energy);
            m.spectrumChanged = false;
        }
        if (m.id == main_)
            primary = &m;
    }
    if (alignment_ != Alignment::Ready || !primary || members_.size() < 2 || !primary->data->advancing ||
        !primary->data->transportRevision)
        return;
    const auto &p = *primary->data;
    std::fill(maskers_.begin(), maskers_.end(), 0.);
    size_t others = 0;
    for (auto &m : members_) {
        if (!m.data || m.data->sampleRate != rate_ || m.data->binsDbfs.size() != bins_ ||
            m.data->firstStamp != p.firstStamp || m.data->advancing != p.advancing ||
            m.data->transportRevision != p.transportRevision)
            continue;
        if (m.id == main_)
            continue;
        ++others;
        for (size_t j = 0; j < maskers_.size(); ++j)
            maskers_[j] += m.energy[j];
    }
    if (!others)
        return;
    const auto result = dsp::measureMasking(maskers_, primary->energy, *bands_);
    for (int band = 0; band < bands_->count(); ++band) {
        if (result.excessDb[static_cast<size_t>(band)] <= 0.f ||
            primary->energy[static_cast<size_t>(band)] < 1.e-9)
            continue;
        const auto freq = bands_->centreHz(band);
        const auto left = band ? (freq + bands_->centreHz(band - 1)) * 0.5 : 20.;
        const auto right =
            band + 1 < bands_->count() ? (freq + bands_->centreHz(band + 1)) * 0.5 : rate_ * 0.5;
        masked_.emplace_back(xAt(left), xAt(right));
    }
}
void MixFocus::clearScopeSum() {
    scopePhase_.reset();
    scopeRange_.reset();
    scopeWaveformRevision_.reset();
    sum_.reset();
    differenceRms_.reset();
    for (auto &path : scopeSumPaths_) path.clear();
    if (differenceRenderedRevision_) {
        for (auto &path : scopeDifferencePaths_) path.clear();
        differenceSpectrumPath_.clear();
        differenceRenderedRevision_.reset();
    }
}
FocusScopeChannels MixFocus::scopeChannelView() const noexcept {
    return scope.getToggleState() ? static_cast<FocusScopeChannels>(scopeChannels_.getSelectedId())
                                  : FocusScopeChannels::Stereo;
}
FocusScopeWindow MixFocus::scopeWindowChoice() const noexcept {
    const auto window = static_cast<FocusScopeWindow>(scopeWindow_.getSelectedId());
    switch (window) {
    case FocusScopeWindow::Sixteenth:
    case FocusScopeWindow::Beat:
    case FocusScopeWindow::TwoBars:
    case FocusScopeWindow::Milliseconds85:
    case FocusScopeWindow::Milliseconds170:
    case FocusScopeWindow::Milliseconds340:
    case FocusScopeWindow::FourBars: return window;
    default: return FocusScopeWindow::Bar;
    }
}
bool MixFocus::waitsForCompleteScopeWindow() const noexcept {
    return scope.getToggleState() && alignment_ == Alignment::Ready && !members_.empty() &&
           (scopeWindowAwaiting_ || scopeWindowChoice() != FocusScopeWindow::Bar);
}
bool MixFocus::showsScopeDifference() const noexcept {
    return scope.getToggleState() &&
           scopeDisplay_.getSelectedId() == static_cast<int>(ScopeDisplay::Difference);
}
void MixFocus::updateScopeDifference() {
    differenceSpectrum_.setVisible(showsScopeDifference());
    std::optional<SharedAnalysis::ScopeDifference> next;
    if (showsScopeDifference()) {
        const auto pair = matchMembers();
        const auto view = scopeChannelView();
        const bool spectrum = differenceSpectrum_.getToggleState();
        // The waveform already holds both lanes. The spectrum uses the actual channel selection.
        const auto fold = spectrum || focusScopeIsFolded(view) ? view : FocusScopeChannels::Stereo;
        if (pair[0] && pair[1])
            next = SharedAnalysis::ScopeDifference{
                {pair[0]->id, engine::ScopePoint::PostFader, 0},
                {pair[1]->id, engine::ScopePoint::PostFader, 0},
                fold, differenceRequest_ ? differenceRequest_->selection : 0, spectrum};
    }
    if (next == differenceRequest_) return;
    if (frozenScopeRevision_ || stoppedScopeRevision_) {
        // Thaw clears the former picture and registers the new source pair.
        thawScope();
        return;
    }
    clearScopeRegion();
    scopeCursor_.setVisible(false);
    if (next) next->selection = ++differenceSelection_;
    differenceRequest_ = next;
    barScope_.setDifferenceRequested(next);
    differenceRms_.reset();
    differenceRmsReadout_.setVisible(false);
    if (differenceRenderedRevision_) {
        for (auto &path : scopeDifferencePaths_) path.clear();
        differenceSpectrumPath_.clear();
        differenceRenderedRevision_.reset();
    }
    redraw_ = true;
}

void MixFocus::updateScopeTruePeakControls() {
    const bool eligible = scope.getToggleState() && scopeChannelView() == FocusScopeChannels::Stereo &&
                          scopeDisplay_.getSelectedId() == static_cast<int>(ScopeDisplay::Sum);
    const bool changed = scopeTruePeak_.isVisible() != eligible;
    scopeTruePeak_.setVisible(eligible);
    const bool requested = eligible && scopeTruePeak_.getToggleState();
    barScope_.setSumTruePeakRequested(requested);
    if (!requested) scopeTruePeakReadout_.setVisible(false);
    if (changed) resized();
}

void MixFocus::rebuildDifferenceSpectrum(const SharedAnalysis::ScopeDifferenceSpectrum &spectrum, double sampleRate) {
    if (spectrum.binsDbfs.empty() || spectrum.fftSize <= 0 || plot_.isEmpty()) return;
    const double binHz = sampleRate / static_cast<double>(spectrum.fftSize);
    const double maximum = std::min(20000., sampleRate * .5);
    if (!(binHz > 0. && maximum > 20.)) return;
    const double logRange = std::log(maximum / 20.);
    const auto width = static_cast<std::size_t>(plot_.toNearestInt().getWidth());
    if (!width) return;
    const auto firstBin = [&](std::size_t x) {
        const auto frequency = 20. * std::exp(logRange * static_cast<double>(x) / static_cast<double>(width));
        return std::min(spectrum.binsDbfs.size() - 1, static_cast<std::size_t>(std::ceil(frequency / binHz)));
    };
    auto first = firstBin(0);
    for (std::size_t x = 0; x < width; ++x) {
        const auto next = firstBin(x + 1);
        const auto last = x + 1 == width ? next + 1 : std::max(first + 1, next);
        const auto bins = std::span(spectrum.binsDbfs).subspan(first, last - first);
        const auto peak = std::clamp(static_cast<double>(*std::ranges::max_element(bins)),
                                     dsp::analyser::kFloorDb, dsp::analyser::kCeilingDb);
        const auto fraction = static_cast<float>((peak - dsp::analyser::kFloorDb) /
            (dsp::analyser::kCeilingDb - dsp::analyser::kFloorDb));
        const auto point = juce::Point<float>{plot_.getX() + static_cast<float>(x) * plot_.getWidth() /
            static_cast<float>(width), plot_.getBottom() - fraction * plot_.getHeight()};
        if (x == 0) differenceSpectrumPath_.startNewSubPath(point);
        else differenceSpectrumPath_.lineTo(point);
        first = next;
    }
}

void MixFocus::rebuildScope() {
    rebuildScopePaths();
    if (scopeTraceRevision()) rebuildScopeGrid(SnapshotReader(root_.current()));
    const auto peak = scopeTruePeak_.isVisible() && scopeTruePeak_.getToggleState() && scopeTraceRevision() &&
                      sum_ && shownBar_ && shownBar_->projection ? shownBar_->projection->sumTruePeak() : std::nullopt;
    if (peak) {
        const auto dbText = [](double value) { return value > 0. ? juce::String(20. * std::log10(value), 2) : juce::String("-inf"); };
        const auto samplePeak = std::max(sum_->peak[0], sum_->peak[1]);
        scopeTruePeakReadout_.setText("Peak " + dbText(samplePeak) + " dBFS   Capture " + dbText(*peak) + " dBTP",
                                     juce::dontSendNotification);
    }
    scopeTruePeakReadout_.setVisible(peak.has_value());
    if (!differenceRms_) {
        differenceRmsReadout_.setVisible(false);
        return;
    }
    const auto &rms = *differenceRms_;
    const auto db = [](double sample) {
        return (sample > 0. ? juce::String(20. * std::log10(sample), 1) : juce::String("-inf")) + " dBFS";
    };
    const auto channels = scopeChannelView();
    const auto reading = channels == FocusScopeChannels::Stereo ? "RMS L " + db(rms[0]) + "   R " + db(rms[1])
        : "RMS " + db(rms[channels == FocusScopeChannels::Right ? 1 : 0]);
    differenceRmsReadout_.setText(reading, juce::dontSendNotification);
    differenceRmsReadout_.setVisible(true);
}

void MixFocus::rebuildScopeGrid(const SnapshotReader &reader) {
    if (!scopeRange_) return;
    const bool heldTempo = shownBar_ || frozenScopeRevision_;
    const auto *tempo = heldTempo ? barTempo_.get() : reader.tempo();
    const auto plot = plot_.toNearestInt();
    if (!tempo || plot.isEmpty()) {
        scopeGridStamp_.reset();
        return;
    }
    const auto range = scopeVisibleRange();
    const ScopeGridStamp stamp{range, plot, heldTempo ? std::nullopt : std::optional{reader.seq()}};
    if (scopeGridStamp_ == stamp) return;
    scopeGridStamp_ = stamp;
    scopeGrid_.clear();
    const auto &meters = heldTempo ? barMeters_ : reader.meters();
    forEachFocusScopeGridLine(range, *tempo, meters, [&](std::int64_t sample, bool bar) {
        const auto offset = static_cast<double>(sample - range.first) / static_cast<double>(range.frames);
        const auto x = std::min(plot.getRight() - 1, plot.getX() + juce::roundToInt(offset * plot.getWidth()));
        if (scopeGrid_.empty() || scopeGrid_.back().x != x) scopeGrid_.push_back({x, bar});
        else scopeGrid_.back().bar |= bar;
    });
}

void MixFocus::rebuildScopePaths() {
    clearScopeSum();
    scopeHasTrace_ = false;
    scopeSources_.clear();
    const bool scopeShown = scope.getToggleState();
    if (scopeShown)
        for (auto &member : members_)
            for (auto &path : member.scopePaths) path.clear();
    if ((!scopePlaying_ && !frozenScopeRevision_ && !stoppedScopeRevision_) || (!scopeShown && sumLabel_.isEmpty())) return;
    if (scopeShown && !scopeTimingCurrent()) return;
    if (!shownBar_ && waitsForCompleteScopeWindow()) return;

    const auto view = scopeChannelView();
    const auto display = static_cast<ScopeDisplay>(scopeDisplay_.getSelectedId());
    const bool phaseVisible = scopeShown && display != ScopeDisplay::Difference && !phaseBar_.isEmpty();
    comparesScopeMembers_ = phaseVisible && members_.size() >= 2 &&
                            (display == ScopeDisplay::Layers || display == ScopeDisplay::Stack);
    const bool folded = focusScopeIsFolded(view);
    const std::size_t visibleLanes = view == FocusScopeChannels::Stereo ? 2 : 1;
    const auto sourceLane = [view](std::size_t path) { return view == FocusScopeChannels::Right ? 1 : path; };
    const auto plotColumns = static_cast<std::size_t>(std::max(0.f, plot_.getWidth()));
    const auto pathPoint = [](juce::Path &path) {
        return [&path](FocusScopePoint point, bool first) {
            if (first) path.startNewSubPath(point.x, point.y);
            else path.lineTo(point.x, point.y);
        };
    };
    const auto appendColumn = [this, axisScale = 1. / scopeAxisPeak_, centre = scopePanCentre()]
        (FocusScopePathReducer &trace, const auto &emit, std::size_t column, std::size_t columns,
         double low, double high) {
        const auto x = plot_.getX() + static_cast<float>(column) * plot_.getWidth() / static_cast<float>(columns);
        const auto y = [this, axisScale, centre](double value) {
            return plot_.getCentreY() - static_cast<float>(std::clamp(value * axisScale - centre, -1., 1.)) *
                plot_.getHeight() * .5f;
        };
        trace.add({x, y(low)}, emit);
        if (high != low) trace.add({x, y(high)}, emit);
    };
    const bool aligned = alignment_ == Alignment::Ready;
    if (showsScopeDifference()) {
        const auto locate = scopeDisplayFloor();
        if (!differenceRequest_ || !aligned || !locate || !plotColumns) return;
        if (differenceRequest_->spectrum) {
            if (!shownBar_ || shownBar_->transportRevision < *locate) return;
            const auto *spectrum = shownBar_->differenceSpectrum(*differenceRequest_);
            if (!spectrum) return;
            scopeRange_ = FocusScopeRange{shownBar_->firstStamp, shownBar_->frames, shownBar_->sampleRate};
            differenceRenderedRevision_ = shownBar_->transportRevision;
            rebuildDifferenceSpectrum(*spectrum, shownBar_->sampleRate);
            return;
        }
        std::array<FocusScopePathReducer, 2> traces;
        const auto draw = [&](std::size_t column, std::size_t columns, const FocusSumColumn &extrema) {
            for (std::size_t ch = 0; ch < visibleLanes; ++ch) {
                const auto lane = sourceLane(ch);
                appendColumn(traces[ch], pathPoint(scopeDifferencePaths_[ch]), column, columns,
                             extrema.low[lane], extrema.high[lane]);
            }
        };
        if (shownBar_ && shownBar_->projection) {
            if (shownBar_->transportRevision < *locate) return;
            const auto columns = shownBar_->projection->differenceColumns(*differenceRequest_);
            if (columns.empty()) return;
            scopeRange_ = FocusScopeRange{shownBar_->firstStamp, shownBar_->frames, shownBar_->sampleRate};
            differenceRenderedRevision_ = shownBar_->transportRevision;
            differenceRms_ = shownBar_->projection->differenceRms(*differenceRequest_);
            for (std::size_t i = 0; i < columns.size(); ++i) draw(i, columns.size(), columns[i]);
        } else {
            const auto pair = matchMembers();
            if (!pair[0] || !pair[1] || !pair[0]->data || !pair[1]->data ||
                pair[0]->data->transportRevision < *locate) return;
            const auto &a = *pair[0]->data, &b = *pair[1]->data;
            const std::array<const SharedAnalysis::Result *, 2> windows{&a, &b};
            const auto range = focusScopeRange(windows);
            if (!range) return;
            const auto columns = std::min(plotColumns, range->frames);
            std::array<double, 2> differenceEnergy{};
            scopeRange_ = focusProjectDifference(a, b, columns, differenceRequest_->fold,
                [&](std::size_t i, const FocusSumColumn &extrema) { draw(i, columns, extrema); },
                pair[0]->scopeAdjustment, pair[1]->scopeAdjustment, &differenceEnergy);
            if (scopeRange_) differenceRms_ = focusReadDifferenceRms(differenceEnergy, scopeRange_->frames);
            differenceRenderedRevision_ = a.transportRevision;
        }
        for (std::size_t ch = 0; ch < visibleLanes; ++ch)
            traces[ch].finish(pathPoint(scopeDifferencePaths_[ch]));
        return;
    }
    if (scopeShown && aligned && shownBar_ && shownBar_->projection) {
        const auto &bar = *shownBar_->projection;
        const auto drawColumns = [&](std::array<juce::Path, 2> &paths, std::span<const FocusSumColumn> columns,
                                     FocusScopeChannels channels) {
            const std::size_t count = channels == FocusScopeChannels::Stereo ? 2 : 1;
            for (std::size_t ch = 0; ch < count; ++ch) {
                const auto lane = channels == FocusScopeChannels::Right ? 1 : ch;
                FocusScopePathReducer trace;
                const auto emit = pathPoint(paths[ch]);
                for (std::size_t i = 0; i < columns.size(); ++i)
                    appendColumn(trace, emit, i, columns.size(), columns[i].low[lane], columns[i].high[lane]);
                trace.finish(emit);
            }
        };
        const auto drawMonoColumns = [&](juce::Path &path, auto points) {
            FocusScopePathReducer trace;
            const auto emit = pathPoint(path);
            for (std::size_t i = 0; i < points.size(); ++i)
                appendColumn(trace, emit, i, points.size(), points[i].low, points[i].high);
            trace.finish(emit);
        };
        if (folded) {
            const auto columns = bar.sumMonoColumns(view);
            if (columns.empty()) return; // The lease can supply the new fold on its next full bar.
            drawMonoColumns(scopeSumPaths_[0], columns);
        } else {
            drawColumns(scopeSumPaths_, bar.sumColumns(), view);
        }
        if (display != ScopeDisplay::Sum) {
            for (std::size_t i = 0; i < members_.size(); ++i) {
                auto &member = members_[i];
                const auto channels = member.scopeChannel.value_or(view);
                if (focusScopeIsFolded(channels))
                    drawMonoColumns(member.scopePaths[0], bar.memberMonoColumns(i, channels));
                else
                    drawColumns(member.scopePaths, bar.memberColumns(i), channels);
            }
        }
        scopeRange_ = FocusScopeRange{shownBar_->firstStamp, shownBar_->frames, shownBar_->sampleRate};
        scopeWaveformRevision_ = shownBar_->transportRevision;
        sum_ = bar.sumReading();
        if (phaseVisible) scopePhase_ = bar.phase(comparesScopeMembers_);
        const auto hasPath = [](const auto &paths) {
            return std::ranges::any_of(paths, [](const auto &path) { return !path.isEmpty(); });
        };
        const bool showSum = display == ScopeDisplay::Sum || (display == ScopeDisplay::Overlay && members_.size() >= 2);
        scopeHasTrace_ = (showSum && hasPath(scopeSumPaths_)) ||
            (display != ScopeDisplay::Sum && std::ranges::any_of(members_, [&](const auto &member) {
                return hasPath(member.scopePaths);
            }));
        return;
    }
    FocusScopePhase sumPhase;
    auto *const sumPhaseTarget = phaseVisible && !comparesScopeMembers_ ? &sumPhase : nullptr;
    if (aligned) {
        for (const auto &member : members_) scopeSources_.push_back(member.data.get());
        scopeRange_ = focusScopeRange(scopeSources_);
        if (scopeRange_ && scopeShown)
            scopeWaveformRevision_ = scopeSources_.front()->transportRevision;
        if (scopeRange_ && scopeShown && folded) {
            const auto columns = std::min(plotColumns, scopeRange_->frames);
            FocusScopePathReducer trace;
            const auto emit = pathPoint(scopeSumPaths_[0]);
            const auto publish = [&](std::size_t column, const FocusScopeBarProjection::MonoColumn &extrema) {
                appendColumn(trace, emit, column, columns, extrema.low, extrema.high);
            };
            sum_ = view == FocusScopeChannels::Side
                ? focusProjectSum<FocusScopeChannels::Side>(scopeSources_, *scopeRange_, columns, publish,
                                                         scopeAdjustments_, sumPhaseTarget)
                : focusProjectSum<FocusScopeChannels::Mid>(scopeSources_, *scopeRange_, columns, publish,
                                                        scopeAdjustments_, sumPhaseTarget);
            trace.finish(emit);
        } else if (scopeRange_ && scopeShown) {
            const auto columns = std::min(plotColumns, scopeRange_->frames);
            std::array<FocusScopePathReducer, 2> traces;
            sum_ = focusProjectSum(scopeSources_, *scopeRange_, columns,
                [&](std::size_t column, const FocusSumColumn &extrema) {
                    for (std::size_t ch = 0; ch < visibleLanes; ++ch) {
                        const auto lane = sourceLane(ch);
                        appendColumn(traces[ch], pathPoint(scopeSumPaths_[ch]), column, columns,
                                     extrema.low[lane], extrema.high[lane]);
                    }
                }, scopeAdjustments_, sumPhaseTarget);
            for (std::size_t ch = 0; ch < visibleLanes; ++ch)
                traces[ch].finish(pathPoint(scopeSumPaths_[ch]));
        } else if (scopeRange_ && !sumLabel_.isEmpty()) {
            sum_ = focusReadSum(scopeSources_, *scopeRange_);
        }
    }
    if (phaseVisible) {
        if (scopeRange_ && comparesScopeMembers_)
            scopePhase_ = focusScopePhase(*scopeSources_[0], scopeSources_[1], *scopeRange_,
                                         scopeAdjustments_[0], scopeAdjustments_[1]);
        else if (scopeRange_)
            scopePhase_ = sumPhase.reading();
        else if (members_.size() == 1 && members_.front().data) {
            const auto &layer = members_.front();
            if (const auto range = focusScopeMember(*layer.data))
                scopePhase_ = focusScopePhase(*layer.data, nullptr, *range, layer.scopeAdjustment);
        }
    }
    if (display == ScopeDisplay::Sum) {
        scopeHasTrace_ = !scopeSumPaths_[0].isEmpty() || !scopeSumPaths_[1].isEmpty();
        return;
    }
    if (!scopeShown || plotColumns == 0 || (aligned && !scopeRange_)) return;

    for (auto &member : members_) {
        if (!member.data) continue;
        const auto &data = *member.data;
        // A loop can replay the same heard stamp with the same revision.
        // Draw each member's own window without implying a shared time range.
        const auto window = aligned ? scopeRange_ : focusScopeMember(data);
        if (!window) continue;
        const auto &range = *window;
        scopeHasTrace_ = true;
        const auto steps = std::min(range.frames, plotColumns);
        const auto start = static_cast<std::size_t>(range.first - data.firstStamp);
        const auto channels = member.scopeChannel.value_or(view);
        if (focusScopeIsFolded(channels)) {
            FocusScopePathReducer trace;
            const auto emit = pathPoint(member.scopePaths[0]);
            for (std::size_t p = 0; p < steps; ++p) {
                const auto from = p * range.frames / steps;
                const auto to = (p + 1) * range.frames / steps;
                FocusScopeBarProjection::MonoColumn extrema;
                for (auto i = from; i < to; ++i) {
                    const auto left = focusScopeDisplayedSample(data.scopeLeft, start, range.frames, i, member.scopeAdjustment);
                    const auto right = focusScopeDisplayedSample(data.scopeRight, start, range.frames, i, member.scopeAdjustment);
                    extrema.include(focusScopeFoldSample(channels, left, right));
                }
                appendColumn(trace, emit, p, steps, extrema.low, extrema.high);
            }
            trace.finish(emit);
            continue;
        }
        const std::size_t count = channels == FocusScopeChannels::Stereo ? 2 : 1;
        for (std::size_t ch = 0; ch < count; ++ch) {
            FocusScopePathReducer trace;
            const auto emit = pathPoint(member.scopePaths[ch]);
            const auto &samples = channels == FocusScopeChannels::Right || ch == 1 ? data.scopeRight : data.scopeLeft;
            for (std::size_t p = 0; p < steps; ++p) {
                const auto from = (p * range.frames) / steps;
                const auto to = ((p + 1) * range.frames) / steps;
                if (member.scopeAdjustment.offsetSamples == 0) {
                    const auto [low, high] = std::minmax_element(samples.data() + start + from, samples.data() + start + to);
                    appendColumn(trace, emit, p, steps, member.scopeAdjustment.inverted ? -*high : *low,
                                 member.scopeAdjustment.inverted ? -*low : *high);
                } else {
                    FocusScopeBarProjection::MonoColumn extrema;
                    for (auto i = from; i < to; ++i)
                        extrema.include(focusScopeDisplayedSample(samples, start, range.frames, i, member.scopeAdjustment));
                    appendColumn(trace, emit, p, steps, extrema.low, extrema.high);
                }
            }
            trace.finish(emit);
        }
    }
}
void MixFocus::freezeScope() {
    const auto revision = scopeTraceRevision();
    const auto broker = broker_.lock();
    if (!revision || redraw_ || !plotShowing() || !broker || broker != root_.sharedAnalysis || !broker->active()) {
        scopeFreeze_.setToggleState(false, juce::dontSendNotification);
        return;
    }
    if (frozenScopeRevision_) return;
    if (scopeRegion_ && scopeRegion_->dragging) clearScopeRegion();
    if (!shownBar_) {
        const SnapshotReader reader(root_.current());
        barTempo_ = reader.get() ? reader.get()->tempo : nullptr;
        barMeters_ = reader.meters();
        scopeCursorBeat_.reset();
    }
    frozenScopeRevision_ = revision;
    stoppedScopeRevision_.reset();
    repaint();
    releaseScopeLeases();
}

void MixFocus::releaseScopeLeases() {
    std::vector<SharedAnalysis::Lease> retired;
    retired.reserve(members_.size());
    auto retiredBar = std::move(barScope_);
    for (auto &member : members_) {
        member.afterLoop.reset();
        retired.push_back(std::move(member.lease));
        member.outputs = 0;
    }
}

void MixFocus::thawScope() {
    if (!frozenScopeRevision_ && !stoppedScopeRevision_) return;
    const SafePointer<MixFocus> alive(this);
    drop();
    if (alive) repaint();
}

std::optional<std::uint64_t> MixFocus::scopeDisplayFloor() const noexcept {
    if (frozenScopeRevision_) return frozenScopeRevision_;
    if (stoppedScopeRevision_) return scopeTimingCurrent() ? stoppedScopeRevision_ : std::nullopt;
    return root_.settledLocateRevision();
}

bool MixFocus::scopeTimingCurrent() const noexcept {
    return frozenScopeRevision_ || (scopeLocate_ && root_.settledLocateRevision() == scopeLocate_ &&
                                   (!stoppedScopeRevision_ || !root_.playing()));
}

void MixFocus::mouseDown(const juce::MouseEvent &event) {
    if (!event.mods.isLeftButtonDown() || !plot_.contains(event.position)) return;
    clearScopeRegion();
    const auto revision = scopeTraceRevision();
    if (!revision || !plotShowing()) return;
    const auto &range = *scopeRange_;
    if (range.frames == 0 || !std::in_range<std::int64_t>(range.frames) ||
        !focusWindowEnd(range.first, static_cast<std::int64_t>(range.frames)) ||
        !std::isfinite(range.sampleRate) || range.sampleRate <= 0.) return;
    const auto boundary = scopeBoundaryAt(event.position.x);
    scopeRegion_ = ScopeRegion{range, *revision, membershipPose_, scopeLaneAt(event.position.y),
                               boundary, boundary};
}

void MixFocus::mouseDrag(const juce::MouseEvent &event) {
    if (!scopeRegion_ || !scopeRegion_->dragging) return;
    const auto p = event.position;
    if (!scopeRegionCurrent() || p.y < plot_.getY() || p.y >= plot_.getBottom() ||
        scopeRegion_->lane != scopeLaneAt(p.y)) {
        clearScopeRegion();
        return;
    }
    const auto edge = scopeBoundaryAt(p.x);
    if (edge == scopeRegion_->edge) return;
    scopeRegion_->edge = edge;
    refreshScopeRegion();
}

void MixFocus::mouseUp(const juce::MouseEvent &event) {
    if (!scopeRegion_ || !scopeRegion_->dragging) return;
    mouseDrag(event);
    if (!scopeRegion_) return;
    if (scopeRegion_->anchor == scopeRegion_->edge) clearScopeRegion();
    else scopeRegion_->dragging = false;
}

std::size_t MixFocus::scopeLaneAt(float y) const noexcept {
    if (scopeDisplay_.getSelectedId() != static_cast<int>(ScopeDisplay::Stack) || members_.empty()) return 0;
    const auto ordinate = (static_cast<double>(y) - plot_.getY()) / plot_.getHeight();
    return std::min(static_cast<std::size_t>(ordinate * static_cast<double>(members_.size())), members_.size() - 1);
}

std::int64_t MixFocus::scopeBoundaryAt(float x) const noexcept {
    // Selection uses sample boundaries. A drag through the right edge may
    // therefore include the final sample of the displayed half-open interval.
    const auto horizontal = std::clamp((static_cast<double>(x) - plot_.getX()) / plot_.getWidth(), 0., 1.);
    const auto view = scopeVisibleRange();
    const auto frames = static_cast<std::int64_t>(view.frames);
    const auto scaled = std::round(static_cast<long double>(horizontal) * static_cast<long double>(frames));
    const auto withinView = scaled >= static_cast<long double>(frames) ? frames : static_cast<std::int64_t>(scaled);
    return (view.first - scopeRange_->first) + withinView;
}

FocusScopeRange MixFocus::scopeVisibleRange() const noexcept {
    if (!scopeRange_) return {};
    const auto &capture = *scopeRange_;
    if (scopeTimeZoomSteps_ == 0 || !std::in_range<std::int64_t>(capture.frames) ||
        !focusWindowEnd(capture.first, static_cast<std::int64_t>(capture.frames))) return capture;
    const auto frames = std::max(std::size_t{1}, capture.frames >> scopeTimeZoomSteps_);
    const auto last = static_cast<std::int64_t>(capture.frames - frames);
    const auto rounded = std::round(scopeTimeCentre_ * static_cast<long double>(capture.frames) -
                                    .5L * static_cast<long double>(frames));
    const auto offset = rounded >= static_cast<long double>(last)
        ? last : static_cast<std::int64_t>(std::max(0.L, rounded));
    return {capture.first + offset, frames, capture.sampleRate};
}

juce::AffineTransform MixFocus::scopeTimeTransform() const noexcept {
    if (!scopeRange_ || scopeTimeZoomSteps_ == 0) return {};
    const auto view = scopeVisibleRange();
    if (view.frames == 0 || view == *scopeRange_) return {};
    const auto scale = static_cast<float>(static_cast<double>(scopeRange_->frames) / static_cast<double>(view.frames));
    const auto offset = static_cast<double>(scopeRange_->first - view.first) / static_cast<double>(view.frames);
    return juce::AffineTransform::scale(scale, 1.f, plot_.getX(), 0.f)
        .translated(static_cast<float>(offset * plot_.getWidth()), 0.f);
}

bool MixFocus::changeScopeTimeView(float x, float delta, bool zoom) noexcept {
    const auto view = scopeVisibleRange();
    if (view.frames == 0 || !scopeRange_) return false;
    const auto &capture = *scopeRange_;
    const int direction = delta > 0.f ? 1 : -1;
    const int steps = zoom ? std::clamp(scopeTimeZoomSteps_ + direction, 0, scopeTimeZoomLimit_) : scopeTimeZoomSteps_;
    const auto frames = std::max(std::size_t{1}, capture.frames >> steps);
    if (zoom && frames == view.frames) return false;
    const auto anchor = (static_cast<long double>(x) - plot_.getX()) / plot_.getWidth();
    auto first = static_cast<long double>(view.first - capture.first);
    const auto span = static_cast<long double>(view.frames);
    const auto next = static_cast<long double>(frames);
    first += zoom ? (span - next) * anchor : -.25L * span * direction;
    first = std::clamp(first, 0.L, static_cast<long double>(capture.frames - frames));
    scopeTimeZoomSteps_ = steps;
    scopeTimeCentre_ = (first + .5L * next) / static_cast<long double>(capture.frames);
    return scopeVisibleRange() != view;
}

bool MixFocus::scopeRegionCurrent() const noexcept {
    if (!scopeRegion_ || scopeRegion_->membership != membershipPose_ || !scopeRange_) return false;
    const auto &range = scopeRegion_->range;
    return scopeRange_->first == range.first && scopeRange_->frames == range.frames &&
           scopeRange_->sampleRate == range.sampleRate && scopeTraceRevision() == scopeRegion_->traceRevision;
}

void MixFocus::clearScopeRegion() {
    if (!scopeRegion_) return;
    scopeRegion_.reset();
    const bool visible = scopeRegionReadout_.isVisible();
    scopeRegionReadout_.setVisible(false);
    if (visible) repaint(plot_.toNearestInt());
}

void MixFocus::refreshScopeRegion() {
    const auto &region = *scopeRegion_;
    const auto [left, right] = std::minmax(region.anchor, region.edge);
    if (left != right) {
        const auto first = region.range.first + left;
        const auto last = region.range.first + right;
        const auto hz = region.range.sampleRate / static_cast<double>(right - left);
        const auto milliseconds = [rate = region.range.sampleRate](std::int64_t sample) {
            return juce::String((static_cast<double>(sample) / rate) * 1000., 1);
        };
        const auto key = juce::roundToInt(69. + 12. * (std::log2(hz) - std::log2(440.)));
        const auto octave = static_cast<int>(std::floor(static_cast<double>(key) / 12.)) - 2;
        juce::String reading;
        reading << static_cast<juce::int64>(first) << " to " << static_cast<juce::int64>(last)
                << " samples   " << milliseconds(first) << " to " << milliseconds(last)
                << " ms   " << milliseconds(right - left) << " ms   "
                << juce::String(hz, 1) << " Hz   " << pitchClassName(key) << octave;
        scopeCursor_.setVisible(false);
        scopeRegionReadout_.setText(reading, juce::dontSendNotification);
    }
    scopeRegionReadout_.setVisible(left != right);
    repaint(plot_.toNearestInt());
}

void MixFocus::paintScopeRegion(juce::Graphics &g) const {
    if (!scopeRegionCurrent()) return;
    const auto &region = *scopeRegion_;
    const auto view = scopeVisibleRange();
    const auto offset = view.first - region.range.first;
    const auto left = std::max(std::min(region.anchor, region.edge), offset);
    const auto right = std::min(std::max(region.anchor, region.edge), offset + static_cast<std::int64_t>(view.frames));
    if (left >= right) return;
    const auto frames = static_cast<double>(view.frames);
    const auto x = plot_.getX() + static_cast<float>(static_cast<double>(left - offset) / frames) * plot_.getWidth();
    const auto width = static_cast<float>(static_cast<double>(right - left) / frames) * plot_.getWidth();
    const bool stacked = scopeDisplay_.getSelectedId() == static_cast<int>(ScopeDisplay::Stack) && !members_.empty();
    const auto height = stacked ? plot_.getHeight() / static_cast<float>(members_.size()) : plot_.getHeight();
    const auto y = plot_.getY() + static_cast<float>(region.lane) * height;
    g.setColour(look::selection);
    g.fillRect(x, y, width, height);
}

void MixFocus::mouseMove(const juce::MouseEvent &event) {
    if (!scope.getToggleState() || !plot_.contains(event.position)) {
        mouseExit(event);
        return;
    }
    scopeCursorPoint_ = event.position;
    refreshScopeCursor(SnapshotReader(root_.current()));
}

void MixFocus::mouseExit(const juce::MouseEvent &) {
    scopeCursorPoint_.reset();
    scopeCursor_.setVisible(false);
}

void MixFocus::mouseWheelMove(const juce::MouseEvent &event, const juce::MouseWheelDetails &wheel) {
    const bool spectrum = showsScopeDifference() && differenceSpectrum_.getToggleState();
    const bool vertical = event.mods.isAltDown();
    const bool timeZoom = !vertical && event.mods.isCommandDown();
    const bool timePan = !vertical && !timeZoom && (event.mods.isShiftDown() || wheel.deltaX != 0.f);
    if (!scope.getToggleState() || spectrum || (!vertical && !timeZoom && !timePan) || !plot_.contains(event.position)) {
        juce::Component::mouseWheelMove(event, wheel);
        return;
    }
    const auto delta = timePan && !event.mods.isShiftDown() ? wheel.deltaX : wheel.deltaY;
    if ((scopeRegion_ && scopeRegion_->dragging) || !std::isfinite(delta) || delta == 0.f) return;
    if (timeZoom || timePan) {
        if (!scopeTimingCurrent() || !scopeTraceRevision() || !changeScopeTimeView(event.position.x, delta, timeZoom)) return;
        const SnapshotReader reader(root_.current());
        rebuildScopeGrid(reader);
        scopeCursorPoint_ = event.position;
        refreshScopeCursor(reader);
        repaint(plot_.toNearestInt().withTop(scroll_.getBottom()));
        return;
    }
    const bool pan = event.mods.isShiftDown();
    auto &steps = pan ? scopePanSteps_ : scopeZoomSteps_;
    const auto next = std::clamp(steps + (delta > 0.f ? 1 : -1), -scopeVerticalStepLimit_, scopeVerticalStepLimit_);
    if (steps == next) return;
    steps = next;
    if (!pan) scopeAxisPeak_ = std::pow(10., -.3 * static_cast<double>(steps));
    scopeCursorPoint_ = event.position;
    rebuildScope();
    refreshScopeCursor(SnapshotReader(root_.current()));
    repaint(plot_.toNearestInt().withTop(scroll_.getBottom()));
}

double MixFocus::scopePanCentre() const noexcept {
    return static_cast<double>(scopePanSteps_) / scopeVerticalStepLimit_;
}

std::optional<std::uint64_t> MixFocus::scopeTraceRevision() const noexcept {
    if (!scopeRange_ || (!scopePlaying_ && !frozenScopeRevision_ && !stoppedScopeRevision_) || suspended_ ||
        alignment_ != Alignment::Ready || !scope.getToggleState())
        return {};
    const auto locate = scopeDisplayFloor();
    if (!locate) return {};
    if (showsScopeDifference()) {
        if (differenceSpectrum_.getToggleState() || !differenceRenderedRevision_ ||
            *differenceRenderedRevision_ < *locate) return {};
        return differenceRenderedRevision_;
    }
    if (!scopeHasTrace_ || !scopeWaveformRevision_ || *scopeWaveformRevision_ < *locate) return {};
    return scopeWaveformRevision_;
}

bool MixFocus::scopeCursorCurrent() const noexcept {
    return !scopeRegionReadout_.isVisible() && scopeCursorPoint_ &&
           plot_.contains(*scopeCursorPoint_) && scopeTraceRevision().has_value();
}

void MixFocus::refreshScopeCursor(const SnapshotReader &reader) {
    if (!scopeCursorCurrent() || !plotShowing() || scopeCursor_.getBounds().isEmpty()) {
        scopeCursor_.setVisible(false);
        return;
    }
    const auto range = scopeVisibleRange();
    if (range.frames == 0 || !std::in_range<std::int64_t>(range.frames) ||
        !focusWindowEnd(range.first, static_cast<std::int64_t>(range.frames))) {
        scopeCursor_.setVisible(false);
        return;
    }
    const auto point = *scopeCursorPoint_;
    const auto horizontal = (static_cast<double>(point.x) - plot_.getX()) / plot_.getWidth();
    const auto offset = std::min(static_cast<std::size_t>(horizontal * static_cast<long double>(range.frames)),
                                 range.frames - 1);
    const auto sample = range.first + static_cast<std::int64_t>(offset);
    const auto seconds = static_cast<double>(sample) / range.sampleRate;

    // Stack lays out the same signed waveform axis once per member.
    auto ordinate = (static_cast<double>(point.y) - plot_.getY()) / plot_.getHeight();
    if (scopeDisplay_.getSelectedId() == static_cast<int>(ScopeDisplay::Stack) && !members_.empty()) {
        ordinate *= static_cast<double>(members_.size());
        ordinate -= std::floor(ordinate);
    }
    const auto magnitude = std::abs(1. - 2. * ordinate + scopePanCentre()) * scopeAxisPeak_;
    const auto decibels = magnitude > 0. ? juce::String(20. * std::log10(magnitude), 1) : juce::String("-inf");

    juce::String reading;
    const bool heldTempo = shownBar_ || frozenScopeRevision_;
    if (const auto *tempo = heldTempo ? barTempo_.get() : reader.tempo(); tempo && sample >= 0) {
        const auto &meters = heldTempo ? barMeters_ : reader.meters();
        const auto liveSnapshot = heldTempo ? std::nullopt : std::optional{reader.seq()};
        // The cursor and capture share the same rounded, half-open beat edges.
        if (scopeCursorBeat_ && scopeCursorBeat_->liveSnapshot == liveSnapshot &&
            scopeCursorBeat_->range.sampleRate == range.sampleRate &&
            sample >= scopeCursorBeat_->range.first &&
            sample - scopeCursorBeat_->range.first < static_cast<std::int64_t>(scopeCursorBeat_->range.frames)) {
            reading = scopeCursorBeat_->label;
        } else {
            const auto beat = focusScopeWindowAtSample(sample, range.sampleRate, *tempo, meters, FocusScopeWindow::Beat);
            const auto at = barPosition(beat ? beat->firstTick : tempo->secondsToTicks(seconds), meters);
            reading = juce::String(static_cast<juce::int64>(at.bar)) + "." +
                      juce::String(static_cast<juce::int64>(at.beat)) + "   ";
            if (beat) scopeCursorBeat_ = ScopeCursorBeat{beat->range, liveSnapshot, reading};
        }
    }
    reading << static_cast<juce::int64>(sample) << " samples   " << juce::String(seconds * 1000., 1)
            << " ms   " << decibels << " dBFS";
    scopeCursor_.setText(reading, juce::dontSendNotification);
    scopeCursor_.setVisible(true);
}

void MixFocus::resized() {
    auto r = getLocalBounds().reduced(look::gap);
    const int hairline = juce::roundToInt(look::hairline);
    const int chipHeight = look::mixFocusChipHeight;
    const int buttonRow = chipHeight + 2 * hairline;
    const int chipWidth = look::mixFocusChipWidth;
    auto toolbar = r.removeFromTop(buttonRow);
    available.setBounds(toolbar.removeFromLeft(std::min(look::mixFocusSourceWidth, toolbar.getWidth() / 2)));
    add.setBounds(toolbar.removeFromLeft(look::mixFocusAddWidth));
    scroll_.setBounds(toolbar);
    strip_.setSize(std::max(hairline, static_cast<int>(members_.size()) * chipWidth), chipHeight);
    int x = 0;
    for (auto &m : members_) {
        m.chip->setBounds(x, 0, chipWidth - 2 * hairline, chipHeight);
        x += chipWidth;
    }
    const int plotInset = look::border + hairline;
    auto titleRow = r.removeFromTop(buttonRow);
    titleRow_ = titleRow;
    alignReadout_.setBounds(titleRow.withTrimmedLeft(
        std::max(0, titleRow.getWidth() - 3 * look::mixFocusSourceWidth)));
    differenceSpectrum_.setBounds(titleRow.withTrimmedLeft(
        std::max(0, titleRow.getWidth() - look::mixFocusSourceWidth)));
    if (scopeTruePeak_.isVisible())
        scopeTruePeak_.setBounds(titleRow.removeFromRight(std::min(titleRow.getWidth(), look::mixFocusSourceWidth)));
    if (differenceSpectrum_.isVisible()) titleRow.removeFromRight(std::min(titleRow.getWidth(), look::mixFocusSourceWidth));
    if (scopeWindow_.isVisible())
        scopeWindow_.setBounds(titleRow.removeFromRight(std::min(titleRow.getWidth(), look::mixFocusSourceWidth)));
    const auto summaryCells = scopeWindow_.isVisible() ? 3 : 4;
    sumLabel_ = titleRow.getWidth() >= summaryCells * look::mixFocusSourceWidth
                    ? titleRow.removeFromRight(2 * look::mixFocusSourceWidth) : juce::Rectangle<int>{};
    auto actions = r.removeFromBottom(buttonRow);
    const auto w = std::min(chipWidth, actions.getWidth() / (scope.getToggleState() ? 11 : 10));
    for (auto *button : {&star, &mute, &solo, &hearOne, &hearAll, &edit})
        button->setBounds(actions.removeFromLeft(w).reduced(hairline));
    scope.setBounds(actions.removeFromLeft(w).reduced(hairline));
    learnMatch.setBounds(actions.removeFromLeft(w).reduced(hairline));
    clearMatch.setBounds(actions.removeFromLeft(w).reduced(hairline));
    alignMeasure_.setBounds(actions.removeFromLeft(w).reduced(hairline));
    scopeFreeze_.setBounds(actions.removeFromLeft(w).reduced(hairline));
    scopeDisplay_.setBounds(learnMatch.getBounds());
    scopeChannels_.setBounds(clearMatch.getBounds());
    scopeLayer_.setBounds(alignMeasure_.getBounds());
    phaseBar_ = scope.getToggleState() && !showsScopeDifference()
                    ? r.removeFromBottom(std::min(look::fieldHeight, r.getHeight())).reduced(plotInset, 0)
                    : juce::Rectangle<int>{};
    barScope_.setPhaseRequested(!phaseBar_.isEmpty());
    const auto nextPlot = r.reduced(plotInset).toFloat();
    const bool changedPlot = plot_ != nextPlot;
    if (changedPlot && scopeRegion_ && scopeRegion_->dragging) clearScopeRegion();
    plot_ = nextPlot;
    if (changedPlot && (frozenScopeRevision_ || stoppedScopeRevision_)) rebuildScope();
    auto cursorBounds = plot_.toNearestInt().reduced(look::gap);
    cursorBounds.setHeight(std::min(cursorBounds.getHeight(), look::fieldHeight));
    scopeCursor_.setFont(look::font(look::fontCaption));
    scopeCursor_.setBounds(cursorBounds.removeFromRight(std::min(cursorBounds.getWidth(), 3 * look::mixFocusSourceWidth)));
    scopeCursor_.setVisible(false);
    scopeRegionReadout_.setFont(look::font(look::fontCaption));
    scopeRegionReadout_.setBounds(plot_.toNearestInt().reduced(look::gap).withHeight(cursorBounds.getHeight()));
    auto rmsArea = plot_.toNearestInt().reduced(look::gap);
    auto rmsRow = rmsArea.removeFromBottom(std::min(look::fieldHeight, rmsArea.getHeight()));
    differenceRmsReadout_.setFont(look::font(look::fontCaption));
    const auto readingBounds = rmsRow.removeFromRight(std::min(2 * look::mixFocusSourceWidth, rmsRow.getWidth()));
    differenceRmsReadout_.setBounds(readingBounds);
    scopeTruePeakReadout_.setFont(look::font(look::fontCaption));
    scopeTruePeakReadout_.setBounds(readingBounds);
    rebuildMatchPath();
    for (auto &m : members_)
        m.spectrumChanged = true;
    redraw_ = true;
    visibilityChanged();
}
void MixFocus::paintScopePhase(juce::Graphics &g) const {
    if (phaseBar_.isEmpty()) return;
    auto area = phaseBar_;
    const auto labelWidth = std::min(look::mixFocusSourceWidth, area.getWidth() / 3);
    g.setFont(look::font(look::fontCaption));
    g.setColour(look::textDim);
    const auto label = comparesScopeMembers_ ? "Layers Mid phase"
                       : members_.size() == 1 ? "Layer L/R phase" : "Sum L/R phase";
    g.drawFittedText(label, area.removeFromLeft(labelWidth), juce::Justification::centredLeft, 1);
    auto readout = area.removeFromRight(labelWidth);
    juce::String number = "--";
    if (scopePhase_) number = (*scopePhase_ > 0. ? "+" : "") + juce::String(*scopePhase_, 2);
    g.drawFittedText(number, readout, juce::Justification::centredRight, 1);
    const auto bar = area.reduced(look::gap, std::max(0, (area.getHeight() - look::gap) / 2)).toFloat();
    if (bar.isEmpty()) return;
    g.setColour(look::outline);
    g.fillRect(bar);
    if (scopePhase_) {
        const auto middle = bar.getCentreX();
        const auto value = middle + static_cast<float>(*scopePhase_) * bar.getWidth() * .5f;
        g.setColour(*scopePhase_ < 0. ? look::record : look::accent);
        g.fillRect(bar.withX(std::min(middle, value)).withWidth(std::abs(value - middle)));
    }
    g.setColour(look::textDim);
    g.drawVerticalLine(juce::roundToInt(bar.getCentreX()), bar.getY(), bar.getBottom());
}

void MixFocus::paint(juce::Graphics &g) {
    g.fillAll(look::panel);
    g.setColour(look::text);
    g.setFont(look::font(look::fontBody, true));
    const auto plot = plot_.toNearestInt();
    const bool scopeShown = scope.getToggleState();
    const bool scopeCurrent = !scopeShown || scopeTimingCurrent();
    const auto display = static_cast<ScopeDisplay>(scopeDisplay_.getSelectedId());
    const bool sumOnly = display == ScopeDisplay::Sum;
    const bool differenceOnly = scopeShown && display == ScopeDisplay::Difference;
    bool differenceReady = false;
    if (differenceOnly && differenceRequest_ && differenceRenderedRevision_) {
        const auto locate = scopeDisplayFloor();
        differenceReady = locate && *differenceRenderedRevision_ >= *locate;
    }
    const bool wantsSum = scopeShown && (sumOnly || (display == ScopeDisplay::Overlay && members_.size() >= 2));
    const bool sumShown = scopeCurrent && wantsSum && (!scopeSumPaths_[0].isEmpty() || !scopeSumPaths_[1].isEmpty());
    const bool sumOverRange = scopeCurrent && wantsSum && sum_ && (sum_->peak[0] > 1. || sum_->peak[1] > 1.);
    const bool spectrumOnly = differenceOnly && differenceSpectrum_.getToggleState();
    juce::String title = spectrumOnly ? "Difference spectrum (dBFS)"
                         : differenceOnly ? "Difference (A - B)" : scopeShown ? "Scope" : "Together";
    if (scopeShown && alignment_ != Alignment::Ready && !sumOnly && !differenceOnly)
        title += "   Individual traces";
    else if (scopeShown && scopeCurrent && scopeRange_ && (!differenceOnly || differenceReady)) {
        if (sumOverRange) {
            title = scopeChannelView() == FocusScopeChannels::Stereo ? "Sum over 0 dBFS" : "Stereo sum over 0 dBFS";
            g.setColour(look::record);
        }
        if (shownBar_) {
            const auto window = scopeWindowChoice();
            switch (window) {
            case FocusScopeWindow::Sixteenth: title += "   1/16 note"; break;
            case FocusScopeWindow::Beat: title += "   1 beat"; break;
            case FocusScopeWindow::Bar: title += "   1 bar"; break;
            case FocusScopeWindow::TwoBars: title += "   2 bars"; break;
            case FocusScopeWindow::FourBars: title += "   4 bars"; break;
            case FocusScopeWindow::Milliseconds85:
            case FocusScopeWindow::Milliseconds170:
            case FocusScopeWindow::Milliseconds340: title += "   " + juce::String(focusScopeWindowMilliseconds(window)) + " ms"; break;
            }
        } else {
            title += "   " + juce::String(1000. * static_cast<double>(scopeRange_->frames) / scopeRange_->sampleRate, 1) + " ms";
        }
        if (sumShown && !sumOverRange && !sumOnly) title += "   Sum overlay";
    }
    if (frozenScopeRevision_) title += "   Frozen";
    else if (stoppedScopeRevision_ && scopeCurrent) title += "   Held on stop";
    if (scopeShown && !spectrumOnly && scopeZoomSteps_ != 0) {
        const auto zoomDb = 6 * scopeZoomSteps_;
        title += "   Zoom " + juce::String(zoomDb > 0 ? "+" : "") + juce::String(zoomDb) + " dB";
    }
    if (scopeShown && !spectrumOnly && scopePanSteps_ != 0) {
        const int percent = 100 * scopePanSteps_ / scopeVerticalStepLimit_;
        title += "   Pan " + juce::String(percent > 0 ? "+" : "") + juce::String(percent) + "%";
    }
    if (scopeShown && !spectrumOnly && scopeTimeZoomSteps_ != 0)
        title += "   Time x" + juce::String(1 << scopeTimeZoomSteps_);
    if (!scopeShown && match_) title += matchCaption_;
    const auto summaryBounds = alignReadout_.isVisible() ? alignReadout_.getBounds()
        : !differenceOnly && !sumLabel_.isEmpty() ? sumLabel_
        : scopeWindow_.isVisible() ? scopeWindow_.getBounds() : juce::Rectangle<int>{};
    const int titleWidth = summaryBounds.isEmpty() ? plot.getWidth() : std::max(0, summaryBounds.getX() - plot.getX());
    g.drawText(title, plot.getX(), scroll_.getBottom(), titleWidth,
               std::max(0, plot.getY() - scroll_.getBottom()), juce::Justification::centredLeft);
    if (!differenceOnly && !alignReadout_.isVisible() && !sumLabel_.isEmpty()) {
        const auto stereoDb = [](const std::array<double, 2> &v) {
            const auto db = [](double sample) { return sample > 0. ? juce::String(20. * std::log10(sample), 1) : juce::String("-inf"); };
            return db(v[0]) + " / " + db(v[1]);
        };
        g.setColour(look::textDim);
        g.setFont(look::font());
        const juce::String summary = sum_ && scopeCurrent
            ? "Focus sum   Peak " + stereoDb(sum_->peak) + "   RMS " + stereoDb(sum_->rms) + " dBFS"
            : "Focus sum   --";
        g.drawFittedText(summary, sumLabel_, juce::Justification::centredRight, 1);
    }
    g.setColour(look::frame);
    g.fillRect(plot_);
    const float traceOpacity = stoppedScopeRevision_ ? .55f : 1.f;
    if (scopeGridStamp_ && scopeGridStamp_->plot == plot && scopeTraceRevision()) {
        for (const auto &line : scopeGrid_) {
            g.setColour((line.bar ? look::outline : look::gridLine).withMultipliedAlpha(traceOpacity));
            g.drawVerticalLine(line.x, plot_.getY(), plot_.getBottom());
        }
    }
    paintScopeRegion(g);
    if (members_.empty()) {
        g.setColour(look::text);
        g.setFont(look::font());
        g.drawText("Choose channels to compare", plot_, juce::Justification::centred);
        return;
    }
    if (!scopeCurrent) return;
    const auto timeTransform = scopeShown && !spectrumOnly ? scopeTimeTransform() : juce::AffineTransform{};
    g.setColour(look::outline);
    const auto drawScopeZero = [&](juce::Rectangle<int> lane) {
        const auto y = lane.getCentreY() + juce::roundToInt(scopePanCentre() * .5 * lane.getHeight());
        if (y >= lane.getY() && y < lane.getBottom()) g.drawHorizontalLine(y, plot_.getX(), plot_.getRight());
    };
    if (differenceOnly) {
        if (!spectrumOnly) drawScopeZero(plot);
        if (differenceReady) {
            juce::Graphics::ScopedSaveState saved(g);
            g.reduceClipRegion(plot);
            if (spectrumOnly) {
                g.setColour(look::text.withMultipliedAlpha(traceOpacity));
                if (differenceSpectrumPath_.isEmpty())
                    g.drawText("No difference", plot_, juce::Justification::centred);
                else g.strokePath(differenceSpectrumPath_, juce::PathStrokeType(look::heavyLine));
            } else {
                g.setColour(look::textDim.withMultipliedAlpha(traceOpacity));
                g.strokePath(scopeDifferencePaths_[1], juce::PathStrokeType(look::line), timeTransform);
                g.setColour(look::text.withMultipliedAlpha(traceOpacity));
                g.strokePath(scopeDifferencePaths_[0], juce::PathStrokeType(look::heavyLine), timeTransform);
            }
        } else {
            g.setColour(look::textDim);
            g.setFont(look::font());
            g.drawText(differenceRequest_ ? "Waiting for aligned playback" : "Select another channel for B",
                       plot_, juce::Justification::centred);
        }
        return;
    }
    if (scopeShown) {
        paintScopePhase(g);
        juce::Graphics::ScopedSaveState scopeClip(g);
        g.reduceClipRegion(plot);
        const bool stacked = display == ScopeDisplay::Stack && !plot_.isEmpty() && !members_.empty();
        if (!stacked) drawScopeZero(plot);
        if (sumShown) {
            g.setColour(look::text.withAlpha(.6f * traceOpacity));
            g.strokePath(scopeSumPaths_[1], juce::PathStrokeType(look::line), timeTransform);
            g.setColour(look::text.withMultipliedAlpha(traceOpacity));
            g.strokePath(scopeSumPaths_[0], juce::PathStrokeType(look::heavyLine), timeTransform);
        }
        const auto drawMember = [&g, traceOpacity](const Member &member, const juce::AffineTransform &transform) {
            g.setColour(member.colour.withMultipliedAlpha(.6f * traceOpacity));
            g.strokePath(member.scopePaths[1], juce::PathStrokeType(look::hairline), transform);
            g.setColour(member.colour.withMultipliedAlpha(traceOpacity));
            g.strokePath(member.scopePaths[0], juce::PathStrokeType(look::line), transform);
        };
        if (stacked) {
            const auto scale = 1.f / static_cast<float>(members_.size());
            const auto laneHeight = plot_.getHeight() * scale;
            for (std::size_t i = 0; i < members_.size(); ++i) {
                const auto y = plot_.getY() + static_cast<float>(i) * laneHeight;
                const auto lane = juce::Rectangle<float>(plot_.getX(), y, plot_.getWidth(), laneHeight).toNearestInt();
                juce::Graphics::ScopedSaveState saved(g);
                g.reduceClipRegion(lane);
                g.setColour(look::outline);
                drawScopeZero(lane);
                const auto transform = timeTransform.followedBy(
                    juce::AffineTransform::scale(1.f, scale).translated(0.f, y - plot_.getY() * scale));
                drawMember(members_[i], transform);
            }
        } else if (!sumOnly) {
            for (const auto &member : members_) drawMember(member, timeTransform);
        }
        if (sumOnly ? !sumShown : !scopeHasTrace_) {
            g.setColour(look::textDim);
            g.setFont(look::font());
            const bool waitingForAlignment = sumOnly ? scopePlaying_ : alignment_ == Alignment::Ready;
            g.drawText(waitingForAlignment ? "Waiting for aligned playback" : "Waiting for playback",
                       plot_, juce::Justification::centred);
        }
        return;
    }
    for (int i = 1; i < 6; ++i)
        g.drawHorizontalLine(juce::roundToInt(plot_.getY() + plot_.getHeight() * static_cast<float>(i) / 6.f), plot_.getX(),
                             plot_.getRight());
    g.setColour(look::selection);
    if (!masked_.empty()) {
        juce::RectangleList<int> hatch;
        for (const auto range : masked_)
            hatch.add(
                juce::Rectangle<float>(range.getStart(), plot_.getY(), range.getLength(), plot_.getHeight())
                    .toNearestInt());
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(hatch);
        for (float x = plot_.getX() - plot_.getHeight(); x < plot_.getRight(); x += static_cast<float>(look::gap))
            g.drawLine(x, plot_.getBottom(), x + plot_.getHeight(), plot_.getY(), look::hairline);
    }
    for (const auto &m : members_) {
        g.setColour(m.colour);
        g.strokePath(m.path, juce::PathStrokeType(m.id == main_ ? look::heavyLine : look::line));
    }
    if (match_) {
        g.setColour(look::accent);
        g.drawHorizontalLine(plot.getCentreY(), plot_.getX(), plot_.getRight());
        g.strokePath(matchPath_, juce::PathStrokeType(look::heavyLine));
    }
}
} // namespace adi::ui
