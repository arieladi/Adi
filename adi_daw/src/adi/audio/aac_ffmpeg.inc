// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled twice with the matching pinned public headers. This is ADI's adapter,
// not FFmpeg implementation code. Never access one ABI with another's layouts.
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/wav_file.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
namespace adi::audio {
namespace {
struct Api {
    void *codec = nullptr;
    void *util = nullptr;
#define ADI_FF(name) decltype(&name) name##_ = nullptr
    ADI_FF(avcodec_version);
    ADI_FF(avutil_version);
    ADI_FF(avcodec_find_encoder);
    ADI_FF(avcodec_find_decoder);
    ADI_FF(avcodec_alloc_context3);
    ADI_FF(avcodec_free_context);
    ADI_FF(avcodec_open2);
    ADI_FF(avcodec_send_frame);
    ADI_FF(avcodec_receive_packet);
    ADI_FF(avcodec_send_packet);
    ADI_FF(avcodec_receive_frame);
    ADI_FF(av_packet_alloc);
    ADI_FF(av_packet_free);
    ADI_FF(av_packet_unref);
    ADI_FF(av_new_packet);
    ADI_FF(av_frame_alloc);
    ADI_FF(av_frame_free);
    ADI_FF(av_frame_get_buffer);
    ADI_FF(av_frame_make_writable);
    ADI_FF(av_channel_layout_default);
    ADI_FF(av_mallocz);
#undef ADI_FF
    Api() {
        codec = dlopen(ADI_AVCODEC_NAME, RTLD_NOW | RTLD_LOCAL);
        util = dlopen(ADI_AVUTIL_NAME, RTLD_NOW | RTLD_LOCAL);
        try {
            if (!codec || !util)
                throw std::runtime_error("AAC codec unavailable: install supported libavcodec/libavutil");
#define ADI_LOAD(handle, name)                                                                               \
    name##_ = reinterpret_cast<decltype(name##_)>(dlsym(handle, #name));                                     \
    if (!name##_)                                                                                            \
    throw std::runtime_error("AAC runtime is missing " #name)
            ADI_LOAD(codec, avcodec_version);
            ADI_LOAD(util, avutil_version);
            if ((avcodec_version_() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
                (avutil_version_() >> 16) != LIBAVUTIL_VERSION_MAJOR)
                throw std::runtime_error("AAC runtime ABI does not match its pinned headers");
            ADI_LOAD(codec, avcodec_find_encoder);
            ADI_LOAD(codec, avcodec_find_decoder);
            ADI_LOAD(codec, avcodec_alloc_context3);
            ADI_LOAD(codec, avcodec_free_context);
            ADI_LOAD(codec, avcodec_open2);
            ADI_LOAD(codec, avcodec_send_frame);
            ADI_LOAD(codec, avcodec_receive_packet);
            ADI_LOAD(codec, avcodec_send_packet);
            ADI_LOAD(codec, avcodec_receive_frame);
            ADI_LOAD(codec, av_packet_alloc);
            ADI_LOAD(codec, av_packet_free);
            ADI_LOAD(codec, av_packet_unref);
            ADI_LOAD(codec, av_new_packet);
            ADI_LOAD(util, av_frame_alloc);
            ADI_LOAD(util, av_frame_free);
            ADI_LOAD(util, av_frame_get_buffer);
            ADI_LOAD(util, av_frame_make_writable);
            ADI_LOAD(util, av_channel_layout_default);
            ADI_LOAD(util, av_mallocz);
#undef ADI_LOAD
        } catch (...) {
            close();
            throw;
        }
    }
    void close() noexcept {
        if (codec)
            dlclose(codec);
        if (util)
            dlclose(util);
        codec = nullptr;
        util = nullptr;
    }
    ~Api() {
        close();
    }
    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;
};
void check(int status, const char *operation) {
    if (status < 0)
        throw std::runtime_error(std::string("AAC ") + operation + " failed (" + std::to_string(status) +
                                 ")");
}
struct State {
    Api api;
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    State() = default;
    ~State() {
        api.av_packet_free_(&packet);
        api.av_frame_free_(&frame);
        api.avcodec_free_context_(&context);
    }
    void allocate(const AVCodec *codec) {
        if (!codec)
            throw std::runtime_error("AAC codec unavailable in this libavcodec build");
        context = api.avcodec_alloc_context3_(codec);
        frame = api.av_frame_alloc_();
        packet = api.av_packet_alloc_();
        if (!context || !frame || !packet)
            throw std::bad_alloc();
    }
};
void checkpoint(const CodecCheckpoint &call) {
    if (call)
        call();
}
} // namespace
bool ADI_FF_AVAILABLE() noexcept {
    try {
        Api api;
        return api.avcodec_find_encoder_(AV_CODEC_ID_AAC) && api.avcodec_find_decoder_(AV_CODEC_ID_AAC);
    } catch (...) {
        return false;
    }
}
AacTrack ADI_FF_ENCODE(const std::filesystem::path &wav, const std::filesystem::path &path,
                       std::uint32_t bitrate, CodecCheckpoint call) {
    WavReader reader(wav);
    if (reader.channels() != 2 || (reader.sampleRate() != 44100 && reader.sampleRate() != 48000) ||
        !reader.frames() || reader.frames() > 999990ULL * 1024)
        throw std::invalid_argument(
            "AAC export requires nonempty stereo 44.1/48 kHz audio within the packet limit");
    if (bitrate < 64000 || bitrate > 320000)
        throw std::invalid_argument("AAC bit rate must be 64000..320000");
    State s;
    s.allocate(s.api.avcodec_find_encoder_(AV_CODEC_ID_AAC));
    auto *c = s.context;
    c->sample_rate = static_cast<int>(reader.sampleRate());
    c->bit_rate = bitrate;
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->time_base = {1, c->sample_rate};
    s.api.av_channel_layout_default_(&c->ch_layout, 2);
    check(s.api.avcodec_open2_(c, c->codec, nullptr), "encoder open");
    if (c->frame_size != 1024 || c->initial_padding < 0 || c->extradata_size < 2 || c->extradata_size > 64)
        throw std::runtime_error("AAC encoder returned unsupported frame mode or config");
    AacTrack result;
    result.file = path;
    result.sampleRate = reader.sampleRate();
    result.frames = reader.frames();
    result.priming = static_cast<std::uint64_t>(c->initial_padding);
    result.config.assign(c->extradata, c->extradata + c->extradata_size);
    (void)aacSampleRate(result.config);
    s.frame->format = c->sample_fmt;
    s.frame->sample_rate = c->sample_rate;
    s.frame->nb_samples = 1024;
    s.api.av_channel_layout_default_(&s.frame->ch_layout, 2);
    check(s.api.av_frame_get_buffer_(s.frame, 0), "frame allocation");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("AAC packet file create failed");
    std::uint64_t offset = 0;
    std::int64_t pts = 0;
    auto receive = [&] {
        for (;;) {
            checkpoint(call);
            const auto status = s.api.avcodec_receive_packet_(c, s.packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
                break;
            check(status, "encode");
            if (s.packet->size <= 0 || s.packet->size > 1024 * 1024 || result.packets.size() >= 1000000)
                throw std::runtime_error("AAC encoder exceeded packet bounds");
            out.write(reinterpret_cast<const char *>(s.packet->data), s.packet->size);
            if (!out)
                throw std::runtime_error("AAC packet file write failed");
            result.packets.push_back({offset, static_cast<std::uint32_t>(s.packet->size)});
            offset += static_cast<std::uint32_t>(s.packet->size);
            s.api.av_packet_unref_(s.packet);
        }
    };
    std::array<float, 2048> pcm{};
    for (;;) {
        checkpoint(call);
        const auto frames = reader.read(pcm.data(), 1024);
        if (!frames)
            break;
        check(s.api.av_frame_make_writable_(s.frame), "writable frame");
        for (std::size_t channel = 0; channel < 2; ++channel) {
            auto *dest = reinterpret_cast<float *>(s.frame->data[channel]);
            for (std::uint32_t i = 0; i < 1024; ++i) {
                const auto v = i < frames ? pcm[i * 2 + channel] : 0.0f;
                if (!std::isfinite(v) || std::abs(v) > 1.0f)
                    throw std::runtime_error("AAC input exceeds finite [-1,1]; lower the render gain");
                dest[i] = v;
            }
        }
        s.frame->pts = pts;
        pts += 1024;
        check(s.api.avcodec_send_frame_(c, s.frame), "send frame");
        receive();
    }
    check(s.api.avcodec_send_frame_(c, nullptr), "encoder flush");
    receive();
    out.close();
    if (!out)
        throw std::runtime_error("AAC packet close failed");
    return result;
}
void ADI_FF_DECODE(const AacTrack &track, const std::filesystem::path &wav, CodecCheckpoint call) {
    if (aacSampleRate(track.config) != track.sampleRate)
        throw std::invalid_argument("AAC decode config mismatch");
    State s;
    s.allocate(s.api.avcodec_find_decoder_(AV_CODEC_ID_AAC));
    auto *c = s.context;
    c->extradata =
        static_cast<std::uint8_t *>(s.api.av_mallocz_(track.config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!c->extradata)
        throw std::bad_alloc();
    std::copy(track.config.begin(), track.config.end(), c->extradata);
    c->extradata_size = static_cast<int>(track.config.size());
    check(s.api.avcodec_open2_(c, c->codec, nullptr), "decoder open");
    std::ifstream input(track.file, std::ios::binary);
    if (!input)
        throw std::runtime_error("AAC packet source open failed");
    WavWriter writer(wav, track.sampleRate, 2, WavFormat::Float32);
    std::uint64_t decoded = 0, written = 0;
    auto receive = [&] {
        for (;;) {
            checkpoint(call);
            const auto status = s.api.avcodec_receive_frame_(c, s.frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
                break;
            check(status, "decode");
            if (s.frame->format != AV_SAMPLE_FMT_FLTP || s.frame->ch_layout.nb_channels != 2 ||
                s.frame->sample_rate != static_cast<int>(track.sampleRate) || s.frame->nb_samples <= 0 ||
                s.frame->nb_samples > 2048)
                throw std::runtime_error("AAC decoder returned an unsupported frame");
            std::array<float, 4096> pcm{};
            std::uint32_t count = 0;
            for (int i = 0; i < s.frame->nb_samples; ++i, ++decoded) {
                if (decoded < track.priming || written >= track.frames)
                    continue;
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    const auto v = reinterpret_cast<const float *>(s.frame->data[channel])[i];
                    if (!std::isfinite(v))
                        throw std::runtime_error("nonfinite AAC decode");
                    pcm[count * 2 + channel] = v;
                }
                ++count;
                ++written;
            }
            if (count)
                writer.write(pcm.data(), count);
        }
    };
    for (const auto &packet : track.packets) {
        checkpoint(call);
        if (!packet.size || packet.size > 1024 * 1024 ||
            packet.offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
            throw std::runtime_error("invalid AAC packet reference");
        check(s.api.av_new_packet_(s.packet, static_cast<int>(packet.size)), "packet allocation");
        input.clear();
        input.seekg(static_cast<std::streamoff>(packet.offset));
        input.read(reinterpret_cast<char *>(s.packet->data), packet.size);
        if (!input)
            throw std::runtime_error("AAC packet read failed");
        check(s.api.avcodec_send_packet_(c, s.packet), "send packet");
        s.api.av_packet_unref_(s.packet);
        receive();
    }
    check(s.api.avcodec_send_packet_(c, nullptr), "decoder flush");
    receive();
    if (written != track.frames)
        throw std::runtime_error("AAC decoder produced incomplete audio after priming");
    writer.close();
}
} // namespace adi::audio
