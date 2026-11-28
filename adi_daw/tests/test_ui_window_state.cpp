// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/store.hpp"
#include "adi/ui/density.hpp"
#include "adi/ui/midi_input_feed.hpp"
#include "adi/ui/midi_ports.hpp"
#include "adi/ui/session_state.hpp"
#include "adi/ui/theme.hpp"
#include "adi/ui/window_state.hpp"
#include "adi/ui/window_place.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdint>
#include <cstdio>
#include <optional>
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
template <class Mailbox>
std::optional<std::uint64_t> settledLocateRevision(const Mailbox &mailbox) noexcept {
    if constexpr (requires { mailbox.settledLocateRevision(); })
        return mailbox.settledLocateRevision();
    else
        // The previous mailbox has no Locate witness and treats every read as settled.
        return std::uint64_t{0};
}

void locateFence() {
    using Command = TransportMailbox::Command;
    TransportMailbox mailbox;
    engine::Transport transport;
    check(settledLocateRevision(mailbox) == 0, "a new mailbox has a settled, neutral Locate revision");
    check(mailbox.post(Command::Play), "queue playback for the Locate fixture");
    mailbox.drain(transport);
    constexpr std::int64_t position = 1024;
    transport.advance(static_cast<std::int32_t>(position));
    mailbox.publish(transport);
    const auto before = transport.revision();

    check(mailbox.post(Command::Locate, position) && mailbox.position() == position && mailbox.playing(),
          "queue a Locate at the already published position while playing");
    check(!settledLocateRevision(mailbox), "a same-position Locate is unsettled before the driver drains it");
    mailbox.publish(transport);
    check(!settledLocateRevision(mailbox), "ordinary position publication cannot acknowledge a queued Locate");
    mailbox.drain(transport);
    check(mailbox.position() == position && transport.revision() == before + 1 &&
              settledLocateRevision(mailbox) == transport.revision(),
          "the acknowledged same-position Locate exposes its applied revision");

    const auto locateInMixedDrain = transport.revision() + 1;
    check(mailbox.post(Command::Locate, position) && mailbox.post(Command::Stop) && mailbox.post(Command::Play),
          "queue Locate followed by Stop and Play in one drain");
    mailbox.drain(transport);
    check(mailbox.playing() && mailbox.position() == position && transport.revision() == locateInMixedDrain + 2 &&
              settledLocateRevision(mailbox) == locateInMixedDrain,
          "the mixed drain exposes the Locate revision before its Stop and Play changes");

    transport.locate(position); // Another driver action, with no mailbox command.
    for (int i = 0; i < 4; ++i)
        mailbox.drain(transport);
    check(transport.revision() > locateInMixedDrain && settledLocateRevision(mailbox) == locateInMixedDrain,
          "repeated empty drains leave the applied Locate witness alone");
    transport.advance(64);
    mailbox.publish(transport);
    check(mailbox.position() == position + 64 && settledLocateRevision(mailbox) == locateInMixedDrain,
          "ordinary position feedback after an empty drain leaves the Locate witness alone");

    check(mailbox.post(Command::Stop) && mailbox.post(Command::Play), "queue playback commands without a Locate");
    mailbox.drain(transport);
    check(mailbox.playing() && settledLocateRevision(mailbox) == locateInMixedDrain,
          "a Stop and Play drain leaves the applied Locate witness alone");

    constexpr std::int64_t recordTick = 720;
    std::int64_t observedRecordTick = -1;
    check(mailbox.post(Command::Record, recordTick), "queue the existing Record command");
    mailbox.drain(transport, [&](std::int64_t tick) noexcept {
        observedRecordTick = tick;
        transport.locate(position);
    });
    check(observedRecordTick == recordTick && settledLocateRevision(mailbox) == locateInMixedDrain,
          "Record still dispatches its tick without replacing the last explicit Locate revision");
}

void laneMarkerTransport() {
    using Command = TransportMailbox::Command;
    TransportMailbox mailbox;
    engine::Transport transport;
    mailbox.post(Command::Locate, 1024);
    mailbox.post(Command::Play);
    mailbox.post(Command::LocateMarker, 4096);
    mailbox.drain(transport, [](std::int64_t) noexcept {},
                  [](std::int64_t) noexcept { return false; });
    check(transport.position() == 1024 && mailbox.desiredPlaying(),
          "SC-02: a lane press cannot move playback waiting to start");
    mailbox.drain(transport);
    transport.advance(64);
    mailbox.post(Command::Stop);
    mailbox.post(Command::LocateMarker, 4096);
    mailbox.post(Command::Continue);
    mailbox.drain(transport);
    check(transport.playing() && transport.position() == 1088,
          "SC-03: Continue uses the position where the driver applied Stop in the same drain");

    mailbox.post(Command::Stop);
    mailbox.post(Command::Record, 2048);
    mailbox.post(Command::LocateMarker, 4096);
    mailbox.drain(transport, [&](std::int64_t start) noexcept {
        transport.locate(start);
        transport.countIn(96000);
    });
    check(transport.position() == 2048 && transport.countInRemaining() == 96000 && mailbox.playing(),
          "SC-02: a lane press queued with Record leaves its count-in in place");

    mailbox.post(Command::Stop);
    mailbox.drain(transport);
    check(!mailbox.playing() && transport.position() == 2048,
          "Stop during count-in cancels the start while keeping its position");
    mailbox.post(Command::Stop);
    mailbox.drain(transport);
    check(mailbox.position() == 0 && transport.position() == 0 && !mailbox.playing(),
          "SC-75: Stop while already stopped locates the driver at the song start");
    mailbox.post(Command::LocateMarker, 8192);
    mailbox.post(Command::Continue);
    mailbox.drain(transport);
    check(transport.playing() && transport.position() == 0,
          "SC-75: a later lane selection does not replace the reset Continue position");
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

// WIN-02: the one placement rule, headless (adi/ui/window_place.hpp). The laptop is 1366 by 768 at
// 125 %: 1093 by 614 units, a 32-unit task bar at the foot.
void placementRule() {
    using namespace window_place;
    const Monitor laptop{"laptop", {0, 0, 1093, 614}, {0, 0, 1093, 582}, 120, true};
    const Monitor wide{"wide", {1093, 0, 1920, 1080}, {1093, 0, 1920, 1040}, 96, false};
    const Monitor left{"left", {-1920, 0, 1920, 1080}, {-1920, 0, 1920, 1040}, 96, false};
    const std::vector<Monitor> one{laptop}, two{laptop, wide};
    // Sanitize: a record's nonsense made a rectangle.
    check(sanitize({5, 5, -10, 0}, 520, 180) == Rect{5, 5, 520, 180} &&
              sanitize({-900000, 900000, 900000, 50}, 1, 1) == Rect{-kLimit, kLimit, kLimit, 50},
          "sanitize: a size below the least grows to it, one past the limit and a far corner come back in");
    // The monitor by the area shown.
    check(monitorOf({1000, 100, 400, 300}, two) == 1 && monitorOf({800, 100, 400, 300}, two) == 0 &&
              monitorOf({9000, 9000, 10, 10}, two) == 0,
          "the monitor showing most of a window is its monitor; one shown nowhere is the main display's");
    // Identity: id, area, origin, the one of the same size and DPI, the main display.
    Place p;
    p.display = "wide";
    check(identify(p, two).index == 1 && !identify(p, two).gone, "a monitor is found by its id");
    p.display = "renamed";
    p.area = {1093, 0, 1920, 1080};
    check(identify(p, two).index == 1, "then by its area");
    p.area = {1093, 0, 2560, 1440};
    check(identify(p, two).index == 1, "then by its origin");
    p.area = {5000, 0, 1920, 1080};
    p.dpi = 96;
    check(identify(p, two).index == 1, "then as the one monitor of its size and DPI");
    check(identify(p, {laptop, wide, left}).gone && identify(p, {laptop, wide, left}).index == 0,
          "two of that size and DPI answer neither: the main display stands in, the place's monitor gone");
    // The laptop: a title bar is never above the screen, nor off its sides or foot.
    Place onLaptop;
    onLaptop.display = "laptop";
    onLaptop.area = laptop.area;
    onLaptop.rect = {200, -40, 700, 400};
    check(restore(onLaptop, one, 520, 180).y == 0, "a window saved above the top opens with its title bar on the screen");
    onLaptop.rect = {200, 600, 700, 400};
    check(restore(onLaptop, one, 520, 180).y == 582 - 18, "one saved below the task bar keeps a grab of its title bar above it");
    onLaptop.rect = {1080, 100, 700, 400};
    check(restore(onLaptop, one, 520, 180).x == 1093 - 40, "one off the right edge keeps 40 units of its title bar on");
    onLaptop.rect = {-690, 100, 700, 400};
    check(restore(onLaptop, one, 520, 180).x == -700 + 40, "and off the left edge the same");
    onLaptop.rect = {700, 100, 700, 400};
    check(restore(onLaptop, one, 520, 180) == Rect{700, 100, 700, 400}, "a window partly off an edge stays where its owner put it");
    onLaptop.rect = {0, 0, 1920, 1080};
    check(restore(onLaptop, one, 520, 180) == Rect{0, 0, 1093, 582}, "a window bigger than the screen opens no bigger than its usable area");
    // Every place a record may hold opens with its title bar reachable on the laptop.
    bool always = true;
    for (int x = -4000; x <= 4000; x += 250)
        for (int y = -3000; y <= 3000; y += 250)
            for (const auto &[w, h] : {std::pair{600, 300}, std::pair{1600, 1000}, std::pair{40, 20}}) {
                Place any;
                any.display = (x + y) % 500 == 0 ? "laptop" : "gone";
                any.area = (x % 1000 == 0) ? Rect{1093, 0, 1920, 1080} : Rect{};
                any.rect = {x, y, w, h};
                const auto r = restore(any, one, 520, 180);
                if (!reachable(r, one) || r.y < 0 || r.w > 1093 || r.h > 582)
                    always = false;
            }
    check(always, "at 1366 x 768 and 125 %, every saved place opens with its title bar on the screen and no larger than it");
    // Restore: same monitor; in proportion when it changed; on the main display when gone.
    Place onWide;
    onWide.display = "wide";
    onWide.area = wide.area;
    onWide.dpi = 96;
    onWide.rect = {1593, 200, 800, 500};
    check(restore(onWide, two, 520, 180) == Rect{1593, 200, 800, 500}, "on its own monitor a window opens where it was");
    const Monitor wideNow{"wide@2560", {1093, 0, 2560, 1440}, {1093, 0, 2560, 1400}, 96, false};
    check(restore(onWide, {laptop, wideNow}, 520, 180) == Rect{1093 + 666, 266, 1066, 666},
          "on its monitor at another resolution, in proportion");
    Place small = onWide;
    small.rect = {1293, 100, 300, 200};
    check(restore(small, one, 100, 100) == Rect{200, 100, 300, 200},
          "with its monitor gone, on the main display at the same offset from its corner");
    check(restore(onWide, one, 520, 180) == Rect{293, 82, 800, 500}, "and wholly on it, where the offset would leave it partly off");
    Place onLeft;
    onLeft.display = "left";
    onLeft.area = left.area;
    onLeft.rect = {-1600, 100, 400, 300};
    check(restore(onLeft, {laptop, left}, 100, 100) == Rect{-1600, 100, 400, 300}, "negative desktop coordinates are kept");
    const Monitor leftMoved{"left", {-2560, 0, 1920, 1080}, {-2560, 0, 1920, 1040}, 96, false};
    check(restore(onLeft, {laptop, leftMoved}, 100, 100).x == -2240, "a monitor moved on the desktop takes its windows with it");
    // Cascade.
    const std::vector<Rect> open{{100, 100, 400, 300}, {122, 122, 400, 300}};
    check(cascade({100, 100, 400, 300}, open, laptop.usable) == Rect{144, 144, 400, 300},
          "a new window at another's corner steps 22 down and right past every one there [P:7.5-floating#15]");
    check(cascade({700, 300, 400, 300}, {{700, 300, 400, 300}}, laptop.usable) == Rect{0, 0, 400, 300},
          "a step past the screen's edge goes back to its corner");
    check(cascade({10, 10, 400, 300}, open, laptop.usable) == Rect{10, 10, 400, 300}, "a free corner is kept");
    // The record.
    const auto kept = placeOf({1593, 200, 800, 500}, true, two);
    const auto back = fromJson(toJson(kept));
    check(kept.display == "wide" && kept.area == wide.area && kept.dpi == 96 && back && back->display == "wide" &&
              back->area == wide.area && back->rect == kept.rect && back->fullScreen && back->dpi == 96,
          "a place reads back from its record as it was written");
    check(!fromJson(nlohmann::json::array()) && !fromJson({{"rect", {1, 2, 3}}}) && !fromJson({{"rect", {1, 2, 0, 5}}}) &&
              !fromJson({{"rect", {1, "2", 3, 4}}}),
          "a record the rule cannot read is none");
    const auto partial = fromJson({{"rect", {10, 20, 300, 200}}});
    check(partial && partial->display.empty() && partial->area.empty() && !partial->fullScreen,
          "a record holding only a rectangle reads with its monitor unknown");
    std::string why;
    const auto *row = settings::find("window.places");
    check(row != nullptr && settings::validate(*row, nlohmann::json{{"main", toJson(kept)}}, why),
          "window.places takes the record as the rule writes it");
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
    // N61: a port's record keeps its name, and follows the port to a new identifier.
    {
        const auto named = withMidiPortSwitch(settings::Value::object(), "in:a", "track", true, "Keys");
        check(named["in:a"]["name"] == "Keys" && settings::validate(*row, named, why),
              "a switch written keeps the port's name with its record (midi.port.name), and the registry takes it");
        auto old = settings::Value::object();
        old["in:a"] = {{"track", true}};
        old["out:a"] = {{"remote", true}};
        const auto stamped = reconcileMidiPorts(old, {{"in:a", "Keys"}, {"out:a", "Keys"}, {"in:b", "Pads"}});
        check(stamped && (*stamped)["in:a"]["name"] == "Keys" && (*stamped)["out:a"]["name"] == "Keys" &&
                  !stamped->contains("in:b") && settings::validate(*row, *stamped, why),
              "a listed port's record is stamped with its name; a port with no record gets none");
        check(!reconcileMidiPorts(*stamped, {{"in:a", "Keys"}, {"out:a", "Keys"}, {"in:b", "Pads"}}),
              "a record that agrees is not written again");
        auto kept = settings::Value::object();
        kept["in:old"] = {{"track", true}, {"mpe", true}, {"name", "Keys"}};
        kept["out:old"] = {{"remote", true}, {"name", "Keys"}};
        const auto moved = reconcileMidiPorts(kept, {{"in:new", "Keys"}, {"out:new", "Keys"}});
        const auto in = moved ? midiPortSwitches(*moved, "in:new") : MidiPortSwitches{};
        check(moved && in.track && in.mpe && midiPortSwitches(*moved, "out:new").remote && !moved->contains("in:old") &&
                  !moved->contains("out:old") && settings::validate(*row, *moved, why),
              "the identifier changes, the name is kept: each direction's record moves to the new identifier, "
              "Track and MPE kept");
        const auto other = reconcileMidiPorts(kept, {{"in:new", "Pads"}});
        check(!other, "a port of another name adopts no record");
        const auto stillThere = reconcileMidiPorts(kept, {{"in:old", "Keys"}, {"in:new", "Keys"}});
        check(!stillThere, "a record whose port is still listed stays with it; the second port of that name gets none");
        const auto twoNew = reconcileMidiPorts(kept, {{"in:new", "Keys"}, {"in:newer", "Keys"}});
        check(!twoNew, "two new ports of one name: which one the record was is unknown, so neither takes it");
        auto pair = kept;
        pair["in:older"] = {{"sync", true}, {"name", "Keys"}};
        const auto ambiguous = reconcileMidiPorts(pair, {{"in:new", "Keys"}});
        check(!ambiguous, "two records of one name and one new port: neither moves");
        auto reused = settings::Value::object();
        reused["in:1"] = {{"track", true}, {"name", "Keys"}};
        check(!reconcileMidiPorts(reused, {{"in:1", "Pads"}}),
              "another device at a record's identifier does not rename it, so its own device can still find it");
        const auto back = reconcileMidiPorts(reused, {{"in:1", "Pads"}, {"in:2", "Keys"}});
        check(back && back->contains("in:2") && (*back)["in:2"].value("name", std::string()) == "Keys" &&
                  midiPortSwitches(*back, "in:2").track && !back->contains("in:1"),
              "the keyboard back on another identifier takes its record from the one the other device now holds");
        auto swapped = settings::Value::object();
        swapped["in:1"] = {{"track", true}, {"name", "Keys"}};
        swapped["in:2"] = {{"mpe", true}, {"name", "Pads"}};
        const auto both = reconcileMidiPorts(swapped, {{"in:1", "Pads"}, {"in:2", "Keys"}});
        check(both && (*both)["in:1"].value("name", std::string()) == "Pads" && midiPortSwitches(*both, "in:1").mpe &&
                  (*both)["in:2"].value("name", std::string()) == "Keys" && midiPortSwitches(*both, "in:2").track &&
                  settings::validate(*row, *both, why),
              "two devices that swapped sockets each keep their own record");
    }
    {
        const settings::Value saved = {
            {"in:manual", {{"track", false}, {"name", "Manual Keys"}}},
            {"in:legacy", {{"remote", true}}},
            {"out:existing", {{"track", true}, {"name", "External Synth"}}},
        };
        const auto first = reconcileMidiPorts(saved, {{"in:old", "Old Input"}, {"in:legacy", "Legacy Input"},
                                                      {"out:old", "Old Input"}}, false);
        check(first && first->at("in:old").at("track") == false && !first->contains("out:old"),
              "discovering a new input saves even an off default and does not change its output");
        const std::vector<MidiPortListing> expanded = {
            {"in:old", "Old Input"}, {"in:legacy", "Legacy Input"}, {"in:moved", "Manual Keys"},
            {"in:new", "New Input"}, {"out:new", "New Input"}, {"out:existing", "External Synth"},
        };
        const auto second = first ? reconcileMidiPorts(*first, expanded, true) : std::nullopt;
        check(second && midiPortSwitches(*second, "in:new").track && !second->contains("out:new") &&
                  midiPortSwitches(*second, "out:existing").track && !midiPortSwitches(*second, "out:new").track &&
                  settings::validate(*row, *second, why),
              "the enabled default reaches a new input while output Track switches keep their prior behavior");
        check(second && !midiPortSwitches(*second, "in:old").track && !second->at("in:legacy").contains("track") &&
                  !midiPortSwitches(*second, "in:moved").track && !second->contains("in:manual"),
              "changing the default preserves seeded off, legacy partial records, and a moved manual off");
        check(!reconcileMidiPorts(saved, {{"in:one", "Manual Keys"}, {"in:two", "Manual Keys"}}, true),
              "an ambiguous saved input is left for the user instead of being enabled by the new default");
    }
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
void computerMidiFeed() {
    using engine::Event;
    using engine::EventType;
    constexpr std::int64_t blockStart = 9600;
    constexpr std::int32_t frames = 480;
    constexpr double rate = 48000;
    constexpr std::uint64_t keyOne = (std::uint64_t{1} << 48) + 1;
    constexpr std::uint64_t keyTwo = keyOne + 1;
    MidiInputFeed feed;
    MidiInputFeed::Ring port;
    const std::array sources{MidiInputFeed::Source{&port, 0, true, false, false, 1}};
    feed.publish(sources);
    const auto wire = [&](double time, std::array<std::uint8_t, 3> bytes) {
        MidiPortEvent event;
        event.timeMs = time;
        event.bytes = bytes;
        event.size = 3;
        return port.push(event);
    };
    check(feed.pushComputerMidi(191, {99, EventType::NoteOn, 1, 60, keyOne, 0, .75}) &&
              feed.pushComputerMidi(196, {0, EventType::NoteOn, 1, 60, keyTwo, 0, .5}) &&
              feed.pushComputerMidi(197, {0, EventType::NoteOff, 1, 60, keyOne, 0, 0}) &&
              feed.pushComputerMidi(198, {0, EventType::NoteOff, 1, 60, keyTwo, 0, 0}) &&
              wire(192, {0x91, 60, 100}) && wire(195, {0x81, 60, 0}),
          "typed computer keys and a physical port can play the same pitch simultaneously");

    struct Heard { std::int64_t sample; Event event; std::int64_t inputSample; };
    std::array<Heard, MidiInputFeed::kMaxPerBlock> heard{};
    std::size_t count = 0;
    // SC-50 supplies the played sample as well. The scheduled clock and ordering are shared.
    const auto collect = [&](std::int64_t sample, const Event &event, std::int64_t inputSample) noexcept {
        if (count == heard.size())
            return false;
        heard[count++] = {sample, event, inputSample};
        return true;
    };
    feed.pump(blockStart, frames, rate, 200, collect);
    const auto is = [&](std::size_t i, std::int64_t offset, EventType type, std::uint64_t id) {
        return heard[i].sample == blockStart + offset && heard[i].inputSample == blockStart + offset - frames &&
               heard[i].event.type == type && heard[i].event.noteId == id;
    };
    const auto physicalId = heard[1].event.noteId;
    check(count == 6 && is(0, 48, EventType::NoteOn, keyOne) && is(1, 96, EventType::NoteOn, physicalId) &&
              physicalId != 0 && physicalId < keyOne && is(2, 240, EventType::NoteOff, physicalId) &&
              is(3, 288, EventType::NoteOn, keyTwo) && is(4, 336, EventType::NoteOff, keyOne) &&
              is(5, 384, EventType::NoteOff, keyTwo) && heard[0].event.channel == 1 &&
              heard[0].event.frame == 0 && heard[0].event.value == .75,
          "the callback orders both sources by the port clock and preserves each computer key's identity and velocity");

    const Event note{0, EventType::NoteOn, 1, 60, keyOne, 0, .75};
    std::size_t filled = 0;
    while (filled < 4096 && feed.pushComputerMidi(201, note))
        ++filled;
    check(filled > 0 && filled < 4096 && wire(202, {0x91, 62, 100}), "computer input has a finite queue and its own overflow");
    count = 0;
    int resets = 0;
    feed.pump(blockStart + frames, frames, rate, 210, collect, [&]() noexcept {
        if (count == 0)
            ++resets;
    });
    check(resets == 1 && count == 1 && heard[0].event.dim == 62,
          "overflow asks to release existing voices before the callback accepts any new input");
    count = 0;
    check(feed.pushComputerMidi(211, note) && feed.pump(blockStart + 2 * frames, frames, rate, 220, collect) == 1 &&
              count == 1 && heard[0].event.noteId == keyOne,
          "the computer input recovers after its old queued notes are discarded");
    count = 0;
    check(feed.pushComputerMidi(221, note), "a computer note is waiting when the device restarts");
    feed.restart();
    check(feed.pump(0, frames, rate, 230, collect) == 0 && count == 0,
          "a device restart discards notes stamped against the previous device clock");
}
} // namespace
int main() {
    themeAndDensityTables();
    locateFence();
    laneMarkerTransport();
    midiPortsCore();
    computerMidiFeed();
    placementRule();
    test::TempDirectory temp("ui_state", "roundtrip");
    StoreError err{};
    auto store = Store::createEmpty(temp.path() / "p.adi", err);
    if (!store)
        return 2;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'UI test')");
    ViewStateStore views(*store);
    std::string error;
    {
        // WIN-02: a floating window's place keeps its monitor's area and DPI, and after a kill (the store
        // opened again, nothing else said) the rule puts it back where it stood.
        FloatingState moved;
        moved.monitor = "wide";
        moved.x = 1593;
        moved.y = 200;
        moved.width = 800;
        moved.height = 500;
        moved.open = true;
        moved.areaX = 1093;
        moved.areaWidth = 1920;
        moved.areaHeight = 1080;
        moved.dpi = 96;
        check(views.saveFloating("Mix View", moved, error), "a moved floating window's place is saved");
        auto reopened = Store::open(temp.path() / "p.adi", err);
        check(reopened != nullptr, "the project opens again after the kill");
        if (reopened) {
            const auto back = ViewStateStore(*reopened).loadFloating("Mix View");
            window_place::Place p{back.monitor, {back.areaX, back.areaY, back.areaWidth, back.areaHeight}, back.dpi,
                           {back.x, back.y, back.width, back.height}, false};
            const std::vector<window_place::Monitor> desk{{"laptop", {0, 0, 1093, 614}, {0, 0, 1093, 582}, 120, true},
                                                   {"wide", {1093, 0, 1920, 1080}, {1093, 0, 1920, 1040}, 96, false}};
            check(back.areaWidth == 1920 && back.dpi == 96 && window_place::restore(p, desk, 100, 100) == window_place::Rect{1593, 200, 800, 500},
                  "a moved floating window keeps its place after a kill");
        }
        store->db().exec("UPDATE ui_view SET value='{\"monitor\":\"m\",\"x\":1,\"y\":2,\"width\":300,\"height\":200,\"open\":true}' WHERE key='floating.Mix View'");
        const auto old = views.loadFloating("Mix View");
        check(old.monitor == "m" && old.width == 300 && old.areaWidth == 0 && old.dpi == 0,
              "a place saved before the area was kept reads with its area unknown");
    }
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
        check(old.follow == WindowState::kDefaultFollow &&
                  views.load("other").follow == WindowState::kDefaultFollow,
              "a fresh window and a saved window without a Follow setting share the default");
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
    auto followOff = restored;
    followOff.follow = false;
    check(restored.follow && views.save("follow-off", followOff, error) && !views.load("follow-off").follow,
          "explicit Follow settings persist independently in each window");
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
              views.load("other").noteGridSnap && views.load("other").noteGridFixed &&
              views.load("other").noteGridFixedQuarters == .25 && views.load("other").highlightScale &&
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
    check(mailbox.post(TransportMailbox::Command::Stop), "queue a Stop while playing");
    for (int i = 1; i < 16; ++i)
        check(mailbox.post(TransportMailbox::Command::Locate, 64), "bounded queue accepts capacity");
    check(!mailbox.post(TransportMailbox::Command::Play), "full queue refuses without overwrite");
    mailbox.drain(transport);
    check(!transport.playing(), "queued stop survives saturation");
    {
        bool ready = false;
        std::int64_t requested = -1;
        const auto apply = [&] {
            mailbox.drain(transport, [](std::int64_t) noexcept {},
                [&](std::int64_t sample) noexcept { requested = sample; return ready; });
        };
        check(mailbox.post(TransportMailbox::Command::Play), "request a cold Play");
        apply();
        transport.advance(64);
        check(requested == 64 && transport.position() == 64 && !mailbox.playing() && mailbox.desiredPlaying(),
              "pending Play holds the timeline and still exposes the intent for Stop");
        check(mailbox.post(TransportMailbox::Command::Locate, 1024), "locate during a pending Play");
        apply();
        check(requested == 1024 && !transport.playing(), "the readiness target follows the latest Locate");
        ready = true;
        apply();
        check(transport.playing() && transport.position() == 1024, "the first running block starts at that Locate");
        mailbox.post(TransportMailbox::Command::Stop);
        mailbox.post(TransportMailbox::Command::Play);
        ready = false;
        apply();
        mailbox.post(TransportMailbox::Command::Stop);
        ready = true;
        apply();
        check(!mailbox.playing() && !mailbox.desiredPlaying(), "Stop cancels a cold Play");
    }
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
