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
// Whether a clip on track `from` may be dragged onto track `to` (Live 12 manual 6.7, "to another
// song position or track"): a track that plays clips (an audio or a MIDI track, never a group, a
// return or Main) of its own track's family, audio to audio and MIDI to MIDI, as Live keeps audio
// clips on audio tracks. The snapshot carries no clip's kind, so its track's stands in (ADR-0045:
// a MIDI clip on an audio track is legal, and moves among audio tracks).
inline bool canTakeClip(const engine::TrackNode &to, const engine::TrackNode &from) noexcept {
    const bool plays = to.kind == "audio" || isMidiTrack(to);
    return plays && isMidiTrack(to) == isMidiTrack(from);
}
} // namespace adi::ui
