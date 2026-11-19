// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The plug-in scan off the message thread (DF-W34): one worker at a time scans with the app's one loader
// while the owner (the message thread) waits for nothing. Live indexes plug-ins in the background
// (Live 12 manual 4.6) and rescans from the Plug-Ins page of its Settings (Rescan); so does ADI.
//
// The scan itself is handed in (`Scan`): the app's scans the loader's VST3 and CLAP folders through the
// dedicated helper and reads back what the loader holds; a test hands in one it drives. The worker posts
// its end to the owner (`Post`, the app's `MessageManager::callAsync`), where the result is adopted.
//
// While a scan runs the worker owns the loader. Anything else that needs the loader on the owner thread
// (a project open, an Undo that brings a plug-in back) holds the service for the whole use (`Hold`): the
// hold waits for the scan to end and adopts its result there and then, and no scan begins until it is
// released, so the loader never runs two scans at once and never scans on two threads (a second scanner would meet the registry's refusal of a pending key and
// quarantine good plug-ins). Quit cancels the scan through the helper's cancel flag and waits for it.
//
// Until engine-win's catalogue API (N58) is on main this is the window's interim: N58 brings per-file
// progress, a VST3's kind, a startup cache and an adopt step, and this service then wraps that instead.
#pragma once
#include "adi/ui/plugin_listing.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
namespace adi::ui {
// What the Browser shows of the scan: whether one is running, how many files it probes, and the list.
struct PluginScan {
    enum class Phase { NotStarted, Scanning, Done };
    Phase phase = Phase::NotStarted;
    std::size_t candidates = 0; // the files and bundles the running scan probes; 0 until counted
    PluginListing listing;      // what is listed: the last scan's, the previous list while one runs
};
// The Status Bar's line for the scan's state (DF-W34 a and b). A scan that stopped early is named with
// why, and never worded as "the rest are listed".
inline std::string pluginScanStatus(const PluginScan &scan) {
    const auto &l = scan.listing;
    if (scan.phase == PluginScan::Phase::NotStarted)
        return "The plug-ins have not been scanned yet.";
    if (scan.phase == PluginScan::Phase::Scanning)
        return scan.candidates > 0 ? "Scanning plug-ins: " + std::to_string(scan.candidates) +
                                         (scan.candidates == 1 ? " file…" : " files…")
                                   : std::string("Scanning plug-ins…");
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
class PluginScanService {
  public:
    // Runs on the worker: scans, calls `counted` once the files to probe are known, and returns what the
    // loader holds afterwards with the report's state (`applyReport`). It stops early when `cancel` is set.
    using Scan = std::function<PluginListing(const std::atomic<bool> &cancel, const std::function<void(std::size_t)> &counted)>;
    // Runs `job` on the owner thread later; the job may find the service gone and does nothing then.
    using Post = std::function<void(std::function<void()>)>;
    PluginScanService(Scan scan, Post post) : scan_(std::move(scan)), post_(std::move(post)) {}
    PluginScanService(const PluginScanService &) = delete;
    PluginScanService &operator=(const PluginScanService &) = delete;
    ~PluginScanService() {
        shared_->alive = false;
        stop();
    }
    // Called on the owner after each change of `state()`: a scan begun, its files counted, its end.
    std::function<void()> onChange;
    const PluginScan &state() const noexcept { return state_; }
    bool running() const noexcept { return thread_.joinable(); }
    // Begins a scan on the worker. One already running is let finish and another follows it (the folders
    // changed, or Rescan was asked twice); the list stays on screen meanwhile.
    void start() {
        if (running() || held_ > 0) {
            again_ = true;
            return;
        }
        again_ = false;
        auto shared = shared_;
        shared->cancel = false;
        {
            std::lock_guard lock(shared->mutex);
            shared->result.reset();
            shared->counted = 0;
        }
        const auto generation = ++generation_;
        state_.phase = PluginScan::Phase::Scanning;
        state_.candidates = 0;
        shared->owner = this;
        thread_ = std::thread([shared, generation, scan = scan_, post = post_] {
            PluginListing found;
            try {
                found = scan(shared->cancel, [&shared, generation, &post](std::size_t n) {
                    {
                        std::lock_guard lock(shared->mutex);
                        shared->counted = n;
                    }
                    post([shared, generation] {
                        if (shared->alive && shared->owner != nullptr)
                            shared->owner->counted(generation);
                    });
                });
            } catch (const std::exception &e) {
                found.complete = false;
                found.stopped = std::string("the scan failed (") + e.what() + ")";
            }
            {
                std::lock_guard lock(shared->mutex);
                shared->result = std::move(found);
            }
            post([shared, generation] {
                if (shared->alive && shared->owner != nullptr)
                    shared->owner->finished(generation);
            });
        });
        changed();
    }
    // The gate in front of the loader: `hold()` waits for a running scan to end and adopts its result
    // there and then; until the matching `release()` no scan begins (one asked for meanwhile, by Rescan,
    // a folder change or a choice of Plug-Ins, only marks itself), so the owner has the loader to itself
    // for the whole load, even if a plug-in runs a message loop of its own while it is made. Held twice
    // (a load inside a load), it is released twice. The scan asked for begins from the owner's queue
    // after the last release.
    void hold() {
        ++held_;
        if (running()) {
            thread_.join();
            adopt(false);
        }
    }
    void release() {
        if (held_ == 0 || --held_ != 0 || !again_)
            return;
        post_([shared = shared_] {
            if (shared->alive && shared->owner != nullptr)
                shared->owner->resume();
        });
    }
    bool held() const noexcept { return held_ > 0; }
    // The RAII form for one load.
    struct Hold {
        explicit Hold(PluginScanService &s) : service(s) { service.hold(); }
        ~Hold() { service.release(); }
        Hold(const Hold &) = delete;
        Hold &operator=(const Hold &) = delete;
        PluginScanService &service;
    };
    // Quit: cancels a running scan and waits for it; its result is dropped and the list stays as it was.
    void stop() {
        if (!running())
            return;
        shared_->cancel = true;
        thread_.join();
        again_ = false;
        ++generation_;
        state_.phase = PluginScan::Phase::Done;
        state_.candidates = 0;
    }
    // A listing read on the owner while no scan runs: the loader scanned on its own, for a project.
    // Offered while a scan runs, it is ignored: the scan's own result follows.
    void adoptListing(PluginListing listing) {
        if (running())
            return;
        state_.listing = std::move(listing);
        state_.phase = PluginScan::Phase::Done;
        changed();
    }

  private:
    struct Shared {
        std::atomic<bool> cancel{false};
        std::mutex mutex;
        std::optional<PluginListing> result;
        std::size_t counted = 0;
        bool alive = true;                   // owner thread only
        PluginScanService *owner = nullptr;  // owner thread only
    };
    void counted(std::uint64_t generation) {
        if (generation != generation_ || !running())
            return;
        std::size_t n = 0;
        {
            std::lock_guard lock(shared_->mutex);
            n = shared_->counted;
        }
        state_.candidates = n;
        changed();
    }
    void finished(std::uint64_t generation) {
        if (generation != generation_ || !running())
            return;
        thread_.join();
        adopt(true);
    }
    void adopt(bool restart) {
        std::optional<PluginListing> result;
        {
            std::lock_guard lock(shared_->mutex);
            result = std::move(shared_->result);
            shared_->result.reset();
        }
        ++generation_;
        state_.phase = PluginScan::Phase::Done;
        state_.candidates = 0;
        if (result)
            state_.listing = std::move(*result);
        changed();
        if (restart && again_)
            start();
    }
    // The scan asked for while the loader was held, once the hold is gone.
    void resume() {
        if (!running() && held_ == 0 && again_)
            start();
    }
    void changed() {
        if (onChange)
            onChange();
    }
    Scan scan_;
    Post post_;
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    std::thread thread_;
    std::uint64_t generation_ = 0;
    bool again_ = false;
    int held_ = 0;
    PluginScan state_;
};
} // namespace adi::ui
