// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace adi::mix::eq {
inline constexpr std::size_t bandCount = 24;
enum class Structure : std::uint8_t { Minimum, SVF, Parallel, Matched, Mixed, Zero };
enum class Status : std::uint8_t { Off, Bypass, On };
enum class Route : std::uint8_t { Stereo, Left, Right, Mid, Side };
enum class Shape : std::uint8_t {
    Bell,
    LowShelf,
    HighCut,
    HighShelf,
    LowCut,
    Notch,
    BandPass,
    TiltShelf,
    FlatTilt,
    AllPass,
    FlatGain
};
enum class SideShape : std::uint8_t { BandPass, LowPass, HighPass, All };
enum class Resolution : std::uint8_t { Low, Medium, High };
struct BandSettings {
    Status status = Status::Off;
    Shape shape = Shape::Bell;
    std::uint8_t order = 2;
    Route route = Route::Stereo;
    float frequencyHz = 1000, gainDb = 0, q = 1;
    bool sideLink = false, dynamicOn = false, dynamicBypass = false, dynamicLearn = false,
         dynamicRelative = false, sideSwap = false;
    float thresholdDb = -40, kneeDb = 8, attackMs = 100, releaseMs = 500, rmsLengthMs = 0, rmsMixPercent = 50,
          smoothPercent = 100;
    SideShape sideShape = SideShape::BandPass;
    std::uint8_t sideOrder = 2;
    float sideFrequencyHz = 1000, sideQ = .707f, targetGainDb = 0;
    float fractionalSlope = -1;
    bool spectralOn = false;
    float spectralDensityPercent = 50;
    bool spectralTilt = true, spectralAutomaticThreshold = true;
    bool operator==(const BandSettings &) const = default;
};
struct Settings {
    std::array<BandSettings, bandCount> bands{};
    Structure structure = Structure::Minimum;
    bool externalSidechain = false, bypass = false;
    float outputGainDb = 0, gainScalePercent = 100;
    bool automaticGain = false, staticGain = false, polarity = false;
    float lookaheadMs = 0;
    Resolution spectralResolution = Resolution::Medium;
    // Transient audition controls are distinct from the persisted parameter model.
    std::int32_t soloBand = -1;
    bool soloSide = false;
    float soloGainDb = 0;
    bool loudnessMatch = false;
    bool operator==(const Settings &) const = default;
};
// Resolves the existing main→linked-side control dependency. The owner persists
// returned side values through its normal edit/gesture path. No host is called.
Settings resolveLinkedChanges(const Settings &previous, Settings desired) noexcept;
// Retain an immutable latency/topology plan while applying current scalar
// controls. Does not create a request, mutate state or admit audio processing.
Settings pinPlanSettings(Settings controls, const Settings &plan) noexcept;
bool valid(const Settings &) noexcept;
} // namespace adi::mix::eq
