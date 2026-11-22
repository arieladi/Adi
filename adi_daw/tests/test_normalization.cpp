// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/render/normalization.hpp"
#include "adi/engine/main_loudness.hpp"
#include "adi/audio/wav_file.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <stdexcept>
namespace {
using namespace adi;using namespace adi::render;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
std::vector<float> read(const std::filesystem::path& file) {
    audio::WavReader reader(file);std::vector<float> result(static_cast<std::size_t>(reader.frames())*reader.channels());reader.read(result.data(),static_cast<std::uint32_t>(reader.frames()));return result;
}
std::vector<char> bytes(const std::filesystem::path& file){std::ifstream in(file,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
std::pair<double,double> measure(const std::filesystem::path& file) {
    audio::WavReader reader(file);engine::MainLoudness meter;if(!meter.prepare(reader.sampleRate(),reader.channels()))throw std::runtime_error("meter");
    std::vector<float> interleaved(static_cast<std::size_t>(reader.channels())*4096),planar(interleaved.size());std::vector<float*> channels(reader.channels());
    for(std::size_t channel=0;channel<channels.size();++channel)channels[channel]=planar.data()+channel*4096;
    for(std::uint64_t first=0;first<reader.frames();) {
        const auto frames=static_cast<std::uint32_t>(std::min<std::uint64_t>(4096,reader.frames()-first));reader.read(interleaved.data(),frames);
        for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<channels.size();++channel)channels[channel][frame]=interleaved[static_cast<std::size_t>(frame)*channels.size()+channel];
        engine::AudioIo io;io.out=channels.data();io.numOut=reader.channels();io.frames=static_cast<std::int32_t>(frames);meter.process(io);first+=frames;
    }
    const double lufs=meter.readout().integratedLufs.load();for(auto* channel:channels)std::fill_n(channel,16,0.f);engine::AudioIo tail;tail.out=channels.data();tail.numOut=reader.channels();tail.frames=16;meter.process(tail);
    return {lufs,meter.readout().truePeakDb.load()};
}
NormalizationReport tone(const std::filesystem::path& folder,const std::string& name,Normalization options,std::uint32_t rate=48000,
                         bool gated=false,audio::WavFormat format=audio::WavFormat::Float32,audio::WavDither dither=audio::WavDither::None) {
    NormalizationCapture capture(folder/(name+".f64"),rate,2,options);std::vector<double> block(8192);const auto total=static_cast<std::uint64_t>(rate)*(gated?2:1);
    for(std::uint64_t first=0;first<total;) {
        const auto frames=static_cast<std::uint32_t>(std::min<std::uint64_t>(4096,total-first));
        for(std::uint32_t frame=0;frame<frames;++frame) {
            const auto at=first+frame;const double amp=gated?std::pow(10,(at<rate?-69.0:-74.0)/20):(options.truePeakCeilingDb==-12?0.8:0.01);
            const double hz=options.truePeakCeilingDb==-12?rate/4.0:1000;
            const double value=amp*std::sin(2*std::numbers::pi*hz*static_cast<double>(at)/rate+(options.truePeakCeilingDb==-12?std::numbers::pi/4:0));
            block[static_cast<std::size_t>(frame)*2]=value;block[static_cast<std::size_t>(frame)*2+1]=value;
        }
        capture.append(std::span<const double>(block.data(),static_cast<std::size_t>(frames)*2),frames);first+=frames;
    }
    return capture.finish(folder/(name+".wav"),format,dither,123);
}
void peakAndWidePrecision() {
    test::TempDirectory directory{"normalization","peak"};Normalization options;options.mode=NormalizeMode::Peak;
    NormalizationCapture capture(directory.path()/"wide.f64",48000,1,options);const std::array<double,2> wide={0.50000003,1};capture.append(wide,2);
    const auto report=capture.finish(directory.path()/"wide.wav",audio::WavFormat::Pcm24,audio::WavDither::None,0);const auto heard=read(directory.path()/"wide.wav");
    check(heard.size()==2 && heard[0]==0.5f,"double capture retains a value that premature Float32 narrowing would move by one PCM24 LSB");
    check(report.appliedGainDb==0 && report.encodingPasses==1 && report.sourceSamplePeakDb==0,"Peak targets zero sample-peak dB without a second graph render");
    NormalizationCapture tiny(directory.path()/"tiny.f64",48000,1,options);const std::array<double,2> quiet={1e-310,0};tiny.append(quiet,2);
    const auto small=tiny.finish(directory.path()/"tiny.wav",audio::WavFormat::Float32,audio::WavDither::None,0);
    check(read(directory.path()/"tiny.wav")==std::vector<float>({1,0}) && std::isfinite(small.appliedGainDb),"tiny peak normalization uses a ratio even when a reciprocal gain would overflow");
    NormalizationCapture dithered(directory.path()/"dither.f64",48000,1,options);std::vector<double> input(512,0.25);input[0]=1;dithered.append(input,512);
    dithered.finish(directory.path()/"dither.wav",audio::WavFormat::Pcm16,audio::WavDither::Triangular,99);
    {audio::WavWriter reference(directory.path()/"reference.wav",48000,1,audio::WavFormat::Pcm16,nullptr,std::uint64_t{1}<<32,audio::WavDither::Triangular,99);reference.write(input.data(),512);reference.close();}
    check(bytes(directory.path()/"dither.wav")==bytes(directory.path()/"reference.wav"),"normalization applies destination dither exactly once with the original deterministic stream");
    NormalizationCapture shaped(directory.path()/"shaped.f64",48000,1,options);
    shaped.append(input,512);
    const auto reportShaped=shaped.finish(directory.path()/"shaped.wav",audio::WavFormat::Pcm16,
                                         audio::WavDither::Shaped3,99);
    double expectedGain=0;
    {
        audio::WavWriter writer(directory.path()/"shaped_reference.wav",48000,1,audio::WavFormat::Pcm16,nullptr,
                                std::uint64_t{1}<<32,audio::WavDither::Shaped3,99);
        expectedGain=20*std::log10(writer.quantizationGain());
        writer.write(input.data(),512);
    }
    check(reportShaped.encodingPasses==1 && reportShaped.appliedGainDb==expectedGain && expectedGain<0 &&
          bytes(directory.path()/"shaped.wav")==bytes(directory.path()/"shaped_reference.wav"),
          "Peak normalization writes shaped dither once and reports the applied quantization headroom");
    NormalizationCapture silence(directory.path()/"silence.f64",48000,1,options);const std::array<double,2> zero={0,0};silence.append(zero,2);const auto silent=silence.finish(directory.path()/"silence.wav",audio::WavFormat::Float32,audio::WavDither::None,0);
    check(!silent.sourceSamplePeakDb && silent.appliedGainDb==0 && read(directory.path()/"silence.wav")==std::vector<float>({0,0}),"silent Peak export remains silent without dividing by zero");
}
void loudnessCeilingAndGates() {
    test::TempDirectory directory{"normalization","loudness"};Normalization options;options.mode=NormalizeMode::Loudness;
    for(const auto rate:{44100U,48000U,96000U,768000U}) {
        const auto name="tone"+std::to_string(rate);const auto report=tone(directory.path(),name,options,rate);const auto actual=measure(directory.path()/(name+".wav"));
        check(report.sourceLufs && report.outputLufs && std::abs(actual.first+14)<0.1 && std::abs(*report.outputLufs-actual.first)<0.001,"actual encoded tone reaches the integrated LUFS target at each source rate");
        check(actual.second<=-1 && !report.ceilingLimited && report.appliedGainDb>0,"ordinary LUFS boost stays under its true-peak ceiling");
    }
    const auto ordinary=tone(directory.path(),"weights_default",options);
    auto weightedOptions=options;weightedOptions.channelWeights={1,0};const auto weighted=tone(directory.path(),"weights_explicit",weightedOptions);
    check(std::abs(weighted.appliedGainDb-ordinary.appliedGainDb-10*std::log10(2.0))<0.001 && weighted.outputLufs && std::abs(*weighted.outputLufs+14)<0.1,"explicit channel weights affect measured loudness without guessing an LFE ordinal");
    options.targetLufs=0;options.truePeakCeilingDb=-12;
    const auto capped=tone(directory.path(),"ceiling",options,48000,false,audio::WavFormat::Pcm16,audio::WavDither::Triangular);const auto cappedActual=measure(directory.path()/"ceiling.wav");
    check(capped.ceilingLimited && cappedActual.second<=-12 && capped.outputTruePeakDb && *capped.outputTruePeakDb<=-12,"encoded dithered PCM with intersample crests actually respects the requested true-peak ceiling");
    check(capped.sourceTruePeakDb && capped.sourceSamplePeakDb && *capped.sourceTruePeakDb>*capped.sourceSamplePeakDb+2,"ceiling analysis measures intersample peak rather than substituting sample peak");
    const auto repeated=tone(directory.path(),"ceiling_repeat",options,48000,false,audio::WavFormat::Pcm16,audio::WavDither::Triangular);
    check(repeated.appliedGainDb==capped.appliedGainDb && bytes(directory.path()/"ceiling.wav")==bytes(directory.path()/"ceiling_repeat.wav"),"re-encoding from one capture restarts the same dither seed and is byte reproducible");
    options.targetLufs=-14;options.truePeakCeilingDb=-1;const auto gated=tone(directory.path(),"gated",options,48000,true);
    check(gated.outputLufs && std::abs(*gated.outputLufs+14)<=0.1 && gated.encodingPasses>=2,"normalization corrects the gain when a boost changes absolute-gate membership");
}
void conditionalNormalization() {
    test::TempDirectory directory{"normalization", "conditional"};
    Normalization options;
    options.mode = NormalizeMode::Loudness;
    options.target = NormalizationTarget::Rms;
    options.targetRmsDbfs = -24;
    options.when = NormalizeWhen::OnlyAboveTarget;
    bool needsPolicy = false;
    try {
        validateNormalization(options, 48000, 2);
    } catch (const std::invalid_argument&) {
        needsPolicy = true;
    }
    check(needsPolicy, "conditional loudness requires an explicit policy for skipped gain");

    options.skippedLoudnessCeiling = SkippedLoudnessCeiling::PreserveLevel;
    const auto quiet = tone(directory.path(), "quiet-above", options);
    check(quiet.conditionalGainSkipped && !quiet.brickwallApplied && !quiet.ceilingLimited &&
          quiet.appliedGainDb == 0 && quiet.captured.rmsDbfs && quiet.decoded.rmsDbfs &&
          std::abs(*quiet.decoded.rmsDbfs - *quiet.captured.rmsDbfs) < 1e-6,
          "Only Above preserves a quiet programme");
    options.when = NormalizeWhen::OnlyBelowTarget;
    const auto boosted = tone(directory.path(), "quiet-below", options);
    check(!boosted.conditionalGainSkipped && boosted.appliedGainDb > 0 &&
          boosted.decoded.rmsDbfs && std::abs(*boosted.decoded.rmsDbfs + 24) <= 0.1,
          "Only Below boosts the same programme to the target");

    options.targetRmsDbfs = -1;
    options.truePeakCeilingDb = -12;
    options.when = NormalizeWhen::OnlyAboveTarget;
    const auto unchanged = tone(directory.path(), "crest-preserved", options);
    check(unchanged.conditionalGainSkipped && !unchanged.ceilingLimited &&
          unchanged.outputTruePeakDb && *unchanged.outputTruePeakDb > -12 && unchanged.appliedGainDb == 0,
          "the explicit Preserve Level policy keeps a skipped crest unaltered");
    options.skippedLoudnessCeiling = SkippedLoudnessCeiling::EnforceCeiling;
    const auto capped = tone(directory.path(), "crest-capped", options, 48000, false,
                             audio::WavFormat::Pcm24, audio::WavDither::Triangular);
    check(capped.conditionalGainSkipped && capped.ceilingLimited &&
          capped.outputTruePeakDb && *capped.outputTruePeakDb <= -12 && capped.appliedGainDb < 0,
          "the explicit ceiling policy attenuates a skipped crest and reports both outcomes");

    Normalization peak;
    peak.mode = NormalizeMode::Peak;
    peak.when = NormalizeWhen::OnlyAboveTarget;
    const std::array<double, 2> samples{0.5, -0.5};
    NormalizationCapture peakCapture(directory.path() / "peak.f64", 48000, 1, peak);
    peakCapture.append(samples, 2);
    const auto out = directory.path() / "peak.wav";
    const auto skipped = peakCapture.finish(out, audio::WavFormat::Float32, audio::WavDither::None, 0);
    check(skipped.conditionalGainSkipped && read(out) == std::vector<float>{0.5f, -0.5f},
          "Peak Only Above preserves below-full-scale PCM");
}

void exportStatistics() {
    ExportAnalysis shortMeter(48000, 2);
    const std::array<double, 8> impulse{1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    shortMeter.append(impulse, 4);
    const auto shortResult = shortMeter.finish();
    check(shortResult.samplePeakDbfs == 0.0 && shortResult.rmsDbfs &&
          std::abs(*shortResult.rmsDbfs - 10.0 * std::log10(1.0 / 8)) < 1e-12,
          "sample peak and whole-file RMS measure raw samples across both channels");
    check(!shortResult.integratedLufs && !shortResult.maximumMomentaryLufs &&
          !shortResult.maximumShortTermLufs && !shortResult.loudnessRangeLu,
          "short audio does not invent readings for underfilled windows");

    test::TempDirectory directory{"normalization", "statistics"};
    Normalization options;
    options.mode = NormalizeMode::Peak;
    NormalizationCapture capture(directory.path() / "long.f64", 48000, 1, options);
    constexpr std::uint32_t block = 1200;
    std::vector<double> samples(block);
    for (std::uint32_t first = 0; first < 384000; first += block) {
        const double gain = first < 192000 ? 0.1 : 0.5;
        for (std::uint32_t i = 0; i < block; ++i) {
            samples[i] = gain * std::sin(2.0 * std::numbers::pi * i / 48.0);
        }
        capture.append(samples, block);
    }
    const auto report = capture.finish(directory.path() / "long.wav",
                                       audio::WavFormat::Float32, audio::WavDither::None, 0);
    check(report.captured.loudnessRangeLu && *report.captured.loudnessRangeLu > 10.0 &&
          report.captured.maximumMomentaryLufs && report.captured.maximumShortTermLufs,
          "one shared meter reports range and held maxima after the required windows");
    check(report.captured.integratedLufs == report.sourceLufs &&
          report.decoded.integratedLufs == report.outputLufs &&
          report.decoded.truePeakDbtp == report.outputTruePeakDb &&
          report.decoded.samplePeakDbfs == 0.0 && report.captured.samplePeakDbfs &&
          *report.captured.samplePeakDbfs < -6.0,
          "captured PCM and decoded normalized delivery are distinguished");
}

void moreTargets() {
    test::TempDirectory directory{"normalization", "targets"};
    const auto appendTone = [](NormalizationCapture& capture, std::uint32_t totalFrames) {
        constexpr std::uint32_t blockSize = 1200;
        std::vector<double> block(blockSize);
        for (std::uint32_t start = 0; start < totalFrames; start += blockSize) {
            const double gain = start < 120000 ? 0.05 : 0.2;
            for (std::uint32_t i = 0; i < blockSize; ++i) {
                block[i] = gain * std::sin(2.0 * std::numbers::pi * i / 48.0);
            }
            capture.append(block, blockSize);
        }
    };
    Normalization options;
    options.mode = NormalizeMode::Loudness;
    options.targetLufs = -18.0;
    options.targetRmsDbfs = -21.0;
    for (const auto target : {NormalizationTarget::Rms, NormalizationTarget::MaximumMomentary,
                              NormalizationTarget::MaximumShortTerm}) {
        options.target = target;
        const auto name = std::string(normalizationTargetName(target));
        NormalizationCapture capture(directory.path() / (name + ".f64"), 48000, 1, options);
        appendTone(capture, 192000);
        const auto out = directory.path() / (name + ".wav");
        const auto report = capture.finish(out, audio::WavFormat::Pcm24, audio::WavDither::Triangular, 321);
        check(report.decoded.truePeakDbtp && *report.decoded.truePeakDbtp <= options.truePeakCeilingDb,
              "each extra target verifies the decoded true-peak ceiling");
        if (target == NormalizationTarget::Rms) {
            double squares = 0.0;
            const auto samples = read(out);
            for (const double value : samples) squares += value * value;
            const auto rmsDbfs = 10.0 * std::log10(squares / static_cast<double>(samples.size()));
            check(std::abs(rmsDbfs - options.targetRmsDbfs) <= 0.1 && report.captured.rmsDbfs &&
                  std::abs(*report.captured.rmsDbfs - 10.0 * std::log10(
                      (0.05 * 0.05 * 2.5 + 0.2 * 0.2 * 1.5) / 8.0)) < 1e-6,
                  "RMS meets the requested level on the decoded samples");
        } else {
            const auto loudness = target == NormalizationTarget::MaximumMomentary ?
                report.decoded.maximumMomentaryLufs : report.decoded.maximumShortTermLufs;
            check(loudness && std::abs(*loudness - options.targetLufs) <= 0.1 &&
                  report.decoded.integratedLufs && std::abs(*report.decoded.integratedLufs - options.targetLufs) > 0.15,
                  "window maximum targets its own statistic instead of integrated LUFS");
        }
    }
    options.target = NormalizationTarget::MaximumShortTerm;
    NormalizationCapture shortCapture(directory.path() / "short_term.f64", 48000, 1, options);
    appendTone(shortCapture, 48000);
    bool rejected = false;
    try {
        shortCapture.finish(directory.path() / "too_short.wav",
                            audio::WavFormat::Pcm24, audio::WavDither::None, 0);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected && !std::filesystem::exists(directory.path() / "too_short.wav"),
          "short-term target refuses an underfilled analysis window before encoding");
}

void refusalsAndCancellation() {
    test::TempDirectory directory{"normalization","refusals"};Normalization options;options.mode=NormalizeMode::Loudness;
    bool refused=false;try{NormalizationCapture capture(directory.path()/"short.f64",48000,1,options);const std::array<double,2> samples={0.1,-0.1};capture.append(samples,2);capture.finish(directory.path()/"short.wav",audio::WavFormat::Float32,audio::WavDither::None,0);}catch(const std::exception&){refused=true;}
    check(refused && !std::filesystem::exists(directory.path()/"short.wav"),"unmeasurable short LUFS audio refuses before encoding an output");
    auto invalid=options;invalid.targetLufs=1;refused=false;try{NormalizationCapture capture(directory.path()/"invalid.f64",48000,1,invalid);}catch(const std::exception&){refused=true;}
    check(refused && !std::filesystem::exists(directory.path()/"invalid.f64"),"invalid targets refuse before touching a capture path");
    invalid=options;invalid.channelWeights={3};refused=false;try{NormalizationCapture capture(directory.path()/"weights-invalid.f64",48000,1,invalid);}catch(const std::exception&){refused=true;}
    check(refused && !std::filesystem::exists(directory.path()/"weights-invalid.f64"),"invalid channel weights refuse before creating the private capture");
    options.mode=NormalizeMode::Peak;NormalizationCapture unsupported(directory.path()/"encoding.f64",48000,1,options);const std::array<double,2> valid={0.1,-0.1};unsupported.append(valid,2);
    refused=false;try{unsupported.finish(directory.path()/"bad-encoding.wav",static_cast<audio::WavFormat>(99),audio::WavDither::None,0);}catch(const std::exception&){refused=true;}
    check(refused && !std::filesystem::exists(directory.path()/"bad-encoding.wav"),"unknown encoding refuses without creating a misleading normalized output");
    NormalizationCapture cancelled(directory.path()/"cancel.f64",48000,1,options);const std::array<double,2> samples={0.1,-0.1};cancelled.append(samples,2);
    refused=false;try{cancelled.finish(directory.path()/"cancel.wav",audio::WavFormat::Float32,audio::WavDither::None,0,[]{throw std::runtime_error("cancelled");});}catch(const std::exception&){refused=true;}
    check(refused && !std::filesystem::exists(directory.path()/"cancel.wav"),"owner cancellation interrupts normalization before its output is encoded");
}
}
int main(){try{peakAndWidePrecision();loudnessCeilingAndGates();exportStatistics(); conditionalNormalization();moreTargets();refusalsAndCancellation();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
