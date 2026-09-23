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

namespace adi::engine {

class Session;

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
    std::size_t drain(std::int64_t nowMs, std::vector<OpRequest>& out);

    /// A `device.setParam` payload that something else wrote -- an undo, the
    /// UI, the agent. Pushes the value into the device with the echo guard
    /// armed. Returns true when the device was set; false when the device
    /// already held the value, the payload clears the row, or nothing matched.
    bool applied(const Payload& payload, std::int64_t nowMs);

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
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const ParamEditCapture::Stats* captureStats(std::int64_t deviceId) const noexcept;
    [[nodiscard]] bool isAttached(std::int64_t deviceId) const noexcept;

    /// How close is "the same value" for `applied` (decision 3). Default 1e-6,
    /// the capture's own echo tolerance.
    void setTolerance(double t) noexcept { tolerance_ = t > 0.0 ? t : 0.0; }

private:
    struct Attached {
        device::DeviceInstance* inst = nullptr;
        std::unique_ptr<ParamEditCapture> capture;   ///< stable address: the device holds it
        std::string name;
        bool active = false;
    };
    [[nodiscard]] static std::int32_t indexOf(const device::DeviceInstance& inst,
                                              const std::string& paramId) noexcept;
    static void fillRequest(OpRequest& r, std::int64_t deviceId, const std::string& name,
                            const device::ParamDescriptor& d, double norm);

    std::map<std::int64_t, Attached> devices_;
    std::set<std::pair<std::int64_t, std::int32_t>> touched_;   ///< decision 4
    std::vector<ParamEdit> scratch_;
    std::int32_t capacity_;
    double tolerance_ = 1e-6;
    Stats stats_;
};

}  // namespace adi::engine
