// SPDX-License-Identifier: GPL-3.0-or-later

#include "juce/pd_device.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace adi::device {

// ---------------------------------------------------------------------------
// PdLatencyReceiver
// ---------------------------------------------------------------------------

std::string PdLatencyReceiver::nameFor(int dollarZero) {
    return std::to_string(dollarZero) + kReportSuffix;
}

std::string PdLatencyReceiver::queryFor(int dollarZero) {
    return std::to_string(dollarZero) + kQuerySuffix;
}

PdLatencyReceiver::Verdict PdLatencyReceiver::report(double samples) noexcept {
    if (!std::isfinite(samples)) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return Verdict::RejectedNonFinite;
    }
    if (samples < 0.0) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return Verdict::RejectedNegative;
    }
    const double whole = std::nearbyint(samples);
    if (whole > static_cast<double>(max_.load(std::memory_order_relaxed))) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return Verdict::RejectedTooLarge;
    }

    // A Pd float is single precision, so a sample count computed as
    // ms * sr / 1000 can arrive as 288.00002. That is an integer in all but
    // representation; a value half a sample off is not, and is counted so a
    // patch that forgot to round shows up in its stats rather than as a
    // compensation that is quietly one sample out.
    const bool fractional = std::fabs(samples - whole) > 1.0e-3;
    if (fractional) rounded_.fetch_add(1, std::memory_order_relaxed);

    const auto n = static_cast<std::int32_t>(whole);
    if (n == latency_.load(std::memory_order_relaxed)) {
        unchanged_.fetch_add(1, std::memory_order_relaxed);
        return fractional ? Verdict::Rounded : Verdict::Unchanged;
    }

    // THE VALUE BEFORE THE EPOCH, both release. The coalescer loads the epoch
    // (acquire) and then asks the graph to re-read latencies, which loads this
    // value (acquire). Stored the other way round, a poll could see the new
    // epoch, retap, and read the OLD latency -- and then see no further change
    // to make it look again.
    latency_.store(n, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_release);
    accepted_.fetch_add(1, std::memory_order_relaxed);
    return fractional ? Verdict::Rounded : Verdict::Accepted;
}

PdLatencyReceiver::Stats PdLatencyReceiver::stats() const noexcept {
    Stats s;
    s.accepted = accepted_.load(std::memory_order_relaxed);
    s.unchanged = unchanged_.load(std::memory_order_relaxed);
    s.rounded = rounded_.load(std::memory_order_relaxed);
    s.rejected = rejected_.load(std::memory_order_relaxed);
    return s;
}

// ---------------------------------------------------------------------------
// PdReceiverTable
// ---------------------------------------------------------------------------

namespace {

/// "<d>-report_latency" into a fixed buffer, with no allocation.
bool formatName(char (&out)[PdReceiverTable::kNameMax], int dollarZero) noexcept {
    const int n = std::snprintf(out, PdReceiverTable::kNameMax, "%d%s", dollarZero,
                                PdLatencyReceiver::kReportSuffix);
    return n > 0 && n < PdReceiverTable::kNameMax;
}

}  // namespace

bool PdReceiverTable::bind(int dollarZero, PdLatencyReceiver& receiver) noexcept {
    char name[kNameMax];
    if (!formatName(name, dollarZero)) return false;

    const int n = count_.load(std::memory_order_acquire);
    // One LIVE entry per name. A tombstone with the same name is fine: it is
    // skipped by dispatch, and this is exactly how a patch closed and
    // reopened with a recycled $0 comes back.
    for (int i = 0; i < n; ++i) {
        const Entry& e = entries_[static_cast<std::size_t>(i)];
        if (e.target.load(std::memory_order_acquire) != nullptr &&
            std::strcmp(e.name, name) == 0)
            return false;
    }
    if (n >= kCapacity) return false;

    Entry& e = entries_[static_cast<std::size_t>(n)];
    std::memcpy(e.name, name, sizeof name);
    e.target.store(&receiver, std::memory_order_release);
    // PUBLISHED LAST. Until this store the audio thread cannot see the slot,
    // so it never reads a name that is still being written.
    count_.store(n + 1, std::memory_order_release);
    return true;
}

bool PdReceiverTable::unbind(int dollarZero) noexcept {
    char name[kNameMax];
    if (!formatName(name, dollarZero)) return false;
    const int n = count_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        Entry& e = entries_[static_cast<std::size_t>(i)];
        if (e.target.load(std::memory_order_acquire) != nullptr &&
            std::strcmp(e.name, name) == 0) {
            // A tombstone, not a reuse: the name stays, only the target goes.
            e.target.store(nullptr, std::memory_order_release);
            return true;
        }
    }
    return false;
}

bool PdReceiverTable::dispatchFloat(const char* receiver, float value) noexcept {
    if (receiver == nullptr) return false;
    const int n = count_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        const Entry& e = entries_[static_cast<std::size_t>(i)];
        PdLatencyReceiver* t = e.target.load(std::memory_order_acquire);
        if (t != nullptr && std::strcmp(e.name, receiver) == 0) {
            t->report(static_cast<double>(value));
            return true;
        }
    }
    return false;
}

int PdReceiverTable::bound() const noexcept {
    const int n = count_.load(std::memory_order_acquire);
    int live = 0;
    for (int i = 0; i < n; ++i)
        if (entries_[static_cast<std::size_t>(i)].target.load(std::memory_order_acquire) != nullptr)
            ++live;
    return live;
}

void PdReceiverTable::clear() noexcept {
    const int n = count_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        Entry& e = entries_[static_cast<std::size_t>(i)];
        e.target.store(nullptr, std::memory_order_relaxed);
        e.name[0] = '\0';
    }
    count_.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// PdDevice
// ---------------------------------------------------------------------------

PdDevice::PdDevice(std::unique_ptr<PdPatchEngine> engine, DeviceIdentity id)
    : engine_(std::move(engine)), id_(std::move(id)) {
    if (engine_ == nullptr) {
        openError_ = "no Pd engine";
        return;
    }
    loaded_ = engine_->open(latency_, openError_);
}

PdDevice::~PdDevice() {
    if (engine_ != nullptr && loaded_) engine_->close();
}

void PdDevice::prepare(double sampleRate, std::int32_t maxFrames) {
    if (!loaded_) return;
    latency_.setMaxSamples(static_cast<std::int32_t>(sampleRate * 10.0));
    engine_->prepare(sampleRate, maxFrames);
    // AFTER the engine knows the real rate. A report from [loadbang] was made
    // at Pd's default 44.1 kHz; this is the one that is true.
    engine_->requestLatencyReport();
}

void PdDevice::process(const engine::NodeIo& io) noexcept {
    // A patch that did not open is still a device (ADR-0011): it passes audio
    // through and holds its place in the chain.
    if (!loaded_) { passThrough(io); return; }
    engine_->process(io);
}

std::int32_t PdDevice::latencySamples() const noexcept {
    return loaded_ ? latency_.latencySamples() : 0;
}

}  // namespace adi::device
