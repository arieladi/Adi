// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace adi::engine {

enum class ParamEventKind : std::uint8_t { Begin, Value, End };
struct ParamEvent {
    std::int64_t deviceId = 0;
    std::int32_t paramIndex = 0;
    ParamEventKind kind = ParamEventKind::Value;
    double value = 0.0;
};
struct ParamEdit {
    std::int64_t deviceId = 0;
    std::int32_t paramIndex = 0;
    double before = 0.0;
    double after = 0.0;
    bool implicit = false;
};

/// ADR-0122 d11 / ADR-0110: capture only, never plugin calls or store ops.
/// Exactly one producer calls push (thread handover requires external sync).
/// All other methods, including stats(), belong to a single consumer thread.
/// Caller supplies monotonic milliseconds. Events carry no timestamp: their
/// lastValueMs is the drain time at which the consumer observes them.
/// The per-parameter table grows only on the consumer: drain discovers events;
/// seed and expectEcho may register a parameter before its first event. push
/// never accesses that table or allocates.
/// Modulation and automation playback must never be submitted by the glue.
class ParamEditCapture {
public:
    // capacity is the number of usable slots; zero rejects every event.
    // Negative capacity or negative/NaN settings throw invalid_argument.
    explicit ParamEditCapture(std::int32_t capacity);
    bool push(const ParamEvent&) noexcept;
    void seed(std::int64_t deviceId, std::int32_t paramIndex, double value);
    void expectEcho(std::int64_t deviceId, std::int32_t paramIndex, double value,
                    std::int64_t nowMs);
    std::size_t drain(std::int64_t nowMs, std::vector<ParamEdit>& out);
    /// ADR-0142: the drain for an interval in which the plugin signalled a
    /// state change (a preset load). Explicit gestures that END in it are
    /// still edits; every ungestured edit open at the end of it is ABSORBED:
    /// no edit, `last` moves to where it went, and the state snapshot the
    /// caller takes next carries the change.
    std::size_t drainAbsorbing(std::int64_t nowMs, std::vector<ParamEdit>& out);
    /// ADR-0154: the parameter most recently touched in the plug-in's own
    /// window -- Live's "temporary entry" and the Master Focus Dial's target
    /// (ADR-0130). Updated by a drain; -1 before any touch. An echo of our own
    /// set is not a touch, and neither is a preset's broadcast absorbed into a
    /// snapshot (drainAbsorbing), unless it was inside an explicit gesture.
    [[nodiscard]] std::int32_t lastTouched() const noexcept { return lastTouched_; }
    void setQuietMs(std::int64_t ms);
    void setEchoTtlMs(std::int64_t ms);
    void setEchoTolerance(double tol);
    struct Stats {
        std::int64_t pushed = 0, dropped = 0, edits = 0, implicitEdits = 0,
                     echoesSwallowed = 0, guardsExpired = 0, strayBegins = 0,
                     strayEnds = 0, unseeded = 0, absorbed = 0;
    };
    // Consumer snapshot; the returned reference is refreshed by the next call.
    // `pushed` and `dropped` are the producer's, copied in only WHEN this is
    // called: a pointer held across pushes reads stale values (ADR-0141).
    // pushed counts accepted events. dropped counts rejected push attempts.
    [[nodiscard]] const Stats& stats() const noexcept;

private:
    struct Parameter {
        bool known = false, open = false, implicit = false, hasValue = false;
        bool guarded = false;
        double last = 0.0, before = 0.0, after = 0.0, echo = 0.0;
        std::int64_t lastValueMs = 0, armedMs = 0;
    };
    using Key = std::pair<std::int64_t, std::int32_t>;
    void close(const Key&, Parameter&, std::vector<ParamEdit>&);
    std::size_t drainImpl(std::int64_t nowMs, std::vector<ParamEdit>& out, bool absorb);
    std::vector<ParamEvent> ring_; // one additional sentinel slot
    std::atomic<std::size_t> read_{0}, write_{0};
    // Only the producer writes these counters; no contended RMW or retry loop.
    std::atomic<std::int64_t> pushed_{0}, dropped_{0};
    std::map<Key, Parameter> parameters_;
    std::int64_t quietMs_ = 150, echoTtlMs_ = 500;
    std::int32_t lastTouched_ = -1;
    double echoTolerance_ = 1e-6;
    mutable Stats stats_{};
};

} // namespace adi::engine
