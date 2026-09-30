// SPDX-License-Identifier: GPL-3.0-or-later
#include "midi_notes.hpp"
#include "microtuner.hpp"
#include "midi_modulator.hpp"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <numeric>
namespace adi::device {
namespace {
constexpr const char *ids[]{"pitch",      "chord",       "random",    "scale",
                            "velocity",   "note_length", "note_echo", "midi_monitor",
                            "cc_control", "arpeggiator"};
constexpr const char *names[]{"Pitch",      "Chord",       "Random",    "Scale",
                              "Velocity",   "Note Length", "Note Echo", "MIDI Monitor",
                              "CC Control", "Arpeggiator"};
int mod12(int x) noexcept { return (x % 12 + 12) % 12; }
bool same(const engine::Event &a, const engine::Event &b) noexcept {
    return a.channel == b.channel && (a.noteId || b.noteId ? a.noteId == b.noteId : a.dim == b.dim);
}
} // namespace
MidiNotes::MidiNotes(Kind k)
    : kind_(k), identity_{"internal", std::string("adi.") + ids[static_cast<int>(k)],
                          names[static_cast<int>(k)], "ADI", "1"} {
    switch (k) {
    case Kind::Pitch:
        add("pitch", -128, 128, 0, true);
        add("lowest", 0, 127, 0, true);
        add("range", 0, 127, 127, true);
        add("mode", 0, 2, 0, true);
        break;
    case Kind::Chord:
        for (int i = 1; i <= 6; ++i) {
            add("shift" + std::to_string(i), -36, 36, 0, true);
            add("velocity" + std::to_string(i), .01, 2, 1);
            add("chance" + std::to_string(i), 0, 1, 1);
        }
        add("expression", 0, 1, 1, true);
        add("seed", 1, 16777215, 1, true);
        add("strum", -400, 400, 0);
        add("tension", -1, 1, 0);
        add("crescendo", -1, 1, 0);
        break;
    case Kind::Random:
        add("chance", 0, 1, 0);
        add("choices", 1, 24, 12, true);
        add("interval", 1, 12, 1, true);
        add("sign", 0, 2, 0, true);
        add("mode", 0, 1, 0, true);
        add("seed", 1, 16777215, 1, true);
        break;
    case Kind::Scale:
        add("root", 0, 11, 0, true);
        add("transpose", -36, 36, 0, true);
        add("fold", 0, 1, 0, true);
        add("lowest", 0, 127, 0, true);
        add("range", 0, 127, 127, true);
        for (int i = 0; i < 12; ++i)
            add("map" + std::to_string(i), -1, 11, i, true);
        break;
    case Kind::Velocity:
        add("operation", 0, 2, 0, true);
        add("mode", 0, 2, 0, true);
        add("lowest", 0, 1, 0);
        add("range", 0, 1, 1);
        add("out_low", 0, 1, 0);
        add("out_high", 0, 1, 1);
        add("drive", -1, 1, 0);
        add("compand", -1, 1, 0);
        add("random", 0, 1, 0);
        add("seed", 1, 16777215, 1, true);
        break;
    case Kind::NoteLength:
        add("trigger", 0, 1, 0, true);
        add("sync", 0, 1, 0, true);
        add("time", 1, 60000, 100);
        add("beats", .015625, 32, 1);
        add("gate", .01, 2, 1);
        add("release_velocity", 0, 1, 0);
        add("decay", 0, 60000, 0);
        add("key_scale", -1, 1, 0);
        add("latch", 0, 1, 0, true);
        break;
    case Kind::NoteEcho:
        add("sync", 0, 1, 1, true);
        add("time", 1, 60000, 250);
        add("sixteenths", 1, 32, 4, true);
        add("fraction", .01, 2, 1);
        add("input", 0, 1, 0, true);
        add("pitch", -36, 36, 0, true);
        add("delay_velocity", 0, 1, .5);
        add("feedback", 0, .99, .5);
        add("mpe", 0, 1, 1, true);
        add("press", 0, 1, 1);
        add("slide", 0, 1, 1);
        add("pitch_feedback", 0, 1, 1);
        break;

    case Kind::Monitor:
        add("freeze", 0, 1, 0, true);
        break;
    case Kind::CCControl:
        add("modulation", 0, 1, 0);
        add("pitch_bend", -48, 48, 0);
        add("pressure", 0, 1, 0);
        for (int i = 0; i < 13; ++i) {
            add("controller" + std::to_string(i), 0, 127, i == 0 ? 64 : i, true);
            add("value" + std::to_string(i), 0, 1, 0);
        }
        add("channel", 0, 15, 0, true);
        add("send_all", 0, 1, 0, true);
        add("learn", 0, 13, 0, true);
        break;

    case Kind::Arpeggiator:
        add("style", 0, 10, 0, true);
        add("sync", 0, 1, 1, true);
        add("time", 1, 60000, 125);
        add("beats", .015625, 32, .25);
        add("gate", .01, 2, .5);
        add("hold", 0, 1, 0, true);
        add("distance", -36, 36, 12, true);
        add("steps", 0, 8, 0, true);
        add("offset", 0, 127, 0, true);
        add("retrigger", 0, 2, 1, true);
        add("repeats", 0, 128, 0, true);
        add("target", 0, 1, 1);
        add("decay", 0, 60000, 0);
        add("seed", 1, 16777215, 1, true);
        break;
    }
    if (k == Kind::Pitch || k == Kind::Chord || k == Kind::Random || k == Kind::Arpeggiator) {
        scaleIndex_ = specs_.size();
        add("use_scale", 0, 1, 0, true);
        add("scale_root", 0, 11, 0, true);
        add("scale_mask", 1, 4095, 2741, true);
    }
}
std::unique_ptr<DeviceInstance> MidiNotes::create(const std::string &uid) {
    if (uid == "adi.expression_control")
        return std::make_unique<MidiModulator>(MidiModulator::Kind::Expression);
    if (uid == "adi.shaper")
        return std::make_unique<MidiModulator>(MidiModulator::Kind::Shaper);
    if (uid == "adi.mpe_control")
        return std::make_unique<MidiModulator>(MidiModulator::Kind::MPE);
    if (uid == "adi.microtuner")
        return std::make_unique<Microtuner>();
    for (int i = 0; i < 10; ++i)
        if (uid == std::string("adi.") + ids[i])
            return std::make_unique<MidiNotes>(static_cast<Kind>(i));
    return {};
}
void MidiNotes::add(const std::string &id, double lo, double hi, double def, bool integer) {
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
    values_[specs_.size()].store(p.defaultValue.normalized);
    specs_.push_back({p, integer});
}
double MidiNotes::real(std::size_t i, double n) const noexcept {
    const auto &s = specs_[i];
    const auto &p = s.descriptor;
    const double v = p.minReal + std::clamp(n, 0., 1.) * (p.maxReal - p.minReal);
    return s.integer ? std::round(v) : v;
}
double MidiNotes::value(std::size_t i) const noexcept {
    return real(i, values_[i].load(std::memory_order_relaxed));
}
const ParamDescriptor *MidiNotes::paramAt(std::int32_t i) const noexcept {
    return i >= 0 && static_cast<std::size_t>(i) < specs_.size()
               ? &specs_[static_cast<std::size_t>(i)].descriptor
               : nullptr;
}
ParamValue MidiNotes::getParam(const std::string &id) const noexcept {
    for (std::size_t i = 0; i < specs_.size(); ++i)
        if (specs_[i].descriptor.id == id) {
            const double n = values_[i].load(std::memory_order_relaxed);
            return ParamValue::withReal(n, real(i, n));
        }
    return {};
}
bool MidiNotes::setParam(const std::string &id, const ParamValue &v) {
    if (!std::isfinite(v.normalized))
        return false;
    for (std::size_t i = 0; i < specs_.size(); ++i)
        if (specs_[i].descriptor.id == id) {
            values_[i].store(std::clamp(v.normalized, 0., 1.), std::memory_order_relaxed);
            return true;
        }
    return false;
}
void MidiNotes::prepare(double rate, std::int32_t) {
    for (auto &voice : voices_)
        voice = Voice{};
    for (auto &note : sounding_)
        note.active = false;
    hadTransport_ = false;
    wasPlaying_ = false;
    for (std::size_t i = 0; i < values_.size(); ++i)
        sentControls_[i] = values_[i].load(std::memory_order_relaxed);
    pendingCount_ = 0;
    clock_ = 0;
    order_ = 0;
    nextArp_ = 0;
    arpStep_ = 0;
    arpTotal_ = 0;
    rate_ = std::isfinite(rate) && rate > 0 ? rate : 48000;
    alternate_ = 0;
    nextId_ = 1;
    rng_ = 1;
    for (std::size_t i = 0; i < specs_.size(); ++i)
        if (specs_[i].descriptor.id == "seed")
            rng_ = static_cast<std::uint32_t>(value(i));
}
double MidiNotes::random() noexcept {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<double>(rng_) / 4294967296.;
}
bool MidiNotes::emit(engine::EventList &out, const engine::Event &e) noexcept {
    if (out.push(e))
        return true;
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
}
double MidiNotes::velocity(double v) noexcept {
    const double lo = value(2), hi = std::min(1., lo + value(3));
    const int mode = static_cast<int>(value(1));
    if (mode == 1 && (v < lo || v > hi))
        return -1;
    double x = mode == 2 ? 1. : (hi > lo ? std::clamp((v - lo) / (hi - lo), 0., 1.) : 1.);
    x = std::pow(x, std::exp2(-2 * value(6)));
    const double centered = 2 * x - 1;
    x = .5 + .5 * std::copysign(std::pow(std::abs(centered), std::exp2(-2 * value(7))), centered);
    x = std::clamp(x + (random() * 2 - 1) * value(8), 0., 1.);
    return std::clamp(value(4) + x * (value(5) - value(4)), 0., 1.);
}
int MidiNotes::transpose(int key, int steps) const noexcept {
    if (scaleIndex_ >= specs_.size() || value(scaleIndex_) == 0)
        return key + steps;
    const int root = static_cast<int>(value(scaleIndex_ + 1)),
              mask = static_cast<int>(value(scaleIndex_ + 2));
    std::array<int, 12> degrees{};
    int count = 0;
    for (int i = 0; i < 12; ++i)
        if (mask & (1 << i))
            degrees[static_cast<std::size_t>(count++)] = i;
    if (count == 0)
        return key;
    const int octave = static_cast<int>(std::floor(static_cast<double>(key - root) / 12));
    const int degree = key - root - octave * 12;
    int nearest = 0;
    for (int i = 1; i < count; ++i)
        if (std::abs(degrees[static_cast<std::size_t>(i)] - degree) <
            std::abs(degrees[static_cast<std::size_t>(nearest)] - degree))
            nearest = i;
    const int index = octave * count + nearest + steps,
              resultOctave = static_cast<int>(std::floor(static_cast<double>(index) / count));
    return root + resultOctave * 12 +
           degrees[static_cast<std::size_t>(index - resultOctave * count)];
}
void MidiNotes::attack(const engine::Event &e, Voice &v) noexcept {
    v = {};
    v.active = true;
    v.input = e;
    auto append = [&](int key, double velocityValue) {
        if (key < 0 || key > 127 || velocityValue <= 0)
            return;
        for (int i = 0; i < v.count; ++i)
            if (v.output[static_cast<std::size_t>(i)].dim == key)
                return;
        auto o = e;
        o.dim = static_cast<std::uint16_t>(key);
        o.value = std::clamp(velocityValue, 0., 1.);
        // Generated chord voices need separate expression identities, even for an
        // unassigned input.
        if (kind_ == Kind::Chord || kind_ == Kind::NoteEcho)
            o.noteId = nextId_++ * 1024;
        v.output[static_cast<std::size_t>(v.count++)] = o;
    };
    int key = e.dim;
    switch (kind_) {
    case Kind::Pitch: {
        key = transpose(key, static_cast<int>(value(0)));
        const int lo = static_cast<int>(value(1)),
                  hi = std::min(127, lo + static_cast<int>(value(2)));
        const int mode = static_cast<int>(value(3));
        if (mode == 1) {
            while (key < lo)
                key += 12;
            while (key > hi)
                key -= 12;
        }
        if (mode == 2)
            key = std::clamp(key, lo, hi);
        if (key >= lo && key <= hi)
            append(key, e.value);
        break;
    }
    case Kind::Chord:
        append(key, e.value);
        for (std::size_t i = 0; i < 6; ++i)
            if (random() < value(3 * i + 2))
                append(transpose(key, static_cast<int>(value(3 * i))), e.value * value(3 * i + 1));
        break;
    case Kind::Random: {
        int offset = 0;
        const auto choices = static_cast<unsigned>(value(1));
        if (random() < value(0)) {
            if (value(4) > 0)
                offset = static_cast<int>(alternate_++ % (choices + 1));
            else
                offset = 1 + static_cast<int>(random() * choices);
            offset *= static_cast<int>(value(2));
            if (value(3) == 1 || (value(3) == 2 && random() < .5))
                offset = -offset;
        }
        append(transpose(key, offset), e.value);
        break;
    }
    case Kind::Scale: {
        const int lo = static_cast<int>(value(3)),
                  hi = std::min(127, lo + static_cast<int>(value(4)));
        if (key >= lo && key <= hi) {
            const int degree = mod12(key - static_cast<int>(value(0))),
                      mapped = static_cast<int>(value(static_cast<std::size_t>(5 + degree)));
            if (mapped < 0)
                break;
            int delta = mapped - degree;
            if (value(2) > 0) {
                if (delta > 6)
                    delta -= 12;
                if (delta < -6)
                    delta += 12;
            }
            key += delta + static_cast<int>(value(1));
        }
        append(key, e.value);
        break;
    }
    case Kind::Velocity:
        append(key, value(0) == 1 ? e.value : velocity(e.value));
        break;
    case Kind::NoteLength:
        append(key, e.value);
        break;
    case Kind::NoteEcho: {
        if (value(4) == 0)
            append(key, e.value);
        const double velocityValue = e.value * value(6), feedback = value(7);
        const int pitch = key + static_cast<int>(value(5));
        if (velocityValue >= 1. / 16384 && pitch >= 0 && pitch <= 127) {
            const double ms = value(0) > 0 ? 60000 / bpm_ * value(2) * .25 * value(3) : value(1);
            auto o = e;
            o.dim = static_cast<std::uint16_t>(pitch);
            o.noteId = nextId_++ * 1024;
            o.value = velocityValue;
            const auto index = static_cast<std::size_t>(v.count++);
            v.output[index] = o;
            v.delay[index] = std::max<std::int64_t>(
                1, static_cast<std::int64_t>(std::llround(ms * .001 * rate_)));
            const int repeats =
                feedback > 0
                    ? std::min(1022,
                               static_cast<int>(std::floor(std::log((1. / 16384) / velocityValue) /
                                                           std::log(feedback))))
                    : 0;
            v.repeats[index] = {std::max(0, repeats), static_cast<int>(value(5)), v.delay[index],
                                feedback};
        }
        break;
    }

    case Kind::Monitor:
    case Kind::CCControl:
    case Kind::Arpeggiator:
        append(key, e.value);
        break;
    }
}
void MidiNotes::noteOffDelivered(const engine::Event &e) noexcept {
    for (auto &n : sounding_)
        if (n.active && n.note.noteId == e.noteId && n.note.dim == e.dim &&
            n.note.channel == e.channel) {
            n.active = false;
            break;
        }
}
void MidiNotes::noteOffRejected(const engine::Event &e) noexcept {
    auto retry = e;
    retry.frame = 0;
    queue(retry, 0);
}
bool MidiNotes::queue(engine::Event e, std::int64_t delay, const Repeat *repeat) noexcept {
    e.owner = this;
    if (immediate_ && pendingCount_ == 0 && delay == 0 && (!repeat || repeat->remaining == 0) &&
        immediate_->size() < immediate_->capacity()) {
        if (e.type == engine::EventType::NoteOn) {
            auto free = std::find_if(sounding_.begin(), sounding_.end(),
                                     [](const OutputNote &n) { return !n.active; });
            if (free == sounding_.end()) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            *free = {e, true, currentSource_};
        }
        return emit(*immediate_, e);
    }
    // Half the queue is reserved for releases. Expression storms cannot use it.
    const auto limit = e.type == engine::EventType::NoteOff ? pending_.size() : pending_.size() / 2;
    if (pendingCount_ >= limit) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    pending_[pendingCount_++] = {e, clock_ + e.frame + delay, order_++, repeat ? *repeat : Repeat{},
                                 currentSource_};
    return true;
}
void MidiNotes::flush(engine::EventList &out, std::int32_t frames) noexcept {
    // In-place heap-free sort; std::sort has no dynamic storage requirement in
    // our three standard libraries (covered by the allocation audit).
    std::sort(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pendingCount_),
              [](const Pending &a, const Pending &b) {
                  return a.due == b.due ? a.order < b.order : a.due < b.due;
              });
    std::size_t keep = 0;
    for (std::size_t i = 0; i < pendingCount_; ++i) {
        auto p = pending_[i];
        bool finished = false;
        while (p.due < clock_ + frames) {
            p.event.frame = static_cast<std::int32_t>(std::max<std::int64_t>(0, p.due - clock_));
            bool emitted = false;
            if (p.event.type == engine::EventType::NoteOn) {
                auto free = std::find_if(sounding_.begin(), sounding_.end(),
                                         [](const OutputNote &n) { return !n.active; });
                if (free == sounding_.end()) {
                    rejected_.fetch_add(1, std::memory_order_relaxed);
                    finished = true;
                    break;
                }
                if (emit(out, p.event)) {
                    *free = {p.event, true, p.source};
                    emitted = true;
                }
            } else
                emitted = emit(out, p.event);
            if (!emitted)
                break;
            if (p.repeat.remaining <= 0) {
                finished = true;
                break;
            }
            --p.repeat.remaining;
            p.due += p.repeat.period;
            ++p.event.noteId;
            p.event.value *= p.repeat.decay;
            if (p.event.type != engine::EventType::NoteExpression) {
                const int key = static_cast<int>(p.event.dim) + p.repeat.pitch;
                if (key < 0 || key > 127) {
                    finished = true;
                    break;
                }
                p.event.dim = static_cast<std::uint16_t>(key);
            }
        }
        if (!finished)
            pending_[keep++] = p;
    }
    pendingCount_ = keep;
    out.sortByFrame();
    clock_ += frames;
}
void MidiNotes::arpeggiate(engine::EventSpan inputEvents, engine::EventList &out,
                           std::int32_t frames, const engine::TransportInfo *transport) noexcept {
    const int count = inputEvents.count;
    int input = 0;
    for (int frame = 0; frame < frames; ++frame) {
        while (input < count && inputEvents.first[input].frame <= frame) {
            const auto e = inputEvents.first[input++];
            if (e.type == engine::EventType::ParamValue) {
                if (e.paramId < specs_.size() && std::isfinite(e.value))
                    values_[e.paramId].store(std::clamp(e.value, 0., 1.),
                                             std::memory_order_relaxed);
                emit(out, e);
                continue;
            }
            if (e.type == engine::EventType::NoteOn && e.value > 0 && e.dim < 128) {
                auto existing = std::find_if(voices_.begin(), voices_.end(), [&](const Voice &v) {
                    return v.active && v.input.dim == e.dim && v.input.channel == e.channel;
                });
                if (value(5) > 0 && existing != voices_.end()) {
                    existing->active = false;
                    continue;
                }
                const bool empty = std::none_of(voices_.begin(), voices_.end(),
                                                [](const Voice &v) { return v.active; });
                auto it = std::find_if(voices_.begin(), voices_.end(),
                                       [](const Voice &v) { return !v.active; });
                if (it == voices_.end()) {
                    rejected_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                *it = {};
                it->active = true;
                it->input = e;
                it->started = clock_ + frame;
                if (empty || value(9) == 1) {
                    nextArp_ = static_cast<double>(clock_ + frame);
                    arpStep_ = 0;
                }
                if (value(9) == 2 && transport) {
                    const double interval = rate_ * 60 / bpm_ * value(3);
                    const double timeline = static_cast<double>(transport->timelineSample + frame);
                    nextArp_ = static_cast<double>(clock_ + frame) +
                               std::fmod(interval - std::fmod(timeline, interval), interval);
                }
            } else if (e.type == engine::EventType::NoteOff ||
                       (e.type == engine::EventType::NoteOn && e.value == 0)) {
                if (e.owner)
                    e.owner->noteOffDelivered(e);
                if (value(5) == 0) {
                    auto it = std::find_if(voices_.begin(), voices_.end(), [&](const Voice &v) {
                        return v.active && same(v.input, e);
                    });
                    if (it != voices_.end())
                        it->active = false;
                }
            } else if (e.type == engine::EventType::NoteExpression && e.noteId != 0 && e.dim < 3) {
                auto held = std::find_if(voices_.begin(), voices_.end(), [&](const Voice &v) {
                    return v.active && same(v.input, e);
                });
                if (held != voices_.end()) {
                    held->expression[e.dim] = e.value;
                    held->hasExpression[e.dim] = true;
                }
                currentSource_ = e.noteId;
                for (const auto &n : sounding_)
                    if (n.active && n.source == e.noteId) {
                        auto expression = e;
                        expression.noteId = n.note.noteId;
                        queue(expression, 0);
                    }
            } else
                emit(out, e);
        }
        if (static_cast<double>(clock_ + frame) + .5 < nextArp_)
            continue;
        std::size_t held = 0;
        for (std::size_t i = 0; i < voices_.size(); ++i)
            if (voices_[i].active)
                arpOrder_[held++] = i;
        if (held == 0) {
            nextArp_ = std::numeric_limits<double>::infinity();
            continue;
        }
        const int style = static_cast<int>(value(0));
        std::sort(arpOrder_.begin(), arpOrder_.begin() + static_cast<std::ptrdiff_t>(held),
                  [&](std::size_t a, std::size_t b) {
                      return style == 6 ? voices_[a].started < voices_[b].started
                                        : voices_[a].input.dim < voices_[b].input.dim;
                  });
        const double period = value(1) > 0 ? rate_ * 60 / bpm_ * value(3) : rate_ * .001 * value(2);
        const auto octaveSteps = static_cast<std::uint64_t>(value(7)) + 1,
                   total = held * octaveSteps;
        const auto step = arpStep_ + static_cast<std::uint64_t>(value(8));
        if (value(10) > 0 && arpStep_ / total >= static_cast<std::uint64_t>(value(10))) {
            nextArp_ += period;
            continue;
        }
        std::uint64_t index = step % total;
        if (style == 1)
            index = total - 1 - index;
        if (style == 2 || style == 3) {
            const auto cycle = total > 1 ? total * 2 - 2 : 1;
            index = step % cycle;
            if (index >= total)
                index = cycle - index;
            if (style == 3)
                index = total - 1 - index;
        }
        if (style == 4)
            index = index % 2 == 0 ? index / 2 : total - 1 - index / 2;
        if (style == 5)
            index = index % 2 == 0 ? (total - 1) / 2 - index / 2 : total / 2 + index / 2;
        if (style == 8)
            index = static_cast<std::uint64_t>(random() * static_cast<double>(total));
        if (style == 9) {
            if (arpTotal_ != total || step % total == 0) {
                for (std::uint64_t i = 0; i < total; ++i)
                    arpPermutation_[static_cast<std::size_t>(i)] = i;
                for (std::uint64_t i = total; i > 1; --i) {
                    const auto pick = static_cast<std::size_t>(random() * static_cast<double>(i));
                    std::swap(arpPermutation_[static_cast<std::size_t>(i - 1)],
                              arpPermutation_[pick]);
                }
                arpTotal_ = total;
            }
            index = arpPermutation_[static_cast<std::size_t>(step % total)];
        }
        // Random Once is a seed-dependent permutation with a coprime stride.
        if (style == 10) {
            auto stride = static_cast<std::uint64_t>(value(13)) | 1u;
            while (std::gcd(stride, total) != 1)
                stride += 2;
            index = (step * stride) % total;
        }
        auto play = [&](std::uint64_t selection) {
            const auto &heldVoice = voices_[arpOrder_[static_cast<std::size_t>(selection % held)]];
            auto on = heldVoice.input;
            const int key = transpose(static_cast<int>(on.dim), static_cast<int>(selection / held) *
                                                                    static_cast<int>(value(6)));
            if (key < 0 || key > 127)
                return;
            on.frame = frame;
            on.dim = static_cast<std::uint16_t>(key);
            on.noteId = nextId_++;
            if (value(12) > 0) {
                const double elapsed =
                    static_cast<double>(clock_ + frame - heldVoice.started) / rate_;
                on.value =
                    value(11) + (on.value - value(11)) * std::exp(-elapsed / (value(12) * .001));
            }
            currentSource_ = heldVoice.input.noteId;
            if (queue(on, 0)) {
                for (std::size_t dim = 0; dim < 3; ++dim)
                    if (heldVoice.hasExpression[dim]) {
                        auto expression = on;
                        expression.type = engine::EventType::NoteExpression;
                        expression.dim = static_cast<std::uint16_t>(dim);
                        expression.value = heldVoice.expression[dim];
                        queue(expression, 0);
                    }
                auto off = on;
                off.type = engine::EventType::NoteOff;
                queue(off, std::max<std::int64_t>(
                               1, static_cast<std::int64_t>(std::llround(period * value(4)))));
            }
        };
        if (style == 7)
            for (std::uint64_t i = 0; i < total; ++i)
                play(i);
        else
            play(index);
        ++arpStep_;
        nextArp_ += period;
    }
    flush(out, frames);
}
bool MidiNotes::readMonitor(engine::Event &e) noexcept {
    const auto tail = monitorTail_.load(std::memory_order_relaxed);
    if (tail == monitorHead_.load(std::memory_order_acquire))
        return false;
    e = monitor_[static_cast<std::size_t>(tail % monitor_.size())];
    monitorTail_.store(tail + 1, std::memory_order_release);
    return true;
}
void MidiNotes::clearMonitor() noexcept {
    monitorTail_.store(monitorHead_.load(std::memory_order_acquire), std::memory_order_release);
}
void MidiNotes::transformEvents(engine::EventSpan inputEvents, engine::EventList &out,
                                std::int32_t frames, double,
                                const engine::TransportInfo *transport) noexcept {
    immediate_ = &out;
    struct ClearImmediate {
        engine::EventList *&pointer;
        ~ClearImmediate() { pointer = nullptr; }
    } guard{immediate_};

    if (transport && std::isfinite(transport->bpm) && transport->bpm > 0)
        bpm_ = transport->bpm;
    if (transport) {
        const bool discontinuity =
            hadTransport_ && wasPlaying_ &&
            (!transport->playing || transport->timelineSample != expectedTimeline_);
        if (discontinuity && kind_ != Kind::Monitor && kind_ != Kind::CCControl) {
            pendingCount_ = 0;
            for (auto &voice : voices_)
                voice = Voice{};
            arpStep_ = 0;
            nextArp_ = static_cast<double>(clock_);
            for (const auto &n : sounding_)
                if (n.active) {
                    auto off = n.note;
                    off.type = engine::EventType::NoteOff;
                    off.frame = 0;
                    queue(off, 0);
                }
        }
        hadTransport_ = true;
        wasPlaying_ = transport->playing;
        expectedTimeline_ = transport->timelineSample + frames;
    }
    if (kind_ == Kind::Arpeggiator) {
        arpeggiate(inputEvents, out, frames, transport);
        return;
    }
    if (kind_ == Kind::Monitor) {
        for (const auto &e : inputEvents) {
            if (e.type == engine::EventType::ParamValue && e.paramId == 0 && std::isfinite(e.value))
                values_[0].store(std::clamp(e.value, 0., 1.), std::memory_order_relaxed);
            out.push(e);
            if (value(0) > 0)
                continue;
            const auto head = monitorHead_.load(std::memory_order_relaxed),
                       tail = monitorTail_.load(std::memory_order_acquire);
            if (head - tail < monitor_.size()) {
                monitor_[static_cast<std::size_t>(head % monitor_.size())] = e;
                monitorHead_.store(head + 1, std::memory_order_release);
            } else
                rejected_.fetch_add(1, std::memory_order_relaxed);
        }
        clock_ += frames;
        return;
    }
    auto control = [&](std::size_t index, int frame) {
        engine::Event c;
        c.type = engine::EventType::Control;
        c.frame = frame;
        c.channel = static_cast<std::uint8_t>(value(29));
        c.dim = index == 0   ? 1
                : index == 1 ? 129
                : index == 2 ? 128
                             : static_cast<std::uint16_t>(value(index - 1));
        c.value = value(index);
        queue(c, 0);
        sentControls_[index] = values_[index].load(std::memory_order_relaxed);
    };
    if (kind_ == Kind::CCControl) {
        for (std::size_t i = 0; i < 29; ++i)
            if (i < 3 || (i >= 4 && i % 2 == 0))
                if (sentControls_[i] != values_[i].load(std::memory_order_relaxed))
                    control(i, 0);
        if (values_[30].load(std::memory_order_relaxed) > .5 && sentControls_[30] <= .5) {
            for (std::size_t i = 0; i < 29; ++i)
                if (i < 3 || (i >= 4 && i % 2 == 0))
                    control(i, 0);
        }
        sentControls_[30] = values_[30].load(std::memory_order_relaxed);
    }
    const int count = inputEvents.count;
    auto release = [&](Voice &v) {
        int keep = 0;
        for (int j = 0; j < v.count; ++j) {
            const auto index = static_cast<std::size_t>(j);
            auto off = v.release;
            off.type = engine::EventType::NoteOff;
            off.noteId = v.output[index].noteId;
            off.dim = v.output[index].dim;
            if (!queue(off, v.delay[index], &v.repeats[index])) {
                v.output[static_cast<std::size_t>(keep)] = v.output[index];
                v.repeats[static_cast<std::size_t>(keep)] = v.repeats[index];
                v.delay[static_cast<std::size_t>(keep++)] = v.delay[index];
            }
        }
        v.count = keep;
        if (keep == 0)
            v.active = false;
        else {
            v.releasing = true;
            v.release.frame = 0;
        }
    };
    for (auto &v : voices_)
        if (v.active && v.releasing)
            release(v);
    for (int i = 0; i < count; ++i) {
        const auto e = inputEvents.first[i];
        currentSource_ = e.noteId;
        if (e.type == engine::EventType::NoteExpression && e.noteId == 0) {
            emit(out, e);
            continue;
        }
        if (e.type == engine::EventType::ParamValue) {
            if (e.paramId < specs_.size() && std::isfinite(e.value))
                values_[e.paramId].store(std::clamp(e.value, 0., 1.), std::memory_order_relaxed);
            emit(out, e);
            if (kind_ == Kind::CCControl) {
                if (e.paramId < 3 || (e.paramId >= 4 && e.paramId < 29 && e.paramId % 2 == 0))
                    control(e.paramId, e.frame);
                if (e.paramId == 30 && e.value > .5) {
                    for (std::size_t index = 0; index < 3; ++index)
                        control(index, e.frame);
                    for (std::size_t index = 4; index < 29; index += 2)
                        control(index, e.frame);
                }
            }
            continue;
        }
        if (kind_ == Kind::CCControl && e.type == engine::EventType::Control && e.dim < 128 &&
            value(31) > 0) {
            const auto index = 3 + 2 * (static_cast<std::size_t>(value(31)) - 1);
            values_[index].store(static_cast<double>(e.dim) / 127., std::memory_order_relaxed);
            values_[31].store(0, std::memory_order_relaxed);
        }
        if (!engine::isNoteStream(e.type) || e.type == engine::EventType::Control ||
            kind_ == Kind::CCControl) {
            emit(out, e);
            continue;
        }
        if (e.type == engine::EventType::NoteOn && e.value > 0 && e.dim < 128 &&
            std::isfinite(e.value)) {
            auto it = std::find_if(voices_.begin(), voices_.end(),
                                   [](const Voice &v) { return !v.active; });
            if (it == voices_.end()) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            attack(e, *it);
            it->started = clock_ + e.frame;
            it->lengthOff = kind_ == Kind::NoteLength && value(0) > 0;
            it->latch = kind_ == Kind::NoteLength && value(8) > 0;
            if (it->lengthOff)
                continue;
            if (it->latch) {
                for (auto &old : voices_)
                    if (&old != &*it && old.active) {
                        old.release = e;
                        release(old);
                    }
            }
            const auto strum = kind_ == Kind::Chord ? value(20) : 0.;
            int written = 0;
            for (int j = 0; j < it->count; ++j) {
                const auto index = static_cast<std::size_t>(j);
                auto o = it->output[index];
                double position = it->count > 1
                                      ? static_cast<double>(strum < 0 ? it->count - 1 - j : j) /
                                            static_cast<double>(it->count - 1)
                                      : 0.;
                if (kind_ == Kind::Chord) {
                    position = std::pow(position, std::exp2(value(21) * 2));
                    o.value = std::clamp(o.value * (1 + value(22) * (2 * position - 1)), 0., 1.);
                }
                const auto delay = kind_ == Kind::NoteEcho
                                       ? it->delay[index]
                                       : static_cast<std::int64_t>(std::llround(
                                             std::abs(strum) * .001 * rate_ * position));
                if (queue(o, delay, &it->repeats[index])) {
                    it->output[static_cast<std::size_t>(written)] = o;
                    it->repeats[static_cast<std::size_t>(written)] = it->repeats[index];
                    it->delay[static_cast<std::size_t>(written++)] = delay;
                }
            }
            it->count = written;
            if (kind_ == Kind::NoteLength && !it->latch && written > 0) {
                auto off = e;
                off.type = engine::EventType::NoteOff;
                const double ms = (value(1) > 0 ? 60000 / bpm_ * value(3) : value(2)) * value(4);
                queue(off, static_cast<std::int64_t>(std::llround(ms * .001 * rate_)));
            }
        } else {
            if (e.type != engine::EventType::NoteExpression && e.owner)
                e.owner->noteOffDelivered(e);
            auto it = std::find_if(voices_.begin(), voices_.end(), [&](const Voice &v) {
                return v.active && !v.releasing && !v.latchedSounding && same(v.input, e);
            });
            if (it == voices_.end())
                continue;
            if (e.type == engine::EventType::NoteExpression) {
                if ((kind_ == Kind::Chord && value(18) == 0) ||
                    (kind_ == Kind::NoteEcho && value(8) == 0))
                    continue;
                for (int j = 0; j < it->count; ++j) {
                    const auto index = static_cast<std::size_t>(j);
                    auto o = e;
                    o.noteId = it->output[index].noteId;
                    if (kind_ == Kind::NoteEcho) {
                        const auto dimension = e.dim == 0 ? 11u : e.dim == 1 ? 9u : 10u;
                        const auto repeat = static_cast<double>(j + (value(4) > 0 ? 1 : 0));
                        o.value *= std::pow(value(dimension), repeat);
                    }
                    auto repeat = it->repeats[index];
                    if (kind_ == Kind::NoteEcho)
                        repeat.decay = value(e.dim == 0 ? 11u : e.dim == 1 ? 9u : 10u);
                    queue(o, it->delay[index], &repeat);
                }
            } else {
                if (kind_ == Kind::NoteLength) {
                    if (it->lengthOff && it->latch) {
                        it->inputReleased = true;
                        const bool allReleased =
                            std::none_of(voices_.begin(), voices_.end(), [](const Voice &v) {
                                return v.active && v.lengthOff && v.latch && !v.latchedSounding &&
                                       !v.inputReleased;
                            });
                        if (allReleased) {
                            for (auto &old : voices_)
                                if (old.active && old.latchedSounding) {
                                    old.release = e;
                                    release(old);
                                }
                            for (auto &pending : voices_)
                                if (pending.active && pending.lengthOff && pending.latch &&
                                    pending.inputReleased && !pending.latchedSounding) {
                                    auto on = pending.input;
                                    on.frame = e.frame;
                                    if (queue(on, 0))
                                        pending.latchedSounding = true;
                                    else
                                        pending.active = false;
                                }
                        }
                        continue;
                    }
                    if (it->lengthOff) {
                        auto on = it->input;
                        on.frame = e.frame;
                        on.value = it->input.value * (1 - value(5)) + e.value * value(5);
                        if (value(6) > 0)
                            on.value *= std::exp(
                                -(static_cast<double>(clock_ + e.frame - it->started) / rate_) /
                                (value(6) * .001));
                        if (queue(on, 0)) {
                            auto off = on;
                            off.type = engine::EventType::NoteOff;
                            const double ms =
                                (value(1) > 0 ? 60000 / bpm_ * value(3) : value(2)) * value(4) *
                                std::exp2(value(7) * (60 - static_cast<double>(on.dim)) / 12);
                            queue(off, static_cast<std::int64_t>(std::llround(ms * .001 * rate_)));
                        }
                    }
                    if (!it->latch)
                        it->active = false;
                    continue;
                }
                it->release = e;
                if (kind_ == Kind::Velocity && value(0) != 0)
                    it->release.value = std::max(0., velocity(e.value));
                release(*it);
            }
        }
    }
    flush(out, frames);
}
void MidiNotes::process(const engine::NodeIo &io) noexcept {
    for (int c = 0; c < io.channels; ++c)
        if (io.out && io.out[c]) {
            if (io.in && io.in[c])
                std::copy_n(io.in[c] + io.blockOffset, io.frames, io.out[c] + io.blockOffset);
            else
                std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.f);
        }
}
std::vector<std::uint8_t> MidiNotes::saveState(const std::string &role) const {
    if (role != "component")
        return {};
    nlohmann::json j = {{"version", 1}, {"uid", identity_.uid}};
    for (std::size_t i = 0; i < specs_.size(); ++i)
        j["parameters"][specs_[i].descriptor.id] = values_[i].load(std::memory_order_relaxed);
    return nlohmann::json::to_cbor(j);
}
bool MidiNotes::loadState(const std::string &role, const std::vector<std::uint8_t> &bytes) {
    if (role != "component" || bytes.size() > 65536)
        return false;
    try {
        const auto j = nlohmann::json::from_cbor(bytes);
        if (j.at("version") != 1 || j.at("uid") != identity_.uid)
            return false;
        std::array<double, 64> pending{};
        for (std::size_t i = 0; i < specs_.size(); ++i) {
            const double n = j.at("parameters").at(specs_[i].descriptor.id).get<double>();
            if (!std::isfinite(n) || n < 0 || n > 1)
                return false;
            pending[i] = n;
        }
        for (std::size_t i = 0; i < specs_.size(); ++i)
            values_[i].store(pending[i], std::memory_order_relaxed);
        return true;
    } catch (const nlohmann::json::exception &) {
        return false;
    }
}
} // namespace adi::device
