// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include "adi/mix/filters/MatchedSection.hpp"
#include "detail/InclusivePeak.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>

namespace adi::mix::deesser {
namespace {
float f(double x) noexcept { return static_cast<float>(x); }
constexpr double pi = std::numbers::pi;
std::uint64_t nextEpoch() noexcept {
    static std::atomic<std::uint64_t> serial{1};
    auto value = serial.fetch_add(1, std::memory_order_relaxed);
    if (value == 0)
        value = serial.fetch_add(1, std::memory_order_relaxed);
    return value;
}
float exp32(float x) noexcept {
    x = std::clamp(x, -88.37625885009766f, 88.37625885009766f);
    const float n = std::floor(x * 1.4426950216293335f + .5f);
    float r = x - n * .693359375f;
    r -= n * -.00021219444170128554f;
    float y = .00019875691214110702f;
    y = y * r + .001398199936375022f;
    y = y * r + .008333452045917511f;
    y = y * r + .04166579619050026f;
    y = y * r + .1666666567325592f;
    y = y * r + .5f;
    y = ((y * r) * r + r) + 1.0f;
    const auto bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(n) + 127) << 23;
    return y * std::bit_cast<float>(bits);
}
float log32(float x) noexcept {
    auto bits = std::bit_cast<std::uint32_t>(std::max(x, std::numeric_limits<float>::min()));
    float e = static_cast<float>(static_cast<int>(bits >> 23) - 126);
    bits = (bits & 0x007fffffu) | 0x3f000000u;
    const float m = std::bit_cast<float>(bits);
    float r = m - 1.0f;
    if (m < .7071067690849304f) {
        --e;
        r += m;
    }
    float y = .07037683576345444f;
    y = y * r - .11514610052108765f;
    y = y * r + .11676998436450958f;
    y = y * r - .12420140951871872f;
    y = y * r + .14249323308467865f;
    y = y * r - .16668057441711426f;
    y = y * r + .2000071406364441f;
    y = y * r - .24999994039535522f;
    y = y * r + .3333333134651184f;
    y = ((y * r) * r) * r;
    y += e * -.00021219444170128554f;
    y -= (r * r) * .5f;
    y += r;
    return y + e * .693359375f;
}
float blend(float a, float b, float p) noexcept {
    const float weight = (p + 1.0f) * .5f;
    return (1.0f - weight) * a + weight * b;
}
bool unit(float x) noexcept { return std::isfinite(x) && x >= 0 && x <= 1; }
Status validate(const Settings &s) noexcept {
    if ((s.mode != Mode::Allround && s.mode != Mode::SingleVocal) ||
        (s.keySource != KeySource::Internal && s.keySource != KeySource::External) ||
        (s.linkMode != LinkMode::Mid && s.linkMode != LinkMode::Side) ||
        (s.monitor != Monitor::Normal && s.monitor != Monitor::Sidechain && s.monitor != Monitor::Removed) ||
        (s.band != Band::Wide && s.band != Band::Split) ||
        (s.quality != Quality::BaseRate && s.quality != Quality::Times2 && s.quality != Quality::Times4))
        return Status::InvalidSettings;
    if (!std::isfinite(s.thresholdDb) || s.thresholdDb < -60 || s.thresholdDb > 0 ||
        !std::isfinite(s.rangeDb) || s.rangeDb < 0 || s.rangeDb > 24 || !std::isfinite(s.highPassHz) ||
        s.highPassHz < 1999.99f || s.highPassHz > 20000.01f || !std::isfinite(s.lowPassHz) ||
        s.lowPassHz < 1999.99f || s.lowPassHz > 20000.01f || !unit(s.stereoLink) ||
        !std::isfinite(s.inputGain) || s.inputGain < 0 || s.inputGain > 64 || !std::isfinite(s.outputGain) ||
        s.outputGain < 0 || s.outputGain > 64 || !std::isfinite(s.inputPan) || s.inputPan < -1 ||
        s.inputPan > 1 || !std::isfinite(s.outputPan) || s.outputPan < -1 || s.outputPan > 1 ||
        !std::isfinite(s.lookaheadMs) || s.lookaheadMs < 0 || s.lookaheadMs > 15)
        return Status::InvalidSettings;
    if (s.band != Band::Wide || s.quality != Quality::BaseRate || s.lookaheadMs != 0 || s.thresholdDb < -48 ||
        s.thresholdDb == 0 || s.rangeDb == 0 || (s.mode == Mode::SingleVocal && s.stereoLink > .5f))
        return Status::UnsupportedConfiguration;
    return Status::Ok;
}
Status validate(const PrepareSpec &s) noexcept {
    if (!std::isfinite(s.sampleRate) || s.sampleRate < 44100 || s.sampleRate > 96000 ||
        s.maximumFrames <= 0 || s.maximumFrames > (1 << 24))
        return Status::InvalidSpec;
    return s.mainChannels == 2 ? Status::Ok : Status::UnsupportedLayout;
}
std::array<float, 2> gainPan(float gain, float pan) noexcept {
    return {gain - gain * std::max(pan, 0.0f), gain + gain * std::min(pan, 0.0f)};
}

bool cut(filters::Coefficients &output, float logHz, double damping, bool high, float rate) noexcept {
    const float hz = f(std::exp(static_cast<double>(logHz) * std::numbers::ln2));
    const float omega = f(static_cast<double>(hz + hz) * pi / static_cast<double>(rate));
    const double w = omega;
    const auto power = [w, damping, high](double x) {
        const double t = (x / w) * (x / w);
        const double denominator = (1 - t) * (1 - t) + 4 * damping * damping * t;
        return (high ? t * t : 1.0) / denominator;
    };
    double extremum = 0;
    if (damping < std::sqrt(.5)) {
        const double root = std::sqrt(1 - 2 * damping * damping);
        extremum = high ? w / root : w * root;
        const double g = power(extremum);
        if (std::abs(g - 1) <= g * 1e-6 || std::abs(g) <= g * 1e-6)
            extremum = 0;
    }
    filters::MatchRequest request;
    request.angularFrequency = w;
    request.denominator = {1, 2 * damping, 1};
    request.numerator = high ? std::array<double, 3>{1, 0, 0} : std::array<double, 3>{0, 0, 1};
    request.highReference = pi;
    double f1 = w, f2 = 0, f3 = 0;
    if (!high) {
        f3 = std::max(w * .5, extremum);
        f2 = f3 * .5;
    } else {
        if (extremum != 0) {
            float z = f(extremum * .44209706414415373 - .41666666666666663);
            z = f(static_cast<double>(z * z) * .2 + .785);
            if (z > .96)
                z = -.96f;
            request.highReference = std::min(pi, std::max(std::min(extremum, pi), z * pi));
        }
        const float d = f(static_cast<double>(f(damping) * f(damping)) * 4 - 2);
        const double scale = d < 0 ? std::max(.5, std::sqrt(static_cast<double>(d) * -.5)) : .5;
        const double mix = std::clamp(static_cast<double>(f(d * -.5)), 0.0, 1.0);
        const double p = w / ((scale - 1) * mix + 1);
        f1 = std::min({p, pi - std::clamp(p - pi, 0.0, .3 * pi), mix * .3 * pi + .7 * pi});
        const double factor =
            .2 - (d >= 6 ? (std::min(static_cast<double>(d), 20.0) - 6) * .012857142857142859 : 0);
        f2 = .001 * f1;
        f3 = factor * f1;
    }
    request.points = {std::min(f1, 0x1.8a14d57b373dfp+1), std::min(f2, 0x1.69e9565708efcp+1),
                      std::min(f3, 0x1.881c68e4d6f74p+1)};
    return filters::design(request, output) == filters::Status::Ok;
}
bool bell(filters::Coefficients &output, float hp, float lp, float rate, bool &active) noexcept {
    const float width = std::max(0.0f, lp - hp), deficit = std::max(0.0f, 2.0f - width);
    const float gainDb = (deficit * deficit) * 6.25f;
    active = gainDb != 0;
    if (!active)
        return true;
    const float q = std::max(.025f, (gainDb * gainDb) * .024f);
    const float logHz = (hp + lp) * .5f;
    const float hz = f(std::exp(static_cast<double>(logHz) * std::numbers::ln2));
    const double w = f(static_cast<double>(hz + hz) * pi / static_cast<double>(rate));
    const float inverseQ = 1.0f / (q * .7071067690849304f);
    const float linear = f(std::exp(static_cast<double>(gainDb) * std::numbers::ln10 / 20));
    const double a = std::sqrt(linear);
    const float damp = f((static_cast<double>(inverseQ) * .5 / .7071067690849304f) * f(1 / a));
    const double distance = std::clamp(w / pi - .8, 0.0, .2);
    const float bias = f(distance * distance * 25);
    const float mix = std::clamp(f(static_cast<double>(bias) * bias * .25 + damp), .1f, .8f);
    filters::MatchRequest request;
    request.numerator = {1, a * inverseQ, 1};
    request.denominator = {1, inverseQ / a, 1};
    request.angularFrequency = w;
    request.points = {w, (1 - static_cast<double>(mix)) * w, (1 - static_cast<double>(mix) * .05) * w};
    request.highReference = std::max(.9 * pi, std::min(pi, 1.2 * w));
    return filters::design(request, output) == filters::Status::Ok;
}
struct Schmitt {
    float upper = 1, lower = 1;
    bool on = false;
    void configure(float threshold, float hysteresis) noexcept {
        upper = f(std::exp(static_cast<double>(threshold) * std::numbers::ln10 / 20));
        lower = f(std::exp(static_cast<double>(threshold - hysteresis) * std::numbers::ln10 / 20));
    }
    bool process(float value) noexcept {
        if (on) {
            if (std::abs(value) < lower)
                on = false;
        } else if (std::abs(value) > upper)
            on = true;
        return on;
    }
};
template <class T> bool overlaps(const T *a, const T *b, std::size_t bytes) noexcept {
    const auto aa = reinterpret_cast<std::uintptr_t>(a), bb = reinterpret_cast<std::uintptr_t>(b);
    return aa < bb ? bb - aa < bytes : aa - bb < bytes;
}
template <class T>
bool buffers(T *const *main, const T *const *key, std::int32_t keyChannels, std::int32_t frames) noexcept {
    if (!main || !main[0] || !main[1] || (keyChannels != 0 && keyChannels != 2))
        return false;
    const auto bytes = static_cast<std::size_t>(frames) * sizeof(T);
    const auto valid = [bytes](const T *p) {
        const auto address = reinterpret_cast<std::uintptr_t>(p);
        return p && address <= std::numeric_limits<std::uintptr_t>::max() - bytes;
    };
    if (!valid(main[0]) || !valid(main[1]) || overlaps(main[0], main[1], bytes))
        return false;
    if (keyChannels != 0) {
        if (!key || !valid(key[0]) || !valid(key[1]))
            return false;
        for (int c = 0; c < 2; ++c)
            for (int m = 0; m < 2; ++m)
                if (key[c] != main[m] && overlaps(key[c], main[m], bytes))
                    return false;
    }
    return true;
}
} // namespace

struct Core::Impl {
    struct Channel {
        std::array<filters::DfiSection, 5> key;
        std::array<filters::DfiSection, 4> classifier;
        filters::DfiSection smoothing;
        detail::InclusivePeak peak, lowPeak, highPeak;
        Schmitt ratio, level;
        double envelope = 0;
        void reset() noexcept {
            for (auto &section : key)
                section.reset();
            for (auto &section : classifier)
                section.reset();
            smoothing.reset();
            peak.reset();
            lowPeak.reset();
            highPeak.reset();
            envelope = 0;
        }
    };
    PrepareSpec spec;
    Settings settings;
    std::array<Channel, 2> channels;
    std::array<float, 2> inputGain{}, outputGain{};
    std::size_t keySections = 0;
    float rate = 0, keyBlend = 0, wetFade = 0, attack = 0, release = 0, scale = 0;
    bool sideKey = false;
    std::uint64_t epoch = 0, sequence = 0;
    std::int32_t blockFrames = 0, nextOffset = 0;
    int precision = 0;

    Status prepare(const PrepareSpec &p, const Settings &s) {
        spec = p;
        settings = s;
        rate = f(p.sampleRate);
        inputGain = gainPan(s.inputGain, s.inputPan);
        outputGain = gainPan(s.outputGain, s.outputPan);
        const float extra = std::max(s.stereoLink, .5f) - .5f;
        const float a = std::min(s.stereoLink, .5f) - (s.linkMode == LinkMode::Side ? extra : 0);
        keyBlend = 4.0f * a - 1.0f;
        wetFade = 4.0f * extra - 1.0f;
        sideKey = s.linkMode == LinkMode::Side && extra > 0;
        const float hp = f(std::log2(static_cast<double>(s.highPassHz)));
        const float lp = f(std::log2(static_cast<double>(s.lowPassHz)));
        std::array<filters::Coefficients, 5> key{};
        // Low damping precedes high damping; every section publishes float.
        for (const bool high : {true, false}) {
            for (const double damping : {std::cos(3 * pi / 8), std::cos(pi / 8)})
                if (!cut(key[keySections++], high ? hp : lp, damping, high, rate))
                    return Status::UnsupportedConfiguration;
            if (high) {
                bool active = false;
                if (!bell(key[keySections], hp, lp, rate, active))
                    return Status::UnsupportedConfiguration;
                if (active)
                    ++keySections;
            }
        }
        std::array<filters::Coefficients, 4> classifier{};
        filters::Coefficients smoothing;
        if (s.monitor != Monitor::Sidechain) {
            if (!cut(smoothing, f(std::log2(50.0)), std::sqrt(.5), false, rate))
                return Status::UnsupportedConfiguration;
            const float normalized = s.rangeDb / 24.0f, squared = normalized * normalized;
            const float attackSeconds = f(.012 * squared), releaseSeconds = f(.0075 + .0025 * squared);
            attack = 1.0f - exp32(f(-std::numbers::ln10) / (attackSeconds * rate));
            release = 1.0f - exp32(f(-std::numbers::ln10) / (releaseSeconds * rate));
            scale = s.rangeDb / s.thresholdDb;
            if (s.mode == Mode::SingleVocal) {
                const float fc = std::min(hp, f(std::log2(10000.0)));
                const float fa =
                    f(std::exp(static_cast<double>(fc - 1.5849618911743164f) * std::numbers::ln2));
                const float fb =
                    f(std::exp(static_cast<double>(fc + .6780729293823242f) * std::numbers::ln2));
                const float center = f(std::exp(static_cast<double>(fc) * std::numbers::ln2));
                const std::array<float, 4> frequencies{200, fa, center, fb};
                for (std::size_t i = 0; i < classifier.size(); ++i)
                    if (!cut(classifier[i], f(std::log2(static_cast<double>(frequencies[i]))), std::sqrt(.5),
                             i % 2 == 0, rate))
                        return Status::UnsupportedConfiguration;
            }
        }
        const auto hold = [this](float milliseconds) {
            return static_cast<std::uint32_t>(static_cast<double>(rate * milliseconds) * .001);
        };
        for (auto &channel : channels) {
            for (std::size_t i = 0; i < keySections; ++i)
                (void)channel.key[i].setCoefficients(key[i]);
            if (s.monitor == Monitor::Sidechain)
                continue;
            (void)channel.smoothing.setCoefficients(smoothing);
            channel.peak.prepare(hold(5));
            if (s.mode != Mode::SingleVocal)
                continue;
            for (std::size_t i = 0; i < classifier.size(); ++i)
                (void)channel.classifier[i].setCoefficients(classifier[i]);
            channel.lowPeak.prepare(hold(3));
            channel.highPeak.prepare(hold(3));
            channel.ratio.configure(0, 7);
            channel.level.configure(s.thresholdDb, 12);
        }
        epoch = nextEpoch();
        return Status::Ok;
    }
    void reset() noexcept {
        for (auto &channel : channels)
            channel.reset();
        epoch = nextEpoch();
        sequence = 0;
        blockFrames = nextOffset = 0;
        precision = 0;
    }
    template <bool Vocal> float gain(Channel &c, float key, float filtered) noexcept {
        float level =
            std::max(-180.0f, log32(std::max(c.peak.push(std::abs(filtered)), 1e-10f)) * 8.68588924407959f);
        if constexpr (Vocal) {
            const float a = c.lowPeak.push(std::abs(c.classifier[1].process(c.classifier[0].process(key))));
            const float b = c.highPeak.push(std::abs(c.classifier[3].process(c.classifier[2].process(key))));
            const float ratio = (b * .375f) / std::max(a, 1e-6f);
            const bool first = c.ratio.process(ratio), second = c.level.process(std::max(a, b));
            if (!first || !second)
                level = -180;
        }
        const float target = std::abs(
            c.smoothing.process(std::clamp(level - settings.thresholdDb, 0.0f, -settings.thresholdDb)));
        c.envelope += (static_cast<double>(target) - c.envelope) * (target > c.envelope ? attack : release);
        return exp32((f(c.envelope) * scale) * .1151292546497023f);
    }
    template <class T, bool Vocal, Monitor Monitoring>
    void render(std::int32_t offset, std::int32_t frames, T *const *main, const T *const *key,
                std::int32_t keyChannels) noexcept {
        for (std::int32_t i = offset; i < offset + frames; ++i) {
            const std::array<float, 2> programme{f(main[0][i]) * inputGain[0], f(main[1][i]) * inputGain[1]};
            // Read both key planes before either programme plane can be written.
            const std::array<float, 2> detector =
                settings.keySource == KeySource::Internal
                    ? programme
                    : (keyChannels == 0 ? std::array<float, 2>{0, 0}
                                        : std::array<float, 2>{f(key[0][i]), f(key[1][i])});
            const float side = (programme[1] - programme[0]) * .5f, mid = side + programme[0];
            for (std::size_t ch = 0; ch < 2; ++ch) {
                const float keySide = (detector[1 - ch] - detector[ch]) * .5f;
                const float keyMid = keySide + detector[ch];
                const float selected = blend(sideKey ? keySide : detector[ch], keyMid, keyBlend);
                auto &channel = channels[ch];
                float filtered = selected;
                for (std::size_t j = 0; j < keySections; ++j)
                    filtered = channel.key[j].process(filtered);
                if constexpr (Monitoring == Monitor::Sidechain)
                    main[ch][i] = static_cast<T>(filtered);
                else {
                    const float g = gain<Vocal>(channel, selected, filtered);
                    const float gm = blend(g, 1, settings.linkMode == LinkMode::Side ? wetFade : -1.0f);
                    const float gs = blend(g, 1, settings.linkMode == LinkMode::Mid ? wetFade : -1.0f);
                    const float m = mid * gm, s = side * gs;
                    const float wet = ch == 0 ? m - s : s + m;
                    if constexpr (Monitoring == Monitor::Removed)
                        main[ch][i] = static_cast<T>(programme[ch] - wet);
                    else
                        main[ch][i] = static_cast<T>(wet * outputGain[ch]);
                }
            }
        }
    }
    template <class T>
    Status process(const BlockContext &context, std::int32_t offset, std::int32_t frames, T *const *main,
                   const T *const *key, std::int32_t keyChannels, const Settings &s) noexcept {
        const auto validation = validate(s);
        if (validation != Status::Ok)
            return validation;
        if (s != settings)
            return Status::UnsupportedConfiguration;
        if (context.epoch != epoch || context.sequence != sequence || context.frames != blockFrames ||
            !context.valid() || offset != nextOffset || frames <= 0 || offset < 0 ||
            frames > blockFrames - offset || (precision != 0 && precision != static_cast<int>(sizeof(T))))
            return Status::InvalidBlock;
        if (keyChannels != 0 && keyChannels != 2)
            return Status::UnsupportedLayout;
        if (!buffers(main, key, keyChannels, blockFrames))
            return Status::InvalidBuffers;
        if (settings.monitor == Monitor::Sidechain)
            render<T, false, Monitor::Sidechain>(offset, frames, main, key, keyChannels);
        else if (settings.mode == Mode::SingleVocal) {
            if (settings.monitor == Monitor::Removed)
                render<T, true, Monitor::Removed>(offset, frames, main, key, keyChannels);
            else
                render<T, true, Monitor::Normal>(offset, frames, main, key, keyChannels);
        } else {
            if (settings.monitor == Monitor::Removed)
                render<T, false, Monitor::Removed>(offset, frames, main, key, keyChannels);
            else
                render<T, false, Monitor::Normal>(offset, frames, main, key, keyChannels);
        }
        precision = static_cast<int>(sizeof(T));
        nextOffset += frames;
        return Status::Ok;
    }
};

Core::Core() noexcept = default;
Core::~Core() = default;
Status Core::prepare(const PrepareSpec &p, const Settings &s) noexcept {
    if (const auto status = validate(p); status != Status::Ok)
        return status;
    if (const auto status = validate(s); status != Status::Ok)
        return status;
    try {
        auto candidate = std::make_unique<Impl>();
        if (const auto status = candidate->prepare(p, s); status != Status::Ok)
            return status;
        impl_.swap(candidate);
        return Status::Ok;
    } catch (...) {
        return Status::AllocationFailed;
    }
}
Status Core::reprepare(const PrepareSpec &p) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (const auto status = validate(p); status != Status::Ok)
        return status;
    if (f(p.sampleRate) != impl_->rate || p.mainChannels != impl_->spec.mainChannels)
        return Status::UnsupportedConfiguration;
    try {
        auto candidate = std::make_unique<Impl>();
        if (const auto status = candidate->prepare(p, impl_->settings); status != Status::Ok)
            return status;
        for (std::size_t i = 0; i < 2; ++i) {
            candidate->channels[i].ratio.on = impl_->channels[i].ratio.on;
            candidate->channels[i].level.on = impl_->channels[i].level.on;
        }
        impl_.swap(candidate);
        return Status::Ok;
    } catch (...) {
        return Status::AllocationFailed;
    }
}
void Core::reset() noexcept {
    if (impl_)
        impl_->reset();
}
Status Core::restore(const Settings &s) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (const auto status = validate(s); status != Status::Ok)
        return status;
    return s == impl_->settings ? Status::Ok : Status::UnsupportedConfiguration;
}
bool Core::isPrepared() const noexcept { return static_cast<bool>(impl_); }
BlockContext Core::beginBlock(std::int32_t frames) noexcept {
    if (!impl_ || frames <= 0 || frames > impl_->spec.maximumFrames ||
        impl_->nextOffset != impl_->blockFrames)
        return {};
    if (++impl_->sequence == 0)
        ++impl_->sequence;
    impl_->blockFrames = frames;
    impl_->nextOffset = 0;
    return {impl_->epoch, impl_->sequence, frames};
}
Status Core::process(const BlockContext &c, std::int32_t offset, std::int32_t frames, float *const *main,
                     const float *const *key, std::int32_t keyChannels, const Settings &s) noexcept {
    return impl_ ? impl_->process(c, offset, frames, main, key, keyChannels, s) : Status::NotPrepared;
}
Status Core::process(const BlockContext &c, std::int32_t offset, std::int32_t frames, double *const *main,
                     const double *const *key, std::int32_t keyChannels, const Settings &s) noexcept {
    return impl_ ? impl_->process(c, offset, frames, main, key, keyChannels, s) : Status::NotPrepared;
}
} // namespace adi::mix::deesser
