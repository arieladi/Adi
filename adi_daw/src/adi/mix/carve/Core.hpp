// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <array>
#include <cstdint>

namespace adi::mix::carve {
struct PrepareSpec {
    // Rates below 44.1 kHz cannot represent the complete authored band range.
    double sampleRate = 48000.;
    std::int32_t maximumFrames = 512, channels = 2;
};
enum class Activity : std::uint8_t { Sleeping, Releasing, Active };

// Single audio owner. prepare/reset require stopped processing. No allocation,
// synchronization, host services or background work occurs in this component.
// Both ports use float DSP states. A double active signal is converted at the
// boundary; inactive and disabled signals preserve the original samples exactly.
class Core {
  public:
    void prepare(PrepareSpec, const Settings &);
    // Invalid settings leave the current instance unchanged.
    void reset(const Settings &) noexcept;
    // Planar mono/stereo, in place. Key layout is independent of main layout:
    // zero channels releases with zero input; one channel broadcasts to both.
    // Disabled processing retains histories and performs no signal work.
    // Each call is a control boundary for Freeze hold and tiny-state cleanup.
    // Invalid calls return false without changing samples or DSP state.
    bool process(float *const *main, const float *const *key, std::int32_t keyChannels,
                 std::int32_t frames, const Settings &) noexcept;
    bool process(double *const *main, const double *const *key, std::int32_t keyChannels,
                 std::int32_t frames, const Settings &) noexcept;
    [[nodiscard]] std::int32_t latencySamples() const noexcept { return 0; }
    [[nodiscard]] Activity activity() const noexcept { return activity_; }

  private:
    static constexpr std::size_t bands = 32;
    struct Coefficients {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct Memory {
        float first = 0, second = 0;
        float tick(float, const Coefficients &) noexcept;
        void clearTiny() noexcept;
    };
    struct Band {
        Coefficients detector, bandpass, notch;
        std::array<Memory, 2> detection{}, pass{}, reject{};
        float envelope = 0, smoothing = 0, held = 0;
    };
    template <class Sample>
    bool processImpl(Sample *const *, const Sample *const *, std::int32_t, std::int32_t,
                     const Settings &) noexcept;
    static Coefficients design(float, float, double, bool) noexcept;
    void admit(const Settings &) noexcept;
    void clearSignalState(bool preserveHeld = false) noexcept;
    void clearProgrammeState() noexcept;
    void observeKey(float) noexcept;
    float detect(float, std::array<float, bands> &) noexcept;
    float programme(float, std::size_t, float, const std::array<float, bands> &, bool) noexcept;
    void finish(bool, const std::array<float, bands> &) noexcept;
    std::array<Band, bands> bank_{};
    PrepareSpec spec_{};
    Settings settings_{};
    float attack_ = 0, release_ = 0, smoothing_ = 0, amount_ = 0;
    float blend_ = 1, blendStep_ = 1;
    std::int32_t quietFrames_ = 0, quietLimit_ = 1;
    bool prepared_ = false, coefficientsValid_ = false, keyPresent_ = false;
    bool firstActivation_ = true, programmeCleared_ = true;
    bool normalSleeping_ = true, auditionSleeping_ = true;
    Activity activity_ = Activity::Sleeping;
};
} // namespace adi::mix::carve
