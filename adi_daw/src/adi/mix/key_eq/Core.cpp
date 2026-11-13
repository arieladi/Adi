// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include "Policy.hpp"
#include "detail/Prototype.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>

namespace adi::mix::key_eq {
namespace {
constexpr float epsilon = std::numeric_limits<float>::epsilon();
float f(double value) noexcept { return static_cast<float>(value); }
struct Ramp {
    float base = 0, target = 0, slope = 0;
    int index = 0, length = 0;
    float publication() const noexcept {
        return index < length ? f(f(f(index) * slope) + base) : target;
    }
    void finish(float value) noexcept {
        base = target = value;
        slope = 0;
        index = length = 0;
    }
    void set(float value, float span, float rate) noexcept {
        if (std::abs(value - target) <= epsilon)
            return;
        base = publication();
        const float u = std::max(f(.30000001192092896f - f(std::abs(f(value - base)) / span)), 0.f);
        const float ms = f(f(f(f(130.f * u) * u) * 11.11111068725586f) + 20.f);
        length = static_cast<int>(f(f(ms * rate) * .0010000000474974513f));
        slope = length ? f(f(value - base) / f(length)) : 0;
        index = 0;
        target = value;
    }
};
struct Event {
    int offset = 0;
    float blend = 1;
    std::uint64_t reserved = 0;
};
static_assert(sizeof(Event) == 16);
struct Moving {
    MovingLane lane = MovingLane::None;
    float rate = 0, span = 0;
    std::array<float, 3> initial{}, designed{};
    bool designedValid = false;
    Ramp ramp{};
    detail::MappedDesigner designer{};
    std::unique_ptr<Event[]> events;
    std::unique_ptr<filters::Coefficients[]> coefficients;
    std::size_t eventCount = 0, eventCursor = 0, eventCapacity = 0, sections = 0;
    std::uint64_t owner = 0, generation = 1, sequence = 0;
    int blockFrames = 0, cursor = 0;
    std::size_t selected() const noexcept { return static_cast<std::size_t>(lane) - 1; }
    void newGeneration() noexcept {
        if (++generation == 0)
            ++generation;
        sequence = 0;
        blockFrames = cursor = 0;
        eventCount = eventCursor = 0;
    }
};
std::uint64_t prepareIdentity() noexcept {
    // Preparation only; running boundary/reset epochs are instance-local.
    static std::atomic<std::uint64_t> serial{1};
    auto result = serial.fetch_add(1, std::memory_order_relaxed);
    if (!result)
        result = serial.fetch_add(1, std::memory_order_relaxed);
    return result;
}
} // namespace
struct Core::Impl {
    struct Stage {
        std::array<std::array<filters::DfiSection, 16>, 2> channels{};
        std::size_t count = 0;
        unsigned mask = 3;
        float blend = 1;
    };
    std::array<Stage, 6> stages{};
    std::size_t active = 0;
    Settings accepted{};
    int maximumBlockSize = 0;
    std::unique_ptr<float[]> blendDry;
    std::unique_ptr<Moving> moving;
};
namespace {
bool overlap(const float *left, const float *right, std::size_t samples) noexcept {
    const auto a = reinterpret_cast<std::uintptr_t>(left),
               b = reinterpret_cast<std::uintptr_t>(right);
    const auto bytes = samples * sizeof(float);
    if (a > std::numeric_limits<std::uintptr_t>::max() - bytes ||
        b > std::numeric_limits<std::uintptr_t>::max() - bytes)
        return true;
    return a < b + bytes && b < a + bytes;
}
bool gainShape(Shape shape) noexcept {
    return shape == Shape::Bell || shape == Shape::LowShelf || shape == Shape::HighShelf ||
           shape == Shape::Tilt || shape == Shape::FlatTilt;
}
} // namespace

Core::Core() noexcept = default;
Core::~Core() = default;
Core::Core(Core &&) noexcept = default;
Core &Core::operator=(Core &&) noexcept = default;

Status Core::prepare(const Preparation &p, const Settings &settings) noexcept {
    if (!std::isfinite(p.sampleRate) || p.sampleRate < 8000 || p.sampleRate > 384000 ||
        p.maximumBlockSize <= 0 || p.maximumBlockSize > 1048576)
        return Status::InvalidArgument;
    if (p.channels != 2)
        return Status::UnsupportedConfiguration;
    if (p.movingLane < MovingLane::None || p.movingLane > MovingLane::Q)
        return Status::InvalidArgument;
    const bool moving = p.movingLane != MovingLane::None;
    if (moving && (p.maximumBlockSize > 4096 || !settings.bands[0].enabled ||
                   settings.bands[0].placement != Placement::Stereo ||
                   (p.movingLane == MovingLane::GainDb && !gainShape(settings.bands[0].shape)) ||
                   std::any_of(settings.bands.begin() + 1, settings.bands.end(),
                               [](const Band &band) { return band.enabled; })))
        return Status::UnsupportedConfiguration;
    std::array<Cascade, 6> cascades{};
    std::array<float, 6> blends{};
    bool needsBlend = false;
    for (std::size_t i = 0; i < settings.bands.size(); ++i) {
        auto band = settings.bands[i];
        if (!validBand(band))
            return Status::InvalidArgument;
        if (!band.enabled)
            continue;
        blends[i] = 1;
        if (gainShape(band.shape) && std::abs(band.gainDb) < .05f) {
            blends[i] = std::abs(band.gainDb) * 20.f;
            needsBlend = true;
            if (std::abs(band.gainDb) < .01f)
                band.gainDb = band.gainDb >= 0 ? .01f : -.01f;
        }
        const auto result = build(p.sampleRate, band, cascades[i]);
        if (result != Status::Ok)
            return result;
    }
    try {
        auto candidate = std::make_unique<Impl>();
        candidate->accepted = settings;
        candidate->maximumBlockSize = p.maximumBlockSize;
        if (needsBlend || (moving && gainShape(settings.bands[0].shape)))
            candidate->blendDry =
                std::make_unique<float[]>(static_cast<std::size_t>(p.maximumBlockSize));
        for (std::size_t i = 0; i < settings.bands.size(); ++i) {
            const auto &band = settings.bands[i];
            if (!band.enabled)
                continue;
            auto &stage = candidate->stages[candidate->active++];
            stage.count = cascades[i].count;
            stage.mask =
                band.placement == Placement::Stereo ? 3U : static_cast<unsigned>(band.placement);
            stage.blend = blends[i];
            for (auto &channel : stage.channels)
                for (std::size_t section = 0; section < stage.count; ++section)
                    if (!channel[section].setCoefficients(cascades[i].sections[section]))
                        return Status::UnsupportedConfiguration;
        }
        if (moving) {
            candidate->moving = std::make_unique<Moving>();
            auto &m = *candidate->moving;
            m.lane = p.movingLane;
            m.rate = f(p.sampleRate);
            m.initial = {f(std::log2(static_cast<double>(settings.bands[0].frequencyHz))),
                         settings.bands[0].gainDb, settings.bands[0].q};
            m.span = m.lane == MovingLane::FrequencyLog2 ? 15.f
                     : m.lane == MovingLane::GainDb      ? 60.f
                                                         : f(40.f - .025f);
            m.ramp.finish(m.initial[m.selected()]);
            m.sections = candidate->stages[0].count;
            if (m.sections == 0 || m.sections > 8)
                return Status::UnsupportedConfiguration;
            m.eventCapacity = (static_cast<std::size_t>(p.maximumBlockSize) + 3) / 4 + 1;
            m.events = std::make_unique<Event[]>(m.eventCapacity);
            m.coefficients =
                std::make_unique<filters::Coefficients[]>(m.eventCapacity * m.sections);
            m.owner = prepareIdentity();
        }
        impl_.swap(candidate);
        return Status::Ok;
    } catch (const std::bad_alloc &) {
        return Status::AllocationFailed;
    }
}

void Core::reset() noexcept {
    if (impl_) {
        for (std::size_t i = 0; i < impl_->active; ++i)
            for (auto &channel : impl_->stages[i].channels)
                for (std::size_t section = 0; section < impl_->stages[i].count; ++section)
                    channel[section].reset();
        if (impl_->moving) {
            auto &m = *impl_->moving;
            m.ramp.finish(m.ramp.target);
            m.designedValid = false;
            m.newGeneration();
        }
    }
}
bool Core::isPrepared() const noexcept { return static_cast<bool>(impl_); }
const Settings *Core::settings() const noexcept { return impl_ ? &impl_->accepted : nullptr; }

Status Core::process(float *left, float *right, int frames) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (impl_->moving)
        return Status::UnsupportedConfiguration;
    if (frames < 0 || frames > impl_->maximumBlockSize ||
        (frames != 0 &&
         (!left || !right || overlap(left, right, static_cast<std::size_t>(frames)))))
        return Status::InvalidArgument;
    if (frames == 0)
        return Status::Ok;
    for (int i = 0; i < frames; ++i) {
        const float l = left[i], r = right[i];
        const float side = (r - l) * .5f;
        left[i] = side + l;
        right[i] = side;
    }
    const std::array<float *, 2> channels{left, right};
    const auto samples = static_cast<std::size_t>(frames);
    for (std::size_t i = 0; i < impl_->active; ++i) {
        auto &stage = impl_->stages[i];
        for (std::size_t axis = 0; axis < channels.size(); ++axis) {
            if ((stage.mask & (1U << axis)) == 0)
                continue;
            auto *audio = channels[axis];
            if (stage.blend != 1)
                std::copy_n(audio, samples, impl_->blendDry.get());
            for (std::size_t section = 0; section < stage.count; ++section)
                stage.channels[axis][section].processInPlace({audio, samples});
            if (stage.blend != 1)
                for (std::size_t sample = 0; sample < samples; ++sample) {
                    const float dry = impl_->blendDry[sample];
                    const float difference = audio[sample] - dry;
                    audio[sample] = static_cast<float>(
                        static_cast<double>(dry) + static_cast<double>(difference) * stage.blend);
                }
        }
    }
    for (int i = 0; i < frames; ++i) {
        const float mid = left[i], side = right[i];
        const float l = mid - side, r = mid + side;
        if (impl_->accepted.audition) {
            left[i] = right[i] = (l + r) * .5f;
        } else {
            left[i] = l;
            right[i] = r;
        }
    }
    return Status::Ok;
}
Status Core::target(float &output) const noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (!impl_->moving)
        return Status::UnsupportedConfiguration;
    output = impl_->moving->ramp.target;
    return Status::Ok;
}
Status Core::beginBlock(const BlockRequest &request, BlockContext &output) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (!impl_->moving)
        return Status::UnsupportedConfiguration;
    auto &m = *impl_->moving;
    if (request.frames <= 0 || request.frames > impl_->maximumBlockSize ||
        m.cursor != m.blockFrames)
        return Status::InvalidBlock;
    if (request.action < BoundaryAction::None || request.action > BoundaryAction::RestoreTarget)
        return Status::InvalidArgument;
    if (request.mappedTarget) {
        if (!detail::validTarget(m.lane, *request.mappedTarget))
            return Status::InvalidArgument;
    }
    auto ramp = m.ramp;
    if (request.action != BoundaryAction::None)
        ramp.finish(request.mappedTarget.value_or(ramp.target));
    else if (request.mappedTarget)
        ramp.set(*request.mappedTarget, m.span, m.rate);
    auto designed = m.designed;
    bool designedValid = m.designedValid && request.action == BoundaryAction::None;
    const int movingSamples = std::min(request.frames, std::max(0, ramp.length - ramp.index));
    const bool constant = movingSamples == 0;
    std::size_t count = 0;
    std::optional<detail::MappedDesigner> stagedDesigner;
    auto values = m.initial;
    values[m.selected()] = constant ? ramp.target : ramp.publication();
    const auto changed = [&designed](const auto &v) {
        return std::abs(v[0] - designed[0]) > epsilon || std::abs(v[1] - designed[1]) > epsilon ||
               std::abs(v[2] - designed[2]) > epsilon;
    };
    // Constant controls avoid scanning the callback and copying prototype state.
    if (!constant || !designedValid || changed(values)) {
        double running = ramp.publication();
        for (int j = 0; j < request.frames; ++j) {
            values[m.selected()] = j < movingSamples ? f(running) : ramp.target;
            const bool update = !designedValid || (j == 0 && constant && changed(values)) ||
                                ((j % 32) == 0 && changed(values)) ||
                                ((j % 4) == 0 && std::abs(values[0] - designed[0]) > .1f);
            if (update) {
                if (count == m.eventCapacity)
                    return Status::UnsupportedConfiguration;
                if (!stagedDesigner)
                    stagedDesigner = m.designer;
                auto band = impl_->accepted.bands[0];
                band.gainDb = values[1];
                band.q = values[2];
                float blend = 1;
                if (gainShape(band.shape) && std::abs(band.gainDb) < .05f) {
                    blend = std::abs(band.gainDb) * 20.f;
                    if (std::abs(band.gainDb) < .01f)
                        band.gainDb = band.gainDb >= 0 ? .01f : -.01f;
                }
                Cascade cascade{};
                const auto status = stagedDesigner->build(m.rate, band, values[0], cascade);
                if (status != Status::Ok)
                    return status;
                if (cascade.count != m.sections)
                    return Status::UnsupportedConfiguration;
                bool same = blend == (count ? m.events[count - 1].blend : impl_->stages[0].blend);
                for (std::size_t k = 0; k < m.sections && same; ++k) {
                    const auto &previous = count ? m.coefficients[(count - 1) * m.sections + k]
                                                 : impl_->stages[0].channels[0][k].coefficients();
                    same = cascade.sections[k] == previous;
                }
                if (!same) {
                    m.events[count] = {j, blend, 0};
                    std::copy_n(cascade.sections.begin(), m.sections,
                                m.coefficients.get() + count * m.sections);
                    ++count;
                }
                designed = values;
                designedValid = true;
            }
            if (j < movingSamples)
                running += ramp.slope;
        }
    }
    ramp.index += movingSamples;
    // No refusal remains: publish the small control state and borrowed plan.
    m.ramp = ramp;
    m.designed = designed;
    m.designedValid = designedValid;
    if (stagedDesigner)
        m.designer = *stagedDesigner;
    if (request.action != BoundaryAction::None) {
        m.newGeneration();
        if (request.action == BoundaryAction::Reset ||
            request.action == BoundaryAction::ReprepareSame)
            for (auto &channel : impl_->stages[0].channels)
                for (std::size_t k = 0; k < m.sections; ++k)
                    channel[k].reset();
    }
    if (++m.sequence == 0)
        ++m.sequence;
    m.blockFrames = request.frames;
    m.cursor = 0;
    m.eventCount = count;
    m.eventCursor = 0;
    output = {m.owner, m.generation, m.sequence, request.frames};
    return Status::Ok;
}
Status Core::process(const BlockContext &context, int offset, int count, float *left,
                     float *right) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (!impl_->moving)
        return Status::UnsupportedConfiguration;
    auto &m = *impl_->moving;
    if (context.owner != m.owner || context.generation != m.generation ||
        context.sequence != m.sequence || context.frames != m.blockFrames || offset != m.cursor ||
        offset < 0 || count <= 0 || count > m.blockFrames - offset)
        return Status::InvalidBlock;
    if (!left || !right || overlap(left, right, static_cast<std::size_t>(m.blockFrames)))
        return Status::InvalidArgument;
    const int end = offset + count;
    for (int i = offset; i < end; ++i) {
        const float l = left[i], r = right[i];
        const float side = (r - l) * .5f;
        left[i] = side + l;
        right[i] = side;
    }
    auto &stage = impl_->stages[0];
    for (int start = offset; start < end;) {
        if (m.eventCursor < m.eventCount && m.events[m.eventCursor].offset == start) {
            const auto *co = m.coefficients.get() + m.eventCursor * m.sections;
            for (auto &channel : stage.channels)
                for (std::size_t k = 0; k < m.sections; ++k)
                    (void)channel[k].setCoefficients(co[k]);
            stage.blend = m.events[m.eventCursor].blend;
            ++m.eventCursor;
        }
        const int finish =
            m.eventCursor < m.eventCount ? std::min(end, m.events[m.eventCursor].offset) : end;
        const auto samples = static_cast<std::size_t>(finish - start);
        for (std::size_t axis = 0; axis < 2; ++axis) {
            auto *audio = (axis == 0 ? left : right) + start;
            if (stage.blend != 1)
                std::copy_n(audio, samples, impl_->blendDry.get());
            for (std::size_t k = 0; k < stage.count; ++k)
                stage.channels[axis][k].processInPlace({audio, samples});
            if (stage.blend != 1)
                for (std::size_t i = 0; i < samples; ++i) {
                    const float dry = impl_->blendDry[i];
                    const float difference = audio[i] - dry;
                    audio[i] =
                        f(static_cast<double>(dry) + static_cast<double>(difference) * stage.blend);
                }
        }
        start = finish;
    }
    for (int i = offset; i < end; ++i) {
        const float mid = left[i], side = right[i];
        const float l = mid - side, r = mid + side;
        if (impl_->accepted.audition)
            left[i] = right[i] = (l + r) * .5f;
        else {
            left[i] = l;
            right[i] = r;
        }
    }
    m.cursor = end;
    return Status::Ok;
}
} // namespace adi::mix::key_eq
