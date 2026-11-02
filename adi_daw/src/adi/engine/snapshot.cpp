// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/snapshot.hpp"

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <map>
#include <set>
#include <sqlite3.h>

namespace adi::engine {
namespace {
bool same(const SendRoute& a, const SendRoute& b) {
    return a.id==b.id && a.sourceKind==b.sourceKind && a.destinationKind==b.destinationKind &&
        a.sourceId==b.sourceId && a.destinationId==b.destinationId && a.order==b.order &&
        a.gainDb==b.gainDb && a.pan==b.pan && a.preFader==b.preFader && a.enabled==b.enabled;
}

constexpr double kTicksPerQuarter = 5765760.0;   // ADI_PPQ, SPEC §4.2

/// Do two clips hold the same values? Used to decide whether a node can be
/// reused. Comparing content rather than tracking dirty flags is deliberate: a
/// dirty flag is a second source of truth about what changed, and it is wrong
/// the first time someone forgets to set it.
bool same(const ClipNode& a, const ClipNode& b) {
    return a.id == b.id && a.name == b.name && a.posTicks == b.posTicks && a.lengthTicks == b.lengthTicks &&
           a.muted == b.muted && a.gainDb == b.gainDb;
}

bool same(const TrackNode& a, const TrackNode& b) {
    if (a.id != b.id || a.name != b.name || a.parent != b.parent || a.indexInParent != b.indexInParent ||
        a.kind != b.kind || a.color != b.color || a.muted != b.muted || a.soloed != b.soloed ||
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
    // A frame and its anchor owners must come from one WAL state. Reuse a
    // caller's read/write transaction rather than nesting or committing it.
    std::optional<SQLite::Transaction> readSnapshot;
    if(sqlite3_get_autocommit(db.getHandle())!=0)
        readSnapshot.emplace(db,SQLite::TransactionBehavior::DEFERRED);

    // Index the previous snapshot by id so reuse is a lookup rather than a scan.
    std::map<std::int64_t, std::shared_ptr<const TrackNode>> prevTracks;
    std::map<std::int64_t, std::shared_ptr<const ClipNode>> prevClips;
    std::map<std::int64_t, std::shared_ptr<const SendRoute>> prevSends;
    for (const auto& send : previous.sends) prevSends[send->id] = send;
    for (const auto& t : previous.tracks) {
        prevTracks[t->id] = t;
        for (const auto& c : t->clips) prevClips[c->id] = c;
    }

    try {
        snap->sampleRate = db.execAndGet("SELECT sample_rate FROM project WHERE id=1").getInt();
    } catch (const std::exception&) {
        snap->sampleRate = 44100;
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

    // --- time signatures --------------------------------------------------------
    {
        SQLite::Statement st(db,
            "SELECT pos_ticks, numerator, denominator FROM time_signature_map ORDER BY pos_ticks, id");
        while (st.executeStep())
            snap->meters.push_back({st.getColumn(0).getInt64(),
                                    static_cast<std::uint16_t>(st.getColumn(1).getInt()),
                                    static_cast<std::uint16_t>(st.getColumn(2).getInt())});
        // Match textproj_store: a late first event must not reset bar numbering.
        if (snap->meters.empty() || snap->meters.front().start_ticks != 0)
            snap->meters.insert(snap->meters.begin(), {0, 4, 4});
    }

    // --- clips, grouped by track -------------------------------------------------
    std::map<std::int64_t, std::vector<std::shared_ptr<const ClipNode>>> clipsByTrack;
    try {
        SQLite::Statement st(db,
            "SELECT id, track_id, IFNULL(pos_ticks,0), IFNULL(length_ticks,0), muted, "
            "gain_db, name FROM clips WHERE track_id IS NOT NULL ORDER BY track_id, pos_ticks, id");
        while (st.executeStep()) {
            ClipNode c;
            c.id = st.getColumn(0).getInt64();
            const auto trackId = st.getColumn(1).getInt64();
            c.posTicks = st.getColumn(2).getInt64();
            c.lengthTicks = st.getColumn(3).getInt64();
            c.muted = st.getColumn(4).getInt() != 0;
            c.gainDb = static_cast<float>(st.getColumn(5).getDouble());
            c.name = st.getColumn(6).getString();

            const auto prev = prevClips.find(c.id);
            if (prev != prevClips.end() && same(*prev->second, c))
                clipsByTrack[trackId].push_back(prev->second);        // shared
            else
                clipsByTrack[trackId].push_back(std::make_shared<const ClipNode>(c));
        }
    } catch (const std::exception&) {
    }

    // --- tracks --------------------------------------------------------------------
    // This deterministic flat traversal is not a display order. The window
    // orders the tree using parent and indexInParent from the snapshot.
    try {
        SQLite::Statement st(db,
            "SELECT t.id, t.name, t.muted, t.soloed, "
            "  IFNULL(m.volume_db, 0.0), IFNULL(m.pan, 0.0), "
            "  IFNULL(t.parent_id, 0), t.index_in_parent, t.kind, t.color "
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
            t.parent = st.getColumn(6).getInt64();
            t.indexInParent = st.getColumn(7).getInt64();
            t.kind = st.getColumn(8).getString();
            if (!st.getColumn(9).isNull()) t.color = st.getColumn(9).getInt64();
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

    // --- sends ------------------------------------------------------------------
    // Snapshot construction is off the audio thread; every control value is
    // captured once for this rebuild, including muted/disabled send controls.
    try {
        SQLite::Statement st(db,
            "SELECT id,src_kind,src_id,dst_kind,dst_id,ord,gain_db,pan,pre_fader,enabled "
            "FROM routing WHERE kind='send' ORDER BY id");
        while (st.executeStep()) {
            SendRoute send;
            send.id=st.getColumn(0).getInt64();
            send.sourceKind=st.getColumn(1).getString();send.sourceId=st.getColumn(2).getInt64();
            send.destinationKind=st.getColumn(3).getString();send.destinationId=st.getColumn(4).getInt64();
            send.order=st.getColumn(5).getInt64();send.gainDb=st.getColumn(6).getDouble();
            send.pan=st.getColumn(7).getDouble();send.preFader=st.getColumn(8).getInt()!=0;
            send.enabled=st.getColumn(9).getInt()!=0;
            const auto prior=prevSends.find(send.id);
            if(prior!=prevSends.end() && same(*prior->second,send))snap->sends.push_back(prior->second);
            else snap->sends.push_back(std::make_shared<const SendRoute>(std::move(send)));
        }
    } catch (const std::exception&) {
    }
    // Most projects have no remarks: avoid reading device ownership at all.
    try {if(db.execAndGet("SELECT EXISTS(SELECT 1 FROM remarks)").getInt()==0)return snap;}
    catch(const std::exception&){return snap;}
    // Resolve weak remark anchors from the same read snapshot. Build small
    // ownership maps once; no per-device filesystem/host/state queries.
    std::map<std::int64_t,std::int64_t> clipTrack,deviceChain;
    struct ChainOwner {std::optional<std::int64_t> track,parentDevice;};
    std::map<std::int64_t,ChainOwner> chainOwner;
    try {
        SQLite::Statement clips(db,"SELECT id,track_id FROM clips");
        while(clips.executeStep())clipTrack.emplace(clips.getColumn(0).getInt64(),clips.getColumn(1).getInt64());
        SQLite::Statement devices(db,"SELECT id,chain_id FROM devices");
        while(devices.executeStep())deviceChain.emplace(devices.getColumn(0).getInt64(),devices.getColumn(1).getInt64());
        SQLite::Statement chains(db,"SELECT id,track_id,parent_device_id FROM device_chains");
        while(chains.executeStep()) {
            ChainOwner owner;
            if(!chains.getColumn(1).isNull())owner.track=chains.getColumn(1).getInt64();
            if(!chains.getColumn(2).isNull())owner.parentDevice=chains.getColumn(2).getInt64();
            chainOwner.emplace(chains.getColumn(0).getInt64(),owner);
        }
    }catch(const std::exception&){}
    const auto deviceTrack=[&](std::int64_t deviceId)->std::optional<std::int64_t> {
        std::set<std::int64_t> visited;
        for(;;) {
            const auto device=deviceChain.find(deviceId);if(device==deviceChain.end())return {};
            if(!visited.insert(device->second).second)return {};
            const auto chain=chainOwner.find(device->second);if(chain==chainOwner.end())return {};
            const auto& owner=chain->second;
            if(owner.track && !owner.parentDevice)return owner.track;
            if(owner.track || !owner.parentDevice)return {};
            deviceId=*owner.parentDevice;
        }
    };
    std::map<std::int64_t,std::shared_ptr<const RemarkNode>> previousRemarks;
    for(const auto& remark:previous.remarks)previousRemarks.emplace(remark->id,remark);
    try {
        SQLite::Statement query(db,"SELECT id,target_kind,target_id,param_id,author,actor_detail,text,created_utc,resolved FROM remarks ORDER BY id");
        while(query.executeStep()) {
            RemarkNode remark;remark.id=query.getColumn(0).getInt64();remark.targetKind=query.getColumn(1).getString();
            remark.targetId=query.getColumn(2).getInt64();if(!query.getColumn(3).isNull())remark.paramId=query.getColumn(3).getString();
            remark.author=query.getColumn(4).getString();remark.actorDetail=query.getColumn(5).getString();remark.text=query.getColumn(6).getString();
            remark.createdUtc=query.getColumn(7).getInt64();remark.resolved=query.getColumn(8).getInt()!=0;
            if(remark.targetKind=="track")remark.trackId=remark.targetId;
            else if(remark.targetKind=="clip") {
                const auto owner=clipTrack.find(remark.targetId);if(owner!=clipTrack.end())remark.trackId=owner->second;
            }else if(remark.targetKind=="device")remark.trackId=deviceTrack(remark.targetId);
            if(remark.trackId && !snap->findTrack(*remark.trackId))remark.trackId.reset();
            remark.anchorMissing=!remark.trackId;
            const auto old=previousRemarks.find(remark.id);
            if(old!=previousRemarks.end() && *old->second==remark)snap->remarks.push_back(old->second);
            else snap->remarks.push_back(std::make_shared<const RemarkNode>(std::move(remark)));
        }
    }catch(const std::exception&){}
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
    for (const auto& sa : a.sends)
        if (std::find(b.sends.begin(),b.sends.end(),sa)!=b.sends.end())++shared;
    for(const auto& remark:a.remarks)
        if(std::find(b.remarks.begin(),b.remarks.end(),remark)!=b.remarks.end())++shared;
    return shared;
}

}  // namespace adi::engine
