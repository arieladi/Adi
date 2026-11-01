// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
#include "adi/audio/wav_file.hpp"
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
// runner exports direct wet sources as WAV. Other containers, noise shaping,
// normalization, dry processing and isolated through-master stems need support.
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
};
struct Result {
    JobState state = JobState::Pending;
    std::filesystem::path output;
    std::uint64_t framesWritten = 0;
    std::string error;
};
struct Entry { JobId id = 0; Job job; Result result; };
struct NameContext {
    std::string project, track, group, bpm;
    std::uint64_t counter = 1;
    std::string region = "";
};
// Strict brace expansion, {{ / }} for literals; returns one portable filename
// component without an extension. Unknown/malformed tokens and empty names throw.
[[nodiscard]] std::string resolveName(const std::string&, const NameContext&);
struct Control {
    std::stop_token stop;
    std::function<void(JobId, std::uint64_t written, std::uint64_t total)> progress;
    // Called between blocks, on the same thread as the loader and the runner.
    // Hosts with a message-thread contract must be run on that thread (or in
    // a dedicated render process). This does not negotiate plugin offline mode.
    std::function<void()> serviceHost;
};
[[nodiscard]] Result renderMasterWav(Job, engine::DeviceLoader,
                                    Control = {}, JobId = 0,
                                    std::uint64_t counter = 1);
[[nodiscard]] Result renderWav(Job, engine::DeviceLoader,
                              Control = {}, JobId = 0,
                              std::uint64_t counter = 1);

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
    void run(engine::DeviceLoader = {}, Control = {});
private:
    std::vector<Entry> jobs_;
    JobId next_ = 1;
    bool running_ = false;
};
} // namespace adi::render
