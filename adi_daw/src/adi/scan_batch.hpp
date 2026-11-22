// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/audio/io_audit.hpp"
#include "adi/plugin_scanner.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <type_traits>
#include <utility>

namespace adi::scan::detail {

// Run at most one probe per connection at any time. Admission and the child
// run on that worker, while the owner visits completed results in input order.
// At most one validated result per worker waits for the owner.
template<class Probe, class Visit>
bool visitProbes(std::size_t count, const Config& config, Probe probe, Visit visit, std::string& error,
                 State* openFailure = nullptr) {
    if (audio::onAudioThread) return false;
    error.clear();
    if (config.workers == 0 || config.workers > kMaximumProbeWorkers) {
        error = "scanner worker count must be 1 to 4";
        return false;
    }
    const auto progress = config.progress;
    if (progress) {
        progress->completed.store(0, std::memory_order_relaxed);
        progress->total.store(count, std::memory_order_relaxed);
        progress->enumerated.store(true, std::memory_order_release);
    }
    if (count == 0) return true;
    const auto deliver = [&](std::size_t index, auto&& row) {
        const bool accepted = visit(index, std::forward<decltype(row)>(row));
        if (progress) progress->completed.store(index + 1, std::memory_order_relaxed);
        return accepted;
    };
    try {
        const auto active = std::min(count, static_cast<std::size_t>(config.workers));
        auto first = Scanner::open(config, error, openFailure);
        if (!first) return false;
        if (active == 1) {
            for (std::size_t i = 0; i < count; ++i)
                if (!deliver(i, probe(*first, i))) return false;
            return true;
        }
        std::array<std::unique_ptr<Scanner>, kMaximumProbeWorkers> scanners;
        scanners[0] = std::move(first);
        for (std::size_t slot = 1; slot < active; ++slot) {
            scanners[slot] = scanners[0]->openPeer(error);
            if (!scanners[slot]) return false;
        }
        using Row = std::invoke_result_t<Probe&, Scanner&, std::size_t>;
        // Future destruction waits for the bounded in-flight work before its
        // scanner or caller-owned cancellation flag can go out of scope.
        std::array<std::future<Row>, kMaximumProbeWorkers> pending;
        const auto launch = [&](std::size_t index) {
            const auto slot = index % active;
            pending[slot] = std::async(std::launch::async, [&, slot, index] {
                return probe(*scanners[slot], index);
            });
        };
        for (std::size_t slot = 0; slot < active; ++slot) launch(slot);
        for (std::size_t i = 0; i < count; ++i) {
            if (!deliver(i, pending[i % active].get())) return false;
            if (count - i > active) launch(i + active);
        }
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

} // namespace adi::scan::detail
