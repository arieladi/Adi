// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/vorbis_writer.hpp"

#include <stdexcept>

#ifdef ADI_WITH_VORBIS_EXPORT
#include "adi/audio/pcm_stream.hpp"

#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <vector>
#endif

namespace adi::render {

#ifdef ADI_WITH_VORBIS_EXPORT

struct VorbisWriter::Impl {
    static constexpr std::uint32_t batchFrames = 1024;

    Impl(const std::filesystem::path& file, unsigned rate, unsigned count, std::uint64_t total)
        : path(file), output(file, std::ios::binary), sampleRate(rate), channels(count), expected(total) {
        if (!output) throw std::runtime_error("cannot open staged Ogg output");
        vorbis_info_init(&info);
        vorbis_comment_init(&comment);
    }

    ~Impl() {
        if (streamReady) ogg_stream_clear(&stream);
        if (blockReady) vorbis_block_clear(&block);
        if (dspReady) vorbis_dsp_clear(&dsp);
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
    }

    void start(float quality) {
        if (vorbis_encode_init_vbr(&info, static_cast<long>(channels),
                                   static_cast<long>(sampleRate), quality) != 0 ||
            vorbis_analysis_init(&dsp, &info) != 0) {
            throw std::runtime_error("Vorbis rejects the export settings");
        }
        dspReady = true;
        if (vorbis_block_init(&dsp, &block) != 0) throw std::runtime_error("cannot allocate Vorbis blocks");
        blockReady = true;
        if (ogg_stream_init(&stream, 0x41444931) != 0) throw std::runtime_error("cannot create Ogg stream");
        streamReady = true;
        ogg_packet header{}, tags{}, setup{};
        if (vorbis_analysis_headerout(&dsp, &comment, &header, &tags, &setup) != 0) {
            throw std::runtime_error("cannot write Vorbis headers");
        }
        packet(header);
        packet(tags);
        packet(setup);
        pages(true);
    }

    void packet(ogg_packet& value) {
        if (ogg_stream_packetin(&stream, &value) != 0) {
            throw std::runtime_error("Ogg rejected a Vorbis packet");
        }
    }

    void pages(bool flush) {
        ogg_page page{};
        auto next = flush ? ogg_stream_flush : ogg_stream_pageout;
        while (next(&stream, &page) != 0) {
            output.write(reinterpret_cast<const char*>(page.header), page.header_len);
            output.write(reinterpret_cast<const char*>(page.body), page.body_len);
            if (!output) throw std::runtime_error("cannot write staged Ogg output");
            if (ogg_page_eos(&page) != 0) {
                sawEnd = true;
                if (ogg_page_granulepos(&page) != static_cast<ogg_int64_t>(expected)) {
                    throw std::runtime_error("Vorbis end position disagrees with the render length");
                }
            }
        }
    }

    void drain() {
        int available = 0;
        while ((available = vorbis_analysis_blockout(&dsp, &block)) == 1) {
            if (vorbis_analysis(&block, nullptr) != 0 || vorbis_bitrate_addblock(&block) != 0) {
                throw std::runtime_error("Vorbis could not encode an analysis block");
            }
            ogg_packet encoded{};
            int packets = 0;
            while ((packets = vorbis_bitrate_flushpacket(&dsp, &encoded)) == 1) {
                packet(encoded);
                pages(false);
            }
            if (packets != 0) throw std::runtime_error("Vorbis could not flush an encoded packet");
        }
        if (available != 0) throw std::runtime_error("Vorbis analysis failed");
    }

    void submit() {
        if (vorbis_analysis_wrote(&dsp, static_cast<int>(pendingFrames)) != 0) {
            throw std::runtime_error("Vorbis refused an audio block");
        }
        pendingFrames = 0;
        drain();
    }

    std::filesystem::path path;
    std::ofstream output;
    unsigned sampleRate;
    unsigned channels;
    std::uint64_t expected;
    std::uint64_t written = 0;
    std::uint32_t pendingFrames = 0;
    float** input = nullptr; // Borrowed until the next submit.
    vorbis_info info{};
    vorbis_comment comment{};
    vorbis_dsp_state dsp{};
    vorbis_block block{};
    ogg_stream_state stream{};
    bool dspReady = false;
    bool blockReady = false;
    bool streamReady = false;
    bool sawEnd = false;
};

bool vorbisExportAvailable() noexcept { return true; }

VorbisWriter::VorbisWriter(const std::filesystem::path& path, unsigned rate, unsigned channels,
                           std::uint64_t frames, float quality) {
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2 || frames == 0 ||
        frames > static_cast<std::uint64_t>(std::numeric_limits<ogg_int64_t>::max()) ||
        !std::isfinite(quality) || quality < -0.1f || quality > 1.0f) {
        throw std::invalid_argument("invalid Vorbis settings");
    }
    impl_ = std::make_unique<Impl>(path, rate, channels, frames);
    impl_->start(quality);
}

VorbisWriter::~VorbisWriter() = default;

void VorbisWriter::write(std::span<const double> samples, std::uint32_t frames) {
    auto& state = *impl_;
    if (frames == 0 || frames > 65536 || frames > state.expected - state.written ||
        samples.size() != static_cast<std::size_t>(frames) * state.channels) {
        throw std::invalid_argument("invalid Vorbis audio block");
    }
    for (const auto value : samples) {
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
            throw std::runtime_error("nonfinite or out-of-range Vorbis input");
        }
    }
    // Equal encoder feeds regardless of the graph's processing block size:
    // the Vorbis analysis window uses the lookahead available at each submit.
    for (std::uint32_t first = 0; first < frames;) {
        if (state.pendingFrames == 0) {
            state.input = vorbis_analysis_buffer(&state.dsp, static_cast<int>(Impl::batchFrames));
            if (!state.input) throw std::runtime_error("cannot allocate a Vorbis audio block");
        }
        const auto count = (std::min)(frames - first, Impl::batchFrames - state.pendingFrames);
        for (unsigned c = 0; c < state.channels; ++c) {
            for (std::uint32_t f = 0; f < count; ++f) {
                state.input[c][state.pendingFrames + f] =
                    static_cast<float>(samples[static_cast<std::size_t>(first + f) * state.channels + c]);
            }
        }
        first += count;
        state.pendingFrames += count;
        if (state.pendingFrames == Impl::batchFrames) state.submit();
    }
    state.written += frames;
}

void VorbisWriter::finish(const std::function<void()>& service, std::optional<std::uint64_t> frames) {
    auto& state = *impl_;
    if (frames && (*frames == 0 || *frames > state.expected || *frames != state.written))
        throw std::runtime_error("Encoded tail length disagrees with the rendered range");
    if (frames) state.expected = *frames;
    if (state.written != state.expected) {
        throw std::runtime_error("Vorbis stream has an incomplete input range");
    }
    if (state.pendingFrames != 0) state.submit();
    if (vorbis_analysis_wrote(&state.dsp, 0) != 0) throw std::runtime_error("cannot close Vorbis input");
    state.drain();
    state.pages(true);
    state.output.close();
    if (!state.output || !state.sawEnd) throw std::runtime_error("cannot finalize staged Ogg output");
    if (service) service();
    auto decoded = audio::openPcmStream(state.path);
    if (!decoded.ready() || decoded.stream->info().channels != state.channels ||
        decoded.stream->info().sampleRate != state.sampleRate ||
        decoded.stream->info().frames != state.expected) {
        throw std::runtime_error("decoded Vorbis header disagrees with the render");
    }
    std::vector<float> buffer(4096u * state.channels);
    do {
        if (service) service();
        const auto read = decoded.stream->read(buffer);
        if (read.status == audio::PcmReadStatus::End) break;
        if (read.status != audio::PcmReadStatus::Frames) {
            throw std::runtime_error("cannot verify the complete staged Vorbis stream");
        }
    } while (true);
    if (decoded.stream->framesRead() != state.expected) {
        throw std::runtime_error("decoded Vorbis length disagrees with the render");
    }
}

#else

struct VorbisWriter::Impl {};

bool vorbisExportAvailable() noexcept { return false; }

VorbisWriter::VorbisWriter(const std::filesystem::path&, unsigned, unsigned, std::uint64_t, float) {
    throw std::runtime_error("Ogg Vorbis export requires the pinned Xiph build");
}

VorbisWriter::~VorbisWriter() = default;

void VorbisWriter::write(std::span<const double>, std::uint32_t) {
    throw std::logic_error("Vorbis writer is unavailable");
}

void VorbisWriter::finish(const std::function<void()>&, std::optional<std::uint64_t>) {
    throw std::logic_error("Vorbis writer is unavailable");
}

#endif

} // namespace adi::render
