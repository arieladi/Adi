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
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
