// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/audio/wav_file.hpp"
#include <functional>

namespace adi::audio {

// AIFF and AIFF-C have 32-bit chunk sizes. Check before starting a long render.
void validateAiffSize(std::uint64_t frames, std::uint16_t channels, WavFormat format);

// Write a private delivery from the finalized WAV, changing container and byte
// order without applying gain, clipping or a second dither. poll may throw to cancel.
void writeAiffFromWav(const std::filesystem::path& input, const std::filesystem::path& output,
                      const std::function<void()>& poll);

} // namespace adi::audio
