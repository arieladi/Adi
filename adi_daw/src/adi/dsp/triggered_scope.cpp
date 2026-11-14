// SPDX-License-Identifier: GPL-3.0-only
// Derived acquisition law: Bram de Jong's s(M)exoscope, modernized by
// Matthijs Hollemans. Independently stored source-valued captures; see notice.
#include "triggered_scope.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace adi::dsp {
namespace {
bool unit(float x) noexcept { return std::isfinite(x) && x >= 0 && x <= 1; }
bool valid(const TriggeredScope::Settings &s) noexcept {
    return std::isfinite(s.sampleRate) && s.sampleRate >= 8000 && s.sampleRate <= 384000 &&
        s.columns && s.columns <= TriggeredScope::kMaximumColumns &&
        s.trigger >= TriggeredScope::Trigger::Free && s.trigger <= TriggeredScope::Trigger::Internal &&
        std::isfinite(s.threshold) && s.threshold >= -1 && s.threshold <= 1 &&
        std::isfinite(s.gain) && s.gain >= .001f && s.gain <= 1000.f &&
        std::isfinite(s.columnsPerSample) && s.columnsPerSample >= .0001 && s.columnsPerSample <= 32 &&
        std::isfinite(s.internalCyclesPerSample) && s.internalCyclesPerSample >= 0 && s.internalCyclesPerSample <= 1 &&
        s.holdoffSamples >= 1 && s.holdoffSamples <= 10000;
}
std::uint64_t nextEpoch(std::uint64_t old) noexcept { return old == std::numeric_limits<std::uint64_t>::max() ? 1 : old + 1; }
}
struct TriggeredScope::Impl {
    Settings spec;
    std::unique_ptr<Column[]> a, b;
    Column *live = nullptr, *past = nullptr;
    std::size_t count = 0, pastCount = 0;
    std::uint64_t epoch = 1, serial = 1, pastSerial = 0;
    std::int64_t triggerSequence = -1, pastTriggerSequence = -1;
    std::int64_t nextSequence = 0, rangeStart = 0;
    bool started = false, rangeStarted = false, lastIsMaximum = false;
    double counter = 1, phase = 0, dc = 0, previousInput = 0, dcCoefficient;
    float previousSample = 0, maximum = -1, minimum = 1;
    std::uint32_t holdoff = 0;
    explicit Impl(const Settings &s) : spec(s), a(std::make_unique<Column[]>(s.columns)),
        b(std::make_unique<Column[]>(s.columns)), live(a.get()), past(b.get()), dcCoefficient(1. - 250. / s.sampleRate) {}
    void resetState() noexcept {
        count = pastCount = 0; serial = 1; pastSerial = 0;
        triggerSequence = pastTriggerSequence = -1;
        nextSequence = rangeStart = 0; started = rangeStarted = lastIsMaximum = false;
        counter = 1; phase = dc = previousInput = 0; previousSample = 0;
        maximum = -1; minimum = 1; holdoff = 0;
    }
    bool accepts(std::span<const float> input, std::int64_t first) const noexcept {
        if (input.empty()) return true;
        return input.data() && first >= 0 &&
            input.size() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - first) &&
            (!started || first == nextSequence);
    }
    void ingest(std::span<const float> input, std::int64_t first) noexcept {
        if (input.empty()) return;
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto sequence = first + static_cast<std::int64_t>(i);
            const float raw = input[i];
            dc = raw - previousInput + dcCoefficient * dc;
            previousInput = raw;
            if (std::abs(dc) < 1e-10f) dc = 0;
            float sample = spec.removeDc ? static_cast<float>(dc) : raw;
            sample = std::clamp(sample * spec.gain, -1.f, 1.f);
            bool trigger = false;
            switch (spec.trigger) {
            case Trigger::Free: trigger = count >= spec.columns; break;
            case Trigger::Rising: trigger = sample >= spec.threshold && previousSample < spec.threshold; break;
            case Trigger::Falling: trigger = sample <= spec.threshold && previousSample > spec.threshold; break;
            case Trigger::Internal:
                phase += spec.internalCyclesPerSample;
                if (phase >= 1) { phase -= 1; trigger = true; }
                break;
            }
            // Saturation retains the edge predicate without long-run signed overflow.
            if (holdoff < spec.holdoffSamples) ++holdoff;
            if (trigger && holdoff < spec.holdoffSamples &&
                (spec.trigger == Trigger::Rising || spec.trigger == Trigger::Falling)) trigger = false;
            if (trigger) {
                std::swap(live, past); pastCount = count; pastSerial = serial;
                pastTriggerSequence = triggerSequence;
                count = 0; serial = nextEpoch(serial); triggerSequence = sequence;
                counter = 1; maximum = -1; minimum = 1; holdoff = 0; rangeStarted = false;
            }
            if (!rangeStarted) { rangeStart = sequence; rangeStarted = true; }
            if (sample > maximum) { maximum = sample; lastIsMaximum = true; }
            if (sample < minimum) { minimum = sample; lastIsMaximum = false; }
            counter += spec.columnsPerSample;
            if (counter >= 1) {
                if (count < spec.columns) {
                    live[count++] = {lastIsMaximum ? minimum : maximum, lastIsMaximum ? maximum : minimum,
                                     rangeStart, sequence};
                }
                maximum = -1; minimum = 1; rangeStarted = false; counter -= 1;
            }
            previousSample = sample;
        }
        started = true;
        nextSequence = first + static_cast<std::int64_t>(input.size());
    }
};
TriggeredScope::TriggeredScope() noexcept = default;
TriggeredScope::~TriggeredScope() = default;
bool TriggeredScope::mapNormalized(const Normalized &n, Settings &out) noexcept {
    if (!unit(n.level) || !unit(n.amplitude) || !unit(n.time) || !unit(n.holdoff) || !unit(n.speed)) return false;
    auto candidate = out;
    candidate.threshold = n.level * 2.f - 1.f;
    candidate.gain = std::pow(10.f, n.amplitude * 6.f - 3.f);
    candidate.columnsPerSample = std::pow(10., 1.5 - static_cast<double>(n.time) * 5.);
    candidate.holdoffSamples = static_cast<std::uint32_t>(std::pow(10., static_cast<double>(n.holdoff) * 4.));
    candidate.internalCyclesPerSample = std::pow(10., static_cast<double>(n.speed) * 2.5 - 5.);
    out = candidate;
    return true;
}
bool TriggeredScope::prepare(const Settings &s) noexcept {
    if (!valid(s)) return false;
    try {
        auto candidate = std::make_unique<Impl>(s);
        candidate->epoch = impl_ ? nextEpoch(impl_->epoch) : 1;
        impl_ = std::move(candidate);
    } catch (...) { return false; }
    return true;
}
bool TriggeredScope::prepared() const noexcept { return static_cast<bool>(impl_); }
const TriggeredScope::Settings *TriggeredScope::settings() const noexcept { return impl_ ? &impl_->spec : nullptr; }
void TriggeredScope::reset() noexcept {
    if (!impl_) return;
    impl_->epoch = nextEpoch(impl_->epoch); impl_->resetState();
}
bool TriggeredScope::processChecked(std::span<const float> input, std::int64_t first) noexcept {
    if (!impl_ || !impl_->accepts(input, first) ||
        !std::all_of(input.begin(), input.end(), [](float x) { return std::isfinite(x); })) return false;
    impl_->ingest(input, first); return true;
}
bool TriggeredScope::processTrustedFinite(std::span<const float> input, std::int64_t first) noexcept {
    if (!impl_ || !impl_->accepts(input, first)) return false;
    impl_->ingest(input, first); return true;
}
TriggeredScope::Capture TriggeredScope::current() const noexcept {
    return impl_ ? Capture{{impl_->live, impl_->count}, impl_->epoch, impl_->serial, impl_->triggerSequence} : Capture{};
}
TriggeredScope::Capture TriggeredScope::previous() const noexcept {
    return impl_ ? Capture{{impl_->past, impl_->pastCount}, impl_->epoch, impl_->pastSerial, impl_->pastTriggerSequence} : Capture{};
}
} // namespace adi::dsp
