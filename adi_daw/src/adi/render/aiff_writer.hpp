// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>

namespace adi::render {

// Preserve the PCM codes (or Float32 bits) already produced by the export
// quantizer. The caller owns both paths and publishes only after success.
void encodeStagedAiff(const std::filesystem::path& wave, const std::filesystem::path& aiff,
                      const std::function<void()>& service);

} // namespace adi::render
