// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Session View's model and commands, headless (TM-SV-05). A model test for every state a slot,
// a scene and a track can show; a command test for every command and key; and every edit committed
// on a real project, where one Undo must put back exactly what the command changed.
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/engine/launch_ops.hpp"
#include "adi/engine/launch_store.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "adi/ui/session_view_commands.hpp"
#include "adi/ui/session_view_edits.hpp"
#include "adi/ui/session_view_model.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
bool counting = false;
std::size_t allocations = 0;
} // namespace
void adi::test::countReplacedNew() noexcept {
    if (counting)
        ++allocations;
}

namespace {
using namespace adi;
using namespace adi::ui::session_view;
using engine::LaunchQueuedAction;
using engine::LaunchStatusHeader;
using engine::LaunchTrackSource;
using engine::LaunchTrackStatus;
using engine::launch::SlotKey;
using engine::launch::SlotRecordingState;

int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

// The grid every pure test uses. Columns: G (group) holding A and B, then C (armed), D, R (return).
// Scenes 10 "Intro" (tempo 128 on), 20, 30, 40 (empty everywhere).
constexpr std::int64_t G = 1, A = 2, B = 3, C = 4, D = 6, R = 5;
enum Col : std::size_t { cG, cA, cB, cC, cD, cR };

ModelInput grid() {
    ModelInput in;
    // A, B and C take MIDI clips; D is an audio track.
    in.tracks = {{G, ColumnKind::Group, 0, "Group", 3, false, false}, {A, ColumnKind::Clips, G, "Drums", 5, false, true},
                 {B, ColumnKind::Clips, G, "Bass", 7, false, true},   {C, ColumnKind::Clips, 0, "Keys", {}, true, true},
                 {D, ColumnKind::Clips, 0, "Pads", {}, false, false}, {R, ColumnKind::Return, 0, "Reverb", {}, false, false}};
    engine::launch::Scene intro;
    intro.id = 10;
    intro.order = 0;
    intro.name = "Intro";
    intro.tempoBpm = 128;
    intro.tempoEnabled = true;
    engine::launch::Scene verse;
    verse.id = 20;
    verse.order = 1;
    verse.signatureNumerator = 7;
    verse.signatureDenominator = 8;
    verse.signatureEnabled = true;
    engine::launch::Scene chorus;
    chorus.id = 30;
    chorus.order = 2;
    chorus.follow.enabled = true;
    engine::launch::Scene outro;
    outro.id = 40;
    outro.order = 3;
    in.scenes = {intro, verse, chorus, outro};
    in.slots = {{A, 10, 100, true}, {B, 10, 101, true}, {A, 20, 102, true}, {C, 20, 0, false},
                {D, 20, 0, false},  {B, 30, 103, true}, {R, 10, 0, true}};
    engine::launch::FollowSettings again;
    again.enabled = true;
    again.first = {engine::FollowAction::Again, std::nullopt};
    again.loopCount = 2;
    in.clips = {{100, "Beat", 5, false, engine::LaunchMode::Trigger, false, true, engine::launch::FollowSettings{}},
                {101, "Line", 9, false, engine::LaunchMode::Trigger, true, true, again},
                {102, "Fill", {}, true, engine::LaunchMode::Toggle, false, true, engine::launch::FollowSettings{}},
                {103, "Hold", 11, false, engine::LaunchMode::Gate, false, true, engine::launch::FollowSettings{}}};
    return in;
}

Model built() {
    Model m;
    m.rebuild(grid());
    return m;
}

LaunchTrackStatus playing(std::int64_t track, std::int64_t clip, std::int64_t scene) {
    LaunchTrackStatus s;
    s.track = track;
    s.clip = clip;
    s.scene = scene;
    s.source = LaunchTrackSource::Session;
    s.run = 1;
    return s;
}

LaunchStatusHeader header(bool back = false, bool rolling = true) {
    LaunchStatusHeader h;
    h.incarnation = 7;
    h.transportPlaying = rolling;
    h.backToArrangement = back;
    return h;
}

void layout() {
    const auto m = built();
    check(m.columnCount() == 6 && m.sceneCount() == 4, "the grid has every track and scene");
    check(m.column(cG).kind == ColumnKind::Group && m.column(cR).kind == ColumnKind::Return &&
              m.column(cC).armed && m.column(cA).name == "Drums",
          "each column keeps its kind, name and arm");
    check(m.scene(0).number == 1 && m.scene(3).number == 4 && m.scene(0).name == "Intro",
          "scenes are numbered by place, as Live shows them");
    check(m.scene(0).tempoEnabled && m.scene(0).tempoBpm == 128 && !m.scene(0).signatureEnabled &&
              m.scene(1).signatureEnabled && m.scene(1).signatureNumerator == 7 && m.scene(1).signatureDenominator == 8,
          "the scene column carries each scene's tempo and signature with their enables");
    check(m.scene(2).follow && !m.scene(0).follow, "a scene's Follow Action shows");
    check(m.scene(0).occupied == 2 && m.scene(1).occupied == 1 && m.scene(2).occupied == 1 && m.scene(3).occupied == 0,
          "each scene counts the clip tracks holding a clip in it");
    check(m.slot(cA, 0).clip == 100 && m.slot(cA, 0).name == "Beat" && m.slot(cA, 0).color == 5,
          "a slot shows its clip's name and colour");
    check(m.slot(cB, 0).follow && m.slot(cA, 1).deactivated && m.slot(cB, 2).mode == engine::LaunchMode::Gate,
          "a slot carries Follow Actions, Deactivate and its launch mode");
    check(!m.slot(cC, 1).hasStop && m.slot(cC, 0).hasStop && m.slot(cD, 3).hasStop,
          "an empty slot has its Stop button unless one was removed");
    check(m.slot(cR, 0).clip == 0 && m.slotState(cR, 0) == SlotState::Empty, "a return column holds no slot clips");
    const auto held = m.heldColumns(cG);
    check(held.size() == 2 && held[0] == cA && held[1] == cB && m.heldColumns(cA).empty(),
          "a group stands for the tracks it holds");
    check(m.slot(cG, 0).groupOccupied && m.slot(cG, 0).color == 5 && m.slot(cG, 0).clip == 0 && !m.slot(cG, 0).hasStop,
          "a group slot is shaded in its left-most clip's colour (7.1)");
    check(m.slot(cG, 2).groupOccupied && m.slot(cG, 2).color == 11, "a group slot follows its held tracks");
    check(!m.slot(cG, 3).groupOccupied && m.slot(cG, 3).hasStop && m.slotState(cG, 3) == SlotState::Empty,
          "a group slot with no clips is a Stop button");
    check(m.slotState(cA, 0) == SlotState::Stopped && m.slotState(cG, 0) == SlotState::Stopped &&
              m.slotState(cD, 0) == SlotState::Empty,
          "before any frame, a clip rests and an empty slot is empty");
    check(m.columnOf(D) == cD && !m.columnOf(99) && m.rowOf(30) == 2u && !m.rowOf(99), "identities resolve to places");
}

void slotStates() {
    auto m = built();
    const auto revision = m.liveRevision();
    std::array rows{playing(A, 100, 10)};
    check(m.applyStatus(rows, header()), "a launched clip changes the grid");
    check(m.slotState(cA, 0) == SlotState::Playing && m.trackLive(cA).source == LaunchTrackSource::Session &&
              m.trackLive(cA).playingClip == 100 && m.trackLive(cA).playingScene == 10,
          "playing: the slot and the Track Status");
    check(m.sceneLive(0).playing == 1 && m.sceneLive(1).playing == 0, "the scene counts its playing tracks");
    check(m.slotState(cG, 0) == SlotState::Playing && m.trackLive(cG).source == LaunchTrackSource::Session,
          "a group slot plays while a held clip plays");
    check(m.liveRevision() == revision + 1 && !m.applyStatus(rows, header()) && m.liveRevision() == revision + 1,
          "an unchanged frame changes nothing");

    auto queued = playing(B, 0, 0);
    queued.source = LaunchTrackSource::Arrangement;
    queued.queued = {LaunchQueuedAction::Start, 101, 10, 3840, std::nullopt};
    std::array two{playing(A, 100, 10), queued};
    (void)m.applyStatus(two, header());
    check(m.slotState(cB, 0) == SlotState::QueuedStart && m.trackLive(cB).queued == LaunchQueuedAction::Start,
          "queued to start: the slot waits for its boundary");

    auto stopping = playing(A, 100, 10);
    stopping.queued = {LaunchQueuedAction::Stop, 100, 10, 3840, std::nullopt};
    std::array stop{stopping};
    (void)m.applyStatus(stop, header());
    check(m.slotState(cA, 0) == SlotState::QueuedStop && m.trackLive(cA).stopQueued && m.trackLive(cG).stopQueued,
          "queued to stop: the clip and its track's Stop button wait");

    auto trackStop = playing(A, 100, 10);
    trackStop.queued = {LaunchQueuedAction::Stop, 0, 20, 3840, std::nullopt};
    std::array scene{trackStop};
    (void)m.applyStatus(scene, header());
    check(m.slotState(cA, 0) == SlotState::QueuedStop && m.slotState(cA, 1) == SlotState::Stopped,
          "a track's Stop from an empty slot in a launched scene stops the clip playing, not the slot");

    auto relaunch = playing(A, 100, 10);
    relaunch.queued = {LaunchQueuedAction::Start, 102, 20, 3840, std::nullopt};
    std::array other{relaunch};
    (void)m.applyStatus(other, header());
    check(m.slotState(cA, 0) == SlotState::Playing && m.slotState(cA, 1) == SlotState::QueuedStart,
          "one track can play one slot while another waits to start");

    auto back = playing(A, 100, 10);
    back.queued = {LaunchQueuedAction::BackToArrangement, 0, 0, 3840, std::nullopt};
    std::array backRows{back};
    (void)m.applyStatus(backRows, header(true));
    check(m.trackLive(cA).queued == LaunchQueuedAction::BackToArrangement && m.slotState(cA, 0) == SlotState::Playing &&
              m.backToArrangementLit() && m.transportPlaying(),
          "Back to Arrangement lights and waits for its boundary");

    LaunchTrackStatus silent;
    silent.track = A;
    silent.source = LaunchTrackSource::Silence;
    std::array silence{silent, playing(99, 5, 10)};
    (void)m.applyStatus(silence, header(true, false));
    check(m.trackLive(cA).source == LaunchTrackSource::Silence && m.slotState(cA, 0) == SlotState::Stopped &&
              !m.transportPlaying() && m.sceneLive(0).playing == 0,
          "a stopped track is silent and its clip rests; a row for a track not shown is ignored");

    auto loop = playing(B, 101, 10);
    loop.completedLoops = 3;
    loop.loopLengthTicks = 4 * 5765760;
    auto shot = playing(A, 102, 20);
    shot.remainingSamples = 48000;
    std::array status{shot, loop};
    (void)m.applyStatus(status, header());
    check(m.trackLive(cB).completedLoops == 3 && m.trackLive(cB).loopLengthTicks == 4 * 5765760 &&
              m.trackLive(cA).remainingSamples == 48000,
          "the Track Status carries a loop's count and length and a one-shot's time left (7.3)");

    (void)m.applyStatus(status, header(), 30);
    check(m.pendingScene() == 30 && m.sceneLive(2).triggered && !m.sceneLive(1).triggered,
          "a queued scene launch shows on its scene");
    (void)m.applyStatus(status, header());
    check(!m.sceneLive(2).triggered, "the scene stops waiting once it launched or was cancelled");

    std::array armed{SlotKey{C, 30}};
    check(m.applyRecording(SlotRecordingState::Armed, armed) && m.slotState(cC, 2) == SlotState::QueuedRecord,
          "an armed take waits to record");
    check(m.applyRecording(SlotRecordingState::Recording, armed) && m.slotState(cC, 2) == SlotState::Recording,
          "a take records into its slot");
    check(m.applyRecording(SlotRecordingState::Complete, armed) && m.slotState(cC, 2) == SlotState::Empty,
          "a finished take leaves the slot to the project's next rebuild");
    std::array onGroup{SlotKey{R, 10}};
    check(!m.applyRecording(SlotRecordingState::Recording, onGroup), "a return cannot record a clip");
}

void allocationFree() {
    auto m = built();
    std::array<LaunchTrackStatus, 3> a{playing(A, 100, 10), playing(B, 101, 10), playing(C, 0, 0)};
    auto b = a;
    b[0].queued = {LaunchQueuedAction::Stop, 100, 10, 960, std::nullopt};
    b[1] = playing(B, 103, 30);
    std::array keys{SlotKey{C, 30}, SlotKey{D, 40}};
    (void)m.applyStatus(a, header());
    allocations = 0;
    counting = true;
    for (int frame = 0; frame < 200; ++frame) {
        (void)m.applyStatus(frame % 2 == 0 ? std::span<const LaunchTrackStatus>(b) : std::span<const LaunchTrackStatus>(a),
                            header(frame % 3 == 0), frame % 4 == 0 ? std::optional<std::int64_t>(20) : std::nullopt);
        (void)m.applyRecording(frame % 2 == 0 ? SlotRecordingState::Recording : SlotRecordingState::Armed, keys);
        (void)m.slotState(cG, 2);
        (void)m.slotSelected(cA, 0);
    }
    counting = false;
    check(allocations == 0, "a frame of the model allocates nothing");
}

void selection() {
    auto m = built();
    Selection s;
    s.focus = Focus::Slot;
    s.track = B;
    s.scene = 30;
    s.anchorTrack = A;
    s.anchorScene = 10;
    m.select(s);
    check(m.slotSelected(cA, 0) && m.slotSelected(cB, 2) && m.slotSelected(cA, 1) && !m.slotSelected(cC, 0) &&
              !m.slotSelected(cA, 3) && !m.sceneSelected(0),
          "a slot selection is the rectangle from its anchor to its focus");
    const auto revision = m.selectionRevision();
    m.select(s);
    check(m.selectionRevision() == revision, "the same selection is not news");
    Selection onReturn;
    onReturn.focus = Focus::Slot;
    onReturn.track = R;
    onReturn.scene = 10;
    m.select(onReturn);
    check(m.selection().focus == Focus::None, "a return has no slot to select");
    Selection scenes;
    scenes.focus = Focus::Scene;
    scenes.scene = 30;
    scenes.anchorScene = 20;
    m.select(scenes);
    check(m.sceneSelected(1) && m.sceneSelected(2) && !m.sceneSelected(0) && !m.slotSelected(cA, 1),
          "a scene selection is a run of scenes");
    Selection titles;
    titles.focus = Focus::Track;
    titles.track = C;
    m.select(titles);
    check(m.trackSelected(cC) && !m.trackSelected(cD) && m.selection().anchorTrack == C, "a track title selects alone");

    // A rebuild keeps the selection and the live frame where they still stand.
    Selection one;
    one.focus = Focus::Slot;
    one.track = A;
    one.scene = 10;
    m.select(one);
    std::array rows{playing(A, 100, 10)};
    (void)m.applyStatus(rows, header());
    auto in = grid();
    in.scenes.erase(in.scenes.begin() + 1);
    m.rebuild(in);
    check(m.sceneCount() == 3 && m.selection() == Selection{Focus::Slot, A, 10, A, 10, {}} &&
              m.slotState(cA, 0) == SlotState::Playing && m.scene(1).number == 2,
          "a rebuild keeps the selection, the frame and renumbers the scenes");
    auto gone = grid();
    gone.scenes.erase(gone.scenes.begin());
    m.rebuild(gone);
    check(m.selection().focus == Focus::None, "a selection whose scene went selects nothing");
}

/// Applies a click's selection to the model, as the window does.
void clickOn(Model& m, Target what, std::size_t column, std::size_t row, bool command = false, bool shift = false) {
    const auto out = click(m, {what, column, row, command, shift});
    if (out.selection)
        m.select(*out.selection);
}

void picks() {
    auto m = built();
    clickOn(m, Target::Slot, cA, 0);
    clickOn(m, Target::Slot, cB, 2, true);
    check(m.slotSelected(cA, 0) && m.slotSelected(cB, 2) && !m.slotSelected(cA, 1) && !m.slotSelected(cB, 0) &&
              m.selection().picks.size() == 2 && m.selection().track == B && m.selection().scene == 30,
          "Ctrl+click picks nonadjacent clips (7.1), the focus on the last");
    const auto box = m.selectedRange();
    check(box && box->c0 == cA && box->c1 == cB && box->r0 == 0 && box->r1 == 2, "the picks' box holds them all");
    allocations = 0;
    counting = true;
    for (std::size_t c = 0; c < m.columnCount(); ++c)
        for (std::size_t r = 0; r < m.sceneCount(); ++r)
            (void)m.slotSelected(c, r);
    counting = false;
    check(allocations == 0, "asking what is picked allocates nothing");
    clickOn(m, Target::Slot, cA, 0, true);
    check(!m.slotSelected(cA, 0) && m.slotSelected(cB, 2) && m.selection().picks.size() == 1,
          "Ctrl+click on a picked clip takes it out");
    clickOn(m, Target::Slot, cB, 2, true);
    check(m.selection().focus == Focus::None, "taking out the last pick selects nothing");

    clickOn(m, Target::Slot, cA, 0);
    clickOn(m, Target::Slot, cB, 1, false, true);
    check(m.slotSelected(cA, 1) && m.slotSelected(cB, 0) && m.selection().picks.empty() && m.selection().anchorTrack == A,
          "Shift+click selects the rectangle from the anchor (41.5)");
    clickOn(m, Target::Slot, cC, 3, true);
    check(m.slotSelected(cA, 0) && m.slotSelected(cB, 1) && m.slotSelected(cC, 3) && !m.slotSelected(cC, 0) &&
              m.selection().picks.size() == 5,
          "Ctrl+click after a rectangle keeps the rectangle's slots and adds the one clicked");
    clickOn(m, Target::Slot, cD, 0);
    check(m.selection() == Selection{Focus::Slot, D, 10, D, 10, {}}, "a plain click selects one slot again");
    clickOn(m, Target::Slot, cR, 0, true);
    check(m.selection() == Selection{Focus::Slot, D, 10, D, 10, {}}, "a return's empty place takes no click");

    clickOn(m, Target::Scene, 0, 0);
    clickOn(m, Target::Scene, 0, 2, true);
    check(m.sceneSelected(0) && !m.sceneSelected(1) && m.sceneSelected(2) && !m.slotSelected(cD, 0),
          "Ctrl+click picks nonadjacent scenes (7.2)");
    clickOn(m, Target::Slot, cA, 1, true);
    check(m.selection() == Selection{Focus::Slot, A, 20, A, 20, {}}, "Ctrl+click on another kind starts a new selection");
    clickOn(m, Target::Track, cA, 0);
    clickOn(m, Target::Track, cC, 0, true);
    check(m.trackSelected(cA) && !m.trackSelected(cB) && m.trackSelected(cC), "Ctrl+click picks track titles");

    // A rebuild keeps the picks that stand.
    clickOn(m, Target::Scene, 0, 0);
    clickOn(m, Target::Scene, 0, 1, true);
    clickOn(m, Target::Scene, 0, 3, true);
    auto in = grid();
    in.scenes.erase(in.scenes.begin() + 1);
    m.rebuild(in);
    check(m.selection().picks == std::vector<Pick>{{0, 10}, {0, 40}} && m.sceneSelected(0) && m.sceneSelected(2) &&
              !m.sceneSelected(1),
          "a rebuild drops a pick whose scene went and keeps the rest");

    const auto band = rubberBand(built(), cD, 3, cA, 1);
    check(band.selection == Selection{Focus::Slot, A, 20, D, 40, {}}, "the rubber band selects from where it began");
    check(!rubberBand(built(), cR, 0, cA, 1).selection, "a rubber band starts on a slot");
}

// --- commands --------------------------------------------------------------------------------------

Context ctx() {
    Context c;
    c.incarnation = 7;
    return c;
}

Model selected(Selection s) {
    auto m = built();
    if (s.anchorTrack == 0)
        s.anchorTrack = s.track;
    if (s.anchorScene == 0)
        s.anchorScene = s.scene;
    m.select(s);
    return m;
}
Model slotSelected(std::int64_t track, std::int64_t scene) { return selected({Focus::Slot, track, scene, 0, 0, {}}); }
Model sceneSelected(std::int64_t scene) { return selected({Focus::Scene, 0, scene, 0, 0, {}}); }

bool launchesValid(const Outcome& out) {
    for (const auto& request : out.launches) {
        const auto descriptors = engine::launchOperationDescriptors();
        const auto found = std::find_if(descriptors.begin(), descriptors.end(),
                                        [&](const OpDescriptor& d) { return d.name == request.opType; });
        if (found == descriptors.end() || !validateSubmission(*found, request.payload).empty())
            return false;
    }
    return true;
}

bool editsValid(const Outcome& out) {
    for (const auto& request : out.edits) {
        const auto* descriptor = OpRegistry::instance().find(request.opType);
        if (descriptor == nullptr || !validateSubmission(*descriptor, request.payload).empty())
            return false;
    }
    return true;
}

KeyChord key(Key k, bool command = false, bool shift = false) { return {k, 0, command, shift, false}; }
KeyChord chr(char32_t c, bool command = false, bool shift = false) { return {Key::Character, c, command, shift, false}; }

void keys() {
    const auto S = MainView::Session, Arr = MainView::Arrangement;
    check(commandForKey(key(Key::Tab), Arr) == Command::ToggleView && commandForKey(key(Key::Tab), S) == Command::ToggleView,
          "Tab switches between the Arrangement and the Session View (41.1)");
    check(!commandForKey(key(Key::Tab), S, {true}), "with Use Tab to Move Focus on, Tab moves focus instead (41.26.1)");
    check(!commandForKey(key(Key::Tab, false, true), S), "Shift+Tab is the Detail View's");
    check(commandForKey(key(Key::F3), Arr) == Command::ToggleView, "F3 switches views as the plan names it");
    check(!commandForKey(key(Key::Enter), Arr) && !commandForKey(chr(U'i', true), Arr),
          "in the Arrangement the Session's keys are not the Session's");
    const std::array<std::pair<KeyChord, Command>, 33> table{{
        {key(Key::Enter, false, true), Command::ToggleFollowActions},
        {key(Key::Enter, true, true), Command::CreateFollowActionChain},
        {chr(U'm', true, true), Command::InsertMidiClip},
        {chr(U'd', true), Command::Duplicate},
        {chr(U'c', true), Command::Copy},
        {chr(U'x', true), Command::Cut},
        {chr(U'v', true), Command::Paste},
        {key(Key::Enter), Command::LaunchSelected},
        {key(Key::Enter, true), Command::StopSelectedTracks},
        {key(Key::Up), Command::SelectUp},
        {key(Key::Down), Command::SelectDown},
        {key(Key::Left), Command::SelectLeft},
        {key(Key::Right), Command::SelectRight},
        {key(Key::Up, false, true), Command::ExtendUp},
        {key(Key::Down, false, true), Command::ExtendDown},
        {key(Key::Left, false, true), Command::ExtendLeft},
        {key(Key::Right, false, true), Command::ExtendRight},
        {key(Key::Up, true), Command::MoveScenesUp},
        {key(Key::Down, true), Command::MoveScenesDown},
        {key(Key::PageUp), Command::PageUp},
        {key(Key::PageDown), Command::PageDown},
        {key(Key::Home), Command::FirstTrack},
        {key(Key::End), Command::LastTrack},
        {key(Key::Escape), Command::TrackTitle},
        {key(Key::Delete), Command::Delete},
        {key(Key::Backspace), Command::Delete},
        {chr(U'0'), Command::Deactivate},
        {chr(U'a', true), Command::SelectAll},
        {chr(U'i', true), Command::InsertScene},
        {chr(U'i', true, true), Command::CaptureAndInsertScene},
        {chr(U'e', true), Command::AddRemoveStopButton},
        {chr(U'r', true), Command::Rename},
        {key(Key::Tab), Command::ToggleView},
    }};
    bool all = true;
    for (const auto& [chord, command] : table)
        all = all && commandForKey(chord, S) == command;
    check(all, "every Session View key of 41.15 maps to its command");
    check(!commandForKey(key(Key::Left, true), S) && !commandForKey(chr(U'q', true), S) && !commandForKey(chr(U'1'), S) &&
              !commandForKey(chr(U'm', true), S),
          "keys the Session View does not own pass by");
    check(commandForKeyRelease(key(Key::Enter), S) == Command::ReleaseSelected && !commandForKeyRelease(key(Key::Enter), Arr),
          "letting Enter go is a release edge");
}

void viewSwitch() {
    const auto m = built();
    auto c = ctx();
    c.view = MainView::Arrangement;
    const auto toSession = run(Command::ToggleView, m, c);
    c.view = MainView::Session;
    const auto toArrangement = run(Command::ToggleView, m, c);
    check(toSession.view == MainView::Session && toArrangement.view == MainView::Arrangement &&
              toSession.edits.empty() && toSession.launches.empty(),
          "the view switch is the window's own, never an op");
    c.view = MainView::Arrangement;
    const auto ignored = run(Command::LaunchSelected, slotSelected(A, 10), c);
    check(ignored.launches.empty() && !ignored.selection, "the Session's commands wait for the Session View");
}

void launching() {
    auto m = slotSelected(A, 10);
    const auto out = run(Command::LaunchSelected, m, ctx());
    check(out.launches.size() == 1 && out.launches[0].opType == "session.launchClip" &&
              out.launches[0].payload.at("track") == A && out.launches[0].payload.at("scene") == 10 &&
              out.launches[0].payload.at("clip") == 100 && out.launches[0].payload.at("incarnation") == 7u &&
              out.edits.empty() && launchesValid(out),
          "Enter launches the selected clip, stamped with the Session's incarnation, outside Undo");
    check(out.selection == Selection{Focus::Slot, A, 10, A, 10, {}}, "Select on Launch keeps the clip selected (7.4.1)");
    auto none = ctx();
    none.incarnation = 0;
    const auto early = run(Command::LaunchSelected, m, none);
    check(early.launches.empty() && !early.refusal.empty(), "with nothing published, a launch is refused by name");

    const auto stop = launchSlot(built(), cD, 0, ctx());
    check(stop.launches.size() == 1 && stop.launches[0].opType == "session.stopTrack" &&
              stop.launches[0].payload.at("track") == D && launchesValid(stop),
          "an empty slot's square stops its track");
    check(launchSlot(built(), cD, 1, ctx()).launches.empty(), "an empty slot without its Stop button does nothing");
    const auto record = launchSlot(built(), cC, 0, ctx());
    check(record.recordInto == SlotKey{C, 10} && record.launches.empty(),
          "an armed track's empty slot records into it (Session Record)");
    check(launchSlot(built(), cR, 0, ctx()).launches.empty(), "a return has nothing to launch");

    const auto group = launchSlot(built(), cG, 0, ctx());
    check(group.launches.size() == 2 && group.launches[0].payload.at("clip") == 100 &&
              group.launches[1].payload.at("clip") == 101 && launchesValid(group),
          "a group slot launches every clip it refers to (7.1)");
    const auto partly = launchSlot(built(), cG, 1, ctx());
    check(partly.launches.size() == 2 && partly.launches[0].opType == "session.launchClip" &&
              partly.launches[1].opType == "session.stopTrack" && partly.launches[1].payload.at("track") == B,
          "a group launch stops the held tracks it has no clip for");
    const auto groupStop = launchSlot(built(), cG, 3, ctx());
    check(groupStop.launches.size() == 2 && groupStop.launches[0].opType == "session.stopTrack" &&
              groupStop.launches[1].opType == "session.stopTrack",
          "a group slot with no clips is its tracks' Stop button");

    const auto release = run(Command::ReleaseSelected, slotSelected(B, 30), ctx());
    check(release.launches.size() == 1 && release.launches[0].payload.at("release") == true &&
              release.launches[0].payload.at("clip") == 103 && launchesValid(release),
          "letting Enter go sends the release edge Gate needs");

    const auto scene = run(Command::LaunchSelected, sceneSelected(10), ctx());
    check(scene.launches.size() == 1 && scene.launches[0].opType == "session.launchScene" &&
              scene.launches[0].payload.at("scene") == 10 && launchesValid(scene) &&
              scene.selection == Selection{Focus::Scene, 0, 20, 0, 20, {}},
          "Enter on a scene launches it and selects the next (7.2)");
    auto stay = ctx();
    stay.selectNextSceneOnLaunch = false;
    check(run(Command::LaunchSelected, sceneSelected(10), stay).selection == Selection{Focus::Scene, 0, 10, 0, 10, {}},
          "with Select Next Scene on Launch off, the scene stays selected");
    check(run(Command::LaunchSelected, sceneSelected(40), ctx()).selection == Selection{Focus::Scene, 0, 40, 0, 40, {}},
          "the last scene has no next");

    const auto stops = run(Command::StopSelectedTracks, selected({Focus::Slot, B, 20, G, 10, {}}), ctx());
    check(stops.launches.size() == 2 && stops.launches[0].payload.at("track") == A &&
              stops.launches[1].payload.at("track") == B && launchesValid(stops),
          "Ctrl+Enter stops each track of the slot selection once");
    const auto all = run(Command::StopAll, built(), ctx());
    const auto back = run(Command::BackToArrangement, built(), ctx());
    check(all.launches.size() == 1 && all.launches[0].opType == "session.stopAll" && launchesValid(all) &&
              back.launches.size() == 1 && back.launches[0].opType == "session.backToArrangement" && launchesValid(back),
          "Stop All Clips and Back to Arrangement are one gesture each");
    auto pending = built();
    std::array<LaunchTrackStatus, 0> noRows{};
    (void)pending.applyStatus(noRows, header(), 30);
    const auto cancel = run(Command::CancelSceneLaunch, pending, ctx());
    check(cancel.launches.size() == 1 && cancel.launches[0].opType == "session.cancelSceneLaunch" &&
              cancel.launches[0].payload.at("scene") == 30 && launchesValid(cancel),
          "Cancel Scene Launch cancels the queued scene");
    check(run(Command::CancelSceneLaunch, built(), ctx()).launches.empty(), "with no scene queued there is none to cancel");
}

Selection after(const Model& m, Command c) {
    const auto out = run(c, m, ctx());
    return out.selection ? *out.selection : m.selection();
}

void navigation() {
    check(after(built(), Command::SelectDown) == Selection{Focus::Slot, G, 10, G, 10, {}}, "an arrow with nothing selected starts at the top left");
    check(after(slotSelected(A, 10), Command::SelectDown) == Selection{Focus::Slot, A, 20, A, 20, {}}, "Down moves one scene");
    check(after(slotSelected(A, 10), Command::SelectUp) == Selection{Focus::Slot, A, 10, A, 10, {}}, "Up stops at the first scene");
    check(after(slotSelected(A, 10), Command::SelectRight) == Selection{Focus::Slot, B, 10, B, 10, {}}, "Right moves one track");
    check(after(slotSelected(G, 10), Command::SelectLeft) == Selection{Focus::Slot, G, 10, G, 10, {}}, "Left stops at the first track");
    check(after(slotSelected(D, 20), Command::SelectRight) == Selection{Focus::Scene, 0, 20, 0, 20, {}},
          "Right from the last track reaches the scene (7.2.1)");
    check(after(sceneSelected(20), Command::SelectLeft) == Selection{Focus::Slot, D, 20, D, 20, {}},
          "Left from a scene comes back to the last track");
    check(after(sceneSelected(20), Command::SelectDown) == Selection{Focus::Scene, 0, 30, 0, 30, {}}, "Down moves between scenes");
    check(after(slotSelected(A, 10), Command::PageDown) == Selection{Focus::Slot, A, 40, A, 40, {}}, "Page Down jumps eight scenes, to the last");
    check(after(slotSelected(A, 40), Command::PageUp) == Selection{Focus::Slot, A, 10, A, 10, {}}, "Page Up jumps back");
    check(after(slotSelected(B, 20), Command::FirstTrack) == Selection{Focus::Slot, G, 20, G, 20, {}}, "Home: the first track of the scene");
    check(after(slotSelected(B, 20), Command::LastTrack) == Selection{Focus::Slot, D, 20, D, 20, {}}, "End: the last track of the scene");
    check(after(slotSelected(D, 20), Command::LastTrack) == Selection{Focus::Scene, 0, 20, 0, 20, {}},
          "End from the last track: the current scene (41.15)");
    check(after(slotSelected(B, 20), Command::TrackTitle) == Selection{Focus::Track, B, 0, B, 0, {}}, "Esc: the track's title bar");
    check(after(selected({Focus::Track, B, 0, 0, 0, {}}), Command::SelectDown) == Selection{Focus::Slot, B, 10, B, 10, {}},
          "Down from a title enters its slots");
    check(after(selected({Focus::Track, B, 0, 0, 0, {}}), Command::SelectRight) == Selection{Focus::Track, C, 0, C, 0, {}},
          "titles move between tracks");
    check(after(slotSelected(A, 10), Command::ExtendDown) == Selection{Focus::Slot, A, 20, A, 10, {}} &&
              after(slotSelected(A, 10), Command::ExtendRight) == Selection{Focus::Slot, B, 10, A, 10, {}},
          "Shift+arrows extend the selection from its anchor");
    check(after(slotSelected(D, 10), Command::ExtendRight) == Selection{Focus::Slot, D, 10, D, 10, {}},
          "a slot range does not reach into the scene column");
    check(after(slotSelected(B, 20), Command::SelectAll) == Selection{Focus::Slot, D, 40, G, 10, {}}, "Ctrl+A selects every slot");
    check(after(sceneSelected(20), Command::SelectAll) == Selection{Focus::Scene, 0, 40, 0, 10, {}}, "Ctrl+A on scenes selects every scene");
}

void editing() {
    const auto insert = run(Command::InsertScene, sceneSelected(10), ctx());
    check(insert.edits.size() == 2 && insert.edits[0].opType == "scene.create" && insert.edits[0].payload.at("id") == 41 &&
              insert.edits[0].payload.at("ord") == 4 && insert.edits[1].opType == "scene.reorder" &&
              insert.edits[1].payload.at("scenes") == Payload::array({10, 41, 20, 30, 40}) && editsValid(insert) &&
              insert.selection == Selection{Focus::Scene, 0, 41, 0, 41, {}} && insert.launches.empty(),
          "Insert Scene: an empty scene below the selection, selected, in one batch");
    const auto atEnd = run(Command::InsertScene, sceneSelected(40), ctx());
    check(atEnd.edits.size() == 1 && atEnd.edits[0].opType == "scene.create", "below the last scene no reorder is needed");
    const auto below = run(Command::InsertScene, selected({Focus::Slot, A, 30, A, 10, {}}), ctx());
    check(below.edits.size() == 2 && below.edits[1].payload.at("scenes") == Payload::array({10, 20, 30, 41, 40}),
          "below a slot range: under its last scene");
    check(run(Command::InsertScene, built(), ctx()).edits.size() == 1, "with nothing selected the scene is appended");

    const auto capture = run(Command::CaptureAndInsertScene, slotSelected(A, 20), ctx());
    check(capture.capture == CaptureRequest{20} && capture.edits.empty() && capture.launches.empty(),
          "Capture and Insert Scene goes to the engine's own capture, below the selection");
    check(run(Command::CaptureAndInsertScene, built(), ctx()).capture == CaptureRequest{},
          "with nothing selected the captured scene is appended");

    const auto clips = run(Command::Delete, selected({Focus::Slot, G, 20, G, 10, {}}), ctx());
    check(clips.edits.size() == 3 && std::all_of(clips.edits.begin(), clips.edits.end(),
                                                 [](const OpRequest& r) { return r.opType == "session.clearSlot"; }) &&
              editsValid(clips),
          "Delete on group slots deletes every clip they refer to, in one batch");
    check(run(Command::Delete, slotSelected(D, 10), ctx()).edits.empty(), "Delete on an empty slot does nothing");
    const auto scene = run(Command::Delete, sceneSelected(20), ctx());
    check(scene.edits.size() == 1 && scene.edits[0].opType == "scene.delete" && scene.edits[0].payload.at("id") == 20 &&
              scene.selection == Selection{Focus::Scene, 0, 30, 0, 30, {}} && editsValid(scene),
          "Delete on a scene deletes it and selects the one that took its place");
    check(run(Command::Delete, sceneSelected(40), ctx()).selection == Selection{Focus::Scene, 0, 30, 0, 30, {}},
          "deleting the last scene selects the new last one");
    const auto every = run(Command::Delete, selected({Focus::Scene, 0, 40, 0, 10, {}}), ctx());
    check(every.edits.empty() && !every.refusal.empty(), "the last scene of a Session is kept");
    check(run(Command::Delete, selected({Focus::Track, A, 0, 0, 0, {}}), ctx()).passToShell, "a track's Delete is the window's");

    const auto removeStop = run(Command::AddRemoveStopButton, slotSelected(D, 10), ctx());
    check(removeStop.edits.size() == 1 && removeStop.edits[0].opType == "session.setStopButton" &&
              removeStop.edits[0].payload.at("on") == false && editsValid(removeStop),
          "Ctrl+E removes a Stop button (7.4.2)");
    const auto addStop = run(Command::AddRemoveStopButton, selected({Focus::Slot, D, 20, D, 10, {}}), ctx());
    check(addStop.edits.size() == 1 && addStop.edits[0].payload.at("scene") == 20 && addStop.edits[0].payload.at("on") == true,
          "across a range, every slot takes the focus slot's new state");

    const auto off = run(Command::Deactivate, selected({Focus::Slot, B, 10, A, 10, {}}), ctx());
    check(off.edits.size() == 2 && off.edits[0].opType == "clip.setMute" && off.edits[0].payload.at("muted") == true &&
              editsValid(off),
          "0 deactivates the selected clips (7.1)");
    const auto on = run(Command::Deactivate, slotSelected(A, 20), ctx());
    check(on.edits.size() == 1 && on.edits[0].payload.at("muted") == false, "0 on a deactivated clip activates it");
    check(run(Command::Deactivate, selected({Focus::Track, A, 0, 0, 0, {}}), ctx()).passToShell,
          "0 on a track title is the Track Activator's");

    const auto down = run(Command::MoveScenesDown, selected({Focus::Scene, 0, 20, 0, 10, {}}), ctx());
    check(down.edits.size() == 1 && down.edits[0].payload.at("scenes") == Payload::array({30, 10, 20, 40}) && editsValid(down),
          "Ctrl+Down moves the selected scenes together");
    const auto up = run(Command::MoveScenesUp, sceneSelected(30), ctx());
    check(up.edits.size() == 1 && up.edits[0].payload.at("scenes") == Payload::array({10, 30, 20, 40}), "Ctrl+Up moves a scene up");
    check(run(Command::MoveScenesUp, sceneSelected(10), ctx()).edits.empty(), "the first scene moves no higher");

    const auto renameScene = run(Command::Rename, sceneSelected(10), ctx());
    const auto renameClip = run(Command::Rename, slotSelected(A, 10), ctx());
    const auto renameTrack = run(Command::Rename, selected({Focus::Track, B, 0, 0, 0, {}}), ctx());
    check(renameScene.rename == RenameTarget{Focus::Scene, 10} && renameClip.rename == RenameTarget{Focus::Slot, 100} &&
              renameTrack.rename == RenameTarget{Focus::Track, B} && !run(Command::Rename, slotSelected(D, 10), ctx()).rename,
          "Ctrl+R opens the name of a scene, a clip or a track");
    const auto m = built();
    const auto named = renameTo(m, {Focus::Scene, 10}, "Drop");
    const auto clip = renameTo(m, {Focus::Slot, 100}, "Groove");
    const auto track = renameTo(m, {Focus::Track, B}, "Sub");
    check(named.edits.size() == 1 && named.edits[0].opType == "scene.rename" && clip.edits.size() == 1 &&
              clip.edits[0].opType == "clip.setName" && track.edits.size() == 1 && track.edits[0].opType == "track.rename" &&
              editsValid(named) && editsValid(clip) && editsValid(track),
          "each rename is its own op");
    check(renameTo(m, {Focus::Scene, 10}, "Intro").edits.empty(), "an unchanged name is no edit");
    check(nextRenameTarget(m, {Focus::Scene, 10}, false) == RenameTarget{Focus::Scene, 20} &&
              nextRenameTarget(m, {Focus::Scene, 20}, true) == RenameTarget{Focus::Scene, 10} &&
              !nextRenameTarget(m, {Focus::Scene, 40}, false) &&
              nextRenameTarget(m, {Focus::Track, A}, false) == RenameTarget{Focus::Track, B},
          "Tab while renaming moves to the next scene or track (7.2)");
}

Model picked(Focus focus, std::vector<Pick> items) {
    auto m = built();
    Selection s;
    s.focus = focus;
    s.track = items.back().track;
    s.scene = items.back().scene;
    s.picks = std::move(items);
    m.select(s);
    return m;
}

bool copyIs(const SlotCopy& copy, std::int64_t clip, std::int64_t track, std::int64_t scene) {
    return copy.clip == clip && copy.track == track && copy.scene == scene;
}

void pickedEdits() {
    const auto down = run(Command::MoveScenesDown, picked(Focus::Scene, {{0, 10}, {0, 30}}), ctx());
    check(down.edits.size() == 1 && down.edits[0].payload.at("scenes") == Payload::array({20, 10, 40, 30}),
          "Ctrl+Down moves nonadjacent scenes without collapsing them (41.15)");
    const auto up = run(Command::MoveScenesUp, picked(Focus::Scene, {{0, 20}, {0, 40}}), ctx());
    check(up.edits.size() == 1 && up.edits[0].payload.at("scenes") == Payload::array({20, 10, 40, 30}), "and Ctrl+Up");
    const auto gone = run(Command::Delete, picked(Focus::Scene, {{0, 10}, {0, 30}}), ctx());
    check(gone.edits.size() == 2 && gone.edits[1].payload.at("id") == 30 && gone.selection == Selection{Focus::Scene, 0, 40, 0, 40, {}},
          "Delete on picked scenes deletes each, and selects the next that stays");
    const auto clips = run(Command::Delete, picked(Focus::Slot, {{A, 10}, {B, 30}}), ctx());
    check(clips.edits.size() == 2 && clips.edits[0].payload.at("track") == A && clips.edits[1].payload.at("scene") == 30,
          "Delete on picked clips deletes only those");
    const auto stops = run(Command::StopSelectedTracks, picked(Focus::Slot, {{A, 10}, {C, 30}}), ctx());
    check(stops.launches.size() == 2 && stops.launches[1].payload.at("track") == C, "Ctrl+Enter stops the picked slots' tracks");
}

void slotCommands() {
    auto make = ctx();
    make.nextClipId = 500;
    const auto midi = run(Command::InsertMidiClip, slotSelected(C, 10), make);
    check(midi.edits.size() == 3 && midi.edits[0].opType == "clip.create" && midi.edits[0].payload.at("id") == 500 &&
              midi.edits[0].payload.at("kind").get_ref<const std::string&>() == "midi" && midi.edits[0].payload.at("length") == 4 * textproj::kPPQ &&
              midi.edits[1].opType == "clip.setLoop" && midi.edits[2].opType == "session.setSlotClip" &&
              midi.edits[2].payload.at("scene") == 10 && midi.edits[2].payload.at("clip") == 500 && editsValid(midi) &&
              midi.selection == Selection{Focus::Slot, C, 10, C, 10, {}},
          "Ctrl+Shift+M makes a one-bar MIDI clip in the empty slot, selected, in one batch");
    const auto several = run(Command::InsertMidiClip, selected({Focus::Slot, D, 40, A, 30, {}}), make);
    // A30, A40, B40, C30 and C40 are empty slots of MIDI tracks; B30 is full and D is audio.
    check(several.edits.size() == 15 && several.edits[0].payload.at("track") == A && several.edits[3].payload.at("id") == 501 &&
              several.edits[6].payload.at("track") == B && several.edits[9].payload.at("track") == C &&
              several.edits[14].payload.at("clip") == 504,
          "over a range, each empty slot of a MIDI track takes one; a group, a full slot and an audio track none");
    check(run(Command::InsertMidiClip, slotSelected(D, 10), make).edits.empty() &&
              !run(Command::InsertMidiClip, slotSelected(A, 10), make).refusal.empty() &&
              !run(Command::InsertMidiClip, slotSelected(C, 10), ctx()).refusal.empty(),
          "an audio track, a full slot or an unknown next identity is refused by name");

    const auto off = run(Command::ToggleFollowActions, selected({Focus::Slot, B, 10, A, 10, {}}), ctx());
    check(off.edits.size() == 1 && off.edits[0].opType == "session.setFollowAction" && off.edits[0].payload.at("clip") == 101 &&
              off.edits[0].payload.at("enabled") == false && off.edits[0].payload.at("actionA") == 2 &&
              off.edits[0].payload.at("loops") == 2 && editsValid(off),
          "Shift+Enter takes the focus clip's Follow Actions off, the rest of its settings kept");
    const auto on = run(Command::ToggleFollowActions, slotSelected(A, 10), ctx());
    check(on.edits.size() == 1 && on.edits[0].payload.at("enabled") == true && editsValid(on), "and on");

    const auto chain = run(Command::CreateFollowActionChain, picked(Focus::Slot, {{A, 10}, {A, 20}, {B, 10}, {B, 30}}), ctx());
    const auto jumps = [&](std::size_t i, std::int64_t clip, std::int64_t scene) {
        const auto& p = chain.edits[i].payload;
        return p.at("clip") == clip && p.at("jumpA") == scene && p.at("actionA") == 9 && p.at("chanceB") == 0.0 &&
               p.at("enabled") == true;
    };
    check(chain.edits.size() == 4 && jumps(0, 100, 20) && jumps(1, 102, 10) && jumps(2, 101, 30) && jumps(3, 103, 10) &&
              editsValid(chain),
          "Ctrl+Shift+Enter chains each track's selected clips in a loop (16.7.2)");

    const auto dup = run(Command::Duplicate, slotSelected(A, 20), ctx());
    check(dup.edits.empty() && dup.copies.size() == 1 && copyIs(dup.copies[0], 102, A, 30) &&
              dup.selection == Selection{Focus::Slot, A, 30, A, 30, {}},
          "Ctrl+D copies a clip into the empty slot below, the copy selected");
    const auto pushed = run(Command::Duplicate, slotSelected(A, 10), ctx());
    check(pushed.edits.size() == 2 && pushed.edits[0].opType == "scene.create" && pushed.edits[0].payload.at("id") == 41 &&
              pushed.edits[1].payload.at("scenes") == Payload::array({10, 41, 20, 30, 40}) && pushed.copies.size() == 1 &&
              copyIs(pushed.copies[0], 100, A, 41) && pushed.copies[0].before == 2 && editsValid(pushed) &&
              pushed.selection == Selection{Focus::Slot, A, 41, A, 41, {}},
          "where the slot below is taken, Duplicate inserts a scene for the copy");
    const auto group = run(Command::Duplicate, slotSelected(G, 30), ctx());
    check(group.copies.size() == 1 && copyIs(group.copies[0], 103, B, 40), "a group slot duplicates the clips it refers to");

    const auto scene = run(Command::Duplicate, sceneSelected(10), ctx());
    const auto& create = scene.edits[0];
    check(create.opType == "scene.create" && create.payload.at("id") == 41 && create.payload.at("name").get_ref<const std::string&>() == "Intro" &&
              scene.edits[1].opType == "scene.setTempo" && scene.edits[1].payload.at("bpm") == 128.0 &&
              scene.edits[2].opType == "scene.setEnabled" && scene.edits[2].payload.at("tempo") == true &&
              scene.edits.back().opType == "scene.reorder" && scene.copies.size() == 2 &&
              copyIs(scene.copies[0], 100, A, 41) && copyIs(scene.copies[1], 101, B, 41) && editsValid(scene) &&
              scene.selection == Selection{Focus::Scene, 0, 41, 0, 41, {}},
          "Ctrl+D on a scene inserts its copy below, its tempo and clips with it");
    const auto verse = run(Command::Duplicate, sceneSelected(20), ctx());
    const auto stopsOff = std::count_if(verse.edits.begin(), verse.edits.end(), [](const OpRequest& r) {
        return r.opType == "session.setStopButton" && r.payload.at("on") == false && r.payload.at("scene") == 41;
    });
    check(stopsOff == 2 && verse.copies.size() == 1 && editsValid(verse) &&
              std::any_of(verse.edits.begin(), verse.edits.end(), [](const OpRequest& r) {
                  return r.opType == "scene.setSignature" && r.payload.at("numerator") == 7;
              }),
          "a scene's copy keeps its signature and the Stop buttons it lacked");
    const auto follows = run(Command::Duplicate, sceneSelected(30), ctx());
    check(std::any_of(follows.edits.begin(), follows.edits.end(),
                      [](const OpRequest& r) { return r.opType == "session.setFollowAction" && r.payload.at("scene") == 41; }) &&
              editsValid(follows),
          "and its Follow Action");
    check(run(Command::Duplicate, selected({Focus::Track, A, 0, 0, 0, {}}), ctx()).passToShell, "Duplicate on a track is the track's");

    const auto copied = run(Command::Copy, selected({Focus::Slot, B, 20, A, 10, {}}), ctx());
    check(copied.clipboard && copied.clipboard->kind == Clipboard::Kind::Clips && copied.clipboard->clips.size() == 3 &&
              copied.clipboard->clips[1].clip == 102 && copied.clipboard->clips[1].row == 1 &&
              copied.clipboard->clips[2].column == 1 && copied.edits.empty(),
          "Ctrl+C keeps the clips by their place in the selection");
    auto paste = ctx();
    paste.clipboard = &*copied.clipboard;
    const auto into = run(Command::Paste, slotSelected(C, 30), paste);
    check(into.copies.size() == 2 && copyIs(into.copies[0], 100, C, 30) && copyIs(into.copies[1], 102, C, 40) &&
              into.edits.empty() && into.selection == Selection{Focus::Slot, C, 40, C, 30, {}},
          "Ctrl+V pastes from the selected slot; an audio track does not take a MIDI clip");
    const auto past = run(Command::Paste, slotSelected(C, 40), paste);
    check(past.edits.size() == 1 && past.edits[0].opType == "scene.create" && past.copies.size() == 2 &&
              copyIs(past.copies[1], 102, C, 41) && editsValid(past),
          "a paste past the last scene adds the scene it needs");
    check(!run(Command::Paste, slotSelected(D, 10), paste).refusal.empty(), "where no track takes the clips, the paste is refused");
    check(!run(Command::Paste, sceneSelected(10), paste).refusal.empty(), "clips paste into a slot");
    const auto cut = run(Command::Cut, slotSelected(A, 10), ctx());
    check(cut.clipboard && cut.clipboard->clips.size() == 1 && cut.edits.size() == 1 && cut.edits[0].opType == "session.clearSlot" &&
              cut.edits[0].label == "Cut",
          "Ctrl+X keeps the clip and deletes it in one step");
    const auto scenes = run(Command::Copy, sceneSelected(20), ctx());
    auto pasteScene = ctx();
    pasteScene.clipboard = &*scenes.clipboard;
    const auto again = run(Command::Paste, sceneSelected(40), pasteScene);
    check(scenes.clipboard->kind == Clipboard::Kind::Scenes && again.edits[0].opType == "scene.create" &&
              again.copies.size() == 1 && copyIs(again.copies[0], 102, A, 41) && editsValid(again),
          "a copied scene pastes as a new scene below the selection");

    const auto moved = dropSlots(slotSelected(A, 10), cA, 0, cA, 3, false);
    check(moved.edits.size() == 1 && moved.edits[0].opType == "session.setSlotClip" && moved.edits[0].payload.at("scene") == 40 &&
              moved.copies.empty() && editsValid(moved) && moved.selection == Selection{Focus::Slot, A, 40, A, 40, {}},
          "a clip dropped along its track moves itself");
    const auto block = dropSlots(selected({Focus::Slot, A, 20, A, 10, {}}), cA, 0, cA, 1, false);
    check(block.edits.size() == 2 && block.edits[0].payload.at("clip") == 102 && block.edits[0].payload.at("scene") == 30 &&
              block.edits[1].payload.at("clip") == 100 && block.edits[1].payload.at("scene") == 20,
          "clips moved down their track go lowest first, so none lands on another");
    const auto across = dropSlots(slotSelected(A, 10), cA, 0, cC, 3, false);
    check(across.edits.size() == 1 && across.edits[0].opType == "session.clearSlot" && across.copies.size() == 1 &&
              copyIs(across.copies[0], 100, C, 40) && across.copies[0].before == 1,
          "dropped on another track, the clip goes as its copy, its place cleared first");
    const auto copyDrag = dropSlots(slotSelected(A, 10), cA, 0, cA, 3, true);
    check(copyDrag.edits.empty() && copyDrag.copies.size() == 1 && copyIs(copyDrag.copies[0], 100, A, 40),
          "Ctrl+drag copies (41.15)");
    check(!dropSlots(slotSelected(A, 10), cA, 0, cD, 0, false).refusal.empty() &&
              !dropSlots(slotSelected(A, 10), cA, 0, cR, 0, false).refusal.empty(),
          "a MIDI clip does not drop on an audio track or a return");

    const auto scenesDown = dropScenes(selected({Focus::Scene, 0, 20, 0, 10, {}}), 4);
    check(scenesDown.edits.size() == 1 && scenesDown.edits[0].payload.at("scenes") == Payload::array({30, 40, 10, 20}) &&
              scenesDown.selection == Selection{Focus::Scene, 0, 20, 0, 10, {}},
          "dropped scenes move together");
    const auto collapsed = dropScenes(picked(Focus::Scene, {{0, 10}, {0, 30}}), 1);
    check(collapsed.edits.size() == 1 && collapsed.edits[0].payload.at("scenes") == Payload::array({10, 30, 20, 40}),
          "nonadjacent scenes collapse together when dropped (7.2)");
    check(dropScenes(sceneSelected(20), 1).edits.empty(), "a scene dropped where it is does not move");

    const auto two = selected({Focus::Slot, B, 10, A, 10, {}});
    check(nextRenameTarget(two, {Focus::Slot, 100}, false) == RenameTarget{Focus::Slot, 101} &&
              !nextRenameTarget(two, {Focus::Slot, 101}, false) &&
              nextRenameTarget(two, {Focus::Slot, 101}, true) == RenameTarget{Focus::Slot, 100} &&
              run(Command::Rename, selected({Focus::Slot, D, 20, A, 20, {}}), ctx()).rename == RenameTarget{Focus::Slot, 102},
          "several selected clips are renamed one after another (7.1), from the first where the focus is empty");

    const auto colour = recolour(selected({Focus::Slot, B, 10, A, 10, {}}), 12);
    check(colour.edits.size() == 2 && colour.edits[0].opType == "clip.setColor" && colour.edits[1].payload.at("color") == 12 &&
              editsValid(colour),
          "the palette colours the selected clips");
    const auto track = recolour(selected({Focus::Track, A, 0, 0, 0, {}}), 5);
    check(track.edits.empty(), "a track already in the colour takes no edit");
    const auto titles = recolour(selected({Focus::Track, C, 0, 0, 0, {}}), 5);
    check(titles.edits.size() == 1 && titles.edits[0].opType == "track.setColor" && editsValid(titles), "and the selected tracks");
    check(!recolour(sceneSelected(10), 3).refusal.empty(), "a scene's colour waits for its op");
}

// --- on a real project ----------------------------------------------------------------------------

struct Project {
    test::TempDirectory temp{"session_view", "project"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};
        store = Store::create(temp.path() / "session.adi", error);
        if (!store)
            throw std::runtime_error("create a project");
        if (db().execAndGet("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='scenes'").getInt() == 0)
            test::applySessionSchema(db());
        db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(23,'group','G',0),(24,'midi','C',1);"
                  "INSERT INTO tracks(id,kind,name,parent_id,index_in_parent) VALUES(21,'midi','A',23,0),(22,'midi','B',23,1);"
                  "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) VALUES"
                  "(51,21,'midi','One',0,3840),(52,22,'midi','Two',0,3840),(53,21,'midi','Three',0,3840);");
        for (const auto& [id, ord] : std::array<std::pair<int, int>, 3>{{{11, 0}, {12, 1}, {13, 2}}})
            commit({request("scene.create", {{"id", id}, {"ord", ord}})});
        commit({request("session.setSlotClip", {{"track", 21}, {"scene", 11}, {"clip", 51}}),
                request("session.setSlotClip", {{"track", 22}, {"scene", 11}, {"clip", 52}}),
                request("session.setSlotClip", {{"track", 21}, {"scene", 12}, {"clip", 53}})});
    }
    SQLite::Database& db() { return store->db(); }
    static OpRequest request(const char* name, Payload payload) {
        OpRequest r;
        r.opType = name;
        r.payload = std::move(payload);
        return r;
    }
    bool commit(const std::vector<OpRequest>& ops) {
        const auto result = OpJournal(*store).commit(std::span<const OpRequest>(ops));
        if (!result.ok)
            std::printf("commit: %s\n", result.error.c_str());
        return result.ok;
    }
    bool undo() { return History(*store).undo().ok; }
    Model model() {
        const auto project = rows::readModel(*store);
        Model m;
        m.rebuild(modelInput(project, engine::launch::readLaunchData(*store, project)));
        return m;
    }
    Payload dump() {
        auto out = Payload::object();
        for (const auto* table : {"scenes", "clip_slots", "clip_launch", "clips", "tracks"}) {
            auto& rows = out[table];
            rows = Payload::array();
            SQLite::Statement q(db(), std::string("SELECT * FROM ") + table + " ORDER BY 1,2");
            while (q.executeStep())
                rows.push_back(detail::captureQueryRow(q));
        }
        return out;
    }
};

/// The command commits as one batch, changes the project, and one Undo puts back exactly what was.
bool oneUndoStep(Project& p, const Outcome& out) {
    if (out.edits.empty())
        return false;
    const auto before = p.dump();
    if (!p.commit(out.edits) || p.dump() == before || !p.undo())
        return false;
    return p.dump() == before;
}

void onProject() {
    Project p;
    auto m = p.model();
    check(m.columnCount() == 4 && m.column(0).track == 23 && m.column(1).track == 21 && m.column(2).track == 22 &&
              m.column(3).track == 24 && m.column(0).kind == ColumnKind::Group,
          "the project's tracks lay out in the Arrangement's order, a group before what it holds, Main left out");
    check(m.sceneCount() == 3 && m.slot(1, 0).clip == 51 && m.slot(1, 0).name == "One" && m.slot(2, 0).clip == 52 &&
              m.slot(1, 1).clip == 53 && m.slot(0, 0).groupOccupied,
          "the project's scenes and slots reach the model");

    const auto pick = [&](Selection s) {
        if (s.anchorTrack == 0)
            s.anchorTrack = s.track;
        if (s.anchorScene == 0)
            s.anchorScene = s.scene;
        m.select(s);
    };
    pick({Focus::Scene, 0, 11, 0, 0, {}});
    const auto insert = run(Command::InsertScene, m, ctx());
    check(oneUndoStep(p, insert), "Insert Scene is one step of Undo");
    check(p.commit(insert.edits), "Insert Scene commits again");
    m = p.model();
    check(m.sceneCount() == 4 && m.scene(1).id == 14 && m.scene(2).id == 12, "the inserted scene stands below the selected one");
    check(p.undo(), "and is undone");
    m = p.model();

    pick({Focus::Slot, 21, 12, 22, 11, {}});
    check(oneUndoStep(p, run(Command::Delete, m, ctx())), "Delete of several clips is one step of Undo");
    pick({Focus::Slot, 22, 12, 0, 0, {}});
    check(oneUndoStep(p, run(Command::AddRemoveStopButton, m, ctx())), "Add/Remove Stop Button is one step of Undo");
    pick({Focus::Slot, 22, 11, 21, 11, {}});
    check(oneUndoStep(p, run(Command::Deactivate, m, ctx())), "Deactivate is one step of Undo");
    pick({Focus::Scene, 0, 12, 11, 0, {}});
    check(oneUndoStep(p, run(Command::MoveScenesDown, m, ctx())), "moving scenes is one step of Undo");
    pick({Focus::Scene, 0, 12, 0, 0, {}});
    check(oneUndoStep(p, run(Command::Delete, m, ctx())), "Delete Scene, with its clips, is one step of Undo");
    check(oneUndoStep(p, renameTo(m, {Focus::Scene, 12}, "Verse")), "renaming a scene is one step of Undo");
    check(oneUndoStep(p, renameTo(m, {Focus::Slot, 51}, "Groove")), "renaming a clip is one step of Undo");
    check(oneUndoStep(p, renameTo(m, {Focus::Track, 24}, "Keys")), "renaming a track is one step of Undo");

    // After an edit the model takes the project again: a clip renamed shows its new name.
    check(p.commit(renameTo(m, {Focus::Slot, 51}, "Groove").edits), "a rename commits");
    m = p.model();
    check(m.slot(1, 0).name == "Groove", "a rebuild after the commit shows it");
}

/// The same for an outcome with clip copies: prepared against the project, committed as one batch.
bool oneUndoStepPrepared(Project& p, const Outcome& out, const Clipboard* board = nullptr) {
    const auto prepared = prepareEdit(*p.store, out, board);
    if (!prepared.ok) {
        std::printf("prepare: %s\n", prepared.error.c_str());
        return false;
    }
    const auto before = p.dump();
    const auto result = clip_edit::commitPrepared(*p.store, prepared);
    if (!result.ok) {
        std::printf("commit: %s\n", result.error.c_str());
        return false;
    }
    if (p.dump() == before || !p.undo())
        return false;
    return p.dump() == before;
}

bool commitCopies(Project& p, const Outcome& out, const Clipboard* board = nullptr) {
    const auto prepared = prepareEdit(*p.store, out, board);
    const auto result = prepared.ok ? clip_edit::commitPrepared(*p.store, prepared) : CommitResult{};
    if (!result.ok)
        std::printf("prepared commit: %s %s\n", prepared.error.c_str(), result.error.c_str());
    return result.ok;
}

void copiesOnProject() {
    Project p;
    // Columns: 0 the group 23, 1 track 21, 2 track 22, 3 track 24. Scenes 11, 12, 13.
    check(p.commit({Project::request("session.setLaunchMode", {{"clip", 53}, {"mode", 2}})}), "a clip takes a launch mode");
    auto m = p.model();
    const auto pick = [&](Selection s) {
        if (s.anchorTrack == 0)
            s.anchorTrack = s.track;
        if (s.anchorScene == 0)
            s.anchorScene = s.scene;
        m.select(s);
    };
    check(m.column(3).midi && m.slot(1, 0).midiClip && m.slot(1, 1).mode == engine::LaunchMode::Toggle,
          "the project's track and clip kinds and launch settings reach the model");

    auto make = ctx();
    make.nextClipId = 54;
    pick({Focus::Slot, 24, 13, 0, 0, {}});
    const auto midi = run(Command::InsertMidiClip, m, make);
    check(oneUndoStep(p, midi), "Insert MIDI Clip is one step of Undo");
    check(p.commit(midi.edits), "and commits again");
    m = p.model();
    check(m.slot(3, 2).clip == 54 && m.slot(3, 2).midiClip, "the new clip stands in its slot");
    check(p.undo(), "and is undone");
    m = p.model();

    pick({Focus::Slot, 21, 11, 0, 0, {}});
    check(oneUndoStep(p, run(Command::ToggleFollowActions, m, ctx())), "Toggle Follow Actions is one step of Undo");
    m.select({Focus::Slot, 21, 12, 21, 12, {{21, 11}, {21, 12}}});
    const auto chain = run(Command::CreateFollowActionChain, m, ctx());
    check(chain.edits.size() == 2 && oneUndoStep(p, chain), "Create Follow Action Chain is one step of Undo");

    pick({Focus::Slot, 21, 12, 0, 0, {}});
    const auto dup = run(Command::Duplicate, m, ctx());
    check(dup.copies.size() == 1 && oneUndoStepPrepared(p, dup), "Duplicate of a clip is one step of Undo");
    check(commitCopies(p, dup), "and commits again");
    m = p.model();
    check(m.slot(1, 2).clip > 53 && m.slot(1, 2).name == "Three" && m.slot(1, 2).mode == engine::LaunchMode::Toggle &&
              m.slot(1, 1).clip == 53,
          "the copy stands below, named as its clip, with its launch settings");
    check(p.undo(), "and is undone");
    m = p.model();

    pick({Focus::Scene, 0, 11, 0, 0, {}});
    const auto scene = run(Command::Duplicate, m, ctx());
    check(scene.copies.size() == 2 && oneUndoStepPrepared(p, scene), "Duplicate of a scene, with its clips, is one step of Undo");

    pick({Focus::Slot, 22, 11, 21, 11, {}});
    const auto copied = run(Command::Copy, m, ctx());
    pick({Focus::Slot, 21, 13, 0, 0, {}});
    auto paste = ctx();
    paste.clipboard = &*copied.clipboard;
    const auto pasted = run(Command::Paste, m, paste);
    check(pasted.copies.size() == 2 && oneUndoStepPrepared(p, pasted), "Paste is one step of Undo");

    pick({Focus::Slot, 21, 12, 0, 0, {}});
    auto cut = run(Command::Cut, m, ctx());
    std::string error;
    check(cut.clipboard && snapshotClipboard(*p.store, *cut.clipboard, error) && cut.clipboard->images.contains("53"),
          "a Cut takes its clips' images first");
    check(oneUndoStep(p, cut), "Cut is one step of Undo");
    check(p.commit(cut.edits), "the Cut commits");
    m = p.model();
    check(m.slot(1, 1).clip == 0, "the cut clip is gone");
    pick({Focus::Slot, 21, 13, 0, 0, {}});
    paste.clipboard = &*cut.clipboard;
    const auto afterCut = run(Command::Paste, m, paste);
    check(oneUndoStepPrepared(p, afterCut, &*cut.clipboard), "a paste after a Cut copies from the images");
    check(commitCopies(p, afterCut, &*cut.clipboard), "and commits");
    m = p.model();
    check(m.slot(1, 2).name == "Three" && m.slot(1, 2).mode == engine::LaunchMode::Toggle, "the cut clip is back in its new place");
    check(p.undo() && p.undo(), "the paste and the Cut are undone, one step each");
    m = p.model();
    check(m.slot(1, 1).clip == 53, "the clip is back where it was");

    pick({Focus::Slot, 21, 11, 0, 0, {}});
    const auto along = dropSlots(m, 1, 0, 1, 2, false);
    check(along.copies.empty() && oneUndoStep(p, along), "a drop along the track is one step of Undo");
    const auto across = dropSlots(m, 1, 0, 3, 0, false);
    check(across.copies.size() == 1 && oneUndoStepPrepared(p, across), "a drop on another track is one step of Undo");
    check(commitCopies(p, across), "and commits");
    m = p.model();
    check(m.slot(1, 0).clip == 0 && m.slot(3, 0).clip != 0 && m.slot(3, 0).name == "One",
          "the clip left its track for the other");
    check(p.undo(), "and is undone");
    m = p.model();
    pick({Focus::Slot, 21, 11, 0, 0, {}});
    const auto copyDrag = dropSlots(m, 1, 0, 2, 2, true);
    check(copyDrag.copies.size() == 1 && oneUndoStepPrepared(p, copyDrag), "a Ctrl+drag copy is one step of Undo");

    pick({Focus::Scene, 0, 13, 0, 0, {}});
    check(oneUndoStep(p, dropScenes(m, 0)), "dropping scenes is one step of Undo");
    pick({Focus::Slot, 22, 11, 21, 11, {}});
    check(oneUndoStep(p, recolour(m, 7)), "colouring clips is one step of Undo");
}

} // namespace

int main() {
    try {
        layout();
        slotStates();
        allocationFree();
        selection();
        picks();
        keys();
        viewSwitch();
        launching();
        navigation();
        editing();
        pickedEdits();
        slotCommands();
        onProject();
        copiesOnProject();
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what());
        return 1;
    }
    std::printf("%d session view checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
