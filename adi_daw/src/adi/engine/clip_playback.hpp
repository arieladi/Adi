// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/realize.hpp"
#include "adi/engine/transport.hpp"
#include "adi/store_rows.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>
namespace adi { class Store; }
namespace adi::engine {
struct ClipProject;
// Read the media hints and immutable rows off the callback. No Store survives.
std::shared_ptr<const ClipProject> readClipProject(const Store&, const rows::Model&);
class ClipPlayback {
public:
    ClipPlayback(std::shared_ptr<const ClipProject>, Transport&, double rate, std::int32_t channels,
                 std::int32_t maxFrames);
    ~ClipPlayback();
    ClipPlayback(const ClipPlayback&) = delete;
    ClipPlayback& operator=(const ClipPlayback&) = delete;
    std::vector<Node*> sourcesFor(std::int64_t trackId);
    const std::vector<std::string>& problems() const noexcept;
    // Offline/test driver support ONLY, between callbacks: request the next
    // block and wait for disk. Session::process NEVER calls this.
    bool prime(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    // Simulates an unavailable disk worker, without stopping the audio driver.
    void stallForTest(bool stall) noexcept;
    std::uint32_t underrunSamples() const noexcept;
    std::uint32_t readErrors() const noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
