// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <span>

namespace adi::mix::filters {

struct Coefficients {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    bool operator==(const Coefficients &) const = default;
};

// Descending analog polynomials, angular scale and three explicit match points.
// Shape/prototype selection and control scheduling belong to the caller.
struct MatchRequest {
    std::array<double, 3> numerator{0, 1, 1};
    std::array<double, 3> denominator{0, 1, 1};
    double angularFrequency = 1;
    std::array<double, 3> points{1, .1, .5};
    double highReference = 3;
    bool operator==(const MatchRequest &) const = default;
};

enum class Status { Ok, InvalidArgument, UnsupportedConfiguration };
enum class Update { Changed, Unchanged, InvalidArgument, UnsupportedConfiguration };

bool validCoefficients(const Coefficients &) noexcept;

// Preparation only. The ordinary, distinct, positive three-point branch and
// positive constant response are supported. Other singular branches are refused without
// changing output. No coefficient fitting, allocation or recursive state here.
Status design(const MatchRequest &, Coefficients &output) noexcept;

class Designer final {
  public:
    Update update(const MatchRequest &) noexcept;
    void reset() noexcept { prepared_ = false; }
    bool isPrepared() const noexcept { return prepared_; }
    const Coefficients &coefficients() const noexcept { return coefficients_; }

  private:
    MatchRequest request_{};
    Coefficients coefficients_{};
    bool prepared_ = false;
};

// One channel/section. Coefficient publication preserves double Direct Form I
// histories. Every section publishes float; callers must preserve that boundary
// when composing a cascade. The serialized caller supplies finite samples.
class DfiSection final {
  public:
    bool setCoefficients(const Coefficients &) noexcept;
    const Coefficients &coefficients() const noexcept { return coefficients_; }
    void reset() noexcept { x1_ = x2_ = y1_ = y2_ = 0; }
    // Both loops compile with this component's precise arithmetic contract,
    // independently of the caller's contraction/fast-math flags.
    float process(float input) noexcept;
    void processInPlace(std::span<float> samples) noexcept;

  private:
    Coefficients coefficients_{};
    double x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

} // namespace adi::mix::filters
