// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/launch_audio.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>

#ifdef ADI_TEST_LAUNCH_WRAP_LOCKS
#include <pthread.h>
#endif

namespace {
int allocations = 0;
#ifdef ADI_TEST_LAUNCH_WRAP_LOCKS
int locks = 0;
#endif
}

#ifdef ADI_TEST_LAUNCH_WRAP_LOCKS
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_trylock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (adi::audio::onAudioThread)
        ++locks;
    return __real_pthread_mutex_lock(mutex);
}
extern "C" int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (adi::audio::onAudioThread)
        ++locks;
    return __real_pthread_mutex_trylock(mutex);
}
#endif

void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread)
        ++allocations;
}

namespace {
using namespace adi;
using namespace adi::engine;

constexpr std::uint32_t rate = 48000;
constexpr std::int64_t quarter = textproj::kPPQ;
constexpr std::int64_t beatFrames = rate / 2;
constexpr std::int64_t barFrames = 4 * beatFrames;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::int64_t tickAtSample(const void*, std::int64_t sample) noexcept {
    return sample * quarter / beatFrames;
}

std::int64_t sampleAtTick(const void*, std::int64_t tick) noexcept {
    return (tick * beatFrames + quarter / 2) / quarter;
}

void writeSignal(const std::filesystem::path& path, std::uint32_t frames, float value, float step = 0.0f) {
    std::vector<float> pcm(frames, value);
    for (std::uint32_t frame = 0; frame < frames; ++frame)
        pcm[frame] += static_cast<float>(frame % 256) * step;
    audio::WavWriter writer(path, rate, 1, audio::WavFormat::Float32);
    writer.write(pcm.data(), frames);
    writer.close();
}

struct Render {
    AudioIo io;
    const LaunchAudioClip& first;
    const LaunchAudioClip& second;
    std::array<std::int64_t, 2> edges{};
    std::size_t edgeCount = 0;

    static void renderSection(void* context, const LaunchSection& section) noexcept {
        auto& self = *static_cast<Render*>(context);
        if (section.arrangement)
            return;
        const auto* clip = section.clip == self.first.clip() ? &self.first
                         : section.clip == self.second.clip() ? &self.second : nullptr;
        const auto frame = clip
            ? clip->frameForTick(section.sourceAnchorTick, 120) + section.framesSinceSourceAnchor : 0;
        renderLaunchAudio(self.io, section.offset, section.frames, clip, frame, section.velocity);
    }
    static void noteEdge(void* context, const LaunchTransition& edge) noexcept {
        auto& self = *static_cast<Render*>(context);
        if (edge.newClip && self.edgeCount < self.edges.size())
            self.edges[self.edgeCount++] = edge.sample;
    }
};

std::vector<float> renderScenes(const LaunchAudioClip& first, const LaunchAudioClip& second,
                                std::int32_t blockSize, bool enableLaunch = true) {
    constexpr std::array<std::int64_t, 1> tracks{1};
    constexpr std::array<std::int64_t, 2> scenes{10, 11};
    std::array<LaunchSlot, 2> slots{};
    slots[0].track = slots[1].track = tracks[0];
    slots[0].scene = scenes[0];
    slots[1].scene = scenes[1];
    slots[0].clip = first.clip();
    slots[1].clip = second.clip();
    slots[0].endTick = slots[1].endTick = 4 * quarter;
    LaunchView view{tracks, slots, scenes};
    Launcher launcher(tracks.size());
    Render render{{}, first, second};
    const LaunchSink sink{&render, Render::renderSection, Render::noteEdge};
    std::vector<float> output(static_cast<std::size_t>(barFrames + 1000), 0.125f);
    float* channel = nullptr;
    render.io.numOut = 1;
    render.io.out = &channel;
    bool secondPosted = false;
    auto postScene = [&](std::int64_t scene) {
        LaunchRequest request;
        request.kind = LaunchRequestKind::ScenePress;
        request.scene = scene;
        request.incarnation = 1;
        require(launcher.post(std::span(&request, 1)), "post a whole scene gesture");
    };
    if (enableLaunch)
        postScene(scenes[0]);
    const auto end = static_cast<std::int64_t>(output.size());
    for (std::int64_t sample = 0; sample < end; sample += render.io.frames) {
        if (enableLaunch && !secondPosted && sample >= barFrames / 2) {
            postScene(scenes[1]);
            secondPosted = true;
        }
        channel = output.data() + sample;
        render.io.frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockSize, end - sample));
        LaunchBlock block;
        block.sample = sample;
        block.frames = render.io.frames;
        block.incarnation = 1;
        block.global = LaunchQuantization::Bar;
        block.tickAtSample = tickAtSample;
        block.sampleAtTick = sampleAtTick;
        bool rendered = false;
        {
            audio::CallbackScope callback;
            rendered = launcher.process(block, view, sink);
        }
        require(rendered, "launch scheduler accepted the render block");
    }
    if (enableLaunch)
        require(render.edgeCount == 2 && render.edges[0] == 0 && render.edges[1] == barFrames,
                "both scene edges occur exactly at their bar samples");
    else
        require(render.edgeCount == 0, "Arrangement-only control has no launch edges");
    return output;
}

void verifyFade(const LaunchAudioClip& clip) {
    std::array<float, 193> narrow{};
    std::array<double, 193> wide{};
    float* out = narrow.data();
    double* out64 = wide.data();
    AudioIo io;
    io.out = &out;
    io.out64 = &out64;
    io.numOut = 1;
    io.frames = static_cast<std::int32_t>(narrow.size());
    {
        audio::CallbackScope callback;
        renderLaunchAudio(io, 0, 73, &clip, 0, 1.0f, 0);
        renderLaunchAudio(io, 73, 120, &clip, 73, 1.0f, 73);
    }
    constexpr std::array gains{0.0, 2.0 / 27, 7.0 / 27, 0.5, 20.0 / 27, 25.0 / 27, 1.0};
    for (std::size_t i = 0; i < gains.size(); ++i) {
        const auto target = gains[i] * clip.sample(static_cast<std::int64_t>(i * 32), 0);
        require(std::abs(wide[i * 32] - target) < 1e-12 &&
                std::abs(narrow[i * 32] - target) < 1e-7,
                "launch-in Fade matches the 48 kHz reference knots across a split");
    }
}

void verifyOutgoingFade(const LaunchAudioClip& clip) {
    std::array<float, 200> narrow;
    std::array<double, 200> wide;
    narrow.fill(0.125f);
    wide.fill(0.125);
    float* out = narrow.data();
    double* out64 = wide.data();
    AudioIo io;
    io.out = &out;
    io.out64 = &out64;
    io.numOut = 1;
    io.frames = static_cast<std::int32_t>(narrow.size());
    constexpr std::int64_t startFrame = 743;
    {
        audio::CallbackScope callback;
        mixLaunchAudioTail(io, 3, 83, clip, startFrame, 0.5f, 0);
        mixLaunchAudioTail(io, 86, 114, clip, startFrame + 83, 0.5f, 83);
    }
    constexpr std::array gains{1.0, 25.0 / 27, 20.0 / 27, 0.5, 7.0 / 27, 2.0 / 27, 0.0};
    for (std::size_t i = 0; i < gains.size(); ++i) {
        const auto position = i * 32;
        const auto target = 0.125 + 0.5 * gains[i] *
            clip.sample(startFrame + static_cast<std::int64_t>(position), 0);
        require(std::abs(wide[3 + position] - target) < 1e-12 &&
                std::abs(narrow[3 + position] - target) < 1e-7,
                "the outgoing fade continues source and curve across callback splits");
    }
    require(narrow[0] == 0.125f && narrow[199] == 0.125f,
            "an outgoing fade leaves audio beyond its exact ramp untouched");
    require(launchFadeFrames(44100) == 176 && launchFadeFrames(48000) == 192 &&
            launchFadeFrames(96000) == 384 && launchFadeFrames(192000) == 768,
            "incoming and outgoing fade lengths use the selected sample rate");
    const auto last = static_cast<std::int64_t>(clip.frames() - 1);
    {
        audio::CallbackScope callback;
        mixLaunchAudioTail(io, 0, 1, clip, last + 1, 0.5f, 0, 96);
    }
    require(wide[0] == 0.125 + 0.25 * clip.sample(last, 0),
            "an early stop continues at the current fade-in level with bounded source access");
}
} // namespace

int main() {
    try {
        adi::test::TempDirectory temporary{"launch_audio", "scenes"};
        const auto firstWav = temporary.path() / "first.wav";
        const auto secondWav = temporary.path() / "second.wav";
        const auto renderWav = temporary.path() / "scenes.wav";
        writeSignal(firstWav, static_cast<std::uint32_t>(barFrames), 0.25f, 1.0f / 4096);
        writeSignal(secondWav, static_cast<std::uint32_t>(barFrames / 2), -0.5f);
        constexpr std::uint64_t budget = 2 * 1024 * 1024;
        const auto first = LaunchAudioClip::read(100, 0, firstWav, rate, budget);
        verifyFade(first);
        verifyOutgoingFade(first);
        audio::WarpRenderPlan warp;
        warp.source = secondWav;
        warp.workFolder = temporary.path();
        warp.sourceFrames = static_cast<std::uint64_t>(barFrames / 2);
        warp.outputRate = rate;
        warp.anchors = {{0, 0}, {warp.sourceFrames, static_cast<std::uint64_t>(barFrames)}};
        warp.mode = audio::WarpMode::RePitch;
        const auto start = std::chrono::steady_clock::now();
        const auto second = LaunchAudioClip::stretch(101, 0, warp, budget);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        require(second.frames() == static_cast<std::uint64_t>(barFrames), "constant-tempo Warp produces one bar");
        const auto baseline = renderScenes(first, second, 511, false);
        require(std::all_of(baseline.begin(), baseline.end(), [](float value) { return value == 0.125f; }),
                "without launch the Arrangement buffer plays bit identically");
        const auto output = renderScenes(first, second, 511);
        for (const auto block : {32, 64, 128, 512, 1024, 4096})
            require(renderScenes(first, second, block) == output, "launch audio is block-size invariant");
        {
            audio::WavWriter writer(renderWav, rate, 1, audio::WavFormat::Float32);
            writer.write(output.data(), static_cast<std::uint32_t>(output.size()));
            writer.close();
        }
        audio::WavReader reader(renderWav);
        std::vector<float> reread(output.size());
        require(reader.read(reread.data(), static_cast<std::uint32_t>(reread.size())) == reread.size()
                && reread == output, "verify the actual WAV samples");
        const auto split = reread.begin() + barFrames;
        require(reread[static_cast<std::size_t>(barFrames - 1)] == first.sample(barFrames - 1, 0)
                && *split == second.sample(0, 0) && *split < 0,
                "scene one ends at frame 95999 and warped scene two begins at frame 96000");
        for (auto frame = std::int64_t{0}; frame < barFrames; ++frame)
            require(reread[static_cast<std::size_t>(frame)] == first.sample(frame, 0),
                    "scene one reads consecutive source frames across blocks");
        for (auto frame = std::int64_t{0}; frame < 1000; ++frame)
            require(*(split + frame) == second.sample(frame, 0), "warped source advances from its exact first sample");
        require(allocations == 0 && audio::callbackFileIo.load() == 0,
                "zero counted allocations and WAV I/O on the driver");
#ifdef ADI_TEST_LAUNCH_WRAP_LOCKS
        require(locks == 0, "zero counted pthread mutex calls on the driver");
#endif
        const auto ms = std::chrono::duration<double, std::milli>(elapsed).count();
        std::printf("PASS -- scenes switch at sample %lld (0 sample error), 7 block sizes, 0 callback allocations or I/O, Warp prepared in %.2f ms\n",
                    static_cast<long long>(barFrames), ms);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
