// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mix_focus.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include "adi/ui/track_order.hpp"
#include <algorithm>
#include <array>
#include <cmath>
namespace adi::ui {
namespace {
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
    scroll_.setViewedComponent(&strip_, false);
    scroll_.setScrollBarsShown(false, true);
    for (auto *c : std::array<juce::Component *, 9>{&available, &add, &star, &mute, &solo, &hearOne, &hearAll,
                                                    &edit, &scroll_})
        addAndMakeVisible(*c);
    refreshControls();
}
MixFocus::~MixFocus() { scroll_.setViewedComponent(nullptr); }
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
    members_ = std::move(next);
    ++membershipPose_;
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
    for (auto &m : members_)
        if (const auto *t = snapshot.findTrack(m.id)) {
            m.colour =
                t->color ? look::paletteColour(*t->color) : look::trackColour(static_cast<int>(m.id % 70));
            m.chip->setButtonText((main_ == m.id ? juce::String::fromUTF8("\xe2\x98\x85 ") : juce::String()) +
                                  juce::String::fromUTF8(t->name.c_str()));
            m.chip->setColour(juce::TextButton::textColourOffId, m.colour.brighter());
            m.chip->setToggleState(m.id == active_, juce::dontSendNotification);
        }
    const auto *t = snapshot.findTrack(active_);
    for (auto *button : {&star, &mute, &solo, &hearOne, &hearAll, &edit})
        button->setEnabled(t != nullptr);
    mute.setToggleState(t && t->muted, juce::dontSendNotification);
    solo.setToggleState(t && t->soloed, juce::dontSendNotification);
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
    std::vector<SharedAnalysis::Lease> released;
    released.reserve(members_.size());
    for (auto &m : members_) {
        released.push_back(std::move(m.lease));
        m.data.reset();
        m.energy.clear();
        m.spectrumChanged = true;
        m.path.clear();
    }
    masked_.clear();
    broker_.reset();
    redraw_ = true;
}
void MixFocus::suspend(bool v) {
    if (v == suspended_) return;
    suspended_ = v;
    if (v) drop();
}
void MixFocus::visibilityChanged() {
    if (!isShowing())
        drop();
}
void MixFocus::parentHierarchyChanged() {
    if (!isShowing())
        drop();
}
void MixFocus::frame(const SnapshotReader &reader) {
    if (!isShowing() || suspended_)
        return;
    const SafePointer<MixFocus> alive(this);
    if (density_ != look::densityId()) {
        density_ = look::densityId();
        resized();
        if (!alive) return;
    }
    if (generation_ != root_.generation() || snapshot_ != reader.get()) {
        const auto last = available.getSelectedId() - 1;
        const auto oldChoice = last >= 0 && static_cast<size_t>(last) < choices_.size()
                                   ? choices_[static_cast<size_t>(last)]
                                   : 0;
        choices_.clear();
        available.clear(juce::dontSendNotification);
        for (const auto &t : reader.tracks())
            if (focusable(*t)) {
                choices_.push_back(t->id);
                const int item = static_cast<int>(choices_.size());
                const auto name = juce::String::fromUTF8(t->name.c_str());
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
    const auto broker = root_.sharedAnalysis;
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
    if (broker && broker->active()) {
        const auto pose = membershipPose_;
        for (std::size_t i = 0; i < members_.size(); ++i) {
            if (!members_[i].lease) {
                SharedAnalysis::Request req;
                req.track = members_[i].id;
                req.channel = SharedAnalysis::Channel::StereoPower;
                SharedAnalysis::Lease acquired;
                try { acquired = broker->acquire(req); }
                catch (...) {}
                if (!alive || membershipPose_ != pose || !isShowing() || suspended_ || root_.sharedAnalysis != broker)
                    return;
                members_[i].lease = std::move(acquired);
            }
            auto &member = members_[i];
            auto next = member.lease.read();
            if (next && next->advancing &&
                next->firstStamp + next->request.fftSize > root_.renderedSamplePosition())
                next = member.data && member.data->sourceEpoch == next->sourceEpoch ? member.data : nullptr;
            if (next != member.data) {
                redraw_ = true;
                member.data = std::move(next);
                member.spectrumChanged = true;
            }
        }
        if (!members_.empty())
            broker->requestFrame();
    }
    if (redraw_) {
        rebuildPlot();
        redraw_ = false;
        repaint(plot_.toNearestInt());
    }
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
        for (size_t i = 0; i < bins_ && i * binHz <= 20000.; ++i)
            normalizedBinX_.push_back(
                static_cast<float>(std::max(0., std::log(std::max(20., i * binHz) / 20.) / std::log(1000.))));
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
    if (!primary || members_.size() < 2)
        return;
    const auto &p = *primary->data;
    std::fill(maskers_.begin(), maskers_.end(), 0.);
    size_t others = 0;
    for (auto &m : members_) {
        if (!m.data || m.data->sampleRate != rate_ || m.data->binsDbfs.size() != bins_ ||
            m.data->firstStamp != p.firstStamp || m.data->advancing != p.advancing)
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
    r.removeFromTop(buttonRow);
    auto actions = r.removeFromBottom(buttonRow);
    const auto w = std::min(chipWidth, actions.getWidth() / 6);
    for (auto *button : {&star, &mute, &solo, &hearOne, &hearAll, &edit})
        button->setBounds(actions.removeFromLeft(w).reduced(hairline));
    plot_ = r.reduced(plotInset).toFloat();
    for (auto &m : members_)
        m.spectrumChanged = true;
    redraw_ = true;
}
void MixFocus::paint(juce::Graphics &g) {
    g.fillAll(look::panel);
    g.setColour(look::text);
    g.setFont(look::font(look::fontBody, true));
    const auto plot = plot_.toNearestInt();
    g.drawText("Together", plot.getX(), scroll_.getBottom(), plot.getWidth(),
               std::max(0, plot.getY() - scroll_.getBottom()), juce::Justification::centredLeft);
    g.setColour(look::frame);
    g.fillRect(plot_);
    if (members_.empty()) {
        g.setColour(look::text);
        g.setFont(look::font());
        g.drawText("Choose channels to compare", plot_, juce::Justification::centred);
        return;
    }
    g.setColour(look::outline);
    for (int i = 1; i < 6; ++i)
        g.drawHorizontalLine(juce::roundToInt(plot_.getY() + plot_.getHeight() * i / 6.f), plot_.getX(),
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
}
} // namespace adi::ui
