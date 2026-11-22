// SPDX-License-Identifier: AGPL-3.0-or-later
//
// WTS-01 (ADR-0248 d2, d12): one usable session from nothing, proven headless. The shared half of
// the two acceptance targets: the step report, a simulated audio driver that calls the document the
// way the app's device callback and timer do, and the transport requests the window builds itself.
//
// Every edit goes through the window's own builders and `ProjectDocument`'s calls; where the window
// builds a request itself, the code here copies it and names the place it copies.
#pragma once
#include "adi/ui/project_document.hpp"
#include "juce/device_model.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
namespace adi::wts01 {
inline constexpr double kRate = 48000;
inline constexpr std::int32_t kFrames = 256;
// One 4/4 bar at 120 BPM: two seconds, in ticks and in samples.
inline constexpr std::int64_t kBarTicks = 23063040;
inline constexpr std::int64_t kBarSamples = 96000;
inline constexpr double kPi = 3.141592653589793;
// One line per step: `WTS01 <step> PASS|FAIL|NOT_BUILT | observed | expected | missing piece, owner`.
// NOT_BUILT never fails the run; FAIL does. `ENGINE` lines are informative: the engine call a missing
// app path would use, never the step's verdict.
class Report {
  public:
    enum class Verdict { Pass, Fail, NotBuilt };
    void step(const char *id, Verdict v, const std::string &observed, const std::string &expected,
              const std::string &missing = {}) {
        const char *word = v == Verdict::Pass ? "PASS" : v == Verdict::Fail ? "FAIL" : "NOT_BUILT";
        if (v == Verdict::Fail)
            ++failures_;
        std::printf("WTS01 %-3s %-9s | %s | %s | %s\n", id, word, observed.c_str(), expected.c_str(),
                    missing.empty() ? "-" : missing.c_str());
        std::fflush(stdout);
    }
    void check(const char *id, bool ok, const std::string &observed, const std::string &expected) {
        step(id, ok ? Verdict::Pass : Verdict::Fail, observed, expected);
    }
    void engine(const char *id, const std::string &observed, const std::string &expected) {
        std::printf("WTS01 %-3s ENGINE    | %s | %s | informative, not the step's verdict\n", id, observed.c_str(),
                    expected.c_str());
        std::fflush(stdout);
    }
    int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};
inline std::string format(const char *f, ...) {
    std::array<char, 1024> text{};
    va_list args;
    va_start(args, f);
    std::vsnprintf(text.data(), text.size(), f, args);
    va_end(args);
    return text.data();
}
inline double dbOf(double gain) { return gain > 0 ? 20 * std::log10(gain) : -999; }
inline double gainOf(double db) { return std::pow(10.0, db / 20); }
// A stereo signal, one vector per channel.
struct Stereo {
    std::array<std::vector<float>, 2> c;
    std::size_t size() const noexcept { return c[0].size(); }
    float at(std::size_t ch, std::size_t i) const noexcept { return i < c[ch].size() ? c[ch][i] : 0.f; }
};
// A deterministic test input: a hashed counter (white noise in -0.4..0.4) on the left, a 220 Hz sine on
// the right, each a function of the stream sample alone, so a take can be checked against what was fed.
inline float fedInput(std::size_t channel, std::int64_t stream) noexcept {
    if (channel == 1)
        return static_cast<float>(.3 * std::sin(2 * kPi * 220 * static_cast<double>(stream) / kRate));
    auto z = static_cast<std::uint64_t>(stream) + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<float>(static_cast<double>(z >> 40) / 16777216.0 * .8 - .4);
}
// The audio device: two inputs, two outputs, 256-frame blocks at 48 kHz, its stream clock advancing. The
// app's timer (`ProjectDocument::tick`, the only caller of `synchronise()`, the take's landing and the
// parameter drain) runs every four blocks, about 21 ms. Clips are primed between blocks, as an offline
// driver must (`ClipPlayback::prime`); the real driver's read-ahead has the wall clock instead. Priming
// reads the transport as it stands, so it can only be more generous than the app's disk worker: a page it
// does not fetch before a callback, the app's asynchronous worker cannot have fetched either.
class Driver {
  public:
    explicit Driver(ui::ProjectDocument &doc) : doc_(&doc) {
        for (auto &v : in_)
            v.assign(static_cast<std::size_t>(kFrames), 0.f);
        for (auto &v : out_)
            v.assign(static_cast<std::size_t>(kFrames), 0.f);
    }
    void attach(ui::ProjectDocument &doc) { doc_ = &doc; }
    // What the inputs carry; none is silence.
    std::function<float(std::size_t, std::int64_t)> input;
    std::int64_t stream = 0;
    // Clip underrun samples over every callback so far. Each clip generation counts its own, and the
    // timer's rebuilds replace the generation, so the count is taken per callback.
    std::uint64_t underruns() const noexcept { return underruns_; }
    // One callback; its output is appended to `heard` when given.
    void block(Stereo *heard = nullptr) {
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < in_[c].size(); ++i)
                in_[c][i] = input ? input(c, stream + static_cast<std::int64_t>(i)) : 0.f;
        auto *clips = doc_->session().clips();
        if (clips)
            (void)clips->prime();
        const auto missedBefore = clips ? clips->underrunSamples() : 0;
        const std::array<const float *, 2> ins{in_[0].data(), in_[1].data()};
        const std::array<float *, 2> outs{out_[0].data(), out_[1].data()};
        engine::AudioIo io;
        io.in = ins.data();
        io.numIn = 2;
        io.out = outs.data();
        io.numOut = 2;
        io.frames = kFrames;
        io.streamTimeSamples = stream;
        doc_->process(io);
        if (clips && clips == doc_->session().clips())
            underruns_ += clips->underrunSamples() - missedBefore;
        stream += kFrames;
        if (++blocks_ % 4 == 0)
            doc_->tick(stream * 1000 / static_cast<std::int64_t>(kRate));
        if (heard)
            for (std::size_t c = 0; c < 2; ++c)
                heard->c[c].insert(heard->c[c].end(), out_[c].begin(), out_[c].end());
    }
    void run(int blocks, Stereo *heard = nullptr) {
        for (int b = 0; b < blocks; ++b)
            block(heard);
    }
    // The timer alone, as when the window's frame comes between callbacks.
    void frame() { doc_->tick(stream * 1000 / static_cast<std::int64_t>(kRate)); }

  private:
    ui::ProjectDocument *doc_;
    std::array<std::vector<float>, 2> in_, out_;
    std::uint64_t blocks_ = 0;
    std::uint64_t underruns_ = 0;
};
// The window's Stop (`AdiRootComponent`'s PlayStop/Stop, ui_shell.cpp: a recording is stopped first, then
// a `transport.stop` op and the mailbox's Stop).
inline bool windowStop(ui::ProjectDocument &doc) {
    if (doc.recording())
        doc.stopRecording();
    OpRequest request;
    request.opType = "transport.stop";
    request.payload = Payload::object();
    return doc.ops().submit(std::move(request)).ok && doc.mailbox().post(ui::TransportMailbox::Command::Stop);
}
// The window's Play from the insert marker (the same case: a `transport.play` op, `locate(insert)`, then Play).
// `locate` posts the tick as a sample at the device's rate (ui_shell.cpp, `AdiRootComponent::locate`).
inline bool windowPlayFrom(ui::ProjectDocument &doc, std::int64_t tick) {
    OpRequest request;
    request.opType = "transport.play";
    request.payload = Payload::object();
    const auto current = doc.view().current();
    if (!current || !current->tempo || !doc.ops().submit(std::move(request)).ok)
        return false;
    const auto rate = doc.mailbox().sampleRate() > 0 ? doc.mailbox().sampleRate() : kRate;
    return doc.mailbox().post(ui::TransportMailbox::Command::Locate,
                              static_cast<std::int64_t>(current->tempo->ticksToSeconds(tick) * rate)) &&
           doc.mailbox().post(ui::TransportMailbox::Command::Play);
}
// The window's submit (`AdiRootComponent::submitEdits`): a batch from the window's builders, one undo step.
inline bool submitEdits(ui::ProjectDocument &doc, std::vector<OpRequest> ops, std::string *error = nullptr) {
    if (ops.empty())
        return true;
    const auto result = doc.ops().submit(std::move(ops));
    if (error)
        *error = result.error;
    return result.ok && doc.synchronise();
}
inline std::int64_t scalar(ui::ProjectDocument &doc, const std::string &sql) {
    return doc.store().db().execAndGet(sql).getInt64();
}
inline double real(ui::ProjectDocument &doc, const std::string &sql) {
    return doc.store().db().execAndGet(sql).getDouble();
}
// The least-squares gain of `heard` against `source` over [from, to) of both channels.
inline double fitGain(const Stereo &heard, const Stereo &source, std::size_t from, std::size_t to) {
    double num = 0, den = 0;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = from; i < to; ++i) {
            num += static_cast<double>(heard.at(c, i)) * source.at(c, i);
            den += static_cast<double>(source.at(c, i)) * source.at(c, i);
        }
    return den > 0 ? num / den : 0;
}
// The largest difference over [from, to) of both channels; `where` gets its frame.
inline double maxDiff(const Stereo &a, const Stereo &b, std::size_t from, std::size_t to, std::size_t *where = nullptr) {
    double worst = 0;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = from; i < to; ++i) {
            const double d = std::abs(static_cast<double>(a.at(c, i)) - b.at(c, i));
            if (d > worst) {
                worst = d;
                if (where)
                    *where = i;
            }
        }
    return worst;
}
inline double peak(const Stereo &a, std::size_t from, std::size_t to) {
    double worst = 0;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = from; i < std::min(to, a.size()); ++i)
            worst = std::max(worst, std::abs(static_cast<double>(a.at(c, i))));
    return worst;
}
// The Export page's loader rule (ui_main.cpp, `startRender`): only ADI's own devices render; another
// maker's plug-in waits for the engine's render worker (EX-11).
inline constexpr const char *kExportRefusal =
    "A project with VST3 or CLAP plug-ins renders once the engine's render worker is on main (EX-11).";
inline engine::DeviceLoader exportLoader(engine::DeviceLoader made) {
    return [made = std::move(made)](const engine::DeviceRequest &rq, std::string &error) {
        if (rq.ref == nullptr || rq.ref->format != "internal") {
            error = kExportRefusal;
            return std::unique_ptr<device::DeviceInstance>{};
        }
        return made(rq, error);
    };
}
// The Export page's check before it asks for a file (ui_main.cpp, `chooseExportFile`).
inline bool exportPrecheckRefuses(ui::ProjectDocument &doc) {
    return scalar(doc, "SELECT count(*) FROM devices d JOIN plugin_refs r ON r.id = d.plugin_ref_id "
                       "WHERE r.format <> 'internal'") > 0;
}
// A query's rows as one text, for comparing a project before closing and after reopening.
inline std::string rowsText(ui::ProjectDocument &doc, const std::string &sql, int *count = nullptr) {
    SQLite::Statement q(doc.store().db(), sql);
    std::string out;
    int rows = 0;
    while (q.executeStep()) {
        ++rows;
        for (int i = 0; i < q.getColumnCount(); ++i)
            out += q.getColumn(i).getText() + std::string(i + 1 < q.getColumnCount() ? "," : ";");
    }
    if (count)
        *count = rows;
    return out;
}
// The first frame above `floor` on either channel; the size when none is.
inline std::size_t firstSound(const Stereo &a, float floor = 1e-6f) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a.at(0, i)) > floor || std::abs(a.at(1, i)) > floor)
            return i;
    return a.size();
}
inline bool finite(const Stereo &a) {
    for (const auto &ch : a.c)
        for (const float v : ch)
            if (!std::isfinite(v))
                return false;
    return true;
}
} // namespace adi::wts01
