// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "focus_scope.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <span>

namespace adi::ui {
struct FocusLagRequest {
    static constexpr std::size_t kFrames = 4096;
    // Identities witnessed when the owner selected the exact heard interval.
    std::array<FocusScopeBarSource, 2> sources{};
    std::uint64_t transportRevision = 0;
    std::int64_t firstStamp = 0, heardEnd = 0;
    std::int32_t maxLagSamples = 0;
};

struct FocusLagPreview {
    std::int32_t lagSamples = 0; // positive when B is late
    double correlation = 0.;           // signed correlation at the proposed lag
    double unalignedCorrelation = 0.;  // signed correlation at the current time alignment
};

namespace focus_lag_detail {
inline constexpr auto frames = FocusLagRequest::kFrames;
inline constexpr std::size_t minimumSupport = 64, maxRadius = (frames - minimumSupport) / 2;
inline constexpr double minimumCorrelation = .70;
using MidWindows = std::array<std::array<double, frames>, 2>;

inline std::optional<std::int64_t> requestedEnd(const FocusLagRequest &request) noexcept {
    if (!request.transportRevision || request.firstStamp < 0 || request.maxLagSamples <= 0 ||
        static_cast<std::size_t>(request.maxLagSamples) > maxRadius ||
        request.sources[0].source.track == request.sources[1].source.track)
        return {};
    const auto end = focusWindowEnd(request.firstStamp, static_cast<std::int64_t>(frames));
    return end && *end <= request.heardEnd ? end : std::nullopt;
}

inline bool validSource(const FocusScopeBarSource &identity, std::uint64_t generation) noexcept {
    return identity.source.track > 0 && identity.source.point == engine::ScopePoint::PostFader &&
           !identity.source.device && identity.generation && identity.generation == generation;
}

inline bool validRate(const FocusLagRequest &request, double sampleRate) noexcept {
    return std::isfinite(sampleRate) && sampleRate > 0. &&
           static_cast<double>(request.maxLagSamples) <= std::round(.020 * sampleRate);
}

inline std::optional<engine::ScopeTap::ReadResult> advancingHead(const engine::ScopeTap &tap,
                                                                std::uint64_t revision) noexcept {
    auto cursor = tap.cursorAtEnd();
    if (cursor.next <= 0) return {};
    const auto sequence = --cursor.next;
    std::array<float, 1> left, right;
    const auto read = tap.read(cursor, left, right);
    if (read.status != engine::ScopeTap::ReadStatus::Ready || read.frames != 1 || read.firstSequence != sequence ||
        !read.advancing || read.transportRevision != revision) return {};
    return read;
}

inline bool mixToMid(std::span<const float> left, std::span<const float> right,
                     std::array<double, frames> &mid) noexcept {
    for (std::size_t i = 0; i < frames; ++i) {
        const double mono = .5 * (static_cast<double>(left[i]) + right[i]);
        if (!std::isfinite(mono)) return false;
        mid[i] = mono;
    }
    return true;
}

inline std::optional<FocusLagPreview> measure(const FocusLagRequest &request, MidWindows &mid) noexcept {
    constexpr double rmsFloor = 1e-6, peakSeparation = .05;
    const auto radius = static_cast<std::size_t>(request.maxLagSamples);
    const auto support = frames - 2 * radius;
    const auto scale = static_cast<double>(support);
    const double minimumEnergy = rmsFloor * rmsFloor * scale;
    const auto a = std::span(mid[0]).subspan(radius, support);
    const double aMean = std::accumulate(a.begin(), a.end(), 0.) / scale;
    double aEnergy = 0.;
    for (double &sample : a) {
        sample -= aMean;
        aEnergy += sample * sample;
    }
    if (aEnergy < minimumEnergy) return {};

    // Use the same interior A frames at every lag. B index i + lag means
    // a positive result should advance B by that many samples.
    std::array<double, 2 * maxRadius + 1> scores;
    const auto candidates = 2 * radius + 1;
    std::size_t winner = 0;
    double best = 0.;
    for (std::size_t offset = 0; offset < candidates; ++offset) {
        const auto b = std::span(mid[1]).subspan(offset, support);
        const double bMean = std::accumulate(b.begin(), b.end(), 0.) / scale;
        double bEnergy = 0., cross = 0.;
        for (std::size_t i = 0; i < support; ++i) {
            const double centred = b[i] - bMean;
            bEnergy += centred * centred;
            cross += a[i] * centred;
        }
        const auto score = bEnergy >= minimumEnergy ? std::clamp(cross / std::sqrt(aEnergy * bEnergy), -1., 1.) : 0.;
        scores[offset] = score;
        if (std::abs(score) > best) {
            best = std::abs(score);
            winner = offset;
        }
    }
    if (best < minimumCorrelation || !winner || winner + 1 == candidates) return {};

    // The shoulders of the best lobe are not separate hypotheses. A competing
    // local maximum, including a tie or flat peak, makes the shift uncertain.
    for (std::size_t i = 0; i < candidates; ++i) {
        const auto magnitude = std::abs(scores[i]);
        if (i == winner || best - magnitude >= peakSeparation) continue;
        if ((i == 0 || magnitude >= std::abs(scores[i - 1])) &&
            (i + 1 == candidates || magnitude >= std::abs(scores[i + 1]))) return {};
    }
    return FocusLagPreview{static_cast<std::int32_t>(winner) - request.maxLagSamples, scores[winner], scores[radius]};
}
} // namespace focus_lag_detail

// Plan from the two current published windows and their existing tap heads.
// The windows locate a common heard end; the worker verifies the older raw
// interval, its continuity and the broker's physical tap bindings.
[[nodiscard]] inline std::optional<FocusLagRequest>
focusLagPreviewRequest(const std::array<const SharedAnalysis::Result *, 2> &windows,
                       const std::array<SharedAnalysis::Lease::SourcePosition, 2> &heads,
                       std::uint64_t settledRevision, std::int64_t heardEnd) noexcept {
    using namespace focus_lag_detail;
    constexpr auto frameCount = static_cast<std::int64_t>(FocusLagRequest::kFrames);
    if (!settledRevision || heardEnd < frameCount || !windows[0] || !windows[1]) return {};
    const auto sampleRate = windows[0]->sampleRate;
    if (!std::isfinite(sampleRate) || sampleRate <= 0. || windows[1]->sampleRate != sampleRate) return {};

    FocusLagRequest request;
    request.transportRevision = settledRevision;
    request.heardEnd = heardEnd;
    auto end = heardEnd;
    for (std::size_t side = 0; side < windows.size(); ++side) {
        const auto &window = *windows[side];
        const auto &head = heads[side];
        auto &source = request.sources[side];
        source = {window.request.source(), window.generation};
        if (!validSource(source, head.generation) || !window.windowSerial || !window.advancing ||
            window.discontinuity || window.transportRevision != settledRevision ||
            window.firstStamp < 0 || window.firstStamp >= heardEnd || window.firstSequence < 0 ||
            head.written < frameCount) return {};
        const auto windowEnd = focusWindowEnd(window.firstStamp, window.request.fftSize);
        const auto sequenceEnd = focusWindowEnd(window.firstSequence, window.request.fftSize);
        if (!windowEnd || !sequenceEnd || *sequenceEnd > head.written) return {};
        end = std::min(end, *windowEnd);
    }
    if (end < frameCount) return {};
    request.firstStamp = end - frameCount;
    request.maxLagSamples = static_cast<std::int32_t>(
        std::min(std::round(.020 * sampleRate), static_cast<double>(maxRadius)));
    return requestedEnd(request) ? std::optional{request} : std::nullopt;
}

// One explicit analysis job over immutable raw samples. The caller holds the
// current transport/source witnesses and excludes ambiguous loop passes.
[[nodiscard]] inline std::optional<FocusLagPreview>
focusFindLag(const FocusLagRequest &request, const SharedAnalysis::Result &main,
             const SharedAnalysis::Result &target) noexcept {
    using namespace focus_lag_detail;
    const auto end = requestedEnd(request);
    if (!end || main.sampleRate != target.sampleRate || !validRate(request, main.sampleRate)) return {};

    const std::array inputs{&main, &target};
    MidWindows mid;
    for (std::size_t side = 0; side < inputs.size(); ++side) {
        const auto &source = *inputs[side];
        const auto &expected = request.sources[side];
        const auto range = focusScopeMember(source);
        if (!range || !validSource(expected, source.generation) || source.request.source() != expected.source ||
            !source.windowSerial || source.transportRevision != request.transportRevision || range->first > request.firstStamp)
            return {};
        const auto sourceEnd = focusWindowEnd(range->first, static_cast<std::int64_t>(range->frames));
        if (!sourceEnd || *sourceEnd < *end || *sourceEnd > request.heardEnd) return {};
        const auto offset = static_cast<std::size_t>(request.firstStamp - range->first);
        if (!mixToMid(std::span(source.scopeLeft).subspan(offset, frames),
                      std::span(source.scopeRight).subspan(offset, frames), mid[side])) return {};
    }
    return measure(request, mid);
}

[[nodiscard]] inline std::optional<FocusLagPreview>
focusFindLagOnTaps(const FocusLagRequest &request,
                  const std::array<std::shared_ptr<const engine::ScopeTap>, 2> &taps) noexcept {
    using namespace focus_lag_detail;
    const auto wantedEnd = requestedEnd(request);
    if (!wantedEnd || !taps[0] || !taps[1] || taps[0]->sampleRate() != taps[1]->sampleRate() ||
        !validRate(request, taps[0]->sampleRate())) return {};

    MidWindows mid;
    std::array<float, frames> left, right;
    std::array<engine::ScopeTap::ReadResult, 2> heads;
    using Status = engine::ScopeTap::ReadStatus;
    for (std::size_t side = 0; side < taps.size(); ++side) {
        const auto &tap = *taps[side];
        if (!validSource(request.sources[side], tap.generation())) return {};
        const auto last = advancingHead(tap, request.transportRevision);
        const auto observedEnd = last ? focusWindowEnd(last->firstStamp, 1) : std::nullopt;
        if (!observedEnd || *observedEnd < *wantedEnd) return {};
        heads[side] = *last;

        // A newer same-epoch head can locate the already-heard requested samples.
        // The exact read verifies the inverse and refuses a split span.
        const auto back = last->firstStamp - request.firstStamp;
        if (back > last->firstSequence) return {};
        const auto beginning = last->firstSequence - back;
        engine::ScopeTap::Cursor cursor{tap.generation(), beginning};
        const auto read = tap.read(cursor, left, right);
        if (read.status != Status::Ready || read.frames != static_cast<std::int32_t>(frames) ||
            read.firstSequence != beginning || read.firstStamp != request.firstStamp || !read.advancing ||
            read.transportRevision != request.transportRevision || !mixToMid(left, right, mid[side])) return {};
    }
    const auto preview = measure(request, mid);
    if (!preview) return {};
    // A seek or Stop can occur while the worker correlates the immutable input.
    for (std::size_t side = 0; side < taps.size(); ++side) {
        const auto latest = advancingHead(*taps[side], request.transportRevision);
        if (!latest || latest->firstSequence < heads[side].firstSequence) return {};
        const auto delta = latest->firstSequence - heads[side].firstSequence;
        const auto expected = delta ? focusWindowEnd(heads[side].firstStamp, delta)
                                    : std::optional{heads[side].firstStamp};
        if (!expected || *expected != latest->firstStamp) return {};
    }
    return preview;
}
} // namespace adi::ui
