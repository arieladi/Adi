// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/appdata.hpp"
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/sample_preview.hpp"
#include "adi/media/media_ops.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned> allocations{0}, frees{0};
}
#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void *operator new(std::size_t bytes) {
    if (adi::audio::onAudioThread)
        ++allocations;
    if (auto *block = std::malloc(bytes == 0 ? 1 : bytes))
        return block;
    throw std::bad_alloc{};
}
ADI_TEST_NOINLINE void *operator new[](std::size_t bytes) { return operator new(bytes); }
ADI_TEST_NOINLINE void operator delete(void *block) noexcept {
    if (block && adi::audio::onAudioThread)
        ++frees;
    std::free(block);
}
ADI_TEST_NOINLINE void operator delete[](void *block) noexcept { operator delete(block); }
ADI_TEST_NOINLINE void operator delete(void *block, std::size_t) noexcept { operator delete(block); }
ADI_TEST_NOINLINE void operator delete[](void *block, std::size_t) noexcept { operator delete(block); }
#undef ADI_TEST_NOINLINE

namespace {
using namespace adi;
using namespace std::chrono_literals;
using engine::SamplePreview;
using Phase = SamplePreview::Phase;
int checks = 0, failures = 0;
void check(bool okay, const char *message) {
    ++checks;
    if (!okay) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}
template <typename Ready>
bool await(Ready &&ready) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    do {
        if (ready())
            return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}
struct Block {
    std::array<std::array<float, 64>, 2> pcm{};
    std::array<float *, 2> out{pcm[0].data(), pcm[1].data()};
    engine::AudioIo io;
    explicit Block(int frames = 64) {
        io.out = out.data();
        io.frames = frames;
        io.numOut = 2;
    }
    void render(SamplePreview &preview, double rate = 48000., double gain = 1., int firstChannel = 0) {
        pcm[0].fill(0);
        pcm[1].fill(0);
        const audio::CallbackScope scope;
        preview.mix(io, rate, firstChannel, gain);
    }
};
void wav(const std::filesystem::path &path, float first, float second, std::size_t frames) {
    const std::array<float, 2> values{first, second};
    audio::WavWriter writer(path, 48000, 2, audio::WavFormat::Float32);
    for (std::size_t i = 0; i < frames; ++i)
        writer.write(values.data(), 1);
    writer.close();
}
void native(const std::filesystem::path &folder) {
    const auto shortFile = folder / "short.wav", longFile = folder / "long.wav";
    wav(shortFile, .5f, -.75f, 64);
    wav(longFile, .25f, -.125f, 32768);
    SamplePreview preview;
    preview.select(shortFile);
    check(await([&] { return static_cast<bool>(preview.status().waveform); }),
          "a selection requests a worker waveform without playback");
    const auto selected = preview.status();
    check(selected.phase == Phase::Idle && selected.waveform && selected.waveform->path == shortFile &&
              selected.waveform->frames == 64 && selected.waveform->sampleRate == 48000 &&
              selected.waveform->bins.front() == media::PeakBin{-.75f, .5f},
          "selection retains the exact source length and audible extrema");

    preview.play(longFile);
    Block block;
    block.io.numOut = 1;
    block.render(preview);
    check(!SamplePreview::canOutput(block.io, 0) && preview.status().phase == Phase::Loading,
          "an incomplete output pair stays silent without consuming the sample");
    block.io.numOut = 2;
    block.io.exporting = true;
    block.render(preview);
    check(block.pcm[0][0] == 0.f && preview.status().phase == Phase::Loading,
          "an export cannot consume preview audio");
    block.io.exporting = false;
    check(await([&] {
        block.render(preview);
        return block.pcm[0][0] != 0.f;
    }) && block.pcm[0][0] == .25f && block.pcm[1][0] == -.125f,
          "native WAV audition reaches the standalone first pair");
    preview.play(longFile);
    block.io.outputChannels.enabled = 0xc0; // only physical 7/8
    block.render(preview);
    check(!SamplePreview::canOutput(block.io, 0) && preview.status().position == 0. &&
              block.pcm[0][0] == 0.f && block.pcm[1][0] == 0.f,
          "SC-431: preview for missing 1/2 cannot leak into the physical 7/8 buffers");
    check(await([&] {
        block.render(preview, 48000., 1., 6);
        return preview.status().phase == Phase::Playing && block.pcm[0].back() == .25f;
    }) && block.pcm[1].back() == -.125f,
          "SC-431: preview named for physical 7/8 uses the granted packed pair");
    block.io.outputChannels = {};

    preview.stop();
    check(await([&] {
        block.render(preview);
        return !preview.active();
    }), "stop releases the earlier preview before testing an independent seek");
    preview.play(shortFile, .5);
    std::size_t heard = 0;
    bool allNew = true;
    check(await([&] {
        block.render(preview, 96000., .5);
        for (std::size_t i = 0; i < block.pcm[0].size(); ++i) {
            if (block.pcm[0][i] == 0.f && block.pcm[1][i] == 0.f)
                continue;
            ++heard;
            allNew = allNew && block.pcm[0][i] > 0.f && block.pcm[0][i] <= .25f &&
                     std::abs(block.pcm[0][i] * 1.5f + block.pcm[1][i]) < 1e-6f;
        }
        return preview.status().phase == Phase::Finished;
    }) && heard == 64 && allNew && preview.status().position == 1.,
          "a new request discards the older buffered file, seeks and finishes once at the device rate");
    block.render(preview);
    check(block.pcm[0][0] == 0.f, "an ended sample does not loop");

    preview.play(longFile);
    preview.stop();
    block.render(preview);
    check(preview.status().phase == Phase::Idle && block.pcm[0][0] == 0.f,
          "Stop cancels decoding and prevents stale output on the next callback");
    preview.play(folder / "missing.wav");
    check(await([&] { return preview.status().phase == Phase::Failed; }) && !preview.status().error.empty(),
          "unreadable audio reports failure on the owner thread");
}

void transitions(const std::filesystem::path &folder) {
    const auto source = folder / "transition-a.wav", replacement = folder / "transition-b.wav";
    wav(source, .5f, -.75f, 32768);
    wav(replacement, -.25f, .5f, 32768);
    SamplePreview preview;
    Block block;
    const auto render = [&](float destinationLeft, float destinationRight, float initialLeft, float initialRight) {
        float left = initialLeft, right = initialRight;
        float step = 0.f;
        const auto reached = await([&] {
            block.render(preview);
            for (std::size_t i = 0; i < block.pcm[0].size(); ++i) {
                step = std::max({step, std::abs(block.pcm[0][i] - left), std::abs(block.pcm[1][i] - right)});
                left = block.pcm[0][i];
                right = block.pcm[1][i];
            }
            return left == destinationLeft && right == destinationRight;
        });
        return reached && step < .012f;
    };
    preview.play(source);
    check(await([&] {
        block.render(preview);
        return block.pcm[0].back() == .5f && block.pcm[1].back() == -.75f;
    }), "the first preview reaches its source level before a transition");
    preview.play(replacement);
    check(render(-.25f, .5f, .5f, -.75f), "retrigger blends the audible and replacement previews without a step");
    preview.stop();
    check(preview.active() && preview.status().phase == Phase::Idle && render(0.f, 0.f, -.25f, .5f) && !preview.active(),
          "Stop leaves the Cue tail audible until its short fade reaches zero");
    preview.play(source, .5);
    check(render(.5f, -.75f, 0.f, 0.f) && preview.status().position > .5,
          "starting at a non-zero seek fades in from silence while advancing from the chosen source position");
}

void downsampling(const std::filesystem::path &folder) {
    const auto source = folder / "downsample.wav";
    constexpr std::uint32_t sourceRate = 96000, frames = 8192;
    std::array<float, frames * 2> pcm{};
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto t = static_cast<double>(i) * 2. * std::numbers::pi / sourceRate;
        pcm[i * 2] = static_cast<float>(.5 * std::sin(3000. * t));
        pcm[i * 2 + 1] = static_cast<float>(.5 * std::sin(30000. * t));
    }
    audio::WavWriter writer(source, sourceRate, 2, audio::WavFormat::Float32);
    writer.write(pcm.data(), frames);
    writer.close();

    for (const auto deviceRate : {48000., 44100.}) {
        SamplePreview preview;
        Block block;
        int blocks = 0;
        double passband = 0., stopband = 0.;
        preview.play(source);
        const bool rendered = await([&] {
            const auto before = preview.status().position;
            block.render(preview, deviceRate);
            if (preview.status().position <= before)
                return false;
            ++blocks;
            if (blocks > 4) { // Leave the filter's start-up transient out of the steady-state measurement.
                for (std::size_t i = 0; i < block.pcm[0].size(); ++i) {
                    passband += block.pcm[0][i] * static_cast<double>(block.pcm[0][i]);
                    stopband += block.pcm[1][i] * static_cast<double>(block.pcm[1][i]);
                }
            }
            return blocks == 32;
        });
        const auto count = 28. * static_cast<double>(block.pcm[0].size());
        check(rendered && std::sqrt(passband / count) > .33 && std::sqrt(passband / count) < .37 &&
                  std::sqrt(stopband / count) < .005,
              "downsampling keeps the 3 kHz tone and suppresses the alias of the 30 kHz tone");
    }
}

void suspended(const std::filesystem::path &folder) {
    const auto source = folder / "while-suspended.wav", replacement = folder / "after-suspend.wav";
    wav(source, .25f, -.125f, 262144); // More than the playback queue can hold without a callback.
    wav(replacement, -.5f, .75f, 8192);
    SamplePreview preview;
    preview.play(source);
    check(await([&] { return static_cast<bool>(preview.status().waveform); }) && preview.status().phase == Phase::Loading,
          "waveform finishes while playback waits for output space");
    preview.stop();
    preview.play(replacement);
    Block block;
    check(await([&] {
        block.render(preview);
        return block.pcm[0].front() != 0.f;
    }) && block.pcm[0].front() == -.5f && block.pcm[1].front() == .75f,
          "Stop and a new preview release the suspended request and play only replacement PCM");
}

void compressed() {
    const auto source = media::pathFromUtf8(ADI_PREVIEW_DECODE_FIXTURES) / "tone16.flac";
    audio::WavReader reference(media::pathFromUtf8(ADI_PREVIEW_DECODE_FIXTURES) / "src16.wav");
    const auto frames = static_cast<std::uint32_t>(reference.frames());
    std::vector<float> expected(static_cast<std::size_t>(frames) * 2);
    check(reference.channels() == 2 && reference.read(expected.data(), frames) == frames,
          "the FLAC test has independent source PCM");
    SamplePreview preview;
    preview.play(source);
    Block block(1);
    std::size_t heard = 0;
    bool exact = true;
    check(await([&] {
        // Frequent callbacks are deterministic about position even if some
        // start before the decoder publishes its first PCM chunk.
        for (int i = 0; i < 128 && heard < frames; ++i) {
            const auto before = preview.status().position;
            block.render(preview, static_cast<double>(reference.sampleRate()));
            const auto after = preview.status().position;
            if (after <= before)
                break;
            exact = exact && block.pcm[0][0] == expected[heard * 2] && block.pcm[1][0] == expected[heard * 2 + 1];
            ++heard;
        }
        return heard == frames;
    }) && exact, "compressed FLAC streams both channels, sample for sample and with no preliminary complete decode");
    check(await([&] {
        block.render(preview, static_cast<double>(reference.sampleRate()));
        return preview.status().phase == Phase::Finished;
    }), "compressed EOF ends the one-shot");
    if (audio::aacCodecAvailable()) {
        preview.play(media::pathFromUtf8(ADI_PREVIEW_AAC_FIXTURES) / "mono.m4a");
        check(await([&] {
            block.render(preview, 44100.);
            return block.pcm[0][0] != 0.f;
        }) && block.pcm[0][0] == block.pcm[1][0], "native AAC PCM feeds both output channels for a mono file");
    }
    const auto cache = appdata::pathsFor(appdata::App::Daw).cache;
    check(!std::filesystem::exists(cache / "decoded"), "compressed sample preview creates no decoded cache");
}
} // namespace

int main() {
    try {
        const test::TempDirectory temp{"sample_preview", "stream"};
        const auto &folder = temp.path();
#if defined(_WIN32)
        if (_wputenv_s(L"ADI_HOME", folder.c_str()) != 0)
#else
        if (setenv("ADI_HOME", folder.c_str(), 1) != 0)
#endif
            throw std::runtime_error("set isolated preview app data");
        native(folder);
        transitions(folder);
        downsampling(folder);
        suspended(folder);
        compressed();
        check(allocations == 0 && frees == 0 && audio::callbackFileIo == 0,
              "preview callback allocates, frees and reads files zero times");
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
