// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The MIDI Note Editor's geometry (STEP-7-PLAN 7.7 step 2), headless as ArrangementGeometry is.
//
// Time runs along the arrangement's own axis: the editor holds an ArrangementGeometry for its
// zoom, scroll and grid, in arrangement ticks, so its ruler reads the song's bars as Live 12's
// does for an Arrangement clip (manual 10.2). Pitch runs down the note ruler: one key track per
// MIDI key, the highest at the top (manual 10.1), or only the keys that hold notes when folded.
//
// A note's times are source-relative (ENGINE-CLIP-DETAILS): `ClipWindow` places them on the
// arrangement's axis through the clip's position and its start marker.
#pragma once

#include "adi/engine/clip_details.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adi::ui {

inline constexpr int kNoteKeys = 128;

// A key's name with middle C (key 60) as C3, English names: the director's defaults (Master
// Reference 5.1). Key 0 is C-2 and key 127 is G8, the range of Live's note ruler.
inline const char *pitchClassName(int key) noexcept {
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[((key % 12) + 12) % 12];
}
inline std::string noteName(int key) {
    const int k = std::clamp(key, 0, kNoteKeys - 1);
    return std::string(pitchClassName(k)) + std::to_string(k / 12 - 2);
}
inline constexpr bool blackKey(int key) noexcept {
    const int pc = ((key % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// Where a clip's source time sits on the arrangement's axis. Without a loop, the source tick
// `startMarker` plays at the clip's position.
struct ClipWindow {
    std::int64_t pos = 0, length = 0; // the clip in the arrangement, ticks
    std::int64_t startMarker = 0;     // the source tick that plays at `pos`
    constexpr std::int64_t toArrangement(std::int64_t source) const noexcept { return pos + (source - startMarker); }
    constexpr std::int64_t toSource(std::int64_t arrangement) const noexcept {
        return startMarker + (arrangement - pos);
    }
    constexpr std::int64_t end() const noexcept { return pos + length; }
    static ClipWindow of(const engine::ClipDetails &clip) noexcept {
        ClipWindow w;
        w.pos = clip.posTicks.value_or(0);
        w.length = clip.lengthTicks.value_or(0);
        w.startMarker = clip.startMarkerTicks;
        return w;
    }
};

// The note ruler: key tracks, their height, the scroll and Fold.
struct KeyTracks {
    static constexpr int kMinHeight = 4, kMaxHeight = 40, kDefaultHeight = 12;
    int keyHeight = kDefaultHeight;
    int scrollY = 0; // pixels scrolled down from the top row
    // Fold to Notes (manual 10.2, F): only these keys have a track, highest first. Empty: all 128.
    std::vector<int> folded;

    bool isFolded() const noexcept { return !folded.empty(); }
    int rowCount() const noexcept { return isFolded() ? static_cast<int>(folded.size()) : kNoteKeys; }
    int contentHeight() const noexcept { return rowCount() * keyHeight; }
    // The row a key is drawn in, the top row being 0; none when Fold hides it.
    std::optional<int> rowOf(int key) const noexcept {
        if (key < 0 || key >= kNoteKeys)
            return std::nullopt;
        if (!isFolded())
            return kNoteKeys - 1 - key;
        const auto it = std::find(folded.begin(), folded.end(), key);
        if (it == folded.end())
            return std::nullopt;
        return static_cast<int>(it - folded.begin());
    }
    std::optional<int> keyOfRow(int row) const noexcept {
        if (row < 0 || row >= rowCount())
            return std::nullopt;
        return isFolded() ? folded[static_cast<std::size_t>(row)] : kNoteKeys - 1 - row;
    }
    // The top of a key's track in the editor, in pixels; none when Fold hides it.
    std::optional<int> y(int key) const noexcept {
        const auto row = rowOf(key);
        if (!row)
            return std::nullopt;
        return *row * keyHeight - scrollY;
    }
    std::optional<int> keyAt(int pixel) const noexcept {
        const int at = pixel + scrollY;
        if (at < 0 || keyHeight <= 0)
            return std::nullopt;
        return keyOfRow(at / keyHeight);
    }
    int maxScroll(int viewHeight) const noexcept { return std::max(0, contentHeight() - std::max(0, viewHeight)); }
    void clampScroll(int viewHeight) noexcept { scrollY = std::clamp(scrollY, 0, maxScroll(viewHeight)); }
    void scrollBy(int pixels, int viewHeight) noexcept {
        scrollY += pixels;
        clampScroll(viewHeight);
    }
    // Page Up and Page Down move by an octave; with Shift, by one key track (manual 10.2).
    void page(bool up, bool oneTrack, int viewHeight) noexcept {
        scrollBy((up ? -1 : 1) * keyHeight * (oneTrack ? 1 : 12), viewHeight);
    }
    // Zooms the key tracks, keeping the track under `anchor` where it is.
    void zoom(int nextHeight, int anchor, int viewHeight) noexcept {
        const int next = std::clamp(nextHeight, kMinHeight, kMaxHeight);
        if (next == keyHeight)
            return;
        const double row = static_cast<double>(anchor + scrollY) / static_cast<double>(keyHeight);
        keyHeight = next;
        scrollY = static_cast<int>(row * static_cast<double>(next)) - anchor;
        clampScroll(viewHeight);
    }
    // Centres the keys `low` to `high` in the view, as the editor opens on a clip's notes.
    void show(int low, int high, int viewHeight) noexcept {
        const auto top = rowOf(std::clamp(high, 0, kNoteKeys - 1));
        const auto bottom = rowOf(std::clamp(low, 0, kNoteKeys - 1));
        if (!top || !bottom)
            return;
        const int span = (*bottom - *top + 1) * keyHeight;
        scrollY = *top * keyHeight - (std::max(0, viewHeight) - span) / 2;
        clampScroll(viewHeight);
    }
    // Fold to Notes: the keys the notes use, highest first; none of them, and Fold stays off.
    void fold(const std::vector<engine::ClipNote> &notes, int viewHeight) {
        bool used[kNoteKeys] = {};
        for (const auto &n : notes)
            if (n.key < kNoteKeys)
                used[n.key] = true;
        folded.clear();
        for (int key = kNoteKeys - 1; key >= 0; --key)
            if (used[key])
                folded.push_back(key);
        clampScroll(viewHeight);
    }
    void unfold(int viewHeight) noexcept {
        folded.clear();
        clampScroll(viewHeight);
    }
};

// The lowest and highest key among the notes; none without notes.
struct KeyRange {
    int low = 0, high = 0;
};
inline std::optional<KeyRange> keyRange(const std::vector<engine::ClipNote> &notes) noexcept {
    if (notes.empty())
        return std::nullopt;
    KeyRange r{kNoteKeys - 1, 0};
    for (const auto &n : notes) {
        r.low = std::min(r.low, static_cast<int>(n.key));
        r.high = std::max(r.high, static_cast<int>(n.key));
    }
    return r;
}

// A note's box in the editor: x from the time axis, y from the key tracks.
struct NoteBox {
    double x = 0, width = 0;
    int y = 0, height = 0;
    constexpr bool contains(double px, double py) const noexcept {
        return px >= x && px < x + width && py >= static_cast<double>(y) && py < static_cast<double>(y + height);
    }
};

// What a point on a note takes hold of: its start edge, its end edge, or the note itself (manual
// 10.5.4, 10.5.5). A note too narrow for both edges is all body, so it can still be moved.
enum class NotePart { None, Body, Start, End };
inline constexpr double kNoteEdge = 4;
inline constexpr NotePart notePart(const NoteBox &box, double px, double py) noexcept {
    if (!box.contains(px, py))
        return NotePart::None;
    if (box.width < 3 * kNoteEdge)
        return NotePart::Body;
    if (px < box.x + kNoteEdge)
        return NotePart::Start;
    if (px >= box.x + box.width - kNoteEdge)
        return NotePart::End;
    return NotePart::Body;
}

// The Velocity lane (manual 10.6): a marker's height for a velocity 1 to 127, in a lane of
// `height` pixels whose top is `top`; and back.
inline constexpr double velocityY(int velocity, double top, double height) noexcept {
    const double v = static_cast<double>(std::clamp(velocity, 1, 127));
    return top + height - height * v / 127.;
}
inline constexpr int velocityAt(double pixel, double top, double height) noexcept {
    if (height <= 0)
        return 1;
    const double v = (top + height - pixel) * 127. / height;
    return std::clamp(static_cast<int>(v + .5), 1, 127);
}

// A length of musical time as Live's Length fields read it, bars.beats.sixteenths from zero, in a
// meter (manual 8.2): two bars of 4/4 is 2.0.0.
struct BarsBeats {
    std::int64_t bars = 0, beats = 0, sixteenths = 0;
    bool operator==(const BarsBeats &) const = default;
};
inline BarsBeats barsBeats(std::int64_t ticks, const textproj::Meter &meter) noexcept {
    BarsBeats out;
    const std::int64_t bar = meter.barTicks();
    const std::int64_t beat = meter.denominator == 0 ? 0 : textproj::kWhole / meter.denominator;
    const std::int64_t sixteenth = textproj::kPPQ / 4;
    if (ticks <= 0 || bar <= 0 || beat <= 0)
        return out;
    out.bars = ticks / bar;
    const std::int64_t inBar = ticks % bar;
    out.beats = inBar / beat;
    out.sixteenths = (inBar % beat) / sixteenth;
    return out;
}

} // namespace adi::ui
