// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include "Policy.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>

namespace adi::mix::key_eq {
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
};
namespace {
bool overlap(const float *left, const float *right, std::size_t samples) noexcept {
    const auto a = reinterpret_cast<std::uintptr_t>(left), b = reinterpret_cast<std::uintptr_t>(right);
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
        if (needsBlend)
            candidate->blendDry = std::make_unique<float[]>(static_cast<std::size_t>(p.maximumBlockSize));
        for (std::size_t i = 0; i < settings.bands.size(); ++i) {
            const auto &band = settings.bands[i];
            if (!band.enabled)
                continue;
            auto &stage = candidate->stages[candidate->active++];
            stage.count = cascades[i].count;
            stage.mask = band.placement == Placement::Stereo ? 3U : static_cast<unsigned>(band.placement);
            stage.blend = blends[i];
            for (auto &channel : stage.channels)
                for (std::size_t section = 0; section < stage.count; ++section)
                    if (!channel[section].setCoefficients(cascades[i].sections[section]))
                        return Status::UnsupportedConfiguration;
        }
        impl_.swap(candidate);
        return Status::Ok;
    } catch (const std::bad_alloc &) {
        return Status::AllocationFailed;
    }
}

void Core::reset() noexcept {
    if (impl_)
        for (std::size_t i = 0; i < impl_->active; ++i)
            for (auto &channel : impl_->stages[i].channels)
                for (std::size_t section = 0; section < impl_->stages[i].count; ++section)
                    channel[section].reset();
}
bool Core::isPrepared() const noexcept { return static_cast<bool>(impl_); }
const Settings *Core::settings() const noexcept { return impl_ ? &impl_->accepted : nullptr; }

Status Core::process(float *left, float *right, int frames) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (frames < 0 || frames > impl_->maximumBlockSize ||
        (frames != 0 && (!left || !right || overlap(left, right, static_cast<std::size_t>(frames)))))
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
                    audio[sample] = static_cast<float>(static_cast<double>(dry) +
                                                       static_cast<double>(difference) * stage.blend);
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
} // namespace adi::mix::key_eq
