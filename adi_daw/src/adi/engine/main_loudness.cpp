// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/main_loudness.hpp"

#include "adi/dsp/loudness.hpp"
#include "adi/engine/publisher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace adi::engine {
namespace {
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
constexpr int maxChannels = 64;
constexpr int paddingFrames = 512;
constexpr double maxWideMagnitude = 1.0e100;
constexpr double noSharedPeak = -300.0;  // TruePeakMeter::peakDb() with an empty hold.
constexpr std::array<double, paddingFrames> silentInput{};

struct Format final : Sequenced {
    // The audio reader alone mutates the meter after the owner publishes it.
    mutable dsp::LoudnessMeter meter;
    mutable std::vector<double> widened;
    Format(double rate, const std::vector<double>& weights)
        : widened(weights.size() * paddingFrames) {
        meter.prepare(rate, weights);
    }
};

bool safeWideRange(const double* samples, int frames) noexcept {
    return samples && std::none_of(samples, samples + frames,
        [](double value) { return std::abs(value) > maxWideMagnitude; });
}

void processOutput(const Format& format, const AudioIo& io) noexcept {
    const int channels = format.meter.channels();
    if (io.out64 && io.numOut >= channels &&
        std::all_of(io.out64, io.out64 + channels, [&](const auto* channel) {
            return safeWideRange(channel, io.frames);
        })) {
        format.meter.process(io.out64, io.frames);
        return;
    }

    // The filters and true peak both work in double. Widen each narrow
    // sample once, and provide a zero buffer for any missing channel.
    std::array<const double*, maxChannels> input;
    for (int at = 0; at < io.frames;) {
        const int length = std::min(paddingFrames, io.frames - at);
        for (int channel = 0; channel < channels; ++channel) {
            const auto index = static_cast<std::size_t>(channel);
            input[index] = silentInput.data();
            if (channel >= io.numOut) continue;
            if (io.out64) {
                if (!io.out64[channel]) continue;
                const auto* samples = io.out64[channel] + at;
                if (safeWideRange(samples, length)) {
                    input[index] = samples;
                } else {
                    auto* sanitized = format.widened.data() + index * paddingFrames;
                    std::transform(samples, samples + length, sanitized, [](double sample) {
                        return std::isfinite(sample) ? std::clamp(sample, -maxWideMagnitude, maxWideMagnitude) : 0.0;
                    });
                    input[index] = sanitized;
                }
            } else if (io.out && io.out[channel]) {
                auto* widened = format.widened.data() + index * paddingFrames;
                std::copy_n(io.out[channel] + at, length, widened);
                input[index] = widened;
            }
        }
        format.meter.process(input.data(), length);
        at += length;
    }
}

MainLoudnessSnapshot makeSnapshot(const dsp::LoudnessMeter::Readings& reading, std::uint64_t format) noexcept {
    MainLoudnessSnapshot snapshot{
        static_cast<float>(reading.momentary), static_cast<float>(reading.shortTerm),
        static_cast<float>(reading.integrated), static_cast<float>(reading.truePeak),
        static_cast<float>(reading.range), static_cast<float>(reading.maxMomentary),
        static_cast<float>(reading.maxShortTerm), format, 0, reading.running,
    };
    const auto valid = [&](MainLoudnessField field, bool present) {
        if (present) snapshot.validFields |= static_cast<std::uint32_t>(field);
    };
    valid(MainLoudnessField::Momentary, std::isfinite(reading.momentary));
    valid(MainLoudnessField::ShortTerm, std::isfinite(reading.shortTerm));
    valid(MainLoudnessField::Integrated, std::isfinite(reading.integrated));
    valid(MainLoudnessField::TruePeak, std::isfinite(reading.truePeak) && reading.truePeak != noSharedPeak);
    valid(MainLoudnessField::Range, reading.rangeValid);
    valid(MainLoudnessField::MaxMomentary, std::isfinite(reading.maxMomentary));
    valid(MainLoudnessField::MaxShortTerm, std::isfinite(reading.maxShortTerm));
    return snapshot;
}
}  // namespace

void MainLoudnessReadout::publish(const MainLoudnessSnapshot& value) noexcept {
    const auto previous = publication_.load(std::memory_order_relaxed);
    publication_.store(previous + 1, std::memory_order_relaxed);
    // Publish the odd sequence before any reader can observe the new fields.
    std::atomic_thread_fence(std::memory_order_release);
    const bool newFormat = formatSequence.load(std::memory_order_relaxed) != value.formatSequence;
    if (newFormat) formatSequence.store(0, std::memory_order_release);
    momentaryLufs.store(value.momentaryLufs, std::memory_order_relaxed);
    shortTermLufs.store(value.shortTermLufs, std::memory_order_relaxed);
    integratedLufs.store(value.integratedLufs, std::memory_order_relaxed);
    // Older readers use -infinity for an empty hold. The snapshot carries
    // the common meter's numeric value together with its validity.
    const float peak = value.valid(MainLoudnessField::TruePeak)
        ? value.truePeakDb : MainLoudnessSnapshot::silence;
    truePeakDb.store(peak, std::memory_order_relaxed);
    publishedPeakDb_.store(value.truePeakDb, std::memory_order_relaxed);
    rangeLu_.store(value.rangeLu, std::memory_order_relaxed);
    maxMomentaryLufs_.store(value.maxMomentaryLufs, std::memory_order_relaxed);
    maxShortTermLufs_.store(value.maxShortTermLufs, std::memory_order_relaxed);
    validFields_.store(value.validFields, std::memory_order_relaxed);
    running_.store(value.running, std::memory_order_relaxed);
    if (newFormat) formatSequence.store(value.formatSequence, std::memory_order_release);
    publication_.store(previous + 2, std::memory_order_release);
}

std::optional<MainLoudnessSnapshot> MainLoudnessReadout::snapshot() const noexcept {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto before = publication_.load(std::memory_order_acquire);
        if (before & 1U) continue;
        MainLoudnessSnapshot value{
            momentaryLufs.load(std::memory_order_relaxed), shortTermLufs.load(std::memory_order_relaxed),
            integratedLufs.load(std::memory_order_relaxed), publishedPeakDb_.load(std::memory_order_relaxed),
            rangeLu_.load(std::memory_order_relaxed), maxMomentaryLufs_.load(std::memory_order_relaxed),
            maxShortTermLufs_.load(std::memory_order_relaxed), formatSequence.load(std::memory_order_relaxed),
            validFields_.load(std::memory_order_relaxed), running_.load(std::memory_order_relaxed),
        };
        std::atomic_thread_fence(std::memory_order_acquire);
        if (publication_.load(std::memory_order_relaxed) == before) {
            if (value.formatSequence == 0) return std::nullopt;
            return value;
        }
    }
    return std::nullopt;
}

struct MainLoudness::Impl {
    SnapshotPublisher<Format> formats;
    double rate = 0.0;
    std::vector<double> weights;
};

MainLoudness::MainLoudness() : impl_(std::make_unique<Impl>()) {}
MainLoudness::~MainLoudness() = default;

bool MainLoudness::prepare(double sampleRate, int channels, std::span<const double> weights) {
    if (!std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 768000 ||
        channels < 1 || channels > maxChannels ||
        (!weights.empty() && weights.size() != static_cast<std::size_t>(channels))) return false;
    std::vector<double> next(static_cast<std::size_t>(channels), 1);
    if (!weights.empty()) std::copy(weights.begin(), weights.end(), next.begin());
    for (double weight : next) {
        if (!std::isfinite(weight) || weight < 0 || weight > 2) return false;
    }
    if (impl_->rate == sampleRate && impl_->weights == next) return true;
    impl_->formats.publish(std::make_unique<Format>(sampleRate, next));
    impl_->rate = sampleRate;
    impl_->weights = std::move(next);
    collect();
    return true;
}

void MainLoudness::collect() { impl_->formats.collect(); }

std::uint64_t MainLoudness::preparedFormatSequence() const noexcept {
    const auto* format = impl_->formats.peek();
    return format ? format->seq : 0;
}

void MainLoudness::process(const AudioIo& io) noexcept {
    SnapshotPublisher<Format>::AudioRead format(impl_->formats);
    if (!format.valid()) return;
    auto& meter = format->meter;
    if (reset_.exchange(false, std::memory_order_acq_rel)) meter.reset();
    meter.setRunning(running_.load(std::memory_order_acquire));
    if (io.frames > 0) processOutput(*format.get(), io);
    readout_.publish(makeSnapshot(meter.readings(), format->seq));
}

}  // namespace adi::engine
