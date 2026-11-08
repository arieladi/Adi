// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
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
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
