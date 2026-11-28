// SPDX-License-Identifier: GPL-3.0-or-later
// SC-479: exporting while the song plays harms neither the export nor the
// playback. The export is a second engine that renders the project on a worker
// with plug-ins of its own; these checks prove what the sanity check listed as
// not yet proved: an edit made during the export, two engines on one project
// file, a plug-in that cannot be loaded a second time, and the memory the
// second set of plug-ins and samples holds while the export runs.
#include "temp_directory.hpp"
#include "adi/render/queue.hpp"
#include "adi/render/host_dispatch.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <new>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace adi;
using namespace adi::engine;
using namespace adi::render;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {++failures; std::printf("FAIL: %s\n", name);}
}
float source(std::int64_t frame, int channel) {
    return static_cast<float>((frame % 191) - 95) / 128.0f + static_cast<float>(channel) / 16.0f;
}
struct Fixture {
    test::TempDirectory dir{"export_while_playing","project"};
    std::unique_ptr<Store> store;
    Fixture() {
        StoreError error{};
        store = Store::createEmpty(dir.path()/"project.adi", error);
        if (!store) throw std::runtime_error("fixture store");
        store->db().exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Song',48000);"
            "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Master',1);"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,1000000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,4096);");
        std::vector<float> samples(8192);
        for (int i = 0; i < 4096; ++i) for (int c = 0; c < 2; ++c)
            samples[static_cast<std::size_t>(2*i+c)] = source(i,c);
        audio::WavWriter wave(dir.path()/"audio.wav",48000,2,audio::WavFormat::Float32);
        wave.write(samples.data(),4096); wave.close();
        fs::create_directory(dir.path()/"out");
    }
    Job job(const std::string& name, const fs::path& project) const {
        Job j;
        j.project = project; j.directory = dir.path()/"out"; j.nameTemplate = name;
        j.beginSample = 137; j.endSample = 1003; j.session = {2,48000,64};
        j.encoding.format = audio::WavFormat::Float32;
        return j;
    }
    void parallel() {
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(2,'audio','Parallel',2);"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(2,2,'audio',1,0,1000000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(2,1,0,4096);");
    }
    void effect(std::int64_t device = 1, std::int64_t track = 1, const char* name = "Delay") {
        const auto id = std::to_string(device);
        store->db().exec("INSERT OR IGNORE INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.delay','Delay','effect');"
            "INSERT INTO device_chains(id,track_id) VALUES(" + id + "," + std::to_string(track) + ");"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(" + id + "," + id + ",0,1,'" + name + "');");
    }
    // A saved device state of `bytes` bytes, as a sampler keeps its samples.
    void state(std::int64_t device, std::size_t bytes) {
        std::vector<std::byte> data(bytes);
        for (std::size_t i = 0; i < bytes; ++i) data[i] = static_cast<std::byte>((i * 131 + static_cast<std::size_t>(device)) & 0xff);
        const auto hash = media::blake3Bytes(data).hex;
        SQLite::Statement blob(store->db(),"INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
        blob.bind(1,hash);
        blob.bind(2,data.data(),static_cast<int>(bytes));
        blob.bind(3,static_cast<std::int64_t>(bytes));
        blob.exec();
        SQLite::Statement row(store->db(),
            "INSERT INTO plugin_state(device_id,stream_role,state_hash,format_hint) VALUES(?,'state',?,'opaque')");
        row.bind(1,device);
        row.bind(2,hash);
        row.exec();
    }
    bool clean() const {
        for (const auto& p : fs::directory_iterator(dir.path())) {
            const auto name = media::pathUtf8(p.path().filename());
            if (name.starts_with(".adi-export-")) return false;
        }
        for (const auto& p : fs::directory_iterator(dir.path()/"out"))
            if (media::pathUtf8(p.path().filename()).starts_with(".adi-render-")) return false;
        return true;
    }
};
void edit(Store& store, std::int64_t track, bool muted) {
    OpRequest r;
    r.opType = "track.setMute";
    r.payload = {{"id", track}, {"muted", muted}};
    if (!OpJournal(store).commit(r).ok) throw std::runtime_error("fixture mute edit");
}
std::int64_t head(Store& store) {
    const auto value = store.db().execAndGet("SELECT head_seq FROM op_branches WHERE is_current=1");
    return value.isNull() ? 0 : value.getInt64();
}
// Latency 77, compensated by the export; a plug-in the test can count.
struct Probe {
    std::atomic<int> created{0}, exported{0}, destroyed{0};
    std::atomic<std::int64_t> stateBytes{0}, peakBytes{0};
    std::atomic<bool> exportOnLiveThread{false}, liveOffLiveThread{false};
    std::thread::id liveThread = std::this_thread::get_id();
};
struct Delay : device::DeviceInstance {
    Probe& probe;
    bool exporting;
    device::DeviceIdentity identity_;
    std::array<std::array<double,256>,2> ring{};
    int at = 0;
    std::vector<std::uint8_t> held;
    Delay(Probe& p, bool forExport) : probe(p), exporting(forExport) {
        ++probe.created;
        if (exporting) ++probe.exported;
    }
    ~Delay() override {
        probe.stateBytes -= static_cast<std::int64_t>(held.size());
        ++probe.destroyed;
    }
    const device::DeviceIdentity& identity() const noexcept override {return identity_;}
    bool loaded() const noexcept override {return true;}
    bool processesDouble() const noexcept override {return true;}
    std::int32_t latencySamples() const noexcept override {return 77;}
    std::int64_t tailSamples() const noexcept override {return 77;}
    bool loadState(const std::string&, const std::vector<std::uint8_t>& data) override {
        held = data;
        const auto now = probe.stateBytes += static_cast<std::int64_t>(held.size());
        auto peak = probe.peakBytes.load();
        while (now > peak && !probe.peakBytes.compare_exchange_weak(peak, now)) {}
        return true;
    }
    void process(const NodeIo& io) noexcept override {
        const bool live = std::this_thread::get_id() == probe.liveThread;
        if (exporting && live) probe.exportOnLiveThread = true;
        if (!exporting && !live) probe.liveOffLiveThread = true;
        for (int i = 0; i < io.frames; ++i) {
            for (int c = 0; c < io.channels; ++c) {
                const auto channel = static_cast<std::size_t>(c);
                const auto index = static_cast<std::size_t>(io.blockOffset+i);
                const double x = io.wide() ? io.in64[c][index] : static_cast<double>(io.in[c][index]);
                const double y = ring[channel][static_cast<std::size_t>((at+256-77)%256)];
                ring[channel][static_cast<std::size_t>(at)] = x;
                if (io.wide()) io.out64[c][index] = y;
                else io.out[c][index] = static_cast<float>(y);
            }
            at = (at+1)%256;
        }
    }
};
DeviceLoader loaderFor(Probe& probe) {
    return [&probe](const DeviceRequest& request, std::string&) -> std::unique_ptr<device::DeviceInstance> {
        return std::make_unique<Delay>(probe, request.exporting);
    };
}
std::vector<float> read(const Result& result, std::uint64_t frames) {
    check(result.state==JobState::Completed, result.error.empty() ? "job completed" : result.error.c_str());
    if (result.state!=JobState::Completed) return {};
    audio::WavReader wave(result.output);
    check(wave.frames()==frames && wave.channels()==2, "published WAV has the range's frames in stereo");
    std::vector<float> samples(static_cast<std::size_t>(2*frames));
    check(wave.read(samples.data(),static_cast<std::uint32_t>(frames))==frames,"published samples decode");
    return samples;
}
bool matches(const std::vector<float>& samples, std::int64_t begin, double gain) {
    if (samples.empty()) return false;
    for (std::size_t i = 0; i < samples.size()/2; ++i) for (int c = 0; c < 2; ++c) {
        const auto at = begin + static_cast<std::int64_t>(i);
        const float expected = static_cast<float>(gain * source(at,c));
        if (samples[2*i+static_cast<std::size_t>(c)] != expected) return false;
    }
    return true;
}

// An edit made while the export runs reaches the song and never the files.
void editDuringExport() {
    {
        // The finding, kept as the control: without a pinned copy each file of
        // a batch reads the project when its own render starts, so an edit made
        // during the first file reaches the second.
        Fixture f; f.parallel();
        Queue queue;
        queue.add(f.job("unpinned-first", f.store->path()));
        queue.add(f.job("unpinned-second", f.store->path()));
        Control control;
        bool edited = false;
        control.wavProgress = [&](JobId id, double) {
            if (id == 1 && !edited) { edited = true; edit(*f.store, 2, true); }
        };
        queue.run({}, control);
        const auto entries = queue.snapshot();
        check(edited && matches(read(entries[0].result,866),137,2) && matches(read(entries[1].result,866),137,1),
              "control: without a pinned copy the second file of a batch takes an edit made during the first");
    }
    Fixture f; f.parallel();
    const auto before = head(*f.store);
    auto pinned = PinnedProject::capture(*f.store);
    check(pinned && fs::exists(pinned->path()) && pinned->path().parent_path() == f.store->path().parent_path(),
          "the export's copy of the song is a file beside the project");
    // An edit between the press of Export and the worker's start.
    edit(*f.store, 1, true);
    Queue queue;
    queue.add(f.job("pinned-first", pinned->path()));
    queue.add(f.job("pinned-second", pinned->path()));
    Control control;
    bool edited = false;
    control.wavProgress = [&](JobId id, double) {
        if (id == 1 && !edited) { edited = true; edit(*f.store, 2, true); }
    };
    queue.run({}, control);
    const auto entries = queue.snapshot();
    check(edited && matches(read(entries[0].result,866),137,2) && matches(read(entries[1].result,866),137,2),
          "edits made after Export was pressed, before and during the render, reach no file of the batch");
    check(entries[0].result.snapshot && entries[1].result.snapshot &&
          entries[0].result.snapshot->journalHead == before && entries[1].result.snapshot->journalHead == before &&
          head(*f.store) != before,
          "every file of the export records the one revision of the press, while the song moved on");
    check(f.store->db().execAndGet("SELECT muted FROM tracks WHERE id=2").getInt() == 1,
          "the edit made during the export is in the song");
    pinned.reset();
    check(f.clean(), "the export's copy of the song is removed when the export lets go of it");
}

// A plug-in that plays but cannot be loaded a second time is named.
void secondInstance() {
    Fixture f; f.effect();
    auto pinned = PinnedProject::capture(*f.store);
    auto refusing = [](const DeviceRequest& request, std::string& error) -> std::unique_ptr<device::DeviceInstance> {
        if (request.exporting) error = "only one instance may run on this computer";
        return nullptr;
    };
    auto job = f.job("single-instance", pinned->path());
    job.playingDevices = {1};
    const auto refused = renderWav(job, refusing);
    check(refused.state == JobState::Failed && refused.error.find("Delay") != std::string::npos &&
          refused.error.find("second time") != std::string::npos &&
          refused.error.find("only one instance may run on this computer") != std::string::npos &&
          !fs::exists(f.dir.path()/"out"/"single-instance.wav"),
          "a playing plug-in that refuses a second instance stops the export, named with its reason");
    job.playingDevices.clear();
    job.nameTemplate = "missing-in-playback";
    const auto missing = renderWav(job, refusing);
    check(missing.state == JobState::Completed && matches(read(missing,866),137,1),
          "a plug-in missing in playback too is bypassed in the file, as in playback (SC-395)");
    job.playingDevices = {1};
    job.nameTemplate = "out-of-memory";
    const auto full = renderWav(job, [](const DeviceRequest& request, std::string&) -> std::unique_ptr<device::DeviceInstance> {
        if (request.exporting) throw std::bad_alloc();
        return nullptr;
    });
    check(full.state == JobState::Failed && full.error.find("Delay: there is not enough memory") != std::string::npos &&
          !fs::exists(f.dir.path()/"out"/"out-of-memory.wav"),
          "a second set that does not fit in memory stops the export and names the device");
    pinned.reset();
    check(f.clean(), "refused exports leave no copy and no stage");
}

// Two engines on one project file: the playing one is not touched.
std::vector<float> play(Session& session, int blocks, const std::function<void()>& between) {
    constexpr int frames = 64;
    std::vector<float> heard;
    std::array<std::array<float,frames>,2> out{};
    float* channels[] = {out[0].data(), out[1].data()};
    for (int b = 0; b < blocks; ++b) {
        if (between) between();
        if (!session.clips()->prime(std::chrono::seconds(10))) throw std::runtime_error("live prime");
        AudioIo io; io.out = channels; io.numOut = 2; io.frames = frames;
        {
            audio::CallbackScope callback;
            session.process(io);
        }
        for (int i = 0; i < frames; ++i) for (std::size_t c = 0; c < 2; ++c)
            heard.push_back(out[c][static_cast<std::size_t>(i)]);
    }
    return heard;
}
void twoEngines() {
    constexpr int blocks = 400;
    Fixture f; f.effect();
    Probe alone;
    std::vector<float> reference;
    {
        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*f.store, loaderFor(alone), {2,48000,64})) throw std::runtime_error(session.error());
        session.transport().play();
        reference = play(session, blocks, {});
    }
    auto pinned = PinnedProject::capture(*f.store);
    auto job = f.job("export-alone", pinned->path());
    job.beginSample = 0; job.endSample = 24000;
    job.playingDevices = {1};
    const auto exportedAlone = read(renderWav(job, loaderFor(alone)), 24000);
    job.nameTemplate = "while-playing";
    Probe probe;
    Session live;
    live.graph().setFadeFrames(0);
    if (!live.load(*f.store, loaderFor(probe), {2,48000,64})) throw std::runtime_error(live.error());
    live.transport().play();
    const auto liveInstance = live.instanceFor(1);
    HostDispatch host;
    Control control;
    control.onHost = [&](std::function<void()> action) { host.invoke(std::move(action)); };
    int serviced = 0;
    std::packaged_task<Result()> task([&] { return renderWav(job, loaderFor(probe), control); });
    auto done = task.get_future();
    std::jthread worker(std::move(task));
    // The message thread services the export between the playing engine's
    // blocks, as the app's does between driver callbacks.
    const auto heard = play(live, blocks, [&] {
        while (host.service()) ++serviced;
    });
    host.wait(done);
    const auto result = done.get();
    const auto file = read(result, 24000);
    std::vector<float> opening;
    if (file.size() >= 2 * 4000) opening.assign(file.begin(), file.begin() + 2 * 4000);
    check(!file.empty() && file == exportedAlone && matches(opening, 0, 1) && serviced > 0,
          "the file exported beside playback is identical to the file exported alone");
    check(heard == reference, "the playing engine's output is identical with and without an export running beside it");
    check(probe.created == 2 && probe.exported == 1 && probe.destroyed == 1 && live.instanceFor(1) == liveInstance,
          "the export loads its own instance and frees it; the playing instance stays the same one");
    check(!probe.exportOnLiveThread && !probe.liveOffLiveThread,
          "the export's plug-ins never process on the playing thread, the playing ones never on the export's");
    pinned.reset();
    check(f.clean(), "the export beside playback leaves no copy and no stage");
}

// The director's question of 28 November: is a huge project twice as heavy?
// Yes, while the export runs: its own plug-ins hold their states (a sampler's
// samples) a second time. Measured here, and freed when the export ends.
void secondSetMemory() {
    constexpr std::size_t perDevice = std::size_t{4} << 20;
    Fixture f;
    f.effect(1, 1, "Sampler A"); f.effect(2, 9, "Sampler B");
    f.state(1, perDevice); f.state(2, perDevice);
    Probe probe;
    Session live;
    if (!live.load(*f.store, loaderFor(probe), {2,48000,64})) throw std::runtime_error(live.error());
    const auto playing = probe.stateBytes.load();
    auto pinned = PinnedProject::capture(*f.store);
    auto job = f.job("memory", pinned->path());
    job.playingDevices = {1, 2};
    std::int64_t during = 0;
    Control control;
    control.wavProgress = [&](JobId, double) { during = (std::max)(during, probe.stateBytes.load()); };
    const auto started = std::chrono::steady_clock::now();
    const auto result = renderWav(job, loaderFor(probe), control);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("SC-479 second set: playing %lld bytes, during export %lld bytes, after %lld bytes, export %.3f s\n",
                static_cast<long long>(playing), static_cast<long long>(during),
                static_cast<long long>(probe.stateBytes.load()), seconds);
    check(result.state == JobState::Completed, result.error.empty() ? "memory export completes" : result.error.c_str());
    check(playing == static_cast<std::int64_t>(2 * perDevice) && during == 2 * playing,
          "while the export runs, its plug-ins hold the song's device states a second time");
    check(probe.stateBytes.load() == playing && probe.exported == 2 && probe.created == 4 && probe.destroyed == 2,
          "the second set is freed when the export ends");
    pinned.reset();
    check(f.clean(), "the memory export leaves no copy and no stage");
}
} // namespace

int main() {
    try {
        editDuringExport();
        secondInstance();
        twoEngines();
        secondSetMemory();
    } catch (const std::exception& e) {
        check(false, e.what());
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
