// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/io_audit.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <stop_token>
namespace adi::render::detail {
// Closed files inside an exclusively owned private stage. Public destinations
// always go through media::publishFile. Never replaces an existing stage file.
inline void copyPrivate(const std::filesystem::path &source, const std::filesystem::path &target,
                        std::stop_token stop) {
    if (audio::onAudioThread || stop.stop_requested())
        throw std::runtime_error("private copy cancelled or on audio thread");
    audio::fileIoPoint();
    if (std::filesystem::exists(target))
        throw std::runtime_error("private copy destination already exists");
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(target, std::ios::binary);
    if (!input || !output)
        throw std::runtime_error("cannot open private copy");
    std::array<char, 65536> buffer{};
    while (input) {
        if (stop.stop_requested())
            throw std::runtime_error("private copy cancelled");
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        output.write(buffer.data(), input.gcount());
        if (!output)
            throw std::runtime_error("cannot write private copy");
    }
    if (!input.eof())
        throw std::runtime_error("cannot read private copy");
    output.close();
    if (!output)
        throw std::runtime_error("cannot close private copy");
    if (stop.stop_requested())
        throw std::runtime_error("private copy cancelled");
}
} // namespace adi::render::detail
