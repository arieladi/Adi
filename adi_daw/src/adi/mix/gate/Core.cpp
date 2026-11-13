// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include "ControlMapping.hpp"
#include "detail/InclusivePeak.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>

namespace adi::mix::gate {
namespace {
constexpr float epsilon = std::numeric_limits<float>::epsilon();
float f(double x) noexcept { return static_cast<float>(x); }
std::uint64_t nextEpoch() noexcept {
    static std::atomic<std::uint64_t> serial{1};
    auto result = serial.fetch_add(1, std::memory_order_relaxed);
    if (!result)
        result = serial.fetch_add(1, std::memory_order_relaxed);
    return result;
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
bool between(float x, float lo, float hi) noexcept { return std::isfinite(x) && x >= lo && x <= hi; }
Status validate(const Settings &s) noexcept {
    if (s.style < Style::Classic || s.style > Style::Ducking ||
        (s.keySource != KeySource::Internal && s.keySource != KeySource::External) ||
        !between(s.thresholdDb, -60, 0) || !between(s.ratio, 1, 100) || !between(s.rangeDb, 0, 100) ||
        !between(s.attackSeconds, 0, 1) || !between(s.releaseSeconds, 0, 5) || !between(s.holdMs, 0, 250) ||
        !between(s.kneeDb, 0, 30) || !between(s.leftKeyMix, -1, 1) || !between(s.rightKeyMix, -1, 1))
        return Status::InvalidSettings;
    return s.style == Style::Classic ? Status::Ok : Status::UnsupportedConfiguration;
}
Status validate(const PrepareSpec &s) noexcept {
    if (std::fegetround() != FE_TONEAREST)
        return Status::UnsupportedEnvironment;
    if (!std::isfinite(s.sampleRate) || s.sampleRate < 44100 || s.sampleRate > 96000 ||
        s.maximumFrames <= 0 || s.maximumFrames > (1 << 24))
        return Status::InvalidSpec;
    return s.mainChannels == 2 ? Status::Ok : Status::UnsupportedLayout;
}
template <std::size_t N>
float interpolate(float v, const std::array<float, N> &x, const std::array<float, N> &y) noexcept {
    if (v <= x.front())
        return y.front();
    for (std::size_t i = 1; i < N; ++i) {
        if (v <= x[i]) {
            const float t = (v - x[i - 1]) / (x[i] - x[i - 1]);
            return t * (y[i] - y[i - 1]) + y[i - 1];
        }
    }
    return y.back();
}
float timeCurve(float v) noexcept {
    constexpr std::array<float, 7> x{0, .1f, .25f, .5f, .7f, .9f, 1};
    constexpr std::array<float, 7> y{0, .002f, .05f, .16f, .36f, .91f, 1};
    return interpolate(v, x, y);
}
float slotCurve(float v) noexcept {
    constexpr std::array<float, 8> x{0, .1f, .25f, .5f, .7f, .9f, .95f, 1};
    constexpr std::array<float, 8> y{0, .0001f, .003f, .01f, .04f, .2f, .5f, 1};
    return interpolate(v, x, y) * 100.0f;
}
float fourth(float raw) noexcept {
    const double square = static_cast<double>(raw) * raw;
    return f(square * square);
}
float releaseCurve(float raw) noexcept {
    const double a = raw;
    return f(a < .9 ? (a * a) * (a * .6858710562414267) : .5 + 5 * (a - .9));
}
float simpleCoefficient(float seconds, float rate) noexcept {
    const float n = seconds * rate;
    return n > epsilon ? 1.0f - exp32(-2.3025851249694824f / n) : 1.0f;
}
double generalCoefficient(float n) noexcept {
    return n > epsilon ? 1.0 - static_cast<double>(f(std::exp(static_cast<double>(-2.3025851249694824f / n))))
                       : 1.0;
}
struct ExpTable {
    std::array<double, 8193> values{};
    void prepare() noexcept {
        constexpr double increment = 25.0 / 8191.0;
        double x = -25;
        values[0] = std::exp(x);
        for (std::size_t i = 1; i < 8191; ++i) {
            x += increment;
            values[i] = std::exp(x);
        }
        values[8191] = values[8192] = 1;
    }
    float lookup(double x) const noexcept {
        const double position = std::clamp(x, -25.0, 0.0) * (8191.0 / 25.0) + 8191.0;
        const auto index = static_cast<std::uint32_t>(position) & 8191u;
        return f((values[index + 1] - values[index]) * (position - index) + values[index]);
    }
};
const ExpTable &sharedTable() noexcept {
    // Called only during stopped preparation. Audio retains the address and
    // never encounters this static initialization guard.
    static const ExpTable table = [] {
        ExpTable value;
        value.prepare();
        return value;
    }();
    return table;
}
struct EnvelopePlan {
    float attack = 0, release = 0, smoothing = 0, rate = 0;
    float simpleAttack = 1, simpleRelease = 1;
    double generalAttack = 1;
    bool general = false;
    void prepare(float a, float r, float s, float fs) noexcept {
        attack = a;
        release = r;
        smoothing = s;
        general = std::abs(s) > epsilon;
        rate = fs;
        simpleAttack = simpleCoefficient(a, fs);
        simpleRelease = simpleCoefficient(r, fs);
        generalAttack = generalCoefficient(a * fs);
    }
};
struct Envelope {
    double state = 0, releaseCoefficient = 0;
    float previousDelta = 0, cachedLut = 1, previousN = -1;
    float resetInitial = 0;
    bool resetPending = true;
    void requestReset(float initial) noexcept {
        resetInitial = initial;
        resetPending = true;
    }
    float step(float input, const EnvelopePlan &p, const ExpTable &table, std::int32_t offset) noexcept {
        if (resetPending) {
            state = resetInitial;
            resetPending = false;
        }
        const double delta = static_cast<double>(input) - state;
        if (!p.general) {
            state += delta * (delta > 0 ? p.simpleAttack : p.simpleRelease);
        } else if (delta <= -static_cast<double>(epsilon)) {
            float seconds = p.release;
            if (delta > -10) {
                const double tolerance = ((delta * delta) * delta) * delta * static_cast<double>(1e-5f) +
                                         static_cast<double>(4e-5f);
                if ((offset & 15) == 0 || std::abs(static_cast<double>(previousDelta) - delta) > tolerance) {
                    previousDelta = f(delta);
                    cachedLut = table.lookup(delta);
                }
                seconds += cachedLut * p.smoothing;
            }
            const float n = seconds * p.rate;
            if (n != previousN) {
                previousN = n;
                releaseCoefficient = generalCoefficient(n);
            }
            state += delta * releaseCoefficient;
        } else {
            state += delta * p.generalAttack;
        }
        return f(state);
    }
};
struct Detector {
    bool open = false;
    float step(float level, float threshold, float ratio, float knee, float limit) noexcept {
        const float distance = -(level - threshold);
        float effectiveRatio = ratio;
        if (!open) {
            if (distance < 0) {
                open = true;
                effectiveRatio = 1;
            }
        } else {
            effectiveRatio = 1;
            if (7.0f - distance < 0) {
                open = false;
                effectiveRatio = ratio;
            }
        }
        const float half = knee * .5f;
        float v = distance - half;
        if (knee > epsilon && v >= -half && v <= half) {
            const float scale = distance / knee;
            v = (scale * scale) * half;
        }
        return std::clamp(v * (effectiveRatio - 1.0f), 0.0f, limit);
    }
};
template <class T>
bool buffers(T *const *main, const T *const *key, std::int32_t channels, std::int32_t frames) noexcept {
    if (!main || !main[0] || !main[1] || (channels && (!key || !key[0] || !key[1])))
        return false;
    const auto bytes = static_cast<std::uintptr_t>(frames) * sizeof(T);
    const auto valid = [bytes](const T *p) {
        const auto a = reinterpret_cast<std::uintptr_t>(p);
        return a % alignof(T) == 0 && a <= std::numeric_limits<std::uintptr_t>::max() - bytes;
    };
    const auto overlap = [bytes](const T *a, const T *b) {
        const auto x = reinterpret_cast<std::uintptr_t>(a), y = reinterpret_cast<std::uintptr_t>(b);
        return x < y + bytes && y < x + bytes;
    };
    if (!valid(main[0]) || !valid(main[1]) || overlap(main[0], main[1]))
        return false;
    if (channels) {
        if (!valid(key[0]) || !valid(key[1]))
            return false;
        if (key[0] != key[1] && overlap(key[0], key[1]))
            return false;
        for (std::size_t k = 0; k < 2; ++k)
            for (std::size_t m = 0; m < 2; ++m)
                if (key[k] != main[m] && overlap(key[k], main[m]))
                    return false;
    }
    return true;
}
} // namespace

struct Core::Impl {
    PrepareSpec spec{};
    Settings settings{};
    float rate = 0, ratio = 0, knee = 0, limit = 0, initial = 0;
    std::array<std::array<float, 2>, 2> weights{};
    bool commonGain = false;
    const ExpTable *table = nullptr;
    EnvelopePlan prePlan, reductionPlan, gainPlan;
    std::array<Envelope, 2> pre, reduction, gain;
    std::array<Detector, 2> detector;
    std::array<detail::InclusivePeak, 2> hold;
    std::uint64_t epoch = 0, sequence = 0;
    std::int32_t blockFrames = 0, nextOffset = 0;
    int precision = 0;

    void prepare(const PrepareSpec &p, const Settings &s) {
        spec = p;
        settings = s;
        rate = f(p.sampleRate);
        float a = 0, r = 0;
        (void)effectiveAttackSeconds(s.attackSeconds, a);
        (void)effectiveReleaseSeconds(s.releaseSeconds, r);
        const float custom1 = s.holdMs * .004f;
        const float custom2 = s.kneeDb * (1.0f / 30.0f);
        const float slot = slotCurve(.6000000238418579f);
        float nested = 200.0f * slotCurve(.010000000707805157f);
        nested = (s.ratio - 1.0f) * nested;
        nested = (s.ratio - 1.0f) * nested;
        ratio = std::clamp(s.ratio + custom2 * nested, 1.0f, 100.0f);
        knee = std::clamp(custom2 * 30.0f, 0.0f, 30.0f);
        limit = std::clamp(s.rangeDb + 8.0f, 0.0f, 180.0f);
        const float holdMs = std::clamp(50.0f * timeCurve(.15625f) + custom1 * (50.0f * slot), 0.0f, 250.0f);
        const auto holdSamples = static_cast<std::uint32_t>(static_cast<double>(rate * holdMs) * .001);
        table = &sharedTable();
        for (auto &h : hold)
            h.prepare(holdSamples);
        prePlan.prepare(fourth(.0508766733109951f) * 10.0f, releaseCurve(.08999999612569809f) * 10.0f, 0,
                        rate);
        reductionPlan.prepare(a, r + releaseCurve(.15389783680438995f) * 10.0f,
                              std::clamp(r * slot, -1.0f, 1.0f), rate);
        gainPlan.prepare(0, releaseCurve(.24348053336143494f) * 10.0f, std::clamp(custom1, -1.0f, 1.0f),
                         rate);
        const std::array<float, 2> mixes{s.leftKeyMix, s.rightKeyMix};
        for (std::size_t i = 0; i < 2; ++i)
            weights[i] = {1.0f - std::max(mixes[i], 0.0f), 1.0f + std::min(mixes[i], 0.0f)};
        commonGain = weights[0] == weights[1];
        Detector silence;
        initial = 8.0f - silence.step(-180, s.thresholdDb, ratio, knee, limit);
        reset();
    }
    void reset() noexcept {
        for (std::size_t i = 0; i < 2; ++i) {
            pre[i].requestReset(0);
            hold[i].reset();
            detector[i] = {};
            reduction[i].requestReset(initial);
            gain[i].requestReset(std::min(initial, 0.0f));
        }
        epoch = nextEpoch();
        sequence = 0;
        blockFrames = nextOffset = 0;
        precision = 0;
    }
    float gainFor(std::size_t channel, const std::array<float, 2> &held, std::int32_t offset) noexcept {
        const float joined = std::max(weights[channel][0] * held[0], weights[channel][1] * held[1]);
        const float level = std::max(-180.0f, log32(joined) * 8.68588924407959f);
        const float amount = detector[channel].step(level, settings.thresholdDb, ratio, knee, limit);
        const float shaped = reduction[channel].step(8.0f - amount, reductionPlan, *table, offset);
        const float db = gain[channel].step(std::min(shaped, 0.0f), gainPlan, *table, offset);
        return exp32(db * .1151292546497023f);
    }
    template <class T, bool Common>
    void render(std::int32_t offset, std::int32_t frames, T *const *main, const T *const *key,
                std::int32_t keyChannels) noexcept {
        for (std::int32_t i = offset; i < offset + frames; ++i) {
            const std::array<float, 2> audio{f(main[0][i]), f(main[1][i])};
            const std::array<float, 2> input = settings.keySource == KeySource::Internal ? audio
                                               : keyChannels
                                                   ? std::array<float, 2>{f(key[0][i]), f(key[1][i])}
                                                   : std::array<float, 2>{0, 0};
            const std::array<float, 2> held{
                hold[0].push(pre[0].step(std::abs(input[0]), prePlan, *table, i)),
                hold[1].push(pre[1].step(std::abs(input[1]), prePlan, *table, i))};
            if constexpr (Common) {
                // Fixed identical weights and reset states keep the two post-
                // mixer trajectories equal. Key-channel histories stay separate.
                const float multiplier = gainFor(0, held, i);
                main[0][i] = static_cast<T>(audio[0] * multiplier);
                main[1][i] = static_cast<T>(audio[1] * multiplier);
            } else {
                for (std::size_t channel = 0; channel < 2; ++channel)
                    main[channel][i] = static_cast<T>(audio[channel] * gainFor(channel, held, i));
            }
        }
    }
    template <class T>
    Status process(const BlockContext &c, std::int32_t offset, std::int32_t frames, T *const *main,
                   const T *const *key, std::int32_t keyChannels, const Settings &s) noexcept {
        if (const auto status = validate(s); status != Status::Ok)
            return status;
        if (s != settings)
            return Status::UnsupportedConfiguration;
        if (!c.valid() || c.epoch != epoch || c.sequence != sequence || c.frames != blockFrames ||
            offset != nextOffset || offset < 0 || frames <= 0 || frames > blockFrames - offset ||
            (precision != 0 && precision != static_cast<int>(sizeof(T))))
            return Status::InvalidBlock;
        if (keyChannels != 0 && keyChannels != 2)
            return Status::UnsupportedLayout;
        if (!buffers(main, key, keyChannels, blockFrames))
            return Status::InvalidBuffers;
        if (commonGain)
            render<T, true>(offset, frames, main, key, keyChannels);
        else
            render<T, false>(offset, frames, main, key, keyChannels);
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
        candidate->prepare(p, s);
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
    // Capacity does not own audio scratch. A stopped processing restart keeps
    // the prepared graph and its adaptive caches, just as reset does.
    impl_->spec = p;
    impl_->reset();
    return Status::Ok;
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
} // namespace adi::mix::gate
