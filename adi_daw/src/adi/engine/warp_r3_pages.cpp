// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/warp_r3_pages.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/engine/warp_arrangement_clock.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace adi::engine {
namespace {

constexpr std::int64_t frameLimit = std::int64_t{1} << 50;
constexpr std::int64_t inputPageFrames = 2048;
constexpr std::int64_t steadyTileFrames = 8192;
constexpr std::int64_t dynamicTileFrames = 16384;
constexpr std::int64_t tileOverlap = 512;
constexpr std::int32_t maxStitchLag = 64;

std::int64_t checkedFrame(double frame) {
    if (!std::isfinite(frame) || std::abs(frame) >= static_cast<double>(frameLimit))
        throw std::invalid_argument("R3 Warp placement is outside the supported frame range");
    return std::llround(frame);
}

std::int32_t bestStitchLag(const float* previous, const float* current, std::uint16_t channels) noexcept {
    // Estimate one local phase offset shared by all channels. Restrict the
    // search to the overlap and skip shifting weakly related or silent audio.
    constexpr std::int64_t stride = 4;
    std::uint16_t channel = 0;
    double energy = 0;
    for (std::uint16_t c = 0; c < channels; ++c) {
        double candidate = 0;
        for (std::int64_t i = 0; i < tileOverlap; i += stride) {
            const auto sample = previous[i*channels+c];
            candidate += static_cast<double>(sample)*sample;
        }
        if (candidate > energy) { energy = candidate; channel = c; }
    }
    if (energy < 1e-10) return 0;

    const auto correlation = [&](std::int32_t lag) {
        double dot = 0, currentEnergy = 0;
        for (std::int64_t i = 0; i < tileOverlap; i += stride) {
            const auto a = previous[i*channels+channel];
            const auto b = current[(i+lag)*channels+channel];
            dot += static_cast<double>(a)*b;
            currentEnergy += static_cast<double>(b)*b;
        }
        return currentEnergy > 1e-10 ? dot/std::sqrt(energy*currentEnergy) : 0;
    };

    constexpr double minimumCorrelation = 0.9, minimumImprovement = 0.1;
    const auto unchanged = correlation(0);
    double best = unchanged;
    std::int32_t offset = 0;
    for (std::int32_t lag = -maxStitchLag; lag <= maxStitchLag; ++lag) {
        const auto score = correlation(lag);
        if (score > best || (score == best && std::abs(lag) < std::abs(offset))) {
            best = score;
            offset = lag;
        }
    }
    return best > minimumCorrelation && best-unchanged > minimumImprovement ? offset : 0;
}

float stitchSample(const float* start, double position, std::uint16_t channels, std::size_t channel) noexcept {
    const auto base = static_cast<std::ptrdiff_t>(std::floor(position));
    const auto index = base*channels + static_cast<std::ptrdiff_t>(channel);
    const auto fraction = static_cast<float>(position-static_cast<double>(base));
    return std::lerp(start[index], start[index+channels], fraction);
}

} // namespace

WarpR3Pages::WarpR3Pages(const audio::WarpRenderPlan& plan, std::uint32_t sourceRate,
                         std::uint16_t channels, std::span<const WarpMarker> markers,
                         const WarpArrangementClock& tempo, double sourceOriginTick,
                         double endTick, double loopStartTick, double loopLengthTicks,
                         std::optional<std::int64_t> placementEndFrame)
    : tempo_(tempo), markers_(markers.begin(), markers.end()), sourceStart_(plan.sourceStart),
      sourceRate_(sourceRate), channels_(channels), originTick_(sourceOriginTick), loopStartTick_(loopStartTick),
      loopLengthTicks_(loopLengthTicks), steadyFramesPerTick_(tempo.constantFramesPerTick()) {
    if (!audio::RealtimeWarpR3::supports(plan, sourceRate_, channels_) ||
        markers_.size() < 2 || markers_.front() != WarpMarker{} || plan.sourceFrames == 0 ||
        plan.sourceFrames >= static_cast<std::uint64_t>(frameLimit) ||
        plan.sourceFrames > std::numeric_limits<std::uint64_t>::max() - sourceStart_ ||
        markers_.back().sourceFrame != static_cast<std::int64_t>(plan.sourceFrames) ||
        !std::isfinite(originTick_) || !std::isfinite(endTick) || endTick <= 0 ||
        !std::isfinite(loopStartTick_) || loopStartTick_ < 0 ||
        !std::isfinite(loopLengthTicks_) || loopLengthTicks_ < 0) {
        throw std::invalid_argument("invalid R3 Warp source window or placement");
    }
    for (std::size_t i = 1; i < markers_.size(); ++i) {
        if (markers_[i].sourceFrame <= markers_[i-1].sourceFrame || markers_[i].tick <= markers_[i-1].tick)
            throw std::invalid_argument("R3 Warp source clocks must increase");
    }

    const auto exactOriginFrame = tempo_.frameAtTick(originTick_);
    firstOutputFrame_ = checkedFrame(exactOriginFrame);
    originRounding_ = exactOriginFrame - static_cast<double>(firstOutputFrame_);
    endOutputFrame_ = checkedFrame(tempo_.frameAtTick(originTick_ + endTick) - originRounding_);
    endFileFrame_ = static_cast<double>(plan.sourceFrames);
    if (steadyFramesPerTick_ == 0) endFileFrame_ = std::min(endFileFrame_, sourceAtTick(endTick));
    if (loopLengthTicks_ > 0) {
        loopStartFileFrame_ = sourceAtTick(loopStartTick_);
        loopLengthFileFrames_ = sourceAtTick(loopStartTick_+loopLengthTicks_) - loopStartFileFrame_;
        if (!std::isfinite(loopLengthFileFrames_) || loopLengthFileFrames_ <= 0)
            throw std::invalid_argument("invalid R3 Warp loop range");
        if (steadyFramesPerTick_ != 0) {
            steadyLoopStart_ = checkedFrame(loopStartTick_ * steadyFramesPerTick_);
            steadyLoopLength_ = checkedFrame((loopStartTick_ + loopLengthTicks_) * steadyFramesPerTick_) - steadyLoopStart_;
            if (steadyLoopLength_ <= 0) throw std::invalid_argument("R3 Warp loop is shorter than one output frame");
        }
        endOutputFrame_ = frameLimit;
    }
    if (placementEndFrame) {
        endOutputFrame_ = std::min(endOutputFrame_, *placementEndFrame);
        // Dynamic R3 can read the next lap ahead of the output cursor. Keep
        // that future attack out of the synthesis window when the placement
        // ends. Steady tiles retain their full shared source-clock history.
        if (steadyFramesPerTick_ == 0) {
            terminalInputFrame_ = unrolledSourceAt(endOutputFrame_);
            if (loopLengthTicks_ > 0) {
                const auto laps = std::round((relativeTickAt(endOutputFrame_) - loopStartTick_) / loopLengthTicks_);
                const auto edgeTick = originTick_ + loopStartTick_ + laps * loopLengthTicks_;
                const auto edgeFrame = tempo_.frameAtTick(edgeTick) - originRounding_;
                // Round a terminal loop edge on the same clock as placement.
                // The frame-to-tick inverse can otherwise include the first
                // source sample of a lap that starts at the rounded cutoff.
                if (laps >= 1 && std::abs(edgeFrame - static_cast<double>(endOutputFrame_)) <= 0.5)
                    terminalInputFrame_ = loopStartFileFrame_ + laps * loopLengthFileFrames_;
            }
        }
    }

    voicePlan_.mode = plan.mode;
    voicePlan_.outputRate = plan.outputRate;
    voicePlan_.transposeSemis = plan.transposeSemis;
    voicePlan_.formantScale = plan.formantScale;
    voicePlan_.grainSeconds = plan.grainSeconds;
    latency_ = audio::RealtimeWarpR3::configuredLatencySamples(voicePlan_, sourceRate_, timeRatioAt(firstOutputFrame_));
}

std::int32_t WarpR3Pages::latencySamples() const noexcept { return latency_; }

void WarpR3Pages::releaseIdle() noexcept {
    for (auto& tile : renderTiles_) tile = {};
    for (auto& page : inputPages_) page = {};
}

double WarpR3Pages::sourceAtTick(double tick) const noexcept {
    const auto next = std::upper_bound(markers_.begin(), markers_.end(), tick,
        [](double value, const WarpMarker& marker) { return value < static_cast<double>(marker.tick); });
    const auto last = std::clamp(next, markers_.begin()+1, markers_.end()-1);
    const auto& left = *(last-1);
    const auto& right = *last;
    return static_cast<double>(left.sourceFrame) + (tick-static_cast<double>(left.tick)) *
        static_cast<double>(right.sourceFrame-left.sourceFrame) / static_cast<double>(right.tick-left.tick);
}

double WarpR3Pages::relativeTickAt(std::int64_t arrangementFrame) const noexcept {
    return tempo_.tickAtFrame(static_cast<double>(arrangementFrame) + originRounding_) - originTick_;
}

double WarpR3Pages::loopLapAt(double tick) const noexcept {
    if (loopLengthTicks_ == 0 || tick < loopStartTick_+loopLengthTicks_) return 0;
    return std::floor((tick-loopStartTick_) / loopLengthTicks_);
}

double WarpR3Pages::unrolledSourceAt(std::int64_t arrangementFrame) const noexcept {
    const auto tick = relativeTickAt(arrangementFrame);
    const auto lap = loopLapAt(tick);
    return sourceAtTick(tick-lap*loopLengthTicks_) + lap*loopLengthFileFrames_;
}

double WarpR3Pages::foldSource(double frame) const noexcept {
    if (loopLengthFileFrames_ == 0 || frame < loopStartFileFrame_+loopLengthFileFrames_) return frame;
    const auto lap = std::floor((frame-loopStartFileFrame_) / loopLengthFileFrames_);
    return frame-lap*loopLengthFileFrames_;
}

double WarpR3Pages::timeRatioAt(std::int64_t arrangementFrame) const noexcept {
    const auto tick = relativeTickAt(arrangementFrame);
    const auto localTick = tick-loopLapAt(tick)*loopLengthTicks_;
    return ticksPerSourceFrameAt(localTick) * tempo_.framesPerTickAt(originTick_+tick);
}

double WarpR3Pages::ticksPerSourceFrameAt(double localTick) const noexcept {
    const auto next = std::upper_bound(markers_.begin(), markers_.end(), localTick,
        [](double value, const WarpMarker& marker) { return value < static_cast<double>(marker.tick); });
    const auto last = std::clamp(next, markers_.begin()+1, markers_.end()-1);
    const auto& left = *(last-1);
    const auto& right = *last;
    return static_cast<double>(right.tick-left.tick) /
        static_cast<double>(right.sourceFrame-left.sourceFrame);
}

const float* WarpR3Pages::sourceFrame(std::int64_t frame, const WarpR3Source& reader) {
    if (frame < 0 || static_cast<double>(frame) >= endFileFrame_) return nullptr;
    const auto first = (frame / inputPageFrames) * inputPageFrames;
    auto page = std::find_if(inputPages_.begin(), inputPages_.end(), [first](const InputPage& entry) {
        return entry.first == first;
    });
    if (page == inputPages_.end()) {
        page = inputPages_.begin() + static_cast<std::ptrdiff_t>(nextInputPage_);
        page->first = -1;
        const auto count = std::min(inputPageFrames, static_cast<std::int64_t>(std::ceil(endFileFrame_)) - first);
        page->pcm.resize(static_cast<std::size_t>(count) * channels_);
        reader.read(reader.context, sourceStart_+static_cast<std::uint64_t>(first), page->pcm.data(),
                    static_cast<std::uint32_t>(count));
        page->first = first;
        nextInputPage_ = (nextInputPage_+1) % inputPages_.size();
    }
    return page->pcm.data() + static_cast<std::size_t>(frame-first)*channels_;
}

bool WarpR3Pages::readFrame(std::int64_t frame, std::span<float> pcm, const WarpR3Source& reader) {
    const auto unrolled = static_cast<double>(frame);
    if (terminalInputFrame_ && unrolled >= *terminalInputFrame_) {
        std::fill(pcm.begin(), pcm.end(), 0.0f);
        return true;
    }
    const auto position = foldSource(unrolled);
    const auto first = static_cast<std::int64_t>(std::floor(position));
    const auto fraction = static_cast<float>(position-static_cast<double>(first));
    const auto* a = sourceFrame(first, reader);
    if (a) std::copy_n(a, channels_, pcm.begin());
    else std::fill_n(pcm.begin(), channels_, 0.0f);
    if (fraction == 0) return true;

    auto second = first+1;
    if (loopLengthFileFrames_ > 0 && static_cast<double>(second) >= loopStartFileFrame_+loopLengthFileFrames_)
        second = static_cast<std::int64_t>(std::floor(loopStartFileFrame_));
    const bool retainNext = !terminalInputFrame_ || unrolled + (1.0 - fraction) < *terminalInputFrame_;
    const auto* b = retainNext ? sourceFrame(second, reader) : nullptr;
    for (std::size_t channel = 0; channel < channels_; ++channel)
        pcm[channel] = std::lerp(pcm[channel], b ? b[channel] : 0.0f, fraction);
    return true;
}

bool WarpR3Pages::render(std::int64_t first, std::uint32_t count, float* output,
                         const WarpR3Source& reader) {
    if (audio::onAudioThread || !output || !reader.read || first < -frameLimit ||
        first > frameLimit-static_cast<std::int64_t>(count)) return false;
    std::fill_n(output, static_cast<std::size_t>(count)*channels_, 0.0f);
    const auto begin = std::max(first, firstOutputFrame_);
    const auto end = std::min(first+count, endOutputFrame_);
    if (begin >= end) return true;
    return renderTiled(first, begin, end, output, reader);
}

std::int64_t WarpR3Pages::tileFrames() const noexcept {
    // Keep the established source-clock tiles for steady clips. Dynamic
    // tiles cover two 48 kHz output pages, amortizing their R3 preroll.
    return steadyFramesPerTick_ == 0 ? dynamicTileFrames : steadyTileFrames;
}

WarpR3Pages::RenderTile* WarpR3Pages::renderTile(std::int64_t first, const WarpR3Source& reader) {
    for (std::size_t i = 0; i < renderTiles_.size(); ++i) {
        if (renderTiles_[i].first == first) {
            lastRenderTile_ = i;
            return &renderTiles_[i];
        }
    }

    // Each tile has a fixed clock origin and bounded history. A seek or a
    // request in a different order reconstructs the same output.
    const auto latency = static_cast<std::int64_t>(latencySamples());
    const auto framesInTile = tileFrames();
    auto frame = std::max<std::int64_t>(0, first - 2 * latency);
    const auto sourceAt = steadyFramesPerTick_ != 0
        ? sourceAtTick(static_cast<double>(frame) / steadyFramesPerTick_)
        : unrolledSourceAt(firstOutputFrame_ + frame);
    const auto source = checkedFrame(std::max(0.0, sourceAt));
    const auto ratioAt = [this](std::int64_t at) noexcept {
        if (steadyFramesPerTick_ != 0)
            return ticksPerSourceFrameAt(static_cast<double>(at) / steadyFramesPerTick_) * steadyFramesPerTick_;
        // Dynamic ratio commands are scheduled from the arranged output
        // clock, including R3's synthesis delay passed by the caller below.
        return timeRatioAt(std::min(firstOutputFrame_+at, endOutputFrame_-1));
    };
    // R3's reset retains some analysis state in the multi-window modes.
    // Begin from a fresh worker voice so cache order cannot change the tile.
    const auto ratio = ratioAt(frame);
    if (!audio::RealtimeWarpR3::supportsRatio(ratio)) return nullptr;
    audio::RealtimeWarpR3 tileVoice(voicePlan_, sourceRate_, channels_, ratio, source);
    lastRenderTile_ = 1 - lastRenderTile_;
    auto& tile = renderTiles_[lastRenderTile_];
    tile.first = -1;
    tile.stitchLag.reset();
    tile.pcm.resize(static_cast<std::size_t>(framesInTile + tileOverlap + 2 * maxStitchLag) * channels_);

    struct Inputs {
        WarpR3Pages& pages;
        const WarpR3Source& reader;
        const decltype(ratioAt)& ratioAtFrame;
        const std::int64_t& frame;
        std::int64_t latency;
    } inputs{*this, reader, ratioAt, frame, latency};
    const audio::R3WarpInput input{&inputs,
        [](void* context, std::int64_t sourceFrame, std::span<float> pcm) {
            const auto& i = *static_cast<const Inputs*>(context);
            if (i.pages.steadyFramesPerTick_ == 0)
                return i.pages.readFrame(sourceFrame, pcm, i.reader);
            if (const auto* sample = i.pages.sourceFrame(sourceFrame, i.reader))
                std::copy_n(sample, i.pages.channels_, pcm.begin());
            else
                std::fill(pcm.begin(), pcm.end(), 0.0f);
            return true;
        }, [](void* context, std::int64_t) noexcept {
            const auto& i = *static_cast<const Inputs*>(context);
            return i.ratioAtFrame(i.frame + i.latency);
        }};
    std::array<float, 64> history{};
    for (; frame < first + framesInTile + tileOverlap + maxStitchLag; ++frame) {
        auto* output = frame < first-maxStitchLag ? history.data() :
            tile.pcm.data() + static_cast<std::size_t>(frame-first+maxStitchLag) * channels_;
        if (!tileVoice.pull({output, channels_}, input)) return nullptr;
    }
    tile.first = first;
    return &tile;
}

bool WarpR3Pages::renderTiled(std::int64_t first, std::int64_t begin, std::int64_t end,
                             float* output, const WarpR3Source& reader) {
    const auto framesInTile = tileFrames();
    for (auto frame = begin; frame < end; ++frame) {
        auto local = frame - firstOutputFrame_;
        if (steadyLoopLength_ > 0 && local >= steadyLoopStart_)
            local = steadyLoopStart_ + (local - steadyLoopStart_) % steadyLoopLength_;
        const auto tileOffset = local % framesInTile;
        const auto tileFirst = local - tileOffset;
        auto* tile = renderTile(tileFirst, reader);
        if (!tile) return false;
        auto* pcm = output + static_cast<std::size_t>(frame - first) * channels_;
        const auto* current = tile->pcm.data() + static_cast<std::size_t>(maxStitchLag) * channels_;
        if (tileFirst == 0 || tileOffset >= tileOverlap) {
            std::copy_n(current+static_cast<std::size_t>(tileOffset)*channels_, channels_, pcm);
            continue;
        }

        // Both fixed tiles cover this span. Blend their matching samples so
        // changing spectral history at the tile edge has no step.
        const auto* previous = renderTile(tileFirst - framesInTile, reader);
        if (!previous) return false;
        const auto* prior = previous->pcm.data() + static_cast<std::size_t>(framesInTile+maxStitchLag) * channels_;
        if (!tile->stitchLag) tile->stitchLag = bestStitchLag(prior, current, channels_);
        const auto t = static_cast<float>(tileOffset) / static_cast<float>(tileOverlap);
        const auto blend = t*t*(3-2*t);
        // Fade out the lead/lag before either tile becomes the sole source.
        // The matching samples blend without a level dip in correlated tones.
        const auto lag = static_cast<double>(*tile->stitchLag);
        const auto previousPosition = static_cast<double>(tileOffset)-blend*lag;
        const auto currentPosition = static_cast<double>(tileOffset)+(1-blend)*lag;
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            const auto a = stitchSample(prior, previousPosition, channels_, channel);
            const auto b = stitchSample(current, currentPosition, channels_, channel);
            pcm[channel] = std::lerp(a, b, blend);
        }
    }
    return true;
}

} // namespace adi::engine
