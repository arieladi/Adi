// SPDX-License-Identifier: GPL-3.0-or-later
#include "sampler.hpp"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
namespace adi::device {
namespace {
// One zone's shape as its saved object: the bank's state and the slot table
// (SC-483) write the same fields under the same names.
nlohmann::json zoneJson(const Sampler::Zone &z) {
    const auto range = [](const Sampler::Range &r) {
        return std::array{r.low, r.high, r.fadeLow, r.fadeHigh};
    };
    return {{"key", range(z.key)},
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
            {"loopEnd", z.loopEnd},
            {"pingPong", z.pingPong},
            {"release", z.release},
            {"releaseLoop", z.releaseLoop},
            {"releaseFade", z.releaseFade},
            {"loopDetune", z.loopDetune},
            {"releaseDetune", z.releaseDetune}};
}
// The other way. A field the object lacks keeps the value z came with; a field
// of the wrong type throws, which the caller turns into a refusal.
void zoneFrom(const nlohmann::json &a, Sampler::Zone &z) {
    const auto range = [&a](const char *name, Sampler::Range &r) {
        if (a.contains(name)) {
            const auto v = a.at(name).get<std::array<double, 4>>();
            r = {v[0], v[1], v[2], v[3]};
        }
    };
    range("key", z.key);
    range("velocity", z.velocity);
    range("select", z.select);
    z.root = a.value("root", z.root);
    z.start = a.value("start", z.start);
    z.end = a.value("end", z.end);
    z.loopStart = a.value("loopStart", z.loopStart);
    z.crossfade = a.value("fade", z.crossfade);
    z.loop = a.value("loop", z.loop);
    z.constantPower = a.value("power", z.constantPower);
    z.gainDb = a.value("gain", z.gainDb);
    z.pan = a.value("pan", z.pan);
    z.detune = a.value("detune", z.detune);
    z.loopEnd = a.value("loopEnd", z.loopEnd);
    z.pingPong = a.value("pingPong", z.pingPong);
    z.release = a.value("release", z.release);
    z.releaseLoop = a.value("releaseLoop", z.releaseLoop);
    z.releaseFade = a.value("releaseFade", z.releaseFade);
    z.loopDetune = a.value("loopDetune", z.loopDetune);
    z.releaseDetune = a.value("releaseDetune", z.releaseDetune);
}
} // namespace
Sampler::Sampler() {
    initialise();
    prepare(48000, 4096);
}
Sampler::Sampler(SlotBacked) {
    initialise();
    slotMode_ = true;
    for (auto &engine : slotEngines_) {
        engine = std::make_unique<OneShot>();
        applyZone(*engine, Zone{});
    }
    slotZones_.publish(std::make_unique<SlotZones>());
    prepare(48000, 4096);
}
void Sampler::initialise() {
    OneShot source;
    for (int i : {OneShot::Volume,    OneShot::Transpose,  OneShot::Attack,   OneShot::Decay,
                  OneShot::Sustain,   OneShot::Release,    OneShot::Voices,   OneShot::Retrigger,
                  OneShot::FilterOn,  OneShot::FilterType, OneShot::Slope,    OneShot::Cutoff,
                  OneShot::Resonance, OneShot::LfoOn,      OneShot::LfoShape, OneShot::LfoRate,
                  OneShot::LfoSync,   OneShot::LfoBeat,    OneShot::LfoPitch, OneShot::LfoVolume,
                  OneShot::LfoPan,    OneShot::LfoFilter, OneShot::VelToVol}) {
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
    // SC-483: the Sampler's own switches, after Sample Select. Reverse is
    // Live's global Reverse; the other is Live's title-bar option "Use
    // Constant Power Fade for Loops", on as Live's is.
    const auto toggle = [this](const char *id, const char *name, double def) {
        ParamDescriptor p;
        p.id = id;
        p.name = name;
        p.hasRealRange = true;
        p.minReal = 0;
        p.maxReal = 1;
        p.domain = ParamDomain::Enum;
        p.shape = ParamShape::Switch;
        p.stepCount = 2;
        p.defaultValue = ParamValue::withReal(def, def);
        params_.push_back(p);
        destinations_.push_back(OneShot::Count);
    };
    toggle("reverse", "Reverse", 0);
    toggle("loop_fade_power", "Loop fade: constant power", 1);
    // Live's Round Robin: off, the layers a note matches all play it, as before.
    toggle("round_robin", "Round Robin", 0);
    const auto menu = [this](const char *id, const char *name, int items, double def) {
        ParamDescriptor p;
        p.id = id;
        p.name = name;
        p.hasRealRange = true;
        p.minReal = 0;
        p.maxReal = items - 1;
        p.domain = ParamDomain::Enum;
        p.shape = ParamShape::Menu;
        p.stepCount = items;
        p.defaultValue = ParamValue::withReal(def / (items - 1), def);
        params_.push_back(p);
        destinations_.push_back(OneShot::Count);
    };
    menu("round_robin_mode", "Round Robin mode", 4, 0);
    // Live's Interpolation: None, Normal, Good. Normal, ADI's straight line
    // between frames, is the sound every Sampler had before it.
    menu("interpolation", "Interpolation", 3, 1);
    // Live's Snap: the start, end and loop points move to the next zero crossing.
    toggle("snap", "Snap", 0);
    const auto dial = [this](const char *id, const char *name, const char *unit, double lo, double hi,
                             int steps) {
        ParamDescriptor p;
        p.id = id;
        p.name = name;
        p.unit = unit;
        p.hasRealRange = true;
        p.minReal = lo;
        p.maxReal = hi;
        p.domain = steps ? ParamDomain::Enum : ParamDomain::Real;
        p.shape = steps ? ParamShape::Menu : ParamShape::Continuous;
        p.stepCount = steps;
        p.defaultValue = ParamValue::withReal(lo < 0 ? -lo / (hi - lo) : 0., 0);  // at 0
        params_.push_back(p);
        destinations_.push_back(OneShot::Count);
    };
    // Live's Pitch/Osc tab: the instrument's Detune, Spread (two voices a
    // note, one each side, detuned apart) and Key Zone Shift (the key a note
    // chooses its zones by, its pitch kept). Each is 0 by default: no change.
    dial("detune", "Detune", "cents", -50, 50, 0);
    dial("spread", "Spread", "%", 0, 100, 0);
    dial("zone_shift", "Zone shift", "st", -48, 48, 97);
    for (std::size_t i = 0; i < params_.size(); ++i)
        values_[i].store(params_[i].defaultValue.normalized);
}
bool Sampler::setParamAt(std::size_t index, double normalized) noexcept {
    if (index >= params_.size() || !std::isfinite(normalized)) {
        return false;
    }
    const auto &p = params_[index];
    const double r = real(p, normalized);
    const double n = p.maxReal > p.minReal ? (r - p.minReal) / (p.maxReal - p.minReal) : 0;
    values_[index].store(std::clamp(n, 0., 1.), std::memory_order_relaxed);
    return true;
}
const ParamDescriptor *Sampler::paramAt(std::int32_t i) const noexcept {
    return i >= 0 && static_cast<std::size_t>(i) < params_.size()
               ? &params_[static_cast<std::size_t>(i)]
               : nullptr;
}
ParamValue Sampler::getParam(const std::string &id) const noexcept {
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (params_[i].id == id) {
            const double n = values_[i].load(std::memory_order_relaxed);
            return ParamValue::withReal(n, real(params_[i], n));
        }
    return {};
}
double Sampler::real(const ParamDescriptor &p, double normalized) noexcept {
    const double r = p.minReal + std::clamp(normalized, 0., 1.) * (p.maxReal - p.minReal);
    return p.stepCount > 0 ? std::round(r) : r;
}
bool Sampler::setParam(const std::string &id, const ParamValue &v) {
    if (!std::isfinite(v.normalized))
        return false;
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (params_[i].id == id) {
            // Canonical, as OneShot stores it: a stepped control lands on a step.
            const auto &p = params_[i];
            const double r = real(p, v.normalized);
            const double n = p.maxReal > p.minReal ? (r - p.minReal) / (p.maxReal - p.minReal) : 0;
            values_[i].store(std::clamp(n, 0., 1.), std::memory_order_relaxed);
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
    for (double v : {z.start, z.end, z.loopStart, z.crossfade, z.gainDb, z.pan, z.detune, z.loopEnd,
                     z.releaseLoop, z.releaseFade, z.loopDetune, z.releaseDetune})
        if (!std::isfinite(v))
            return false;
    if (std::abs(z.loopDetune) > 50 || std::abs(z.releaseDetune) > 50)
        return false;
    // SC-483: a sustain loop ends inside the zone, after it starts; a release
    // loop starts inside the zone.
    if (!(z.loopEnd < 0 || (z.loopEnd > z.loopStart && z.loopEnd <= z.end)) || z.release < 0 ||
        z.release > 3 || z.releaseLoop < 0 || z.releaseLoop > 1 || z.releaseFade < 0 ||
        z.releaseFade > 1 || (z.release >= 2 && (z.releaseLoop < z.start || z.releaseLoop >= z.end)))
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
void Sampler::applyZone(OneShot &engine, const Zone &z) noexcept {
    engine.setRootKey(z.root);
    engine.setRealAt(OneShot::Mode, 0);
    engine.setRealAt(OneShot::Snap, 0);
    engine.setRealAt(OneShot::Start, z.start * 100);
    engine.setRealAt(OneShot::End, z.end * 100);
    engine.setRealAt(OneShot::Length, 100);
    engine.setRealAt(OneShot::LoopOn, z.loop ? 1 : 0);
    engine.setRealAt(OneShot::LoopLength, 100 * (z.end - z.loopStart) / (z.end - z.start));
    engine.setRealAt(OneShot::Fade, z.crossfade * 100);
    engine.setRealAt(OneShot::Detune, z.detune);
    engine.setRealAt(OneShot::Gain, 0);
}
bool Sampler::publishSlotZones(std::span<const Zone, kSlots> zones) {
    if (!slotMode_) {
        return false;
    }
    auto table = std::make_unique<SlotZones>();
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (!valid(zones[i]) || !zones[i].samples.empty()) {
            return false;
        }
        table->zones[i] = zones[i];
    }
    slotTable_ = table->zones;
    slotZones_.publish(std::move(table));
    return true;
}
std::vector<std::uint8_t> Sampler::saveSlotZones() const {
    nlohmann::json j = {{"version", 1}, {"zones", nlohmann::json::array()}};
    const auto fresh = zoneJson(Zone{});
    for (std::size_t i = 0; i < kSlots; ++i) {
        auto zone = zoneJson(slotTable_[i]);
        if (zone == fresh) {
            continue;
        }
        zone["slot"] = i + 1;
        j["zones"].push_back(std::move(zone));
    }
    return nlohmann::json::to_cbor(j);
}
bool Sampler::loadSlotZones(std::span<const std::uint8_t> bytes) {
    if (!slotMode_ || bytes.empty() || bytes.size() > 1024 * 1024) {
        return false;
    }
    try {
        const auto j = nlohmann::json::from_cbor(bytes.begin(), bytes.end());
        if (!j.is_object() || j.at("version") != 1 || !j.at("zones").is_array() ||
            j.at("zones").size() > kSlots) {
            return false;
        }
        std::array<Zone, kSlots> table{};
        std::array<bool, kSlots> listed{};
        for (const auto &a : j.at("zones")) {
            const auto slot = a.at("slot").get<std::int64_t>();
            if (slot < 1 || slot > static_cast<std::int64_t>(kSlots)) {
                return false;
            }
            const auto index = static_cast<std::size_t>(slot - 1);
            if (listed[index]) {
                return false;
            }
            listed[index] = true;
            zoneFrom(a, table[index]);
        }
        return publishSlotZones(table);
    } catch (const nlohmann::json::exception &) {
        return false;
    }
}
void Sampler::resetVoices() noexcept {
    midiChannels_ = {};
    for (auto &engine : slotEngines_) {
        if (engine) {
            engine->resetVoices();
        }
    }
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
    set(OneShot::Snap, 0); // a zone's points are exact; Live's per-zone Snap is unread
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
    if (slotMode_ || zones.size() > 32)
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
    rate = std::isfinite(rate) && rate >= 1000 && rate <= 768000 ? rate : 48000;
    frames = std::max(1, frames);
    // Idempotent (ADR-0090 d5): the same format again keeps sounding voices.
    if (prepared_ && rate == rate_ && frames == maxFrames_)
        return;
    prepared_ = true;
    rate_ = rate;
    maxFrames_ = frames;
    scratch_.resize(static_cast<std::size_t>(maxFrames_) * 2);
    // The graph's capacity for a block, or the hosted MIDI's, whichever is
    // larger, plus one ParamMod per parameter pushed ahead of them.
    events_.resize(std::max(static_cast<std::size_t>(engine::Graph::deriveEventCapacity(
                                rate_, maxFrames_, 32)),
                            kEventCapacity) +
                   params_.size());
    chosen_.assign(events_.size(), kNoLayer);
    robinCount_ = 0;
    robinSeed_ = 1;
    robinLast_ = kNoLayer;
    modulation_.fill(0);
    midiChannels_ = {};
    if (messageBank_)
        for (auto &l : messageBank_->layers) {
            // A Sampler that prepares afresh prepares its layers afresh: their
            // own prepare() is idempotent now (DF-E37 f).
            l.engine->release();
            l.engine->prepare(rate_, maxFrames_);
        }
    for (auto &engine : slotEngines_) {
        if (engine) {
            engine->release();
            engine->prepare(rate_, maxFrames_);
        }
    }
}
void Sampler::chooseLayers(const engine::NodeIo &io,
                           const std::array<double, OneShot::Count> &initial,
                           std::span<const Zone *const, kSlots> zones) noexcept {
    roundRobin_ = initial[kRoundRobin] + modulation_[kRoundRobin] >= .5;
    if (!roundRobin_) {
        return;
    }
    // Forward, Backward, Random, Other (random, never the layer before).
    const int mode = static_cast<int>(
        real(params_[kRoundRobinMode], initial[kRoundRobinMode] + modulation_[kRoundRobinMode]));
    double select = initial[kSelect], selectMod = modulation_[kSelect];
    const double shift = real(params_[kZoneShift], initial[kZoneShift] + modulation_[kZoneShift]);
    std::size_t note = 0;
    for (const auto &e : io.events) {
        if ((e.type == engine::EventType::ParamValue || e.type == engine::EventType::ParamMod) &&
            e.paramId == kSelect && std::isfinite(e.value)) {
            // Sample Select as each layer will read it at this note.
            if (e.type == engine::EventType::ParamValue) {
                select = std::clamp(e.value, 0., 1.);
            } else {
                selectMod = e.value;
            }
            continue;
        }
        if (e.type != engine::EventType::NoteOn || !(e.value > 0)) {
            continue;
        }
        if (note == chosen_.size()) {
            break;
        }
        std::array<std::int8_t, kSlots> matching{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < kSlots; ++i) {
            if (zones[i] == nullptr) {
                continue;
            }
            const auto &z = *zones[i];
            if (weight(z.key, e.dim + shift, z.constantPower) * weight(z.velocity, e.value * 127, z.constantPower) *
                    weight(z.select, std::clamp(select + selectMod, 0., 1.) * 127, z.constantPower) > 0) {
                matching[count++] = static_cast<std::int8_t>(i);
            }
        }
        std::int8_t pick = kNoLayer;
        if (count > 0) {
            std::size_t at = 0;
            if (mode == 0) {
                at = robinCount_ % count;
            } else if (mode == 1) {
                at = count - 1 - robinCount_ % count;
            } else {
                robinSeed_ = 1664525u * robinSeed_ + 1013904223u;
                const std::size_t draw = robinSeed_ >> 16;
                std::size_t before = count;   // where the layer before sits among these, if it does
                if (mode == 3 && count > 1) {
                    for (std::size_t k = 0; k < count; ++k) {
                        if (matching[k] == robinLast_) {
                            before = k;
                        }
                    }
                }
                if (before < count) {
                    at = draw % (count - 1);
                    if (at >= before) {
                        ++at;
                    }
                } else {
                    at = draw % count;
                }
            }
            ++robinCount_;
            pick = matching[at];
            robinLast_ = pick;
        }
        chosen_[note++] = pick;
    }
}
void Sampler::playLayer(const engine::NodeIo &io, OneShot &engine, const Zone &z,
                        const std::array<double, OneShot::Count> &initial,
                        const OneShot::PreparedSample *sample, std::uint64_t generation,
                        bool slot, std::size_t layer) noexcept {
    engine.setMidiChannels(midiChannels_);
    // This block's values, by index: no string lookup per layer (DEV-21).
    for (std::size_t i = 0; i < kSelect; ++i) {
        engine.setParamAt(static_cast<int>(destinations_[i]), initial[i]);
    }
    engine::EventList events(events_.data(), static_cast<int>(events_.size()));
    for (std::size_t i = 0; i < kSelect; ++i) {
        engine::Event e;
        e.type = engine::EventType::ParamMod;
        e.frame = io.blockOffset;
        e.paramId = destinations_[i];
        e.value = modulation_[i];
        events.push(e);
    }
    double select = initial[kSelect], selectMod = modulation_[kSelect];
    // SC-483: Reverse is latched by a note-on, as Sample Select is; the zone's
    // loops and the loop fades' law are this block's for the layer's voices.
    double reverse = initial[kReverse], reverseMod = modulation_[kReverse];
    OneShot::Travel travel;
    travel.loopEnd = z.loopEnd;
    travel.pingPong = z.pingPong;
    travel.release = z.release;
    travel.releaseLoop = z.releaseLoop;
    travel.releaseFade = z.releaseFade;
    travel.linearFade = initial[kLoopFadePower] + modulation_[kLoopFadePower] < .5;
    travel.loopCents = z.loopDetune;
    travel.releaseCents = z.releaseDetune;
    travel.interpolation = std::clamp(
        static_cast<int>(real(params_[kInterpolation], initial[kInterpolation] + modulation_[kInterpolation])), 0, 2);
    travel.cents = real(params_[kDetune], initial[kDetune] + modulation_[kDetune]);
    const double spread = real(params_[kSpread], initial[kSpread] + modulation_[kSpread]);
    const double shift = real(params_[kZoneShift], initial[kZoneShift] + modulation_[kZoneShift]);
    engine.setTravel(travel);
    engine.setRealAt(OneShot::Snap, initial[kSnap] + modulation_[kSnap] >= .5 ? 1 : 0);
    bool notes = false;  // a note-on this layer takes this block
    std::size_t noteOn = 0;  // which note-on of the call, as chooseLayers counted them
    for (const auto &input : io.events) {
        auto e = input;
        if (e.type == engine::EventType::ParamValue || e.type == engine::EventType::ParamMod) {
            if (e.paramId >= params_.size() || !std::isfinite(e.value)) {
                continue;
            }
            if (e.paramId >= kSelect) {
                // The Sampler's own parameters never reach a layer.
                const bool value = e.type == engine::EventType::ParamValue;
                if (e.paramId == kSelect) {
                    (value ? select : selectMod) = value ? std::clamp(e.value, 0., 1.) : e.value;
                } else if (e.paramId == kReverse) {
                    (value ? reverse : reverseMod) = value ? std::clamp(e.value, 0., 1.) : e.value;
                }
                continue;
            }
            e.paramId = destinations_[e.paramId];
        } else if (e.type == engine::EventType::NoteOn && e.value > 0) {
            // SC-483, Round Robin: one of the matching layers plays the note.
            const std::size_t index = noteOn++;
            if (roundRobin_ && (index >= chosen_.size() ||
                                chosen_[index] != static_cast<std::int8_t>(layer))) {
                continue;
            }
            // SC-483, Key Zone Shift: the note chooses its zones as the key
            // `shift` semitones away, and keeps its own pitch.
            const double w = weight(z.key, e.dim + shift, z.constantPower) *
                             weight(z.velocity, e.value * 127, z.constantPower) *
                             weight(z.select, std::clamp(select + selectMod, 0., 1.) * 127,
                                    z.constantPower);
            if (w <= 0) {
                continue;
            }
            // DF-E37 (e): the zone's weight is the voice's own gain, latched
            // at note-on -- not a scaled velocity, which Vel > Vol would
            // squeeze (0.825 for a 0.5 fade at 35 %). Queued only for a
            // note-on the layer will see, so later notes keep their own.
            const bool backwards = reverse + reverseMod >= .5;
            if (spread > 0) {
                // SC-483, Spread: two voices, the lower one left, the higher right.
                const double cents = spread / 100 * kSpreadCents;
                if (events.push(e)) {
                    engine.queueNoteGain(w, backwards, -cents, -1);
                    notes = true;
                    if (events.push(e)) {
                        engine.queueNoteGain(w, backwards, cents, 1, true);
                    }
                }
            } else if (events.push(e)) {
                engine.queueNoteGain(w, backwards);
                notes = true;
            }
            continue;
        }
        // If a release cannot fit, discard this layer's segment and queued
        // onsets together so none of those onsets can get stuck.
        if (!events.push(e)) {
            if (e.type == engine::EventType::NoteOff ||
                (e.type == engine::EventType::Controller && e.paramId == 64 && e.value < .5)) {
                engine.resetVoices();
                return;
            }
        } else if (e.type == engine::EventType::NoteOn) {
            notes = true;  // a velocity-0 note-on still reaches the engine
        }
    }
    float *channels[]{scratch_.data(), scratch_.data() + maxFrames_};
    auto child = io;
    child.out = channels;
    child.channels = 2;
    child.events = {events.begin(), events.size()};
    // A layer with no voice sounding and no note-on plays exact silence
    // (OneShot's own fast path): it is not mixed. 32 layers, most of them idle.
    const bool silent = !notes && !engine.sounding();
    if (slot) {
        engine.processPrepared(child, sample, generation);
    } else {
        engine.process(child);
    }
    if (silent) {
        return;
    }
    const double gain = std::pow(10., z.gainDb / 20.);
    if (io.out) {
        for (int c = 0; c < std::min(2, io.channels); ++c) {
            if (io.out[c]) {
                const auto g = gain * std::sqrt(1 + (c == 0 ? -z.pan : z.pan));
                for (int i = io.blockOffset; i < io.blockOffset + io.frames; ++i) {
                    io.out[c][i] += static_cast<float>(channels[c][i] * g);
                }
            }
        }
    }
}
void Sampler::process(const engine::NodeIo &io) noexcept {
    if (slotMode_ || io.frames <= 0 || io.blockOffset < 0 || io.blockOffset + io.frames > maxFrames_)
        return;
    if (io.out)
        for (int c = 0; c < io.channels; ++c)
            if (io.out[c])
                std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.f);
    engine::SnapshotPublisher<Bank>::AudioRead bank(publisher_);
    std::array<double, OneShot::Count> initial{};
    for (std::size_t i = 0; i < params_.size(); ++i)
        initial[i] = values_[i].load(std::memory_order_relaxed);
    if (bank.valid()) {
        std::array<const Zone *, kSlots> zones{};
        for (std::size_t i = 0; i < std::min(bank->layers.size(), kSlots); ++i)
            zones[i] = &bank->layers[i].zone;
        chooseLayers(io, initial, zones);
        for (std::size_t i = 0; i < bank->layers.size(); ++i)
            playLayer(io, *bank->layers[i].engine, bank->layers[i].zone, initial, nullptr, 0, false, i);
    }
    rememberEvents(io.events);
}
void Sampler::processSlots(const engine::NodeIo &io,
                           std::span<const SlotSample, kSlots> slots) noexcept {
    if (!slotMode_ || io.frames <= 0 || io.blockOffset < 0 ||
        io.blockOffset + io.frames > maxFrames_) {
        return;
    }
    if (io.out) {
        for (int c = 0; c < io.channels; ++c) {
            if (io.out[c]) {
                std::fill_n(io.out[c] + io.blockOffset, io.frames, 0.f);
            }
        }
    }
    engine::SnapshotPublisher<SlotZones>::AudioRead table(slotZones_);
    if (!table.valid()) {
        return;
    }
    // A new zone table reshapes the layers here, on the audio thread, where
    // the layers' own state is read (setRootKey is a plain field).
    if (table->seq != slotZonesSeen_) {
        for (std::size_t i = 0; i < kSlots; ++i) {
            applyZone(*slotEngines_[i], table->zones[i]);
        }
        slotZonesSeen_ = table->seq;
    }
    std::array<double, OneShot::Count> initial{};
    for (std::size_t i = 0; i < params_.size(); ++i) {
        initial[i] = values_[i].load(std::memory_order_relaxed);
    }
    {
        // Round Robin chooses among the filled slots only.
        std::array<const Zone *, kSlots> zones{};
        for (std::size_t i = 0; i < kSlots; ++i) {
            if (slots[i].sample != nullptr && slots[i].generation != 0) {
                zones[i] = &table->zones[i];
            }
        }
        chooseLayers(io, initial, zones);
    }
    for (std::size_t i = 0; i < kSlots; ++i) {
        auto &engine = *slotEngines_[i];
        const auto &slot = slots[i];
        if (slot.sample == nullptr || slot.generation == 0) {
            // An empty slot takes no notes and queues no gains; one emptied
            // since the last call stops its voices.
            if (slotSeen_[i] != 0) {
                engine.resetVoices();
                slotSeen_[i] = 0;
            }
            continue;
        }
        slotSeen_[i] = slot.generation;
        playLayer(io, engine, table->zones[i], initial, slot.sample, slot.generation, true, i);
    }
    rememberEvents(io.events);
}
void Sampler::rememberEvents(engine::EventSpan events) noexcept {
    for (const auto &e : events) {
        if (engine::isNoteStream(e.type)) {
            OneShot::rememberMidiControl(midiChannels_, e);
            continue;
        }
        if (e.paramId < params_.size() && std::isfinite(e.value)) {
            if (e.type == engine::EventType::ParamValue) {
                values_[e.paramId].store(std::clamp(e.value, 0., 1.), std::memory_order_relaxed);
            } else if (e.type == engine::EventType::ParamMod) {
                modulation_[e.paramId] = e.value;
            }
        }
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
            auto zone = zoneJson(l.zone);
            zone["voice"] = nlohmann::json::binary(l.engine->saveState("component"));
            j["zones"].push_back(std::move(zone));
        }
    return nlohmann::json::to_cbor(j);
}
bool Sampler::loadState(const std::string &role, const std::vector<std::uint8_t> &bytes) {
    if (role != "component" || bytes.size() > 160 * 1024 * 1024)
        return false;
    try {
        const auto j = nlohmann::json::from_cbor(bytes);
        // A state from before SC-483 holds the first 24 parameters; the ones
        // added since take their defaults.
        if (j.at("version") != 1 || j.at("zones").size() > 32 ||
            j.at("parameters").size() < kFirstParams || j.at("parameters").size() > params_.size())
            return false;
        auto values = j.at("parameters").get<std::vector<double>>();
        for (std::size_t i = values.size(); i < params_.size(); ++i)
            values.push_back(params_[i].defaultValue.normalized);
        for (double v : values)
            if (!std::isfinite(v) || v < 0 || v > 1)
                return false;
        auto bank = std::make_unique<Bank>();
        for (const auto &a : j.at("zones")) {
            Layer l;
            auto &z = l.zone;
            // Every field a version 1 state wrote is required, as before.
            for (const char *name : {"key", "velocity", "select", "root", "start", "end", "loopStart",
                                     "fade", "loop", "power", "gain", "pan", "detune"})
                (void)a.at(name);
            zoneFrom(a, z);
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
