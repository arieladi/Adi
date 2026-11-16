// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The MIDI Note Editor's geometry (STEP-7-PLAN 7.7 step 2), without a window: the note ruler's
// names, key tracks and their scroll, zoom and Fold, a clip's source time on the arrangement's
// axis, a note's parts, the Velocity lane, and Live's Length reading.

#include "adi/ui/note_geometry.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace adi::ui;
using adi::engine::ClipNote;

int g_checks = 0, g_failures = 0;
void check(bool cond, const std::string &what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

ClipNote note(std::uint64_t id, int key, std::int64_t start, std::int64_t duration) {
    ClipNote n;
    n.id = id;
    n.key = static_cast<std::uint8_t>(key);
    n.startTicks = start;
    n.durationTicks = duration;
    n.velocity = 100;
    return n;
}

void names() {
    check(noteName(60) == "C3", "middle C is C3, the director's default");
    check(noteName(0) == "C-2" && noteName(127) == "G8", "the ruler runs from C-2 to G8");
    check(noteName(61) == "C#3" && noteName(59) == "B2" && noteName(72) == "C4", "sharps, and the octave turns at C");
    check(noteName(-5) == "C-2" && noteName(500) == "G8", "a key outside the range reads as the nearest end");
    check(blackKey(61) && blackKey(63) && blackKey(66) && blackKey(68) && blackKey(70), "the five black keys");
    check(!blackKey(60) && !blackKey(62) && !blackKey(64) && !blackKey(65) && !blackKey(67) && !blackKey(69) &&
              !blackKey(71),
          "the seven white keys");
}

void keyTracks() {
    KeyTracks k;
    check(k.rowCount() == 128 && k.contentHeight() == 128 * 12, "128 key tracks at the default height");
    check(k.rowOf(127) == 0 && k.rowOf(0) == 127 && !k.rowOf(128) && !k.rowOf(-1), "the highest key is the top row");
    check(k.y(127) == 0 && k.y(126) == 12 && k.keyAt(0) == 127 && k.keyAt(11) == 127 && k.keyAt(12) == 126,
          "a key's track and the key under a pixel agree");
    k.scrollBy(24, 300);
    check(k.scrollY == 24 && k.y(125) == 0 && k.keyAt(0) == 125, "scrolled two tracks, key 125 is at the top");
    k.scrollBy(-1000, 300);
    check(k.scrollY == 0, "the scroll stops at the top");
    k.scrollBy(100000, 300);
    check(k.scrollY == 128 * 12 - 300 && k.keyAt(299) == 0, "and at the bottom, where key 0 is the last track");
    check(!k.keyAt(300 + 12), "below the last track there is no key");
    k.scrollY = 0;
    k.page(false, false, 300);
    check(k.scrollY == 12 * 12, "Page Down moves an octave");
    k.page(true, true, 300);
    check(k.scrollY == 11 * 12, "Shift+Page Up moves one key track");

    // Zoom keeps the track under the pointer where it is.
    KeyTracks z;
    z.scrollY = 40 * 12;
    const auto under = z.keyAt(150);
    z.zoom(24, 150, 300);
    check(z.keyHeight == 24 && z.keyAt(150) == under, "zooming the key tracks keeps the key under the pointer");
    z.zoom(1000, 150, 300);
    check(z.keyHeight == KeyTracks::kMaxHeight, "the key height stops at its maximum");
    z.zoom(0, 150, 300);
    check(z.keyHeight == KeyTracks::kMinHeight, "and at its minimum");

    // The editor opens on the clip's notes.
    KeyTracks s;
    s.show(60, 72, 300);
    const auto top = s.y(72), bottom = s.y(60);
    check(top && bottom && *top >= 0 && *bottom + s.keyHeight <= 300, "show() brings the low and the high key into view");
    check(*top == 300 - (*bottom + s.keyHeight) || *top + 1 == 300 - (*bottom + s.keyHeight) ||
              *top == 300 - (*bottom + s.keyHeight) + 1,
          "and centres them");
}

void fold() {
    const std::vector<ClipNote> notes{note(1, 60, 0, 10), note(2, 67, 0, 10), note(3, 60, 20, 10), note(4, 64, 0, 10)};
    KeyTracks k;
    k.fold(notes, 300);
    check(k.isFolded() && k.rowCount() == 3, "folded, only the three keys in use have a track");
    check(k.rowOf(67) == 0 && k.rowOf(64) == 1 && k.rowOf(60) == 2 && !k.rowOf(61), "highest first, the rest hidden");
    check(k.keyAt(0) == 67 && k.keyAt(12) == 64 && k.keyAt(24) == 60 && !k.keyAt(36), "and pixels map to those keys");
    check(k.scrollY == 0, "three tracks fit, so nothing is scrolled");
    k.unfold(300);
    check(!k.isFolded() && k.rowCount() == 128, "unfolded, all 128 are back");
    KeyTracks empty;
    empty.fold({}, 300);
    check(!empty.isFolded(), "a clip without notes cannot fold");
    const auto range = keyRange(notes);
    check(range && range->low == 60 && range->high == 67 && !keyRange({}), "the notes' lowest and highest key");
}

void clipWindow() {
    adi::engine::ClipDetails clip;
    clip.posTicks = 4 * adi::textproj::kWhole;
    clip.lengthTicks = 2 * adi::textproj::kWhole;
    clip.startMarkerTicks = adi::textproj::kPPQ;
    const auto w = ClipWindow::of(clip);
    check(w.toArrangement(adi::textproj::kPPQ) == 4 * adi::textproj::kWhole,
          "the source tick at the start marker plays at the clip's position");
    check(w.toArrangement(0) == 4 * adi::textproj::kWhole - adi::textproj::kPPQ,
          "what lies before the start marker lies before the clip");
    check(w.toSource(w.toArrangement(12345)) == 12345, "and back");
    check(w.end() == 6 * adi::textproj::kWhole, "the clip's end");
}

void parts() {
    const NoteBox wide{100, 40, 24, 12};
    check(notePart(wide, 101, 30) == NotePart::Start && notePart(wide, 120, 30) == NotePart::Body &&
              notePart(wide, 138, 30) == NotePart::End,
          "a note's start edge, body and end edge");
    check(notePart(wide, 99, 30) == NotePart::None && notePart(wide, 140, 30) == NotePart::None &&
              notePart(wide, 120, 23) == NotePart::None && notePart(wide, 120, 36) == NotePart::None,
          "outside the box there is no note");
    const NoteBox narrow{100, 8, 24, 12};
    check(notePart(narrow, 100, 30) == NotePart::Body && notePart(narrow, 107, 30) == NotePart::Body,
          "a note too narrow for edges is all body, so it can still be moved");
}

void velocity() {
    check(velocityY(127, 10, 60) == 10 && velocityY(1, 10, 60) > 69 && velocityY(1, 10, 60) < 70,
          "full velocity reaches the lane's top, velocity 1 sits just above its foot");
    for (int v : {1, 2, 64, 100, 126, 127})
        check(velocityAt(velocityY(v, 10, 60), 10, 60) == v, "velocity " + std::to_string(v) + " and back");
    check(velocityAt(-100, 10, 60) == 127 && velocityAt(1000, 10, 60) == 1, "a drag past the lane stops at 127 and at 1");
    check(velocityAt(20, 10, 0) == 1, "a lane with no height reads 1");
}

void lengths() {
    const adi::textproj::Meter four{0, 4, 4}, six{0, 6, 8};
    check(barsBeats(2 * adi::textproj::kWhole, four) == BarsBeats{2, 0, 0}, "two bars of 4/4 read 2.0.0");
    check(barsBeats(adi::textproj::kWhole + adi::textproj::kPPQ + adi::textproj::kPPQ / 4, four) == BarsBeats{1, 1, 1},
          "a bar, a beat and a sixteenth read 1.1.1");
    check(barsBeats(6 * adi::textproj::kPPQ / 2 + adi::textproj::kPPQ / 2, six) == BarsBeats{1, 1, 0},
          "in 6/8 a bar is six eighths and a beat is an eighth");
    check(barsBeats(0, four) == BarsBeats{} && barsBeats(-5, four) == BarsBeats{}, "nothing, and less, read 0.0.0");
}

} // namespace

int main() {
    names();
    keyTracks();
    fold();
    clipWindow();
    parts();
    velocity();
    lengths();
    std::printf("%s -- %d checks, %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
