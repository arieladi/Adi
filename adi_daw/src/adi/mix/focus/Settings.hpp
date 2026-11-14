// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace adi::mix::focus {
inline constexpr std::size_t kNodes = 10;
enum class Engine : std::uint8_t { Iir, Fir };
enum class Detector : std::uint8_t { SideChain, Difference };
enum class PhaseMode : std::uint8_t { Off, Match, Rotation };
struct NodeSettings {
    bool enabled = false, bypass = false, solo = false;
    double frequencyHz = 1000., q = 3.;
    double thresholdDb = -30., differenceDb = -4.5;
    double attackMs = 10., releaseMs = 50.;
    // 0=Mid, 0.5=Stereo, 1=Side; intermediate positions attenuate one domain.
    double position = .5;
    bool operator==(const NodeSettings &) const = default;
};
struct Settings {
    Engine engine = Engine::Iir;
    Detector detector = Detector::SideChain;
    PhaseMode phaseMode = PhaseMode::Off;
    bool externalKey = true;
    double matchAmount = 0., rotationDegrees = 0.;
    double phaseMinimumHz = 0.;
    // Reserved storage only: the current phase transform has no working upper
    // bound control. Adapters must not advertise this as an editable parameter.
    double phaseMaximumHz = 20000.;
    double wet = 1., inputGainDb = 0., outputGainDb = 0.;
    // Calibration changes detector context; main calibration is compensated
    // after processing and is distinct from the audible input/output trims.
    double keyCalibrationDb = 0., inputCalibrationDb = 0.;
    std::array<NodeSettings, kNodes> nodes{};
    bool operator==(const Settings &) const = default;
};
// Prepared identity: node frequency/Q, bank and phase mode/topology class.
// Active Match amounts and interior Rotation angles are live scalar controls.
[[nodiscard]] bool sameConfiguration(const Settings &, const Settings &) noexcept;
// Preserve a chosen prepared plan while retaining current scalar controls. A
// phase mode/class transition pins its amount/angle to the chosen plan too.
[[nodiscard]] Settings pinConfiguration(const Settings &controls, const Settings &plan) noexcept;
[[nodiscard]] bool valid(const Settings &) noexcept;
} // namespace adi::mix::focus
