// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/telemetry.hpp"

#include "adi/engine/main_loudness.hpp"
#include "adi/engine/mixer.hpp"

#include <atomic>
#include <cmath>
#include <utility>

namespace adi::control {
namespace {

/// Two decimals; -0.0 (a level just under 0 dB, say) comes back as 0.0 (R5.4).
double round2(double x) {
    const double r = std::round(x * 100.0) / 100.0;
    return r == 0.0 ? 0.0 : r;
}

/// Linear to dBFS; silence (or anything not positive and finite) is null.
Json dbfs(float linear) {
    if (!(linear > 0.0f) || !std::isfinite(linear)) return nullptr;
    return round2(20.0 * std::log10(static_cast<double>(linear)));
}

/// A loudness readout; -infinity (silence, or a window not yet warmed up) is null.
Json lufs(float v) {
    if (!std::isfinite(v)) return nullptr;
    return round2(static_cast<double>(v));
}

Json unavailableList() {
    const auto row = [](const char* what, const char* why, const char* when) {
        return Json{{"what", what}, {"why", why}, {"when", when}};
    };
    return Json::array({
        row("masking pairs",
            "the masking kernel exists (dsp::measureMasking) but needs spectra from the visual "
            "worker (MX-30) or analyze.masking (AGT-07)",
            "M4 / P2"),
        row("correlation and phase", "no running correlation meter; MX-30's worker brings it", "M4"),
        row("spectra", "one visual worker per process (MX-30, ADR-0195 d5)", "M4"),
        row("crest factor and dynamics", "no code yet", "unplanned"),
        row("per-track LUFS",
            "live loudness per bus is rejected (ADR-0203 d14); offline as analyze.loudness (AGT-07)",
            "P2"),
        row("loudness range (LRA), max momentary and short-term", "ADR-0219's one loudness meter", "M4"),
        row("held peaks and overs", "N48: a held peak per strip", "open"),
        row("gain reduction, CPU load and xruns", "no code yet", "unplanned"),
        row("the AI Mix Radar's findings", "ADR-0209 section 6, MX-21", "P2"),
    });
}

}  // namespace

std::shared_ptr<const TelemetryFrame> sample(const MeterTaps& taps, std::uint64_t sequence,
                                             std::int64_t nowMs) {
    auto frame = std::make_shared<TelemetryFrame>();
    frame->sequence = sequence;
    frame->capturedMs = nowMs;
    frame->strips.reserve(taps.strips.size());
    for (const auto& [track, meter] : taps.strips) {
        if (!meter) continue;
        // The same relaxed loads the mixer panel makes every frame.
        frame->strips.push_back({track, meter->peak.load(std::memory_order_relaxed),
                                 meter->rms.load(std::memory_order_relaxed)});
    }
    if (taps.main) {
        LoudnessReading m;
        m.momentaryLufs = taps.main->momentaryLufs.load(std::memory_order_relaxed);
        m.shortTermLufs = taps.main->shortTermLufs.load(std::memory_order_relaxed);
        m.integratedLufs = taps.main->integratedLufs.load(std::memory_order_relaxed);
        m.truePeakDb = taps.main->truePeakDb.load(std::memory_order_relaxed);
        frame->main = m;
    }
    if (taps.transport) frame->transport = taps.transport();
    return frame;
}

void TelemetryBoard::publish(std::shared_ptr<const TelemetryFrame> frame) {
    std::lock_guard lock(mutex_);
    frame_ = std::move(frame);
}

std::shared_ptr<const TelemetryFrame> TelemetryBoard::latest() const {
    std::lock_guard lock(mutex_);
    return frame_;
}

Json telemetryJson(const TelemetryFrame* frame, std::int64_t nowMs, bool live) {
    Json out = Json::object();
    out["unavailable"] = unavailableList();
    if (!live) {
        out["available"] = false;
        out["reason"] =
            "no audio engine runs here: adi-mcp --project reads a file. Live meters come from a "
            "running ADI (adi-mcp --connect, with the control listener of CTL-04, not built yet).";
        return out;
    }
    if (!frame) {
        out["available"] = false;
        out["reason"] = "the engine has not published a frame yet (audio stopped, or just started)";
        return out;
    }
    out["available"] = true;
    out["sequence"] = frame->sequence;
    out["ageMs"] = nowMs >= frame->capturedMs ? nowMs - frame->capturedMs : 0;
    if (frame->transport) {
        out["transport"] = {{"playing", frame->transport->playing},
                            {"positionSamples", frame->transport->positionSamples},
                            {"sampleRate", frame->transport->sampleRate}};
    } else {
        out["transport"] = nullptr;
    }
    Json strips = Json::array();
    for (const auto& s : frame->strips) {
        strips.push_back({{"track", s.track},
                          {"peak", static_cast<double>(s.peak)},
                          {"peakDbfs", dbfs(s.peak)},
                          {"rms", static_cast<double>(s.rms)},
                          {"rmsDbfs", dbfs(s.rms)}});
    }
    out["strips"] = std::move(strips);
    if (frame->main) {
        out["main"] = {{"momentaryLufs", lufs(frame->main->momentaryLufs)},
                       {"shortTermLufs", lufs(frame->main->shortTermLufs)},
                       {"integratedLufs", lufs(frame->main->integratedLufs)},
                       {"truePeakDbtp", lufs(frame->main->truePeakDb)}};
    } else {
        out["main"] = nullptr;
    }
    out["semantics"] = {
        {"strips",
         "sample peak (not true peak) and RMS, post-fader, as last stored by the audio thread: "
         "two independent reads, possibly from different callbacks or from part of one, so rms may "
         "exceed peak; not held, and a poll sees at most one callback in many (about 1 in 9 at "
         "10 Hz), so it cannot show that a strip never clipped"},
        {"main",
         "Main's momentary, short-term and integrated LUFS and held true peak: four independent "
         "reads, not a coherent set, and not a certified EBU R128 measurement; null is silence or a "
         "window not yet warmed up; when integration started is not known"},
        {"ageMs", "how long ago the frame was sampled on the message thread"}};
    return out;
}

}  // namespace adi::control
