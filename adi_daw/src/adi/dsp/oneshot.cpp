// SPDX-License-Identifier: GPL-3.0-or-later
#include "oneshot.hpp"
#include "adi/audio/wav_file.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <nlohmann/json.hpp>
#include <numbers>
namespace adi::device {
namespace {
constexpr double pi = std::numbers::pi;
struct Spec {
    const char *id;
    const char *name;
    const char *unit;
    double lo, hi, def;
    int steps;
};
constexpr Spec specs[]{{"mode", "Mode", "", 0, 2, 0, 3},
                       {"start", "Start", "%", 0, 100, 0, 0},
                       {"end", "End", "%", 0, 100, 100, 0},
                       {"length", "Length", "%", .01, 100, 100, 0},
                       {"loop_on", "Loop", "", 0, 1, 0, 2},
                       {"loop_length", "Loop length", "%", .01, 100, 100, 0},
                       {"fade", "Loop fade", "%", 0, 100, 0, 0},
                       {"snap", "Snap", "", 0, 1, 0, 2},
                       {"voices", "Voices", "", 1, 32, 8, 32},
                       {"retrigger", "Retrigger", "", 0, 1, 1, 2},
                       {"gate", "Gate (off: Trigger)", "", 0, 1, 0, 2},
                       {"slice_by", "Slice by", "", 0, 3, 0, 4},
                       {"regions", "Regions", "", 1, 64, 8, 64},
                       {"beat_division", "Slice beat division", "quarter notes", .0625, 16, 1, 0},
                       {"slice_playback", "Slice playback", "", 0, 2, 0, 3},
                       {"sample_bpm", "Sample tempo", "BPM", 20, 999, 120, 0},
                       {"gain", "Sample gain", "dB", -48, 24, 0, 0},
                       {"volume", "Volume", "dB", -90, 12, -12, 0},
                       {"transpose", "Transpose", "st", -48, 48, 0, 97},
                       {"detune", "Detune", "cents", -50, 50, 0, 0},
                       {"attack", "Attack", "ms", 0, 20000, 1, 0},
                       {"decay", "Decay", "ms", 0, 20000, 100, 0},
                       {"sustain", "Sustain", "%", 0, 100, 100, 0},
                       {"release", "Release", "ms", 0, 20000, 50, 0},
                       {"fade_in", "Fade in", "ms", 0, 20000, 0, 0},
                       {"fade_out", "Fade out", "ms", 0, 20000, 5, 0},
                       {"filter_on", "Filter", "", 0, 1, 0, 2},
                       {"filter_type", "Filter type", "", 0, 3, 0, 4},
                       {"slope", "24 dB (off: 12 dB)", "", 0, 1, 0, 2},
                       {"cutoff", "Frequency", "Hz", 20, 20000, 20000, 0},
                       {"resonance", "Resonance", "Q", .5, 12, .7071067811865476, 0},
                       {"lfo_on", "LFO", "", 0, 1, 0, 2},
                       {"lfo_shape", "LFO shape", "", 0, 5, 0, 6},
                       {"lfo_rate", "LFO rate", "Hz", .01, 30, 1, 0},
                       {"lfo_sync", "LFO sync", "", 0, 1, 0, 2},
                       {"lfo_beat", "LFO period", "quarter notes", .0625, 32, 1, 0},
                       {"lfo_pitch", "LFO pitch", "st", 0, 24, 0, 0},
                       {"lfo_volume", "LFO volume", "%", 0, 100, 0, 0},
                       {"lfo_pan", "LFO pan", "%", 0, 100, 0, 0},
                       {"lfo_filter", "LFO filter", "octaves", 0, 8, 0, 0}};
static_assert(std::size(specs) == OneShot::Count);
double real(int i, double n) {
    const auto &s = specs[i];
    const double v = s.lo + std::clamp(n, 0., 1.) * (s.hi - s.lo);
    return s.steps ? std::round(v) : v;
}
double normal(int i, double v) {
    return std::clamp((v - specs[i].lo) / (specs[i].hi - specs[i].lo), 0., 1.);
}
} // namespace
OneShot::OneShot() {
    for (int i = 0; i < Count; ++i) {
        const auto &s = specs[i];
        auto &p = params_[static_cast<std::size_t>(i)];
        p.id = s.id;
        p.name = s.name;
        p.unit = s.unit;
        p.domain = s.steps ? ParamDomain::Enum : ParamDomain::Real;
        p.minReal = s.lo;
        p.maxReal = s.hi;
        p.hasRealRange = true;
        p.defaultValue = ParamValue::withReal(normal(i, s.def), s.def);
        p.shape = s.steps == 2 ? ParamShape::Switch
                  : s.steps    ? ParamShape::Menu
                               : ParamShape::Continuous;
        p.stepCount = s.steps;
        values_[static_cast<std::size_t>(i)].store(p.defaultValue.normalized);
    }
}
const ParamDescriptor *OneShot::paramAt(std::int32_t i) const noexcept {
    return i >= 0 && i < Count ? &params_[static_cast<std::size_t>(i)] : nullptr;
}
ParamValue OneShot::getParam(const std::string &id) const noexcept {
    for (int i = 0; i < Count; ++i)
        if (params_[static_cast<std::size_t>(i)].id == id) {
            const auto n = values_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
            return ParamValue::withReal(n, real(i, n));
        }
    return {};
}
bool OneShot::setParam(const std::string &id, const ParamValue &v) {
    if (!std::isfinite(v.normalized))
        return false;
    for (int i = 0; i < Count; ++i)
        if (params_[static_cast<std::size_t>(i)].id == id) {
            values_[static_cast<std::size_t>(i)].store(normal(i, real(i, v.normalized)),
                                                       std::memory_order_relaxed);
            return true;
        }
    return false;
}
std::string OneShot::paramText(const std::string &id, double n) const {
    if (!std::isfinite(n))
        return {};
    for (int i = 0; i < Count; ++i)
        if (params_[static_cast<std::size_t>(i)].id == id) {
            const auto value = real(i, n);
            const auto k = static_cast<std::size_t>(std::max(0., value));
            if (i == Mode)
                return std::array{"Classic", "1-Shot", "Slice"}[k];
            if (i == SliceBy)
                return std::array{"Region", "Beat", "Transient", "Manual"}[k];
            if (i == SlicePlayback)
                return std::array{"Mono", "Poly", "Thru"}[k];
            if (i == FilterType)
                return std::array{"Low-pass", "High-pass", "Band-pass", "Notch"}[k];
            if (i == LfoShape)
                return std::array{"Sine", "Square", "Triangle", "Saw down", "Saw up", "Random"}[k];
            if (specs[i].steps == 2)
                return value > 0 ? "On" : "Off";
            return std::to_string(value) + " " + specs[i].unit;
        }
    return {};
}
void OneShot::prepare(double rate, std::int32_t) {
    rate_ = std::isfinite(rate) && rate >= 1000 && rate <= 768000 ? rate : 48000;
    voices_ = {};
    age_ = 0;
    modulation_.fill(0);
}
void OneShot::pumpMainThread() { publisher_.collect(); }
bool OneShot::publishSample(std::span<const float> audio, int channels, double rate,
                            std::span<const std::size_t> manual) {
    if (channels < 1 || channels > 2 || !std::isfinite(rate) || rate < 1000 || rate > 768000 ||
        audio.empty() || audio.size() > 16 * 1024 * 1024 ||
        audio.size() % static_cast<std::size_t>(channels) || manual.size() > 64)
        return false;
    for (float x : audio)
        if (!std::isfinite(x))
            return false;
    auto s = std::make_shared<Sample>();
    s->data.assign(audio.begin(), audio.end());
    s->channels = channels;
    s->rate = rate;
    s->manual = {0};
    for (auto m : manual) {
        if (m >= s->frames())
            return false;
        s->manual.push_back(m);
    }
    std::sort(s->manual.begin(), s->manual.end());
    s->manual.erase(std::unique(s->manual.begin(), s->manual.end()), s->manual.end());
    if (s->manual.size() > 64)
        return false;
    s->transients = {0};
    s->zeros = {0};
    double envelope = 0;
    std::size_t last = 0;
    for (std::size_t i = 1; i < s->frames(); ++i) {
        const auto ch = static_cast<std::size_t>(channels);
        const float x = s->data[i * ch], prev = s->data[(i - 1) * ch];
        if ((x >= 0 && prev < 0) || (x < 0 && prev >= 0))
            s->zeros.push_back(i);
        const double level = std::abs(x);
        if (s->transients.size() < 64 && level > std::max(.05, envelope * 3) &&
            i - last > static_cast<std::size_t>(rate * .01)) {
            s->transients.push_back(i);
            last = i;
        }
        envelope = .995 * envelope + .005 * level;
    }
    auto publication = std::make_unique<Publication>();
    publication->sample = s;
    sample_ = std::move(s);
    publisher_.publish(std::move(publication));
    publisher_.collect();
    return true;
}
bool OneShot::loadSample(const std::filesystem::path &path, std::string &error) {
    try {
        audio::WavReader w(path);
        if (w.channels() < 1 || w.channels() > 2 || w.frames() > 16 * 1024 * 1024 / w.channels())
            throw std::runtime_error("OneShot needs mono/stereo audio up to 64 MiB decoded");
        std::vector<float> data(static_cast<std::size_t>(w.frames()) * w.channels());
        if (w.read(data.data(), static_cast<std::uint32_t>(w.frames())) != w.frames())
            throw std::runtime_error("incomplete sample");
        if (!publishSample(data, w.channels(), w.sampleRate()))
            throw std::runtime_error("invalid sample");
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
std::vector<std::uint8_t> OneShot::saveState(const std::string &role) const {
    if (role != "component")
        return {};
    nlohmann::json j = {{"version", 1}, {"params", nlohmann::json::array()}};
    for (const auto &v : values_)
        j["params"].push_back(v.load(std::memory_order_relaxed));
    if (sample_) {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(sample_->data.size() * 4);
        for (float f : sample_->data) {
            const auto u = std::bit_cast<std::uint32_t>(f);
            for (unsigned k = 0; k < 4; ++k)
                bytes.push_back(static_cast<std::uint8_t>(u >> (k * 8)));
        }
        j["audio"] = nlohmann::json::binary(bytes);
        j["channels"] = sample_->channels;
        j["rate"] = sample_->rate;
        j["slices"] = sample_->manual;
    }
    return nlohmann::json::to_cbor(j);
}
bool OneShot::loadState(const std::string &role, const std::vector<std::uint8_t> &bytes) {
    if (role != "component" || bytes.size() > 70 * 1024 * 1024)
        return false;
    try {
        const auto j = nlohmann::json::from_cbor(bytes);
        if (j.at("version") != 1 || j.at("params").size() != Count)
            return false;
        std::array<double, Count> values{};
        for (int i = 0; i < Count; ++i) {
            auto &v = values[static_cast<std::size_t>(i)];
            v = j.at("params").at(static_cast<std::size_t>(i)).get<double>();
            if (!std::isfinite(v) || v < 0 || v > 1)
                return false;
        }
        if (j.contains("audio")) {
            const auto &data = j.at("audio").get_binary();
            if (data.size() % 4)
                return false;
            std::vector<float> audio(data.size() / 4);
            for (std::size_t i = 0; i < audio.size(); ++i) {
                std::uint32_t u = 0;
                for (unsigned k = 0; k < 4; ++k)
                    u |= static_cast<std::uint32_t>(data[i * 4 + k]) << (k * 8);
                audio[i] = std::bit_cast<float>(u);
            }
            if (!publishSample(audio, j.at("channels").get<int>(), j.at("rate").get<double>(),
                               j.at("slices").get<std::vector<std::size_t>>()))
                return false;
        } else {
            auto p = std::make_unique<Publication>();
            sample_.reset();
            publisher_.publish(std::move(p));
        }
        for (int i = 0; i < Count; ++i)
            values_[static_cast<std::size_t>(i)].store(values[static_cast<std::size_t>(i)],
                                                       std::memory_order_relaxed);
        return true;
    } catch (...) {
        return false;
    }
}
double OneShot::Filter::run(double x, double g, double q, int type) noexcept {
    const double r = 1 / q;
    const double v1 = (a + g * (x - b)) / (1 + g * (g + r));
    const double v2 = b + g * v1;
    a = 2 * v1 - a;
    b = 2 * v2 - b;
    const double high = x - r * v1 - v2;
    return type == 0 ? v2 : type == 1 ? high : type == 2 ? v1 : high + v2;
}
double OneShot::read(const Sample &s, double position, int channel) const noexcept {
    position = std::clamp(position, 0., static_cast<double>(s.frames() - 1));
    const auto a = static_cast<std::size_t>(position);
    const auto b = std::min(a + 1, s.frames() - 1);
    const auto ch = static_cast<std::size_t>(std::min(channel, s.channels - 1)),
               stride = static_cast<std::size_t>(s.channels);
    const double x = s.data[a * stride + ch];
    return x + (s.data[b * stride + ch] - x) * (position - static_cast<double>(a));
}
void OneShot::noteOn(const engine::Event &e, const Sample &s,
                     const std::array<double, Count> &p) noexcept {
    if(e.dim>127 || !std::isfinite(e.value) || e.value<=0)return;
    const int mode = static_cast<int>(p[Mode]);
    const bool mono = mode == 1 || (mode == 2 && p[SlicePlayback] != 1);
    const int count = mono ? 1 : static_cast<int>(p[Voices]);
    Voice *selected = nullptr;
    for (int i = 0; i < 32; ++i)
        if (i >= count || (p[Retrigger] > 0 && voices_[static_cast<std::size_t>(i)].key == e.dim))
            voices_[static_cast<std::size_t>(i)].active = false;
    for (int i = 0; i < count; ++i)
        if (!voices_[static_cast<std::size_t>(i)].active) {
            selected = &voices_[static_cast<std::size_t>(i)];
            break;
        }
    if (!selected)
        selected = &*std::min_element(voices_.begin(), voices_.begin() + count,
                                      [](const Voice &a, const Voice &b) { return a.age < b.age; });
    *selected = {};
    auto &v = *selected;
    v.active = true;
    v.held = true;
    v.id = e.noteId;
    v.key = e.dim;
    v.age = ++age_;
    v.velocity = std::clamp(e.value, 0., 1.);
    v.rng = static_cast<std::uint32_t>(v.age) | 1u;
    const double frames = static_cast<double>(s.frames());
    v.begin = std::min(frames - 1, frames * p[Start] / 100.);
    v.end = std::max(v.begin + 1, frames * p[End] / 100.);
    if (mode == 0)
        v.end = v.begin + (v.end - v.begin) * p[Length] / 100.;
    if (mode == 2) {
        const int key = v.key - 36;
        if (key < 0) {
            v.active = false;
            return;
        }
        const int by = static_cast<int>(p[SliceBy]);
        if (by < 2) {
            const double size = by == 0 ? (v.end - v.begin) / p[Regions]
                                        : s.rate * 60 / p[SampleBpm] * p[BeatDivision];
            const auto begin = v.begin + key * size;
            if (begin >= v.end) {
                v.active = false;
                return;
            }
            if (p[SlicePlayback] != 2)
                v.end = std::min(v.end, begin + size);
            v.begin = begin;
        } else {
            const auto &slices = by == 2 ? s.transients : s.manual;
            auto first =
                std::lower_bound(slices.begin(), slices.end(), static_cast<std::size_t>(v.begin));
            if (static_cast<std::size_t>(slices.end() - first) <= static_cast<std::size_t>(key)) {
                v.active = false;
                return;
            }
            auto it = first + key;
            v.begin = static_cast<double>(*it);
            if (p[SlicePlayback] != 2 && it + 1 != slices.end())
                v.end = static_cast<double>(*(it + 1));
        }
    }
    v.loop = v.end - (v.end - v.begin) * p[LoopLength] / 100.;
    if (p[Snap] > 0) {
        auto snap = [&](double x) {
            auto it = std::lower_bound(s.zeros.begin(), s.zeros.end(), static_cast<std::size_t>(x));
            if (it == s.zeros.end())
                return x;
            return static_cast<double>(*it);
        };
        v.begin = snap(v.begin);
        v.loop = snap(v.loop);
        v.end = std::min(frames, snap(v.end));
    }
    v.end = std::max(v.begin + 1, v.end);
    v.loop = std::clamp(v.loop, v.begin, v.end - 1);
    v.position = v.begin;
}
void OneShot::event(const engine::Event &e, const Sample *s,
                    std::array<double, Count> &p) noexcept {
    if ((e.type == engine::EventType::ParamValue || e.type==engine::EventType::ParamMod) && e.paramId < Count && std::isfinite(e.value)) {
        if(e.type==engine::EventType::ParamMod) modulation_[e.paramId]=e.value;
        else values_[e.paramId].store(normal(static_cast<int>(e.paramId),real(static_cast<int>(e.paramId),e.value)),std::memory_order_relaxed);
        p[e.paramId]=real(static_cast<int>(e.paramId),values_[e.paramId].load(std::memory_order_relaxed)+modulation_[e.paramId]);
    } else if (e.type == engine::EventType::NoteOn && s)
        noteOn(e, *s, p);
    else if (e.type == engine::EventType::NoteOff) {
        for (auto &v : voices_)
            if (v.active && (e.noteId ? v.id == e.noteId : v.key == e.dim)) {
                if (p[Mode] != 0 && p[Gate] == 0)
                    continue;
                v.held = false;
                v.stage = 3;
                v.releaseStep =
                    v.env / std::max(1., rate_ * (p[Mode] == 0 ? p[Release] : p[FadeOut]) * .001);
            }
    } else if (e.type == engine::EventType::NoteExpression && e.dim == 0 &&
               std::isfinite(e.value)) {
        for (auto &v : voices_)
            if (v.active && v.id == e.noteId)
                v.tuning = std::clamp(e.value, -96., 96.);
    }
}
void OneShot::process(const engine::NodeIo &io) noexcept {
    engine::SnapshotPublisher<Publication>::AudioRead publication(publisher_);
    const Sample *s = publication.valid() ? publication->sample.get() : nullptr;
    if (publication.valid() && sampleSeq_ != publication->seq) {
        voices_ = {};
        sampleSeq_ = publication->seq;
    }
    std::array<double, Count> p{};
    for (int i = 0; i < Count; ++i)
        p[static_cast<std::size_t>(i)] =
            real(i, values_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed)+modulation_[static_cast<std::size_t>(i)]);
    int next = 0;
    for (int i = 0; i < io.frames; ++i) {
        const int frame = io.blockOffset + i;
        while (next < io.events.count && io.events.first[next].frame <= frame) {
            event(io.events.first[next], s, p);
            ++next;
        }
        double output[2]{};
        if (s)
            for (auto &v : voices_)
                if (v.active) {
                    const bool classic = p[Mode] == 0;
                    const bool looping = classic && p[LoopOn] > 0 && v.held;
                    if (v.position >= v.end) {
                        if (looping) {
                            const double overlap = (v.end - v.loop) * p[Fade] / 200.;
                            v.position = v.loop + overlap +
                                         std::fmod(v.position - v.end, v.end - v.loop - overlap);
                        } else {
                            v.active = false;
                            continue;
                        }
                    }
                    if (v.stage == 3) {
                        v.env = std::max(0., v.env - v.releaseStep);
                        if (v.env == 0) {
                            v.active = false;
                            continue;
                        }
                    } else if (classic) {
                        if (v.stage == 0) {
                            v.env =
                                std::min(1., v.env + 1 / std::max(1., rate_ * p[Attack] * .001));
                            if (v.env >= 1)
                                v.stage = 1;
                        } else if (v.stage == 1) {
                            v.env = std::max(p[Sustain] / 100.,
                                             v.env - (1 - p[Sustain] / 100.) /
                                                         std::max(1., rate_ * p[Decay] * .001));
                            if (v.env <= p[Sustain] / 100.)
                                v.stage = 2;
                        } else
                            v.env = p[Sustain] / 100.;
                    } else
                        v.env = std::min(1., static_cast<double>(v.played + 1) /
                                                 std::max(1., rate_ * p[FadeIn] * .001));
                    double lfo = 0;
                    if (p[LfoOn] > 0) {
                        const int shape = static_cast<int>(p[LfoShape]);
                        lfo = shape == 0   ? std::sin(2 * pi * v.phase)
                              : shape == 1 ? (v.phase < .5 ? 1. : -1.)
                              : shape == 2 ? 1 - 4 * std::abs(v.phase - .5)
                              : shape == 3 ? 1 - 2 * v.phase
                              : shape == 4 ? 2 * v.phase - 1
                                           : v.random;
                        const double hz =
                            p[LfoSync] > 0
                                ? (io.transport ? io.transport->bpm : 120.) / 60 / p[LfoBeat]
                                : p[LfoRate];
                        v.phase += hz / rate_;
                        if (v.phase >= 1) {
                            v.phase -= std::floor(v.phase);
                            v.rng = 1664525u * v.rng + 1013904223u;
                            v.random = static_cast<double>(v.rng) / 2147483648. - 1;
                        }
                    }
                    const double pitch = (p[Mode] == 2 ? 0 : v.key - 60) + p[Transpose] +
                                         p[Detune] / 100. + v.tuning + lfo * p[LfoPitch];
                    const double increment = s->rate / rate_ * std::exp2(pitch / 12.);
                    double amplitude = v.env * v.velocity *
                                       std::pow(10., (p[Gain] + p[Volume]) / 20.) *
                                       (1 - p[LfoVolume] / 200. * (1 - lfo));
                    if (!classic)
                        amplitude *= std::min(1., (v.end - v.position) / increment /
                                                      std::max(1., rate_ * p[FadeOut] * .001));
                    const double fade = looping ? (v.end - v.loop) * p[Fade] / 200. : 0;
                    for (int c = 0; c < 2; ++c) {
                        double x = read(*s, v.position, c);
                        if (fade > 0 && v.position > v.end - fade) {
                            const auto t = (v.position - (v.end - fade)) / fade;
                            x = x * std::cos(t * pi * .5) +
                                read(*s, v.loop + v.position - (v.end - fade), c) *
                                    std::sin(t * pi * .5);
                        }
                        if (p[FilterOn] > 0) {
                            const double hz = std::clamp(p[Cutoff] * std::exp2(lfo * p[LfoFilter]),
                                                         5., rate_ * .45);
                            const double g = std::tan(pi * hz / rate_);
                            x = v.filters[c][0].run(x, g, p[Resonance],
                                                    static_cast<int>(p[FilterType]));
                            if (p[Slope] > 0)
                                x = v.filters[c][1].run(x, g, p[Resonance],
                                                        static_cast<int>(p[FilterType]));
                        }
                        const double pan = lfo * p[LfoPan] / 100.;
                        output[c] += x * amplitude *
                                     std::sqrt(std::clamp(1 + (c == 0 ? -pan : pan), 0., 2.));
                    }
                    v.position += increment;
                    ++v.played;
                }
        if (io.out)
            for (int c = 0; c < io.channels; ++c)
                if (io.out[c])
                    io.out[c][frame] = static_cast<float>(c < 2 ? output[c] : 0);
    }
}
} // namespace adi::device
