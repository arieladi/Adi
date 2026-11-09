// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/ui/project_document.hpp"
#include "knobs_device.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <optional>
#include <thread>
using namespace adi;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
class Tone final : public engine::Node {
  public:
    void process(const engine::NodeIo &io) noexcept override {
        for (int i = 0; i < io.frames; ++i) {
            const auto sample =
                io.transport ? io.transport->timelineSample + io.blockOffset + i : 0;
            const float value =
                io.transport && io.transport->playing
                    ? static_cast<float>(.2 * std::sin(2 * 3.141592653589793 * 750 *
                                                       static_cast<double>(sample) / 48000))
                    : 0;
            for (int c = 0; c < io.channels; ++c)
                io.out[c][io.blockOffset + i] = value;
        }
    }
    const char *name() const noexcept override { return "document tone"; }
};
double bin(const std::array<float, 2048> &data, int k) {
    std::complex<double> sum{};
    for (int i = 0; i < 1024; ++i)
        sum += static_cast<double>(data[static_cast<std::size_t>(i) * 2]) *
               std::polar(1., -2 * 3.141592653589793 * k * i / 1024.);
    return std::abs(sum) / 512.;
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--fixture") {
        std::string error;
        auto doc = ui::ProjectDocument::open(ui::pathFromUtf8(argv[2]), true, {}, error);
        if (!doc) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    test::TempDirectory temp("ui_document", "lifetime");
    std::string error;
    Tone tone;
    auto doc = ui::ProjectDocument::open(temp.path() / "project.adi", true, {}, error);
    check(doc != nullptr, "New creates a playable project");
    if (!doc) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    // New is the store's (#307, N2): Main and Live's default Set, at the history's root.
    const auto *made = doc->view().current().get();
    check(made != nullptr && made->tracks.size() == 7 && made->findTrack(1) != nullptr &&
              made->findTrack(1)->kind == "master" && made->findTrack(2)->name == "1 MIDI" &&
              made->findTrack(3)->name == "2 MIDI" && made->findTrack(4)->name == "3 Audio" &&
              made->findTrack(5)->name == "4 Audio" && made->findTrack(6)->name == "A Reverb" &&
              made->findTrack(7)->name == "B Delay",
          "New makes Main and Live's default Set: 1 MIDI, 2 MIDI, 3 Audio, 4 Audio, A Reverb, B Delay (N41)");
    check(doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == 0,
          "as the history's root: no op, no undo step");
    check(!doc->ops().canUndo() && !doc->ops().undo().ok && doc->session().model().tracks.size() == 7,
          "creating the project cannot be undone (Live 5.4.2)");
    OpRequest add;
    add.opType = "track.create";
    add.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Tone"}};
    check(doc->ops().submit(add).ok && doc->synchronise(), "edit reaches Session and ProjectView");
    doc->session().setSourcesFor([&](std::int64_t id) {
        return id == 8 ? std::vector<engine::Node *>{&tone} : std::vector<engine::Node *>{};
    });
    check(doc->session().rebuild(), "source enters the real graph");
    doc->prepare(48000, 64);
    std::array<float, 64> l{}, r{};
    float *channels[]{l.data(), r.data()};
    engine::AudioIo io;
    io.out = channels;
    io.numOut = 2;
    io.frames = 64;
    doc->process(io);
    check(doc->mailbox().position() == 0, "stopped transport does not advance");
    check(doc->mailbox().post(ui::TransportMailbox::Command::Play), "UI queues play");
    std::array<float, 2048> render{};
    for (int b = 0; b < 16; ++b) {
        io.streamTimeSamples = b * 64;
        doc->process(io);
        for (int i = 0; i < 64; ++i) {
            render[static_cast<std::size_t>(b * 64 + i) * 2] = l[static_cast<std::size_t>(i)];
            render[static_cast<std::size_t>(b * 64 + i) * 2 + 1] = r[static_cast<std::size_t>(i)];
        }
    }
    check(doc->mailbox().playing() && doc->mailbox().position() == 1024,
          "real Session driver advances and feeds back");
    {
        audio::WavWriter file(temp.path() / "render.wav", 48000, 2, audio::WavFormat::Float32);
        file.write(render.data(), 1024);
        file.close();
    }
    audio::WavReader file(temp.path() / "render.wav");
    std::array<float, 2048> decoded{};
    check(file.read(decoded.data(), 1024) == 1024 && decoded == render,
          "file-only float render round trips exactly");
    check(bin(decoded, 157) < 1e-6, "far bin at floor before trusting peak");
    check(bin(decoded, 16) > .05, "750 Hz reaches actual master");
    doc->mailbox().post(ui::TransportMailbox::Command::Stop);
    doc->process(io);
    check(!doc->mailbox().playing() && doc->mailbox().position() == 1024,
          "stop reaches driver without advancing");
    check(doc->ops().undo().ok && doc->synchronise(), "undo refreshes the real engine");
    check(doc->session().model().tracks.size() == 7, "undo removes the added track and keeps the rest");
    check(doc->ops().redo().ok && doc->synchronise(), "redo restores graph rows");
    // Ctrl/Cmd+S takes a named snapshot (ADR-0203 d6, d13): history metadata, no op.
    const std::int64_t noon = 1'761'566'400'000'000; // 2025-10-27 12:00 UTC, in microseconds
    const auto opsBefore = doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
    const auto generation = doc->view().generation();
    check(doc->snapshotName(noon, 120) == "project 2025-10-27 14:00",
          "the prompt is pre-filled with the project's name and the local time");
    const auto taken = doc->takeSnapshot("Verse idea", noon, 120);
    check(taken.ok && taken.name == "Verse idea", "Take Snapshot names the current point");
    check(doc->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBefore &&
              doc->view().generation() == generation &&
              doc->ops().nextUndo()->label != "Verse idea",
          "a snapshot is not an op, an edit or an undo step");
    check(History(doc->store()).listSnapshots().size() == 1, "and the history lists it");
    auto invalid = ui::ProjectDocument::open(temp.path() / "missing.adi", false, {}, error);
    check(!invalid && doc->session().loaded(), "failed Open leaves current document intact");
    check(!ui::ProjectDocument::open(doc->store().path(), true, {}, error),
          "New refuses overwrite");
    doc->release();
    doc.reset();
    {
        auto reopen = ui::ProjectDocument::open(temp.path() / "project.adi", false, {}, error);
        check(reopen && reopen->view().current()->tracks.size() == 8,
              "every edit is in the file once it closes; there is nothing to save");
        check(reopen && reopen->ops().undo().ok, "a reopened project undoes its edits");
        check(reopen && !reopen->ops().canUndo() && !reopen->ops().undo().ok,
              "but never its creation");
        check(reopen && reopen->ops().redo().ok, "and redoes them");
    }
    {
        // A project an earlier build made began its history with a "New project" step that made
        // Main: undo still stops there.
        const auto earlier = temp.path() / "earlier.adi";
        {
            StoreError madeError{};
            auto st = Store::create(earlier, madeError);
            check(st != nullptr, "a project as an earlier build made it");
            if (st) {
                engine::ProjectView pv;
                pv.refresh(*st);
                ui::OpSubmitter sub(*st, pv);
                OpRequest main;
                main.opType = "track.create";
                main.payload = {{"id", 1}, {"kind", "master"}, {"name", "Main"}};
                main.label = ui::kNewProjectLabel;
                check(sub.submit(std::move(main)).ok, "with Main made by its first step");
            }
        }
        auto old = ui::ProjectDocument::open(earlier, false, {}, error);
        check(old && !old->ops().canUndo() && !old->ops().undo().ok && old->view().current()->tracks.size() == 1,
              "that step is still the floor undo stops at");
    }
    for (int i = 0; i < 12; ++i) {
        auto opened = ui::ProjectDocument::open(temp.path() / "project.adi", false, {}, error);
        check(opened != nullptr, "repeated open/close lifecycle");
        if (opened) {
            opened->prepare(48000, 64);
            opened->process(io);
            opened->release();
        }
    }
    {
        // A dropped audio file (#308, N5): the document reads it and captures its pool row without
        // writing anything; the window's batch adds the clip; one commit, one undo step. New makes
        // Live's default Set (#307, with its two returns since N41), tracks 1 to 7, so the drop's audio track is an eighth.
        auto dropped = ui::ProjectDocument::open(temp.path() / "drop.adi", true, {}, error);
        check(dropped != nullptr, "a project for the drop");
        if (dropped) {
            OpRequest audioTrack;
            audioTrack.opType = "track.create";
            audioTrack.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Drums"}};
            check(dropped->ops().submit(audioTrack).ok && dropped->synchronise(), "an audio track");
            const auto wav = temp.path() / "kick.wav";
            {
                std::array<float, 24000> samples{};
                for (std::size_t i = 0; i < samples.size(); ++i)
                    samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * .05) * .5);
                audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
                writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
            }
            const auto opsBeforeDrop = dropped->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            ui::AudioImport import;
            check(dropped->prepareImport(wav.string(), 1'700'000'000'000'000, import) && import.mediaId == 1 &&
                      import.sampleRate == 48000 && import.frames == 24000 && import.media.opType == "media.import" &&
                      dropped->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBeforeDrop &&
                      dropped->store().db().execAndGet("SELECT count(*) FROM media_files").getInt() == 0,
                  "prepareImport reads the file and captures its row, writing nothing");
            auto edit = ui::dropAudio(*dropped->view().current(), 8, 0, "kick", import);
            check(edit.refusal.empty() && edit.ops.size() == 3 && dropped->ops().submit(std::move(edit.ops)).ok &&
                      dropped->synchronise(),
                  "the pool row, the clip and its audio commit together");
            const auto *track = dropped->view().current()->findTrack(8);
            check(track != nullptr && track->clips.size() == 1 && track->clips[0]->name == "kick" &&
                      dropped->ops().nextUndo() && dropped->ops().nextUndo()->opCount == 3 &&
                      dropped->ops().nextUndo()->label == "Insert Audio Clip",
                  "the clip is on the track, one undo step");
            dropped->prepare(48000, 64);
            check(dropped->mailbox().post(ui::TransportMailbox::Command::Play), "play");
            std::array<float, 64> left{}, right{};
            float *dropChannels[] = {left.data(), right.data()};
            engine::AudioIo out;
            out.out = dropChannels;
            out.numOut = 2;
            out.frames = 64;
            bool heard = false;
            for (int block = 0; block < 64 && !heard; ++block) {
                out.streamTimeSamples = block * 64;
                dropped->process(out);
                // The clip's reader fills on the message thread, as #308's own test primes it.
                if (dropped->session().clips() != nullptr)
                    dropped->session().clips()->prime();
                for (const float v : left)
                    heard = heard || std::abs(v) > 1.e-4f;
            }
            check(heard, "and it plays");
            dropped->release();
            {
                // Its waveform (N40, #404): the first request queues the file and draws nothing; the
                // worker builds its peaks once, through its own read-only store; the second request is
                // served from memory; a new generation that still uses the file keeps them, and one
                // that no longer does drops them.
                auto &waves = dropped->waveforms();
                const auto snapshot = dropped->view().current();
                const auto &clip = *snapshot->findTrack(8)->clips[0];
                const auto hash = clip.audio && clip.audio->mediaInfo ? clip.audio->mediaInfo->contentHash : std::string{};
                const bool first = clip.audio && waves.peaks(*clip.audio) == nullptr;
                for (int i = 0; i < 2000 && !(waves.idle() && waves.builds() > 0); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const auto built = clip.audio ? waves.peaks(*clip.audio) : nullptr;
                check(first && !hash.empty() && built != nullptr && built->contentHash == hash &&
                          built->sampleRate == 48000 && built->channels == 1 && built->frames == 24000 &&
                          !built->bins.empty() && waves.builds() == 1 && waves.delivered() == 1 && waves.problem().empty(),
                      "the worker builds the file's peaks once: none drawn at the first request, there at the next");
                const auto again = waves.peaks(*clip.audio);
                for (int i = 0; i < 20; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                check(again == built && waves.builds() == 1, "the second request is served from memory, nothing built");
                waves.follow(*snapshot, 1001);
                check(waves.holds(hash) && waves.peaks(*clip.audio) == built,
                      "a new generation that still uses the file keeps its peaks");
                OpRequest rename;
                rename.opType = "track.rename";
                rename.payload = {{"id", 8}, {"name", "Kick"}};
                check(dropped->ops().submit(rename).ok, "an edit elsewhere");
                waves.follow(*dropped->view().current(), 1002);
                check(waves.holds(hash), "and so does the edit's");
                check(dropped->ops().undo().ok && dropped->ops().undo().ok && dropped->synchronise(), "undo the drop");
                waves.follow(*dropped->view().current(), 1003);
                check(!waves.holds(hash) && waves.builds() == 1, "a generation without the clip drops its peaks");
                check(dropped->ops().redo().ok && dropped->synchronise(), "redo the drop");
            }
            check(dropped->ops().undo().ok && dropped->synchronise() &&
                      dropped->view().current()->findTrack(8)->clips.empty() &&
                      dropped->store().db().execAndGet("SELECT count(*) FROM media_files").getInt() == 0,
                  "one undo removes the clip, its audio and the new pool row");
            ui::AudioImport none;
            check(!dropped->prepareImport((temp.path() / "missing.wav").string(), 1, none) && !dropped->error().empty(),
                  "a file that can't be read is refused, with a reason");
        }
    }
    {
        // Save a Copy (Live 12 manual 5.4.1), Collect All and Save (5.7) and Collect and Export (ADI's,
        // MR 11.3) from the document (A4, N30). The copy opens and the open project stays where it is;
        // collecting brings an outside file into `audio/` beside the project and the view re-reads it;
        // the export runs on its own worker and leaves the project as it was. No ZIP is opened here.
        const auto home = temp.path() / "collect";
        std::filesystem::create_directories(home / "outside");
        auto copying = ui::ProjectDocument::open(home / "song.adi", true, {}, error);
        check(copying != nullptr, "a project to copy and collect");
        if (copying) {
            const auto wav = home / "outside" / "snare.wav";
            {
                std::array<float, 4800> samples{};
                for (std::size_t i = 0; i < samples.size(); ++i)
                    samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * .1) * .4);
                audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
                writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
            }
            ui::AudioImport import;
            auto ok = copying->prepareImport(wav.string(), 1'700'000'000'000'000, import);
            auto edit = ui::dropAudio(*copying->view().current(), 4, 0, "snare", import);
            check(ok && edit.refusal.empty() && copying->ops().submit(std::move(edit.ops)).ok && copying->synchronise(),
                  "an outside file dropped on 3 Audio");
            const auto copy = home / "copy.adi";
            check(copying->saveCopy(copy) && copying->error().empty() && std::filesystem::exists(copy) &&
                      std::filesystem::equivalent(copying->store().path(), home / "song.adi") && copying->name() == "song",
                  "Save a Copy writes a second project, and the open one stays where it is");
            {
                StoreError opened{};
                auto copied = Store::open(copy, opened, true);
                check(copied && opened == StoreError::Ok &&
                          copied->db().execAndGet("SELECT count(*) FROM tracks").getInt() ==
                              copying->store().db().execAndGet("SELECT count(*) FROM tracks").getInt(),
                      "the copy opens, with the same tracks");
            }
            check(!copying->saveCopy(copy) && copying->error().find("already exists") != std::string::npos,
                  "a copy over an existing file is refused, with the name");
            const auto collectGeneration = copying->view().generation();
            check(copying->collectAllAndSave() && copying->error().empty() && copying->view().generation() > collectGeneration,
                  "Collect All and Save succeeds, and the view reads the project again");
            std::size_t collected = 0;
            if (std::filesystem::is_directory(home / "audio"))
                for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator(home / "audio"))
                    ++collected;
            check(collected == 1 &&
                      copying->store().db().execAndGet("SELECT rel_path FROM media_files").getString().rfind("audio/", 0) == 0,
                  "the outside file is in audio/ beside the project, and the pool points at it there");
            const auto archive = home / "song.zip";
            const auto opsBeforeExport = copying->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            ui::ProjectDocument::ExportJob job;
            check(copying->prepareCollectAndExport(archive, job) && copying->error().empty() && job.zip == archive &&
                      std::filesystem::equivalent(job.project, copying->store().path()),
                  "Collect and Export is prepared on the message thread: the project's file and the ZIP");
            // The app runs it on a worker of its own while the project stays open and writable.
            StoreFileResult exported;
            std::thread worker([&] { exported = ui::ProjectDocument::runCollectAndExport(job); });
            worker.join();
            if (!exported.ok)
                std::printf("export: %s\n", exported.error.c_str());
            check(exported.ok && std::filesystem::exists(archive) &&
                      copying->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBeforeExport,
                  "run on a worker, it writes the ZIP and leaves the project unchanged");
            ui::ProjectDocument::ExportJob again;
            check(!copying->prepareCollectAndExport(archive, again) && copying->error().find("already exists") != std::string::npos,
                  "an export over an existing file is refused before it starts");
            copying->release();
        }
    }
    {
        // Save As (Live 12 manual 5.4.1, 41.3; ADR-0203 d6; N3): the project carries on in the new
        // file, its history with it, and the old file keeps what it had. No audio media here:
        // relocating media is what fails on macOS, and this suite runs there.
        const auto before = temp.path() / "before.adi";
        auto saving = ui::ProjectDocument::open(before, true, {}, error);
        check(saving != nullptr, "a project to save as");
        if (saving) {
            OpRequest first;
            first.opType = "track.create";
            first.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Before"}};
            check(saving->ops().submit(first).ok && saving->synchronise(), "an edit before Save As");
            const auto opsBeforeSave = saving->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            const auto folder = temp.path() / "saved";
            std::filesystem::create_directories(folder);
            const auto after = folder / "after.adi";
            const auto generationBefore = saving->view().generation();
            check(saving->saveAs(after) && saving->error().empty() && saving->warning().empty() &&
                      std::filesystem::equivalent(saving->store().path(), after) && saving->name() == "after",
                  "Save As writes the new file and the document carries on in it, under its name");
            check(saving->snapshotName(noon, 120) == "after 2025-10-27 14:00",
                  "the Take Snapshot prompt is pre-filled with the new name");
            // The re-read the Store header asks for after a Save As: a new generation of the
            // view, which the session has already caught up with.
            check(saving->view().generation() > generationBefore && saving->synchronise() &&
                      saving->view().current()->findTrack(8) != nullptr &&
                      saving->session().model().tracks.size() == 8,
                  "the view and the session read the new file");
            OpRequest second;
            second.opType = "track.create";
            second.payload = {{"id", 9}, {"kind", "audio"}, {"name", "After"}};
            check(saving->ops().submit(second).ok && saving->synchronise() &&
                      saving->view().current()->tracks.size() == 9,
                  "the next edit lands in the new file");
            {
                StoreError opened{};
                auto old = Store::open(before, opened, true);
                check(old && opened == StoreError::Ok &&
                          old->db().execAndGet("SELECT count(*) FROM tracks").getInt() == 8 &&
                          old->db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBeforeSave,
                      "and the old file is unchanged");
            }
            check(saving->ops().undo().ok && saving->synchronise() && saving->ops().undo().ok &&
                      saving->synchronise() && saving->view().current()->tracks.size() == 7 &&
                      saving->session().model().tracks.size() == 7,
                  "undo still reaches the edit made before Save As");
            check(!saving->saveAs(before) && saving->error().find("already exists") != std::string::npos &&
                      std::filesystem::equivalent(saving->store().path(), after) && saving->name() == "after",
                  "an existing destination is refused with its reason, and nothing moves");
            check(!saving->saveAs(temp.path() / "nowhere" / "lost.adi") && !saving->error().empty() &&
                      std::filesystem::equivalent(saving->store().path(), after),
                  "a folder that does not exist is refused with the store's reason");
            check(saving->ops().redo().ok && saving->synchronise() && saving->view().current()->tracks.size() == 8,
                  "and the history still redoes after a refusal");
        }
    }
    {
        // A parameter drag in the device strip is one undo step (N7, worklist A3; ADR-0124 d4,
        // ADR-0181 d5; MR 9.1, audit row 110). The plug-in is the loader's headless Knobs; the
        // gesture is what ParameterControl sends (ui_devices.cpp): Begin, Values, End.
        using engine::ParamEventKind;
        auto strip = ui::ProjectDocument::open(temp.path() / "strip.adi", true, test::knobsLoader(), error);
        check(strip != nullptr, "a project whose plug-ins the loader makes");
        // An exception here is a failed check with its text, not a CRT dialog on a desktop.
        if (strip) try {
            // A chain on 1 MIDI and a device in it, after New's two returns (refs, chains and
            // devices 1 and 2: Reverb and Delay); the plug-in reference is the catalogue's row,
            // not an op.
            strip->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                                     test::kKnobsUid + "','UI knobs')");
            std::vector<OpRequest> make(2);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
            check(strip->ops().submit(std::move(make)).ok && strip->synchronise(), "a device on track 2");
            strip->tick(0); // the timer's frame: the feed publishes the new device
            auto *knobs = dynamic_cast<test::Knobs *>(strip->session().instanceFor(3));
            const auto *entry = strip->session().entryFor(3);
            check(knobs != nullptr && entry != nullptr && !entry->placeholder && entry->name == "UI knobs",
                  "the loader made it and the session holds it under its name");
            // The feed's panel for it, among the returns' two.
            auto panelOf = [&](std::int64_t device) -> const ui::DevicePanelData * {
                const auto feed = strip->parameterFeed().current();
                if (feed)
                    for (const auto &data : *feed)
                        if (data.id == device)
                            return &data;
                return nullptr;
            };
            const auto *panel = panelOf(3);
            check(panel != nullptr && panel->track == 2 && panel->records.size() == 2 &&
                      panel->records[0].id == "cutoff" && std::abs(panel->records[0].playing - .5) < 1e-9 &&
                      panel->records[0].text == "10.0 kHz" && panel->records[1].text == "-24.0 dB" &&
                      panel->defaults.size() == 2 && std::abs(panel->defaults.at("cutoff") - .5) < 1e-9 &&
                      std::abs(panel->defaults.at("gain") - .25) < 1e-9,
                  "the strip's feed lists the device on its track, its parameters by the plug-in's ids, in the "
                  "plug-in's own words, with their defaults (manual 41.6's Return to Default)");
            const auto opsBeforeDrag = strip->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            auto headTxn = [&] {
                const auto head = strip->ops().nextUndo();
                return head ? head->txnId : std::int64_t{0};
            };
            const auto txnBefore = headTxn();
            bool moving = knobs != nullptr && strip->parameterGesture(3,"cutoff", ParamEventKind::Begin, .5);
            for (const double v : {.6, .7, .8})
                moving = moving && strip->parameterGesture(3,"cutoff", ParamEventKind::Value, v);
            check(moving && knobs != nullptr && std::abs(knobs->getParam("cutoff").normalized - .8) < 1e-9 &&
                      strip->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBeforeDrag &&
                      headTxn() == txnBefore && strip->error().empty(),
                  "every value of the drag reaches the plug-in at once, and nothing is written before End");
            check(strip->parameterGesture(3,"cutoff", ParamEventKind::End, .8) && strip->error().empty(),
                  "End commits the gesture");
            const auto step = strip->ops().nextUndo();
            check(step && step->opCount == 2 && step->label == "UI knobs: Cutoff" && step->txnId != 0 &&
                      step->txnId != txnBefore,
                  "Begin, three Values and End are ONE undo step, named for the device and its control");
            if (step) {
                SQLite::Statement first(strip->store().db(),
                                        "SELECT op_type, payload FROM ops WHERE txn_id = ? ORDER BY seq LIMIT 1");
                first.bind(1, step->txnId);
                const bool found = first.executeStep();
                Payload opener;
                if (found) {
                    const auto column = first.getColumn(1); // the journal stores payloads as CBOR
                    const auto *bytes = static_cast<const std::uint8_t *>(column.getBlob());
                    opener = Payload::from_cbor(
                        std::vector<std::uint8_t>(bytes, bytes + static_cast<std::size_t>(column.getBytes())));
                }
                check(found && first.getColumn(0).getString() == "device.setParam" && opener.contains("norm") &&
                          std::abs(opener.at("norm").get<double>() - .5) < 1e-9 && opener.value("dev", 0) == 3,
                      "whose first op is the opener: the value the parameter had before the drag (ADR-0124 d4)");
            }
            // The project's own row for a parameter, as the session re-reads it on synchronise();
            // the snapshot carries lanes, not plug-in rows.
            auto rowOf = [&](const char *param) -> std::optional<double> {
                for (const auto &p : strip->session().model().pluginParams)
                    if (p.deviceId == 3 && p.paramId == param)
                        return p.normalized;
                return std::nullopt;
            };
            check(rowOf("cutoff") && std::abs(*rowOf("cutoff") - .8) < 1e-9 && panelOf(3) != nullptr &&
                      std::abs(panelOf(3)->records[0].stored - .8) < 1e-9,
                  "the project holds the final value and the strip's feed shows it stored");
            check(strip->ops().undo().ok && strip->synchronise() && !rowOf("cutoff") && headTxn() == txnBefore,
                  "one undo restores the value: the parameter is never touched again, as before the drag");
            check(strip->ops().redo().ok && strip->synchronise() && rowOf("cutoff") &&
                      std::abs(*rowOf("cutoff") - .8) < 1e-9,
                  "and one redo brings the whole drag back");
            // A value nothing brackets (a typed value, a key) closes on the drain's quiet timer.
            // An event the plug-in refuses after that still commits the older gesture: the
            // producer drains before it judges (docs/ENGINE-UI-PARAM-CAPTURE.md).
            check(strip->parameterGesture(3,"gain", ParamEventKind::Value, .9) && knobs != nullptr &&
                      std::abs(knobs->getParam("gain").normalized - .9) < 1e-9 && !rowOf("gain"),
                  "an unbracketed value sounds at once and waits for the quiet timer");
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (knobs)
                knobs->refuse = true;
            const auto refusedCount = strip->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            check(!strip->parameterGesture(3,"cutoff", ParamEventKind::Value, .3) &&
                      strip->error() == "The plug-in did not take the value." && rowOf("gain") &&
                      std::abs(*rowOf("gain") - .9) < 1e-9 && strip->ops().nextUndo() &&
                      strip->ops().nextUndo()->label == "UI knobs: Gain" && strip->ops().nextUndo()->opCount == 2 &&
                      strip->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == refusedCount + 2,
                  "an event the plug-in refuses is refused with its reason, and the older gesture it drained commits");
            if (knobs)
                knobs->refuse = false;
            check(!strip->parameterGesture(99, "cutoff", ParamEventKind::Begin, .5) &&
                      strip->error() == "That device is not loaded, so its values cannot be edited." &&
                      !strip->parameterGesture(3,"nothing", ParamEventKind::Begin, .5) &&
                      strip->error() == "The plug-in has no parameter \"nothing\".",
                  "a device that is not loaded and a parameter the plug-in has not are refused, each with its reason");
            check(strip->parameterGesture(3,"cutoff", ParamEventKind::Begin, .8) &&
                      strip->parameterGesture(3,"cutoff", ParamEventKind::Value, .4) &&
                      strip->parameterGesture(3,"cutoff", ParamEventKind::End, .4) && strip->ops().nextUndo() &&
                      strip->ops().nextUndo()->opCount == 1 && strip->ops().nextUndo()->label == "UI knobs: Cutoff",
                  "a later gesture on the same parameter needs no opener: one op, one step");
        } catch (const std::exception &e) {
            check(false, e.what());
        }
    }
    {
        // Re-Enable Automation lit, and Automation Arm (N26, worklist C1; Live 12 manual 25.1 and
        // 25.4; MR 6.1, audit rows 97 and 133). A3's device, its Cutoff under the numeric id a
        // lane's param_ref must parse as, and a lane on it: a strip gesture overrides the lane
        // (ADR-0162), the snapshot says so, and Re-Enable follows the envelope again. The
        // reference row says vst3 so the session's own re-read of a changed row finds the
        // override too (a clap row's ids are hex, which "7" is not); the loader goes by uid.
        using engine::ParamEventKind;
        auto armed = ui::ProjectDocument::open(temp.path() / "arm.adi", true, test::knobsLoader("7"), error);
        check(armed != nullptr, "a project for the automated device");
        if (armed) try {
            armed->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'vst3','") +
                                   test::kKnobsUid + "','UI knobs')");
            std::vector<OpRequest> make(2);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
            check(armed->ops().submit(std::move(make)).ok && armed->synchronise(), "a device on 1 MIDI");
            automation::LaneSpec spec;
            spec.id = 1;
            spec.ownerKind = "device";
            spec.ownerId = 3;
            spec.paramRef = "7";
            spec.paramName = "Cutoff";
            spec.defaultValue = .5;
            check(armed->ops().submit(automation::createLane(spec)).ok && armed->synchronise(),
                  "a lane on its Cutoff, with no points: the lane plays its default");
            armed->tick(0); // the timer's frame: the feed publishes the device
            auto *knobs = dynamic_cast<test::Knobs *>(armed->session().instanceFor(3));
            auto snapshot = [&] { return armed->view().current(); };
            auto laneOverridden = [&] {
                const auto *lane = snapshot()->findAutomationLane(1);
                return lane != nullptr && lane->overridden;
            };
            auto cutoffRecord = [&]() -> const panel::Record * {
                const auto feed = armed->parameterFeed().current();
                if (feed)
                    for (const auto &data : *feed)
                        if (data.id == 3 && !data.records.empty())
                            return &data.records[0];
                return nullptr;
            };
            auto opCount = [&] { return armed->store().db().execAndGet("SELECT count(*) FROM ops").getInt(); };
            auto headTxn = [&] {
                const auto head = armed->ops().nextUndo();
                return head ? head->txnId : std::int64_t{0};
            };
            check(knobs != nullptr && snapshot()->automationLanes.size() == 1 && !snapshot()->automationOverridden &&
                      !laneOverridden() && !armed->session().automationOverridden(),
                  "nothing is overridden: Re-Enable Automation is unlit");
            check(cutoffRecord() != nullptr && cutoffRecord()->id == "7" && cutoffRecord()->automated &&
                      !cutoffRecord()->overridden,
                  "the strip's feed marks Cutoff automated, not overridden");
            check(armed->automationArmed() && armed->session().automationArmed() && !armed->session().automationRecording(),
                  "Automation Arm is on in a new Set (manual 25.1), and the window never records");
            const auto generationBefore = armed->view().generation();
            const auto opsBeforeDrag = opCount();
            const auto txnBefore = headTxn();
            check(armed->parameterGesture(3, "7", ParamEventKind::Begin, .5) &&
                      armed->parameterGesture(3, "7", ParamEventKind::Value, .8) && armed->error().empty() &&
                      armed->session().overriddenLanes().contains(1) && snapshot()->automationOverridden &&
                      laneOverridden() && armed->view().generation() == generationBefore + 1 && opCount() == opsBeforeDrag &&
                      headTxn() == txnBefore,
                  "a value the plug-in takes overrides the lane at once, with no op: the snapshot says so "
                  "before any frame, under a new generation, and the undo list is untouched (ADR-0162)");
            check(armed->parameterGesture(3, "7", ParamEventKind::End, .8) && armed->error().empty() &&
                      opCount() == opsBeforeDrag + 2 && snapshot()->automationOverridden && laneOverridden(),
                  "End commits the gesture, and the override stands through the submitter's refresh");
            check(cutoffRecord() != nullptr && cutoffRecord()->overridden, "the strip's feed marks Cutoff overridden");
            const auto generationAfterEnd = armed->view().generation();
            armed->tick(0);
            check(armed->view().generation() == generationAfterEnd && snapshot()->automationOverridden,
                  "a frame that changes nothing rebuilds nothing, and the switch stays lit");
            check(armed->ops().undo().ok && armed->synchronise() && snapshot()->automationOverridden && laneOverridden() &&
                      armed->ops().redo().ok && armed->synchronise() && snapshot()->automationOverridden,
                  "undo and redo of the move edit the stored value, not the override: it stands (not an op)");
            const auto opsBeforeReenable = opCount();
            const auto txnBeforeReenable = headTxn();
            check(armed->reenableAutomation() && armed->error().empty() && !armed->session().automationOverridden() &&
                      !snapshot()->automationOverridden && !laneOverridden() && opCount() == opsBeforeReenable &&
                      headTxn() == txnBeforeReenable,
                  "Re-Enable Automation: every lane follows its envelope again, in the same call; not an op, "
                  "not an undo step (manual 25.4)");
            check(cutoffRecord() != nullptr && cutoffRecord()->automated && !cutoffRecord()->overridden,
                  "and the strip's feed marks Cutoff automated again at once");
            check(armed->reenableAutomation() && armed->error().empty() && !snapshot()->automationOverridden,
                  "with nothing overridden it succeeds and changes nothing");
            // An override the session finds on its own re-read of the rows (ADR-0162: an op, the
            // agent, anything that changes an automated value): the submitter refreshes before the
            // session re-reads, and synchronise() brings the snapshot up in the same call.
            OpRequest op;
            op.opType = "device.setParam";
            op.payload = {{"dev", 3}, {"param", "7"}, {"norm", .3}};
            const auto generationBeforeOp = armed->view().generation();
            check(armed->ops().submit(op).ok && !snapshot()->automationOverridden && armed->synchronise() &&
                      armed->session().overriddenLanes().contains(1) && snapshot()->automationOverridden &&
                      laneOverridden() && armed->view().generation() == generationBeforeOp + 2,
                  "an op on the automated parameter: the session finds the override when it re-reads, and "
                  "synchronise() puts it in the snapshot (one more generation, the rows unchanged)");
            check(armed->reenableAutomation() && !snapshot()->automationOverridden, "and Re-Enable clears that too");
            armed->setAutomationArmed(false);
            check(!armed->automationArmed() && !armed->session().automationArmed() && !armed->session().automationRecording(),
                  "Automation Arm off: the session is told, recording still never on");
            armed->setAutomationArmed(true);
            check(armed->automationArmed(), "and on again");
        } catch (const std::exception &e) {
            check(false, e.what());
        }
    }
    {
        // A CLAP plug-in spells its parameter ids in hex where its lanes' refs are decimal
        // (ADR-0165): the strip's feed finds the lane through the same spelling the session's
        // override uses, so Re-Enable lighting and the control's LED going off agree (C1 review).
        using engine::ParamEventKind;
        auto clap = ui::ProjectDocument::open(temp.path() / "clap-arm.adi", true, test::knobsLoader("0000001f", "clap"),
                                              error);
        check(clap != nullptr, "a project for a CLAP-spelled device");
        if (clap) try {
            clap->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                                    test::kKnobsUid + "','UI knobs')");
            std::vector<OpRequest> make(2);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            make[1].opType = "device.insert";
            make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
            automation::LaneSpec spec;
            spec.id = 1;
            spec.ownerKind = "device";
            spec.ownerId = 3;
            spec.paramRef = "31"; // 0x1f
            spec.paramName = "Cutoff";
            spec.defaultValue = .5;
            make.push_back(automation::createLane(spec));
            check(clap->ops().submit(std::move(make)).ok && clap->synchronise(), "the device and a lane on its Cutoff");
            clap->tick(0);
            auto cutoffRecord = [&]() -> const panel::Record * {
                const auto feed = clap->parameterFeed().current();
                if (feed)
                    for (const auto &data : *feed)
                        if (data.id == 3 && !data.records.empty())
                            return &data.records[0];
                return nullptr;
            };
            check(cutoffRecord() != nullptr && cutoffRecord()->id == "0000001f" && cutoffRecord()->automated &&
                      !cutoffRecord()->overridden,
                  "the feed keeps the plug-in's hex id and finds the lane under its decimal ref");
            check(clap->parameterGesture(3, "0000001f", ParamEventKind::Begin, .5) &&
                      clap->parameterGesture(3, "0000001f", ParamEventKind::Value, .8) && clap->error().empty() &&
                      clap->session().overriddenLanes().contains(1) && clap->view().current()->automationOverridden &&
                      clap->parameterGesture(3, "0000001f", ParamEventKind::End, .8) && cutoffRecord() != nullptr &&
                      cutoffRecord()->overridden,
                  "a drag overrides the lane and the feed marks the control overridden: Re-Enable and the LED agree");
            check(clap->reenableAutomation() && cutoffRecord() != nullptr && cutoffRecord()->automated &&
                      !cutoffRecord()->overridden && !clap->view().current()->automationOverridden,
                  "and Re-Enable lights the LED again");
        } catch (const std::exception &e) {
            check(false, e.what());
        }
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
