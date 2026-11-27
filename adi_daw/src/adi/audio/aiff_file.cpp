// SPDX-License-Identifier: GPL-3.0-or-later
#include "aiff_file.hpp"

#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace adi::audio {
namespace {
unsigned sampleBytes(WavFormat format) {
    switch (format) {
    case WavFormat::Pcm16: return 2;
    case WavFormat::Pcm24: return 3;
    case WavFormat::Float32: return 4;
    }
    throw std::invalid_argument("Unsupported AIFF sample format.");
}

constexpr std::string_view floatName = "32-bit floating point";
constexpr std::uint32_t floatNameBytes = (1 + floatName.size() + 1) & ~std::uint32_t{1};

std::uint32_t commonSize(WavFormat format) {
    return 18 + (format == WavFormat::Float32 ? 4 + floatNameBytes : 0);
}

std::uint32_t formOverhead(WavFormat format) {
    // Includes the form type, optional FVER, COMM, and the SSND header and its fields.
    return 4 + (format == WavFormat::Float32 ? 12 : 0) + 8 + commonSize(format) + 16;
}

std::uint32_t payloadSize(std::uint64_t frames, std::uint16_t channels, WavFormat format) {
    const auto frameBytes = channels * sampleBytes(format);
    const auto maxPayload = std::numeric_limits<std::uint32_t>::max() - formOverhead(format) - 1;
    if (frameBytes == 0 || frames > maxPayload / frameBytes)
        throw std::invalid_argument("The AIFF file would exceed its size limit. Choose WAV or a shorter range.");
    return static_cast<std::uint32_t>(frames * frameBytes);
}

void writeBigEndian(std::ostream& out, std::uint64_t value, unsigned bytes) {
    std::array<unsigned char, 8> buffer{};
    for (unsigned i = 0; i < bytes; ++i)
        buffer[i] = static_cast<unsigned char>(value >> (8 * (bytes - 1 - i)));
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(bytes));
}

void writeHeader(std::ostream& out, const WavReader& input, std::uint32_t dataBytes) {
    const auto format = input.format();
    const bool floating = format == WavFormat::Float32;
    out.write("FORM", 4);
    writeBigEndian(out, formOverhead(format) + dataBytes + (dataBytes & 1), 4);
    out.write(floating ? "AIFC" : "AIFF", 4);
    if (floating) {
        out.write("FVER", 4);
        writeBigEndian(out, 4, 4);
        writeBigEndian(out, 0xa2805140, 4);
    }
    out.write("COMM", 4);
    writeBigEndian(out, commonSize(format), 4);
    writeBigEndian(out, input.channels(), 2);
    writeBigEndian(out, input.frames(), 4);
    writeBigEndian(out, 8 * sampleBytes(format), 2);

    // IEEE extended precision, with an explicit integer bit. An integer sample
    // rate has an exact representation without computing with host floats.
    const auto width = std::bit_width(input.sampleRate());
    writeBigEndian(out, static_cast<std::uint64_t>(16382 + width), 2);
    writeBigEndian(out, std::uint64_t{input.sampleRate()} << (64 - width), 8);
    if (floating) {
        out.write("fl32", 4);
        out.put(static_cast<char>(floatName.size()));
        out.write(floatName.data(), static_cast<std::streamsize>(floatName.size()));
        if ((floatName.size() & 1) == 0)
            out.put('\0');
    }
    out.write("SSND", 4);
    writeBigEndian(out, 8 + dataBytes, 4);
    writeBigEndian(out, 0, 8); // offset and block size
}
} // namespace

void validateAiffSize(std::uint64_t frames, std::uint16_t channels, WavFormat format) {
    (void)payloadSize(frames, channels, format);
}

void writeAiffFromWav(const std::filesystem::path& input, const std::filesystem::path& output,
                      const std::function<void()>& poll) {
    WavReader wave(input);
    const auto dataBytes = payloadSize(wave.frames(), wave.channels(), wave.format());
    std::ofstream out(output, std::ios::binary);
    if (!out)
        throw std::runtime_error("Could not create the AIFF file.");
    writeHeader(out, wave, dataBytes);
    constexpr std::uint32_t block = 4096;
    const auto bytes = sampleBytes(wave.format());
    const bool floating = wave.format() == WavFormat::Float32;
    const double scale = static_cast<double>(std::uint32_t{1} << (8 * bytes - 1));
    std::vector<float> samples(static_cast<std::size_t>(block) * wave.channels());
    std::vector<unsigned char> encoded(samples.size() * bytes);
    for (std::uint64_t done = 0; done < wave.frames();) {
        poll();
        const auto frames = wave.read(samples.data(), block);
        if (frames == 0)
            throw std::runtime_error("The staged audio ended before the AIFF file was complete.");
        const auto count = static_cast<std::size_t>(frames) * wave.channels();
        for (std::size_t i = 0; i < count; ++i) {
            // PCM16 and PCM24 are exact binary fractions in the WAV reader's floats.
            const auto bits = floating ? std::bit_cast<std::uint32_t>(samples[i])
                                       : static_cast<std::uint32_t>(static_cast<std::int32_t>(samples[i] * scale));
            for (unsigned byte = 0; byte < bytes; ++byte)
                encoded[i * bytes + byte] = static_cast<unsigned char>(bits >> (8 * (bytes - 1 - byte)));
        }
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(count * bytes));
        if (!out)
            throw std::runtime_error("Could not write the AIFF file.");
        done += frames;
    }
    if ((dataBytes & 1) != 0)
        out.put('\0');
    out.close();
    if (!out)
        throw std::runtime_error("Could not finish writing the AIFF file.");
}

} // namespace adi::audio
