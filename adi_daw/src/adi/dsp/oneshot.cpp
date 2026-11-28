// SPDX-License-Identifier: GPL-3.0-or-later
#include "oneshot.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/blob.hpp"
#include "adi/controller_data.hpp"
#include "adi/engine/mpe_input.hpp"
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
                       {"loop_on", "Loop", "", 0, 1, 1, 2},
                       {"loop_length", "Loop length", "%", .01, 100, 100, 0},
                       {"fade", "Loop fade", "%", 0, 100, 0, 0},
                       {"snap", "Snap", "", 0, 1, 1, 2},
                       {"voices", "Voices", "", 1, 32, 6, 32},
                       {"retrigger", "Retrigger", "", 0, 1, 1, 2},
                       {"gate", "Gate (off: Trigger)", "", 0, 1, 0, 2},
                       {"slice_by", "Slice by", "", 0, 3, 0, 4},
                       {"regions", "Regions", "", 1, 64, 8, 64},
                       {"beat_division", "Slice beat division", "quarter notes", .0625, 16, 1, 0},
                       {"slice_playback", "Slice playback", "", 0, 2, 0, 3},
                       {"sample_bpm", "Sample tempo", "BPM", 20, 999, 120, 0},
                       {"gain", "Sample gain", "dB", -48, 24, 0, 0},
                       {"volume", "Volume", "dB", -36, 36, -12, 0},
                       {"transpose", "Transpose", "st", -48, 48, 0, 97},
                       {"detune", "Detune", "cents", -50, 50, 0, 0},
                       {"attack", "Attack", "ms", .1, 20000, .1, 0},
                       {"decay", "Decay", "ms", 1, 60000, 600, 0},
                       {"sustain", "Sustain", "dB", -70, 0, 0, 0},
                       {"release", "Release", "ms", 1, 60000, 50, 0},
                       {"fade_in", "Fade in", "ms", 0, 2000, .1, 0},
                       {"fade_out", "Fade out", "ms", 0, 2000, .1, 0},
                       {"filter_on", "Filter", "", 0, 1, 1, 2},
                       {"filter_type", "Filter type", "", 0, 3, 0, 4},
                       {"slope", "24 dB (off: 12 dB)", "", 0, 1, 1, 2},
                       {"cutoff", "Frequency", "Hz", 30, 22000, 22000, 0},
                       {"resonance", "Resonance", "%", 0, 125, 0, 0},
                       {"lfo_on", "LFO", "", 0, 1, 1, 2},
                       {"lfo_shape", "LFO shape", "", 0, 5, 0, 6},
                       {"lfo_rate", "LFO rate", "Hz", .01, 30, 1, 0},
                       {"lfo_sync", "LFO sync", "", 0, 1, 0, 2},
                       {"lfo_beat", "LFO period", "quarter notes", .0625, 32, 1, 0},
                       {"lfo_pitch", "LFO pitch", "st", 0, 24, 0, 0},
                       {"lfo_volume", "LFO volume", "%", 0, 100, 0, 0},
                       {"lfo_pan", "LFO pan", "%", 0, 100, 0, 0},
                       {"lfo_filter", "LFO filter", "octaves", 0, 8, 0, 0},
                       {"vel_to_vol", "Vel > Vol", "%", 0, 100, 35, 0}};
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
void OneShot::setParamAt(int i, double n) noexcept {
    if (i < 0 || i >= Count || !std::isfinite(n)) {
        return;
    }
    values_[static_cast<std::size_t>(i)].store(normal(i, real(i, n)), std::memory_order_relaxed);
}
void OneShot::setRealAt(int i, double value) noexcept {
    if (i < 0 || i >= Count || !std::isfinite(value)) {
        return;
    }
    setParamAt(i, normal(i, value));
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
void OneShot::prepare(double rate, std::int32_t frames) {
    const double accepted = std::isfinite(rate) && rate >= 1000 && rate <= 768000 ? rate : 48000;
    // DF-E37 (f), ADR-0090 d5: an unchanged rate and block size changes
    // nothing, so a graph rebuild does not cut sounding notes. release()
    // makes the next prepare() a fresh start.
    if (prepared_ && accepted == rate_ && frames == frames_)
        return;
    prepared_ = true;
    frames_ = frames;
    rate_ = accepted;
    voices_ = {};
    midiChannels_ = {};
    age_ = 0;
    sampleSeq_ = 0;
    borrowed_ = false;
    modulation_.fill(0);
}
void OneShot::pumpMainThread() { publisher_.collect(); }
std::shared_ptr<const OneShot::PreparedSample> OneShot::prepareSample(
    std::span<const float> audio, int channels, double rate,
    std::span<const std::size_t> manual) {
    if (channels < 1 || channels > 2 || !std::isfinite(rate) || rate < 1000 || rate > 768000 ||
        audio.empty() || audio.size() > 16 * 1024 * 1024 ||
        audio.size() % static_cast<std::size_t>(channels) || manual.size() > 64)
        return {};
    for (float x : audio)
        if (!std::isfinite(x))
            return {};
    auto s = std::shared_ptr<Sample>(new Sample);
    s->data.assign(audio.begin(), audio.end());
    s->channels = channels;
    s->rate = rate;
    s->manual = {0};
    for (auto m : manual) {
        if (m >= s->frames())
            return {};
        s->manual.push_back(m);
    }
    std::sort(s->manual.begin(), s->manual.end());
    s->manual.erase(std::unique(s->manual.begin(), s->manual.end()), s->manual.end());
    if (s->manual.size() > 64)
        return {};
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
        if (envelope < 1e-30) {
            envelope = 0;
        }
    }
    return s;
}
bool OneShot::publishSample(std::span<const float> audio, int channels, double rate,
                            std::span<const std::size_t> manual) {
    auto s = prepareSample(audio, channels, rate, manual);
    if (!s) return false;
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
double OneShot::Filter::run(double x, double g, double r, double d, int type) noexcept {
    // r = 1 / q and d = 1 + g * (g + r), the caller's once for every stage
    // and channel of a voice's sample (the same values, so the same bits).
    const double v1 = (a + g * (x - b)) / d;
    const double v2 = b + g * v1;
    a = 2 * v1 - a;
    b = 2 * v2 - b;
    if (std::abs(a) < 1e-30) {
        a = 0;
    }
    if (std::abs(b) < 1e-30) {
        b = 0;
    }
    const double high = x - r * v1 - v2;
    return type == 0 ? v2 : type == 1 ? high : type == 2 ? v1 : high + v2;
}
double OneShot::cubic(const Sample &s, std::size_t a, std::size_t ch, double t) noexcept {
    // Catmull-Rom through the frames a - 1 .. a + 2, the edges repeated: it
    // passes through every frame and draws any quadratic exactly.
    const std::size_t last = s.frames() - 1, stride = static_cast<std::size_t>(s.channels);
    const double before = s.data[(a > 0 ? a - 1 : 0) * stride + ch], x0 = s.data[a * stride + ch],
                 x1 = s.data[std::min(a + 1, last) * stride + ch],
                 x2 = s.data[std::min(a + 2, last) * stride + ch];
    const double c1 = .5 * (x1 - before), c2 = before - 2.5 * x0 + 2 * x1 - .5 * x2,
                 c3 = .5 * (x2 - before) + 1.5 * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}
double OneShot::read(const Sample &s, double position, int channel, int quality) const noexcept {
    position = std::clamp(position, 0., static_cast<double>(s.frames() - 1));
    const auto a = static_cast<std::size_t>(position);
    const auto b = std::min(a + 1, s.frames() - 1);
    const auto ch = static_cast<std::size_t>(std::min(channel, s.channels - 1)),
               stride = static_cast<std::size_t>(s.channels);
    const double x = s.data[a * stride + ch];
    if (quality == 0)
        return x;
    if (quality == 2)
        return cubic(s, a, ch, position - static_cast<double>(a));
    return x + (s.data[b * stride + ch] - x) * (position - static_cast<double>(a));
}
void OneShot::readBoth(const Sample &s, double position, double *x, int quality) const noexcept {
    // read() for channels 0 and 1 at one position: the same arithmetic, the
    // position's clamp and index taken once.
    position = std::clamp(position, 0., static_cast<double>(s.frames() - 1));
    const auto a = static_cast<std::size_t>(position);
    const auto b = std::min(a + 1, s.frames() - 1);
    const auto stride = static_cast<std::size_t>(s.channels);
    const double t = position - static_cast<double>(a);
    for (int c = 0; c < 2; ++c) {
        const auto ch = static_cast<std::size_t>(std::min(c, s.channels - 1));
        const double y = s.data[a * stride + ch];
        if (quality == 0)
            x[c] = y;
        else if (quality == 2)
            x[c] = cubic(s, a, ch, t);
        else
            x[c] = y + (s.data[b * stride + ch] - y) * t;
    }
}
void OneShot::queueNoteGain(double gain, bool reverse) noexcept {
    if (noteGainCount_ == noteGains_.size() || !std::isfinite(gain)) return;
    noteGains_[(noteGainHead_ + noteGainCount_) % noteGains_.size()] = {std::max(0., gain), reverse};
    ++noteGainCount_;
}

// DF-E46: the source region a Slice-mode key plays, or false for a key with
// no slice. Every Slice By mode refuses a slice that begins at or past End
// and cuts one that runs past it, unless Slice Playback is Thru.
bool OneShot::slice(int key, const Sample &s, const std::array<double, Count> &p, double &begin,
                    double &end) noexcept {
    if (key < 0)
        return false;
    const int by = static_cast<int>(p[SliceBy]);
    double from = 0, to = end;
    if (by < 2) {
        const double size = by == 0 ? (end - begin) / p[Regions]
                                    : s.rate * 60 / p[SampleBpm] * p[BeatDivision];
        from = begin + key * size;
        if (p[SlicePlayback] != 2)
            to = std::min(end, from + size);
    } else {
        const auto &slices = by == 2 ? s.transients : s.manual;
        auto first = std::lower_bound(slices.begin(), slices.end(), static_cast<std::size_t>(begin));
        if (static_cast<std::size_t>(slices.end() - first) <= static_cast<std::size_t>(key))
            return false;
        const auto it = first + key;
        from = static_cast<double>(*it);
        if (p[SlicePlayback] != 2 && it + 1 != slices.end())
            to = std::min(end, static_cast<double>(*(it + 1)));
    }
    if (from >= end)
        return false;
    begin = from;
    end = to;
    return true;
}
void OneShot::noteOn(const engine::Event &e, const Sample &s,
                     const std::array<double, Count> &p) noexcept {
    if(e.dim>127 || e.channel>=midiChannels_.size() || !std::isfinite(e.value) || e.value<=0)return;
    const int mode = static_cast<int>(p[Mode]);
    // The region this note plays is decided BEFORE a voice is chosen (DF-E46
    // a): in Slice mode a key with no slice plays nothing and steals nothing.
    const double frames = static_cast<double>(s.frames());
    double begin = std::min(frames - 1, frames * p[Start] / 100.);
    double end = std::max(begin + 1, frames * p[End] / 100.);
    if (mode == 0)
        end = begin + (end - begin) * p[Length] / 100.;
    if (mode == 2 && !slice(e.dim - 36, s, p, begin, end)) {
        // The per-note gain queued for this note is its own: take it, so the
        // next note gets the one queued for it.
        if (noteGainCount_ > 0) {
            noteGainHead_ = (noteGainHead_ + 1) % noteGains_.size();
            --noteGainCount_;
        }
        return;
    }
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
    v.channel = e.channel;
    v.age = ++age_;
    v.velocity = std::clamp(e.value, 0., 1.);
    v.gain = 1;
    bool reverse = false;
    if (noteGainCount_ > 0) {
        v.gain = noteGains_[noteGainHead_].gain;
        reverse = noteGains_[noteGainHead_].reverse;
        noteGainHead_ = (noteGainHead_ + 1) % noteGains_.size();
        --noteGainCount_;
    }
    v.rng = static_cast<std::uint32_t>(v.age) | 1u;
    v.begin = begin;
    v.end = end;
    v.loop = v.end - (v.end - v.begin) * p[LoopLength] / 100.;
    const auto snap = [&](double x) {
        auto it = std::lower_bound(s.zeros.begin(), s.zeros.end(), static_cast<std::size_t>(x));
        if (it == s.zeros.end())
            return x;
        return static_cast<double>(*it);
    };
    if (p[Snap] > 0) {
        v.begin = snap(v.begin);
        v.loop = snap(v.loop);
        v.end = std::min(frames, snap(v.end));
    }
    v.end = std::max(v.begin + 1, v.end);
    v.loop = std::clamp(v.loop, v.begin, v.end - 1);
    // SC-483: the Sampler's own loop points. With a fresh Travel the sustain
    // loop ends where the region does, as it always did.
    v.loopEnd = v.end;
    if (travel_.loopEnd >= 0)
        v.loopEnd = std::clamp(frames * travel_.loopEnd, v.loop + 1, v.end);
    v.releaseLoop = std::clamp(frames * travel_.releaseLoop, v.begin, v.end - 1);
    if (p[Snap] > 0) {
        // The Sampler's own loop points snap as the others do (a fresh Travel
        // has none: its loop ends at the snapped end).
        if (travel_.loopEnd >= 0)
            v.loopEnd = std::clamp(snap(v.loopEnd), v.loop + 1, v.end);
        v.releaseLoop = std::clamp(snap(v.releaseLoop), v.begin, v.end - 1);
    }
    if (reverse) {
        // Played from the end: position p reads the frame begin + end - 1 - p,
        // and the sustain loop is the same stretch of the sample, entered
        // from above. The release loop stays the last stretch the voice
        // reaches, now the sample's first frames (Live's manual does not say).
        v.reversed = true;
        v.mirror = v.begin + v.end - 1;
        const double low = v.begin + v.end - v.loopEnd, high = v.begin + v.end - v.loop;
        v.loop = low;
        v.loopEnd = high;
    }
    v.position = v.begin;
}
void OneShot::rememberMidiControl(MidiChannels &channels, const engine::Event &e) noexcept {
    if (e.channel >= channels.size() || !std::isfinite(e.value))
        return;
    auto &channel = channels[e.channel];
    if (e.type == engine::EventType::Controller) {
        if (e.paramId == 64)
            channel.sustain = e.value >= .5;
        else if (e.paramId == 1)
            channel.modulation = std::clamp(e.value, 0., 1.);
        else if (e.paramId == controllers::kPitchBend)
            channel.bend = (std::clamp(e.value, 0., 1.) * 2. - 1.) * engine::MpeZone{}.masterBendSemitones;
    } else if (e.type == engine::EventType::NoteExpression && e.noteId == 0 &&
               e.dim == static_cast<std::uint16_t>(ExpressionDim::Pitch)) {
        channel.bend = std::clamp(e.value, -96., 96.);
    }
}
void OneShot::releaseVoice(Voice &v, const std::array<double, Count> &p) noexcept {
    v.held = false;
    v.sustained = false;
    v.stage = 3;
    v.releaseStep = v.env / std::max(1., rate_ * (p[Mode] == 0 ? p[Release] : p[FadeOut]) * .001);
}
void OneShot::event(const engine::Event &e, const Sample *s,
                    std::array<double, Count> &p) noexcept {
    if ((e.type == engine::EventType::ParamValue || e.type == engine::EventType::ParamMod) &&
        e.paramId < Count && std::isfinite(e.value)) {
        const auto id = static_cast<int>(e.paramId);
        if (e.type == engine::EventType::ParamMod)
            modulation_[e.paramId] = e.value;
        else
            values_[e.paramId].store(normal(id, real(id, e.value)), std::memory_order_relaxed);
        p[e.paramId] = real(id, values_[e.paramId].load(std::memory_order_relaxed) +
                                  modulation_[e.paramId]);
    } else if (e.type == engine::EventType::NoteOn && s)
        noteOn(e, *s, p);
    else if (e.type == engine::EventType::NoteOff) {
        for (auto &v : voices_)
            if (v.active && v.held && (e.noteId ? v.id == e.noteId : v.channel == e.channel && v.key == e.dim)) {
                if (p[Mode] != 0 && p[Gate] == 0)
                    continue;
                if (midiChannels_[v.channel].sustain)
                    v.sustained = true;
                else
                    releaseVoice(v, p);
            }
    } else if (e.type == engine::EventType::NoteExpression && e.noteId != 0 &&
               e.dim == static_cast<std::uint16_t>(ExpressionDim::Pitch) &&
               std::isfinite(e.value)) {
        for (auto &v : voices_)
            if (v.active && v.id == e.noteId)
                v.tuning = std::clamp(e.value, -96., 96.);
    } else {
        rememberMidiControl(midiChannels_, e);
        if (e.type == engine::EventType::Controller && e.paramId == 64 && std::isfinite(e.value) && e.value < .5)
            for (auto &v : voices_)
                if (v.active && v.sustained && v.channel == e.channel)
                    releaseVoice(v, p);
    }
}
void OneShot::process(const engine::NodeIo &io) noexcept {
    engine::SnapshotPublisher<Publication>::AudioRead publication(publisher_);
    const Sample *s = publication.valid() ? publication->sample.get() : nullptr;
    processSample(io, s, publication.valid() ? publication->seq : 0, false);
}
void OneShot::processPrepared(const engine::NodeIo& io, const PreparedSample* sample,
                             std::uint64_t generation) noexcept {
    processSample(io, generation ? sample : nullptr, generation, true);
}
void OneShot::processSample(const engine::NodeIo& io, const Sample* s,
                           std::uint64_t generation, bool borrowed) noexcept {
    if (!s || sampleSeq_ != generation || borrowed_ != borrowed) {
        voices_ = {};
        sampleSeq_ = generation;
        borrowed_ = borrowed;
    }
    std::array<double, Count> p{};
    for (int i = 0; i < Count; ++i)
        p[static_cast<std::size_t>(i)] =
            real(i, values_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) +
                        modulation_[static_cast<std::size_t>(i)]);
    int next = 0;
    // Silent and no note to come this block: the events still apply, in
    // order, and the block is silence (a voice's filter is its own, so an
    // idle OneShot has no tail). The Sampler runs 32 of these a block.
    bool sounding = false;
    for (const auto &v : voices_)
        sounding = sounding || v.active;
    bool noteComing = false;
    if (s)
        for (int k = 0; k < io.events.count; ++k)
            noteComing = noteComing || io.events.first[k].type == engine::EventType::NoteOn;
    if (!sounding && !noteComing) {
        const int last = io.blockOffset + io.frames - 1;
        while (next < io.events.count && io.events.first[next].frame <= last) {
            event(io.events.first[next], s, p);
            ++next;
        }
        if (io.out)
            for (int c = 0; c < io.channels; ++c)
                if (io.out[c])
                    std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.f);
        return;
    }
    // Live 12.4 shows Sustain in dB, -70 dB to 0 dB (its level runs
    // 0.000316..1); Vel > Vol blends a fixed level with the velocity. Both
    // laws are ADI's until a Live render measures them (DEV-21). These read
    // only the parameters, so they change only at an event.
    // With every LFO amount at 0 the LFO's value reaches nothing (each use is
    // a product with 0, and x + (-0) is x), so only its phase runs. With LFO
    // filter at 0 the filter's coefficients are the block's (exp2(+-0) is
    // exactly 1), not each voice's each sample.
    double sustain = 0, velocityAmount = 0, level = 0, gFixed = 0, qFixed = 0, rFixed = 0, dFixed = 0;
    double lfoStep = 0;  // the LFO's phase a sample: block-constant
    bool classic = true, lfoHeard = true, filterFixed = false;
    const auto coefficients = [&](double lfo, double depth, double &g, double &q) {
        const double hz = std::clamp(p[Cutoff] * std::exp2(lfo * p[LfoFilter] * depth), 5., rate_ * .45);
        g = std::tan(pi * hz / rate_);
        // Res 0..125 % (Live's 0..1.25) onto the state-variable filter's Q
        // from 1/sqrt(2) to 10: ADI's law, unmeasured.
        const double res = p[Resonance] / 125.;
        q = .7071067811865476 + res * res * 9.292893218813452;
    };
    const auto settle = [&] {
        sustain = std::pow(10., p[Sustain] / 20.);
        velocityAmount = p[VelToVol] / 100.;
        level = std::pow(10., (p[Gain] + p[Volume]) / 20.);
        classic = p[Mode] == 0;
        lfoHeard = p[LfoPitch] != 0 || p[LfoVolume] != 0 || p[LfoPan] != 0 || p[LfoFilter] != 0;
        filterFixed = p[FilterOn] > 0 && p[LfoFilter] == 0;
        const double hz =
            p[LfoSync] > 0 ? (io.transport ? io.transport->bpm : 120.) / 60 / p[LfoBeat] : p[LfoRate];
        lfoStep = hz / rate_;
        if (filterFixed) {
            coefficients(0, 1, gFixed, qFixed);
            rFixed = 1 / qFixed;
            dFixed = 1 + gFixed * (gFixed + rFixed);
        }
    };
    settle();
    // Voice by voice over each stretch between events (a voice's state stays
    // in registers across the stretch), in stretches of at most kStretch for
    // the accumulator on the stack. Each output sample still sums the voices
    // from 0 in voice order, so the result is the sample-by-sample loop's,
    // bit for bit.
    constexpr int kStretch = 64;
    double acc[2][kStretch];
    const Travel travel = travel_;  // SC-483: this call's, for every voice
    for (int i = 0; i < io.frames;) {
        const int frame = io.blockOffset + i;
        bool moved = false;
        while (next < io.events.count && io.events.first[next].frame <= frame) {
            event(io.events.first[next], s, p);
            ++next;
            moved = true;
        }
        if (moved)
            settle();
        int n = std::min(io.frames - i, kStretch);
        if (next < io.events.count)
            n = std::clamp(io.events.first[next].frame - frame, 1, n);
        for (int c = 0; c < 2; ++c)
            std::fill_n(acc[c], n, 0.);
        if (s)
            for (auto &v : voices_)
                for (int k = 0; k < n && v.active; ++k) {
                    // DF-E37 (d): a Classic voice with Loop on keeps looping
                    // through its release (Live's manual for Sampler; reading 3
                    // in the devices log for Simpler) and ends when the release
                    // reaches zero, not at the loop's end with a click.
                    const bool looping = classic && p[LoopOn] > 0 && (v.held || v.stage == 3);
                    // SC-483 (the Sampler's layers; a fresh Travel changes
                    // nothing here): after note-off a voice may leave its
                    // sustain loop for the rest of the sample.
                    if (looping && v.stage == 3 && travel.release != 0 && !v.tail) {
                        v.tail = true;
                        v.backward = false;
                    }
                    const bool sustainLoop = looping && !v.tail;
                    const bool tailLoop = looping && v.tail && travel.release >= 2;
                    // The loop the voice is in, if it is in one: from..to.
                    const double from = sustainLoop ? v.loop : v.releaseLoop;
                    const double to = sustainLoop ? v.loopEnd : v.end;
                    const double fadePercent = sustainLoop ? p[Fade] : travel.releaseFade * 100;
                    const bool turns = sustainLoop ? travel.pingPong : travel.release == 3;
                    if (v.backward && !(sustainLoop || tailLoop))
                        v.backward = false;
                    if (v.backward) {
                        if (v.position <= from) {
                            v.position = std::min(from + (from - v.position), to);
                            v.backward = false;
                        }
                    } else if (v.position >= to) {
                        if (!(sustainLoop || tailLoop)) {
                            v.active = false;
                            break;
                        }
                        if (turns) {
                            v.position = std::max(to - (v.position - to), from);
                            v.backward = true;
                        } else {
                            const double overlap = (to - from) * fadePercent / 200.;
                            v.position = from + overlap +
                                         std::fmod(v.position - to, to - from - overlap);
                        }
                    }
                    if (v.stage == 3) {
                        v.env = std::max(0., v.env - v.releaseStep);
                        if (v.env == 0) {
                            v.active = false;
                            break;
                        }
                    } else if (classic) {
                        if (v.stage == 0) {
                            v.env =
                                std::min(1., v.env + 1 / std::max(1., rate_ * p[Attack] * .001));
                            if (v.env >= 1)
                                v.stage = 1;
                        } else if (v.stage == 1) {
                            v.env = std::max(sustain,
                                             v.env - (1 - sustain) /
                                                         std::max(1., rate_ * p[Decay] * .001));
                            if (v.env <= sustain)
                                v.stage = 2;
                        } else
                            v.env = sustain;
                    } else
                        v.env = std::min(1., static_cast<double>(v.played + 1) /
                                                 std::max(1., rate_ * p[FadeIn] * .001));
                    double lfo = 0;
                    if (p[LfoOn] > 0) {
                        const int shape = static_cast<int>(p[LfoShape]);
                        if (lfoHeard)
                            lfo = shape == 0   ? std::sin(2 * pi * v.phase)
                                  : shape == 1 ? (v.phase < .5 ? 1. : -1.)
                                  : shape == 2 ? 1 - 4 * std::abs(v.phase - .5)
                                  : shape == 3 ? 1 - 2 * v.phase
                                  : shape == 4 ? 2 * v.phase - 1
                                               : v.random;
                        v.phase += lfoStep;
                        if (v.phase >= 1) {
                            v.phase -= std::floor(v.phase);
                            v.rng = 1664525u * v.rng + 1013904223u;
                            v.random = static_cast<double>(v.rng) / 2147483648. - 1;
                        }
                    }
                    const auto &midi = midiChannels_[v.channel];
                    // SC-483: a loop's own Detune, while the voice is inside
                    // that loop (a fresh Travel adds 0, which changes no pitch).
                    const double loopCents = (sustainLoop || tailLoop) && v.position >= from
                                                 ? (sustainLoop ? travel.loopCents : travel.releaseCents)
                                                 : 0.;
                    const double pitch = (p[Mode] == 2 ? 0 : v.key - rootKey_) + p[Transpose] +
                                         p[Detune] / 100. + v.tuning + midi.bend + lfo * p[LfoPitch] * midi.modulation +
                                         loopCents / 100.;
                    // exp2 only when the pitch moves (an LFO, an expression):
                    // the same pitch gives the same increment, bit for bit.
                    if (!(pitch == v.pitchSeen)) {
                        v.pitchSeen = pitch;
                        v.increment = s->rate / rate_ * std::exp2(pitch / 12.);
                    }
                    const double increment = v.increment;
                    double amplitude = v.gain * v.env * (1 - velocityAmount + velocityAmount * v.velocity) *
                                       level * (1 - p[LfoVolume] / 200. * midi.modulation * (1 - lfo));
                    if (!classic)
                        amplitude *= std::min(1., (v.end - v.position) / increment /
                                                      std::max(1., rate_ * p[FadeOut] * .001));
                    const double fade =
                        (sustainLoop || tailLoop) && !turns ? (to - from) * fadePercent / 200. : 0;
                    // The filter's coefficients: the block's, or this voice's
                    // this sample when the LFO moves the cutoff.
                    double g = gFixed, q = qFixed, r = rFixed, d = dFixed;
                    if (p[FilterOn] > 0 && !filterFixed) {
                        coefficients(lfo, midi.modulation, g, q);
                        r = 1 / q;
                        d = 1 + g * (g + r);
                    }
                    double both[2];
                    readBoth(*s, v.reversed ? v.mirror - v.position : v.position, both, travel.interpolation);
                    for (int c = 0; c < 2; ++c) {
                        double x = both[c];
                        if (fade > 0 && v.position > to - fade) {
                            const auto t = (v.position - (to - fade)) / fade;
                            const double entering = from + v.position - (to - fade);
                            const double other =
                                read(*s, v.reversed ? v.mirror - entering : entering, c, travel.interpolation);
                            x = travel.linearFade
                                    ? x * (1 - t) + other * t
                                    : x * std::cos(t * pi * .5) + other * std::sin(t * pi * .5);
                        }
                        if (p[FilterOn] > 0) {
                            x = v.filters[c][0].run(x, g, r, d, static_cast<int>(p[FilterType]));
                            if (p[Slope] > 0)
                                x = v.filters[c][1].run(x, g, r, d, static_cast<int>(p[FilterType]));
                        }
                        const double pan = lfo * p[LfoPan] * midi.modulation / 100.;
                        // sqrt(1) is exactly 1: no pan, no square root.
                        acc[c][k] += x * amplitude *
                                     (pan == 0 ? 1. : std::sqrt(std::clamp(1 + (c == 0 ? -pan : pan), 0., 2.)));
                    }
                    if (v.backward)
                        v.position -= increment;
                    else
                        v.position += increment;
                    ++v.played;
                }
        if (io.out)
            for (int c = 0; c < io.channels; ++c)
                if (io.out[c])
                    for (int k = 0; k < n; ++k)
                        io.out[c][frame + k] = static_cast<float>(c < 2 ? acc[c][k] : 0);
        i += n;
    }
}
} // namespace adi::device
