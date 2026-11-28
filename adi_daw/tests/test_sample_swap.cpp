// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/library/index.hpp"
#include "adi/library/sample_query.hpp"
#include "adi/library/sample_swap.hpp"
#include "adi/ui/project_document.hpp"
#include "juce/builtin_host.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace {
using namespace adi;
using namespace adi::library;
namespace fs = std::filesystem;

int failures = 0;
void check(bool pass, const char *description) {
    if (!pass) {
        ++failures;
        std::printf("FAIL %s\n", description);
        std::fflush(stdout);
    }
}

// Flushed stage lines: a ctest timeout keeps them, so a hang names its step.
// The watchdog bounds every wait in this fixture, including unwinding and
// process exit: a stage that does not advance within kStageLimit ends the
// process with the stage named, instead of running into ctest's timeout.
constexpr auto kStageLimit = std::chrono::seconds(90);
std::atomic<const char *> currentStage{"start"};
std::atomic<std::int64_t> stageStarted{0};

std::int64_t nowTicks() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

void stage(const char *name) {
    currentStage.store(name);
    stageStarted.store(nowTicks());
    std::printf("stage: %s\n", name);
    std::fflush(stdout);
}

void startWatchdog() {
    stageStarted.store(nowTicks());
    std::thread([] {
        const auto limit = std::chrono::duration_cast<std::chrono::steady_clock::duration>(kStageLimit).count();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (nowTicks() - stageStarted.load() > limit) {
                std::printf("FAIL stage '%s' did not finish within %lld s\n", currentStage.load(),
                            static_cast<long long>(kStageLimit.count()));
                std::fflush(stdout);
                std::_Exit(3);
            }
        }
    }).detach();
}

#if ADI_WITH_PD
constexpr int kRate = 48000;
void wave(const fs::path &path, double amplitude) {
    std::array<float, 384> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 *
                                                             static_cast<double>(i) / kRate));
    audio::WavWriter writer(path, kRate, 1, audio::WavFormat::Float32);
    writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    writer.close();
}

void swapInPlayingProject() {
    test::TempDirectory temp{"sample_swap", "project"};
    const auto root = temp.path() / "samples";
    fs::create_directory(root);
    constexpr std::array names{"reference.wav", "similar.wav", "other.wav", "new.wav"};
    wave(root / names[0], 0.15);
    wave(root / names[1], 0.45);
    wave(root / names[2], 0.65);

    std::string error;
    auto index = Index::open(temp.path() / "library.sqlite", error);
    Volume volume;
    MarkerVolumeProvider provider;
    if (!index || !provider.identify(root, volume, error))
        throw std::runtime_error(error);

    std::array<SampleAnalysis, names.size()> analysis{};
    constexpr std::array energy{1.f, 0.95f, 0.7f, 0.99f};
    for (std::size_t i = 0; i < analysis.size(); ++i) {
        analysis[i].lengthSeconds = 0.008;
        analysis[i].descriptors.spectralEnergy[0] = energy[i];
        analysis[i].descriptors.spectralEnergy[1] = 1.f - energy[i];
    }
    const auto rescanAndRank = [&] {
        stage("library scan");
        if (!index->scan(volume).ok)
            throw std::runtime_error("sample index scan failed");
        stage("library hashing");
        index->waitIdle();
        stage("library entries");
        const auto entries = index->entries();
        stage("library ranking");
        std::vector<SimilarSampleCandidate> candidates;
        std::vector<const SampleAnalysis *> measurements;
        for (const auto &entry : entries) {
            const auto name = std::ranges::find(names, entry.relativePath);
            if (entry.present && name != names.end()) {
                candidates.push_back({root / *name, entry.hash});
                measurements.push_back(&analysis[static_cast<std::size_t>(name - names.begin())]);
            }
        }
        SampleQuery query;
        query.rank = SampleQueryRank::Similar;
        query.reference = &analysis[0];
        std::vector<SampleQueryMatch> matches;
        querySamples(measurements, query, matches);
        std::vector<SimilarSampleCandidate> ranked;
        for (const auto &match : matches)
            ranked.push_back(candidates[match.row]);
        std::printf("ranked %zu of %zu indexed samples\n", ranked.size(), entries.size());
        stage("library ranked");
        return ranked;
    };
    const auto initialRank = rescanAndRank();
    if (initialRank.size() != 2 || initialRank.front().file.filename() != names[1]) {
        stage("unwinding after a ranking mismatch");
        throw std::runtime_error("fixture did not rank the two similar files");
    }

    stage("open the playing project");
    auto document = ui::ProjectDocument::open(temp.path() / "swap.adi", true, device::withBuiltinDevices(), error);
    if (!document)
        throw std::runtime_error(error);
    document->session().graph().setFadeFrames(0);
    document->prepare(kRate, 64);
    constexpr int kTrack = 2;
    std::vector<OpRequest> notes(2);
    notes[0].opType = "clip.create";
    notes[0].payload = {{"id", 1}, {"track", kTrack}, {"kind", "midi"}, {"pos", 0}, {"length", textproj::kWhole}};
    notes[1].opType = "note.insert";
    notes[1].payload = {{"clip", 1}, {"note", 1}, {"start", 0}, {"dur", textproj::kPPQ}, {"key", 60}, {"vel", 127}};
    if (!document->ops().submit(std::move(notes)).ok || !document->synchronise() ||
        !document->loadSampleInstrument(kTrack, root / names[0], 1))
        throw std::runtime_error("could not build playing OneShot: " + document->error());

    const auto devices = document->parameterFeed().current();
    if (!devices)
        throw std::runtime_error("the project did not publish any device identities");
    const auto device = std::ranges::find_if(*devices, [](const auto &entry) {
        return entry.uid == "org.adidaw.builtin.oneshot";
    });
    if (device == devices->end())
        throw std::runtime_error("OneShot did not publish its runtime identity");
    const auto currentHash = [&] {
        const auto view = document->view().committed();
        for (const auto &slot : view->sampleSlots)
            if (slot && slot->deviceId == device->id && slot->slot == 1)
                return slot->mediaHash;
        return std::string{};
    };
    auto *const runtime = document->session().instanceFor(device->id);
    const auto referenceHash = currentHash();
    const auto referenceHead = OpJournal(document->store()).headSeq();
    if (!runtime || referenceHash.empty())
        throw std::runtime_error("reference sample was not assigned");

    std::int64_t stream = 0;
    const auto render = [&] {
        std::array<float, 64> left{}, right{};
        float *outputs[]{left.data(), right.data()};
        engine::AudioIo io;
        io.out = outputs;
        io.numOut = 2;
        io.frames = 64;
        document->mailbox().post(ui::TransportMailbox::Command::Locate, 0);
        float peak = 0.f;
        for (int block = 0; block < 16; ++block) {
            io.streamTimeSamples = stream;
            audio::CallbackScope callback;
            document->process(io);
            stream += io.frames;
            for (const auto value : left)
                peak = std::max(peak, std::abs(value));
        }
        return peak;
    };
    stage("play and swap");
    document->mailbox().post(ui::TransportMailbox::Command::Play);
    const auto originalPeak = render();
    check(document->mailbox().playing() && originalPeak > 0.01f, "reference sample plays through the live device");

    check(swapSimilarSample(*document, *device, 1, initialRank, SimilarSampleStep::Next, 2) == SimilarSampleSwap::Applied &&
              currentHash() == initialRank[0].contentHash && document->session().instanceFor(device->id) == runtime,
          "next loads the best ranked sound into the same live instance");
    const auto swappedPeak = render();
    check(document->mailbox().playing() && swappedPeak > 2.5f * originalPeak,
          "new sample is audible while the transport keeps playing");
    check(document->ops().undo().ok && document->synchronise() && OpJournal(document->store()).headSeq() == referenceHead &&
              currentHash() == referenceHash, "one Undo restores the original project content and head");
    document->tick(1); // deliver the history batch to the retained device
    const auto restoredPeak = render();
    check(std::abs(restoredPeak - originalPeak) < 0.0001f, "the original audible level returns after Undo");
    check(document->ops().redo().ok && document->synchronise() && currentHash() == initialRank[0].contentHash,
          "one Redo recalls the selected sample");

    wave(root / names[3], 0.8);
    const auto rescanned = rescanAndRank();
    if (rescanned.size() != 3 || rescanned.front().file.filename() != names[3])
        throw std::runtime_error("fixture did not reorder the alternatives during rescan");
    check(currentHash() == initialRank[0].contentHash, "a new library publication preserves the assigned sample");
    check(swapSimilarSample(*document, *device, 1, rescanned, SimilarSampleStep::Next, 3) == SimilarSampleSwap::Applied &&
              currentHash() == initialRank[1].contentHash,
          "after rescan next advances from the loaded content in the refreshed ranking");
    check(swapSimilarSample(*document, *device, 1, rescanned, SimilarSampleStep::Previous, 4) == SimilarSampleSwap::Applied &&
              currentHash() == initialRank[0].contentHash,
          "previous moves back toward the reference");

    wave(root / names[3], 0.75);
    const auto beforeStale = OpJournal(document->store()).headSeq();
    check(swapSimilarSample(*document, *device, 1, rescanned, SimilarSampleStep::Previous, 5) == SimilarSampleSwap::StaleCandidate &&
              OpJournal(document->store()).headSeq() == beforeStale && currentHash() == initialRank[0].contentHash,
          "changed bytes at a ranked path leave the current sample and history unchanged");
    check(swapSimilarSample(*document, *device, 1, std::span(initialRank).first(1), SimilarSampleStep::Next, 6) ==
              SimilarSampleSwap::NoCandidate && OpJournal(document->store()).headSeq() == beforeStale,
          "an alias of the current content never creates a redundant undo step");
    {
        audio::CallbackScope callback;
        check(swapSimilarSample(*document, *device, 1, rescanned, SimilarSampleStep::Next, 7) == SimilarSampleSwap::WrongThread,
              "swap refuses immediately on the audio callback");
    }
    std::printf("Playing OneShot peak %.6f -> %.6f, Undo %.6f\n", static_cast<double>(originalPeak),
                static_cast<double>(swappedPeak), static_cast<double>(restoredPeak));
    stage("release");
    document->release();
    stage("teardown");
}
#endif

void callbackRefusal() {
    test::TempDirectory temp{"sample_swap", "callback"};
    std::string error;
    auto document = ui::ProjectDocument::open(temp.path() / "swap.adi", true, {}, error);
    check(document != nullptr, "open the project for callback refusal");
    if (document) {
        audio::CallbackScope callback;
        check(swapSimilarSample(*document, {}, 1, {}, SimilarSampleStep::Next, 0) == SimilarSampleSwap::WrongThread,
              "the callback refuses before checking even an empty target and list");
    }
}

} // namespace

int main() {
    startWatchdog();
    try {
        stage("callback refusal");
        callbackRefusal();
#if ADI_WITH_PD
        swapInPlayingProject();
#else
        std::printf("OneShot playback fixture requires Pd.\n");
#endif
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    stage("process exit");
    return failures == 0 ? 0 : 1;
}
