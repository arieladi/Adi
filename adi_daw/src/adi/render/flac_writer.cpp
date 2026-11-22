// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/flac_writer.hpp"

#include <stdexcept>
#if defined(ADI_HAVE_FLAC)
#include "adi/audio/wav_file.hpp"
#include <FLAC/stream_encoder.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>
#endif

namespace adi::render {

bool flacExportAvailable() noexcept {
#if defined(ADI_HAVE_FLAC)
    return true;
#else
    return false;
#endif
}

#if defined(ADI_HAVE_FLAC)
namespace {
using Encoder=FLAC__StreamEncoder;

FLAC__StreamEncoderWriteStatus writeBytes(const Encoder*,const FLAC__byte buffer[],std::size_t bytes,
                                          unsigned,unsigned,void* context) {
    auto& output=*static_cast<std::ofstream*>(context);
    if(bytes>static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    output.write(reinterpret_cast<const char*>(buffer),static_cast<std::streamsize>(bytes));
    return output ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}
FLAC__StreamEncoderSeekStatus seek(const Encoder*,FLAC__uint64 offset,void* context) {
    auto& output=*static_cast<std::ofstream*>(context);
    if(offset>static_cast<FLAC__uint64>(std::numeric_limits<std::streamoff>::max()))
        return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    output.seekp(static_cast<std::streamoff>(offset));
    return output ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
}
FLAC__StreamEncoderTellStatus tell(const Encoder*,FLAC__uint64* offset,void* context) {
    auto& output=*static_cast<std::ofstream*>(context);
    const auto position=output.tellp();
    if(position==std::streampos(-1))return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
    *offset=static_cast<FLAC__uint64>(static_cast<std::streamoff>(position));
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}
}
#endif

void encodeStagedFlac(const std::filesystem::path& wave,const std::filesystem::path& flac,
                      const std::function<void()>& service) {
#if defined(ADI_HAVE_FLAC)
    audio::WavReader input(wave);
    if(input.format()!=audio::WavFormat::Pcm16 && input.format()!=audio::WavFormat::Pcm24)
        throw std::invalid_argument("FLAC export requires PCM16 or PCM24");
    const unsigned bits=input.format()==audio::WavFormat::Pcm16?16U:24U;
    const auto channels=input.channels();
    if(channels==0 || channels>8 || input.frames()==0 || input.frames()>=(std::uint64_t{1}<<36))
        throw std::invalid_argument("FLAC export needs 1..8 channels and a representable frame count");
    std::ofstream output(flac,std::ios::binary|std::ios::trunc);
    if(!output)throw std::runtime_error("Cannot open staged FLAC");
    std::unique_ptr<Encoder,decltype(&FLAC__stream_encoder_delete)>
        encoder(FLAC__stream_encoder_new(),FLAC__stream_encoder_delete);
    if(!encoder || !FLAC__stream_encoder_set_channels(encoder.get(),channels) ||
       !FLAC__stream_encoder_set_bits_per_sample(encoder.get(),bits) ||
       !FLAC__stream_encoder_set_sample_rate(encoder.get(),input.sampleRate()) ||
       !FLAC__stream_encoder_set_total_samples_estimate(encoder.get(),input.frames()) ||
       FLAC__stream_encoder_init_stream(encoder.get(),writeBytes,seek,tell,nullptr,&output)!=FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        throw std::runtime_error("Cannot configure staged FLAC encoding");

    constexpr std::uint32_t blockFrames=4096;
    std::vector<float> samples(static_cast<std::size_t>(channels)*blockFrames);
    std::vector<FLAC__int32> pcm(samples.size());
    for(std::uint64_t first=0;first<input.frames();) {
        if(service)service();
        const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,input.frames()-first));
        if(input.read(samples.data(),count)!=count)throw std::runtime_error("Incomplete PCM stage for FLAC");
        const auto values=static_cast<std::size_t>(count)*channels;
        // Every PCM16/24 value that WavReader returns is exactly representable
        // in Float32. Scaling by a power of two recovers the original code.
        for(std::size_t i=0;i<values;++i)pcm[i]=static_cast<FLAC__int32>(std::ldexp(samples[i],static_cast<int>(bits)-1));
        if(!FLAC__stream_encoder_process_interleaved(encoder.get(),pcm.data(),count))
            throw std::runtime_error("Cannot encode FLAC samples");
        first+=count;
    }
    if(service)service();
    if(!FLAC__stream_encoder_finish(encoder.get()))throw std::runtime_error("Cannot finish staged FLAC");
    output.close();
    if(!output)throw std::runtime_error("Cannot close staged FLAC");
#else
    (void)wave;(void)flac;(void)service;
    throw std::runtime_error("FLAC export requires the pinned libFLAC build");
#endif
}

} // namespace adi::render
