// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace adi::render {
using JobId = std::uint64_t;
enum class JobState { Pending, Running, Completed, Failed, Cancelled };

// ADR-0071/0205: application job state, never project operations. This first
// runner exports the heard master as Float32 WAV. Other encodings, dither,
// normalization and stem sources require their own explicit runner support.
struct Job {
    std::filesystem::path project;
    std::filesystem::path directory;
    std::string nameTemplate = "{project} - {track}"; // extension added by the runner
    std::int64_t beginSample = 0;
    std::int64_t endSample = 0; // exclusive, at session.sampleRate
    engine::SessionSpec session;
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
