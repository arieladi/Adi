// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/digest.hpp"
#include "adi/engine/mpe_output.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/interchange/midi_clip_file.hpp"
#include "adi/pitch_midi.hpp"
#include "adi/pitch_midi_smf.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace adi;
using namespace adi::clip_edit;
using namespace adi::engine;

constexpr std::int32_t blockFrames = 64;
constexpr std::int64_t sourceClip = 11, midiTrack = 2;
constexpr std::uint16_t center = 8192, upQuarter = 9216, downQuarter = 7168, upSemitone = 12288;
int checks = 0, failures = 0;

void check(bool passed, const char* description) {
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

struct Heard {
    std::int64_t sample = 0;
    EventType type = EventType::NoteOn;
    std::uint8_t channel = 0;
    std::uint16_t dimension = 0;
    std::uint32_t address = 0;
    double value = 0;
    std::uint16_t word = 0;
    bool operator==(const Heard&) const = default;
};

bool isWheel(const Heard& event) noexcept {
    return event.type == EventType::Controller && event.address == controllers::kPitchBend;
}

// Like the authored controller fixture in test_midi_clips, consume real
// routed events from Session and keep the trace in fixed callback storage.
struct Probe final : device::DeviceInstance {
    device::DeviceIdentity deviceId;
    ExpressionCaps caps;
    std::array<Heard, 256> trace{};
    std::size_t used = 0;
    std::int64_t blockStart = 0;
    bool overflow = false;

    const device::DeviceIdentity& identity() const noexcept override { return deviceId; }
    bool loaded() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }
    std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
    void process(const NodeIo& io) noexcept override {
        for (const auto& event : io.events) {
            if (!isNoteStream(event.type)) continue;
            if (used == trace.size()) {
                overflow = true;
                break;
            }
            auto& entry = trace[used++];
            entry = {blockStart + event.frame, event.type, event.channel, event.dim, event.paramId, event.value, 0};
            if (isWheel(entry)) {
                MpeOut wire;
                if (noteExpressionOut(event, caps, wire)) entry.word = wire.word;
            }
        }
        for (std::int32_t channel = 0; channel < io.channels; ++channel)
            std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.0f);
    }
};

std::vector<Heard> play(Store& store, std::int64_t start, std::int64_t end) {
    Session session;
    session.graph().setFadeFrames(0);
    Probe* probe = nullptr;
    const DeviceLoader loader = [&probe](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
        auto recorder = std::make_unique<Probe>();
        probe = recorder.get();
        return recorder;
    };
    SessionSpec spec;
    spec.maxFrames = blockFrames;
    spec.performanceState = false;
    if (!session.load(store, loader, spec) || !probe) throw std::runtime_error(session.error());
    session.transport().locate(start);
    session.transport().play();
    std::array<float, blockFrames> left{}, right{};
    float* output[]{left.data(), right.data()};
    AudioIo io;
    io.out = output;
    io.numOut = 2;
    for (auto at = start; at < end;) {
        io.frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockFrames, end - at));
        io.streamTimeSamples = at - start;
        probe->blockStart = at;
        {
            const audio::CallbackScope callback;
            session.process(io);
        }
        at += io.frames;
    }
    if (probe->overflow) throw std::runtime_error("native wheel probe overflow");
    return {probe->trace.data(), probe->trace.data() + probe->used};
}

struct Fixture {
    test::TempDirectory directory{"pitch_native_bend", "placed-extraction"};
    std::unique_ptr<Store> store;
    media::ClipPitchAnalysis analysis{sourceClip, {std::string(64, 'a'), 48000, 1, 800}, 0, 800, false, 1, 0,
        {48000, 800, {{100, 300, 6925, 0, 0, {{100, 6925}, {200, 7000}, {299, 6925}}},
                      {300, 800, 7075, 0, 0, {{300, 7075}, {799, 7075}}}}}};

    Fixture() {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "song.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        // The extraction consumes the captured analysis and source identity.
        // Muting the source avoids needing disk pages for the MIDI device probe.
        store->db().exec(
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
            "(1,'audio','Voice',0),(2,'midi','Wheel',1),(9,'master','Main',2);"
            "INSERT INTO media_files(id,hash_blake3,sample_rate,channels,frames) VALUES"
            "(1,'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',48000,1,800);"
            "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks,muted) VALUES"
            "(11,1,'audio','Phrase',0,192192,1);"
            "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(11,1,800);"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.pitch-wheel','Wheel Probe','instrument');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,2);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Wheel Probe')");
    }

    void reopen() {
        store.reset();
        StoreError error{};
        store = Store::open(directory.path() / "song.adi", error);
        if (!store) throw std::runtime_error(toString(error));
    }
};

bool hasWheel(const std::vector<Heard>& trace, std::int64_t sample, std::uint16_t word) {
    return std::any_of(trace.begin(), trace.end(), [&](const Heard& event) {
        return isWheel(event) && event.channel == 0 && event.sample == sample && event.word == word;
    });
}

void checkFileImport(const Fixture& fixture, const PitchMidiProjection& projection, PitchMidiBendMode mode) {
    const auto encoded = encodePitchMidiChannelBendSmf(projection, mode, 2, {960, 500000});
    if (!encoded) throw std::runtime_error(encoded.detail.value_or("encode native pitch extraction as SMF"));
    const auto path = fixture.directory.path() / "extracted.mid";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(encoded.data->data()), static_cast<std::streamsize>(encoded.data->size()));
        if (!file) throw std::runtime_error("write native pitch extraction fixture");
    }
    const auto imported = interchange::readMidiClipFile(path);
    check(imported.error.empty() && imported.tracks.size() == 1 && imported.tracks.front().notes.size() == 2,
          "the ordinary SMF importer reads both extracted notes");
    if (imported.tracks.size() != 1) return;
    const auto& lanes = imported.tracks.front().controllers;
    const auto wheel = lanes.find({0, controllers::kPitchBend});
    const auto containsWord = [&lanes, &wheel](std::uint16_t expected, std::int64_t time) {
        return wheel != lanes.end() && std::any_of(wheel->second.begin(), wheel->second.end(), [&](const auto& point) {
            return point.time == time && std::lround(controllers::pitchBendWordExact(point.value)) == expected;
        });
    };
    check(containsWord(upQuarter, projection.notes[0].start_ticks) &&
          containsWord(downQuarter, projection.notes[1].start_ticks) && containsWord(center, projection.lengthTicks),
          "SMF import preserves both quantized wheel attacks and the file's final center");
}

void placedExtraction(PitchMidiBendMode mode) {
    Fixture fixture;
    auto& store = *fixture.store;
    check(!store.db().tableExists("clip_pitch_extractions"), "native pitch wheel needs only released 1.23 tables");
    const auto before = digestProject(store).text;
    PitchMidiOptions options;
    options.destinationTrack = midiTrack;
    options.channelBend = mode;
    const auto prepared = preparePitchMidi(store, fixture.analysis, options);
    if (!prepared) throw std::runtime_error(prepared.detail.value_or("prepare native pitch wheel extraction"));
    const auto clip = prepared.data->midiClip;
    const auto committed = commitPitchMidi(store, *prepared.data);
    if (!committed.ok) throw std::runtime_error(committed.error);
    const auto after = digestProject(store).text;

    const auto lane = controllers::readLanes(store, clip);
    check(lane.ok && lane.lanes.size() == 1 && lane.lanes.front().channel == 0 &&
          lane.lanes.front().controller == controllers::kPitchBend && lane.lanes.front().error.empty(),
          "native ACTL reader resolves one channel-zero wheel");
    if (!lane.ok || lane.lanes.size() != 1) return;
    const auto rawWheel = lane.lanes.front().data;
    {
        SQLite::Statement query(store.db(), "SELECT channel, controller FROM event_streams WHERE clip_id=? AND stream_kind='pitchbend'");
        query.bind(1, clip);
        check(query.executeStep() && query.getColumn(0).getInt() == 0 && query.getColumn(1).isNull() && !query.executeStep(),
              "SQL uses pitchbend kind with a NULL controller column");
        SQLite::Statement count(store.db(), "SELECT count(*) FROM note_expression WHERE clip_id=? AND dimension=0");
        count.bind(1, clip);
        check(count.executeStep() && count.getColumn(0).getInt() == 0, "native wheel owns the pitch without per-note Pitch AEXP");
    }
    const auto bytes = store.getEventStream(clip, "notes");
    if (!bytes) throw std::runtime_error("read extracted note stream");
    const StreamReader<NoteRecord> notes(*bytes, FourCC::Notes);
    check(notes.ok() && notes.count() == 2 && notes.at(0)->key == 69 && notes.at(1)->key == 71 &&
          notes.at(0)->tuning_cents == 0 && notes.at(1)->tuning_cents == 0,
          "both MIDI keys keep zero ANOT tuning when the channel wheel supplies the offsets");
    const auto& points = lane.lanes.front().points;
    check(!points.empty() && std::all_of(points.begin(), points.end(), [&](const controllers::Point& point) {
              return point.curve == static_cast<std::uint8_t>(Curve::Hold) && point.timeTicks >= 0 &&
                     point.timeTicks < prepared.data->projection.lengthTicks;
          }) && std::adjacent_find(points.begin(), points.end(), [](const auto& a, const auto& b) {
              return a.timeTicks >= b.timeTicks;
          }) == points.end(), "placed curve has unique sorted Hold points inside the clip's half-open extent");

    const auto trace = play(store, 0, 864);
    check(hasWheel(trace, 100, upQuarter) && hasWheel(trace, 300, downQuarter) && hasWheel(trace, 800, center),
          "Session delivers both attack bends and centers its wheel when the lane owner ends");
    if (mode == PitchMidiBendMode::Continuous)
        check(hasWheel(trace, 200, upSemitone), "continuous extraction delivers its contour knot at the projected sample");
    else
        check(std::none_of(trace.begin(), trace.end(), [](const Heard& event) {
                  return isWheel(event) && event.sample > 100 && event.sample < 300;
              }), "static extraction keeps its note's wheel held between boundaries");
    std::vector<EventType> handoff;
    for (const auto& event : trace)
        if (event.sample == 300 && (isWheel(event) || event.type == EventType::NoteOn || event.type == EventType::NoteOff))
            handoff.push_back(event.type);
    check(handoff == std::vector{EventType::NoteOff, EventType::Controller, EventType::NoteOn},
          "the old note releases before the incoming wheel and note at the same sample");
    const auto chased = play(store, 400, 464);
    check(hasWheel(chased, 400, downQuarter) && std::any_of(chased.begin(), chased.end(), [](const Heard& event) {
              return event.sample == 400 && event.type == EventType::NoteOn;
          }), "starting inside the second note chases both its wheel and note");
    checkFileImport(fixture, prepared.data->projection, mode);

    check(History(store).undo().ok && digestProject(store).text == before &&
          controllers::readLanes(store, clip).lanes.empty() && play(store, 0, 864).empty(),
          "one Undo clears the extraction's wheel, notes and played events");
    fixture.reopen();
    auto& reopened = *fixture.store;
    if (!History(reopened).redo().ok) throw std::runtime_error("redo native pitch wheel extraction");
    const auto restored = controllers::readLanes(reopened, clip);
    check(digestProject(reopened).text == after && restored.ok && restored.lanes.size() == 1 &&
          restored.lanes.front().data == rawWheel && play(reopened, 0, 864) == trace,
          "Redo after reopen restores exact ACTL bytes and the scheduled event trace");
}

void rejectsOversizedBend() {
    Fixture fixture;
    const auto before = digestProject(*fixture.store).text;
    const auto head = OpJournal(*fixture.store).headSeq();
    fixture.analysis.analysis.notes.front().curve[1].cents = 7210;
    PitchMidiOptions options;
    options.destinationTrack = midiTrack;
    options.channelBend = PitchMidiBendMode::Continuous;
    const auto rejected = preparePitchMidi(*fixture.store, fixture.analysis, options);
    check(!rejected && rejected.problem == PitchMidiProblem::Edits && digestProject(*fixture.store).text == before &&
              OpJournal(*fixture.store).headSeq() == head, "an out-of-range native contour refuses before recording any edit");
}
} // namespace

int main() {
    try {
        placedExtraction(PitchMidiBendMode::Static);
        placedExtraction(PitchMidiBendMode::Continuous);
        rejectsOversizedBend();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::printf("%d native pitch bend checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
