// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <optional>

namespace adi::mix::deesser {
// Invalid/nonfinite unit controls return no value. Helpers do not clamp.
// Threshold is piecewise over [0,1], producing finite [-180,0] dB. A display
// may label the lower endpoint as silence; it is not an infinite DSP value.
// These are physical conversion helpers, without previous controller state or
// a same-target deadband. They do not replay a host parameter setter lifecycle.
[[nodiscard]] std::optional<float> thresholdFromNormalized(float) noexcept;
[[nodiscard]] std::optional<float> rangeFromNormalized(float) noexcept;
[[nodiscard]] std::optional<float> frequencyFromNormalized(float) noexcept;
[[nodiscard]] std::optional<float> gainFromNormalized(float) noexcept;
[[nodiscard]] std::optional<float> panFromNormalized(float) noexcept;

enum class Admission { Changed, Unchanged, Invalid };

// Small scalar target state, independent of the prepared audio Core. Admission
// compares raw target values with an absolute binary32 epsilon deadband; an
// ignored request retains the last admitted target. This is not smoothing,
// audio automation, host serialization, or a UI/undo controller.
// Factories use the individual control's raw default. Settings defaults are
// separate convenience values and do not describe a complete factory preset.
class ScalarControlState {
  public:
    [[nodiscard]] static ScalarControlState threshold() noexcept;
    [[nodiscard]] static ScalarControlState range() noexcept;
    [[nodiscard]] static ScalarControlState level() noexcept;
    [[nodiscard]] static ScalarControlState pan() noexcept;
    [[nodiscard]] Admission admitNormalized(float) noexcept;
    void reset() noexcept;
    [[nodiscard]] float rawValue() const noexcept { return raw_; }
    // Threshold/Range: dB; Level: linear amplitude; Pan: bipolar position.
    [[nodiscard]] float physicalValue() const noexcept;
    // Normalization of the admitted raw target, not a display-text roundtrip.
    [[nodiscard]] float normalizedValue() const noexcept;

  private:
    enum class Kind { Threshold, Range, Level, Pan };
    explicit ScalarControlState(Kind kind) noexcept : kind_(kind) { reset(); }
    Kind kind_;
    float raw_ = 0;
};
} // namespace adi::mix::deesser
