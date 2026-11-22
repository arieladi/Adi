// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace adi::render {

[[nodiscard]] bool deliverySrcAvailable() noexcept;

// Streams the selected double-precision render to the file's sample rate.
// The job supplies enough source frames to cover the output and owns all
// writes, cancellation and publication.
class DeliverySrc {
public:
    using Writer = std::function<void(std::span<const double>, std::uint32_t)>;
    DeliverySrc(unsigned sourceRate, unsigned outputRate, unsigned channels,
                std::uint64_t requestedFrames, Writer);
    ~DeliverySrc();
    DeliverySrc(const DeliverySrc&) = delete;
    DeliverySrc& operator=(const DeliverySrc&) = delete;

    void append(std::span<const double> source, std::uint32_t frames);
    void finish(const std::function<void()>& checkpoint, std::uint64_t frames = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adi::render
