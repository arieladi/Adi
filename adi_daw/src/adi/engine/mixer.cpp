// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <queue>
#include <type_traits>

namespace adi::engine {

StereoGainWide panGainsWide(double pan, PanLaw law) noexcept {
    const double p = std::isfinite(pan) ? std::clamp(pan, -1.0, 1.0) : 0.0;
    // THE CENTRE AND THE SIDES ARE EXACT, not computed. A centred track at 0 dB
    // must pass its samples through bit for bit -- ADR-0021's oracle compares
    // bytes, and every project rendered before the strip existed is centred --
    // and a hard side must be silence, not cos(pi/2) = 6e-17.
    const double theta = (p + 1.0) * std::numbers::pi / 4.0;
    switch (law) {
        case PanLaw::EqualPower:
            if (p == -1.0) return {1.0, 0.0};
            if (p == 1.0) return {0.0, 1.0};
            return {std::cos(theta), std::sin(theta)};
        case PanLaw::Balance:
            return p <= 0.0 ? StereoGainWide{1.0, 1.0 + p} : StereoGainWide{1.0 - p, 1.0};
        case PanLaw::Linear:
            return {(1.0 - p) / 2.0, (1.0 + p) / 2.0};
        case PanLaw::Live:
            break;
    }
    if (p == 0.0) return {1.0, 1.0};
    if (p == -1.0) return {std::numbers::sqrt2, 0.0};
    if (p == 1.0) return {0.0, std::numbers::sqrt2};
    return {std::numbers::sqrt2 * std::cos(theta), std::numbers::sqrt2 * std::sin(theta)};
}

StereoGain panGains(double pan, PanLaw law) noexcept {
    // Every float gain was computed in double and rounded once; it still is.
    const StereoGainWide w = panGainsWide(pan, law);
    return {static_cast<float>(w.left), static_cast<float>(w.right)};
}

double faderGain(double volumeDb) noexcept {
    if (std::isnan(volumeDb)) return 1.0;           // named by MixerStrips::sync
    if (volumeDb <= kSilenceDb) return 0.0;          // -inf included
    if (volumeDb == 0.0) return 1.0;                 // exact, for the same reason as the centre
    return std::pow(10.0, volumeDb / 20.0);
}

// ---------------------------------------------------------------------------

void StripNode::setStatic(double volumeDb, double pan, PanLaw law, bool audible) noexcept {
    volumeDb_.store(static_cast<float>(volumeDb), std::memory_order_relaxed);
    pan_.store(static_cast<float>(pan), std::memory_order_relaxed);
    law_.store(static_cast<int>(law), std::memory_order_relaxed);
    audible_.store(audible, std::memory_order_relaxed);
}

void StripNode::setLanes(const StripLanes* lanes, std::shared_ptr<void> keepAlive) noexcept {
    // The lifetime first, so a graph realised after this sees it; the pointer
    // is published with release so the audio thread sees a whole StripLanes.
    // The lanes it replaces stay alive: the retired graph that may still read
    // them retained their keepAlive when it was realised.
    keepAlive_ = std::move(keepAlive);
    lanes_.store(lanes, std::memory_order_release);
}

void StripNode::setDelay(std::int64_t projectSamples, std::int64_t projectRate) noexcept {
    delayProject_.store(projectSamples, std::memory_order_relaxed);
    delayRate_.store(projectRate > 0 ? projectRate : 48000, std::memory_order_relaxed);
}

std::int32_t StripNode::latencySamples() const noexcept {
    // Samples at the project's rate, played at the session's: a project at
    // 48 kHz opened at 96 kHz delays the same TIME, twice the samples.
    const double seconds = static_cast<double>(delayProject_.load(std::memory_order_relaxed)) /
                           static_cast<double>(delayRate_.load(std::memory_order_relaxed));
    const double rate = sampleRate_.load(std::memory_order_relaxed);
    const double limit = kMaxDelaySeconds * rate;
    const double samples = std::clamp(std::round(seconds * rate), -limit, limit);
    return static_cast<std::int32_t>(-samples);
}

void StripNode::prepare(double sampleRate, std::int32_t) {
    // Every graph that places this strip prepares it, while the graph before
    // may still be playing it: both values are atomics for that reason.
    sampleRate_.store(sampleRate > 0.0 ? sampleRate : 48000.0, std::memory_order_relaxed);
    // A time, not a count: five milliseconds at every rate ADR-0157 allows.
    const auto frames = std::lround(kRampSeconds * sampleRate);
    rampFrames_.store(static_cast<std::int32_t>(std::max<long>(1, frames)), std::memory_order_relaxed);
}

StripNode::Gains StripNode::desired(const StripLanes* lanes, std::int32_t frame) noexcept {
    double vol = volumeDb_.load(std::memory_order_relaxed);
    double pan = pan_.load(std::memory_order_relaxed);
    const int law = law_.load(std::memory_order_relaxed);
    bool heard = audible_.load(std::memory_order_relaxed);
    if (lanes != nullptr && transport_ != nullptr) {
        // A parked playhead is ONE position. sampleAt answers position + offset
        // whether the transport is playing or not, and reading a block ahead
        // of a stopped playhead made a sloped lane saw at the block rate.
        const std::int64_t at = transport_->sampleAt(transport_->playing() ? frame : 0);
        if (lanes->volume != nullptr) vol = valueAt(*lanes->volume, at);
        if (lanes->pan != nullptr) pan = valueAt(*lanes->pan, at);
        if (lanes->mute != nullptr && valueAt(*lanes->mute, at) >= 0.5) heard = false;
    }
    // Recomputed only when an input moved: a pow and a sin per change, never
    // per sample.
    if (haveLast_ && vol == lastVol_ && pan == lastPan_ && law == lastLaw_ && heard == lastHeard_)
        return lastGains_;
    const double fader = heard ? faderGain(vol) : 0.0;
    const StereoGainWide w = panGainsWide(pan, static_cast<PanLaw>(law));
    const StereoGain g = panGains(pan, static_cast<PanLaw>(law));
    Gains out;
    out.g[0] = static_cast<float>(fader * g.left);
    out.g[1] = static_cast<float>(fader * g.right);
    out.g[2] = static_cast<float>(fader);
    out.w[0] = fader * w.left;
    out.w[1] = fader * w.right;
    out.w[2] = fader;
    lastVol_ = vol;
    lastPan_ = pan;
    lastLaw_ = law;
    lastHeard_ = heard;
    lastGains_ = out;
    haveLast_ = true;
    return out;
}

template <typename T>
void StripNode::Ramp<T>::retarget(const T (&t)[3], std::int32_t frames) noexcept {
    if (!started) {
        for (int k = 0; k < 3; ++k) cur[k] = to[k] = t[k];
        remaining = 0;
        started = true;
        return;
    }
    if (t[0] == to[0] && t[1] == to[1] && t[2] == to[2]) return;
    // A new target, from wherever the gain is now -- mid-ramp included.
    for (int k = 0; k < 3; ++k) {
        to[k] = t[k];
        step[k] = (t[k] - cur[k]) / static_cast<T>(frames);
    }
    remaining = frames;
}

template <typename T>
void StripNode::Ramp<T>::land(const T (&t)[3]) noexcept {
    for (int k = 0; k < 3; ++k) cur[k] = to[k] = t[k];
    remaining = 0;
    started = true;
}

template <typename T>
StripNode::Ramp<T>& StripNode::ramp() noexcept {
    if constexpr (std::is_same_v<T, float>) return ramp32_;
    else return ramp64_;
}

template <typename T>
void StripNode::handover() noexcept {
    constexpr std::int8_t width = std::is_same_v<T, float> ? 32 : 64;
    const std::int8_t last = lastWidth_;
    lastWidth_ = width;
    if (last == 0 || last == width) return;
    using Other = std::conditional_t<std::is_same_v<T, float>, double, float>;
    const Ramp<Other>& from = ramp<Other>();
    Ramp<T>& to = ramp<T>();
    if (from.remaining == 0) {
        // At rest: snap to this width's own exact target on the next
        // retarget, as a strip does when a project opens. At 32 that target
        // is the float gain it always was, so the bytes are 32's own.
        to.started = false;
        return;
    }
    // Mid-ramp: go on from the gain actually playing, toward the same target
    // and in the time that was left; the retarget that follows redirects it
    // to this width's own target if that differs.
    for (int k = 0; k < 3; ++k) {
        to.cur[k] = static_cast<T>(from.cur[k]);
        to.to[k] = static_cast<T>(from.to[k]);
        to.step[k] = static_cast<T>(from.step[k]);
    }
    to.remaining = from.remaining;
    to.started = true;
}

void StripNode::process(const NodeIo& io) noexcept {
    // ADR-0207 d2: one body, at the width the graph hands.
    if (io.wide()) {
        writeTap<double>(preFaderTap_, io, io.in64);
        processGains<double>(io, io.in64, io.out64);
        writeTap<double>(tap_, io, io.out64);
        return;
    }
    writeTap<float>(preFaderTap_, io, io.in);
    processGains<float>(io, io.in, io.out);
    writeTap<float>(tap_, io, io.out);
}

void StripNode::silenceTaps(const NodeIo& io) noexcept {
    writeTap<float>(preFaderTap_, io, nullptr);
    writeTap<float>(tap_, io, nullptr);
    // Asleep (ADR-0043): the input is silent, so a gain set meanwhile -- a
    // fader or a send pulled down, a mute -- is landed now, where nobody can
    // hear it. Woken, the strip starts at it, rather than ramping away from
    // the gain it fell asleep at and playing that gain's first 5 ms.
    const Gains d = desired(lanes_.load(std::memory_order_acquire), io.blockOffset);
    if (lastWidth_ == 32) ramp32_.land(d.g);
    else if (lastWidth_ == 64) ramp64_.land(d.w);
}

template <typename T>
void StripNode::writeTap(std::atomic<ScopeTap*>& slot, const NodeIo& io,
                         const T* const* audio) noexcept {
    // ADR-0175: a watched strip hands its output to the scope, stamped with
    // the timeline position it is heard at. One relaxed load when unwatched.
    if (ScopeTap* tap = slot.load(std::memory_order_relaxed); tap != nullptr && io.channels > 0) {
        std::atomic_thread_fence(std::memory_order_acquire);
        const bool playing = transport_ != nullptr && transport_->playing();
        const std::int64_t at = transport_ != nullptr ? transport_->sampleAt(playing ? io.blockOffset : 0) : 0;
        const T* l = audio != nullptr && audio[0] != nullptr ? audio[0] + io.blockOffset : nullptr;
        const T* r = audio != nullptr && io.channels > 1 && audio[1] != nullptr ? audio[1] + io.blockOffset : nullptr;
        tap->write(l, r, io.frames, at - tap->latency(), playing);
    }
}

template <typename T>
void StripNode::processGains(const NodeIo& io, const T* const* in, T* const* out) noexcept {
    const StripLanes* lanes = lanes_.load(std::memory_order_acquire);
    const bool moving = lanes != nullptr && transport_ != nullptr &&
                        (lanes->volume != nullptr || lanes->pan != nullptr || lanes->mute != nullptr);
    const std::int32_t off = io.blockOffset;
    auto gainIndex = [](std::int32_t c) { return c < 2 ? c : 2; };
    handover<T>();
    if (restart_.load(std::memory_order_relaxed) && restart_.exchange(false, std::memory_order_acq_rel)) {
        ramp32_.started = false;
        ramp64_.started = false;
    }
    Ramp<T>& r = ramp<T>();
    const std::int32_t frames = rampFrames_.load(std::memory_order_relaxed);
    auto retargetAt = [&](std::int32_t frame) {
        const Gains d = desired(lanes, frame);
        if constexpr (std::is_same_v<T, float>) r.retarget(d.g, frames);
        else r.retarget(d.w, frames);
    };

    retargetAt(off);

    if (!moving && r.remaining == 0) {
        // Steady: one gain per channel for the whole segment.
        for (std::int32_t c = 0; c < io.channels; ++c) {
            T* o = out[c] + off;
            const T* x = (in != nullptr && in[c] != nullptr) ? in[c] + off : nullptr;
            const T g = r.cur[gainIndex(c)];
            if (x == nullptr || g == T{0}) std::fill_n(o, io.frames, T{0});
            else if (g == T{1}) std::copy_n(x, io.frames, o);
            else for (std::int32_t i = 0; i < io.frames; ++i) o[i] = x[i] * g;
        }
        return;
    }

    for (std::int32_t i = 0; i < io.frames; ++i) {
        // A bound lane is read on a fixed grid of the block, so where a
        // segment boundary falls does not move where it is read.
        if (moving && i > 0 && (off + i) % kControlFrames == 0) retargetAt(off + i);
        if (r.remaining > 0) {
            for (int k = 0; k < 3; ++k) r.cur[k] += r.step[k];
            if (--r.remaining == 0)
                for (int k = 0; k < 3; ++k) r.cur[k] = r.to[k];   // land exactly
        }
        for (std::int32_t c = 0; c < io.channels; ++c) {
            const T x = (in != nullptr && in[c] != nullptr) ? in[c][off + i] : T{0};
            out[c][off + i] = x * r.cur[gainIndex(c)];
        }
    }
}

// ---------------------------------------------------------------------------

std::map<std::int64_t, bool> audibleTracks(const rows::Model& model, const GraphPlan& plan) {
    std::map<std::int64_t, const rows::Track*> track;
    for (const rows::Track& t : model.tracks) track[t.id] = &t;
    const std::size_t n = plan.nodes.size();

    std::vector<bool> muted(n, false), soloed(n, false), defeat(n, false);
    bool anySolo = false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto it = track.find(plan.nodes[i].trackId);
        if (it == track.end()) continue;
        muted[i] = it->second->muted;
        defeat[i] = it->second->soloDefeat;
        // Soloing the master would solo everything it hears, which is nothing
        // a user means; it is ignored rather than refused.
        soloed[i] = it->second->soloed && plan.nodes[i].kind != PlannedKind::Master;
        anySolo = anySolo || soloed[i];
    }

    std::map<std::int64_t, bool> audible;
    if (!anySolo) {
        for (std::size_t i = 0; i < n; ++i) audible[plan.nodes[i].trackId] = !muted[i];
        return audible;
    }

    // Main routes only. A sidechain key is not listening, so a soloed track's
    // key source is not kept audible by it (ADR-0163 d4). A send is not a
    // main route either: a return is not a soloed track's group.
    std::map<std::int64_t, const rows::Routing*> sendRow;
    for (const rows::Routing& r : model.routing)
        if (r.kind == "send") sendRow[r.id] = &r;
    std::vector<std::vector<std::size_t>> down(n), up(n), sentTo(n);
    for (const PlannedEdge& e : plan.edges) {
        if (e.bus != Bus::Main || e.from >= n || e.to >= n) continue;
        if (e.routingId != 0) {
            // What Solo in Place keeps open through a send: a return it
            // reaches, as Live keeps a soloed track's returns; any other
            // track (ADI's addition) only through a send that carries
            // something -- on, and above silence -- so a send switched off or
            // fully down never opens that track's own audio.
            const auto row = sendRow.find(e.routingId);
            const bool carries = row != sendRow.end() && row->second->enabled &&
                                 row->second->gainDb > kSilenceDb;   // false for NaN too
            if (carries || plan.nodes[e.to].kind == PlannedKind::Return) sentTo[e.from].push_back(e.to);
            continue;
        }
        down[e.from].push_back(e.to);
        up[e.to].push_back(e.from);
    }
    auto reach = [&](const std::vector<std::vector<std::size_t>>& adj) {
        std::vector<bool> seen(n, false);
        std::queue<std::size_t> q;
        for (std::size_t i = 0; i < n; ++i)
            if (soloed[i]) { seen[i] = true; q.push(i); }
        while (!q.empty()) {
            const std::size_t i = q.front();
            q.pop();
            for (const std::size_t j : adj[i])
                if (!seen[j]) { seen[j] = true; q.push(j); }
        }
        return seen;
    };
    std::vector<bool> fed = reach(down);            // what a soloed track feeds: its group, the master
    const std::vector<bool> feeding = reach(up);    // what feeds it: a soloed group's children
    // ADR-0203 d3, Live's Solo in Place: a soloed track's returns stay
    // audible, and whatever they feed. Over sends from anything kept so far,
    // to a fixed point, so a return a soloed group's child sends to is kept.
    for (bool grew = true; grew;) {
        grew = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!fed[i] && !feeding[i]) continue;
            for (const std::size_t j : sentTo[i]) {
                std::vector<std::size_t> todo{j};
                while (!todo.empty()) {
                    const std::size_t k = todo.back();
                    todo.pop_back();
                    if (fed[k]) continue;
                    fed[k] = true;
                    grew = true;
                    for (const std::size_t next : down[k]) todo.push_back(next);
                }
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const bool kept = fed[i] || feeding[i] || defeat[i] || plan.nodes[i].kind == PlannedKind::Master;
        audible[plan.nodes[i].trackId] = !muted[i] && kept;
    }
    return audible;
}

// ---------------------------------------------------------------------------

std::shared_ptr<StripAutomation> bindStripAutomation(std::shared_ptr<const AutomationProgram> program,
                                                     const std::set<std::int64_t>& overridden) {
    auto out = std::make_shared<StripAutomation>();
    out->program = std::move(program);
    if (!out->program) return out;
    for (const AutomationLaneProgram& lane : out->program->lanes()) {
        const std::string who = "automation_lanes#" + std::to_string(lane.laneId);
        if (lane.ownerKind != "track") continue;   // devices: ADR-0165's binding; the compiler named the rest
        StripParam param;
        if (lane.paramRef == "volume") param = StripParam::Volume;
        else if (lane.paramRef == "pan") param = StripParam::Pan;
        else if (lane.paramRef == "mute") param = StripParam::Mute;
        else {
            out->problems.push_back(who + ": a track has no parameter '" + lane.paramRef +
                                    "'; volume, pan and mute are automatable (SPEC 6.9)");
            continue;
        }
        // A fader in dB and a pan in -1..1 are real values; normalized has no
        // meaning for them until a fader curve is decided, and a guess would
        // play the wrong level.
        if (param != StripParam::Mute && lane.valueDomain != "real") {
            out->problems.push_back(who + ": a " + lane.paramRef + " lane must be in real units, not '" +
                                    lane.valueDomain + "'; not played");
            continue;
        }
        if (!lane.enabled) continue;
        const auto key = std::make_pair(lane.ownerId, param);
        if (out->laneFor.count(key)) {
            out->problems.push_back(who + ": a second " + lane.paramRef + " lane for tracks#" +
                                    std::to_string(lane.ownerId) + "; the first is played");
            continue;
        }
        out->laneFor[key] = lane.laneId;
        if (overridden.count(lane.laneId)) continue;   // ADR-0162: known, not bound
        StripLanes& bound = out->byTrack[lane.ownerId];
        if (param == StripParam::Volume) bound.volume = &lane;
        else if (param == StripParam::Pan) bound.pan = &lane;
        else bound.mute = &lane;
    }
    return out;
}

void MixerStrips::sync(const rows::Model& model, const Transport* transport,
                       const std::shared_ptr<StripAutomation>& automation) {
    problems_.clear();
    const GraphPlan plan = planGraph(model);
    const std::map<std::int64_t, bool> audible = audibleTracks(model, plan);
    std::map<std::int64_t, const rows::MixerStrip*> row;
    for (const rows::MixerStrip& s : model.strips) row[s.trackId] = &s;

    std::map<std::int64_t, bool> present;
    for (const PlannedNode& node : plan.nodes) {
        if (node.kind == PlannedKind::Vca) continue;   // a VCA carries no audio (ADR-0072)
        present[node.trackId] = true;
        auto& strip = strips_[node.trackId];
        if (!strip) strip = std::make_unique<StripNode>();
        strip->setTransport(transport);

        double volume = 0.0, pan = 0.0;
        PanLaw law = PanLaw::Live;
        const std::string who = "tracks#" + std::to_string(node.trackId);
        if (const auto it = row.find(node.trackId); it != row.end()) {
            const rows::MixerStrip& r = *it->second;
            volume = r.volumeDb;
            pan = r.pan;
            if (r.panLaw >= 0 && r.panLaw <= 3) law = static_cast<PanLaw>(r.panLaw);
            else problems_.push_back(who + ": pan law " + std::to_string(r.panLaw) +
                                     " is not one of SPEC 6.9's; Live's is used");
            if (std::isnan(volume)) {
                problems_.push_back(who + ": volume is not a number; 0 dB is used");
                volume = 0.0;
            }
            if (!std::isfinite(pan)) {
                problems_.push_back(who + ": pan is not finite; centre is used");
                pan = 0.0;
            }
        }
        const auto heard = audible.find(node.trackId);
        strip->setStatic(volume, pan, law, heard != audible.end() && heard->second);

        // ADR-0172: the track delay, played as latency. The master is the
        // output: there is nothing after it to delay against, and a negative
        // latency there would make the graph's own latency negative.
        std::int64_t delay = 0;
        if (const auto it = row.find(node.trackId); it != row.end()) delay = it->second->delaySamples;
        const bool isMaster = plan.output && plan.nodes[*plan.output].trackId == node.trackId;
        const std::int64_t rate = model.project.sampleRate > 0 ? model.project.sampleRate : 48000;
        const auto limit = static_cast<std::int64_t>(StripNode::kMaxDelaySeconds * static_cast<double>(rate));
        if (isMaster && delay != 0) {
            problems_.push_back(who + ": the master's delay is not played; there is nothing after "
                                      "the master to delay it against");
            delay = 0;
        } else if (delay > limit || delay < -limit) {
            problems_.push_back(who + ": a delay of " + std::to_string(delay) +
                                " samples is past one second; it is played at one second");
            delay = delay > 0 ? limit : -limit;
        }
        strip->setDelay(delay, rate);

        const StripLanes* lanes = nullptr;
        if (automation) {
            const auto it = automation->byTrack.find(node.trackId);
            if (it != automation->byTrack.end()) lanes = &it->second;
        }
        strip->setLanes(lanes, lanes != nullptr ? std::shared_ptr<void>(automation) : std::shared_ptr<void>{});
    }
    // A departed track's strip stays, silent, until the session ends.
    for (auto& [id, strip] : strips_) {
        if (present.count(id)) continue;
        strip->setStatic(0.0, 0.0, PanLaw::Live, false);
        strip->setLanes(nullptr, {});
    }

    // ADR-0203 d3: a node for every send the plan made, its row's level, pan
    // and switch. The source's fader, pan and mute are its strip's, which the
    // send follows: a Post send leaves after it. A departed send stays, off.
    std::set<std::int64_t> planned, live;
    for (const PlannedEdge& e : plan.edges)
        if (e.routingId != 0) planned.insert(e.routingId);
    for (const rows::Routing& r : model.routing) {
        if (r.kind != "send" || !planned.count(r.id)) continue;
        auto& send = sends_[r.id];
        const bool fresh = !send;
        if (fresh) send = std::make_unique<SendNode>();
        // Back in the plan after time out of it, in no graph and so never
        // ramped: it starts at its level, as a new send does, rather than
        // ramping away from the one it last played.
        else if (!liveSends_.count(r.id)) send->restart();
        const std::string who = "routing#" + std::to_string(r.id);
        if (std::isnan(r.gainDb)) problems_.push_back(who + ": the send's level is not a number; it is silent");
        else if (r.gainDb > kSendTopDb)
            problems_.push_back(who + ": a send level of " + std::to_string(r.gainDb) + " dB is past the top, " +
                                std::to_string(kSendTopDb) + " dB, and is played there");
        double pan = r.pan;
        if (!std::isfinite(pan)) {
            problems_.push_back(who + ": the send's pan is not finite; centre is used");
            pan = 0.0;
        }
        send->set(r.gainDb, pan, PanLaw::Live, r.enabled);
        live.insert(r.id);
    }
    for (auto& [id, send] : sends_)
        if (!live.count(id)) send->set(kSilenceDb, 0.0, PanLaw::Live, false);
    liveSends_ = std::move(live);
}

SendNode* MixerStrips::sendFor(std::int64_t routingId) noexcept {
    const auto it = sends_.find(routingId);
    return it == sends_.end() ? nullptr : it->second.get();
}

StripNode* MixerStrips::stripFor(std::int64_t trackId) noexcept {
    const auto it = strips_.find(trackId);
    return it == strips_.end() ? nullptr : it->second.get();
}

}  // namespace adi::engine
