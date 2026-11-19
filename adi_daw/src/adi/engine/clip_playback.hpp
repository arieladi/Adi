// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/realize.hpp"
#include "adi/engine/transport.hpp"
#include "adi/store_rows.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>
namespace adi { class Store; }
namespace adi::audio { class ProgressiveDecoder; }
namespace adi::engine {
struct ClipProject;
// Read the media hints and immutable rows off the callback. No Store survives.
std::shared_ptr<const ClipProject> readClipProject(const Store&, const rows::Model&);
class ClipPlayback {
public:
    ClipPlayback(std::shared_ptr<const ClipProject>, Transport&, double rate, std::int32_t channels,
                 std::int32_t maxFrames, audio::ProgressiveDecoder* = nullptr);
    ~ClipPlayback();
    ClipPlayback(const ClipPlayback&) = delete;
    ClipPlayback& operator=(const ClipPlayback&) = delete;
    std::vector<Node*> sourcesFor(std::int64_t trackId);
    const std::vector<std::string>& problems() const noexcept;
    // Offline/test driver support ONLY, between callbacks: request the next
    // block and wait for disk. Session::process NEVER calls this.
    bool prime(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    // Owner only, BEFORE publishing a candidate source generation: request and
    // wait for the pages selected by an immutable, owner-owned transport value.
    // Unlike prime(), this never reads the live driver Transport. Construct the
    // value from captured sample/loop state; copying a running Transport on the
    // owner is not safe. Do not process this candidate concurrently: its request
    // slots have one producer. False on callback use, read failure, or timeout;
    // the caller must retain its previous graph on failure.
    bool primeAt(const Transport& snapshot,
                 std::chrono::milliseconds timeout = std::chrono::seconds(10));
    // Prime two possible adoption positions under one request, e.g. the
    // current position and a retimed loop head. Refuses if their combined
    // page set cannot fit the existing bounded page slots.
    bool primeAt(const Transport& snapshot,const Transport& alternate,
                 std::chrono::milliseconds timeout = std::chrono::seconds(10));
    using Readiness = std::function<bool(const Transport&,std::int32_t)>;
    // Owner-created query retained with the graph's source generation. Calling
    // it on the single audio driver requests read-ahead and checks only the
    // pages needed for the given frames, without allocation, waits or I/O.
    // A true result retains actual page reservations until rendering or a
    // superseding request. Partial/refused admission releases its reservations;
    // call immediately before rendering the same transport range.
    // The closure retains its source generation even after this wrapper dies.
    // Construction on the audio callback refuses with an empty function.
    [[nodiscard]] Readiness readiness() const;
    // Simulates an unavailable disk worker, without stopping the audio driver.
    void stallForTest(bool stall) noexcept;
    struct RequestSnapshotProbeForTest {
        void* context = nullptr;
        void (*afterSnapshot)(void*,std::int64_t firstPage) noexcept = nullptr;
        void (*afterService)(void*) noexcept = nullptr;
        void (*beforePageClaim)(void*,std::int64_t oldPage,std::int64_t requestedPage) noexcept = nullptr;
        void (*afterPageClaimAttempt)(void*,std::int64_t oldPage,std::int64_t requestedPage,bool claimed) noexcept = nullptr;
    };
    // Deterministic worker-race tests only. The immutable probe and its context
    // must outlive this source generation's worker (including readiness holds).
    // All hooks run on the disk worker, never the audio callback.
    void setRequestSnapshotProbeForTest(const RequestSnapshotProbeForTest*) noexcept;
    std::uint32_t underrunSamples() const noexcept;
    std::uint32_t readErrors() const noexcept;
    // Off-callback owner snapshot of terminal asynchronous decode diagnostics.
    // Initial construction problems remain in problems().
    std::vector<std::string> decodeProblems() const;
private:
    bool primeTargets(const Transport&,const Transport*,std::chrono::milliseconds);
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
