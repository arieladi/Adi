// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Export page's choices (EX-11; Live 12 manual 5.2.1, Export Audio/Video) and the render job they make
// (`render::Job`, the engine's renderer on main: `render::renderWav`). No JUCE: the page lays them out,
// the tests make jobs from them.
//
// Live's dialog, in its order: Rendered Track; Render Start and Render Length; Include Return and Main
// Effects; Render as Loop; Convert to Mono; Normalize; Create Analysis File; Sample Rate; the PCM
// encoding (Encode PCM, File Type, Bit Depth, Dither Options); MP3; Video. What the renderer cannot do yet
// is shown and says why (`unsupported`), never silently dropped.
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/render/queue.hpp"
#include "adi/ui/meters.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
struct ExportChoices {
    std::int64_t track = 0;                  // Rendered Track: 0 is Main, else the track's id
    std::int64_t startTicks = 0, lengthTicks = 0; // Render Start and Render Length
    bool includeReturnAndMain = true;        // Live's default; for a single track only
    bool renderAsLoop = false;
    bool convertToMono = false;
    bool normalize = false;
    bool createAnalysisFile = false;
    int sampleRate = 48000;
    bool encodePcm = true;
    std::string fileType = "WAV";            // WAV, AIFF, FLAC
    int bitDepth = 24;                       // 16, 24, 32 (float)
    std::string dither = "Triangular";       // No Dither, Rectangular, Triangular, POW-r 1, 2, 3
    bool encodeMp3 = false;
    bool video = false;
};
inline constexpr std::array<int, 6> kExportSampleRates{22050, 32000, 44100, 48000, 88200, 96000};
inline constexpr std::array<const char *, 3> kExportFileTypes{"WAV", "AIFF", "FLAC"};
inline constexpr std::array<int, 3> kExportBitDepths{16, 24, 32};
inline constexpr std::array<const char *, 6> kExportDithers{"No Dither", "Rectangular", "Triangular",
                                                            "POW-r 1", "POW-r 2", "POW-r 3"};
// Why a choice cannot be made yet, for the page to show beside it; empty when the renderer takes it.
inline std::string unsupported(const ExportChoices &c, const char *choice) {
    const std::string what(choice);
    // The renderer takes a track's own signal after its fader; a track through the returns and Main is
    // the engine's through-Main stems, not on main yet. Main always renders whole.
    if (what == "includeReturnAndMain" && c.track != 0 && c.includeReturnAndMain)
        return "A track through the returns and Main waits for the engine's stems (EX-07); turn Include Return "
               "and Main Effects off to render the track's own signal.";
    if (what == "renderAsLoop" && c.renderAsLoop)
        return "Render as Loop waits for the engine's export (EX-07).";
    if (what == "convertToMono" && c.convertToMono)
        return "Convert to Mono waits for the engine's export (EX-07).";
    if (what == "createAnalysisFile" && c.createAnalysisFile)
        return "Analysis files wait for the engine's export (EX-07).";
    if (what == "fileType" && c.fileType != "WAV")
        return c.fileType + " waits for the engine's encoders (EX-08); WAV renders now.";
    if (what == "dither" && c.dither.rfind("POW-r", 0) == 0)
        return "The shaped dithers wait for the engine's noise shaping (EX-06).";
    if (what == "encodeMp3" && c.encodeMp3)
        return "MP3 waits for the engine's encoders (EX-08).";
    if (what == "video" && c.video)
        return "Video export is not part of ADI yet (P2, ADR-0206 d4).";
    if (what == "encodePcm" && !c.encodePcm)
        return "With PCM off there is nothing to render until MP3 does.";
    return {};
}
// Every reason the choices cannot render now, in the dialog's order: empty when Export can go.
inline std::vector<std::string> exportRefusals(const ExportChoices &c) {
    std::vector<std::string> out;
    for (const char *choice : {"includeReturnAndMain", "renderAsLoop", "convertToMono", "createAnalysisFile",
                               "encodePcm", "fileType", "dither", "encodeMp3", "video"})
        if (auto why = unsupported(c, choice); !why.empty())
            out.push_back(std::move(why));
    if (c.lengthTicks <= 0)
        out.push_back("Render Length is zero: there is nothing to render.");
    return out;
}
// Render Length as Live's field shows it: bars, beats and sixteenths, each from 0, in the meter in force at
// the start (`barSpan`); and back, from text such as "4.0.0" (parts left out are 0). None for text that
// is not a length.
inline std::string exportLengthText(std::int64_t start, std::int64_t length, const std::vector<textproj::Meter> &meters) {
    const auto span = barSpan(start, length, meters);
    return std::to_string(span.bar) + "." + std::to_string(span.beat) + "." + std::to_string(span.sixteenth);
}
inline std::optional<std::int64_t> parseExportLength(std::string_view text, std::int64_t start,
                                               const std::vector<textproj::Meter> &meters) {
    std::array<std::int64_t, 3> parts{0, 0, 0};
    std::size_t n = 0;
    bool digit = false;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            if (n == parts.size() || parts[n] > 100000)
                return std::nullopt;
            parts[n] = parts[n] * 10 + (ch - '0');
            digit = true;
        } else if (ch == '.' || ch == ',' || ch == ':' || ch == ' ') {
            if (digit)
                ++n;
            digit = false;
        } else
            return std::nullopt;
    }
    const auto &m = meterAt(start, meters);
    if (m.denominator == 0 || m.barTicks() <= 0)
        return std::nullopt;
    const auto beatTicks = textproj::kWhole / static_cast<std::int64_t>(m.denominator);
    return parts[0] * m.barTicks() + parts[1] * beatTicks + parts[2] * (textproj::kWhole / 16);
}
// A tick on the project's tempo map as a sample at `rate`.
inline std::int64_t exportSample(const engine::TempoMap &tempo, std::int64_t ticks, double rate) {
    const double s = tempo.ticksToSeconds(ticks) * rate;
    if (!std::isfinite(s) || s < 0)
        return 0;
    return s >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) ? std::numeric_limits<std::int64_t>::max()
                                                                             : static_cast<std::int64_t>(std::llround(s));
}
// A file name as the renderer's name template: its stem with the template's braces doubled, so the
// name is taken as written.
inline std::string literalTemplate(const std::string &stem) {
    std::string out;
    for (const char ch : stem) {
        out += ch;
        if (ch == '{' || ch == '}')
            out += ch;
    }
    return out;
}
// The render job for `c`: the project's file, the chosen file (its folder and its name, the extension the
// renderer's), the range on the tempo map at the chosen rate, the source (Main, or a track post-fader),
// the PCM format and dither, Peak normalization to 0 dB as Live's Normalize.
inline render::Job exportJob(const ExportChoices &c, const std::filesystem::path &project,
                             const std::filesystem::path &file, const engine::TempoMap &tempo) {
    render::Job job;
    job.project = project;
    job.directory = file.parent_path();
    auto stem = file.stem().u8string();
    job.nameTemplate = literalTemplate(std::string(stem.begin(), stem.end()));
    job.session.sampleRate = static_cast<double>(c.sampleRate);
    job.session.exporting = true;
    job.session.offline = true;
    job.session.performanceState = false;
    job.beginSample = exportSample(tempo, c.startTicks, job.session.sampleRate);
    job.endSample = exportSample(tempo, c.startTicks + c.lengthTicks, job.session.sampleRate);
    job.source.trackId = c.track;
    job.source.tap = engine::OfflineTap::PostFader;
    job.encoding.format = c.bitDepth == 16 ? audio::WavFormat::Pcm16
                          : c.bitDepth == 32 ? audio::WavFormat::Float32
                                             : audio::WavFormat::Pcm24;
    job.encoding.dither = c.dither == "No Dither" ? audio::WavDither::None
                          : c.dither == "Rectangular" ? audio::WavDither::Rectangular
                                                      : audio::WavDither::Triangular;
    if (c.normalize) {
        job.normalization.mode = render::NormalizeMode::Peak;
        job.normalization.truePeakCeilingDb = 0;
    }
    // The page moves a file the user chose to replace to the trash before it renders; a file that appeared
    // since is not overwritten.
    job.collision = render::Collision::Fail;
    return job;
}
} // namespace adi::ui
