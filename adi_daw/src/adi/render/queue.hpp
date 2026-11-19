// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/render/normalization.hpp"
#include "adi/media/publish_file.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace adi::render {
using JobId = std::uint64_t;
enum class JobState { Pending, Running, Completed, Failed, Cancelled };

// ADR-0071/0205: application job state, never project operations. This first
// runner exports direct wet sources as WAV, or NI Stems through niStems. Noise
// shaping, dry processing and isolated through-master stems need support. Normalization
// captures the selected source once and verifies the final encoded WAV.
struct Source {
    std::int64_t trackId = 0; // zero selects the project's master at render start
    engine::OfflineTap tap = engine::OfflineTap::PostFader;
};
struct Encoding {
    audio::WavFormat format = audio::WavFormat::Pcm24;
    audio::WavDither dither = audio::WavDither::Triangular;
    // Mixed with the stable enqueue ordinal. Retry is reproducible; different
    // jobs get separate streams. Float32 ignores dither without adding noise.
    std::uint64_t ditherSeed = 0;
};
struct NiStem {
    Source source;
    std::string name;
    std::string color; // optional #RRGGBB, NI metadata, never a Live palette index
};
struct NiStems {
    std::vector<NiStem> stems; // one to four direct wet sources; unused slots silent
    std::uint32_t bitRate = 256000; // per stereo track, one codec setting for all five
};
struct Job {
    std::filesystem::path project;
    std::filesystem::path directory;
    std::string nameTemplate = "{project} - {track}"; // extension added by the runner
    std::int64_t beginSample = 0;
    std::int64_t endSample = 0; // exclusive, at session.sampleRate
    engine::SessionSpec session;
    Source source;
    // A single cycle marker overrides begin/end, resolved when the job starts.
    // Independent jobs are not ADR-0070's continuous gapless region batch.
    std::optional<std::int64_t> regionId;
    Encoding encoding;
    std::optional<NiStems> niStems = {};
    Normalization normalization{};
};
struct PublicationReport {
    bool durabilityConfirmed = false;
    bool usedFallback = false;
    std::string warning;
};
struct NameContext {
    std::string project, track, group, bpm;
    std::uint64_t counter = 1;
    std::string region = "";
};
struct RenderSnapshot {
    // Zero represents the captured SQL NULL head before any journal operation.
    // An absent Result::snapshot, not zero, means the snapshot is unavailable.
    std::int64_t journalHead = 0;
    Source source; // resolved track ID (including the actual Main ID), and tap
    std::int64_t beginSample = 0, endSample = 0; // resolved output range, exclusive end
    double sampleRate = 0;
    std::optional<std::int64_t> regionId{};
    NameContext names; // the values used to resolve this output's name
};
struct Result {
    JobState state = JobState::Pending;
    std::filesystem::path output;
    std::uint64_t framesWritten = 0;
    std::string error;
    std::optional<NormalizationReport> normalization{};
    // Absent means no successful publication. A warning never changes Completed
    // to Failed; durability is distinct from verified publication.
    std::optional<PublicationReport> publication{};
    // Captured inside the same read transaction as the graph and output names.
    // May exist on a later failed/cancelled attempt; it does not mean published.
    std::optional<RenderSnapshot> snapshot{};
    // NI's actual supplied stems, in stream order; unused silent slots have no
    // project source. Main is snapshot above. All share its revision and range.
    std::vector<RenderSnapshot> stemSnapshots{};
};
struct Entry { JobId id = 0; Job job; Result result; };
struct RunMember {
    JobId id = 0;
    bool started = false;
    bool removedBeforeStart = false;
    JobState state = JobState::Pending;
    std::optional<Job> job{};
    bool jobCapturedAtStart = false;
    std::optional<Result> result{};
};
struct RunReport {
    std::size_t plannedCount = 0;
    // Best-effort copies cannot fail a render or interrupt queue controls.
    // False means some report fields/members could not be copied.
    bool captureComplete = true;
    std::vector<RunMember> members;
};
// Caller-owned serialization/persistence; no sidecar is written by the queue.
// May throw (allocation, invalid UTF-8). Never changes queue or output state.
// Version 1 captures requested values plus available resolved revision/results;
// media identities/hashes are not captured yet.
[[nodiscard]] std::string serializeRunManifest(const RunReport&);
// Strict brace expansion, {{ / }} for literals; returns one portable filename
// component without an extension. Unknown/malformed tokens and empty names throw.
[[nodiscard]] std::string resolveName(const std::string&, const NameContext&);
// Check the 200 UTF-8 byte budget on the final filename component, after
// resolveName's escaping and the actual output format's complete suffix.
// Does not rename, truncate or otherwise change the resolved component.
void validateOutputName(const std::filesystem::path&);
struct Control {
    std::stop_token stop;
    // Work in sample frames; NI Stems spans five encoded streams.
    std::function<void(JobId, std::uint64_t written, std::uint64_t total)> progress;
    // Called between blocks, on the same thread as the loader and the runner.
    // Hosts with a message-thread contract must be run on that thread (or in
    // a dedicated render process). This does not negotiate plugin offline mode.
    std::function<void()> serviceHost;
    // Per-call, off-callback diagnostic hooks; only the final user output.
    media::PublishHooks publicationHooks{};
};
[[nodiscard]] Result renderMasterWav(Job, engine::DeviceLoader,
                                    Control = {}, JobId = 0,
                                    std::uint64_t counter = 1);
[[nodiscard]] Result renderWav(Job, engine::DeviceLoader,
                              Control = {}, JobId = 0,
                              std::uint64_t counter = 1);

[[nodiscard]] Result renderNiStems(Job, engine::DeviceLoader, Control = {},
                                    JobId = 0, std::uint64_t counter = 1);
void validateNiStems(const Job&);
namespace detail {
// Shared WAV/NI commit point. Allocate warning storage before publication;
// nothing after successful publication can throw and turn it into Failed.
void publishOutput(const std::filesystem::path& stage, Result&, media::PublishHooks,
                   const char* collisionMessage);
}

// Single-owner synchronous queue. The caller chooses its execution thread;
// snapshots are independent values for posting to a UI. No callback runs on an
// audio driver. A running job is immutable; callbacks may edit pending jobs.
class Queue {
public:
    JobId add(Job);
    bool update(JobId, Job);
    bool remove(JobId);
    bool retry(JobId); // only Failed/Cancelled, never silently rerender completed files
    [[nodiscard]] std::vector<Entry> snapshot() const { return jobs_; }
    // Valid until the next run/destruction; copy it for longer retention.
    // Unstarted run members remain Pending in the queue (not Cancelled).
    [[nodiscard]] const RunReport& lastRun() const noexcept { return lastRun_; }
    void run(engine::DeviceLoader = {}, Control = {});
private:
    std::vector<Entry> jobs_;
    JobId next_ = 1;
    bool running_ = false;
    RunReport lastRun_;
};
} // namespace adi::render
