// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/snapshot.hpp"

#include "adi/store.hpp"
#include "adi/media/media_ops.hpp"
#include "juce/builtin_samples.hpp"
#include <filesystem>

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <map>

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
    // State bytes are immutable: decode only a changed state hash. Ordinary
    // edits reuse its metadata and check only the small pool row and locator.
    std::map<std::pair<std::int64_t,std::int32_t>,std::shared_ptr<const SampleSlotNode>> previousSlots;
    for(const auto& slot:previous.sampleSlots)previousSlots[{slot->deviceId,slot->slot}]=slot;
    SQLite::Statement slots(db,"SELECT s.device_id,s.stream_role,s.state_hash,p.uid FROM plugin_state s "
        "JOIN devices d ON d.id=s.device_id JOIN plugin_refs p ON p.id=d.plugin_ref_id "
        "WHERE p.format='internal' AND s.stream_role LIKE 'sample:%' ORDER BY s.device_id,s.stream_role");
    while(slots.executeStep()) {
        SampleSlotNode node;node.deviceId=slots.getColumn(0).getInt64();node.stateHash=slots.getColumn(2).getString();
        try {
            const auto role=slots.getColumn(1).getString();
            if(role!="sample:1")throw std::runtime_error("unknown built-in sample role");
            node.slot=1;node.name="Impulse";const auto old=previousSlots.find({node.deviceId,node.slot});
            if(old!=previousSlots.end() && old->second->stateHash==node.stateHash && old->second->problem.empty())node=*old->second;
            else {
                SQLite::Statement size(db,"SELECT length(data) FROM state_blobs WHERE hash_blake3=?");size.bind(1,node.stateHash);
                if(!size.executeStep() || size.getColumn(0).getInt64()<=0 || size.getColumn(0).getInt64()>40LL*1024*1024)
                    throw std::runtime_error("sample state is missing or too large");
                const auto bytes=store.getStateBlob(node.stateHash);
                if(!bytes || bytes->empty() || bytes->size()>40u*1024u*1024u)throw std::runtime_error("sample state is missing or too large");
                const auto* first=reinterpret_cast<const std::uint8_t*>(bytes->data());
                const auto value=Payload::from_cbor(first,first+bytes->size());const auto info=device::inspectBuiltinSample(value);
                if(info.uid!=slots.getColumn(3).getString())throw std::runtime_error("sample state device UID differs");
                node.uid=info.uid;node.empty=info.empty;node.mediaHash=info.mediaHash;node.hasDerivedState=!info.empty && value.contains("derived");
            }
            if(node.uid!=slots.getColumn(3).getString())throw std::runtime_error("sample state device UID differs");
            node.mediaId.reset();node.missing=false;
            if(!node.empty && node.problem.empty()) {
                SQLite::Statement media(db,"SELECT id,rel_path,abs_path_hint,missing FROM media_files WHERE hash_blake3=?");media.bind(1,node.mediaHash);
                node.missing=true;
                if(media.executeStep()) {
                    node.mediaId=media.getColumn(0).getInt64();
                    if(media.getColumn(3).getInt()==0) {
                        std::error_code ec;bool present=false;
                        if(!media.getColumn(1).isNull()) {
                            const auto relative=media::pathFromUtf8(media.getColumn(1).getString());
                            if(!relative.is_absolute())present=std::filesystem::is_regular_file(media::projectFolder(store)/relative,ec);
                        }
                        if(!present && !media.getColumn(2).isNull())present=std::filesystem::is_regular_file(media::pathFromUtf8(media.getColumn(2).getString()),ec);
                        node.missing=!present;
                    }
                }
            }
        }catch(const std::exception& exception){node.problem=exception.what();node.missing=true;}
        const auto old=previousSlots.find({node.deviceId,node.slot});
        if(old!=previousSlots.end() && *old->second==node)snap->sampleSlots.push_back(old->second);
        else snap->sampleSlots.push_back(std::make_shared<const SampleSlotNode>(std::move(node)));
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
    for(const auto& slot:a.sampleSlots)
        if(std::find(b.sampleSlots.begin(),b.sampleSlots.end(),slot)!=b.sampleSlots.end())++shared;
    return shared;
}

}  // namespace adi::engine
