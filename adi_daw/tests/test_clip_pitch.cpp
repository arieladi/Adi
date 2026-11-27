// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/clip_pitch.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_render.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::media;
int checks = 0, failures = 0;

void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

void run() {
    constexpr std::uint32_t rate = 48000, frames = 24000;
    test::TempDirectory temp{"clip_pitch", "analysis"};
    const auto path = temp.path() / "voice.wav";
    std::array<float, frames> mono{};
    {
        std::array<float, 2 * frames> pcm{};
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto phase = 2 * std::numbers::pi * 220 * static_cast<double>(frame) / rate;
            pcm[2 * frame] = static_cast<float>(0.6 * std::sin(phase));
            pcm[2 * frame + 1] = pcm[2 * frame] * 0.5f;
            mono[frame] = (pcm[2 * frame] + pcm[2 * frame + 1]) * 0.5f;
        }
        audio::WavWriter writer(path, rate, 2, audio::WavFormat::Float32);
        writer.write(pcm.data(), frames);
        writer.close();
    }

    std::string error;
    StoreError storeError{};
    auto store = Store::create(temp.path() / "pitch.adi", storeError);
    if (!store) throw std::runtime_error(toString(storeError));
    auto& database = store->db();
    database.exec("INSERT INTO tracks(id,kind) VALUES(1,'audio');"
                  "INSERT INTO clips(id,track_id,kind,pos_ticks) VALUES"
                  "(11,1,'audio',0),(12,1,'audio',0),(13,1,'midi',0)");
    auto imported = importMedia(*store, path, 1, 0);
    check(imported.ok, "source is imported into the content-addressed media pool");
    if (!imported.ok) throw std::runtime_error(imported.error);
    database.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,reverse) "
                  "VALUES(11,1,1000,21000,0),(12,1,5000,5000,1)");

    audio::DecodeCache decoded(temp.path() / "decoded");
    AnalysisCache analysed(temp.path() / "analysed");
    const AnalysisIdentity identity{blake3File(path).hex, rate, 2, frames};
    const AnalysisSection other{61, 2, {std::byte{19}}};
    check(analysed.update(identity, "FUTR", other, error) == AnalysisCacheProblem::None,
          "other analysis is present for the source");
    const auto start = std::chrono::steady_clock::now();
    const auto fresh = buildClipPitchAnalysis(*store, 11, {}, &decoded, &analysed);
    const auto ready = std::chrono::steady_clock::now();
    check(fresh && fresh.data->source == identity && fresh.data->sourceStartFrame == 1000 &&
          fresh.data->sourceEndFrame == 22000 && !fresh.data->reversed,
          "audio clip resolves exact source and original-frame read window");
    if (!fresh) throw std::runtime_error(fresh.detail.value_or("pitch analysis failed"));
    check(fresh.data->analysis.notes.size() == 1 &&
          std::abs(fresh.data->analysis.notes.front().pitchCents - 5700) < 5,
          "decoded stereo file analyzes as a 220 Hz single voice within 5 cents");
    if (fresh.data->analysis.notes.size() != 1) throw std::runtime_error("expected one source note");
    audio::PitchEditState edit;
    const std::array commands{audio::PitchEditCommand{0, audio::PitchSetShift{100}}};
    check(audio::applyPitchEdits(fresh.data->analysis, edit, commands) == audio::PitchEditError::None,
          "the analyzed clip note can be edited through the typed seam");
    const auto rendered = audio::renderPitchEdits(mono, fresh.data->analysis, edit);
    check(rendered && rendered.samples->size() == mono.size() &&
          !std::equal(rendered.samples->begin(), rendered.samples->end(), mono.begin()),
          "the offline renderer consumes the edit and original analysis");
    auto sections = analysed.read(identity);
    check(sections && sections.data->sections.count("PNOT") == 1 &&
          sections.data->sections.at("FUTR") == other,
          "pitch is written into the same ADIA envelope as other sections");
    if (!sections || !sections.data->sections.contains("PNOT"))
        throw std::runtime_error("pitch section missing from ADIA");

    auto broken = sections.data->sections.at("PNOT");
    broken.bytes.pop_back();
    check(analysed.update(identity, "PNOT", broken, error) == AnalysisCacheProblem::None,
          "cache fixture contains a truncated section within a valid envelope");
    const auto builtAgain = buildClipPitchAnalysis(*store, 11, {}, &decoded, &analysed);
    const auto rebuilt = analysed.read(identity);
    check(builtAgain && builtAgain.data->analysis.notes.size() == fresh.data->analysis.notes.size() &&
          rebuilt && rebuilt.data->sections.at("PNOT") == sections.data->sections.at("PNOT"),
          "corrupt pitch payload is rebuilt byte-for-byte from verified audio");

    database.exec("UPDATE media_files SET sample_rate=NULL WHERE id=1");
    auto legacy = sections.data->sections.at("PNOT");
    constexpr std::size_t firstPitchOffset = 8 + 2 * 8;
    auto word = std::bit_cast<std::uint64_t>(5701.0);
    for (std::size_t index = 0; index < 8; ++index) {
        legacy.bytes[firstPitchOffset + index] = static_cast<std::byte>(word & 0xffu);
        word >>= 8;
    }
    check(analysed.update(identity, "PNOT", legacy, error) == AnalysisCacheProblem::None,
          "legacy cache fixture has a distinguishable valid note");
    const auto withoutPoolRate = buildClipPitchAnalysis(*store, 11, {}, &decoded, &analysed);
    check(withoutPoolRate && withoutPoolRate.data->analysis.notes.size() == 1 &&
          withoutPoolRate.data->analysis.notes.front().pitchCents == 5701,
          "a legacy media row reuses pitch analysis after probing decoded dimensions");
    database.exec("UPDATE media_files SET sample_rate=48000 WHERE id=1");
    check(analysed.update(identity, "PNOT", sections.data->sections.at("PNOT"), error) ==
          AnalysisCacheProblem::None, "restore the measured cache fixture");

    {
        audio::WavWriter replacement(path, rate, 2, audio::WavFormat::Float32);
        const std::array<float, 2 * frames> silence{};
        replacement.write(silence.data(), frames);
        replacement.close();
    }
    check(buildClipPitchAnalysis(*store, 11, {}, &decoded, nullptr).problem == ClipPitchProblem::InvalidSource,
          "an uncached analysis rejects a source replaced at the same path");
    std::filesystem::remove(path);
    const auto warmStart = std::chrono::steady_clock::now();
    const auto shared = buildClipPitchAnalysis(*store, 12, {}, &decoded, &analysed);
    const auto warmReady = std::chrono::steady_clock::now();
    check(shared && shared.data->sourceStartFrame == 5000 && shared.data->sourceEndFrame == 10000 &&
          shared.data->reversed && shared.data->source == fresh.data->source &&
          shared.data->analysis.notes.size() == 1 &&
          shared.data->analysis.notes.front().pitchCents == fresh.data->analysis.notes.front().pitchCents,
          "second clip reads the same source pitch even when the file is missing");
    using Milliseconds = std::chrono::duration<double, std::milli>;
    std::printf("0.5 s stereo source: first analysis %.2f ms, cache reuse %.2f ms\n",
                Milliseconds(ready - start).count(), Milliseconds(warmReady - warmStart).count());
    check(buildClipPitchAnalysis(*store, 13, {}, &decoded, &analysed).problem == ClipPitchProblem::NotAudio,
          "MIDI clips cannot request audio analysis");
    database.exec("UPDATE audio_clips SET src_len_frames=24000 WHERE clip_id=12");
    check(buildClipPitchAnalysis(*store, 12, {}, &decoded, &analysed).problem == ClipPitchProblem::InvalidSource,
          "stored clip window cannot extend beyond the source");
    std::stop_source cancelled;
    cancelled.request_stop();
    check(buildClipPitchAnalysis(*store, 11, cancelled.get_token(), &decoded, &analysed).problem ==
          ClipPitchProblem::Cancelled, "cancelled worker returns no analysis");
    ClipPitchProblem realtime{};
    const auto fileIo = audio::callbackFileIo.load();
    {
        audio::CallbackScope callback;
        realtime = buildClipPitchAnalysis(*store, 11).problem;
    }
    check(realtime == ClipPitchProblem::Realtime && fileIo == audio::callbackFileIo.load(),
          "callback refuses before cache initialization or I/O");
}
} // namespace

int main() {
    try { run(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
