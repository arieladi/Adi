// SPDX-License-Identifier: AGPL-3.0-or-later
#include "analysis_comparison.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace adi::ui {
namespace {
bool sameCalibration(const SharedAnalysis::Request &a, const SharedAnalysis::Request &b) noexcept {
    return a.fftSize == b.fftSize && a.hopSize == b.hopSize && a.window == b.window && a.channel == b.channel;
}

AnalysisComparison::Difference differenceBetweenLevels(float a, float b) noexcept {
    AnalysisComparison::Difference result;
    if (!std::isfinite(a) || !std::isfinite(b)) return result;
    result.aCensored = a <= dsp::kDbFloor;
    result.bCensored = b <= dsp::kDbFloor;
    if (!result.aCensored && !result.bCensored) {
        const auto delta = a - b;
        if (std::isfinite(delta)) result.db = delta;
    }
    return result;
}
} // namespace

AnalysisComparison::AnalysisComparison(std::shared_ptr<SharedAnalysis> broker) noexcept
    : broker_(std::move(broker)) {}

bool AnalysisComparison::normalize(Endpoint &endpoint) noexcept {
    auto &r = endpoint.request;
    // Match SharedAnalysis's admitted source/FFT capability, including its positive
    // track identity. There is no special numeric Main fallback in this component.
    if (!endpoint.ownerToken || r.track <= 0 || r.fftSize < 32 || r.fftSize > SharedAnalysis::kMaxFftSize ||
        r.fftSize % 32 || r.hopSize < 0 || r.hopSize > r.fftSize ||
        r.outputs != SharedAnalysis::Spectrum || r.heardSpectrogramGrid)
        return false;
    auto factors = r.fftSize;
    for (const auto factor : {2, 3, 5})
        while (factors % factor == 0)
            factors /= factor;
    const bool device =
        r.point == engine::ScopePoint::DeviceInput || r.point == engine::ScopePoint::DeviceOutput;
    if (factors != 1 || (r.window != dsp::Window::Hann && r.window != dsp::Window::Rectangular) ||
        r.channel < SharedAnalysis::Channel::Mid || r.channel > SharedAnalysis::Channel::StereoPower ||
        r.point < engine::ScopePoint::PostFader || r.point > engine::ScopePoint::DeviceOutput ||
        (device ? r.device <= 0 : r.device != 0))
        return false;
    if (!r.hopSize)
        r.hopSize = r.fftSize / 4;
    return true;
}

bool AnalysisComparison::validPicture(Picture picture) noexcept {
    if (!picture.metadata)
        return false;
    const auto &m = *picture.metadata;
    auto endpoint = m.endpoint;
    if (!normalize(endpoint) || endpoint != m.endpoint || !m.generation || !m.windowSerial ||
        !(m.outputs & SharedAnalysis::Spectrum) || m.firstSequence < 0 || m.pendingFrames < 0 ||
        m.firstSequence > std::numeric_limits<std::int64_t>::max() - endpoint.request.fftSize ||
        !std::isfinite(m.sampleRate) || m.sampleRate <= 0 || !std::isfinite(m.coherentGain) ||
        m.coherentGain <= 0 || m.hopSize != endpoint.request.hopSize ||
        picture.binsDbfs.size() != static_cast<std::size_t>(endpoint.request.fftSize / 2 + 1))
        return false;
    return std::all_of(picture.binsDbfs.begin(), picture.binsDbfs.end(),
                       [](float value) { return std::isfinite(value) && value >= dsp::kDbFloor; });
}

AnalysisComparison::Status AnalysisComparison::classify(Picture a, Picture b) noexcept {
    if (!validPicture(a) || !validPicture(b))
        return Status::Unavailable;
    const auto &am = *a.metadata;
    const auto &bm = *b.metadata;
    return sameCalibration(am.endpoint.request, bm.endpoint.request) && am.sampleRate == bm.sampleRate &&
                   am.coherentGain == bm.coherentGain
               ? Status::Independent
               : Status::Incompatible;
}

AnalysisComparison::Metadata AnalysisComparison::metadata(const SharedAnalysis::Result &r,
                                                          std::uint64_t token) noexcept {
    Metadata m;
    m.endpoint = {r.request, token};
    m.endpoint.request.outputs = SharedAnalysis::Spectrum;
    m.endpoint.request.hopSize = r.hopSize;
    m.revision = r.revision;
    m.sourceEpoch = r.sourceEpoch;
    m.generation = r.generation;
    m.windowSerial = r.windowSerial;
    m.transportRevision = r.transportRevision;
    m.firstStamp = r.firstStamp;
    m.firstSequence = r.firstSequence;
    m.sampleRate = r.sampleRate;
    m.coherentGain = r.coherentGain;
    m.hopSize = r.hopSize;
    m.outputs = r.outputs;
    m.gapCount = r.gapCount;
    m.overrunCount = r.overrunCount;
    m.historySerial = r.historySerial;
    m.droppedHistoryColumns = r.droppedHistoryColumns;
    m.nonFiniteSamples = r.nonFiniteSamples;
    m.pendingFrames = r.pendingFrames;
    m.advancing = r.advancing;
    m.discontinuity = r.discontinuity;
    m.budgetLimited = r.budgetLimited;
    m.historyLimited = r.historyLimited;
    return m;
}

AnalysisComparison::Snapshot AnalysisComparison::snapshot(Picture picture) {
    return {*picture.metadata, {picture.binsDbfs.begin(), picture.binsDbfs.end()}};
}

void AnalysisComparison::fence(Input &input) noexcept {
    const auto position = input.lease.sourcePosition();
    input.watermark = {position.generation, position.written};
    input.live.reset();
}

void AnalysisComparison::release(Input &input) noexcept {
    input.live.reset();
    input.lease.reset();
}

bool AnalysisComparison::acquire(Input &input) const {
    input.lease = broker_->acquire(input.endpoint.request);
    if (!input.lease)
        return false;
    fence(input);
    return true;
}

void AnalysisComparison::update(Input &input) noexcept {
    auto next = input.lease.read();
    if (!next) {
        input.live.reset();
        return;
    }
    if (next == input.live)
        return;
    // Replacement/unavailable-to-live admission establishes a new tap barrier. Even
    // a newly read Result can contain older buffered audio from that generation.
    if (next->generation != input.watermark.generation) {
        fence(input);
        return;
    }
    const auto m = metadata(*next, input.endpoint.ownerToken);
    if (m.endpoint != input.endpoint || !validPicture({&m, next->binsDbfs})) {
        input.live.reset();
        return;
    }
    const auto end = m.firstSequence + m.endpoint.request.fftSize;
    if (end < input.watermark.end || (end == input.watermark.end && !input.live)) {
        input.live.reset();
        return;
    }
    input.watermark = {m.generation, end};
    input.metadata = m;
    input.live = std::move(next);
}

bool AnalysisComparison::configure(Endpoint a, Endpoint b) noexcept {
    if (!broker_ || !broker_->active())
        return false;
    if (!normalize(a) || !normalize(b))
        return false;
    if (configured_ && a == a_.endpoint && b == b_.endpoint)
        return true;
    const bool keepB = configured_ && b == b_.endpoint;
    if (keepB && heldB_ && !sameCalibration(a.request, heldB_->metadata.endpoint.request))
        return false;
    try {
        Input nextA, nextB;
        nextA.endpoint = a;
        nextB.endpoint = b;
        if (visible_ && (!acquire(nextA) || (!(keepB && heldB_) && !acquire(nextB))))
            return false;
        a_ = std::move(nextA);
        b_ = std::move(nextB);
        if (!keepB)
            heldB_.reset();
        frozenA_.reset();
        frozenB_.reset();
        frozen_ = false;
        configured_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool AnalysisComparison::setVisible(bool visible) noexcept {
    if (visible && (!broker_ || !broker_->active()))
        return false;
    if (visible == visible_)
        return true;
    if (!visible) {
        release(a_);
        release(b_);
        visible_ = false;
        return true;
    }
    if (!configured_)
        return false;
    if (frozen_) {
        visible_ = true;
        return true;
    }
    try {
        Input nextA, nextB;
        nextA.endpoint = a_.endpoint;
        nextB.endpoint = b_.endpoint;
        if (!acquire(nextA) || (!heldB_ && !acquire(nextB)))
            return false;
        a_ = std::move(nextA);
        b_ = std::move(nextB);
        visible_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool AnalysisComparison::setFrozen(bool frozen) noexcept {
    if (!broker_ || !broker_->active())
        return false;
    if (frozen == frozen_)
        return true;
    if (!frozen)
        return restart(true);
    const auto current = view();
    if (!validPicture(current.a) || !validPicture(current.b))
        return false;
    try {
        auto nextA = snapshot(current.a);
        std::optional<Snapshot> nextB;
        if (!heldB_)
            nextB = snapshot(current.b);
        frozenA_ = std::move(nextA);
        frozenB_ = std::move(nextB);
        release(a_);
        release(b_);
        frozen_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool AnalysisComparison::holdReference(std::uint64_t expectedBToken) noexcept {
    if (!broker_ || !broker_->active())
        return false;
    if (!visible_ || !expectedBToken || expectedBToken != b_.endpoint.ownerToken)
        return false;
    if (heldB_)
        return true;
    const auto current = view().b;
    if (!validPicture(current))
        return false;
    try {
        auto next = snapshot(current);
        heldB_ = std::move(next);
        frozenB_.reset();
        release(b_);
        return true;
    } catch (...) {
        return false;
    }
}

bool AnalysisComparison::resumeReference() noexcept {
    if (!broker_ || !broker_->active())
        return false;
    if (!heldB_)
        return true;
    // Leaving reference hold also leaves whole-comparison freeze, preserving A's
    // displayed data only until the successful fresh-stream admission begins.
    return restart(false);
}

bool AnalysisComparison::restart(bool retainReference) noexcept {
    if (!configured_ || !broker_ || !broker_->active())
        return false;
    try {
        Input nextA, nextB;
        nextA.endpoint = a_.endpoint;
        nextB.endpoint = b_.endpoint;
        if (visible_ && (!acquire(nextA) || (!(retainReference && heldB_) && !acquire(nextB))))
            return false;
        a_ = std::move(nextA);
        b_ = std::move(nextB);
        if (!retainReference)
            heldB_.reset();
        frozenA_.reset();
        frozenB_.reset();
        frozen_ = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool AnalysisComparison::clear() noexcept { return restart(false); }

void AnalysisComparison::frame() noexcept {
    if (!broker_ || !broker_->active()) {
        release(a_);
        release(b_);
        return;
    }
    if (!visible_ || frozen_ || !configured_)
        return;
    broker_->requestFrame();
    update(a_);
    if (!heldB_)
        update(b_);
}

AnalysisComparison::View AnalysisComparison::view() const noexcept {
    View result;
    result.visible = visible_;
    result.frozen = frozen_;
    result.referenceHeld = heldB_.has_value();
    if (!visible_ || !broker_ || !broker_->active())
        return result;
    result.a = frozenA_ ? frozenA_->picture() : a_.picture();
    result.b = heldB_ ? heldB_->picture() : frozenB_ ? frozenB_->picture() : b_.picture();
    // Storage is validated once on publication/capture. Avoid rescanning every bin
    // merely to obtain a view; pure classify() validates untrusted external pictures.
    if (!result.a.metadata || !result.b.metadata)
        return result;
    const auto &am = *result.a.metadata;
    const auto &bm = *result.b.metadata;
    if (!sameCalibration(am.endpoint.request, bm.endpoint.request) || am.sampleRate != bm.sampleRate ||
        am.coherentGain != bm.coherentGain)
        result.status = Status::Incompatible;
    else
        result.status = heldB_ ? Status::Held : Status::Independent;
    return result;
}

std::size_t AnalysisComparison::capturedBinCount() const noexcept {
    return (frozenA_ ? frozenA_->bins.size() : 0) + (frozenB_ ? frozenB_->bins.size() : 0) +
           (heldB_ ? heldB_->bins.size() : 0);
}

AnalysisComparison::Difference AnalysisComparison::difference(const View &view, std::size_t bin) noexcept {
    if ((view.status != Status::Independent && view.status != Status::Held) ||
        bin >= view.a.binsDbfs.size() || bin >= view.b.binsDbfs.size())
        return {};
    return differenceBetweenLevels(view.a.binsDbfs[bin], view.b.binsDbfs[bin]);
}

std::optional<float> SpectrogramColumnPair::changeDb(std::size_t bin) const noexcept {
    if (!pre || !post || !pre->binsDbfs || !post->binsDbfs ||
        bin >= pre->binsDbfs->size() || bin >= post->binsDbfs->size()) return {};
    return differenceBetweenLevels((*post->binsDbfs)[bin], (*pre->binsDbfs)[bin]).db;
}

std::span<const SpectrogramColumnPair> matchSpectrogramColumns(
    const SharedAnalysis::Result &pre, const SharedAnalysis::Result &post, std::int64_t heardEnd,
    std::uint64_t minimumRevision, std::span<SpectrogramColumnPair> storage,
    std::array<std::int64_t, 2> after) noexcept {
    const auto &a = pre.request, &b = post.request;
    if (storage.empty() || !pre.advancing || !post.advancing ||
        !pre.transportRevision || pre.transportRevision < minimumRevision ||
        pre.transportRevision != post.transportRevision ||
        a.track <= 0 || a.track != b.track || a.device || b.device ||
        a.point != engine::ScopePoint::ChainInput || b.point != engine::ScopePoint::PreFader ||
        !a.heardSpectrogramGrid || !b.heardSpectrogramGrid ||
        !(pre.outputs & post.outputs & SharedAnalysis::Spectrogram) ||
        !sameCalibration(a, b) || a.fftSize < 32 || a.fftSize > SharedAnalysis::kMaxFftSize ||
        a.hopSize <= 0 || a.hopSize > a.fftSize || pre.hopSize != a.hopSize || post.hopSize != a.hopSize ||
        !std::isfinite(pre.sampleRate) || pre.sampleRate <= 0. || pre.sampleRate != post.sampleRate ||
        !std::isfinite(pre.coherentGain) || pre.coherentGain <= 0. || pre.coherentGain != post.coherentGain ||
        heardEnd < std::numeric_limits<std::int64_t>::min() + a.fftSize)
        return {};

    const auto latestStart = heardEnd - a.fftSize;
    const auto bins = static_cast<std::size_t>(a.fftSize / 2 + 1);
    std::size_t ai = pre.columns.size(), bi = post.columns.size(), out = storage.size();
    // Each history is time-ordered within its own transport revision. The
    // source epochs, serials and input sequence numbers describe different taps.
    while (ai > 0 && bi > 0 && out > 0) {
        const auto &left = pre.columns[ai - 1], &right = post.columns[bi - 1];
        if (left.firstStamp > right.firstStamp) { --ai; continue; }
        if (right.firstStamp > left.firstStamp) { --bi; continue; }
        --ai;
        --bi;
        if (left.firstStamp > latestStart || left.firstStamp % a.hopSize != 0 ||
            (after[0] > 0 && left.firstSequence <= after[0] - a.fftSize) ||
            (after[1] > 0 && right.firstSequence <= after[1] - a.fftSize) ||
            !left.binsDbfs || !right.binsDbfs || left.binsDbfs->size() != bins || right.binsDbfs->size() != bins)
            continue;
        storage[--out] = {&left, &right};
    }
    return storage.subspan(out);
}

SpectrogramComparison::SpectrogramComparison(std::shared_ptr<SharedAnalysis> broker) noexcept
    : broker_(std::move(broker)) {}

bool SpectrogramComparison::activate(SharedAnalysis::Request profile) noexcept {
    if (!broker_ || !broker_->active()) return false;
    try {
        Source nextPre, nextPost;
        nextPre.lease = broker_->acquire(profile);
        if (!nextPre.lease) return false;
        auto postProfile = profile;
        postProfile.point = engine::ScopePoint::PreFader;
        nextPost.lease = broker_->acquire(postProfile);
        if (!nextPost.lease) return false;
        nextPre.start = nextPre.lease.sourcePosition();
        nextPost.start = nextPost.lease.sourcePosition();

        auto oldPre = std::move(pre_);
        auto oldPost = std::move(post_);
        pre_ = std::move(nextPre);
        post_ = std::move(nextPost);
        profile_ = profile;
        visible_ = true;
        held_.reset();
        return true;
    } catch (...) {
        return false;
    }
}

bool SpectrogramComparison::show(SharedAnalysis::Request profile) noexcept {
    if (profile.track <= 0 || profile.device || profile.stereoMeter || !broker_ || !broker_->active()) return false;
    profile.point = engine::ScopePoint::ChainInput;
    profile.outputs = SharedAnalysis::Spectrogram;
    profile.heardSpectrogramGrid = true;
    if (profile.hopSize == 0) profile.hopSize = profile.fftSize / 4;
    if (profile == profile_ && (visible_ || held_)) {
        visible_ = true;
        return true;
    }
    return activate(profile);
}

void SpectrogramComparison::release() noexcept {
    auto pre = std::move(pre_.lease);
    auto post = std::move(post_.lease);
    if (!held_) {
        pre_.picture.reset();
        post_.picture.reset();
    }
}

void SpectrogramComparison::hide() noexcept {
    visible_ = false;
    release();
}

void SpectrogramComparison::update(Source &source) noexcept {
    auto picture = source.lease.read();
    if (picture && picture->generation != source.start.generation) {
        source.start = source.lease.sourcePosition();
        picture.reset();
    }
    source.picture = std::move(picture);
}

void SpectrogramComparison::frame() noexcept {
    if (!visible_ || held_ || !broker_ || !broker_->active()) return;
    broker_->requestFrame();
    update(pre_);
    update(post_);
}

bool SpectrogramComparison::freeze(std::int64_t heardEnd, std::uint64_t minimumRevision) noexcept {
    if (held_) return true;
    std::array<SpectrogramColumnPair, 1> candidate;
    if (columns(heardEnd, minimumRevision, candidate).empty()) return false;
    held_ = HeldPosition{heardEnd, minimumRevision};
    release();
    return true;
}

bool SpectrogramComparison::thaw() noexcept {
    if (!held_) return true;
    if (visible_) return activate(profile_);
    held_.reset();
    release();
    return true;
}

std::optional<SpectrogramComparison::Format> SpectrogramComparison::format() const noexcept {
    if (!visible_ || !pre_.picture || !post_.picture) return {};
    const auto &pre = *pre_.picture, &post = *post_.picture;
    return Format{pre.sampleRate, pre.hopSize, pre.generation, post.generation, pre.sourceEpoch, post.sourceEpoch};
}

std::span<const SpectrogramColumnPair> SpectrogramComparison::columns(
    std::int64_t heardEnd, std::uint64_t minimumRevision, std::span<SpectrogramColumnPair> storage) const noexcept {
    if (!visible_ || !broker_ || !broker_->active() || !pre_.picture || !post_.picture) return {};
    if (held_) {
        heardEnd = held_->heardEnd;
        minimumRevision = held_->minimumRevision;
    }
    return matchSpectrogramColumns(*pre_.picture, *post_.picture, heardEnd, minimumRevision, storage,
                                   {pre_.start.written, post_.start.written});
}
} // namespace adi::ui
