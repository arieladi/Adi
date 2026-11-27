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

std::int64_t checkedFrame(double frame) {
    if (!std::isfinite(frame) || std::abs(frame) >= static_cast<double>(frameLimit))
        throw std::invalid_argument("R3 Warp placement is outside the supported frame range");
    return std::llround(frame);
}

} // namespace

WarpR3Pages::WarpR3Pages(const audio::WarpRenderPlan& plan, std::uint32_t sourceRate,
                         std::uint16_t channels, std::span<const WarpMarker> markers,
                         const WarpArrangementClock& tempo, double sourceOriginTick,
                         double endTick, double loopStartTick, double loopLengthTicks)
    : tempo_(tempo), markers_(markers.begin(), markers.end()), sourceStart_(plan.sourceStart),
      channels_(channels), originTick_(sourceOriginTick), loopStartTick_(loopStartTick),
      loopLengthTicks_(loopLengthTicks) {
    if (markers_.size() < 2 || markers_.front() != WarpMarker{} || plan.sourceFrames == 0 ||
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
    endFileFrame_ = std::min(static_cast<double>(plan.sourceFrames), sourceAtTick(endTick));
    if (loopLengthTicks_ > 0) {
        loopStartFileFrame_ = sourceAtTick(loopStartTick_);
        loopLengthFileFrames_ = sourceAtTick(loopStartTick_+loopLengthTicks_) - loopStartFileFrame_;
        if (!std::isfinite(loopLengthFileFrames_) || loopLengthFileFrames_ <= 0)
            throw std::invalid_argument("invalid R3 Warp loop range");
        endOutputFrame_ = frameLimit;
    }

    voice_ = std::make_unique<audio::RealtimeWarpR3>(plan, sourceRate, channels_, timeRatioAt(firstOutputFrame_));
    nextOutputFrame_ = firstOutputFrame_;
    for (auto& page : inputPages_)
        page.pcm.resize(static_cast<std::size_t>(inputPageFrames) * channels_);
}

std::int32_t WarpR3Pages::latencySamples() const noexcept { return voice_->latencySamples(); }

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
    const auto next = std::upper_bound(markers_.begin(), markers_.end(), localTick,
        [](double value, const WarpMarker& marker) { return value < static_cast<double>(marker.tick); });
    const auto last = std::clamp(next, markers_.begin()+1, markers_.end()-1);
    const auto& left = *(last-1);
    const auto& right = *last;
    const auto ticksPerSourceFrame = static_cast<double>(right.tick-left.tick) /
        static_cast<double>(right.sourceFrame-left.sourceFrame);
    return ticksPerSourceFrame * tempo_.framesPerTickAt(originTick_+tick);
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
        reader.read(reader.context, sourceStart_+static_cast<std::uint64_t>(first), page->pcm.data(),
                    static_cast<std::uint32_t>(count));
        page->first = first;
        nextInputPage_ = (nextInputPage_+1) % inputPages_.size();
    }
    return page->pcm.data() + static_cast<std::size_t>(frame-first)*channels_;
}

bool WarpR3Pages::readFrame(std::int64_t frame, std::span<float> pcm, const WarpR3Source& reader) {
    const auto position = foldSource(static_cast<double>(frame));
    const auto first = static_cast<std::int64_t>(std::floor(position));
    const auto fraction = static_cast<float>(position-static_cast<double>(first));
    const auto* a = sourceFrame(first, reader);
    if (a) std::copy_n(a, channels_, pcm.begin());
    else std::fill_n(pcm.begin(), channels_, 0.0f);
    if (fraction == 0) return true;

    auto second = first+1;
    if (loopLengthFileFrames_ > 0 && static_cast<double>(second) >= loopStartFileFrame_+loopLengthFileFrames_)
        second = static_cast<std::int64_t>(std::floor(loopStartFileFrame_));
    const auto* b = sourceFrame(second, reader);
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

    auto cursor = begin;
    if (nextOutputFrame_ != begin) {
        // A cold page starts with real source context. Running consecutive
        // pages never enters this path or discards the previous phase state.
        const auto history = std::int64_t{2} * latencySamples();
        cursor = std::max(firstOutputFrame_, begin-history);
        const auto source = checkedFrame(std::max(0.0, unrolledSourceAt(cursor)));
        if (!voice_->restart(source, timeRatioAt(cursor))) return false;
    }
    nextOutputFrame_.reset();

    struct Inputs {
        WarpR3Pages& pages;
        const WarpR3Source& reader;
        const std::int64_t& outputFrame;
    } inputs{*this, reader, cursor};
    const audio::R3WarpInput input{&inputs,
        [](void* context, std::int64_t frame, std::span<float> pcm) {
            const auto& i = *static_cast<const Inputs*>(context);
            return i.pages.readFrame(frame, pcm, i.reader);
        }, [](void* context, std::int64_t) noexcept {
            const auto& i = *static_cast<const Inputs*>(context);
            // R3 applies the ratio before its synthesis window reaches the
            // aligned output. Schedule the musical command one R3 delay
            // ahead, independently of how far the file read cursor has run.
            const auto ratioFrame = std::min(i.outputFrame + i.pages.latencySamples(),
                                             i.pages.endOutputFrame_-1);
            return i.pages.timeRatioAt(ratioFrame);
        }};
    std::array<float, 64> preroll{};
    for (; cursor < end; ++cursor) {
        auto* destination = cursor < begin ? preroll.data() :
            output + static_cast<std::size_t>(cursor-first)*channels_;
        if (!voice_->pull({destination, channels_}, input)) return false;
    }
    nextOutputFrame_ = end;
    return true;
}

} // namespace adi::engine
