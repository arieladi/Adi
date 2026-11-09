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
//      Session-bound lifetime: immutable callback bindings retain capture and
//      fence owners. Detach unpublishes the binding; owner reclamation waits
//      for announced readers to be quiescent without blocking callbacks.
//      Session retains pending runtime restoration after frontend destruction
//      and revokes its endpoint before devices die. Standalone attach keeps
//      the borrowed contract: device outlives ParamOps or caller detaches and
//      establishes callback quiescence first.
//   2. **Normalized is the wire unit.** Both formats can produce 0..1 (VST3
//      natively; CLAP from the declared range), and `device.setParam` carries
//      `norm` always and `real` when the descriptor has a range (ADR-0057).
//   3. **`applied` compares before it sets.** Matching current value AND last
//      accepted intent is skipped. A getter can lag a queued CLAP set, so an
//      undo back to the old audible value still supersedes that pending set.
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
// With attachSession, one completed common gesture is offered per output
// vector. Requests carry a sealed capturedTransaction owning guards/resources;
// ordinary OpJournal::commit installs its state blobs atomically. Refusal and
// cancellation remain owned and inspectable. writeBlobs stages without SQL;
// takeBlobs returns a copy and cannot release the retained resources.
// Standalone attach preserves the earlier unsealed packet/blob interface.
#pragma once

#include "adi/engine/param_edits.hpp"
#include "adi/ops.hpp"
#include "adi/gesture_presentation.hpp"
#include "juce/device_model.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace adi {
class Store;
}

namespace adi::engine {

class Session;

/// ADR-0142: owned bytes named by a device.loadState request. Session-bound
/// copies are diagnostic/staging values; the sealed journal packet owns the
/// resources inserted in its transaction. Standalone callers install them.
struct StateBlob {
    std::string hash;                 ///< BLAKE3, 64 lowercase hex digits
    std::vector<std::uint8_t> bytes;
};

enum class AppliedDeliveryState { Idle, Pending, AwaitingAttachment, Refused, Invalidated };
struct AppliedDeliveryStatus {
    AppliedDeliveryState state = AppliedDeliveryState::Idle;
    std::size_t pendingBatchCount = 0;
    std::uint64_t batchSerial = 0;
    std::size_t opIndex = 0;
    bool reconcilingParameters = false;
    std::size_t projectionIndex = 0;
    std::size_t parameterIndex = 0;
    std::string error;
};

/// Immutable value observation of an internally retained common gesture.
/// The owner token is core-issued; retaining this snapshot never retains SQL.
struct PendingParameterGesture {
    std::shared_ptr<const gesture::OwnerToken> owner;
    std::int64_t deviceId = 0;
    std::string parameterId; // empty for a whole-device state gesture
    gesture::State state = gesture::State::Open;
    CapturedSubmissionPhase submission = CapturedSubmissionPhase::Offered;
    gesture::Status status;
    std::vector<OpRequest> requests;
};

class ParamOps {
public:
    explicit ParamOps(std::int32_t ringCapacity = 1024);
    ~ParamOps();
    ParamOps(const ParamOps&) = delete;
    ParamOps& operator=(const ParamOps&) = delete;

    // --- message thread -----------------------------------------------------
    /// Session-bound capture retains ownership after offering legacy packets.
    /// Failed submit/cancel stays discoverable; retry never resubmits Durable.
    [[nodiscard]] std::vector<PendingParameterGesture> pendingGestures() const;
    gesture::CommitOutcome commitPending(const gesture::OwnerToken&, std::int64_t nowMs);
    gesture::CommitOutcome retryPendingCommit(const gesture::OwnerToken&, std::int64_t nowMs);
    gesture::CancelOutcome cancelPending(const gesture::OwnerToken&, std::int64_t nowMs);
    gesture::CancelOutcome retryPendingCancel(const gesture::OwnerToken&, std::int64_t nowMs);
    gesture::DrainOutcome pumpGestures(std::int64_t nowMs);


    /// N7, engine lift of 537c56d: synchronous window producer, separate from
    /// every plug-in callback ring. Values reach the plug-in immediately;
    /// Begin/Value/End emit a gesture and its first-touch opener into out.
    /// Unbracketed values close on drain's quiet timer. The return is whether
    /// THIS event was accepted. Commit ALL appended requests (even on refusal:
    /// an older gesture may have completed), writing takeBlobs/writeBlobs first
    /// as for drain. Every call belongs to the same owner/message thread.
    bool uiEvent(std::int64_t deviceId, const std::string& paramId, ParamEventKind,
                 double value, std::int64_t nowMs, std::vector<OpRequest>& out);

    /// Standalone compatibility: seed one borrowed sink/ring. Device must
    /// outlive this frontend or be detached with caller-proven quiescence.
    /// False when the device is already attached. Prefer attachSession for
    /// owned callback bindings and the common gesture lifecycle.
    bool attach(std::int64_t deviceId, device::DeviceInstance& inst);

    /// Unpublish input without waiting. Session-bound rings/fences/instances
    /// remain retained for in-flight capture and restoration. Reattachment
    /// defers while a prior callback is announced; it preserves queued input.
    void detach(std::int64_t deviceId);

    /// Attach every loaded, non-retired entry of a session that is not yet
    /// attached, detach its retired entries and retain the session owner for
    /// live automation override. Returns new attachments; call after refresh.
    std::size_t attachSession(Session& s);

    /// N47: source notifications only enqueue owned committed batches. Before
    /// destructive delivery, transfer pending parameter/state input into owned
    /// common gestures. A refused suffix and later batches remain FIFO.
    /// SQL success is never changed by a runtime refusal. Repeated attachSession
    /// on the same source preserves this queue and its acknowledged cursor.
    [[nodiscard]] AppliedDeliveryStatus retryApplied(std::int64_t nowMs);
    [[nodiscard]] const AppliedDeliveryStatus& appliedDeliveryStatus() const noexcept;

    /// Session-bound: retain all observed input, then offer at most one
    /// completed gesture into an empty vector. Later gestures remain FIFO;
    /// Refused requires explicit retry, Durable is never offered again.
    /// Ordinary OpJournal::commit consumes the complete typed group. The
    /// return is the number of appended requests. Standalone compatibility
    /// drains legacy edits/state packets together and uses takeBlobs/writeBlobs.
    std::size_t drain(std::int64_t nowMs, std::vector<OpRequest>& out);

    /// Snapshot one device now, signalled or not: a preset our own browser
    /// loaded into it, or a save. The same requests a signalled drain makes,
    /// and the same absorbing of its ungestured edits -- and one more: a role
    /// the project has NO row for is written even when the chunk has not
    /// changed since attach, as the first row (no opener: it is where the
    /// plugin already is). That is what saving means.
    std::size_t snapshot(std::int64_t deviceId, std::int64_t nowMs,
                         std::vector<OpRequest>& out);

    /// Session-bound: copy the first offered group's retained resources.
    /// Taking/dropping this copy cannot release common ownership. Standalone:
    /// move out the legacy blob staging vector for caller installation.
    [[nodiscard]] std::vector<StateBlob> takeBlobs();

    /// Session-bound: validate the source and stage only, with zero SQL
    /// writes; ordinary typed commit installs resources atomically. Standalone:
    /// INSERT OR IGNORE legacy blobs, returning false/error on write failure.
    bool writeBlobs(Store& store, std::string& error);

    /// A `device.setParam` payload that something else wrote -- an undo, the
    /// UI, the agent. Pushes the value into the device with the echo guard
    /// armed. Returns true when the device was set; false when the device
    /// already held the value, the payload clears the row, or nothing matched.
    /// A non-clearing payload alone does not acknowledge a stored row. That
    /// bookkeeping belongs to the committed applied-batch receipt.
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
        std::int64_t appliedRefused = 0; ///< setParam refused; no mirror/echo intent retained
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
    [[nodiscard]] const Stats& stats() const noexcept;
    [[nodiscard]] const ParamEditCapture::Stats* captureStats(std::int64_t deviceId) const noexcept;
    [[nodiscard]] const ParamEditCapture::Stats* uiCaptureStats(std::int64_t deviceId) const noexcept;
    [[nodiscard]] bool isAttached(std::int64_t deviceId) const noexcept;

    /// ADR-0154: the parameter last touched in this device's own window,
    /// as of the last drain; -1 when none, or the device is not attached.
    /// Live's temporary entry, and the Master Focus Dial's target (ADR-0130).
    [[nodiscard]] std::int32_t lastTouched(std::int64_t deviceId) const noexcept;

    /// How close is "the same value" for `applied` (decision 3). Default 1e-6,
    /// the capture's own echo tolerance.
    void setTolerance(double t) noexcept;

private:
    struct Runtime;
    std::shared_ptr<Runtime> runtime_;
};

}  // namespace adi::engine
