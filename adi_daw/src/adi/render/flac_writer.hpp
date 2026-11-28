// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>

namespace adi::render {

[[nodiscard]] bool flacExportAvailable() noexcept;

// Repack PCM16/24 from an owned WAV stage. Those samples already carry the
// job's dither and quantization, so FLAC receives their exact integer codes.
void encodeStagedFlac(const std::filesystem::path& wave, const std::filesystem::path& flac,
                      const std::function<void()>& service = {});

} // namespace adi::render
