// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/blob.hpp"
#include "adi/ops.hpp"

namespace adi { class Store; }
namespace adi::automation {
enum class Shape {
    Sine, Triangle, Sawtooth, InverseSawtooth, Square,
    ExponentialRise, ExponentialFall, LogarithmicRise, LogarithmicFall, ADSR
};
struct ShapeSpec {
    Shape shape = Shape::Sine;
    std::int64_t start = 0, end = 0;
    double low = 0, high = 1;
    std::uint32_t cycles = 1, stepsPerCycle = 128;
    // Fractions of one cycle in hundredths of a percent; sustain is normalized.
    std::uint32_t attack = 1000, decay = 2000, release = 2000;
    double sustain = 0.7;
};
// Pure, bounded, owner-thread generation. IDs are zero until preparation resolves
// them; exact integer division places times even beyond double's integer range.
std::vector<AutomationPoint> generateShape(const ShapeSpec&);
struct PreparedShape {
    std::vector<OpRequest> requests;
    std::optional<std::int64_t> head;
    const Store* owner = nullptr;
    std::string client;
};
struct AutomationThinning {
    double maxValueError = 0;
    double sampleRate = 0;
};
// Arrangement shapes replace [start,end) and retain the played exterior
// through the shared envelope splice. A supplied rate is the owner's session
// sample clock, otherwise the stored project rate is used. Clip envelopes
// use their existing local point-edit path. Preparation writes no rows.
std::optional<PreparedShape> prepareShapeInsertion(Store&, std::int64_t data,
    std::int64_t lane, std::optional<std::int64_t> clip, const ShapeSpec&, std::string& error,
    std::optional<double> sessionRate = {});
CommitResult commitShapeInsertion(Store&, const PreparedShape&);
// Prepare one atomic native write from explicitly timestamped points.
// Arrangement passes with a nonempty played interval retain the old curve
// outside [first,last). Local clip and single-sample writes use native points.
// Optional thinning is checked at the Arrangement lane's projected samples.
std::optional<PreparedShape> prepareAutomationWrite(Store&, std::int64_t data,
    std::int64_t lane, std::optional<std::int64_t> clip,
    std::span<const AutomationPoint>, std::string& error,
    std::optional<AutomationThinning> = {});
} // namespace adi::automation
