// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/builtin_samples.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#include "adi/sampler_declarations.hpp"
#if ADI_WITH_PD
#include "adi/pd_builtins/colorcab_sample.hpp"
#include "adi/pd_builtins/hybrid_reverb_sample.hpp"
#include "adi/pd_builtins/oneshot_sample.hpp"
#endif
#include <samplerate.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace adi::device {
namespace {
constexpr std::string_view colorCabUid="org.adidaw.builtin.color-cab";
constexpr std::string_view oneShotUid="org.adidaw.builtin.oneshot";
constexpr std::string_view hybridReverbUid="org.adidaw.builtin.hybrid-reverb";
void offThread(){if(audio::onAudioThread)throw std::runtime_error("sample preparation belongs off the audio thread");}
void validRate(double rate){if(!std::isfinite(rate)||rate<=0||rate>768000)throw std::runtime_error("invalid sample processing rate");}
bool hashValid(const std::string& hash){return hash.size()==64 && std::all_of(hash.begin(),hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
float sourceSample(std::span<const std::uint8_t> bytes, std::size_t sample) noexcept {
    const auto offset=sample*4;
    const std::uint32_t bits=static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset+1])<<8) | (static_cast<std::uint32_t>(bytes[offset+2])<<16) |
        (static_cast<std::uint32_t>(bytes[offset+3])<<24);
    return std::bit_cast<float>(bits);
}
#if ADI_WITH_PD
std::vector<float> sourceMono(const Payload& value) {
    const auto& bytes=value.at("source_f32le").get_binary();
    if(bytes.empty() || bytes.size()%4!=0 || bytes.size()/4>builtinSampleMaxFrames)throw std::runtime_error("invalid sample backup length");
    std::vector<float> result(bytes.size()/4);
    for(std::size_t i=0;i<result.size();++i) {
        result[i]=sourceSample(bytes,i);if(!std::isfinite(result[i]))throw std::runtime_error("nonfinite sample backup");
    }
    return result;
}
std::vector<float> atRate(const std::vector<float>& source,double from,double to) {
    validRate(from);validRate(to);if(from==to)return source;
    const double count=std::ceil(static_cast<double>(source.size())*to/from)+64;
    if(count>static_cast<double>(builtinSampleMaxFrames))throw std::runtime_error("resampled sample exceeds preparation limit");
    std::vector<float> output(static_cast<std::size_t>(count));SRC_DATA data{};
    data.data_in=source.data();data.input_frames=static_cast<long>(source.size());data.data_out=output.data();
    data.output_frames=static_cast<long>(output.size());data.src_ratio=to/from;data.end_of_input=1;
    const int error=src_simple(&data,SRC_SINC_BEST_QUALITY,1);
    if(error!=0 || data.input_frames_used!=data.input_frames || data.output_frames_gen<=0)throw std::runtime_error("sample resampling failed");
    output.resize(static_cast<std::size_t>(data.output_frames_gen));return output;
}
std::unique_ptr<PdSampleBuffer> design(const std::vector<float>& mono,double sourceRate,double rate,const BuiltinSampleInfo& info) {
    auto raw=std::make_unique<PdSampleBuffer>();raw->id=info.slot;raw->channels=1;raw->sampleRate=rate;
    raw->interleaved=atRate(mono,sourceRate,rate);raw->frames=static_cast<std::int64_t>(raw->interleaved.size());raw->blake3=info.mediaHash;
    return prepareColorCabSample(std::move(raw));
}
void captureDesign(Payload& value,const PdColorCabSample& sample) {
    value["derived"]={{"rate",sample.design.sampleRate},{"kernel",std::vector<double>(sample.design.kernel.taps.begin(),
        sample.design.kernel.taps.begin()+static_cast<std::ptrdiff_t>(sample.design.kernel.size))},{"target",sample.design.target}};
}
void validateDesign(const Payload& value) {
    const auto& derived=value.at("derived");validRate(derived.at("rate").get<double>());
    const auto& taps=derived.at("kernel");const auto& target=derived.at("target");
    if(!taps.is_array()||taps.empty()||taps.size()>dsp::ColorCabKernel::capacity || !target.is_array()||target.empty()||target.size()>65537)
        throw std::runtime_error("invalid derived sample profile");
    for(const auto& tap:taps)if(!tap.is_number()||!std::isfinite(tap.get<double>()))throw std::runtime_error("nonfinite sample kernel");
    for(const auto& magnitude:target)if(!magnitude.is_number()||!std::isfinite(magnitude.get<double>())||magnitude.get<double>()<0)
        throw std::runtime_error("invalid sample magnitude profile");
}
#endif
}
Payload emptyBuiltinSample(std::string_view uid,std::int32_t slot) {
    return {{"kind","adi.builtin.sample"},{"version",1},{"uid",uid},{"slot",slot},{"empty",true}};
}
BuiltinSampleInfo inspectBuiltinSample(const Payload& value) {
    const auto& kind=value.at("kind");
    if(!kind.is_string() || kind.get_ref<const std::string&>()!="adi.builtin.sample" || value.at("version")!=1)throw std::runtime_error("unsupported built-in sample state");
    BuiltinSampleInfo info;info.uid=value.at("uid").get<std::string>();
    const auto slot=value.at("slot").get<std::int64_t>();
    // Color Cab, OneShot and Hybrid Reverb declare slot 1; the Sampler 1..32 (DEV-21).
    const bool single=info.uid==colorCabUid || info.uid==oneShotUid || info.uid==hybridReverbUid;
    const bool sampler=info.uid==samplerUid;
    if(!value.at("slot").is_number_integer() || !((single && slot==1) || (sampler && slot>=1 && slot<=32))) {
        throw std::runtime_error("unsupported built-in sample slot");
    }
    info.slot=static_cast<std::int32_t>(slot);info.empty=value.at("empty").get<bool>();
    if(!info.empty){info.mediaHash=value.at("mediaHash").get<std::string>();if(!hashValid(info.mediaHash))throw std::runtime_error("invalid sample media hash");}
    return info;
}
media::PeakResult buildBuiltinSamplePeaks(const Payload &value, std::stop_token stop) {
    using media::PeakProblem;
    if (audio::onAudioThread) return {PeakProblem::audioThread, {}, {}};
    try {
        const auto info = inspectBuiltinSample(value);
        if (info.empty) return {PeakProblem::invalidSource, {}, {}};
        const auto channels = value.value("channels", 1);
        const auto rate = value.at("sourceRate").get<double>();
        validRate(rate);
        const auto &bytes = value.at("source_f32le").get_binary();
        if (channels < 1 || channels > 2 || rate < 1 || bytes.empty() ||
            bytes.size() % (4 * static_cast<std::size_t>(channels)) != 0 || bytes.size() / 4 > builtinSampleMaxFrames)
            return {PeakProblem::invalidSource, "Invalid sample backup dimensions.", {}};
        media::MediaPeaks peaks;
        peaks.contentHash = info.mediaHash;
        peaks.sampleRate = static_cast<std::uint32_t>(rate);
        peaks.channels = static_cast<std::uint32_t>(channels);
        peaks.frames = bytes.size() / (4 * peaks.channels);
        constexpr auto binsLimit = std::uint64_t{4096};
        peaks.framesPerBin = (peaks.frames + binsLimit - 1) / binsLimit;
        const auto bins = (peaks.frames + peaks.framesPerBin - 1) / peaks.framesPerBin;
        peaks.bins.resize(static_cast<std::size_t>(bins * peaks.channels));
        for (std::uint64_t bin = 0; bin < bins; ++bin) {
            if (stop.stop_requested()) return {PeakProblem::cancelled, {}, {}};
            const auto first = bin * peaks.framesPerBin;
            const auto last = std::min(peaks.frames, first + peaks.framesPerBin);
            for (std::uint32_t ch = 0; ch < peaks.channels; ++ch) {
                auto &range = peaks.bins[static_cast<std::size_t>(bin * peaks.channels + ch)];
                const auto sample = [&](std::uint64_t frame) {
                    return sourceSample(bytes, static_cast<std::size_t>(frame * peaks.channels + ch));
                };
                range.minimum = range.maximum = sample(first);
                if (!std::isfinite(range.minimum)) return {PeakProblem::invalidSource, "Invalid sample backup audio.", {}};
                for (auto frame = first + 1; frame < last; ++frame) {
                    const auto amplitude = sample(frame);
                    if (!std::isfinite(amplitude)) return {PeakProblem::invalidSource, "Invalid sample backup audio.", {}};
                    range.minimum = std::min(range.minimum, amplitude);
                    range.maximum = std::max(range.maximum, amplitude);
                }
            }
        }
        return {PeakProblem::none, {}, std::move(peaks)};
    } catch (const std::exception &e) {
        return {PeakProblem::invalidSource, e.what(), {}};
    }
}
Payload captureBuiltinSample(std::string_view uid,std::int32_t slot,const std::filesystem::path& source,
    const std::filesystem::path& playable,double rate) {
    offThread();auto value=emptyBuiltinSample(uid,slot);auto info=inspectBuiltinSample(value);validRate(rate);
#if ADI_WITH_PD
    audio::WavReader reader(playable);
    if(reader.frames()==0||reader.frames()>builtinSampleMaxFrames||reader.channels()==0||reader.channels()>64)
        throw std::runtime_error("sample is empty or exceeds the 8M-frame preparation limit");
    if(info.uid==oneShotUid || info.uid==hybridReverbUid || info.uid==samplerUid) {
        // DEV-21: OneShot plays the source itself, and Hybrid Reverb convolves with it, in stereo when it has two
        // channels or more (the first two), at the source's own rate.
        const std::uint16_t channels=reader.channels()>=2?2:1;
        if(!builtinSampleStateFits(reader.frames(),channels)) {
            throw std::runtime_error("sample is too long for a built-in's state (a stereo sample holds at most 4M frames)");
        }
        std::vector<float> interleaved(static_cast<std::size_t>(reader.frames())*channels);
        std::vector<float> block(4096u*reader.channels());std::size_t done=0;
        while(done<static_cast<std::size_t>(reader.frames())) {
            const auto want=static_cast<std::uint32_t>(std::min<std::size_t>(4096,static_cast<std::size_t>(reader.frames())-done));
            if(reader.read(block.data(),want)!=want)throw std::runtime_error("sample ended early");
            for(std::uint32_t frame=0;frame<want;++frame)for(std::uint16_t c=0;c<channels;++c) {
                const float sample=block[static_cast<std::size_t>(frame)*reader.channels()+c];
                if(!std::isfinite(sample))throw std::runtime_error("sample contains nonfinite audio");
                interleaved[(done+frame)*channels+c]=sample;
            }
            done+=want;
        }
        const auto hash=media::blake3File(source);if(!hash)throw std::runtime_error("sample media hashing failed");
        value["empty"]=false;value["mediaHash"]=hash.hex;value["sourceRate"]=reader.sampleRate();value["channels"]=channels;
        std::vector<std::uint8_t> bytes(interleaved.size()*4);
        for(std::size_t i=0;i<interleaved.size();++i) {
            const auto bits=std::bit_cast<std::uint32_t>(interleaved[i]);for(unsigned byte=0;byte<4;++byte)bytes[i*4+byte]=static_cast<std::uint8_t>((bits>>(8*byte))&255u);
        }
        value["source_f32le"]=Payload::binary(std::move(bytes));
        return value;
    }
    std::vector<float> mono(static_cast<std::size_t>(reader.frames()));std::vector<float> block(4096u*reader.channels());
    std::size_t done=0;
    while(done<mono.size()) {
        const auto want=static_cast<std::uint32_t>(std::min<std::size_t>(4096,mono.size()-done));
        if(reader.read(block.data(),want)!=want)throw std::runtime_error("sample ended early");
        for(std::uint32_t frame=0;frame<want;++frame) {
            double sum=0;for(std::uint16_t channel=0;channel<reader.channels();++channel) {
                const float sample=block[static_cast<std::size_t>(frame)*reader.channels()+channel];
                if(!std::isfinite(sample))throw std::runtime_error("sample contains nonfinite audio");
                sum+=sample;
            }
            mono[done+frame]=static_cast<float>(sum/reader.channels());
        }
        done+=want;
    }
    const auto hash=media::blake3File(source);if(!hash)throw std::runtime_error("sample media hashing failed");
    info.empty=false;info.mediaHash=hash.hex;value["empty"]=false;value["mediaHash"]=hash.hex;value["sourceRate"]=reader.sampleRate();
    std::vector<std::uint8_t> bytes(mono.size()*4);
    for(std::size_t i=0;i<mono.size();++i) {
        const auto bits=std::bit_cast<std::uint32_t>(mono[i]);for(unsigned byte=0;byte<4;++byte)bytes[i*4+byte]=static_cast<std::uint8_t>((bits>>(8*byte))&255u);
    }
    value["source_f32le"]=Payload::binary(std::move(bytes));
    auto prepared=design(mono,reader.sampleRate(),rate,info);if(!prepared)throw std::runtime_error("sample preparation refused");
    captureDesign(value,dynamic_cast<const PdColorCabSample&>(*prepared));return value;
#else
    (void)source;(void)playable;(void)info;throw std::runtime_error("built-in sample backend is disabled");
#endif
}
std::unique_ptr<PdSampleBuffer> restoreBuiltinSample(const Payload& value,double rate) {
    offThread();const auto info=inspectBuiltinSample(value);validRate(rate);
#if ADI_WITH_PD
    if(info.uid==oneShotUid || info.uid==samplerUid) {
        // DEV-21: a Sampler slot is a OneShot sample, as OneShot's own slot is.
        if(info.empty)return clearOneShotSample(info.slot);
        const auto channels=value.at("channels").get<int>();const double sourceRate=value.at("sourceRate").get<double>();validRate(sourceRate);
        if(channels<1||channels>2)throw std::runtime_error("invalid OneShot sample channels");
        auto interleaved=sourceMono(value);   // the raw floats, any channel count
        if(interleaved.size()%static_cast<std::size_t>(channels)!=0)throw std::runtime_error("invalid OneShot sample length");
        auto raw=std::make_unique<PdSampleBuffer>();raw->id=info.slot;raw->channels=channels;raw->sampleRate=sourceRate;
        raw->frames=static_cast<std::int64_t>(interleaved.size()/static_cast<std::size_t>(channels));
        raw->interleaved=std::move(interleaved);raw->blake3=info.mediaHash;
        auto prepared=prepareOneShotSample(std::move(raw));
        if(!prepared)throw std::runtime_error("OneShot sample preparation refused");
        return prepared;
    }
    if(info.uid==hybridReverbUid) {
        // DEV-22: the impulse response at the device's rate. Resampling keeps a
        // signal's amplitude, which would scale an IR's DC gain by the rate
        // ratio; scaled back, a reverb is as loud at every session rate.
        if(info.empty){auto result=std::make_unique<PdHybridReverbSample>();result->id=info.slot;result->impulse.sampleRate=0;return result;}  // no rate: the core convolves with nothing
        const auto channels=value.at("channels").get<int>();const double sourceRate=value.at("sourceRate").get<double>();validRate(sourceRate);
        if(channels<1||channels>2)throw std::runtime_error("invalid Hybrid Reverb impulse channels");
        const auto interleaved=sourceMono(value);
        if(interleaved.size()%static_cast<std::size_t>(channels)!=0)throw std::runtime_error("invalid Hybrid Reverb impulse length");
        const auto frames=interleaved.size()/static_cast<std::size_t>(channels);
        std::vector<std::vector<float>> planes(static_cast<std::size_t>(channels));
        for(std::size_t c=0;c<planes.size();++c) {
            std::vector<float> plane(frames);
            for(std::size_t i=0;i<frames;++i)plane[i]=interleaved[i*planes.size()+c];
            // An IR whose sound starts on its very first sample loses the
            // leading half of that band-limited pulse, which falls before time
            // zero: about 0.5 dB of that first click from 44.1 to 48 kHz. Kept
            // so, since keeping it would delay every IR; at the session's own
            // rate nothing is resampled.
            planes[c]=atRate(plane,sourceRate,rate);
            const auto gain=static_cast<float>(sourceRate/rate);
            for(auto& sample:planes[c])sample*=gain;
        }
        const auto length=planes[0].size();
        auto raw=std::make_unique<PdSampleBuffer>();raw->id=info.slot;raw->channels=channels;raw->sampleRate=rate;
        raw->frames=static_cast<std::int64_t>(length);raw->blake3=info.mediaHash;raw->interleaved.resize(length*planes.size());
        for(std::size_t i=0;i<length;++i)for(std::size_t c=0;c<planes.size();++c)raw->interleaved[i*planes.size()+c]=planes[c][i];
        auto prepared=prepareHybridReverbSample(std::move(raw));
        if(!prepared)throw std::runtime_error("Hybrid Reverb impulse preparation refused");
        return prepared;
    }
    if(info.empty) {
        auto result=std::make_unique<PdColorCabSample>();result->id=info.slot;result->design.sampleRate=rate;
        result->design.kernel.taps[0]=1;result->design.kernel.size=1;return result;
    }
    validateDesign(value);auto mono=sourceMono(value);const double sourceRate=value.at("sourceRate").get<double>();validRate(sourceRate);
    const auto& derived=value.at("derived");
    if(derived.at("rate").get<double>()!=rate)return design(mono,sourceRate,rate,info);
    auto result=std::make_unique<PdColorCabSample>();result->id=info.slot;result->blake3=info.mediaHash;result->sampleRate=rate;
    result->design.sampleRate=rate;result->design.target=derived.at("target").get<std::vector<double>>();
    const auto taps=derived.at("kernel").get<std::vector<double>>();result->design.kernel.size=taps.size();
    std::copy(taps.begin(),taps.end(),result->design.kernel.taps.begin());return result;
#else
    (void)info;throw std::runtime_error("built-in sample backend is disabled");
#endif
}
}
