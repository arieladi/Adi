// SPDX-License-Identifier: AGPL-3.0-or-later
//
// WTS-01 (ADR-0248 d2, d12; PLAN row WTS-01): one usable session from nothing on Windows, the seven
// steps, headless. Each step drives the calls the app makes for the user's action (`ProjectDocument`,
// the window's edit builders, the mailbox) against a simulated audio driver; no window opens. The
// VST3 parts of steps 3 and 7 need JUCE and run in `test_session_wts01_vst3.cpp`.
//
// Prints one line per step (see `wts01::Report`) and exits non-zero only on a FAIL.
#include "wts01_harness.hpp"
#include "../temp_directory.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/ui/automation_mode.hpp"
#include "adi/ui/export_choices.hpp"
#include "adi/ui/send_edits.hpp"
#include "juce/builtin_host.hpp"
#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>
using namespace adi;
using namespace adi::wts01;
using V = Report::Verdict;
namespace {
std::int64_t nowUtc() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
// The timeline sample of a tick on the 120 BPM map, and back.
std::int64_t tickAtSample(std::size_t sample) {
    return static_cast<std::int64_t>(sample) * kBarTicks / kBarSamples;
}
Stereo readWav(const std::filesystem::path &file) {
    Stereo out;
    audio::WavReader reader(file);
    const auto frames = static_cast<std::size_t>(reader.frames());
    const auto channels = static_cast<std::size_t>(reader.channels());
    std::vector<float> interleaved(frames * channels);
    std::size_t done = 0;
    while (done < frames) {
        const auto got = reader.read(interleaved.data() + done * channels,
                                     static_cast<std::uint32_t>(std::min<std::size_t>(frames - done, 65536)));
        if (got == 0)
            break;
        done += got;
    }
    for (std::size_t c = 0; c < 2; ++c) {
        out.c[c].resize(done);
        for (std::size_t i = 0; i < done; ++i)
            out.c[c][i] = interleaved[i * channels + std::min(c, channels - 1)];
    }
    return out;
}
void writeWav(const std::filesystem::path &file, const Stereo &s) {
    std::vector<float> interleaved(s.size() * 2);
    for (std::size_t i = 0; i < s.size(); ++i) {
        interleaved[2 * i] = s.c[0][i];
        interleaved[2 * i + 1] = s.c[1][i];
    }
    audio::WavWriter writer(file, static_cast<std::uint32_t>(kRate), 2, audio::WavFormat::Float32);
    writer.write(interleaved.data(), static_cast<std::uint32_t>(s.size()));
    writer.close();
}
std::string bytesOf(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
// Lets the take land: the driver keeps calling back and the timer runs, as in the app.
std::optional<ui::ProjectDocument::TakeLanded> awaitTake(ui::ProjectDocument &doc, Driver &driver) {
    std::optional<ui::ProjectDocument::TakeLanded> landed;
    for (int i = 0; i < 6000 && !landed; ++i) {
        driver.block();
        driver.frame();
        landed = doc.takeLanded();
        if (!landed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return landed;
}
// The fader's drag on the mixer (ui_mixer.cpp `MixerStrip::beginLive`): begun where the rows stand, moved,
// let go.
bool dragTo(ui::ProjectDocument &doc, const gesture::RelativeScalarSelection &sel, double value, const char *label) {
    return doc.beginMixerDrag(sel, std::nullopt, label) && doc.mixerDrag(value) && doc.endMixerDrag();
}
gesture::RelativeScalarSelection fader(std::int64_t track) {
    return {gesture::ScalarKind::TrackVolume, track, {}, {-70, 6, -150}}; // ui_mixer.cpp kFloorDb, kTopDb, kSilenceDb
}
struct Session {
    test::TempDirectory temp{"wts01", "session"};
    std::filesystem::path path = temp.path() / "WTS01 Session.adi";
    std::unique_ptr<ui::ProjectDocument> doc;
    std::optional<Driver> driver;
    Stereo take, strings;   // what track 4 recorded (x4) and what track 5 imported (x5)
    std::int64_t playOffset = 0; // the playback's measured delay against the take
    double returnGain = 1;   // the A Return's fader once step 5 has set it
    bool sendsOn = false;
};
std::size_t stepStart(std::size_t block) { return block * static_cast<std::size_t>(kFrames); }

// 1. A new project from nothing, run at 48 kHz.
bool step1(Session &s, Report &report) {
    ui::RecordingDefaults defaults; // the registry's Record defaults (`commands_->recordingDefaults()`)
    std::string error;
    s.doc = ui::ProjectDocument::open(s.path, true, device::withBuiltinDevices(), error, &defaults);
    if (!s.doc) {
        report.step("1", V::Fail, "open failed: " + error, "a New project");
        return false;
    }
    auto &doc = *s.doc;
    doc.prepare(kRate, kFrames); // the driver's prepare, at the device's rate
    s.driver.emplace(doc);
    const auto snap = doc.view().current();
    // By id: the snapshot lists them in the Arrangement's order, Main last.
    auto tracks = snap->tracks;
    std::sort(tracks.begin(), tracks.end(), [](const auto &a, const auto &b) { return a->id < b->id; });
    std::string kinds;
    for (const auto &t : tracks)
        kinds += std::to_string(t->id) + ":" + t->kind + " ";
    const auto bpm = real(doc, "SELECT bpm FROM tempo_map WHERE pos_ticks=0");
    const auto num = scalar(doc, "SELECT numerator FROM time_signature_map WHERE pos_ticks=0");
    const auto den = scalar(doc, "SELECT denominator FROM time_signature_map WHERE pos_ticks=0");
    const auto ops = scalar(doc, "SELECT count(*) FROM ops");
    const bool undo = doc.ops().canUndo();
    const auto engineRate = doc.session().spec().sampleRate;
    const auto projectRate = scalar(doc, "SELECT sample_rate FROM project");
    Stereo heard;
    s.driver->run(188, &heard); // one second
    const double loud = peak(heard, 0, heard.size());
    const bool layout = kinds == "1:master 2:midi 3:midi 4:audio 5:audio 6:return 7:return ";
    const bool ok = layout && bpm == 120 && num == 4 && den == 4 && ops == 0 && !undo && engineRate == kRate &&
                    finite(heard) && loud == 0 && !snap->transport.loopEnabled && snap->transport.countInBars == 0;
    report.check("1", ok,
                 format("tracks %s; %.0f BPM %lld/%lld; ops %lld, canUndo %d; engine %.0f Hz (project row %lld Hz); "
                        "1 s finite %d peak %.3g; loop %d, count-in %d",
                        kinds.c_str(), bpm, static_cast<long long>(num), static_cast<long long>(den),
                        static_cast<long long>(ops), undo ? 1 : 0, engineRate, static_cast<long long>(projectRate),
                        finite(heard) ? 1 : 0, loud, snap->transport.loopEnabled ? 1 : 0, snap->transport.countInBars),
                 "Main 1, MIDI 2-3, audio 4-5, returns 6-7; 120 BPM 4/4; 0 ops, no undo; engine 48000 Hz; silence; "
                 "loop off, count-in none");
    return true;
}

// 2. Four bars of audio recorded on an armed track from the device's input, stopped, played back.
void step2(Session &s, Report &report) {
    auto &doc = *s.doc;
    auto &driver = *s.driver;
    const auto snap = doc.view().current();
    auto arm = ui::armTracks(snap->tracks, doc.recordStates(), 4, {4}, true, false); // Exclusive Arm, as Live's
    const bool armed = arm.refusal.empty() && submitEdits(doc, std::move(arm.ops)) && doc.recordStates().at(4).armed;
    const auto monitor = doc.recordStates().at(4).monitor;
    driver.input = fedInput;
    const auto clipsBefore = scalar(doc, "SELECT count(*) FROM clips WHERE track_id=4");
    const auto startStream = driver.stream;
    // The window's Arrangement Record (ui_main.cpp `startRecording`): stopped, from the insert marker 0.
    const bool started = armed && doc.startRecording(0, 2, false);
    const auto startError = doc.error();
    if (started)
        driver.run(1500); // 1500 x 256 = 384000 frames = 4 bars at 120 BPM
    windowStop(doc);
    const auto landed = started ? awaitTake(doc, driver) : std::nullopt;
    const auto clips = scalar(doc, "SELECT count(*) FROM clips WHERE track_id=4") - clipsBefore;
    const auto undo = doc.ops().nextUndo();
    std::int64_t pos = -1, len = -1, srcFrames = 0;
    std::filesystem::path media;
    if (landed && landed->ok && landed->clips.size() == 1) {
        SQLite::Statement q(doc.store().db(),
                            "SELECT c.pos_ticks, c.length_ticks, m.rel_path, a.src_len_frames FROM clips c "
                            "JOIN audio_clips a ON a.clip_id=c.id JOIN media_files m ON m.id=a.media_id WHERE c.id=?");
        q.bind(1, landed->clips.front());
        if (q.executeStep()) {
            pos = q.getColumn(0).getInt64();
            len = q.getColumn(1).getInt64();
            media = media::projectFolder(doc.store()) / media::pathFromUtf8(q.getColumn(2).getString());
            srcFrames = q.getColumn(3).getInt64();
        }
    }
    double fedDiff = -1;
    std::int64_t fedOffset = 0;
    if (!media.empty()) {
        s.take = readWav(media);
        const auto compare = [&](std::int64_t offset) {
            double worst = 0;
            for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t i = 0; i < s.take.size(); ++i)
                    worst = std::max(worst, std::abs(static_cast<double>(s.take.c[c][i]) -
                                                     fedInput(c, startStream + offset + static_cast<std::int64_t>(i))));
            return worst;
        };
        fedDiff = compare(0);
        // Where it does not match at once, find where the take sits in what was fed.
        for (std::int64_t d = -4096; fedDiff != 0 && d <= 4096; ++d)
            if (s.take.size() > 64 && std::abs(s.take.c[0][0] - fedInput(0, startStream + d)) == 0 &&
                compare(d) == 0) {
                fedOffset = d;
                break;
            }
    }
    const bool recorded = landed && landed->ok && clips == 1 && undo.has_value() && pos == 0 &&
                          len == 4 * kBarTicks && srcFrames == 4 * kBarSamples && fedDiff == 0;
    report.check("2a", recorded,
                 format("armed %d (monitor %s); record %s; landed %s \"%s\"; clips +%lld; undo \"%s\" (%d ops); "
                        "pos %lld, length %lld ticks, %lld frames; take vs fed max|d| %.3g%s",
                        armed ? 1 : 0, ui::monitorName(monitor), started ? "started" : startError.c_str(),
                        landed ? (landed->ok ? "ok" : "refused") : "never", landed ? landed->message.c_str() : "",
                        static_cast<long long>(clips), undo ? undo->label.c_str() : "", undo ? undo->opCount : 0,
                        static_cast<long long>(pos), static_cast<long long>(len), static_cast<long long>(srcFrames),
                        fedDiff, fedDiff != 0 && fedOffset != 0 ? format(" (it matches at offset %lld)",
                                                                       static_cast<long long>(fedOffset)).c_str()
                                                             : ""),
                 "one clip on track 4, one undo step; pos 0, length 92252160 ticks, 384000 frames; take == fed "
                 "sample for sample (0)");
    if (s.take.size() == 0)
        return;
    // Play it back from the start: the live input is still fed, and monitoring Off keeps it out.
    const auto underrunsBefore = driver.underruns();
    windowPlayFrom(doc, 0);
    Stereo heard;
    driver.run(1, &heard); // the block Play starts in
    const auto firstBlockUnderruns = driver.underruns() - underrunsBefore;
    driver.run(1507, &heard);
    const auto underruns = driver.underruns() - underrunsBefore;
    windowStop(doc);
    driver.run(4);
    std::size_t worstAt = 0;
    const double best = maxDiff(heard, s.take, 0, s.take.size(), &worstAt);
    // Where the frames that differ are, and whether the rest is the take delayed.
    std::size_t differing = 0, firstDiffering = 0, lastDiffering = 0;
    for (std::size_t i = 0; i < s.take.size(); ++i)
        if (std::abs(heard.at(0, i) - s.take.at(0, i)) > 1e-6f || std::abs(heard.at(1, i) - s.take.at(1, i)) > 1e-6f) {
            if (differing++ == 0)
                firstDiffering = i;
            lastDiffering = i;
        }
    const auto playback = format("playback vs take max|d| %.3g at frame %zu (heard %.4f, take %.4f); %zu of %zu frames "
                                 "differ, from %zu to %zu; clip underruns %llu (%llu in the block Play starts in); "
                                 "monitoring %s",
                                 best, worstAt, heard.at(0, worstAt), s.take.at(0, worstAt), differing, s.take.size(),
                                 firstDiffering, lastDiffering, static_cast<unsigned long long>(underruns),
                                 static_cast<unsigned long long>(firstBlockUnderruns), ui::monitorName(monitor));
    const char *playbackExpected = "output == take delayed by 0 samples (max|d| <= 1e-6); the live input is not heard";
    std::string cause;
    // The same start with the locate one callback ahead of Play (the playhead already at 0 when Play comes):
    // whether a stopped playhead reads ahead at all.
    if (differing > 0) {
        OpRequest play;
        play.opType = "transport.play";
        play.payload = Payload::object();
        doc.mailbox().post(ui::TransportMailbox::Command::Locate, 0);
        driver.run(1);
        const bool posted = doc.ops().submit(std::move(play)).ok && doc.mailbox().post(ui::TransportMailbox::Command::Play);
        Stereo again;
        driver.run(40, &again);
        windowStop(doc);
        driver.run(4);
        std::size_t at = 0;
        const double early = maxDiff(again, s.take, 0, again.size(), &at);
        report.engine("2b", format("Locate posted one block before Play (%d): first 40 blocks vs take max|d| %.3g at "
                                   "frame %zu; first sound at frame %zu",
                                   posted ? 1 : 0, early, at, firstSound(again)),
                      "0 if the stopped playhead's pages are read ahead before Play");
        // And with Play taken by an empty callback first (the mailbox drained, nothing rendered), so that this
        // harness's prime between callbacks sees a playing transport: whether the loss is the pages, not the
        // order of the drain and the render in ProjectDocument::process. The app has no such prime.
        OpRequest replay;
        replay.opType = "transport.play";
        replay.payload = Payload::object();
        doc.mailbox().post(ui::TransportMailbox::Command::Locate, 0);
        driver.run(1);
        doc.ops().submit(std::move(replay));
        doc.mailbox().post(ui::TransportMailbox::Command::Play);
        {
            engine::AudioIo empty;
            empty.streamTimeSamples = driver.stream;
            doc.process(empty);
        }
        Stereo third;
        driver.run(40, &third);
        windowStop(doc);
        driver.run(4);
        const double drained = maxDiff(third, s.take, 0, third.size(), &at);
        report.engine("2b", format("Play drained by an empty callback, then the playing transport primed before the "
                                   "first block: max|d| %.3g; first sound at frame %zu",
                                   drained, firstSound(third)),
                      "0 if the first block is lost for want of the clip's pages, not the drain order");
        if (drained == 0 && firstSound(third) == 0 && firstDiffering == 0 && firstBlockUnderruns > 0)
            cause = format("Play from Stop renders its first block before any of the clip's pages are read: ClipPlayback "
                           "asks for read-ahead only while the transport plays (ClipPlayback wanted() is empty when "
                           "stopped), and nothing holds the start until RenderBlock::sourcesReadyAt; that block underruns "
                           "%llu samples, silent while the playhead moves on. The app's disk worker is asynchronous, so "
                           "there the loss is this block at least. Owner engine (read-ahead at the stopped playhead, "
                           "src/adi/engine/clip_playback.cpp), or the window holding Play in ProjectDocument::process "
                           "until sourcesReadyAt",
                           static_cast<unsigned long long>(firstBlockUnderruns));
    }
    if (best <= 1e-6 && monitor == ui::Monitor::Off)
        report.step("2b", V::Pass, playback, playbackExpected);
    else
        report.step("2b", V::Fail, playback, playbackExpected, cause);
    driver.input = nullptr;
}

// 3. A MIDI track with OneShot and a sample; live notes from a keyboard.
void step3(Session &s, Report &report) {
    auto &doc = *s.doc;
    auto &driver = *s.driver;
#if ADI_WITH_PD
    // The browser's built-in (ui_main.cpp `loadBuiltin`), onto 1 MIDI (id 2).
    const bool inserted = doc.insertBuiltin("org.adidaw.builtin.oneshot", 2, std::nullopt);
    const auto insertError = doc.error();
    const auto label = doc.ops().nextUndo() ? doc.ops().nextUndo()->label : std::string();
    const auto dev = scalar(doc, "SELECT MAX(id) FROM devices"); // as `loadPlugin` reads the newest
    // A 1 kHz sine at 0.5 on the slot (ui_main.cpp `assignSample`).
    const auto tone = s.temp.path() / "Tone 1k.wav";
    {
        Stereo sine;
        for (auto &ch : sine.c)
            ch.resize(48000);
        for (std::size_t i = 0; i < 48000; ++i)
            sine.c[0][i] = sine.c[1][i] = static_cast<float>(.5 * std::sin(2 * kPi * 1000 * static_cast<double>(i) / kRate));
        writeWav(tone, sine);
    }
    const bool assigned = inserted && doc.assignSample(dev, 1, tone, nowUtc());
    const auto assignError = doc.error();
    const auto *entry = doc.session().entryFor(dev);
    const auto snap = doc.view().current();
    bool slot = false;
    for (const auto &n : snap->sampleSlots)
        slot = slot || (n->deviceId == dev && n->slot == 1 && !n->empty && !n->missing);
    auto arm = ui::armTracks(snap->tracks, doc.recordStates(), 2, {2}, true, false);
    const bool armed = arm.refusal.empty() && submitEdits(doc, std::move(arm.ops)) && doc.recordStates().at(2).armed &&
                       !doc.recordStates().at(4).armed;
    report.check("3a", inserted && label == "Insert OneShot" && assigned && entry && !entry->placeholder && slot && armed,
                 format("insert %s (undo \"%s\"), device %lld loaded %d; sample %s, slot filled %d; track 2 armed %d "
                        "(track 4 disarmed by Exclusive Arm)",
                        inserted ? "ok" : insertError.c_str(), label.c_str(), static_cast<long long>(dev),
                        entry && !entry->placeholder ? 1 : 0, assigned ? "assigned" : assignError.c_str(), slot ? 1 : 0,
                        armed ? 1 : 0),
                 "OneShot on track 2, \"Insert OneShot\"; loaded; the sample in slot 1; track 2 armed");
    // Live notes (N60): a key played on a port reaches the session through the app's consumer, the feed
    // the device callback pumps in front of the document (ui_main.cpp, LiveMidiProcessor).
    ui::MidiInputFeed feed;
    ui::MidiInputFeed::Ring keyboard;
    const std::array<ui::MidiInputFeed::Source, 1> port{ui::MidiInputFeed::Source{&keyboard, 0, true, false, false, 1}};
    feed.publish(port);
    driver.feed = &feed;
    // A note 37 frames into a block, released 0.25 s later.
    const auto blockStart = driver.stream;
    constexpr std::int64_t k = 37;
    Stereo heard;
    driver.play(keyboard, {{blockStart + k, {0x90, 60, 127}}, {blockStart + k + 12000, {0x80, 60, 0}}}, 300, &heard);
    driver.feed = nullptr;
    std::int64_t first = -1;
    for (std::size_t i = 0; i < heard.size() && first < 0; ++i)
        if (std::abs(heard.c[0][i]) > 1e-4f)
            first = static_cast<std::int64_t>(i);
    const double loud = peak(heard, 0, heard.size());
    const double expected = .5 * gainOf(-12);
    report.check("3b", first - k == 1 && std::abs(loud - expected) < 1e-3,
                 format("through the app's MIDI consumer: first sound at +%lld frames from the note's sample; peak %.4f; "
                        "%llu refused",
                        static_cast<long long>(first < 0 ? -1 : first - k), loud,
                        static_cast<unsigned long long>(feed.refused())),
                 format("a key played on an armed MIDI track is heard at its time: onset at +1, peak %.4f", expected));
#else
    report.step("3a", V::NotBuilt, "this build has no Pd", "OneShot on a MIDI track", "build with ADI_WITH_PD=ON; owner win");
    (void)doc; // a build without Pd plays nothing here
    (void)driver;
    report.step("3b", V::NotBuilt, "this build has no Pd, so no OneShot to play",
                "a key played on an armed MIDI track is heard at its time", "build with ADI_WITH_PD=ON; owner win");
#endif
    std::printf("WTS01 3c, 3d: the VST3 instrument, in adi_acceptance_wts01_vst3 (JUCE)\n");
}

// 4. A fader dragged while the take plays: heard during the drag, one undo step at the end.
void step4(Session &s, Report &report) {
    auto &doc = *s.doc;
    auto &driver = *s.driver;
    if (s.take.size() == 0) {
        report.step("4", V::Fail, "no take from step 2", "a source to drag over");
        return;
    }
    windowPlayFrom(doc, 0);
    Stereo heard;
    driver.run(24, &heard);
    const auto volume = [&] { return real(doc, "SELECT volume_db FROM mixer_strip WHERE track_id=4"); };
    const auto opsNow = [&] { return scalar(doc, "SELECT count(*) FROM ops"); };
    // The level heard over the last 12 of `blocks` blocks.
    const auto level = [&](int blocks) {
        const auto from = heard.size();
        driver.run(blocks, &heard);
        const auto settled = from + static_cast<std::size_t>(blocks - 12) * static_cast<std::size_t>(kFrames);
        Stereo source;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < heard.size(); ++i)
                source.c[c].push_back(s.take.at(c, i - static_cast<std::size_t>(s.playOffset)));
        return fitGain(heard, source, settled, heard.size());
    };
    const double base = level(24);
    const auto ops = opsNow();
    const bool began = doc.beginMixerDrag(fader(4), std::nullopt, "Change Track Volume");
    std::string moves;
    bool during = began, quiet = true;
    for (const double db : {-6.0, -12.0, -24.0}) {
        during = during && doc.mixerDrag(db);
        const double g = level(24);
        during = during && std::abs(g - gainOf(db)) <= .03 * gainOf(db);
        quiet = quiet && opsNow() == ops && volume() == 0;
        moves += format("%.0f dB -> x%.4f (%.2f dB, ops +%lld, row %.1f); ", db, g, dbOf(g),
                        static_cast<long long>(opsNow() - ops), volume());
    }
    const bool ended = doc.endMixerDrag();
    const auto step = doc.ops().nextUndo();
    const double after = level(24);
    const auto rowAfter = volume();
    const auto opsAfter = opsNow() - ops;
    const bool committed = ended && opsAfter == 1 && step && step->label == "Change Track Volume" &&
                           std::abs(rowAfter + 24) < 1e-9 && std::abs(after - gainOf(-24)) <= .03 * gainOf(-24);
    const bool undone = doc.ops().undo().ok && doc.synchronise() && volume() == 0;
    const double back = level(24);
    windowStop(doc);
    driver.run(4);
    report.check("4", std::abs(base - 1) < 1e-6 && during && quiet && committed && undone && std::abs(back - 1) <= .03,
                 format("before x%.6f; %slet go: ops +%lld, undo \"%s\", row %.1f dB, heard x%.4f; Undo: row %.1f, heard x%.4f",
                        base, moves.c_str(), static_cast<long long>(opsAfter), step ? step->label.c_str() : "",
                        rowAfter, after, volume(), back),
                 "each move heard at 10^(dB/20) within 3% with no op and the row at 0; let go: +1 op \"Change Track "
                 "Volume\", -24 dB; one Undo: 0 dB in the row and the sound");
}

// 5. A return fed by sends from two tracks; that return inside a group.
void step5(Session &s, Report &report) {
    auto &doc = *s.doc;
    auto &driver = *s.driver;
    // Track 5's material, imported the way the Browser's import with Copy does (ui_main.cpp: `materialJob`,
    // `prepareMaterial` on a worker, `commitMaterial`).
    const auto file = s.temp.path() / "Strings.wav";
    for (auto &ch : s.strings.c)
        ch.resize(static_cast<std::size_t>(4 * kBarSamples));
    for (std::size_t i = 0; i < s.strings.size(); ++i) {
        const auto t = static_cast<double>(i) / kRate;
        s.strings.c[0][i] = static_cast<float>(.2 * std::sin(2 * kPi * 330 * t));
        s.strings.c[1][i] = static_cast<float>(.15 * std::sin(2 * kPi * 523.25 * t + .3));
    }
    writeWav(file, s.strings);
    ui::ImportChoices choices;
    choices.copyIn = true;
    const auto job = doc.materialJob(media::pathUtf8(file), 5, 0, choices, nowUtc());
    auto material = ui::ProjectDocument::prepareMaterial(job);
    const auto clip = doc.commitMaterial(material, job);
    const auto importError = doc.error();
    driver.frame();
    // A Return's Reverb switched off from its device strip (`devices.submit`).
    const auto reverb = scalar(doc, "SELECT d.id FROM devices d JOIN device_chains c ON c.id=d.chain_id WHERE c.track_id=6");
    const bool off = doc.deviceAction("device.setEnabled", Payload{{"id", reverb}, {"enabled", false}});
    // Each send knob into A Return dragged up to 0 dB (ui_mixer.cpp, the knob's live drag; `setSend` when
    // the track has no row).
    std::string sends;
    bool sent = true;
    for (const std::int64_t track : {4, 5}) {
        const auto snap = doc.view().current();
        if (const auto *row = ui::findSend(snap->sends, track, 6)) {
            sent = sent && dragTo(doc, {gesture::ScalarKind::RoutingGain, row->id, {}, {ui::kSendFloorDb, ui::kSendTopDb,
                                                                                         ui::kSendSilenceDb}},
                                  0, "Change Send Level");
            sends += format("send %lld (track %lld) dragged; ", static_cast<long long>(row->id), static_cast<long long>(track));
        } else {
            auto edit = ui::setSend(*snap, track, 6, 0);
            sent = sent && edit.refusal.empty() && submitEdits(doc, std::move(edit.ops));
            sends += format("track %lld's send made; ", static_cast<long long>(track));
        }
    }
    s.sendsOn = sent;
    const auto sendDb = real(doc, "SELECT MIN(gain_db) FROM routing WHERE kind='send' AND dst_id=6 AND src_id IN (4,5)");
    windowPlayFrom(doc, 0);
    Stereo heard;
    driver.run(200, &heard);
    // x4 + x5 at each timeline sample of what was heard.
    Stereo both;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < 400 * static_cast<std::size_t>(kFrames); ++i) {
            const auto t = i - static_cast<std::size_t>(s.playOffset);
            both.c[c].push_back(s.take.at(c, t) + s.strings.at(c, t));
        }
    const auto scaled = [&](double gain) {
        Stereo out = both;
        for (auto &ch : out.c)
            for (auto &v : ch)
                v = static_cast<float>(gain * v);
        return out;
    };
    const double sum = maxDiff(heard, scaled(2), stepStart(8), heard.size());
    const double sumGain = fitGain(heard, both, stepStart(8), heard.size());
    const double startBlocks = maxDiff(heard, scaled(2), 0, stepStart(8));
    const auto started = firstSound(heard);
    // The A Return's fader to -12 dB: only the return's half moves.
    const bool pulled = dragTo(doc, fader(6), -12, "Change Track Volume");
    s.returnGain = gainOf(-12);
    driver.run(200, &heard);
    const double half = maxDiff(heard, scaled(1 + gainOf(-12)), stepStart(216), heard.size());
    const double halfGain = fitGain(heard, both, stepStart(216), heard.size());
    windowStop(doc);
    driver.run(4);
    report.check("5a", clip > 0 && off && sent && sendDb == 0 && sum <= 1e-6 && pulled && half <= 1e-6,
                 format("import clip %lld%s; Reverb off %d; %ssends at %.1f dB; Main vs 2(x4+x5) max|d| %.3g from "
                        "block 8 (gain x%.6f; blocks 0-7 max|d| %.3g, first sound at frame %zu); A Return at -12 dB: "
                        "Main vs (1+10^(-12/20))(x4+x5) max|d| %.3g (gain x%.6f)",
                        static_cast<long long>(clip), clip > 0 ? "" : (" " + importError).c_str(), off ? 1 : 0,
                        sends.c_str(), sendDb, sum, sumGain, startBlocks, started, half, halfGain),
                 format("Main == 2(x4+x5) within 1e-6; then == %.6f(x4+x5) within 1e-6", 1 + gainOf(-12)));
    // The group: Group Tracks with the return among the selected (ui::groupTracks), and a drag of the return
    // into a group (ui::moveTracks) are both refused by the window today.
    auto snap = doc.view().current();
    const auto refused = ui::groupTracks(snap->tracks, {4, 5, 6});
    if (!refused.refusal.empty()) {
        report.step("5b", V::NotBuilt, "Group Tracks with A Return selected: \"" + refused.refusal + "\"",
                    "tracks 4, 5 and A Return in one group; the return heard inside it; a send from outside refused",
                    "grouped returns: engine DF-E22 in #677 (engine/df-e23-plugin-workers, waits behind #651), owner "
                    "engine; the window's grouping rules for a return, owner window-fn");
        // The engine as it stands: group 4 and 5, put the return in the group by a raw op, the group at -12 dB.
        auto grouped = ui::groupTracks(snap->tracks, {4, 5});
        const auto group = grouped.select;
        int steps = 0;
        if (grouped.refusal.empty() && submitEdits(doc, std::move(grouped.ops)))
            ++steps;
        OpRequest parent;
        parent.opType = "track.setParent";
        parent.payload = {{"id", 6}, {"parent", group}};
        const auto moved = doc.ops().submit(parent);
        if (moved.ok) {
            ++steps;
            doc.synchronise();
        }
        if (dragTo(doc, fader(group), -12, "Change Track Volume"))
            ++steps;
        windowPlayFrom(doc, 0);
        Stereo grouped0;
        driver.run(200, &grouped0);
        windowStop(doc);
        driver.run(4);
        const double k = fitGain(grouped0, both, stepStart(16), grouped0.size());
        bool restored = true;
        for (int i = 0; i < steps; ++i)
            restored = restored && doc.ops().undo().ok;
        doc.synchronise();
        restored = restored && scalar(doc, "SELECT count(*) FROM tracks WHERE parent_id IS NOT NULL") == 0 &&
                   scalar(doc, "SELECT count(*) FROM tracks") == 7;
        report.engine("5b", format("group %lld of 4, 5; track.setParent(6 -> group) %s; group at -12 dB: Main = "
                                   "x%.4f (x4+x5); %d steps undone, project back %d",
                                   static_cast<long long>(group), moved.ok ? "accepted" : ("refused: " + moved.error).c_str(),
                                   k, steps, restored ? 1 : 0),
                      format("the return heard inside the group: x%.4f (group x (1 + A Return's %.1f dB)); the return "
                             "straight to Main instead: x%.4f (group + return); pre-#677 engine",
                             gainOf(-12) * (1 + s.returnGain), dbOf(s.returnGain), gainOf(-12) + s.returnGain));
    } else
        report.step("5b", V::Fail, "Group Tracks accepted the return; this harness does not test it yet",
                    "update the acceptance test now that grouped returns are built");
}

// 6. One automated parameter: a track's Volume (Automation Mode), and a built-in device's parameter.
void step6(Session &s, Report &report) {
    auto &doc = *s.doc;
    auto &driver = *s.driver;
    const std::array<std::pair<std::int64_t, double>, 3> points{
        {{0, 0.0}, {2 * kBarTicks, -24.0}, {4 * kBarTicks, -6.0}}};
    bool added = true;
    std::string labels;
    for (const auto &[tick, db] : points) {
        // AdiRootComponent::addAutomationPoint (ui_shell.cpp): the next free lane and data ids, point 1.
        const auto snap = doc.view().current();
        const auto *t = snap->findTrack(4);
        ui::AutomationIds ids;
        ids.lane = scalar(doc, "SELECT COALESCE(MAX(id), 0) + 1 FROM automation_lanes");
        ids.data = scalar(doc, "SELECT COALESCE(MAX(id), 0) + 1 FROM automation_data");
        ids.point = 1;
        added = added && t && submitEdits(doc, ui::addAutomationPoint(ui::trackEnvelope(*snap, 4), 4, "volume", tick, db,
                                                                       t->volumeDb, ids));
        const auto step = doc.ops().nextUndo();
        added = added && step && step->label == "Add Automation Point";
        labels += step ? step->label + "; " : "none; ";
        driver.frame();
    }
    const auto snap = doc.view().current();
    const auto env = ui::trackEnvelope(*snap, 4);
    windowPlayFrom(doc, 0);
    Stereo heard;
    driver.run(1500, &heard);
    windowStop(doc);
    driver.run(4);
    // Main = (1 + return)(g4 x4 + x5): g4 per block, against the envelope at the block's middle.
    const double both = 1 + (s.sendsOn ? s.returnGain : 0);
    double worst = 0;
    std::size_t worstBlock = 0;
    for (std::size_t b = 2; b < 1500 && env.data; ++b) {
        double num = 0, den = 0;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = stepStart(b); i < stepStart(b + 1); ++i) {
                const auto t = i - static_cast<std::size_t>(s.playOffset);
                const double residual = heard.at(c, i) / both - s.strings.at(c, t);
                num += residual * s.take.at(c, t);
                den += static_cast<double>(s.take.at(c, t)) * s.take.at(c, t);
            }
        const auto want = ui::envelopeValue(*env.data, tickAtSample(stepStart(b) + kFrames / 2));
        const double got = dbOf(den > 0 ? num / den : 0);
        if (want && std::abs(got - *want) > worst) {
            worst = std::abs(got - *want);
            worstBlock = b;
        }
    }
    report.check("6a", added && env.data && env.data->points.size() == 3 && worst <= .1,
                 format("3 points added (%s); per block the take's gain vs the envelope: max |d| %.4f dB (block %zu)",
                        labels.c_str(), worst, worstBlock),
                 "each point one step \"Add Automation Point\"; the gain follows (0, 0 dB) (2 bars, -24) (4 bars, -6) "
                 "within 0.1 dB");
    // A built-in device's parameter: the window offers only Volume.
    report.step("6b", V::NotBuilt, "Automation Mode shows each track's Volume only (automation_mode.hpp)",
                "a device parameter's envelope drawn and followed",
                "the lane chooser for a device parameter from an agreed sheet (AU-01 window half, ADR-0243), or "
                "automation recording wired to the session (AU-03); owner window-fn");
    // The engine as it stands: Utility on track 5 (track 4 deactivated so only x5 is heard), a lane on its Gain
    // (+6 dB for 2 bars, then 0 dB).
    int steps = 0;
    auto mute = ui::activateTracks(snap->tracks, 4, {});
    if (mute.refusal.empty() && submitEdits(doc, std::move(mute.ops)))
        ++steps;
    const bool utility = doc.insertBuiltin("org.adidaw.builtin.utility", 5, std::nullopt);
    if (utility)
        ++steps;
    const auto dev = scalar(doc, "SELECT MAX(id) FROM devices");
    automation::LaneSpec lane;
    lane.id = scalar(doc, "SELECT COALESCE(MAX(id), 0) + 1 FROM automation_lanes");
    lane.ownerKind = "device";
    lane.ownerId = dev;
    lane.paramRef = "11";
    lane.paramName = "Gain";
    lane.valueDomain = "normalized";
    const auto data = scalar(doc, "SELECT COALESCE(MAX(id), 0) + 1 FROM automation_data");
    const auto point = [](std::uint64_t id, std::int64_t time, double value) {
        return Payload{{"id", id}, {"time", time}, {"value", value}, {"curve", 1}, {"tension", 0.0}};
    };
    std::vector<OpRequest> laneOps{automation::createLane(lane),
                                   automation::writePoints(data, lane.id, nullptr,
                                                           Payload::array({point(1, 0, 126.0 / 155),
                                                                           point(2, 2 * kBarTicks, 126.0 / 155),
                                                                           point(3, 2 * kBarTicks + 1, 120.0 / 155),
                                                                           point(4, 4 * kBarTicks, 120.0 / 155)}))};
    std::string laneError;
    if (utility && submitEdits(doc, std::move(laneOps), &laneError))
        ++steps;
    windowPlayFrom(doc, 0);
    Stereo gained;
    driver.run(1500, &gained);
    windowStop(doc);
    driver.run(4);
    double first = 0, second = 0, spread = 0, spreadGain = 0;
    std::size_t spreadAt = 0;
    for (std::size_t b = 4; b < 1496; ++b) {
        if (b >= 746 && b < 790)
            continue; // the jump at 2 bars (sample 192000, block 750) and Utility's glide after it
        double num = 0, den = 0;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = stepStart(b); i < stepStart(b + 1); ++i) {
                const auto t = i - static_cast<std::size_t>(s.playOffset);
                num += gained.at(c, i) / both * s.strings.at(c, t);
                den += static_cast<double>(s.strings.at(c, t)) * s.strings.at(c, t);
            }
        const double u = den > 0 ? num / den : 0;
        const double want = b < 750 ? gainOf(6) : 1.0;
        if (std::abs(u - want) > spread) {
            spread = std::abs(u - want);
            spreadAt = b;
            spreadGain = u;
        }
        (b < 750 ? first : second) = u;
    }
    report.engine("6b", format("Utility %s; device lane %s; Utility's gain x%.6f before 2 bars, x%.6f after; max |d| "
                               "%.2g (block %zu: x%.6f)",
                               utility ? "inserted" : doc.error().c_str(),
                               steps == 3 ? "submitted" : ("refused: " + laneError).c_str(), first, second, spread,
                               spreadAt, spreadGain),
                  format("x%.6f (+6 dB) then x1.000000 (0 dB), within 2e-5 outside blocks 746-789 (the jump)", gainOf(6)));
    for (int i = 0; i < steps; ++i)
        (void)doc.ops().undo();
    doc.synchronise();
}

// 7. Saved, closed and reopened: it renders the same; an exported WAV matches an offline render.
void step7(Session &s, Report &report) {
    auto &driver = *s.driver;
    {
        auto &doc = *s.doc;
        const auto snap = doc.view().current();
        for (const auto &[id, state] : doc.recordStates())
            if (state.armed) {
                auto edit = ui::armTracks(snap->tracks, doc.recordStates(), id, {id}, true, false);
                submitEdits(doc, std::move(edit.ops));
            }
    }
    auto &doc = *s.doc;
    bool disarmed = true;
    for (const auto &[id, state] : doc.recordStates())
        disarmed = disarmed && !state.armed;
    // Ctrl+S: a named snapshot; every edit is committed as it happens (project_document.hpp).
    const auto saved = doc.takeSnapshot(doc.snapshotName(nowUtc(), 0), nowUtc(), 0);
    const std::size_t liveBlocks = 1600;
    // Played twice from the start, before closing and after reopening: the second plays of each are compared,
    // so both come after the same history (a play stops where its automation left the gains).
    const auto playTwice = [&](ui::ProjectDocument &d, Stereo &first, Stereo &second) {
        for (auto *heard : {&first, &second}) {
            windowPlayFrom(d, 0);
            driver.run(static_cast<int>(liveBlocks), heard);
            windowStop(d);
            driver.run(4);
        }
    };
    Stereo liveA1, liveA, liveB1, liveB;
    playTwice(doc, liveA1, liveA);
    // Export: Main, 8 bars from the start, 48 kHz, 32-bit float, no dither, no normalize (the Export page).
    ui::ExportChoices choices;
    choices.track = 0;
    choices.startTicks = 0;
    choices.lengthTicks = 8 * kBarTicks;
    choices.sampleRate = 48000;
    choices.bitDepth = 32;
    choices.dither = "No Dither";
    choices.normalize = false;
    const auto refusals = ui::exportRefusals(choices);
    const auto exportTo = [&](ui::ProjectDocument &d, const char *name) {
        render::Result result;
        if (exportPrecheckRefuses(d)) {
            result.state = render::JobState::Failed;
            result.error = kExportRefusal;
            return result;
        }
        const auto current = d.view().current();
        return render::renderWav(ui::exportJob(choices, d.store().path(), s.temp.path() / name, *current->tempo),
                                 exportLoader(device::withBuiltinDevices()));
    };
    const auto a = exportTo(doc, "A.wav");
    const std::array<const char *, 7> tables{
        "SELECT id,kind,name,IFNULL(parent_id,0),index_in_parent,muted,record_armed FROM tracks ORDER BY id",
        "SELECT track_id,volume_db,pan FROM mixer_strip ORDER BY track_id",
        "SELECT id,chain_id,ord,IFNULL(plugin_ref_id,0),enabled FROM devices ORDER BY id",
        "SELECT id,src_kind,src_id,dst_kind,dst_id,kind,gain_db,pre_fader,enabled FROM routing ORDER BY id",
        "SELECT id,owner_kind,owner_id,param_ref,value_domain FROM automation_lanes ORDER BY id",
        "SELECT id,lane_id,hex(data) FROM automation_data ORDER BY id",
        "SELECT c.id,c.track_id,c.pos_ticks,c.length_ticks,IFNULL(m.hash_blake3,'') FROM clips c LEFT JOIN "
        "audio_clips a ON a.clip_id=c.id LEFT JOIN media_files m ON m.id=a.media_id ORDER BY c.id"};
    std::array<std::string, 7> before;
    std::array<int, 7> counts{};
    for (std::size_t i = 0; i < tables.size(); ++i)
        before[i] = rowsText(doc, tables[i], &counts[i]);
    // Close, as `install()` lets a document go: the callback detached, a take settled, then destroyed.
    doc.release();
    doc.settleRecording();
    s.doc.reset();
    std::string error;
    s.doc = ui::ProjectDocument::open(s.path, false, device::withBuiltinDevices(), error);
    if (!s.doc) {
        report.step("7", V::Fail, "reopen failed: " + error, "the project reopens");
        return;
    }
    auto &again = *s.doc;
    again.prepare(kRate, kFrames);
    driver.attach(again);
    bool same = true;
    std::string differs;
    for (std::size_t i = 0; i < tables.size(); ++i)
        if (rowsText(again, tables[i]) != before[i]) {
            same = false;
            differs += std::to_string(i) + " ";
        }
    const auto b = exportTo(again, "B.wav");
    const bool bitExact = a.state == render::JobState::Completed && b.state == render::JobState::Completed &&
                          bytesOf(a.output) == bytesOf(b.output) && !bytesOf(a.output).empty();
    playTwice(again, liveB1, liveB);
    std::size_t liveAt = 0;
    const double live = maxDiff(liveA, liveB, 0, liveA.size(), &liveAt);
    const double liveFirst = maxDiff(liveA1, liveB1, 0, liveA1.size());
    // Where a first play and a second one stop differing: the glide from where the last play stopped.
    const auto lastDiffering = [](const Stereo &x, const Stereo &y) {
        std::size_t last = 0;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (std::abs(x.at(0, i) - y.at(0, i)) > 1e-6f || std::abs(x.at(1, i) - y.at(1, i)) > 1e-6f)
                last = i + 1;
        return last;
    };
    std::size_t glideAt = 0;
    const double glide = maxDiff(liveB1, liveB, 0, liveB.size(), &glideAt);
    const auto glideEnd = lastDiffering(liveB1, liveB);
    const double liveLoud = peak(liveA, 0, liveA.size());
    // The offline render it must match: the project file in an engine Session of its own, with the job's
    // spec, Main from the start (render::renderWav's own loop, `test_builtin_devices.cpp`'s offline session).
    double offline = -1;
    std::string offlineError;
    std::size_t offlineFrames = 0;
    Stereo wav;
    if (b.state == render::JobState::Completed) {
        wav = readWav(b.output);
        const auto job = ui::exportJob(choices, again.store().path(), s.temp.path() / "unused.wav",
                                       *again.view().current()->tempo);
        StoreError opened{};
        auto store = Store::open(again.store().path(), opened, true);
        engine::Session session;
        session.graph().setFadeFrames(0);
        auto spec = job.session;
        spec.performanceState = false;
        spec.exporting = true;
        if (store && session.load(*store, exportLoader(device::withBuiltinDevices()), spec)) {
            session.transport().loop(0, 0, false);
            session.transport().locate(0);
            session.transport().play();
            Stereo rendered;
            const auto frames = static_cast<std::size_t>(spec.maxFrames);
            std::array<std::vector<float>, 2> out{std::vector<float>(frames), std::vector<float>(frames)};
            const std::array<float *, 2> outs{out[0].data(), out[1].data()};
            for (auto at = std::int64_t{0}; at < job.endSample;) {
                (void)session.clips()->prime();
                const auto n = static_cast<std::int32_t>(std::min<std::int64_t>(spec.maxFrames, job.endSample - at));
                engine::AudioIo io;
                io.out = outs.data();
                io.numOut = 2;
                io.frames = n;
                session.process(io);
                for (std::size_t c = 0; c < 2; ++c)
                    rendered.c[c].insert(rendered.c[c].end(), out[c].begin(), out[c].begin() + n);
                at += n;
            }
            offlineFrames = rendered.size();
            offline = rendered.size() == wav.size() ? maxDiff(rendered, wav, 0, wav.size()) : 1;
        } else
            offlineError = store ? session.error() : std::string(toString(opened));
    }
    report.check("7a", disarmed && saved.ok && same,
                 format("disarmed %d; snapshot \"%s\" %s; reopened rows equal %d (tracks %d, strips %d, devices %d, "
                        "routing %d, lanes %d, lane data %d, clips %d)%s",
                        disarmed ? 1 : 0, saved.name.c_str(), saved.ok ? "taken" : saved.error.c_str(), same ? 1 : 0,
                        counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6],
                        same ? "" : (" differing: " + differs).c_str()),
                 "every track disarmed; the snapshot taken; tracks, strips, devices, sends, lanes, clips and media "
                 "hashes equal after reopening");
    report.check("7b", live <= 1e-6 && liveLoud > .1,
                 format("second live play after reopening vs before: max|d| %.3g (frame %zu) over %zu frames, peak "
                        "%.3f; first plays: max|d| %.3g; after reopening, first vs second play differ by up to %.3g "
                        "(frame %zu) until frame %zu",
                        live, liveAt, liveA.size(), liveLoud, liveFirst, glide, glideAt, glideEnd),
                 "the same play after the same history sounds the same: max|d| <= 1e-6");
    // The offline render above runs the same engine Session the export's job runs, so it cannot see a fault the
    // two share; the live play of the same project is the independent witness. From block 8: the live play's
    // first block is 2b's.
    const double liveVsWav = wav.size() > 0 ? maxDiff(liveB, wav, stepStart(8), liveB.size()) : 1;
    report.check("7c", refusals.empty() && bitExact && offline >= 0 && offline <= 1e-7 && liveVsWav <= 1e-6,
                 format("choices refused %zu; A %s (%llu frames), B %s; A == B bit for bit %d; B vs offline render "
                        "(%zu frames) max|d| %.3g%s; the WAV's first sound at frame %zu, the live render's at %zu; live "
                        "vs WAV from block 8 max|d| %.3g",
                        refusals.size(), a.state == render::JobState::Completed ? "exported" : a.error.c_str(),
                        static_cast<unsigned long long>(a.framesWritten),
                        b.state == render::JobState::Completed ? "exported" : b.error.c_str(), bitExact ? 1 : 0,
                        offlineFrames, offline, offlineError.empty() ? "" : (" " + offlineError).c_str(),
                        firstSound(wav), firstSound(liveB), liveVsWav),
                 "both export 768000 frames (8 bars); A == B bit for bit; the WAV == the offline render within 1e-7 "
                 "and == the live play from block 8 within 1e-6");
    std::printf("WTS01 7d, 7e: export with a VST3 in the project, in adi_acceptance_wts01_vst3 (JUCE)\n");
}
} // namespace
int main() {
    Report report;
    try {
        Session s;
        if (step1(s, report)) {
            step2(s, report);
            step3(s, report);
            step4(s, report);
            step5(s, report);
            step6(s, report);
            step7(s, report);
            if (s.doc) {
                s.doc->release();
                s.doc->settleRecording();
                s.doc.reset();
            }
        }
    } catch (const std::exception &e) {
        report.step("-", V::Fail, std::string("exception: ") + e.what(), "no exception");
    }
    std::printf("WTS01 session: %d FAIL\n", report.failures());
    return report.failures() == 0 ? 0 : 1;
}
