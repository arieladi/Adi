// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/projection.hpp"

#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "adi/textproj_store.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace adi::control {
namespace {

constexpr const char* kPitchNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::vector<textproj::Meter> metersOf(const rows::Model& m) {
    auto meters = textproj::metersOf(m.signatures);
    std::sort(meters.begin(), meters.end(),
              [](const textproj::Meter& a, const textproj::Meter& b) { return a.start_ticks < b.start_ticks; });
    return meters;
}

std::string meterText(const rows::Model& m) {
    if (m.signatures.empty()) return "4/4";
    const auto first = std::min_element(m.signatures.begin(), m.signatures.end(),
                                        [](const auto& a, const auto& b) { return a.posTicks < b.posTicks; });
    return std::to_string(first->numerator) + "/" + std::to_string(first->denominator);
}

Json tempoJson(const rows::Model& m) {
    if (m.tempo.empty()) return {{"bpm", 120.0}, {"changes", 0}};
    const auto first = std::min_element(m.tempo.begin(), m.tempo.end(),
                                        [](const auto& a, const auto& b) { return a.posTicks < b.posTicks; });
    return {{"bpm", first->bpm}, {"changes", static_cast<std::int64_t>(m.tempo.size()) - 1}};
}

Json keyJson(const rows::Model& m) {
    if (m.keyEvents.empty()) return nullptr;
    const auto first = std::min_element(m.keyEvents.begin(), m.keyEvents.end(),
                                        [](const auto& a, const auto& b) { return a.posTicks < b.posTicks; });
    const auto root = ((first->root % 12) + 12) % 12;
    return {{"root", kPitchNames[root]}, {"scale", first->scaleId},
            {"changes", static_cast<std::int64_t>(m.keyEvents.size()) - 1}};
}

/// Tracks in the arrangement's tree order: roots by index, each followed by
/// its children. A parent cycle (which the schema does not forbid) cannot loop.
std::vector<const rows::Track*> treeOrder(const rows::Model& m) {
    std::unordered_map<std::int64_t, const rows::Track*> byId;
    for (const auto& t : m.tracks) byId.emplace(t.id, &t);
    std::unordered_map<std::int64_t, std::vector<const rows::Track*>> children;
    std::vector<const rows::Track*> roots;
    for (const auto& t : m.tracks) {
        if (t.parentId && byId.count(*t.parentId) && *t.parentId != t.id) children[*t.parentId].push_back(&t);
        else roots.push_back(&t);
    }
    const auto byIndex = [](const rows::Track* a, const rows::Track* b) {
        return a->indexInParent != b->indexInParent ? a->indexInParent < b->indexInParent : a->id < b->id;
    };
    std::sort(roots.begin(), roots.end(), byIndex);
    for (auto& [id, list] : children) std::sort(list.begin(), list.end(), byIndex);

    std::vector<const rows::Track*> out;
    std::unordered_set<std::int64_t> seen;
    std::function<void(const rows::Track*)> visit = [&](const rows::Track* t) {
        if (!seen.insert(t->id).second) return;
        out.push_back(t);
        const auto it = children.find(t->id);
        if (it != children.end())
            for (const auto* c : it->second) visit(c);
    };
    for (const auto* r : roots) visit(r);
    // Anything only reachable through a cycle still appears, once.
    for (const auto& t : m.tracks)
        if (!seen.count(t.id)) visit(&t);
    return out;
}

Json endpoint(const std::string& kind, std::int64_t id) { return {{"kind", kind}, {"id", id}}; }

Json devicesOfTrack(const rows::Model& m, std::int64_t trackId) {
    std::vector<const rows::DeviceChain*> chains;
    for (const auto& c : m.deviceChains)
        if (c.trackId && *c.trackId == trackId) chains.push_back(&c);
    std::sort(chains.begin(), chains.end(), [](const auto* a, const auto* b) { return a->ord < b->ord; });
    Json out = Json::array();
    for (const auto* c : chains) {
        std::vector<const rows::Device*> devices;
        for (const auto& d : m.devices)
            if (d.chainId == c->id) devices.push_back(&d);
        std::sort(devices.begin(), devices.end(), [](const auto* a, const auto* b) {
            return a->ord != b->ord ? a->ord < b->ord : a->id < b->id;
        });
        for (const auto* d : devices) {
            Json plugin = nullptr;
            if (d->pluginRefId) {
                for (const auto& r : m.pluginRefs)
                    if (r.id == *d->pluginRefId) {
                        plugin = r.format + ": " + r.name;
                        break;
                    }
            }
            out.push_back({{"id", d->id}, {"chain", c->id}, {"name", d->name}, {"enabled", d->enabled},
                           {"rack", d->isRack}, {"plugin", plugin}, {"missing", d->missing}});
        }
    }
    return out;
}

std::set<std::int64_t> devicesOn(const rows::Model& m, const std::set<std::int64_t>& tracks) {
    std::set<std::int64_t> chains;
    for (const auto& c : m.deviceChains)
        if (c.trackId && tracks.count(*c.trackId)) chains.insert(c.id);
    std::set<std::int64_t> out;
    for (const auto& d : m.devices)
        if (chains.count(d.chainId)) out.insert(d.id);
    return out;
}

Json routingRow(const rows::Routing& r) {
    return {{"id", r.id},        {"kind", r.kind},         {"from", endpoint(r.srcKind, r.srcId)},
            {"to", endpoint(r.dstKind, r.dstId)}, {"gainDb", r.gainDb}, {"pan", r.pan},
            {"preFader", r.preFader}, {"enabled", r.enabled}, {"origin", r.origin}};
}

template <class Rows, class Id>
std::int64_t nextOf(const Rows& rowsIn, Id id) {
    std::int64_t top = 0;
    for (const auto& r : rowsIn) top = std::max(top, id(r));
    return top + 1;
}

}  // namespace

std::string noteName(std::int64_t key) {
    const std::int64_t pitch = ((key % 12) + 12) % 12;
    const std::int64_t octave = (key - pitch) / 12 - 2;   // 60 -> C3
    return std::string(kPitchNames[pitch]) + std::to_string(octave);
}

std::int64_t ticksAtBar(std::int64_t bar, const rows::Model& m) {
    auto meters = metersOf(m);
    if (meters.empty() || meters.front().start_ticks > 0) meters.insert(meters.begin(), textproj::Meter{});
    const auto barTicksOf = [](const textproj::Meter& meter) {
        const auto t = meter.barTicks();
        return t > 0 ? t : textproj::Meter{}.barTicks();
    };
    if (bar <= 1) return meters.front().start_ticks + (bar - 1) * barTicksOf(meters.front());
    std::int64_t barAtStart = 1;
    for (std::size_t i = 0; i < meters.size(); ++i) {
        const auto len = barTicksOf(meters[i]);
        if (i + 1 < meters.size()) {
            const auto span = meters[i + 1].start_ticks - meters[i].start_ticks;
            const auto bars = (span + len - 1) / len;   // a change lands on a bar line
            if (bar < barAtStart + bars) return meters[i].start_ticks + (bar - barAtStart) * len;
            barAtStart += bars;
        } else {
            return meters[i].start_ticks + (bar - barAtStart) * len;
        }
    }
    return 0;
}

Json nextIds(const rows::Model& m) {
    return {
        {"track", nextOf(m.tracks, [](const rows::Track& r) { return r.id; })},
        {"clip", nextOf(m.clips, [](const rows::Clip& r) { return r.id; })},
        {"lane", nextOf(m.lanes, [](const rows::Lane& r) { return r.id; })},
        {"routing", nextOf(m.routing, [](const rows::Routing& r) { return r.id; })},
        {"chain", nextOf(m.deviceChains, [](const rows::DeviceChain& r) { return r.id; })},
        {"device", nextOf(m.devices, [](const rows::Device& r) { return r.id; })},
        {"pluginRef", nextOf(m.pluginRefs, [](const rows::PluginRef& r) { return r.id; })},
        {"marker", nextOf(m.markers, [](const rows::Marker& r) { return r.id; })},
        {"automationLane", nextOf(m.automationLanes, [](const rows::AutomationLane& r) { return r.id; })},
    };
}

Json projectLevel(const rows::Model& m, int level, const ReadScope& scope,
                  std::optional<std::int64_t> head, std::string_view project) {
    const auto meters = metersOf(m);
    const auto at = [&](std::int64_t ticks) { return textproj::renderPosition(ticks, meters); };

    Json out = Json::object();
    out["project"] = std::string(project);
    out["level"] = level;
    out["head"] = head ? Json(*head) : Json(nullptr);
    out["units"] = {
        {"positions", "ticks (ppq below) and bar|beat|tick, 1-based"},
        {"lengths", "ticks and a fraction of a whole note (or ticks with a t suffix)"},
        {"ids", "durable row ids; a changeset that creates rows takes their ids from nextIds"},
        {"notes", "middle C (60) is C3"}};
    out["song"] = {{"name", m.project.name},
                   {"sampleRate", m.project.sampleRate},
                   {"ppq", m.project.ppq},
                   {"lengthTicks", m.project.lengthTicks},
                   {"end", at(m.project.lengthTicks)},
                   {"tempo", tempoJson(m)},
                   {"meter", meterText(m)},
                   {"key", keyJson(m)}};

    std::unordered_map<std::int64_t, const rows::MixerStrip*> strips;
    for (const auto& s : m.strips) strips.emplace(s.trackId, &s);
    std::unordered_map<std::int64_t, std::int64_t> clipCount;
    for (const auto& c : m.clips) ++clipCount[c.trackId];

    Json tracks = Json::array();
    for (const auto* t : treeOrder(m)) {
        Json row = {{"id", t->id},
                    {"kind", t->kind},
                    {"name", t->name},
                    {"parent", t->parentId ? Json(*t->parentId) : Json(nullptr)},
                    {"index", t->indexInParent},
                    {"muted", t->muted},
                    {"soloed", t->soloed},
                    {"armed", t->recordArmed},
                    {"clips", clipCount.count(t->id) ? clipCount.at(t->id) : 0},
                    {"devices", devicesOfTrack(m, t->id)}};
        if (const auto it = strips.find(t->id); it != strips.end()) {
            row["volumeDb"] = it->second->volumeDb;
            row["pan"] = it->second->pan;
        }
        tracks.push_back(std::move(row));
    }
    out["tracks"] = std::move(tracks);

    Json routing = Json::array();
    for (const auto& r : m.routing) routing.push_back(routingRow(r));
    out["routing"] = std::move(routing);

    Json markers = Json::array();
    for (const auto& mk : m.markers) {
        Json row = {{"id", mk.id}, {"name", mk.name}, {"kind", mk.kind}};
        if (mk.posTicks) {
            row["pos"] = *mk.posTicks;
            row["at"] = at(*mk.posTicks);
        }
        markers.push_back(std::move(row));
    }
    out["markers"] = std::move(markers);
    out["nextIds"] = nextIds(m);
    if (!m.problems.empty()) out["problems"] = m.problems;
    if (level < 1) return out;

    // --- level 1: clips, automation lanes and device parameters, in scope ---------
    std::set<std::int64_t> inTracks(scope.tracks.begin(), scope.tracks.end());
    if (inTracks.empty())
        for (const auto& t : m.tracks) inTracks.insert(t.id);
    const std::optional<std::int64_t> fromTick =
        scope.fromBar ? std::optional<std::int64_t>(ticksAtBar(*scope.fromBar, m)) : std::nullopt;
    const std::optional<std::int64_t> toTick =
        scope.toBar ? std::optional<std::int64_t>(ticksAtBar(*scope.toBar, m)) : std::nullopt;
    const auto inRange = [&](const rows::Clip& c) {
        if (!c.posTicks) return !fromTick && !toTick;   // a linear-time clip: only unbounded reads
        const auto start = *c.posTicks;
        const auto end = start + c.lengthTicks.value_or(0);
        if (fromTick && end <= *fromTick) return false;
        if (toTick && start >= *toTick) return false;
        return true;
    };

    std::vector<const rows::Clip*> clips;
    for (const auto& c : m.clips)
        if (inTracks.count(c.trackId) && inRange(c)) clips.push_back(&c);
    std::sort(clips.begin(), clips.end(), [](const auto* a, const auto* b) {
        if (a->trackId != b->trackId) return a->trackId < b->trackId;
        return a->posTicks.value_or(0) != b->posTicks.value_or(0) ? a->posTicks.value_or(0) < b->posTicks.value_or(0)
                                                                  : a->id < b->id;
    });
    Json clipRows = Json::array();
    for (const auto* c : clips) {
        Json row = {{"id", c->id}, {"track", c->trackId}, {"kind", c->kind}, {"name", c->name},
                    {"muted", c->muted}, {"gainDb", c->gainDb}, {"loop", c->loopEnabled}};
        row["lane"] = c->laneId ? Json(*c->laneId) : Json(nullptr);
        if (c->posTicks) {
            row["pos"] = *c->posTicks;
            row["at"] = at(*c->posTicks);
        } else if (c->posNs) {
            row["posNs"] = *c->posNs;
        }
        if (c->lengthTicks) {
            row["lengthTicks"] = *c->lengthTicks;
            row["length"] = textproj::renderDuration(*c->lengthTicks);
        } else if (c->lengthNs) {
            row["lengthNs"] = *c->lengthNs;
        }
        clipRows.push_back(std::move(row));
    }
    out["clips"] = std::move(clipRows);

    const auto devices = devicesOn(m, inTracks);
    Json lanes = Json::array();
    for (const auto& l : m.automationLanes) {
        const bool inScope = (l.ownerKind == "track" && inTracks.count(l.ownerId)) ||
                             (l.ownerKind == "device" && devices.count(l.ownerId)) ||
                             (l.ownerKind != "track" && l.ownerKind != "device" && scope.tracks.empty());
        if (!inScope) continue;
        lanes.push_back({{"id", l.id}, {"owner", endpoint(l.ownerKind, l.ownerId)}, {"param", l.paramRef},
                         {"name", l.paramName}, {"enabled", l.enabled}, {"default", l.defaultValue}});
    }
    out["automationLanes"] = std::move(lanes);

    Json params = Json::array();
    bool paramsTruncated = false;
    for (const auto& p : m.pluginParams) {
        if (!devices.count(p.deviceId)) continue;
        if (params.size() >= kMaxParamsPerRead) {
            paramsTruncated = true;
            break;
        }
        Json row = {{"device", p.deviceId}, {"param", p.paramId}, {"name", p.name}, {"norm", p.normalized}};
        if (!p.display.empty()) row["display"] = p.display;
        params.push_back(std::move(row));
    }
    out["params"] = std::move(params);
    if (paramsTruncated) out["paramsTruncated"] = true;
    if (level < 2) return out;

    // --- level 2: notes, clip-relative, under the meter at each clip's start ------
    std::set<std::int64_t> noteClips(scope.clips.begin(), scope.clips.end());
    if (noteClips.empty())
        for (const auto* c : clips) noteClips.insert(c->id);
    std::unordered_map<std::int64_t, std::vector<textproj::Meter>> clipMeter;
    for (const auto& c : m.clips) {
        if (!noteClips.count(c.id)) continue;
        textproj::Meter local;
        const auto start = c.posTicks.value_or(0);
        for (const auto& meter : meters)
            if (meter.start_ticks <= start) local = meter;
        local.start_ticks = 0;
        clipMeter[c.id] = {local};
    }
    Json notes = Json::array();
    bool notesTruncated = false;
    for (const auto& n : m.notes) {
        if (!noteClips.count(n.clipId)) continue;
        if (notes.size() >= kMaxNotesPerRead) {
            notesTruncated = true;
            break;
        }
        const auto& local = clipMeter[n.clipId];
        Json row = {{"clip", n.clipId},
                    {"id", n.noteId},
                    {"at", textproj::renderPosition(n.startTicks, local)},
                    {"start", n.startTicks},
                    {"durTicks", n.durTicks},
                    {"length", textproj::renderDuration(n.durTicks)},
                    {"key", n.key},
                    {"name", noteName(n.key)},
                    {"vel", n.velOn}};
        if (n.channel != 0) row["chan"] = n.channel;
        if (n.probability != 10000) row["prob"] = n.probability;
        notes.push_back(std::move(row));
    }
    out["notes"] = std::move(notes);
    if (notesTruncated) out["notesTruncated"] = true;
    return out;
}

Json manifest(const rows::Model& m, std::optional<std::int64_t> head, std::string_view project,
              std::string_view label, std::string_view fileSchema) {
    const auto meters = metersOf(m);
    return {{"project", std::string(project)},
            {"label", std::string(label)},
            {"name", m.project.name},
            {"sampleRate", m.project.sampleRate},
            {"ppq", m.project.ppq},
            {"lengthTicks", m.project.lengthTicks},
            {"end", textproj::renderPosition(m.project.lengthTicks, meters)},
            {"tempo", tempoJson(m)},
            {"meter", meterText(m)},
            {"key", keyJson(m)},
            {"schema", {{"build", std::to_string(kSchemaMajor) + "." + std::to_string(kSchemaMinor)},
                        {"file", std::string(fileSchema)}}},
            {"head", head ? Json(*head) : Json(nullptr)},
            {"tracks", m.tracks.size()},
            {"clips", m.clips.size()}};
}

Json graph(const rows::Model& m, std::string_view project) {
    Json out = Json::object();
    out["project"] = std::string(project);

    std::unordered_map<std::int64_t, std::vector<std::int64_t>> children;
    std::optional<std::int64_t> master;
    for (const auto& t : m.tracks) {
        if (t.parentId) children[*t.parentId].push_back(t.id);
        if (t.kind == "master" && !master) master = t.id;
    }
    Json tracks = Json::array();
    for (const auto* t : treeOrder(m)) {
        Json kids = Json::array();
        if (const auto it = children.find(t->id); it != children.end())
            for (const auto c : it->second) kids.push_back(c);
        tracks.push_back({{"id", t->id}, {"kind", t->kind}, {"name", t->name},
                          {"parent", t->parentId ? Json(*t->parentId) : Json(nullptr)},
                          {"children", std::move(kids)}});
    }
    out["tracks"] = std::move(tracks);

    Json byKind = {{"main", Json::array()}, {"send", Json::array()}, {"sidechain", Json::array()},
                   {"vca", Json::array()}, {"cue", Json::array()}};
    std::set<std::int64_t> explicitOutput;
    for (const auto& r : m.routing) {
        if (!byKind.contains(r.kind)) byKind[r.kind] = Json::array();
        byKind[r.kind].push_back(routingRow(r));
        if (r.kind == "main" && r.srcKind == "track") explicitOutput.insert(r.srcId);
    }
    out["routing"] = std::move(byKind);

    // Where each signal-carrying track goes: an explicit `main` row, else its
    // group, else the master (ops_catalog's default when no main row exists).
    Json outputs = Json::array();
    static const std::set<std::string> kCarries = {"audio", "midi", "instrument", "group", "return"};
    for (const auto& t : m.tracks) {
        if (!kCarries.count(t.kind) || explicitOutput.count(t.id)) continue;
        Json to = t.parentId ? endpoint("track", *t.parentId)
                             : master ? endpoint("track", *master) : Json(nullptr);
        outputs.push_back({{"track", t.id}, {"to", std::move(to)}, {"implicit", true}});
    }
    out["implicitOutputs"] = std::move(outputs);

    Json chains = Json::array();
    for (const auto& c : m.deviceChains) {
        Json devices = Json::array();
        std::vector<const rows::Device*> list;
        for (const auto& d : m.devices)
            if (d.chainId == c.id) list.push_back(&d);
        std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) {
            return a->ord != b->ord ? a->ord < b->ord : a->id < b->id;
        });
        for (const auto* d : list) devices.push_back(d->id);
        chains.push_back({{"id", c.id},
                          {"track", c.trackId ? Json(*c.trackId) : Json(nullptr)},
                          {"device", c.parentDeviceId ? Json(*c.parentDeviceId) : Json(nullptr)},
                          {"ord", c.ord},
                          {"name", c.name},
                          {"devices", std::move(devices)}});
    }
    out["chains"] = std::move(chains);
    out["notes"] = Json::array({
        "a sidechain row is planned (track to track) but no device hears the key on this build: "
        "keying a device is DEV-05 and MX-24, at M4",
        "a track with no main row goes to its group, else to the master"});
    return out;
}

}  // namespace adi::control
