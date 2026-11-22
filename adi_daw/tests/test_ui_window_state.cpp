// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/ui/density.hpp"
#include "adi/ui/midi_ports.hpp"
#include "adi/ui/session_state.hpp"
#include "adi/ui/theme.hpp"
#include "adi/ui/window_state.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <thread>
#include <vector>
#include <array>
using namespace adi;
using namespace adi::ui;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
// The theme and density tables (DF-W24): the window's every colour and size, by role.
void themeAndDensityTables() {
    // Each theme by its settings word, and back; "os" and anything unknown read as Dark, the default.
    for (const auto id : kThemes)
        check(themeFromSetting(themeName(id)) == id, "a theme's word reads back as the theme");
    check(themeFromSetting("os") == ThemeId::Dark && themeFromSetting("") == ThemeId::Dark &&
              themeFromSetting("purple") == ThemeId::Dark,
          "the registry's \"os\", and anything unknown, read as Dark");
    // Dark is the sheets' (lib.js's C): the window, the panel, a control, the text, the accent, the value.
    const auto &dark = themeFor(ThemeId::Dark);
    check(dark.frame == 0xff1b1b1b && dark.panel == 0xff2b2b2b && dark.chooser == 0xff3d3d3d &&
              dark.text == 0xffdcdcdc && dark.accent == 0xffffad3b && dark.value == 0xff4fc3ea &&
              dark.ink == 0xff1a1a1a && dark.lane == 0xff323232 && dark.laneAlt == 0xff2e2e2e,
          "Dark is the language sheet's colours");
    // Mid grey is the window as built before the sheets.
    const auto &grey = themeFor(ThemeId::MidGrey);
    check(grey.frame == 0xff707070 && grey.panel == 0xff8f8f8f && grey.field == 0xffaaaaaa && grey.text == 0xff000000 &&
              grey.accent == 0xfff4b43e && grey.value == 0xff7ddbf3 && grey.outline == 0xff444444,
          "Mid grey is the window as built");
    // Light and Mix View carry the sheet's six swatches.
    const auto &light = themeFor(ThemeId::Light), &mix = themeFor(ThemeId::MixView);
    check(light.frame == 0xffe9e9e9 && light.panel == 0xfff6f6f6 && light.chooser == 0xffffffff &&
              light.text == 0xff1c1c1c && light.accent == 0xffff9d1c && light.value == 0xff1fa3d1,
          "Light is the sheet's six swatches");
    check(mix.frame == 0xff131316 && mix.panel == 0xff202027 && mix.chooser == 0xff30303a && mix.text == 0xffd9d9de &&
              mix.accent == 0xffffb13b && mix.value == 0xff5ec8ff,
          "Mix View is the sheet's six swatches");
    // Every theme is opaque, and its text stands off its panel, its ink off its accent.
    const auto level = [](std::uint32_t c) { return ((c >> 16 & 0xff) + (c >> 8 & 0xff) + (c & 0xff)) / 3; };
    for (const auto id : kThemes) {
        const auto &t = themeFor(id);
        bool opaque = true;
#define ADI_THEME_OPAQUE(name) opaque = opaque && (t.name >> 24) == 0xff;
        ADI_THEME_ROLES(ADI_THEME_OPAQUE)
#undef ADI_THEME_OPAQUE
        check(opaque, "every colour of every theme is opaque");
        const auto apart = [&](std::uint32_t a, std::uint32_t b) {
            return (level(a) > level(b) ? level(a) - level(b) : level(b) - level(a)) >= 100;
        };
        check(apart(t.text, t.panel) && apart(t.text, t.lane) && apart(t.ink, t.accent),
              "a theme's text stands off its panel and lanes, and its ink off its accent");
        check(apart(t.knobNeedle, t.panel) && apart(t.knobNeedle, t.lane) && t.knobRing != t.panel,
              "a theme's knob needle stands off its panel and lanes, and its ring is not the panel");
        check(level(t.knobFaceHigh) > level(t.knobFaceLow) && level(t.knobFaceHoverHigh) > level(t.knobFaceHoverLow),
              "a theme's knob dial is lit from its lighter stop, under the pointer too");
    }
    // The densities: Compact is Live's sizes as built, Open is roomier in every measure.
    check(densityFromSetting(densityName(DensityId::Open)) == DensityId::Open &&
              densityFromSetting("") == DensityId::Compact && densityFromSetting("compact") == DensityId::Compact,
          "a density's word reads back; anything else is Compact");
    const auto &compact = densityFor(DensityId::Compact), &open = densityFor(DensityId::Open);
    check(compact.controlBarHeight == 35 && compact.statusBarHeight == 38 && compact.gap == 8 && compact.border == 4 &&
              compact.fieldHeight == 17 && compact.detailHeight == 196 && compact.fontBody == 11.5f,
          "Compact is the window's measures as built");
    check(compact.knobSmall == 11.f && compact.knobMedium == 15.f && compact.knobLarge == 21.f &&
              compact.radiusControl == 5.f && compact.radiusGroup == 8.f && open.fontTiny == 11.f && open.knobSmall == 16.f,
          "the language sheet's knobs and corners; Open's compressor as the sheet draws it");
    bool roomier = true;
#define ADI_DENSITY_ROOMIER(name) roomier = roomier && open.name >= compact.name;
    ADI_DENSITY_ROLES(ADI_DENSITY_ROOMIER, ADI_DENSITY_ROOMIER)
#undef ADI_DENSITY_ROOMIER
    check(roomier, "Open is at least Compact in every measure");
}

// SRF-05's JUCE-free half: a port's switches from the Settings' `midi.ports` record, the lock-free ring
// a port's events go through, and a Universal MIDI Packet read as the MIDI 1.0 message it carries.
void midiPortsCore() {
    const auto defaults = midiPortSwitches(settings::Value::object(), "port-1");
    check(defaults.track == midiPortDefault("track") && defaults.remote == midiPortDefault("remote") &&
              defaults.sync == midiPortDefault("sync") && defaults.mpe == midiPortDefault("mpe"),
          "a port with no record takes the midi.port.* rows' defaults");
    auto record = withMidiPortSwitch(settings::Value::object(), "port-1", "track", true);
    record = withMidiPortSwitch(record, "port-1", "remote", true);
    record = withMidiPortSwitch(record, "port-2", "track", false);
    const auto one = midiPortSwitches(record, "port-1"), two = midiPortSwitches(record, "port-2");
    check(one.track && one.remote && !two.track && two.remote == midiPortDefault("remote"),
          "each port's own fields, and the defaults for the rest");
    const auto *row = settings::find("midi.ports");
    std::string why;
    check(row != nullptr && settings::validate(*row, record, why), "the record written back is one the registry takes");
    // The ring: in order, refusing when full, counting what it lost.
    MidiEventRing<4> ring;
    for (std::uint32_t i = 0; i < 5; ++i) {
        MidiPortEvent e;
        e.port = i;
        ring.push(e);
    }
    MidiPortEvent out;
    std::vector<std::uint32_t> got;
    while (ring.pop(out))
        got.push_back(out.port);
    check(got == std::vector<std::uint32_t>{0, 1, 2, 3} && ring.dropped() == 1, "the ring keeps order, refuses when full and counts");
    // One producer thread, one consumer: every event arrives once, in order.
    MidiEventRing<64> shared;
    std::thread producer([&] {
        for (std::uint32_t i = 0; i < 20000;) {
            MidiPortEvent e;
            e.port = i;
            if (shared.push(e))
                ++i;
        }
    });
    std::uint32_t expect = 0;
    bool ordered = true;
    while (expect < 20000)
        if (shared.pop(out)) {
            ordered = ordered && out.port == expect;
            ++expect;
        }
    producer.join();
    check(ordered && expect == 20000, "across two threads every event arrives once, in order");
    // A UMP word in the MIDI 1.0 protocol: a note on, a program change, the clock, and what is not carried.
    MidiPortEvent e;
    check(midiFromUmpWord(0x20903c64u, e) && e.size == 3 && e.bytes == std::array<std::uint8_t, 3>{0x90, 0x3c, 0x64},
          "a MIDI 1.0 note on (type 2) reads as its three bytes");
    check(midiFromUmpWord(0x23c50577u, e) && e.size == 2 && e.bytes[0] == 0xc5 && e.bytes[1] == 5 && e.bytes[2] == 0 &&
              e.group == 3,
          "a program change on group 3 reads as two bytes, its stray third byte cleared, its group kept");
    check(midiFromUmpWord(0x10f80000u, e) && e.size == 1 && e.bytes[0] == 0xf8, "the clock (type 1) reads as one byte");
    check(!midiFromUmpWord(0x30160102u, e) && !midiFromUmpWord(0x40903c00u, e) && !midiFromUmpWord(0x00000000u, e) &&
              !midiFromUmpWord(0x10f40000u, e),
          "system exclusive, MIDI 2.0, utility and an undefined status are not carried");
    check(midiMessageSize(0xe0) == 3 && midiMessageSize(0xd3) == 2 && midiMessageSize(0xf2) == 3 &&
              midiMessageSize(0xf0) == 0 && midiMessageSize(0x40) == 0,
          "each status byte's length");
}
} // namespace
int main() {
    themeAndDensityTables();
    midiPortsCore();
    test::TempDirectory temp("ui_state", "roundtrip");
    StoreError err{};
    auto store = Store::createEmpty(temp.path() / "p.adi", err);
    if (!store)
        return 2;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'UI test')");
    ViewStateStore views(*store);
    std::string error;
    {
        WindowState stack;
        stack.zoom = 1.5;
        stack.mixStackSearch = "Bass";
        stack.mixStackBaseHeight = 140;
        check(views.save("stack", stack, error) && views.load("stack").mixStackSearch == "Bass" &&
                  views.load("stack").mixStackBaseHeight == 140 && !views.load("other").mixStackBaseHeight,
              "Stack search and row size persist per project window");
        store->db().exec(R"(INSERT INTO ui_view(scope_kind,scope_id,key,value) VALUES('project',NULL,'shell.old-stack',
            '{"version":1,"zoom":1.5,"deviceHeight":196,"docked":true,"panels":[{"id":0,"width":300,"collapsed":false},
             {"id":1,"width":200,"collapsed":false}],"mixview.stack.baseHeight":18446744073709551615,
             "mixview.stack.search":42}'))");
        const auto old = views.load("old-stack");
        check(old.zoom == 1.5 && old.mixStackSearch.empty() && !old.mixStackBaseHeight,
              "invalid optional Stack fields keep the rest of the window geometry");
        constexpr std::array<std::int64_t, 1> first{7}, second{99};
        check(views.saveStackPins({}, first, error) && views.saveStackPins({}, second, error) &&
                  views.stackPins().ids == std::vector<std::int64_t>{7, 99},
              "additions from two stale window projections do not overwrite each other");
        check(views.saveStackPins(first, {}, error), "remove one project's pin without changing the others");
        const ViewStateStore reopened(*store);
        check(reopened.stackPins().ids == std::vector<std::int64_t>{99},
              "a fresh view owner reloads stable project pins from storage");
        store->db().exec(R"(UPDATE ui_view SET value='[99,7,7,0,-1,"99",18446744073709551615]'
                            WHERE key='mixview.stack.pins')");
        const ViewStateStore legacyPins(*store);
        check(legacyPins.stackPins().ids == std::vector<std::int64_t>{7, 99},
              "pin decoding deduplicates positive track IDs and ignores invalid entries");
    }
    WindowState state;
    check(!state.panels.setWidth(Panel::Browser, 333, 1400), "resize browser");
    check(!state.panels.swapSides(1400), "swap panel identities");
    state.zoom = 2;
    state.deviceHeight = 245;
    state.docked = false;
    state.follow = true;
    state.overview = false;
    state.inOut = false;
    state.sends = false;
    state.mixerInOut = true;
    state.mixerSends = false;
    state.trackOptions = true;
    state.mixerTrackOptions = true;
    state.largeTracks = false;
    state.viewGroup = 42;
    state.foldedGroups = {7, 13};
    state.mixView = true;
    state.mixViewTab = 3;
    state.mixViewDetached = true;
    state.mixAnalyzerDock = true;
    state.mixAnalyzerWidth = 360;
    state.mixAnalyzerSource = 1;
    state.mixAnalyzerPoint = 2;
    state.mixAnalyzerChannel = 4;
    state.analysisPalette = "viridis";
    state.analysisPaletteReverse = true;
    state.analysisSpectrumSlope = "tilt45";
    state.showReference = true;
    // The Note Editor's own settings.
    state.noteGridSnap = false;
    state.noteGridFixed = true;
    state.noteGridTriplet = true;
    state.noteGridNarrow = -1;
    state.noteGridFixedQuarters = 2;
    state.highlightScale = false;
    state.noteColours = 3;
    state.noteLaneHeight = 90;
    state.clipPanelsArrangement = 2;
    state.quantizeValue = 6;
    state.quantizeAmount = 40;
    state.quantizeSwing = 15;
    state.quantizeStarts = false;
    state.quantizeEnds = true;
    state.hideReturns = true;
    state.gridSnap = false;
    state.gridFixed = true;
    state.gridTriplet = true;
    state.gridNarrow = 2;
    state.gridFixedQuarters = .125;
    check(views.save("main", state, error), "save view");
    const auto restored = views.load("main");
    check(restored.follow && !views.load("other").follow, "Follow is kept per window, off until switched on");
    check(!restored.overview && !restored.inOut && views.load("other").overview && views.load("other").inOut,
          "the Overview and In/Out are kept per window, shown until hidden");
    check(!restored.sends && views.load("other").sends, "so are the sends (Hide/Show Sends, §42.1)");
    check(restored.mixerInOut && !views.load("other").mixerInOut,
          "the Mixer View's own In/Out is kept per window, hidden until shown (Live 12's Mixer Controls)");
    check(!restored.mixerSends && views.load("other").mixerSends,
          "so are the Mixer View's own sends, shown until hidden (Live 12's Mixer Controls)");
    check(restored.trackOptions && restored.mixerTrackOptions && !views.load("other").trackOptions &&
              !views.load("other").mixerTrackOptions,
          "whether the Track Delay shows is kept per window, the track controls' and the Mixer View's apart, hidden "
          "until shown (Live's Track Options, manual 18.7)");
    check(!restored.largeTracks && views.load("other").largeTracks,
          "Use Large Track Height is kept per window, on until switched off");
    check(restored.viewGroup == 42 && views.load("other").viewGroup == 0,
          "the project view's group is kept per window, the whole project until one is chosen");
    check(restored.foldedGroups == std::set<std::int64_t>{7, 13} && views.load("other").foldedGroups.empty(),
          "the folded groups are kept per window, every group open until folded");
    check(restored.mixView && restored.mixViewTab == 3 && !views.load("other").mixView &&
              views.load("other").mixViewTab == 1,
          "the Mix View and its tab are kept per window: closed, on STACK, until opened (ADR-0209)");
    check(restored.mixViewDetached && !views.load("other").mixViewDetached,
          "and where it opens: docked until moved into a window of its own");
    check(restored.mixAnalyzerDock && restored.mixAnalyzerWidth == 360 && restored.mixAnalyzerSource == 1 &&
              restored.mixAnalyzerPoint == 2 && restored.mixAnalyzerChannel == 4 &&
              !views.load("other").mixAnalyzerDock && views.load("other").mixAnalyzerWidth == 300 &&
              views.load("other").mixAnalyzerSource == 0,
          "optional analyzer dock source/tap/channel/width persist per window, off and Main by default");
    check(!restored.noteGridSnap && restored.noteGridFixed && restored.noteGridTriplet && restored.noteGridNarrow == -1 &&
              restored.noteGridFixedQuarters == 2 && !restored.highlightScale && restored.noteColours == 3 && restored.quantizeValue == 6 &&
              restored.noteLaneHeight == 90 && views.load("other").noteLaneHeight == 56 &&
              restored.clipPanelsArrangement == 2 && views.load("other").clipPanelsArrangement == 0 &&
              restored.quantizeAmount == 40 && restored.quantizeSwing == 15 && !restored.quantizeStarts && restored.quantizeEnds &&
              views.load("other").noteGridSnap && !views.load("other").noteGridFixed && views.load("other").highlightScale &&
              views.load("other").noteColours == 0 && views.load("other").quantizeAmount == 100 && views.load("other").quantizeStarts &&
              views.load("other").clipViewHeight == 286,
          "the Note Editor's grid, Highlight Scale, note colours and Quantize settings persist per window, with Live's defaults");
    check(restored.analysisPalette == "viridis" && restored.analysisPaletteReverse &&
              views.load("other").analysisPalette == "adi" && !views.load("other").analysisPaletteReverse,
          "spectrogram palette/reverse persist per window with original ADI colors as the default");
    check(restored.analysisSpectrumSlope == "tilt45" && views.load("other").analysisSpectrumSlope == "flat",
          "Spectrum display slope persists per window with the exact Flat default for old preferences");
    {
        auto invalid = state; invalid.analysisSpectrumSlope = "unknown-slope";
        check(!views.save("main", invalid, error) && views.load("main").analysisSpectrumSlope == "tilt45",
              "unknown display slope cannot replace the saved known preference");
    }
    {
        auto invalid = state; invalid.analysisPalette = "unknown-palette";
        check(!views.save("main", invalid, error) && views.load("main").analysisPalette == "viridis",
              "unknown palette cannot replace a saved known palette");
    }
    {
        auto invalid = state;
        invalid.mixAnalyzerWidth = 239;
        check(!views.save("main", invalid, error) && views.load("main").mixAnalyzerWidth == 360,
              "invalid dock geometry is refused without replacing saved preferences");
    }
    check(restored.showReference && !views.load("other").showReference,
          "the Reference track's toggle is kept per window: hidden until shown (ADR-0209 d11)");
    check(restored.hideReturns && !views.load("other").hideReturns,
          "Hide/Show Return Tracks is kept per window: shown until hidden (§18.4)");
    check(!restored.gridSnap && restored.gridFixed && restored.gridTriplet && restored.gridNarrow == 2 &&
              restored.gridFixedQuarters == .125 && views.load("other").gridSnap && !views.load("other").gridFixed,
          "the editing grid is kept per window, snapping and adaptive until changed");
    {
        WindowState beyond = state;
        beyond.gridNarrow = 3;
        beyond.gridFixedQuarters = 1. / 32;
        check(views.save("beyond", beyond, error) && views.load("beyond").gridNarrow == 2 &&
                  views.load("beyond").gridFixedQuarters == .125,
              "a grid width beyond the lanes' menu loads as the nearest it names: Narrowest, 1/32");
    }
    check(restored.panels.slots()[0].panel == Panel::Mixer, "restore order");
    check(restored.panels.find(Panel::Browser)->width == 333, "width follows panel");
    check(restored.zoom == 2 && restored.deviceHeight == 245 && !restored.docked,
          "restore zoom and dock");
    check(views.load("other").zoom == 1, "windows have independent view state");
    check(store->db().execAndGet("SELECT count(*) FROM ops").getInt() == 0,
          "view persistence creates no edit/undo entry");
    state.zoom = 0;
    check(!views.save("main", state, error), "invalid view refused");
    check(views.load("main").zoom == 2, "refusal preserves saved state");
    store->db().exec("UPDATE ui_view SET value='{broken'");
    check(views.load("main").zoom == 1, "malformed view defaults");
    store->db().exec("UPDATE ui_view SET value='{\"version\":99}'");
    check(views.load("main").zoom == 1, "future view defaults without crashing");
    auto readonly = Store::open(temp.path() / "p.adi", err, true);
    ViewStateStore ro(*readonly);
    check(!ro.save("main", WindowState{}, error), "read-only view refuses save");
    {
        SessionState roSession(*readonly);
        Selection s;
        s.track = 3;
        check(roSession.select(s, error) && roSession.selection().track == 3,
              "a read-only project still selects, in memory");
    }
    // Selection is session state (ADR-0021): one per project, never an op.
    {
        SessionState session(*store);
        check(session.selection() == Selection{}, "a new project selects nothing");
        Selection s;
        s.track = 7;
        s.start = 10;
        s.end = 20;
        s.marked = true;
        s.insert = 10;
        const auto before = session.generation();
        check(session.select(s, error) && session.generation() == before + 1,
              "a new selection is a new generation");
        check(session.select(s, error) && session.generation() == before + 1,
              "the same selection again is no change");
        check(SessionState(*store).selection() == s, "it is kept in session_state and reloads");
        check(store->db().execAndGet("SELECT count(*) FROM ops").getInt() == 0,
              "and selecting writes no op");
        const auto blob = session.advisory();
        Selection other;
        other.track = 9;
        check(session.select(other, error), "select something else");
        session.restore(blob);
        check(session.selection() == s, "an undo's sel_before puts the selection back");
        session.restore(Payload::object());
        check(session.selection() == s, "a step without one leaves it alone");
        store->db().exec("UPDATE session_state SET value='{broken' WHERE key='selection'");
        check(SessionState(*store).selection() == Selection{},
              "a malformed row is a lost selection, never a failed open");
    }
    DirtySet a, b;
    check(a.drain() == DirtySet::All && b.drain() == DirtySet::All, "new windows initially dirty");
    a.mark(DirtySet::Transport);
    a.mark(DirtySet::Transport);
    check(a.drain() == DirtySet::Transport && a.drain() == 0 && b.drain() == 0,
          "coalescing and per-window isolation");
    TransportMailbox mailbox;
    engine::Transport transport;
    check(mailbox.post(TransportMailbox::Command::Play), "queue play");
    check(!transport.playing() && mailbox.desiredPlaying(),
          "UI never mutates driver; queued intent visible");
    mailbox.drain(transport);
    check(transport.playing() && mailbox.playing(), "driver applies command and feeds back");
    transport.advance(64);
    mailbox.publish(transport);
    check(mailbox.position() == 64, "driver position feedback");
    for (int i = 0; i < 16; ++i)
        check(mailbox.post(TransportMailbox::Command::Stop), "bounded queue accepts capacity");
    check(!mailbox.post(TransportMailbox::Command::Play), "full queue refuses without overwrite");
    mailbox.drain(transport);
    check(!transport.playing(), "queued stop survives saturation");
    // Two actual threads exercise release/acquire ownership, with a final stop sentinel.
    std::atomic<bool> finished{false};
    std::thread driver([&] {
        while (!finished.load(std::memory_order_acquire))
            mailbox.drain(transport);
        mailbox.drain(transport);
    });
    for (int i = 0; i < 10000; ++i) {
        while (!mailbox.post(i % 2 ? TransportMailbox::Command::Stop
                                   : TransportMailbox::Command::Play))
            std::this_thread::yield();
    }
    finished.store(true, std::memory_order_release);
    driver.join();
    check(!transport.playing() && !mailbox.playing(),
          "concurrent driver preserves ordered final stop");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
