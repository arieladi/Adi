// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Note Editor's edits and Insert Empty MIDI Clip(s), without a window (STEP-7-PLAN 7.7 step 3):
// what each gesture's batch holds, its label, what it selects, and what it refuses.

#include "adi/ui/clip_edits.hpp"
#include "adi/ui/note_edits.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace adi;
using namespace adi::ui;
using engine::ClipDetails;
using engine::ClipNote;

int g_checks = 0, g_failures = 0;
void check(bool cond, const std::string &what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}
const auto beat = textproj::kPPQ;

ClipDetails clip() {
    ClipDetails c;
    c.id = 7;
    c.kind = "midi";
    auto note = [](std::uint64_t id, int key, std::int64_t start, std::int64_t duration, int velocity) {
        ClipNote n;
        n.id = id;
        n.key = static_cast<std::uint8_t>(key);
        n.startTicks = start;
        n.durationTicks = duration;
        n.velocity = static_cast<std::uint8_t>(velocity);
        return n;
    };
    c.notes = {note(3, 60, 0, beat, 100), note(9, 64, beat, 2 * beat, 120), note(4, 126, 0, kShortestNote, 5)};
    return c;
}

void adding() {
    const auto c = clip();
    check(freeNoteId(c) == 10 && freeNoteId(ClipDetails{}) == 1, "a free id is one past the highest, never zero");
    const auto e = addNote(c, 3 * beat, beat, 67);
    check(e.refusal.empty() && e.ops.size() == 1 && e.ops[0].opType == "note.insert" && e.ops[0].label == "Add Note" &&
              e.ops[0].payload ==
                  Payload({{"clip", 7}, {"note", std::uint64_t{10}}, {"start", 3 * beat}, {"dur", beat}, {"key", 67}, {"vel", 100}}) &&
              e.select == std::vector<std::uint64_t>{10},
          "a new note: one note.insert with a free id, selected");
    check(addNote(c, 0, 1, 60).ops[0].payload.at("dur") == kShortestNote, "a new note is never shorter than the shortest");
    check(addNote(c, 0, beat, 60, 500).ops[0].payload.at("vel") == 127 && addNote(c, 0, beat, 60, 0).ops[0].payload.at("vel") == 1,
          "its velocity stays within 1 to 127");
    check(!addNote(c, 0, beat, 128).refusal.empty() && !addNote(c, 0, beat, -1).refusal.empty(), "there is no key past the ends");
}

void deleting() {
    const auto c = clip();
    const auto one = deleteNotes(c, {9});
    check(one.ops.size() == 1 && one.ops[0].opType == "note.delete" && one.ops[0].label == "Delete Note" &&
              one.ops[0].payload == Payload({{"clip", 7}, {"note", std::uint64_t{9}}}) && one.select.empty(),
          "one note deleted: one note.delete, nothing selected after");
    const auto two = deleteNotes(c, {3, 9, 555});
    check(two.ops.size() == 2 && two.ops[0].label == "Delete Notes", "several: one op each, a note the clip does not hold is left out");
    check(deleteNotes(c, {}).ops.empty() && deleteNotes(c, {}).refusal.empty(), "none selected: nothing to do");
}

void moving() {
    const auto c = clip();
    const auto e = moveNotes(c, {3, 9}, beat, 2);
    check(e.ops.size() == 2 && e.ops[0].opType == "note.move" && e.ops[0].label == "Move Notes" &&
              e.ops[0].payload == Payload({{"clip", 7}, {"note", std::uint64_t{3}}, {"start", beat}, {"key", 62}}) &&
              e.ops[1].payload == Payload({{"clip", 7}, {"note", std::uint64_t{9}}, {"start", 2 * beat}, {"key", 66}}) &&
              e.select == std::vector<std::uint64_t>({3, 9}),
          "two notes moved together: a note.move each, with start and key, still selected");
    check(moveNotes(c, {3}, 0, 0).ops.empty() && moveNotes(c, {3}, 0, 0).refusal.empty(), "a drag that ends where it began writes nothing");
    check(moveNotes(c, {3}, -beat, 0).ops[0].payload.at("start") == -beat, "a note may start before the clip's start");
    // The chord keeps its shape at the ruler's ends.
    const auto up = moveNotes(c, {3, 4}, 0, 12);
    check(up.ops.size() == 2 && up.ops[0].payload.at("key") == 61 && up.ops[1].payload.at("key") == 127,
          "an octave up with a note one key from the top moves both one key");
    const auto down = moveNotes(c, {3, 9}, 0, -100);
    check(down.ops[0].payload.at("key") == 0 && down.ops[1].payload.at("key") == 4, "and down, both stop when the lowest reaches key 0");
    check(moveNotes(c, {3}, beat, 0).ops[0].label == "Move Note", "one note is named in the singular");
}

void resizing() {
    const auto c = clip();
    const auto e = resizeNotes(c, {3, 9}, beat);
    check(e.ops.size() == 2 && e.ops[0].opType == "note.resize" && e.ops[0].label == "Resize Notes" &&
              e.ops[0].payload == Payload({{"clip", 7}, {"note", std::uint64_t{3}}, {"dur", 2 * beat}}) &&
              e.ops[1].payload.at("dur") == 3 * beat,
          "the ends moved a beat later: a note.resize each");
    const auto shrink = resizeNotes(c, {3, 4}, -2 * beat);
    check(shrink.ops.size() == 1 && shrink.ops[0].payload.at("dur") == kShortestNote,
          "shortened past nothing, a note stops at the shortest; one already there is left alone");
    const auto trim = trimNotes(c, {9}, beat);
    check(trim.ops.size() == 2 && trim.ops[0].opType == "note.move" &&
              trim.ops[0].payload == Payload({{"clip", 7}, {"note", std::uint64_t{9}}, {"start", 2 * beat}, {"key", 64}}) &&
              trim.ops[1].opType == "note.resize" && trim.ops[1].payload.at("dur") == beat && trim.ops[0].label == "Resize Note",
          "the start moved a beat later: the note starts later and ends where it did, as one batch");
    const auto past = trimNotes(c, {9}, 10 * beat);
    check(past.ops[0].payload.at("start") == beat + 2 * beat - kShortestNote && past.ops[1].payload.at("dur") == kShortestNote,
          "the start stops a shortest note before the end");
    const auto earlier = trimNotes(c, {9}, -beat);
    check(earlier.ops[0].payload.at("start") == 0 && earlier.ops[1].payload.at("dur") == 3 * beat, "moved earlier, the note grows");
    check(trimNotes(c, {4}, beat).ops.empty(), "a note already the shortest cannot be trimmed");
}

void velocities() {
    const auto c = clip();
    const auto e = changeVelocities(c, {3, 9, 4}, 10);
    check(e.ops.size() == 3 && e.ops[0].opType == "note.setVelocity" && e.ops[0].label == "Change Velocity" &&
              e.ops[0].payload == Payload({{"clip", 7}, {"note", std::uint64_t{3}}, {"vel", 110}}) &&
              e.ops[1].payload.at("vel") == 127 && e.ops[2].payload.at("vel") == 15,
          "velocities raised by 10, each within 1 to 127");
    check(changeVelocities(c, {4}, -50).ops[0].payload.at("vel") == 1, "and lowered, never under 1");
    check(changeVelocities(c, {3}, 0).ops.empty(), "no change writes nothing");
}

void activating() {
    auto c = clip();
    const auto off = toggleNotesActive(c, {3, 9});
    check(off.ops.size() == 2 && off.ops[0].opType == "note.setMute" && off.ops[0].label == "Deactivate Notes" &&
              off.ops[0].payload == Payload({{"clip", 7}, {"note", std::uint64_t{3}}, {"muted", true}}) &&
              off.select == std::vector<std::uint64_t>({3, 9}),
          "0 on two active notes deactivates both, still selected");
    c.notes[0].active = false;
    const auto mixed = toggleNotesActive(c, {3, 9});
    check(mixed.ops.size() == 1 && mixed.ops[0].payload.at("note") == 9 && mixed.ops[0].payload.at("muted") == true,
          "with one already off, only the other is deactivated");
    c.notes[1].active = false;
    const auto on = toggleNotesActive(c, {3, 9});
    check(on.ops.size() == 2 && on.ops[0].payload.at("muted") == false && on.ops[0].label == "Activate Notes",
          "with all of them off, 0 activates them");
    check(toggleNotesActive(c, {4}).ops[0].label == "Deactivate Note" && toggleNotesActive(c, {}).ops.empty(),
          "one note is named in the singular; none selected does nothing");
}

void refusals() {
    auto audio = clip();
    audio.kind = "audio";
    auto broken = clip();
    broken.notesProblem = "duplicate note ids";
    for (const auto &c : {audio, broken}) {
        check(!addNote(c, 0, beat, 60).refusal.empty() && addNote(c, 0, beat, 60).ops.empty() &&
                  !deleteNotes(c, {3}).refusal.empty() && !moveNotes(c, {3}, beat, 0).refusal.empty() &&
                  !resizeNotes(c, {3}, beat).refusal.empty() && !trimNotes(c, {3}, 1).refusal.empty() &&
                  !changeVelocities(c, {3}, 1).refusal.empty() && !toggleNotesActive(c, {3}).refusal.empty(),
              c.kind == "audio" ? "an audio clip has no notes to edit" : "a clip whose notes could not be read refuses every edit");
    }
    check(deleteNotes(broken, {3}).refusal.find("duplicate note ids") != std::string::npos, "and says what is wrong with them");
}

void clipFields() {
    const auto bar = textproj::kWhole;
    ClipDetails c;
    c.id = 7;
    c.kind = "midi";
    c.lengthTicks = 2 * bar;
    check(endMarkerOf(c) == 2 * bar && loopLengthOf(c) == 2 * bar, "with no End marker or loop of its own, the clip's length gives both");
    const auto start = setClipStart(c, beat);
    check(start.ops.size() == 1 && start.ops[0].opType == "clip.setStartMarker" && start.ops[0].label == "Set Clip Start" &&
              start.ops[0].payload == Payload({{"id", 7}, {"ticks", beat}}),
          "Start: one clip.setStartMarker");
    check(setClipStart(c, 0).ops.empty() && setClipStart(c, 0).refusal.empty(), "Start where it is writes nothing");
    check(!setClipStart(c, 2 * bar).refusal.empty() && !setClipStart(c, -1).refusal.empty(),
          "Start at or past the End, or before the beginning, is refused");
    const auto end = setClipEnd(c, bar);
    check(end.ops.size() == 1 && end.ops[0].opType == "clip.setEndMarker" && end.ops[0].payload == Payload({{"id", 7}, {"ticks", bar}}) &&
              end.ops[0].label == "Set Clip End",
          "End: one clip.setEndMarker");
    check(!setClipEnd(c, 0).refusal.empty() && setClipEnd(c, 2 * bar).ops.empty(), "End at the Start is refused; where it is, nothing");

    // The Loop switch: a clip that never had a window gets Start to End; one that has keeps it.
    const auto on = toggleClipLoop(c);
    check(on.ops.size() == 1 && on.ops[0].label == "Activate Loop" &&
              on.ops[0].payload == Payload({{"id", 7}, {"on", true}, {"start", 0}, {"length", 2 * bar}}),
          "the Loop switch on a clip with no loop window: on, from Start to End");
    c.loopStartTicks = beat;
    c.loopLengthTicks = bar;
    check(toggleClipLoop(c).ops[0].payload == Payload({{"id", 7}, {"on", true}}), "with a window already, only the switch");
    c.loopEnabled = true;
    const auto off = toggleClipLoop(c);
    check(off.ops[0].payload == Payload({{"id", 7}, {"on", false}}) && off.ops[0].label == "Deactivate Loop", "and off, the same");
    const auto moved = setClipLoop(c, 2 * beat, bar);
    check(moved.ops.size() == 1 && moved.ops[0].label == "Set Loop Position" &&
              moved.ops[0].payload == Payload({{"id", 7}, {"on", true}, {"start", 2 * beat}, {"length", bar}}),
          "the loop's Position: the window moved, the switch as it is");
    check(setClipLoop(c, beat, 2 * bar).ops[0].label == "Set Loop Length" && setClipLoop(c, beat, bar).ops.empty(),
          "its Length; and the same window writes nothing");
    check(!setClipLoop(c, -1, bar).refusal.empty() && !setClipLoop(c, 0, 0).refusal.empty(), "a loop before the beginning, or with no length, is refused");

    const auto signature = setClipSignature(c, 3, 4);
    check(signature.ops.size() == 1 && signature.ops[0].opType == "clip.setTimeSignature" &&
              signature.ops[0].payload == Payload({{"id", 7}, {"num", 3}, {"den", 4}}) &&
              signature.ops[0].label == "Change Clip Time Signature",
          "the Signature: one clip.setTimeSignature");
    check(setClipSignature(c, 4, 4).ops.size() == 1, "4/4 on a clip that inherits is still the clip's own choice");
    c.signature.local = true;
    check(setClipSignature(c, 4, 4).ops.empty(), "and writes nothing once it is");
    check(!setClipSignature(c, 0, 4).refusal.empty() && !setClipSignature(c, 100, 4).refusal.empty() &&
              !setClipSignature(c, 3, 5).refusal.empty() && !setClipSignature(c, 3, 32).refusal.empty(),
          "1 to 99 over 1, 2, 4, 8 or 16; anything else is refused");

    // What is typed.
    check(parseBarsBeats("2.0.0") == BarsBeats{2, 0, 0} && parseBarsBeats("1. 2. 3") == BarsBeats{1, 2, 3} &&
              parseBarsBeats("4") == BarsBeats{4, 0, 0} && parseBarsBeats("0:1") == BarsBeats{0, 1, 0},
          "a length as bars, beats and sixteenths from zero, the later ones 0 when left out");
    check(!parseBarsBeats("") && !parseBarsBeats("two") && !parseBarsBeats("1.2.3.4") && !parseBarsBeats("1234567"),
          "nothing, words, a fourth part and an endless number are not lengths");
    const textproj::Meter four{0, 4, 4}, six{0, 6, 8};
    check(ticksOf(BarsBeats{1, 2, 1}, four) == bar + 2 * beat + beat / 4 && ticksOf(BarsBeats{1, 1, 0}, six) == 3 * beat + beat / 2,
          "a length in ticks, in the clip's meter");
    check(barsBeats(ticksOf(BarsBeats{3, 1, 2}, four), four) == BarsBeats{3, 1, 2}, "and back");
    check(parseSignature("3/4") == std::pair<std::int64_t, std::int64_t>{3, 4} &&
              parseSignature("6 8") == std::pair<std::int64_t, std::int64_t>{6, 8} &&
              parseSignature(" 12 / 16 ") == std::pair<std::int64_t, std::int64_t>{12, 16},
          "a signature as 3/4 or 6 8");
    check(!parseSignature("3") && !parseSignature("3/4/4") && !parseSignature("three four") && !parseSignature(""),
          "one number, three, or words are not signatures");
}

void midiClips() {
    engine::Snapshot snapshot;
    auto track = [](std::int64_t id, const char *kind, const char *name) {
        auto t = std::make_shared<engine::TrackNode>();
        t->id = id;
        t->kind = kind;
        t->name = name;
        return t;
    };
    auto keys = track(1, "midi", "Keys"), audio = track(2, "audio", "Audio"), bass = track(3, "midi", "Bass");
    auto existing = std::make_shared<engine::ClipNode>();
    existing->id = 40;
    existing->posTicks = 4 * beat;
    existing->lengthTicks = 4 * beat;
    bass->clips.push_back(existing);
    snapshot.tracks = {keys, audio, bass};
    const auto one = insertMidiClips(snapshot, {1}, 0, 4 * beat);
    check(one.refusal.empty() && one.ops.size() == 1 && one.ops[0].opType == "clip.create" &&
              one.ops[0].label == "Insert MIDI Clip" &&
              one.ops[0].payload == Payload({{"id", 41}, {"track", 1}, {"kind", "midi"}, {"pos", 0}, {"length", 4 * beat}}) &&
              one.select == 1 && one.clip == 41,
          "an empty MIDI clip across the selection, with an id past every clip's, selected");
    const auto both = insertMidiClips(snapshot, {1, 2, 3}, 0, 4 * beat);
    check(both.ops.size() == 2 && both.ops[0].payload.at("track") == 1 && both.ops[1].payload.at("track") == 3 &&
              both.ops[1].payload.at("id") == 42 && both.clip == 41,
          "on several tracks: one on each MIDI track, none on the audio track, the first selected");
    check(!insertMidiClips(snapshot, {2}, 0, beat).refusal.empty() && insertMidiClips(snapshot, {2}, 0, beat).ops.empty(),
          "with no MIDI track it is refused");
    check(!insertMidiClips(snapshot, {1}, beat, beat).refusal.empty(), "and with no time selected");
    const auto over = insertMidiClips(snapshot, {1, 3}, 2 * beat, 6 * beat);
    check(over.ops.empty() && over.refusal.find("Bass") != std::string::npos,
          "over an existing clip nothing is made on any track, and the track is named");
    check(insertMidiClips(snapshot, {3}, 8 * beat, 9 * beat).refusal.empty(), "right after a clip is free");
}
} // namespace

int main() {
    adding();
    deleting();
    moving();
    resizing();
    velocities();
    activating();
    refusals();
    clipFields();
    midiClips();
    std::printf("%s -- %d checks, %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
