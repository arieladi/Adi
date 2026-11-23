// SPDX-License-Identifier: GPL-3.0-or-later
//
// DEV-28 / LD6 -- the one loudness meter against ffmpeg's ebur128 filter.
//
// test_loudness_oracle.cpp holds dsp::LoudnessMeter to libebur128, whose
// loudness range hops the short-term window at 1 Hz; Tech 3342 asks for at
// least 10 Hz, which is what ADI's meter does, and what ffmpeg's filter does.
// So this suite is the outside check on ADI's 10 Hz range (and on I with it):
// each signal is written as a 32-bit float WAV, measured by the ffmpeg program
// on PATH (test-only: nothing of it is linked), and its last integrated and
// range readings (lavfi.r128.I and .LRA, to a thousandth) held to ADI's.
//
// ffmpeg's filter measures at 48 kHz only (it resamples anything else), so
// the signals are 48 kHz: Tech 3342's cases 1-4, and a speech-like noise
// programme with level steps and pauses, so both gates do work.
//
// Without an ffmpeg on PATH the suite reports itself skipped (exit 77, which
// CTest shows as Not Run), unless ADI_REQUIRE_ORACLES=1 is set: then it fails.
// No CI leg sets it yet; win adds one with ffmpeg installed.

#include "adi/dsp/loudness.hpp"

#include "temp_directory.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Meter = adi::dsp::LoudnessMeter;
using Signal = std::vector<std::vector<double>>;   // [channel][frame]

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;
/// The plan asks for 0.1 LU. Measured: the range agrees to 0.001 on every
/// case, and I by 0.005 to 0.007 (ffmpeg's integrated histogram), so the suite
/// holds I to 0.02 and the range to 0.01: a wrong percentile or gate that
/// moves a reading by a tenth would otherwise pass where it moves it by less.
constexpr double kAgreeI = 0.02, kAgreeRange = 0.01;

int checks = 0, failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
    }
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

// std::getenv, with MSVC's C4996 silenced at one site (test_wav_file.cpp's helper).
const char* envVar(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

std::string quote(const fs::path& path) {
#if defined(_WIN32)
    return "\"" + path.string() + "\"";
#else
    std::string result = "'";
    for (const char c : path.string()) {
        result += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return result + "'";
#endif
}

/// Runs a shell command line; its output goes to `log`.
int run(const std::string& line, const fs::path& log) {
    std::string command = line + " > " + quote(log) + " 2>&1";
#if defined(_WIN32)
    command = "\"" + command + "\"";
#endif
    return std::system(command.c_str());
}

/// 32-bit float WAV, little-endian whatever the host.
void writeWav(const fs::path& path, const Signal& x) {
    const auto channels = static_cast<std::uint32_t>(x.size());
    const auto frames = static_cast<std::uint32_t>(x[0].size());
    const std::uint32_t dataBytes = frames * channels * 4u;
    std::vector<unsigned char> bytes;
    const auto put = [&](std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i) {
            bytes.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xffu));
        }
    };
    const auto tag = [&](const char* t) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<unsigned char>(t[i]));
        }
    };
    tag("RIFF");
    put(36u + dataBytes, 4);
    tag("WAVE");
    tag("fmt ");
    put(16u, 4);
    put(3u, 2);   // IEEE float
    put(channels, 2);
    put(static_cast<std::uint32_t>(kRate), 4);
    put(static_cast<std::uint32_t>(kRate) * channels * 4u, 4);
    put(channels * 4u, 2);
    put(32u, 2);
    tag("data");
    put(dataBytes, 4);
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const auto sample = static_cast<float>(x[c][f]);
            std::uint32_t word = 0;
            static_assert(sizeof word == sizeof sample);
            std::memcpy(&word, &sample, sizeof word);
            put(word, 4);
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// A 1 kHz sine on both channels, in segments of `seconds` at `dbfs` peak.
Signal tones(const std::vector<std::pair<double, double>>& segments) {
    Signal x(2);
    double t = 0;
    for (const auto& [seconds, dbfs] : segments) {
        const auto n = static_cast<std::size_t>(std::llround(seconds * kRate));
        const double a = std::isfinite(dbfs) ? std::pow(10.0, dbfs / 20.0) : 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double v = a * std::sin(2 * kPi * 1000.0 * (t + static_cast<double>(i) / kRate));
            x[0].push_back(v);
            x[1].push_back(v);
        }
        t += static_cast<double>(n) / kRate;
    }
    return x;
}

/// Speech-like: noise in syllables of 0.18-0.55 s at -32 to -14 dBFS, with
/// pauses of 0.1-0.9 s, and three seconds of near-silence twice.
Signal speechLike(double seconds) {
    std::uint32_t s = 20261120u;
    const auto uniform = [&] {   // [0, 1)
        s = s * 1664525u + 1013904223u;
        return static_cast<double>(s >> 8) / 16777216.0;
    };
    const auto n = static_cast<std::size_t>(seconds * kRate);
    Signal x(2, std::vector<double>(n, 0.0));
    std::size_t at = 0;
    double low = 0.0;   // a one-pole low-pass, so it is not white
    while (at < n) {
        const bool hush = at > n / 3 && at < n / 3 + static_cast<std::size_t>(3 * kRate);
        const double level = hush ? -60.0 : -32.0 + 18.0 * uniform();
        const auto syllable = static_cast<std::size_t>((0.18 + 0.37 * uniform()) * kRate);
        const double gain = std::pow(10.0, level / 20.0);
        for (std::size_t i = 0; i < syllable && at < n; ++i, ++at) {
            low += 0.3 * ((2.0 * uniform() - 1.0) - low);
            x[0][at] = gain * low;
            x[1][at] = gain * 0.8 * low;
        }
        at += static_cast<std::size_t>((0.1 + 0.8 * uniform()) * kRate);
    }
    return x;
}

struct Readings {
    double integrated = 0.0, range = 0.0;
    bool ok = false;
};

Readings measuredByAdi(const Signal& x) {
    Meter meter;
    meter.prepare(kRate, std::vector<double>{1.0, 1.0});
    const double* p[2] = {x[0].data(), x[1].data()};
    meter.process(p, static_cast<int>(x[0].size()));
    const auto r = meter.readings();
    return {r.integrated, r.range, true};
}

/// The last value of `key` in ametadata's print file ("lavfi.r128.I=-23.000").
bool lastValue(const std::string& text, const std::string& key, double& value) {
    const auto at = text.rfind(key + "=");
    if (at == std::string::npos) {
        return false;
    }
    value = std::strtod(text.c_str() + at + key.size() + 1, nullptr);
    return std::isfinite(value);
}

Readings measuredByFfmpeg(const fs::path& dir, const Signal& x) {
    writeWav(dir / "in.wav", x);
    std::error_code ignored;
    fs::remove(dir / "out.txt", ignored);
    // Run in `dir`, so the filter's file option is a bare name (a Windows
    // path's drive colon would need escaping in a filter graph).
    const std::string line = "cd " +
#if defined(_WIN32)
                             std::string("/d ") +
#endif
                             quote(dir) +
                             " && ffmpeg -nostdin -hide_banner -nostats -loglevel error -y -i in.wav -af "
                             "\"ebur128=metadata=1,ametadata=mode=print:file=out.txt\" -f null -";
    if (run(line, dir / "ffmpeg.log") != 0) {
        return {};
    }
    std::ifstream in(dir / "out.txt", std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    Readings r;
    r.ok = lastValue(text.str(), "lavfi.r128.I", r.integrated) && lastValue(text.str(), "lavfi.r128.LRA", r.range);
    return r;
}

/// CTest's SKIP_RETURN_CODE for this suite (cmake/devices/loudness.cmake).
constexpr int kSkipped = 77;

int suite() {
    adi::test::TempDirectory tmp("loudness_ffmpeg", "wav");
    const bool required = [] {
        const char* v = envVar("ADI_REQUIRE_ORACLES");
        return v != nullptr && std::string(v) == "1";
    }();
    if (run("ffmpeg -nostdin -hide_banner -version", tmp.path() / "version.log") != 0) {
        if (!required) {
            std::printf("  skipped: no ffmpeg on PATH (ADI_REQUIRE_ORACLES=1 makes this a failure)\n");
            return kSkipped;
        }
        check(false, "ADI_REQUIRE_ORACLES=1, and there is no ffmpeg on PATH");
        std::printf("\nFAILED -- %d checks, %d failure(s)\n", checks, failures);
        return 1;
    }

    const double inf = std::numeric_limits<double>::infinity();
    struct Case {
        std::string name;
        Signal x;
    };
    std::vector<Case> cases;
    cases.push_back({"Tech 3342 case 1 (-20 / -30 dBFS)", tones({{20, -20}, {20, -30}})});
    cases.push_back({"Tech 3342 case 2 (-20 / -15 dBFS)", tones({{20, -20}, {20, -15}})});
    cases.push_back({"Tech 3342 case 3 (-40 / -20 dBFS)", tones({{20, -40}, {20, -20}})});
    cases.push_back({"Tech 3342 case 4 (-50 ... -20 ... -50)", tones({{20, -50}, {20, -35}, {20, -20}, {20, -35}, {20, -50}})});
    cases.push_back({"speech-like noise with pauses, 40 s", speechLike(40)});
    cases.push_back({"a tone after silence (the gates)", tones({{4, -inf}, {10, -26}, {4, -inf}})});
    for (const auto& c : cases) {
        const auto a = measuredByAdi(c.x);
        const auto f = measuredByFfmpeg(tmp.path(), c.x);
        char line[256];
        std::snprintf(line, sizeof line, "%s: I %.3f / %.3f LUFS, LRA %.3f / %.3f LU (ADI / ffmpeg)", c.name.c_str(),
                      a.integrated, f.integrated, a.range, f.range);
        check(f.ok && std::abs(a.integrated - f.integrated) <= kAgreeI && std::abs(a.range - f.range) <= kAgreeRange,
              std::string(line) + (f.ok ? "" : " -- ffmpeg gave no reading"));
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_loudness_ffmpeg_tests -- DEV-28 / LD6, against ffmpeg's ebur128 filter\n");
    try {
        return suite();
    } catch (const std::exception& e) {
        std::printf("FAIL  the suite threw: %s\n", e.what());
        return 1;
    }
}
