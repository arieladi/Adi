// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/transients.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace adi::audio {
TransientAnalyzer::TransientAnalyzer(std::uint32_t rate, std::uint32_t channels, int sensitivity) {
    if (onAudioThread) {
        result_.error = AnalysisError::Realtime;
        return;
    }
    if (rate == 0 || rate > 768000 || channels == 0 || channels > 64 || sensitivity < 0 || sensitivity > 100) {
        result_.error = AnalysisError::InvalidSource;
        return;
    }
    channels_ = channels;
    pending_.resize(static_cast<std::size_t>(std::max(1u, rate / 200)) * channels);
    separation_ = std::max(1u, rate / 25);
    multiplier_ = 5.0 - 3.0 * sensitivity / 100.0;
}

void TransientAnalyzer::measure(std::span<const float> samples) {
    double square = 0, peak = 0;
    for (const float sample : samples) {
        const double value = std::isfinite(sample) ? sample : 0;
        square += value * value;
        peak = std::max(peak, std::abs(value));
    }
    const double energy = std::sqrt(square / static_cast<double>(samples.size()));
    const double rise = std::max(0.0, energy - previous_);
    if (peak > 0.0005 && rise > std::max({1e-7, baseline_ * multiplier_, energy * 0.3}) &&
        (result_.frames.empty() || position_ - last_ >= separation_)) {
        const auto onset = std::find_if(samples.begin(), samples.end(), [peak](float sample) {
            return std::isfinite(sample) && std::abs(sample) >= peak * 0.1;
        });
        if (result_.frames.size() == 1000000) {
            result_.error = AnalysisError::TooManyOnsets;
            return;
        }
        last_ = position_ + static_cast<std::uint64_t>(onset - samples.begin()) / channels_;
        result_.frames.push_back(last_);
    }
    baseline_ = baseline_ * 0.98 + rise * 0.02;
    previous_ = energy;
    position_ += samples.size() / channels_;
}

bool TransientAnalyzer::append(std::span<const float> samples) {
    if (onAudioThread || !result_ || pending_.empty()) return false;
    if (samples.size() % channels_ != 0) {
        result_.error = AnalysisError::InvalidSource;
        return false;
    }
    if (samples.empty()) return true;
    if (pendingSamples_ != 0) {
        const auto needed = std::min(samples.size(), pending_.size() - pendingSamples_);
        std::copy_n(samples.data(), needed, pending_.data() + pendingSamples_);
        pendingSamples_ += needed;
        samples = samples.subspan(needed);
        if (pendingSamples_ == pending_.size()) {
            measure(pending_);
            pendingSamples_ = 0;
        }
    }
    while (result_ && samples.size() >= pending_.size()) {
        measure(samples.first(pending_.size()));
        samples = samples.subspan(pending_.size());
    }
    if (result_ && !samples.empty()) {
        std::copy_n(samples.data(), samples.size(), pending_.data());
        pendingSamples_ = samples.size();
    }
    return static_cast<bool>(result_);
}

TransientResult TransientAnalyzer::finish() {
    if (onAudioThread) return {AnalysisError::Realtime, {}};
    if (result_ && pending_.empty()) return {AnalysisError::InvalidSource, {}};
    if (result_ && pendingSamples_ != 0) measure(std::span(pending_).first(pendingSamples_));
    pendingSamples_ = 0;
    return std::move(result_);
}

TransientResult detectTransients(WavReader& reader, std::uint64_t first, std::uint64_t frames,
                                 int sensitivity, std::stop_token stop) {
    if (onAudioThread) return {AnalysisError::Realtime, {}};
    if (first > reader.frames() || frames == 0 || frames > reader.frames() - first ||
        reader.sampleRate() < 8000 || reader.sampleRate() > 768000 ||
        reader.channels() == 0 || reader.channels() > 64 || sensitivity < 0 || sensitivity > 100)
        return {AnalysisError::InvalidSource, {}};
    if (stop.stop_requested()) return {AnalysisError::Cancelled, {}};

    try {
        constexpr std::uint32_t blockFrames = 4096;
        std::vector<float> input(static_cast<std::size_t>(blockFrames) * reader.channels());
        TransientAnalyzer analyzer(reader.sampleRate(), reader.channels(), sensitivity);
        reader.seek(first);
        while (frames != 0) {
            if (stop.stop_requested()) return {AnalysisError::Cancelled, {}};
            const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames, frames));
            if (reader.read(input.data(), count) != count) return {AnalysisError::Io, {}};
            if (!analyzer.append(std::span(input).first(static_cast<std::size_t>(count) * reader.channels()))) break;
            frames -= count;
        }
        return analyzer.finish();
    } catch (...) {
        return {AnalysisError::Io, {}};
    }
}
}
