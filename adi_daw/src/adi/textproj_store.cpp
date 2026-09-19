// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/textproj_store.hpp"

#include "adi/store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <unordered_map>

namespace adi::textproj {
namespace {

// ---------------------------------------------------------------------------
// Attributes, with defaults omitted (TEXT-PROJECTION 3)
// ---------------------------------------------------------------------------

/// Appends `key value` lines, skipping anything equal to its DDL default.
///
/// The default for every field is passed in at the call site rather than read
/// from the schema, and that is a real coupling: change a DEFAULT in
/// `schema.sql` and this file is wrong until someone notices. It is why the
/// projection header carries a `defaults` generation number -- the coupling is
/// declared in the output instead of hidden here.
class Attrs {
public:
    explicit Attrs(std::vector<std::string>& out) : out_(out) {}

    void num(std::string_view key, std::int64_t v, std::int64_t dflt) {
        if (v != dflt) out_.emplace_back(std::string(key) + " " + std::to_string(v));
    }

    /// Bit comparison, not `!=`. `-0.0 != 0.0` is false, so a value comparison
    /// would omit a gain of -0.0 as "the default" and the projection would lose
    /// a distinction `renderF64` is careful to preserve.
    void real(std::string_view key, double v, double dflt) {
        if (!sameBits(v, dflt)) out_.emplace_back(std::string(key) + " " + renderF64(v));
    }

    /// A true flag renders as its bare name: `muted`, not `muted 1`. Flags are
    /// the most common non-default and the bare form is what makes a diff hunk
    /// readable at a glance.
    void flag(std::string_view key, bool v, bool dflt = false) {
        if (v != dflt) out_.emplace_back(std::string(key));
    }

    void text(std::string_view key, std::string_view v, std::string_view dflt = "") {
        if (v != dflt)
            out_.emplace_back(std::string(key) + " " + renderLabelToken(v));
    }

    void raw(std::string line) { out_.push_back(std::move(line)); }

private:
    static bool sameBits(double a, double b) {
        return std::memcmp(&a, &b, sizeof a) == 0;
    }
    std::vector<std::string>& out_;
};

// ---------------------------------------------------------------------------
// Tree building blocks
// ---------------------------------------------------------------------------

std::size_t addNode(Tree& t, std::string kind, std::string selector,
                    std::string base_label, std::vector<SortKey> keys) {
    Node n;
    n.kind = std::move(kind);
    n.selector = std::move(selector);
    n.base_label = std::move(base_label);
    n.keys = std::move(keys);
    t.nodes.push_back(std::move(n));
    return t.nodes.size() - 1;
}

/// Section rank: the first sort key on every root.
///
/// An addition to TEXT-PROJECTION 7.3, which declares keys per collection and
/// does not contemplate one collection holding several kinds. `project()`
/// orders the roots as ONE collection, so without a leading rank the top-level
/// sections would interleave by whatever their own keys happened to be, and a
/// project would open with its markers. The rank is a constant per kind, so it
/// costs no churn.
enum Section : std::int64_t {
    SecProject = 0,
    SecMap = 1,
    SecMedia = 2,
    SecTrack = 3,
    SecMarker = 4,
    SecRoute = 5,
};

/// `roleRoot` returns `trk`, not `/trk` -- its own doc comment says otherwise,
/// and stripping a leading slash that is not there cost this file a `rk Bass`.
/// Reported to mac rather than edited here; `textproj.*` is theirs.
std::string roleName(std::string_view trackKind) {
    return std::string(roleRoot(roleOfKind(trackKind)));
}

}  // namespace

// ---------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------

std::vector<Meter> metersOf(const std::vector<rows::TimeSignature>& sigs) {
    std::vector<Meter> out;
    out.reserve(sigs.size() + 1);
    // A map with no event at zero has no defined meter before its first one.
    // 4/4 is the assumption, the same one snapshot.cpp makes about a missing
    // initial tempo, and for the same reason: a projector cannot render a
    // warning.
    if (sigs.empty() || sigs.front().posTicks != 0) out.push_back(Meter{});
    for (const auto& s : sigs)
        out.push_back(Meter{s.posTicks,
                            static_cast<std::uint16_t>(s.numerator),
                            static_cast<std::uint16_t>(s.denominator)});
    return out;
}

// ---------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------

std::span<const TableCoverage> coverage() {
    static constexpr std::array<TableCoverage, 37> kTables{{
        // --- projected ------------------------------------------------------
        {"project", Coverage::Projected, ""},
        {"tempo_map", Coverage::Projected, ""},
        {"time_signature_map", Coverage::Projected, ""},
        {"tracks", Coverage::Projected, ""},
        {"mixer_strip", Coverage::Projected, "inlined on its track"},
        {"lanes", Coverage::Projected, ""},
        {"clips", Coverage::Projected, ""},
        {"audio_clips", Coverage::Projected, "inlined on its clip"},
        {"event_streams", Coverage::Projected, "notes decoded; other stream kinds excluded"},
        {"markers", Coverage::Projected, ""},
        {"media_files", Coverage::Projected, "peaks excluded: regenerable, never authoritative"},
        {"routing", Coverage::Projected, ""},

        // --- excluded by TEXT-PROJECTION 9: Layer 3 is not projected at all,
        //     and that exclusion is what makes ADR-0021's oracle possible ----
        {"ops", Coverage::Excluded, "Layer 3"},
        {"op_branches", Coverage::Excluded, "Layer 3"},
        {"session_state", Coverage::Excluded, "Layer 3"},
        {"ui_view", Coverage::Excluded, "Layer 3"},
        {"window_state", Coverage::Excluded, "Layer 3"},
        {"session_lock", Coverage::Excluded, "Layer 3"},
        {"controller_maps", Coverage::Excluded, "Layer 3"},
        {"snapshots", Coverage::Excluded, "Layer 3"},
        {"adi_meta", Coverage::Excluded, "Layer 0 bookkeeping, not project content"},

        // --- excluded pending step 6 ----------------------------------------
        // No op can create any of these, so there is no way to build a fixture
        // except hand-written SQL, and a projection nothing exercises is a
        // projection that is wrong. They arrive with plugin hosting, and
        // ADR-0038 has just changed what plugin_state holds.
        {"plugin_refs", Coverage::Excluded, "step 6; identity inlines on the device"},
        {"devices", Coverage::Excluded, "step 6 (a routing endpoint on one renders unresolved)"},
        {"device_chains", Coverage::Excluded, "step 6"},
        {"plugin_state", Coverage::Excluded, "step 6; ADR-0038 changed its shape"},
        {"plugin_params", Coverage::Excluded, "step 6"},
        {"state_blobs", Coverage::Excluded, "step 6; digest only when it lands (ADR-0038)"},
        {"macros", Coverage::Excluded, "step 6"},
        {"macro_mappings", Coverage::Excluded, "step 6"},

        // --- excluded pending their own ops ---------------------------------
        {"automation_lanes", Coverage::Excluded, "no automation op is implemented"},
        {"automation_data", Coverage::Excluded, "no automation op is implemented"},
        {"note_expression", Coverage::Excluded, "no expression op is implemented"},
        {"arranger_sections", Coverage::Excluded, "no op writes it"},
        {"arranger_chain", Coverage::Excluded, "no op writes it"},
        {"key_map", Coverage::Excluded, "no op writes it"},
        {"media_blobs", Coverage::Excluded, "embedded bytes; the media row carries the flag"},
        {"extensions", Coverage::Excluded, "digest projection not designed"},
    }};
    return kTables;
}

// ---------------------------------------------------------------------------
// buildTree
// ---------------------------------------------------------------------------

Tree buildTree(const rows::Model& m) {
    Tree t;
    const std::vector<Meter> meters = metersOf(m.signatures);

    // --- project header --------------------------------------------------
    {
        const std::size_t n = addNode(t, "project", "", m.project.name,
                                      {SortKey::integer(SecProject)});
        Attrs a(t.nodes[n].attrs);
        a.raw("format 0.1");
        a.raw("schema " + std::to_string(kSchemaMajor) + "." + std::to_string(kSchemaMinor));
        a.raw("ppq " + std::to_string(m.project.ppq));
        a.raw("defaults 1");
        a.num("sampleRate", m.project.sampleRate, 48000);
        a.num("length", m.project.lengthTicks, 0);
        a.text("author", m.project.author);
        a.text("notes", m.project.notes);
        t.roots.push_back(n);
    }

    // --- tempo and signature maps ----------------------------------------
    // Events are attribute lines, not child nodes: they have no identity,
    // nothing references them, and a node per event would make a tempo ramp
    // twice as many lines as it has information.
    if (!m.tempo.empty()) {
        const std::size_t n = addNode(t, "map", "", "tempo",
                                      {SortKey::integer(SecMap), SortKey::integer(0)});
        for (const auto& e : m.tempo) {
            std::string line = renderPosition(e.posTicks, meters) + " bpm " +
                               renderF64(e.bpm);
            if (e.curve != 0) line += " curve " + std::to_string(e.curve);
            if (e.tension != 0.0) line += " tension " + renderF64(e.tension);
            t.nodes[n].attrs.push_back(std::move(line));
        }
        t.roots.push_back(n);
    }
    if (!m.signatures.empty()) {
        const std::size_t n = addNode(t, "map", "", "sig",
                                      {SortKey::integer(SecMap), SortKey::integer(1)});
        for (const auto& s : m.signatures)
            t.nodes[n].attrs.push_back(renderPosition(s.posTicks, meters) + " " +
                                       std::to_string(s.numerator) + "/" +
                                       std::to_string(s.denominator));
        t.roots.push_back(n);
    }

    // --- media pool -------------------------------------------------------
    // Content addressing, because the format already made content the identity
    // of a media file (TEXT-PROJECTION 6.3).
    std::unordered_map<std::int64_t, std::size_t> mediaNode;
    {
        std::vector<std::string> hashes;
        hashes.reserve(m.media.size());
        for (const auto& f : m.media) hashes.push_back(f.hashBlake3);
        const std::vector<std::string> prefixes = mediaPrefixes(hashes);

        for (std::size_t i = 0; i < m.media.size(); ++i) {
            const auto& f = m.media[i];
            const std::size_t n = addNode(
                t, "media", "media", i < prefixes.size() ? prefixes[i] : f.hashBlake3,
                {SortKey::integer(SecMedia), SortKey::text(f.hashBlake3)});
            Attrs a(t.nodes[n].attrs);
            a.text("name", f.origName);
            a.text("format", f.format);
            if (f.sampleRate) a.num("rate", *f.sampleRate, -1);
            if (f.channels) a.num("channels", *f.channels, -1);
            if (f.frames) a.num("frames", *f.frames, -1);
            if (f.bitDepth) a.num("bits", *f.bitDepth, -1);
            if (f.relPath) a.text("path", *f.relPath);
            a.flag("embedded", f.embedded);
            a.flag("missing", f.missing);
            mediaNode[f.id] = n;
            t.roots.push_back(n);
        }
    }

    // --- tracks -----------------------------------------------------------
    std::unordered_map<std::int64_t, std::size_t> trackNode;
    std::unordered_map<std::int64_t, const rows::MixerStrip*> stripOf;
    for (const auto& s : m.strips) stripOf[s.trackId] = &s;

    std::unordered_map<std::int64_t, const rows::Track*> trackById;
    for (const auto& tr : m.tracks) trackById[tr.id] = &tr;

    /// Does following parent_id from `id` reach a root, or does it loop?
    ///
    /// `tracks.parent_id` has no constraint forbidding a cycle -- only
    /// `id <> parent_id`, which stops the one-element case and nothing else.
    /// The pure projector renders a revisited node as `<cycle>` rather than
    /// recursing forever, but it never gets the chance if the TREE BUILDER
    /// loops first, so the guard has to be here as well.
    const auto reachesRoot = [&](std::int64_t id) {
        std::int64_t at = id;
        for (std::size_t steps = 0; steps <= m.tracks.size(); ++steps) {
            const auto it = trackById.find(at);
            if (it == trackById.end()) return true;     // a missing parent is a root
            if (!it->second->parentId) return true;
            at = *it->second->parentId;
        }
        return false;
    };

    for (const auto& tr : m.tracks) {
        const bool nested = tr.parentId.has_value() && trackById.count(*tr.parentId) != 0 &&
                            reachesRoot(tr.id);
        const std::string role = roleName(tr.kind);

        std::vector<SortKey> keys;
        if (!nested) keys.push_back(SortKey::integer(SecTrack));
        keys.push_back(SortKey::integer(tr.indexInParent));
        keys.push_back(SortKey::text(tr.kind));
        keys.push_back(SortKey::text(tr.name));

        // The selector is the role space only at top level. A nested track is
        // already addressed by the hierarchy, so repeating `trk` at every depth
        // would double the length of every designator for no information.
        const std::size_t n =
            addNode(t, role, nested ? "" : role, tr.name, std::move(keys));
        trackNode[tr.id] = n;

        Attrs a(t.nodes[n].attrs);
        // The role is already the block keyword, so `ret Reverb` need not also
        // say `kind return`. Only the generic `trk` space carries several kinds.
        if (role == "trk") a.text("kind", tr.kind, "audio");
        a.num("color", tr.color.value_or(-1), -1);
        a.num("timeBase", tr.timeBase, 0);
        a.flag("muted", tr.muted);
        a.flag("soloed", tr.soloed);
        a.flag("soloDefeat", tr.soloDefeat);
        a.flag("armed", tr.recordArmed);
        a.flag("locked", tr.locked);
        a.flag("frozen", tr.frozen);
        a.num("monitor", tr.monitorMode, 0);
        a.num("automation", tr.automationMode, 0);
        if (tr.inputRef) a.text("input", *tr.inputRef);

        if (const auto it = stripOf.find(tr.id); it != stripOf.end()) {
            const rows::MixerStrip& s = *it->second;
            a.real("vol", s.volumeDb, 0.0);
            a.real("pan", s.pan, 0.0);
            a.real("width", s.width, 1.0);
            a.real("inGain", s.inputGainDb, 0.0);
            a.num("panLaw", s.panLaw, 0);
            a.num("delay", s.delaySamples, 0);
            a.flag("phaseInvert", s.phaseInvert);
        }
    }

    // Hierarchy, in a second pass so every node index exists.
    for (const auto& tr : m.tracks) {
        const std::size_t n = trackNode[tr.id];
        if (!tr.parentId) { t.roots.push_back(n); continue; }
        const auto parent = trackNode.find(*tr.parentId);
        if (parent == trackNode.end() || !reachesRoot(tr.id)) {
            // A dangling or cyclic parent. Attach at top level rather than drop:
            // a corrupt file must still project, and a track that vanished from
            // the output is a diff that says it was deleted.
            t.roots.push_back(n);
            continue;
        }
        t.nodes[parent->second].children.push_back(n);
    }

    // --- lanes ------------------------------------------------------------
    std::unordered_map<std::int64_t, std::size_t> laneNode;
    for (const auto& l : m.lanes) {
        const auto owner = trackNode.find(l.trackId);
        if (owner == trackNode.end()) continue;
        const std::size_t n = addNode(t, "lane", "lane", l.name,
                                      {SortKey::integer(l.ord), SortKey::text(l.kind),
                                       SortKey::text(l.name)});
        Attrs a(t.nodes[n].attrs);
        a.text("kind", l.kind, "take");
        a.flag("muted", l.muted);
        a.flag("compTarget", l.isCompTarget);
        laneNode[l.id] = n;
        t.nodes[owner->second].children.push_back(n);
    }

    // --- clips ------------------------------------------------------------
    std::unordered_map<std::int64_t, std::size_t> clipNode;
    std::unordered_map<std::int64_t, const rows::AudioClip*> audioOf;
    for (const auto& a : m.audioClips) audioOf[a.clipId] = &a;

    // Notes render clip-relative (TEXT-PROJECTION 8), under the signature in
    // effect at the clip's start held constant for the clip's whole length.
    // Absolute glosses would rewrite every note line when the clip moves, and a
    // clip-relative gloss that still followed the global map would rewrite them
    // when an unrelated meter changed. Both defeat the point.
    const auto sigAt = [&](std::int64_t ticks) {
        std::vector<Meter> one{Meter{}};
        for (const auto& s : m.signatures) {
            if (s.posTicks > ticks) break;
            one[0] = Meter{0, static_cast<std::uint16_t>(s.numerator),
                           static_cast<std::uint16_t>(s.denominator)};
        }
        return one;
    };

    for (const auto& c : m.clips) {
        std::size_t parent;
        if (c.laneId && laneNode.count(*c.laneId)) {
            parent = laneNode[*c.laneId];
        } else {
            const auto owner = trackNode.find(c.trackId);
            if (owner == trackNode.end()) continue;
            parent = owner->second;
        }

        const std::size_t n =
            addNode(t, "clip", "clip", c.name,
                    {SortKey::integer(c.posTicks.value_or(c.posNs.value_or(0))),
                     SortKey::integer(c.lengthTicks.value_or(c.lengthNs.value_or(0))),
                     SortKey::text(c.kind), SortKey::text(c.name)});
        clipNode[c.id] = n;

        Attrs a(t.nodes[n].attrs);
        if (c.timeBase == 0)
            a.raw("at " + renderPosition(c.posTicks.value_or(0), meters));
        else
            a.raw("at " + std::to_string(c.posNs.value_or(0)) + "ns");
        if (c.lengthTicks) a.raw("len " + renderDuration(*c.lengthTicks));
        else if (c.lengthNs) a.raw("len " + std::to_string(*c.lengthNs) + "ns");
        a.text("kind", c.kind, "audio");
        a.num("color", c.color.value_or(-1), -1);
        a.real("gain", c.gainDb, 0.0);
        a.flag("muted", c.muted);
        if (c.fadeInTicks) a.raw("fadeIn " + renderDuration(c.fadeInTicks));
        if (c.fadeOutTicks) a.raw("fadeOut " + renderDuration(c.fadeOutTicks));
        a.num("fadeInCurve", c.fadeInCurve, 1);
        a.num("fadeOutCurve", c.fadeOutCurve, 1);
        a.flag("loop", c.loopEnabled);
        if (c.loopStartTicks) a.raw("loopStart " + renderDuration(c.loopStartTicks));
        if (c.loopLenTicks) a.raw("loopLen " + renderDuration(*c.loopLenTicks));
        if (c.contentOffsetTicks)
            a.raw("offset " + renderDuration(c.contentOffsetTicks));

        if (const auto it = audioOf.find(c.id); it != audioOf.end()) {
            const rows::AudioClip& ac = *it->second;
            a.num("srcStart", ac.srcStartFrames, 0);
            a.num("srcLen", ac.srcLenFrames, -1);
            a.flag("warp", ac.warpEnabled);
            a.text("warpMode", ac.warpMode, "none");
            a.real("transpose", ac.transposeSemis, 0.0);
            a.real("formant", ac.formantShift, 0.0);
            a.flag("reverse", ac.reverse);
            a.num("channelMode", ac.channelMode, 0);
        }

        t.nodes[parent].children.push_back(n);
    }

    // --- notes ------------------------------------------------------------
    {
        std::unordered_map<std::int64_t, const rows::Clip*> clipById;
        for (const auto& c : m.clips) clipById[c.id] = &c;

        for (const auto& note : m.notes) {
            const auto owner = clipNode.find(note.clipId);
            if (owner == clipNode.end()) continue;
            const auto cl = clipById.find(note.clipId);
            const std::int64_t clipStart =
                cl != clipById.end() ? cl->second->posTicks.value_or(0) : 0;

            const std::size_t n =
                addNode(t, "note", "", "",
                        {SortKey::integer(note.startTicks), SortKey::integer(note.key),
                         SortKey::integer(note.channel), SortKey::integer(note.durTicks),
                         SortKey::integer(note.velOn)});
            Attrs a(t.nodes[n].attrs);
            a.raw("at " + renderPosition(note.startTicks, sigAt(clipStart)) + " key " +
                  std::to_string(note.key) + " dur " + renderDuration(note.durTicks) +
                  " vel " + std::to_string(note.velOn));
            a.num("velOff", note.velOff, 64);   // ops_catalog.cpp: the op default
            a.num("chan", note.channel, 0);
            a.num("prob", note.probability, 10000);
            a.real("cents", note.tuningCents, 0.0);
            // HasExpression is derived from whether an expression block is
            // present, and TEXT-PROJECTION 3 forbids emitting a derived field.
            a.num("flags", note.flags & ~NoteFlags::HasExpression, 0);
            t.nodes[owner->second].children.push_back(n);
        }
    }

    // --- markers ----------------------------------------------------------
    for (const auto& k : m.markers) {
        const std::size_t n =
            addNode(t, "mark", "mark", k.name,
                    {SortKey::integer(SecMarker),
                     SortKey::integer(k.posTicks.value_or(k.posNs.value_or(0))),
                     SortKey::text(k.kind), SortKey::text(k.name)});
        Attrs a(t.nodes[n].attrs);
        if (k.timeBase == 0)
            a.raw("at " + renderPosition(k.posTicks.value_or(0), meters));
        else
            a.raw("at " + std::to_string(k.posNs.value_or(0)) + "ns");
        if (k.lengthTicks) a.raw("len " + renderDuration(*k.lengthTicks));
        a.text("kind", k.kind, "position");
        a.num("color", k.color.value_or(-1), -1);
        t.roots.push_back(n);
    }

    // --- routing ----------------------------------------------------------
    // The one collection whose K1 (TEXT-PROJECTION 7.3) is "src designator, dst
    // designator, kind" -- which cannot be a sort key, because designators are
    // only determined after ordering. Kind and ordinal are the available keys;
    // K3 refinement then separates rows through their endpoint edges, which is
    // the mechanism the ordering chain has for exactly this.
    for (const auto& r : m.routing) {
        const std::size_t n = addNode(t, "route", "route", r.kind,
                                      {SortKey::integer(SecRoute), SortKey::text(r.kind),
                                       SortKey::integer(r.ord)});
        Attrs a(t.nodes[n].attrs);
        a.real("gain", r.gainDb, 0.0);
        a.real("pan", r.pan, 0.0);
        a.flag("preFader", r.preFader);
        a.flag("disabled", !r.enabled);

        // A hardware port is not a row and never was (SPEC 6.7). Rendering it
        // as `!unresolved(hw_in)` would report a healthy project as broken, so
        // it is an attribute and only track and device endpoints are refs.
        const auto endpoint = [&](std::string_view role, const std::string& kind,
                                  std::int64_t id) {
            if (kind == "hw_in" || kind == "hw_out") {
                a.raw(std::string(role) + " " + kind + ":" + std::to_string(id));
                return;
            }
            const auto at = trackNode.find(id);
            const bool found = kind == "track" && at != trackNode.end();
            t.nodes[n].refs.push_back(
                Ref{std::string(role), found ? at->second : Ref::kDangling, kind});
        };
        endpoint("from", r.srcKind, r.srcId);
        endpoint("to", r.dstKind, r.dstId);

        t.roots.push_back(n);
    }

    // --- the references that survived containment (TEXT-PROJECTION 2) ------
    for (const auto& tr : m.tracks) {
        const std::size_t n = trackNode[tr.id];
        if (const auto it = stripOf.find(tr.id);
            it != stripOf.end() && it->second->vcaGroupId) {
            const auto target = trackNode.find(*it->second->vcaGroupId);
            t.nodes[n].refs.push_back(Ref{
                "vca", target == trackNode.end() ? Ref::kDangling : target->second,
                "track"});
        }
        if (tr.freezeMediaId) {
            const auto target = mediaNode.find(*tr.freezeMediaId);
            t.nodes[n].refs.push_back(Ref{
                "freeze", target == mediaNode.end() ? Ref::kDangling : target->second,
                "media"});
        }
    }
    for (const auto& c : m.clips) {
        const auto self = clipNode.find(c.id);
        if (self == clipNode.end()) continue;
        if (c.aliasOf) {
            const auto target = clipNode.find(*c.aliasOf);
            t.nodes[self->second].refs.push_back(Ref{
                "alias", target == clipNode.end() ? Ref::kDangling : target->second,
                "clip"});
        }
    }
    for (const auto& ac : m.audioClips) {
        const auto self = clipNode.find(ac.clipId);
        if (self == clipNode.end()) continue;
        const auto target = mediaNode.find(ac.mediaId);
        t.nodes[self->second].refs.push_back(Ref{
            "media", target == mediaNode.end() ? Ref::kDangling : target->second,
            "media"});
    }

    return t;
}

Projection projectStore(const Store& store) {
    return project(buildTree(rows::readModel(store)));
}

}  // namespace adi::textproj
