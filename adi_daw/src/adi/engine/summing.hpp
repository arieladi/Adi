// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native analog group summing -- ADR-0173, ADR-0174.
//
// A group with summing on plays each child through its flavour's CHANNEL half
// before the sum, and the sum through the BUSS half: an Airwindows console
// system, run by the mixer rather than by a plug-in on every track.
//
// Where the halves go, without touching the realiser: a track's chain is the
// list `devicesFor` returns (ADR-0122), realised in order after the track's
// junction and before whatever the track feeds. So
//   - the buss half is FIRST in the group's chain: right after the sum, before
//     the group's own devices;
//   - a channel half is LAST in a child's chain: after the child's strip, so
//     after its fader and pan, on its way into the group.
//
// The halves are session-owned, like strips (ADR-0163), and kept across
// rebuilds while their flavour stands, so an edit does not reset a console's
// filters mid-song. A replaced half lives on in any retired graph that still
// plays it: `sourceLifetime` hands the realiser a share of it (ADR-0164).
//
// Drive is gain staging (ADR-0173 d5): +d dB into every channel half and -d dB
// after the buss half. The curves are hit harder and the level stays where it
// was, whatever the flavour. And at 0 dB the level is where it was without
// summing: each flavour's own gain staging is MEASURED once and made up after
// its buss half (ADR-0174), so turning summing on changes the colour only.

#pragma once

#include "adi/engine/graph.hpp"
#include "adi/store_rows.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct AirwinConsolidatedBase;   // Airwindows' own base, only in summing.cpp

namespace adi::engine {

/// One half of a console on the audio path: a gain in, the algorithm in place,
/// a gain out. Zero latency.
class ConsoleNode final : public Node, public std::enable_shared_from_this<ConsoleNode> {
public:
    ConsoleNode(std::unique_ptr<AirwinConsolidatedBase> fx, std::string what);
    ~ConsoleNode() override;
    ConsoleNode(const ConsoleNode&) = delete;
    ConsoleNode& operator=(const ConsoleNode&) = delete;

    /// Message thread: the linear gains either side of the algorithm.
    void setGains(float pre, float post) noexcept;

    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void process(const NodeIo& io) noexcept override;
    [[nodiscard]] std::shared_ptr<void> sourceLifetime() const override;
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "console"; }

    /// What this half is, for a test or a problem: "Console9Channel".
    [[nodiscard]] const std::string& what() const noexcept { return what_; }

private:
    std::unique_ptr<AirwinConsolidatedBase> fx_;
    std::string what_;
    std::atomic<float> pre_{1.0f};
    std::atomic<float> post_{1.0f};
    double sampleRate_ = 0.0;    ///< message thread
};

/// The session's summing: which halves exist, and where each one goes.
class GroupSumming {
public:
    /// Message thread, before the graph that plays them is built.
    void sync(const rows::Model& model);

    /// The buss half, first in a group's chain; null for none.
    [[nodiscard]] Node* headFor(std::int64_t trackId) noexcept;
    /// The channel half, last in a child's chain; null for none.
    [[nodiscard]] Node* tailFor(std::int64_t trackId) noexcept;

    [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }

    /// For tests: the half on a track, or null.
    [[nodiscard]] const ConsoleNode* bussOf(std::int64_t groupId) const noexcept;
    [[nodiscard]] const ConsoleNode* channelOf(std::int64_t childId) const noexcept;

private:
    struct Half {
        std::string flavor;
        std::int64_t group = 0;
        std::shared_ptr<ConsoleNode> node;
    };
    std::map<std::int64_t, Half> busses_;     ///< by group
    std::map<std::int64_t, Half> channels_;   ///< by child
    std::vector<std::string> problems_;
};

/// Builds one half of a flavour, or null for an unknown key. For tests too.
[[nodiscard]] std::shared_ptr<ConsoleNode> makeConsoleHalf(const std::string& flavor, bool buss);

/// The measured gain that brings a flavour's channel-then-buss to unity at a
/// low level (summing.cpp). The buss half's output is scaled by it. 1 for an
/// unknown key.
[[nodiscard]] double summingUnityGain(const std::string& flavor);

}  // namespace adi::engine
