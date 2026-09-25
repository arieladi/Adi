// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session: a project, live. ADR-0122 -- roadmap step 6 opens here.
//
// Every part of this existed before this file and nothing joined them up. A
// `.adi` could be read into a `rows::Model` (ADR-0090's projection), a model
// could be planned and realised into a `Graph` (ADR-0077), a `GraphHost` could
// publish that graph to an audio thread and replace it (ADR-0089), a
// `DeviceHost` could hold plugin instances and answer their restart requests
// (ADR-0090), and a `DeviceBridge` could turn a driver callback into
// `BlockProcessor::process` (ADR-0049). What did not exist was the object that
// owns all of them for the lifetime of an open project and is the ONE thing a
// device callback talks to. This is it.
//
// THREE DECISIONS, each of which the pieces below it did not make:
//
//   1. **The rows place devices; the host only holds them.** `DeviceHost::add`
//      takes a track id and `chainSupplier()` places by it, which is right for
//      a probe and wrong for a project: a device's position is
//      `devices.chain_id` and `devices.ord`, and an edit moves it without
//      re-instantiating anything. So every instance is added UNPLACED and the
//      session answers `RealizeOptions::devicesFor` from the model, in row
//      order. A rebuild after `refresh` therefore follows the rows, and the
//      rows are the truth (ADR-0003).
//   2. **A plugin that will not load is a placeholder, never a gap.** ADR-0011
//      and SPEC §7.1: the device stays in the chain, bypassed, carrying its
//      `plugin_params` as answers and its `plugin_state` bytes untouched. The
//      chain's shape -- what feeds what, where the compensation goes -- is the
//      same on the machine that has the plugin and the one that does not.
//   3. **A format change is a REBUILD, not a reload.** `prepare` at a new rate
//      or block size builds a new graph at that size and publishes it; the same
//      instances are re-injected and re-prepared (ADR-0042 d5, honoured by
//      ADR-0090 d5's guard). `prepare` at the SAME format, on a graph that is
//      live, does nothing at all -- a driver that restarts without changing
//      anything must not cause a seam.
//
// THREADS. `load`, `refresh`, `rebuild`, `prepare`, `release` and `tick` are
// message-thread calls; `prepare` and `release` arrive from whatever thread
// opens and closes the device, which JUCE keeps off the audio thread. `process`
// is the audio thread and touches nothing the others write: it reads the
// published graph through `GraphHost`, whose handoff is ADR-0010's.
#pragma once

#include "adi/engine/host.hpp"
#include "adi/engine/clip_playback.hpp"
#include "adi/engine/midi_clips.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/engine/process.hpp"
#include "adi/engine/realize.hpp"
#include "adi/store_rows.hpp"
#include "juce/device_host.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace adi {
class Store;
}

namespace adi::engine {

/// The format the session is asked to run at. `load` takes the expected one;
/// `prepare` replaces it with what the driver actually granted.
struct SessionSpec {
    std::int32_t channels = 2;
    double sampleRate = 48000.0;
    std::int32_t maxFrames = 512;
};

/// What a loader is asked for: the row, the identity it names, and the format
/// the session expects to run at. `ref` is null when the row has no
/// `plugin_ref_id`, and the session does not call the loader then.
struct DeviceRequest {
    const rows::Device* device = nullptr;
    const rows::PluginRef* ref = nullptr;
    double sampleRate = 48000.0;
    std::int32_t maxFrames = 512;
};

/// Turns a request into a live instance, or returns null with `error` set.
/// Injected for the same reason `DeviceChainFn` is: this file compiles on
/// every ABI and cannot construct a `Vst3Device`. A missing loader is the
/// headless case, and every row then becomes a placeholder.
using DeviceLoader = std::function<std::unique_ptr<device::DeviceInstance>(
    const DeviceRequest&, std::string& error)>;

class Session final : public BlockProcessor {
public:
    /// One device row the session resolved, placeholder or not.
    struct Entry {
        std::int64_t deviceId = 0;
        std::int64_t trackId = 0;      ///< the chain's track; 0 once retired
        std::string name;
        bool placeholder = false;      ///< a `MissingDevice` stands in (ADR-0011)
        bool retired = false;          ///< the row is gone; the instance is kept
        std::string error;             ///< why the loader refused, when it did
        std::size_t hostIndex = 0;     ///< into `devices()`
        std::uint8_t route = 0;        ///< the RouteChoice last asked of the instance (ADR-0149)
    };

    struct Stats {
        std::int64_t loaded = 0;         ///< instances the loader produced
        std::int64_t placeholders = 0;   ///< rows kept as `MissingDevice`
        std::int64_t skipped = 0;        ///< racks and their nested chains (ADR-0060)
        std::int64_t retired = 0;        ///< rows that left the project after `load`
        std::int64_t statesLoaded = 0;   ///< `loadState` calls that succeeded
        std::int64_t paramsApplied = 0;  ///< `setParam` calls made from `plugin_params`
        std::int64_t paramsMatched = 0;  ///< rows a loaded chunk already agreed with (ADR-0142)
        std::int64_t routesApplied = 0;  ///< recorded routes a device took (ADR-0149)
        std::int64_t routesRefused = 0;  ///< recorded routes a device could not take
        std::int64_t rebuilds = 0;       ///< graphs this session asked for
        std::int64_t prepares = 0;       ///< `prepare` calls, including no-ops
        std::int64_t formatChanges = 0;  ///< prepares that changed rate or size
    };

    Session();
    ~Session() override;

    // --- message thread -----------------------------------------------------

    /// Read the project, resolve every device row, build the first graph at
    /// `spec`. False with `error()` set when no graph could be built; the
    /// devices resolved so far are kept, so a caller can still report them.
    bool load(const Store& store, DeviceLoader loader, SessionSpec spec);

    /// Re-read the project after an edit. Rows that are new are resolved
    /// through the loader; rows that are gone are RETIRED -- left out of every
    /// chain but kept alive, because a graph the audio thread may still be
    /// rendering holds their nodes. Then rebuild.
    bool refresh(const Store& store);

    /// Plan, realise, prepare and publish a new graph from the current model.
    bool rebuild();

    // --- automation override (ADR-0162) -------------------------------------
    // Live 12 25.4: changing an automated control while not recording turns
    // that lane off and the manual value stands. Here a refresh that finds an
    // automated strip value changed -- by a user, an op or the agent -- marks
    // its lane overridden. Session state: not an op, not saved.

    /// True while any lane is overridden: Live's Re-Enable Automation button lit.
    [[nodiscard]] bool automationOverridden() const noexcept { return !overridden_.empty(); }
    [[nodiscard]] const std::set<std::int64_t>& overriddenLanes() const noexcept { return overridden_; }
    /// Follow every overridden lane again, from the playhead at once. Rebuilds.
    bool reenableAutomation();
    /// One lane only: the parameter's context menu in Live. False if that lane
    /// was not overridden.
    bool reenableAutomation(std::int64_t laneId);

    /// What pushes into each track's junction -- a clip reader, a test tone.
    /// Effective from the next rebuild. The nodes are the caller's and must
    /// outlive every graph that holds them, which means: until the session is
    /// destroyed or a later rebuild without them has been collected.
    void setSourcesFor(SourceFn fn);

    /// Drain plugin callbacks, answer restart requests, free retired graphs.
    /// Returns true when a latency change was applied.
    bool tick(std::int64_t nowMs);

    // --- BlockProcessor -----------------------------------------------------

    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void process(const AudioIo& io) noexcept override;
    void release() override;

    // --- inspection (message thread) ---------------------------------------

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] const rows::Model& model() const noexcept { return model_; }
    [[nodiscard]] const SessionSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// Everything the model read, the plan found and the session decided,
    /// in that order. A project with problems still plays where it can.
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept {
        return problems_;
    }

    // Driver thread only, commands between callbacks; see transport.hpp.
    [[nodiscard]] Transport& transport() noexcept { return transport_; }
    // Message/offline driver inspection. Pointer valid until the next rebuild.
    [[nodiscard]] ClipPlayback* clips() noexcept { return clips_.get(); }
    [[nodiscard]] MidiClips* midiClips() noexcept { return midi_.get(); }

    [[nodiscard]] device::DeviceHost& devices() noexcept { return devices_; }
    [[nodiscard]] GraphHost& graph() noexcept { return graph_; }

    [[nodiscard]] std::size_t entryCount() const noexcept { return entries_.size(); }
    [[nodiscard]] const Entry& entryAt(std::size_t i) const { return entries_[i]; }
    [[nodiscard]] const Entry* entryFor(std::int64_t deviceId) const noexcept;

    /// The live node and instance behind a device row; null for a row the
    /// session did not resolve (a rack) or never saw.
    [[nodiscard]] device::DeviceNode* nodeFor(std::int64_t deviceId) noexcept;
    [[nodiscard]] device::DeviceInstance* instanceFor(std::int64_t deviceId) noexcept;

    /// A track's chain as the rows order it: every chain on the track by
    /// `ord`, every device in it by `ord`, skipping what was not resolved.
    /// This is what `RealizeOptions::devicesFor` answers with.
    [[nodiscard]] std::vector<device::DeviceNode*> chainFor(std::int64_t trackId);

private:
    void attach();
    void resolveNewRows(const Store& store);
    void resolveOne(const Store& store, const rows::Device& row);
    void retireDepartedRows();
    void syncFlags();
    void syncRoute(Entry& e, device::DeviceInstance& inst);
    [[nodiscard]] const rows::PluginRef* refFor(const rows::Device& row) const noexcept;
    [[nodiscard]] const rows::DeviceChain* chainOf(std::int64_t chainId) const noexcept;
    [[nodiscard]] std::unique_ptr<device::DeviceInstance> makePlaceholder(
        const Store& store, const rows::Device& row, const rows::PluginRef* ref);
    void restoreState(const Store& store, const rows::Device& row,
                      device::DeviceInstance& inst);

    // Declared so that destruction runs graphs FIRST: a retired graph holds
    // raw pointers to device nodes, and the devices must still exist while
    // it is freed. (Nothing dereferences them today; the order is the rule.)
    Transport transport_;
    std::shared_ptr<const ClipProject> clipProject_;
    // Published graphs retain their own old source generations until reclamation.
    std::unique_ptr<ClipPlayback> clips_;
    std::shared_ptr<MidiClipState> midiState_ = makeMidiClipState();
    std::unique_ptr<MidiClips> midi_;
    // ADR-0163: one strip per track, for the whole session. Declared before
    // the graphs so it outlives every graph that names one of its strips.
    MixerStrips strips_;
    // ADR-0164: this rebuild's program, bound to strips. Kept here as well as
    // by every strip and graph that plays it.
    std::shared_ptr<StripAutomation> stripAutomation_;
    std::set<std::int64_t> overridden_;   ///< ADR-0162: lane ids
    device::DeviceHost devices_;
    GraphHost graph_;
    rows::Model model_;
    DeviceLoader loader_;
    SourceFn sources_;
    SessionSpec spec_;
    Stats stats_;
    std::vector<Entry> entries_;
    std::unordered_map<std::int64_t, std::size_t> byId_;   ///< device id -> entries_
    std::vector<std::string> problems_;
    std::vector<std::string> sessionProblems_;   ///< ours, re-appended after each read
    std::string error_;
    bool loaded_ = false;
    bool live_ = false;      ///< a graph is prepared and published for a device
};

}  // namespace adi::engine
