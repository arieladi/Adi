// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace adi::render {

[[nodiscard]] bool mp3ExportAvailable() noexcept;

// Owner-worker stream, fed undithered interleaved doubles. The caller owns
// the stage and publishes only after the complete MP3 verifies.
class Mp3Writer {
public:
    Mp3Writer(const std::filesystem::path&, unsigned rate, unsigned channels, std::uint64_t frames);
    ~Mp3Writer();
    Mp3Writer(const Mp3Writer&) = delete;
    Mp3Writer& operator=(const Mp3Writer&) = delete;

    void write(std::span<const double> samples, std::uint32_t frames);
    void finish(const std::function<void()>& service, std::optional<std::uint64_t> frames = std::nullopt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adi::render
