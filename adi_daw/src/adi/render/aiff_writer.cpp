// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/aiff_writer.hpp"
#include "adi/audio/wav_file.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace adi::render {
namespace {
void bigEndian(std::ostream& out, std::uint64_t value, unsigned bytes) {
    char encoded[8]{};
    for (unsigned i = 0; i < bytes; ++i)
        encoded[i] = static_cast<char>((value >> (8 * (bytes - 1 - i))) & 0xffU);
    out.write(encoded, static_cast<std::streamsize>(bytes));
}

void header(std::ostream& out, const audio::WavReader& input, unsigned sampleBytes,
            bool floating, std::uint64_t soundBytes, std::uint64_t formBytes) {
    out.write("FORM", 4);
    bigEndian(out, formBytes, 4);
    if (floating) {
        out.write("AIFCFVER", 8);
        bigEndian(out, 4, 4);
        bigEndian(out, 0xa2805140, 4); // AIFF-C version 1
    } else {
        out.write("AIFF", 4);
    }
    out.write("COMM", 4);
    bigEndian(out, floating ? 24 : 18, 4);
    bigEndian(out, input.channels(), 2);
    bigEndian(out, input.frames(), 4);
    bigEndian(out, sampleBytes * 8, 2);
    // An ext80 exponent and explicit integer bit represent any integer rate exactly.
    const auto exponent = std::bit_width(input.sampleRate()) - 1U;
    bigEndian(out, 16383U + exponent, 2);
    bigEndian(out, static_cast<std::uint64_t>(input.sampleRate()) << (63U - exponent), 8);
    if (floating) {
        out.write("fl32", 4);
        bigEndian(out, 0, 2); // empty Pascal codec name, then alignment pad
    }
    out.write("SSND", 4);
    bigEndian(out, 8 + soundBytes, 4);
    bigEndian(out, 0, 8); // data offset and block alignment
}
}

void encodeStagedAiff(const std::filesystem::path& wave, const std::filesystem::path& aiff,
                      const std::function<void()>& service) {
    audio::WavReader input(wave);
    const bool floating = input.format() == audio::WavFormat::Float32;
    const unsigned sampleBytes = floating ? 4U : (input.format() == audio::WavFormat::Pcm24 ? 3U : 2U);
    const auto channels = input.channels();
    const auto frameBytes = static_cast<std::uint64_t>(sampleBytes) * channels;
    const std::uint64_t formOverhead = floating ? 64 : 46;
    const auto maxSoundBytes = std::numeric_limits<std::uint32_t>::max() - formOverhead;
    if (channels == 0 || channels > 32767 || input.sampleRate() == 0 || input.frames() == 0 ||
        input.frames() > maxSoundBytes / frameBytes)
        throw std::invalid_argument("AIFF output exceeds its channel, rate or FORM size limits");

    const auto soundBytes = input.frames() * frameBytes;
    const auto pad = soundBytes & 1U;
    const auto formBytes = formOverhead + soundBytes + pad;
    if (formBytes > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("AIFF output exceeds its FORM size limit");
    std::ofstream output(aiff, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot open staged AIFF");
    header(output, input, sampleBytes, floating, soundBytes, formBytes);

    constexpr std::uint32_t blockFrames = 1024;
    std::vector<float> samples(static_cast<std::size_t>(blockFrames) * channels);
    std::vector<char> encoded(samples.size() * sampleBytes);
    const auto magnitudeBits = static_cast<int>(sampleBytes * 8 - 1);
    for (std::uint64_t first = 0; first < input.frames();) {
        if (service) service();
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames, input.frames() - first));
        if (input.read(samples.data(), count) != count) throw std::runtime_error("Incomplete WAV stage for AIFF");
        const auto values = static_cast<std::size_t>(count) * channels;
        for (std::size_t i = 0; i < values; ++i) {
            const auto bits = floating ? std::bit_cast<std::uint32_t>(samples[i]) :
                static_cast<std::uint32_t>(static_cast<std::int32_t>(std::ldexp(samples[i], magnitudeBits)));
            for (unsigned b = 0; b < sampleBytes; ++b)
                encoded[i * sampleBytes + b] = static_cast<char>((bits >> (8 * (sampleBytes - 1 - b))) & 0xffU);
        }
        output.write(encoded.data(), static_cast<std::streamsize>(values * sampleBytes));
        if (!output) throw std::runtime_error("Cannot write staged AIFF");
        first += count;
    }
    if (pad) output.put(0);
    output.close();
    if (!output) throw std::runtime_error("Cannot close staged AIFF");
}

} // namespace adi::render
