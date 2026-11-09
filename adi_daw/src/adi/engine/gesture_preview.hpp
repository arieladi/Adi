// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/gesture_presentation.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
namespace adi::engine {
class Session;
class ProjectView;
class ClipPlayback;
class MidiClips;
struct GestureRuntime;
/// Owned raw-control images. Only the single audio owner calls apply; the
/// publisher/generation keeps every lane and node owner alive through it.
class GestureControls {
public:
    virtual ~GestureControls()=default;
    virtual void apply() const noexcept=0;
};
class GestureAudio {
public:
    virtual ~GestureAudio()=default;
    virtual void apply(std::uint64_t generation) noexcept=0;
    virtual std::uint64_t reserveGeneration()=0;
    virtual std::uint64_t currentGeneration() const noexcept=0;
    virtual void publishedGeneration(std::uint64_t) noexcept=0;
    virtual void collect()=0;
    virtual void reset()=0;
    virtual void stageReset(std::uint64_t generation)=0;
};
std::shared_ptr<GestureAudio> makeGestureAudio();
std::shared_ptr<const GestureControls> nativeGestureControls(Session&);
std::vector<std::uint8_t> capturePresentationImage(const Store&);
/// Existing refresh/tick integration. A handled refresh has adopted committed
/// truth without overwriting a same-base preview; reapply uses the real tick.
bool isGestureCommittedRefresh(const std::shared_ptr<GestureRuntime>&) noexcept;
std::optional<bool> refreshGestureRuntime(const std::shared_ptr<GestureRuntime>&,const Store&);
void pumpGestureCoordinator(Session&,std::int64_t nowMs);
std::optional<bool> rebuildGestureRuntime(const std::shared_ptr<GestureRuntime>&);
ClipPlayback* gestureClips(const std::shared_ptr<GestureRuntime>&) noexcept;
MidiClips* gestureMidi(const std::shared_ptr<GestureRuntime>&) noexcept;
} // namespace adi::engine
