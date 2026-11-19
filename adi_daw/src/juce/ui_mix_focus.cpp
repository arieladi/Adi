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
    scope.setTooltip("Show the Focus channels' post-fader waveforms and their sum when aligned.");
    scope.onClick = [this] { redraw_ = true; repaint(); };
    scroll_.setViewedComponent(&strip_, false);
    scroll_.setScrollBarsShown(false, true);
    for (auto *c : std::array<juce::Component *, 10>{&available, &add, &star, &mute, &solo, &hearOne, &hearAll,
                                                     &edit, &scope, &scroll_})
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
        m.outputs = 0;
        m.data.reset();
        m.energy.clear();
        m.spectrumChanged = true;
        m.path.clear();
        for (auto &path : m.scopePaths) path.clear();
    }
    scopeSources_.clear();
    clearScopeSum();
    scopeHasTrace_ = false;
    masked_.clear();
    broker_.reset();
    redraw_ = true;
}
void MixFocus::dropIfActive() {
    if (!broker_.expired() ||
        std::any_of(members_.begin(), members_.end(), [](const auto &member) { return member.outputs != 0; }))
        drop();
}
void MixFocus::suspend(bool v) {
    if (v == suspended_) return;
    suspended_ = v;
    if (v) dropIfActive();
}
void MixFocus::visibilityChanged() {
    if (!isShowing())
        dropIfActive();
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
    const bool playing = root_.playing();
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
    const bool scopeShown = scope.getToggleState();
    if (scopeShown && !playing) {
        dropIfActive();
        if (!alive) return;
        if (redraw_) {
            rebuildScope();
            redraw_ = false;
            repaint(plot_.toNearestInt().getUnion(sumLabel_));
        }
        return;
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
            if (next != member.data) {
                redraw_ = true;
                member.data = std::move(next);
                member.spectrumChanged = true;
            }
        }
        if (!members_.empty())
            broker->requestFrame();
    }
    if (playing && alignment_ == Alignment::WaitForFresh && !members_.empty() &&
        std::all_of(members_.begin(), members_.end(), [](const auto &member) {
            if (!member.afterLoop || !member.data || !member.data->advancing) return false;
            const auto &boundary = *member.afterLoop;
            return member.data->generation > boundary.generation ||
                   (member.data->generation == boundary.generation && member.data->firstSequence >= boundary.written);
        })) {
        alignment_ = Alignment::Ready;
        redraw_ = true;
    }
    if (redraw_) {
        if (!scope.getToggleState()) rebuildPlot();
        rebuildScope();
        redraw_ = false;
        repaint(plot_.toNearestInt().getUnion(sumLabel_));
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
    scopeRange_.reset();
    sum_.reset();
    for (auto &path : scopeSumPaths_) path.clear();
}
void MixFocus::rebuildScope() {
    clearScopeSum();
    scopeHasTrace_ = false;
    scopeSources_.clear();
    const bool scopeShown = scope.getToggleState();
    if (scopeShown)
        for (auto &member : members_)
            for (auto &path : member.scopePaths) path.clear();
    if (!scopePlaying_ || (!scopeShown && sumLabel_.isEmpty())) return;

    const auto plotColumns = static_cast<std::size_t>(std::max(0.f, plot_.getWidth()));
    const auto appendColumn = [this](juce::Path &path, std::size_t column, std::size_t columns,
                                     double low, double high) {
        const auto x = plot_.getX() + static_cast<float>(column) * plot_.getWidth() / static_cast<float>(columns);
        const auto y = [this](double value) {
            return plot_.getCentreY() - static_cast<float>(std::clamp(value, -1., 1.)) * plot_.getHeight() * .5f;
        };
        if (column == 0) path.startNewSubPath(x, y(low));
        else path.lineTo(x, y(low));
        if (high != low) path.lineTo(x, y(high));
    };
    const bool aligned = alignment_ == Alignment::Ready;
    if (aligned) {
        for (const auto &member : members_) scopeSources_.push_back(member.data.get());
        scopeRange_ = focusScopeRange(scopeSources_);
        if (scopeRange_ && scopeShown) {
            const auto columns = std::min(plotColumns, scopeRange_->frames);
            const auto reading = focusProjectSum(scopeSources_, *scopeRange_, columns,
                [&](std::size_t column, const FocusSumColumn &extrema) {
                    for (std::size_t ch = 0; ch < scopeSumPaths_.size(); ++ch)
                        appendColumn(scopeSumPaths_[ch], column, columns, extrema.low[ch], extrema.high[ch]);
                });
            if (!sumLabel_.isEmpty()) sum_ = reading;
        } else if (scopeRange_ && !sumLabel_.isEmpty()) {
            sum_ = focusReadSum(scopeSources_, *scopeRange_);
        }
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
        for (std::size_t ch = 0; ch < member.scopePaths.size(); ++ch) {
            auto &path = member.scopePaths[ch];
            const auto &samples = ch == 0 ? data.scopeLeft : data.scopeRight;
            for (std::size_t p = 0; p < steps; ++p) {
                const auto from = start + (p * range.frames) / steps;
                const auto to = start + ((p + 1) * range.frames) / steps;
                const auto [low, high] = std::minmax_element(samples.data() + from, samples.data() + to);
                appendColumn(path, p, steps, *low, *high);
            }
        }
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
    auto titleRow = r.removeFromTop(buttonRow);
    sumLabel_ = titleRow.getWidth() >= 4 * look::mixFocusSourceWidth
                    ? titleRow.removeFromRight(2 * look::mixFocusSourceWidth) : juce::Rectangle<int>{};
    auto actions = r.removeFromBottom(buttonRow);
    const auto w = std::min(chipWidth, actions.getWidth() / 7);
    for (auto *button : {&star, &mute, &solo, &hearOne, &hearAll, &edit})
        button->setBounds(actions.removeFromLeft(w).reduced(hairline));
    scope.setBounds(actions.removeFromLeft(w).reduced(hairline));
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
    const bool scopeShown = scope.getToggleState();
    const bool sumShown = scopeShown && (!scopeSumPaths_[0].isEmpty() || !scopeSumPaths_[1].isEmpty());
    juce::String title = scopeShown ? "Scope" : "Together";
    if (scopeShown && alignment_ != Alignment::Ready)
        title += "   Individual traces";
    else if (scopeShown && scopeRange_) {
        title += "   " + juce::String(1000. * static_cast<double>(scopeRange_->frames) / scopeRange_->sampleRate, 1) + " ms";
        if (sumShown) title += "   Sum overlay";
    }
    const int titleWidth = sumLabel_.isEmpty() ? plot.getWidth() : std::max(0, sumLabel_.getX() - plot.getX());
    g.drawText(title, plot.getX(), scroll_.getBottom(), titleWidth,
               std::max(0, plot.getY() - scroll_.getBottom()), juce::Justification::centredLeft);
    if (!sumLabel_.isEmpty()) {
        const auto stereoDb = [](const std::array<double, 2> &v) {
            const auto db = [](double sample) { return sample > 0. ? juce::String(20. * std::log10(sample), 1) : juce::String("-inf"); };
            return db(v[0]) + " / " + db(v[1]);
        };
        g.setColour(look::textDim);
        g.setFont(look::font());
        const juce::String summary = sum_ ? "Focus sum   Peak " + stereoDb(sum_->peak) + "   RMS " + stereoDb(sum_->rms) + " dBFS"
                                          : "Focus sum   --";
        g.drawFittedText(summary, sumLabel_, juce::Justification::centredRight, 1);
    }
    g.setColour(look::frame);
    g.fillRect(plot_);
    if (members_.empty()) {
        g.setColour(look::text);
        g.setFont(look::font());
        g.drawText("Choose channels to compare", plot_, juce::Justification::centred);
        return;
    }
    g.setColour(look::outline);
    if (scopeShown) {
        g.drawHorizontalLine(plot.getCentreY(), plot_.getX(), plot_.getRight());
        if (sumShown) {
            juce::Graphics::ScopedSaveState saved(g);
            g.reduceClipRegion(plot);
            g.setColour(look::text.withAlpha(.6f));
            g.strokePath(scopeSumPaths_[1], juce::PathStrokeType(look::line));
            g.setColour(look::text);
            g.strokePath(scopeSumPaths_[0], juce::PathStrokeType(look::heavyLine));
        }
        for (const auto &member : members_) {
            g.setColour(member.colour.withMultipliedAlpha(.6f));
            g.strokePath(member.scopePaths[1], juce::PathStrokeType(look::hairline));
            g.setColour(member.colour);
            g.strokePath(member.scopePaths[0], juce::PathStrokeType(look::line));
        }
        if (!scopeHasTrace_) {
            g.setColour(look::textDim);
            g.setFont(look::font());
            g.drawText(alignment_ == Alignment::Ready ? "Waiting for aligned playback" : "Waiting for playback",
                       plot_, juce::Justification::centred);
        }
        return;
    }
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
