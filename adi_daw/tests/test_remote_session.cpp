// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/remote_devices.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"
#include "juce/audiogridder_transport.hpp"
#include "juce/clap_host.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using namespace adi;
namespace fs = std::filesystem;
constexpr auto fixtureId = "org.adi.catalogue.runtime";
constexpr auto fixtureName = "Catalogue runtime";
constexpr std::int32_t blockSize = 64;
constexpr double sampleRate = 48000;
constexpr std::int64_t requestedHost = 7;
constexpr std::int64_t beginSample = 137;
constexpr std::int64_t endSample = 1003;

void require(bool result, const std::string& error) {
    if (!result) throw std::runtime_error(error);
}

std::unique_ptr<device::DeviceInstance> loadFixture(device::ClapHost& host, const std::string& path,
                                                    std::string& error) {
    device::ClapPluginRef ref;
    ref.bundlePath = path;
    ref.id = fixtureId;
    ref.name = fixtureName;
    auto instance = host.makeDevice(ref, sampleRate, blockSize, error);
    if (!instance || !instance->loaded()) return {};
    if (!instance->loadState("chunk", {'A', 'D', 'C', '1', 2, 0, 0, 0})) {
        error = "fixture refused its half-gain state";
        return {};
    }
    return instance;
}

// The TCP fixture runs this child. A fixed-size pipe exchange calls the same
// real CLAP module as the local render, with fresh activation and state.
int hostFixture(const std::string& path) {
#if defined(_WIN32)
    require(_setmode(_fileno(stdin), _O_BINARY) != -1 && _setmode(_fileno(stdout), _O_BINARY) != -1,
            "cannot use binary fixture pipes");
#endif
    device::ClapHost host;
    std::string error;
    auto instance = loadFixture(host, path, error);
    require(instance != nullptr, error);
    instance->prepare(sampleRate, blockSize);
    std::array<float, 2 * blockSize> input{}, output{};
    std::array<const float*, 2> in{input.data(), input.data() + blockSize};
    std::array<float*, 2> out{output.data(), output.data() + blockSize};
    engine::NodeIo io;
    io.in = in.data();
    io.out = out.data();
    io.frames = blockSize;
    io.channels = 2;
    for (;;) {
        const auto got = std::fread(input.data(), sizeof(float), input.size(), stdin);
        if (got == 0 && std::feof(stdin)) break;
        require(got == input.size(), "fixture received a partial PCM block");
        instance->process(io);
        require(std::fwrite(output.data(), sizeof(float), output.size(), stdout) == output.size() &&
                std::fflush(stdout) == 0, "fixture could not return PCM");
    }
    return 0;
}

struct Project {
    test::TempDirectory dir{"remote_session", "render"};
    std::unique_ptr<Store> store;

    Project() {
        StoreError error{};
        store = Store::createEmpty(dir.path() / "remote.adi", error);
        require(store != nullptr, "could not create fixture project");
        store->db().exec(
            "INSERT INTO project(id,name,sample_rate) VALUES(1,'Remote test',48000);"
            "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Master',1);"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','input.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,1000000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,2048);"
            "INSERT INTO remote_hosts(id,name,host,port) VALUES(7,'Fixture','127.0.0.1',55056);"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype)"
            " VALUES(1,'clap','org.adi.catalogue.runtime','Catalogue runtime','effect');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name)"
            " VALUES(1,1,0,1,'Catalogue runtime');");
        std::vector<float> samples(4096);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<float>(static_cast<int>(i % 191) - 95) / 128.f;
        audio::WavWriter wave(dir.path() / "input.wav", 48000, 2, audio::WavFormat::Float32);
        wave.write(samples.data(), 2048);
        wave.close();
    }

    render::Job job(std::string name) const {
        render::Job job;
        job.project = store->path();
        job.directory = dir.path();
        job.exactWavFilename = std::move(name);
        job.beginSample = beginSample;
        job.endSample = endSample;
        job.session = {2, sampleRate, blockSize};
        job.encoding.format = audio::WavFormat::Float32;
        return job;
    }
};

std::vector<float> samples(const render::Result& result) {
    require(result.state == render::JobState::Completed, result.error);
    audio::WavReader file(result.output);
    constexpr auto frames = static_cast<std::uint32_t>(endSample - beginSample);
    require(file.frames() == frames && file.channels() == 2, "wrong rendered extent");
    std::vector<float> output(2 * frames);
    require(file.read(output.data(), frames) == frames, "could not read rendered PCM");
    return output;
}

int testRender(const std::string& path, std::uint16_t port, bool expectDisconnect) {
    Project project;
    device::ClapHost host;
    int localLoads = 0;
    const engine::DeviceLoader local = [&](const engine::DeviceRequest&, std::string& error) {
        ++localLoads;
        return loadFixture(host, path, error);
    };
    const auto localAudio = samples(render::renderWav(project.job("local.wav"), engine::withRemoteDevices(local)));

    project.store->db().exec("UPDATE devices SET remote_host_id=7 WHERE id=1;");
    auto noGrant = engine::withRemoteDevices(local);
    {
        engine::Session live;
        require(live.load(*project.store, noGrant, {2, sampleRate, blockSize}), live.error());
        require(live.stats().loaded == 1 && localLoads == 2, "unavailable live server did not fall back locally");
        require(std::ranges::any_of(live.problems(), [](const std::string& issue) {
            return issue.find("server #7") != std::string::npos;
        }), "the live fallback omitted the server diagnostic");
    }
    const auto refused = render::renderWav(project.job("unauthorized.wav"), noGrant);
    require(refused.state == render::JobState::Failed && refused.error.find(fixtureName) != std::string::npos &&
            !fs::exists(project.dir.path() / "unauthorized.wav") && localLoads == 2,
            "unavailable offline server did not fail by name before publishing");

    const engine::DeviceLoader grant = [port](const engine::DeviceRequest& rq, std::string& error)
        -> std::unique_ptr<device::DeviceInstance> {
        if (rq.device->remoteHostId != requestedHost || rq.ref->format != "clap" || rq.ref->uid != fixtureId) {
            error = "fixture grant does not cover this use";
            return {};
        }
        // The v13 loopback fixture maps this wire token to the test CLAP child.
        device::AudioGridderPlugin plugin;
        plugin.serverToken = "VST3-12ab";
        auto remote = std::make_unique<device::RemoteDevice>(
            device::DeviceIdentity{rq.ref->format, rq.ref->uid, rq.ref->name, {}, {}}, rq.channels,
            device::makeAudioGridderProcessor({"127.0.0.1", port}, std::move(plugin)));
        remote->setOffline(rq.offline);
        remote->prepare(rq.sampleRate, rq.maxFrames);
        return remote;
    };
    const auto job = project.job("remote.wav");
    const auto result = render::renderWav(job, engine::withRemoteDevices(local, grant));
    require(localLoads == 2, "remote render used a local instance");
    if (expectDisconnect) {
        require(result.state == render::JobState::Failed && result.framesWritten > 0 &&
                result.error.find(fixtureName) != std::string::npos && result.error.find("server #7") != std::string::npos &&
                !fs::exists(job.directory / "remote.wav") &&
                std::ranges::none_of(fs::directory_iterator(job.directory), [](const fs::directory_entry& entry) {
                    return media::pathUtf8(entry.path().filename()).starts_with(".adi-render-");
                }), "server loss did not fail by name and discard staged audio: " + result.error);
        return 0;
    }
    const auto remoteAudio = samples(result);
    float peak = 0.f;
    float maxDifference = 0.f;
    for (std::size_t i = 0; i < localAudio.size(); ++i) {
        peak = std::max(peak, std::abs(localAudio[i]));
        maxDifference = std::max(maxDifference, std::abs(localAudio[i] - remoteAudio[i]));
    }
    std::printf("%zu rendered samples, peak %.8f, maximum difference %.8f\n",
                remoteAudio.size(), static_cast<double>(peak), static_cast<double>(maxDifference));
    require(peak > .1f && maxDifference <= 1e-7f, "the compensated remote render differs from the local plug-in");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "host") return hostFixture(argv[2]);
        require(argc == 4, "missing fixture paths or port");
        const std::string_view mode(argv[1]);
        require(mode == "render" || mode == "disconnect", "invalid fixture mode");
        std::uint16_t port = 0;
        const std::string_view value(argv[3]);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
        require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && port != 0, "invalid fixture port");
        return testRender(argv[2], port, mode == "disconnect");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
