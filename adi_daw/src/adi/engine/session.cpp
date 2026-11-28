// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/device_param_ref.hpp"
#include "adi/engine/midi_hardware.hpp"
#include "adi/freeze_groups.hpp"
#include "adi/freeze_state.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/row_capture.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
//
// The session. See session.hpp for the three decisions; this file is their
// mechanics. ADR-0122.

#include "adi/engine/session.hpp"
#include "adi/engine/project_view.hpp"

#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "adi/audio/io_audit.hpp"
#include "juce/clap_host.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/midi_input_capture.hpp"
#include "adi/gesture_capture_detail.hpp"

#include <algorithm>
#include <numbers>
#include <limits>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <functional>
#include <mutex>
#include <thread>

namespace adi::engine {
namespace {

// Preserve playback with Monitor Off until the overlap behavior is measured (R45).
constexpr bool kMonitorOffMutesPreviousTake = false;

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

// The project a capture is stamped with, as recording_commands compares it when the take lands:
// SQLite's resolved name of the main database.
std::string recordingIdentity(const Store& store) {
    SQLite::Statement row(store.db(),"PRAGMA database_list");
    while(row.executeStep())if(row.getColumn(1).getString()=="main")return row.getColumn(2).getString();
    return {};
}

}  // namespace

struct ScopeLease::State {
    mutable std::mutex mutex;
    std::shared_ptr<const ScopeTap> current;
    std::function<void()> release;
    bool alive = true;
};
ScopeLease::ScopeLease(std::shared_ptr<State> state) : state_(std::move(state)) {}
ScopeLease::~ScopeLease() { reset(); }
ScopeLease::ScopeLease(ScopeLease&& other) noexcept : state_(std::move(other.state_)) {}
ScopeLease& ScopeLease::operator=(ScopeLease&& other) noexcept {
    if (this != &other) { reset(); state_ = std::move(other.state_); }
    return *this;
}
void ScopeLease::reset() noexcept {
    auto state = std::move(state_);
    if (!state) return;
    std::function<void()> release;
    {
        std::lock_guard lock(state->mutex);
        release = std::move(state->release);
        state->current.reset();
        state->alive = false;
    }
    // Owner publication may fail to allocate. The consumer is already gone; leaving
    // its previous capture publication alive until the next rebuild is safe.
    if (release) try { release(); } catch (...) {}
}
ScopeLease::operator bool() const noexcept {
    if (!state_) return false;
    std::lock_guard lock(state_->mutex);
    return state_->alive;
}
std::shared_ptr<const ScopeTap> ScopeLease::tap() const {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    return state_->current;
}

struct Session::InputMonitors {
    // The driver lends these buffers for one logical slice. Graph processing
    // joins its workers before the driver clears or replaces the pointers.
    const float* const* input = nullptr;
    std::int32_t channels = 0;

    class Source final : public Node {
    public:
        Source(const InputMonitors& owner, const Transport& transport,
               const std::span<const std::int64_t>& recordingTracks, std::int64_t track,
               std::array<std::int32_t, 2> sourceChannels, std::vector<ClipPlayback::SampleRange> ranges)
            : owner_(owner), transport_(transport), recordingTracks_(recordingTracks), track_(track),
              channels_(sourceChannels), ranges_(std::move(ranges)) {
            std::sort(ranges_.begin(), ranges_.end());
            std::size_t count = 0;
            for (const auto& range : ranges_) {
                if (count && range.first <= ranges_[count-1].second)
                    ranges_[count-1].second = std::max(ranges_[count-1].second, range.second);
                else
                    ranges_[count++] = range;
            }
            ranges_.resize(count);
        }
        bool processesDouble() const noexcept override { return true; }
        const char* name() const noexcept override { return "hardware input monitor"; }
        void process(const NodeIo& io) noexcept override {
            if (io.wide()) copy(io, io.out64);
            else copy(io, io.out);
        }
    private:
        const InputMonitors& owner_;
        const Transport& transport_;
        const std::span<const std::int64_t>& recordingTracks_;
        std::int64_t track_;
        std::array<std::int32_t, 2> channels_;
        std::vector<ClipPlayback::SampleRange> ranges_;

        template <class Sample>
        void copy(const NodeIo& io, Sample* const* out) noexcept {
            if (!out) return;
            const bool mono = channels_[0] == channels_[1];
            for (std::int32_t c = 0; c < io.channels; ++c) {
                if (!out[c]) continue;
                const auto channel = mono ? channels_[0] : c < 2 ? channels_[static_cast<std::size_t>(c)] : -1;
                const float* source = owner_.input && channel >= 0 && channel < owner_.channels
                    ? owner_.input[channel] : nullptr;
                if (source) std::copy_n(source + io.blockOffset, io.frames, out[c] + io.blockOffset);
                else std::fill_n(out[c] + io.blockOffset, io.frames, Sample{});
            }
            if (ranges_.empty() || !transport_.playing())
                return;
            if (!recordingTracks_.empty()) {
                const auto* first = recordingTracks_.data();
                if (std::binary_search(first, first + recordingTracks_.size(), track_))
                    return;
            }
            const auto* begin = ranges_.data();
            const auto* end = begin + ranges_.size();
            for (std::int32_t i = io.blockOffset; i < io.blockOffset + io.frames; ++i) {
                const auto sample = transport_.sampleAt(i);
                const auto* after = std::upper_bound(begin, end, sample,
                    [](std::int64_t at, const ClipPlayback::SampleRange& range) { return at < range.first; });
                if (after != begin && sample < (after-1)->second)
                    for (std::int32_t c = 0; c < io.channels; ++c)
                        if (out[c]) out[c][i] = Sample{};
            }
        }
    };
    std::map<std::int64_t, std::unique_ptr<Source>> tracks;
};

struct Session::ScopeRuntime {
    struct Request { ScopeSource source; double seconds; std::shared_ptr<ScopeLease::State> state; };
    struct PortKey {
        Node::ScopePorts* ports = nullptr;
        bool output = false;
        std::uint64_t epoch = 0;
        auto operator<=>(const PortKey&) const = default;
    };
    struct Binding {
        PortKey key;
        std::shared_ptr<Node::ScopePorts> ports;
        std::shared_ptr<const void> identity;
        std::shared_ptr<ScopeTap> tap;
    };
    struct Bindings : Sequenced { std::vector<Binding> items; };
    std::map<std::uint64_t, Request> requests;
    std::map<ScopeSource, double> legacy;
    std::map<PortKey, Binding> groups;
    std::map<ScopeSource, std::shared_ptr<ScopeTap>> bySource;
    SnapshotPublisher<Bindings> publisher;
    std::unique_ptr<Bindings> deferred; // owner only; newest desired plan behind one unacknowledged publication
    bool dirty = false; // owner only; a failed demand rebuild must reconcile before deferred publication
    const Bindings* audio = nullptr; // audio only, protected by publisher's strict epoch
    std::uint64_t next = 0;

    bool awaitingAudio() const noexcept {
        return publisher.publishedSeq() > publisher.inUseSeq();
    }
    void publishOrDefer(std::unique_ptr<Bindings>& plan) {
        publisher.collect();
        if (awaitingAudio()) {
            deferred = std::move(plan);
            return;
        }
        // Any publication failure preserves both the caller's candidate and
        // the previous deferred demand. Acceptance below is nonthrowing.
        publisher.publishPrepared(plan);
        deferred.reset();
    }
    std::size_t collect() {
        const auto freed = publisher.collect();
        if (deferred && !awaitingAudio()) publisher.publishPrepared(deferred);
        return freed;
    }

    void adopt() noexcept {
        const auto* plan = publisher.peek();
        if (plan == audio) return;
        // Finish all access to the previous plan before announcing the new one.
        // Ports themselves outlive a retired Node; no graph-collection assumption.
        if (audio) for (const auto& binding : audio->items) {
            auto& port = binding.key.output ? binding.ports->output : binding.ports->input;
            port.store(nullptr, std::memory_order_release);
        }
        if (plan) for (const auto& binding : plan->items) {
            auto& port = binding.key.output ? binding.ports->output : binding.ports->input;
            port.store(binding.tap.get(), std::memory_order_release);
        }
        publisher.announce(plan);
        audio = plan;
    }
    void revoke() noexcept {
        for (auto& [id, request] : requests) {
            (void)id;
            std::lock_guard lock(request.state->mutex);
            request.state->alive = false;
            request.state->release = {};
            request.state->current.reset();
        }
        // Session destruction is after the driver stops. The last publication is
        // still protected, even if a graph holding its Node has already retired.
        if (audio) for (const auto& binding : audio->items) {
            auto& port = binding.key.output ? binding.ports->output : binding.ports->input;
            port.store(nullptr, std::memory_order_release);
        }
    }
};

struct Session::PendingLoad {
    const Store* store = nullptr;
    std::thread::id owner;
    std::optional<gesture::detail::StoreWriteStamp> stamp;
    std::uint64_t sequence = 0;
    std::vector<std::size_t> deviceRows;
    std::size_t next = 0;
    bool failed = false;
};

Session::Session()
    : midiInput_(std::make_unique<LiveMidiInput>()), midiInputCapture_(std::make_unique<MidiInputCapture>()),
      recording_(std::make_unique<AudioInputRecorder>()), gestureAudio_(makeGestureAudio()),
      endpoint_(std::make_shared<SessionEndpoint>()) {
    scopeRuntime_ = std::make_unique<ScopeRuntime>();
    endpoint_->owner_=this;
}
Session::~Session() {
    scopeRuntime_->revoke();
    endpoint_->owner_=nullptr;
    endpoint_->keyRoutes_.reset();
    lifetime_.reset();
    for(const auto& client:runtimeClients_)client->sessionEnded();
    runtimeClients_.clear();
}

DeviceRuntimeToken Session::deviceRuntimeToken(std::int64_t deviceId) const {
    DeviceRuntimeToken result;
    const auto* entry=entryFor(deviceId);
    if(!entry || entry->retired || !entry->runtimeIdentity)return result;
    auto* instance=const_cast<Session*>(this)->instanceFor(deviceId);
    if(!instance)return result;
    auto data=std::make_shared<DeviceRuntimeToken::Data>();
    data->session=endpoint_;data->identity=entry->runtimeIdentity;data->instance=instance;
    data->deviceId=deviceId;data->runtimeEpoch=runtimeEpoch_;data->shapeEpoch=instance->preparationShapeEpoch();
    data->pluginRef=entry->resolvedPluginRef;data->format=entry->resolvedFormat;data->uid=entry->resolvedUid;
    result.data_=std::move(data);return result;
}

device::DeviceInstance* Session::resolveDevice(const DeviceRuntimeToken& token) noexcept {
    if(!token.data_ || token.data_->runtimeEpoch!=runtimeEpoch_)return nullptr;
    const auto owner=token.data_->session.lock();
    if(!owner || owner->get()!=this || token.data_->identity.expired())return nullptr;
    const auto* entry=entryFor(token.data_->deviceId);
    if(!entry || entry->retired || !entry->runtimeIdentity ||
       token.data_->identity.owner_before(entry->runtimeIdentity) ||
       entry->runtimeIdentity.owner_before(token.data_->identity) ||
       entry->resolvedPluginRef!=token.data_->pluginRef || entry->resolvedFormat!=token.data_->format ||
       entry->resolvedUid!=token.data_->uid)return nullptr;
    auto* instance=instanceFor(token.data_->deviceId);
    return instance==token.data_->instance && instance->preparationShapeEpoch()==token.data_->shapeEpoch?instance:nullptr;
}

bool Session::presentationReady(const AppliedSource& source,std::uint64_t sequence) const noexcept {
    if(presentationPending_)return false;
    for(const auto& client:runtimeClients_)if(!client->presentationReady(source,sequence))return false;
    return true;
}
void Session::retainRuntimeClient(std::shared_ptr<SessionRuntimeClient> client) {
    if(!client)return;
    if(std::find(runtimeClients_.begin(),runtimeClients_.end(),client)==runtimeClients_.end())
        runtimeClients_.push_back(std::move(client));
}

AudioInputRecorder& Session::recording() noexcept{return *recording_;}
bool Session::recordingStartsAfterLoop(std::int64_t insertTick,
                                       std::optional<std::int64_t> playingSample) const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.currentTransportState());
    if (!timing || !timing->settings.loopEnabled || !timing->settings.hasLoop)
        return false;
    const auto start = playingSample ? *playingSample
                                     : timing->sampleForTick(timing->settings.songStartTicks.value_or(insertTick));
    return start >= timing->loopEnd;
}
bool Session::configureArrangementLoopRecording(AudioRecordingSpec& options, std::string& error) const {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.currentTransportState());
    if (!timing || !timing->settings.loopEnabled || !timing->settings.hasLoop) {
        error = "The loop brace is not ready for recording.";
        return false;
    }

    TempoMap authored;
    for (const auto& point : model_.tempo)
        authored.events.push_back({point.posTicks, point.bpm, static_cast<int>(point.curve)});
    if (authored.events.empty())
        authored.events.push_back({0, 120.0, 0});
    if (timing->spec.externalTempoBpm || timing->tempo.events != authored.events) {
        error = "Loop recording needs playback to follow the project's authored tempo.";
        return false;
    }

    const auto begin = timing->loopBegin;
    const auto end = timing->loopEnd;
    const auto representable = [&](std::int64_t sample) {
        const auto tick = timing->tempo.secondsToTicks(static_cast<double>(sample) / timing->sampleRate);
        return timing->sampleForTick(tick) == sample;
    };
    if (begin < 0 || end <= begin || end > (std::int64_t{1} << 50) ||
        end != timing->punchOutSample || !representable(begin) || !representable(end)) {
        error = "Set a loop brace with a duration that can be recorded at this sample rate.";
        return false;
    }

    AudioRecordingLoop loop{begin, end};
    loop.tempo = std::make_shared<const TempoMap>(std::move(authored));
    options.loop = std::move(loop);
    return true;
}
bool Session::setSelectionArmedTrack(std::int64_t track) {
    if (track == selectionArmedTrack_) return true;
    const auto previous = std::exchange(selectionArmedTrack_, track);
    if (!loaded_ || rebuild()) return true;
    selectionArmedTrack_ = previous;
    return false;
}

bool Session::startRecording(const AudioRecordingSpec& options,std::string& error) {
    if(!live_){error="No active audio session for recording";return false;}
    if(options.sampleRate!=spec_.sampleRate || options.maxFrames!=static_cast<std::uint32_t>(spec_.maxFrames)) {
        error="Recording format must match the active audio device";return false;
    }
    for(const auto& target:options.targets) {
        const auto track=std::find_if(model_.tracks.begin(),model_.tracks.end(),[&](const auto& row){return row.id==target.trackId;});
        if(track==model_.tracks.end() || !recordArmed(*track) || (track->kind!="audio" && track->kind!="midi")) {
            error="Recording target must be an armed audio or MIDI track";return false;
        }
        if (target.source == RecordingSource::MainMix && (track->kind != "audio" || spec_.channels < 2)) {
            error = "Resampling needs Main stereo and an armed audio track";
            return false;
        }
    }
    if(!options.recoveryProjectUuid.empty() && options.sourceProject!=recordingProject_) {
        error="Recording recovery was configured for a different project";
        return false;
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

bool Session::setHearDifference(std::int64_t deviceId) {
    if (deviceId == 0) {
        hearDifference_ = 0;
        graph_.setDifferenceNode(nullptr);
        return true;
    }
    const Entry* entry = entryFor(deviceId);
    auto* node = entry != nullptr && !entry->retired && !entry->parked ? nodeFor(deviceId) : nullptr;
    if (node == nullptr) return false;
    hearDifference_ = deviceId;
    graph_.setDifferenceNode(node);
    return true;
}

bool Session::setPreviewTrack(std::int64_t trackId) {
    if (trackId == previewTrack_) return true;
    if (trackId != 0) {
        if (samplePreview_ == nullptr || !spec_.performanceState || spec_.exporting || spec_.channels < 2)
            return false;
        const auto track = std::find_if(model_.tracks.begin(), model_.tracks.end(),
                                        [trackId](const rows::Track& t) { return t.id == trackId; });
        if (track == model_.tracks.end() || track->kind != "audio" || track->frozen ||
            frozenSources_.contains(trackId))
            return false;
        if (!previewSource_) {
            previewSource_ = std::make_unique<PreviewThroughTrack>(*samplePreview_);
            cueMonitor_.setPreviewThroughTrack(previewSource_.get());
        }
    }
    const auto previous = previewTrack_;
    previewTrack_ = trackId;
    if (loaded_ && !rebuild()) {
        previewTrack_ = previous;   // the graph that plays is unchanged
        return false;
    }
    return true;
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

void Session::resolveOne(const Store& store, const rows::Device& row,
                         std::unique_ptr<device::DeviceInstance> preloaded) {
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
    e.runtimeIdentity=std::make_shared<const int>(0);
    e.resolvedPluginRef = row.pluginRefId;
    if(ref!=nullptr) {e.resolvedFormat=ref->format;e.resolvedUid=ref->uid;}
    e.trackId = chain->trackId.value_or(0);
    e.name = !row.name.empty() ? row.name
           : (ref != nullptr && !ref->name.empty()) ? ref->name
           : "devices#" + std::to_string(row.id);

    std::unique_ptr<device::DeviceInstance> inst = std::move(preloaded);
    std::string err;
    if (inst != nullptr) {
        // SC-318: a placeholder's plug-in the loader has just provided.
    } else if (ref == nullptr) {
        err = "no plugin reference";
    } else if (!loader_) {
        err = "no device loader";
    } else {
        DeviceRequest rq;
        rq.device = &row;
        rq.ref = ref;
        rq.sampleRate = spec_.sampleRate;
        rq.maxFrames = spec_.maxFrames;
        rq.channels = spec_.channels;
        rq.exporting = spec_.exporting;
        inst = loader_(rq, err);
        if (inst == nullptr && err.empty()) err = "the loader returned nothing";
    }

    if (inst == nullptr) {
        e.placeholder = true;
        e.error = err;
        inst = makePlaceholder(store, row, ref);
        ++stats_.placeholders;
    } else {
        inst->setOffline(spec_.offline || spec_.exporting);
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
        const auto mapped = byId_.find(row.id);
        if (mapped == byId_.end()) {
            resolveOne(store, row);
            continue;
        }

        // Plug-in references are ordered by ID. The reference row itself can
        // be replaced within the same journal step as the device.
        const rows::PluginRef* ref = nullptr;
        if (row.pluginRefId) {
            const auto found = std::lower_bound(
                model_.pluginRefs.begin(), model_.pluginRefs.end(), *row.pluginRefId,
                [](const rows::PluginRef& candidate, std::int64_t id) { return candidate.id < id; });
            if (found != model_.pluginRefs.end() && found->id == *row.pluginRefId)
                ref = &*found;
        }
        const auto compatible = [&](const Entry& candidate) {
            return candidate.deviceId == row.id && candidate.resolvedPluginRef == row.pluginRefId &&
                (ref ? candidate.resolvedFormat == ref->format && candidate.resolvedUid == ref->uid
                     : candidate.resolvedFormat.empty() && candidate.resolvedUid.empty());
        };

        const auto previousIndex = mapped->second;
        const auto retireDisplaced = [&] {
            // The numeric row remains present, so retireDepartedRows cannot
            // distinguish these incarnations. Reacquire after a fresh load,
            // which can grow entries_.
            auto& previous = entries_[previousIndex];
            if (!previous.retired) {
                previous.retired = true;
                previous.trackId = 0;
                ++stats_.retired;
            }
        };

        auto index = previousIndex;
        if (!compatible(entries_[index])) {
            const auto retained = std::find_if(entries_.rbegin(), entries_.rend(), compatible);
            if (retained == entries_.rend()) {
                resolveOne(store, row);
                retireDisplaced();
                continue;
            }
            index = static_cast<std::size_t>(retained.base() - entries_.begin() - 1);
        }

        Entry& entry = entries_[index];
        if (entry.retired && entry.parked) {
            // Unfreeze restores saved state instead of old voices and delays.
            resolveOne(store, row);
            retireDisplaced();
            continue;
        }
        if (index != previousIndex)
            retireDisplaced();
        if (entry.retired) {
            // Undo and Redo revive the retained state for the restored identity.
            entry.retired = false;
            --stats_.retired;
        }
        mapped->second = index;
    }
    // A newly opened Session prepares its first graph straight after
    // resolving the devices. Give those preparations the saved key intent.
    if (!live_) syncFlags();
}

std::size_t Session::retryPlaceholders(const Store& store) {
    if (!loaded_ || !loader_ || audio::onAudioThread) return 0;
    std::size_t woke = 0;
    bool reasonChanged = false;
    for (const rows::Device& row : model_.devices) {
        if (parkedDevice(row)) continue;
        const auto mapped = byId_.find(row.id);
        if (mapped == byId_.end()) continue;
        const std::size_t previous = mapped->second;
        if (!entries_[previous].placeholder || entries_[previous].retired) continue;
        const rows::PluginRef* ref = refFor(row);
        if (ref == nullptr) continue;
        DeviceRequest rq;
        rq.device = &row;
        rq.ref = ref;
        rq.sampleRate = spec_.sampleRate;
        rq.maxFrames = spec_.maxFrames;
        rq.channels = spec_.channels;
        rq.exporting = spec_.exporting;
        std::string err;
        auto inst = loader_(rq, err);
        if (inst == nullptr) {
            if (!err.empty() && entries_[previous].error != err) {
                entries_[previous].error = std::move(err);
                reasonChanged = true;
            }
            continue;
        }
        resolveOne(store, row, std::move(inst));
        Entry& old = entries_[previous];  // resolveOne may have grown entries_
        old.retired = true;
        old.trackId = 0;
        ++stats_.retired;
        --stats_.placeholders;
        ++woke;
    }
    if (woke != 0) {
        syncFlags();
        (void)rebuild();
    } else if (reasonChanged) refreshProblems();
    return woke;
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
    }
}

void Session::syncFlags() {
    std::set<std::int64_t> keyed;
    for(const auto& route:model_.routing)
        if(route.enabled && route.kind=="sidechain" && route.dstKind=="device")keyed.insert(route.dstId);
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
            if (!e.placeholder) {
                syncRoute(e, node.instance());
                if(!node.instance().configureSidechain(keyed.contains(e.deviceId))) {
                    const auto problem="devices#"+std::to_string(e.deviceId)+": key input is unavailable";
                    if(std::find(sessionProblems_.begin(),sessionProblems_.end(),problem)==sessionProblems_.end())
                        sessionProblems_.push_back(problem);
                }
            }
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

struct Session::MidiHardwareNode final : Node {
    MidiHardwareNode(Session& owner, const StripNode& strip, std::int64_t track, rows::MidiOutput route,
                     std::uint64_t serial)
        : owner_(owner), strip_(strip), track_(track), route_(std::move(route)), port_(midiPortKey(route_.port)),
          serial_(serial) {}

    EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }
    bool processesDouble() const noexcept override { return true; }
    const char* name() const noexcept override { return "MIDI hardware output"; }

    void process(const NodeIo& io) noexcept override {
        if (io.wide()) copyAudio(io.in64, io.out64, io.channels, io.blockOffset, io.frames);
        else copyAudio(io.in, io.out, io.channels, io.blockOffset, io.frames);
        sendMidi(io);
    }

    // The graph can sleep after notes end even while a hardware pedal is held.
    void silenceTaps(const NodeIo& io) noexcept override { sendMidi(io); }

    const rows::MidiOutput& route() const noexcept { return route_; }
    const StripNode& strip() const noexcept { return strip_; }
    std::uint64_t serial() const noexcept { return serial_; }
    // Only the owner accesses this reference. Each prepared input route
    // retains its own shared reference for audio-thread use.
    std::shared_ptr<LiveMidiInput::Voices> liveVoices;

private:
    void sendMidi(const NodeIo& io) noexcept {
        const auto* sink = owner_.midiHardwareSink_;
        if (!sink || !sink->send || io.exporting || (!releasePending_ && io.events.empty())) return;
        const auto now = owner_.midiHardwareStreamSample_;
        const auto blockEnd = io.blockOffset + io.frames;
        auto event = io.events.begin();
        const auto eventsEnd = io.events.end();
        for (auto frame = io.blockOffset; frame < blockEnd;) {
            const auto run = strip_.midiAudibleRun(io, frame);
            if (!run.audible && releasePending_) {
                sink->send(sink->context, port_, track_, now + frame, kMidiHardwareRelease);
                releasePending_ = false;
            }
            for (; event != eventsEnd && event->frame < run.endFrame; ++event) {
                if (!run.audible) continue;
                if (const auto word = midiHardwareWord(*event, route_.channel); word != 0) {
                    sink->send(sink->context, port_, track_, now + event->frame, word);
                    releasePending_ = true;
                }
            }
            if (event == eventsEnd && !releasePending_) break;
            frame = run.endFrame;
        }
    }

    template<class Sample>
    static void copyAudio(const Sample* const* input, Sample* const* output,
                          std::int32_t channels, std::int32_t offset, std::int32_t frames) noexcept {
        for (std::int32_t channel = 0; channel < channels; ++channel) {
            if (input && input[channel])
                std::copy_n(input[channel] + offset, frames, output[channel] + offset);
            else
                std::fill_n(output[channel] + offset, frames, Sample{});
        }
    }

    Session& owner_;
    const StripNode& strip_;
    std::int64_t track_;
    rows::MidiOutput route_;
    std::uint64_t port_;
    std::uint64_t serial_;
    bool releasePending_ = false;
};

void Session::attach(std::shared_ptr<const TimingView> timing) {
    device::DeviceHost::RebuildSpec rs;
    rs.transportState=timing;
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
    rs.devicesFor = [this, timing](std::int64_t trackId) { return graphTrackChain(trackId, *timing); };
    rs.keyTapFor = [this](std::int64_t track,Tap tap)->std::optional<Node*> {
        if(tap==Tap::PreFx && frozenSources_.contains(track))return std::nullopt;
        return tapFor(track,tap);
    };
    rs.keyDeviceFor = [this](std::int64_t device)->Node* {
        const auto* entry=entryFor(device);auto* instance=instanceFor(device);
        if(!entry || entry->retired || entry->parked || entry->placeholder || !instance ||
           !instance->loaded() || instance->sidechainInputChannels()<=0)return nullptr;
        return nodeFor(device);
    };
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
    rs.sourcesFor = playbackSources(clips(), midi_.get(), std::move(timing));
    rs.monitors=cueMonitor_.points([this](std::int64_t id,Tap tap){return tapFor(id,tap);});
    devices_.attachHost(graph_, std::move(rs));
}

SourceFn Session::playbackSources(ClipPlayback* clipSource, MidiClips* midiSource,
                                  std::shared_ptr<const TimingView> timing) {
    return [this, clipSource, midiSource, timing=std::move(timing)](std::int64_t id) {
        auto nodes = sources_ && !frozenSources_.contains(id) ? sources_(id) : std::vector<Node *>{};
        if (auto* monitor = timing->inputMonitorSource(id)) nodes.push_back(monitor);
        // MX-40: the preview, at the same seam as the track's input monitor.
        if (previewSource_ && previewTrack_ != 0 && id == previewTrack_ && !frozenSources_.contains(id) &&
            timing->spec.performanceState && !timing->spec.exporting)
            nodes.push_back(previewSource_.get());
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
}

bool Session::validateRouting(const rows::Model& candidate,std::string& error) {
    RealizeOptions options;options.channels=spec_.channels;
    options.devicesFor=[this](std::int64_t track){return trackChain(track);};
    options.keyTapFor=[this](std::int64_t track,Tap tap)->std::optional<Node*> {
        if(tap==Tap::PreFx && frozenSources_.contains(track))return std::nullopt;
        return tapFor(track,tap);
    };
    options.keyDeviceFor=[this](std::int64_t device)->Node* {
        const auto* e=entryFor(device);auto* i=instanceFor(device);
        return e && !e->retired && !e->parked && !e->placeholder && i && i->loaded() && i->sidechainInputChannels()>0?nodeFor(device):nullptr;
    };
    options.tapFor=[this](std::int64_t track,Tap tap){return tapFor(track,tap==Tap::PostFader?Tap::PostFx:tap);};
    options.sendFor=[this](std::int64_t id)->Node*{return strips_.sendFor(id);};
    options.sendTailFor=[this](std::int64_t id){return summing_.sendTailFor(id);};
    options.feedbackFor=[this](std::int64_t id){auto* loop=strips_.loopFor(id);return loop?FeedbackNodes{&loop->writer(),&loop->reader()}:FeedbackNodes{};};
    auto draft=realize(planGraph(candidate),options);
    if(!draft->ok()){error=draft->error();return false;}
    if(!draft->problems().empty()) {
        const auto baseline=realize(planGraph(model_),options);
        for(const auto& problem:draft->problems())
            if(std::find(baseline->problems().begin(),baseline->problems().end(),problem)==baseline->problems().end()) {
                error=problem;return false;
            }
    }
    if(!draft->graph().validateTopology()){error=draft->graph().error();return false;}
    return true;
}

Node* Session::TimingView::inputMonitorSource(std::int64_t track) const noexcept {
    if (!inputMonitors) return nullptr;
    const auto found = inputMonitors->tracks.find(track);
    return found == inputMonitors->tracks.end() ? nullptr : found->second.get();
}

void Session::prepareInputMonitors(TimingView& timing, const rows::Model& model, ClipPlayback& clips) {
    if (!timing.spec.performanceState || timing.spec.exporting) return;
    for (const auto& track : model.tracks) {
        if (track.kind != "audio" || track.frozen || frozenSources_.contains(track.id) ||
            frozenGroupChildren_.contains(track.id))
            continue;
        const bool inputOnly = track.monitorMode == 1;
        const bool armed = recordArmed(track);
        const bool armedAuto = track.monitorMode == 2 && armed;
        const bool armedOff = track.monitorMode == 0 && armed;
        if (armedAuto || (armedOff && kMonitorOffMutesPreviousTake))
            clips.muteWhileRecording(track.id, recordingAudioTracks_);
        if (!inputOnly && !armedAuto) continue;
        const auto binding = monitorInputs_.find(track.id);
        if (binding == monitorInputs_.end()) continue;
        if (!timing.inputMonitors) timing.inputMonitors = std::make_shared<InputMonitors>();
        auto ranges = armedAuto ? clips.arrangementRanges(track.id) : std::vector<ClipPlayback::SampleRange>{};
        timing.inputMonitors->tracks.emplace(track.id, std::make_unique<InputMonitors::Source>(
            *timing.inputMonitors, transport_, recordingAudioTracks_, track.id, binding->second, std::move(ranges)));
        if (inputOnly) clips.useMonitoredInput(track.id);
    }
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

std::vector<Node*> Session::graphTrackChain(std::int64_t track, const TimingView& timing) {
    auto chain = trackChain(track);
    if (const auto output = timing.midiHardware.find(track); output != timing.midiHardware.end()) {
        const auto stopsMidi = std::find_if(chain.begin(), chain.end(),
            [](const Node* node) { return node->eventFlow() != EventFlow::Through; });
        chain.insert(stopsMidi, output->second.get());
    }
    return chain;
}

DeviceChainFn Session::eventPathFor(const rows::Model& model, const TimingView& timing) {
    return [this, &timing, plan = std::make_shared<const GraphPlan>(planGraph(model))](std::int64_t track) {
        std::vector<Node*> result;
        const auto start = plan->indexOf(track);
        if (!start) return result;
        std::vector<bool> visited(plan->nodes.size(), false);
        std::vector<std::size_t> pending{*start};
        for (std::size_t at = 0; at < pending.size(); ++at) {
            const auto index = pending[at];
            if (visited[index]) continue;
            visited[index] = true;
            if (plan->nodes[index].kind == PlannedKind::Vca) continue;
            const auto trackId = plan->nodes[index].trackId;
            MidiHardwareNode* output = nullptr;
            if (const auto found = timing.midiHardware.find(trackId); found != timing.midiHardware.end())
                output = found->second.get();
            bool through = true;
            for (auto* node : chainFor(trackId)) {
                const bool passesMidi = node->eventFlow() == EventFlow::Through;
                if (output && !passesMidi) break;
                result.push_back(node);
                if (!passesMidi) { through = false; break; }
            }
            if (!through) continue;
            if (output) {
                result.push_back(output);
                continue;
            }
            // Sends stop notes and sidechains carry only audio. An
            // unconsumed main path can reach more than one instrument;
            // retain every destination identity, never its later audio FX.
            for (const auto& edge : plan->edges)
                if (edge.from == index && edge.to < plan->nodes.size() &&
                    edge.bus == Bus::Main && edge.routingId == 0 && !edge.feedback)
                    pending.push_back(edge.to);
        }
        return result;
    };
}

void Session::prepareMidiHardware(TimingView& timing, const rows::Model& model, const TimingView* previous) {
    for (const auto& route : model.routing) {
        if (!route.enabled || !route.midiOutput || timing.midiHardware.contains(route.srcId)) continue;
        const auto track = std::find_if(model.tracks.begin(), model.tracks.end(),
            [&](const rows::Track& row) { return row.id == route.srcId; });
        if (track == model.tracks.end() || (track->kind != "midi" && track->kind != "instrument") ||
            track->frozen || frozenGroupChildren_.contains(track->id)) continue;
        const auto* strip = strips_.stripFor(track->id);
        if (!strip) continue;
        std::shared_ptr<MidiHardwareNode> output;
        if (previous) {
            const auto found = previous->midiHardware.find(track->id);
            if (found != previous->midiHardware.end() && found->second->route() == *route.midiOutput &&
                &found->second->strip() == strip)
                output = found->second;
        }
        if (!output)
            output = std::make_shared<MidiHardwareNode>(*this, *strip, track->id, *route.midiOutput,
                                                        ++midiHardwareNodeSerial_);
        timing.midiHardware.emplace(track->id, std::move(output));
    }
    if (timing.midiHardware.empty()) return;

    // Reused nodes keep their sounding voices through an unrelated rebuild.
    // A new node may replace one that has sent notes even when its wire
    // address is the same (for example a removed and restored track).
    auto key = std::uint64_t{14695981039346656037ull};
    constexpr std::uint64_t prime = 1099511628211ull;
    for (const auto& [track, output] : timing.midiHardware) {
        key = (key ^ static_cast<std::uint64_t>(track)) * prime;
        key = (key ^ output->serial()) * prime;
    }
    timing.midiHardwareRouting = key != 0 ? key : 1;
}

void Session::prepareMidiInput(TimingView& timing, const rows::Model& model, const MidiClips& clips) {
    if (!timing.spec.performanceState || timing.spec.exporting) return;
    struct Target {
        std::int64_t track;
        std::span<const MidiMonitorRange> blockedRanges;
        MidiInputFilter filter;
        std::int64_t playGrid;
        std::int64_t playForgiveness;
    };
    std::map<const Node*, Target> monitored;
    const auto setTarget = [&clips](LiveMidiInput::Route& route, const Target& target) {
        route.monitorTrack = target.track;
        route.filter = target.filter;
        route.playGrid = target.playGrid;
        route.playForgiveness = target.playForgiveness;
        if (!target.blockedRanges.empty()) {
            route.blockedRanges.assign(target.blockedRanges.begin(), target.blockedRanges.end());
            route.authoredPitchChase = clips.pitchChaseFor(target.track);
        }
    };
    const auto firstMidiEffect = [this](std::int64_t track, Node* output) {
        const auto chain = chainFor(track);
        return !chain.empty() && chain.front()->eventFlow() == EventFlow::Through ? chain.front() : output;
    };
    decltype(midiHistories_) histories;
    const bool validPlayRate = std::isfinite(timing.sampleRate) && timing.sampleRate >= 1 &&
                               timing.sampleRate <= std::numeric_limits<std::uint32_t>::max();
    for (const auto& track : model.tracks) {
        const bool midi = track.kind == "midi" || track.kind == "instrument";
        const bool armed = recordArmed(track);
        const auto filter = midi ? midiInputFilter(track.inputRef) : MidiInputFilter{};
        if (midi && !track.locked && !track.frozen && !frozenGroupChildren_.contains(track.id) &&
            (armed || track.monitorMode == 1) && timing.sampleRate >= 8000 && timing.sampleRate <= 768000) {
            const auto rate = static_cast<std::uint32_t>(std::lround(timing.sampleRate));
            const auto found = midiHistories_.find(track.id);
            std::shared_ptr<LiveMidiInput::History> history;
            if (found != midiHistories_.end() && found->second->sampleRate == rate && found->second->filter == filter) {
                history = found->second;
            } else {
                if (lastMidiHistoryId_ == std::numeric_limits<std::int64_t>::max())
                    throw std::overflow_error("MIDI history identifiers exhausted");
                history = std::make_shared<LiveMidiInput::History>(++lastMidiHistoryId_, track.id, rate, filter);
            }
            timing.midiHistories.push_back(history);
            histories.emplace(track.id, std::move(history));
        }
        const bool monitorMidi = midi && (track.monitorMode == 1 || (track.monitorMode == 2 && armed));
        if (!monitorMidi && !(track.kind == "audio" && armed))
            continue;
        const auto blocked = midi && track.monitorMode == 2 ? clips.monitorRanges(track.id)
                                                           : std::span<const MidiMonitorRange>{};
        const auto& recording = track.recording;
        const auto playGrid = midi && recording.playEnabled && validPlayRate
            ? trackRecordingGridTicks(recording.playGrid) * playQuantizePpq / textproj::kPPQ : 0;
        const auto playForgiveness = playGrid != 0 && std::isfinite(recording.playForgivenessMs)
            ? std::llround(std::clamp(recording.playForgivenessMs, 0., 1000.) * timing.sampleRate / 1000.) : 0;
        const Target target{track.id, blocked, filter, playGrid, playForgiveness};
        if (const auto output = timing.midiHardware.find(track.id); output != timing.midiHardware.end()) {
            auto& voices = output->second->liveVoices;
            if (!voices) voices = std::make_shared<LiveMidiInput::Voices>();
            LiveMidiInput::Route route;
            route.node = firstMidiEffect(track.id, output->second.get());
            route.voices = voices;
            route.processes = true;
            setTarget(route, target);
            timing.midiInputRoutes.push_back(std::move(route));
            continue;
        }
        for (const auto* node : chainFor(track.id)) {
            if (node->eventFlow() == EventFlow::Consume) {
                monitored.emplace(node, target);
                break;
            }
        }
    }
    for (const auto& entry : entries_) {
        if (entry.retired || entry.placeholder) {
            midiInputVoices_.erase(entry.hostIndex);
            continue;
        }
        auto& instance = devices_.deviceAt(entry.hostIndex);
        if (instance.eventFlow() != EventFlow::Consume) continue;
        auto& node = devices_.nodeAt(entry.hostIndex);
        const auto target = monitored.find(&node);
        // Keep a disarmed or parked instrument's state until it can render
        // its releases. A retired instance's last graph still owns its state.
        auto voices = midiInputVoices_.find(entry.hostIndex);
        if (target == monitored.end() && voices == midiInputVoices_.end()) continue;
        if (voices == midiInputVoices_.end())
            voices = midiInputVoices_.emplace(entry.hostIndex, std::make_shared<LiveMidiInput::Voices>()).first;
        LiveMidiInput::Route route;
        route.node = &node;
        route.voices = voices->second;
        route.processes = node.eventFlow() == EventFlow::Consume;
        if (target != monitored.end()) setTarget(route, target->second);
        timing.midiInputRoutes.push_back(std::move(route));
    }
    for (const auto& [track, output] : timing.midiHardware) {
        const auto* voice = output->liveVoices.get();
        if (!voice || std::any_of(timing.midiInputRoutes.begin(), timing.midiInputRoutes.end(),
                                  [voice](const auto& route) { return route.voices.get() == voice; })) continue;
        LiveMidiInput::Route route;
        route.node = firstMidiEffect(track, output.get());
        route.voices = output->liveVoices;
        route.processes = true;
        timing.midiInputRoutes.push_back(std::move(route));
    }
    if (std::any_of(timing.midiInputRoutes.begin(), timing.midiInputRoutes.end(),
                    [](const auto& route) { return route.playGrid != 0; })) {
        timing.playQuantizeRate = static_cast<std::uint32_t>(std::llround(timing.sampleRate));
        timing.playQuantizeTempo.push_back({0, timing.tempo.bpmAt(0)});
        for (const auto& point : timing.tempo.events) {
            const auto sample = timing.sampleForTick(point.posTicks);
            if (sample == timing.playQuantizeTempo.back().sample)
                timing.playQuantizeTempo.back().bpm = point.bpm;
            else
                timing.playQuantizeTempo.push_back({sample, point.bpm});
        }
    }
    midiHistories_.swap(histories);
    prepareMidiPreview(timing, model);
}

void Session::prepareMidiPreview(TimingView& timing, const rows::Model& model) {
    std::map<const Node*, std::int64_t> targets;
    for (const auto& track : model.tracks) {
        if ((track.kind != "midi" && track.kind != "instrument" && track.kind != "audio") ||
            track.frozen || frozenGroupChildren_.contains(track.id))
            continue;
        for (const auto* node : chainFor(track.id)) {
            if (node->eventFlow() != EventFlow::Consume)
                continue;
            targets.emplace(node, track.id);
            midiPreviewVoices_.try_emplace(node);
            break;
        }
    }
    timing.midiPreviewRoutes.reserve(midiPreviewVoices_.size());
    for (auto& [node, voice] : midiPreviewVoices_) {
        const auto target = targets.find(node);
        timing.midiPreviewRoutes.push_back({node, &voice, target == targets.end() ? 0 : target->second});
    }
}

const device::DeviceInstance* Session::resolvedAutomationDevice(
    std::int64_t id, const rows::Model& model, bool requireAudioEffect) const noexcept {
    const auto* entry = entryFor(id);
    if (!entry || entry->retired || entry->parked || entry->placeholder)
        return nullptr;

    // A preview can propose different rows with the same numeric device ID.
    const auto row = std::find_if(model.devices.begin(), model.devices.end(),
        [id](const auto& candidate) { return candidate.id == id; });
    if (row == model.devices.end() || !row->pluginRefId || row->pluginRefId != entry->resolvedPluginRef)
        return nullptr;
    const auto reference = std::find_if(model.pluginRefs.begin(), model.pluginRefs.end(),
        [&](const auto& candidate) { return candidate.id == *row->pluginRefId; });
    if (reference == model.pluginRefs.end() || reference->format != entry->resolvedFormat ||
        reference->uid != entry->resolvedUid)
        return nullptr;

    const auto& instance = devices_.deviceAt(entry->hostIndex);
    if (!instance.loaded())
        return nullptr;
    // Switching a note processor off also needs a protocol to release notes
    // it holds or generated. Until that exists, only audio effects can play
    // an Activator lane.
    if (requireAudioEffect && (reference->subtype != "effect" || instance.eventFlow() != EventFlow::Through))
        return nullptr;
    return &instance;
}

std::shared_ptr<DeviceAutomationBinding> Session::bindResolvedDeviceAutomation(
    std::shared_ptr<const AutomationProgram> program, const std::set<std::int64_t>& suspended,
    const rows::Model& model) {
    return bindDeviceAutomation(std::move(program), suspended, transport_, spec_.sampleRate,
        [&](std::int64_t id, std::uint32_t parameter) -> DeviceParameterSteps {
            const auto* instance = resolvedAutomationDevice(id, model);
            if (!instance) return {};
            const auto native = entryFor(id)->resolvedFormat == "clap" ? device::ClapDevice::paramIdToText(parameter)
                                                                     : std::to_string(parameter);
            const auto* descriptor = instance->findParam(native);
            if (!descriptor || descriptor->missing || !descriptor->automatable)
                return {};
            if (descriptor->shape == device::ParamShape::Switch)
                return {true, 2};
            const bool stepped = descriptor->shape == device::ParamShape::Menu || descriptor->stepCount > 1;
            return {stepped, descriptor->stepCount};
        }, [&](std::int64_t id) { return resolvedAutomationDevice(id, model, true) != nullptr; });
}

bool Session::rebuild() {
    if(const auto preview=rebuildGestureRuntime(gestureRuntime_))return *preview;
    error_.clear();
    if (!loaded_) {
        error_ = "no project loaded";
        return false;
    }
    (void)mainLoudness_.prepare(spec_.sampleRate,spec_.channels);
    auto timing = std::make_shared<TimingView>();
    timing->session=this;
    timing->gestureAudio=gestureAudio_;
    timing->gestureGeneration=gestureAudio_->reserveGeneration();
    const auto gestureGeneration=timing->gestureGeneration;
    timing->sampleRate = spec_.sampleRate;
    timing->spec = spec_;
    timing->settings=spec_.performanceState ? transportSettings_ : TransportSettings{};
    timing->inputSlice.resize(64);
    timing->outputSlice.resize(static_cast<std::size_t>(std::max(64, spec_.channels)));
    timing->outputSlice64.resize(timing->outputSlice.size());
    // Same construction as MidiClips: TempoMap integrates STEP tempos, even
    // when a stored event requests a ramp. Do not create a second converter.
    for (const auto& e : playbackModel_.tempo)
        timing->tempo.events.push_back({e.posTicks, e.bpm, static_cast<int>(e.curve)});
    if (timing->tempo.events.empty()) timing->tempo.events.push_back({0, 120.0, 0});
    const auto loopSample = [&](std::int64_t ticks) { return timing->sampleForTick(ticks); };
    timing->setBraceSamples(loopSample(transportSettings_.loopStartTicks), loopSample(transportSettings_.loopEndTicks));
    // Snapshot has tempo but no meter data. rows::readModel reads this vector
    // from time_signature_map, sorted by position and id.
    timing->signatures = model_.signatures;
    timing->prepareDeviceClockChanges();
    std::shared_ptr<ClipPlayback> nextClips;
    std::shared_ptr<MidiClips> nextMidi;
    try {
        nextClips = std::make_shared<ClipPlayback>(clipProject_, transport_, spec_.sampleRate,
                                                spec_.channels, spec_.maxFrames, spec_.progressiveDecoder,
                                                spec_.synchronousClipReads);
        prepareInputMonitors(*timing, model_, *nextClips);
    } catch (const std::exception& e) { error_ = e.what(); return false; }
    // ADR-0164: the automation this graph plays, bound to strips, less every
    // lane the user overrode. An override of a lane that is gone goes with it.
    const std::shared_ptr<const AutomationProgram> program =
        compileAutomation(playbackModel_, spec_.sampleRate);
    if (primeExternalAt_) {
        // Warp construction and automation compilation can take much longer
        // than a physical callback. Prime at a freshly published driver sample,
        // not the older packet boundary. Never copy/read the live Transport on
        // the owner. Only this unpublished candidate has a request producer.
        std::optional<DriverClock> origin;
        bool ready = false;
        const auto prime = [&](const Transport& requested, std::chrono::milliseconds timeout) {
            if (!timing->settings.hasLoop || !timing->settings.loopEnabled)
                return nextClips->primeAt(requested, timeout);
            Transport loopHead = requested;
            loopHead.locate(timing->loopBegin);
            // One request protects the union until the candidate is published;
            // two separate requests would let the worker evict the loop head.
            return nextClips->primeAt(requested, loopHead, timeout);
        };
        for (unsigned attempt = 0; attempt < 8 && !ready; ++attempt) {
            DriverClock before;
            if (!readDriverClock(before)) continue;
            if (!origin) origin = before;
            if (before.revision != origin->revision || before.playing != origin->playing) break;
            Transport requested;
            requested.locate(before.playing && primeExternalFollowDriver_ ? before.sample : loopSample(*primeExternalAt_));
            if (timing->settings.hasLoop)
                requested.loop(timing->loopBegin, timing->loopEnd, timing->settings.loopEnabled);
            requested.play();
            if (!prime(requested, std::chrono::milliseconds(100))) continue;
            DriverClock after;
            if (!readDriverClock(after)) continue;
            if (after.revision != origin->revision || after.playing != origin->playing) break;
            requested.locate(after.playing && primeExternalFollowDriver_ ? after.sample : loopSample(*primeExternalAt_));
            ready = prime(requested, std::chrono::milliseconds(0));
        }
        if (!ready) {
            error_ = "external tempo source pages could not catch the current driver position";
            return false;
        }
    } else {
        primeWhilePlaying(*nextClips, *timing);
    }
    if (externalPublishGuard_ && *externalPublishGuard_ && !(*externalPublishGuard_)()) {
        error_ = "external tempo preparation became stale";
        return false;
    }
    timing->sourceReadiness = nextClips->readiness();
    clips_ = std::move(nextClips);
    auto suspended = overridden_;
    suspended.insert(writingAutomation_.begin(), writingAutomation_.end());
    stripAutomation_ = bindStripAutomation(program, suspended);
    deviceAutomation_ = bindResolvedDeviceAutomation(program, suspended, model_);
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
    auto laneBindings = strips_.captureLanes();
    cueMonitor_.captureLanes(*laneBindings);
    timing->stripLanes = std::move(laneBindings);
    timing->generationControls=nativeGestureControls(*this);
    try {
        const auto* previousTiming =
            dynamic_cast<const TimingView*>(devices_.rebuildSpec().transportState.get());
        prepareMidiHardware(*timing, model_, previousTiming);
        const auto eventPath = eventPathFor(model_, *timing);
        nextMidi = std::make_shared<MidiClips>(playbackModel_, transport_, spec_.sampleRate, midiState_,
                                              eventPath, clipNoteExpressions(*clipProject_),
                                              spec_.performanceState && !spec_.exporting);
        midi_ = std::move(nextMidi);
    } catch (const std::exception& e) { error_ = e.what(); return false; }
    prepareMidiInput(*timing, model_, *midi_);
    attach(std::move(timing)); // publish the matching clock with this graph
    gestureAudio_->stageReset(gestureGeneration); // tagged, inert until this graph is adopted
    ++stats_.rebuilds;
    // MX-38: Hear Difference follows its device into the graph about to play,
    // and ends with it when the device is gone.
    if (hearDifference_ != 0) {
        // A removed device's entry is kept, retired, with its instance: gone all the same.
        const Entry* entry = entryFor(hearDifference_);
        const auto* listened = entry != nullptr && !entry->retired && !entry->parked ? nodeFor(hearDifference_) : nullptr;
        if (listened == nullptr) hearDifference_ = 0;
        graph_.setDifferenceNode(listened);
    }
    const bool ok = devices_.rebuildNow();
    if (!ok) error_ = devices_.lastRebuildError();
    else {
        live_ = true; // durable audio publication; later auxiliary work cannot undo it
        gestureAudio_->publishedGeneration(gestureGeneration);
        presentationPending_=!cueMonitor_.publish(cueSettings_,spec_.sampleRate,spec_.maxFrames,spec_.channels,spec_.performanceState,graph_.currentGraph());
    }
    try {
    attachTaps();   // ADR-0175: the new graph's latencies

    refreshProblems();
    } catch(const std::exception&) {
        // The graph may already be audible. Preserve that fact and let the
        // presentation owner retry; never report an untouched failed preview.
        if(!ok)return false;
        presentationPending_=true;
    }
    return ok;
}

void Session::refreshProblems() {
    problems_ = collectProblems(true);
}

std::vector<std::string> Session::exportBlockingProblems() {
    return collectProblems(false);
}

std::vector<std::string> Session::collectProblems(bool includeUnavailable) {
    auto problems = model_.problems;
    const auto append = [&](const std::vector<std::string>& found) {
        problems.insert(problems.end(), found.begin(), found.end());
    };
    if (midi_) append(midi_->problems());
    if (auto* c = clips()) {
        if (includeUnavailable) c->appendPlaybackProblems(problems);
        else c->appendExportBlockingProblems(problems);
    }
    append(sessionProblems_);
    if (includeUnavailable)
        for (const auto& row : model_.devices) {
            const auto* entry = entryFor(row.id);
            if (entry && entry->placeholder && !entry->retired)
                problems.push_back(deviceRef(row) + ": kept as a placeholder (ADR-0011): " + entry->error);
        }
    append(strips_.problems());
    append(summing_.problems());
    // The program's own findings only when there is automation to find them in:
    // a project with a tempo ramp and no lanes has no automation problem.
    if (stripAutomation_ && stripAutomation_->program && !model_.automationLanes.empty()) {
        append(stripAutomation_->program->problems());
        append(stripAutomation_->problems);
    }
    if (deviceAutomation_) append(deviceAutomation_->problems);
    append(deviceSourceProblems_);
    if (includeUnavailable) append(placeholderAutomationProblems_);
    append(graph_.problems());
    return problems;
}

bool Session::parkedDevice(const rows::Device &device) const {
    const auto *chain = chainOf(device.chainId);
    return chain && chain->trackId &&
           (frozenSources_.contains(*chain->trackId) || frozenGroupChildren_.contains(*chain->trackId));
}
std::vector<rows::TempoEvent> Session::playbackTempo(const rows::Model& model) const {
    if (performedTempo_.empty()) {
        if (spec_.externalTempoBpm) return {rows::TempoEvent{0, *spec_.externalTempoBpm, 0, 0}};
        return model.tempo;
    }
    auto result = performedTempo_;
    if (authoredResumeAt_) {
        const auto boundary = *authoredResumeAt_;
        std::erase_if(result, [boundary](const auto& event) { return event.posTicks >= boundary; });
        TempoMap authored;
        for (const auto& event : model.tempo)
            authored.events.push_back({event.posTicks, event.bpm, static_cast<int>(event.curve)});
        result.push_back({boundary, authored.bpmAt(boundary), 0, 0});
        for (const auto& event : model.tempo)
            if (event.posTicks > boundary) result.push_back(event);
        if (result.size() > 4096)
            throw std::runtime_error("performed tempo history exceeds 4096 boundaries");
    }
    return result;
}
bool Session::readPlayback(const Store &store) {
    try {
        auto& db = const_cast<SQLite::Database&>(store.db());
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        model_ = rows::readModel(store);
        transportSettings_ = readTransportSettings(store);
        cueSettings_ = readCueSettings(store);
        // A malformed/stale cache never falls back to a different live sound.
        freeze::validate(const_cast<Store &>(store));
        freeze::remember(const_cast<Store &>(store));
        const auto cached = freeze::renders(const_cast<SQLite::Database &>(store.db()));
        if((spec_.externalTempoBpm || !performedTempo_.empty()) && !cached.empty())
            throw std::runtime_error("unfreeze cached tracks before preparing an external tempo generation");
        std::set<std::int64_t> before, after;
        for (const auto &entry : frozenSources_)
            before.insert(entry.first);
        for (const auto &row : cached)
            after.insert(adi::detail::signedInteger(row.at("track_id")));
        if (loaded_ && playbackRunning_.load(std::memory_order_acquire) && before != after)
            throw std::runtime_error("stop playback before freezing or unfreezing");
        playbackModel_ = model_;
        playbackModel_.tempo = playbackTempo(model_);
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
        std::erase_if(auditionedTakeLanes_, [&](const auto& selection) {
            return frozenSources_.contains(selection.first) || frozenGroupChildren_.contains(selection.first) ||
                   std::none_of(model_.lanes.begin(), model_.lanes.end(), [&](const auto& lane) {
                return lane.trackId == selection.first && lane.id == selection.second &&
                       lane.kind == "take" && !lane.muted;
            });
        });
        playbackModel_.auditionedTakeLanes = auditionedTakeLanes_;
        clipProject_ = readClipProject(store, playbackModel_);
        if (read) read->commit();
        return true;
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
}

struct SessionEndpoint::KeyRoutes final : AppliedObserver {
    AppliedSubscription subscription;
    std::uint64_t applied = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> image;
    std::shared_ptr<const AppliedBatch> latest;
    void committed(std::shared_ptr<const AppliedBatch> batch) override {
        latest.reset();
        image.reset();
        if (batch->operations().empty()) return;
        for (const auto& op : batch->operations())
            if (op.dispatchedType != "routing.setKeyTap" && op.dispatchedType != "routing.connect" &&
                op.dispatchedType != "routing.disconnect") return;
        latest = std::move(batch);
    }
    void sourceInvalidated(const AppliedSource&, AppliedSourceInvalidation) override { latest.reset(); }
};

void Session::observeKeyRoutes() {
    auto routes = std::make_shared<SessionEndpoint::KeyRoutes>();
    auto observed = appliedSource_.observe(routes);
    routes->subscription = std::move(observed.subscription);
    endpoint_->keyRoutes_ = std::move(routes);
}

std::optional<bool> Session::refreshDeviceKeys(const Store& store, const std::shared_ptr<const std::vector<std::uint8_t>>& image) {
    const auto& receipts = endpoint_->keyRoutes_;
    if (!live_ || !receipts || !receipts->latest ||
        gestureHoldsExternalTempo(gestureRuntime_)) return {};
    const auto& batch = *receipts->latest;
    if (!batch.source().sameConnection(adi::appliedSource(store)) ||
        batch.sequence() != appliedSource_.sequence()) return {};
    if (receipts->applied == batch.sequence() && appliedSequence_ == receipts->applied) {
        if (image && receipts->image && (image == receipts->image || *image == *receipts->image)) return true;
        return {};
    }
    if (batch.sequence() != appliedSequence_ + 1) return {};
    auto next = rows::readModel(store);
    if (!next.problems.empty()) return {};
    const auto isKey = [](const rows::Routing& row) {
        return row.srcKind == "track" && row.dstKind == "device" && row.kind == "sidechain";
    };
    std::set<std::int64_t> recipients;
    for (const auto& op : batch.operations()) {
        const auto id = op.payload.at("id").get<std::int64_t>();
        const auto& routes = op.dispatchedType == "routing.disconnect" ? model_.routing : next.routing;
        const auto route = std::find_if(routes.begin(), routes.end(), [&](const auto& row) {
            return row.id == id && isKey(row);
        });
        if (route == routes.end()) return {};
        recipients.insert(route->dstId);
    }
    const auto needsKey = [&](const auto& routes, std::int64_t id) {
        return std::any_of(routes.begin(), routes.end(), [&](const auto& row) {
            return row.enabled && row.dstId == id && isKey(row);
        });
    };
    struct ChangedIntent {
        device::DeviceInstance* instance;
        bool previous;
    };
    std::vector<ChangedIntent> changedIntents;
    changedIntents.reserve(recipients.size());
    auto playbackRoutes = next.routing;
    std::vector<std::string> previousProblems;
    const auto diagnosticCount = sessionProblems_.size();
    bool stagedRoutes = false;
    const auto rollback = [&] {
        if (stagedRoutes) {
            model_.routing.swap(next.routing);
            playbackModel_.routing.swap(playbackRoutes);
            problems_.swap(previousProblems);
        }
        sessionProblems_.resize(diagnosticCount);
        for (auto change = changedIntents.rbegin(); change != changedIntents.rend(); ++change) {
            try { (void)change->instance->configureSidechain(change->previous); }
            catch (const std::exception&) { /* keep the original preparation error */ }
        }
    };

    bool shapeChanged = false;
    try {
        for (const auto id : recipients) {
            const auto before = needsKey(model_.routing, id);
            const auto after = needsKey(next.routing, id);
            if (before == after) continue;
            const auto* entry = entryFor(id);
            if (!entry || entry->retired || entry->parked || entry->placeholder) continue;
            auto* instance = instanceFor(id);
            if (!instance || !instance->loaded()) continue;
            const auto shape = instance->preparationShapeEpoch();
            const auto width = instance->sidechainInputChannels();
            changedIntents.push_back({instance, before});
            if (!instance->configureSidechain(after)) {
                const auto problem = "devices#" + std::to_string(id) + ": key input is unavailable";
                if (std::find(sessionProblems_.begin(), sessionProblems_.end(), problem) == sessionProblems_.end())
                    sessionProblems_.push_back(problem);
            }
            shapeChanged = shapeChanged || shape != instance->preparationShapeEpoch() ||
                           width != instance->sidechainInputChannels();
        }

        if (shapeChanged) {
            // The adapter cannot activate a dormant bus in a preserved graph.
            // The full path prepares it behind its existing reactivation gate.
            // Keep the authored receipt pending if preparation refuses.
            model_.routing.swap(next.routing);
            playbackModel_.routing.swap(playbackRoutes);
            problems_.swap(previousProblems);
            stagedRoutes = true;
            // As on an ordinary refresh, release a completed preview's
            // reader selection before building a new timing generation.
            if (gestureClips(gestureRuntime_))
                (void)refreshGestureRuntime(gestureRuntime_, store);
            if (!rebuild()) {
                rollback();
                return false;
            }
        } else {
            // Source and tap changes against an already prepared shape keep
            // the current readers, timing view and device DSP running.
            if (!devices_.publishDeviceKeys(next, [this] { return &model_; })) {
                error_ = devices_.lastRebuildError();
                rollback();
                return false;
            }
            model_.routing.swap(next.routing);
            playbackModel_.routing.swap(playbackRoutes);
            ++stats_.rebuilds;
            try {
                presentationPending_ = !cueMonitor_.publish(cueSettings_, spec_.sampleRate, spec_.maxFrames,
                                                            spec_.channels, spec_.performanceState, graph_.currentGraph());
                attachTaps();
                refreshProblems();
            } catch (const std::exception&) {
                presentationPending_ = true;
            }
        }
    } catch (const std::exception& failure) {
        rollback();
        error_ = failure.what();
        return false;
    }

    appliedSequence_ = receipts->applied = batch.sequence();
    receipts->image = image;
    error_.clear();
    return true;
}

bool Session::load(const Store& store, DeviceLoader loader, SessionSpec spec) {
    if (!prepareLoad(store, std::move(loader), spec, false)) return false;
    while (pendingLoad_->next < pendingLoad_->deviceRows.size())
        if (!loadNextDevice(store)) return false;
    return finishLoad(store);
}

bool Session::beginLoad(const Store& store, DeviceLoader loader, SessionSpec spec) {
    return prepareLoad(store, std::move(loader), spec, true);
}

bool Session::prepareLoad(const Store& store, DeviceLoader loader, SessionSpec spec, bool staged) {
    if(audio::onAudioThread)return false;
    error_.clear();
    if (loaded_ || pendingLoad_) {
        // One session, one project. A second project is a second session --
        // the instances this one holds are in a graph that may be rendering.
        error_ = "a project is already loaded in this session";
        return false;
    }
    if(spec.externalTempoBpm && (!spec.performanceState || !std::isfinite(*spec.externalTempoBpm) ||
       *spec.externalTempoBpm<20 || *spec.externalTempoBpm>999)) {
        error_="external tempo needs a performance generation and finite 20..999 BPM";
        return false;
    }
    loader_ = std::move(loader);
    appliedSource_ = adi::appliedSource(store);
    const auto sequence = appliedSource_.sequence();
    // A staged open may yield to another owner turn. Synchronous load uses
    // the coherent playback image read in readPlayback, including a caller's
    // pinned transaction when present.
    const auto revision = staged ? gesture::detail::storeWriteStamp(store) : std::nullopt;
    if (staged && !revision) {
        error_ = "cannot capture the project revision for opening";
        return false;
    }
    // Offline rendering loads a coherent read-only project. It retains its
    // source identity without entering the editable gesture domain.
    if(!store.readOnly()) {
        observeKeyRoutes();
        const auto registered=gesture::detail::registerSessionEndpoint(appliedSource_,endpoint_);
        if(!registered.ok()) {error_=registered.message;return false;}
    }
    spec_ = spec;
    graph_.setPrecision(spec_.precision);   // ADR-0207 d2
    graph_.setDelayCompensation(spec_.delayCompensation);
    stats_ = Stats{};
    recordingProject_ = recordingIdentity(store);
    if (!readPlayback(store))
        return false;
    if (revision && (gesture::detail::storeWriteStamp(store) != revision || appliedSource_.sequence() != sequence)) {
        error_ = "the project changed while it was opening";
        return false;
    }
    auto pending = std::make_unique<PendingLoad>();
    pending->store = &store;
    pending->owner = std::this_thread::get_id();
    pending->stamp = revision;
    pending->sequence = sequence;
    pending->deviceRows.reserve(model_.devices.size());
    for (std::size_t index = 0; index < model_.devices.size(); ++index)
        if (!parkedDevice(model_.devices[index])) pending->deviceRows.push_back(index);
    pendingLoad_ = std::move(pending);
    return true;
}

Session::LoadProgress Session::loadProgress() const noexcept {
    if (!pendingLoad_) return {};
    const auto& pending = *pendingLoad_;
    LoadProgress result{pending.next, pending.deviceRows.size(), {}};
    if (pending.next < pending.deviceRows.size()) {
        const auto& row = model_.devices[pending.deviceRows[pending.next]];
        result.nextDevice = row.name;
        if (result.nextDevice.empty()) {
            if (const auto* ref = refFor(row)) result.nextDevice = ref->name;
        }
    }
    return result;
}

bool Session::checkLoadRevision(const Store& store) {
    if (audio::onAudioThread) return false;
    if (!pendingLoad_) {
        error_ = "no project opening is in progress";
        return false;
    }
    auto& pending = *pendingLoad_;
    if (pending.owner != std::this_thread::get_id() || pending.failed) return false;
    if (pending.store != &store || !appliedSource_.sameConnection(adi::appliedSource(store)) ||
        (pending.stamp && (appliedSource_.sequence() != pending.sequence ||
                           gesture::detail::storeWriteStamp(store) != pending.stamp))) {
        pending.failed = true;
        error_ = "the project changed while it was opening";
        return false;
    }
    return true;
}

bool Session::loadNextDevice(const Store& store) {
    if (!checkLoadRevision(store)) return false;
    auto& pending = *pendingLoad_;
    if (pending.next == pending.deviceRows.size()) return true;
    try {
        // SDK construction and state restoration stay on the owner. Leave
        // no SQLite transaction open across a yield between devices.
        resolveOne(store, model_.devices[pending.deviceRows[pending.next]]);
        if (!checkLoadRevision(store)) return false;
        ++pending.next;
        error_.clear();
        return true;
    } catch (const std::exception& exception) {
        pending.failed = true;
        error_ = exception.what();
        return false;
    }
}

bool Session::finishLoad(const Store& store) {
    if (!checkLoadRevision(store)) return false;
    auto& pending = *pendingLoad_;
    if (pending.next != pending.deviceRows.size()) {
        error_ = "project devices are still opening";
        return false;
    }
    try {
        // Initial device preparation uses the saved sidechain/routing flags.
        syncFlags();
        loaded_ = true;
        if (!rebuild()) {
            pending.failed = true;
            return false;
        }
        if (!checkLoadRevision(store)) {
            loaded_ = false;
            return false;
        }
        appliedSequence_ = pending.stamp ? pending.sequence : appliedSource_.sequence();
        pendingLoad_.reset();
        return true;
    } catch (const std::exception& exception) {
        pending.failed = true;
        error_ = exception.what();
        return false;
    }
}

std::optional<std::string> automationParamRef(const std::string& format, const std::string& id) {
    if (format != "clap") return id;
    std::uint32_t numeric = 0;
    if (!device::ClapDevice::paramIdFromText(id,numeric)) return {};
    return std::to_string(numeric); // ADR-0165: lane refs stay decimal for every format
}

namespace {
// What a user can move on a strip, per track: what an override compares.
struct StripValues {
    double volume = 0.0, pan = 0.0, panLeft = -1.0, panRight = 1.0;
    bool muted = false;
};
bool knownNonAutomatable(const device::DeviceInstance* instance,const std::string& parameter,bool laneReference) {
    if (!instance || !instance->loaded()) return false;
    for (std::int32_t i=0;i<instance->paramCount();++i) {
        const auto* descriptor=instance->paramAt(i);
        if (!descriptor || descriptor->missing) continue;
        const auto reference=laneReference ? automationParamRef(instance->identity().format,descriptor->id)
                                           : std::optional<std::string>{descriptor->id};
        if (reference && *reference==parameter) return !descriptor->automatable;
    }
    // Unknown and temporarily missing declarations keep their existing waiting
    // semantics. Only a live, explicit non-automatable declaration rejects it.
    return false;
}
std::map<std::pair<std::int64_t, std::string>, double> paramValues(const rows::Model& m) {
    std::map<std::pair<std::int64_t, std::string>, double> v;
    std::set<std::int64_t> clapRefs, clapDevices;
    for (const auto& reference : m.pluginRefs) if (reference.format == "clap") clapRefs.insert(reference.id);
    for (const auto& item : m.devices) {
        if (item.pluginRefId && clapRefs.contains(*item.pluginRefId)) clapDevices.insert(item.id);
        v[{item.id, std::string(kDeviceActivatorParamRef)}] = item.enabled ? 1.0 : 0.0;
    }
    for (const rows::PluginParam& p : m.pluginParams) {
        const auto ref = automationParamRef(clapDevices.contains(p.deviceId) ? "clap" : "",p.paramId);
        if (ref && *ref != kDeviceActivatorParamRef) v[{p.deviceId,*ref}] = p.normalized;
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

std::optional<bool> Session::refreshLocalEdit(const Store& store, const ProjectView& view) {
    using Kind = ProjectView::LocalEdit::Kind;
    const auto& delta = view.localEdit();
    const auto source = adi::appliedSource(store);
    if (!delta || !live_ || !appliedSource_.sameConnection(source) || !view.storeCurrent(store) ||
        gestureHoldsExternalTempo(gestureRuntime_) || !model_.problems.empty()) return {};
    if (delta->kind == Kind::TrackName) {
        const auto byId = [&](const rows::Track& track) { return track.id == delta->id; };
        const auto authored = std::find_if(model_.tracks.begin(), model_.tracks.end(), byId);
        const auto playback = std::find_if(playbackModel_.tracks.begin(), playbackModel_.tracks.end(), byId);
        if (authored == model_.tracks.end() || playback == playbackModel_.tracks.end()) return {};
        auto name = delta->name;
        auto playbackName = delta->name;
        authored->name.swap(name);
        playback->name.swap(playbackName);
        appliedSequence_ = source.sequence();
        error_.clear();
        return true;
    }
    if (delta->kind == Kind::Transport || !midi_ || !clipProject_ || gestureClips(gestureRuntime_) ||
        !frozenSources_.empty() || !frozenGroupChildren_.empty()) return {};
    const auto timing = std::dynamic_pointer_cast<const TimingView>(devices_.rebuildSpec().transportState);
    if (!timing) return {};
    if (!playbackModel_.controllers.empty() &&
        std::any_of(timing->midiInputRoutes.begin(), timing->midiInputRoutes.end(),
                    [](const auto& route) { return route.authoredPitchChase != nullptr; })) {
        // With controller material the MIDI compiler replaces every source.
        // An Auto route in this timing view still points at its old source's pitch chase.
        return {};
    }
    const auto byId = [&](const rows::Clip& clip) { return clip.id == delta->id && clip.kind == "midi"; };
    const auto authored = std::find_if(model_.clips.begin(), model_.clips.end(), byId);
    if (authored == model_.clips.end()) return {};
    if (delta->kind == Kind::MidiPlacement && spec_.performanceState && !spec_.exporting &&
        std::any_of(model_.tracks.begin(), model_.tracks.end(), [&](const rows::Track& track) {
            return (track.id == delta->fromTrack || track.id == delta->toTrack) && recordArmed(track) &&
                   track.monitorMode == 2 && (track.kind == "midi" || track.kind == "instrument");
        })) {
        // Auto monitoring copies the Arrangement ranges into the audio
        // timing view. A move needs those bindings rebuilt with the sources.
        return {};
    }
    try {
        auto candidate = playbackModel_;
        const auto clip = std::find_if(candidate.clips.begin(), candidate.clips.end(), byId);
        if (clip == candidate.clips.end()) return {};
        std::set<std::int64_t> affected{clip->trackId};
        std::vector<rows::Note> authoredNotes;
        if (delta->kind == Kind::MidiPlacement) {
            // A clip-local automation stream moves with the clip. Its clock
            // changes the existing full automation generation as well.
            if (authored->trackId != delta->fromTrack || clip->trackId != delta->fromTrack ||
                std::any_of(model_.automationData.begin(), model_.automationData.end(),
                            [&](const auto& data) { return data.clipId == delta->id; })) return {};
            clip->trackId = delta->toTrack;
            clip->posTicks = delta->position;
            affected.insert(delta->toTrack);
        } else {
            rows::Model material;
            rows::readClipNotes(material, store, delta->id);
            if (material.notes.empty() || !material.problems.empty()) return {};
            const auto replaceNotes = [&](std::vector<rows::Note>& notes) {
                const auto byClip = [](const rows::Note& note, std::int64_t id) { return note.clipId < id; };
                const auto first = std::lower_bound(notes.begin(), notes.end(), delta->id, byClip);
                const auto last = std::find_if(first, notes.end(), [&](const auto& note) { return note.clipId != delta->id; });
                notes.insert(notes.erase(first, last), material.notes.begin(), material.notes.end());
            };
            const auto byClipOrder = [](const rows::Note& a, const rows::Note& b) { return a.clipId < b.clipId; };
            if (!std::is_sorted(candidate.notes.begin(), candidate.notes.end(), byClipOrder) ||
                !std::is_sorted(model_.notes.begin(), model_.notes.end(), byClipOrder)) return {};
            replaceNotes(candidate.notes);
            authoredNotes = candidate.notes;
        }
        auto midi = std::make_shared<MidiClips>(candidate, transport_, spec_.sampleRate, midiState_,
            eventPathFor(model_, *timing), clipNoteExpressions(*clipProject_), midi_.get(), affected);
        if (!view.storeCurrent(store)) return {};
        if (!devices_.publishSources(candidate, playbackSources(clips_.get(), midi.get(), timing))) return {};
        // The graph now owns the affected MIDI sources and the old timing.
        // No subsequent housekeeping failure can roll back that publication.
        midi_ = std::move(midi);
        if (delta->kind == Kind::MidiPlacement) {
            authored->trackId = delta->toTrack;
            authored->posTicks = delta->position;
        } else model_.notes.swap(authoredNotes);
        playbackModel_ = std::move(candidate);
        appliedSequence_ = source.sequence();
        ++stats_.rebuilds;
        error_.clear();
    } catch (const std::exception&) { return {}; }
    try {
        presentationPending_ = !cueMonitor_.publish(cueSettings_, spec_.sampleRate, spec_.maxFrames, spec_.channels,
                                                    spec_.performanceState, graph_.currentGraph());
        attachTaps();
        refreshProblems();
    } catch (const std::exception&) { presentationPending_ = true; }
    return true;
}

bool Session::refresh(const Store& store) {
    if(const auto handled=refreshGestureRuntime(gestureRuntime_,store))return *handled;
    error_.clear();
    if (!loaded_) {
        error_ = "no project loaded";
        return false;
    }
    const auto source = adi::appliedSource(store);
    const bool replaced = !appliedSource_.sameConnection(source);
    if (replaced) appliedSequence_ = 0;
    appliedSource_ = source;
    // Save As switched the store to its new file: a take recorded from now on is stamped with it, or
    // it would be refused as another project's when it lands (walk-A A18). A capture running at the
    // switch kept its stamp and has landed in the old file first (PD06-SAVEAS-GUARD).
    if (replaced) recordingProject_ = recordingIdentity(store);
    if (replaced && !store.readOnly()) observeKeyRoutes();
    if(!store.readOnly() && !isGestureCommittedRefresh(gestureRuntime_)) {
        const auto registered=gesture::detail::registerSessionEndpoint(appliedSource_,endpoint_);
        if(!registered.ok()) {error_=registered.message;return false;}
    }
    const std::map<std::int64_t, StripValues> before = stripValues(model_);
    const std::map<std::pair<std::int64_t, std::string>, double> paramsBefore = paramValues(model_);
    std::map<std::int64_t, double> sendsBefore;
    for (const auto& route : model_.routing) if (route.kind == "send") sendsBefore[route.id] = route.gainDb;
    if (!readPlayback(store))
        return false;
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
            if (key.second == kDeviceActivatorParamRef) {
                if (!deviceActivatorAvailable(key.first)) continue;
            } else if (knownNonAutomatable(instanceFor(key.first),key.second,true)) continue;
            const auto b = paramsBefore.find(key);
            const auto a = paramsAfter.find(key);
            const bool had = b != paramsBefore.end(), has = a != paramsAfter.end();
            if (had != has || (had && b->second != a->second)) overridden_.insert(laneId);
        }
    }
    resolveNewRows(store);
    retireDepartedRows();
    syncFlags();
    const bool rebuilt = rebuild();
    if (rebuilt) appliedSequence_ = appliedSource_.sequence();
    return rebuilt;
}

bool Session::auditionTakeLane(const Store& store, std::int64_t track, std::int64_t lane) {
    if (audio::onAudioThread)
        return false;
    if (!loaded_ || gestureHoldsExternalTempo(gestureRuntime_)) {
        error_ = "take audition needs an open session with no active gesture";
        return false;
    }
    if (lane != 0 && (frozenSources_.contains(track) || frozenGroupChildren_.contains(track) ||
        std::none_of(model_.lanes.begin(), model_.lanes.end(), [track, lane](const auto& candidate) {
            return candidate.trackId == track && candidate.id == lane && candidate.kind == "take" && !candidate.muted;
        }))) {
        error_ = "that take lane cannot be auditioned";
        return false;
    }
    const auto previous = auditionedTakeLanes_.find(track);
    const auto prior = previous != auditionedTakeLanes_.end() ? previous->second : 0;
    if (prior == lane)
        return true;
    if (lane != 0)
        auditionedTakeLanes_[track] = lane;
    else
        auditionedTakeLanes_.erase(track);
    if (refresh(store))
        return true;
    if (prior != 0)
        auditionedTakeLanes_[track] = prior;
    else
        auditionedTakeLanes_.erase(track);
    playbackModel_.auditionedTakeLanes = auditionedTakeLanes_;
    return false;
}

bool Session::setExternalTempo(const Store& store, std::optional<double> bpm,
                               const std::function<bool()>& publishGuard, std::int64_t primeTick) {
    if (audio::onAudioThread) return false;
    if (!loaded_ || !spec_.performanceState || primeTick < 0 ||
        (bpm && (!std::isfinite(*bpm) || *bpm < 20 || *bpm > 999))) {
        error_ = "external tempo needs a loaded performance session and finite 20..999 BPM";
        return false;
    }
    if (gestureHoldsExternalTempo(gestureRuntime_)) {
        error_ = "external tempo waits for the active gesture";
        return false;
    }
    auto previousPlayback = playbackModel_;
    auto previousProject = clipProject_;
    const auto previousBpm = spec_.externalTempoBpm;
    auto previousHistory = performedTempo_;
    const auto previousResume = authoredResumeAt_;
    spec_.externalTempoBpm = bpm;
    performedTempo_.clear();
    authoredResumeAt_.reset();
    primeExternalAt_ = primeTick;
    primeExternalFollowDriver_ = false;
    externalPublishGuard_ = &publishGuard;
    bool ok = false;
    try { ok = refresh(store); }
    catch (const std::exception& failure) { error_ = failure.what(); }
    catch (...) { error_ = "external tempo preparation failed"; }
    externalPublishGuard_ = nullptr;
    primeExternalAt_.reset();
    if (!ok) {
        playbackModel_ = std::move(previousPlayback);
        clipProject_ = std::move(previousProject);
        spec_.externalTempoBpm = previousBpm;
        performedTempo_ = std::move(previousHistory);
        authoredResumeAt_ = previousResume;
    }
    return ok;
}

bool Session::setExternalTempoAt(const Store& store, std::optional<double> bpm, std::int64_t tick,
                                 const std::function<bool()>& publishGuard) {
    if (audio::onAudioThread) return false;
    if (!loaded_ || !spec_.performanceState || tick < 0 ||
        (bpm && (!std::isfinite(*bpm) || *bpm < 20 || *bpm > 999))) {
        error_ = "performed tempo needs a loaded performance session, nonnegative tick and finite 20..999 BPM";
        return false;
    }
    if (gestureHoldsExternalTempo(gestureRuntime_)) {
        error_ = "external tempo waits for the active gesture";
        return false;
    }
    std::vector<rows::TempoEvent> candidate;
    for (const auto& event : playbackModel_.tempo)
        if (event.posTicks < tick) candidate.push_back(event);
    if (candidate.empty() && tick > 0) candidate.push_back({0, 120, 0, 0});
    if (bpm) candidate.push_back({tick, *bpm, 0, 0});
    else {
        TempoMap authored;
        for (const auto& event : model_.tempo)
            authored.events.push_back({event.posTicks, event.bpm, static_cast<int>(event.curve)});
        candidate.push_back({tick, authored.bpmAt(tick), 0, 0});
        for (const auto& event : model_.tempo)
            if (event.posTicks > tick) candidate.push_back(event);
    }
    if (candidate.size() > 4096) {
        error_ = "performed tempo history exceeds 4096 boundaries";
        return false;
    }
    auto previousPlayback = playbackModel_;
    auto previousProject = clipProject_;
    const auto previousBpm = spec_.externalTempoBpm;
    auto previousHistory = std::move(performedTempo_);
    const auto previousResume = authoredResumeAt_;
    spec_.externalTempoBpm = bpm;
    performedTempo_ = std::move(candidate);
    authoredResumeAt_ = bpm ? std::nullopt : std::optional<std::int64_t>{tick};
    primeExternalAt_ = tick;
    primeExternalFollowDriver_ = true;
    externalPublishGuard_ = &publishGuard;
    bool ok = false;
    try { ok = refresh(store); }
    catch (const std::exception& failure) { error_ = failure.what(); }
    catch (...) { error_ = "external tempo preparation failed"; }
    externalPublishGuard_ = nullptr;
    primeExternalAt_.reset();
    if (!ok) {
        playbackModel_ = std::move(previousPlayback);
        clipProject_ = std::move(previousProject);
        spec_.externalTempoBpm = previousBpm;
        performedTempo_ = std::move(previousHistory);
        authoredResumeAt_ = previousResume;
    }
    return ok;
}

void Session::setSourcesFor(SourceFn fn) { sources_ = std::move(fn); }

bool Session::setAudioMonitorInputs(std::span<const RecordingTarget> targets) {
    if (!loaded_) {
        error_ = "audio monitoring needs a loaded project";
        return false;
    }
    decltype(monitorInputs_) selected;
    for (const auto& source : targets) {
        const auto track = std::find_if(model_.tracks.begin(), model_.tracks.end(),
            [&](const rows::Track& t) { return t.id == source.trackId && t.kind == "audio"; });
        const auto& channels = source.channels;
        if (track == model_.tracks.end() || channels.empty() || channels.size() > 2 ||
            channels.front() >= 64 || channels.back() >= 64 ||
            (channels.size() == 2 && channels.front() == channels.back())) {
            error_ = "audio monitoring needs an audio track and one input channel or a distinct pair";
            return false;
        }
        if (!selected.emplace(source.trackId, std::array{static_cast<std::int32_t>(channels.front()),
                                                       static_cast<std::int32_t>(channels.back())}).second) {
            error_ = "duplicate audio monitor input for the same track";
            return false;
        }
    }
    if (selected == monitorInputs_) {
        error_.clear();
        return true;
    }
    monitorInputs_.swap(selected);
    if (rebuild()) return true;
    const auto failure = error_;
    monitorInputs_.swap(selected);
    (void)rebuild();
    error_ = failure;
    return false;
}

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
    if (strips_.stripFor(trackId) == nullptr || !std::isfinite(seconds) || seconds <= 0 ||
        point == ScopePoint::DeviceInput || point == ScopePoint::DeviceOutput) return {};
    const ScopeSource source{trackId, point, 0};
    // Preserve the legacy idempotent open contract: repeat calls keep the first
    // demand, even if a caller omits its earlier custom history argument.
    scopeRuntime_->legacy.try_emplace(source, seconds);
    attachTaps();
    const auto it = scopeRuntime_->bySource.find(source);
    return it != scopeRuntime_->bySource.end() ? it->second : nullptr;
}

void Session::closeScope(std::int64_t trackId, ScopePoint point) {
    if (scopeRuntime_->legacy.erase({trackId, point, 0}) != 0) attachTaps();
}

ScopeLease Session::requestScope(const ScopeRequest& request) {
    if (request.source.track <= 0 || !std::isfinite(request.seconds) || request.seconds <= 0 ||
        !std::isfinite(request.seconds * spec_.sampleRate) ||
        request.seconds * spec_.sampleRate > static_cast<double>(std::int64_t{1} << 24)) return {};
    const bool device = request.source.point == ScopePoint::DeviceInput ||
                        request.source.point == ScopePoint::DeviceOutput;
    if (device != (request.source.device > 0)) return {};
    auto state = std::make_shared<ScopeLease::State>();
    const auto id = ++scopeRuntime_->next;
    state->release = [this, id] {
        scopeRuntime_->requests.erase(id);
        attachTaps();
    };
    scopeRuntime_->requests.emplace(id, ScopeRuntime::Request{request.source, request.seconds, state});
    try { attachTaps(); }
    catch (...) { scopeRuntime_->requests.erase(id); throw; }
    return ScopeLease(std::move(state));
}

std::size_t Session::collectScopeTaps() {
    if (scopeRuntime_->dirty) attachTaps();
    return scopeRuntime_->collect();
}
Session::ScopeStats Session::scopeStats() const {
    return {scopeRuntime_->requests.size() + scopeRuntime_->legacy.size(),
            scopeRuntime_->groups.size(), scopeRuntime_->publisher.retainedCount(),
            scopeRuntime_->deferred ? std::size_t{1} : std::size_t{0}, scopeRuntime_->awaitingAudio()};
}
void Session::beginScopeRead() noexcept { scopeRuntime_->adopt(); }

void Session::attachTaps() {
    scopeRuntime_->dirty = true;
    struct Demand {
        std::shared_ptr<Node::ScopePorts> ports;
        std::shared_ptr<const void> identity;
        double seconds = 0;
        std::int32_t latency = 0;
    };
    std::map<ScopeRuntime::PortKey, Demand> desired;
    std::map<ScopeSource, ScopeRuntime::PortKey> resolved;
    Graph* graph = graph_.currentGraph();
    const auto add = [&](const ScopeSource& source, double seconds) {
        auto* strip = strips_.stripFor(source.track);
        if (!strip || !graph) return;
        Node* node = strip;
        bool output = source.point == ScopePoint::PostFader;
        std::shared_ptr<const void> identity;
        if (source.point == ScopePoint::DeviceInput || source.point == ScopePoint::DeviceOutput) {
            const auto* entry = entryFor(source.device);
            if (!entry || entry->retired || entry->parked || entry->placeholder ||
                entry->trackId != source.track || !entry->runtimeIdentity) return;
            node = &devices_.nodeAt(entry->hostIndex);
            identity = entry->runtimeIdentity;
            output = source.point == ScopePoint::DeviceOutput;
        } else if (source.point == ScopePoint::ChainInput) {
            const auto chain = chainFor(source.track);
            std::size_t first = 0;
            for (std::size_t i = 0; i < chain.size(); ++i)
                if (chain[i]->eventFlow() == EventFlow::Consume) first = i + 1;
            if (first < chain.size()) node = chain[first];
            const auto gain = inputGains_.find(source.track);
            if (gain != inputGains_.end()) node = gain->second.get();
            if (frozenSources_.contains(source.track)) {
                const auto cached = trackChain(source.track);
                if (!cached.empty()) node = cached.front();
            }
        }
        const auto nodeId = graph->find(*node);
        if (nodeId == kInvalidNode) return;
        const auto ports = node->scopePorts();
        // Port identity distinguishes a replaced Node under the same authored ID;
        // aliases such as ChainInput/first DeviceInput must share one writer. The
        // format epoch distinguishes newly prepared rates, precision and runtime.
        ScopeRuntime::PortKey key{ports.get(), output, runtimeEpoch_};
        auto& demand = desired[key];
        demand.ports = ports;
        demand.identity = std::move(identity);
        demand.seconds = (std::max)(demand.seconds, seconds);
        const auto arrival = static_cast<std::int64_t>(graph->arrivalOf(nodeId));
        const auto own = graph->outputLatencyOf(nodeId).value_or(arrival) - arrival;
        const auto latency = (spec_.delayCompensation ? arrival : 0) + (output ? own : 0);
        demand.latency = static_cast<std::int32_t>(std::clamp(latency,
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
        resolved[source] = key;
    };
    for (const auto& [source, seconds] : scopeRuntime_->legacy) add(source, seconds);
    for (const auto& [id, request] : scopeRuntime_->requests) { (void)id; add(request.source, request.seconds); }

    std::map<ScopeRuntime::PortKey, ScopeRuntime::Binding> groups;
    std::map<ScopeSource, std::shared_ptr<ScopeTap>> bySource;
    auto plan = std::make_unique<ScopeRuntime::Bindings>();
    plan->items.reserve(desired.size());
    bool changed = desired.size() != scopeRuntime_->groups.size();
    for (const auto& [key, demand] : desired) {
        std::shared_ptr<ScopeTap> tap;
        const auto previous = scopeRuntime_->groups.find(key);
        const auto frames = std::ceil(spec_.sampleRate * demand.seconds);
        if (previous != scopeRuntime_->groups.end() &&
            previous->second.tap->sampleRate() == spec_.sampleRate &&
            static_cast<double>(previous->second.tap->capacity()) >= frames)
            tap = previous->second.tap;
        else { tap = std::make_shared<ScopeTap>(spec_.sampleRate, demand.seconds); changed = true; }
        tap->setLatency(demand.latency); // compatibility diagnostic; writes use Graph's cached values
        ScopeRuntime::Binding binding{key, demand.ports, demand.identity, tap};
        groups.emplace(key, binding);
        plan->items.push_back(std::move(binding));
    }
    for (const auto& [source, key] : resolved) bySource.emplace(source, groups.at(key).tap);
    // Acquire every potentially throwing lease lock before accepting a plan.
    // A requestScope rollback must not leave its failed request in a published
    // or deferred plan because a later lease-state lock failed.
    std::vector<std::unique_lock<std::mutex>> leaseLocks;
    leaseLocks.reserve(scopeRuntime_->requests.size());
    for (const auto& [id, request] : scopeRuntime_->requests) {
        (void)id;
        leaseLocks.emplace_back(request.state->mutex);
    }
    if (changed) scopeRuntime_->publishOrDefer(plan);
    scopeRuntime_->groups.swap(groups);
    scopeRuntime_->bySource.swap(bySource);
    for (const auto& [id, request] : scopeRuntime_->requests) {
        (void)id;
        const auto it = scopeRuntime_->bySource.find(request.source);
        request.state->current = it != scopeRuntime_->bySource.end() ? it->second : nullptr;
    }
    scopeRuntime_->dirty = false;
}

void Session::bindDeviceSources() {
    deviceSourceProblems_.clear();
    placeholderAutomationProblems_.clear();
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
                placeholderAutomationProblems_.push_back("devices#" + std::to_string(e.deviceId) +
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

bool Session::deviceActivatorAvailable(std::int64_t deviceId) const noexcept {
    return resolvedAutomationDevice(deviceId, model_, true) != nullptr;
}

std::optional<bool> Session::deviceActivatorPlaying(std::int64_t deviceId) const noexcept {
    if (!deviceAutomation_) return std::nullopt;
    const auto source = deviceAutomation_->byDevice.find(deviceId);
    return source == deviceAutomation_->byDevice.end() ? std::nullopt : source->second->playingEnabled();
}

bool Session::overrideDeviceActivator(std::int64_t deviceId) {
    if (!deviceAutomation_ || !deviceActivatorAvailable(deviceId)) return true;
    const auto lane = deviceAutomation_->laneFor.find({deviceId, std::string(kDeviceActivatorParamRef)});
    if (lane == deviceAutomation_->laneFor.end() || overridden_.contains(lane->second) ||
        writingAutomation_.contains(lane->second)) return true;
    overridden_.insert(lane->second);
    return rebuild();
}

bool Session::overrideDeviceAutomation(std::int64_t deviceId, const std::string& paramId) {
    if (!deviceAutomation_) return true;
    const auto* instance = instanceFor(deviceId);
    if (knownNonAutomatable(instance,paramId,false)) return true;
    const auto ref = automationParamRef(instance ? instance->identity().format : "",paramId);
    if (!ref || *ref == kDeviceActivatorParamRef) return true;
    const auto lane = deviceAutomation_->laneFor.find({deviceId,*ref});
    if (lane == deviceAutomation_->laneFor.end() || overridden_.contains(lane->second) ||
        writingAutomation_.contains(lane->second)) return true;
    overridden_.insert(lane->second);
    return rebuild();
}

bool Session::tick(std::int64_t nowMs) {
    if(presentationPending_ && cueMonitor_.publish(cueSettings_,spec_.sampleRate,spec_.maxFrames,spec_.channels,spec_.performanceState,graph_.currentGraph())) {
        presentationPending_=false;error_.clear();
    }
    collectScopeTaps();
    if(gestureAudio_)gestureAudio_->collect();
    mainLoudness_.collect();
    cueMonitor_.collect();
    const auto clients=runtimeClients_;
    for(const auto& client:clients)client->pump(nowMs);
    std::erase_if(runtimeClients_,[](const auto& client){return client->releasable();});
    pumpGestureCoordinator(*this,nowMs);
    if (auto* playback = clips(); playback && playback->pollDecodeProblems())
        refreshProblems();
    return devices_.tick(nowMs);
}

// ---------------------------------------------------------------------------
// BlockProcessor
// ---------------------------------------------------------------------------

void Session::prepare(double sampleRate, std::int32_t maxFrames) {
    prepareFormat(sampleRate, maxFrames, false);
}

void Session::prepareWithGraphWorkers(double sampleRate, std::int32_t maxFrames,
                                     std::uint32_t workers, std::int32_t minimumFrames) {
    constexpr std::uint32_t minimumNodes = 2;
    const auto revision = graph_.configurationRevision();
    const bool configured = graph_.setParallelRendering(workers, minimumFrames, minimumNodes);
    if (prepareFormat(sampleRate, maxFrames, graph_.configurationRevision() != revision))
        return;
    if (configured && workers != 0) {
        // A failed parallel preparation must still give the device a chance to play serially.
        graph_.setParallelRendering(0, minimumFrames, minimumNodes);
        rebuild();
    }
}

bool Session::prepareFormat(double sampleRate, std::int32_t maxFrames, bool workerPolicyChanged) {
    ++stats_.prepares;
    const bool changed = (sampleRate != spec_.sampleRate) || (maxFrames != spec_.maxFrames);
    if (changed) {
        recording_->stop();
        midiInputCapture_->deviceStopped();
        resetMidiInput();
        clearMidiPreviewDevice();
        if (transport_.countInRemaining() > 0)
            transport_.play(false);
        countInTail_ = false;
        clickCaptureEdgeCount_ = 0;
        clickEnvelope_ = 0;
        clickWasPlaying_ = false;
    }

    // Decision 3, second half: same format, graph live, nothing to do. A driver
    // that stops and restarts at the same size -- a device-list change, a
    // sleep/wake -- must not produce a seam the user did not ask for.
    if (!changed && !workerPolicyChanged && live_) return true;

    if (changed) {
        ++runtimeEpoch_;
        ++stats_.formatChanges;
        spec_.sampleRate = sampleRate;
        spec_.maxFrames = maxFrames;
    }
    return !loaded_ || rebuild();  // `process` writes silence until a project arrives
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
    ++runtimeEpoch_;
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
    info.stripLanes = stripLanes.get();
    info.playing = transport.playing();
    info.transportRevision = transport.revision();
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

std::int64_t Session::TimingView::sampleForTick(std::int64_t tick) const noexcept {
    const long double sample = static_cast<long double>(tempo.ticksToSeconds(tick)) * sampleRate;
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (!std::isfinite(sample) || sample >= static_cast<long double>(limit)) return limit;
    if (sample <= 0) return 0;
    return static_cast<std::int64_t>(std::llround(sample));
}

void Session::TimingView::setBraceSamples(std::int64_t begin, std::int64_t end) noexcept {
    loopBegin = begin;
    punchOutSample = end;
    // A sub-sample loop plays one frame. Linear capture uses the original boundary.
    loopEnd = end <= begin && begin < std::numeric_limits<std::int64_t>::max() ? begin + 1 : end;
}

void Session::TimingView::prepareDeviceClockChanges() {
    lastTickSeconds = tempo.ticksToSeconds(std::numeric_limits<std::int64_t>::max());
    deviceClockChanges.clear();
    deviceClockChanges.reserve(tempo.events.size() + signatures.size());
    const auto add = [&](std::int64_t tick) {
        deviceClockChanges.emplace_back(sampleForTick(tick), tick);
    };
    for (const auto& event : tempo.events) add(event.posTicks);
    for (const auto& meter : signatures) add(meter.posTicks);
    std::sort(deviceClockChanges.begin(), deviceClockChanges.end());
}

double Session::TimingView::quartersAt(std::int64_t sample) const noexcept {
    const double seconds = static_cast<double>(sample) / sampleRate;
    if (sample <= 0)
        return seconds * tempo.bpmAt(0) / 60.0;
    constexpr auto maximumTick = std::numeric_limits<std::int64_t>::max();
    constexpr double ppq = static_cast<double>(textproj::kPPQ);
    if (seconds >= lastTickSeconds)
        return static_cast<double>(maximumTick) / ppq;
    const auto wholeTick = tempo.secondsToTicks(seconds);
    const double remainder = seconds - tempo.ticksToSeconds(wholeTick);
    return static_cast<double>(wholeTick) / ppq + remainder * tempo.bpmAt(wholeTick) / 60.0;
}

DeviceTransport Session::TimingView::deviceAt(std::int64_t sample) const noexcept {
    constexpr double ppq = static_cast<double>(textproj::kPPQ);
    DeviceTransport result;
    result.sample = sample;
    result.quarterNotes = quartersAt(sample);
    const auto next = std::upper_bound(deviceClockChanges.begin(), deviceClockChanges.end(), sample,
        [](std::int64_t value, const auto& change) { return value < change.first; });
    if (next != deviceClockChanges.end()) result.nextChange = next->first;
    constexpr auto maximumTick = std::numeric_limits<std::int64_t>::max();
    const auto seconds = static_cast<double>(sample) / sampleRate;
    auto tick = seconds >= lastTickSeconds ? maximumTick : tempo.secondsToTicks(seconds);
    if (next != deviceClockChanges.begin()) {
        const auto changeTick = std::prev(next)->second;
        tick = std::max(tick, changeTick);
        result.quarterNotes = std::max(result.quarterNotes, static_cast<double>(changeTick) / ppq);
    }
    result.bpm = tempo.bpmAt(tick);
    const auto& transport = session->transport_;
    result.playing = transport.playing();
    result.recording = result.playing && session->recordingForDevices_.load(std::memory_order_acquire);
    result.looping = transport.looping();
    if (result.looping) {
        result.loopBegin = transport.loopBegin();
        result.loopEnd = transport.loopEnd();
        result.loopBeginQuarterNotes = quartersAt(result.loopBegin);
        result.loopEndQuarterNotes = quartersAt(result.loopEnd);
    }

    std::int64_t start = 0;
    std::int64_t barsBefore = 0;
    std::int64_t barTicks = textproj::kWhole;
    for (const auto& meter : signatures) {
        if (meter.posTicks > tick) break;
        const auto span = meter.posTicks - start;
        barsBefore += span / barTicks + (span % barTicks != 0 ? 1 : 0);
        start = meter.posTicks;
        result.numerator = static_cast<int>(meter.numerator);
        result.denominator = static_cast<int>(meter.denominator);
        barTicks = textproj::kWhole * meter.numerator / meter.denominator;
    }
    const double startQuarters = static_cast<double>(start) / ppq;
    const double barQuarters = static_cast<double>(barTicks) / ppq;
    const auto barsIntoMeter = sample < 0
        ? static_cast<std::int64_t>(std::floor(result.quarterNotes / barQuarters))
        : (tick - start) / barTicks;
    result.bar = barsBefore + barsIntoMeter;
    result.barStart = startQuarters + static_cast<double>(barsIntoMeter) * barQuarters;
    return result;
}

void Session::TimingView::applyLoop() const noexcept {
    const bool sameLoop = session->loopApplied_ && session->loopOn_ == settings.loopEnabled &&
                          session->loopStartTick_ == settings.loopStartTicks && session->loopEndTick_ == settings.loopEndTicks;
    if(settings.hasLoop && (!sameLoop || session->loopBegin_!=loopBegin || session->loopEnd_!=loopEnd)) {
        session->loopApplied_=true;session->loopOn_=settings.loopEnabled;session->loopBegin_=loopBegin;session->loopEnd_=loopEnd;
        session->loopStartTick_ = settings.loopStartTicks; session->loopEndTick_ = settings.loopEndTicks;
        if (sameLoop) session->transport_.retimeLoop(loopBegin,loopEnd,settings.loopEnabled);
        else session->transport_.loop(loopBegin,loopEnd,settings.loopEnabled);
    }
}

const TransportInfo* Session::TimingView::beginBlock()const noexcept {
    if(generationControls)generationControls->apply();
    const StripLaneBindings* overlay=gestureAudio?gestureAudio->apply(gestureGeneration):nullptr;
    applyLoop();
    blockInfo=at(session->transport_);
    blockInfo.deviceClock = &session->transport_;
    blockInfo.deviceClockContext = this;
    blockInfo.devicePosition = [](const void* context, std::int64_t sample) noexcept {
        return static_cast<const TimingView*>(context)->deviceAt(sample);
    };
    if(overlay)blockInfo.stripLanes=overlay;
    return &blockInfo;
}

void Session::publishDriverClock() noexcept {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    // All fields and the sequence use one sequentially-consistent order. This
    // is a bounded atomic snapshot, never an audio-side lock or retry loop.
    driverClockSequence_.fetch_add(1);
    driverSample_.store(transport_.position());
    driverStreamed_.store(transport_.streamedFrames());
    driverLoopBegin_.store(transport_.looping() ? transport_.loopBegin() : 0);
    driverLoopEnd_.store(transport_.looping() ? transport_.loopEnd() : 0);
    driverRevision_.store(transport_.revision());
    driverPlaying_.store(transport_.playing());
    driverClockSequence_.fetch_add(1);
}
bool Session::readDriverClock(DriverClock& result) const noexcept {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const auto before = driverClockSequence_.load();
        if ((before & 1U) != 0) continue;
        DriverClock candidate;
        candidate.sample = driverSample_.load();
        candidate.streamedFrames = driverStreamed_.load();
        candidate.loopBegin = driverLoopBegin_.load();
        candidate.loopEnd = driverLoopEnd_.load();
        candidate.revision = driverRevision_.load();
        candidate.playing = driverPlaying_.load();
        if (driverClockSequence_.load() == before) { result = candidate; return true; }
    }
    return false;
}
// Live: an edit committed during playback never interrupts it (walk-A A21). Every new
// generation's readers start cold, so while the driver plays, read the pages at its
// playhead (and the loop head) before the graph is published; priming waits for the
// two pages of look-ahead too. A timeout publishes anyway: the readers catch up.
// Stopped, the pages at the playhead are read too, where Play starts (walk-B B12): Space
// commits `transport.play` before it posts Play, and the generation that commit builds
// while stopped could be published just after the driver started, cold, so the second
// block of playback was silent. Only while a driver runs: with no device nothing plays.
// An unfinished Warp render must not hold the owner here. Prime the other
// sources and post the Warp page requests so its worker can join when ready.
void Session::primeWhilePlaying(ClipPlayback& clips, const TimingView& timing) {
    DriverClock clock;
    if (driverClockSequence_.load() == 0 || !readDriverClock(clock)) return;
    Transport requested;
    requested.locate(clock.sample);
    if (timing.settings.hasLoop) requested.loop(timing.loopBegin, timing.loopEnd, timing.settings.loopEnabled);
    requested.play();
    constexpr auto timeout = std::chrono::milliseconds(100);
    if (!timing.settings.hasLoop || !timing.settings.loopEnabled) {
        (void)clips.primeAt(requested, timeout, false);
        return;
    }
    Transport loopHead = requested;
    loopHead.locate(timing.loopBegin);
    (void)clips.primeAt(requested, loopHead, timeout, false);
}

Session::RenderBlock::RenderBlock(Session& session) noexcept
    : session_(session), graph_(session.graph_.beginRender()) {
    session_.beginScopeRead();
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    if (timing) timing->applyLoop();
    session_.publishDriverClock();
}

double Session::RenderBlock::sampleRate() const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing ? timing->sampleRate : 0;
}
std::int32_t Session::RenderBlock::maxFrames() const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing ? timing->spec.maxFrames : 0;
}
Precision Session::RenderBlock::precision() const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing ? timing->spec.precision : Precision::Float32;
}
std::optional<double> Session::RenderBlock::externalTempoBpm() const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing ? timing->spec.externalTempoBpm : std::nullopt;
}
bool Session::RenderBlock::constantExternalTempo() const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing && timing->spec.externalTempoBpm && timing->tempo.events.size() == 1 &&
           timing->tempo.events.front().posTicks == 0 &&
           timing->tempo.events.front().bpm == *timing->spec.externalTempoBpm;
}
std::optional<std::int64_t> Session::RenderBlock::loopBeginTick() const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing && timing->settings.hasLoop ? std::optional<std::int64_t>{timing->settings.loopStartTicks} : std::nullopt;
}
std::optional<std::int64_t> Session::RenderBlock::loopEndTick() const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing && timing->settings.hasLoop ? std::optional<std::int64_t>{timing->settings.loopEndTicks} : std::nullopt;
}
std::int64_t Session::RenderBlock::tick() const noexcept {
    return tickAtSample(session_.transport_.position());
}
std::int64_t Session::RenderBlock::tickAtSample(std::int64_t sample) const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing ? timing->tempo.secondsToTicks(static_cast<double>(sample) / timing->sampleRate) : 0;
}
double Session::RenderBlock::beatAtSample(std::int64_t sample) const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing && sample > 0 ? timing->quartersAt(sample) : 0;
}
bool Session::RenderBlock::sourcesReadyAt(std::int64_t sample, std::int32_t frames,
                                         bool allowFailedSources) const noexcept {
    if (sample < 0) return false;
    Transport requested = session_.transport_; // this is the single driver, not the owner
    requested.locate(sample);
    requested.play();
    return sourcesReady(requested, frames, allowFailedSources);
}
bool Session::RenderBlock::sourcesReady(const Transport& requested, std::int32_t frames,
                                        bool allowFailedSources) const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    if (!timing || frames <= 0 || frames > timing->spec.maxFrames || !timing->sourceReadiness)
        return false;
    Transport target = requested;
    if (timing->settings.hasLoop)
        target.retimeLoop(timing->loopBegin, timing->loopEnd, timing->settings.loopEnabled);
    return timing->sourceReadiness(target, frames, allowFailedSources);
}
std::int64_t Session::RenderBlock::sampleAtTick(std::int64_t tick) const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing ? timing->sampleForTick(tick) : 0;
}
double Session::RenderBlock::bpmAtTick(std::int64_t tick) const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    return timing ? timing->tempo.bpmAt(tick) : 0;
}
bool Session::RenderBlock::startPlayback(std::int64_t insertMarkerTick, bool recording) noexcept {
    const auto *timing = dynamic_cast<const TimingView *>(graph_.transportState());
    if (!timing || insertMarkerTick < 0)
        return false;
    const auto tick = timing->settings.songStartTicks.value_or(insertMarkerTick);
    const auto sample = sampleAtTick(tick);
    if (sample == std::numeric_limits<std::int64_t>::max())
        return false;
    std::int64_t numerator = 4, denominator = 4;
    for (const auto &meter : timing->signatures) {
        if (meter.posTicks > tick)
            break;
        numerator = meter.numerator;
        denominator = meter.denominator;
    }
    const double beatSamples = timing->sampleRate * 60.0 / timing->tempo.bpmAt(tick) * 4.0 /
                               static_cast<double>(denominator);
    const long double frames = static_cast<long double>(beatSamples) *
                               static_cast<long double>(numerator) *
                               static_cast<long double>(timing->settings.countInBars);
    if (!std::isfinite(beatSamples) || beatSamples <= 0 || numerator <= 0 ||
        frames > static_cast<long double>(std::int64_t{1} << 50))
        return false;
    timing->applyLoop();
    session_.transport_.locate(sample);
    session_.countInTail_ = false;
    if (recording && timing->settings.countInBars > 0) {
        session_.countInBeatSamples_ = beatSamples;
        session_.countInBeatsPerBar_ = numerator;
        session_.countInBars_ = timing->settings.countInBars;
        session_.countInDivisionsPerBeat_ = static_cast<std::int32_t>(std::max<std::int64_t>(1, 16 / denominator));
        session_.clickEnvelope_ = 0;
        session_.clickWasPlaying_ = false;
        session_.transport_.countIn(
            std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(frames))));
    } else
        session_.transport_.play();
    session_.publishDriverClock();
    return true;
}
CountInPosition Session::RenderBlock::countInPosition() const noexcept {
    if (session_.transport_.countInRemaining() == 0 || session_.countInBars_ == 0)
        return {};
    const auto divisions = session_.countInDivisionsPerBeat_;
    const auto perBar = session_.countInBeatsPerBar_ * divisions;
    const auto elapsed = session_.transport_.countInElapsed();
    // Find the last display boundary reached on the sample clock. At a beat
    // boundary the rounding is the same as for the click's unshifted grid.
    std::int64_t step = 0;
    auto end = perBar * session_.countInBars_;
    while (step + 1 < end) {
        const auto middle = step + (end - step) / 2;
        const auto boundary = std::llround(static_cast<double>(middle) / divisions * session_.countInBeatSamples_);
        if (boundary <= elapsed)
            step = middle;
        else
            end = middle;
    }
    return {static_cast<int>(step / perBar) - session_.countInBars_,
            static_cast<int>((step % perBar) / divisions) + 1,
            static_cast<int>(step % divisions) + 1};
}
bool Session::RenderBlock::process(const AudioIo& io, bool captureRecording) noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.transportState());
    if (!graph_.valid() || io.frames <= 0 || io.numIn < 0 || io.numOut < 0 ||
        (timing && io.frames > timing->spec.maxFrames)) {
        // A stopped device uses an empty callback to consume the meter's
        // pending reset. It renders no graph and advances no transport.
        if (io.frames == 0) session_.mainLoudness_.process(io);
        if (io.frames > 0) {
            SilenceProcessor silence;
            silence.process(io);
        }
        return false;
    }
    const auto remaining = session_.transport_.countInRemaining();
    auto prefix = io.frames;
    auto punch = RecordingPunch::Within;
    RecordingPunchWindow punchWindow;
    if (remaining > 0 && remaining < prefix && timing)
        prefix = static_cast<std::int32_t>(remaining);
    const auto &transport = session_.transport_;
    if (timing && captureRecording) {
        const bool looping = transport.looping();
        if (timing->settings.punchIn) {
            const auto begin = looping ? transport.loopBegin() : timing->loopBegin;
            punchWindow.in = RecordingPunchWindow::Boundary{begin, timing->settings.loopStartTicks};
        }
        // Loop already wraps at this end. Let the recorder keep every pass.
        if (timing->settings.punchOut && !looping)
            punchWindow.out = RecordingPunchWindow::Boundary{timing->punchOutSample, timing->settings.loopEndTicks};
        if (transport.playing() && punchWindow.active()) {
            const auto position = transport.sampleAt(0);
            if (punchWindow.out && position >= punchWindow.out->sample)
                punch = RecordingPunch::After;
            else if (punchWindow.in && position < punchWindow.in->sample)
                punch = RecordingPunch::Before;
            const auto &boundary = punch == RecordingPunch::Before ? punchWindow.in : punchWindow.out;
            if (boundary && boundary->sample > position && boundary->sample - position < prefix)
                prefix = static_cast<std::int32_t>(boundary->sample - position);
        }
        if (looping)
            punchWindow = {}; // only linear takes use these bounds for placement
    }
    if ((remaining > 0 || prefix < io.frames) && timing &&
        (static_cast<std::size_t>(io.numIn) > timing->inputSlice.size() ||
         static_cast<std::size_t>(io.numOut) > timing->outputSlice.size() ||
         io.streamTimeSamples < 0 ||
         io.streamTimeSamples > std::numeric_limits<std::int64_t>::max() - io.frames)) {
        SilenceProcessor silence;
        silence.process(io);
        return false;
    }
    if (prefix < io.frames && timing) {
        // Bounds and the whole physical stream interval were validated before
        // rendering either half. No heap work or new graph acquisition.
        AudioIo lead = io;
        lead.frames = prefix;
        if (!process(lead, captureRecording))
            return false;
        for (std::int32_t channel = 0; channel < io.numIn; ++channel)
            timing->inputSlice[static_cast<std::size_t>(channel)] =
                io.in && io.in[channel] ? io.in[channel] + prefix : nullptr;
        for (std::int32_t channel = 0; channel < io.numOut; ++channel) {
            const auto c = static_cast<std::size_t>(channel);
            timing->outputSlice[c] = io.out && io.out[channel] ? io.out[channel] + prefix : nullptr;
            timing->outputSlice64[c] =
                io.out64 && io.out64[channel] ? io.out64[channel] + prefix : nullptr;
        }
        AudioIo tail = io;
        tail.frames -= prefix;
        tail.streamTimeSamples += prefix;
        tail.in = io.in ? timing->inputSlice.data() : nullptr;
        tail.out = io.out ? timing->outputSlice.data() : nullptr;
        tail.out64 = io.out64 ? timing->outputSlice64.data() : nullptr;
        return process(tail, captureRecording);
    }
    session_.playbackRunning_.store(session_.transport_.playing(), std::memory_order_release);
    AudioIo block = io;
    if (timing) {
        block.transport = &timing->blockInfo;
        block.exporting = timing->spec.exporting;
        auto* capture = captureRecording && timing->spec.performanceState && !timing->spec.exporting
            ? session_.midiInputCapture_.get() : nullptr;
        session_.midiInput_->render(graph_, io, timing->midiInputRoutes, timing->midiHistories, session_.transport_,
                                    {timing->playQuantizeTempo, timing->playQuantizeRate}, capture, punch, punchWindow);
        session_.midiInput_->renderPreview(graph_, io.frames, timing->midiPreviewRoutes, timing->sampleRate);
    }
    const auto* midiSink = timing && timing->spec.performanceState && !block.exporting && !io.exporting &&
                           io.streamTimeSamples >= 0 &&
                           io.streamTimeSamples <= std::numeric_limits<std::int64_t>::max() - io.frames
        ? io.midiHardware : nullptr;
    if (midiSink && session_.midiHardwareRouting_ != timing->midiHardwareRouting) {
        session_.midiHardwareRouting_ = timing->midiHardwareRouting;
        if (midiSink->reset) midiSink->reset(midiSink->context);
    }
    struct RenderContext {
        RenderBlock& driver;
        const AudioIo& block;
        InputMonitors* monitors;
        const MidiHardwareSink* midi;
    };
    RenderContext renderContext{*this, block, timing ? timing->inputMonitors.get() : nullptr, midiSink};
    const auto render = [](void* raw, std::span<const std::int64_t> tracks,
                           std::span<const std::int64_t> resampling, const AudioIo* mainOutput) noexcept {
        const auto& call = *static_cast<RenderContext*>(raw);
        auto& liveSession = call.driver.session_;
        liveSession.recordingAudioTracks_ = tracks;
        if (call.monitors) {
            call.monitors->input = call.block.in;
            call.monitors->channels = call.block.numIn;
        }
        liveSession.midiHardwareSink_ = call.midi;
        liveSession.midiHardwareStreamSample_ = call.block.streamTimeSamples;
        const bool rendered = call.driver.graph_.process(call.block, resampling);
        liveSession.midiHardwareSink_ = nullptr;
        liveSession.recordingAudioTracks_ = {};
        if (call.monitors) {
            call.monitors->input = nullptr;
            call.monitors->channels = 0;
        }
        return rendered && (!mainOutput || call.driver.graph_.copyMainOutput(*mainOutput));
    };
    const auto slice = captureRecording
        ? session_.recording_->captureAndRender(io, transport, punch, punchWindow,
                                               timing ? &timing->tempo : nullptr, graph_.generation(),
                                               {&renderContext, render})
        : AudioInputRecorder::SliceResult{render(&renderContext, {}, {}, nullptr), false};
    // MIDI can still report Recording after a locate outside the punch. Only
    // a playing slice inside the punch can contribute to the capture gate.
    const bool capturing = captureRecording
        ? slice.captured || (transport.playing() && punch == RecordingPunch::Within &&
                             session_.midiInputCapture_->state() == MidiCaptureState::Recording)
        : transport.playing() && session_.recordingForDevices_.load(std::memory_order_acquire);
    if (!slice.rendered) return false;
    session_.noteClickCapture(capturing);
    session_.mainLoudness_.process(block);
    const bool rollingClick = timing && timing->settings.metronome && session_.transport_.playing() &&
                              (!timing->settings.metronomeOptions.recordingOnly || session_.recordingClickPending());
    struct ClickContext { Session* session; const TimingView* timing; bool rolling; };
    ClickContext click{&session_, timing, rollingClick};
    CueMonitor::Click renderClick;
    if (timing && (session_.transport_.countInRemaining() > 0 || session_.countInTailActive() || rollingClick)) {
        renderClick.context = &click;
        renderClick.render = [](void* context, const AudioIo& cueIo) noexcept {
            const auto& call = *static_cast<ClickContext*>(context);
            if (call.session->transport_.countInRemaining() > 0)
                call.session->renderCountIn(*call.timing, cueIo);
            else
                call.session->renderMetronome(*call.timing, cueIo, call.rolling);
        };
    } else if (timing)
        session_.renderMetronome(*timing, AudioIo{}, false);
    session_.cueMonitor_.process(session_.graph_, block, renderClick);
    session_.transport_.advance(io.frames);
    if (remaining > 0 && session_.transport_.countInRemaining() == 0 &&
        session_.transport_.playing()) {
        // This callback ended the count-in (a crossing callback was split
        // there). Its delayed grid may owe clicks to the start of the roll.
        session_.countInTail_ = true;
        session_.countInTailRevision_ = session_.transport_.revision();
        session_.countInTailTotal_ = session_.transport_.countInElapsed();
        session_.countInTailLag_ = session_.countInLag(session_.graph_.renderedLatency());
    }
    session_.publishDriverClock();
    return true;
}
void Session::process(const AudioIo& io) noexcept {
    auto block = beginRender();
    (void)block.process(io);
}

bool Session::enqueueMidiInput(std::int64_t sample, const Event& event, std::int64_t inputSample,
                               std::uint64_t port) noexcept {
    return midiInput_->push(sample, event, inputSample < 0 ? sample : inputSample, port);
}
void Session::resetMidiInput() noexcept {
    midiInput_->reset();
}

std::optional<Session::MidiRecordMonitor> Session::midiRecordMonitor(std::int64_t track) const {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.currentTransportState());
    const auto* graph = graph_.currentGraph();
    if (!live_ || !timing || !graph || track <= 0) return std::nullopt;
    for (const auto& route : timing->midiInputRoutes) {
        if (route.monitorTrack != track || !route.processes) continue;
        const auto instrument = graph->find(*route.node);
        if (instrument != kInvalidNode)
            return MidiRecordMonitor{std::max(0, graph->arrivalOf(instrument)), route.blockedRanges};
    }
    return std::nullopt;
}

bool Session::midiHistoryAvailable() const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.currentTransportState());
    if (!live_ || !timing) return false;
    return std::any_of(timing->midiHistories.begin(), timing->midiHistories.end(), [](const auto& history) {
        return history->memory.hasEventsAfter(history->consumed);
    });
}

bool Session::readMidiHistories(std::vector<MidiMemorySnapshot>& output, std::string& error) const {
    output.clear();
    const auto* timing = dynamic_cast<const TimingView*>(graph_.currentTransportState());
    if (live_ && timing) {
        const auto before = midiInput_->historyRevision();
        if ((before & 1) != 0) {
            error = "A MIDI input block is arriving. Try Capture MIDI again.";
            return false;
        }
        for (const auto& history : timing->midiHistories) {
            if (!history->memory.hasEventsAfter(history->consumed)) continue;
            MidiMemorySnapshot snapshot;
            if (!history->memory.snapshot(snapshot, error)) {
                output.clear();
                return false;
            }
            std::erase_if(snapshot.events, [&](const auto& event) { return event.serial < history->consumed; });
            if (!snapshot.events.empty()) output.push_back(std::move(snapshot));
        }
        if (midiInput_->historyRevision() != before) {
            output.clear();
            error = "MIDI input changed during Capture. Try Capture MIDI again.";
            return false;
        }
    }
    if (!output.empty()) return true;
    error = "No recent MIDI input on an armed or input-monitored track.";
    return false;
}

void Session::consumeMidiHistories(std::span<const MidiMemorySnapshot> snapshots) noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.currentTransportState());
    if (!timing) return;
    for (const auto& snapshot : snapshots) {
        if (snapshot.events.empty()) continue;
        const auto found = std::find_if(timing->midiHistories.begin(), timing->midiHistories.end(), [&](const auto& history) {
            return history->inputId == snapshot.inputId;
        });
        if (found != timing->midiHistories.end())
            (*found)->consumed = std::max((*found)->consumed, snapshot.events.back().serial + 1);
    }
}

const LiveMidiInput::PreviewRoute* Session::midiPreviewRoute(std::int64_t track) const noexcept {
    const auto* timing = dynamic_cast<const TimingView*>(graph_.currentTransportState());
    if (!live_ || !timing || track <= 0)
        return nullptr;
    for (const auto& route : timing->midiPreviewRoutes)
        if (route.track == track)
            return &route;
    return nullptr;
}

bool Session::previewMidiNote(std::int64_t track, int key, int velocity, bool held) noexcept {
    const auto* route = midiPreviewRoute(track);
    return route && midiInput_->preview(*route, key, velocity, held);
}

void Session::stopMidiPreview() noexcept { midiInput_->stopPreview(); }

std::array<std::uint64_t, 2> Session::midiKeysSounding(std::int64_t track) const noexcept {
    const auto* route = midiPreviewRoute(track);
    return route ? route->node->heldNotes().keyMask() : std::array<std::uint64_t, 2>{};
}

void Session::clearMidiPreviewDevice() noexcept {
    midiInput_->clearPreviewDevice();
    for (auto& [node, voice] : midiPreviewVoices_) {
        (void)node;
        voice = {};
    }
}

bool Session::startMidiInputCapture(std::span<const std::int64_t> tracks, std::string& error, bool overdub) {
    if (!loaded_ || !spec_.performanceState || spec_.exporting || tracks.empty() || tracks.size() > 256) {
        error = "MIDI capture needs a live project and between 1 and 256 armed tracks";
        return false;
    }
    std::set<std::int64_t> seen;
    for (const auto id : tracks) {
        const auto track = std::find_if(model_.tracks.begin(), model_.tracks.end(),
            [id](const rows::Track& t) { return t.id == id; });
        if (id <= 0 || track == model_.tracks.end() || !recordArmed(*track) ||
            (track->kind != "midi" && track->kind != "instrument") || !seen.insert(id).second) {
            error = "MIDI capture needs distinct armed MIDI or instrument tracks";
            return false;
        }
    }
    if (!midiInputCapture_->start({tracks.begin(), tracks.end()}, spec_.sampleRate, overdub)) {
        error = "finish and drain the previous MIDI capture first";
        return false;
    }
    error.clear();
    return true;
}

void Session::finishMidiInputCapture() noexcept {
    midiInputCapture_->deviceStopped();
}

// DF-E6 (a). The count-in clicks on the clock the listener hears, the same
// latency-compensated clock as the rolling metronome: with L samples of
// render latency every count-in beat sounds L samples late, as bar one's
// program and accent do, so the last count-in beat and bar one are exactly
// one beat apart. Delay compensation keeps every track in sync, and the
// count-in runs up to bar one, where recording starts; the transport and the
// recorded take still start when the count-in ends, unchanged.
// DF-E6 (b) is the director's decision: when L is more than one beat the
// count-in keeps today's immediate grid (no lag here) and the roll's
// metronome waits for the compensated bar one, as before.
std::int64_t Session::countInLag(std::int32_t latency) const noexcept {
    return latency > 0 && static_cast<double>(latency) <= countInBeatSamples_ ? latency : 0;
}
bool Session::countInTailActive() const noexcept {
    return countInTail_ && transport_.playing() &&
           transport_.revision() == countInTailRevision_ &&
           transport_.streamedFrames() < countInTailLag_;
}
namespace {
double clickDecay(MetronomeSound sound, double rate) noexcept {
    const double seconds = sound == MetronomeSound::Wood ? 0.002 : sound == MetronomeSound::Bell ? 0.016 : 0.004;
    return std::exp(-1.0 / (seconds * rate));
}
}
void Session::triggerClick(bool accent, MetronomeSound sound) noexcept {
    clickPhase_ = 0;
    clickEnvelope_ = 0.2;
    switch (sound) {
    case MetronomeSound::Wood: clickFrequency_ = accent ? 1200 : 690; break;
    case MetronomeSound::Bell: clickFrequency_ = accent ? 1280 : 960; break;
    case MetronomeSound::Classic: clickFrequency_ = accent ? 1760 : 880; break;
    }
}
double Session::nextClickSample(double rate, double decay, MetronomeSound sound) noexcept {
    const double fundamental = std::sin(clickPhase_);
    double shape = fundamental;
    if (sound == MetronomeSound::Wood)
        shape = 0.6 * fundamental - 0.4 * std::sin(3 * clickPhase_);
    else if (sound == MetronomeSound::Bell)
        shape = 0.65 * fundamental + 0.25 * std::sin(2 * clickPhase_) + 0.1 * std::sin(3 * clickPhase_);
    clickPhase_ += 2 * std::numbers::pi * clickFrequency_ / rate;
    if (clickPhase_ >= 2 * std::numbers::pi)
        clickPhase_ -= 2 * std::numbers::pi;
    const double value = shape * clickEnvelope_;
    clickEnvelope_ *= decay;
    return value;
}
// One count-in frame at `heard` frames after the first heard count-in beat;
// before that beat it only carries the (silent) envelope on.
double Session::countInClick(std::int64_t heard, double rate, double decay, MetronomeSound sound) noexcept {
    if (heard >= 0) {
        const auto beat = static_cast<std::int64_t>(
            std::llround(static_cast<double>(heard) / countInBeatSamples_));
        const auto boundary = static_cast<std::int64_t>(
            std::llround(static_cast<double>(beat) * countInBeatSamples_));
        if (heard == boundary)
            triggerClick(beat % countInBeatsPerBar_ == 0, sound);
    }
    return nextClickSample(rate, decay, sound);
}
void Session::renderCountIn(const TimingView &timing, const AudioIo &io) noexcept {
    const auto heard = transport_.countInElapsed() - countInLag(graph_.renderedLatency());
    const auto sound = timing.settings.metronomeOptions.sound;
    const double decay = clickDecay(sound, timing.sampleRate);
    for (std::int32_t frame = 0; frame < io.frames; ++frame) {
        const double value = countInClick(heard + frame, timing.sampleRate, decay, sound);
        for (std::int32_t channel = 0; channel < io.numOut; ++channel) {
            if (io.out && io.out[channel])
                io.out[channel][frame] += static_cast<float>(value);
            if (io.out64 && io.out64[channel])
                io.out64[channel][frame] += value;
        }
    }
}
void Session::noteClickCapture(bool capturing) noexcept {
    if (clickCaptureRevision_ != transport_.revision()) {
        clickCaptureRevision_ = transport_.revision();
        clickCaptureEdgeCount_ = 0;
    }
    const bool wasCapturing = clickCaptureEdgeCount_ && clickCaptureEdges_[clickCaptureEdgeCount_ - 1].capturing;
    if (capturing == wasCapturing)
        return;
    if (clickCaptureEdgeCount_ == clickCaptureEdges_.size()) {
        // If more than 32 takes fit inside the delay, drop the earliest edge.
        // An unheard frame older than the retained history is treated as silent.
        std::move(clickCaptureEdges_.begin() + 1, clickCaptureEdges_.end(), clickCaptureEdges_.begin());
        --clickCaptureEdgeCount_;
    }
    clickCaptureEdges_[clickCaptureEdgeCount_++] = {transport_.streamedFrames(), capturing};
}

bool Session::recordingClickPending() const noexcept {
    if (clickCaptureEdgeCount_ == 0)
        return false;
    const auto& lastEdge = clickCaptureEdges_[clickCaptureEdgeCount_ - 1];
    return lastEdge.capturing || transport_.streamedFrames() - lastEdge.streamFrame < graph_.renderedLatency();
}

void Session::renderMetronome(const TimingView& timing, const AudioIo& io, bool rollingClick) noexcept {
    const bool tail=countInTailActive();
    if((!rollingClick && !tail) || !transport_.playing()) {
        clickEnvelope_=0;clickWasPlaying_=false;countInTail_=false;return;
    }
    const auto& tempo=timing.tempo;const double rate=timing.sampleRate;
    const auto latency=graph_.renderedLatency();
    const auto options = timing.settings.metronomeOptions;
    const double decay = clickDecay(options.sound, rate);
    const auto captureEdges = std::span{clickCaptureEdges_}.first(options.recordingOnly ? clickCaptureEdgeCount_ : 0);
    auto edge = captureEdges.begin();
    bool clickAllowed = !options.recordingOnly;
    const auto streamFrame = transport_.streamedFrames();
    // Heard count-in clock of this callback's first frame (DF-E6 (a)).
    const auto heard=countInTailTotal_-countInTailLag_+transport_.streamedFrames();
    for(std::int32_t i=0;i<io.frames;++i) {
        const auto sample=transport_.delayedSample(i,latency);
        if(sample<0 && tail && heard+i<countInTailTotal_) {
            // The compensated program is not heard yet: the count-in's last
            // clicks and their decay play on, on the count-in grid.
            const double click=countInClick(heard+i,rate,decay,options.sound);
            for(std::int32_t c=0;c<std::min<std::int32_t>(io.numOut,2);++c) {
                if(io.out && io.out[c])io.out[c][i]+=static_cast<float>(click);
                if(io.out64 && io.out64[c])io.out64[c][i]+=click;
            }
            clickWasPlaying_=false;continue;
        }
        if(!rollingClick){clickEnvelope_=0;clickWasPlaying_=false;countInTail_=false;return;}
        // Capture changed on the undelayed clock, which can be several
        // slices ahead of the timeline heard now, even for a complete punch.
        while (edge != captureEdges.end() && edge->streamFrame - streamFrame <= i - latency)
            clickAllowed = (edge++)->capturing;
        if(sample<0 || !clickAllowed){clickWasPlaying_=false;clickEnvelope_=0;continue;}
        const auto tick=tempo.secondsToTicks(static_cast<double>(sample)/rate);
        std::int64_t signature=0,numerator=4,denominator=4;
        for(const auto& meter:timing.signatures)if(meter.posTicks<=tick){signature=meter.posTicks;numerator=meter.numerator;denominator=meter.denominator;}else break;
        const auto signatureBeat = textproj::kWhole / denominator;
        const auto barTicks = signatureBeat * numerator;
        const auto barStart = tick - (tick - signature) % barTicks;
        const auto interval = options.eighthNotes && metronomeEighthNotesFit(barTicks)
            ? kMetronomeEighthNoteTicks : signatureBeat;
        const auto beatTick = tick - (tick - barStart) % interval;
        const double boundary=tempo.ticksToSeconds(beatTick)*rate;
        const bool adjacent=clickWasPlaying_ && sample>0 && clickLastSample_==sample-1;
        const bool atBoundary=std::abs(static_cast<double>(sample)-boundary)<=1.01;
        if((adjacent && beatTick!=clickLastBeat_) || (!adjacent && atBoundary))
            triggerClick(beatTick == barStart, options.sound);
        const double click = nextClickSample(rate, decay, options.sound);
        if(clickEnvelope_<1e-12)clickEnvelope_=0;
        // Default Cue pair until the separate Cue Out routing (N23) is built.
        for(std::int32_t c=0;c<std::min<std::int32_t>(io.numOut,2);++c) {
            if(io.out && io.out[c])io.out[c][i]+=static_cast<float>(click);
            if(io.out64 && io.out64[c])io.out64[c][i]+=click;
        }
        clickLastSample_=sample;clickLastBeat_=beatTick;clickWasPlaying_=true;
    }
}

void Session::release() {
    ++runtimeEpoch_;
    recording_->stop();
    midiInputCapture_->deviceStopped();
    resetMidiInput();
    clearMidiPreviewDevice();
    // A device stop cancels a pending lead-in, as a format change does in
    // prepare(): the recorder it was counting in for has just stopped, so a
    // same-format restart must not resume clicking and then start the
    // timeline with nothing recording. Nothing renders now; no race.
    if (transport_.countInRemaining() > 0)
        transport_.play(false);
    countInTail_ = false;
    clickCaptureEdgeCount_ = 0;
    clickEnvelope_ = 0;
    clickWasPlaying_ = false;
    // The device has stopped; nothing is rendering. Release the plugins so a
    // later `prepare` -- at the same format or not -- prepares them again
    // rather than finding ADR-0090 d5's guard satisfied on a released
    // instance. `live_` false is what defeats the no-op path above.
    live_ = false;
    cueMonitor_.release();
    mainLoudness_.requestReset();
    playbackRunning_.store(false, std::memory_order_release);
    if (Graph* g = graph_.currentGraph()) g->release();
}

}  // namespace adi::engine
