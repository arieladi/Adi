# SPDX-License-Identifier: GPL-3.0-or-later
"""Plant each defect in a copy of the core, build the unit test against it, and report which checks fire.
A plant counts only if its text was found exactly once (the file changed) and the build succeeded."""
import os, re, shutil, subprocess, sys
W = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
S = os.path.join(os.environ.get("ADI_LIVE_PROBE_WORK", os.path.expanduser("~/adi-live-probe")), "plants")
PLANTS = [
 ("downsampler pairs the current second sample", "const float b = odd.step(held);", "const float b = odd.step(second);"),
 ("Butterworth Q 1/sqrt2 instead of 0.707", "constexpr double kQ = 0.707;", "constexpr double kQ = 0.70710678118654752;"),
 ("no flush-to-zero", "[[maybe_unused]] const FlushDenormals flush;", ""),
 ("mid gain dB/20 instead of dB*0.05f", "midGain = 1.0f - std::pow(10.0f, decibels * 0.05f);", "midGain = 1.0f - std::pow(10.0f, decibels / 20.0f);"),
 ("peak detector |L+R|/2 instead of (|L|+|R|)/2", "const float s = std::fabs(dl) + std::fabs(dr);", "const float s = std::fabs(dl + dr);"),
 ("cap 7 octaves instead of 6", "constexpr float kGainCapLog2 = 6.0f;", "constexpr float kGainCapLog2 = 7.0f;"),
 ("envelope starts at 0", "constexpr float kEnvelopeStart = 0.01f;", "constexpr float kEnvelopeStart = 0.0f;"),
 ("envelope update d + c*(env-d)", "env = kept + taken;", "env = d + coef * (env - d);"),
 ("knob gain dB/20f", "return std::pow(10.0f, static_cast<float>(db) * 0.05f);", "return std::pow(10.0f, static_cast<float>(db) / 20.0f);"),
 ("S/C mix dry = cos", "scDry_ = std::sqrt(1.0f - wetSquared);", "scDry_ = static_cast<float>(std::cos(static_cast<double>(mix) * kPi / 2));"),
 ("S/C ramp 1.6 ms", "std::lround(rate_ * 0.0015)", "std::lround(rate_ * 0.0016)"),
 ("Listen only while S/C runs", "const bool listen = listenOn;", "const bool listen = sidechain && listenOn;"),
 ("S/C gain off only at -70 dB", "constexpr double kSidechainGainOffDb = -69.7;", "constexpr double kSidechainGainOffDb = -70.0;"),
 ("Peak/RMS switch restarts the envelope", "d.env = v >= 0.5 ? std::sqrt(d.env) : d.env * d.env;", "d.env = d.env;"),
 ("thresholds over 6.0206 dB/oct", "constexpr float kOctavesPerDb = 1.0f / MultibandDynamics::kDbPerOctave;", "constexpr float kOctavesPerDb = 1.0f / 6.0206f;"),
 ("knee curve 6.0206/40", "constexpr float kKneeCurve = MultibandDynamics::kDbPerOctave / 40.0f;", "constexpr float kKneeCurve = 6.0206f / 40.0f;"),
 ("fast log2: minimax first coefficient", "constexpr float kLog1 = 0x1.6bbdc8p+0f,", "constexpr float kLog1 = 0x1.6c4f2ap+0f,"),
 ("solo ignores band existence", "if (!exists[bi] || (anySolo && !solo[bi]))", "if ((anySolo && !solo[bi]) || (!anySolo && !exists[bi]))"),
 ("RMS level by sqrtf", "const float level = fastLog2(peak ? env : rmsAmplitude(env));", "const float level = fastLog2(peak ? env : std::sqrt(env));"),
 ("inactive band adds to Listen", "if (!active[bi]) {", "if (!active[bi] && !listen) {"),
 ("S/C gain without Live's fader table", "scGain_ = sidechainGain(at(SidechainGain));", "scGain_ = dbToGain(at(SidechainGain));"),
]
def run():
    src = open(f"{W}/src/adi/dsp/multiband_dynamics.cpp").read()
    os.makedirs(f"{S}/core/adi/dsp", exist_ok=True)
    shutil.copy(f"{W}/src/adi/dsp/multiband_dynamics.hpp", f"{S}/core/adi/dsp/")
    report = []
    for name, old, new in PLANTS:
        n = src.count(old)
        if n != 1:
            report.append((name, f"NOT PLANTED (found {n}x)", [])); continue
        open(f"{S}/core/adi/dsp/multiband_dynamics.cpp", "w").write(src.replace(old, new))
        b = subprocess.run(["clang++", "-std=c++20", "-O2", "-ffp-contract=off", "-I", f"{S}/core", "-I", f"{W}/src",
                            f"{W}/tests/test_multiband_dynamics.cpp", f"{S}/core/adi/dsp/multiband_dynamics.cpp",
                            "-o", f"{S}/t_plant"], capture_output=True, text=True)
        if b.returncode:
            report.append((name, "BUILD FAILED", [b.stderr[:300]])); continue
        r = subprocess.run([f"{S}/t_plant"], capture_output=True, text=True)
        fails = [l[5:] for l in r.stdout.splitlines() if l.startswith("FAIL ") and "checks" not in l]
        report.append((name, "CAUGHT" if fails else "NOT CAUGHT", fails))
    # restore check: the untouched core must pass
    open(f"{S}/core/adi/dsp/multiband_dynamics.cpp", "w").write(src)
    subprocess.run(["clang++", "-std=c++20", "-O2", "-ffp-contract=off", "-I", f"{S}/core", "-I", f"{W}/src",
                    f"{W}/tests/test_multiband_dynamics.cpp", f"{S}/core/adi/dsp/multiband_dynamics.cpp", "-o", f"{S}/t_plant"], check=True)
    last = subprocess.run([f"{S}/t_plant"], capture_output=True, text=True).stdout.strip().splitlines()[-1]
    for name, status, fails in report:
        print(f"{status:12s} {name}" + ("" if not fails else f"  [{len(fails)}] " + " | ".join(fails[:3])))
    print("restored:", last)
run()
