// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/render/normalization.hpp"
#include "adi/engine/main_loudness.hpp"
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
std::optional<double> measured(float value){return std::isfinite(value)?std::optional<double>(value):std::nullopt;}
void worker(){if(audio::onAudioThread)throw std::runtime_error("Normalization requires an offline worker");}
struct Measurement {
    engine::MainLoudness meter;
    std::vector<double> planar;
    std::vector<double*> pointers;
    int channels=0;
    Measurement(std::uint32_t rate,int count,const std::vector<double>& weights):planar(static_cast<std::size_t>(count)*blockFrames),pointers(static_cast<std::size_t>(count)),channels(count) {
        if(!meter.prepare(rate,count,weights))throw std::runtime_error("Cannot prepare export loudness measurement");
        for(int channel=0;channel<count;++channel)pointers[static_cast<std::size_t>(channel)]=planar.data()+static_cast<std::size_t>(channel)*blockFrames;
    }
    void process(std::span<const double> values,std::uint32_t frames) {
        for(std::uint32_t frame=0;frame<frames;++frame)for(int channel=0;channel<channels;++channel)
            pointers[static_cast<std::size_t>(channel)][frame]=values[static_cast<std::size_t>(frame)*static_cast<std::size_t>(channels)+static_cast<std::size_t>(channel)];
        engine::AudioIo io;io.frames=static_cast<std::int32_t>(frames);io.numOut=channels;io.out64=pointers.data();meter.process(io);
    }
    std::pair<std::optional<double>,std::optional<double>> finish() {
        const auto lufs=measured(meter.readout().integratedLufs.load(std::memory_order_relaxed));
        // Flush only the true-peak FIR. Do not count this padding in LUFS.
        for(auto* channel:pointers)std::fill_n(channel,16,0.0);
        engine::AudioIo io;io.frames=16;io.numOut=channels;io.out64=pointers.data();meter.process(io);
        return {lufs,measured(meter.readout().truePeakDb.load(std::memory_order_relaxed))};
    }
};
void service(const std::function<void()>& callback){if(callback)callback();}
}
void validateNormalization(const Normalization& options,std::uint32_t rate,int channels) {
    if(options.mode!=NormalizeMode::Off && options.mode!=NormalizeMode::Peak && options.mode!=NormalizeMode::Loudness)throw std::invalid_argument("Unknown export normalization mode");
    if(options.mode==NormalizeMode::Off)return;
    if(rate<8000 || rate>768000 || channels<1 || channels>64)throw std::invalid_argument("Unsupported normalization format");
    if(options.mode==NormalizeMode::Loudness && (!std::isfinite(options.targetLufs) || options.targetLufs<-36 || options.targetLufs>0 ||
       !std::isfinite(options.truePeakCeilingDb) || options.truePeakCeilingDb<-12 || options.truePeakCeilingDb>0))
        throw std::invalid_argument("Loudness target must be -36..0 LUFS and ceiling -12..0 dBTP");
    if(!options.channelWeights.empty()) {
        if(options.channelWeights.size()!=static_cast<std::size_t>(channels))throw std::invalid_argument("Normalization needs one weight per channel");
        for(const double weight:options.channelWeights)if(!std::isfinite(weight) || weight<0 || weight>2)throw std::invalid_argument("Invalid normalization channel weight");
    }
}
struct NormalizationCapture::Impl {
    std::filesystem::path spool;
    std::ofstream raw;
    std::uint32_t rate;
    int channels;
    Normalization options;
    Measurement input;
    std::uint64_t frames=0;
    double samplePeak=0;
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
    for(const double value:values) {
        if(!std::isfinite(value) || std::abs(value)>1e100)throw std::runtime_error("Audio exceeds supported normalization measurement range");
        state.samplePeak=std::max(state.samplePeak,std::abs(value));
    }
    state.raw.write(reinterpret_cast<const char*>(values.data()),static_cast<std::streamsize>(values.size_bytes()));
    if(!state.raw)throw std::runtime_error("Cannot write normalization capture");
    state.input.process(values,frames);state.frames+=frames;
}
NormalizationReport NormalizationCapture::finish(const std::filesystem::path& output,audio::WavFormat format,audio::WavDither dither,
                                                 std::uint64_t seed,const std::function<void()>& callback) {
    worker();auto& state=*impl_;
    if(state.finished || state.frames==0)throw std::runtime_error("Normalization capture is empty or already finalized");
    if((format!=audio::WavFormat::Float32 && format!=audio::WavFormat::Pcm16 && format!=audio::WavFormat::Pcm24) ||
       (dither!=audio::WavDither::None && dither!=audio::WavDither::Triangular && dither!=audio::WavDither::Rectangular))
        throw std::runtime_error("Unsupported normalization output encoding or dither");
    if(output==state.spool)throw std::runtime_error("Normalization output must differ from its capture");
    state.finished=true;state.raw.close();if(state.raw.fail())throw std::runtime_error("Cannot finalize normalization capture");
    service(callback);NormalizationReport report;const auto source=state.input.finish();
    report.sourceLufs=source.first;report.sourceTruePeakDb=source.second;
    if(state.samplePeak>0)report.sourceSamplePeakDb=20*std::log10(state.samplePeak);
    double gainDb=0;
    if(state.options.mode==NormalizeMode::Peak)gainDb=report.sourceSamplePeakDb?-*report.sourceSamplePeakDb:0;
    else {
        if(state.frames<static_cast<std::uint64_t>(std::llround(state.rate*0.4)) || !source.first || !source.second)
            throw std::runtime_error("LUFS target requires at least 400 ms of measurable gated audio; choose Peak or Off");
        gainDb=state.options.targetLufs-*source.first;
        const auto ceilingGain=state.options.truePeakCeilingDb-*source.second;
        if(gainDb>ceilingGain){gainDb=ceilingGain;report.ceilingLimited=true;}
    }
    std::vector<double> values(static_cast<std::size_t>(state.channels)*blockFrames);
    std::vector<float> encoded(values.size());
    for(unsigned pass=0;pass<4;++pass) {
        service(callback);
        {
            std::ifstream sourceFile(state.spool,std::ios::binary);if(!sourceFile)throw std::runtime_error("Cannot reopen normalization capture");
            audio::WavWriter writer(output,state.rate,static_cast<std::uint16_t>(state.channels),format,nullptr,std::uint64_t{1}<<32,dither,seed);
            const double gain=state.options.mode==NormalizeMode::Loudness?std::pow(10,gainDb/20):1;
            for(std::uint64_t first=0;first<state.frames;) {
                service(callback);const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,state.frames-first));
                const auto samples=static_cast<std::size_t>(count)*static_cast<std::size_t>(state.channels);
                const auto bytes=static_cast<std::streamsize>(samples*sizeof(double));sourceFile.read(reinterpret_cast<char*>(values.data()),bytes);
                if(sourceFile.gcount()!=bytes)throw std::runtime_error("Incomplete normalization capture");
                for(std::size_t sample=0;sample<samples;++sample) {
                    const auto value=state.options.mode==NormalizeMode::Peak?(state.samplePeak>0?values[sample]/state.samplePeak:values[sample]):values[sample]*gain;
                    if(!std::isfinite(value) || (format==audio::WavFormat::Float32 && std::abs(value)>std::numeric_limits<float>::max()))throw std::runtime_error("Normalized audio exceeds the output format");
                    values[sample]=value;
                }
                writer.write(values.data(),count);first+=count;
            }
            writer.close();
        }
        Measurement final(state.rate,state.channels,state.options.channelWeights);
        {
            audio::WavReader reader(output);if(reader.frames()!=state.frames || reader.channels()!=state.channels || reader.sampleRate()!=state.rate)throw std::runtime_error("Normalized output has wrong dimensions");
            for(std::uint64_t first=0;first<state.frames;) {
                service(callback);const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,state.frames-first));
                if(reader.read(encoded.data(),count)!=count)throw std::runtime_error("Incomplete normalized output");
                const auto samples=static_cast<std::size_t>(count)*static_cast<std::size_t>(state.channels);
                for(std::size_t sample=0;sample<samples;++sample)values[sample]=encoded[sample];
                final.process(std::span<const double>(values.data(),samples),count);first+=count;
            }
        }
        const auto measuredOutput=final.finish();report.outputLufs=measuredOutput.first;report.outputTruePeakDb=measuredOutput.second;
        report.appliedGainDb=gainDb;report.encodingPasses=pass+1;
        if(state.options.mode!=NormalizeMode::Loudness)return report;
        if(!measuredOutput.second)throw std::runtime_error("Normalized output has no measurable true peak");
        if(*measuredOutput.second<=state.options.truePeakCeilingDb) {
            // Absolute gating can admit windows after a large boost. Correct
            // against the encoded result, retaining the same captured audio.
            if(measuredOutput.first) {
                const auto correction=state.options.targetLufs-*measuredOutput.first;
                if(std::abs(correction)>0.1 && (correction<0 || !report.ceilingLimited)) {
                    const auto available=state.options.truePeakCeilingDb-*measuredOutput.second;
                    const auto delta=std::min(correction,available);
                    if(delta<correction)report.ceilingLimited=true;
                    if(std::abs(delta)>1e-6){gainDb+=delta;continue;}
                }
            }
            return report;
        }
        gainDb-=*measuredOutput.second-state.options.truePeakCeilingDb+0.01;report.ceilingLimited=true;
    }
    throw std::runtime_error("Encoded output cannot meet its normalization target and ceiling");
}
}
