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
    void setQuietMs(std::int64_t ms);
    void setEchoTtlMs(std::int64_t ms);
    void setEchoTolerance(double tol);
    struct Stats {
        std::int64_t pushed = 0, dropped = 0, edits = 0, implicitEdits = 0,
                     echoesSwallowed = 0, guardsExpired = 0, strayBegins = 0,
                     strayEnds = 0, unseeded = 0;
    };
    // Consumer snapshot; the returned reference is refreshed by the next call.
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
    std::vector<ParamEvent> ring_; // one additional sentinel slot
    std::atomic<std::size_t> read_{0}, write_{0};
    // Only the producer writes these counters; no contended RMW or retry loop.
    std::atomic<std::int64_t> pushed_{0}, dropped_{0};
    std::map<Key, Parameter> parameters_;
    std::int64_t quietMs_ = 150, echoTtlMs_ = 500;
    double echoTolerance_ = 1e-6;
    mutable Stats stats_{};
};

} // namespace adi::engine
