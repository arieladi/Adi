// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <memory>
#include <span>

namespace adi::mix::opto {
struct NormalizedAttack {
    float value = .341940462589264f;
    bool operator==(const NormalizedAttack &) const = default;
};
struct NormalizedRelease {
    float value = .277943134307861f;
    bool operator==(const NormalizedRelease &) const = default;
};
struct Settings {
    float thresholdDb = -30;
    NormalizedAttack attack;
    NormalizedRelease release;
    bool operator==(const Settings &) const = default;
};
struct Preparation {
    double sampleRate = 48000;
    int maximumFrames = 512;
};
enum class Status {
    Ok,
    NotPrepared,
    InvalidSpec,
    InvalidSettings,
    UnsupportedEnvironment,
    InvalidBuffers,
    AllocationFailed
};
// Unregistered fixed Opto target/envelope: one selected key waveform lane to
// linear gain. Ratio4/knee0/range60 are fixed. The caller owns channel selection,
// linking, programme multiplication, routing, controls and source lifetime.
// One serialized owner; prepare/reset/destruction require stopped processing.
class GainEnvelope final {
  public:
    GainEnvelope() noexcept;
    ~GainEnvelope();
    GainEnvelope(const GainEnvelope &) = delete;
    GainEnvelope &operator=(const GainEnvelope &) = delete;
    GainEnvelope(GainEnvelope &&) = delete;
    GainEnvelope &operator=(GainEnvelope &&) = delete;
    // Rates44.1..96kHz use the float-rounded processing rate; maximumFrames1..4096.
    // Normalized A/R must be finite in[0,1], threshold finite in[-60,0]dB.
    // Nearest rounding is required before shared-table initialization and must
    // remain stable during processing. The caller retains its FTZ/DAZ policy.
    // Failure preserves all previous settings, histories and cache state.
    [[nodiscard]] Status prepare(const Preparation &, const Settings & = {}) noexcept;
    // Authored fresh-state lifecycle: clear histories/cache, retain controls.
    void reset() noexcept;
    [[nodiscard]] bool isPrepared() const noexcept;
    [[nodiscard]] const Settings *settings() const noexcept;
    // Equal lengths up to maximumFrames; empty prepared spans are a no-op.
    // Valid complete aligned storage is required. Exact alias works; partial
    // overlap and unrepresentable byte spans refuse before any reads/writes.
    // Checked finite-input refusal is atomic, so corrected input may retry.
    [[nodiscard]] Status processChecked(std::span<const float> key, std::span<float> gain) noexcept;
    // Same guards without the finite scan. Producer guarantees finite samples.
    // Consecutive spans retain continuous histories; no callback-local cadence.
    // Both paths allocate/free nothing and use no locks, worker or virtual call.
    [[nodiscard]] Status processTrustedFinite(std::span<const float> key,
                                              std::span<float> gain) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::mix::opto
