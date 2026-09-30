// SPDX-License-Identifier: GPL-3.0-or-later
#include "sampler.hpp"
#include <cmath>
#include <nlohmann/json.hpp>
namespace adi::device {
Sampler::Sampler() {
    OneShot source;
    for (int i : {OneShot::Volume,    OneShot::Transpose,  OneShot::Attack,   OneShot::Decay,
                  OneShot::Sustain,   OneShot::Release,    OneShot::Voices,   OneShot::Retrigger,
                  OneShot::FilterOn,  OneShot::FilterType, OneShot::Slope,    OneShot::Cutoff,
                  OneShot::Resonance, OneShot::LfoOn,      OneShot::LfoShape, OneShot::LfoRate,
                  OneShot::LfoSync,   OneShot::LfoBeat,    OneShot::LfoPitch, OneShot::LfoVolume,
                  OneShot::LfoPan,    OneShot::LfoFilter}) {
        params_.push_back(*source.paramAt(i));
        destinations_.push_back(static_cast<std::uint32_t>(i));
    }
    ParamDescriptor select;
    select.id = "sample_select";
    select.name = "Sample Select";
    select.hasRealRange = true;
    select.minReal = 0;
    select.maxReal = 127;
    select.domain = ParamDomain::Real;
    select.defaultValue = ParamValue::withReal(0, 0);
    params_.push_back(select);
    destinations_.push_back(OneShot::Count);
    for (std::size_t i = 0; i < params_.size(); ++i)
        values_[i].store(params_[i].defaultValue.normalized);
    prepare(48000, 4096);
}
const ParamDescriptor *Sampler::paramAt(std::int32_t i) const noexcept {
    return i >= 0 && static_cast<std::size_t>(i) < params_.size()
               ? &params_[static_cast<std::size_t>(i)]
               : nullptr;
}
ParamValue Sampler::getParam(const std::string &id) const noexcept {
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (params_[i].id == id) {
            double n = values_[i].load(std::memory_order_relaxed);
            return ParamValue::withReal(n, params_[i].minReal +
                                               n * (params_[i].maxReal - params_[i].minReal));
        }
    return {};
}
bool Sampler::setParam(const std::string &id, const ParamValue &v) {
    if (!std::isfinite(v.normalized))
        return false;
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (params_[i].id == id) {
            values_[i].store(std::clamp(v.normalized, 0., 1.), std::memory_order_relaxed);
            return true;
        }
    return false;
}
bool Sampler::valid(const Zone &z) noexcept {
    for (const auto &r : {z.key, z.velocity, z.select})
        if (!std::isfinite(r.low) || !std::isfinite(r.high) || !std::isfinite(r.fadeLow) ||
            !std::isfinite(r.fadeHigh) || r.low < 0 || r.high > 127 || r.high < r.low ||
            r.fadeLow < 0 || r.fadeHigh < 0 || r.fadeLow + r.fadeHigh > r.high - r.low)
            return false;
    for (double v : {z.start, z.end, z.loopStart, z.crossfade, z.gainDb, z.pan, z.detune})
        if (!std::isfinite(v))
            return false;
    return z.root >= 0 && z.root <= 127 && z.start >= 0 && z.end <= 1 && z.end > z.start &&
           z.loopStart >= z.start && z.loopStart < z.end && z.crossfade >= 0 && z.crossfade <= 1 &&
           z.gainDb >= -90 && z.gainDb <= 24 && std::abs(z.pan) <= 1 && std::abs(z.detune) <= 50;
}
double Sampler::weight(const Range &r, double x, bool power) noexcept {
    if (x < r.low || x > r.high)
        return 0;
    double w = 1;
    if (r.fadeLow > 0)
        w = std::min(w, (x - r.low) / r.fadeLow);
    if (r.fadeHigh > 0)
        w = std::min(w, (r.high - x) / r.fadeHigh);
    return power ? std::sin(w * 1.5707963267948966) : w;
}
void Sampler::configure(Layer &l) {
    auto set = [&](int i, double real) {
        const auto *p = l.engine->paramAt(i);
        l.engine->setParam(
            p->id, ParamValue::fromNormalized((real - p->minReal) / (p->maxReal - p->minReal)));
    };
    const auto &z = l.zone;
    l.engine->setRootKey(z.root);
    set(OneShot::Mode, 0);
    set(OneShot::Start, z.start * 100);
    set(OneShot::End, z.end * 100);
    set(OneShot::Length, 100);
    set(OneShot::LoopOn, z.loop ? 1 : 0);
    set(OneShot::LoopLength, 100 * (z.end - z.loopStart) / (z.end - z.start));
    set(OneShot::Fade, z.crossfade * 100);
    set(OneShot::Detune, z.detune);
    set(OneShot::Gain, 0);
    l.engine->prepare(rate_, maxFrames_);
}
void Sampler::publish(std::unique_ptr<Bank> bank) {
    messageBank_ = bank.get();
    publisher_.publish(std::move(bank));
    publisher_.collect();
}
bool Sampler::publishZones(std::span<const Zone> zones) {
    if (zones.size() > 32)
        return false;
    auto bank = std::make_unique<Bank>();
    std::size_t total = 0;
    for (const auto &z : zones) {
        if (!valid(z))
            return false;
        total += z.samples.size();
        if (total > 32 * 1024 * 1024)
            return false;
        Layer l;
        l.zone = z;
        l.engine = std::make_unique<OneShot>();
        if (!l.engine->publishSample(z.samples, z.channels, z.sampleRate))
            return false;
        l.zone.samples.clear();
        l.zone.samples.shrink_to_fit();
        configure(l);
        bank->layers.push_back(std::move(l));
    }
    publish(std::move(bank));
    return true;
}
void Sampler::prepare(double rate, std::int32_t frames) {
    rate_ = std::isfinite(rate) && rate >= 1000 && rate <= 768000 ? rate : 48000;
    maxFrames_ = std::max(1, frames);
    scratch_.resize(static_cast<std::size_t>(maxFrames_) * 2);
    events_.resize(
        static_cast<std::size_t>(engine::Graph::deriveEventCapacity(rate_, maxFrames_, 32)) +
        params_.size());
    modulation_.fill(0);
    if (messageBank_)
        for (auto &l : messageBank_->layers)
            l.engine->prepare(rate_, maxFrames_);
}
void Sampler::process(const engine::NodeIo &io) noexcept {
    if (io.frames <= 0 || io.blockOffset < 0 || io.blockOffset + io.frames > maxFrames_)
        return;
    if (io.out)
        for (int c = 0; c < io.channels; ++c)
            if (io.out[c])
                std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.f);
    engine::SnapshotPublisher<Bank>::AudioRead bank(publisher_);
    std::array<double, OneShot::Count> initial{};
    for (std::size_t i = 0; i < params_.size(); ++i)
        initial[i] = values_[i].load(std::memory_order_relaxed);
    if (bank.valid())
        for (const auto &layer : bank->layers) {
            for (std::size_t i = 0; i + 1 < params_.size(); ++i)
                layer.engine->setParam(params_[i].id, ParamValue::fromNormalized(initial[i]));
            engine::EventList events(events_.data(), static_cast<int>(events_.size()));
            for (std::size_t i = 0; i + 1 < params_.size(); ++i) {
                engine::Event e;
                e.type = engine::EventType::ParamMod;
                e.frame = io.blockOffset;
                e.paramId = destinations_[i];
                e.value = modulation_[i];
                events.push(e);
            }
            double select = initial[params_.size() - 1],
                   selectMod = modulation_[params_.size() - 1];
            for (const auto &input : io.events) {
                auto e = input;
                if (e.type == engine::EventType::ParamValue ||
                    e.type == engine::EventType::ParamMod) {
                    if (e.paramId >= params_.size() || !std::isfinite(e.value))
                        continue;
                    if (e.paramId == params_.size() - 1) {
                        if (e.type == engine::EventType::ParamValue)
                            select = std::clamp(e.value, 0., 1.);
                        else
                            selectMod = e.value;
                        continue;
                    }
                    e.paramId = destinations_[e.paramId];
                } else if (e.type == engine::EventType::NoteOn && e.value > 0) {
                    const auto &z = layer.zone;
                    double w = weight(z.key, e.dim, z.constantPower) *
                               weight(z.velocity, e.value * 127, z.constantPower) *
                               weight(z.select, std::clamp(select + selectMod, 0., 1.) * 127,
                                      z.constantPower);
                    if (w <= 0)
                        continue;
                    e.value *= w;
                }
                if (!events.push(e) && e.owner && e.type == engine::EventType::NoteOff)
                    e.owner->noteOffRejected(e);
            }
            float *channels[]{scratch_.data(), scratch_.data() + maxFrames_};
            auto child = io;
            child.out = channels;
            child.channels = 2;
            child.events = {events.begin(), events.size()};
            layer.engine->process(child);
            const double gain = std::pow(10., layer.zone.gainDb / 20.);
            if (io.out)
                for (int c = 0; c < std::min(2, io.channels); ++c)
                    if (io.out[c]) {
                        const auto g =
                            gain * std::sqrt(1 + (c == 0 ? -layer.zone.pan : layer.zone.pan));
                        for (int i = io.blockOffset; i < io.blockOffset + io.frames; ++i)
                            io.out[c][i] += static_cast<float>(channels[c][i] * g);
                    }
        }
    for (const auto &e : io.events)
        if (e.paramId < params_.size() && std::isfinite(e.value)) {
            if (e.type == engine::EventType::ParamValue)
                values_[e.paramId].store(std::clamp(e.value, 0., 1.), std::memory_order_relaxed);
            else if (e.type == engine::EventType::ParamMod)
                modulation_[e.paramId] = e.value;
        }
}
std::vector<std::uint8_t> Sampler::saveState(const std::string &role) const {
    if (role != "component")
        return {};
    nlohmann::json j = {{"version", 1},
                        {"zones", nlohmann::json::array()},
                        {"parameters", nlohmann::json::array()}};
    for (std::size_t i = 0; i < params_.size(); ++i)
        j["parameters"].push_back(values_[i].load(std::memory_order_relaxed));
    if (messageBank_)
        for (const auto &l : messageBank_->layers) {
            const auto &z = l.zone;
            auto range = [](const Range &r) {
                return std::array{r.low, r.high, r.fadeLow, r.fadeHigh};
            };
            j["zones"].push_back(
                {{"key", range(z.key)},
                 {"velocity", range(z.velocity)},
                 {"select", range(z.select)},
                 {"root", z.root},
                 {"start", z.start},
                 {"end", z.end},
                 {"loopStart", z.loopStart},
                 {"fade", z.crossfade},
                 {"loop", z.loop},
                 {"power", z.constantPower},
                 {"gain", z.gainDb},
                 {"pan", z.pan},
                 {"detune", z.detune},
                 {"voice", nlohmann::json::binary(l.engine->saveState("component"))}});
        }
    return nlohmann::json::to_cbor(j);
}
bool Sampler::loadState(const std::string &role, const std::vector<std::uint8_t> &bytes) {
    if (role != "component" || bytes.size() > 160 * 1024 * 1024)
        return false;
    try {
        const auto j = nlohmann::json::from_cbor(bytes);
        if (j.at("version") != 1 || j.at("zones").size() > 32 ||
            j.at("parameters").size() != params_.size())
            return false;
        auto values = j.at("parameters").get<std::vector<double>>();
        for (double v : values)
            if (!std::isfinite(v) || v < 0 || v > 1)
                return false;
        auto bank = std::make_unique<Bank>();
        for (const auto &a : j.at("zones")) {
            Layer l;
            auto &z = l.zone;
            auto range = [](const nlohmann::json &r) {
                auto v = r.get<std::array<double, 4>>();
                return Range{v[0], v[1], v[2], v[3]};
            };
            z.key = range(a.at("key"));
            z.velocity = range(a.at("velocity"));
            z.select = range(a.at("select"));
            z.root = a.at("root");
            z.start = a.at("start");
            z.end = a.at("end");
            z.loopStart = a.at("loopStart");
            z.crossfade = a.at("fade");
            z.loop = a.at("loop");
            z.constantPower = a.at("power");
            z.gainDb = a.at("gain");
            z.pan = a.at("pan");
            z.detune = a.at("detune");
            if (!valid(z))
                return false;
            l.engine = std::make_unique<OneShot>();
            if (!l.engine->loadState("component", a.at("voice").get_binary()))
                return false;
            configure(l);
            bank->layers.push_back(std::move(l));
        }
        for (std::size_t i = 0; i < values.size(); ++i)
            values_[i].store(values[i], std::memory_order_relaxed);
        publish(std::move(bank));
        return true;
    } catch (const nlohmann::json::exception &) {
        return false;
    }
}
} // namespace adi::device
