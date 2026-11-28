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
                       {"vel_to_vol", "Vel > Vol", "%", 0, 100, 35, 0},
                       // SC-482. Times take the amp envelope's ranges and
                       // defaults; Sustain is a level (Live shows these two
                       // envelopes' Sustain as a level, not in dB). The amounts
                       // are Live's Fe < Env (+-72 st) and Pe < Env (+-48 st)
                       // as the parity list names them; the defaults of the
                       // times and sustains are a reading still to take, and
                       // play nothing while the amount is 0 or Pitch env off.
                       {"fe_attack", "Filter env attack", "ms", .1, 20000, .1, 0},
                       {"fe_decay", "Filter env decay", "ms", 1, 60000, 600, 0},
                       {"fe_sustain", "Filter env sustain", "%", 0, 100, 0, 0},
                       {"fe_release", "Filter env release", "ms", 1, 60000, 50, 0},
                       {"fe_amount", "Filter env amount", "st", -72, 72, 0, 0},
                       {"pe_on", "Pitch env", "", 0, 1, 0, 2},
                       {"pe_attack", "Pitch env attack", "ms", .1, 20000, .1, 0},
                       {"pe_decay", "Pitch env decay", "ms", 1, 60000, 600, 0},
                       {"pe_sustain", "Pitch env sustain", "%", 0, 100, 0, 0},
                       {"pe_release", "Pitch env release", "ms", 1, 60000, 50, 0},
                       {"pe_amount", "Pitch env amount", "st", -48, 48, 0, 0},
                       // SC-482: Live's two circuit choosers, Drive (0..24 dB,
                       // for every circuit but Clean, on low-, high- and
                       // band-pass), and the cutoff's velocity and key amounts.
                       {"filter_circuit", "Filter circuit (LP/HP)", "", 0, 4, 0, 5},
                       {"filter_circuit_bp", "Filter circuit (BP/Notch)", "", 0, 1, 0, 2},
                       {"drive", "Filter drive", "dB", 0, 24, 0, 0},
                       {"filter_vel", "Filter < Vel", "%", 0, 100, 0, 0},
                       {"filter_key", "Filter < Key", "%", 0, 100, 0, 0},
                       // SC-482: the LFO's Attack (its depth fades in), its
                       // Retrigger (on: each note starts it at Offset, as
                       // every note did before; off: it joins the free-running
                       // LFO), Offset in degrees, and its rate's key amount.
                       // The Attack range is a reading still to take.
                       {"lfo_attack", "LFO attack", "ms", 0, 10000, 0, 0},
                       {"lfo_retrigger", "LFO retrigger", "", 0, 1, 1, 2},
                       {"lfo_offset", "LFO offset", "deg", 0, 360, 0, 0},
                       {"lfo_key", "LFO < Key", "%", 0, 100, 0, 0},
                       // SC-482: Glide (mono, legato: a note played over a
                       // held one slides the same voice to it) and Portamento
                       // (polyphonic: each new note slides from the last
                       // one's pitch), both only from a note still sounding.
                       // Off is what OneShot did. The Time range is a reading.
                       {"glide_mode", "Glide", "", 0, 2, 0, 3},
                       {"glide_time", "Glide time", "ms", 0, 5000, 50, 0}};
// SC-482: Filter < Vel at 100 % opens the cutoff this far at full velocity
// (ADI's span: Live's is a reading still to take); a softer note, less.
constexpr double kFilterVelocityOctaves = 4;
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
            if (i == FilterCircuit)
                return std::array{"Clean", "OSR", "MS2", "SMP", "PRD"}[k];
            if (i == FilterCircuitBp)
                return std::array{"Clean", "OSR"}[k];
            if (i == GlideMode)
                return std::array{"Off", "Glide", "Portamento"}[k];
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
    lfoClock_ = 0;
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
        // SC-482: a state saved before a control existed holds fewer values
        // (41 before SC-482); each missing one takes its default, which plays
        // what that state played. More than this build knows is refused.
        const auto saved = j.at("params").size();
        if (j.at("version") != 1 || saved < static_cast<std::size_t>(kLegacyCount) ||
            saved > static_cast<std::size_t>(Count))
            return false;
        std::array<double, Count> values{};
        for (int i = 0; i < Count; ++i) {
            auto &v = values[static_cast<std::size_t>(i)];
            if (static_cast<std::size_t>(i) >= saved) {
                v = params_[static_cast<std::size_t>(i)].defaultValue.normalized;
                continue;
            }
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
double OneShot::Filter::runCircuit(double x, double g, double r, double d, int type, int circuit,
                                   double drive) noexcept {
    // ADI's circuits, each a saturation around Clean's stage (unit slope at
    // 0, so a quiet signal hears little of it): OSR clips its resonance (the
    // band state); MS2 soft-clips its input and its resonance; SMP shapes its
    // input asymmetrically and clips its resonance harder; PRD leaves its input
    // clean and lets the resonance overdrive. Live's own laws are readings.
    x *= drive;
    if (circuit == Ms2)
        x = std::tanh(x);
    else if (circuit == Smp)
        x = x >= 0 ? std::tanh(x) : std::tanh(1.5 * x) / 1.5;
    const double v1 = (a + g * (x - b)) / d;
    const double v2 = b + g * v1;
    a = 2 * v1 - a;
    b = 2 * v2 - b;
    if (circuit == Osr || circuit == Ms2)
        a = std::tanh(a);
    else if (circuit == Smp)
        a = a / std::sqrt(1 + a * a);
    else if (circuit == Prd)
        a = 1.5 * std::tanh(a / 1.5);
    if (std::abs(a) < 1e-30) {
        a = 0;
    }
    if (std::abs(b) < 1e-30) {
        b = 0;
    }
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
void OneShot::readBoth(const Sample &s, double position, double *x) const noexcept {
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
        x[c] = y + (s.data[b * stride + ch] - y) * t;
    }
}
void OneShot::queueNoteGain(double gain) noexcept {
    if (noteGainCount_ == noteGains_.size() || !std::isfinite(gain)) return;
    noteGains_[(noteGainHead_ + noteGainCount_) % noteGains_.size()] = std::max(0., gain);
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
    // SC-482: Glide and Portamento slide from the pitch the newest sounding
    // note is at (its key and what it still has to slide). A slice's pitch is
    // not its key's, so Slice mode does not glide.
    const int glide = mode == 2 ? 0 : static_cast<int>(p[GlideMode]);
    double from = std::numeric_limits<double>::quiet_NaN();
    if (glide != 0) {
        const Voice *newest = nullptr;
        for (const auto &w : voices_)
            if (w.active && (!newest || w.age > newest->age))
                newest = &w;
        if (newest)
            from = newest->key + newest->glide;
    }
    const double glideSamples = std::max(1., rate_ * p[GlideTime] * .001);
    if (glide == 1 && voices_[0].active && voices_[0].held) {
        // Glide is legato: the held voice keeps its envelopes and its place
        // in the sample, takes the new note's identity and slides to its key.
        auto &v = voices_[0];
        if (noteGainCount_ > 0) {
            noteGainHead_ = (noteGainHead_ + 1) % noteGains_.size();
            --noteGainCount_;
        }
        v.glide = from - e.dim;
        v.glideStep = v.glide / glideSamples;
        v.key = e.dim;
        v.id = e.noteId;
        v.channel = e.channel;
        v.age = ++age_;
        v.sustained = false;
        return;
    }
    const bool mono = mode == 1 || (mode == 2 && p[SlicePlayback] != 1) || glide == 1;
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
    if (noteGainCount_ > 0) {
        v.gain = noteGains_[noteGainHead_];
        noteGainHead_ = (noteGainHead_ + 1) % noteGains_.size();
        --noteGainCount_;
    }
    v.rng = static_cast<std::uint32_t>(v.age) | 1u;
    // SC-482: Retrigger on starts the LFO at Offset (0 is where every note's
    // started before); off, at the free-running LFO's phase.
    v.phase = p[LfoRetrigger] > 0 ? p[LfoOffset] / 360. : lfoClock_;
    v.phase -= std::floor(v.phase);
    if (!std::isnan(from)) {
        v.glide = from - e.dim;
        v.glideStep = v.glide / glideSamples;
    }
    v.begin = begin;
    v.end = end;
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
    // SC-482: the filter and pitch envelopes release with the note, each in
    // its own time, whatever the mode's amp release is.
    v.filterEnv.release(rate_ * p[FilterEnvRelease] * .001);
    v.pitchEnv.release(rate_ * p[PitchEnvRelease] * .001);
}
void OneShot::Envelope::run(double attackSamples, double decaySamples, double sustain) noexcept {
    // The amp envelope's arithmetic: a step, then the value is the sample's.
    if (stage == 0) {
        value = std::min(1., value + 1 / std::max(1., attackSamples));
        if (value >= 1)
            stage = 1;
    } else if (stage == 1) {
        value = std::max(sustain, value - (1 - sustain) / std::max(1., decaySamples));
        if (value <= sustain)
            stage = 2;
    } else if (stage == 2) {
        value = sustain;
    } else {
        value = std::max(0., value - releaseStep);
    }
}
void OneShot::Envelope::release(double releaseSamples) noexcept {
    stage = 3;
    releaseStep = value / std::max(1., releaseSamples);
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
    const auto lfoHz = [&] {
        return p[LfoSync] > 0 ? (io.transport ? io.transport->bpm : 120.) / 60 / p[LfoBeat]
                              : p[LfoRate];
    };
    if (!sounding && !noteComing) {
        const int last = io.blockOffset + io.frames - 1;
        while (next < io.events.count && io.events.first[next].frame <= last) {
            event(io.events.first[next], s, p);
            ++next;
        }
        // The free-running LFO runs on through silence (SC-482).
        lfoClock_ += io.frames * lfoHz() / rate_;
        lfoClock_ -= std::floor(lfoClock_);
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
    // SC-482: the filter and pitch envelopes' segments, in samples, and their
    // sustain levels; constant between events, like every parameter here.
    double feAttack = 0, feDecay = 0, feSustain = 0, peAttack = 0, peDecay = 0, peSustain = 0;
    bool classic = true, lfoHeard = true, filterFixed = false, filterEnvHeard = false,
         pitchEnvHeard = false;
    // SC-482: the circuit the chosen type takes, its input gain, and the
    // cutoff's key (octaves an octave of pitch) and velocity (octaves at 1).
    int filterType = 0, circuit = Clean;
    double driveGain = 1, filterKeyAmount = 0, filterVelAmount = 0;
    // SC-482: the LFO's attack in samples (0: none) and its rate's key amount.
    double lfoAttack = 0, lfoKeyAmount = 0;
    // The cutoff moved by `octaves`: 0 is the knob's own cutoff (exp2(0) is 1).
    const auto coefficients = [&](double octaves, double &g, double &q) {
        const double hz = std::clamp(p[Cutoff] * std::exp2(octaves), 5., rate_ * .45);
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
        filterEnvHeard = p[FilterEnvAmount] != 0;
        pitchEnvHeard = p[PitchEnvOn] > 0 && p[PitchEnvAmount] != 0;
        filterType = static_cast<int>(p[FilterType]);
        circuit = static_cast<int>(filterType < 2 ? p[FilterCircuit] : p[FilterCircuitBp]);
        // Drive: every circuit but Clean, on low-, high- and band-pass.
        driveGain = circuit != Clean && filterType != 3 ? std::pow(10., p[Drive] / 20.) : 1.;
        filterKeyAmount = p[FilterKey] / 100.;
        filterVelAmount = p[FilterVel] / 100. * kFilterVelocityOctaves;
        filterFixed = p[FilterOn] > 0 && p[LfoFilter] == 0 && !filterEnvHeard &&
                      filterKeyAmount == 0 && filterVelAmount == 0;
        lfoStep = lfoHz() / rate_;
        lfoAttack = rate_ * p[LfoAttack] * .001;
        lfoKeyAmount = p[LfoKey] / 100.;
        feAttack = rate_ * p[FilterEnvAttack] * .001;
        feDecay = rate_ * p[FilterEnvDecay] * .001;
        feSustain = p[FilterEnvSustain] / 100.;
        peAttack = rate_ * p[PitchEnvAttack] * .001;
        peDecay = rate_ * p[PitchEnvDecay] * .001;
        peSustain = p[PitchEnvSustain] / 100.;
        // A voice's cached coefficients were for the old cutoff and resonance.
        for (auto &v : voices_)
            v.cutoffSeen = std::numeric_limits<double>::quiet_NaN();
        if (filterFixed) {
            coefficients(0., gFixed, qFixed);
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
            for (auto &v : voices_) {
                // SC-482: LFO < Key scales this voice's LFO rate by its pitch
                // from C3 (100 %: an octave up, twice the rate).
                const double voiceLfoStep =
                    lfoKeyAmount == 0 ? lfoStep
                                      : lfoStep * std::exp2((v.key - 60) / 12. * lfoKeyAmount);
                for (int k = 0; k < n && v.active; ++k) {
                    // DF-E37 (d): a Classic voice with Loop on keeps looping
                    // through its release (Live's manual for Sampler; reading 3
                    // in the devices log for Simpler) and ends when the release
                    // reaches zero, not at the loop's end with a click.
                    const bool looping = classic && p[LoopOn] > 0 && (v.held || v.stage == 3);
                    if (v.position >= v.end) {
                        if (looping) {
                            const double overlap = (v.end - v.loop) * p[Fade] / 200.;
                            v.position = v.loop + overlap +
                                         std::fmod(v.position - v.end, v.end - v.loop - overlap);
                        } else {
                            v.active = false;
                            break;
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
                    // SC-482: they run whether heard or not, so an amount
                    // raised mid-note finds the envelope where it would be.
                    v.filterEnv.run(feAttack, feDecay, feSustain);
                    v.pitchEnv.run(peAttack, peDecay, peSustain);
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
                        // SC-482: LFO Attack fades its depth in from note-on.
                        if (lfoAttack > 0)
                            lfo *= std::min(1., static_cast<double>(v.played + 1) / lfoAttack);
                        v.phase += voiceLfoStep;
                        if (v.phase >= 1) {
                            v.phase -= std::floor(v.phase);
                            v.rng = 1664525u * v.rng + 1013904223u;
                            v.random = static_cast<double>(v.rng) / 2147483648. - 1;
                        }
                    }
                    const auto &midi = midiChannels_[v.channel];
                    double pitch = (p[Mode] == 2 ? 0 : v.key - rootKey_) + p[Transpose] +
                                   p[Detune] / 100. + v.tuning + midi.bend + lfo * p[LfoPitch] * midi.modulation;
                    if (pitchEnvHeard)
                        pitch += v.pitchEnv.value * p[PitchEnvAmount];
                    if (v.glide != 0) {
                        pitch += v.glide;
                        v.glide = std::abs(v.glide) <= std::abs(v.glideStep) ? 0 : v.glide - v.glideStep;
                    }
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
                    const double fade = looping ? (v.end - v.loop) * p[Fade] / 200. : 0;
                    // The filter's coefficients: the block's, or this voice's
                    // this sample when the LFO moves the cutoff.
                    double g = gFixed, r = rFixed, d = dFixed;
                    if (p[FilterOn] > 0 && !filterFixed) {
                        double octaves = lfo * p[LfoFilter] * midi.modulation;
                        if (filterEnvHeard)
                            octaves += v.filterEnv.value * p[FilterEnvAmount] / 12.;
                        if (filterKeyAmount != 0)
                            octaves += (p[Mode] == 2 ? 0 : v.key - rootKey_) / 12. * filterKeyAmount;
                        if (filterVelAmount != 0)
                            octaves += v.velocity * filterVelAmount;
                        if (!(octaves == v.cutoffSeen)) {
                            double q = 0;
                            v.cutoffSeen = octaves;
                            coefficients(octaves, v.g, q);
                            v.r = 1 / q;
                            v.d = 1 + v.g * (v.g + v.r);
                        }
                        g = v.g;
                        r = v.r;
                        d = v.d;
                    }
                    double both[2];
                    readBoth(*s, v.position, both);
                    for (int c = 0; c < 2; ++c) {
                        double x = both[c];
                        if (fade > 0 && v.position > v.end - fade) {
                            const auto t = (v.position - (v.end - fade)) / fade;
                            x = x * std::cos(t * pi * .5) +
                                read(*s, v.loop + v.position - (v.end - fade), c) *
                                    std::sin(t * pi * .5);
                        }
                        if (p[FilterOn] > 0 && circuit != Clean) {
                            x = v.filters[c][0].runCircuit(x, g, r, d, filterType, circuit, driveGain);
                            if (p[Slope] > 0)
                                x = v.filters[c][1].runCircuit(x, g, r, d, filterType, circuit, 1.);
                        } else if (p[FilterOn] > 0) {
                            x = v.filters[c][0].run(x, g, r, d, filterType);
                            if (p[Slope] > 0)
                                x = v.filters[c][1].run(x, g, r, d, filterType);
                        }
                        const double pan = lfo * p[LfoPan] * midi.modulation / 100.;
                        // sqrt(1) is exactly 1: no pan, no square root.
                        acc[c][k] += x * amplitude *
                                     (pan == 0 ? 1. : std::sqrt(std::clamp(1 + (c == 0 ? -pan : pan), 0., 2.)));
                    }
                    v.position += increment;
                    ++v.played;
                }
            }
        lfoClock_ += n * lfoStep;
        lfoClock_ -= std::floor(lfoClock_);
        if (io.out)
            for (int c = 0; c < io.channels; ++c)
                if (io.out[c])
                    for (int k = 0; k < n; ++k)
                        io.out[c][frame + k] = static_cast<float>(c < 2 ? acc[c][k] : 0);
        i += n;
    }
}
} // namespace adi::device
