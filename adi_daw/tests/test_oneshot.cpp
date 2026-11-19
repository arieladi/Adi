// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/blob.hpp"
#include "adi/dsp/oneshot.hpp"
#include "adi/pd_builtins/oneshot_midi.hpp"
#include "adi/pd_builtins/oneshot_sample.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
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
void *operator new(std::size_t n) {
    if (audit)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { if (audit && p) ++deallocations; std::free(p); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
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
    set(d, OneShot::Attack, 0);
    set(d, OneShot::Release, 0);
    set(d, OneShot::Volume, 0);
    set(d, OneShot::FadeOut, 0);
}
std::vector<float> render(OneShot &d, int block, int count = 4096, int key = 60, int off = -1) {
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
                0, engine::EventType::NoteOn, 0, static_cast<std::uint16_t>(key), 1, 0, 1};
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
        expected[static_cast<std::size_t>(i)] = static_cast<float>(.5 * .625 * std::pow(10., db / 20.));
    }
    for (int block : {1, 7, 17, 32, 64, 127, 256, 513}) {
        OneShot sampler;
        configure(sampler);
        check(sampler.publishSample(std::vector<float>(1024, .5f), 1, 48000),
              "modulation oracle sample publishes");
        sampler.prepare(48000, frames);
        const double stored = sampler.getParam("volume").normalized;
        constexpr double automated = 87. / 102.; // -3 dB in the declared -90..12 range.
        const std::array<engine::Event, 5> events{{
            {0, engine::EventType::NoteOn, 0, 60, 1, 0, .625},
            {17, engine::EventType::ParamMod, 0, 0, 0, OneShot::Volume, -6. / 102.},
            {65, engine::EventType::ParamValue, 0, 0, 0, OneShot::Volume, automated},
            {129, engine::EventType::ParamMod, 0, 0, 0, OneShot::Volume, 6. / 102.},
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
    auto constant = [&](float value) {
        return std::all_of(left.begin(), left.end(), [value](float x) {
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
    core.prepare(48000, 64);
    process(a.get(), 6, false);
    check(constant(0), "same-rate reprepare resets borrowed voices");
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
    check(constant(.25f), "Pd repeated key releases the previous looping voice with Retrigger off");
    repeated.note(60, 0, 1); midiBlock();
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
} // namespace
int main() {
    parameterModulation();
    preparedSamples();
    pdMidi();
    test::TempDirectory dir{"oneshot", "render"};
    const auto sample = tone();
    OneShot d;
    configure(d);
    check(d.publishSample(sample, 1, 48000), "immutable sample publication");
    auto base = render(d, 64);
    check(bin(base, 733) < 1e-6, "far bin is at floor first");
    check(std::abs(bin(base, 64) - .4) < 1e-6, "original pitch plays at MIDI 60");
    auto octave = render(d, 127, 4096, 72);
    check(bin(octave, 733) < 1e-6 && std::abs(bin(octave, 128) - .4) < 1e-6,
          "octave doubles measured frequency");
    bool identical = true;
    for (int b = 32; b <= 4096; ++b)
        if (render(d, b) != base) {
            identical = false;
            break;
        }
    check(identical, "every block size 32 through 4096 is sample-identical");
    auto gated = render(d, 64, 4096, 60, 512);
    check(energy(gated, 512) == 0, "Classic note-off follows amp release");
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
    check(energy(render(d, 64)) < energy(base) * .01,
          "24 dB low-pass attenuates 750 Hz above cutoff");
    set(d, OneShot::FilterOn, 0);
    set(d, OneShot::LfoOn, 1);
    set(d, OneShot::LfoVolume, 100);
    set(d, OneShot::LfoRate, 10);
    check(energy(render(d, 64)) < energy(base) * .7, "per-voice LFO changes volume");
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
        w.write(base.data(), static_cast<std::uint32_t>(base.size()));
        w.close();
    }
    std::string fileError;
    OneShot fromFile;
    check(fromFile.loadSample(wave, fileError), "file sample loads");
    configure(fromFile);
    check(render(fromFile, 64) == base, "float WAV sample round trip keeps exact values");
    {
        OneShot envelope;
        configure(envelope);
        std::vector<float> constant(8192, .5f);
        envelope.publishSample(constant, 1, 48000);
        set(envelope, OneShot::Attack, 10);
        set(envelope, OneShot::Decay, 10);
        set(envelope, OneShot::Sustain, 50);
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
        auto step=[](const std::vector<float>& audio){float peak=0;for(std::size_t i=1;i<audio.size();++i)peak=std::max(peak,std::abs(audio[i]-audio[i-1]));return peak;};
        const auto hard=step(render(loop,64));set(loop,OneShot::Fade,50);
        check(step(render(loop,64))<hard*.1f,"loop crossfade smooths wrap without replaying overlap");
        set(loop,OneShot::Start,1);set(loop,OneShot::Snap,1);
        check(std::abs(render(loop,64)[0])<1e-6,"Snap places region start at left-channel zero crossing");
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
        std::vector<float> output(4096);
        std::array<float, 64> l{}, r{};
        float *out[]{l.data(), r.data()};
        for (int b = 0; b < 64; ++b) {
            session.process({out, nullptr, 2, 0, 64, 0});
            std::copy(l.begin(), l.end(), output.begin() + b * 64);
        }
        check(bin(output, 733) < 1e-6, "Session render far bin floor first");
        check(bin(output, 64) > .1, "native sampler makes sound through ADI Session");
        audio::WavWriter w(dir.path() / "oneshot-session.wav", 48000, 1, audio::WavFormat::Float32);
        w.write(output.data(), 4096);
        w.close();
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
