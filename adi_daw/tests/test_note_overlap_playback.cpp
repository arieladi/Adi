// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/blob.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/note_overlap.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace { std::atomic<unsigned> callbackAllocations{0}; }
// The same platform-neutral callback probe as the native MIDI clip suite.
void* operator new(std::size_t size) {
    if (adi::audio::onAudioThread) callbackAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return operator new[](size); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
using namespace adi;
using namespace adi::engine;
int checks = 0, failures = 0;
std::string context;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL [%s] %s\n", context.c_str(), message); }
}
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

// All coordinates below are sample-frame equivalents of exact 100-frame
// units: 24024 ticks at 48 kHz, 120 BPM, PPQ 5765760. The oracle does not call
// the overlap resolver or read materialized notes to derive expected events.
std::int64_t ticks(std::int64_t frame) {
    require(frame >= -200 && frame <= 1600 && frame % 100 == 0, "fixture tick bounds");
    return frame / 100 * 24024;
}
NoteRecord note(std::uint64_t id, std::int64_t on, std::int64_t off,
                std::uint8_t key, std::uint8_t channel, float cents) {
    require(off > on, "fixture positive note interval");
    NoteRecord n{};
    n.note_id = id; n.start_ticks = ticks(on); n.dur_ticks = ticks(off - on);
    n.key = key; n.channel = channel; n.tuning_cents = cents;
    n.vel_on = 93; n.vel_off = 37; n.probability = 10000;
    return n;
}
notes::Record rawRecord(const NoteRecord& value) {
    notes::Record result;
    result.bytes.resize(sizeof value);
    std::memcpy(result.bytes.data(), &value, sizeof value);
    return result;
}
std::vector<std::byte> envelope(const std::vector<NoteRecord>& records) {
    return writeStream<NoteRecord>(FourCC::Notes, records);
}
Payload binary(const std::vector<std::byte>& bytes) {
    std::vector<std::uint8_t> copy(bytes.size());
    if (!bytes.empty()) std::memcpy(copy.data(), bytes.data(), bytes.size());
    return Payload::binary(std::move(copy));
}
// Full canonical row, independent of the mutation capture. All fixture columns
// are STRICT integer/text/null/blob; retaining this row also checks its ID and
// canonical address, not just ANOT data.
Payload streamRow(Store& store) {
    SQLite::Statement q(store.db(), "SELECT * FROM event_streams WHERE clip_id=1 AND "
                                  "stream_kind='notes' AND channel IS NULL AND controller IS NULL");
    require(q.executeStep(), "canonical notes row exists");
    Payload row = Payload::object();
    for (int column = 0; column < q.getColumnCount(); ++column) {
        const auto value = q.getColumn(column);
        const std::string name = q.getColumnName(column);
        switch (value.getType()) {
            case SQLITE_INTEGER: row[name] = value.getInt64(); break;
            case SQLITE_TEXT: row[name] = value.getString(); break;
            case SQLITE_NULL: row[name] = nullptr; break;
            case SQLITE_BLOB: {
                std::vector<std::uint8_t> bytes;
                if (value.getBytes() > 0) {
                    const auto* begin = static_cast<const std::uint8_t*>(value.getBlob());
                    require(begin != nullptr, "nonempty SQL BLOB pointer");
                    bytes.assign(begin, begin + value.getBytes());
                }
                row[name] = Payload::binary(std::move(bytes));
                break;
            }
            default: throw std::runtime_error("unexpected fixture SQL storage class");
        }
    }
    require(!q.executeStep(), "one canonical notes row");
    return row;
}
std::int64_t opCount(Store& store) {
    SQLite::Statement q(store.db(), "SELECT COUNT(*) FROM ops");
    require(q.executeStep(), "journal count row");
    return q.getColumn(0).getInt64();
}

struct Voice {
    std::int64_t on, off;
    std::uint16_t key;
    std::uint8_t channel;
    double pitch;
};
Voice v(std::int64_t on, std::int64_t off) { return {on, off, 60, 2, -0.25}; }
Voice i(std::int64_t on, std::int64_t off) { return {on, off, 60, 9, 0.5}; }
Voice u(std::int64_t on, std::int64_t off) { return {on, off, 67, 3, 0}; }
enum class Family { ShortenHeld, RemoveHeld, LoopPrefix, LoopOffset };
struct Case {
    const char* name = "";
    std::vector<NoteRecord> original, final;
    notes::Diff intent;
    std::vector<Voice> live, undo, redo;
    std::uint64_t victim = 0;
    notes::OverlapAction action = notes::OverlapAction::ShortenEnd;
    std::int64_t editBarrier = 0;
    bool loop = false;
    std::int64_t sourceOffset = 0;
};
Case example(Family family) {
    Case c;
    if (family == Family::ShortenHeld) {
        c.name = "shorten held"; c.victim = 101; c.editBarrier = 300;
        const auto oldV = note(101, -100, 800, 60, 2, -25);
        const auto oldI = note(202, 1000, 1200, 60, 9, 50);
        const auto untouched = note(303, 400, 800, 67, 3, 0);
        const auto newV = note(101, -100, 400, 60, 2, -25);
        const auto newI = note(202, 400, 600, 60, 9, 50);
        c.original = {oldV, untouched, oldI};
        c.final = {newV, untouched, newI}; // retained ordinal at equal starts
        c.intent.changed.push_back({rawRecord(oldI), rawRecord(newI)});
        // U starts after the refresh. This does not require a retrigger of an
        // unrelated held voice that the pending Link consumer may preserve.
        c.live = {v(100, 300), v(300, 500), i(500, 700), u(500, 900)};
        c.undo = {v(100, 900), u(500, 900), i(1100, 1300)};
        c.redo = {v(100, 500), u(500, 900), i(500, 700)};
    } else if (family == Family::RemoveHeld) {
        c.name = "remove held"; c.victim = 401; c.editBarrier = 200;
        c.action = notes::OverlapAction::Remove;
        const auto oldV = note(401, -100, 800, 60, 2, -25);
        const auto newI = note(402, -200, 200, 60, 9, 50);
        c.original = {oldV}; c.final = {newI};
        c.intent.added.push_back(rawRecord(newI));
        c.live = {v(100, 200), i(200, 300)};
        c.undo = {v(100, 900)}; c.redo = {i(100, 300)};
    } else {
        c.loop = true; c.victim = 501;
        const auto oldV = note(501, -100, 300, 60, 2, -25);
        const auto oldI = note(502, 500, 600, 60, 9, 50);
        const auto newV = note(501, -100, 200, 60, 2, -25);
        const auto newI = note(502, 200, 300, 60, 9, 50);
        c.original = {oldV, oldI}; c.final = {newV, newI};
        c.intent.changed.push_back({rawRecord(oldI), rawRecord(newI)});
        if (family == Family::LoopPrefix) {
            c.name = "loop prefix";
            c.live = {v(100, 300), i(300, 400), v(500, 600), i(600, 700),
                      v(800, 900), i(900, 1000)};
            c.undo = {v(100, 400), v(500, 700), v(800, 1000)};
        } else {
            c.name = "loop offset at shortened end"; c.sourceOffset = 200;
            c.live = {i(100, 200), v(300, 400), i(400, 500), v(600, 700),
                      i(700, 800), v(900, 1000), i(1000, 1100)};
            c.undo = {v(100, 200), v(300, 500), v(600, 800), v(900, 1100)};
        }
        c.redo = c.live;
    }
    return c;
}

struct Heard { Event event; std::int64_t clock = 0; };
struct Recorder final : device::DeviceInstance {
    device::DeviceIdentity id;
    std::array<Heard, 128> heard{};
    std::size_t count = 0;
    std::int64_t clock = 0;
    Precision expected = Precision::Float32;
    unsigned calls = 0;
    bool overflow = false, wrongWidth = false, wrongFrame = false;
    const device::DeviceIdentity& identity() const noexcept override { return id; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }
    std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
    void process(const NodeIo& io) noexcept override {
        ++calls;
        wrongWidth = wrongWidth || io.wide() != (expected == Precision::Float64);
        for (const auto& event : io.events) {
            wrongFrame = wrongFrame || event.frame < io.blockOffset ||
                         event.frame >= io.blockOffset + io.frames;
            if (count < heard.size()) heard[count++] = {event, clock + event.frame};
            else overflow = true;
        }
        for (int channel = 0; channel < io.channels; ++channel) {
            if (io.wide()) std::fill_n(io.out64[channel] + io.blockOffset, io.frames, 0.0);
            else std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.0f);
        }
    }
    void reset() noexcept {
        count = 0; calls = 0; overflow = false; wrongWidth = false; wrongFrame = false;
    }
};

struct Fixture {
    test::TempDirectory directory{"note_overlap_playback", "session"};
    std::unique_ptr<Store> store;
    Session session;
    Recorder* recorder = nullptr;
    Precision precision;
    bool irregular;
    unsigned loads = 0;
    std::int64_t clock = 0, phaseOrigin = 0;
    std::size_t partition = 0;
    unsigned phaseAllocations = 0;
    std::uint32_t phaseIo = 0;
    Payload originalRow, finalRow;
    std::optional<std::int64_t> originalHead, finalHead;
    explicit Fixture(const Case& c, Precision width, bool varied) : precision(width), irregular(varied) {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "session.adi", error);
        require(store != nullptr, "create overlap playback project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
                         "(1,'midi','Notes',0),(9,'master','Master',1);"
                         "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES"
                         "(1,'vst3','test.overlap','Native event recorder','instrument');"
                         "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
                         "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES"
                         "(1,1,0,1,'Native event recorder')");
        SQLite::Statement clip(store->db(), "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,"
                                           "length_ticks,loop_enabled,loop_start_ticks,loop_len_ticks,"
                                           "content_offset_ticks) VALUES(1,1,'midi',0,?,?,?,?,?,?)");
        clip.bind(1, ticks(100)); clip.bind(2, ticks(c.loop ? 1000 : 1400));
        clip.bind(3, c.loop ? 1 : 0); clip.bind(4, ticks(c.loop ? 100 : 0));
        clip.bind(5, ticks(c.loop ? 300 : 0)); clip.bind(6, ticks(c.sourceOffset)); clip.exec();
        require(store->putEventStream(1, "notes", envelope(c.original)), "write original ANOT");
        originalRow = streamRow(*store); finalRow = originalRow;
        finalRow["data"] = binary(envelope(c.final));
        originalHead = History(*store).head();
        require(opCount(*store) == 0 && !originalHead, "fixture starts with empty journal");
        SessionSpec spec;
        spec.channels = 2; spec.sampleRate = 48000; spec.maxFrames = 256; spec.precision = precision;
        session.graph().setFadeFrames(0);
        const bool loaded = session.load(*store, [this](const DeviceRequest&, std::string&)
                                         -> std::unique_ptr<device::DeviceInstance> {
            ++loads;
            auto instance = std::make_unique<Recorder>();
            instance->expected = precision; recorder = instance.get(); return instance;
        }, spec);
        require(loaded, session.error());
        require(recorder && session.midiClips(), "native recorder and MIDI source exist");
        check(session.midiClips()->problems().empty(), "original signed notes load without MIDI problems");
    }
    void commit(const Case& c) {
        const auto snapshot = notes::captureNotes(*store, 1);
        require(snapshot.ok, snapshot.error);
        const auto prepared = notes::prepareOverlapDiff(snapshot, c.intent);
        require(prepared.ok(), prepared.error());
        check(prepared.status() == notes::OverlapStatus::Ready && prepared.requests().size() == 1,
              "the real overlap producer makes one ready request");
        const auto sideEffects = prepared.sideEffects();
        check(sideEffects.size() == 1 && sideEffects.front().victim == c.victim &&
                  sideEffects.front().action == c.action,
              "key-only overlap induces the expected source-ID side effect across channel and cents");
        check(streamRow(*store) == originalRow && opCount(*store) == 0 &&
                  History(*store).head() == originalHead,
              "capture and preparation leave material and journal untouched");
        const auto result = notes::commitOverlapDiff(*store, prepared);
        require(result.ok, result.error);
        finalHead = History(*store).head();
        check(result.seqs.size() == 1 && finalHead && *finalHead == result.seqs.front() &&
                  opCount(*store) == 1,
              "one prepared overlap commits one journal entry");
        SQLite::Statement q(store->db(), "SELECT op_type FROM ops");
        require(q.executeStep(), "committed journal row exists");
        check(q.getColumn(0).getString() == "note.applyDiff" && !q.executeStep(),
              "native playback follows the actual note.applyDiff operation");
        check(streamRow(*store) == finalRow,
              "exact final ANOT and row retain survivor IDs and fields without a right fragment");
    }
    void refresh() {
        require(session.refresh(*store), session.error());
        check(loads == 1 && session.midiClips() && session.midiClips()->problems().empty(),
              "refresh retains the real native recorder and admits the new MIDI material");
    }
    void start() {
        recorder->reset(); phaseOrigin = clock; partition = 0;
        phaseAllocations = callbackAllocations.load(); phaseIo = audio::callbackFileIo.load();
        session.transport().locate(0); session.transport().play();
    }
    void callback(std::int32_t frames) {
        require(frames > 0 && frames <= 256, "bounded callback size");
        std::array<float, 512> floats{};
        std::array<double, 512> doubles{};
        float* out[] = {floats.data(), floats.data() + 256};
        double* out64[] = {doubles.data(), doubles.data() + 256};
        recorder->clock = clock;
        AudioIo io;
        io.out = out; io.numOut = 2; io.frames = frames; io.streamTimeSamples = clock;
        if (precision == Precision::Float64) io.out64 = out64;
        session.process(io); clock += frames;
    }
    void until(std::int64_t end) {
        constexpr std::array<std::int32_t, 7> sizes{{1, 73, 26, 211, 5, 128, 13}};
        while (clock - phaseOrigin < end) {
            const auto candidate = irregular ? sizes[partition++ % sizes.size()] : 100;
            const auto remaining = end - (clock - phaseOrigin);
            callback(static_cast<std::int32_t>(std::min<std::int64_t>(candidate, remaining)));
        }
    }
    void stop() {
        session.transport().play(false);
        callback(100); // Any unexpectedly hanging voice remains visible to the oracle.
    }
    void verify(std::span<const Voice> expected) {
        const auto beforeFailures = failures;
        check(callbackAllocations.load() == phaseAllocations && audio::callbackFileIo.load() == phaseIo,
              "all callbacks, including edit adoption and stop, allocate and perform file I/O zero times");
        check(recorder->calls > 0 && !recorder->wrongWidth && !recorder->wrongFrame && !recorder->overflow,
              "the native consuming recorder receives the requested width and bounded segment events");
        check(session.midiClips() && session.midiClips()->droppedNotes() == 0,
              "the actual source refuses no notes");
        std::size_t expectedCount = 0;
        for (const auto& voice : expected) expectedCount += voice.pitch == 0 ? 2u : 3u;
        check(recorder->count == expectedCount, "exact event count includes every on, pitch and off once");
        std::array<bool, 128> matched{};
        bool paired = true;
        for (const auto& voice : expected) {
            std::optional<std::size_t> on, off, pitch;
            for (std::size_t n = 0; n < recorder->count; ++n) {
                const auto& h = recorder->heard[n];
                if (!matched[n] && h.event.type == EventType::NoteOn &&
                    h.clock - phaseOrigin == voice.on && h.event.dim == voice.key &&
                    h.event.channel == voice.channel && h.event.value == 93.0 / 127) {
                    on = n; matched[n] = true; break;
                }
            }
            if (on) {
                const auto id = recorder->heard[*on].event.noteId;
                paired = paired && id != 0;
                for (std::size_t n = 0; n < recorder->count; ++n) {
                    const auto& h = recorder->heard[n];
                    if (matched[n] || h.event.noteId != id || h.event.channel != voice.channel) continue;
                    if (!off && h.event.type == EventType::NoteOff && h.event.dim == voice.key &&
                        h.clock - phaseOrigin == voice.off && h.event.value == 37.0 / 127) {
                        off = n; matched[n] = true;
                    } else if (voice.pitch != 0 && !pitch && h.event.type == EventType::NoteExpression &&
                               h.event.dim == static_cast<std::uint16_t>(ExpressionDim::Pitch) &&
                               h.clock - phaseOrigin == voice.on && h.event.value == voice.pitch) {
                        pitch = n; matched[n] = true;
                    }
                }
            }
            paired = paired && on && off;
            if (on && off) paired = paired && *on < *off;
            if (voice.pitch != 0) {
                paired = paired && pitch;
                if (on && pitch && off) paired = paired && *on < *pitch && *pitch < *off;
            }
        }
        for (std::size_t n = 0; n < recorder->count; ++n) paired = paired && matched[n];
        check(paired, "literal intervals, channels, velocities and static cents pair with the same runtime voice");
        bool ordered = true, unique = true;
        for (std::size_t a = 0; a < recorder->count; ++a) {
            const auto& first = recorder->heard[a];
            if (a > 0) ordered = ordered && recorder->heard[a - 1].clock <= first.clock;
            if (first.event.type != EventType::NoteOn) continue;
            for (std::size_t b = a + 1; b < recorder->count; ++b) {
                const auto& later = recorder->heard[b];
                if (later.event.type == EventType::NoteOn)
                    unique = unique && later.event.noteId != first.event.noteId;
                if (later.clock == first.clock && later.event.type == EventType::NoteOff &&
                    later.event.dim == first.event.dim) ordered = false;
            }
        }
        check(ordered, "events are chronological and a shared-key endpoint releases before its next onset");
        check(unique, "separate chases and loop occurrences have distinct runtime voice associations");
        if (failures != beforeFailures) {
            for (std::size_t n = 0; n < recorder->count; ++n) {
                const auto& h = recorder->heard[n];
                std::printf("  t=%lld type=%u dim=%u channel=%u voice=%llu value=%.12g\n",
                            static_cast<long long>(h.clock - phaseOrigin),
                            static_cast<unsigned>(h.event.type), static_cast<unsigned>(h.event.dim),
                            static_cast<unsigned>(h.event.channel),
                            static_cast<unsigned long long>(h.event.noteId), h.event.value);
            }
        }
    }
};

void exercise(Family family, Precision precision, bool irregular) {
    const auto c = example(family);
    const std::string label = std::string(c.name) + (precision == Precision::Float64 ? " Float64" : " Float32") +
                              (irregular ? " irregular" : " regular");
    context = label + " load";
    Fixture f(c, precision, irregular);
    context = label + " live commit";
    if (c.editBarrier > 0) { f.start(); f.until(c.editBarrier); }
    f.commit(c); f.refresh();
    if (c.editBarrier == 0) f.start();
    f.until(1600); f.stop(); f.verify(c.live);

    context = label + " undo";
    const auto undone = History(*f.store).undo();
    require(undone.ok, undone.error);
    check(undone.opsApplied == 1 && opCount(*f.store) == 1 &&
              History(*f.store).head() == f.originalHead && streamRow(*f.store) == f.originalRow,
          "one Undo restores exact original source IDs, bytes and row without another submission");
    f.refresh(); f.start(); f.until(1600); f.stop(); f.verify(c.undo);

    context = label + " redo";
    const auto redone = History(*f.store).redo();
    require(redone.ok, redone.error);
    check(redone.opsApplied == 1 && opCount(*f.store) == 1 &&
              History(*f.store).head() == f.finalHead && streamRow(*f.store) == f.finalRow,
          "one Redo restores exact effective source IDs, bytes and row without another submission");
    f.refresh(); f.start(); f.until(1600); f.stop(); f.verify(c.redo);
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned fixtures = 0;
    for (auto precision : {Precision::Float32, Precision::Float64}) {
        for (bool irregular : {false, true}) {
            for (auto family : {Family::ShortenHeld, Family::RemoveHeld, Family::LoopPrefix, Family::LoopOffset}) {
                ++fixtures;
                try { exercise(family, precision, irregular); }
                catch (const std::exception& e) { check(false, e.what()); }
            }
        }
    }
    context = "all fixtures";
    check(callbackAllocations.load() == 0, "no callback allocations across all native overlap phases");
    check(audio::callbackFileIo.load() == 0, "no callback file I/O across all native overlap phases");
    std::printf("%s -- %u Session fixtures, %d checks, %d failure(s)\n",
                failures ? "FAIL" : "PASS", fixtures, checks, failures);
    return failures ? 1 : 0;
}
