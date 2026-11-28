// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/remote_devices.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"
#include "juce/audiogridder_transport.hpp"
#include "juce/clap_host.hpp"
#include "juce/net_plugin_admission.hpp"

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
            "UPDATE adi_meta SET value='remote-session-fixture' WHERE key='project_uuid';"
            "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Master',1);"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','input.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,1000000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,2048);"
            "INSERT INTO remote_hosts(id,name,host,port,enabled) VALUES(7,'Fixture','127.0.0.1',55056,1);"
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

int testRemoteAutomation() {
    struct Delivery {
        std::size_t manual = 0;
        std::size_t automated = 0;
    } delivery;
    struct Processor final : device::RemoteProcessor {
        explicit Processor(Delivery& counts) : counts_(counts) {}
        bool prepare(double, std::int32_t, std::int32_t, bool, std::int32_t& latency, std::string&) override {
            latency = 0;
            return true;
        }
        bool readParameters(std::vector<device::RemoteParameter>& output, StateDeadline, std::string&) override {
            device::RemoteParameter manual;
            manual.selector = 7;
            manual.descriptor.id = "7";
            manual.descriptor.automatable = false;
            manual.descriptor.configurable = true;
            device::RemoteParameter automated;
            automated.selector = 8;
            automated.descriptor.id = "8";
            auto missing = manual;
            missing.selector = 9;
            missing.descriptor.id = "9";
            missing.descriptor.missing = true;
            output = {std::move(manual), std::move(automated), std::move(missing)};
            return true;
        }
        bool exchange(const device::RemoteBlock& input, std::span<float> output, std::string& error) override {
            for (const auto& event : input.events) {
                if (event.type != engine::EventType::ParamValue) continue;
                if (event.paramId == 7 || event.paramId == 9) {
                    ++counts_.manual;
                    error = "manual-only automation reached remote audio";
                    return false;
                }
                if (event.paramId == 8) ++counts_.automated;
            }
            std::ranges::copy(input.audio, output.begin());
            return true;
        }
        Delivery& counts_;
    };

    Project project;
    project.store->db().exec("UPDATE devices SET remote_host_id=7 WHERE id=1;"
        "UPDATE plugin_refs SET format='vst3',uid='00112233445566778899AABBCCDDEEFF' WHERE id=1;"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain,default_value) VALUES"
        "(1,'device',1,'7',1,'normalized',0.25),(2,'device',1,'8',1,'normalized',0.75),"
        "(3,'device',1,'9',1,'normalized',0.5);");
    const engine::DeviceLoader remote = [&](const engine::DeviceRequest& request, std::string&) {
        auto instance = std::make_unique<device::RemoteDevice>(
            device::DeviceIdentity{request.ref->format, request.ref->uid, fixtureName, {}, {}}, request.channels,
            std::make_unique<Processor>(delivery));
        instance->setOffline(request.offline);
        instance->prepare(request.sampleRate, request.maxFrames);
        return instance;
    };
    const auto result = render::renderWav(project.job("automation.wav"), engine::withRemoteDevices({}, remote));
    const auto audio = samples(result);
    require(std::ranges::any_of(audio, [](float sample) { return sample != 0.0f; }) &&
            delivery.manual == 0 && delivery.automated > 0 &&
            project.store->db().execAndGet("SELECT COUNT(*) FROM automation_lanes").getInt() == 3,
            "remote playback must deliver the automatable lane and preserve all authored lanes");
    return 0;
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
    SQLite::Statement location(project.store->db(), "UPDATE remote_hosts SET port=? WHERE id=?");
    location.bind(1, static_cast<int>(port));
    location.bind(2, requestedHost);
    location.exec();
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

    // A positive track delay makes the whole selected range leading silence.
    // The export can complete this path without rendering a graph block.
    project.store->db().exec("INSERT INTO mixer_strip(track_id,delay_samples) VALUES(1,64);"
                             "UPDATE devices SET remote_host_id=NULL WHERE id=1;");
    auto leading = project.job("silent-local.wav");
    leading.beginSample = 0;
    leading.endSample = 32;
    leading.source.trackId = 1;
    const auto localPlaceholder = render::renderWav(leading, {});
    require(localPlaceholder.state == render::JobState::Completed, localPlaceholder.error);
    {
        audio::WavReader wave(localPlaceholder.output);
        std::array<float, 64> pcm{};
        require(wave.frames() == 32 && wave.read(pcm.data(), 32) == 32 &&
                std::ranges::all_of(pcm, [](float sample) { return sample == 0.0f; }),
                "a missing local effect must still export the requested leading silence");
    }
    project.store->db().exec("UPDATE devices SET remote_host_id=7 WHERE id=1;");
    leading.exactWavFilename = "silent-remote.wav";
    const auto remotePlaceholder = render::renderWav(leading, noGrant);
    require(remotePlaceholder.state == render::JobState::Failed && remotePlaceholder.framesWritten == 0 &&
            !remotePlaceholder.snapshot && remotePlaceholder.error.find(fixtureName) != std::string::npos &&
            remotePlaceholder.error.find("server #7") != std::string::npos &&
            !fs::exists(project.dir.path() / "silent-remote.wav") && localLoads == 2,
            "unavailable authored server reached leading-silence output: " +
            std::to_string(remotePlaceholder.framesWritten) + " frames; " + remotePlaceholder.error);
    project.store->db().exec("DELETE FROM mixer_strip WHERE track_id=1;");

    {
        const std::vector<std::uint8_t> bytes{7, 25, 193};
        const auto hash = media::blake3Bytes(std::as_bytes(std::span(bytes))).hex;
        SQLite::Statement blob(project.store->db(), "INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,3)");
        blob.bind(1, hash);
        blob.bind(2, bytes.data(), static_cast<int>(bytes.size()));
        blob.exec();
        SQLite::Statement state(project.store->db(),
            "INSERT INTO plugin_state(device_id,stream_role,state_hash) VALUES(1,'chunk',?)");
        state.bind(1, hash);
        state.exec();
        engine::Session live;
        require(live.load(*project.store, engine::withRemoteDevices({}), {2, sampleRate, blockSize}), live.error());
        const auto* instance = live.instanceFor(1);
        require(live.stats().placeholders == 1 && instance && instance->identity().uid == fixtureId &&
                instance->saveState("chunk") == bytes && std::ranges::any_of(live.problems(), [](const auto& issue) {
                    return issue.find("server #7") != std::string::npos;
                }), "a missing server and local plug-in must retain a named placeholder and its saved state");
        project.store->db().exec("DELETE FROM plugin_state WHERE device_id=1;");
    }

    const net::PluginServerAddress endpoint{"127.0.0.1", port};
    const auto grant = net::makePluginServerLoader(
        "adi-daw",
        [&](const net::PluginServerLocation& location, const device::DeviceIdentity& identity)
            -> std::optional<net::PluginServerBinding> {
            if (location.id != requestedHost || location.address != endpoint ||
                identity.format != "clap" || identity.uid != fixtureId) return {};
            return net::PluginServerBinding{"fixture-connection", "fixture-server",
                [](const net::PluginServerAddress& address) {
                    // The v13 loopback fixture maps this token to the test CLAP child.
                    device::AudioGridderPlugin plugin;
                    plugin.serverToken = "VST3-12ab";
                    return device::makeAudioGridderProcessor({address.host, address.port}, std::move(plugin));
                }};
        },
        [&](const Store&, const net::PluginHostingScope& scope) -> net::PluginHostingLease {
            if (scope.projectUuid != "remote-session-fixture" || scope.address != endpoint ||
                scope.serverFingerprint != "fixture-server") return {};
            return [] { return true; };
        });
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
        if (argc == 2 && std::string_view(argv[1]) == "automation") return testRemoteAutomation();
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
