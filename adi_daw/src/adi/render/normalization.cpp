// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/render/normalization.hpp"
#include "adi/dsp/loudness.hpp"
#include "adi/dsp/limiter.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>
namespace adi::render {
namespace {
constexpr std::uint32_t blockFrames=4096;
std::optional<double> measured(double value) {
    return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

void worker() {
    if (audio::onAudioThread) throw std::runtime_error("Export analysis requires an offline worker");
}

std::vector<double> measurementWeights(std::uint32_t rate, int channels,
                                       std::span<const double> weights) {
    if (rate < 8000 || rate > 768000 || channels < 1 || channels > 64) {
        throw std::invalid_argument("Unsupported export measurement format");
    }
    if (weights.empty()) return std::vector<double>(static_cast<std::size_t>(channels), 1.0);
    if (weights.size() != static_cast<std::size_t>(channels)) {
        throw std::invalid_argument("Export measurement needs one weight per channel");
    }
    for (const auto weight : weights) {
        if (!std::isfinite(weight) || weight < 0 || weight > 2) {
            throw std::invalid_argument("Invalid export measurement channel weight");
        }
    }
    return {weights.begin(), weights.end()};
}

std::optional<double> targetReading(const ExportStatistics& stats, NormalizationTarget target) {
    switch (target) {
    case NormalizationTarget::Integrated: return stats.integratedLufs;
    case NormalizationTarget::Rms: return stats.rmsDbfs;
    case NormalizationTarget::MaximumMomentary: return stats.maximumMomentaryLufs;
    case NormalizationTarget::MaximumShortTerm: return stats.maximumShortTermLufs;
    }
    return std::nullopt;
}

double targetLevel(const Normalization& options) {
    return options.target == NormalizationTarget::Rms ? options.targetRmsDbfs : options.targetLufs;
}

bool appliesTargetGain(NormalizeWhen when, double targetGainDb) noexcept {
    switch (when) {
    case NormalizeWhen::Always: return true;
    case NormalizeWhen::OnlyAboveTarget: return targetGainDb < 0;
    case NormalizeWhen::OnlyBelowTarget: return targetGainDb > 0;
    }
    return false;
}

void service(const std::function<void()>& callback) { if (callback) callback(); }

void readBlock(std::ifstream& source,std::span<double> values) {
    const auto bytes=static_cast<std::streamsize>(values.size_bytes());
    source.read(reinterpret_cast<char*>(values.data()),bytes);
    if(source.gcount()!=bytes)throw std::runtime_error("Incomplete normalization capture");
}
void limitProgramme(std::ifstream& source,std::span<double> scratch,std::vector<double>& planar,
                    std::size_t width,std::uint64_t frames,std::uint32_t rate,double gain,double ceilingDb,
                    const std::function<void()>& callback) {
    if(frames>planar.max_size()/width)
        throw std::runtime_error("Brickwall normalization exceeds the offline address space");
    const auto length=static_cast<std::size_t>(frames);
    planar.resize(length*width);
    std::vector<double*> channels(width);
    for(std::size_t channel=0;channel<width;++channel)channels[channel]=planar.data()+channel*length;
    for(std::size_t first=0;first<length;) {
        service(callback);
        const auto count=std::min<std::size_t>(blockFrames,length-first);
        readBlock(source,scratch.first(count*width));
        for(std::size_t frame=0;frame<count;++frame)
            for(std::size_t channel=0;channel<width;++channel)
                channels[channel][first+frame]=scratch[frame*width+channel]*gain;
        first+=count;
    }
    dsp::LookaheadLimiter limiter;
    limiter.prepare(rate,static_cast<int>(width),0);
    limiter.setTruePeak(true);
    limiter.setCeiling(std::pow(10.0,ceilingDb/20.0));
    service(callback);
    limiter.processOffline(channels.data(),channels.data(),static_cast<std::int64_t>(frames));
    service(callback);
}
} // namespace

struct ExportAnalysis::Impl {
    dsp::LoudnessMeter meter;
    std::vector<double> planar;
    std::vector<const double*> pointers;
    std::uint64_t frames = 0;
    double energy = 0, peak = 0;
    bool finished = false;
    Impl(std::uint32_t rate, const std::vector<double>& weights)
        : planar(weights.size() * blockFrames), pointers(weights.size()) {
        meter.prepare(rate, weights);
        for (std::size_t c = 0; c < weights.size(); ++c) {
            pointers[c] = planar.data() + c * blockFrames;
        }
    }
};

ExportAnalysis::ExportAnalysis(std::uint32_t rate, int channels, std::span<const double> weights) {
    worker();
    impl_ = std::make_unique<Impl>(rate, measurementWeights(rate, channels, weights));
}

ExportAnalysis::~ExportAnalysis() = default;

double ExportAnalysis::samplePeak() const noexcept { return impl_->peak; }

void ExportAnalysis::append(std::span<const double> interleaved, std::uint32_t frames) {
    worker();
    auto& state = *impl_;
    const auto channels = state.pointers.size();
    if (state.finished || frames > blockFrames || interleaved.size() != frames * channels ||
        state.frames > (std::uint64_t{1} << 50) - frames) {
        throw std::invalid_argument("Invalid export measurement block");
    }
    // Validate the entire block before changing the meter or accumulators.
    for (const auto sample : interleaved) {
        if (!std::isfinite(sample) || std::abs(sample) > 1e100) {
            throw std::runtime_error("Audio exceeds supported export measurement range");
        }
    }
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < channels; ++c) {
            const auto sample = interleaved[static_cast<std::size_t>(f) * channels + c];
            state.peak = std::max(state.peak, std::abs(sample));
            state.energy += sample * sample;
            state.planar[c * blockFrames + f] = sample;
        }
    }
    state.meter.process(state.pointers.data(), static_cast<int>(frames));
    state.frames += frames;
}

ExportStatistics ExportAnalysis::finish() {
    worker();
    auto& state = *impl_;
    if (state.finished || state.frames == 0) {
        throw std::runtime_error("Export measurement is empty or already finalized");
    }
    state.finished = true;
    state.meter.finish();
    const auto readings = state.meter.readings();
    ExportStatistics result;
    result.integratedLufs = measured(readings.integrated);
    result.truePeakDbtp = measured(readings.truePeak);
    result.maximumMomentaryLufs = measured(readings.maxMomentary);
    result.maximumShortTermLufs = measured(readings.maxShortTerm);
    if (readings.rangeValid) result.loudnessRangeLu = readings.range;
    if (state.peak > 0) result.samplePeakDbfs = 20 * std::log10(state.peak);
    if (state.energy > 0) {
        const auto samples = static_cast<double>(state.frames) * static_cast<double>(state.pointers.size());
        result.rmsDbfs = 10 * (std::log10(state.energy) - std::log10(samples));
    }
    return result;
}

const char* normalizationTargetName(NormalizationTarget target) noexcept {
    switch (target) {
    case NormalizationTarget::Integrated: return "integrated_lufs";
    case NormalizationTarget::Rms: return "rms_dbfs";
    case NormalizationTarget::MaximumMomentary: return "maximum_momentary_lufs";
    case NormalizationTarget::MaximumShortTerm: return "maximum_short_term_lufs";
    }
    return nullptr;
}

const char* normalizeWhenName(NormalizeWhen when) noexcept {
    switch (when) {
    case NormalizeWhen::Always: return "always";
    case NormalizeWhen::OnlyAboveTarget: return "only_above_target";
    case NormalizeWhen::OnlyBelowTarget: return "only_below_target";
    }
    return nullptr;
}

const char* skippedLoudnessCeilingName(SkippedLoudnessCeiling ceiling) noexcept {
    switch (ceiling) {
    case SkippedLoudnessCeiling::PreserveLevel: return "preserve_level";
    case SkippedLoudnessCeiling::EnforceCeiling: return "enforce_ceiling";
    }
    return nullptr;
}

void validateNormalization(const Normalization& options,std::uint32_t rate,int channels) {
    if(options.mode!=NormalizeMode::Off && options.mode!=NormalizeMode::Peak && options.mode!=NormalizeMode::Loudness)throw std::invalid_argument("Unknown export normalization mode");
    if (!normalizeWhenName(options.when) ||
        (options.mode == NormalizeMode::Off && options.when != NormalizeWhen::Always)) {
        throw std::invalid_argument("Invalid conditional normalization mode");
    }
    const bool conditionalLoudness = options.mode == NormalizeMode::Loudness &&
                                     options.when != NormalizeWhen::Always;
    if (conditionalLoudness != options.skippedLoudnessCeiling.has_value() ||
        (options.skippedLoudnessCeiling && !skippedLoudnessCeilingName(*options.skippedLoudnessCeiling))) {
        throw std::invalid_argument("Conditional loudness normalization requires an explicit skipped ceiling policy");
    }
    if(options.mode==NormalizeMode::Off)return;
    if (options.mode == NormalizeMode::Loudness) {
        if (!normalizationTargetName(options.target)) {
            throw std::invalid_argument("Unknown export normalization target statistic");
        }
        const auto target = targetLevel(options);
        if (!std::isfinite(target) || target < -36 || target > 0) {
            throw std::invalid_argument(options.target == NormalizationTarget::Rms ?
                "RMS target must be -36..0 dBFS" : "Loudness target must be -36..0 LUFS");
        }
        if (!std::isfinite(options.truePeakCeilingDb) || options.truePeakCeilingDb < -12 ||
            options.truePeakCeilingDb > 0) {
            throw std::invalid_argument("True-peak ceiling must be -12..0 dBTP");
        }
    }
    (void)measurementWeights(rate, channels, options.channelWeights);
}
struct NormalizationCapture::Impl {
    std::filesystem::path spool;
    std::ofstream raw;
    std::uint32_t rate;
    int channels;
    Normalization options;
    ExportAnalysis input;
    std::uint64_t frames=0;
    bool finished=false;
    Impl(const std::filesystem::path& file,std::uint32_t sampleRate,int count,const Normalization& normalize):
        spool(file),raw(file,std::ios::binary|std::ios::trunc),rate(sampleRate),channels(count),options(normalize),input(sampleRate,count,normalize.channelWeights) {
        if(!raw)throw std::runtime_error("Cannot open private normalization capture");
    }
};
NormalizationCapture::NormalizationCapture(const std::filesystem::path& file,std::uint32_t rate,int channels,const Normalization& options) {
    worker();validateNormalization(options,rate,channels);
    if(options.mode==NormalizeMode::Off)throw std::invalid_argument("Normalization capture requires an enabled mode");
    impl_=std::make_unique<Impl>(file,rate,channels,options);
}
NormalizationCapture::~NormalizationCapture()=default;
void NormalizationCapture::append(std::span<const double> values,std::uint32_t frames) {
    worker();auto& state=*impl_;
    if(state.finished || frames>blockFrames || values.size()!=static_cast<std::size_t>(frames)*static_cast<std::size_t>(state.channels) ||
       state.frames>(std::uint64_t{1}<<50)-frames)throw std::runtime_error("Invalid normalization capture block");
    state.input.append(values, frames);
    state.raw.write(reinterpret_cast<const char*>(values.data()),static_cast<std::streamsize>(values.size_bytes()));
    if (!state.raw) {
        state.finished = true;
        throw std::runtime_error("Cannot write normalization capture");
    }
    state.frames+=frames;
}
NormalizationReport NormalizationCapture::finish(const std::filesystem::path& output,audio::WavFormat format,audio::WavDither dither,
                                                 std::uint64_t seed,const std::function<void()>& callback,
                                                 const std::function<void(std::uint32_t)>& advanceFrames,
                                                 std::uint32_t speakerMask) {
    worker();auto& state=*impl_;
    if(state.finished || state.frames==0)throw std::runtime_error("Normalization capture is empty or already finalized");
    if((format!=audio::WavFormat::Float32 && format!=audio::WavFormat::Pcm16 && format!=audio::WavFormat::Pcm24) ||
        !audio::validDither(dither))
        throw std::runtime_error("Unsupported normalization output encoding or dither");
    if (!audio::validWavSpeakerMask(static_cast<std::uint16_t>(state.channels), speakerMask)) {
        throw std::invalid_argument("WAV speaker mask needs one defined speaker per interleaved channel");
    }
    if(output==state.spool)throw std::runtime_error("Normalization output must differ from its capture");
    state.finished=true;state.raw.close();if(state.raw.fail())throw std::runtime_error("Cannot finalize normalization capture");
    service(callback);
    NormalizationReport report;
    report.captured = state.input.finish();
    report.sourceLufs = report.captured.integratedLufs;
    report.sourceTruePeakDb = report.captured.truePeakDbtp;
    report.sourceSamplePeakDb = report.captured.samplePeakDbfs;
    const auto samplePeak = state.input.samplePeak();
    const bool loudness = state.options.mode == NormalizeMode::Loudness;
    double targetGainDb = 0;
    if (!loudness) {
        if (report.sourceSamplePeakDb) targetGainDb = -*report.sourceSamplePeakDb;
    } else {
        const auto sourceTarget = targetReading(report.captured, state.options.target);
        if (!sourceTarget || !report.sourceTruePeakDb) {
            throw std::runtime_error(std::string("No measurable ") +
                normalizationTargetName(state.options.target) + " in this range; choose Peak or Off");
        }
        targetGainDb = targetLevel(state.options) - *sourceTarget;
    }
    const bool correctTarget = appliesTargetGain(state.options.when, targetGainDb);
    report.conditionalGainSkipped = !correctTarget;
    double gainDb = correctTarget ? targetGainDb : 0;
    const bool verifyCeiling = loudness && (correctTarget ||
        state.options.skippedLoudnessCeiling == SkippedLoudnessCeiling::EnforceCeiling);
    const bool brickwall = verifyCeiling && state.options.brickwall;
    double limiterCeiling = state.options.truePeakCeilingDb;
    if (verifyCeiling) {
        const auto ceilingGain=state.options.truePeakCeilingDb-*report.sourceTruePeakDb;
        if (gainDb > ceilingGain) {
            if (!brickwall) gainDb = ceilingGain;
            report.ceilingLimited = true;
        }
    }
    const auto width=static_cast<std::size_t>(state.channels);
    std::vector<double> values(width*blockFrames),limited;
    std::vector<float> encoded(values.size());
    for(unsigned pass=0;pass<maxLoudnessEncodingPasses;++pass) {
        service(callback);
        double quantizationGain = 1.0;
        {
            std::ifstream sourceFile(state.spool,std::ios::binary);if(!sourceFile)throw std::runtime_error("Cannot reopen normalization capture");
            const double gain=state.options.mode==NormalizeMode::Loudness?std::pow(10,gainDb/20):1;
            report.brickwallApplied=brickwall && *report.sourceTruePeakDb + gainDb>limiterCeiling;
            if(report.brickwallApplied) {
                report.ceilingLimited=true;
                limitProgramme(sourceFile,values,limited,width,state.frames,state.rate,gain,limiterCeiling,callback);
            }
            audio::WavWriter writer(output,state.rate,static_cast<std::uint16_t>(state.channels),format,nullptr,std::uint64_t{1}<<32,dither,seed,speakerMask);
            quantizationGain = writer.quantizationGain();
            for(std::uint64_t first=0;first<state.frames;) {
                service(callback);const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,state.frames-first));
                const auto samples=static_cast<std::size_t>(count)*width;
                if(report.brickwallApplied) {
                    const auto length=static_cast<std::size_t>(state.frames);
                    for(std::size_t frame=0;frame<count;++frame)
                        for(std::size_t channel=0;channel<width;++channel)
                            values[frame*width+channel]=limited[channel*length+static_cast<std::size_t>(first)+frame];
                } else readBlock(sourceFile,std::span<double>(values).first(samples));
                for(std::size_t sample=0;sample<samples;++sample) {
                    auto value=values[sample];
                    if (!loudness && correctTarget && samplePeak > 0) value /= samplePeak;
                    if (loudness && !report.brickwallApplied) value *= gain;
                    if(!std::isfinite(value) || (format==audio::WavFormat::Float32 && std::abs(value)>std::numeric_limits<float>::max()))throw std::runtime_error("Normalized audio exceeds the output format");
                    values[sample]=value;
                }
                writer.write(values.data(),count);first+=count;
                if(advanceFrames)advanceFrames(count);
            }
            writer.close();
        }
        ExportAnalysis final(state.rate,state.channels,state.options.channelWeights);
        {
            audio::WavReader reader(output);if(reader.frames()!=state.frames || reader.channels()!=state.channels || reader.sampleRate()!=state.rate)throw std::runtime_error("Normalized output has wrong dimensions");
            for(std::uint64_t first=0;first<state.frames;) {
                service(callback);const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,state.frames-first));
                if(reader.read(encoded.data(),count)!=count)throw std::runtime_error("Incomplete normalized output");
                const auto samples=static_cast<std::size_t>(count)*static_cast<std::size_t>(state.channels);
                for(std::size_t sample=0;sample<samples;++sample)values[sample]=encoded[sample];
                final.append(std::span<const double>(values.data(),samples),count);first+=count;
                if(advanceFrames)advanceFrames(count);
            }
        }
        report.decoded = final.finish();
        report.outputLufs = report.decoded.integratedLufs;
        report.outputTruePeakDb = report.decoded.truePeakDbtp;
        report.appliedGainDb=gainDb+20.0*std::log10(quantizationGain);report.encodingPasses=pass+1;
        if (!verifyCeiling) return report;
        if(!report.outputTruePeakDb)throw std::runtime_error("Normalized output has no measurable true peak");
        if(*report.outputTruePeakDb<=state.options.truePeakCeilingDb) {
            // Absolute gating can admit windows after a large boost. Correct
            // against the encoded result, retaining the same captured audio.
            const auto outputTarget = targetReading(report.decoded, state.options.target);
            if (correctTarget && outputTarget && (!brickwall || pass + 1 < maxLoudnessEncodingPasses)) {
                const auto correction = targetLevel(state.options) - *outputTarget;
                if(std::abs(correction)>0.1 && (brickwall || correction < 0 || !report.ceilingLimited)) {
                    const auto available=state.options.truePeakCeilingDb-*report.outputTruePeakDb;
                    const auto delta = brickwall ? std::clamp(correction, -3.0, 3.0) : std::min(correction, available);
                    if(delta<correction)report.ceilingLimited=true;
                    if(std::abs(delta)>1e-6){gainDb+=delta;continue;}
                }
            }
            return report;
        }
        const auto excess=*report.outputTruePeakDb-state.options.truePeakCeilingDb+0.01;
        if(report.brickwallApplied)limiterCeiling-=std::min(excess,3.0);
        else gainDb-=brickwall?std::min(excess,3.0):excess;
        report.ceilingLimited=true;
    }
    throw std::runtime_error("Encoded output cannot meet its normalization target and ceiling");
}
}
