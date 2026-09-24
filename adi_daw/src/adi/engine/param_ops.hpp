// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parameter-op glue: ADR-0110's mechanism, joined up. ADR-0124.
//
// Codex's `ParamEditCapture` (ADR-0122 d11) turns a stream of parameter
// broadcasts into gesture-sized edits. It knows nothing about devices, ops or
// the store, on purpose. This is the layer that does: it hands every attached
// device a ring to broadcast into, drains those rings on the message thread
// into `device.setParam` requests, and, when an op arrives from anywhere else
// (undo, the UI, the agent), pushes the value into the plugin with the echo
// guard armed.
//
// FOUR DECISIONS the capture layer left to its caller:
//
//   1. **One ring per device.** The capture is single-producer, and the
//      producers here are not one thread: a VST3 broadcasts on the message
//      thread (JUCE's listener), a CLAP on the audio thread (output events in
//      `process`). Two devices are two producers, so each device owns a ring
//      and `drain` walks them all. A ring is never freed while its device may
//      still push; `detach` only unhooks the sink.
//
//      LIFETIME, one-sided: **an attached device outlives the glue, or is
//      detached first.** The destructor unhooks every sink it installed, and it
//      writes through the device to do so; a device that is already gone is a
//      write into freed memory (linux found it with ASan in the first test
//      suite, #75). The natural owners obey this without trying: the session
//      owns the devices and outlives the UI's glue. The other order -- glue
//      alive, device gone -- is what `detach` exists for.
//   2. **Normalized is the wire unit.** Both formats can produce 0..1 (VST3
//      natively; CLAP from the declared range), and `device.setParam` carries
//      `norm` always and `real` when the descriptor has a range (ADR-0057).
//   3. **`applied` compares before it sets.** Our own ops come back through
//      the same path as an undo, and the device already holds their value;
//      setting it again would only manufacture an echo. A value the device
//      already has is skipped and counted, not re-sent.
//   4. **The first edit of a parameter in a session writes its starting value
//      first.** `plugin_params` has no row for a parameter that was never
//      touched (ADR-0057), so the inverse of the first op is "delete the row",
//      and an undo would leave the plugin where the gesture put it. Emitting a
//      `setParam` with the pre-gesture value ahead of the edit, in the same
//      transaction, gives undo somewhere to go: the pair undoes to the value
//      the plugin had, and the row's absence is restored after it.
//
// AND THE CHUNK (ADR-0142, closing ADR-0110 d1). Not every change is a
// parameter: a preset picked in the plugin's own browser moves its state
// with no gesture at all. The plugin says so -- `stateEpoch` moves -- and the
// next drain snapshots the chunk as ONE `device.loadState`:
//
//   5. **The preset's parameter broadcasts are folded into it.** A drain in
//      which the device signalled absorbs every ungestured edit instead of
//      writing it; the chunk carries those values. A gesture that ended in
//      the same interval is still its own op.
//   6. **The first snapshot writes the chunk it replaced.** Decision 4 again,
//      for state: with no `plugin_state` row the inverse would only delete
//      the row, and undo would leave the preset in the plugin. The chunk seen
//      at attach is kept, and written first, so the pair undoes to it.
//   7. **Rows follow the chunk.** Every `plugin_params` row of the device is
//      rewritten, in the same transaction, to where the snapshot left it. A
//      row therefore never contradicts a newer chunk, which is what lets the
//      session apply rows ON TOP of a loaded chunk: a row that differs is an
//      edit made after it (session.cpp, `restoreState`).
//   8. **The same chunk is not a change.** A signal whose chunk hashes to what
//      the plugin last reported writes nothing: a latency restart, or a
//      plugin echoing the state our undo just loaded into it.
//
// A snapshot's bytes travel beside the requests, not in them: `takeBlobs` or
// `writeBlobs` puts them in `state_blobs` BEFORE the caller commits, and the
// foreign key refuses a commit that forgot.
#pragma once

#include "adi/engine/param_edits.hpp"
#include "adi/ops.hpp"
#include "juce/device_model.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace adi {
class Store;
}

namespace adi::engine {

class Session;

/// ADR-0142: the bytes a `device.loadState` request names. Written to
/// `state_blobs` before the requests are committed.
struct StateBlob {
    std::string hash;                 ///< BLAKE3, 64 lowercase hex digits
    std::vector<std::uint8_t> bytes;
};

class ParamOps {
public:
    explicit ParamOps(std::int32_t ringCapacity = 1024);
    ~ParamOps();
    ParamOps(const ParamOps&) = delete;
    ParamOps& operator=(const ParamOps&) = delete;

    // --- message thread -----------------------------------------------------

    /// Give a device a ring, seed the ring with every parameter's current
    /// value, and install the sink. False when the device is already attached.
    bool attach(std::int64_t deviceId, device::DeviceInstance& inst);

    /// Unhook the sink. The ring stays allocated: the device's producer thread
    /// may be inside `push` right now.
    void detach(std::int64_t deviceId);

    /// Attach every loaded, non-retired entry of a session that is not yet
    /// attached. Returns how many were new. Call again after `Session::refresh`.
    std::size_t attachSession(Session& s);

    /// Drain every ring into `device.setParam` requests, appended to `out`.
    /// Returns how many requests were appended. The caller commits them; the
    /// devices already hold the values, so nothing is sent back.
    ///
    /// A device whose `stateEpoch` moved since the last drain is snapshotted
    /// (decisions 5 to 8): `device.loadState` requests whose bytes wait in
    /// `takeBlobs`. Commit everything one drain appended as ONE transaction.
    std::size_t drain(std::int64_t nowMs, std::vector<OpRequest>& out);

    /// Snapshot one device now, signalled or not: a preset our own browser
    /// loaded into it, or a save. The same requests a signalled drain makes,
    /// and the same absorbing of its ungestured edits -- and one more: a role
    /// the project has NO row for is written even when the chunk has not
    /// changed since attach, as the first row (no opener: it is where the
    /// plugin already is). That is what saving means.
    std::size_t snapshot(std::int64_t deviceId, std::int64_t nowMs,
                         std::vector<OpRequest>& out);

    /// The bytes behind every `device.loadState` request appended since the
    /// last call, moved out. The caller writes them to `state_blobs` first.
    [[nodiscard]] std::vector<StateBlob> takeBlobs();

    /// `takeBlobs`, written: INSERT OR IGNORE into `state_blobs` (the hash is
    /// the key, so a blob already there is the same bytes). False with
    /// `error` set when the store refused; the blobs are then dropped.
    bool writeBlobs(Store& store, std::string& error);

    /// A `device.setParam` payload that something else wrote -- an undo, the
    /// UI, the agent. Pushes the value into the device with the echo guard
    /// armed. Returns true when the device was set; false when the device
    /// already held the value, the payload clears the row, or nothing matched.
    bool applied(const Payload& payload, std::int64_t nowMs);

    /// ADR-0142. A `device.loadState` payload something else wrote -- an
    /// undo or a redo. Loads the blob into the device (the device mutes its
    /// own answer), then re-reads every parameter. False when the device
    /// already holds that chunk, the blob is missing or refused, or the
    /// payload clears the row -- the inverse of decision 6's opener, which
    /// arrives after the chunk it wrote has been loaded back, so the plugin
    /// already holds it.
    bool appliedState(const Store& store, const Payload& payload, std::int64_t nowMs);

    struct Stats {
        std::int64_t attached = 0;
        std::int64_t edits = 0;          ///< edits drained from the rings
        std::int64_t opsEmitted = 0;     ///< requests appended (edits plus first-touch openers)
        std::int64_t firstTouches = 0;   ///< decision 4: openers emitted
        std::int64_t applied = 0;        ///< devices set by `applied`
        std::int64_t appliedEqual = 0;   ///< skipped: the device already held it
        std::int64_t appliedCleared = 0; ///< skipped: the payload clears the row
        std::int64_t unknownDevice = 0;
        std::int64_t unknownParam = 0;
        // --- ADR-0142 -------------------------------------------------------
        std::int64_t stateSignals = 0;       ///< drains in which a device's epoch moved
        std::int64_t snapshots = 0;          ///< `device.loadState` requests for a new chunk
        std::int64_t snapshotOpeners = 0;    ///< decision 6: the replaced chunk, written first
        std::int64_t snapshotsUnchanged = 0; ///< decision 8: same hash, nothing written
        std::int64_t rowsFollowed = 0;       ///< decision 7: rows rewritten after a chunk
        std::int64_t statelessMoves = 0;     ///< a device with no chunk: moves written as edits
        std::int64_t emptyChunks = 0;        ///< a role that saved nothing
        std::int64_t statesApplied = 0;      ///< chunks loaded by `appliedState`
        std::int64_t statesAppliedEqual = 0; ///< skipped: the device already held it
        std::int64_t statesCleared = 0;      ///< skipped: the payload clears the row
        std::int64_t statesRefused = 0;      ///< the blob was missing, or the plugin refused it
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const ParamEditCapture::Stats* captureStats(std::int64_t deviceId) const noexcept;
    [[nodiscard]] bool isAttached(std::int64_t deviceId) const noexcept;

    /// How close is "the same value" for `applied` (decision 3). Default 1e-6,
    /// the capture's own echo tolerance.
    void setTolerance(double t) noexcept { tolerance_ = t > 0.0 ? t : 0.0; }

private:
    /// One state stream of a device, as the glue last saw it.
    struct Role {
        std::string hash;                    ///< of the chunk the plugin last reported
        bool recorded = false;               ///< a `plugin_state` row exists for it
        std::vector<std::uint8_t> baseline;  ///< decision 6: kept while `!recorded`
    };
    struct Attached {
        device::DeviceInstance* inst = nullptr;
        std::unique_ptr<ParamEditCapture> capture;   ///< stable address: the device holds it
        std::string name;
        bool active = false;
        std::uint64_t seenEpoch = 0;
        std::map<std::string, Role> roles;
        std::vector<double> mirror;   ///< per index: where rows and plugin last agreed
    };
    /// What the project already holds for a device, when a session knows it.
    struct Recorded {
        std::set<std::string> stateRoles;
        std::set<std::string> paramIds;
    };
    bool attachImpl(std::int64_t deviceId, device::DeviceInstance& inst, const Recorded* rec);
    std::size_t emitEdits(std::int64_t id, Attached& a, std::vector<OpRequest>& out);
    std::size_t snapshotOne(std::int64_t id, Attached& a, std::vector<OpRequest>& out, bool save);
    void rereadParams(std::int64_t id, Attached& a);
    [[nodiscard]] static std::string hashOf(const std::vector<std::uint8_t>& bytes);
    [[nodiscard]] static std::int32_t indexOf(const device::DeviceInstance& inst,
                                              const std::string& paramId) noexcept;
    static void fillRequest(OpRequest& r, std::int64_t deviceId, const std::string& name,
                            const device::ParamDescriptor& d, double norm);

    std::map<std::int64_t, Attached> devices_;
    std::set<std::pair<std::int64_t, std::int32_t>> touched_;   ///< decision 4
    std::vector<ParamEdit> scratch_;
    std::vector<StateBlob> blobs_;
    std::int32_t capacity_;
    double tolerance_ = 1e-6;
    Stats stats_;
};

}  // namespace adi::engine
