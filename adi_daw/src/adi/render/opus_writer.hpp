// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace adi::render {

[[nodiscard]] bool opusExportAvailable() noexcept;
[[nodiscard]] bool validOpusBitRate(unsigned channels, std::uint32_t bitRate) noexcept;

// Mono/stereo family 0, at the container clock of 48 kHz.
class OpusWriter {
public:
    OpusWriter(const std::filesystem::path&, unsigned channels, std::uint64_t frames,
               std::uint32_t bitRate);
    ~OpusWriter();
    OpusWriter(const OpusWriter&) = delete;
    OpusWriter& operator=(const OpusWriter&) = delete;
    void write(std::span<const double>, std::uint32_t frames);
    void finish(const std::function<void()>& service, std::optional<std::uint64_t> frames = std::nullopt);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adi::render
