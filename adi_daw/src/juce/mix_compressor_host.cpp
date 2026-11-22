// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mix_compressor_host.hpp"
#include "adi/mix/compressor/ControlMapping.hpp"
#include "adi/mix/compressor/Core.hpp"
#include "adi/mix_functions.hpp"
#include "adi/ops.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace adi::device {
namespace {
namespace c = mix::compressor;
// Keep decaying envelopes out of hardware slow paths, without changing the
// caller's floating environment or adding per-sample work. No JUCE dependency.
struct ScopedFlushDenormals {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t previous = 0;
    ScopedFlushDenormals() noexcept {
        asm volatile("mrs %0, fpcr" : "=r"(previous));
        asm volatile("msr fpcr, %0" :: "r"(previous | (std::uint64_t{1} << 24)));
    }
    ~ScopedFlushDenormals() { asm volatile("msr fpcr, %0" :: "r"(previous)); }
#elif defined(_M_X64) || defined(__SSE2__)
    unsigned previous = _mm_getcsr();
    ScopedFlushDenormals() noexcept { _mm_setcsr(previous | 0x8040U); }
    ~ScopedFlushDenormals() { _mm_setcsr(previous); }
#else
    ScopedFlushDenormals() noexcept {}
#endif
};
constexpr std::size_t count = 8;
using Values = std::array<float, count>;
// Numeric stable IDs are the engine automation wire identity (ADR-0165).
constexpr std::array<const char *, count> ids{"0", "1", "2", "3", "4", "5", "6", "7"};
constexpr std::array<const char *, count> names{
    "Threshold", "Ratio", "Knee", "Range", "Attack", "Release", "Auto Release", "Auto Gain"};
const Values defaults = [] {
    const auto attack = static_cast<float>(std::cbrt((.25 - .005) / 249.995));
    const auto release =
        static_cast<float>(std::sqrt(std::atan(((200. - 10.) / 2490.) / .38877956936820496) / 1.2));
    return Values{.7f, .6f, .25f, 1.f, attack, release, 0, 1};
}();
std::size_t index(const std::string &id) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (id == ids[i])
            return i;
    return count;
}
c::Settings settings(const Values &v) noexcept {
    c::Settings s;
    s.thresholdDb = -60 + 60 * v[0];
    s.ratio = c::ratioFromNormalized(v[1]).value_or(1);
    s.kneeDb = 72 * v[2];
    s.rangeDb = 60 * v[3];
    s.attack = v[4];
    s.release = v[5];
    s.autoRelease = v[6] >= .5f;
    s.autoGain = v[7] >= .5f;
    return s;
}
double realValue(std::size_t i, const Values &v) noexcept {
    switch (i) {
    case 0:
        return -60.f + 60.f * v[0];
    case 1:
        return c::ratioFromNormalized(v[1]).value_or(1);
    case 2:
        return 72.f * v[2];
    case 3:
        return 60.f * v[3];
    case 4:
        return .005 + 249.995 * static_cast<double>(v[4]) * v[4] * v[4];
    case 5:
        return v[6] >= .5f
                   ? 100. * v[5]
                   : 10. + 2490. *
                               static_cast<float>(std::tan(static_cast<double>(v[5] * v[5]) * 1.2) *
                                                  .38877956936820496);
    default:
        return v[i] >= .5f ? 1 : 0;
    }
}
class CompressorDevice final : public DeviceInstance {
  public:
    CompressorDevice(const rows::PluginRef &ref, int channels) : channels_(channels) {
        identity_ = {ref.format, ref.uid, ref.name, ref.vendor, ref.version};
        for (std::size_t i = 0; i < count; ++i) {
            values_[i].store(defaults[i], std::memory_order_relaxed);
            auto &d = descriptors_[i];
            d.id = ids[i];
            d.name = names[i];
            d.automatable = true;
            d.configurable = false;
            d.groupId = "compressor";
            d.groupName = "Clean";
            d.defaultValue = ParamValue::withReal(defaults[i], realValue(i, defaults));
            d.shape = i >= 6 ? ParamShape::Switch : ParamShape::Continuous;
            d.domain = i >= 6 ? ParamDomain::Enum : ParamDomain::Real;
            d.stepCount = i >= 6 ? 2 : 0;
            d.hasRealRange = false;
            if (i >= 6)
                d.choiceLabels = {"Off", "On"};
            d.unit = i == 0 || i == 2 || i == 3 ? "dB" : i == 4 || i == 5 ? "ms" : "";
        }
    }
    const DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    ReactivationGate &gateForTesting() noexcept { return gate_; }
    void prepare(double rate, std::int32_t maximum) override {
        if (prepared_ && rate == rate_ && maximum == maximum_)
            return;
        const ReactivationGate::Hold hold(gate_);
        if (core_.prepare({rate, maximum, channels_}, settings(read())) != c::Status::Ok)
            throw std::runtime_error("Clean compressor preparation failed");
        rate_ = rate;
        maximum_ = maximum;
        prepared_ = true;
        controlsValid_ = false;
    }
    void release() override {
        const ReactivationGate::Hold hold(gate_);
        prepared_ = false;
    }
    std::int32_t latencySamples() const noexcept override { return 0; }
    void process(const engine::NodeIo &io) noexcept override {
        [[maybe_unused]] const ScopedFlushDenormals denormals;
        const ReactivationGate::Pass pass(gate_);
        if (io.frames <= 0 || io.blockOffset < 0)
            return;
        if (!pass) {
            passThrough(io);
            return;
        }
        if (io.blockOffset > maximum_ || io.frames > maximum_ - io.blockOffset)
            return;
        if (!prepared_ || io.channels != channels_ || (io.events.count > 0 && !io.events.first)) {
            passThrough(io);
            return;
        }
        auto v = read();
        for (std::int32_t n = 0; n < io.events.count; ++n) {
            const auto &e = io.events.first[n];
            const auto i = static_cast<std::size_t>(e.paramId);
            if (e.type != engine::EventType::ParamValue || i >= count || !std::isfinite(e.value) ||
                e.value < 0 || e.value > 1)
                continue;
            v[i] = i >= 6 ? (e.value >= .5 ? 1.f : 0.f) : static_cast<float>(e.value);
            values_[i].store(v[i], std::memory_order_release);
        }
        if (!controlsValid_ || appliedValues_ != v) {
            appliedSettings_ = settings(v);
            appliedValues_ = v;
            controlsValid_ = true;
        }
        if (io.wide())
            run(io, io.in64, io.out64, appliedSettings_);
        else
            run(io, io.in, io.out, appliedSettings_);
    }
    std::int32_t paramCount() const noexcept override { return static_cast<std::int32_t>(count); }
    const ParamDescriptor *paramAt(std::int32_t i) const noexcept override {
        return i >= 0 && static_cast<std::size_t>(i) < count
                   ? &descriptors_[static_cast<std::size_t>(i)]
                   : nullptr;
    }
    ParamValue getParam(const std::string &id) const noexcept override {
        const auto i = index(id);
        if (i == count)
            return {};
        const auto v = read();
        return ParamValue::withReal(v[i], realValue(i, v));
    }
    bool setParam(const std::string &id, const ParamValue &value) override {
        const auto i = index(id);
        if (i == count || !std::isfinite(value.normalized) || value.normalized < 0 ||
            value.normalized > 1)
            return false;
        values_[i].store(i >= 6 ? (value.normalized >= .5 ? 1.f : 0.f)
                                : static_cast<float>(value.normalized),
                         std::memory_order_release);
        return true;
    }
    std::string paramText(const std::string &id, double normalized) const override {
        const auto i = index(id);
        if (i == count || !std::isfinite(normalized) || normalized < 0 || normalized > 1)
            return {};
        auto v = read();
        v[i] = static_cast<float>(normalized);
        if (i >= 6)
            return normalized >= .5 ? "On" : "Off";
        const TextKey key{v[i], i == 5 ? v[6] : 0, true};
        if (textKeys_[i] == key)
            return text_[i];
        std::ostringstream out;
        out << std::fixed << std::setprecision(i == 4 ? 3 : i == 1 ? 2 : 1);
        if (i == 5 && v[6] >= .5f)
            out << 100 * normalized << " % (Auto)";
        else if (i == 1 && v[1] == 1)
            out << "Inf:1";
        else {
            out << realValue(i, v);
            if (i == 1)
                out << ":1";
            else
                out << ' ' << descriptors_[i].unit;
        }
        textKeys_[i] = key;
        text_[i] = out.str();
        return text_[i];
    }
    std::vector<std::string> stateRoles() const override { return {"state"}; }
    std::vector<std::uint8_t> saveState(const std::string &role) const override {
        if (role != "state")
            return {};
        auto s = extra_;
        s["version"] = 1;
        s["uid"] = identity_.uid;
        s["params"] = Payload::object();
        const auto v = read();
        for (std::size_t i = 0; i < count; ++i)
            s["params"][ids[i]] = v[i];
        return Payload::to_cbor(s);
    }
    bool loadState(const std::string &role, const std::vector<std::uint8_t> &bytes) override {
        if (role != "state" || bytes.size() > 65536)
            return false;
        try {
            auto s = Payload::from_cbor(bytes);
            if (!s.at("version").is_number_integer() || s.at("version") != 1 ||
                s.at("uid") != identity_.uid || !s.at("params").is_object() ||
                s.at("params").size() != count)
                return false;
            Values v{};
            for (std::size_t i = 0; i < count; ++i) {
                const auto &p = s.at("params").at(ids[i]);
                if (!p.is_number())
                    return false;
                const auto value = p.get<double>();
                if (!std::isfinite(value) || value < 0 || value > 1 ||
                    (i >= 6 && value != 0 && value != 1))
                    return false;
                v[i] = static_cast<float>(value);
            }
            const ReactivationGate::Hold hold(gate_);
            extra_ = std::move(s);
            for (std::size_t i = 0; i < count; ++i)
                values_[i].store(v[i], std::memory_order_release);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

  private:
    Values read() const noexcept {
        Values v{};
        for (std::size_t i = 0; i < count; ++i)
            v[i] = values_[i].load(std::memory_order_acquire);
        return v;
    }
    template <class T>
    void run(const engine::NodeIo &io, const T *const *in, T *const *out,
             const c::Settings &s) noexcept {
        if (!out)
            return;
        for (int ch = 0; ch < channels_; ++ch)
            if (!out[ch])
                return;
        // Refuse overlapping output planes before admitting a core block, so a
        // malformed caller cannot leave its checked stream cursor unfinished.
        const auto bytes = static_cast<std::uintptr_t>(io.frames) * sizeof(T);
        const auto overlaps = [bytes](const T *a, const T *b) noexcept {
            const auto x = reinterpret_cast<std::uintptr_t>(a);
            const auto y = reinterpret_cast<std::uintptr_t>(b);
            return x < y ? y - x < bytes : x - y < bytes;
        };
        if (channels_ == 2 && overlaps(out[0] + io.blockOffset, out[1] + io.blockOffset))
            return;
        for (int ch = 0; ch < channels_; ++ch)
            for (int other = 0; in && other < channels_; ++other)
                if (in[other] && overlaps(out[ch] + io.blockOffset, in[other] + io.blockOffset) &&
                    !(ch == other && out[ch] == in[other]))
                    return;
        std::array<T *, 2> main{};
        for (int ch = 0; ch < channels_; ++ch) {
            const auto cidx = static_cast<std::size_t>(ch);
            main[cidx] = out[ch] + io.blockOffset;
            for (int n = 0; n < io.frames; ++n) {
                const auto sample = in && in[ch] ? in[ch][n + io.blockOffset] : T{};
                main[cidx][n] = std::isfinite(static_cast<float>(sample)) ? sample : T{};
            }
        }
        // NodeIo supplies segments, not the full callback size. Clean has no
        // latency-changing controls and beginBlock never resets its envelope;
        // each contiguous segment is a checked logical block of the same stream.
        const auto block = core_.beginBlock(io.frames);
        static_cast<void>(core_.process(block, 0, io.frames, main.data(), nullptr, 0, s));
    }
    int channels_;
    double rate_ = 0;
    std::int32_t maximum_ = 0;
    bool prepared_ = false;
    DeviceIdentity identity_;
    ReactivationGate gate_;
    c::Core core_;
    // Audio-owner-only cache; normalized controls are read once per segment.
    Values appliedValues_{};
    c::Settings appliedSettings_{};
    bool controlsValid_ = false;
    std::array<std::atomic<float>, count> values_{};
    std::array<ParamDescriptor, count> descriptors_{};
    struct TextKey {
        float value = 0, automatic = 0;
        bool valid = false;
        bool operator==(const TextKey &) const = default;
    };
    // Serialized owner-only text queries. Unchanged visible controls avoid
    // repeated streams/formatting; no text work occurs in the audio callback.
    mutable std::array<TextKey, count> textKeys_{};
    mutable std::array<std::string, count> text_{};
    Payload extra_ = Payload::object();
};
} // namespace
ReactivationGate *detail::mixCompressorGateForTesting(DeviceInstance &instance) noexcept {
    if (auto *compressor = dynamic_cast<CompressorDevice *>(&instance))
        return &compressor->gateForTesting();
    return nullptr;
}
std::unique_ptr<DeviceInstance> makeMixCompressorFunction(const engine::DeviceRequest &request,
                                                          std::string &error) {
    if (!request.ref || !mix::functionForReference(request.ref->format, request.ref->uid) ||
        mix::functionForReference(request.ref->format, request.ref->uid)->kind !=
            mix::Function::Compressor ||
        (request.channels != 1 && request.channels != 2)) {
        error = "Unsupported native Clean compressor request";
        return {};
    }
    try {
        error.clear();
        return std::make_unique<CompressorDevice>(*request.ref, request.channels);
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}
} // namespace adi::device
