// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
namespace adi::ui {
// Live's MIDI track, drawn by its kind's hint (ADR-0045: the hint sets the icon and the default
// device): All Ins and All Channels in its In/Out column, where an audio track has Ext. In and 1/2.
inline bool isMidiTrack(const engine::TrackNode &t) noexcept { return t.kind == "midi" || t.kind == "instrument"; }
// Whether a track has an audio mixer. Every track has, but a MIDI track holding no instrument: Live
// draws that one with no volume, pan or sends, playing to No Output, until an instrument goes on it
// (the director's Live 11.2.7 capture, look-live11.md difference row 3). What the track holds
// decides, never its kind alone (ADR-0045). Without the devices to read, a MIDI track has none.
// `Devices` is the window's device publication (`ParameterPublication`): each has a track and
// whether it is an instrument.
template <class Devices> bool hasAudioMixer(const engine::TrackNode &t, const Devices *devices) noexcept {
    if (!isMidiTrack(t))
        return true;
    if (devices != nullptr)
        for (const auto &d : *devices)
            if (d.track == t.id && d.instrument)
                return true;
    return false;
}
} // namespace adi::ui
