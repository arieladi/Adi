// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/freeze_groups.hpp"
#include "adi/freeze_state.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/row_capture.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
//
// The session. See session.hpp for the three decisions; this file is their
// mechanics. ADR-0122.

#include "adi/engine/session.hpp"

#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "adi/audio/io_audit.hpp"
#include "juce/clap_host.hpp"
#include "adi/engine/audio_recording.hpp"

#include <algorithm>
#include <numbers>
#include <limits>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace adi::engine {
namespace {

std::string deviceRef(const rows::Device& row) {
    std::string s = "devices#" + std::to_string(row.id);
    if (!row.name.empty()) s += " (" + row.name + ")";
    return s;
}

std::vector<std::uint8_t> toU8(const std::vector<std::byte>& in) {
    std::vector<std::uint8_t> out(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) out[i] = static_cast<std::uint8_t>(in[i]);
    return out;
}

device::ParamValue valueOf(const rows::PluginParam& p) noexcept {
    return p.real ? device::ParamValue::withReal(p.normalized, *p.real)
                  : device::ParamValue::fromNormalized(p.normalized);
}

}  // namespace

Session::Session():recording_(std::make_unique<AudioInputRecorder>()) {}
Session::~Session() = default;

AudioInputRecorder& Session::recording() noexcept{return *recording_;}
bool Session::startRecording(const AudioRecordingSpec& options,std::string& error) {
    if(!live_){error="No active audio session for recording";return false;}
    if(options.sampleRate!=spec_.sampleRate || options.maxFrames!=static_cast<std::uint32_t>(spec_.maxFrames)) {
        error="Recording format must match the active audio device";return false;
    }
    for(const auto& target:options.targets) {
        const auto track=std::find_if(model_.tracks.begin(),model_.tracks.end(),[&](const auto& row){return row.id==target.trackId;});
        if(track==model_.tracks.end() || !track->recordArmed || (track->kind!="audio" && track->kind!="midi")) {
            error="Recording target must be an armed audio or MIDI track";return false;
        }
    }
    auto captured=options;captured.sourceProject=recordingProject_;
    return recording_->start(captured,error);
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

const rows::PluginRef* Session::refFor(const rows::Device& row) const noexcept {
    if (!row.pluginRefId) return nullptr;
    for (const rows::PluginRef& r : model_.pluginRefs)
        if (r.id == *row.pluginRefId) return &r;
    return nullptr;
}

const rows::DeviceChain* Session::chainOf(std::int64_t chainId) const noexcept {
    for (const rows::DeviceChain& c : model_.deviceChains)
        if (c.id == chainId) return &c;
    return nullptr;
}

const Session::Entry* Session::entryFor(std::int64_t deviceId) const noexcept {
    const auto it = byId_.find(deviceId);
    return it == byId_.end() ? nullptr : &entries_[it->second];
}

device::DeviceNode* Session::nodeFor(std::int64_t deviceId) noexcept {
    const Entry* e = entryFor(deviceId);
    return e == nullptr ? nullptr : &devices_.nodeAt(e->hostIndex);
}

device::DeviceInstance* Session::instanceFor(std::int64_t deviceId) noexcept {
    const Entry* e = entryFor(deviceId);
    return e == nullptr ? nullptr : &devices_.deviceAt(e->hostIndex);
}

std::vector<device::DeviceNode*> Session::chainFor(std::int64_t trackId) {
    std::vector<device::DeviceNode*> out;
    if (trackId == 0) return out;

    // The rows arrive ordered -- chains by (track, ord), devices by (chain, ord)
    // -- so a track's chain is a walk, not a sort. Several chains directly on
    // one track is not a shape the spec describes (§6.6: ONE ordered list);
    // they are concatenated in `ord` order rather than refused, and named.
    int chainsOnTrack = 0;
    for (const rows::DeviceChain& c : model_.deviceChains) {
        if (!c.trackId || *c.trackId != trackId) continue;
        if (frozenSources_.contains(trackId))
            continue;
        ++chainsOnTrack;
        // `devices` is sorted by chainId first, so the chain's rows are one run.
        auto first = std::lower_bound(
            model_.devices.begin(), model_.devices.end(), c.id,
            [](const rows::Device& d, std::int64_t id) { return d.chainId < id; });
        for (auto it = first; it != model_.devices.end() && it->chainId == c.id; ++it) {
            const Entry* e = entryFor(it->id);
            if (e == nullptr || e->retired) continue;   // a rack, or gone
            out.push_back(&devices_.nodeAt(e->hostIndex));
        }
    }
    if (chainsOnTrack > 1) {
        const std::string note = "tracks#" + std::to_string(trackId) + ": " +
                                 std::to_string(chainsOnTrack) +
                                 " chains directly on one track; concatenated in ord order";
        if (std::find(sessionProblems_.begin(), sessionProblems_.end(), note) ==
            sessionProblems_.end())
            sessionProblems_.push_back(note);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Resolving rows into instances
// ---------------------------------------------------------------------------

std::unique_ptr<device::DeviceInstance> Session::makePlaceholder(
    const Store& store, const rows::Device& row, const rows::PluginRef* ref) {
    device::DeviceIdentity id;
    if (ref != nullptr) {
        id.format = ref->format;
        id.uid = ref->uid;
        id.name = ref->name;
        id.vendor = ref->vendor;
        id.version = ref->version;
    } else {
        id.format = "unknown";
        id.name = row.name;
    }
    auto m = std::make_unique<device::MissingDevice>(id);

    // SPEC §7.1 rule 3: the parameters are the answers a placeholder gives, so
    // an automation lane bound to one still reads a value and a save writes
    // the same mirror back out.
    for (const rows::PluginParam& p : model_.pluginParams) {
        if (p.deviceId != row.id) continue;
        device::ParamDescriptor d;
        d.id = p.paramId;
        d.name = p.name;
        d.unit = p.unit;
        d.flags = static_cast<std::uint32_t>(p.flags);
        d.domain = p.real ? device::ParamDomain::Real : device::ParamDomain::Normalized;
        m->addParam(std::move(d), valueOf(p));
    }

    // Rule 1: the state, byte for byte. A blob the project does not hold is a
    // problem with a name, not an empty state -- saving would otherwise write
    // a row whose hash points at nothing, which is how a corrupt project
    // becomes a corrupt project that validates.
    for (const rows::PluginState& s : model_.pluginState) {
        if (s.deviceId != row.id) continue;
        const auto bytes = store.getStateBlob(s.hash);
        if (!bytes) {
            sessionProblems_.push_back(deviceRef(row) + ": state '" + s.role +
                                       "' names blob " + s.hash +
                                       ", which the project does not hold");
            continue;
        }
        m->keepState(s.role, toU8(*bytes));
    }
    return m;
}

void Session::restoreState(const Store& store, const rows::Device& row,
                           device::DeviceInstance& inst) {
    // State first, then every mirror row the plugin does not already hold
    // (ADR-0142, replacing ADR-0122's "the mirror only when no state
    // loaded"). A chunk is now written when the plugin signals, not only at
    // save, so a knob turned after a preset is a row NEWER than the chunk --
    // and skipping the rows lost it. The other way round cannot happen: a
    // snapshot rewrites every row of its device to match (param_ops.hpp,
    // decision 7), so a row that differs from the loaded chunk is an edit
    // made after it. A row the chunk already agrees with is not pushed,
    // which is what keeps this from fighting a plugin whose parameters are
    // derived from its chunk (a sampler's zone count, a modular's patch).
    bool any = false;
    for (const rows::PluginState& s : model_.pluginState) {
        if (s.deviceId != row.id) continue;
        const auto bytes = store.getStateBlob(s.hash);
        if (!bytes) {
            sessionProblems_.push_back(deviceRef(row) + ": state '" + s.role +
                                       "' names blob " + s.hash +
                                       ", which the project does not hold");
            continue;
        }
        if (inst.loadState(s.role, toU8(*bytes))) {
            ++stats_.statesLoaded;
            any = true;
        } else {
            sessionProblems_.push_back(deviceRef(row) + ": the plugin refused its '" +
                                       s.role + "' state");
        }
    }
    for (const rows::PluginParam& p : model_.pluginParams) {
        if (p.deviceId != row.id) continue;
        const device::ParamValue want = valueOf(p);
        if (any && std::fabs(inst.getParam(p.paramId).normalized - want.normalized) <= 1e-6) {
            ++stats_.paramsMatched;
            continue;
        }
        if (inst.setParam(p.paramId, want)) ++stats_.paramsApplied;
    }
}

void Session::resolveOne(const Store& store, const rows::Device& row) {
    const rows::DeviceChain* chain = chainOf(row.chainId);
    if (chain == nullptr) {
        sessionProblems_.push_back(deviceRef(row) + ": its chain " +
                                   std::to_string(row.chainId) +
                                   " is not in the project; not realised");
        ++stats_.skipped;
        return;
    }
    // ADR-0060 makes a rack a node that owns a nested graph. That node is not
    // built yet, so a rack and everything inside it stay out of the signal
    // path -- named, counted, and never silently. An instrument rack skipped
    // is a silent track, and the problem list is how the user learns why.
    if (chain->parentDeviceId) {
        sessionProblems_.push_back(deviceRef(row) +
                                   ": inside a rack's chain; racks are not realised yet "
                                   "(ADR-0060), so it is not in the signal path");
        ++stats_.skipped;
        return;
    }
    if (row.isRack) {
        sessionProblems_.push_back(deviceRef(row) +
                                   ": a rack; not realised yet (ADR-0060), so it is not "
                                   "in the signal path");
        ++stats_.skipped;
        return;
    }

    const rows::PluginRef* ref = refFor(row);
    Entry e;
    e.deviceId = row.id;
    e.trackId = chain->trackId.value_or(0);
    e.name = !row.name.empty() ? row.name
           : (ref != nullptr && !ref->name.empty()) ? ref->name
           : "devices#" + std::to_string(row.id);

    std::unique_ptr<device::DeviceInstance> inst;
    std::string err;
    if (ref == nullptr) {
        err = "no plugin reference";
    } else if (!loader_) {
        err = "no device loader";
    } else {
        DeviceRequest rq;
        rq.device = &row;
        rq.ref = ref;
        rq.sampleRate = spec_.sampleRate;
        rq.maxFrames = spec_.maxFrames;
        inst = loader_(rq, err);
        if (inst == nullptr && err.empty()) err = "the loader returned nothing";
    }

    if (inst == nullptr) {
        e.placeholder = true;
        e.error = err;
        inst = makePlaceholder(store, row, ref);
        ++stats_.placeholders;
        sessionProblems_.push_back(deviceRef(row) + ": kept as a placeholder (ADR-0011): " + err);
    } else {
        restoreState(store, row, *inst);
        ++stats_.loaded;
        syncRoute(e, *inst);
    }

    // UNPLACED (track 0): the session places from the rows -- decision 1.
    device::DeviceNode& node = devices_.add(std::move(inst), e.name, /*trackId=*/0);
    node.setBypassed(e.placeholder || !row.enabled);
    node.setAlwaysProcess(row.alwaysProcess);
    e.hostIndex = devices_.deviceCount() - 1;
    byId_[row.id] = entries_.size();
    entries_.push_back(std::move(e));
}

void Session::resolveNewRows(const Store& store) {
    for (const rows::Device& row : model_.devices) {
        if (parkedDevice(row))
            continue;
        const auto it = byId_.find(row.id);
        if (it == byId_.end()) {
            resolveOne(store, row);
            continue;
        }
        // A row that came BACK -- an undone removal -- finds its instance
        // waiting, state and all. That is the payoff of retiring rather than
        // destroying: the undo restores the sound, not a fresh default.
        Entry& e = entries_[it->second];
        if (e.retired && e.parked) {
            // Never resume voices/delay history left in an instance before its
            // cache was installed. Restore from the preserved project state.
            resolveOne(store, row);
            continue;
        }
        if (e.retired) {
            e.retired = false;
            --stats_.retired;
        }
    }
}

void Session::retireDepartedRows() {
    for (Entry& e : entries_) {
        if (e.retired) continue;
        const bool present =
            std::any_of(model_.devices.begin(), model_.devices.end(),
                        [&](const rows::Device &d) { return d.id == e.deviceId && !parkedDevice(d); });
        if (present) continue;
        for (const auto &row : model_.devices)
            if (row.id == e.deviceId)
                e.parked = parkedDevice(row);
        e.retired = true;
        e.trackId = 0;
        ++stats_.retired;
        if (e.parked)
            continue;
        sessionProblems_.push_back("devices#" + std::to_string(e.deviceId) + " (" + e.name +
                                   "): removed from the project; its instance is kept out "
                                   "of every chain until the session closes (ADR-0122)");
    }
}

void Session::syncFlags() {
    for (Entry& e : entries_) {
        if (e.retired) continue;
        for (const rows::Device& d : model_.devices) {
            if (d.id != e.deviceId) continue;
            device::DeviceNode& node = devices_.nodeAt(e.hostIndex);
            // DeviceNode publishes these switches atomically; the graph
            // samples bypass once per block before choosing delayed dry/DSP.
            const bool bypassed = e.placeholder || !d.enabled;
            if (node.bypassed() != bypassed) node.setBypassed(bypassed);
            if (node.alwaysProcess() != d.alwaysProcess) node.setAlwaysProcess(d.alwaysProcess);
            if (const rows::DeviceChain* c = chainOf(d.chainId))
                e.trackId = c->trackId.value_or(0);
            if (!e.placeholder) syncRoute(e, node.instance());
            break;
        }
    }
}

void Session::syncRoute(Entry& e, device::DeviceInstance& inst) {
    // ADR-0146, ADR-0149: the project's route, and only the project's. The
    // application registry chose it once, when the device was inserted; a
    // session never asks the registry again, or a project would play
    // differently on a machine whose registry says otherwise (ADR-0134 d7).
    engine::RouteChoice want = engine::RouteChoice::Auto;
    for (const rows::DeviceRoute& r : model_.deviceRoutes) {
        if (r.deviceId != e.deviceId) continue;
        bool known = false;
        want = engine::routeChoiceFromName(r.route, &known);
        if (!known)
            sessionProblems_.push_back("devices#" + std::to_string(e.deviceId) + " (" + e.name +
                                       "): route '" + r.route + "' is not one this build knows; played as Auto");
        break;
    }
    if (static_cast<std::uint8_t>(want) == e.route) return;
    e.route = static_cast<std::uint8_t>(want);
    if (inst.setExpressionRoute(want)) {
        if (want != engine::RouteChoice::Auto) ++stats_.routesApplied;
        return;
    }
    if (want == engine::RouteChoice::Auto) return;   // nothing was asked of it
    ++stats_.routesRefused;
    sessionProblems_.push_back("devices#" + std::to_string(e.deviceId) + " (" + e.name +
                               "): the project plays it on route '" +
                               std::string(engine::routeChoiceName(want)) +
                               "', which this device cannot take; the row is kept");
}

// ---------------------------------------------------------------------------
// The graph
// ---------------------------------------------------------------------------

void Session::attach(std::shared_ptr<const GraphTransportState> timing) {
    device::DeviceHost::RebuildSpec rs;
    rs.transportState=std::move(timing);
    rs.model = [this] { return &model_; };
    if (!frozenGroupChildren_.empty())
        rs.transformPlan = [this](GraphPlan &plan) {
            std::set<std::int64_t> roots;
            for (const auto &source : frozenSources_)
                roots.insert(source.first);
            freeze::pruneGroupPlan(plan, frozenGroupChildren_, roots);
        };
    rs.channels = spec_.channels;
    rs.sampleRate = spec_.sampleRate;
    rs.maxFrames = spec_.maxFrames;
    rs.devicesFor = [this](std::int64_t trackId) { return trackChain(trackId); };
    // ADR-0203 d3: a send leaves its track at its tap and plays through the
    // node the strips keep for its row, then, into a summing group, through
    // the channel half the summing keeps for it (ADR-0174). A Post send's
    // node plays the strip's gain itself, without its solo (SendNode), so it
    // reads where a Pre send does, after the effects: a track soloed away
    // then still reaches a soloed return.
    rs.tapFor = [this](std::int64_t trackId, Tap tap) {
        return tapFor(trackId, tap == Tap::PostFader ? Tap::PostFx : tap);
    };
    rs.sendFor = [this](std::int64_t routingId) -> Node* { return strips_.sendFor(routingId); };
    rs.sendTailFor = [this](std::int64_t routingId) { return summing_.sendTailFor(routingId); };
    // A return's send into itself ends in its loop's writer, and the loop's
    // reader feeds the return a block later (Live's feedback loop).
    rs.feedbackFor = [this](std::int64_t routingId) {
        FeedbackLoop* loop = strips_.loopFor(routingId);
        return loop == nullptr ? FeedbackNodes{} : FeedbackNodes{&loop->writer(), &loop->reader()};
    };
    auto* clipSource = clips();
    auto* midiSource = midi_.get();
    rs.sourcesFor = [this, clipSource, midiSource](std::int64_t id) {
        auto nodes = sources_ && !frozenSources_.contains(id) ? sources_(id) : std::vector<Node *>{};
        if (clipSource) {
            const auto frozen = frozenSources_.find(id);
            auto audio = clipSource->sourcesFor(frozen == frozenSources_.end() ? id : frozen->second.first);
            nodes.insert(nodes.end(), audio.begin(), audio.end());
        }
        if (midiSource) {
            auto notes = midiSource->sourcesFor(id);
            nodes.insert(nodes.end(), notes.begin(), notes.end());
        }
        return nodes;
    };
    rs.monitors=cueMonitor_.points([this](std::int64_t id,Tap tap){return tapFor(id,tap);});
    devices_.attachHost(graph_, std::move(rs));
}

std::vector<Node*> Session::trackChain(std::int64_t trackId) {
    std::vector<Node*> nodes;
    // ADR-0174: a summing group's buss half is first, right after its sum.
    if (!frozenSources_.contains(trackId))
        if (Node *buss = summing_.headFor(trackId))
            nodes.push_back(buss);
    if (const auto frozen = frozenSources_.find(trackId); frozen != frozenSources_.end()) {
        auto wet = clips_->sourcesFor(frozen->second.second);
        nodes.insert(nodes.end(), wet.begin(), wet.end());
        if (StripNode *strip = strips_.stripFor(trackId))
            nodes.push_back(strip);
        if (Node *channel = summing_.tailFor(trackId))
            nodes.push_back(channel);
        return nodes;
    }
    const auto chain = chainFor(trackId);
    std::size_t firstAudio = 0;
    for (std::size_t i = 0; i < chain.size(); ++i)
        if (chain[i]->eventFlow() == EventFlow::Consume) firstAudio = i + 1;
    for (std::size_t i = 0; i <= chain.size(); ++i) {
        if (i == firstAudio) {
            const auto gain = inputGains_.find(trackId);
            if (gain != inputGains_.end()) nodes.push_back(gain->second.get());
        }
        if (i < chain.size()) nodes.push_back(chain[i]);
    }
    // ADR-0163: the strip is the last node of every chain, after the
    // devices, so whatever the track feeds takes its output.
    if (StripNode* strip = strips_.stripFor(trackId)) nodes.push_back(strip);
    // ADR-0174: and a child of a summing group leaves through its
    // channel half, after the strip, on its way into the group's sum.
    if (Node* channel = summing_.tailFor(trackId)) nodes.push_back(channel);
    return nodes;
}

Node* Session::tapFor(std::int64_t trackId, Tap tap) {
    StripNode* strip = strips_.stripFor(trackId);
    const std::vector<Node*> nodes = trackChain(trackId);
    std::size_t at = nodes.size();   // the strip's place
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i] == strip) at = i;
    switch (tap) {
        case Tap::Tail:
            return nodes.empty() ? nullptr : nodes.back();
        case Tap::PostFader:
            return strip;
        case Tap::PostMixer:
            return at + 1 < nodes.size() ? nodes[at + 1] : strip;
        case Tap::PostFx:
            return at > 0 && at <= nodes.size() ? nodes[at - 1] : nullptr;
        case Tap::PreFx: {
            if (frozenSources_.contains(trackId))
                return nullptr;
            // After the instrument, before the first audio effect: the last
            // node that plays the track's notes, or what comes before them.
            Node* last = summing_.headFor(trackId);
            for (Node* n : chainFor(trackId))
                if (n->eventFlow() == EventFlow::Consume) last = n;
            return last;
        }
    }
    return strip;
}

std::optional<OfflineOutput> Session::offlineOutput(std::int64_t trackId, OfflineTap point) {
    if (point != OfflineTap::PreFader && point != OfflineTap::PostFader)
        return std::nullopt;
    return offlineTapOutput(trackId, point == OfflineTap::PreFader ? Tap::PostFx : Tap::PostFader);
}
std::optional<OfflineOutput> Session::offlineTapOutput(std::int64_t trackId, Tap point) {
    if (point != Tap::PreFx && point != Tap::PostFx && point != Tap::PostFader && point != Tap::PostMixer &&
        point != Tap::Tail)
        return std::nullopt;
    const auto track = std::find_if(model_.tracks.begin(), model_.tracks.end(),
                                    [trackId](const rows::Track &row) { return row.id == trackId; });
    if (track == model_.tracks.end() ||
        (track->kind != "audio" && track->kind != "midi" && track->kind != "instrument" &&
         track->kind != "group" && track->kind != "return" && track->kind != "master"))
        return std::nullopt;
    auto *realized = graph_.current();
    if (!realized)
        return std::nullopt;
    auto &graph = realized->graph();
    Node *node = tapFor(trackId, point);
    const NodeId id = node ? graph.find(*node) : realized->inputFor(trackId);
    const auto latency = graph.outputLatencyOf(id);
    if (!latency)
        return std::nullopt;
    return OfflineOutput{id, *latency};
}
std::optional<OfflineSourceInfo> Session::offlineSourceInfo(std::int64_t trackId) {
    if (frozenSources_.contains(trackId) || !offlineTapOutput(trackId, Tap::PostFx))
        return std::nullopt;
    const auto chain = chainFor(trackId);
    std::size_t prefix = 0;
    for (std::size_t i = 0; i < chain.size(); ++i)
        if (chain[i]->eventFlow() == EventFlow::Consume)
            prefix = i + 1;
    OfflineSourceInfo info;
    info.track = trackId;
    for (std::size_t i = 0; i < chain.size(); ++i)
        for (const auto &entry : entries_) {
            if (entry.retired || &devices_.nodeAt(entry.hostIndex) != chain[i])
                continue;
            if (entry.placeholder || !devices_.deviceAt(entry.hostIndex).loaded())
                return std::nullopt;
            info.devices.push_back({entry.deviceId, devices_.deviceAt(entry.hostIndex).identity()});
            if (i < prefix)
                info.prefixDevices.push_back(entry.deviceId);
            break;
        }
    return info;
}

bool Session::rebuild() {
    error_.clear();
    if (!loaded_) {
        error_ = "no project loaded";
        return false;
    }
    (void)mainLoudness_.prepare(spec_.sampleRate,spec_.channels);
    auto timing = std::make_shared<TimingView>();
    timing->session=this;
    timing->sampleRate = spec_.sampleRate;
    timing->settings=spec_.performanceState ? transportSettings_ : TransportSettings{};
    // Same construction as MidiClips: TempoMap integrates STEP tempos, even
    // when a stored event requests a ramp. Do not create a second converter.
    for (const auto& e : model_.tempo)
        timing->tempo.events.push_back({e.posTicks, e.bpm, static_cast<int>(e.curve)});
    if (timing->tempo.events.empty()) timing->tempo.events.push_back({0, 120.0, 0});
    auto loopSample=[&](std::int64_t ticks){
        const long double sample=static_cast<long double>(timing->tempo.ticksToSeconds(ticks))*spec_.sampleRate;
        return sample>=static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(std::llround(sample));
    };
    timing->loopBegin=loopSample(transportSettings_.loopStartTicks);
    timing->loopEnd=loopSample(transportSettings_.loopEndTicks);
    if(timing->loopEnd<=timing->loopBegin && timing->loopBegin<std::numeric_limits<std::int64_t>::max())
        timing->loopEnd=timing->loopBegin+1; // a valid sub-sample tick window plays one frame
    // Snapshot has tempo but no meter data. rows::readModel reads this vector
    // from time_signature_map, sorted by position and id.
    timing->signatures = model_.signatures;
    try {
        clips_ = std::make_unique<ClipPlayback>(clipProject_, transport_, spec_.sampleRate,
                                                spec_.channels, spec_.maxFrames, spec_.progressiveDecoder);
        midi_ = std::make_unique<MidiClips>(playbackModel_, transport_, spec_.sampleRate, midiState_);
    } catch (const std::exception& e) { error_ = e.what(); return false; }
    // ADR-0164: the automation this graph plays, bound to strips, less every
    // lane the user overrode. An override of a lane that is gone goes with it.
    const std::shared_ptr<const AutomationProgram> program =
        compileAutomation(playbackModel_, spec_.sampleRate);
    auto suspended = overridden_;
    suspended.insert(writingAutomation_.begin(), writingAutomation_.end());
    stripAutomation_ = bindStripAutomation(program, suspended);
    deviceAutomation_ = bindDeviceAutomation(program, suspended, transport_, spec_.sampleRate);
    for (auto it = overridden_.begin(); it != overridden_.end();) {
        bool known = false;
        for (const auto& [key, laneId] : stripAutomation_->laneFor) known = known || laneId == *it;
        for (const auto& [key, laneId] : stripAutomation_->sendLaneFor) known = known || laneId == *it;
        for (const auto& [key, laneId] : deviceAutomation_->laneFor) known = known || laneId == *it;
        it = known ? std::next(it) : overridden_.erase(it);
    }
    for (auto it = writingAutomation_.begin(); it != writingAutomation_.end();) {
        bool known = false;
        for (const auto& [key, laneId] : stripAutomation_->laneFor) known = known || laneId == *it;
        for (const auto& [key, laneId] : stripAutomation_->sendLaneFor) known = known || laneId == *it;
        for (const auto& [key, laneId] : deviceAutomation_->laneFor) known = known || laneId == *it;
        it = known ? std::next(it) : writingAutomation_.erase(it);
    }
    for (const auto& track : model_.tracks) {
        double db = 0.0;
        for (const auto& strip : model_.strips)
            if (strip.trackId == track.id) db = strip.inputGainDb;
        if (db != 0.0 || inputGains_.contains(track.id)) {
            auto& gain = inputGains_[track.id];
            if (!gain) gain = std::make_unique<InputGainNode>();
            gain->setDb(db);
        }
    }
    strips_.setFormat(spec_.channels, spec_.maxFrames);    // a return's loop, for the graph built here
    HearingFlags hearingFlags;hearingFlags.soloInPlace=cueSettings_.soloInPlace;
    hearingFlags.soloIsCue=spec_.performanceState && cueSettings_.cueMode;hearingFlags.referenceTrack=cueSettings_.projectReferenceTrack;
    strips_.sync(model_, &transport_, stripAutomation_,spec_.delayCompensation,hearingFlags);   // ADR-0163: before the graph names them
    summing_.sync(model_);                                 // ADR-0174: likewise
    bindDeviceSources();                                   // ADR-0165: likewise
    cueMonitor_.configure(model_,cueSettings_,&transport_,stripAutomation_,spec_.performanceState);
    attach(std::move(timing)); // publish the matching clock with this graph
    ++stats_.rebuilds;
    const bool ok = devices_.rebuildNow();
    if (!ok) error_ = devices_.lastRebuildError();
    else {
        cueMonitor_.publish(cueSettings_,spec_.sampleRate,spec_.maxFrames,spec_.channels,spec_.performanceState,graph_.currentGraph());
        live_ = true;
    }
    attachTaps();   // ADR-0175: the new graph's latencies

    problems_ = model_.problems;
    if (midi_) problems_.insert(problems_.end(), midi_->problems().begin(), midi_->problems().end());
    if (auto* c = clips()) problems_.insert(problems_.end(), c->problems().begin(), c->problems().end());
    problems_.insert(problems_.end(), sessionProblems_.begin(), sessionProblems_.end());
    problems_.insert(problems_.end(), strips_.problems().begin(), strips_.problems().end());
    problems_.insert(problems_.end(), summing_.problems().begin(), summing_.problems().end());
    // The program's own findings only when there is automation to find them in:
    // a project with a tempo ramp and no lanes has no automation problem.
    if (stripAutomation_ && stripAutomation_->program && !model_.automationLanes.empty()) {
        const auto& found = stripAutomation_->program->problems();
        problems_.insert(problems_.end(), found.begin(), found.end());
        problems_.insert(problems_.end(), stripAutomation_->problems.begin(), stripAutomation_->problems.end());
    }
    if (deviceAutomation_)
        problems_.insert(problems_.end(), deviceAutomation_->problems.begin(), deviceAutomation_->problems.end());
    problems_.insert(problems_.end(), deviceSourceProblems_.begin(), deviceSourceProblems_.end());
    const std::vector<std::string>& fromGraph = graph_.problems();
    problems_.insert(problems_.end(), fromGraph.begin(), fromGraph.end());
    return ok;
}

bool Session::parkedDevice(const rows::Device &device) const {
    const auto *chain = chainOf(device.chainId);
    return chain && chain->trackId &&
           (frozenSources_.contains(*chain->trackId) || frozenGroupChildren_.contains(*chain->trackId));
}
bool Session::readPlayback(const Store &store) {
    try {
        // A malformed/stale cache never falls back to a different live sound.
        freeze::validate(const_cast<Store &>(store));
        freeze::remember(const_cast<Store &>(store));
        const auto cached = freeze::renders(const_cast<SQLite::Database &>(store.db()));
        std::set<std::int64_t> before, after;
        for (const auto &entry : frozenSources_)
            before.insert(entry.first);
        for (const auto &row : cached)
            after.insert(adi::detail::signedInteger(row.at("track_id")));
        if (loaded_ && playbackRunning_.load(std::memory_order_acquire) && before != after)
            throw std::runtime_error("stop playback before freezing or unfreezing");
        playbackModel_ = model_;
        frozenSources_.clear();
        frozenGroupChildren_.clear();
        for (const auto &[root, members] : freeze::groups(const_cast<SQLite::Database &>(store.db()))) {
            (void)root;
            for (const auto &member : members)
                frozenGroupChildren_.insert(member.id);
        }
        std::erase_if(playbackModel_.clips,
                      [&](const auto &clip) { return frozenGroupChildren_.contains(clip.trackId); });
        std::int64_t ordinal = 0;
        std::set<std::int64_t> used;
        for (const auto &clip : model_.clips)
            used.insert(clip.id);
        for (const auto &track : model_.tracks)
            used.insert(track.id);
        auto synthetic = [&] {
            while (used.contains(-1 - ordinal))
                ++ordinal;
            const auto id = -1 - ordinal++;
            used.insert(id);
            return id;
        };
        for (const auto &render : cached) {
            const auto id = adi::detail::signedInteger(render.at("track_id"));
            for (const auto *field : {"pre_media_id", "post_media_id"}) {
                const auto mediaId = adi::detail::signedInteger(render.at(field));
                SQLite::Statement q(
                    store.db(),
                    "SELECT hash_blake3,rel_path,abs_path_hint,orig_name FROM media_files WHERE id=?");
                q.bind(1, mediaId);
                if (!q.executeStep())
                    throw std::runtime_error("freeze cache media is missing");
                Payload row = {
                    {"orig_name", q.getColumn(3).getString()},
                    {"hash_blake3", q.getColumn(0).getString()},
                    {"rel_path", q.getColumn(1).isNull() ? Payload() : Payload(q.getColumn(1).getString())},
                    {"abs_path_hint",
                     q.getColumn(2).isNull() ? Payload() : Payload(q.getColumn(2).getString())}};
                const auto key = row.dump();
                auto &verified = verifiedFrozenMedia_[mediaId];
                std::error_code statusError;
                const auto size = std::filesystem::file_size(verified.file, statusError);
                bool current = !statusError && verified.row == key && verified.size == size;
                const auto modified = std::filesystem::last_write_time(verified.file, statusError);
                current = current && !statusError && modified == verified.modified;
                if (!current) {
                    std::string error;
                    const auto resolved = media::resolveMedia(media::projectFolder(store), row, error);
                    if (resolved.empty())
                        throw std::runtime_error("freeze cache media: " + error);
                    verified = {key, resolved, std::filesystem::file_size(resolved),
                                std::filesystem::last_write_time(resolved)};
                }
                // Use the verified resolution in this private playback model;
                // a missing relative path can have a valid original/collected fallback.
                const auto entry = std::find_if(playbackModel_.media.begin(), playbackModel_.media.end(),
                                                [mediaId](const auto &item) { return item.id == mediaId; });
                if (entry == playbackModel_.media.end())
                    throw std::runtime_error("freeze cache has no playback media row");
                entry->relPath = media::pathUtf8(verified.file);
            }
            const auto dry = synthetic(), wet = synthetic();
            frozenSources_.emplace(id, std::pair{dry, wet});
            std::erase_if(playbackModel_.clips, [id](const auto &clip) { return clip.trackId == id; });
            const auto frames = adi::detail::signedInteger(render.at("frames"));
            const auto rate = adi::detail::signedInteger(render.at("sample_rate"));
            if (static_cast<long double>(frames) * 1000000000.0L / static_cast<long double>(rate) >
                static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
                throw std::runtime_error("freeze duration exceeds project clock range");
            for (unsigned point = 0; point < 2; ++point) {
                rows::Clip clip;
                clip.id = point ? wet : dry;
                clip.trackId = clip.id;
                clip.kind = "audio";
                clip.timeBase = 1;
                clip.posNs = 0;
                clip.lengthNs = static_cast<std::int64_t>(
                    std::llround(static_cast<long double>(frames) * 1000000000.0L / static_cast<long double>(rate)));
                playbackModel_.clips.push_back(clip);
                rows::AudioClip audio;
                audio.clipId = clip.id;
                audio.mediaId = adi::detail::signedInteger(render.at(point ? "post_media_id" : "pre_media_id"));
                audio.srcLenFrames = frames;
                audio.warpEnabled = false;
                playbackModel_.audioClips.push_back(audio);
            }
        }
        for (const auto &track : model_.tracks)
            if (track.frozen && !frozenSources_.contains(track.id))
                throw std::runtime_error("legacy frozen flag has no verified cache; unfreeze first");
        std::erase_if(playbackModel_.automationLanes, [&](const auto &lane) {
            if (lane.ownerKind == "device") {
                for (const auto &device : model_.devices)
                    if (device.id == lane.ownerId)
                        return parkedDevice(device);
            }
            return lane.ownerKind == "track" && frozenSources_.contains(lane.ownerId) &&
                   lane.paramRef == "input_gain";
        });
        clipProject_ = readClipProject(store, playbackModel_);
        return true;
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
}

bool Session::load(const Store& store, DeviceLoader loader, SessionSpec spec) {
    error_.clear();
    if (loaded_) {
        // One session, one project. A second project is a second session --
        // the instances this one holds are in a graph that may be rendering.
        error_ = "a project is already loaded in this session";
        return false;
    }
    loader_ = std::move(loader);
    spec_ = spec;
    graph_.setPrecision(spec_.precision);   // ADR-0207 d2
    graph_.setDelayCompensation(spec_.delayCompensation);
    stats_ = Stats{};
    model_ = rows::readModel(store);
    transportSettings_=readTransportSettings(store);
    SQLite::Statement recordingIdentity(store.db(),"PRAGMA database_list");
    while(recordingIdentity.executeStep())if(recordingIdentity.getColumn(1).getString()=="main")recordingProject_=recordingIdentity.getColumn(2).getString();
    cueSettings_=readCueSettings(store);
    if (!readPlayback(store))
        return false;
    loaded_ = true;
    resolveNewRows(store);
    return rebuild();
}

namespace {
// What a user can move on a strip, per track: what an override compares.
struct StripValues {
    double volume = 0.0, pan = 0.0, panLeft = -1.0, panRight = 1.0;
    bool muted = false;
};
std::optional<std::string> automationParamRef(const std::string& format, const std::string& id) {
    if (format != "clap") return id;
    std::uint32_t numeric = 0;
    if (!device::ClapDevice::paramIdFromText(id,numeric)) return {};
    return std::to_string(numeric); // ADR-0165: lane refs stay decimal for every format
}
std::map<std::pair<std::int64_t, std::string>, double> paramValues(const rows::Model& m) {
    std::map<std::pair<std::int64_t, std::string>, double> v;
    std::set<std::int64_t> clapRefs, clapDevices;
    for (const auto& reference : m.pluginRefs) if (reference.format == "clap") clapRefs.insert(reference.id);
    for (const auto& item : m.devices)
        if (item.pluginRefId && clapRefs.contains(*item.pluginRefId)) clapDevices.insert(item.id);
    for (const rows::PluginParam& p : m.pluginParams) {
        const auto ref = automationParamRef(clapDevices.contains(p.deviceId) ? "clap" : "",p.paramId);
        if (ref) v[{p.deviceId,*ref}] = p.normalized;
    }
    return v;
}

std::map<std::int64_t, StripValues> stripValues(const rows::Model& m) {
    std::map<std::int64_t, StripValues> v;
    for (const rows::Track& t : m.tracks) v[t.id].muted = t.muted;
    for (const rows::MixerStrip& s : m.strips) {
        v[s.trackId].volume = s.volumeDb;
        v[s.trackId].pan = s.pan;
        v[s.trackId].panLeft = s.panLeft;
        v[s.trackId].panRight = s.panRight;
    }
    return v;
}
}  // namespace

bool Session::refresh(const Store& store) {
    error_.clear();
    if (!loaded_) {
        error_ = "no project loaded";
        return false;
    }
    const std::map<std::int64_t, StripValues> before = stripValues(model_);
    const std::map<std::pair<std::int64_t, std::string>, double> paramsBefore = paramValues(model_);
    std::map<std::int64_t, double> sendsBefore;
    for (const auto& route : model_.routing) if (route.kind == "send") sendsBefore[route.id] = route.gainDb;
    model_ = rows::readModel(store);
    transportSettings_=readTransportSettings(store);
    cueSettings_=readCueSettings(store);
    // ADR-0162: an automated strip value that changed is overridden. Compared
    // BEFORE the rebuild, so the new graph already plays the manual value.
    if (stripAutomation_) {
        const std::map<std::int64_t, StripValues> after = stripValues(model_);
        for (const auto& [key, laneId] : stripAutomation_->laneFor) {
            if (overridden_.count(laneId) || writingAutomation_.count(laneId)) continue;
            const auto b = before.find(key.first);
            const auto a = after.find(key.first);
            if (b == before.end() || a == after.end()) continue;
            const bool changed = key.second == StripParam::Volume ? b->second.volume != a->second.volume
                               : key.second == StripParam::Pan    ? b->second.pan != a->second.pan
                               : key.second == StripParam::PanLeft ? b->second.panLeft != a->second.panLeft
                               : key.second == StripParam::PanRight ? b->second.panRight != a->second.panRight
                                                                  : b->second.muted != a->second.muted;
            if (changed) overridden_.insert(laneId);
        }
        std::map<std::int64_t, double> sendsAfter;
        for (const auto& route : model_.routing) if (route.kind == "send") sendsAfter[route.id] = route.gainDb;
        for (const auto& [route, laneId] : stripAutomation_->sendLaneFor) {
            if (overridden_.contains(laneId) || writingAutomation_.contains(laneId)) continue;
            const auto beforeSend = sendsBefore.find(route), afterSend = sendsAfter.find(route);
            if (beforeSend != sendsBefore.end() && afterSend != sendsAfter.end() && beforeSend->second != afterSend->second)
                overridden_.insert(laneId);
        }
    }
    // ADR-0162 for a plug-in's parameters (ADR-0165): a changed stored value --
    // an op, a knob in ADI, a gesture in the plug-in's own window turned into
    // an op by ParamOps (ADR-0124) -- overrides that parameter's lane. A row
    // that appears or disappears is a change too.
    if (deviceAutomation_) {
        const std::map<std::pair<std::int64_t, std::string>, double> paramsAfter = paramValues(model_);
        for (const auto& [key, laneId] : deviceAutomation_->laneFor) {
            if (overridden_.count(laneId) || writingAutomation_.count(laneId)) continue;
            const auto b = paramsBefore.find(key);
            const auto a = paramsAfter.find(key);
            const bool had = b != paramsBefore.end(), has = a != paramsAfter.end();
            if (had != has || (had && b->second != a->second)) overridden_.insert(laneId);
        }
    }
    if (!readPlayback(store))
        return false;
    resolveNewRows(store);
    retireDepartedRows();
    syncFlags();
    return rebuild();
}

void Session::setSourcesFor(SourceFn fn) { sources_ = std::move(fn); }

std::int32_t Session::feedbackTrip(std::int64_t routingId) {
    FeedbackLoop* loop = strips_.loopFor(routingId);
    SendNode* send = strips_.sendFor(routingId);
    Graph* g = graph_.currentGraph();
    if (loop == nullptr || send == nullptr || g == nullptr) return -1;
    const NodeId sendAt = g->find(*send);
    if (sendAt == kInvalidNode || g->find(loop->reader()) == kInvalidNode) return -1;
    std::int64_t trackId = 0;
    for (const rows::Routing& r : model_.routing)
        if (r.id == routingId) trackId = r.srcId;
    // The send's arrival is when its tap's output is ready; the chain's first
    // node takes the junction's output as it is, so its arrival is the
    // junction's. Every return's chain has its strip, so it has a first node.
    const std::vector<Node*> chain = trackChain(trackId);
    const NodeId first = chain.empty() ? kInvalidNode : g->find(*chain.front());
    if (first == kInvalidNode) return -1;
    return loop->delay() + g->arrivalOf(sendAt) - g->arrivalOf(first);
}

// ---------------------------------------------------------------------------
// The scope's taps (ADR-0175)
// ---------------------------------------------------------------------------

std::shared_ptr<const ScopeTap> Session::openScope(std::int64_t trackId, double seconds, ScopePoint point) {
    if (strips_.stripFor(trackId) == nullptr) return nullptr;
    const TapKey key{trackId, point};
    auto& tap = taps_[key];
    if (!tap) tap = std::make_shared<ScopeTap>(spec_.sampleRate, seconds);
    openTaps_.insert(key);
    attachTaps();
    return tap;
}

void Session::closeScope(std::int64_t trackId, ScopePoint point) {
    openTaps_.erase({trackId, point});
    if (point == ScopePoint::ChainInput) {
        const auto node = chainTapNodes_.find(trackId);
        if (node != chainTapNodes_.end()) {
            node->second->setInputTap(nullptr);
            chainTapNodes_.erase(node);
        }
    } else if (StripNode* strip = strips_.stripFor(trackId)) {
        if (point == ScopePoint::PreFader) strip->setPreFaderTap(nullptr);
        else strip->setTap(nullptr);
    }
}

void Session::attachTaps() {
    Graph* g = graph_.currentGraph();
    for (const auto& key : openTaps_) {
        const auto [id, point] = key;
        StripNode* strip = strips_.stripFor(id);
        const auto tap = taps_.find(key);
        if (strip == nullptr || tap == taps_.end()) continue;
        // Heard timeline: this point's arrival. Only PostFader adds the
        // strip's own reported latency (negative for ADR-0172 track delay).
        Node* tapped = strip;
        if (point == ScopePoint::ChainInput) {
            const auto chain = chainFor(id);
            std::size_t first = 0;
            for (std::size_t i = 0; i < chain.size(); ++i)
                if (chain[i]->eventFlow() == EventFlow::Consume) first = i + 1;
            if (first < chain.size()) tapped = chain[first];
            const auto gain = inputGains_.find(id);
            if (gain != inputGains_.end()) tapped = gain->second.get();
            if (frozenSources_.contains(id)) {
                const auto cached = trackChain(id);
                if (!cached.empty())
                    tapped = cached.front();
            }
            auto& previous = chainTapNodes_[id];
            if (previous && previous != tapped) previous->setInputTap(nullptr);
            previous = tapped;
        }
        std::int32_t latency = point == ScopePoint::PostFader ? strip->latencySamples() : 0;
        if (g != nullptr) {
            const NodeId n = g->find(*tapped);
            if (n != kInvalidNode) latency += g->arrivalOf(n);
        }
        tap->second->setLatency(latency);
        if (point == ScopePoint::ChainInput) tapped->setInputTap(tap->second.get());
        else if (point == ScopePoint::PreFader) strip->setPreFaderTap(tap->second.get());
        else strip->setTap(tap->second.get());
    }
}

void Session::bindDeviceSources() {
    deviceSourceProblems_.clear();
    std::set<std::int64_t> attached;
    for (Entry& e : entries_) {
        device::DeviceNode& node = devices_.nodeAt(e.hostIndex);
        DeviceAutomation* source = nullptr;
        if (!e.retired && deviceAutomation_) {
            const auto it = deviceAutomation_->byDevice.find(e.deviceId);
            if (it != deviceAutomation_->byDevice.end()) source = it->second.get();
        }
        // The binding itself is the keepAlive: the realiser keeps it with the
        // graph, so a retired graph never calls a freed source.
        node.setEventSource(source, source != nullptr ? std::shared_ptr<void>(deviceAutomation_)
                                                      : std::shared_ptr<void>{});
        if (source != nullptr) {
            attached.insert(e.deviceId);
            if (e.placeholder)
                deviceSourceProblems_.push_back("devices#" + std::to_string(e.deviceId) +
                                                ": a placeholder; its automation waits for the plug-in");
        }
    }
    if (deviceAutomation_)
        for (const auto& [id, source] : deviceAutomation_->byDevice)
            if (!attached.count(id))
                deviceSourceProblems_.push_back("devices#" + std::to_string(id) +
                                                ": has automation but is in no chain; not played");
}

bool Session::reenableAutomation() {
    if (overridden_.empty()) return true;
    overridden_.clear();
    return rebuild();
}

void Session::setAutomationWriteState(bool armed, bool recording) noexcept {
    automationArmed_ = armed;
    automationRecording_ = recording;
}

bool Session::beginAutomationWrite(std::int64_t laneId) {
    if (!loaded_ || !automationArmed_ || !automationRecording_) return false;
    if (writingAutomation_.contains(laneId)) return true;
    bool known = false;
    if (stripAutomation_)
        for (const auto& [key, id] : stripAutomation_->laneFor) known = known || id == laneId;
    if (stripAutomation_)
        for (const auto& [key, id] : stripAutomation_->sendLaneFor) known = known || id == laneId;
    if (deviceAutomation_)
        for (const auto& [key, id] : deviceAutomation_->laneFor) known = known || id == laneId;
    if (!known) return false;
    overridden_.erase(laneId);
    writingAutomation_.insert(laneId);
    return rebuild();
}

bool Session::finishAutomationWrite(std::int64_t laneId, bool follow) {
    if (writingAutomation_.erase(laneId) == 0) return false;
    if (!follow) overridden_.insert(laneId);
    return rebuild();
}

bool Session::reenableAutomation(std::int64_t laneId) {
    if (overridden_.erase(laneId) == 0) return false;
    return rebuild();
}

bool Session::overrideDeviceAutomation(std::int64_t deviceId, const std::string& paramId) {
    if (!deviceAutomation_) return true;
    const auto* instance = instanceFor(deviceId);
    const auto ref = automationParamRef(instance ? instance->identity().format : "",paramId);
    if (!ref) return true;
    const auto lane = deviceAutomation_->laneFor.find({deviceId,*ref});
    if (lane == deviceAutomation_->laneFor.end() || overridden_.contains(lane->second)) return true;
    overridden_.insert(lane->second);
    return rebuild();
}

bool Session::tick(std::int64_t nowMs) {
    mainLoudness_.collect();
    cueMonitor_.collect();
    return devices_.tick(nowMs);
}

// ---------------------------------------------------------------------------
// BlockProcessor
// ---------------------------------------------------------------------------

void Session::prepare(double sampleRate, std::int32_t maxFrames) {
    ++stats_.prepares;
    const bool changed = (sampleRate != spec_.sampleRate) || (maxFrames != spec_.maxFrames);
    if(changed)recording_->stop();

    // Decision 3, second half: same format, graph live, nothing to do. A driver
    // that stops and restarts at the same size -- a device-list change, a
    // sleep/wake -- must not produce a seam the user did not ask for.
    if (!changed && live_) return;

    if (changed) {
        ++stats_.formatChanges;
        spec_.sampleRate = sampleRate;
        spec_.maxFrames = maxFrames;
    }
    if (!loaded_) return;   // `process` writes silence until a project arrives
    rebuild();
}

bool Session::setParallelRendering(std::uint32_t workers,std::int32_t minimumFrames,std::uint32_t minimumNodes,RenderWorkers::Initializer initialize,void* context) {
    if(!graph_.setParallelRendering(workers,minimumFrames,minimumNodes,initialize,context))return false;
    if(!loaded_)return true;
    return rebuild();
}

bool Session::setPrecision(Precision p) {
    if (p == spec_.precision) return true;
    // A format change of its own: every buffer, sum and ring changes width,
    // so it is a new graph, never a change to the playing one (ADR-0207 d2).
    ++stats_.formatChanges;
    spec_.precision = p;
    graph_.setPrecision(p);
    if (!loaded_) return true;
    return rebuild();
}

bool Session::setDelayCompensation(bool on) {
    if(on==spec_.delayCompensation)return true;
    spec_.delayCompensation=on;graph_.setDelayCompensation(on);
    if(!loaded_)return true;
    return rebuild();
}

TransportInfo Session::TimingView::at(const Transport& transport) const noexcept {
    TransportInfo info;
    info.playing = transport.playing();
    info.timelineSample = transport.sampleAt(0);
    const auto ticks = tempo.secondsToTicks(static_cast<double>(info.timelineSample) / sampleRate);
    info.bpm = tempo.bpmAt(ticks);
    constexpr auto ppq = textproj::kPPQ; // SPEC 4.2; exact as Pd float
    constexpr auto whole = textproj::kWhole;
    info.ticksInQuarter = ticks % ppq;

    // Allocation-free counterpart of renderPosition(metersOf(signatures)).
    // Default 4/4 from zero; a change truncating a bar still counts that bar.
    std::int64_t start = 0, barsBefore = 0;
    std::int64_t barTicks = whole;
    for (const auto& meter : signatures) {
        if (meter.posTicks > ticks) break;
        const auto span = meter.posTicks - start;
        barsBefore += span / barTicks + (span % barTicks != 0 ? 1 : 0);
        start = meter.posTicks;
        info.timeSigNumerator = static_cast<int>(meter.numerator);
        info.timeSigDenominator = static_cast<int>(meter.denominator);
        barTicks = whole * meter.numerator / meter.denominator;
    }
    const auto into = ticks - start;
    const auto beatTicks = whole / info.timeSigDenominator;
    info.bar = barsBefore + into / barTicks + 1;
    info.beat = (into % barTicks) / beatTicks + 1;
    return info;
}

const TransportInfo* Session::TimingView::beginBlock()const noexcept {
    if(settings.hasLoop && (!session->loopApplied_ || session->loopOn_!=settings.loopEnabled || session->loopBegin_!=loopBegin || session->loopEnd_!=loopEnd)) {
        session->loopApplied_=true;session->loopOn_=settings.loopEnabled;session->loopBegin_=loopBegin;session->loopEnd_=loopEnd;
        session->transport_.loop(loopBegin,loopEnd,settings.loopEnabled);
    }
    blockInfo=at(session->transport_);
    return &blockInfo;
}

void Session::process(const AudioIo& io) noexcept {
    audio::CallbackScope callback;
    recording_->capture(io,transport_);
    playbackRunning_.store(transport_.playing(), std::memory_order_release);
    graph_.process(io);
    const auto* timing=dynamic_cast<const TimingView*>(graph_.renderedTransportState());
    AudioIo block=io;
    if(timing)block.transport=&timing->blockInfo;
    mainLoudness_.process(block);
    struct ClickContext {Session* session;const TimingView* timing;};
    ClickContext click{this,timing};
    CueMonitor::Click renderClick;
    if(timing!=nullptr && timing->settings.metronome && transport_.playing()) {
        renderClick.context=&click;
        renderClick.render=[](void* context,const AudioIo& cueIo) noexcept {
            const auto& call=*static_cast<ClickContext*>(context);call.session->renderMetronome(*call.timing,cueIo);
        };
    }
    else if(timing!=nullptr)renderMetronome(*timing,AudioIo{});
    cueMonitor_.process(graph_,block,renderClick);
    transport_.advance(io.frames);
}

void Session::renderMetronome(const TimingView& timing,const AudioIo& io) noexcept {
    if(!timing.settings.metronome || !transport_.playing()) {
        clickEnvelope_=0;clickWasPlaying_=false;return;
    }
    const auto& tempo=timing.tempo;const double rate=timing.sampleRate;
    const auto latency=graph_.renderedLatency();
    const double decay=std::exp(-1.0/(0.004*rate));
    for(std::int32_t i=0;i<io.frames;++i) {
        const auto sample=transport_.delayedSample(i,latency);
        if(sample<0){clickWasPlaying_=false;clickEnvelope_=0;continue;}
        const auto tick=tempo.secondsToTicks(static_cast<double>(sample)/rate);
        std::int64_t signature=0,numerator=4,denominator=4;
        for(const auto& meter:timing.signatures)if(meter.posTicks<=tick){signature=meter.posTicks;numerator=meter.numerator;denominator=meter.denominator;}else break;
        const auto beatTicks=textproj::kWhole/denominator;
        const auto beat=(tick-signature)/beatTicks;
        const auto beatTick=signature+beat*beatTicks;
        const double boundary=tempo.ticksToSeconds(beatTick)*rate;
        const bool adjacent=clickWasPlaying_ && sample>0 && clickLastSample_==sample-1;
        const bool atBoundary=std::abs(static_cast<double>(sample)-boundary)<=1.01;
        if((adjacent && beatTick!=clickLastBeat_) || (!adjacent && atBoundary)) {
            clickPhase_=0;clickEnvelope_=0.2;clickFrequency_=beat%numerator==0?1760:880;
        }
        const double click=clickEnvelope_*std::sin(clickPhase_);
        clickPhase_+=2*std::numbers::pi*clickFrequency_/rate;
        if(clickPhase_>=2*std::numbers::pi)clickPhase_-=2*std::numbers::pi;
        clickEnvelope_*=decay;if(clickEnvelope_<1e-12)clickEnvelope_=0;
        // Default Cue pair until the separate Cue Out routing (N23) is built.
        for(std::int32_t c=0;c<std::min<std::int32_t>(io.numOut,2);++c) {
            if(io.out && io.out[c])io.out[c][i]+=static_cast<float>(click);
            if(io.out64 && io.out64[c])io.out64[c][i]+=click;
        }
        clickLastSample_=sample;clickLastBeat_=beatTick;clickWasPlaying_=true;
    }
}

void Session::release() {
    recording_->stop();
    // The device has stopped; nothing is rendering. Release the plugins so a
    // later `prepare` -- at the same format or not -- prepares them again
    // rather than finding ADR-0090 d5's guard satisfied on a released
    // instance. `live_` false is what defeats the no-op path above.
    live_ = false;
    mainLoudness_.requestReset();
    playbackRunning_.store(false, std::memory_order_release);
    if (Graph* g = graph_.currentGraph()) g->release();
}

}  // namespace adi::engine
