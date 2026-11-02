// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/builtin_samples.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#if ADI_WITH_PD
#include "adi/pd_builtins/colorcab_sample.hpp"
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
void offThread(){if(audio::onAudioThread)throw std::runtime_error("sample preparation belongs off the audio thread");}
void validRate(double rate){if(!std::isfinite(rate)||rate<=0||rate>768000)throw std::runtime_error("invalid sample processing rate");}
bool hashValid(const std::string& hash){return hash.size()==64 && std::all_of(hash.begin(),hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
#if ADI_WITH_PD
std::vector<float> sourceMono(const Payload& value) {
    const auto& bytes=value.at("source_f32le").get_binary();
    if(bytes.empty() || bytes.size()%4!=0 || bytes.size()/4>builtinSampleMaxFrames)throw std::runtime_error("invalid sample backup length");
    std::vector<float> result(bytes.size()/4);
    for(std::size_t i=0;i<result.size();++i) {
        const auto offset=i*4;const std::uint32_t bits=static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset+1])<<8) | (static_cast<std::uint32_t>(bytes[offset+2])<<16) |
            (static_cast<std::uint32_t>(bytes[offset+3])<<24);
        result[i]=std::bit_cast<float>(bits);if(!std::isfinite(result[i]))throw std::runtime_error("nonfinite sample backup");
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
    if(value.at("kind")!="adi.builtin.sample" || value.at("version")!=1)throw std::runtime_error("unsupported built-in sample state");
    BuiltinSampleInfo info;info.uid=value.at("uid").get<std::string>();
    const auto slot=value.at("slot").get<std::int64_t>();
    if(!value.at("slot").is_number_integer()||slot!=1||info.uid!=colorCabUid)throw std::runtime_error("unsupported built-in sample slot");
    info.slot=static_cast<std::int32_t>(slot);info.empty=value.at("empty").get<bool>();
    if(!info.empty){info.mediaHash=value.at("mediaHash").get<std::string>();if(!hashValid(info.mediaHash))throw std::runtime_error("invalid sample media hash");}
    return info;
}
Payload captureBuiltinSample(std::string_view uid,std::int32_t slot,const std::filesystem::path& source,
    const std::filesystem::path& playable,double rate) {
    offThread();auto value=emptyBuiltinSample(uid,slot);auto info=inspectBuiltinSample(value);validRate(rate);
#if ADI_WITH_PD
    audio::WavReader reader(playable);
    if(reader.frames()==0||reader.frames()>builtinSampleMaxFrames||reader.channels()==0||reader.channels()>64)
        throw std::runtime_error("sample is empty or exceeds the 8M-frame preparation limit");
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
