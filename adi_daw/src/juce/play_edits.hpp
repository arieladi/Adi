// SPDX-License-Identifier: GPL-3.0-or-later
//
// adi_play's window on the parameter-op glue (ADR-0124) while a project plays.
//
// adi_play used to make a ParamOps only to save state (ADR-0142), and never
// drained one while playing. So nothing could show whether a plug-in sends
// its automated parameters back as edits -- the loop ADR-0162 exists to stop:
// an automation value, echoed by the plug-in, becomes an op, and the op
// overrides the lane that produced it. Checking for that is 27a's DONE.
//
// The watch attaches every loaded device when it is made. While a device
// plays, it drains on the message thread. After the run it reports what came
// back: every parameter change a plug-in sent to the capture, and the
// `device.setParam` / `device.loadState` requests the glue would have
// committed. It never commits them. A player that rewrote the project while
// playing it would hide the very thing it is measuring.
//
// `--expect-no-edits` turns "anything came back" into exit code 4 (2 and 3
// are `--require-devices` and `--require-peak`).
//
// LIFETIME: made after the Session and destroyed before it, because ParamOps
// writes through each device when it unhooks (ADR-0124 d1). `finish`
// detaches every device, so a later `--save-state` can attach its own glue.
//
// Header-only on purpose: adi_play's target lists its sources in
// CMakeLists.txt, and this stays out of that file while other work is open
// on it.
#pragma once

#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"

#include <juce_events/juce_events.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace adi_play {

class EditWatch : private juce::Timer {
public:
    explicit EditWatch(adi::engine::Session& s) : session_(s) {
        ops_.attachSession(s);
        // A baseline, so nothing that reached the capture before the run
        // counts against it. Attaching seeds values without pushing them, so
        // this is normally zero; it is here so the check never depends on it.
        for (std::size_t i = 0; i < s.entryCount(); ++i) {
            const std::int64_t id = s.entryAt(i).deviceId;
            if (ops_.isAttached(id)) baseline_[id] = reached(id);
        }
    }
    ~EditWatch() override { stopTimer(); }
    EditWatch(const EditWatch&) = delete;
    EditWatch& operator=(const EditWatch&) = delete;

    /// Live playback: drain every `ms` on the message thread while the device runs.
    void start(int ms) { startTimer(ms); }

    /// After the run. It lets the message thread deliver what a plug-in queued
    /// for it, drains once more past the capture's quiet window so an open
    /// gesture closes, prints the report, and detaches every device.
    /// Returns `rc` if it is not 0; otherwise 4 when `expectNone` and anything
    /// came back, else 0.
    int finish(int rc, bool expectNone) {
        stopTimer();
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating()) mm->runDispatchLoopUntil(250);
        ops_.drain(now() + 1000, requests_);   // 1 s is past the capture's 150 ms quiet window

        std::map<std::pair<std::int64_t, std::string>, std::int64_t> perParam;
        std::int64_t setParams = 0, loadStates = 0;
        for (const adi::OpRequest& r : requests_) {
            if (r.opType == "device.setParam") {
                ++setParams;
                ++perParam[{r.payload.value("dev", std::int64_t{0}), r.payload.value("param", std::string{})}];
            } else if (r.opType == "device.loadState") {
                ++loadStates;
            }
        }

        std::int64_t cameBack = 0;
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < session_.entryCount(); ++i) {
            const adi::engine::Session::Entry& e = session_.entryAt(i);
            if (!ops_.isAttached(e.deviceId)) continue;
            const adi::engine::ParamEditCapture::Stats* st = ops_.captureStats(e.deviceId);
            const std::int64_t got = reached(e.deviceId) - baseline_[e.deviceId];
            if (got <= 0) continue;
            cameBack += got;
            std::string line = "    devices#" + std::to_string(e.deviceId) + "  " + e.name + "  " +
                               std::to_string(got) + " change(s)";
            if (st != nullptr && st->dropped > 0)
                line += ", " + std::to_string(st->dropped) + " dropped when its ring was full";
            lines.push_back(line);
            int shown = 0;
            for (const auto& [key, count] : perParam) {
                if (key.first != e.deviceId) continue;
                if (++shown > 8) { lines.push_back("      ..."); break; }
                lines.push_back("      param " + key.second + ": " + std::to_string(count) + " request(s)");
            }
        }

        if (cameBack == 0 && requests_.empty()) {
            std::printf("  edits    none: no plug-in sent a parameter change back during the run (ADR-0124)\n");
        } else {
            std::printf("  edits    %lld parameter change(s) came back from plug-ins; the glue would commit "
                        "%lld device.setParam and %lld device.loadState request(s) -- adi_play only watches\n",
                        static_cast<long long>(cameBack), static_cast<long long>(setParams),
                        static_cast<long long>(loadStates));
            for (const std::string& l : lines) std::printf("%s\n", l.c_str());
        }

        for (const auto& [id, base] : baseline_) ops_.detach(id);

        if (rc != 0) return rc;
        if (expectNone && (cameBack > 0 || !requests_.empty())) {
            std::printf("\nFAILED -- --expect-no-edits: plug-ins sent parameter changes back during the run; "
                        "an automation value that becomes an edit overrides its own lane (ADR-0162)\n");
            return 4;
        }
        return 0;
    }

private:
    void timerCallback() override { ops_.drain(now(), requests_); }

    static std::int64_t now() { return static_cast<std::int64_t>(juce::Time::getMillisecondCounter()); }

    std::int64_t reached(std::int64_t deviceId) const {
        const adi::engine::ParamEditCapture::Stats* st = ops_.captureStats(deviceId);
        return st == nullptr ? 0 : st->pushed + st->dropped;
    }

    adi::engine::Session& session_;
    adi::engine::ParamOps ops_;
    std::map<std::int64_t, std::int64_t> baseline_;
    std::vector<adi::OpRequest> requests_;
};

}  // namespace adi_play
