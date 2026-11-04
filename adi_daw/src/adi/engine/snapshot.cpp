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
           a.muted == b.muted && a.gainDb == b.gainDb && a.fadeInTicks==b.fadeInTicks &&
           a.fadeOutTicks==b.fadeOutTicks && a.fadeInCurve==b.fadeInCurve && a.fadeOutCurve==b.fadeOutCurve &&
           a.fadeInTension==b.fadeInTension && a.fadeOutTension==b.fadeOutTension &&
           a.contentOffsetNs==b.contentOffsetNs && a.loopBeginNs==b.loopBeginNs &&
           a.loopEndNs==b.loopEndNs && a.sourceEndNs==b.sourceEndNs && a.fadeClockNs==b.fadeClockNs && a.fadeClockTicks==b.fadeClockTicks;
}

bool same(const TrackNode& a, const TrackNode& b) {
    if (a.id != b.id || a.name != b.name || a.parent != b.parent || a.indexInParent != b.indexInParent ||
        a.kind != b.kind || a.recording != b.recording || a.color != b.color || a.muted != b.muted || a.soloed != b.soloed ||
        a.volumeDb != b.volumeDb || a.pan != b.pan || a.panMode != b.panMode ||
        a.panLeft != b.panLeft || a.panRight != b.panRight || a.delaySamples != b.delaySamples ||
        a.clips.size() != b.clips.size())
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

const MarkerNode* Snapshot::findMarker(std::int64_t id) const noexcept {
    for (const auto& marker : markers)
        if (marker->id == id) return marker.get();
    return nullptr;
}

bool Snapshot::anySoloed() const noexcept {
    for (const auto& t : tracks)
        if (t->soloed) return true;
    return false;
}

const AutomationLaneNode* Snapshot::findAutomationLane(std::int64_t id) const noexcept {
    for (const auto& lane : automationLanes)
        if (lane->id == id) return lane.get();
    return nullptr;
}

// --- building --------------------------------------------------------------------

std::unique_ptr<Snapshot> SnapshotBuilder::fromStore(const Store& store,
                                                    const std::set<std::int64_t>& overridden) {
    Snapshot empty;
    return fromStore(store, empty, overridden);
}

std::unique_ptr<Snapshot> SnapshotBuilder::fromStore(const Store& store,
                                                     const Snapshot& previous,
                                                     const std::set<std::int64_t>& overridden) {
    auto snap = std::make_unique<Snapshot>();
    snap->transport=readTransportSettings(store);
    auto& db = const_cast<SQLite::Database&>(store.db());
    snap->automationLanes = detail::readAutomationSnapshot(db, previous.automationLanes, overridden);
    snap->automationOverridden = !overridden.empty();
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

    // --- marker/locator metadata ------------------------------------------------
    {
        std::map<std::int64_t, std::shared_ptr<const MarkerNode>> prior;
        for (const auto& marker : previous.markers) prior[marker->id] = marker;
        SQLite::Statement q(db, "SELECT m.id,m.time_base,m.pos_ticks,m.pos_ns,m.length_ticks,m.length_ns,m.name,m.color,m.kind,"
            "COALESCE(d.note,''),COALESCE(d.prompt,'') FROM markers m LEFT JOIN marker_details d ON d.marker_id=m.id ORDER BY m.id");
        while (q.executeStep()) {
            MarkerNode marker;
            marker.id = q.getColumn(0).getInt64(); marker.timeBase = q.getColumn(1).getInt64();
            auto integer = [&](int column) -> std::optional<std::int64_t> {
                if (q.getColumn(column).isNull()) return std::nullopt;
                return q.getColumn(column).getInt64();
            };
            marker.posTicks = integer(2); marker.posNs = integer(3);
            marker.lengthTicks = integer(4); marker.lengthNs = integer(5);
            marker.name = q.getColumn(6).getString(); marker.color = integer(7);
            marker.kind = q.getColumn(8).getString(); marker.note = q.getColumn(9).getString();
            marker.prompt = q.getColumn(10).getString();
            const auto found = prior.find(marker.id);
            if (found != prior.end() && *found->second == marker) snap->markers.push_back(found->second);
            else snap->markers.push_back(std::make_shared<const MarkerNode>(std::move(marker)));
        }
    }

    struct FadeShape {std::optional<double> in,out;std::optional<std::int64_t> offset,loopBegin,loopLength,end;std::array<std::optional<std::int64_t>,4> clock{},warpClock{};};
    std::map<std::int64_t,FadeShape> fadeShapes;
    try {
        SQLite::Statement st(db,"SELECT clip_id,in_tension,out_tension,content_offset_ns,loop_begin_ns,loop_end_ns,source_end_ns,in_begin_ns,in_end_ns,out_begin_ns,out_end_ns FROM clip_playback_details");
        while(st.executeStep()) {
            auto& shape=fadeShapes[st.getColumn(0).getInt64()];
            if(!st.getColumn(1).isNull())shape.in=st.getColumn(1).getDouble();
            if(!st.getColumn(2).isNull())shape.out=st.getColumn(2).getDouble();
            if(!st.getColumn(3).isNull())shape.offset=st.getColumn(3).getInt64();
            if(!st.getColumn(4).isNull())shape.loopBegin=st.getColumn(4).getInt64();
            if(!st.getColumn(5).isNull())shape.loopLength=st.getColumn(5).getInt64();
            if(!st.getColumn(6).isNull())shape.end=st.getColumn(6).getInt64();
            for(std::size_t i=0;i<4;++i)if(!st.getColumn(7+static_cast<int>(i)).isNull())shape.clock[i]=st.getColumn(7+static_cast<int>(i)).getInt64();
        }
    } catch(const std::exception&) {}

    try {
        SQLite::Statement st(db,"SELECT clip_id,in_begin_ticks,in_end_ticks,out_begin_ticks,out_end_ticks FROM clip_warp_fade_domains");
        while(st.executeStep()) {
            auto& shape=fadeShapes[st.getColumn(0).getInt64()];
            for(std::size_t i=0;i<4;++i)if(!st.getColumn(1+static_cast<int>(i)).isNull())shape.warpClock[i]=st.getColumn(1+static_cast<int>(i)).getInt64();
        }
    } catch(const std::exception&) {}

    // --- clips, grouped by track -------------------------------------------------
    std::map<std::int64_t, std::vector<std::shared_ptr<const ClipNode>>> clipsByTrack;
    try {
        SQLite::Statement st(db,
            "SELECT id, track_id, IFNULL(pos_ticks,0), IFNULL(length_ticks,0), muted, "
            "gain_db, name, fade_in_ticks, fade_out_ticks, fade_in_curve, fade_out_curve "
            "FROM clips WHERE track_id IS NOT NULL ORDER BY track_id, pos_ticks, id");
        while (st.executeStep()) {
            ClipNode c;
            c.id = st.getColumn(0).getInt64();
            const auto trackId = st.getColumn(1).getInt64();
            c.posTicks = st.getColumn(2).getInt64();
            c.lengthTicks = st.getColumn(3).getInt64();
            c.muted = st.getColumn(4).getInt() != 0;
            c.gainDb = static_cast<float>(st.getColumn(5).getDouble());
            c.name = st.getColumn(6).getString();
            c.fadeInTicks=st.getColumn(7).getInt64();c.fadeOutTicks=st.getColumn(8).getInt64();
            c.fadeInCurve=st.getColumn(9).getInt64();c.fadeOutCurve=st.getColumn(10).getInt64();
            if(const auto it=fadeShapes.find(c.id);it!=fadeShapes.end()) {
                c.fadeInTension=it->second.in;c.fadeOutTension=it->second.out;
                c.contentOffsetNs=it->second.offset;c.loopBeginNs=it->second.loopBegin;
                c.loopEndNs=it->second.loopLength;c.sourceEndNs=it->second.end;c.fadeClockNs=it->second.clock;c.fadeClockTicks=it->second.warpClock;
            }

            const auto prev = prevClips.find(c.id);
            if (prev != prevClips.end() && same(*prev->second, c))
                clipsByTrack[trackId].push_back(prev->second);        // shared
            else
                clipsByTrack[trackId].push_back(std::make_shared<const ClipNode>(c));
        }
    } catch (const std::exception&) {
    }

    struct PanSettings {std::string mode;double left=-1.0,right=1.0;};
    std::map<std::int64_t,PanSettings> panSettings;
    try {
        SQLite::Statement st(db,"SELECT track_id,mode,pan_left,pan_right FROM mixer_pan_settings");
        while(st.executeStep())panSettings[st.getColumn(0).getInt64()]={st.getColumn(1).getString(),st.getColumn(2).getDouble(),st.getColumn(3).getDouble()};
    } catch(const std::exception&) {} // an older read-only file has default stereo

    const auto recording=readTrackRecordingOptions(store);
    // --- tracks --------------------------------------------------------------------
    // This deterministic flat traversal is not a display order. The window
    // orders the tree using parent and indexInParent from the snapshot.
    try {
        SQLite::Statement st(db,
            "SELECT t.id, t.name, t.muted, t.soloed, "
            "  IFNULL(m.volume_db, 0.0), IFNULL(m.pan, 0.0), "
            "  IFNULL(t.parent_id, 0), t.index_in_parent, t.kind, t.color, IFNULL(m.delay_samples,0) "
            "FROM tracks t LEFT JOIN mixer_strip m ON m.track_id = t.id "
            "ORDER BY t.index_in_parent, t.id");
        while (st.executeStep()) {
            TrackNode t;
            t.id = st.getColumn(0).getInt64();
            if(const auto options=recording.find(t.id);options!=recording.end())t.recording=options->second;
            t.name = st.getColumn(1).getString();
            t.muted = st.getColumn(2).getInt() != 0;
            t.soloed = st.getColumn(3).getInt() != 0;
            t.volumeDb = static_cast<float>(st.getColumn(4).getDouble());
            t.pan = static_cast<float>(st.getColumn(5).getDouble());
            if(const auto it=panSettings.find(t.id);it!=panSettings.end()) {
                t.panMode=it->second.mode;t.panLeft=it->second.left;t.panRight=it->second.right;
            }
            t.parent = st.getColumn(6).getInt64();
            t.indexInParent = st.getColumn(7).getInt64();
            t.kind = st.getColumn(8).getString();
            if (!st.getColumn(9).isNull()) t.color = st.getColumn(9).getInt64();
            t.delaySamples = st.getColumn(10).getInt64();
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
    // Most projects have no remarks: avoid reading device ownership at all. A guarded
    // block, not an early return: the sections other items append below still run.
    bool hasRemarks=false;
    try {hasRemarks=db.execAndGet("SELECT EXISTS(SELECT 1 FROM remarks)").getInt()!=0;}
    catch(const std::exception&){hasRemarks=false;}
    if(hasRemarks) {
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
    for (const auto& sa : a.sends)
        if (std::find(b.sends.begin(),b.sends.end(),sa)!=b.sends.end())++shared;
    for (const auto& marker : a.markers)
        if (std::find(b.markers.begin(), b.markers.end(), marker) != b.markers.end()) ++shared;
    for (const auto& lane : a.automationLanes) {
        const auto* other = b.findAutomationLane(lane->id);
        if (!other) continue;
        if (other == lane.get()) ++shared;
        for (const auto& envelope : lane->envelopes)
            if (std::find(other->envelopes.begin(), other->envelopes.end(), envelope) != other->envelopes.end()) ++shared;
    }
    for(const auto& remark:a.remarks)
        if(std::find(b.remarks.begin(),b.remarks.end(),remark)!=b.remarks.end())++shared;
    return shared;
}

}  // namespace adi::engine
