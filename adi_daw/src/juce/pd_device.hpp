// SPDX-License-Identifier: GPL-3.0-or-later
//
// A Pure Data patch as a device, and the latency protocol it speaks — ADR-0095.
//
// ADR-0035 put a visual patching tier into the project via libpd and never said
// how a patch tells the graph it has latency. A lookahead limiter written in Pd
// delays its audio by its lookahead; if the graph does not know, every other
// track is compensated against a delay that is not there (ADR-0058).
//
// THE PROTOCOL, from the patch's side:
//
//     [r $0-query_latency]          the host asks
//     |
//     [samples-of-lookahead]        the patch works out its delay IN SAMPLES
//     |                             at the CURRENT sample rate
//     [s $0-report_latency]         and answers
//
// The patch also reports on its own whenever the delay changes -- a lookahead
// switched from 1.5 ms to 6 ms -- and may report from [loadbang].
//
// TWO DECISIONS HIDE IN THAT, and both correct the obvious version:
//
//   1. **`$0-`, not a bare `report_latency`.** Pd's send/receive names are
//      GLOBAL within a Pd instance. Two limiters on two tracks both sending to
//      `report_latency` put two numbers on one name, and the host cannot tell
//      which patch said which. `$0` is unique per opened patch and libpd hands
//      it back (`libpd_getdollarzero`), so the host binds one name per patch.
//
//   2. **A query, not only a report.** A patch that reports from [loadbang]
//      reports at whatever sample rate Pd has then, and libpd has not been told
//      the real rate yet -- Pd starts at 44.1 kHz. A 1.5 ms lookahead reports
//      66 samples in a 48 kHz session that needs 72, and a compensation one
//      sample short of the truth is the misalignment ADR-0058 exists to remove.
//      So after every `prepare` the host sends `$0-query_latency` and the patch
//      answers at the rate it is actually running at.
//
// THE UNIT IS SAMPLES. Not milliseconds: the graph compensates whole samples,
// and a patch that converted ms to samples its own way and a host that did it
// another would disagree by one exactly when rounding differs.
//
// THE LIBPD GLUE is five calls and lives in the engine adapter, which arrives
// with libpd itself (ADR-0035 has no libpd in the tree yet):
//
//     void* h = libpd_openfile(file, dir);
//     int   d = libpd_getdollarzero(h);
//     table.bind(d, device.latency());
//     libpd_bind(PdLatencyReceiver::nameFor(d).c_str());
//     libpd_set_floathook([](const char* r, float f) { table.dispatchFloat(r, f); });
//     ...
//     libpd_bang(PdLatencyReceiver::queryFor(d).c_str());   // after prepare
//
// Everything with logic in it -- validation, attribution, thread safety, the
// device, its route into DeviceHost and the coalescer -- is here, compiled on
// every ABI and tested with no libpd present.

#pragma once

#include "juce/device_model.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace adi::device {

// ---------------------------------------------------------------------------
// The receiving end
// ---------------------------------------------------------------------------

/// Takes a patch's reported latency, checks it, and holds it where the graph
/// and the coalescer can read it.
///
/// **Called from the audio thread.** libpd invokes its message hooks from
/// inside `libpd_process_*`, so a report arrives mid-block on whatever thread is
/// rendering. It does what every other reporter in this project does: validate,
/// store, bump a counter, return. It never calls into the graph (ADR-0066,
/// ADR-0082).
class PdLatencyReceiver {
public:
    static constexpr const char* kReportSuffix = "-report_latency";
    static constexpr const char* kQuerySuffix = "-query_latency";

    /// "<dollarZero>-report_latency". Builds a string: message thread only.
    [[nodiscard]] static std::string nameFor(int dollarZero);
    /// "<dollarZero>-query_latency", which the host bangs after `prepare`.
    [[nodiscard]] static std::string queryFor(int dollarZero);

    enum class Verdict : std::uint8_t {
        Accepted,           ///< a new value, and the epoch moved
        Unchanged,          ///< the value it already had; nothing moved
        Rounded,            ///< not a whole number of samples; the nearest was taken
        RejectedNonFinite,  ///< NaN or infinity
        RejectedNegative,   ///< a delay cannot be negative
        RejectedTooLarge,   ///< past `maxSamples()`; kept the old value
    };

    /// Any thread. Never allocates, never blocks.
    Verdict report(double samples) noexcept;

    [[nodiscard]] std::int32_t latencySamples() const noexcept {
        return latency_.load(std::memory_order_acquire);
    }
    /// Bumped once per CHANGE, never per report: a patch that re-sends the
    /// same value on every parameter touch must not make the graph retap.
    [[nodiscard]] std::uint64_t epoch() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }

    /// The largest delay believed. A sanity bound, not a feature: a patch that
    /// sends garbage should not make the graph grow a ring to hold it
    /// (ADR-0085). `PdDevice` sets it to ten seconds at the running rate.
    void setMaxSamples(std::int32_t n) noexcept {
        max_.store(n > 0 ? n : 0, std::memory_order_relaxed);
    }
    [[nodiscard]] std::int32_t maxSamples() const noexcept {
        return max_.load(std::memory_order_relaxed);
    }

    struct Stats {
        std::int64_t accepted = 0;
        std::int64_t unchanged = 0;
        std::int64_t rounded = 0;
        std::int64_t rejected = 0;
    };
    /// A snapshot. Each counter is atomic; the set is not, which is fine for
    /// something read to answer "why did this patch not compensate".
    [[nodiscard]] Stats stats() const noexcept;

private:
    std::atomic<std::int32_t> latency_{0};
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<std::int32_t> max_{480000};    // ten seconds at 48 kHz until prepared
    std::atomic<std::int64_t> accepted_{0}, unchanged_{0}, rounded_{0}, rejected_{0};
};

// ---------------------------------------------------------------------------
// Routing libpd's single hook to the right patch
// ---------------------------------------------------------------------------

/// libpd has ONE float hook per Pd instance, and every bound receiver name
/// arrives through it. This routes each message to the patch it names.
///
/// **Allocation-free on the dispatch path**, which is the reason it exists
/// rather than an `unordered_map<std::string, ...>`: looking up a `const char*`
/// in one means constructing a `std::string`, and that allocates on the audio
/// thread. Here the names are fixed character arrays compared with `strcmp`.
///
/// **Safe to bind while the audio thread dispatches.** An entry is written in
/// full before the count that makes it visible is published (release), and a
/// dispatch reads the count first (acquire). Unbinding tombstones the entry
/// instead of reusing the slot, so a dispatch scanning past it never reads a
/// name half-overwritten by the next patch.
class PdReceiverTable {
public:
    /// Patches bound over this table's lifetime. Tombstones are not reclaimed
    /// while audio may be running; `clear()` resets it when libpd is torn down.
    static constexpr int kCapacity = 256;
    /// "-2147483648-report_latency" is 26 characters; this leaves room.
    static constexpr int kNameMax = 40;

    PdReceiverTable() = default;
    PdReceiverTable(const PdReceiverTable&) = delete;
    PdReceiverTable& operator=(const PdReceiverTable&) = delete;

    /// Message thread. False when full, or when this `$0` is already bound.
    bool bind(int dollarZero, PdLatencyReceiver& receiver) noexcept;

    /// Message thread. False when nothing live is bound to this `$0`.
    bool unbind(int dollarZero) noexcept;

    /// AUDIO thread -- libpd's float hook forwards here. True when a live
    /// entry took the message; false for a name we did not bind, which is
    /// every OTHER receiver in every other patch and is not an error.
    bool dispatchFloat(const char* receiver, float value) noexcept;

    /// Live entries.
    [[nodiscard]] int bound() const noexcept;
    /// Slots used, tombstones included.
    [[nodiscard]] int used() const noexcept { return count_.load(std::memory_order_acquire); }

    /// Message thread, with audio STOPPED. Forgets everything.
    void clear() noexcept;

private:
    struct Entry {
        char name[kNameMax] = {};
        std::atomic<PdLatencyReceiver*> target{nullptr};
    };
    std::array<Entry, kCapacity> entries_{};
    std::atomic<int> count_{0};
};

// ---------------------------------------------------------------------------
// The patch as a device
// ---------------------------------------------------------------------------

/// The part that IS libpd: open a patch, render it, ask it things.
///
/// An interface so that `PdDevice`, its latency protocol and its route into the
/// graph are built and tested on every ABI with no libpd present -- the same
/// seam `ClapDevice` has against a fake `clap_plugin_t`.
class PdPatchEngine {
public:
    virtual ~PdPatchEngine() = default;

    /// Open the patch and bind its `$0-report_latency` to `latency`. Message
    /// thread. False with `error` set when it cannot.
    virtual bool open(PdLatencyReceiver& latency, std::string& error) = 0;
    virtual void close() noexcept = 0;

    /// Message thread. Tell libpd the real rate and block size.
    virtual void prepare(double sampleRate, std::int32_t maxFrames) = 0;

    /// Message thread. Bang `$0-query_latency`. libpd delivers a message to a
    /// patch synchronously, so the answer has arrived when this returns.
    virtual void requestLatencyReport() = 0;

    /// Audio thread. The `DeviceInstance::process` contract, including
    /// ADR-0078: `io.in`/`io.out` address the BLOCK, `blockOffset` the segment.
    virtual void process(const engine::NodeIo& io) noexcept = 0;
};

/// A Pd patch in a chain, wired into delay compensation through the ordinary
/// `DeviceInstance` contract.
///
/// That is the whole of "forward the latency to DeviceHost and the coalescer":
/// `DeviceHost::add` registers every device's `latencyEpoch()` as a coalescer
/// source without asking what format it is (ADR-0090), and the graph reads
/// `latencySamples()`. A patch that reports through the protocol is compensated
/// by exactly the machinery a VST3 or a CLAP plugin is.
class PdDevice final : public DeviceInstance {
public:
    PdDevice(std::unique_ptr<PdPatchEngine> engine, DeviceIdentity id);
    ~PdDevice() override;

    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return loaded_; }

    /// Prepares the engine at the real rate, THEN asks the patch its latency
    /// -- the order is the fix for decision 2 above.
    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void release() override {}
    void process(const engine::NodeIo& io) noexcept override;

    /// Zero until loaded: a patch that failed to open passes audio straight
    /// through and delays nothing, so claiming its reported latency would
    /// compensate against a delay that is not happening.
    [[nodiscard]] std::int32_t latencySamples() const noexcept override;
    [[nodiscard]] std::uint64_t latencyEpoch() const noexcept override {
        return latency_.epoch();
    }

    [[nodiscard]] PdLatencyReceiver& latency() noexcept { return latency_; }
    [[nodiscard]] const std::string& openError() const noexcept { return openError_; }

private:
    std::unique_ptr<PdPatchEngine> engine_;
    DeviceIdentity id_;
    PdLatencyReceiver latency_;
    std::string openError_;
    bool loaded_ = false;
};

}  // namespace adi::device
