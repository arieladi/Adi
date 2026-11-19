// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mix_channels.hpp"
#include "adi/engine/mixer.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <utility>

namespace adi::ui {
namespace {
constexpr int kAllChannels = 1, kSoundingNow = 2;
constexpr int kSpectrogram = 1, kSpectrum = 2;
constexpr float kFloor = .001f; // -60 dBFS
}

MixChannelBrowser::MixChannelBrowser(AdiRootComponent &root) : root_(root) {
    setTitle("Mix View channels");
    search.setTitle("Search channels and tools");
    search.setTextToShowWhenEmpty("Search channels and tools", look::textDim);
    search.onTextChange = [this] {
        const auto next = search.getText().trim();
        if (query_ == next) return;
        query_ = next;
        toolMatches_.clear();
        toolSearchGeneration_.reset();
        const juce::Component::SafePointer<MixChannelBrowser> alive(this);
        refreshToolMatches();
        if (alive) rebuildList();
    };
    show.setTitle("Show channels");
    show.addItem("All channels", kAllChannels);
    show.addItem("Sounding now", kSoundingNow);
    show.setSelectedId(kAllChannels, juce::dontSendNotification);
    show.onChange = [this] { chooseFilter(); };
    refreshSounding.setTitle("Refresh sounding channels");
    refreshSounding.setTooltip("Take a fresh snapshot of channels at or above -60 dBFS.");
    refreshSounding.onClick = [this] {
        if (sounding_ && current() && !root_.mixViewPanel.stack.model().capture()) captureSounding();
    };
    addChildComponent(refreshSounding);
    list.setTitle("Channels");
    list.setColour(juce::ListBox::backgroundColourId, look::panel);
    list.setModel(this);
    infoMode.setTitle("Info analyser");
    infoMode.addItem("Spectrogram", kSpectrogram);
    infoMode.addItem("Spectrum", kSpectrum);
    infoMode.setSelectedId(kSpectrogram, juce::dontSendNotification);
    infoMode.onChange = [this] {
        preview.setMode(infoMode.getSelectedId() == kSpectrum ? SharedSpectrumView::Mode::Spectrum
                                                             : SharedSpectrumView::Mode::Spectrogram);
    };
    preview.setMode(SharedSpectrumView::Mode::Spectrogram);
    preview.setTitle("Channel analysis preview");
    preview.setTooltip("Open Analysers");
    preview.setInterceptsMouseClicks(true, false);
    preview.addMouseListener(this, false);
    openInfo.setTitle("Open Analysers");
    openInfo.onClick = [this] { if (current() && onOpenInfo) onOpenInfo(); };
    for (auto *child : std::array<juce::Component *, 6>{&search, &list, &show, &openInfo, &infoMode, &preview})
        addAndMakeVisible(*child);
}

MixChannelBrowser::~MixChannelBrowser() {
    preview.removeMouseListener(this);
    list.setModel(nullptr);
}

bool MixChannelBrowser::current() const noexcept {
    return isShowing() && root_.state().mixView && generation_ == root_.generation() &&
           snapshot_ != nullptr && snapshot_ == root_.current().get();
}

int MixChannelBrowser::getNumRows() { return static_cast<int>(rows_.size()); }

juce::String MixChannelBrowser::getNameForRow(int row) {
    const auto track = trackAt(row);
    if (!track) return {};
    auto name = entries_[rows_[static_cast<std::size_t>(row)]].name;
    const auto label = focusMemberLabel(track);
    if (label.isNotEmpty()) {
        name += ", Focus " + label;
        if (track == focusMain_) name += ", main";
    }
    return name;
}

void MixChannelBrowser::setFocusMembers(std::vector<std::int64_t> members, std::int64_t main) {
    if (focusMembers_ == members && focusMain_ == main) return;
    focusMembers_ = std::move(members);
    focusMain_ = main;
    list.repaint();
}

juce::String MixChannelBrowser::focusMemberLabel(std::int64_t track) const {
    const auto member = std::find(focusMembers_.begin(), focusMembers_.end(), track);
    if (member == focusMembers_.end()) return {};
    juce::String label;
    for (auto ordinal = member - focusMembers_.begin() + 1; ordinal > 0; ordinal = (ordinal - 1) / 26)
        label = juce::String::charToString(static_cast<juce::juce_wchar>('A' + (ordinal - 1) % 26)) + label;
    return label;
}

std::int64_t MixChannelBrowser::trackAt(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return 0;
    return entries_[rows_[static_cast<std::size_t>(row)]].id;
}

void MixChannelBrowser::syncSelection() {
    const juce::ScopedValueSetter<bool> guard(syncing_, true);
    const auto it = std::find_if(rows_.begin(), rows_.end(), [this](std::size_t i) { return entries_[i].id == selected_; });
    if (it == rows_.end()) list.deselectAllRows();
    else list.selectRow(static_cast<int>(it - rows_.begin()), true);
}

void MixChannelBrowser::selectedRowsChanged(int row) {
    if (syncing_) return;
    const auto id = trackAt(row);
    if (!current() || root_.mixViewPanel.stack.model().capture()) {
        syncSelection();
        return;
    }
    if (id && onChoose) onChoose(id);
}

void MixChannelBrowser::rebuildList() {
    rows_.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto &e = entries_[i];
        if (membership_ && !std::binary_search(membership_->begin(), membership_->end(), e.id)) continue;
        if (query_.isNotEmpty() && !e.name.containsIgnoreCase(query_) &&
            !std::binary_search(toolMatches_.begin(), toolMatches_.end(), e.id)) continue;
        rows_.push_back(i);
    }
    const juce::ScopedValueSetter<bool> guard(syncing_, true);
    list.updateContent();
    syncSelection();
    list.repaint();
}

bool MixChannelBrowser::refreshToolMatches() {
    if (query_.isEmpty() || !isShowing()) return false;
    const auto generation = root_.projectViewGeneration();
    if (toolSearchGeneration_ == generation) return false;

    // Read device names from the passive publication. An unchanged project
    // view does not need another lookup when live parameter values change.
    const auto query = query_;
    const juce::Component::SafePointer<MixChannelBrowser> alive(this);
    const auto devices = root_.devices.publication ? root_.devices.publication() : nullptr;
    if (!alive || generation != root_.projectViewGeneration() || query != query_ || !isShowing()) return false;
    toolSearchGeneration_ = generation;

    std::vector<std::int64_t> matches;
    if (devices)
        for (const auto &device : *devices)
            if (juce::String::fromUTF8(device.name.c_str()).containsIgnoreCase(query))
                matches.push_back(device.track);
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    if (matches == toolMatches_) return false;
    toolMatches_ = std::move(matches);
    return true;
}

bool MixChannelBrowser::rebuildGroups(const SnapshotReader &reader, std::int64_t reference) {
    rowsFromSnapshot(reader, nextTree_);
    const auto same = [](const TrackRow &a, const TrackRow &b) {
        return a.id == b.id && a.parent == b.parent && a.index == b.index && a.group == b.group &&
               a.main == b.main && a.ret == b.ret && a.name == b.name && a.colour == b.colour;
    };
    if (reference == reference_ && std::equal(tree_.begin(), tree_.end(), nextTree_.begin(), nextTree_.end(), same))
        return false;
    reference_ = reference;
    tree_.swap(nextTree_);
    order_.build(reader, tree_);
    entries_.clear();
    main_ = 0;
    std::vector<Group> updatedGroups;
    show.clear(juce::dontSendNotification);
    show.addItem("All channels", kAllChannels);
    show.addItem("Sounding now", kSoundingNow);
    int selectedItem = sounding_ ? kSoundingNow : kAllChannels;
    for (const auto &place : order_.places()) {
        if (place.id == reference_) continue;
        if (place.main) main_ = place.id;
        const auto &track = tree_[place.at];
        const auto colour = track.colour >= 0 ? look::paletteColour(track.colour) : look::trackColour(place.at);
        entries_.push_back({place.id, juce::String::fromUTF8(track.name.c_str()), colour, place.depth, place.group});
        if (!place.group) continue;
        const auto old = std::find_if(groups_.begin(), groups_.end(), [&place](const Group &g) { return g.track == place.id; });
        const int item = old == groups_.end() ? nextGroupItem_++ : old->item;
        updatedGroups.push_back({place.id, item});
        const auto &name = entries_.back().name;
        show.addItem(name.isNotEmpty() ? name : juce::String("Group"), item);
        if (place.id == group_) selectedItem = item;
    }
    groups_ = std::move(updatedGroups);
    if (selectedItem == kAllChannels) group_ = 0;
    show.setSelectedId(selectedItem, juce::dontSendNotification);
    return true;
}

int MixChannelBrowser::filterItem() const noexcept {
    if (sounding_) return kSoundingNow;
    const auto group = std::find_if(groups_.begin(), groups_.end(), [this](const Group &g) { return g.track == group_; });
    return group != groups_.end() ? group->item : kAllChannels;
}

void MixChannelBrowser::chooseFilter() {
    const int item = show.getSelectedId();
    const auto selected = std::find_if(groups_.begin(), groups_.end(), [item](const Group &g) { return g.item == item; });
    if (!current() || root_.mixViewPanel.stack.model().capture() ||
        (item != kAllChannels && item != kSoundingNow && selected == groups_.end())) {
        show.setSelectedId(filterItem(), juce::dontSendNotification);
        return;
    }
    if (item == filterItem()) return; // a repeated or late selection is not a Refresh
    group_ = selected != groups_.end() ? selected->track : 0;
    sounding_ = item == kSoundingNow;
    if (refreshSounding.isVisible() != sounding_) {
        refreshSounding.setVisible(sounding_);
        resized();
    }
    if (sounding_) captureSounding();
    else updateGroupMembership();
}

void MixChannelBrowser::updateGroupMembership() {
    std::optional<std::vector<std::int64_t>> updated;
    if (group_ != 0) {
        auto parent = std::find_if(entries_.begin(), entries_.end(), [this](const Entry &e) { return e.id == group_; });
        if (parent != entries_.end()) {
            updated.emplace();
            updated->push_back(parent->id);
            for (auto child = std::next(parent); child != entries_.end() && child->depth > parent->depth; ++child)
                updated->push_back(child->id);
            if (main_ != 0) updated->push_back(main_);
            std::sort(updated->begin(), updated->end());
        }
    }
    setMembership(std::move(updated));
}

void MixChannelBrowser::captureSounding() {
    std::vector<std::int64_t> ids;
    for (const auto &entry : entries_)
        if (peak(entry.id) >= kFloor) ids.push_back(entry.id);
    std::sort(ids.begin(), ids.end());
    setMembership(std::move(ids));
}

void MixChannelBrowser::setMembership(std::optional<std::vector<std::int64_t>> updated) {
    const bool changed = updated != membership_;
    membership_ = std::move(updated);
    rebuildList();
    if (changed || filterApplied_ != membership_.has_value()) {
        filterApplied_ = membership_.has_value();
        if (onFilter) onFilter(membership_);
    }
}

float MixChannelBrowser::peak(std::int64_t track) const {
    const auto *meter = root_.mixer.meter ? root_.mixer.meter(track) : nullptr;
    const float amplitude = meter ? meter->peak.load(std::memory_order_relaxed) : 0.f;
    return std::isfinite(amplitude) ? amplitude : 0.f;
}

int MixChannelBrowser::level(std::int64_t track) const {
    const auto amplitude = peak(track);
    if (!(amplitude > kFloor)) return 0;
    return juce::roundToInt(std::clamp(1.f + std::log10(amplitude) / 3.f, 0.f, 1.f) *
                          static_cast<float>(look::mixChannelLevelWidth));
}

void MixChannelBrowser::frame(const SnapshotReader &reader) {
    if (!isVisible()) {
        if (std::exchange(filterApplied_, false) && onFilter) onFilter({});
        return;
    }
    const juce::Component::SafePointer<MixChannelBrowser> alive(this);
    const auto reference = root_.referenceTrack();
    if (generation_ != root_.generation() || snapshot_ != reader.get() || reference != reference_) {
        generation_ = root_.generation();
        snapshot_ = reader.get();
        if (rebuildGroups(reader, reference)) {
            if (sounding_) rebuildList(); // metadata updates do not resample the signal
            else updateGroupMembership();
            if (!alive) return;
        }
    }
    if (!filterApplied_ && membership_) {
        filterApplied_ = true;
        if (onFilter) onFilter(membership_);
        if (!alive) return;
    }
    if (refreshToolMatches()) rebuildList();
    if (!alive) return;
    const auto selected = root_.analysisTrack(reader, true);
    if (selected_ != selected) { selected_ = selected; syncSelection(); }
    if (!isShowing()) return;
    const int first = std::max(0, list.getRowContainingPosition(1, 1));
    const int last = std::min(getNumRows(), first + list.getHeight() / list.getRowHeight() + 2);
    for (int row = first; row < last; ++row) {
        auto &e = entries_[rows_[static_cast<std::size_t>(row)]];
        if (const int pixels = level(e.id); e.level != pixels) {
            e.level = pixels;
            list.repaintRow(row);
        }
    }
    const auto &state = root_.state();
    preview.setPalette(state.analysisPalette, state.analysisPaletteReverse);
    preview.setSpectrumSlope(spectrumSlopeFromId(state.analysisSpectrumSlope));
    preview.present(root_.sharedAnalysis, selected != 0 ? selected : main_);
    preview.frame();
}

void MixChannelBrowser::paint(juce::Graphics &g) { g.fillAll(look::panel); }

void MixChannelBrowser::paintOverChildren(juce::Graphics &g) {
    if (!sounding_ || !membership_ || !membership_->empty()) return;
    g.setColour(look::textDim);
    g.setFont(look::font(look::fontBody));
    g.drawFittedText("No channels were sounding.", list.getBounds().reduced(look::gap),
                     juce::Justification::centred, 2);
}

void MixChannelBrowser::paintListBoxItem(int row, juce::Graphics &g, int width, int height, bool selected) {
    if (!trackAt(row)) return;
    const auto &e = entries_[rows_[static_cast<std::size_t>(row)]];
    if (selected) g.fillAll(look::chooser);
    auto line = juce::Rectangle<int>(width, height).reduced(look::border, 0);
    const int dotDiameter = look::gap;
    const auto dot = line.removeFromLeft(dotDiameter).withSizeKeepingCentre(dotDiameter, dotDiameter);
    g.setColour(e.colour);
    g.fillEllipse(dot.toFloat());
    line.removeFromLeft(look::border);
    const auto label = focusMemberLabel(e.id);
    if (label.isNotEmpty()) {
        const auto slot = line.removeFromRight(look::mixFocusChipHeight);
        const auto badge = slot.reduced(look::border, look::border).toFloat();
        g.setColour(e.colour);
        g.fillRoundedRectangle(badge, static_cast<float>(look::border));
        if (e.id == focusMain_) {
            g.setColour(look::text);
            g.drawRoundedRectangle(badge, static_cast<float>(look::border), look::hairline);
        }
        g.setColour(look::ink);
        g.setFont(look::font(look::fontBody, true));
        g.drawFittedText(label, badge.toNearestInt(), juce::Justification::centred, 1);
    }
    const auto meter = line.removeFromRight(look::mixChannelLevelWidth);
    line.removeFromRight(look::border);
    g.setColour(look::text);
    g.setFont(look::font(look::fontBody, e.group));
    g.drawFittedText(e.name, line, juce::Justification::centredLeft, 1);
    const auto bar = meter.withSizeKeepingCentre(meter.getWidth(), juce::roundToInt(look::heavyLine));
    g.setColour(look::frame);
    g.fillRect(bar);
    g.setColour(look::accent);
    g.fillRect(bar.withWidth(std::min(e.level, bar.getWidth())));
}

void MixChannelBrowser::resized() {
    auto area = getLocalBounds().reduced(look::border);
    list.setRowHeight(look::mixChannelRowHeight);
    search.setBounds(area.removeFromTop(look::mixChannelRowHeight));
    area.removeFromTop(look::border);
    auto info = area.removeFromBottom(look::mixInfoHeight);
    auto header = info.removeFromTop(look::mixChannelRowHeight);
    openInfo.setBounds(header.removeFromLeft(header.getWidth() / 3));
    header.removeFromLeft(look::border);
    infoMode.setBounds(header);
    info.removeFromTop(look::border);
    preview.setBounds(info);
    area.removeFromBottom(look::gap);
    if (refreshSounding.isVisible()) {
        refreshSounding.setBounds(area.removeFromBottom(look::mixChannelRowHeight));
        area.removeFromBottom(look::border);
    }
    show.setBounds(area.removeFromBottom(look::mixChannelRowHeight));
    area.removeFromBottom(look::border);
    list.setBounds(area);
}

void MixChannelBrowser::mouseDown(const juce::MouseEvent &event) {
    infoPressed_ = event.eventComponent == &preview && event.mods.isLeftButtonDown() && current();
}

void MixChannelBrowser::mouseUp(const juce::MouseEvent &event) {
    if (std::exchange(infoPressed_, false) && event.eventComponent == &preview &&
        preview.getLocalBounds().contains(event.getPosition()) && !event.mouseWasDraggedSinceMouseDown() &&
        current() && onOpenInfo) onOpenInfo();
}
} // namespace adi::ui
