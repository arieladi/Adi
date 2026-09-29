// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
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
        if (!doc || !doc->save(error)) {
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
    check(doc->store().db().execAndGet("SELECT count(*) FROM project").getInt() == 1,
          "initial project row exists");
    check(
        doc->store().db().execAndGet("SELECT count(*) FROM tracks WHERE kind='master'").getInt() ==
            1,
        "initial output exists");
    check(!doc->ops().canUndo(), "bootstrap is not a user edit");
    OpRequest add;
    add.opType = "track.create";
    add.payload = {{"id", 2}, {"kind", "audio"}, {"name", "Tone"}};
    check(doc->ops().submit(add).ok && doc->synchronise(), "edit reaches Session and ProjectView");
    doc->session().setSourcesFor([&](std::int64_t id) {
        return id == 2 ? std::vector<engine::Node *>{&tone} : std::vector<engine::Node *>{};
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
    check(doc->session().model().tracks.size() == 1, "undo removes audio track and keeps master");
    check(doc->ops().redo().ok && doc->synchronise(), "redo restores graph rows");
    {
        StoreError readError{};
        auto reader = Store::open(doc->store().path(), readError, true);
        SQLite::Transaction readTransaction(reader->db());
        SQLite::Statement held(reader->db(), "SELECT name FROM project");
        check(held.executeStep(), "external reader holds old snapshot");
        OpRequest rename;
        rename.opType = "project.setName";
        rename.payload = {{"name", "Changed while read"}};
        check(doc->ops().submit(rename).ok, "WAL accepts edit beside external reader");
        check(!doc->save(error) && !error.empty(),
              "Save reports a blocked checkpoint rather than success");
    }
    check(doc->save(error), "Save checkpoints live document");
    const auto copy = temp.path() / "saved-copy.adi";
    std::filesystem::copy_file(doc->store().path(), copy);
    auto reopen = ui::ProjectDocument::open(copy, false, {}, error);
    check(reopen && reopen->view().current()->tracks.size() == 2,
          "main file alone contains saved edits while original is open");
    auto invalid = ui::ProjectDocument::open(temp.path() / "missing.adi", false, {}, error);
    check(!invalid && doc->session().loaded(), "failed Open leaves current document intact");
    check(!ui::ProjectDocument::open(doc->store().path(), true, {}, error),
          "New refuses overwrite");
    doc->release();
    reopen.reset();
    doc.reset();
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
        auto dropped = ui::ProjectDocument::open(temp.path() / "drop.adi", true, {}, error);
        std::int64_t track = 0;
        check(dropped->addAudioTrack(track), "Add audio track through op");
        const auto path = temp.path() / "tone.wav";
        std::array<float, 2048> samples{};
        for (int i = 0; i < 1024; ++i)
            samples[static_cast<std::size_t>(i) * 2] =
                samples[static_cast<std::size_t>(i) * 2 + 1] =
                    static_cast<float>(.2 * std::sin(2 * 3.141592653589793 * 16 * i / 1024.));
        {
            audio::WavWriter w(path, 48000, 2, audio::WavFormat::Float32);
            w.write(samples.data(), 1024);
            w.close();
        }
        check(dropped->dropAudio(path, track, 0), "Drop creates media, clip and audio attachment");
        if (!dropped->error().empty())
            std::printf("drop: %s\n", dropped->error().c_str());
        auto &db = dropped->store().db();
        check(db.execAndGet("SELECT count(*) FROM audio_clips").getInt() == 1,
              "audio attachment exists");
        check(db.execAndGet("SELECT frames FROM media_files").getInt64() == 1024,
              "drop captures source metadata");
        check(dropped->view().current()->findTrack(track)->clips[0]->name == "tone",
              "snapshot supplies clip name");
        check(dropped->ops().undo().ok && dropped->synchronise(), "one undo removes complete drop");
        check(db.execAndGet("SELECT (SELECT count(*) FROM clips)+(SELECT count(*) FROM "
                            "audio_clips)+(SELECT count(*) FROM media_files)")
                      .getInt() == 0,
              "undo leaves no orphan media or clip");
        check(dropped->ops().redo().ok && dropped->synchronise(), "redo restores playable drop");
        check(!dropped->dropAudio(path, 1, 0), "master refuses audio drop");
        const auto beforeOps = db.execAndGet("SELECT count(*) FROM ops").getInt64();
        OpRequest invalidAttach;
        invalidAttach.opType = "clip.attachAudio";
        invalidAttach.payload = {{"id", 1}, {"media", 1}, {"frames", 0}};
        check(!dropped->ops().submit(invalidAttach).ok, "zero-frame attachment refused");
        check(db.execAndGet("SELECT count(*) FROM ops").getInt64() == beforeOps,
              "refused attachment leaves journal unchanged");
        OpRequest detach;
        detach.opType = "clip.detachAudio";
        detach.payload = {{"id", 1}};
        check(dropped->ops().submit(detach).ok, "direct detach commits");
        check(db.execAndGet("SELECT count(*) FROM audio_clips").getInt() == 0,
              "detach removes attachment without deleting clip");
        check(dropped->ops().undo().ok && dropped->synchronise(),
              "detach inverse restores attachment");
        check(db.execAndGet("SELECT src_len_frames FROM audio_clips").getInt64() == 1024,
              "inverse preserves source frame window");
        dropped->session().graph().setFadeFrames(0);
        dropped->prepare(48000, 64);
        dropped->mailbox().post(ui::TransportMailbox::Command::Play);
        // File read-ahead prepares on the session service, not in process.
        dropped->mailbox().drain(dropped->session().transport());
        check(dropped->session().clips()->prime(std::chrono::seconds(10)),
              "offline clip read-ahead primes");
        std::array<float, 64> dropLeft{}, dropRight{};
        float *outputs[] = {dropLeft.data(), dropRight.data()};
        engine::AudioIo dropIo{};
        dropIo.out = outputs;
        dropIo.numOut = 2;
        dropIo.frames = 64;
        std::array<float, 2048> rendered{};
        for (int b = 0; b < 16; ++b) {
            dropped->process(dropIo);
            for (int i = 0; i < 64; ++i) {
                rendered[static_cast<std::size_t>(b * 64 + i) * 2] =
                    dropLeft[static_cast<std::size_t>(i)];
                rendered[static_cast<std::size_t>(b * 64 + i) * 2 + 1] =
                    dropRight[static_cast<std::size_t>(i)];
            }
        }
        {
            audio::WavWriter w(temp.path() / "drop-dropIo.wav", 48000, 2,
                               audio::WavFormat::Float32);
            w.write(rendered.data(), 1024);
            w.close();
        }
        check(bin(rendered, 157) < 1e-5, "dropped clip far bin at floor");
        check(bin(rendered, 16) > .05, "dropped audio actually plays through Session");
        const auto* meter=dropped->session().meterFor(track);
        check(meter && meter->peak.load()>0 && meter->rms.load()>0,"strip publishes peak and RMS after real clip audio");
        ControllerPolicy policy;policy.remoteEnabled=true;policy.focusDial=ControllerCc{"Remote",1,7};
        const std::array<int,3> trace{10,50,100};
        check(!dropped->learnTrack(track,"volume",{"Remote",1,7},trace,policy),"mixer Learn refuses reserved Focus Dial");
        check(dropped->learnTrack(track,"volume",{"Remote",1,8},trace,policy),"mixer Learn needs no automation lane");
        check(dropped->bindings().size()==1,"resolved binding stored");
        policy.focusDial=ControllerCc{"Remote",1,8};
        check(controllerBindingShadowed(dropped->bindings()[0],policy),"mixer list can report imported or moved reservation as shadowed");
        check(dropped->unbindTrack(track,"volume") && dropped->bindings().empty(),"mixer Unbind removes matching binding");
        check(dropped->ops().undo().ok && dropped->bindings().size()==1,"Unbind inverse restores complete binding");
        dropped->release();
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
