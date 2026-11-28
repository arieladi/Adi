// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "focus_scope.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

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
inline constexpr std::size_t minimumSupport = 64, directRadius = (frames - minimumSupport) / 2;
inline constexpr double searchSeconds = .020, maxSampleRate = 768000.;
inline constexpr auto maxRadius = static_cast<std::size_t>(searchSeconds * maxSampleRate);
inline constexpr double minimumCorrelation = .70, rmsFloor = 1e-6;

inline std::size_t captureFrames(std::size_t radius) noexcept {
    // Preserve the original common interval where it fits. At higher rates a
    // +/-20 ms search uses 40 ms of common support and 20 ms on either side.
    return radius <= directRadius ? frames : 4 * radius;
}

inline std::optional<std::int64_t> requestedEnd(const FocusLagRequest &request) noexcept {
    if (!request.transportRevision || request.firstStamp < 0 || request.maxLagSamples <= 0 ||
        static_cast<std::size_t>(request.maxLagSamples) > maxRadius ||
        request.sources[0].source.track == request.sources[1].source.track)
        return {};
    const auto count = captureFrames(static_cast<std::size_t>(request.maxLagSamples));
    const auto end = focusWindowEnd(request.firstStamp, static_cast<std::int64_t>(count));
    return end && *end <= request.heardEnd ? end : std::nullopt;
}

inline bool validSource(const FocusScopeBarSource &identity, std::uint64_t generation) noexcept {
    return identity.source.track > 0 && identity.source.point == engine::ScopePoint::PostFader &&
           !identity.source.device && identity.generation && identity.generation == generation;
}

inline bool validRate(const FocusLagRequest &request, double sampleRate) noexcept {
    return std::isfinite(sampleRate) && sampleRate > 0. && sampleRate <= maxSampleRate &&
           static_cast<double>(request.maxLagSamples) <= std::round(searchSeconds * sampleRate);
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
                     std::span<double> mid) noexcept {
    for (std::size_t i = 0; i < mid.size(); ++i) {
        const double mono = .5 * (static_cast<double>(left[i]) + right[i]);
        if (!std::isfinite(mono)) return false;
        mid[i] = mono;
    }
    return true;
}

inline std::optional<FocusLagPreview> selectPeak(std::span<const double> scores,
                                                 std::size_t radius, std::size_t support) noexcept {
    constexpr double peakSeparation = .05;
    std::size_t winner = 0;
    double best = 0.;
    for (std::size_t offset = 0; offset < scores.size(); ++offset) {
        if (const auto magnitude = std::abs(scores[offset]); magnitude > best) {
            best = magnitude;
            winner = offset;
        }
    }
    if (!winner || winner + 1 == scores.size()) return {};
    // Equal signal and noise power can pull a clearly isolated peak below .70.
    // Admit that narrow band only with a long common window and stronger separation.
    const bool nearFloor = best < minimumCorrelation;
    if (nearFloor && (support < frames / 2 || best < .68)) return {};
    const double separation = nearFloor ? .15 : peakSeparation;

    // The shoulders of the best lobe are not separate hypotheses. A competing
    // local maximum, including a tie or flat peak, makes the shift uncertain.
    for (std::size_t i = 0; i < scores.size(); ++i) {
        const auto magnitude = std::abs(scores[i]);
        if (i == winner || best - magnitude >= separation) continue;
        if ((i == 0 || magnitude >= std::abs(scores[i - 1])) &&
            (i + 1 == scores.size() || magnitude >= std::abs(scores[i + 1]))) return {};
    }
    return FocusLagPreview{static_cast<std::int32_t>(winner) - static_cast<std::int32_t>(radius),
                           scores[winner], scores[radius]};
}

std::optional<FocusLagPreview> measureWide(std::span<const double> a, std::span<const double> b,
                                           std::size_t radius, double aEnergy);

inline double correlationAt(std::span<const double> a, std::span<const double> b, double aEnergy,
                            double weight, double minimumEnergy) noexcept {
    const double bMean = std::accumulate(b.begin(), b.end(), 0.) / weight;
    double bEnergy = 0., cross = 0.;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double centred = b[i] - bMean;
        bEnergy += centred * centred;
        cross += a[i] * centred;
    }
    return bEnergy >= minimumEnergy
        ? std::clamp(cross / std::sqrt(aEnergy * bEnergy), -1., 1.) : 0.;
}

inline std::optional<FocusLagPreview> measure(std::size_t radius, std::span<double> main,
                                              std::span<const double> target) {
    const auto support = main.size() - 2 * radius;
    const auto scale = static_cast<double>(support);
    const double minimumEnergy = rmsFloor * rmsFloor * scale;
    const auto a = main.subspan(radius, support);
    const double aMean = std::accumulate(a.begin(), a.end(), 0.) / scale;
    double aEnergy = 0.;
    for (double &sample : a) {
        sample -= aMean;
        aEnergy += sample * sample;
    }
    if (aEnergy < minimumEnergy) return {};
    if (radius > directRadius) return measureWide(a, target, radius, aEnergy);

    // Use the same interior A frames at every lag. B index i + lag means
    // a positive result should advance B by that many samples.
    std::array<double, 2 * directRadius + 1> scores;
    const auto candidates = 2 * radius + 1;
    for (std::size_t offset = 0; offset < candidates; ++offset)
        scores[offset] = correlationAt(a, target.subspan(offset, support), aEnergy, scale, minimumEnergy);
    return selectPeak(std::span(scores).first(candidates), radius, support);
}

// The wide store is a plain owning pointer: an empty MSVC Debug vector already
// allocates its iterator proxy, and the ordinary-rate path must allocate nothing.
template <class Sample, std::size_t Capacity>
std::span<Sample> captureStorage(std::array<Sample, Capacity> &local, std::unique_ptr<Sample[]> &wide,
                                 std::size_t count) {
    if (count <= Capacity) return std::span(local).first(count);
    wide = std::make_unique<Sample[]>(count);
    return {wide.get(), count};
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
    if (!settledRevision || !windows[0] || !windows[1]) return {};
    const auto sampleRate = windows[0]->sampleRate;
    if (!std::isfinite(sampleRate) || sampleRate <= 0. || sampleRate > maxSampleRate ||
        windows[1]->sampleRate != sampleRate) return {};
    const auto radius = static_cast<std::size_t>(std::round(searchSeconds * sampleRate));
    const auto frameCount = static_cast<std::int64_t>(captureFrames(radius));
    if (!radius || heardEnd < frameCount) return {};

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
    request.maxLagSamples = static_cast<std::int32_t>(radius);
    return requestedEnd(request) ? std::optional{request} : std::nullopt;
}

// One explicit analysis job over immutable raw samples. The caller holds the
// current transport/source witnesses and excludes ambiguous loop passes.
[[nodiscard]] inline std::optional<FocusLagPreview>
focusFindLag(const FocusLagRequest &request, const SharedAnalysis::Result &main,
             const SharedAnalysis::Result &target) noexcept try {
    using namespace focus_lag_detail;
    const auto end = requestedEnd(request);
    if (!end || main.sampleRate != target.sampleRate || !validRate(request, main.sampleRate)) return {};

    const auto radius = static_cast<std::size_t>(request.maxLagSamples);
    const auto count = captureFrames(radius);
    const std::array inputs{&main, &target};
    std::array<double, 2 * frames> localMid;
    std::unique_ptr<double[]> wideMid;
    auto mid = captureStorage(localMid, wideMid, 2 * count);
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
        if (!mixToMid(std::span(source.scopeLeft).subspan(offset, count),
                      std::span(source.scopeRight).subspan(offset, count), mid.subspan(side * count, count))) return {};
    }
    return measure(radius, mid.first(count), mid.last(count));
} catch (const std::bad_alloc &) { return {}; }

[[nodiscard]] inline std::optional<FocusLagPreview>
focusFindLagOnTaps(const FocusLagRequest &request,
                  const std::array<std::shared_ptr<const engine::ScopeTap>, 2> &taps) noexcept try {
    using namespace focus_lag_detail;
    const auto wantedEnd = requestedEnd(request);
    if (!wantedEnd || !taps[0] || !taps[1] || taps[0]->sampleRate() != taps[1]->sampleRate() ||
        !validRate(request, taps[0]->sampleRate())) return {};

    const auto radius = static_cast<std::size_t>(request.maxLagSamples);
    const auto count = captureFrames(radius);
    std::array<double, 2 * frames> localMid;
    std::array<float, 2 * frames> localStereo;
    std::unique_ptr<double[]> wideMid;
    std::unique_ptr<float[]> wideStereo;
    auto mid = captureStorage(localMid, wideMid, 2 * count);
    const auto stereo = captureStorage(localStereo, wideStereo, 2 * count);
    const auto left = stereo.first(count), right = stereo.last(count);
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
        if (read.status != Status::Ready || read.frames != static_cast<std::int32_t>(count) ||
            read.firstSequence != beginning || read.firstStamp != request.firstStamp || !read.advancing ||
            read.transportRevision != request.transportRevision ||
            !mixToMid(left, right, mid.subspan(side * count, count))) return {};
    }
    const auto preview = measure(radius, mid.first(count), mid.last(count));
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
} catch (const std::bad_alloc &) { return {}; }
} // namespace adi::ui
