// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/opus_writer.hpp"

#include <stdexcept>

#ifdef ADI_HAVE_OPUS_EXPORT
#include <ogg/ogg.h>
#include <opusenc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#endif

namespace adi::render {

bool validOpusBitRate(unsigned channels, std::uint32_t bitRate) noexcept {
    return channels >= 1 && channels <= 2 && bitRate >= 6000 && bitRate <= 256000 * channels;
}

#ifdef ADI_HAVE_OPUS_EXPORT
namespace {

constexpr int sampleRate = 48000;
constexpr int maximumPacketFrames = 5760; // 120 ms at 48 kHz (RFC 7845)

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

void checkEncoder(int status) {
    if (status != OPE_OK) throw std::runtime_error(std::string("Opus encoder: ") + ope_strerror(status));
}

struct OggReadState {
    ogg_sync_state sync{};
    ogg_stream_state stream{};
    bool streamReady = false;

    OggReadState() { require(ogg_sync_init(&sync) == 0, "cannot initialize Ogg verification"); }
    ~OggReadState() {
        if (streamReady) ogg_stream_clear(&stream);
        ogg_sync_clear(&sync);
    }
};

// Verify the bytes written to disk and decode the packets using libopus. The
// independent Ogg reader validates checksums and trims the final packet using
// the granule position and OpusHead pre-skip (RFC 7845 sections 4 and 5).
void verifyOpus(const std::filesystem::path& path, unsigned channels, std::uint64_t expected,
                const std::function<void()>& service) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "cannot reopen staged Opus stream");
    int error = OPUS_OK;
    const std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>
        decoder(opus_decoder_create(sampleRate, static_cast<int>(channels), &error), opus_decoder_destroy);
    require(decoder && error == OPUS_OK, "cannot initialize Opus verification decoder");
    std::vector<float> samples(static_cast<std::size_t>(maximumPacketFrames) * channels);
    OggReadState state;
    unsigned headers = 0;
    unsigned skip = 0;
    std::uint64_t decoded = 0;
    std::uint64_t previousGranule = 0;
    bool ended = false;
    while (true) {
        if (service) service();
        ogg_page page{};
        const int status = ogg_sync_pageout(&state.sync, &page);
        require(status >= 0, "staged Opus has a corrupt Ogg page");
        if (status == 0) {
            constexpr long chunk = 16384;
            auto* bytes = ogg_sync_buffer(&state.sync, chunk);
            require(bytes != nullptr, "cannot allocate Ogg verification buffer");
            file.read(bytes, chunk);
            const auto read = file.gcount();
            require(!file.bad() && ogg_sync_wrote(&state.sync, static_cast<long>(read)) == 0,
                    "cannot read staged Opus bytes");
            if (read == 0) break;
            continue;
        }
        require(!ended, "unexpected pages after Opus stream end");
        if (!state.streamReady) {
            require(ogg_page_bos(&page) != 0 &&
                    ogg_stream_init(&state.stream, ogg_page_serialno(&page)) == 0,
                    "invalid Ogg Opus stream start");
            state.streamReady = true;
        }
        require(ogg_stream_pagein(&state.stream, &page) == 0, "unexpected Ogg Opus page");
        int nextPacket = 0;
        ogg_packet packet{};
        while ((nextPacket = ogg_stream_packetout(&state.stream, &packet)) == 1) {
            const auto* bytes = packet.packet;
            if (headers == 0) {
                require(packet.bytes == 19 && std::memcmp(bytes, "OpusHead", 8) == 0 &&
                        bytes[8] == 1 && bytes[9] == channels && bytes[18] == 0,
                        "unexpected staged Opus channel layout");
                skip = unsigned(bytes[10]) | (unsigned(bytes[11]) << 8);
                ++headers;
            } else if (headers == 1) {
                require(packet.bytes >= 16 && std::memcmp(bytes, "OpusTags", 8) == 0,
                        "missing OpusTags packet");
                ++headers;
            } else {
                require(packet.bytes > 0 && packet.bytes <= std::numeric_limits<opus_int32>::max(),
                        "invalid Opus packet size");
                const auto frames = opus_decode_float(decoder.get(), bytes,
                    static_cast<opus_int32>(packet.bytes), samples.data(), maximumPacketFrames, 0);
                require(frames > 0 && decoded <= std::numeric_limits<std::uint64_t>::max() -
                            static_cast<std::uint64_t>(frames), "cannot decode an Opus delivery packet");
                decoded += static_cast<std::uint64_t>(frames);
            }
        }
        require(nextPacket == 0, "staged Opus stream has a packet hole");
        const auto granule = ogg_page_granulepos(&page);
        ended = ogg_page_eos(&page) != 0;
        if (ended) {
            require(granule >= 0 && static_cast<std::uint64_t>(granule) == expected + skip &&
                    previousGranule <= static_cast<std::uint64_t>(granule) &&
                    static_cast<std::uint64_t>(granule) <= decoded, "Opus trim disagrees with the export range");
        } else if (granule >= 0) {
            require(static_cast<std::uint64_t>(granule) == decoded, "Opus audio page disagrees with its decode position");
            previousGranule = static_cast<std::uint64_t>(granule);
        }
    }
    require(ended && headers == 2 && state.sync.fill == state.sync.returned, "incomplete staged Opus stream");
}

} // namespace

struct OpusWriter::Impl {
    static constexpr std::uint32_t blockFrames = 960;
    Impl(const std::filesystem::path& file, unsigned count, std::uint64_t total)
        : path(file), output(file, std::ios::binary), channels(count), expected(total),
          pending(static_cast<std::size_t>(count) * blockFrames) {
        require(bool(output), "cannot open staged Opus output");
    }

    static int writePage(void* context, const unsigned char* bytes, opus_int32 length) noexcept {
        auto& stream = static_cast<Impl*>(context)->output;
        stream.write(reinterpret_cast<const char*>(bytes), length);
        return stream ? 0 : 1;
    }

    static int closeStream(void*) noexcept { return 0; }

    void start(std::uint32_t bitRate) {
        static constexpr OpusEncCallbacks callbacks{writePage, closeStream};
        const std::unique_ptr<OggOpusComments, decltype(&ope_comments_destroy)>
            comments(ope_comments_create(), ope_comments_destroy);
        require(bool(comments), "cannot allocate Opus comments");
        int error = OPE_OK;
        encoder.reset(ope_encoder_create_callbacks(&callbacks, this, comments.get(), sampleRate,
                                                   static_cast<int>(channels), 0, &error));
        checkEncoder(error);
        require(bool(encoder), "cannot allocate Opus encoder");
        checkEncoder(ope_encoder_ctl(encoder.get(), OPE_SET_SERIALNO(0x41444932)));
        checkEncoder(ope_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(static_cast<int>(bitRate))));
        checkEncoder(ope_encoder_ctl(encoder.get(), OPUS_SET_VBR(1)));
    }

    void flushInput() {
        checkEncoder(ope_encoder_write_float(encoder.get(), pending.data(), static_cast<int>(ready)));
        ready = 0;
    }

    std::filesystem::path path;
    std::ofstream output;
    unsigned channels;
    std::uint64_t expected, written = 0;
    std::vector<float> pending;
    std::uint32_t ready = 0;
    std::unique_ptr<OggOpusEnc, decltype(&ope_encoder_destroy)> encoder{nullptr, ope_encoder_destroy};
};

bool opusExportAvailable() noexcept { return true; }

OpusWriter::OpusWriter(const std::filesystem::path& path, unsigned channels, std::uint64_t frames,
                       std::uint32_t bitRate) {
    if (!validOpusBitRate(channels, bitRate) || frames == 0 ||
        frames > static_cast<std::uint64_t>(std::numeric_limits<ogg_int64_t>::max()) - 65535) {
        throw std::invalid_argument("invalid Opus delivery settings");
    }
    impl_ = std::make_unique<Impl>(path, channels, frames);
    impl_->start(bitRate);
}

OpusWriter::~OpusWriter() = default;

void OpusWriter::write(std::span<const double> pcm, std::uint32_t frames) {
    auto& state = *impl_;
    if (frames > 65536 || frames > state.expected - state.written ||
        pcm.size() != static_cast<std::size_t>(frames) * state.channels) {
        throw std::invalid_argument("invalid Opus audio block");
    }
    for (const auto value : pcm) {
        require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
                "nonfinite or out-of-range Opus input");
    }
    for (std::uint32_t first = 0; first < frames;) {
        const auto take = (std::min)(frames - first, Impl::blockFrames - state.ready);
        const auto source = static_cast<std::size_t>(first) * state.channels;
        const auto destination = static_cast<std::size_t>(state.ready) * state.channels;
        const auto samples = static_cast<std::size_t>(take) * state.channels;
        for (std::size_t i = 0; i < samples; ++i) {
            state.pending[destination + i] = static_cast<float>(pcm[source + i]);
        }
        first += take;
        state.ready += take;
        if (state.ready == Impl::blockFrames) state.flushInput();
    }
    state.written += frames;
}

void OpusWriter::finish(const std::function<void()>& service, std::optional<std::uint64_t> frames) {
    auto& state = *impl_;
    if (frames && (*frames == 0 || *frames > state.expected || *frames != state.written))
        throw std::runtime_error("Encoded tail length disagrees with the rendered range");
    if (frames) state.expected = *frames;
    require(state.written == state.expected, "Opus stream has an incomplete input range");
    if (service) service();
    if (state.ready != 0) state.flushInput();
    checkEncoder(ope_encoder_drain(state.encoder.get()));
    state.encoder.reset();
    state.output.close();
    require(bool(state.output), "cannot finalize staged Opus output");
    verifyOpus(state.path, state.channels, state.expected, service);
}

#else

struct OpusWriter::Impl {};
bool opusExportAvailable() noexcept { return false; }

OpusWriter::OpusWriter(const std::filesystem::path&, unsigned, std::uint64_t, std::uint32_t) {
    throw std::runtime_error("Opus export requires the pinned Xiph build");
}

OpusWriter::~OpusWriter() = default;

void OpusWriter::write(std::span<const double>, std::uint32_t) {
    throw std::logic_error("Opus writer is unavailable");
}

void OpusWriter::finish(const std::function<void()>&, std::optional<std::uint64_t>) {
    throw std::logic_error("Opus writer is unavailable");
}

#endif

} // namespace adi::render
