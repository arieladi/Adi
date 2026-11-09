// SPDX-License-Identifier: GPL-3.0-or-later
//
// The immutable project model the audio thread reads. ADR-0010, ADR-0019.
//
// This is NOT the `.adi` and NOT the SQLite row set. It is what the audio thread
// is allowed to see: plain data, already resolved, with no handles to anything
// that could block. The store loads into this; the audio thread never reaches
// past it.
//
// STRUCTURAL SHARING IS THE POINT, NOT AN OPTIMISATION
//
// ADR-0019 rejected "rebuild the world on every edit" because it is O(project)
// per publication, which reintroduces inside the engine exactly the cost
// ADR-0001 exists to avoid in the file. A 200-track project does not survive
// rebuilding every track because one fader moved.
//
// So nodes are shared_ptr<const>, and a new snapshot reuses every node that did
// not change. Editing one clip copies the path from root to that clip — its
// track, and the snapshot — and shares the other 199 tracks by pointer. The
// snapshot is then a cheap root, and publication costs what the edit cost.
//
// Everything here is `const` after construction. Not by convention: the audio
// thread holds a `const T*` and the builders return `shared_ptr<const>`, so
// mutating a live snapshot does not compile.

#pragma once
#include <array>

#include "adi/controller_data.hpp"
#include "adi/engine/publisher.hpp"
#include "adi/engine/automation_snapshot.hpp"
#include "adi/textproj.hpp"
#include "adi/transport_settings.hpp"
#include "adi/track_recording.hpp"
#include "adi/cue_settings.hpp"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "adi/return_send_tap.hpp"

namespace adi {
class Store;
}

namespace adi::engine {

/// A tempo event, resolved. SPEC §4.4.
struct TempoEvent {
    std::int64_t posTicks = 0;
    double bpm = 120.0;
    int curve = 0;

    // Compared by value so a rebuilt tempo map that happens to be identical can
    // be shared with the previous snapshot rather than replacing it.
    friend bool operator==(const TempoEvent&, const TempoEvent&) = default;
};

/// The tempo map, shared between snapshots whenever it has not changed — which
/// is almost always, since tempo edits are rare and fader moves are not.
struct TempoMap {
    std::vector<TempoEvent> events;   // sorted by posTicks, always has one at 0

    /// Ticks to seconds, integrating the map. Pure and allocation-free, so the
    /// audio thread may call it.
    [[nodiscard]] double ticksToSeconds(std::int64_t ticks) const noexcept;
    [[nodiscard]] std::int64_t secondsToTicks(double seconds) const noexcept;
    [[nodiscard]] double bpmAt(std::int64_t ticks) const noexcept;
};

struct AudioMediaIdentity {
    std::string contentHash;
    std::optional<std::int64_t> sampleRate,channels,frames;
    bool missing=false; // stored pool flag, not a fresh filesystem probe
    friend bool operator==(const AudioMediaIdentity&,const AudioMediaIdentity&)=default;
};
struct AudioClipSource {
    std::int64_t mediaId=0,sourceStartFrames=0,sourceLengthFrames=0,channelMode=0;
    bool warpEnabled=false,reverse=false;
    std::string warpMode;
    std::optional<std::vector<std::byte>> warpMarkers; // NULL differs from empty
    double transposeSemis=0,formantShift=0;
    std::optional<AudioMediaIdentity> mediaInfo; // absent if pool row is missing
    friend bool operator==(const AudioClipSource&,const AudioClipSource&)=default;
};

struct ClipNode {
    std::string name;
    std::int64_t id = 0;
    std::int64_t posTicks = 0;
    std::int64_t lengthTicks = 0;
    bool muted = false;
    float gainDb = 0.0f;
    std::int64_t fadeInTicks=0,fadeOutTicks=0,fadeInCurve=1,fadeOutCurve=1;
    std::optional<double> fadeInTension,fadeOutTension;
    std::optional<std::int64_t> contentOffsetNs,loopBeginNs,loopEndNs,sourceEndNs;
    std::array<std::optional<std::int64_t>,4> fadeClockNs{}; // in begin/end, out begin/end, unrolled source clock
    std::array<std::optional<std::int64_t>,4> fadeClockTicks{}; // retained Warp source musical domain
    std::optional<AudioClipSource> audio;
    std::vector<controllers::Lane> controllers = {};
};

struct TrackNode {
    std::int64_t id = 0;
    std::int64_t parent = 0;      // 0 at the top of the tree
    std::int64_t indexInParent = 0;
    std::string kind;
    TrackRecordingOptions recording;
    std::optional<std::string> role; // nullable native tracks.role; absent on older schemas
    std::optional<std::int64_t> color; // absent is distinct from explicit zero
    std::string name;            // owned; the audio thread only reads it
    bool muted = false;
    bool soloed = false;
    bool frozen = false;
    std::int64_t frozenByGroup = 0; // the group whose cache currently replaces this track
    std::optional<std::int64_t> freezeMediaId;
    std::string freezeMode; // wet, unverified legacy flag, or empty
    float volumeDb = 0.0f;
    float pan = 0.0f;
    std::int64_t delaySamples = 0; // stored samples at Snapshot::sampleRate; ms = 1000 * samples / rate
    std::string panMode = "stereo";
    double panLeft = -1.0, panRight = 1.0;
    std::vector<std::shared_ptr<const ClipNode>> clips;
    std::optional<ReturnSendTap> returnSendTap{};
};

/// A routing row with kind='send', including disabled rows. Destinations can
/// be any track, not just returns (ADR-0203 d3). Endpoint kinds remain explicit
/// so a reader can also identify rows the planner cannot currently play.
struct SendRoute {
    std::int64_t id = 0;
    std::string sourceKind, destinationKind;
    std::int64_t sourceId = 0, destinationId = 0, order = 0;
    double gainDb = 0.0, pan = 0.0;
    bool preFader = false, enabled = true;
};

/// Locator/cycle metadata. Positions and lengths retain their stored units;
/// only the matching time-base columns are present. Notes/prompts are literal.
struct MarkerNode {
    std::int64_t id = 0, timeBase = 0;
    std::optional<std::int64_t> posTicks, posNs, lengthTicks, lengthNs, color;
    std::string name, kind, note, prompt;
    friend bool operator==(const MarkerNode&, const MarkerNode&) = default;
};

/// A remark with its owning track resolved once per rebuild. Orphaned
/// anchors remain visible; a parameter need not have a plugin_params row.
struct RemarkNode {
    std::int64_t id=0,targetId=0,createdUtc=0;
    std::string targetKind,author,actorDetail,text;
    std::optional<std::string> paramId;
    bool resolved=false,anchorMissing=false;
    std::optional<std::int64_t> trackId;
    friend bool operator==(const RemarkNode&,const RemarkNode&)=default;
};

/// Assigned built-in media slot. State hash caches immutable metadata across
/// rebuilds; file presence and pool metadata are refreshed off the callback.
struct SampleSlotNode {
    std::int64_t deviceId=0;
    std::int32_t slot=0;
    std::string name,uid,mediaHash,stateHash,problem;
    std::optional<std::int64_t> mediaId;
    bool empty=true,missing=false,hasDerivedState=false;
    friend bool operator==(const SampleSlotNode&,const SampleSlotNode&)=default;
};

/// What the audio thread sees. Derives from Sequenced so the publisher can
/// stamp it; nothing else touches `seq`.
struct Snapshot : Sequenced {
    std::shared_ptr<const TempoMap> tempo;
    std::vector<std::shared_ptr<const TrackNode>> tracks;
    // Sorted by persistent id, not a display order. Filter by trackId or the
    // raw target address; unresolved anchors retain all their text/metadata.
    std::vector<std::shared_ptr<const RemarkNode>> remarks;
    // All routing kinds, including disabled rows. Zero if no positive id.
    // Observation only: caller must guard INT64_MAX and reserve pending ids.
    std::int64_t maxRoutingId = 0;
    std::vector<std::shared_ptr<const SampleSlotNode>> sampleSlots;
    int sampleRate = 44100;
    TransportSettings transport;
    std::vector<std::shared_ptr<const MarkerNode>> markers; // id order, not a mixed-units display order
    std::vector<std::shared_ptr<const AutomationLaneNode>> automationLanes; // lane id order
    bool automationOverridden = false; // session state supplied at rebuild, never saved
    CueSettings cue;
    // SQL pos_ticks maps to Meter::start_ticks. Copied with the root on every
    // build, while unchanged tracks/clips/tempo retain structural sharing.
    std::vector<textproj::Meter> meters;
    // Sorted by routing id for stable identity, not by the window's return
    // order. Unchanged rows share nodes across rebuilds, as tracks/clips do.
    std::vector<std::shared_ptr<const SendRoute>> sends;

    [[nodiscard]] const TrackNode* findTrack(std::int64_t id) const noexcept;
    [[nodiscard]] bool anySoloed() const noexcept;
    [[nodiscard]] const MarkerNode* findMarker(std::int64_t id) const noexcept;
    [[nodiscard]] const AutomationLaneNode* findAutomationLane(std::int64_t id) const noexcept;
};

/// Builds snapshots, reusing nodes from the previous one.
///
/// Kept as a class rather than a free function because the reuse needs the
/// previous snapshot, and threading that through every call site is how someone
/// eventually passes the wrong one and silently loses sharing — the failure
/// being slower, not wrong, which is the kind nobody notices.
class SnapshotBuilder {
public:
    /// probeMediaFiles=false captures stored media metadata without filesystem
    /// probes; rollback-preview capture uses this mode and prepares readers later.
    /// Reads the whole project. Use for the first snapshot, or after a change
    /// too broad to describe.
    static std::unique_ptr<Snapshot> fromStore(const Store&, const std::set<std::int64_t>& overridden = {}, bool probeMediaFiles = true);

    /// Reads the project but reuses every node from `previous` that is
    /// unchanged. Returns a snapshot sharing structure with it.
    static std::unique_ptr<Snapshot> fromStore(const Store&, const Snapshot& previous,
                                              const std::set<std::int64_t>& overridden = {}, bool probeMediaFiles = true);

    /// How many nodes the two snapshots share by pointer. For tests: the sharing
    /// claim above is checkable, so it is checked.
    static std::size_t sharedNodeCount(const Snapshot&, const Snapshot&);
};

using ProjectPublisher = SnapshotPublisher<Snapshot>;

}  // namespace adi::engine
