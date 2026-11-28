// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/mp3_writer.hpp"

#include <stdexcept>
#if defined(ADI_HAVE_LAME)
#include "adi/audio/pcm_stream.hpp"
#include <lame.h>

#include <array>
#include <fstream>
#include <limits>
#include <vector>
#endif

namespace adi::render {

#if defined(ADI_HAVE_LAME)
namespace {
struct CloseLame {
    void operator()(lame_global_flags* encoder) const noexcept { (void)lame_close(encoder); }
};
using Encoder = std::unique_ptr<lame_global_flags, CloseLame>;
constexpr int bitrate = 320;
constexpr std::uint32_t maxInputFrames = 4096;
constexpr std::size_t maxPacketBytes = maxInputFrames * 5 / 4 + 7200; // LAME API bound

void verifyCbr(const std::filesystem::path& path, unsigned rate, unsigned channels,
               const std::function<void()>& service) {
    const auto length = std::filesystem::file_size(path);
    std::ifstream file(path, std::ios::binary);
    if (!file || length == 0) throw std::runtime_error("Cannot reopen staged MP3");
    std::uintmax_t position = 0;
    unsigned checked = 0;
    while (position < length) {
        if (service && (checked++ & 63U) == 0) service();
        std::array<unsigned char, 4> header{};
        file.seekg(static_cast<std::streamoff>(position));
        if (!file.read(reinterpret_cast<char*>(header.data()), 4))
            throw std::runtime_error("Incomplete staged MP3 frame header");
        const auto rateIndex = (header[2] >> 2U) & 3U;
        constexpr std::array<unsigned, 4> sampleRates{44100, 48000, 32000, 0};
        const bool mono = (header[3] >> 6U) == 3U;
        // MPEG-1, Layer III and bitrate code 14 (320 kb/s).
        if (header[0] != 0xff || (header[1] & 0xfeU) != 0xfaU ||
            (header[2] >> 4U) != 14U || sampleRates[rateIndex] != rate || mono != (channels == 1))
            throw std::runtime_error("Staged MP3 does not contain the requested MPEG-1 CBR frames");
        const auto bytes = 144000U * bitrate / rate + ((header[2] >> 1U) & 1U);
        if (bytes > length - position) throw std::runtime_error("Incomplete staged MP3 frame");
        if (position == 0) {
            // MPEG-1 side information comes before LAME's in-band Info tag.
            file.seekg((header[1] & 1U ? 4 : 6) + (mono ? 17 : 32));
            std::array<char, 4> tag{};
            if (!file.read(tag.data(), 4) || tag != std::array{'I', 'n', 'f', 'o'})
                throw std::runtime_error("Staged MP3 has no CBR Info tag");
        }
        position += bytes;
    }
}

void verifyTrim(const std::filesystem::path& path, unsigned rate, unsigned channels,
                std::uint64_t frames, const std::function<void()>& service) {
    if (service) service();
    auto decoded = audio::openPcmStream(path);
    if (!decoded.ready()) throw std::runtime_error("Cannot decode staged MP3: " + decoded.error);
    const auto& info = decoded.stream->info();
    if (info.format != audio::SourceFormat::Mp3 || info.sampleRate != rate ||
        info.channels != channels || info.frames != frames)
        throw std::runtime_error("MP3 encoder delay/padding does not trim to the original range");
    std::vector<float> pcm(static_cast<std::size_t>(maxInputFrames) * channels);
    for (;;) {
        if (service) service();
        const auto block = decoded.stream->read(pcm);
        if (block.status == audio::PcmReadStatus::End) break;
        if (block.status != audio::PcmReadStatus::Frames)
            throw std::runtime_error("Staged MP3 failed full verification decode");
    }
    if (decoded.stream->framesRead() != frames)
        throw std::runtime_error("Decoded MP3 duration differs from the captured audio");
}
} // namespace

struct Mp3Writer::Impl {
    std::filesystem::path path;
    std::ofstream file;
    Encoder encoder{lame_init()};
    std::array<unsigned char, maxPacketBytes> packet{};
    unsigned rate, channels;
    std::uint64_t expected, written = 0;

    Impl(const std::filesystem::path& target, unsigned sr, unsigned ch, std::uint64_t frames)
        : path(target), file(path, std::ios::binary | std::ios::trunc),
          rate(sr), channels(ch), expected(frames) {
        if (!file || !encoder || frames == 0 || frames > std::numeric_limits<unsigned long>::max() ||
            (rate != 32000 && rate != 44100 && rate != 48000) || (channels != 1 && channels != 2))
            throw std::invalid_argument("Cannot initialize staged MPEG-1 MP3");
        auto* lame = encoder.get();
        lame_set_write_id3tag_automatic(lame, 0);
        if (lame_set_num_samples(lame, static_cast<unsigned long>(frames)) != 0 ||
            lame_set_num_channels(lame, static_cast<int>(channels)) != 0 ||
            lame_set_in_samplerate(lame, static_cast<int>(rate)) != 0 ||
            lame_set_out_samplerate(lame, static_cast<int>(rate)) != 0 ||
            lame_set_VBR(lame, vbr_off) != 0 || lame_set_brate(lame, bitrate) != 0 ||
            lame_set_mode(lame, channels == 1 ? MONO : JOINT_STEREO) != 0 ||
            lame_set_bWriteVbrTag(lame, 1) != 0 || lame_init_params(lame) < 0)
            throw std::runtime_error("Cannot set LAME CBR parameters");
    }

    void writePacket(int size) {
        if (size < 0 || static_cast<std::size_t>(size) > packet.size())
            throw std::runtime_error("Cannot encode staged MP3 frames");
        file.write(reinterpret_cast<const char*>(packet.data()), size);
        if (!file) throw std::runtime_error("Cannot write staged MP3");
    }
};

bool mp3ExportAvailable() noexcept { return true; }

Mp3Writer::Mp3Writer(const std::filesystem::path& path, unsigned rate, unsigned channels, std::uint64_t frames)
    : impl_(std::make_unique<Impl>(path, rate, channels, frames)) {}
Mp3Writer::~Mp3Writer() = default;

void Mp3Writer::write(std::span<const double> samples, std::uint32_t frames) {
    auto& s = *impl_;
    if (frames > maxInputFrames || samples.size() != static_cast<std::size_t>(frames) * s.channels ||
        frames > s.expected - s.written)
        throw std::invalid_argument("Unexpected MP3 capture block");
    const auto n = static_cast<int>(frames);
    const auto bytes = static_cast<int>(s.packet.size());
    if (s.channels == 1)
        s.writePacket(lame_encode_buffer_ieee_double(s.encoder.get(), samples.data(), samples.data(), n, s.packet.data(), bytes));
    else
        s.writePacket(lame_encode_buffer_interleaved_ieee_double(s.encoder.get(), samples.data(), n, s.packet.data(), bytes));
    s.written += frames;
}

void Mp3Writer::finish(const std::function<void()>& service, std::optional<std::uint64_t> frames) {
    auto& s = *impl_;
    if (frames && (*frames == 0 || *frames > s.expected || *frames != s.written))
        throw std::runtime_error("Encoded tail length disagrees with the rendered range");
    if (frames) s.expected = *frames;
    if (s.written != s.expected) throw std::runtime_error("Incomplete staged MP3 capture");
    if (service) service();
    s.writePacket(lame_encode_flush(s.encoder.get(), s.packet.data(), static_cast<int>(s.packet.size())));
    const auto count = lame_get_lametag_frame(s.encoder.get(), s.packet.data(), s.packet.size());
    if (count == 0 || count > s.packet.size()) throw std::runtime_error("Cannot write the MP3 Info tag");
    s.file.seekp(0);
    s.writePacket(static_cast<int>(count));
    s.file.close();
    if (!s.file) throw std::runtime_error("Cannot close staged MP3");
    verifyCbr(s.path, s.rate, s.channels, service);
    verifyTrim(s.path, s.rate, s.channels, s.expected, service);
}

#else

struct Mp3Writer::Impl {};
bool mp3ExportAvailable() noexcept { return false; }
Mp3Writer::Mp3Writer(const std::filesystem::path&, unsigned, unsigned, std::uint64_t) {
    throw std::runtime_error("MP3 export requires the pinned LAME build");
}
Mp3Writer::~Mp3Writer() = default;
void Mp3Writer::write(std::span<const double>, std::uint32_t) {
    throw std::runtime_error("MP3 export requires the pinned LAME build");
}
void Mp3Writer::finish(const std::function<void()>&, std::optional<std::uint64_t>) {
    throw std::runtime_error("MP3 export requires the pinned LAME build");
}

#endif
} // namespace adi::render
