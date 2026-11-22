// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The plug-in scan through engine-win's catalogue API (N58; DF-W34). The loader runs each discovery job
// on a worker of its own; `PluginCatalogueService`, on the owner (the message thread), starts the jobs,
// reads their progress, adopts each one that has finished, and lists what the catalogue holds. It never
// scans and never waits. Live indexes plug-ins in the background (Live 12 manual 4.6) and rescans from
// the Plug-Ins page of its Settings (Rescan); so does ADI.
//
// The jobs, as engine-win's handoff names them: an Incremental discovery once the window is open (the
// catalogue stored by earlier runs is read first, with the project, by the loader's read-only
// hydration), a Refresh for Rescan, and an Incremental again when the Settings' plug-in folders change
// (which leaves a running job stale). One job at a time: a job asked for while one runs follows it, and
// a Refresh asked for during an Incremental stops that one first, since it covers it.
//
// Once the loader has run a discovery job it resolves a plug-in from the catalogue alone: one the
// catalogue lacks is "needs discovery", whether a job runs or not (it no longer probes a project's saved
// path). So a project opened while a job runs waits for that job's adoption (`settleThen`), as
// engine-win's handoff asks, and then finds what the job found; nothing is stopped for it. A project
// opened with no job running opens at once against the catalogue as it stands.
//
// No JUCE: the app's `DiscoveryPort` is the loader's job API (`JuceDeviceLoader::startDiscovery` and
// the rest, adapted in `ui_main.cpp`); a test's drives it by hand.
#pragma once
#include "adi/ui/plugin_listing.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace adi::ui {
// What the Browser shows of the scan: whether one is running, how many files it probes, and the list.
struct PluginScan {
    enum class Phase { NotStarted, Scanning, Done };
    Phase phase = Phase::NotStarted;
    std::size_t candidates = 0; // the files and bundles the running scan probes; 0 until counted
    std::size_t completed = 0;  // of those, the ones probed so far
    std::uint64_t listed = 0;   // counts each change of `listing`: a state with the same count lists the same
    PluginListing listing;      // what is listed: the last scan's, the previous list while one runs
};
// The Status Bar's line for the scan's state (DF-W34 a and b). A scan that stopped early is named with
// why, and never worded as "the rest are listed".
inline std::string pluginScanStatus(const PluginScan &scan) {
    const auto &l = scan.listing;
    if (scan.phase == PluginScan::Phase::NotStarted)
        return "The plug-ins have not been scanned yet.";
    if (scan.phase == PluginScan::Phase::Scanning)
        return scan.candidates == 0 ? std::string("Scanning plug-ins…")
               : scan.completed > 0
                   ? "Scanning plug-ins: " + std::to_string(std::min(scan.completed, scan.candidates)) + " of " +
                         std::to_string(scan.candidates) + (scan.candidates == 1 ? " file…" : " files…")
                   : "Scanning plug-ins: " + std::to_string(scan.candidates) + (scan.candidates == 1 ? " file…" : " files…");
    if (!l.complete)
        return "The plug-in scan stopped early: " + l.stopped + ". " +
               (l.plugins.empty() ? std::string("Nothing could be listed.") : std::string("The list may be incomplete.")) +
               " Rescan from the Settings' Plug-Ins page.";
    if (l.plugins.empty())
        return l.failed > 0 ? std::to_string(l.failed) + (l.failed == 1 ? " plug-in file" : " plug-in files") +
                                  " could not be scanned, and no plug-in was found."
                            : "No VST3 or CLAP plug-ins were found. Add their folders in the Settings' Plug-Ins page.";
    if (l.failed > 0)
        return std::to_string(l.failed) + (l.failed == 1 ? " plug-in file" : " plug-in files") +
               " could not be scanned; the rest are listed.";
    return {};
}
// The loader's job API as the service drives it, in its own words (`juce_device_loader.hpp`).
enum class DiscoveryMode { CacheOnly, Incremental, Refresh };
struct DiscoveryAdmission {
    std::uint64_t job = 0;   // 0: refused
    std::string refusal;     // why, a sentence's middle ("the plug-in scanner could not run (...)")
};
struct DiscoveryProgressRead {
    std::uint64_t job = 0;
    bool hasTotal = false;
    std::size_t completed = 0, total = 0;
};
struct DiscoveryEnd {
    enum class Outcome { Published, Missing, Legacy, Failed, Cancelled, Stale };
    Outcome outcome = Outcome::Failed;
    scan::Report report;
    std::string error;
};
struct DiscoveryPort {
    std::function<DiscoveryAdmission(DiscoveryMode)> start;
    std::function<DiscoveryProgressRead()> progress;
    std::function<std::optional<DiscoveryEnd>(std::uint64_t)> adopt; // nothing until the job has finished
    std::function<bool(std::uint64_t)> cancel;
    std::function<PluginListing()> listing; // what the catalogue holds now, as the Browser lists it
};

class PluginCatalogueService {
  public:
    explicit PluginCatalogueService(DiscoveryPort port) : port_(std::move(port)) {}
    PluginCatalogueService(const PluginCatalogueService &) = delete;
    PluginCatalogueService &operator=(const PluginCatalogueService &) = delete;
    ~PluginCatalogueService() { stop(); }

    // Called on the owner after each change of `state()`: a job begun, its progress, its end.
    std::function<void()> onChange;
    const PluginScan &state() const noexcept { return state_; }
    bool running() const noexcept { return job_ != 0; }
    std::uint64_t job() const noexcept { return job_; }

    // The catalogue's list as it stands, with no job: at startup, once the loader has read the stored
    // catalogue for the first project. Ignored while a job runs: its end lists afresh.
    void showCatalogue() {
        if (running() || !port_.listing)
            return;
        auto listing = port_.listing();
        listing.complete = state_.listing.complete;
        listing.stopped = state_.listing.stopped;
        listing.failed = state_.listing.failed;
        state_.listing = std::move(listing);
        ++state_.listed;
        if (state_.phase == PluginScan::Phase::NotStarted)
            state_.phase = PluginScan::Phase::Done;
        changed();
    }
    // Asks for a job: now when none runs, else after the running one. A Refresh asked for during an
    // Incremental or a CacheOnly stops that one first.
    void request(DiscoveryMode mode) {
        if (stopped_)
            return;
        queue(mode);
        if (!running()) {
            startQueued();
            return;
        }
        if (rank(mode) > rank(mode_))
            cancelRunning();
    }
    // Runs `open` once no job runs: now when none does; else after the running job's adoption, before
    // any job queued behind it starts, so the open finds what the job found.
    void settleThen(std::function<void()> open) {
        if (stopped_)
            return;
        if (!running()) {
            open();
            return;
        }
        settled_.push_back(std::move(open));
    }
    // Reads the running job's progress and adopts it once it has finished. The app's timer calls this.
    void poll() {
        if (!running())
            return;
        if (port_.progress) {
            const auto p = port_.progress();
            if (p.job == job_) {
                const auto total = p.hasTotal ? p.total : 0;
                if (total != state_.candidates || p.completed != state_.completed) {
                    state_.candidates = total;
                    state_.completed = p.completed;
                    changed();
                }
            }
        }
        if (auto end = port_.adopt(job_))
            finish(*end);
    }
    // Quit: the running job is cancelled and nothing more starts; the loader's own teardown ends its
    // worker. Opens waiting for the job are dropped with it.
    void stop() {
        stopped_ = true;
        if (running() && port_.cancel)
            port_.cancel(job_);
        job_ = 0;
        pending_.reset();
        settled_.clear();
    }

  private:
    static int rank(DiscoveryMode m) noexcept { return m == DiscoveryMode::Refresh ? 2 : m == DiscoveryMode::Incremental ? 1 : 0; }
    void queue(DiscoveryMode mode) {
        if (!pending_ || rank(mode) > rank(*pending_))
            pending_ = mode;
    }
    // Stops the running job for one that covers it. A job that has already finished refuses the stop:
    // it is adopted as it ended, and the job queued behind it follows all the same.
    void cancelRunning() {
        if (!running() || quietCancel_)
            return;
        if (port_.cancel && port_.cancel(job_))
            quietCancel_ = true;
    }
    void startQueued() {
        if (stopped_ || running() || !pending_)
            return;
        const auto mode = *pending_;
        pending_.reset();
        const auto admission = port_.start(mode);
        if (admission.job == 0) {
            // Nothing runs; the list stays, named as stopped, and no job is tried again until something
            // asks anew (Rescan, a folder change).
            pending_.reset();
            state_.phase = PluginScan::Phase::Done;
            state_.candidates = state_.completed = 0;
            state_.listing.complete = false;
            state_.listing.failed = 0;
            state_.listing.stopped =
                admission.refusal.empty() ? std::string("the plug-in scan could not start") : admission.refusal;
            ++state_.listed;
            changed();
            return;
        }
        job_ = admission.job;
        mode_ = mode;
        quietCancel_ = false;
        state_.phase = PluginScan::Phase::Scanning;
        state_.candidates = state_.completed = 0;
        changed();
    }
    void finish(const DiscoveryEnd &end) {
        job_ = 0;
        const bool quiet = quietCancel_;
        quietCancel_ = false;
        using O = DiscoveryEnd::Outcome;
        auto listing = port_.listing ? port_.listing() : state_.listing;
        switch (end.outcome) {
        case O::Published:
        case O::Missing: // nothing stored yet: what the catalogue holds, with no failure to name
        case O::Legacy:
            applyReport(listing, end.report);
            state_.listing = std::move(listing);
            ++state_.listed;
            break;
        case O::Stale:
            // The Settings' folders changed under it: the catalogue stays, and the job runs again on them.
            queue(mode_);
            break;
        case O::Cancelled:
            if (quiet)
                break; // stopped for a Refresh that covers it: not the person's stop, nothing to name
            [[fallthrough]];
        case O::Failed:
            // What was accepted before stays listed; why this job stopped is named.
            applyReport(listing, end.report);
            listing.complete = false;
            listing.failed = 0;
            if (listing.stopped.empty() || listing.stopped == "the scan did not finish")
                listing.stopped = end.outcome == O::Cancelled ? std::string("the scan was stopped before it finished")
                                  : end.error.empty()           ? std::string("the scan did not finish")
                                                                : "the scan failed (" + end.error + ")";
            state_.listing = std::move(listing);
            ++state_.listed;
            break;
        }
        state_.candidates = state_.completed = 0;
        // Between jobs: the opens that waited run now, with no job beside them, before the next job
        // starts. A stale job's opens wait for its run on the new folders instead, and those of a job
        // stopped for a Refresh wait for that Refresh: the stopped job published nothing.
        if (end.outcome != O::Stale && !(quiet && end.outcome == O::Cancelled))
            runOpens();
        if (pending_ && !stopped_) {
            startQueued();
            if (running())
                return;
        }
        if (state_.phase != PluginScan::Phase::Done) {
            state_.phase = PluginScan::Phase::Done;
            changed();
        }
        runOpens(); // a stale job that could not run again: its opens go ahead on what is listed
    }
    void runOpens() {
        auto opens = std::move(settled_);
        settled_.clear();
        for (auto &open : opens)
            if (!stopped_)
                open();
    }
    void changed() {
        if (onChange)
            onChange();
    }

    DiscoveryPort port_;
    std::uint64_t job_ = 0;
    DiscoveryMode mode_ = DiscoveryMode::Incremental;
    std::optional<DiscoveryMode> pending_;
    bool quietCancel_ = false;
    bool stopped_ = false;
    std::vector<std::function<void()>> settled_;
    PluginScan state_;
};
} // namespace adi::ui
