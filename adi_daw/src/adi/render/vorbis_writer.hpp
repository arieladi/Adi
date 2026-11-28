// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace adi::render {

[[nodiscard]] bool vorbisExportAvailable() noexcept;

class VorbisWriter {
public:
    VorbisWriter(const std::filesystem::path&, unsigned rate, unsigned channels,
                 std::uint64_t frames, float quality);
    ~VorbisWriter();
    VorbisWriter(const VorbisWriter&) = delete;
    VorbisWriter& operator=(const VorbisWriter&) = delete;

    void write(std::span<const double>, std::uint32_t frames);
    void finish(const std::function<void()>& service, std::optional<std::uint64_t> frames = std::nullopt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adi::render
