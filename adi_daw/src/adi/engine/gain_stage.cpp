// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/session.hpp"
#include "adi/dsp/auto_gain.hpp"
#include "adi/dsp/true_peak.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace adi::engine {
namespace {
struct Filter {
    dsp::Biquad c;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double run(double x) {
        const double y = c.b0*x + c.b1*x1 + c.b2*x2 - c.a1*y1 - c.a2*y2;
        x2=x1; x1=x; y2=y1; y1=y;
        return y;
    }
};
// Offline only. EBU R128: 400 ms windows at 100 ms hops, absolute -70 LUFS
// then relative -10 LU. RMS uses the same gates in dBFS and channel MEAN,
// whereas stereo LUFS uses K-weighted channel SUM and the -0.691 calibration.
// Gate definitions cross-checked against libebur128/ebur128.c; no code copied.
struct Meter {
    explicit Meter(double rate, GainStageMeasure measure) : lufs(measure == GainStageMeasure::IntegratedLufs),
        hop(static_cast<std::size_t>(std::llround(rate * 0.1))), ring(4 * hop) {
        for (auto& f : shelf) f.c = dsp::AutoGain::kShelf(rate);
        for (auto& f : hp) f.c = dsp::AutoGain::kHighPass(rate);
        peak.prepare(rate);
    }
    void add(float l, float r) {
        if (!std::isfinite(l) || !std::isfinite(r)) { valid = false; return; }
        const float* audio[]{&l, &r}; peak.process(audio, 2, 1);
        double a=l, b=r;
        if (lufs) { a=hp[0].run(shelf[0].run(a)); b=hp[1].run(shelf[1].run(b)); }
        const double energy=(a*a+b*b)*(lufs ? 1.0 : 0.5);
        const auto index = static_cast<std::size_t>(frames % ring.size());
        sum += energy-ring[index]; ring[index]=energy; ++frames;
        if (frames >= ring.size() && (frames-ring.size())%hop == 0)
            windows.push_back(std::max(0.0,sum)/static_cast<double>(ring.size()));
    }
    double finish() {
        // Short selections are padded to one gate window; long selections use
        // complete windows only. Peak interpolation is flushed independently.
        if (frames > 0 && frames < ring.size()) {
            while (frames < ring.size()) add(0,0);
        }
        const std::array<float,64> zeros{};
        const float* channels[]{zeros.data(),zeros.data()}; peak.process(channels,2,64);
        const double calibration = lufs ? -0.691 : 0.0;
        const double absolute = std::pow(10.0,(-70.0-calibration)/10.0);
        auto mean = [&](double gate) {
            double total=0; std::size_t count=0;
            for (double e : windows) if (e >= gate) { total+=e; ++count; }
            return count ? total/static_cast<double>(count) : 0.0;
        };
        const double energy = mean(std::max(absolute,mean(absolute)*0.1));
        return energy > 0 ? calibration+10.0*std::log10(energy) : -std::numeric_limits<double>::infinity();
    }
    bool lufs, valid=true;
    std::size_t hop;
    std::uint64_t frames=0;
    std::vector<double> ring, windows;
    double sum=0;
    std::array<Filter,2> shelf{},hp{};
    dsp::TruePeakMeter peak;
};
}

GainStageResult Session::autoGainStage(Store& store, const GainStageOptions& options) {
    GainStageResult result;
    auto fail = [&](const std::string& message) { result.commit.error=message; return result; };
    if (!loaded_ || !std::isfinite(options.targetDb) || !std::isfinite(options.ceilingDbTP) ||
        options.targetDb < -70 || options.targetDb > 0 || options.ceilingDbTP > 0 || options.ceilingDbTP < -120 ||
        options.beginSample < 0 || spec_.sampleRate < 8000 || spec_.sampleRate > 384000)
        return fail("invalid gain-stage selection, level or session format");
    OpJournal journal(store);
    CommitOptions commitOptions;
    commitOptions.expectHead = journal.headSeq();
    Session offline;
    offline.graph().setFadeFrames(0);
    if (!offline.load(store,loader_,{2,spec_.sampleRate,spec_.maxFrames})) return fail(offline.error());
    if (offline.stats().placeholders || offline.stats().skipped || !offline.problems().empty())
        return fail("gain stage requires a fully resolved project: missing device, media or unsupported playback");
    // Track mute/solo are monitoring choices, not a change to a clip's level.
    // Keep clip mutes/fades/gain and instrument automation; disable strip lanes.
    for (auto& track : offline.model_.tracks) { track.muted=false; track.soloed=false; }
    for (auto& lane : offline.model_.automationLanes) if (lane.ownerKind=="track") lane.enabled=false;
    if (!offline.rebuild()) return fail(offline.error());
    std::int64_t end = options.endSample;
    if (end == 0) {
        const auto snapshot = SnapshotBuilder::fromStore(store);
        for (const auto& clip : offline.model_.clips) if (!clip.muted) {
            const auto position=clip.posTicks.value_or(0), length=clip.lengthTicks.value_or(0);
            if (length < 0 || position > std::numeric_limits<std::int64_t>::max()-length)
                return fail("invalid gain-stage clip range");
            const double seconds = clip.timeBase == 1
                ? (static_cast<double>(clip.posNs.value_or(0))+static_cast<double>(clip.lengthNs.value_or(0)))*1e-9
                : snapshot->tempo->ticksToSeconds(clip.posTicks.value_or(0)+clip.lengthTicks.value_or(0));
            if (!std::isfinite(seconds) || seconds > 86400) return fail("gain-stage range exceeds one day");
            end=std::max(end,static_cast<std::int64_t>(std::ceil(seconds*spec_.sampleRate)));
        }
    }
    if (end <= options.beginSample || static_cast<double>(end)/spec_.sampleRate > 86400)
        return fail("gain stage needs a nonempty selection of at most one day");
    struct TrackMeter {
        std::shared_ptr<const ScopeTap> tap;
        Meter meter;
        TrackMeter(std::shared_ptr<const ScopeTap> t, double rate, GainStageMeasure m) : tap(std::move(t)),meter(rate,m) {}
    };
    std::vector<TrackMeter> meters;
    std::int64_t latency=0;
    for (const auto& track : offline.model_.tracks) if (track.kind=="audio" || track.kind=="midi") {
        if (track.kind=="midi") {
            bool hasClips=false, hasInstrument=false;
            for (const auto& clip : offline.model_.clips)
                hasClips=hasClips || (clip.trackId==track.id && !clip.muted);
            for (auto* node : offline.chainFor(track.id))
                hasInstrument=hasInstrument || node->eventFlow()==EventFlow::Consume;
            if (hasClips && !hasInstrument) return fail("MIDI track has clips but no resolved instrument");
        }
        auto tap=offline.openScope(track.id,1.0,ScopePoint::ChainInput);
        if (!tap) return fail("cannot tap track " + std::to_string(track.id));
        latency=std::max(latency,static_cast<std::int64_t>(tap->latency()));
        meters.emplace_back(tap,spec_.sampleRate,options.measure);
        GainStageTrack measurement; measurement.trackId=track.id;
        result.tracks.push_back(measurement);
    }
    const int block = std::min(spec_.maxFrames,4096);
    if (block <= 0) return fail("invalid gain-stage block size");
    std::vector<float> l(static_cast<std::size_t>(block)), r(l.size()), output(2*l.size());
    float* outputs[]{output.data(),output.data()+block};
    // Render from zero so instruments/automation have their preceding history.
    // Every output is a memory buffer; this command never opens an audio driver.
    offline.transport().play();
    for (std::int64_t at=0; at<end+latency; ) {
        const int frames=static_cast<int>(std::min<std::int64_t>(block,end+latency-at));
        if (!offline.clips()->prime()) return fail("offline clip read timed out");
        offline.process({outputs,nullptr,2,0,frames,0});
        for (auto& m : meters) {
            std::int64_t stamp=0;
            if (!m.tap->read(std::span(l.data(),static_cast<std::size_t>(frames)),std::span(r.data(),static_cast<std::size_t>(frames)),stamp))
                return fail("offline tap lost a block");
            for (int i=0;i<frames;++i) if (stamp+i >= options.beginSample && stamp+i < end)
                m.meter.add(l[static_cast<std::size_t>(i)],r[static_cast<std::size_t>(i)]);
        }
        if (offline.clips()->readErrors() || offline.clips()->underrunSamples()) return fail("offline clip read failed");
        at+=frames;
    }
    std::vector<OpRequest> ops;
    for (std::size_t i=0;i<meters.size();++i) {
        auto& m=meters[i].meter; auto& measurement=result.tracks[i];
        measurement.measuredDb=m.finish();
        if (!m.valid) return fail("nonfinite audio in gain-stage render");
        measurement.silent=!std::isfinite(measurement.measuredDb);
        if (measurement.silent) continue;
        measurement.peakDbTP=m.peak.peakDb();
        const double wanted=options.targetDb-measurement.measuredDb;
        const double allowed=options.ceilingDbTP-measurement.peakDbTP;
        measurement.ceilingLimited=allowed<wanted;
        measurement.gainDb=std::min({wanted,allowed,60.0});
        if (measurement.gainDb < -120) return fail("required attenuation exceeds input-gain range");
        OpRequest op;
        op.opType="mixer.setInputGain"; op.label="Auto Gain Stage Session";
        op.payload={{"id",measurement.trackId},{"db",measurement.gainDb}};
        ops.push_back(std::move(op));
    }
    if (ops.empty()) { result.commit.ok=true; return result; }
    result.commit=journal.commit(ops,commitOptions);
    if (result.commit.ok && !refresh(store)) result.commit.error="gain committed, but session refresh failed: "+error();
    return result;
}
}
