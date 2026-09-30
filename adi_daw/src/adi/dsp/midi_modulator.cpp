// SPDX-License-Identifier: GPL-3.0-or-later
#include "midi_modulator.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
namespace adi::device {
namespace {
bool forward(engine::EventList& out,const engine::Event& e) noexcept {
    if(out.push(e)) return true;
    if(e.owner && (e.type==engine::EventType::NoteOff || (e.type==engine::EventType::NoteOn && e.value==0))) e.owner->noteOffRejected(e);
    return false;
}
}

MidiModulator::MidiModulator(Kind k)
    : kind_(k), identity_{"internal",
                          k == Kind::Expression ? "adi.expression_control"
                          : k == Kind::Shaper   ? "adi.shaper"
                                                : "adi.mpe_control",
                          k == Kind::Expression ? "Expression Control"
                          : k == Kind::Shaper   ? "Shaper"
                                                : "MPE Control",
                          "ADI", "1"} {
    if (k == Kind::Expression) {
        add("source", 0, 9, 0, true);
        add("rise", 0, 1000, 0);
        add("fall", 0, 1000, 0);
        add("curve_x", .001, .999, .5);
        add("curve_y", 0, 1, .5);
        add("s_curve", 0, 1, 0, true);
        add("steps", 1, 32, 8, true);
        add("random", 0, 1, 1);
        add("seed", 1, 16777215, 1, true);
        add("depth", 0, 1, 1);
    }
    if (k == Kind::Shaper) {
        add("rate", .01, 40, 1);
        add("sync", 0, 1, 0, true);
        add("beats", .015625, 32, 1);
        add("loop", 0, 1, 0, true);
        add("velocity", 0, 1, 0);
        add("offset", -1, 1, 0);
        add("jitter", 0, 1, 0);
        add("smooth", 0, 1000, 0);
        add("depth", 0, 1, 1);
        add("echo", 0, .99, 0);
        add("echo_time", 1, 2000, 250);
    }
    if (k == Kind::MPE) {
        add("pitch", 0, 4, 1);
        add("pressure_curve", .125, 8, 1);
        add("slide_curve", .125, 8, 1);
        add("rise", 0, 1000, 0);
        add("fall", 0, 1000, 0);
        add("slide_mode", 0, 2, 0, true);
        add("swap", 0, 1, 0, true);
        add("pressure_to_slide", 0, 1, 0, true);
        add("slide_to_pressure", 0, 1, 0, true);
        add("pressure_to_channel", 0, 1, 0, true);
        add("slide_to_mod", 0, 1, 0, true);
        add("pitch_to_channel", 0, 1, 0, true);
        add("pressure_default", 0, 1, 0);
        add("slide_default", 0, 1, .5);
    }
    config_.points[0] = {0, 0, 0, false};
    config_.points[1] = {.01, 1, 0, false};
    config_.points[2] = {1, 0, 0, false};
    publish();
}
void MidiModulator::add(const std::string &id, double lo, double hi, double def, bool integer) {
    ParamDescriptor p;
    p.id = id;
    p.name = id;
    p.hasRealRange = true;
    p.minReal = lo;
    p.maxReal = hi;
    p.domain = integer ? ParamDomain::Enum : ParamDomain::Real;
    p.shape = integer ? ParamShape::Menu : ParamShape::Continuous;
    p.stepCount = integer ? static_cast<int>(hi - lo + 1) : 0;
    p.defaultValue = ParamValue::withReal((def - lo) / (hi - lo), def);
    values_[params_.size()].store(p.defaultValue.normalized);
    params_.push_back(p);
}
double MidiModulator::real(std::size_t i, double n) const noexcept {
    const auto &p = params_[i];
    const double x = p.minReal + std::clamp(n, 0., 1.) * (p.maxReal - p.minReal);
    return p.stepCount ? std::round(x) : x;
}
double MidiModulator::value(std::size_t i) const noexcept {
    return real(i, values_[i].load(std::memory_order_relaxed));
}
const ParamDescriptor *MidiModulator::paramAt(std::int32_t i) const noexcept {
    return i >= 0 && static_cast<std::size_t>(i) < params_.size()
               ? &params_[static_cast<std::size_t>(i)]
               : nullptr;
}
ParamValue MidiModulator::getParam(const std::string &id) const noexcept {
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (params_[i].id == id) {
            const auto n = values_[i].load(std::memory_order_relaxed);
            return ParamValue::withReal(n, real(i, n));
        }
    return {};
}
bool MidiModulator::setParam(const std::string &id, const ParamValue &v) {
    if (!std::isfinite(v.normalized))
        return false;
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (params_[i].id == id) {
            values_[i].store(std::clamp(v.normalized, 0., 1.), std::memory_order_relaxed);
            return true;
        }
    return false;
}
void MidiModulator::publish() {
    auto p = std::make_unique<Config>();
    p->mappings = config_.mappings;
    p->mappingCount = config_.mappingCount;
    p->points = config_.points;
    p->pointCount = config_.pointCount;
    publisher_.publish(std::move(p));
    publisher_.collect();
}
std::vector<std::int64_t> MidiModulator::eventTargets() const {
    std::vector<std::int64_t> ids;
    for (std::size_t i = 0; i < config_.mappingCount; ++i)
        ids.push_back(config_.mappings[i].device);
    return ids;
}
bool MidiModulator::setMappings(std::span<const Mapping> mappings) {
    if (mappings.size() > 8)
        return false;
    for (const auto &m : mappings)
        if (m.device <= 0 || !std::isfinite(m.min) || !std::isfinite(m.max) ||
            !std::isfinite(m.depth) || m.min < 0 || m.min > 1 || m.max < 0 || m.max > 1 ||
            m.depth < 0 || m.depth > 1)
            return false;
    std::copy(mappings.begin(), mappings.end(), config_.mappings.begin());
    config_.mappingCount = mappings.size();
    publish();
    return true;
}
bool MidiModulator::setEnvelope(std::span<const Point> points) {
    if (points.size() < 2 || points.size() > 32 || points.front().time != 0 ||
        points.back().time != 1)
        return false;
    double previous = -1;
    int sustain = 0;
    for (const auto &p : points) {
        if (!std::isfinite(p.time) || !std::isfinite(p.value) || !std::isfinite(p.curve) ||
            p.time <= previous || p.value < 0 || p.value > 1 || p.curve < -1 || p.curve > 1)
            return false;
        previous = p.time;
        if (p.sustain)
            ++sustain;
    }
    if (sustain > 1)
        return false;
    std::copy(points.begin(), points.end(), config_.points.begin());
    config_.pointCount = points.size();
    publish();
    return true;
}
void MidiModulator::refreshMappedOutput() noexcept {
    lastMapping_.fill(std::numeric_limits<double>::quiet_NaN());
    forceRefresh_ = true;
}
void MidiModulator::prepare(double rate, std::int32_t) {
    rate_ = std::isfinite(rate) && rate >= 1000 && rate <= 768000 ? rate : 48000;
    voices_ = {};
    phase_ = 1;
    held_ = false;
    clock_ = 0;
    increment_ = 0;
    current_ = target_ = 0;
    echoAt_ = 0;
    echo_.assign(static_cast<std::size_t>(rate_ * 2) + 1, 0);
    lastMapping_.fill(std::numeric_limits<double>::quiet_NaN());
    random_ = kind_ == Kind::Expression ? static_cast<std::uint32_t>(value(8)) : 1;
}
double MidiModulator::random() noexcept {
    random_ = random_ * 1664525u + 1013904223u;
    return static_cast<double>(random_) / 4294967296.;
}
double MidiModulator::envelope(const Config &c) noexcept {
    for (std::size_t i = 1; i < c.pointCount; ++i)
        if (phase_ <= c.points[i].time) {
            const auto &a = c.points[i - 1];
            const auto &b = c.points[i];
            double x = std::clamp((phase_ - a.time) / (b.time - a.time), 0., 1.);
            x = std::pow(x, std::exp2(a.curve * 4));
            return a.value + (b.value - a.value) * x;
        }
    return c.points[c.pointCount - 1].value;
}
void MidiModulator::transformEvents(engine::EventSpan input, engine::EventList &out,
                                    std::int32_t frames, double,
                                    const engine::TransportInfo *transport) noexcept {
    if (kind_ == Kind::MPE) {
        mpe(input, out, frames);
        return;
    }
    engine::SnapshotPublisher<Config>::AudioRead config(publisher_);
    if (!config.valid())
        return;
    if (seenConfig_ != config->seq) {
        seenConfig_ = config->seq;
        forceRefresh_ = true;
        lastMapping_.fill(std::numeric_limits<double>::quiet_NaN());
    }
    if (transport) {
        if (wasPlaying_ && !transport->playing) {
            increment_ = 0;
            held_ = false;
        }
        wasPlaying_ = transport->playing;
    }
    int next = 0;
    for (int frame = 0; frame < frames; ++frame) {
        bool changed = forceRefresh_ && frame == 0;
        while (next < input.count && input.first[next].frame <= frame) {
            const auto e = input.first[next++];
            forward(out,e);
            if (e.type == engine::EventType::ParamValue && e.paramId < params_.size() &&
                std::isfinite(e.value)) {
                values_[e.paramId].store(std::clamp(e.value, 0., 1.), std::memory_order_relaxed);
                changed = true;
                continue;
            }
            if (e.type == engine::EventType::NoteOn && e.value > 0) {
                held_ = true;
                note_ = e.noteId;
                velocity_ = e.value;
                phase_ = 0;
                changed = true;
                if (kind_ == Kind::Expression) {
                    const int source = static_cast<int>(value(0));
                    if (source == 0)
                        target_ = e.value;
                    if (source == 4)
                        target_ = static_cast<double>(e.dim) / 127;
                    if (source == 6)
                        target_ = random() * value(7);
                    if (source == 7) {
                        const auto steps = static_cast<std::uint64_t>(value(6));
                        target_ = steps > 1 ? static_cast<double>(increment_++ % steps) /
                                                  static_cast<double>(steps - 1)
                                            : 0;
                    }
                }
            }
            if (e.type == engine::EventType::NoteOff && e.noteId == note_)
                held_ = false;
            if (kind_ == Kind::Expression) {
                const int source = static_cast<int>(value(0));
                if (e.type == engine::EventType::Control) {
                    if ((source == 1 && e.dim == 1) || (source == 3 && e.dim == 128) ||
                        (source == 9 && e.dim == 64)) {
                        target_ = e.value;
                        changed = true;
                    }
                    if (source == 2 && e.dim == 129) {
                        target_ = .5 + e.value / 96;
                        changed = true;
                    }
                }
                if (e.type == engine::EventType::NoteExpression) {
                    if ((source == 3 && e.dim == 1) || (source == 8 && e.dim == 2) ||
                        (source == 5 && e.dim == 3)) {
                        target_ = e.value;
                        changed = true;
                    }
                    if (source == 2 && e.dim == 0) {
                        target_ = .5 + e.value / 96;
                        changed = true;
                    }
                }
            }
        }
        double signal = target_;
        if (kind_ == Kind::Shaper) {
            signal = envelope(*config.get()) * (1 - value(4) + value(4) * velocity_);
            const double bpm = transport && transport->bpm > 0 ? transport->bpm : 120;
            const double delta = value(1) > 0 ? bpm / (60 * rate_ * value(2)) : value(0) / rate_;
            bool sustain = false;
            if (held_ && value(3) == 0)
                for (std::size_t i = 0; i < config->pointCount; ++i)
                    if (config->points[i].sustain && phase_ >= config->points[i].time) {
                        phase_ = config->points[i].time;
                        sustain = true;
                        break;
                    }
            if (!sustain)
                phase_ += delta;
            if (phase_ >= 1) {
                if (held_ && value(3) > 0)
                    phase_ -= std::floor(phase_);
                else
                    phase_ = 1;
            }
            if (!echo_.empty()) {
                const auto delay =
                    std::min(echo_.size() - 1,
                             static_cast<std::size_t>(std::llround(value(10) * .001 * rate_)));
                const auto pos = (echoAt_ + echo_.size() - delay) % echo_.size();
                signal += echo_[pos] * value(9);
                echo_[echoAt_] = signal;
                echoAt_ = (echoAt_ + 1) % echo_.size();
            }
            signal += value(5);
        } else {
            const double x = std::clamp(signal, 0., 1.), cx = value(3), cy = value(4);
            signal = x <= cx ? x / cx * cy : cy + (x - cx) / (1 - cx) * (1 - cy);
            if (value(5) > 0)
                signal = signal * signal * (3 - 2 * signal);
        }
        const double ms = kind_ == Kind::Shaper ? value(7) : value(signal > current_ ? 1 : 2);
        current_ =
            ms > 0 ? signal + (current_ - signal) * std::exp(-1 / (ms * .001 * rate_)) : signal;
        if (changed || (clock_ + static_cast<std::uint64_t>(frame)) % 8 == 0) {
            double y = current_;
            if (kind_ == Kind::Shaper)
                y += (random() * 2 - 1) * value(6);
            y = std::clamp(y, 0., 1.);
            for (std::size_t i = 0; i < config->mappingCount; ++i) {
                const auto &m = config->mappings[i];
                const double depth = m.depth * value(kind_ == Kind::Shaper ? 8 : 9);
                const double v = m.modulation ? (m.bipolar ? 2 * y - 1 : y) * depth
                                              : m.min + (m.max - m.min) * y;
                if (v == lastMapping_[i])
                    continue;
                engine::Event e;
                e.type =
                    m.modulation ? engine::EventType::MappedMod : engine::EventType::MappedValue;
                e.frame = frame;
                e.targetDevice = m.device;
                e.paramId = m.parameter;
                e.value = v;
                if (out.push(e))
                    lastMapping_[i] = v;
            }
        }
    }
    out.sortByFrame();
    forceRefresh_ = false;
    clock_ += static_cast<std::uint64_t>(frames);
}
void MidiModulator::mpe(engine::EventSpan input, engine::EventList &out, int frames) noexcept {
    int next = 0;
    for (int frame = 0; frame < frames; ++frame) {
        bool changed = false;
        while (next < input.count && input.first[next].frame <= frame) {
            auto e = input.first[next++];
            if (e.type == engine::EventType::ParamValue && e.paramId < params_.size() &&
                std::isfinite(e.value)) {
                values_[e.paramId].store(std::clamp(e.value, 0., 1.), std::memory_order_relaxed);
                forward(out,e);
                changed = true;
                continue;
            }
            auto it = std::find_if(voices_.begin(), voices_.end(), [&](const Voice &v) {
                return v.active && v.note.noteId == e.noteId && v.note.channel == e.channel;
            });
            if (e.type == engine::EventType::NoteOn && e.value > 0) {
                it = std::find_if(voices_.begin(), voices_.end(),
                                  [](const Voice &v) { return !v.active; });
                if (it != voices_.end()) {
                    *it = {};
                    it->active = true;
                    it->note = e;
                    it->target = {0, value(12), value(13)};
                    it->current = it->target;
                    it->onset = it->target[2];
                    it->sent.fill(std::numeric_limits<double>::quiet_NaN());
                }
                forward(out,e);
                changed = true;
                continue;
            }
            if (e.type == engine::EventType::NoteExpression && e.dim < 3 && it != voices_.end()) {
                if (e.dim == 2 && !it->onsetCaptured) {
                    it->onset = e.value;
                    it->onsetCaptured = true;
                }
                it->target[e.dim] = e.value;
                changed = true;
                continue;
            }
            if (e.type == engine::EventType::NoteOff && it != voices_.end())
                it->active = false;
            forward(out,e);
        }
        for (auto &v : voices_)
            if (v.active) {
                for (std::size_t i = 0; i < 3; ++i) {
                    double goal = v.target[i];
                    if (i == 0)
                        goal *= value(0);
                    else
                        goal = std::pow(std::clamp(goal, 0., 1.), value(i));
                    if (i == 2 && value(5) == 1)
                        goal = std::clamp(goal - v.onset + .5, 0., 1.);
                    if (i == 2 && value(5) == 2)
                        goal = v.onset;
                    const double ms =
                        i == 2 && value(5) == 2 ? 0 : value(goal > v.current[i] ? 3 : 4);
                    v.current[i] =
                        ms > 0 ? goal + (v.current[i] - goal) * std::exp(-1 / (ms * .001 * rate_))
                               : goal;
                }
                if (changed ||
                    (clock_ + static_cast<std::uint64_t>(frame)) %
                            static_cast<std::uint64_t>(std::max(1., std::floor(rate_ / 500.))) ==
                        0) {
                    for (std::size_t i = 0; i < 3; ++i) {
                        if (!changed && v.current[i] == v.sent[i])
                            continue;
                        v.sent[i] = v.current[i];
                        auto e = v.note;
                        e.frame = frame;
                        e.type = engine::EventType::NoteExpression;
                        e.dim = static_cast<std::uint16_t>(i);
                        e.value = v.current[i];
                        if (i > 0 && value(6) > 0)
                            e.dim = static_cast<std::uint16_t>(3 - i);
                        if (i == 1 && value(7) > 0)
                            e.dim = 2;
                        if (i == 2 && value(8) > 0)
                            e.dim = 1;
                        forward(out,e);
                        if ((i == 0 && value(11) > 0) || (i == 1 && value(9) > 0) ||
                            (i == 2 && value(10) > 0)) {
                            e.type = engine::EventType::Control;
                            e.dim = i == 0 ? 129 : i == 1 ? 128 : 1;
                            e.channel = 0;
                            forward(out,e);
                        }
                    }
                }
            }
    }
    out.sortByFrame();
    clock_ += static_cast<std::uint64_t>(frames);
}
void MidiModulator::process(const engine::NodeIo &io) noexcept {
    for (int c = 0; c < io.channels; ++c)
        if (io.out && io.out[c]) {
            if (io.in && io.in[c])
                std::copy_n(io.in[c] + io.blockOffset, io.frames, io.out[c] + io.blockOffset);
            else
                std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.f);
        }
}
std::vector<std::uint8_t> MidiModulator::saveState(const std::string &role) const {
    if (role != "component")
        return {};
    nlohmann::json j = {{"version", 1},
                        {"uid", identity_.uid},
                        {"params", nlohmann::json::array()},
                        {"maps", nlohmann::json::array()},
                        {"points", nlohmann::json::array()}};
    for (std::size_t i = 0; i < params_.size(); ++i)
        j["params"].push_back(values_[i].load(std::memory_order_relaxed));
    for (std::size_t i = 0; i < config_.mappingCount; ++i) {
        const auto &m = config_.mappings[i];
        j["maps"].push_back(
            {m.device, m.parameter, m.modulation, m.bipolar, m.min, m.max, m.depth});
    }
    for (std::size_t i = 0; i < config_.pointCount; ++i) {
        const auto &p = config_.points[i];
        j["points"].push_back({p.time, p.value, p.curve, p.sustain});
    }
    return nlohmann::json::to_cbor(j);
}
bool MidiModulator::loadState(const std::string &role, const std::vector<std::uint8_t> &bytes) {
    if (role != "component" || bytes.size() > 65536)
        return false;
    try {
        const auto j = nlohmann::json::from_cbor(bytes);
        if (j.at("version") != 1 || j.at("uid") != identity_.uid ||
            j.at("params").size() != params_.size())
            return false;
        std::vector<Mapping> maps;
        std::vector<Point> points;
        for (const auto &m : j.at("maps"))
            maps.push_back({m.at(0).get<std::int64_t>(), m.at(1).get<std::uint32_t>(),
                            m.at(2).get<bool>(), m.at(3).get<bool>(), m.at(4).get<double>(),
                            m.at(5).get<double>(), m.at(6).get<double>()});
        for (const auto &p : j.at("points"))
            points.push_back({p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>(),
                              p.at(3).get<bool>()});
        std::array<double, 32> values{};
        for (std::size_t i = 0; i < params_.size(); ++i) {
            values[i] = j.at("params").at(i).get<double>();
            if (!std::isfinite(values[i]) || values[i] < 0 || values[i] > 1)
                return false;
        }
        MidiModulator validation(kind_);
        if (!validation.setMappings(maps) || !validation.setEnvelope(points))
            return false;
        config_.mappings = validation.config_.mappings;
        config_.mappingCount = validation.config_.mappingCount;
        config_.points = validation.config_.points;
        config_.pointCount = validation.config_.pointCount;
        for (std::size_t i = 0; i < params_.size(); ++i)
            values_[i].store(values[i], std::memory_order_relaxed);
        publish();
        return true;
    } catch (const nlohmann::json::exception &) {
        return false;
    }
}
} // namespace adi::device
