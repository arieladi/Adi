// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/realize.hpp"
#include "adi/engine/transport.hpp"
#include "adi/store_rows.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>
namespace adi { class Store; }
namespace adi::audio { class ProgressiveDecoder; }
namespace adi::engine {
struct ClipProject;
struct NoteExpressionsRead;
struct ClipMediaSource {
    std::int64_t clipId = 0, mediaId = 0;
    std::filesystem::path path;
    std::string storedBlake3;
};
// Read the media hints and immutable rows off the callback. No Store survives.
std::shared_ptr<const ClipProject> readClipProject(const Store&, const rows::Model&);
const NoteExpressionsRead& clipNoteExpressions(const ClipProject&) noexcept;
class ClipPlayback {
public:
    ClipPlayback(std::shared_ptr<const ClipProject>, Transport&, double rate, std::int32_t channels,
                 std::int32_t maxFrames, audio::ProgressiveDecoder* = nullptr, bool synchronousReads = false);
    ~ClipPlayback();
    ClipPlayback(const ClipPlayback&) = delete;
    ClipPlayback& operator=(const ClipPlayback&) = delete;
    std::vector<Node*> sourcesFor(std::int64_t trackId);
    using SampleRange = std::pair<std::int64_t, std::int64_t>;
    /// Owner-side half-open Arrangement extents from the prepared clip clock.
    [[nodiscard]] std::vector<SampleRange> arrangementRanges(std::int64_t track) const;
    /// Owner only, before priming or publishing this source generation.
    void useMonitoredInput(std::int64_t track) noexcept;
    /// Owner only, before publishing. The span object outlives rendering of this
    /// generation. Its sorted IDs are set by the driver before rendering each slice.
    void muteWhileRecording(std::int64_t track, const std::span<const std::int64_t>& recordingTracks) noexcept;
    const std::vector<std::string>& problems() const noexcept;
    // This clip was dropped while preparing the generation or its reader failed.
    [[nodiscard]] bool clipUnavailable(std::int64_t clipId) const noexcept;
    // Whether this clip's audio file could not be opened or used. Clip settings
    // that the engine cannot play have separate diagnostics in problems().
    bool mediaUnavailable(std::int64_t clipId) const noexcept;
    // The owner polls terminal reader failures without blocking on preparation.
    // Reports whether this generation has new diagnostics to publish.
    bool pollDecodeProblems();
    void appendPlaybackProblems(std::vector<std::string>&) const;
    // A clip whose source file is absent already contributes silence.
    void appendExportBlockingProblems(std::vector<std::string>&) const;
    // Off-callback owner view of the external files successfully selected
    // by this generation, including Session's frozen-source substitutions.
    // Names the original input to decode/warp, not a second media resolver or
    // a promise about plugin-owned samples. Muted/unopened clips are absent.
    // Immutable for this generation's lifetime; callback use returns empty.
    [[nodiscard]] std::span<const ClipMediaSource> externalSources() const noexcept;
    // Offline/test driver support ONLY, between callbacks: request the next
    // block and wait for disk. Session::process NEVER calls this.
    bool prime(std::chrono::milliseconds timeout = std::chrono::seconds(10), std::stop_token stop = {});
    // Wait for preparation independently of the page-read timeout. Offline
    // renders use this before reading their first block of a cold Warp clip.
    bool waitForWarp(std::stop_token stop = {}) const;
    // Owner only, BEFORE publishing a candidate source generation: request and
    // wait for the pages selected by an immutable, owner-owned transport value.
    // Unlike prime(), this never reads the live driver Transport. Construct the
    // value from captured sample/loop state; copying a running Transport on the
    // owner is not safe. Do not process this candidate concurrently: its request
    // slots have one producer. False on callback use, read failure, or timeout;
    // the caller must retain its previous graph on failure. Live edits can
    // request pages without waiting for Warp jobs that are still preparing.
    bool primeAt(const Transport& snapshot,
                 std::chrono::milliseconds timeout = std::chrono::seconds(10),
                 bool waitForPendingWarp = true);
    // Prime two possible adoption positions under one request, e.g. the
    // current position and a retimed loop head. Refuses if their combined
    // page set cannot fit the existing bounded page slots.
    bool primeAt(const Transport& snapshot,const Transport& alternate,
                 std::chrono::milliseconds timeout = std::chrono::seconds(10),
                 bool waitForPendingWarp = true);
    struct Readiness {
        std::function<bool(const Transport&,std::int32_t,bool)> check;
        explicit operator bool() const noexcept { return static_cast<bool>(check); }
        bool operator()(const Transport& transport,std::int32_t frames,
                        bool allowFailedSources=false) const noexcept {
            return check && check(transport,frames,allowFailedSources);
        }
    };
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
    bool primeTargets(const Transport&,const Transport*,std::chrono::milliseconds,bool waitForPendingWarp,
                      std::stop_token stop = {});
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
