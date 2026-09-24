// SPDX-License-Identifier: GPL-3.0-or-later
//
// Typed rows for the tables the text projection reads — the layer between
// SQLite and anything that wants to know what is in a project.
//
// Why this exists as its own file rather than as queries inside the projector:
//
//   1. `textproj.hpp` argues in its own header comment that the store adapter
//      is a separate concern, and that the pure part must stay testable
//      without a database (ADR-0010). Keeping the rows here means
//      `buildTree(const Model&)` is a total function of a plain value, so the
//      adapter's own logic is testable without SQLite too, and only
//      `readModel` touches a database at all.
//   2. The projection is not the only consumer. The agent's projection
//      (AI-AGENT §4) wants the same rows in a different shape, and so will the
//      engine's snapshot builder eventually. A second `SELECT name, muted …`
//      in a second file is how two views of a project quietly disagree.
//
// Nullable columns are `std::optional`, not sentinels. SQLite's NULL is a real
// third state here — `clips.lane_id` NULL means "not in a lane", and -1 would
// mean "lane number minus one" to anyone reading it a year from now.
//
// Rows carry ids. The PROJECTION must not emit them (TEXT-PROJECTION §2), but
// the adapter needs them to resolve parent_id, alias_of and routing endpoints
// into node indices. They stop here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adi {
class Store;
}

namespace adi::rows {

// --- Layer 0/1: the project itself ------------------------------------------

struct Project {
    std::int64_t id = 1;
    std::string name;
    std::int64_t sampleRate = 48000;
    std::int64_t ppq = 5765760;
    std::int64_t lengthTicks = 0;
    std::string author;
    std::string notes;
};

struct TempoEvent {
    std::int64_t posTicks = 0;
    double bpm = 120.0;
    std::int64_t curve = 0;
    double tension = 0.0;
};

struct TimeSignature {
    std::int64_t posTicks = 0;
    std::int64_t numerator = 4;
    std::int64_t denominator = 4;
};

// --- Layer 1: tracks and their mixer strip ----------------------------------

struct Track {
    std::int64_t id = 0;
    std::optional<std::int64_t> parentId;
    std::int64_t indexInParent = 0;
    std::string kind;
    std::string name;
    std::optional<std::int64_t> color;
    std::int64_t timeBase = 0;
    bool frozen = false;
    bool locked = false;
    bool muted = false;
    bool soloed = false;
    bool soloDefeat = false;
    bool recordArmed = false;
    std::int64_t monitorMode = 0;
    std::optional<std::string> inputRef;
    std::int64_t automationMode = 0;
    std::optional<std::int64_t> freezeMediaId;
};

/// One row, inlined onto its track by the projection (TEXT-PROJECTION §2): the
/// relation is 1:1 and the id adds nothing a reader wants to see.
struct MixerStrip {
    std::int64_t trackId = 0;
    double volumeDb = 0.0;
    double pan = 0.0;
    std::int64_t panLaw = 0;
    double width = 1.0;
    double inputGainDb = 0.0;
    bool phaseInvert = false;
    std::int64_t delaySamples = 0;
    std::optional<std::int64_t> vcaGroupId;
};

struct Lane {
    std::int64_t id = 0;
    std::int64_t trackId = 0;
    std::int64_t ord = 0;
    std::string name;
    std::string kind;
    bool muted = false;
    bool isCompTarget = false;
};

// --- Layer 1: clips and their content ---------------------------------------

struct Clip {
    std::int64_t id = 0;
    std::int64_t trackId = 0;
    std::optional<std::int64_t> laneId;
    std::string kind;
    std::string name;
    std::optional<std::int64_t> color;
    std::int64_t timeBase = 0;
    std::optional<std::int64_t> posTicks;
    std::optional<std::int64_t> posNs;
    std::optional<std::int64_t> lengthTicks;
    std::optional<std::int64_t> lengthNs;
    bool loopEnabled = false;
    std::int64_t loopStartTicks = 0;
    std::optional<std::int64_t> loopLenTicks;
    std::int64_t contentOffsetTicks = 0;
    bool muted = false;
    double gainDb = 0.0;
    std::int64_t fadeInTicks = 0;
    std::int64_t fadeOutTicks = 0;
    std::int64_t fadeInCurve = 1;
    std::int64_t fadeOutCurve = 1;
    std::optional<std::int64_t> aliasOf;
};

struct AudioClip {
    std::int64_t clipId = 0;
    std::int64_t mediaId = 0;
    std::int64_t srcStartFrames = 0;
    std::int64_t srcLenFrames = 0;
    bool warpEnabled = false;
    std::string warpMode;
    double transposeSemis = 0.0;
    double formantShift = 0.0;
    bool reverse = false;
    std::int64_t channelMode = 0;
};

/// A note, already decoded out of its clip's `ANOT` blob. The projection never
/// sees a blob; it sees notes.
struct Note {
    std::int64_t clipId = 0;
    std::int64_t startTicks = 0;
    std::int64_t durTicks = 0;
    std::int64_t key = 0;
    std::int64_t velOn = 0;
    std::int64_t velOff = 0;
    std::int64_t channel = 0;
    std::int64_t probability = 10000;
    double tuningCents = 0.0;
    std::uint16_t flags = 0;
};

// --- Layer 1: the rest ------------------------------------------------------

struct Marker {
    std::int64_t id = 0;
    std::int64_t timeBase = 0;
    std::optional<std::int64_t> posTicks;
    std::optional<std::int64_t> posNs;
    std::optional<std::int64_t> lengthTicks;
    std::optional<std::int64_t> lengthNs;
    std::string name;
    std::optional<std::int64_t> color;
    std::string kind;
};

struct Media {
    std::int64_t id = 0;
    std::string hashBlake3;
    std::string origName;
    std::optional<std::string> relPath;
    std::optional<std::int64_t> sampleRate;
    std::optional<std::int64_t> channels;
    std::optional<std::int64_t> frames;
    std::string format;
    std::optional<std::int64_t> bitDepth;
    bool embedded = false;
    bool missing = false;
};

struct Routing {
    std::int64_t id = 0;
    std::string srcKind;
    std::int64_t srcId = 0;
    std::string dstKind;
    std::int64_t dstId = 0;
    std::string kind;
    std::int64_t ord = 0;
    double gainDb = 0.0;
    double pan = 0.0;
    bool preFader = false;
    bool enabled = true;
    /// 'auto' (grouping owns it) or 'user' (the user does). ADR-0044/ADR-0065:
    /// the difference decides whether re-parenting may rewrite the row, so a
    /// planner that cannot see it cannot honour the rule.
    std::string origin = "user";
};

// --- the whole readable project ---------------------------------------------

/// Everything `buildTree` needs, and nothing that needs a database to reach.
///
/// Deliberately a value: it is read once, under one SQLite read transaction, so
/// the projection cannot see half of one edit and half of the next. A projector
/// that queried lazily would be racing a writer on the same connection, and
/// would produce a file that never existed.
/// `plugin_refs`: the identity a device row points at. Wider than what ADI
/// hosts (SPEC §7.4): an `au` row opens as a placeholder, never as nothing.
struct PluginRef {
    std::int64_t id = 0;
    std::string format;
    std::string uid;
    std::string vendor;
    std::string name;
    std::string version;
    std::string subtype;
    std::string pathHint;
    bool isShell = false;
    std::optional<std::int64_t> shellId;
};

/// `device_chains`: exactly one owner, a track or a rack device (the schema's
/// CHECK). A chain directly on a track IS that track's signal path.
struct DeviceChain {
    std::int64_t id = 0;
    std::optional<std::int64_t> parentDeviceId;
    std::optional<std::int64_t> trackId;
    std::int64_t ord = 0;
    std::string name;
    bool muted = false;
    bool soloed = false;
};

/// `devices`: one row per device in a chain, in `ord` order. Read in
/// (chain, ord, id) order so a consumer can walk a chain without sorting.
struct Device {
    std::int64_t id = 0;
    std::int64_t chainId = 0;
    std::int64_t ord = 0;
    std::optional<std::int64_t> pluginRefId;
    std::string name;
    bool enabled = true;
    bool isRack = false;
    std::optional<std::string> rackKind;
    std::string presetName;
    std::int64_t latencySamples = 0;
    std::optional<std::int64_t> remoteHostId;
    bool alwaysProcess = false;
    bool missing = false;
};

/// `plugin_params`: the missing-plugin safety net (SPEC §7.1), and the
/// values a placeholder answers `getParam` with.
struct PluginParam {
    std::int64_t deviceId = 0;
    std::string paramId;
    std::string name;
    double normalized = 0.0;
    std::optional<double> real;
    std::string display;
    std::string unit;
    std::int64_t flags = 0;
};

/// `plugin_state`: a role and a HASH. The bytes stay in `state_blobs` and
/// come through `Store::getStateBlob` when a loader wants them -- the model
/// is re-read after every edit (ADR-0090 d2) and a sampler's state is not
/// something to re-read on every edit.
struct PluginState {
    std::int64_t deviceId = 0;
    std::string role;
    std::string hash;
    std::string formatHint;
};

/// `device_expression_routes` (ADR-0146, schema 1.4): the route a device
/// plays with. A device with no row is Auto.
struct DeviceRoute {
    std::int64_t deviceId = 0;
    std::string route;   ///< note_expression | mpe_midi | plain
};

/// `device_panels` / `device_panel_params` (ADR-0154, schema 1.5): a
/// configured plug-in panel and its parameters in order. A device with no
/// DevicePanel is on Live's default (panel.hpp resolves it).
struct DevicePanel {
    std::int64_t deviceId = 0;
    std::vector<std::string> params;   ///< in `ord` order; may be empty
};

/// `remarks` (ADR-0131, schema 1.2): a note anchored to a track, a clip or a
/// device, and through `paramId` to one of a device's parameters. The anchor
/// is polymorphic, like a routing endpoint, so it may name nothing; the
/// projection then shows it unresolved rather than dropping it (ADR-0139).
struct Remark {
    std::int64_t id = 0;
    std::string targetKind;
    std::int64_t targetId = 0;
    std::optional<std::string> paramId;
    std::string author = "user";
    std::string actorDetail;
    std::string text;
    std::int64_t createdUtc = 0;
    bool resolved = false;
};

/// `automation_lanes` (SPEC §6.3.3, ADR-0159): what a lane controls and how
/// its values read. Its points live in `AutomationData`.
struct AutomationLane {
    std::int64_t id = 0;
    std::string ownerKind;           ///< track | device | clip | project | routing
    std::int64_t ownerId = 0;
    std::string paramRef;
    std::string paramName;
    std::int64_t timeBase = 0;       ///< 0 ticks, 1 nanoseconds
    std::string valueDomain = "normalized";
    std::string unit;
    double defaultValue = 0.0;
    std::optional<double> minValue;
    std::optional<double> maxValue;
    bool enabled = true;
};

/// `automation_data`: one AAUT stream per lane, or per lane and clip for a
/// clip envelope. Kept as the stored bytes: the engine's compiler validates
/// and decodes them (engine/automation.hpp), and a reader that decoded here
/// would have to decide what a bad point means twice.
struct AutomationData {
    std::int64_t laneId = 0;
    std::optional<std::int64_t> clipId;
    std::vector<std::byte> blob;
};

struct Model {
    Project project;
    std::vector<TempoEvent> tempo;
    std::vector<TimeSignature> signatures;
    std::vector<Track> tracks;
    std::vector<MixerStrip> strips;
    std::vector<Lane> lanes;
    std::vector<Clip> clips;
    std::vector<AudioClip> audioClips;
    std::vector<Note> notes;
    std::vector<Marker> markers;
    std::vector<Media> media;
    std::vector<Routing> routing;
    std::vector<PluginRef> pluginRefs;
    std::vector<DeviceChain> deviceChains;
    std::vector<Device> devices;
    std::vector<PluginParam> pluginParams;
    std::vector<PluginState> pluginState;
    std::vector<DeviceRoute> deviceRoutes;
    std::vector<DevicePanel> devicePanels;
    std::vector<Remark> remarks;
    std::vector<AutomationLane> automationLanes;   ///< by id
    std::vector<AutomationData> automationData;    ///< by lane, then clip

    /// Tables that were present but unreadable — a newer schema that dropped a
    /// column we name, a corrupt blob. Named rather than swallowed, because a
    /// projection silently missing a table is a diff that says something was
    /// deleted.
    std::vector<std::string> problems;
};

/// Read a whole project. The only function in the adapter that touches SQLite.
///
/// Never throws: SQLiteCpp does, and a caller of the projection did not choose
/// that vocabulary. A table that cannot be read leaves its vector empty and
/// appends to `problems`.
[[nodiscard]] Model readModel(const Store&);

}  // namespace adi::rows
