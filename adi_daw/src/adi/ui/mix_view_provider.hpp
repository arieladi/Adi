// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
#include <cstdint>
namespace adi::ui {
// What the Mix View shows of a channel that the engine has not written yet (UI-ARCHITECTURE §12,
// "Stubs, by rule"; ADR-0210 d3). Every channel and group has three slots, present but off
// (ADR-0204 d1, ADR-0209 §1): ADI-Q, ADI-L and the sidechain slot. The window reads them through
// this interface: the stub below until the snapshot carries the slots (window log, N16), then the
// snapshot's provider, and no tab waits.
enum class SidechainMode { Off, Rmsc, Spacer, Fuser };
struct ChannelSlots {
    bool eq = false;        // ADI-Q engaged
    bool limiter = false;   // ADI-L engaged
    bool limiterFull = false; // ADI-L's mode: full on Main, light on every other channel
    SidechainMode sidechain = SidechainMode::Off;
    bool operator==(const ChannelSlots &) const = default;
};
class MixViewProvider {
  public:
    virtual ~MixViewProvider() = default;
    // The slots of a channel in the presented snapshot. Called while painting: never allocates.
    virtual ChannelSlots slots(const engine::TrackNode &) const noexcept = 0;
    // Whether the slots are the engine's or the stub's placeholders, for the Info View and tests.
    virtual bool live() const noexcept = 0;
};
// The layout's placeholders: every slot present and off, ADI-L full on Main and light elsewhere,
// as ADR-0204 d1 sets them up on a new channel.
class StubMixViewProvider final : public MixViewProvider {
  public:
    ChannelSlots slots(const engine::TrackNode &track) const noexcept override {
        ChannelSlots s;
        s.limiterFull = track.kind == "master";
        return s;
    }
    bool live() const noexcept override { return false; }
};
inline const char *sidechainName(SidechainMode mode) noexcept {
    switch (mode) {
    case SidechainMode::Rmsc:
        return "RMSC";
    case SidechainMode::Spacer:
        return "ADI Spacer";
    case SidechainMode::Fuser:
        return "ADI Fuser";
    case SidechainMode::Off:
        break;
    }
    return "Off";
}
} // namespace adi::ui
