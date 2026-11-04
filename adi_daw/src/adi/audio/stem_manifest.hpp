// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
namespace adi::audio {
struct StemLabel {
    std::string name, color;
};
struct StemManifest {
    std::vector<StemLabel> stems;
    bool masteringEnabled = false;
};
[[nodiscard]] bool stemColorValid(const std::string &) noexcept;
[[nodiscard]] StemManifest parseStemManifest(const std::string &original);
[[nodiscard]] std::string makeStemManifest(const std::vector<StemLabel> &);
} // namespace adi::audio
