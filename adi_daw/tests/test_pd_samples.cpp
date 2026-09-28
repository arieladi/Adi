// SPDX-License-Identifier: GPL-3.0-or-later
//
// `[adi.sample]` and its handoff -- ADR-0192 d4.
//
// NO libpd HERE, deliberately, and it is the same property ADR-0177 fix 3 put
// on the declaration parse: the slot is `SnapshotPublisher` and the decode is
// the audio layer, so neither needs Pd running and both are tested on every
// ABI -- including the ones where the Pd runtime is not built at all.

#include "juce/pd_samples.hpp"
#include "juce/pd_declarations.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace adi::device;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void eqi(long long got, long long want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %lld, want %lld\n", what.c_str(), got, want);
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

std::unique_ptr<PdSampleBuffer> rawBuffer(std::int32_t channels, std::int64_t frames,
                                          float first) {
    auto b = std::make_unique<PdSampleBuffer>();
    b->channels = channels;
    b->frames = frames;
    b->sampleRate = 48000.0;
    b->interleaved.assign(static_cast<std::size_t>(frames * channels), 0.f);
    for (std::size_t i = 0; i < b->interleaved.size(); ++i)
        b->interleaved[i] = first + static_cast<float>(i);
    return b;
}

// ---------------------------------------------------------------------------

void testTheDeclaration() {
    section("ADR-0192 d4 -- [adi.sample $0 <id> <name>], parsed by the same scanner");

    const auto d = parsePdDeclarations(
        "#X obj 20 20 adi.sample $0 1 Kick;\n"
        "#X obj 20 60 adi.param $0 1 0 1 0.5 - lin Depth;\n"
        "#X obj 20 100 adi.array $0 1 8 30 -1 1 - Scope;\n");
    eqi(static_cast<long long>(d.samples.size()), 1, "the sample is read");
    eqi(static_cast<long long>(d.problems.size()), 0, "with no problem reported");
    if (!d.samples.empty()) {
        eqi(d.samples[0].id, 1, "its id");
        check(d.samples[0].name == "Kick", "and its name");
    }

    // THREE SEPARATE ID SPACES. All three declarations above are id 1 and none
    // collides: a sample is media, a parameter is automation, an array is a
    // display, and one shared space would make adding a sample change what an
    // automation lane points at.
    eqi(static_cast<long long>(d.params.size()), 1, "the parameter with the same id is kept");
    eqi(static_cast<long long>(d.arrays.size()), 1, "and so is the array");
    check(d.sample(1) != nullptr && d.param(1) != nullptr && d.array(1) != nullptr,
          "and all three answer to id 1, in their own spaces");

    const auto dup = parsePdDeclarations(
        "#X obj 20 20 adi.sample $0 1 Kick;\n"
        "#X obj 20 60 adi.sample $0 1 Snare;\n");
    eqi(static_cast<long long>(dup.samples.size()), 1, "a duplicate id is not taken twice");
    eqi(static_cast<long long>(dup.problems.size()), 1, "and is reported");
    if (!dup.samples.empty())
        check(dup.samples[0].name == "Kick",
              "the FIRST in file order wins, as ADR-0177 d2 has it for parameters");

    const auto arity = parsePdDeclarations("#X obj 20 20 adi.sample $0 1 Kick 44100;");
    eqi(static_cast<long long>(arity.samples.size()), 0, "a fourth argument is refused");
    eqi(static_cast<long long>(arity.problems.size()), 1, "and reported");

    const auto noDollar = parsePdDeclarations("#X obj 20 20 adi.sample 0 1 Kick;");
    eqi(static_cast<long long>(noDollar.samples.size()), 0, "so is a missing $0");

    // `Kick_Drum` reads "Kick Drum", the one rule every declaration shares.
    const auto label = parsePdDeclarations("#X obj 20 20 adi.sample $0 2 Kick_Drum;");
    if (!label.samples.empty())
        check(label.samples[0].name == "Kick Drum", "an underscore reads as a space");
}

void testARawBufferReachesTheSlot() {
    section("ADR-0192 d4 -- a raw buffer, handed over and read back");

    PdSampleSlots slots;
    const auto d = parsePdDeclarations("#X obj 20 20 adi.sample $0 1 Kick;\n");
    slots.declare(d.samples);
    eqi(static_cast<long long>(slots.count()), 1, "one slot is declared");
    check(slots.has(1), "and answers to its id");
    check(!slots.has(2), "and to no other");

    check(slots.forBlock(1) == nullptr, "an empty slot reads as nothing, not as silence");
    check(!slots.publish(2, rawBuffer(2, 4, 1.f)), "an undeclared id is refused");

    auto b = rawBuffer(2, 4, 1.f);
    b->blake3 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    check(slots.publish(1, std::move(b)), "a raw buffer is published");

    const PdSampleBuffer* got = slots.forBlock(1);
    check(got != nullptr, "and the audio thread sees it");
    if (got == nullptr) return;
    eqi(got->channels, 2, "its channels");
    eqi(static_cast<long long>(got->frames), 4, "its frames");
    check(std::fabs(got->sampleRate - 48000.0) < 1e-9, "its rate");
    check(got->blake3.size() == 64,
          "and the MEDIA's BLAKE3 hash came with it -- which is what lets a "
          "project find the file again after it moves (ADR-0127)");

    bool sequential = true;
    for (std::size_t i = 0; i < got->interleaved.size(); ++i)
        if (std::fabs(got->interleaved[i] - (1.f + static_cast<float>(i))) > 1e-6f)
            sequential = false;
    check(sequential, "and every sample is the one that was written");

    const float* f2 = got->frame(2);
    check(f2 != nullptr && std::fabs(f2[0] - 5.f) < 1e-6f,
          "frame(n) indexes by frame and not by sample, which is the one thing "
          "an interleaved buffer gets wrong");
    check(got->frame(4) == nullptr, "and refuses one past the end");
}

void testARetiredBufferIsNotFreedUnderTheReader() {
    section("ADR-0192 d4 -- the strictly-greater rule, which is the whole safety argument");

    // publisher.hpp spends a page on why `collect` frees only what is STRICTLY
    // older than the announced sequence. This is that page, exercised: a
    // buffer the audio thread is ON must survive a publish and a collect.
    PdSampleSlots slots;
    const auto d = parsePdDeclarations("#X obj 20 20 adi.sample $0 1 Kick;\n");
    slots.declare(d.samples);

    check(slots.publish(1, rawBuffer(1, 2, 10.f)), "buffer A is published");
    const PdSampleBuffer* a = slots.forBlock(1);          // announces A
    check(a != nullptr && std::fabs(a->interleaved[0] - 10.f) < 1e-6f, "the reader is on A");

    check(slots.publish(1, rawBuffer(1, 2, 20.f)), "buffer B is published");
    eqi(static_cast<long long>(slots.retained()), 1, "A is retired, not freed");
    eqi(static_cast<long long>(slots.collect()), 0,
        "and a collect does NOT free it -- the reader announced A and has not "
        "moved on, so `inUse > A.seq` is false");
    eqi(static_cast<long long>(slots.retained()), 1, "A is still held");

    // Reading A again here is what the real audio thread does: it has the
    // pointer for the whole block. It must still be the buffer it was.
    check(std::fabs(a->interleaved[0] - 10.f) < 1e-6f,
          "and A still reads as A, which is the property that would have been a "
          "use-after-free");

    const PdSampleBuffer* b = slots.forBlock(1);          // announces B
    check(b != nullptr && std::fabs(b->interleaved[0] - 20.f) < 1e-6f,
          "the next block picks up B");
    eqi(static_cast<long long>(slots.collect()), 1, "and NOW A is freed");
    eqi(static_cast<long long>(slots.retained()), 0, "nothing is retained");
}

void testRedeclaringReplacesTheSet() {
    section("ADR-0192 d4 -- re-declaring is what device.loadState does, and it replaces");

    PdSampleSlots slots;
    slots.declare(parsePdDeclarations(
        "#X obj 20 20 adi.sample $0 1 Kick;\n"
        "#X obj 20 60 adi.sample $0 2 Snare;\n").samples);
    eqi(static_cast<long long>(slots.count()), 2, "two slots");
    check(slots.publish(1, rawBuffer(1, 2, 1.f)), "one is filled");

    // A patch that no longer declares slot 2 should not keep it. The declared
    // set changes only through an op (ADR-0177 fix 3), and that op replaces the
    // patch -- so it replaces the slots with it.
    slots.declare(parsePdDeclarations("#X obj 20 20 adi.sample $0 1 Kick;\n").samples);
    eqi(static_cast<long long>(slots.count()), 1, "a slot the new patch drops is gone");
    check(slots.has(1), "the one it keeps is there");
    check(!slots.has(2), "the one it dropped is not");
    check(slots.forBlock(1) == nullptr,
          "and the kept slot is empty again -- its buffer went with the old "
          "declaration, because the host reloads what the new patch asks for");
}

void testDecodingAFile() {
    section("ADR-0192 d4 -- a file decoded off the audio thread, hashed as MEDIA");

    const auto dir = std::filesystem::temp_directory_path() / "adi-pd-sample-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto wav = dir / "probe.wav";

    {
        adi::audio::WavWriter w(wav, 48000, 2, adi::audio::WavFormat::Float32);
        std::vector<float> frames(2 * 8, 0.f);
        for (std::size_t i = 0; i < frames.size(); ++i)
            frames[i] = static_cast<float>(i) / 100.f;
        w.write(frames.data(), 8);
        w.close();
    }

    // A DIFFERENT FILE stands in for the media the project names, so that
    // "the hash is the media's" is a claim this test can actually fail. With
    // one path for both, a `blake3File(playableWav)` would pass and ADR-0127's
    // whole point -- that the hash identifies the file a project refers to,
    // not the cache's disposable copy -- would go untested.
    const auto source = dir / "source.flac";
    {
        std::ofstream out(source, std::ios::binary);
        out << "not really a flac, and it does not need to be: what matters is "
               "that its bytes differ from the decoded copy's";
    }
    const auto mediaHash = adi::media::blake3File(source);
    check(static_cast<bool>(mediaHash), "the stand-in media hashes");

    std::string err;
    auto buffer = pdDecodeSample(7, source, wav, err);
    check(buffer != nullptr, "the file decodes: " + err);
    if (buffer == nullptr) { std::filesystem::remove_all(dir, ec); return; }
    eqi(buffer->id, 7, "into the slot it was asked for");
    eqi(buffer->channels, 2, "with the file's channels");
    eqi(static_cast<long long>(buffer->frames), 8, "and its frames");
    check(std::fabs(buffer->sampleRate - 48000.0) < 1e-9, "and its rate");
    check(std::fabs(buffer->interleaved[3] - 0.03f) < 1e-5f,
          "and its samples, in order");
    check(buffer->blake3 == mediaHash.hex,
          "and the hash is the SOURCE's, not the decoded copy's -- which is "
          "what makes it the media a project can find again (ADR-0127)");
    check(buffer->blake3 != adi::media::blake3File(wav).hex,
          "and demonstrably not the playable file's, since the two differ");

    std::string missingErr;
    check(pdDecodeSample(7, source, dir / "nope.wav", missingErr) == nullptr,
          "a file that is not there fails");
    check(!missingErr.empty(), "and says so rather than returning an empty buffer");

    std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_pd_sample_tests -- [adi.sample] and its handoff\n\n");
    testTheDeclaration();
    testARawBufferReachesTheSlot();
    testARetiredBufferIsNotFreedUnderTheReader();
    testRedeclaringReplacesTheSet();
    testDecodingAFile();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
