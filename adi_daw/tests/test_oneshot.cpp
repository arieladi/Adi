// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/blob.hpp"
#include "adi/dsp/oneshot.hpp"
#include "adi/pd_builtins/oneshot_midi.hpp"
#include "adi/pd_builtins/oneshot_sample.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#define ADI_TEST_COUNT_REPLACED_DELETE
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>
#include <thread>
namespace {
thread_local bool audit = false;
thread_local unsigned allocations = 0;
thread_local unsigned deallocations = 0;
int checks = 0, failures = 0;
void check(bool ok, const char *name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}
} // namespace
void adi::test::countReplacedNew() noexcept {
    if (audit)
        ++allocations;
}
void adi::test::countReplacedDelete(void *p) noexcept {
    if (audit && p)
        ++deallocations;
}
using namespace adi;
using device::OneShot;
namespace {
void set(OneShot &d, int index, double value) {
    const auto *p = d.paramAt(index);
    check(d.setParam(p->id, device::ParamValue::fromNormalized((value - p->minReal) /
                                                               (p->maxReal - p->minReal))),
          "parameter accepted");
}
std::vector<float> tone(std::size_t n = 65536) {
    std::vector<float> s(n);
    for (std::size_t i = 0; i < n; ++i)
        s[i] = static_cast<float>(
            .4 * std::sin(2 * 3.141592653589793 * 750 * static_cast<double>(i) / 48000));
    return s;
}
void configure(OneShot &d) {
    // Live 12.4's shortest times: Attack 0.1 ms, Release 1 ms (its ranges, DEV-21).
    set(d, OneShot::Attack, .1);
    set(d, OneShot::Release, 1);
    set(d, OneShot::Volume, 0);
    set(d, OneShot::FadeIn, 0);
    set(d, OneShot::FadeOut, 0);
    // The oracles below test mechanics, not defaults: Live's defaults (filter
    // on at 22 kHz, Loop and Snap on, Vel > Vol 35 %) are pinned by their own
    // test, and switched off here so each oracle sees the raw voice.
    set(d, OneShot::FilterOn, 0);
    set(d, OneShot::Snap, 0);
    set(d, OneShot::LoopOn, 0);
    set(d, OneShot::VelToVol, 100);
}
// The 0.1 ms attack at 48 kHz, as the voice runs it: +1/4.8 a sample from 0,
// so full level from the fifth sample on.
constexpr int kAttackFrames = 5;
double attackGain(int frame) {
    double env = 0;
    for (int i = 0; i <= frame; ++i) env = std::min(1., env + 1 / 4.8);
    return env;
}
// Spectral oracles read one 4096-sample window after the attack: 64 frames in
// is one period of 750 Hz and two of 1500 Hz, so the bins stay exact.
constexpr int kSteadyFrames = 4096 + 64;
std::vector<float> steady(const std::vector<float> &x) {
    return {x.begin() + 64, x.begin() + 64 + 4096};
}
std::vector<float> render(OneShot &d, int block, int count = 4096, int key = 60, int off = -1,
                          double velocity = 1) {
    d.release(); // each render starts from silence; prepare() alone is idempotent (DF-E37 f)
    d.prepare(48000, block);
    std::vector<float> out(static_cast<std::size_t>(count));
    std::array<float, 4096> l{}, r{};
    float *ptrs[]{l.data(), r.data()};
    for (int at = 0; at < count; at += block) {
        const int n = std::min(block, count - at);
        engine::Event events[2]{};
        int size = 0;
        if (at == 0) {
            events[size++] = {
                0, engine::EventType::NoteOn, 0, static_cast<std::uint16_t>(key), 1, 0, velocity};
        }
        if (off >= at && off < at + n) {
            events[size++] = {
                off - at, engine::EventType::NoteOff, 0, static_cast<std::uint16_t>(key), 1, 0, 0};
        }
        engine::NodeIo io{};
        io.out = ptrs;
        io.channels = 2;
        io.frames = n;
        io.events = {events, size};
        allocations = 0;
        audit = true;
        d.process(io);
        audit = false;
        if (allocations) {
            ++failures;
            std::printf("FAIL process allocated %u\n", allocations);
        }
        std::copy_n(l.begin(), n, out.begin() + at);
    }
    return out;
}
double bin(const std::vector<float> &x, int k) {
    std::complex<double> z{};
    for (std::size_t i = 0; i < x.size(); ++i)
        z += static_cast<double>(x[i]) *
             std::polar(1., -2 * 3.141592653589793 * k * static_cast<double>(i) /
                                static_cast<double>(x.size()));
    return 2 * std::abs(z) / static_cast<double>(x.size());
}
double energy(const std::vector<float> &x, std::size_t a = 0) {
    double sum = 0;
    for (std::size_t i = a; i < x.size(); ++i)
        sum += x[i] * x[i];
    return sum;
}
bool matches(const std::vector<float>& got, const std::vector<float>& want) {
    if (got.size() != want.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i)
        if (!std::isfinite(got[i]) || std::abs(got[i] - want[i]) > 1e-7f) return false;
    return true;
}
void parameterModulation() {
    constexpr int frames = 320;
    std::vector<float> expected(frames);
    // Independent scalar oracle: a constant sample, unity envelope, fixed
    // velocity, and the requested dB gain. No sampler output feeds the oracle.
    for (int i = 0; i < frames; ++i) {
        const double db = i < 17 ? 0 : i < 65 ? -6 : i < 129 ? -9 : i < 193 ? 3 : -3;
        expected[static_cast<std::size_t>(i)] =
            static_cast<float>(.5 * .625 * std::pow(10., db / 20.) * attackGain(i));
    }
    for (int block : {1, 7, 17, 32, 64, 127, 256, 513}) {
        OneShot sampler;
        configure(sampler);
        check(sampler.publishSample(std::vector<float>(1024, .5f), 1, 48000),
              "modulation oracle sample publishes");
        sampler.prepare(48000, frames);
        const double stored = sampler.getParam("volume").normalized;
        constexpr double automated = 33. / 72.; // -3 dB in Live's -36..36 dB range.
        const std::array<engine::Event, 5> events{{
            {0, engine::EventType::NoteOn, 0, 60, 1, 0, .625},
            {17, engine::EventType::ParamMod, 0, 0, 0, OneShot::Volume, -6. / 72.},
            {65, engine::EventType::ParamValue, 0, 0, 0, OneShot::Volume, automated},
            {129, engine::EventType::ParamMod, 0, 0, 0, OneShot::Volume, 6. / 72.},
            {193, engine::EventType::ParamMod, 0, 0, 0, OneShot::Volume, 0.}
        }};
        std::vector<float> left(frames), right(frames);
        float* output[]{left.data(), right.data()};
        std::size_t next = 0;
        bool noAllocation = true, preserved = true;
        for (int at = 0; at < frames;) {
            // Split once at 65 so the stored-value assertion sees modulation
            // alone before automation, regardless of the tested block size.
            const int end = std::min({frames, at + block, at < 65 ? 65 : frames});
            const auto begin = next;
            while (next < events.size() && events[next].frame < end) ++next;
            engine::NodeIo io{};
            io.out = output;
            io.channels = 2;
            io.blockOffset = at;
            io.frames = end - at;
            io.events = {events.data() + begin, static_cast<std::int32_t>(next - begin)};
            allocations = 0;
            audit = true;
            sampler.process(io);
            audit = false;
            noAllocation &= allocations == 0;
            if (end == 65) preserved &= sampler.getParam("volume").normalized == stored;
            at = end;
        }
        check(matches(left, expected) && matches(right, expected),
              "sample-accurate modulation matches independent dB oracle across callback boundaries");
        check(preserved && sampler.getParam("volume").normalized == automated,
              "modulation preserves stored value before and after automation");
        check(noAllocation, "parameter events and rendering allocate nothing");
    }
    auto plant = expected;
    std::fill(plant.begin(), plant.end(), 0.f);
    check(!matches(plant, expected), "modulation oracle rejects silence");
    plant = expected;
    plant[17] = expected[16];
    check(!matches(plant, expected), "modulation oracle rejects a one-sample event delay");
    plant = expected;
    for (float& sample : plant) sample *= .9f;
    check(!matches(plant, expected), "modulation oracle rejects a gain error");
}
void midiControllers() {
    constexpr auto pitch = static_cast<std::uint16_t>(ExpressionDim::Pitch);
    constexpr auto expression = engine::EventType::NoteExpression;
    constexpr auto cc = engine::EventType::Controller;
    constexpr auto on = engine::EventType::NoteOn;
    constexpr auto off = engine::EventType::NoteOff;
    auto renderEvents = [](OneShot &d, std::span<const engine::Event> events) {
        d.release();
        d.prepare(48000, 256);
        std::array<float, 256> left{}, right{};
        float *output[]{left.data(), right.data()};
        engine::NodeIo io{};
        io.out = output;
        io.channels = 2;
        io.frames = static_cast<int>(left.size());
        io.events = {events.data(), static_cast<std::int32_t>(events.size())};
        allocations = 0;
        audit = true;
        d.process(io);
        audit = false;
        check(allocations == 0, "controller rendering allocates nothing");
        return left;
    };
    OneShot d;
    configure(d);
    check(d.publishSample(std::vector<float>(2048, .5f), 1, 48000), "pedal sample publishes");
    const engine::Event pedal[]{
        {0, on, 1, 60, 1, 0, 1}, {0, on, 2, 62, 2, 0, 1},
        {16, cc, 1, 0, 0, 64, 1},
        {32, off, 1, 60, 1, 0, 0}, {32, off, 2, 62, 2, 0, 0},
        {64, cc, 2, 0, 0, 64, 0}, {160, cc, 1, 0, 0, 64, 0},
    };
    const auto sustained = renderEvents(d, pedal);
    check(sustained[24] == 1.f && sustained[96] == .5f && sustained[159] == .5f &&
              sustained[160] > 0.f && sustained[160] < .5f && sustained[240] == 0.f,
          "CC64 keeps only its channel's key-up voice and then starts its release");
    set(d, OneShot::Mode, 1);
    set(d, OneShot::Gate, 0);
    const auto triggered = renderEvents(d, pedal);
    check(triggered[240] == .5f, "one-shot trigger ignores key-up and pedal-up");
    set(d, OneShot::Gate, 1);
    const auto gated = renderEvents(d, pedal);
    check(gated[240] == 0.f, "one-shot gate still releases when its key goes up");
    set(d, OneShot::Mode, 0);

    std::vector<float> ramp(2048);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i) / 2048.f;
    check(d.publishSample(std::move(ramp), 1, 48000), "pitch and modulation ramp publishes");
    const engine::Event bends[]{
        {0, expression, 1, pitch, 0, 0, 12}, {0, on, 1, 60, 3, 0, 1},
        {8, expression, 2, pitch, 0, 0, -12}, {16, expression, 1, pitch, 3, 0, 12},
        {24, expression, 1, pitch, 0, 0, 0},
    };
    const auto bent = renderEvents(d, bends);
    check(bent[16] == 32.f / 2048 && bent[17] == 36.f / 2048 && bent[25] == 66.f / 2048,
          "channel semitones affect future and active voices alongside per-note pitch expression");
    set(d, OneShot::LfoOn, 1);
    set(d, OneShot::LfoShape, 1);
    set(d, OneShot::LfoPitch, 12);
    const engine::Event wheel[]{
        {0, cc, 1, 0, 0, 1, 0}, {0, on, 1, 60, 4, 0, 1},
        {8, cc, 2, 0, 0, 1, 1}, {16, cc, 1, 0, 0, 1, 1},
    };
    const auto modulated = renderEvents(d, wheel);
    check(modulated[15] == 15.f / 2048 && modulated[16] == 16.f / 2048 &&
              modulated[17] == 18.f / 2048,
          "CC1 scales the authored LFO depth at the event frame on its own channel");
}
void preparedSamples() {
    std::vector<float> original(1024, .25f);
    const auto a = OneShot::prepareSample(original, 1, 48000);
    check(a != nullptr, "sample analysis completes off-thread");
    original.assign(original.size(), -.5f);
    const auto b = OneShot::prepareSample(original, 1, 48000);
    check(b != nullptr, "second immutable sample prepares");
    check(!OneShot::prepareSample({}, 1, 48000) &&
          !OneShot::prepareSample(original, 0, 48000) &&
          !OneShot::prepareSample(original, 3, 48000) &&
          !OneShot::prepareSample(original, 1, 0), "invalid prepared sample shapes reject");
    const std::array<float, 3> odd{0, 1, 0};
    const std::array<float, 1> bad{std::numeric_limits<float>::quiet_NaN()};
    const std::array<std::size_t, 1> outside{1024};
    check(!OneShot::prepareSample(odd, 2, 48000) &&
          !OneShot::prepareSample(bad, 1, 48000) &&
          !OneShot::prepareSample(original, 1, 48000, outside),
          "unaligned stereo, nonfinite audio and out-of-range slices reject");
    OneShot core;
    configure(core);
    core.prepare(48000, 64);
    std::array<float, 64> left{}, right{};
    float* output[]{left.data(), right.data()};
    engine::Event note{0, engine::EventType::NoteOn, 0, 60, 42, 0, 1};
    engine::NodeIo io{};
    io.out = output; io.channels = 2; io.frames = 64;
    bool realtime = true;
    auto process = [&](const OneShot::PreparedSample* sample, std::uint64_t seq, bool on) {
        io.events = on ? engine::EventSpan{&note, 1} : engine::EventSpan{};
        allocations = 0; deallocations = 0; audit = true;
        core.processPrepared(io, sample, seq);
        audit = false;
        realtime &= allocations == 0 && deallocations == 0;
    };
    // From the end of the 0.1 ms attack on (a note starts at frame 0 here).
    auto constant = [&](float value) {
        return std::all_of(left.begin() + kAttackFrames, left.end(), [value](float x) {
            return std::isfinite(x) && std::abs(x - value) < 1e-7f;
        }) && left == right;
    };
    process(a.get(), 1, true);
    check(constant(.25f), "prepared audio owns a copy unaffected by source mutation");
    process(b.get(), 2, false);
    check(constant(0), "new sample generation kills held voices");
    process(b.get(), 2, true);
    check(constant(-.5f), "note at replacement boundary uses new sample");
    process(nullptr, 3, true);
    check(constant(0), "empty publication resets voices and stays silent");
    process(a.get(), 4, false);
    check(constant(0), "recovery does not resurrect cleared voices");
    process(a.get(), 4, true);
    check(constant(.25f), "new note plays after clear");
    process(a.get(), 5, false);
    check(constant(0), "generation change resets even with identical prepared pointer");
    process(a.get(), 0, true);
    check(constant(0), "unpublished generation zero cannot play");
    process(a.get(), 6, true);
    core.release();
    core.prepare(48000, 64);
    process(a.get(), 6, false);
    check(constant(0), "release then a same-rate prepare resets borrowed voices (DF-E37 f)");
    check(realtime, "borrowed rendering and replacement neither allocate nor deallocate");
    set(core, OneShot::LoopOn, 1);
    set(core, OneShot::Retrigger, 0);
    device::OneShotPdMidi repeated;
    auto midiBlock = [&] {
        io.events = repeated.events();
        core.processPrepared(io, a.get(), 8);
        repeated.consumed();
    };
    repeated.note(60, 127, 1); midiBlock();
    repeated.note(60, 127, 1); midiBlock();
    midiBlock();   // the released voice's 1 ms release has ended
    check(constant(.25f), "Pd repeated key releases the previous looping voice with Retrigger off");
    repeated.note(60, 0, 1); midiBlock();
    midiBlock();
    check(constant(0), "Pd repeated key note-off leaves no orphaned loop");
    auto invalid = std::make_unique<device::PdSampleBuffer>();
    invalid->id = 1; invalid->channels = 2; invalid->frames = 2;
    invalid->interleaved = {0, 1, 0}; invalid->sampleRate = 48000;
    check(!device::prepareOneShotSample(std::move(invalid)),
          "Pd preparation rejects inconsistent frame metadata before publication");
    check(!device::clearOneShotSample(0) && device::clearOneShotSample(1) != nullptr,
          "clear uses a real empty snapshot with a valid slot id");
}
void pdMidi() {
    device::OneShotPdMidi midi;
    midi.bend(16383, 2);
    midi.note(60, 127, 2);
    auto events = midi.events();
    check(events.count == 2 && events.first[0].type == engine::EventType::NoteOn &&
          events.first[0].channel == 1 && events.first[0].value == 1 &&
          events.first[1].type == engine::EventType::NoteExpression &&
          events.first[1].noteId == events.first[0].noteId && events.first[1].value == 48,
          "Pd pre-note bend follows note creation with the same reconstructed identity");
    const auto id = events.first[0].noteId;
    midi.consumed();
    midi.bend(0, 2); midi.bend(8192, 2);
    events = midi.events();
    check(events.count == 2 && events.first[0].value == -48 && events.first[1].value == 0,
          "Pd bend endpoints and neutral invert the host wire mapping");
    midi.consumed();
    midi.note(60, 127, 3);
    midi.note(60, 0, 2);
    events = midi.events();
    check(events.count == 2 && events.first[0].noteId != id &&
          events.first[1].type == engine::EventType::NoteOff && events.first[1].noteId == id,
          "Pd same-key notes on different channels release only their own voice");
    midi.consumed();
    midi.note(60, 0, 2); midi.note(60.5f, 127, 1); midi.note(128, 127, 1);
    midi.note(60, -1, 1); midi.bend(std::numeric_limits<float>::quiet_NaN(), 1);
    check(midi.events().empty(), "unknown releases and malformed MIDI cannot affect other notes");
    midi.reset();
    for (std::size_t i = 0; i <= device::OneShotPdMidi::capacity; ++i)
        midi.note(static_cast<float>(i % 128), 127, static_cast<float>(i / 128 + 1));
    check(midi.needsVoiceReset() && midi.dropped() == 1 && midi.events().empty(),
          "bounded queue overflow counts the drop and requests panic instead of stuck notes");
    midi.consumed(); midi.note(60, 127, 1);
    check(!midi.needsVoiceReset() && midi.events().count == 1,
          "MIDI delivery recovers on the tick after overflow");
}
// DEV-21: OneShot's controls carry Live's ranges, units and defaults. The
// numbers are Live 12.4's (the build of 24 April 2026), read from the default
// Simpler preset that build ships, on the director's research machine on 19
// November; the file itself stays there. Live stores Sustain as a level
// 0.000316..1; ADI shows it in dB as Live does (-70..0 dB). Resonance is
// Live's 0..1.25, shown as 0..125 %. Voices: Live's default is the sixth entry
// of its chooser, 6 under any list that begins 1..6; the chooser's list is a
// reading still to take, so ADI keeps every whole number from 1 to 32.
// Several note-ons at their frames, one 64-sample block at a time; the left channel.
std::vector<float> playNotes(OneShot &d, int count, std::initializer_list<std::pair<int, int>> notes) {
    d.release();
    d.prepare(48000, 64);
    std::vector<float> out(static_cast<std::size_t>(count));
    std::array<float, 64> l{}, r{};
    float *ptrs[]{l.data(), r.data()};
    for (int at = 0; at < count; at += 64) {
        engine::Event events[8]{};
        int size = 0;
        for (const auto &[frame, key] : notes)
            if (frame >= at && frame < at + 64 && size < 8)
                events[size++] = {frame - at, engine::EventType::NoteOn, 0,
                                  static_cast<std::uint16_t>(key), 1, 0, 1};
        engine::NodeIo io{};
        io.out = ptrs;
        io.channels = 2;
        io.frames = std::min(64, count - at);
        io.events = {events, size};
        d.process(io);
        std::copy_n(l.begin(), io.frames, out.begin() + at);
    }
    return out;
}
// SC-482: the LFO's Attack, Retrigger, Offset and key amount. A 10 Hz square
// on the volume at full depth: a 0.5 sample plays 0.5 while the LFO is up and
// 0 while it is down, so the output shows the LFO's phase directly.
void lfoControls() {
    OneShot d;
    configure(d);
    check(d.publishSample(std::vector<float>(32768, .5f), 1, 48000), "LFO sample publishes");
    set(d, OneShot::LfoOn, 1);
    set(d, OneShot::LfoShape, 1);
    set(d, OneShot::LfoRate, 10);  // 4,800 samples a cycle; down from 2,400
    set(d, OneShot::LfoVolume, 100);
    const auto about = [](float x, double want) { return std::abs(x - want) < 1e-6; };
    const auto plain = playNotes(d, 8192, {{0, 60}});
    check(about(plain[100], .5) && about(plain[2500], 0) && about(plain[4900], .5),
          "the square LFO's first cycle starts up, as before");
    // Attack: 100 ms, so 52 % depth at 2,500 and full depth from 4,800 on.
    set(d, OneShot::LfoAttack, 100);
    const auto faded = playNotes(d, 8192, {{0, 60}});
    const double depth = 2501. / 4800.;
    check(std::abs(faded[2500] - .25 * (1 - depth)) < 1e-6 && about(faded[7300], 0),
          "LFO Attack fades the depth in over its time");
    set(d, OneShot::LfoAttack, 0);
    // Offset: a quarter cycle in, the square goes down 1,200 samples sooner.
    set(d, OneShot::LfoOffset, 90);
    const auto offset = playNotes(d, 4096, {{0, 60}});
    check(about(offset[1100], .5) && about(offset[1300], 0), "LFO Offset sets the starting phase");
    set(d, OneShot::LfoOffset, 180);
    check(about(playNotes(d, 512, {{0, 60}})[100], 0), "half a cycle in, the note starts down");
    // Retrigger off: a note at 1,200 joins the free-running LFO a quarter
    // cycle in (the LFO ran through the silence before it), so it goes down at
    // 2,400, not at 3,600; Offset no longer applies.
    set(d, OneShot::LfoRetrigger, 0);
    const auto freeRunning = playNotes(d, 4096, {{1200, 60}});
    set(d, OneShot::LfoRetrigger, 1);
    set(d, OneShot::LfoOffset, 0);
    const auto retriggered = playNotes(d, 4096, {{1200, 60}});
    check(about(freeRunning[2300], .5) && about(freeRunning[3000], 0) && about(retriggered[3000], .5),
          "with Retrigger off a note joins the free-running LFO");
    // Key: at 100 %, an octave up runs the LFO twice as fast (down at 1,200).
    set(d, OneShot::LfoKey, 100);
    const auto up = playNotes(d, 4096, {{0, 72}});
    const auto root = playNotes(d, 4096, {{0, 60}});
    set(d, OneShot::LfoKey, 0);
    const auto upFixed = playNotes(d, 4096, {{0, 72}});
    check(about(up[1800], 0) && about(root[1800], .5) && about(upFixed[1800], .5),
          "LFO < Key runs a higher note's LFO faster, and C3's as it was");
}
// SC-482: Glide (mono, legato) and Portamento (poly). On a ramp the output's
// step is the sum of the sounding voices' read increments, 2^(st / 12) each.
void glideModes() {
    std::vector<float> ramp(8192);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i) / 8192.f;
    OneShot d;
    configure(d);
    check(d.publishSample(ramp, 1, 48000), "glide ramp publishes");
    set(d, OneShot::GlideTime, 10);  // 480 samples
    const auto step = [](const std::vector<float> &x, std::size_t i) {
        return (x[i] - x[i - 1]) * 8192.;
    };
    const auto play = [&] { return playNotes(d, 2048, {{0, 60}, {1000, 72}}); };
    const auto off = play();
    check(std::abs(step(off, 1010) - 3) < 2e-3, "with Glide off the octave sounds at once beside C3");
    set(d, OneShot::GlideMode, 1);
    const auto mono = play();
    std::printf("METRIC Glide: step %.4f at +10, %.4f at +240, %.4f at +600\n", step(mono, 1010),
                step(mono, 1240), step(mono, 1600));
    check(std::abs(step(mono, 1001) - 1) < 2e-3,
          "Glide: the new note starts at the held note's pitch, in its voice and place");
    check(std::abs(step(mono, 1240) - std::sqrt(2.)) < .01, "slides through the half-way pitch");
    check(std::abs(step(mono, 1600) - 2) < 2e-3, "and reaches its own, alone");
    set(d, OneShot::GlideMode, 2);
    const auto poly = play();
    std::printf("METRIC Portamento: step %.4f at +10, %.4f at +600\n", step(poly, 1010),
                step(poly, 1600));
    check(step(poly, 1010) > 1.95 && step(poly, 1010) < 2.1,
          "Portamento: a new voice beside the held one, starting at its pitch");
    check(std::abs(step(poly, 1600) - 3) < 2e-3, "and sliding to its own while both sound");
    set(d, OneShot::Mode, 2);
    set(d, OneShot::SlicePlayback, 1);
    const auto sliced = playNotes(d, 2048, {{0, 36}, {1000, 37}});
    check(std::abs(step(sliced, 1010) - 2) < 2e-3, "Slice mode does not glide");
}
// SC-482: Pan and Pan < Rnd, under the LFO pan's law: left sqrt(1 - pan),
// right sqrt(1 + pan), so a 0.5 sample's L^2 + R^2 is 0.5 wherever it sits.
void panControls() {
    OneShot d;
    configure(d);
    check(d.publishSample(std::vector<float>(8192, .5f), 1, 48000), "pan sample publishes");
    set(d, OneShot::Mode, 1);  // each note replaces the last: one pan a block
    std::array<float, 64> l{}, r{};
    float *out[]{l.data(), r.data()};
    const auto notes = [&](int count) {
        std::vector<std::pair<float, float>> heard;
        d.release();
        d.prepare(48000, 64);
        for (int b = 0; b < count; ++b) {
            const engine::Event e{0, engine::EventType::NoteOn, 0,
                                  static_cast<std::uint16_t>(60 + b), static_cast<std::uint64_t>(b + 1),
                                  0, 1};
            engine::NodeIo io{};
            io.out = out;
            io.channels = 2;
            io.frames = 64;
            io.events = {&e, 1};
            d.process(io);
            heard.emplace_back(l[10], r[10]);
        }
        return heard;
    };
    set(d, OneShot::Pan, -50);
    const auto left = notes(1);
    check(std::abs(left[0].first - .5 * std::sqrt(2.)) < 1e-6 && left[0].second == 0,
          "Pan 50 L puts a centred note hard left");
    set(d, OneShot::Pan, 0);
    bool centred = true;
    for (const auto &[a, b] : notes(4))
        centred &= a == .5f && b == .5f;
    check(centred, "with Pan < Rnd at 0 every note sits in the centre");
    set(d, OneShot::PanRandom, 100);
    const auto spread = notes(4);
    bool power = true, moved = false;
    for (const auto &[a, b] : spread) {
        power &= std::abs(a * a + b * b - .5) < 1e-5;
        moved |= std::abs(a - spread[0].first) > .05f;
    }
    check(power && moved, "Pan < Rnd moves each note somewhere else, at the same power");
}
// SC-482: Spread plays a note as two voices, the upper (+25 cents at 100 %)
// panned left and the lower right by the amount, each at half the level. On a
// ramp, a channel's slope is its voice's read increment.
void spreadVoices() {
    std::vector<float> ramp(8192);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i) / 8192.f;
    OneShot d;
    configure(d);
    set(d, OneShot::Mode, 1);  // 1-Shot: plays to the region's end and stops
    std::array<float, 64> l{}, r{};
    float *out[]{l.data(), r.data()};
    const auto both = [&](int count) {
        std::vector<float> left(static_cast<std::size_t>(count)), right(left.size());
        d.release();
        d.prepare(48000, 64);
        for (int at = 0; at < count; at += 64) {
            const engine::Event e{0, engine::EventType::NoteOn, 0, 60, 1, 0, 1};
            engine::NodeIo io{};
            io.out = out;
            io.channels = 2;
            io.frames = 64;
            io.events = {&e, at == 0 ? 1 : 0};
            allocations = 0;
            audit = true;
            d.process(io);
            audit = false;
            check(allocations == 0, "Spread allocates nothing on the audio thread");
            std::copy_n(l.begin(), 64, left.begin() + at);
            std::copy_n(r.begin(), 64, right.begin() + at);
        }
        return std::pair{left, right};
    };
    check(d.publishSample(ramp, 1, 48000), "spread ramp publishes");
    const auto [plainL, plainR] = both(8448);  // Spread at its default, 0
    check(plainL == plainR && std::abs(plainL[1000] - 1000.f / 8192.f) < 1e-6f,
          "Spread 0 plays the one centred voice it always played");
    set(d, OneShot::Spread, 100);
    const auto [wideL, wideR] = both(8448);
    const double rootTwo = std::sqrt(2.), up = std::exp2(.25 / 12), down = std::exp2(-.25 / 12);
    const auto slope = [&](const std::vector<float> &x) {
        return (x[4000] - x[1000]) / 3000. * 8192. / (.5 * rootTwo);
    };
    std::printf("METRIC Spread 100: left increment %.6f (want %.6f), right %.6f (want %.6f)\n",
                slope(wideL), up, slope(wideR), down);
    check(std::abs(slope(wideL) - up) < 2e-4 && std::abs(slope(wideR) - down) < 2e-4,
          "Spread 100: the upper voice hard left, the lower hard right, 50 cents apart");
    // The upper voice reaches the end first (8,192 / 1.0145 = 8,075 samples).
    check(wideL[8150] == 0 && wideR[8150] > .4f && wideL[8400] == 0 && wideR[8400] == 0 &&
              !d.sounding(),
          "the voice that reaches the end stops; the note ends when both have");
    // Half way: each voice half left or right, so a DC sample of 1 sums to
    // 0.5 * (sqrt 1.5 + sqrt 0.5) on each side.
    check(d.publishSample(std::vector<float>(4096, 1.f), 1, 48000), "spread DC publishes");
    set(d, OneShot::Spread, 50);
    const auto [halfL, halfR] = both(512);
    const double want = .5 * (std::sqrt(1.5) + std::sqrt(.5));
    check(std::abs(halfL[200] - want) < 1e-6 && std::abs(halfR[200] - want) < 1e-6,
          "Spread pans its two voices by its amount");
}
// DF-E46: in Slice mode a key with no slice plays nothing and steals nothing;
// a Transient or Manual slice is refused past End and cut at End.
void sliceKeys() {
    std::vector<float> regions(4096);
    for (std::size_t i = 0; i < regions.size(); ++i)
        regions[i] = static_cast<float>(i / 1024 + 1) * .1f;  // .1 .2 .3 .4, 1,024 samples each
    {
        OneShot d;
        configure(d);
        set(d, OneShot::Mode, 2);
        set(d, OneShot::Regions, 4);  // keys 36..39
        set(d, OneShot::SlicePlayback, 0);  // Mono
        d.publishSample(regions, 1, 48000);
        const auto mono = playNotes(d, 512, {{0, 37}, {256, 46}});
        check(std::abs(mono[300] - .2f) < 1e-6f, "Mono: a key with no slice does not cut the playing slice (DF-E46 a)");
        set(d, OneShot::SlicePlayback, 1);  // Poly
        set(d, OneShot::Voices, 2);
        const auto poly = playNotes(d, 512, {{0, 36}, {100, 37}, {200, 46}});
        check(std::abs(poly[300] - .3f) < 1e-6f, "Poly: a key with no slice does not kill the oldest voice (DF-E46 a)");
        // SC-482: Mono plays one slice at a time, Poly lets two overlap, and
        // Poly's Retrigger restarts a playing slice or lets it overlap itself.
        const auto polyPair = playNotes(d, 512, {{0, 36}, {256, 37}});
        const auto again = playNotes(d, 512, {{0, 36}, {256, 36}});
        set(d, OneShot::Retrigger, 0);
        const auto stacked = playNotes(d, 512, {{0, 36}, {256, 36}});
        set(d, OneShot::Retrigger, 1);
        set(d, OneShot::SlicePlayback, 0);
        const auto monoPair = playNotes(d, 512, {{0, 36}, {256, 37}});
        set(d, OneShot::SlicePlayback, 1);
        check(std::abs(monoPair[300] - .2f) < 1e-6f && std::abs(polyPair[300] - .3f) < 1e-6f,
              "Mono plays one slice at a time; Poly lets two overlap");
        check(std::abs(again[300] - .1f) < 1e-6f && std::abs(stacked[300] - .2f) < 1e-6f,
              "Poly: Retrigger on restarts a playing slice, off lets it overlap itself");
        // A refused note still takes the per-note gain queued for it, so the
        // next note gets its own (the Sampler queues one per forwarded note-on).
        set(d, OneShot::SlicePlayback, 0);
        d.release();
        d.prepare(48000, 64);
        d.queueNoteGain(.5);
        d.queueNoteGain(1);
        const auto gains = playNotes(d, 256, {{0, 46}, {64, 37}});
        check(std::abs(gains[100] - .2f) < 1e-6f, "a refused key takes its queued gain; the next note keeps its own");
    }
    {
        OneShot d;
        configure(d);
        set(d, OneShot::Mode, 2);
        set(d, OneShot::SliceBy, 3);  // Manual
        set(d, OneShot::SlicePlayback, 0);
        set(d, OneShot::End, 50);  // End at 2,048
        const std::array<std::size_t, 3> marks{0, 1024, 3072};
        d.publishSample(regions, 1, 48000, marks);
        const auto cut = playNotes(d, 2048, {{0, 37}});  // slice 1024..3072, End 2048
        check(std::abs(cut[500] - .2f) < 1e-6f && cut[1100] == 0, "a Manual slice running past End is cut at End (DF-E46 b)");
        const auto past = playNotes(d, 512, {{0, 38}});  // slice from 3072, past End
        check(energy(past) == 0, "a Manual slice beginning past End is refused: no click (DF-E46 b)");
        set(d, OneShot::SliceBy, 2);  // Transient
        std::vector<float> onsets(4096, 0.f);
        onsets[3072] = .8f;
        d.publishSample(onsets, 1, 48000);
        check(energy(playNotes(d, 512, {{0, 37}})) == 0, "a Transient slice beginning past End is refused too");
    }
}
// SC-482: Slice by Transient's Sensitivity. Clicks of 0.03, 0.08, 0.3 and 0.9
// in silence, 4,800 samples apart: 50 % (the old law: above 0.05) finds the
// last three, 0 % (above 0.2) the last two, 100 % (above 0.0125) all four.
// A slice's first output sample is its click, at full level.
void sliceSensitivity() {
    std::vector<float> clicks(24000, 0.f);
    const std::array<float, 4> levels{.03f, .08f, .3f, .9f};
    for (std::size_t n = 0; n < levels.size(); ++n)
        clicks[4800 * (n + 1)] = levels[n];
    OneShot d;
    configure(d);
    set(d, OneShot::Mode, 2);
    set(d, OneShot::SliceBy, 2);  // Transient
    set(d, OneShot::SlicePlayback, 0);
    check(d.publishSample(clicks, 1, 48000), "clicks publish");
    const auto first = [&](int key) { return playNotes(d, 64, {{0, key}})[0]; };
    const auto silent = [&](int key) { return energy(playNotes(d, 256, {{0, key}})) == 0; };
    check(d.getParam("slice_sensitivity").real == 50 && first(37) == .08f && first(38) == .3f &&
              first(39) == .9f && silent(40),
          "Sensitivity 50 %, the default, slices as OneShot always did");
    set(d, OneShot::SliceSensitivity, 0);
    check(first(37) == .3f && first(38) == .9f && silent(39), "a lower Sensitivity finds fewer slices");
    set(d, OneShot::SliceSensitivity, 100);
    check(first(37) == .03f && first(38) == .08f && first(40) == .9f, "a higher one finds more");
    const auto prepared = OneShot::prepareSample(clicks, 1, 48000);
    check(prepared && OneShot::transientSlices(*prepared, 50) == std::vector<std::size_t>{0, 9600, 14400, 19200} &&
              OneShot::transientSlices(*prepared, 0).size() == 3 &&
              OneShot::transientSlices(*prepared, 100).size() == 5,
          "the window can read the slices each Sensitivity plays");
}
void liveDefaults() {
    struct Row {
        OneShot::Param param;
        double low, high, value;
        const char *unit;
    };
    const Row rows[] = {
        {OneShot::Mode, 0, 2, 0, ""},            {OneShot::Start, 0, 100, 0, "%"},
        {OneShot::Length, .01, 100, 100, "%"},   {OneShot::LoopOn, 0, 1, 1, ""},
        {OneShot::LoopLength, .01, 100, 100, "%"}, {OneShot::Fade, 0, 100, 0, "%"},
        {OneShot::Snap, 0, 1, 1, ""},            {OneShot::Voices, 1, 32, 6, ""},
        {OneShot::Retrigger, 0, 1, 1, ""},       {OneShot::Volume, -36, 36, -12, "dB"},
        {OneShot::Transpose, -48, 48, 0, "st"},  {OneShot::Detune, -50, 50, 0, "cents"},
        {OneShot::Attack, .1, 20000, .1, "ms"},  {OneShot::Decay, 1, 60000, 600, "ms"},
        {OneShot::Sustain, -70, 0, 0, "dB"},     {OneShot::Release, 1, 60000, 50, "ms"},
        {OneShot::FadeIn, 0, 2000, .1, "ms"},    {OneShot::FadeOut, 0, 2000, .1, "ms"},
        {OneShot::FilterOn, 0, 1, 1, ""},        {OneShot::FilterType, 0, 3, 0, ""},
        {OneShot::Slope, 0, 1, 1, ""},           {OneShot::Cutoff, 30, 22000, 22000, "Hz"},
        {OneShot::Resonance, 0, 125, 0, "%"},    {OneShot::LfoOn, 0, 1, 1, ""},
        {OneShot::LfoShape, 0, 5, 0, ""},        {OneShot::LfoRate, .01, 30, 1, "Hz"},
        {OneShot::LfoSync, 0, 1, 0, ""},         {OneShot::VelToVol, 0, 100, 35, "%"},
    };
    OneShot d;
    for (const auto &row : rows) {
        const auto *p = d.paramAt(row.param);
        const bool same = p != nullptr && p->minReal == row.low && p->maxReal == row.high &&
                          std::abs(p->defaultValue.real - row.value) < 1e-12 &&
                          p->unit == row.unit &&
                          std::abs(d.getParam(p->id).real - row.value) < 1e-12;
        check(same, (std::string("Live 12.4's range, unit and default: ") +
                     (p != nullptr ? p->id : std::string("?"))).c_str());
    }
}
// SC-482: a project saved before a control existed opens and plays the same.
// Today's state is the same CBOR with fewer values: those it lacks take their
// defaults, and the defaults play nothing new.
void legacyState() {
    OneShot d;
    configure(d);
    set(d, OneShot::Cutoff, 2000);
    set(d, OneShot::FilterOn, 1);
    set(d, OneShot::Resonance, 60);
    set(d, OneShot::LfoOn, 1);
    set(d, OneShot::LfoPitch, 3);
    set(d, OneShot::LfoRate, 7);
    check(d.publishSample(tone(8192), 1, 48000), "legacy-state sample publishes");
    const auto full = d.saveState("component");
    auto j = nlohmann::json::from_cbor(full);
    check(j.at("params").size() == static_cast<std::size_t>(OneShot::Count),
          "today's state holds every control");
    auto legacy = j;
    legacy["params"] = nlohmann::json::array();
    for (int i = 0; i < OneShot::kLegacyCount; ++i)
        legacy["params"].push_back(j["params"][static_cast<std::size_t>(i)]);
    OneShot opened;
    // A value the old state never held is set first: loading must reset it.
    set(opened, OneShot::FilterEnvAmount, 36);
    set(opened, OneShot::PitchEnvOn, 1);
    set(opened, OneShot::PitchEnvAmount, 12);
    check(opened.loadState("component", nlohmann::json::to_cbor(legacy)),
          "a state saved before SC-482 (41 controls) opens");
    bool defaults = true;
    for (int i = OneShot::kLegacyCount; i < OneShot::Count; ++i) {
        const auto *p = opened.paramAt(i);
        defaults &= opened.getParam(p->id).normalized == p->defaultValue.normalized;
    }
    check(defaults, "every control the old state lacks takes its default");
    check(render(opened, 64, 8192) == render(d, 64, 8192),
          "and the old state plays exactly what it played");
    auto tooMany = j;
    tooMany["params"].push_back(0.5);
    auto tooFew = legacy;
    tooFew["params"].erase(tooFew["params"].size() - 1);
    check(!opened.loadState("component", nlohmann::json::to_cbor(tooMany)) &&
              !opened.loadState("component", nlohmann::json::to_cbor(tooFew)),
          "a state with more controls than this build, or fewer than any build saved, is refused");
}
// SC-482: the filter envelope moves the cutoff and is its own: a 750 Hz tone
// under a 100 Hz 24 dB low-pass opens only as the envelope's attack lets it.
void filterEnvelope() {
    OneShot d;
    configure(d);
    check(d.publishSample(tone(), 1, 48000), "filter-envelope tone publishes");
    set(d, OneShot::FilterOn, 1);
    set(d, OneShot::Slope, 1);
    set(d, OneShot::Cutoff, 100);
    set(d, OneShot::FilterEnvSustain, 100);
    // The window 50..92 ms after note-on, against the open tone's.
    const auto window = [](const std::vector<float> &x) {
        return energy(std::vector<float>(x.begin() + 2400, x.begin() + 4400));
    };
    OneShot unfiltered;
    configure(unfiltered);
    unfiltered.publishSample(tone(), 1, 48000);
    const double reference = window(render(unfiltered, 64, 4400));
    const double closed = window(render(d, 64, 4400));
    set(d, OneShot::FilterEnvAmount, 48);  // four octaves: 1.6 kHz at the top
    set(d, OneShot::FilterEnvAttack, 20);
    const double fast = window(render(d, 64, 4400));
    set(d, OneShot::FilterEnvAttack, 1000);
    const double slow = window(render(d, 64, 4400));
    std::printf("METRIC filter envelope 50..92 ms: closed %.4g, 20 ms attack %.4g, 1 s attack "
                "%.4g, open %.4g\n", closed, fast, slow, reference);
    check(closed < reference * .01, "the closed filter holds the tone back");
    check(fast > reference * .5, "a 20 ms filter-envelope attack has opened the filter by 50 ms");
    check(slow < reference * .05, "a 1 s attack has not: the cutoff follows the envelope's attack");
    set(d, OneShot::FilterEnvAmount, -72);  // six octaves down: 344 Hz
    set(d, OneShot::Cutoff, 22000);
    set(d, OneShot::FilterEnvAttack, 20);
    check(window(render(d, 64, 4400)) < reference * .01, "a negative amount closes the filter");
    const auto blocks = render(d, 64, 4400);
    bool invariant = true;
    for (int size : {1, 33, 513, 4096})
        invariant &= render(d, size, 4400) == blocks;
    check(invariant, "the filter envelope is independent of the callback size");
}
// SC-482: the pitch envelope bends the pitch and falls back to the note's. On
// a ramp the output's step is the read increment: 2^(semitones / 12) / 8192.
void pitchEnvelope() {
    std::vector<float> ramp(8192);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i) / 8192.f;
    OneShot d;
    configure(d);
    check(d.publishSample(ramp, 1, 48000), "pitch-envelope ramp publishes");
    set(d, OneShot::PitchEnvAmount, 12);
    set(d, OneShot::PitchEnvDecay, 10);
    set(d, OneShot::PitchEnvSustain, 0);
    const auto step = [](const std::vector<float> &x, std::size_t i) {
        return (x[i] - x[i - 1]) * 8192.;
    };
    const auto off = render(d, 64, 1200);
    check(std::abs(step(off, 100) - 1) < 1e-3, "with Pitch env off the amount bends nothing");
    set(d, OneShot::PitchEnvOn, 1);
    const auto bent = render(d, 64, 1200);
    // 0.1 ms attack (5 samples), then 480 samples of decay from +12 st to 0.
    const double expected = std::exp2(1. - 95. / 480.);
    std::printf("METRIC pitch envelope: step at 100 %.4f (about %.4f), at 1000 %.4f\n",
                step(bent, 100), expected, step(bent, 1000));
    check(step(bent, 8) > 1.9, "the note starts an octave up");
    check(std::abs(step(bent, 100) - expected) < .01, "and falls through the decay");
    check(std::abs(step(bent, 1000) - 1) < 1e-3, "to the held note's own pitch");
    set(d, OneShot::PitchEnvAmount, -12);
    check(step(render(d, 64, 1200), 8) < .55, "a negative amount starts it below");
}
// A sine at FFT bin k of a 4096-sample window (k * 11.71875 Hz at 48 kHz).
std::vector<float> binTone(int k, double amplitude = .4, std::size_t n = 20480) {
    std::vector<float> s(n);
    for (std::size_t i = 0; i < n; ++i)
        s[i] = static_cast<float>(amplitude * std::sin(2 * 3.141592653589793 * k *
                                                       static_cast<double>(i) / 4096.));
    return s;
}
// The steady window 4096..8191 of a render: the filter's transient is over.
std::vector<float> settled(const std::vector<float> &x) {
    return {x.begin() + 4096, x.begin() + 8192};
}
double peak(const std::vector<float> &x) {
    double m = 0;
    for (float v : x)
        m = std::max(m, static_cast<double>(std::abs(v)));
    return m;
}
// SC-482: the four types at both slopes, the circuits, Drive, and the
// cutoff's velocity and key amounts. Bins 25, 100 and 400 are two octaves apart.
void filterCircuits() {
    constexpr double fc = 100 * 48000. / 4096;  // bin 100: 1171.875 Hz
    const auto gain = [](OneShot &d, int k) {
        check(d.publishSample(binTone(k), 1, 48000), "filter tone publishes");
        return bin(settled(render(d, 64, 8192)), k) / .4;
    };
    {
        OneShot d;
        configure(d);
        set(d, OneShot::FilterOn, 1);
        set(d, OneShot::Cutoff, fc);
        const char *names[] = {"low-pass", "high-pass", "band-pass", "notch"};
        for (int slope = 0; slope < 2; ++slope) {
            set(d, OneShot::Slope, slope);
            const double stopband = slope ? .01 : .1;  // 2 octaves away: 24 or 12 dB an octave
            for (int type = 0; type < 4; ++type) {
                set(d, OneShot::FilterType, type);
                const double below = gain(d, 25), at = gain(d, 100), above = gain(d, 400);
                std::printf("METRIC filter %s %d dB: %.4f at fc/4, %.4f at fc, %.4f at 4 fc\n",
                            names[type], slope ? 24 : 12, below, at, above);
                bool ok = false;
                if (type == 0)
                    ok = below > .9 && above < stopband && above > stopband / 30;
                else if (type == 1)
                    ok = above > .9 && below < stopband && below > stopband / 30;
                else if (type == 2)
                    ok = at > (slope ? .45 : .65) && below < at * .5 && above < at * .5;
                else
                    ok = at < .02 && below > .8 && above > .8;
                check(ok, (std::string("the ") + names[type] + " response at its slope").c_str());
            }
        }
    }
    // Resonant and loud: each circuit but Clean clips its resonance. Quiet:
    // each is Clean's within 2 %, its saturation's slope being 1 at 0.
    OneShot d;
    configure(d);
    set(d, OneShot::FilterOn, 1);
    set(d, OneShot::Slope, 0);
    set(d, OneShot::Cutoff, fc);
    set(d, OneShot::Resonance, 125);
    const auto through = [&](int circuit, double amplitude) {
        set(d, OneShot::FilterCircuit, circuit);
        check(d.publishSample(binTone(100, amplitude), 1, 48000), "resonant tone publishes");
        return peak(settled(render(d, 64, 8192)));
    };
    const double loud = through(OneShot::Clean, .4), quiet = through(OneShot::Clean, .001);
    const char *circuits[] = {"Clean", "OSR", "MS2", "SMP", "PRD"};
    for (int c = OneShot::Osr; c <= OneShot::Prd; ++c) {
        const double clipped = through(c, .4), small = through(c, .001);
        std::printf("METRIC filter circuit %s at Q 10: peak %.4f (Clean %.4f); quiet %.6f (Clean %.6f)\n",
                    circuits[c], clipped, loud, small, quiet);
        check(std::isfinite(clipped) && clipped < loud * .9,
              (std::string(circuits[c]) + " clips a resonant, loud tone").c_str());
        check(std::abs(small - quiet) < quiet * .02,
              (std::string(circuits[c]) + " leaves a quiet tone as Clean does").c_str());
    }
    // Drive: the input's gain into the circuit's saturation, so harmonics.
    set(d, OneShot::Resonance, 0);
    set(d, OneShot::Cutoff, 22000);
    check(d.publishSample(binTone(100), 1, 48000), "drive tone publishes");
    const auto third = [&] {
        const auto x = settled(render(d, 64, 8192));
        return bin(x, 300) / bin(x, 100);
    };
    set(d, OneShot::FilterCircuit, OneShot::Ms2);
    const double plain = third();
    set(d, OneShot::Drive, 24);
    const double driven = third();
    std::printf("METRIC MS2 third harmonic: %.4f at 0 dB drive, %.4f at 24 dB\n", plain, driven);
    check(plain < .02 && driven > .1, "Drive pushes MS2 into its saturation");
    set(d, OneShot::FilterCircuit, OneShot::Clean);
    const auto clean = render(d, 64, 8192);
    set(d, OneShot::Drive, 0);
    check(clean == render(d, 64, 8192), "Clean takes no Drive");
    set(d, OneShot::FilterType, 3);
    set(d, OneShot::FilterCircuitBp, OneShot::Osr);
    const auto notch = render(d, 64, 8192);
    set(d, OneShot::Drive, 24);
    check(notch == render(d, 64, 8192), "the notch takes no Drive");
    set(d, OneShot::Drive, 0);
    set(d, OneShot::FilterType, 2);
    set(d, OneShot::FilterCircuit, OneShot::Ms2);
    set(d, OneShot::FilterCircuitBp, OneShot::Clean);
    const auto band = render(d, 64, 8192);
    set(d, OneShot::FilterCircuit, OneShot::Clean);
    check(band == render(d, 64, 8192), "band-pass reads its own chooser, not the LP/HP one");
    // Key: a note an octave up with Filter < Key at 100 % moves the cutoff an
    // octave with it, so its (doubled) tone is filtered as the root's was.
    {
        OneShot k;
        configure(k);
        set(k, OneShot::FilterOn, 1);
        set(k, OneShot::Cutoff, fc);
        check(k.publishSample(binTone(200), 1, 48000), "key-tracking tone publishes");
        const double root = bin(settled(render(k, 64, 8192, 60)), 200);
        const double fixedUp = bin(settled(render(k, 64, 8192, 72)), 400);
        set(k, OneShot::FilterKey, 100);
        const double trackedUp = bin(settled(render(k, 64, 8192, 72)), 400);
        const double trackedRoot = bin(settled(render(k, 64, 8192, 60)), 200);
        std::printf("METRIC Filter < Key: root %.4f, octave up fixed %.4f, tracked %.4f\n", root,
                    fixedUp, trackedUp);
        check(trackedRoot == root, "key tracking leaves the root note's cutoff alone");
        check(fixedUp < root * .5 && std::abs(trackedUp / root - 1) < .1,
              "and moves the cutoff with the note's pitch");
    }
    // Velocity: Filter < Vel at 100 % opens the cutoff four octaves at full velocity.
    {
        OneShot v;
        configure(v);
        set(v, OneShot::VelToVol, 0);
        set(v, OneShot::FilterOn, 1);
        set(v, OneShot::Cutoff, 25 * 48000. / 4096);
        check(v.publishSample(binTone(100), 1, 48000), "velocity tone publishes");
        const double still = bin(settled(render(v, 64, 8192, 60, -1, 1)), 100);
        set(v, OneShot::FilterVel, 100);
        const double hard = bin(settled(render(v, 64, 8192, 60, -1, 1)), 100);
        const double soft = bin(settled(render(v, 64, 8192, 60, -1, .25)), 100);
        std::printf("METRIC Filter < Vel: off %.4f, full velocity %.4f, quarter %.4f\n", still / .4,
                    hard / .4, soft / .4);
        check(still / .4 < .1 && hard / .4 > .8 && soft / .4 < .2,
              "velocity opens the cutoff by the amount, the harder the note the more");
    }
}
} // namespace
int main() {
    sliceKeys();
    sliceSensitivity();
    liveDefaults();
    legacyState();
    filterEnvelope();
    pitchEnvelope();
    filterCircuits();
    lfoControls();
    glideModes();
    panControls();
    spreadVoices();
    parameterModulation();
    midiControllers();
    preparedSamples();
    pdMidi();
    test::TempDirectory dir{"oneshot", "render"};
    const auto sample = tone();
    OneShot d;
    configure(d);
    check(d.publishSample(sample, 1, 48000), "immutable sample publication");
    auto base = render(d, 64, kSteadyFrames);
    check(bin(steady(base), 733) < 1e-6, "far bin is at floor first");
    check(std::abs(bin(steady(base), 64) - .4) < 1e-6, "original pitch plays at MIDI 60");
    auto octave = render(d, 127, kSteadyFrames, 72);
    check(bin(steady(octave), 733) < 1e-6 && std::abs(bin(steady(octave), 128) - .4) < 1e-6,
          "octave doubles measured frequency");
    bool identical = true;
    for (int b = 32; b <= 4096; ++b)
        if (render(d, b, kSteadyFrames) != base) {
            identical = false;
            break;
        }
    check(identical, "every block size 32 through 4096 is sample-identical");
    auto gated = render(d, 64, 4096, 60, 512);
    // Release 1 ms (Live's shortest): 48 frames at 48 kHz, then silence.
    check(energy(gated, 512) > 0 && energy(gated, 512 + 50) == 0,
          "Classic note-off follows amp release");
    set(d, OneShot::Mode, 1);
    auto triggered = render(d, 64, 4096, 60, 512);
    check(energy(triggered, 512) > 1, "1-Shot Trigger ignores note-off");
    set(d, OneShot::Gate, 1);
    check(energy(render(d, 64, 4096, 60, 512), 513) == 0, "1-Shot Gate releases");
    set(d, OneShot::Mode, 2);
    set(d, OneShot::Regions, 4);
    set(d, OneShot::Gate, 0);
    std::vector<float> regions(4096);
    for (std::size_t i = 0; i < regions.size(); ++i)
        regions[i] = static_cast<float>(i / 1024 + 1) * .1f;
    d.publishSample(regions, 1, 48000);
    auto slice = render(d, 64, 2048, 37);
    check(std::abs(slice[10] - .2f) < 1e-6 && slice[1024] == 0,
          "Slice chromatic key selects region and stops at its end");
    set(d, OneShot::SlicePlayback, 2);
    check(render(d, 64, 2048, 37)[1100] > .29f, "Slice Thru continues into next region");
    set(d, OneShot::SliceBy, 1);
    set(d, OneShot::SlicePlayback, 0);
    set(d, OneShot::BeatDivision, .0625);
    auto beatSlice = render(d, 64, 2048, 37);
    check(std::abs(beatSlice[0] - .2f) < 1e-6 && beatSlice[1500] == 0,
          "beat slice is 1500 samples at 120 BPM and a sixteenth quarter");
    std::vector<float> impulses(4096);
    impulses[512] = .8f;
    impulses[2048] = .8f;
    set(d, OneShot::SliceBy, 2);
    d.publishSample(impulses, 1, 48000);
    check(render(d, 64, 256, 37)[0] > .79f, "transient slice begins at detected onset");
    set(d, OneShot::SliceBy, 3);
    set(d, OneShot::SlicePlayback, 0);
    const std::array<std::size_t, 2> marks{0, 2048};
    d.publishSample(regions, 1, 48000, marks);
    check(render(d, 64, 256, 37)[10] > .29f, "manual slice markers select exact source region");
    set(d, OneShot::Mode, 0);
    set(d, OneShot::LoopOn, 1);
    set(d, OneShot::Length, 25);
    check(energy(render(d, 513, 8192), 4096) > 1, "Classic loop sustains past sample end");
    set(d, OneShot::LoopOn, 0);
    set(d, OneShot::Length, 100);
    d.publishSample(sample, 1, 48000);
    set(d, OneShot::FilterOn, 1);
    set(d, OneShot::Cutoff, 100);
    set(d, OneShot::Slope, 1);
    check(energy(render(d, 64, kSteadyFrames)) < energy(base) * .01,
          "24 dB low-pass attenuates 750 Hz above cutoff");
    set(d, OneShot::FilterOn, 0);
    set(d, OneShot::LfoOn, 1);
    set(d, OneShot::LfoVolume, 100);
    set(d, OneShot::LfoRate, 10);
    check(energy(render(d, 64, kSteadyFrames)) < energy(base) * .7, "per-voice LFO changes volume");
    const auto modulated = render(d, 32, 8192);
    bool modInvariant = true;
    for (int size : {33, 127, 256, 513, 1024, 2048, 4096})
        modInvariant &= render(d, size, 8192) == modulated;
    check(modInvariant, "LFO and voice history are independent of callback size");
    auto state = d.saveState("component");
    OneShot restored;
    check(restored.loadState("component", state), "sample and controls state round-trip");
    check(render(restored, 64) == render(d, 64), "restored sound matches exact samples");
    check(!restored.loadState("component", {1, 2, 3}) && restored.saveState("component") == state,
          "invalid state is rejected without changing sound");
    const auto wave = dir.path() / "oneshot.wav";
    {
        audio::WavWriter w(wave, 48000, 1, audio::WavFormat::Float32);
        w.write(sample.data(), static_cast<std::uint32_t>(sample.size()));
        w.close();
    }
    std::string fileError;
    OneShot fromFile;
    check(fromFile.loadSample(wave, fileError), "file sample loads");
    configure(fromFile);
    check(render(fromFile, 64, kSteadyFrames) == base, "float WAV sample round trip keeps exact values");
    {
        OneShot envelope;
        configure(envelope);
        std::vector<float> constant(8192, .5f);
        envelope.publishSample(constant, 1, 48000);
        set(envelope, OneShot::Attack, 10);
        set(envelope, OneShot::Decay, 10);
        set(envelope, OneShot::Sustain, 20 * std::log10(.5)); // a level of 0.5, in dB as Live shows it
        set(envelope, OneShot::Release, 10);
        const auto curve = render(envelope, 127, 2048, 60, 1200);
        check(std::abs(curve[479] - .5f) < 1e-5 && std::abs(curve[960] - .25f) < 1e-5,
              "ADSR attack and decay land at measured sample times");
        check(curve[1200] < .25f && curve[1681] == 0,
              "ADSR release reaches zero in specified time");
        set(envelope, OneShot::Attack, 0);
        set(envelope, OneShot::Decay, 0);
        set(envelope, OneShot::Sustain, 100);
        set(envelope, OneShot::Mode, 1);
        set(envelope, OneShot::End, 25);
        set(envelope, OneShot::FadeIn, 10);
        set(envelope, OneShot::FadeOut, 10);
        auto fade = render(envelope, 64, 4096);
        check(fade[0] < .002f && std::abs(fade[479] - .5f) < 1e-5 && fade[2047] < .002f &&
                  fade[2048] == 0,
              "1-Shot region endpoint and fade-in/out measured in samples");
        // Two simultaneous notes: Classic sums; 1-Shot replaces its sole voice.
        auto chord = [&] {
            envelope.prepare(48000, 64);
            std::array<float, 64> l{}, r{};
            float *out[]{l.data(), r.data()};
            engine::Event events[] = {{0, engine::EventType::NoteOn, 0, 60, 1, 0, 1},
                                      {0, engine::EventType::NoteOn, 0, 64, 2, 0, 1}};
            engine::NodeIo io{};
            io.out = out;
            io.channels = 2;
            io.frames = 64;
            io.events = {events, 2};
            envelope.process(io);
            return l[10];
        };
        set(envelope, OneShot::FadeIn, 0);
        set(envelope, OneShot::FadeOut, 0);
        check(std::abs(chord() - .5f) < 1e-5, "1-Shot is strictly monophonic");
        set(envelope, OneShot::Mode, 0);
        check(std::abs(chord() - 1.f) < 1e-5, "Classic supports polyphonic voices");
        set(envelope, OneShot::Voices, 1);
        check(std::abs(chord() - .5f) < 1e-5, "voice limit steals oldest voice");
    }
    {
        OneShot loop;configure(loop);std::vector<float> ramp(1024);
        for(std::size_t i=0;i<ramp.size();++i)ramp[i]=static_cast<float>(i)/1024-.5f;
        loop.publishSample(ramp,1,48000);set(loop,OneShot::LoopOn,1);
        auto step=[](const std::vector<float>& audio){float peak=0;for(std::size_t i=kAttackFrames+1;i<audio.size();++i)peak=std::max(peak,std::abs(audio[i]-audio[i-1]));return peak;};
        const auto hard=step(render(loop,64));set(loop,OneShot::Fade,50);
        check(step(render(loop,64))<hard*.1f,"loop crossfade smooths wrap without replaying overlap");
        set(loop,OneShot::Start,1);set(loop,OneShot::Snap,1);
        check(std::abs(render(loop,64)[0])<1e-6,"Snap places region start at left-channel zero crossing");
    }
    {
        // DF-E37 (d): a Classic voice with Loop on keeps looping through its
        // release. Before, it looped only while held: a 100 ms loop with a
        // 1000 ms Release stopped dead at the loop's end, with a click.
        OneShot loop;configure(loop);
        std::vector<float> sine(96000);   // 2 s of 100 Hz: 480 samples a period
        for(std::size_t i=0;i<sine.size();++i)sine[i]=static_cast<float>(.4*std::sin(2*3.141592653589793*100*static_cast<double>(i)/48000));
        loop.publishSample(sine,1,48000);
        set(loop,OneShot::LoopOn,1);set(loop,OneShot::LoopLength,5);   // the last 4800 samples, 10 periods
        set(loop,OneShot::Release,1000);
        constexpr int off=120000;                                        // 2.5 s: well inside the loop
        const auto audio=render(loop,512,off+72000,60,off);
        double rms=0;for(int i=off+9600;i<off+14400;++i)rms+=audio[static_cast<std::size_t>(i)]*audio[static_cast<std::size_t>(i)];
        rms=std::sqrt(rms/4800);
        const double expected=.4/std::sqrt(2.)*(1-.2-.05);   // the release's level 200..300 ms after note-off, on average
        std::printf("METRIC looped release 200..300 ms after note-off: rms %.4f (a release through the loop: about %.4f)\n",rms,expected);
        check(rms>expected*.9,"a looped Classic voice keeps sounding through its release");
        double last=0;for(int i=off+48000+480;i<off+72000;++i)last=std::max(last,static_cast<double>(std::abs(audio[static_cast<std::size_t>(i)])));
        check(last==0,"and ends when the release reaches zero");
        double step=0;for(int i=off-480;i<off+48000;++i)step=std::max(step,static_cast<double>(std::abs(audio[static_cast<std::size_t>(i)+1]-audio[static_cast<std::size_t>(i)])));
        check(step<=.4*2*3.141592653589793*100/48000*1.2,"without a click through the release");
        set(loop,OneShot::LoopOn,0);
        const auto plain=render(loop,512,off+72000,60,off);
        check(std::all_of(plain.begin()+96000,plain.end(),[](float v){return v==0;}),"Loop off still ends at the sample's end");
    }
    {
        // DF-E37 (f): prepare() at an unchanged rate and block size does
        // nothing (ADR-0090 d5); a graph rebuild must not cut a sounding note.
        OneShot held;configure(held);
        std::vector<float> dc(4800,.5f);held.publishSample(dc,1,48000);
        held.prepare(48000,64);
        std::array<float,64> l{},r{};float* out[]{l.data(),r.data()};
        engine::Event note{0,engine::EventType::NoteOn,0,60,7,0,1};
        engine::NodeIo io{};io.out=out;io.channels=2;io.frames=64;io.events={&note,1};
        held.process(io);
        held.prepare(48000,64);
        io.events={};held.process(io);
        check(std::abs(l[10]-.5f)<1e-6f,"a same-format prepare keeps a sounding note (DF-E37 f)");
        held.prepare(44100,64);
        held.process(io);
        check(l[10]==0,"a changed rate still resets the voices");
        held.prepare(44100,64);held.process(io);
        held.release();held.prepare(44100,64);held.process(io);
        check(l[10]==0,"release() then prepare() resets them");
    }
    // One reader renders while the message thread publishes and collects old buffers.
    std::atomic<bool> running{true}, bad{false};
    std::thread worker([&] {
        std::array<float, 64> l{}, r{};
        float *out[]{l.data(), r.data()};
        engine::NodeIo io{};
        io.out = out;
        io.channels = 2;
        io.frames = 64;
        engine::Event retrigger{0,engine::EventType::NoteOn,0,60,1,0,1};io.events={&retrigger,1};
        while (running.load()) {
            audit = true;
            allocations = 0;
            d.process(io);
            audit = false;
            if (allocations)
                bad = true;
            for (float x : l)
                if (!std::isfinite(x))
                    bad = true;
        }
    });
    for (int i = 0; i < 100; ++i) {
        d.publishSample(sample, 1, 48000);
        d.pumpMainThread();
    }
    running = false;
    worker.join();
    check(!bad, "sample publication during rendering is finite and allocation-free");
    // Exercise real Session MIDI playback through its public loader seam.
    // Production discovery/import belongs to the engine/window integration;
    // recovering this DSP does not install that missing factory on main.
    StoreError error{};
    auto store = Store::create(dir.path() / "native.adi", error);
    check(store != nullptr, "create native session");
    store->db().exec(
        "INSERT INTO tracks(id,kind,name,index_in_parent) "
        "VALUES(1,'midi','OneShot',0),(9,'master','Master',1);INSERT INTO mixer_strip(track_id) "
        "VALUES(1),(9);INSERT INTO plugin_refs(id,format,uid,name,subtype) "
        "VALUES(1,'internal','adi.oneshot','OneShot','instrument');INSERT INTO "
        "device_chains(id,track_id) VALUES(1,1);INSERT INTO "
        "devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'OneShot');INSERT INTO "
        "clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(1,1,'midi',0,0,5765760)");
    NoteRecord note{};
    note.note_id = 1;
    note.key = 60;
    note.vel_on = 127;
    note.probability = 10000;
    note.dur_ticks = 5765760;
    check(store->putEventStream(
              1, "notes", writeStream<NoteRecord>(FourCC::Notes, std::vector<NoteRecord>{note})),
          "store native instrument MIDI clip");
    engine::Session session;
    session.graph().setFadeFrames(0);
    const engine::DeviceLoader loader = [](const engine::DeviceRequest& request,
                                           std::string& message)
        -> std::unique_ptr<device::DeviceInstance> {
        if (request.ref && request.ref->format == "internal" &&
            request.ref->uid == "adi.oneshot")
            return std::make_unique<OneShot>();
        message = "unsupported test device";
        return nullptr;
    };
    check(session.load(*store, loader, {2, 48000, 64}),
          "Session loads OneShot through the explicit test loader");
    auto *native = dynamic_cast<OneShot *>(session.instanceFor(1));
    check(native != nullptr, "native device is not placeholder");
    if (native) {
        configure(*native);
        native->publishSample(sample, 1, 48000);
        session.transport().play();
        std::vector<float> output(kSteadyFrames);
        std::array<float, 64> l{}, r{};
        float *out[]{l.data(), r.data()};
        for (int b = 0; b < kSteadyFrames / 64; ++b) {
            session.process({out, nullptr, 2, 0, 64, 0});
            std::copy(l.begin(), l.end(), output.begin() + b * 64);
        }
        check(bin(steady(output), 733) < 1e-6, "Session render far bin floor first");
        check(bin(steady(output), 64) > .1, "native sampler makes sound through ADI Session");
        audio::WavWriter w(dir.path() / "oneshot-session.wav", 48000, 1, audio::WavFormat::Float32);
        w.write(output.data(), 4096);
        w.close();
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
