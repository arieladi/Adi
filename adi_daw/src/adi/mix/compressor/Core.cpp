// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include "ControlMapping.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace adi::mix::compressor {
namespace {
constexpr std::size_t tableSize = 8193;
constexpr double gridIntervals = 8191.0;
constexpr float epsilon = std::numeric_limits<float>::epsilon();
std::atomic<std::uint64_t> nextEpoch{1};

bool unit(float x) noexcept { return std::isfinite(x) && x >= 0.0f && x <= 1.0f; }

double coefficient(double samples) noexcept {
    return samples <= static_cast<double>(epsilon)
               ? 1.0
               : 1.0 - std::exp(static_cast<double>(-2.3025851249694824f) /
                                static_cast<double>(static_cast<float>(samples)));
}

struct CoefficientCache {
    struct Entry {
        float samples = -1;
        double value = 1;
    };
    std::array<Entry, 4> entries{};
    std::size_t cursor = 0;
    double get(double next) noexcept {
        const float rounded = static_cast<float>(next);
        for (const auto &entry : entries)
            if (entry.samples == rounded)
                return entry.value;
        const double value = coefficient(next);
        entries[cursor] = {rounded, value};
        cursor = (cursor + 1) & 3U;
        return value;
    }
};

float logAmplitude(float x) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(std::max(x, std::numeric_limits<float>::min()));
    float exponent = static_cast<float>(static_cast<int>(bits >> 23U) - 126);
    const float mantissa = std::bit_cast<float>((bits & 0x807fffffU) | 0x3f000000U);
    float r = mantissa - 1.0f;
    if (mantissa < 0.7071067690849304f) {
        exponent -= 1.0f;
        r += mantissa;
    }
    float y = 0.07037683576345444f;
    for (const float c :
         {-0.11514610052108765f, 0.11676998436450958f, -0.12420140951871872f, 0.14249323308467865f,
          -0.16668057441711426f, 0.2000071406364441f, -0.24999994039535522f, 0.3333333134651184f})
        y = y * r + c;
    y = ((y * r) * r) * r;
    y += exponent * -0.00021219444170128554f;
    y -= (r * r) * 0.5f;
    y += r;
    return y + exponent * 0.693359375f;
}

float amplitudeFromLog(float x) noexcept {
    x = std::clamp(x, -88.37625885009766f, 88.37625885009766f);
    const int n = static_cast<int>(std::floor(x * 1.4426950216293335f + 0.5f));
    const float nf = static_cast<float>(n);
    float r = x - nf * 0.693359375f;
    r -= nf * -0.00021219444170128554f;
    float y = 0.00019875691214110702f;
    for (const float c :
         {0.001398199936375022f, 0.008333452045917511f, 0.04166579619050026f, 0.1666666567325592f, 0.5f})
        y = y * r + c;
    y = y * (r * r) + r;
    y += 1.0f;
    return y * std::bit_cast<float>(static_cast<std::uint32_t>(n + 127) << 23U);
}

struct Curve {
    double ax, bx, cx, ay, by, cy, x1, y1, x2, y2;
    Curve(float a, float b, float c, float d) noexcept
        : ax(0), bx(0), cx(3.0 * a), ay(0), by(0), cy(3.0 * b), x1(a), y1(b), x2(c), y2(d) {
        bx = 3.0 * (x2 - x1) - cx;
        ax = (1.0 - cx) - bx;
        by = 3.0 * (y2 - y1) - cy;
        ay = (1.0 - cy) - by;
    }
    double x(double t) const noexcept { return ((t * t) * t * ax + t * t * bx) + t * cx; }
    double y(double t) const noexcept { return ((t * t) * t * ay + t * t * by) + t * cy; }
    double value(double input) const noexcept {
        if (input < 0.0)
            return x1 != 0.0 ? input * (y1 / x1) : 0.0;
        if (input > 1.0)
            return (input - 1.0) * ((1.0 - y2) / (1.0 - x2)) + 1.0;
        if (input <= std::numeric_limits<double>::epsilon())
            return 0.0;
        if (input >= 1.0 - std::numeric_limits<double>::epsilon())
            return 1.0;
        double lo = 0.0, hi = 1.0, t = 0.5;
        for (;;) {
            if (std::abs(y(hi) - y(lo)) <= 0.0001)
                return y(t);
            const double v = x(t);
            if (v == input)
                return y(t);
            if (input > v)
                lo = t;
            else
                hi = t;
            t = (hi + lo) * 0.5;
        }
    }
};

template <std::size_t N>
float piece(float v, const std::array<float, N> &x, const std::array<float, N> &y) noexcept {
    for (std::size_t i = 1; i < N; ++i)
        if (v <= x[i])
            return ((v - x[i - 1]) / (x[i] - x[i - 1])) * (y[i] - y[i - 1]) + y[i - 1];
    return 1.0f;
}

bool sameRouting(const Settings &a, const Settings &b) noexcept {
    return a.keySource == b.keySource && a.stereoLink == b.stereoLink && a.linkMode == b.linkMode;
}

struct Span {
    std::uintptr_t begin = 0, end = 0;
};
template <class T> bool span(const T *p, std::int32_t frames, Span &s) noexcept {
    if (p == nullptr)
        return false;
    s.begin = reinterpret_cast<std::uintptr_t>(p);
    const auto bytes = static_cast<std::uintptr_t>(frames) * sizeof(T);
    if (bytes > std::numeric_limits<std::uintptr_t>::max() - s.begin)
        return false;
    s.end = s.begin + bytes;
    return true;
}
bool overlaps(const Span &a, const Span &b) noexcept { return a.begin < b.end && b.begin < a.end; }
} // namespace

bool validSettings(const Settings &s) noexcept {
    return std::isfinite(s.thresholdDb) && s.thresholdDb >= -60.0f && s.thresholdDb <= 0.0f &&
           std::isfinite(s.ratio) && s.ratio >= 1.0f && s.ratio <= 100.0f && std::isfinite(s.kneeDb) &&
           s.kneeDb >= 0.0f && s.kneeDb <= 72.0f && std::isfinite(s.rangeDb) && s.rangeDb >= 0.0f &&
           s.rangeDb <= 60.0f && unit(s.attack) && unit(s.release) && std::isfinite(s.stereoLink) &&
           s.stereoLink >= 0.0f && s.stereoLink <= 2.0f &&
           (s.keySource == KeySource::Internal || s.keySource == KeySource::External) &&
           (s.linkMode == LinkMode::Mid || s.linkMode == LinkMode::Side ||
            s.linkMode == LinkMode::MidToSide || s.linkMode == LinkMode::SideToMid);
}

std::optional<ManualTimes> manualTimes(float attack, float release) noexcept {
    if (!unit(attack) || !unit(release))
        return std::nullopt;
    const double an = attack;
    const double a = static_cast<float>(an * an * an);
    const double r =
        static_cast<float>(std::tan(static_cast<double>(release * release) * 1.2) * 0.38877956936820496);
    const double seconds = 0.0001 + 0.2499 * a;
    const double coupling =
        (0.9 * std::sqrt((std::clamp(seconds, 0.0002, 0.25) - 0.0002) / (0.25 - 0.0002))) * 0.09;
    return ManualTimes{seconds, std::max(0.0025, 0.025 + 1.975 * r - coupling)};
}

std::optional<float> ratioFromNormalized(float n) noexcept {
    if (!unit(n))
        return std::nullopt;
    constexpr std::array<float, 11> x1{0, .1f, .2f, .3f, .4f, .5f, .6f, .7f, .8f, .9f, 1};
    constexpr std::array<float, 11> y1{0,
                                       static_cast<float>(0.1 / 99.0),
                                       static_cast<float>(0.25 / 99.0),
                                       static_cast<float>(0.5 / 99.0),
                                       static_cast<float>(1.0 / 99.0),
                                       static_cast<float>(1.75 / 99.0),
                                       static_cast<float>(3.0 / 99.0),
                                       static_cast<float>(5.0 / 99.0),
                                       static_cast<float>(7.0 / 99.0),
                                       static_cast<float>(9.0 / 99.0),
                                       1};
    constexpr std::array<float, 7> x2{0, .1f, .25f, .5f, .7f, .9f, 1};
    constexpr std::array<float, 7> y2{0, .002f, .05f, .16f, .36f, .91f, 1};
    float v = n <= 0.0f ? 0.0f : piece(n, x1, y1);
    constexpr float pivot = static_cast<float>(1.0 / 11.0);
    if (v > pivot) {
        float q = (v - pivot) * 1.100000023841858f;
        q = q > 0.0f ? piece(q, x2, y2) : 0.0f;
        v = q * (1.0f - pivot) + pivot;
    }
    return v * 99.0f + 1.0f;
}

struct Core::Impl {
    PrepareSpec spec;
    Settings routing, controls;
    std::array<double, tableSize> attackTable{}, kneeTable{}, releaseTable{};
    std::array<double, 2> envelope{}, slow{};
    CoefficientCache coefficientCache;
    double attackSamples = 0, slowUp = 0, slowDown = 0, releaseDown = 0;
    double ratioScale = 0;
    double kneeAt100 = 0, kneeSlope = 0;
    float makeup = 1;
    bool controlsInitialized = false;
    std::uint64_t epoch = 0, sequence = 0;
    std::int32_t blockFrames = 0, cursor = 0;

    explicit Impl(const PrepareSpec &p, const Settings &s) : spec(p), routing(s), controls(s) {
        // The processing-rate domain is float, even though the owner accepts a
        // double host rate. This also makes fractional-rate behavior explicit.
        spec.sampleRate = static_cast<double>(static_cast<float>(p.sampleRate));
        const Curve kneeCurve(.093f, .287f, .52f, .822f), releaseCurve(0, .372f, .492f, .304f);
        const auto kneeValue = [&kneeCurve](double x) {
            const double d = std::max(0.0, x + 11.0);
            return (kneeCurve.value(d * (1.0 / 48.0)) * d) * static_cast<double>(.83f);
        };
        double a = -25.0, k = -20.0, r = 0;
        for (std::size_t i = 0; i < tableSize - 2; ++i) {
            attackTable[i] = std::exp(a);
            kneeTable[i] = kneeValue(k);
            releaseTable[i] = releaseCurve.value(r);
            a += 25.0 / gridIntervals;
            k += 120.0 / gridIntervals;
            r += 4.0 / gridIntervals;
        }
        attackTable[8191] = attackTable[8192] = 1;
        kneeTable[8191] = kneeTable[8192] = kneeValue(100);
        releaseTable[8191] = releaseTable[8192] = releaseCurve.value(4);
        kneeAt100 = kneeLookup(100);
        kneeSlope = kneeAt100 - kneeLookup(99);
        update(s);
        clear();
    }

    void clear() noexcept {
        envelope = {};
        slow = {};
        coefficientCache = {};
        blockFrames = cursor = 0;
        sequence = 0;
        epoch = nextEpoch.fetch_add(1, std::memory_order_relaxed);
        if (epoch == 0)
            epoch = nextEpoch.fetch_add(1, std::memory_order_relaxed);
    }
    static double lookup(const std::array<double, tableSize> &table, double p) noexcept {
        const auto j = static_cast<std::uint32_t>(p) & 8191U;
        return (table[j + 1] - table[j]) * (p - static_cast<double>(j)) + table[j];
    }
    double kneeLookup(double x) const noexcept {
        return lookup(kneeTable, x * (1.0 / (120.0 / gridIntervals)) - (-20.0 * gridIntervals / 120.0));
    }
    double spline(double x) const noexcept {
        if (x <= -20)
            return 0;
        if (x >= 100)
            return (x - 100) * kneeSlope + kneeAt100;
        return kneeLookup(x);
    }
    double knee(double x, float width) const noexcept {
        if (width < epsilon)
            return x;
        if (std::abs(width - 24.0f) <= epsilon)
            return spline(x);
        const float half = width * .5f, xf = static_cast<float>(x);
        double quadratic = xf;
        if (xf >= -half && xf <= half) {
            const float t = (half + xf) / width;
            quadratic = (t * t) * half;
        }
        float f = width * .013888888992369175f;
        if (f < .3333333432674408f) {
            f *= 3;
            const double z = spline(x / static_cast<double>(f)) * static_cast<double>(f);
            return (z - quadratic) * static_cast<float>(static_cast<double>(f) * f) + quadratic;
        }
        const double z = spline(x);
        f = (f - .3333333432674408f) * 1.5f;
        return static_cast<double>(f) * (quadratic - z) + z;
    }
    void update(const Settings &s) noexcept {
        if (!controlsInitialized || s.attack != controls.attack || s.release != controls.release) {
            const auto times = manualTimes(s.attack, s.release).value();
            attackSamples = times.attackSeconds * spec.sampleRate;
            const double releaseSamples = times.releaseSeconds * spec.sampleRate;
            slowUp = coefficient(100.0 * attackSamples + 2.0 * spec.sampleRate);
            slowDown = coefficient(releaseSamples + .2 * spec.sampleRate);
            releaseDown = coefficient(releaseSamples);
        }
        if (!controlsInitialized || s.ratio != controls.ratio)
            ratioScale = 1.0 - 1.0 / static_cast<double>(s.ratio);
        const bool makeupChanged = !controlsInitialized || s.autoGain != controls.autoGain ||
                                   s.thresholdDb != controls.thresholdDb || s.ratio != controls.ratio ||
                                   s.kneeDb != controls.kneeDb || s.rangeDb != controls.rangeDb ||
                                   s.attack != controls.attack;
        if (!s.autoGain)
            makeup = 1;
        else if (makeupChanged) {
            const float z = static_cast<float>(knee(static_cast<double>(0.0f - s.thresholdDb), s.kneeDb));
            const float reduction = std::clamp(
                static_cast<float>(static_cast<double>(z) * (1.0 - 1.0 / static_cast<double>(s.ratio))), 0.0f,
                s.rangeDb);
            const float exponent = -1.0f - (s.attack * s.attack) * 5.0f;
            const float db =
                static_cast<float>(static_cast<double>(reduction) * std::exp(static_cast<double>(exponent)));
            makeup = static_cast<float>(std::exp(static_cast<double>(db) * .1151292546497023));
        }
        controls = s;
        controlsInitialized = true;
    }
    float gain(float detector, std::size_t channel) noexcept {
        const float db = std::max(-180.0f, logAmplitude(std::max(detector, 1e-10f)) * 8.68588924407959f);
        double shaped =
            knee(static_cast<double>(db) - static_cast<double>(controls.thresholdDb), controls.kneeDb);
        if (controls.ratio < 100.0f)
            shaped *= ratioScale;
        const double target = std::clamp(static_cast<float>(shaped), 0.0f, controls.rangeDb);
        double down = slowDown, release = releaseDown;
        auto &y = envelope[channel];
        auto &u = slow[channel];
        if (controls.autoRelease) {
            const double v = lookup(releaseTable, (y * (1.0 / 48.0)) * (1.0 / (4.0 / gridIntervals)));
            const double duration = ((v * .4) + .05) * ((static_cast<double>(controls.release) * 1.5) + .25);
            const double samples = duration * spec.sampleRate;
            down = coefficientCache.get(samples + .2 * spec.sampleRate);
            release = coefficientCache.get(samples);
        }
        u += (target > u ? slowUp : down) * (target - u);
        const double m = std::max(u, target);
        double c = release;
        if (m > y) {
            const double e =
                lookup(attackTable, std::max(-25.0, y - m) * (gridIntervals / 25.0) + gridIntervals);
            c = coefficientCache.get(attackSamples + (.05 * spec.sampleRate - .2 * attackSamples) * e);
        }
        y += c * (m - y);
        return amplitudeFromLog(-static_cast<float>(y) * .115129254758358f);
    }

    template <class T>
    Status process(const BlockContext &context, std::int32_t offset, std::int32_t frames, T *const *main,
                   const T *const *key, std::int32_t keyChannels, const Settings &s) noexcept {
        if (!validSettings(s))
            return Status::InvalidSettings;
        if (!sameRouting(s, routing) || (s.autoGain && s.stereoLink > 1.0f))
            return Status::UnsupportedConfiguration;
        if (!context.valid() || context.epoch != epoch || context.sequence != sequence ||
            context.frames != blockFrames || offset != cursor || frames <= 0 || frames > blockFrames - cursor)
            return Status::InvalidBlock;
        if (keyChannels < 0 || keyChannels > 2)
            return Status::UnsupportedLayout;
        std::array<Span, 2> mainSpans{}, keySpans{};
        if (main == nullptr)
            return Status::InvalidBuffers;
        for (std::int32_t ch = 0; ch < spec.mainChannels; ++ch)
            if (!span(main[ch], blockFrames, mainSpans[static_cast<std::size_t>(ch)]))
                return Status::InvalidBuffers;
        if (spec.mainChannels == 2 && overlaps(mainSpans[0], mainSpans[1]))
            return Status::InvalidBuffers;
        if (keyChannels > 0 && key == nullptr)
            return Status::InvalidBuffers;
        for (std::int32_t ch = 0; ch < keyChannels; ++ch) {
            auto &k = keySpans[static_cast<std::size_t>(ch)];
            if (!span(key[ch], blockFrames, k))
                return Status::InvalidBuffers;
            for (std::int32_t m = 0; m < spec.mainChannels; ++m) {
                const auto &p = mainSpans[static_cast<std::size_t>(m)];
                if (overlaps(k, p) && k.begin != p.begin)
                    return Status::InvalidBuffers;
            }
        }
        if (s != controls)
            update(s);
        const bool stereo = spec.mainChannels == 2;
        for (std::int32_t i = offset; i < offset + frames; ++i) {
            const float left = static_cast<float>(main[0][i]);
            const float right = stereo ? static_cast<float>(main[1][i]) : left;
            float kl = left, kr = right;
            if (s.keySource == KeySource::External) {
                kl = keyChannels > 0 ? static_cast<float>(key[0][i]) : 0;
                kr = keyChannels > 1 ? static_cast<float>(key[1][i]) : kl;
            }
            const float al = std::abs(kl), ar = std::abs(kr), maximum = std::max(al, ar);
            float dl = al + (maximum - al) * s.stereoLink;
            float dr = ar + (maximum - ar) * s.stereoLink;
            if (s.stereoLink > 1.0f) {
                const bool side = s.linkMode == LinkMode::Side || s.linkMode == LinkMode::SideToMid;
                const float joined = side ? kl - kr : kl + kr;
                dl = maximum + (std::abs(joined * .5f) - maximum) * (s.stereoLink - 1.0f);
                dr = dl;
            }
            // Prepared routing cannot change. At and above full linking both
            // detectors start together and remain identical for this stream.
            const float gl = gain(dl, 0), gr = s.stereoLink >= 1.0f ? gl : gain(dr, 1);
            float outL = left * gl, outR = right * gr;
            if (s.stereoLink > 1.0f) {
                // This fixed routing uses the shared linked detector gain.
                const float other = gr + (1.0f - gr) * (s.stereoLink - 1.0f);
                const bool side = s.linkMode == LinkMode::Side || s.linkMode == LinkMode::MidToSide;
                const float mid = ((left + right) * .5f) * (side ? other : gr);
                const float difference = ((left - right) * .5f) * (side ? gr : other);
                outL = mid + difference;
                outR = mid - difference;
            }
            if (s.autoGain) {
                outL *= makeup;
                outR *= makeup;
            }
            main[0][i] = static_cast<T>(outL);
            if (stereo)
                main[1][i] = static_cast<T>(outR);
        }
        cursor += frames;
        return Status::Ok;
    }
};

Core::Core() noexcept = default;
Core::~Core() = default;
Status Core::prepare(const PrepareSpec &p, const Settings &s) noexcept {
    if (!std::isfinite(p.sampleRate) || p.sampleRate < 8000 || p.sampleRate > 384000 ||
        p.maximumFrames <= 0 || p.maximumFrames > 1048576)
        return Status::InvalidSpec;
    if (p.mainChannels != 1 && p.mainChannels != 2)
        return Status::UnsupportedLayout;
    if (!validSettings(s))
        return Status::InvalidSettings;
    if ((p.mainChannels == 1 && s.stereoLink != 1.0f) || (s.autoGain && s.stereoLink > 1.0f))
        return Status::UnsupportedConfiguration;
    try {
        auto replacement = std::make_unique<Impl>(p, s);
        impl_.swap(replacement);
        return Status::Ok;
    } catch (...) {
        return Status::AllocationFailed;
    }
}
void Core::reset() noexcept {
    if (impl_)
        impl_->clear();
}
bool Core::isPrepared() const noexcept { return impl_ != nullptr; }
BlockContext Core::beginBlock(std::int32_t frames) noexcept {
    if (!impl_ || frames <= 0 || frames > impl_->spec.maximumFrames || impl_->cursor != impl_->blockFrames)
        return {};
    impl_->blockFrames = frames;
    impl_->cursor = 0;
    if (++impl_->sequence == 0)
        ++impl_->sequence;
    return {impl_->epoch, impl_->sequence, frames};
}
Status Core::process(const BlockContext &c, std::int32_t offset, std::int32_t frames, float *const *main,
                     const float *const *key, std::int32_t channels, const Settings &s) noexcept {
    return impl_ ? impl_->process(c, offset, frames, main, key, channels, s) : Status::NotPrepared;
}
Status Core::process(const BlockContext &c, std::int32_t offset, std::int32_t frames, double *const *main,
                     const double *const *key, std::int32_t channels, const Settings &s) noexcept {
    return impl_ ? impl_->process(c, offset, frames, main, key, channels, s) : Status::NotPrepared;
}
} // namespace adi::mix::compressor
