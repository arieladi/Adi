// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session. See session.hpp for the three decisions; this file is their
// mechanics. ADR-0122.

#include "adi/engine/session.hpp"

#include "adi/store.hpp"
#include "adi/audio/io_audit.hpp"

#include <algorithm>
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

Session::Session() = default;
Session::~Session() = default;

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
        const auto it = byId_.find(row.id);
        if (it == byId_.end()) {
            resolveOne(store, row);
            continue;
        }
        // A row that came BACK -- an undone removal -- finds its instance
        // waiting, state and all. That is the payoff of retiring rather than
        // destroying: the undo restores the sound, not a fresh default.
        Entry& e = entries_[it->second];
        if (e.retired) {
            e.retired = false;
            --stats_.retired;
        }
    }
}

void Session::retireDepartedRows() {
    for (Entry& e : entries_) {
        if (e.retired) continue;
        const bool present = std::any_of(
            model_.devices.begin(), model_.devices.end(),
            [&](const rows::Device& d) { return d.id == e.deviceId; });
        if (present) continue;
        e.retired = true;
        e.trackId = 0;
        ++stats_.retired;
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
            node.setBypassed(e.placeholder || !d.enabled);
            node.setAlwaysProcess(d.alwaysProcess);
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

void Session::attach() {
    device::DeviceHost::RebuildSpec rs;
    rs.model = [this] { return &model_; };
    rs.channels = spec_.channels;
    rs.sampleRate = spec_.sampleRate;
    rs.maxFrames = spec_.maxFrames;
    rs.devicesFor = [this](std::int64_t trackId) {
        std::vector<Node*> nodes;
        for (device::DeviceNode* n : chainFor(trackId)) nodes.push_back(n);
        return nodes;
    };
    auto* clipSource = clips();
    rs.sourcesFor = [this, clipSource](std::int64_t id) {
        auto nodes = sources_ ? sources_(id) : std::vector<Node*>{};
        if (clipSource) {
            auto audio = clipSource->sourcesFor(id);
            nodes.insert(nodes.end(), audio.begin(), audio.end());
        }
        return nodes;
    };
    devices_.attachHost(graph_, std::move(rs));
}

bool Session::rebuild() {
    error_.clear();
    if (!loaded_) {
        error_ = "no project loaded";
        return false;
    }
    try {
        clips_ = std::make_unique<ClipPlayback>(clipProject_, transport_, spec_.sampleRate,
                                                spec_.channels, spec_.maxFrames);
    } catch (const std::exception& e) { error_ = e.what(); return false; }
    attach();   // the spec, the sources or the placement may have changed
    ++stats_.rebuilds;
    const bool ok = devices_.rebuildNow();
    if (!ok) error_ = devices_.lastRebuildError();
    else live_ = true;

    problems_ = model_.problems;
    if (auto* c = clips()) problems_.insert(problems_.end(), c->problems().begin(), c->problems().end());
    problems_.insert(problems_.end(), sessionProblems_.begin(), sessionProblems_.end());
    const std::vector<std::string>& fromGraph = graph_.problems();
    problems_.insert(problems_.end(), fromGraph.begin(), fromGraph.end());
    return ok;
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
    stats_ = Stats{};
    model_ = rows::readModel(store);
    clipProject_ = readClipProject(store, model_);
    loaded_ = true;
    resolveNewRows(store);
    return rebuild();
}

bool Session::refresh(const Store& store) {
    error_.clear();
    if (!loaded_) {
        error_ = "no project loaded";
        return false;
    }
    model_ = rows::readModel(store);
    clipProject_ = readClipProject(store, model_);
    resolveNewRows(store);
    retireDepartedRows();
    syncFlags();
    return rebuild();
}

void Session::setSourcesFor(SourceFn fn) { sources_ = std::move(fn); }

bool Session::tick(std::int64_t nowMs) { return devices_.tick(nowMs); }

// ---------------------------------------------------------------------------
// BlockProcessor
// ---------------------------------------------------------------------------

void Session::prepare(double sampleRate, std::int32_t maxFrames) {
    ++stats_.prepares;
    const bool changed = (sampleRate != spec_.sampleRate) || (maxFrames != spec_.maxFrames);

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

void Session::process(const AudioIo& io) noexcept {
    audio::CallbackScope callback;
    graph_.process(io);
    transport_.advance(io.frames);
}

void Session::release() {
    // The device has stopped; nothing is rendering. Release the plugins so a
    // later `prepare` -- at the same format or not -- prepares them again
    // rather than finding ADR-0090 d5's guard satisfied on a released
    // instance. `live_` false is what defeats the no-op path above.
    live_ = false;
    if (Graph* g = graph_.currentGraph()) g->release();
}

}  // namespace adi::engine
