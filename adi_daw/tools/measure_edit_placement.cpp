// SPDX-License-Identifier: AGPL-3.0-or-later
// Opt-in diagnostic measurement, not a correctness test or latency threshold.
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/edit_plan.hpp"
#include "adi/textproj.hpp"
#include "adi/ui/project_document.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
thread_local bool countOwner = false;
thread_local std::uint64_t ownerAllocations = 0;
std::atomic<std::uint64_t> callbackAllocations{0};
}
void* operator new(std::size_t size) {
    if (countOwner) ++ownerAllocations;
    if (adi::audio::onAudioThread) callbackAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {
using namespace adi;
using namespace adi::engine;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr std::int32_t frames = 256;
constexpr std::int64_t quarter = textproj::kPPQ;
constexpr double rate = 48000;
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
double milliseconds(std::int64_t begin, std::int64_t end) {
    return static_cast<double>(end - begin) / 1000000.;
}
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Probe final : device::DeviceInstance {
    device::DeviceIdentity identity_;
    bool instrument = false;
    bool held = false; // Audio reader only.
    explicit Probe(const DeviceRequest& request) {
        identity_.format = request.ref->format;
        identity_.uid = request.ref->uid;
        identity_.name = request.ref->name;
        instrument = request.ref->subtype == "instrument";
    }
    const device::DeviceIdentity& identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return instrument ? EventFlow::Consume : EventFlow::Through; }
    void process(const NodeIo& io) noexcept override {
        for (const auto& event : io.events) {
            if (event.type == EventType::NoteOn) held = true;
            if (event.type == EventType::NoteOff) held = false;
        }
        for (std::int32_t channel = 0; channel < io.channels; ++channel) {
            for (std::int32_t frame = 0; frame < io.frames; ++frame) {
                const auto index = io.blockOffset + frame;
                if (io.wide()) {
                    io.out64[channel][index] = instrument ? (held ? 0.0001 : 0.) :
                        (io.in64 && io.in64[channel] ? io.in64[channel][index] : 0.);
                } else {
                    io.out[channel][index] = instrument ? (held ? 0.0001f : 0.f) :
                        (io.in && io.in[channel] ? io.in[channel][index] : 0.f);
                }
            }
        }
    }
};
struct Loader {
    std::uint64_t calls = 0;
    DeviceLoader function() {
        return [this](const DeviceRequest& request, std::string&) -> std::unique_ptr<device::DeviceInstance> {
            ++calls;
            return std::make_unique<Probe>(request);
        };
    }
};
void fixture(const fs::path& directory) {
    fs::create_directories(directory);
    constexpr std::uint32_t sourceFrames = 96000;
    std::vector<float> pcm(static_cast<std::size_t>(sourceFrames) * 2);
    for (std::uint32_t frame = 0; frame < sourceFrames; ++frame) {
        const auto sample = static_cast<float>(0.002 * std::sin(2 * std::numbers::pi * 251 * frame / rate));
        pcm[static_cast<std::size_t>(frame) * 2] = sample;
        pcm[static_cast<std::size_t>(frame) * 2 + 1] = sample * 0.75f;
    }
    audio::WavWriter writer(directory / "source.wav", 48000, 2, audio::WavFormat::Float32);
    writer.write(pcm.data(), sourceFrames); writer.close();
    StoreError error{};
    auto store = Store::createEmpty(directory / "fixture.adi", error);
    require(bool(store), toString(error));
    auto& db = store->db();
    SQLite::Transaction transaction(db);
    db.exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1000,'master','Main',100);"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES"
            "(1,'vst3','adi.measure.pass','Pass','effect'),(2,'vst3','adi.measure.notes','Notes','instrument')");
    SQLite::Statement media(db, "INSERT INTO media_files(id,hash_blake3,abs_path_hint,format,sample_rate,channels,frames) VALUES(1,'placement-fixture',?,'wav',48000,2,96000)");
    media.bind(1, fs::absolute(directory / "source.wav").string()); media.exec();
    const auto map = audio::writeWarpMap(std::vector<WarpMarker>{{0, 0}, {sourceFrames, 4 * quarter}}, sourceFrames);
    for (std::int64_t id = 1; id <= 100; ++id) {
        const bool midi = id % 3 == 0;
        SQLite::Statement track(db, "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(?,?,?,?)");
        track.bind(1, id); track.bind(2, midi ? "midi" : "audio");
        track.bind(3, "Track " + std::to_string(id)); track.bind(4, id - 1); track.exec();
        SQLite::Statement chain(db, "INSERT INTO device_chains(id,track_id) VALUES(?,?)");
        chain.bind(1, id); chain.bind(2, id); chain.exec();
        SQLite::Statement device(db, "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(?,?,0,?,'Probe')");
        device.bind(1, id); device.bind(2, id); device.bind(3, midi ? 2 : 1); device.exec();
        for (std::int64_t part = 0; part < 2; ++part) {
            const auto clip = id + part * 100;
            SQLite::Statement row(db, "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,name) VALUES(?,?,?,?,?,'Material')");
            row.bind(1, clip); row.bind(2, id); row.bind(3, midi ? "midi" : "audio");
            row.bind(4, part == 0 ? 0 : 2 * quarter); row.bind(5, part == 0 ? 2 * quarter : quarter / 4); row.exec();
            if (midi) {
                NoteRecord note{}; note.note_id = static_cast<std::uint32_t>(clip);
                note.dur_ticks = 4 * quarter; note.key = 60; note.vel_on = 100; note.probability = 10000;
                const auto stream = writeStream<NoteRecord>(FourCC::Notes, std::vector<NoteRecord>{note});
                require(store->putEventStream(clip, "notes", stream), "write MIDI source");
            } else {
                SQLite::Statement source(db, "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) VALUES(?,1,96000,?,?,?)");
                source.bind(1, clip); source.bind(2, id % 3 == 2 ? 1 : 0);
                source.bind(3, id % 3 == 2 ? "re-pitch" : "none");
                if (id % 3 == 2) source.bind(4, map.bytes.data(), static_cast<int>(map.bytes.size()));
                else source.bind(4);
                source.exec();
            }
        }
    }
    transaction.commit();
}
// This driver does not touch Store, ProjectView, owner pointers, or non-atomic
// owner graph counters. The swap field is read/written by this reader alone.
struct Driver {
    ui::ProjectDocument& document;
    Precision precision;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> observedSwap{0}, adoptionTime{0}, audibleTime{0};
    std::atomic<std::uint64_t> blocks{0}, overruns{0};
    std::atomic<std::int64_t> maxCallbackNs{0};
    std::int64_t initialSwap = 0;
    std::jthread thread;
    Driver(ui::ProjectDocument& doc, Precision width) : document(doc), precision(width),
        initialSwap(doc.session().graph().stats().swaps) {
        observedSwap.store(initialSwap);
        thread = std::jthread([this] {
            std::array<float, static_cast<std::size_t>(frames) * 2> narrow{};
            std::array<double, static_cast<std::size_t>(frames) * 2> wide{};
            float* output[]{narrow.data(), narrow.data() + frames};
            double* output64[]{wide.data(), wide.data() + frames};
            AudioIo io; io.frames = frames; io.numOut = 2;
            if (precision == Precision::Float64) io.out64 = output64; else io.out = output;
            auto deadline = Clock::now();
            constexpr auto period = std::chrono::nanoseconds(5333333);
            auto previousSwap = initialSwap;
            bool needsAudible = false;
            while (!stop.load(std::memory_order_acquire)) {
                const auto begin = now();
                { audio::CallbackScope callback; document.process(io); }
                const auto finish = now();
                auto maximum = maxCallbackNs.load(std::memory_order_relaxed);
                if (finish - begin > maximum) maxCallbackNs.store(finish - begin, std::memory_order_relaxed);
                if (finish - begin > period.count()) overruns.fetch_add(1, std::memory_order_relaxed);
                const auto swapped = document.session().graph().stats().swaps;
                if (swapped != previousSwap) {
                    previousSwap = swapped;
                    adoptionTime.store(finish, std::memory_order_relaxed);
                    audibleTime.store(0, std::memory_order_relaxed);
                    needsAudible = true;
                    observedSwap.store(swapped, std::memory_order_release);
                }
                if (needsAudible) {
                    const bool audible = precision == Precision::Float64 ?
                        std::any_of(wide.begin(), wide.end(), [](double value) { return std::isfinite(value) && std::abs(value) > 1e-9; }) :
                        std::any_of(narrow.begin(), narrow.end(), [](float value) { return std::isfinite(value) && std::abs(value) > 1e-9f; });
                    if (audible) { audibleTime.store(finish, std::memory_order_release); needsAudible = false; }
                }
                blocks.fetch_add(1, std::memory_order_relaxed);
                io.streamTimeSamples += frames;
                deadline += period;
                std::this_thread::sleep_until(deadline);
            }
        });
    }
    void finish() { stop.store(true, std::memory_order_release); if (thread.joinable()) thread.join(); }
    ~Driver() { finish(); }
};
struct AllocationScope {
    std::uint64_t first = ownerAllocations;
    AllocationScope() { countOwner = true; }
    ~AllocationScope() { countOwner = false; }
    std::uint64_t count() const { return ownerAllocations - first; }
};
Payload measure(ui::ProjectDocument& document, Loader& loader, Precision width, bool planned, int members, bool prime) {
    auto& session = document.session();
    session.transport().loop(24000, 48000);
    session.transport().locate(30000);
    session.transport().play();
    if (prime) require(session.clips()->prime(std::chrono::seconds(30)), "prime warm fixture");
    // Adopt any setup graph before the timed graph; this is outside all spans.
    std::array<float, static_cast<std::size_t>(frames) * 2> setup{};
    float* output[]{setup.data(), setup.data() + frames};
    document.process({output, nullptr, 2, 0, frames, 0});
    const auto oldSwaps = session.graph().stats().swaps;
    const auto oldPublished = session.graph().stats().published;
    const auto oldRebuilds = session.stats().rebuilds;
    const auto oldLoads = loader.calls;
    std::vector<const device::DeviceInstance*> identities;
    for (std::int64_t id = 1; id <= 100; ++id) identities.push_back(session.instanceFor(id));
    const auto callbackBefore = callbackAllocations.load();
    const auto ioBefore = audio::callbackFileIo.load();
    Driver driver(document, width);
    while (driver.blocks.load(std::memory_order_acquire) == 0) std::this_thread::yield();
    std::int64_t begin = 0, preparedAt = 0, committedAt = 0, submittedAt = 0, publishedAt = 0;
    std::uint64_t prepareAlloc = 0, commitAlloc = 0, submitAlloc = 0, syncAlloc = 0;
    {
        AllocationScope allocation;
        begin = now();
        clip_edit::PreparedEdit edit;
        std::vector<OpRequest> raw;
        if (planned) {
            std::vector<std::int64_t> tracks;
            std::vector<edit_plan::Move> moves;
            for (std::int64_t id = 1; id <= members; ++id) {
                tracks.push_back(id); moves.push_back({id, {id, std::nullopt}, {0, quarter / 2}});
            }
            const auto scope = edit_plan::materialScope(tracks, edit_plan::PlacementClock::Ticks, {0, 3 * quarter});
            edit = edit_plan::prepareMove(document.store(), scope, moves, {});
            require(edit.ok, edit.error);
        } else {
            for (std::int64_t id = 1; id <= members; ++id) {
                OpRequest request; request.opType = "clip.move";
                request.payload = {{"id", id}, {"to", quarter / 2}, {"track", id}};
                request.label = "Move Material"; raw.push_back(std::move(request));
            }
        }
        preparedAt = now(); prepareAlloc = allocation.count();
        // Existing submitter runs the real commit then ProjectView refresh.
        // Timestamp inside its helper separates journal from refresh without
        // modifying the UI or approximating it with a second projection.
        ui::SubmitResult submitted;
        if (planned) {
            submitted = document.ops().commitPrepared(std::move(edit),
                [&](Store& store, clip_edit::PreparedEdit& captured, Actor actor) {
                    auto result = clip_edit::commitPrepared(store, captured, actor);
                    committedAt = now(); commitAlloc = allocation.count() - prepareAlloc;
                    return result;
                });
        } else {
            submitted = document.ops().commitPrepared(raw, [&](Store& store, Actor actor) {
                for (auto& request : raw) request.actor = actor;
                auto result = OpJournal(store).commit(raw);
                committedAt = now(); commitAlloc = allocation.count() - prepareAlloc;
                return result;
            });
        }
        submittedAt = now(); submitAlloc = allocation.count() - prepareAlloc - commitAlloc;
        require(submitted.ok, submitted.error);
        require(document.synchronise(), document.error());
        publishedAt = now(); syncAlloc = allocation.count() - prepareAlloc - commitAlloc - submitAlloc;
    }
    const auto timeout = Clock::now() + std::chrono::seconds(10);
    while ((driver.observedSwap.load(std::memory_order_acquire) <= oldSwaps ||
            driver.audibleTime.load(std::memory_order_acquire) == 0) && Clock::now() < timeout)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    driver.finish();
    require(driver.observedSwap.load() > oldSwaps, "new graph adoption timed out");
    require(driver.audibleTime.load() != 0, "adopted graph audible output timed out");
    std::uint64_t retained = 0;
    for (std::int64_t id = 1; id <= 100; ++id)
        if (session.instanceFor(id) == identities[static_cast<std::size_t>(id - 1)]) ++retained;
    Payload result = {
        {"precision", static_cast<int>(width)}, {"path", planned ? "prepared_group_move" : "raw_move_batch"},
        {"members", members}, {"cache", prime ? "existing_document_primed" : "fresh_document_unprimed"},
        {"prepare_ms", milliseconds(begin, preparedAt)}, {"journal_ms", milliseconds(preparedAt, committedAt)},
        {"project_view_and_submit_ms", milliseconds(committedAt, submittedAt)},
        {"session_synchronise_publish_ms", milliseconds(submittedAt, publishedAt)},
        {"owner_total_ms", milliseconds(begin, publishedAt)},
        {"adoption_from_start_ms", milliseconds(begin, driver.adoptionTime.load())},
        {"audible_from_start_ms", milliseconds(begin, driver.audibleTime.load())},
        {"adoption_minus_owner_return_ms", milliseconds(publishedAt, driver.adoptionTime.load())},
        {"prepare_allocations", prepareAlloc}, {"journal_allocations", commitAlloc},
        {"view_submit_allocations", submitAlloc}, {"synchronise_allocations", syncAlloc},
        {"callback_allocations", callbackAllocations.load() - callbackBefore},
        {"callback_file_io", audio::callbackFileIo.load() - ioBefore},
        {"callback_blocks", driver.blocks.load()}, {"callback_overruns", driver.overruns.load()},
        {"callback_max_ms", static_cast<double>(driver.maxCallbackNs.load()) / 1000000.},
        {"new_generation_underrun_samples", session.clips()->underrunSamples()},
        {"new_generation_read_errors", session.clips()->readErrors()},
        {"source_problems", session.clips()->problems()}, {"decode_problems", session.clips()->decodeProblems()},
        {"published", session.graph().stats().published - oldPublished},
        {"rebuilds", session.stats().rebuilds - oldRebuilds}, {"loader_calls", loader.calls - oldLoads},
        {"retained_devices", retained}, {"clips_after", document.store().db().execAndGet("SELECT COUNT(*) FROM clips").getInt64()}
    };
    require(result.at("clips_after").get<std::int64_t>() == (planned ? 200 - members : 200),
        "move fixture did not exercise its expected destination clearing");
    require(retained == 100 && loader.calls == oldLoads, "edit unexpectedly replaced a probe device");
    require(session.graph().stats().published - oldPublished == 1 && session.stats().rebuilds - oldRebuilds == 1,
        "one edit did not publish exactly one graph");
    // Undo/reset is deliberately outside measured intervals and callbacks.
    require(document.ops().undo().ok, "undo benchmark edit");
    require(document.synchronise(), document.error());
    session.graph().collect();
    return result;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 3)
            throw std::runtime_error("usage: adi_edit_placement_measure <new-output-directory> [warm-repetitions:1..100]");
        int warmRepetitions = 12;
        if (argc == 3) {
            const auto* end = argv[2] + std::strlen(argv[2]);
            const auto parsed = std::from_chars(argv[2], end, warmRepetitions);
            require(parsed.ec == std::errc{} && parsed.ptr == end && warmRepetitions >= 1 && warmRepetitions <= 100,
                "warm repetitions must be an integer from 1 to 100");
        }
        const fs::path output = argv[1];
        require(!fs::exists(output), "output directory must not already exist");
        fixture(output);
        Payload rows = Payload::array();
        for (const auto width : {Precision::Float32, Precision::Float64}) {
            for (const bool planned : {false, true}) {
                for (const int members : {1, 8}) {
                    for (const bool prime : {false, true}) {
                        const auto copy = output / (std::to_string(static_cast<int>(width)) + (planned ? "-planned-" : "-raw-") +
                            std::to_string(members) + (prime ? "-primed.adi" : "-fresh.adi"));
                        fs::copy_file(output / "fixture.adi", copy);
                        Loader loader;
                        std::string error;
                        auto document = ui::ProjectDocument::open(copy, false, loader.function(), error);
                        require(bool(document), error);
                        document->prepare(rate, frames);
                        require(document->session().setPrecision(width), document->session().error());
                        require(loader.calls == 100, "fixture did not load 100 devices");
                        const int repetitions = prime ? warmRepetitions : 1;
                        for (int iteration = 0; iteration < repetitions; ++iteration) {
                            auto row = measure(*document, loader, width, planned, members, prime);
                            row["iteration"] = iteration;
                            rows.push_back(std::move(row));
                        }
                        document->release();
                        std::ofstream partial(output / "measurements.partial.json");
                        partial << rows.dump(2) << '\n';
                        require(bool(partial), "write measurement checkpoint");
                        std::printf("%d-bit %s %d-member %s: %d samples complete\n",
                            static_cast<int>(width), planned ? "prepared" : "raw", members,
                            prime ? "primed" : "fresh", repetitions);
                        std::fflush(stdout);
                    }
                }
            }
        }
        Payload report = {{"sample_rate", rate}, {"block_frames", frames}, {"material_tracks", 100},
            {"main_tracks", 1}, {"initial_clips", 200}, {"devices", 100},
            {"fixed_audio_tracks", 34}, {"warp_re_pitch_tracks", 33}, {"midi_tracks", 33},
            {"source_frames", 96000}, {"source_rate", 48000}, {"warm_repetitions", warmRepetitions},
            {"fresh_repetitions", 1}, {"measurements", rows},
            {"limits", {"Synthetic lightweight native probes, not real VST3/CLAP processing.",
                "Actual headless ProjectDocument and OpSubmitter; hardware driver and window drop NOT RUN.",
                "Fresh document means unprimed source pages; OS-cold filesystem cache NOT RUN.",
                "Audible means nonzero aggregate output after graph adoption, not every source becoming ready.",
                "View duration includes submitter result handling; owner phase includes validation and allocations.",
                "Raw move does not clear destinations; prepared move removes one covered victim per moved clip.",
                "Clock reads occur around process in the diagnostic driver, outside CallbackScope.",
                "Allocation counters cover ordinary new/new[]; aligned and direct C allocations are not counted.",
                "No latency threshold or Live comparison is claimed."}}};
        std::ofstream file(output / "measurements.json"); file << report.dump(2) << '\n';
        require(bool(file), "write measurements");
        std::printf("Wrote %zu measurements to %s\n", rows.size(), (output / "measurements.json").string().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "measurement failed: %s\n", error.what()); return 1;
    }
}
