// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/pitch_analysis.hpp"
#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <new>
#include <numbers>
#include <numeric>
#include <utility>

namespace adi::audio {
namespace {

constexpr std::uint32_t minimumSourceRate = 8000;
constexpr std::uint32_t maximumSourceRate = 384000;
constexpr std::uint32_t targetAnalysisRate = 12000;
constexpr std::size_t maximumObservations = 1'000'000;
constexpr double minimumF0 = 55.0;
constexpr double maximumF0 = 1800.0;
constexpr double silenceRms = 0.0005;
constexpr double trustedPeriod = 0.10;
constexpr double fallbackPeriod = 0.17;
constexpr double pitchChangeCents = 45;
constexpr double maximumCentsMagnitude = 24000;
constexpr std::size_t minimumNoteHops = 6; // about 30 ms
constexpr std::size_t minimumLegatoNoteHops = 20; // about 100 ms

bool validRate(std::uint32_t rate) noexcept {
    return rate >= minimumSourceRate && rate <= maximumSourceRate;
}

bool validCents(double cents) noexcept {
    return std::abs(cents) <= maximumCentsMagnitude;
}

struct Observation {
    double cents = 0;
    bool voiced = false;
};

// Cumulative-mean normalized difference (YIN). Comparing pairs symmetric about
// the observation center avoids a lag-dependent bias in the contour's timing.
Observation pitchAt(std::span<const double> signal, std::size_t center, double rate, std::span<double> difference) {
    const auto energyRadius = static_cast<std::size_t>(rate * 0.0025);
    const auto energy = signal.subspan(center > energyRadius ? center - energyRadius : 0,
                                      std::min(center, energyRadius) + std::min(signal.size() - center, energyRadius + 1));
    double sum = 0;
    double squares = 0;
    for (const double sample : energy) {
        sum += sample;
        squares += sample * sample;
    }
    const auto count = static_cast<double>(energy.size());
    if (squares - sum * sum / count < count * silenceRms * silenceRms) return {};

    const auto maxLag = difference.size() - 1;
    const auto pairs = std::min(static_cast<std::size_t>(rate * 0.018), signal.size() - maxLag);
    difference[0] = 1;
    double cumulative = 0;
    for (std::size_t lag = 1; lag <= maxLag; ++lag) {
        const auto radius = (pairs + lag) / 2;
        const auto start = std::min(center > radius ? center - radius : 0, signal.size() - pairs - lag);
        double squaredDifference = 0;
        for (std::size_t index = start; index < start + pairs; ++index) {
            const double delta = signal[index] - signal[index + lag];
            squaredDifference += delta * delta;
        }
        cumulative += squaredDifference;
        difference[lag] = cumulative > 0 ? squaredDifference * static_cast<double>(lag) / cumulative : 1;
    }

    // The first credible trough is the shortest period. In less periodic
    // frames accept only an actual local minimum, never a descending edge.
    const auto minLag = std::max<std::size_t>(2, static_cast<std::size_t>(rate / maximumF0));
    double period = 0;
    double lowest = fallbackPeriod;
    for (std::size_t lag = minLag; lag < maxLag; ++lag) {
        const double left = difference[lag - 1];
        const double middle = difference[lag];
        const double right = difference[lag + 1];
        if (middle > left || middle >= right) continue;
        const double offset = 0.5 * (left - right) / (left + right - 2 * middle);
        const double trough = middle + 0.25 * (right - left) * offset;
        if (trough < lowest) {
            lowest = trough;
            period = static_cast<double>(lag) + offset;
        }
        if (lowest < trustedPeriod) break;
    }
    if (period == 0) return {};
    const double frequency = rate / period;
    if (frequency < minimumF0 || frequency > maximumF0) return {};
    return {6900.0 + 1200.0 * std::log2(frequency / 440.0), true};
}

// Suppress single-observation octave slips and bridge dropouts of at most
// 10 ms when the pitch on both sides agrees. Long rests remain unpitched.
void stabilize(std::span<Observation> observations) {
    for (std::size_t index = 1; index + 1 < observations.size(); ++index) {
        const auto previous = observations[index - 1];
        auto& current = observations[index];
        const auto next = observations[index + 1];
        if (previous.voiced && current.voiced && next.voiced && std::abs(previous.cents - next.cents) < 45 &&
            std::abs(current.cents - (previous.cents + next.cents) / 2) > 500)
            current.cents = (previous.cents + next.cents) / 2;
    }
    for (std::size_t index = 1; index < observations.size();) {
        if (observations[index].voiced) {
            ++index;
            continue;
        }
        const auto start = index;
        while (index < observations.size() && !observations[index].voiced) ++index;
        if (!observations[start - 1].voiced || index == observations.size() || index - start > 2) continue;
        const double left = observations[start - 1].cents;
        const double right = observations[index].cents;
        if (std::abs(left - right) > 50) continue;
        for (std::size_t frame = start; frame < index; ++frame) {
            const double fraction = static_cast<double>(frame - start + 1) / static_cast<double>(index - start + 1);
            observations[frame] = {std::lerp(left, right, fraction), true};
        }
    }
}

struct PitchWindow {
    double center;
    double spread;
};

template <std::size_t Size>
PitchWindow summarize(std::span<const Observation> run, std::ptrdiff_t first) {
    std::array<double, Size> pitches;
    for (std::size_t index = 0; index < Size; ++index) {
        const auto position = std::clamp(first + static_cast<std::ptrdiff_t>(index), std::ptrdiff_t{0},
                                         static_cast<std::ptrdiff_t>(run.size() - 1));
        pitches[index] = run[static_cast<std::size_t>(position)].cents;
    }
    std::sort(pitches.begin(), pitches.end());
    return {std::midpoint(pitches[(Size - 1) / 2], pitches[Size / 2]), pitches[Size - 1 - Size / 5] - pitches[Size / 5]};
}

double transitionAt(std::span<const Observation> run, std::size_t boundary) {
    const auto center = static_cast<std::ptrdiff_t>(boundary);
    const double local = summarize<7>(run, center + 1).center - summarize<7>(run, center - 8).center;
    if (std::abs(local) < pitchChangeCents) return 0;

    const auto before = summarize<12>(run, center - 24);
    const auto after = summarize<12>(run, center + 12);
    const double sustained = after.center - before.center;
    // A change must persist away from the transition. Discount the excursion
    // already present in either plateau so vibrato does not become new notes.
    if (local * sustained <= 0) return 0;
    return std::min(std::abs(local), std::abs(sustained)) - (before.spread + after.spread) / 2;
}

PitchNote makeNote(std::span<const Observation> voiced, std::uint64_t start, std::uint64_t end, std::uint64_t hop,
                   std::vector<double>& pitches) {
    PitchNote note;
    note.startFrame = start;
    note.endFrame = end;
    note.curve.reserve(voiced.size());
    pitches.clear();
    pitches.reserve(voiced.size());
    double pitchSum = 0;
    double timeSum = 0;
    const auto count = static_cast<double>(voiced.size());
    const auto duration = static_cast<double>(end - start);
    for (std::size_t index = 0; index < voiced.size(); ++index) {
        const auto frame = std::min(start + index * hop + hop / 2, end - 1);
        const double pitch = voiced[index].cents;
        note.curve.push_back({frame, pitch});
        pitches.push_back(pitch);
        pitchSum += pitch;
        timeSum += static_cast<double>(frame - start) / duration;
    }

    const auto middle = pitches.begin() + static_cast<std::ptrdiff_t>(pitches.size() / 2);
    std::nth_element(pitches.begin(), middle, pitches.end());
    note.pitchCents = pitches.size() % 2 ? *middle : std::midpoint(*std::max_element(pitches.begin(), middle), *middle);
    const double meanPitch = pitchSum / count;
    const double meanTime = timeSum / count;
    double covariance = 0;
    double variance = 0;
    for (const auto& point : note.curve) {
        const double time = static_cast<double>(point.frame - start) / duration - meanTime;
        covariance += time * (point.cents - meanPitch);
        variance += time * time;
    }
    note.driftCents = variance > 0 ? covariance / variance : 0;
    double residualSquares = 0;
    for (const auto& point : note.curve) {
        const double time = static_cast<double>(point.frame - start) / duration - meanTime;
        const double residual = point.cents - meanPitch - note.driftCents * time;
        residualSquares += residual * residual;
    }
    note.vibratoCents = std::sqrt(residualSquares / count);
    return note;
}

bool segment(PitchAnalysis& result, std::span<const Observation> observations, std::uint64_t hop, std::stop_token stop) {
    std::vector<double> pitches;
    const auto appendNote = [&](std::size_t start, std::size_t end) {
        result.notes.push_back(makeNote(observations.subspan(start, end - start), start * hop,
                                        std::min<std::uint64_t>(end * hop, result.sourceFrames), hop, pitches));
    };
    for (std::size_t position = 0; position < observations.size();) {
        if (stop.stop_requested()) return false;
        if (!observations[position].voiced) {
            ++position;
            continue;
        }
        const auto start = position;
        while (position < observations.size() && observations[position].voiced) ++position;
        const auto end = position;
        if (end - start < minimumNoteHops) continue;
        const auto run = observations.subspan(start, end - start);

        // A legato change can cover several hops. Choose the strongest point
        // in each region that exceeds the transition threshold. Legato-only
        // splits need longer support than a note bounded by silence, so a
        // preparatory scoop or release does not become a separate note.
        auto noteStart = start;
        for (auto boundary = start + minimumLegatoNoteHops; boundary + minimumLegatoNoteHops <= end;) {
            if (stop.stop_requested()) return false;
            const double transition = transitionAt(run, boundary - start);
            if (transition < pitchChangeCents) {
                ++boundary;
                continue;
            }
            auto split = boundary;
            auto largest = transition;
            for (++boundary; boundary + minimumLegatoNoteHops <= end; ++boundary) {
                const double nearby = transitionAt(run, boundary - start);
                if (nearby < pitchChangeCents) break;
                if (nearby > largest) {
                    largest = nearby;
                    split = boundary;
                }
            }
            appendNote(noteStart, split);
            noteStart = split;
            boundary = std::max(boundary, noteStart + minimumLegatoNoteHops);
        }
        appendNote(noteStart, end);
    }
    return !stop.stop_requested();
}

PitchAnalysisResult analyze(std::span<const float> mono, std::uint32_t sourceRate, std::stop_token stop) {
    const auto stride = std::max<std::size_t>(1, sourceRate / targetAnalysisRate);
    const double rate = static_cast<double>(sourceRate) / static_cast<double>(stride);
    const auto hop = static_cast<std::size_t>(std::round(rate * 0.005));
    const auto decimated = (mono.size() - 1) / stride + 1;
    const auto observations = (decimated - 1) / hop + 1;
    if (observations > maximumObservations) return {PitchAnalysisError::Resource, {}};

    PitchAnalysis analysis{sourceRate, mono.size(), {}};
    std::vector<double> signal(decimated);
    // Four low-pass poles before decimation limit high-frequency aliases.
    // At native analysis rates the same filter weights the fundamental.
    const double cutoff = 0.33 * rate;
    const double coefficient = 1 - std::exp(-2 * std::numbers::pi * cutoff / sourceRate);
    double pole1 = 0;
    double pole2 = 0;
    double pole3 = 0;
    double pole4 = 0;
    auto sampleIndex = std::size_t{0};
    for (std::size_t position = 0; position < mono.size(); ++position) {
        if (position % 4096 == 0 && stop.stop_requested()) return {PitchAnalysisError::Cancelled, {}};
        const double sample = mono[position];
        if (!std::isfinite(sample)) return {PitchAnalysisError::InvalidSource, {}};
        pole1 += coefficient * (sample - pole1);
        pole2 += coefficient * (pole1 - pole2);
        pole3 += coefficient * (pole2 - pole3);
        pole4 += coefficient * (pole3 - pole4);
        if (position == sampleIndex * stride) signal[sampleIndex++] = pole4;
    }

    const auto maxLag = static_cast<std::size_t>(std::ceil(rate / minimumF0)) + 1;
    if (decimated <= maxLag || observations < minimumNoteHops) return {PitchAnalysisError::None, std::move(analysis)};
    std::vector<double> difference(maxLag + 1);
    std::vector<Observation> curve(observations);
    for (std::size_t index = 0; index < observations; ++index) {
        if (stop.stop_requested()) return {PitchAnalysisError::Cancelled, {}};
        curve[index] = pitchAt(signal, std::min(index * hop + hop / 2, signal.size() - 1), rate, difference);
    }
    stabilize(curve);
    if (!segment(analysis, curve, hop * stride, stop)) return {PitchAnalysisError::Cancelled, {}};
    return {PitchAnalysisError::None, std::move(analysis)};
}
} // namespace

bool validPitchAnalysis(const PitchAnalysis& analysis) noexcept {
    if (!validRate(analysis.sampleRate) || analysis.sourceFrames == 0) return false;
    std::uint64_t previousEnd = 0;
    for (const auto& note : analysis.notes) {
        if (note.startFrame < previousEnd || note.endFrame <= note.startFrame || note.endFrame > analysis.sourceFrames ||
            !validCents(note.pitchCents) || !validCents(note.driftCents) || !validCents(note.vibratoCents) ||
            note.vibratoCents < 0 || note.curve.empty()) return false;
        auto earliest = note.startFrame;
        for (const auto& point : note.curve) {
            if (point.frame < earliest || point.frame >= note.endFrame || !validCents(point.cents)) return false;
            earliest = point.frame + 1;
        }
        previousEnd = note.endFrame;
    }
    return true;
}

PitchAnalysisResult analyzePitch(std::span<const float> mono, std::uint32_t sourceRate, std::stop_token stop) {
    if (onAudioThread) return {PitchAnalysisError::Realtime, {}};
    if (stop.stop_requested()) return {PitchAnalysisError::Cancelled, {}};
    if (mono.empty() || !validRate(sourceRate)) return {PitchAnalysisError::InvalidSource, {}};
    try {
        return analyze(mono, sourceRate, stop);
    } catch (const std::bad_alloc&) {
        return {PitchAnalysisError::Resource, {}};
    }
}

} // namespace adi::audio
