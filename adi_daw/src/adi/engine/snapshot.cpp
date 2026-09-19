// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/snapshot.hpp"

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <map>

namespace adi::engine {
namespace {

constexpr double kTicksPerQuarter = 5765760.0;   // ADI_PPQ, SPEC §4.2

/// Do two clips hold the same values? Used to decide whether a node can be
/// reused. Comparing content rather than tracking dirty flags is deliberate: a
/// dirty flag is a second source of truth about what changed, and it is wrong
/// the first time someone forgets to set it.
bool same(const ClipNode& a, const ClipNode& b) {
    return a.id == b.id && a.posTicks == b.posTicks && a.lengthTicks == b.lengthTicks &&
           a.muted == b.muted && a.gainDb == b.gainDb;
}

bool same(const TrackNode& a, const TrackNode& b) {
    if (a.id != b.id || a.name != b.name || a.muted != b.muted || a.soloed != b.soloed ||
        a.volumeDb != b.volumeDb || a.pan != b.pan || a.clips.size() != b.clips.size())
        return false;
    for (std::size_t i = 0; i < a.clips.size(); ++i)
        if (a.clips[i] != b.clips[i]) return false;   // pointer identity: already shared
    return true;
}

}  // namespace

// --- tempo ---------------------------------------------------------------------

double TempoMap::bpmAt(std::int64_t ticks) const noexcept {
    if (events.empty()) return 120.0;
    double bpm = events.front().bpm;
    for (const auto& e : events) {
        if (e.posTicks > ticks) break;
        bpm = e.bpm;
    }
    return bpm;
}

double TempoMap::ticksToSeconds(std::int64_t ticks) const noexcept {
    if (ticks <= 0 || events.empty()) return 0.0;

    // Integrate segment by segment. A tempo map is a piecewise-constant function
    // of tick position, so the honest conversion is a sum over segments, not
    // ticks * (60 / bpm / ppq) with one bpm -- which is the bug every DAW has
    // shipped at least once and which only shows up after a tempo change.
    double seconds = 0.0;
    std::int64_t at = 0;
    double bpm = events.front().bpm;

    for (const auto& e : events) {
        if (e.posTicks >= ticks) break;
        if (e.posTicks > at) {
            const double quarters = static_cast<double>(e.posTicks - at) / kTicksPerQuarter;
            seconds += quarters * 60.0 / bpm;
            at = e.posTicks;
        }
        bpm = e.bpm;
    }
    const double quarters = static_cast<double>(ticks - at) / kTicksPerQuarter;
    return seconds + quarters * 60.0 / bpm;
}

std::int64_t TempoMap::secondsToTicks(double seconds) const noexcept {
    if (seconds <= 0.0 || events.empty()) return 0;

    double elapsed = 0.0;
    std::int64_t at = 0;
    double bpm = events.front().bpm;

    for (const auto& e : events) {
        if (e.posTicks <= at) { bpm = e.bpm; continue; }
        const double quarters = static_cast<double>(e.posTicks - at) / kTicksPerQuarter;
        const double segment = quarters * 60.0 / bpm;
        if (elapsed + segment >= seconds) {
            const double into = (seconds - elapsed) * bpm / 60.0;
            return at + static_cast<std::int64_t>(into * kTicksPerQuarter);
        }
        elapsed += segment;
        at = e.posTicks;
        bpm = e.bpm;
    }
    const double into = (seconds - elapsed) * bpm / 60.0;
    return at + static_cast<std::int64_t>(into * kTicksPerQuarter);
}

// --- snapshot -------------------------------------------------------------------

const TrackNode* Snapshot::findTrack(std::int64_t id) const noexcept {
    for (const auto& t : tracks)
        if (t->id == id) return t.get();
    return nullptr;
}

bool Snapshot::anySoloed() const noexcept {
    for (const auto& t : tracks)
        if (t->soloed) return true;
    return false;
}

// --- building --------------------------------------------------------------------

std::unique_ptr<Snapshot> SnapshotBuilder::fromStore(const Store& store) {
    Snapshot empty;
    return fromStore(store, empty);
}

std::unique_ptr<Snapshot> SnapshotBuilder::fromStore(const Store& store,
                                                     const Snapshot& previous) {
    auto snap = std::make_unique<Snapshot>();
    auto& db = const_cast<SQLite::Database&>(store.db());

    // Index the previous snapshot by id so reuse is a lookup rather than a scan.
    std::map<std::int64_t, std::shared_ptr<const TrackNode>> prevTracks;
    std::map<std::int64_t, std::shared_ptr<const ClipNode>> prevClips;
    for (const auto& t : previous.tracks) {
        prevTracks[t->id] = t;
        for (const auto& c : t->clips) prevClips[c->id] = c;
    }

    try {
        snap->sampleRate = db.execAndGet("SELECT sample_rate FROM project WHERE id=1").getInt();
    } catch (const std::exception&) {
        snap->sampleRate = 48000;
    }

    // --- tempo -----------------------------------------------------------------
    {
        auto tempo = std::make_shared<TempoMap>();
        try {
            SQLite::Statement st(db,
                "SELECT pos_ticks, bpm, curve FROM tempo_map ORDER BY pos_ticks");
            while (st.executeStep())
                tempo->events.push_back({st.getColumn(0).getInt64(),
                                         st.getColumn(1).getDouble(),
                                         st.getColumn(2).getInt()});
        } catch (const std::exception&) {
        }
        // A map with no event at 0 has no defined tempo before its first one
        // (adi_tool check warns about it). The engine cannot render a warning,
        // so it assumes 120 from the start rather than dividing by nothing.
        if (tempo->events.empty() || tempo->events.front().posTicks != 0)
            tempo->events.insert(tempo->events.begin(), {0, 120.0, 0});

        if (previous.tempo && previous.tempo->events == tempo->events)
            snap->tempo = previous.tempo;      // shared
        else
            snap->tempo = std::move(tempo);
    }

    // --- clips, grouped by track -------------------------------------------------
    std::map<std::int64_t, std::vector<std::shared_ptr<const ClipNode>>> clipsByTrack;
    try {
        SQLite::Statement st(db,
            "SELECT id, track_id, IFNULL(pos_ticks,0), IFNULL(length_ticks,0), muted, "
            "gain_db FROM clips WHERE track_id IS NOT NULL ORDER BY track_id, pos_ticks, id");
        while (st.executeStep()) {
            ClipNode c;
            c.id = st.getColumn(0).getInt64();
            const auto trackId = st.getColumn(1).getInt64();
            c.posTicks = st.getColumn(2).getInt64();
            c.lengthTicks = st.getColumn(3).getInt64();
            c.muted = st.getColumn(4).getInt() != 0;
            c.gainDb = static_cast<float>(st.getColumn(5).getDouble());

            const auto prev = prevClips.find(c.id);
            if (prev != prevClips.end() && same(*prev->second, c))
                clipsByTrack[trackId].push_back(prev->second);        // shared
            else
                clipsByTrack[trackId].push_back(std::make_shared<const ClipNode>(c));
        }
    } catch (const std::exception&) {
    }

    // --- tracks --------------------------------------------------------------------
    try {
        SQLite::Statement st(db,
            "SELECT t.id, t.name, t.muted, t.soloed, "
            "  IFNULL(m.volume_db, 0.0), IFNULL(m.pan, 0.0) "
            "FROM tracks t LEFT JOIN mixer_strip m ON m.track_id = t.id "
            "ORDER BY t.index_in_parent, t.id");
        while (st.executeStep()) {
            TrackNode t;
            t.id = st.getColumn(0).getInt64();
            t.name = st.getColumn(1).getString();
            t.muted = st.getColumn(2).getInt() != 0;
            t.soloed = st.getColumn(3).getInt() != 0;
            t.volumeDb = static_cast<float>(st.getColumn(4).getDouble());
            t.pan = static_cast<float>(st.getColumn(5).getDouble());
            if (auto it = clipsByTrack.find(t.id); it != clipsByTrack.end())
                t.clips = it->second;

            const auto prev = prevTracks.find(t.id);
            if (prev != prevTracks.end() && same(*prev->second, t))
                snap->tracks.push_back(prev->second);                 // shared
            else
                snap->tracks.push_back(std::make_shared<const TrackNode>(std::move(t)));
        }
    } catch (const std::exception&) {
    }

    return snap;
}

std::size_t SnapshotBuilder::sharedNodeCount(const Snapshot& a, const Snapshot& b) {
    std::size_t shared = 0;
    if (a.tempo && a.tempo == b.tempo) ++shared;
    for (const auto& ta : a.tracks) {
        for (const auto& tb : b.tracks) {
            if (ta == tb) { ++shared; break; }
        }
        for (const auto& ca : ta->clips) {
            for (const auto& tb : b.tracks)
                if (std::find(tb->clips.begin(), tb->clips.end(), ca) != tb->clips.end()) {
                    ++shared;
                    break;
                }
        }
    }
    return shared;
}

}  // namespace adi::engine
