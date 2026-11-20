// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/juce/app_audio_page.hpp"
#include "../src/juce/app_midi_ports.hpp"
#include "../src/juce/ui_export.hpp"
#include "../src/juce/ui_controls.hpp"
#include "../src/juce/ui_floating.hpp"
#include "../src/juce/ui_settings.hpp"
#include "../src/juce/ui_shell.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/cue_settings.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "adi/transport_settings.hpp"
#include "adi/engine/clip_fade.hpp"
#include "adi/ui/fade_edits.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/library_listing.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/ui/recording_edits.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/ui/send_edits.hpp"
#include "knobs_device.hpp"
#include "mix_seek_fixture.hpp"
#include "temp_directory.hpp"
#include "ui_fake_peer.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace {
thread_local bool audit = false;
std::atomic<unsigned> allocations{0};
} // namespace
void *operator new(std::size_t n) {
    if (audit)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
    try {
        return operator new(n);
    } catch (...) {
        return nullptr;
    }
}
void *operator new[](std::size_t n, const std::nothrow_t &t) noexcept { return operator new(n, t); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
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
// One value from the store, or empty when the query finds no row.
std::string value(Store &store, const char *sql) {
    SQLite::Statement q(store.db(), sql);
    return q.executeStep() ? q.getColumn(0).getString() : std::string();
}
OpRequest track(int id) {
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", id}, {"kind", "audio"}, {"name", "Track"}};
    r.label = "Create track";
    return r;
}
// The tracks in the arrangement's order (TrackOrder): each group, then what it holds; the returns,
// then Main, as Live draws them.
std::vector<std::int64_t> laneOrder(AdiRootComponent &root) {
    std::vector<std::int64_t> out;
    for (const auto row : root.arrangement.geometry.rows(root.reader()))
        out.push_back(row.track->id);
    return out;
}
// Where a track's lane begins in the canvas, scrolled, or at the foot for the returns and Main; -1
// for a track with no row.
int laneTop(AdiRootComponent &root, std::int64_t id) {
    return root.arrangement.geometry.rowTop(root.reader(), id);
}
void reference(AdiRootComponent &root, const char *name, bool write) {
    // Explicit software rasterisation, no peer, OpenGL, display, or audio device.
    juce::Image image(juce::Image::ARGB, root.getWidth(), root.getHeight(), true,
                      juce::SoftwareImageType{});
    {
        juce::Graphics g(image);
        root.paintEntireComponent(g, true);
    }
    const auto output =
        juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(name) + "-actual.png");
    {
        auto stream = output.createOutputStream();
        check(stream != nullptr, "open render file");
        if (stream) {
            stream->setPosition(0);
            stream->truncate();
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
        }
    }
    // Platform font metrics and native button fonts are deliberately outside this
    // structural golden. Their labels and activation are checked independently.
    {
        juce::Graphics g(image);
        g.setColour(juce::Colours::black);
        g.fillRect(8, 5, 190, 34);
        g.fillRect(228, 5, root.getWidth() - 228, 34);
        g.fillRect(0, 54, root.getWidth(), 30);
        // The ruler's numbers and the headers' track names are typography as well.
        const auto area = root.arrangement.getBounds();
        // So are the clips' names, in their title strips.
        {
            const auto canvas = root.arrangement.canvas.getBounds() + area.getPosition();
            const auto &geo = root.arrangement.geometry;
            g.saveState();
            g.reduceClipRegion(canvas);
            for (const auto row : geo.rows(root.reader())) {
                const int rowTop = geo.rowTop(root.reader(), row.track->id);
                if (rowTop < 0)
                    continue;
                const int top = canvas.getY() + rowTop;
                for (const auto &c : row.track->clips) {
                    const int x0 = canvas.getX() + static_cast<int>(geo.x(c->posTicks));
                    const int x1 = canvas.getX() + static_cast<int>(geo.x(c->posTicks + c->lengthTicks));
                    if (x1 > x0)
                        g.fillRect(x0, top + 1, x1 - x0, ArrangementCanvas::kClipBarHeight);
                }
            }
            g.restoreState();
        }
        g.fillRect(area.getX(), area.getY() + ArrangementView::kOverviewHeight, area.getWidth(),
                   ArrangementView::kRulerHeight);
        const auto headers = root.arrangement.headers.getBounds() + area.getPosition();
        for (const auto row : root.arrangement.geometry.rows(root.reader()))
            if (const int top = root.arrangement.geometry.rowTop(root.reader(), row.track->id); top >= 0) {
                g.fillRect(headers.getX() + 6, headers.getY() + top + 4, std::max(0, headers.getWidth() - 8), 24);
                // A MIDI track's Rec-Q and Play-Q boxes hold text too: their place, not their glyphs.
                const int height = root.arrangement.geometry.heightFor(row.track->id);
                for (const bool record : {true, false})
                    if (adi::ui::isMidiTrack(*row.track))
                    g.fillRect(root.arrangement.headers.quantizeBox(row.place.depth, top, height, record) +
                               headers.getPosition());
            }
        const auto devices = root.deviceArea();
        if (!devices.isEmpty())
            g.fillRect(devices.getX(), devices.getY() + 10, devices.getWidth(), 30);
        // The Status Bar's message and button, the AI tab's label and the Info
        // View's text are typography too; their geometry is checked by bounds.
        g.fillRect(root.status.getBounds().withTrimmedTop(1));
        if (root.aiTab.isVisible())
            g.fillRect(root.aiTab.getBounds());
        if (!root.infoArea().isEmpty())
            g.fillRect(root.infoArea().reduced(4));
        for (const auto &strip : root.mixerView.strips()) {
            if (!strip->isVisible() || !root.mixerView.isVisible())
                continue;
            const auto b = root.getLocalArea(strip.get(), strip->getLocalBounds());
            g.fillRect(b.getX(), b.getY(), b.getWidth(), 60);
            // Live's scale beside the meter, and the words on the strip's switches.
            g.fillRect(b.getX() + 72, b.getY(), b.getWidth() - 72, b.getHeight());
            for (juce::Component *c : std::initializer_list<juce::Component *>{
                     &strip->active, &strip->solo, &strip->peakHold})
                g.fillRect(root.getLocalArea(c, c->getLocalBounds()));
        }
        for (const auto &strip : root.mixer.strips()) {
            if (!strip->isVisible())
                continue;
            for (juce::Component *c : std::initializer_list<juce::Component *>{
                     &strip->active, &strip->solo, &strip->volume, &strip->pan})
                g.fillRect(root.getLocalArea(c, c->getLocalBounds()));
            // The In/Out column's choosers are words.
            if (strip->showsInOut())
                g.fillRect(root.getLocalArea(strip.get(), strip->getLocalBounds())
                               .withWidth(MixerPanel::kInOutWidth));
        }
        // Live's look puts words in more places: the browser's lists and search, the
        // time ruler, "Set", H and W, the right-hand strip's switches, the drop areas.
        if (root.browser.isVisible()) {
            const auto at = root.browser.getPosition();
            g.fillRect(root.browser.sidebarArea() + at);
            g.fillRect(root.browser.search.getBounds() + at);
            g.fillRect(root.browser.contentArea().withHeight(19) + at);
        }
        g.fillRect(area.getX(), area.getBottom() - ArrangementView::kTimeRulerHeight, area.getWidth(),
                   ArrangementView::kTimeRulerHeight);
        g.fillRect(headers.getX() + 4, area.getY() + ArrangementView::kOverviewHeight +
                                           ArrangementView::kRulerHeight - 3,
                   32, 15);
        const auto canvas = root.arrangement.canvas.getBounds() + area.getPosition();
        {
            const auto &geo = root.arrangement.geometry;
            const auto rowsShown = geo.rows(root.reader());
            const int dropTop = canvas.getY() + geo.tracksHeight(root.reader()) - geo.scrollY;
            const int dropBottom = geo.footStart(rowsShown) < rowsShown.size()
                                       ? canvas.getY() + geo.footTop(root.reader())
                                       : canvas.getBottom();
            if (dropBottom > dropTop)
                g.fillRect(canvas.getX(), dropTop, canvas.getWidth(), dropBottom - dropTop);
        }
        if (root.mixer.isVisible() && !root.mixer.vertical()) {
            const auto m = root.mixer.getBounds();
            g.fillRect(m.getRight() - 44, m.getY() + 3, 42, 17);
        }
        if (root.strip.isVisible())
            g.fillRect(root.stripArea());
        int chainEnd = root.devices.getX();
        for (const auto &device : root.devices.panels) {
            const auto b = root.getLocalArea(device.get(), device->getLocalBounds());
            chainEnd = std::max(chainEnd, b.getRight());
            g.fillRect(b.getX() + 30, b.getY() + 1, std::max(0, b.getWidth() - 31), 16);
            for (const auto &c : device->controls) {
                const auto cb = root.getLocalArea(c.get(), c->getLocalBounds());
                g.fillRect(cb.getX(), cb.getY(), cb.getWidth(), 14);
                g.fillRect(cb.getX(), cb.getY() + 39, cb.getWidth(), 14);
                if (c->menu.isVisible())
                    g.fillRect(cb.getX() + 2, cb.getY() + 14, cb.getWidth() - 4, 18);
            }
        }
        if (root.devices.isVisible())
            g.fillRect(root.devices.getBounds().withTrimmedLeft(
                std::max(0, chainEnd - root.devices.getX())));
    }
    const auto file = juce::File(ADI_UI_REFERENCES).getChildFile(juce::String(name) + ".png");
    if (write) {
        file.getParentDirectory().createDirectory();
        auto stream = file.createOutputStream();
        check(stream != nullptr, "write reference");
        if (stream) {
            stream->setPosition(0);
            stream->truncate();
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
        }
        return;
    }
    auto expected = juce::ImageFileFormat::loadFrom(file);
    check(expected.isValid(), "reference exists");
    if (!expected.isValid())
        return;
    bool equal =
        expected.getWidth() == image.getWidth() && expected.getHeight() == image.getHeight();
    int differences = 0;
    if (equal)
        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
                if (image.getPixelAt(x, y) != expected.getPixelAt(x, y))
                    ++differences;
    check(equal && differences == 0, name);
    if (differences) {
        std::printf("%s: %d differing pixels\n", name, differences);
        const auto diagnostic = output.getSiblingFile(juce::String(name) + "-masked-actual.png");
        if (auto stream = diagnostic.createOutputStream()) {
            stream->setPosition(0);
            stream->truncate();
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
        }
    }
}
// A menu as the window's look draws one, at twice its size, for a picture beside Live's: a
// PopupMenu shows only on the desktop, which the tests never touch, so its rows are drawn here
// through the look's own menu methods, none highlighted.
void renderMenu(juce::LookAndFeel &look, const juce::PopupMenu &menu, const juce::File &file) {
    struct Row {
        juce::String text, shortcut;
        bool separator = false, enabled = true, ticked = false, subMenu = false;
        int height = 0;
    };
    std::vector<Row> rows;
    int width = 0, height = 0;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
        const auto &item = it.getItem();
        Row row{item.text, item.shortcutKeyDescription, item.isSeparator, item.isEnabled, item.isTicked,
                item.subMenu != nullptr, 0};
        // A command item's key text is filled only as the menu shows (JUCE's
        // ItemComponent::updateShortcutKeyDescription), so it is built here the same way.
        if (row.shortcut.isEmpty() && item.commandManager != nullptr && item.itemID != 0) {
            juce::StringArray keys;
            for (const auto &press : item.commandManager->getKeyMappings()->getKeyPressesAssignedToCommand(item.itemID)) {
                const auto key = press.getTextDescriptionWithIcons();
                keys.add(key.length() == 1 && key[0] < 128 ? "shortcut: '" + key + "'" : key);
            }
            row.shortcut = keys.joinIntoString(", ").trim();
        }
        int w = 0;
        look.getIdealPopupMenuItemSize(row.text, row.separator, 0, w, row.height);
        if (row.shortcut.isNotEmpty())
            w += 12 + juce::roundToInt(juce::GlyphArrangement::getStringWidth(look.getPopupMenuFont(), row.shortcut));
        width = std::max(width, w);
        height += row.height;
        rows.push_back(std::move(row));
    }
    juce::Image image(juce::Image::ARGB, 2 * width, 2 * height, true, juce::SoftwareImageType{});
    {
        juce::Graphics g(image);
        g.addTransform(juce::AffineTransform::scale(2.f));
        look.drawPopupMenuBackground(g, width, height);
        int y = 0;
        for (const auto &row : rows) {
            look.drawPopupMenuItem(g, {0, y, width, row.height}, row.separator, row.enabled, false, row.ticked,
                                   row.subMenu, row.text, row.shortcut, nullptr, nullptr);
            y += row.height;
        }
    }
    file.deleteFile();
    if (auto stream = file.createOutputStream())
        juce::PNGImageFormat{}.writeImageToStream(image, *stream);
}
// Live parity (ADR-0203 d5): Live 11's default set as the director's capture shows it
// (collab/window/look-live11.md): four tracks, the fourth selected with EQ Eight and
// Auto Filter on it, at 1440 x 900 points. The golden is this scene; ADI_UI_PARITY
// names a folder for a 2x render to lay beside the capture.
void liveParity(AppCommands &commands, const std::filesystem::path &dir, bool write) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "parity.adi", err);
    check(store != nullptr, "the parity project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Parity')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    for (int id = 1; id <= 4; ++id) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id},
                     {"kind", id <= 2 ? "midi" : "audio"},
                     {"name", id <= 2 ? "# MIDI" : "# Audio"}};
        r.label = "Create track";
        check(submitter.submit(std::move(r)).ok, "Live's default tracks");
    }
    auto publication = std::make_shared<ParameterPublication>();
    struct Knob {
        const char *name, *text;
        double value;
    };
    const std::array<std::pair<const char *, std::vector<Knob>>, 2> devices{
        {{"EQ Eight", {{"Freq", "10.0 Hz", 0}, {"Gain", "0.00 dB", .5}, {"Q", "0.71", .25}}},
         {"Auto Filter",
          {{"Freq", "12.5 kHz", .9}, {"Res", "14 %", .14}, {"Attack", "6.00 ms", .2},
           {"Release", "200 ms", .5}, {"Amount", "0.00", 0}, {"Rate", "0.11 Hz", .1},
           {"Phase", "0.00\u00b0", 0}}}}};
    std::int64_t deviceId = 21;
    for (const auto &[name, knobs] : devices) {
        DevicePanelData data;
        data.id = deviceId++;
        data.track = 4;
        data.name = name;
        for (const auto &k : knobs) {
            panel::Record record;
            record.id = k.name;
            record.name = k.name;
            record.playing = record.stored = k.value;
            record.shape = panel::Shape::Continuous;
            record.text = k.text;
            data.records.push_back(record);
            data.resolved.entries.push_back({record.id, false});
        }
        panel::finalize(data.records);
        publication->push_back(data);
    }
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "parity");
    root.devices.publication = [publication] { return publication; };
    root.setSize(1440, 900);
    // Live's default zoom: a bar is 15.67 points.
    root.arrangement.geometry.scale = 3.92;
    root.arrangement.geometry.selection.track = 4;
    root.frame();
    root.frame();
    // The capture's mouse rests on the lanes: the Info View describes them.
    root.hover(&root.arrangement.canvas);
    check(root.arrangement.headers.getWidth() == ArrangementView::kHeaderWidth &&
              root.mixer.getWidth() + root.strip.getWidth() == MixerPanel::kFullWidth &&
              root.devices.panels.size() == 2,
          "the parity scene has Live's columns and the fourth track's two devices");
    reference(root, "live-parity", write);
    std::fputs("[shell] liveParity render returned\n", stderr);
    const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
    auto render = [&root, folder](const char *name) {
        if (folder.isEmpty())
            return;
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true,
                          juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile(name);
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
    };
    render("adi-parity.png");
    if (folder.isNotEmpty()) {
        StackTab eqView(root);
        eqView.setSize(900, 460);
        SnapshotReader eqReader(view.current());
        eqView.frame(eqReader);
        eqView.focus(4);
        juce::Image image(juce::Image::ARGB, 2 * eqView.getWidth(), 2 * eqView.getHeight(), true,
                          juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            eqView.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-stack.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
    }
    // The same set with Live's mixer open, to lay beside Live 11's Session mixer.
    check(root.command(AppCommands::ToggleMixerView), "the parity scene's Mixer View opens");
    root.resizeMixerView(220);
    root.mark(DirtySet::All);
    root.frame();
    render("adi-parity-mixer.png");
    std::fputs("[shell] liveParity Mixer View frame returned\n", stderr);
    // Bitwig's project view (the director, 2026-10-28), given a tree as the snapshot will give
    // it once it carries groups (N6): track 2 is a group holding 3, a group holding 4; 1 is Main.
    root.arrangement.setTrackRows([](const SnapshotReader &, std::vector<TrackRow> &rows) {
        rows = {{1, 0, false, true, 0, "Main", 0},
                {2, 0, true, false, 0xe080c0, "Body", 0},
                {3, 2, true, false, 0x4080ff, "Fx", 0},
                {4, 3, false, false, 0, "Hits", 0}};
    });
    auto items = [&root] {
        std::vector<std::pair<juce::String, bool>> out;
        const auto menu = root.arrangement.projectViewMenu();
        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
            out.emplace_back(it.getItem().text, it.getItem().isTicked);
        return out;
    };
    auto choose = [&root](const juce::String &name) {
        const auto menu = root.arrangement.projectViewMenu();
        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
            if (it.getItem().text == name && it.getItem().action)
                it.getItem().action();
        root.mark(DirtySet::All);
        root.frame();
        root.frame();
    };
    auto shownRows = [](const MixerPanel &panel) {
        std::vector<std::int64_t> out;
        for (const auto &s : panel.strips())
            if (s->isVisible())
                out.push_back(s->track());
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto before = items();
    std::fputs("[shell] liveParity project menu read\n", stderr);
    check(before.size() == 5 && before[0].first == "Project" && before[0].second && before[1].first == "Body" &&
              before[2].first == "Fx" && !before[2].second && !root.namesBar.show.getToggleState(),
          "the project view's menu: the whole project, ticked, then its groups as a tree");
    check(before[4].first == "Reference Track" && !before[4].second,
          "and, after them, the Reference track's toggle, off (ADR-0209 d11: hidden by default)");
    auto &lanes = root.arrangement.geometry;
    lanes.selection.track = 2;
    choose("Fx");
    bool passesOver = true;
    for (int y = 0; y < root.arrangement.canvas.getHeight(); ++y)
        if (const auto *t = lanes.trackAt(root.reader(), y); t != nullptr && t->id == 2)
            passesOver = false;
    check(root.state().viewGroup == 3 && root.namesBar.show.getToggleState() &&
              root.namesBar.show.getButtonText() == "Fx" &&
              lanes.hidden == std::vector<std::int64_t>{2} && lanes.heightFor(2) == 0 && passesOver &&
              lanes.selection.track == 3 && items()[2].second,
          "Fx chosen: Body is left out of the lanes, Main kept, Show lit and saying Fx, the selection on Fx");
    render("adi-parity-project-view.png");
    check(shownRows(root.mixer) == std::vector<std::int64_t>{1, 3, 4} &&
              shownRows(root.mixerView) == std::vector<std::int64_t>{1, 3, 4},
          "the track controls and the Mixer View show the same tracks as the lanes");
    bool contiguous = true;
    for (const auto &s : root.mixerView.strips())
        if (s->isVisible() && s->getX() % MixerPanel::kStripWidth != 0)
            contiguous = false;
    check(contiguous, "the Mixer View closes up the gap a hidden track leaves");
    lanes.selection.track = 3;
    check(root.arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) &&
              lanes.selection.track == 3,
          "up from Fx stays: Body, above it, is left out");
    lanes.selection.track = 4;
    check(root.arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) &&
              lanes.selection.track == 1,
          "down from Hits reaches Main, last as Live draws it");
    // The tree changes under the view: Fx is no longer a group, so the whole project shows.
    root.arrangement.setTrackRows([](const SnapshotReader &, std::vector<TrackRow> &rows) {
        rows = {{1, 0, false, true, 0, "Main", 0},
                {2, 0, true, false, 0xe080c0, "Body", 0},
                {3, 2, false, false, 0, "Fx", 0},
                {4, 2, false, false, 0, "Hits", 0}};
    });
    check(root.state().viewGroup == 0 && lanes.hidden.empty() && !root.namesBar.show.getToggleState(),
          "a chosen group that is gone gives way to the whole project");
    choose("Body");
    check(root.state().viewGroup == 2 && lanes.hidden.empty(), "a group at the top shows everything inside it");
    choose("Project");
    check(root.state().viewGroup == 0 && lanes.hidden.empty() && !root.namesBar.show.getToggleState() &&
              shownRows(root.mixer) == std::vector<std::int64_t>{1, 2, 3, 4},
          "Project shows every track again, the button unlit");
    std::fputs("[shell] liveParity before teardown\n", stderr);
}
// A menu on one line: its items in order, a disabled one dashed, a ticked one starred when `ticks`
// is set, a separator a bar, so a check sees Live's groups too.
juce::String menuLine(const juce::PopupMenu &menu, bool ticks = true) {
    juce::String out;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
        const auto &item = it.getItem();
        if (item.isSeparator) {
            out << " | ";
            continue;
        }
        if (out.isNotEmpty() && !out.endsWith(" | "))
            out << ", ";
        out << item.text << (ticks && item.isTicked ? " *" : "") << (item.isEnabled ? "" : " -");
    }
    return out;
}
// A mouse press at a point of a component, for its own mouseDown.
juce::MouseEvent pressAt(juce::Component &c, juce::Point<float> at, juce::ModifierKeys mods = {}) {
    const auto now = juce::Time::getCurrentTime();
    return {juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.f, 0.f, 0.f, 0.f, 0.f, &c, &c, now, at,
            now, 1, false};
}
// The mouse at `at` in a drag that began at `from`.
juce::MouseEvent dragAt(juce::Component &c, juce::Point<float> from, juce::Point<float> at, juce::ModifierKeys mods = {}) {
    const auto now = juce::Time::getCurrentTime();
    return {juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.f, 0.f, 0.f, 0.f, 0.f, &c, &c, now, from,
            now, 1, true};
}
// Live's group tracks (manual 18.3) in the director's Bitwig project (Master Reference 10,
// Figure 32): Drums holds Clap/Snare/Rimshot and Hats & Percs; Body holds Bass, Synths and
// Fx And Hits, which holds Rises and Hits; Main. The tree from the snapshot (#299), as the
// application reads it.
void groupTracks(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "groups.adi", err);
    check(store != nullptr, "the groups project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Groups')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    struct Made {
        std::int64_t id;
        const char *kind, *name;
        std::int64_t parent, index;
    };
    // Each in its place in what holds it, so the snapshot's order (by that place alone) mixes
    // one group's tracks with another's.
    const std::array<Made, 10> made{{{1, "master", "Main", 0, 0},
                                      {10, "group", "Drums", 0, 0},
                                      {11, "audio", "Clap/Snare/Rimshot", 10, 0},
                                      {12, "audio", "Hats & Percs", 10, 1},
                                      {13, "group", "Body", 0, 1},
                                      {14, "midi", "Bass", 13, 0},
                                      {15, "midi", "Synths", 13, 1},
                                      {16, "group", "Fx And Hits", 13, 2},
                                      {17, "audio", "Rises", 16, 0},
                                      {18, "audio", "Hits", 16, 1}}};
    bool madeAll = true;
    for (const auto &m : made) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", m.id}, {"kind", m.kind}, {"name", m.name}, {"index", m.index}};
        if (m.parent != 0)
            r.payload["parent"] = m.parent;
        r.label = "Create track";
        madeAll = submitter.submit(std::move(r)).ok && madeAll;
    }
    std::int64_t clipId = 100;
    for (const std::int64_t track : {11, 12, 14, 15, 17, 18}) {
        OpRequest clip;
        clip.opType = "clip.create";
        const auto bars = track % 3 + 2;
        clip.payload = {{"id", clipId++},
                        {"track", track},
                        {"kind", track == 14 || track == 15 ? "midi" : "audio"},
                        {"name", track == 11   ? "Claps"
                                 : track == 12 ? "Hats Loop"
                                 : track == 14 ? "Sub Line"
                                 : track == 15 ? "Pad Chords"
                                 : track == 17 ? "Riser"
                                               : "Impact"},
                        {"pos", (track % 2) * 4 * textproj::kPPQ},
                        {"length", bars * 4 * textproj::kPPQ}};
        clip.label = "Create clip";
        madeAll = submitter.submit(std::move(clip)).ok && madeAll;
    }
    check(madeAll, "the director's project: three groups, one inside another, and clips in them");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "groups");
    root.setSize(1440, 900);
    root.arrangement.geometry.scale = 3.92;
    // Every track in sight: ten at 40 points.
    root.arrangement.geometry.resizeTrack(0, 40, true);
    root.arrangement.geometry.selection.track = 14;
    root.mark(DirtySet::All);
    root.frame();
    root.frame();
    auto &lanes = root.arrangement.geometry;
    auto order = [&root, &lanes] {
        std::vector<std::pair<std::int64_t, std::uint32_t>> out;
        for (const auto row : lanes.rows(root.reader()))
            out.emplace_back(row.track->id, row.place.depth);
        return out;
    };
    const std::vector<std::pair<std::int64_t, std::uint32_t>> tree{
        {10, 0}, {11, 1}, {12, 1}, {13, 0}, {14, 1}, {15, 1}, {16, 1}, {17, 2}, {18, 2}, {1, 0}};
    bool mixed = false;
    const auto &snapshot = root.reader().tracks();
    for (std::size_t i = 0; i < snapshot.size(); ++i)
        mixed = mixed || snapshot[i]->id != tree[i].first;
    check(mixed && order() == tree,
          "the lanes in Live's order: each group, then what it holds, at its depth; Main last");
    check(lanes.trackAt(root.reader(), 1)->id == 10, "the first lane is the first group's");
    auto shownStrips = [](const MixerPanel &panel) {
        std::vector<std::int64_t> out;
        for (const auto &s : panel.strips())
            if (s->isVisible())
                out.push_back(s->track());
        std::sort(out.begin(), out.end());
        return out;
    };
    auto topOf = [&root, &lanes](std::int64_t id) {
        int y = -lanes.scrollY;
        for (const auto row : lanes.rows(root.reader())) {
            if (row.track->id == id)
                return y;
            y += lanes.heightFor(row.track->id);
        }
        return -1;
    };
    // The group lane: Live's overview of the clips of everything the group holds.
    {
        auto &canvas = root.arrangement.canvas;
        juce::Image image(juce::Image::ARGB, canvas.getWidth(), canvas.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            canvas.paintEntireComponent(g, true);
        }
        // Body's first row of the overview is Bass's, whose clip starts at the start.
        const auto body = root.arrangement.colourFor(3).brighter(0.2f);
        const int x = static_cast<int>(lanes.x(0)) + 2, y = topOf(13) + 4;
        check(image.getPixelAt(x, y) == body && image.getPixelAt(x, topOf(14) + 4) != body,
              "a group's lane shows the clips of what it holds, in its colour");
    }
    // A group's fold switch: what it holds leaves the lanes and the track controls, not the
    // Mixer View; the click selects nothing.
    {
        auto &headers = root.arrangement.headers;
        const auto at = headers.foldSwitch(1, topOf(16)).getCentre();
        headers.mouseDown(pressAt(headers, at));
        headers.mouseUp(pressAt(headers, at));
        root.mark(DirtySet::All);
        root.frame();
        check(root.state().foldedGroups == std::set<std::int64_t>{16} && lanes.heightFor(17) == 0 &&
                  lanes.heightFor(18) == 0 && lanes.heightFor(16) > 0 && lanes.selection.track == 14,
              "Fx And Hits's switch folds it: Rises and Hits leave the lanes; the selection stays");
        const auto strips = shownStrips(root.mixer);
        check(std::find(strips.begin(), strips.end(), 17) == strips.end() &&
                  std::find(strips.begin(), strips.end(), 16) != strips.end(),
              "and the track controls, row for row");
        check(root.command(AppCommands::ToggleMixerView), "the Mixer View opens");
        root.resizeMixerView(220);
        root.mark(DirtySet::All);
        root.frame();
        const auto columns = shownStrips(root.mixerView);
        check(std::find(columns.begin(), columns.end(), 17) != columns.end(),
              "the Mixer View still shows them: folding is the timeline's (UI-ARCHITECTURE §4)");
        check(root.command(AppCommands::ToggleMixerView), "the Mixer View closes");
        lanes.selection.track = 16;
        check(root.arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) && lanes.selection.track == 1,
              "down from a folded group passes over what it holds");
        const auto reloaded = persistence.load("groups", {});
        check(reloaded.foldedGroups == std::set<std::int64_t>{16}, "the fold is kept with the window's view");
        headers.mouseDown(pressAt(headers, at));
        root.frame();
        check(root.state().foldedGroups.empty() && lanes.heightFor(17) > 0, "a second click unfolds it");
        // A plain track's Unfold Track button shows its automation lanes, which wait for the engine.
        lanes.selection.track = 16;
        const auto bassUnfold = headers.foldSwitch(1, topOf(14)).getCentre();
        headers.mouseDown(pressAt(headers, bassUnfold));
        headers.mouseUp(pressAt(headers, bassUnfold));
        root.frame();
        check(root.state().foldedGroups.empty() && lanes.selection.track == 16 &&
                  root.error() == ArrangementView::kUnfoldWaits,
              "Bass's Unfold Track button folds nothing, selects nothing and says it waits for automation (N26)");
    }
    // The keys: U folds the selected group, Alt+U unfolds them all (§42.16, §42.19); a selected
    // track folded away gives the selection to its group.
    {
        auto &canvas = root.arrangement.canvas;
        lanes.selection.track = 18;
        root.arrangement.toggleFold(13);
        check(lanes.selection.track == 13 && lanes.heightFor(14) == 0 && lanes.heightFor(16) == 0 &&
                  lanes.heightFor(18) == 0,
              "Body folded: everything in it leaves, Fx And Hits's tracks too; Hits's selection goes to Body");
        lanes.selection.track = 10;
        check(canvas.keyPressed(juce::KeyPress('u')) && root.state().foldedGroups == std::set<std::int64_t>{10, 13},
              "U folds the selected group");
        check(canvas.keyPressed(juce::KeyPress('u')) && root.state().foldedGroups == std::set<std::int64_t>{13},
              "and unfolds it");
        lanes.selection.track = 1;
        root.say({});
        check(canvas.keyPressed(juce::KeyPress('u')) && root.state().foldedGroups == std::set<std::int64_t>{13} &&
                  root.error() == ArrangementView::kUnfoldWaits,
              "U on a track that is not a group folds nothing, and says its lanes wait (N26)");
        // A view of a group inside a folded one shows it: the folded group is left out.
        root.arrangement.chooseView(16);
        check(lanes.heightFor(16) > 0 && lanes.heightFor(17) > 0 && lanes.heightFor(13) == 0 && lanes.heightFor(1) > 0,
              "Fx And Hits viewed alone inside folded Body: it shows, with its tracks and Main");
        root.arrangement.chooseView(0);
        check(canvas.keyPressed(juce::KeyPress('u', juce::ModifierKeys::altModifier, 0)) &&
                  root.state().foldedGroups.empty() && lanes.heightFor(14) > 0,
              "Alt+U unfolds every group");
    }
    // On the bar over the names: Automation Mode switches on and off (AU-01, its own test below); Lock
    // Envelopes still waits for the engine, and a click says what for (the director's R6, 2 November).
    {
        root.say({});
        root.namesBar.lock.onClick();
        check(root.error().rfind("Lock Envelopes waits for the engine's automation", 0) == 0,
              "a click on Lock Envelopes says what it waits for");
        root.namesBar.automation.onClick();
        check(root.automationMode(), "a click on Automation Mode turns it on");
        root.namesBar.automation.onClick();
        check(!root.automationMode(), "and off");
    }
    // Show says the whole project while no group is chosen.
    check(!root.namesBar.show.getToggleState() &&
              root.namesBar.show.getButtonText() == ProjectViewButton::kAllGroups,
          "Show says All groups while the whole project shows");
    // The tree is the snapshot's: a new generation with the same rows costs a frame nothing, groups
    // and all, and the order is built again in place.
    {
        check(root.submitEdit("track.rename", {{"id", 14}, {"name", "Sub Bass"}}, "Rename Track"), "a rename");
        root.frame();
        check(order() == tree, "an edit keeps the tree, read from the snapshot");
        check(root.submitEdit("track.rename", {{"id", 14}, {"name", "Low Bass"}}, "Rename Track"),
              "a new generation with the same rows");
        allocations = 0;
        audit = true;
        root.frame();
        audit = false;
        check(allocations == 0 && order() == tree, "with groups, a generation-change frame allocates zero");
        std::vector<TrackRow> rows = root.arrangement.tree().rows();
        allocations = 0;
        audit = true;
        lanes.order.build(root.reader(), rows);
        audit = false;
        check(allocations == 0 && order() == tree, "the same tree's order is built again without allocating");
    }
    // The names: a band in each group's colour, the switch, then the track's own colour.
    {
        auto &headers = root.arrangement.headers;
        check(headers.nameLeft(0) == 2 && headers.nameLeft(2) == 2 + 2 * ArrangementView::kGroupBand,
              "a track sits past one band for each group it is in");
        juce::Image image(juce::Image::ARGB, headers.getWidth(), headers.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            headers.paintEntireComponent(g, true);
        }
        const auto drums = root.arrangement.colourFor(0), body = root.arrangement.colourFor(3),
                   fx = root.arrangement.colourFor(6), hits = root.arrangement.colourFor(8);
        const int y = topOf(18) + 20;
        check(image.getPixelAt(3, topOf(11) + 20) == drums && image.getPixelAt(3, y) == body &&
                  image.getPixelAt(3 + ArrangementView::kGroupBand, y) == fx &&
                  image.getPixelAt(headers.nameLeft(2) + 1, y) == hits,
              "Hits's header: Body's band, Fx And Hits's band, then its own colour");
    }
    const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
    auto render = [&root, folder](const char *name) {
        if (folder.isEmpty())
            return;
        root.mark(DirtySet::All);
        root.frame();
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true,
                          juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile(name);
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
    };
    // Live's Group Tracks and Ungroup Tracks (manual 18.3; Ctrl/Cmd+G, Ctrl/Cmd+Shift+G, §41.5):
    // each one batch of ops, one undo step, the tracks where they stood.
    {
        const auto cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;
        auto enabled = [&commands, &root](int id) {
            commands.activate(root);
            juce::ApplicationCommandInfo info(id);
            commands.getCommandInfo(id, info);
            return (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0;
        };
        auto refresh = [&root] {
            root.mark(DirtySet::All);
            root.frame();
        };
        auto trackNamed = [&root](std::int64_t id) -> const engine::TrackNode * {
            for (const auto &t : root.reader().tracks())
                if (t->id == id)
                    return t.get();
            return nullptr;
        };
        lanes.selection.track = 15;
        check(enabled(AppCommands::GroupTracks) && !enabled(AppCommands::UngroupTracks),
              "on Synths, Group Tracks is offered and Ungroup Tracks is not");
        check(root.keyPressed(juce::KeyPress('g', cmd, 0)), "Ctrl/Cmd+G groups the selected track");
        refresh();
        const std::vector<std::pair<std::int64_t, std::uint32_t>> grouped{
            {10, 0}, {11, 1}, {12, 1}, {13, 0}, {14, 1}, {19, 1}, {15, 2}, {16, 1}, {17, 2}, {18, 2}, {1, 0}};
        const auto *newGroup = trackNamed(19);
        check(order() == grouped && newGroup != nullptr && newGroup->kind == "group" && newGroup->name == "# Group" &&
                  newGroup->parent == 13,
              "a new group stands where Synths stood in Body, with Synths in it");
        check(lanes.selection.track == 19 && root.undoTitle(false) == "Undo Group Tracks",
              "the new group is selected, and the Edit menu names the step");
        check(enabled(AppCommands::UngroupTracks), "on a group, Ungroup Tracks is offered");
        check(root.command(AppCommands::Undo), "undo");
        refresh();
        check(order() == tree && trackNamed(19) == nullptr, "one undo step puts everything back");
        check(root.command(AppCommands::Redo), "redo");
        refresh();
        check(order() == grouped, "and redo groups it again");
        lanes.selection.track = 19;
        check(root.keyPressed(juce::KeyPress('g', cmd | shift, 0)), "Ctrl/Cmd+Shift+G ungroups the selected group");
        refresh();
        check(order() == tree && trackNamed(19) == nullptr && lanes.selection.track == 15 &&
                  root.undoTitle(false) == "Undo Ungroup Tracks",
              "Synths is back in Body where the group stood, and selected; the group is gone");
        // Body ungrouped: what it held takes its place at the top, in its order.
        lanes.selection.track = 13;
        check(root.command(AppCommands::UngroupTracks), "Ungroup Tracks on Body");
        refresh();
        check(order() == std::vector<std::pair<std::int64_t, std::uint32_t>>{{10, 0}, {11, 1}, {12, 1}, {14, 0}, {15, 0},
                                                                               {16, 0}, {17, 1}, {18, 1}, {1, 0}} &&
                  lanes.selection.track == 14,
              "Bass, Synths and Fx And Hits stand where Body stood, Fx And Hits still a group");
        check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo) && root.command(AppCommands::Undo),
              "three undos");
        refresh();
        const auto *body = trackNamed(13);
        check(order() == tree && body != nullptr && body->name == "Body" && body->kind == "group",
              "Body comes back, by name, holding what it held");
        lanes.selection.track = 1;
        check(!enabled(AppCommands::GroupTracks) && !root.command(AppCommands::GroupTracks) &&
                  root.error() == "Main and return tracks can't be grouped." && order() == tree,
              "Main can't be grouped, and the Status Bar says so");
        lanes.selection.track = 0;
        check(!root.command(AppCommands::UngroupTracks) && root.error() == "Select a group track to ungroup.",
              "Ungroup Tracks with nothing selected says what it needs");
        lanes.selection.track = 15;
        check(root.keyPressed(juce::KeyPress('g', cmd, 0)), "Synths grouped again, for the picture");
        render("adi-group-tracks.png");
        check(root.command(AppCommands::Undo), "and put back");
        refresh();
        check(order() == tree, "the tree as it was");
        // Several tracks selected (Shift+click on the headers reaches across them): Ctrl/Cmd+G groups
        // them all into one group at the first one's place, in their order (manual 18.3).
        lanes.selection.track = 14;
        lanes.selection.toTrack = 15;
        check(root.keyPressed(juce::KeyPress('g', cmd, 0)), "Ctrl/Cmd+G on Bass and Synths");
        refresh();
        const auto *pair = trackNamed(19);
        check(order() == std::vector<std::pair<std::int64_t, std::uint32_t>>{{10, 0}, {11, 1}, {12, 1}, {13, 0}, {19, 1},
                                                                               {14, 2}, {15, 2}, {16, 1}, {17, 2}, {18, 2},
                                                                               {1, 0}} &&
                  pair != nullptr && pair->parent == 13 && lanes.selection.track == 19 && lanes.selection.toTrack == 0 &&
                  root.undoTitle(false) == "Undo Group Tracks",
              "Bass and Synths in one new group where Bass stood in Body, the group selected, one step");
        render("adi-group-selected.png");
        check(root.command(AppCommands::Undo), "undo");
        refresh();
        check(order() == tree, "and the tree is as it was");
        // Live's group rows (manual 18.3): a group records nothing, so its In/Out shows its output
        // only; a grouped track's Audio To is its group, shown as Live's "Group".
        auto outputOf = [&root](std::int64_t id) -> std::string {
            for (const auto &strip : root.mixer.strips())
                if (strip->isVisible() && strip->track() == id)
                    return strip->outputLabel();
            return {};
        };
        check(outputOf(10) == "Master" && outputOf(11) == "Group" && outputOf(16) == "Group" && outputOf(17) == "Group" &&
                  outputOf(1) == "1/2",
              "Drums plays to Master; Clap/Snare, Fx And Hits (in Body) and Rises (in Fx And Hits) to their group; Main to 1/2");
        // Live's group strip (manual 18.3, the figure of a group's strip): no Arm, and its Solo half
        // coloured while a track inside it is soloed.
        auto stripOf = [&root](std::int64_t id) -> MixerStrip * {
            for (const auto &strip : root.mixer.strips())
                if (strip->isVisible() && strip->track() == id)
                    return strip.get();
            return nullptr;
        };
        auto halfLit = [&stripOf](std::int64_t id) {
            const auto *strip = stripOf(id);
            return strip != nullptr && static_cast<bool>(strip->solo.getProperties()["live.half"]) &&
                   !strip->solo.getToggleState();
        };
        check(stripOf(10) != nullptr && stripOf(11) != nullptr && !stripOf(10)->arm.isVisible() && stripOf(11)->arm.isVisible(),
              "Drums, a group, has no Arm; Clap/Snare inside it has");
        check(root.submitEdit("track.setSolo", {{"id", 11}, {"soloed", true}}, "Solo Track"), "Clap/Snare soloed");
        refresh();
        check(halfLit(10) && !halfLit(13) && !halfLit(11), "Drums' Solo is half coloured; Body's and Clap/Snare's are not");
        render("adi-group-solo.png");
        check(root.command(AppCommands::Undo), "undo");
        refresh();
        check(!halfLit(10), "unsoloed, Drums' Solo is whole again");
        // Live's colour chooser at the foot of a track's menu (Live 12 manual 2.3.2): the palette's
        // measured row; on a group, Assign Track Color to Grouped Tracks and Clips (manual 18.3).
        auto headerItems = [&root] {
            juce::StringArray out;
            const auto menu = root.arrangement.headerMenu();
            for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                if (!it.getItem().isSeparator)
                    out.add(it.getItem().text + (it.getItem().customComponent != nullptr ? " (swatches)" : ""));
            return out;
        };
        auto colourOf = [&trackNamed](std::int64_t id) -> std::int64_t {
            const auto *t = trackNamed(id);
            return t != nullptr && t->color ? *t->color : -1;
        };
        lanes.selection.track = 10;
        lanes.selection.toTrack = 0;
        check(headerItems().contains("Assign Track Color to Grouped Tracks and Clips") &&
                  headerItems()[headerItems().size() - 1] == "Colour (swatches)",
              "a group's menu ends with Assign Track Color to Grouped Tracks and Clips, then the colour chooser");
        lanes.selection.track = 11;
        check(!headerItems().contains("Assign Track Color to Grouped Tracks and Clips") &&
                  headerItems().contains("Colour (swatches)"),
              "a track's has the chooser only");
        check(root.arrangement.recolour(5), "a swatch picked on Clap/Snare");
        refresh();
        check(colourOf(11) == 5 && root.undoTitle(false) == "Undo Recolour Track", "Clap/Snare takes colour 5, one step");
        render("adi-colour-chosen.png");
        // The chooser itself, drawn as the menu draws it, for the picture.
        if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty()) {
            const auto menu = root.arrangement.headerMenu();
            for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                if (auto *chooser = it.getItem().customComponent.get()) {
                    int w = 0, h = 0;
                    chooser->getIdealSize(w, h);
                    chooser->setSize(w, h);
                    juce::Image image(juce::Image::ARGB, 4 * w, 4 * h, true, juce::SoftwareImageType{});
                    {
                        juce::Graphics g(image);
                        g.addTransform(juce::AffineTransform::scale(4.f));
                        g.fillAll(juce::Colour(0xff2b2b2b));
                        chooser->paintEntireComponent(g, true);
                    }
                    const auto file = juce::File(pictures).getChildFile("adi-colour-chooser.png");
                    file.deleteFile();
                    if (auto stream = file.createOutputStream())
                        juce::PNGImageFormat{}.writeImageToStream(image, *stream);
                }
        }
        lanes.selection.toTrack = 12;
        check(root.arrangement.recolour(7), "a swatch picked on Clap/Snare and Hats & Percs");
        refresh();
        check(colourOf(11) == 7 && colourOf(12) == 7 && root.undoTitle(false) == "Undo Recolour Track",
              "every selected track takes it, one step");
        lanes.selection.track = 10;
        lanes.selection.toTrack = 0;
        check(root.arrangement.recolour(3) && root.arrangement.assignGroupColour(), "Drums coloured, then its colour assigned");
        refresh();
        check(colourOf(10) == 3 && colourOf(11) == 3 && colourOf(12) == 3 &&
                  root.undoTitle(false) == "Undo Assign Track Color to Grouped Tracks and Clips",
              "the tracks Drums holds take its colour, one step named as Live's");
        for (int i = 0; i < 4; ++i)
            root.command(AppCommands::Undo);
        refresh();
        check(colourOf(10) != 3 && colourOf(11) != 7 && colourOf(12) != 7, "four undos put the colours back");
    }
    // The Mix View's STACK lists every channel the window's project view shows: the group chosen,
    // what it holds, and Main (ADR-0209 d11: "every channel of the selected group or filter").
    {
        check(root.command(AppCommands::ToggleMixView), "the Mix View opens on the groups");
        root.mixViewPanel.stack.channelScope.setSelectedId(2, juce::sendNotificationSync);
        root.frame();
        auto stackIds = [&root] {
            std::vector<std::int64_t> out;
            const auto &model = root.mixViewPanel.stack.model();
            for (std::size_t i = 0; i < model.shownCount(); ++i)
                out.push_back(model.shownTrack(i));
            return out;
        };
        check(stackIds() == std::vector<std::int64_t>{10, 11, 12, 13, 14, 15, 16, 17, 18, 1},
              "every channel, in the arrangement's order");
        root.arrangement.chooseView(13);
        root.frame();
        check(stackIds() == std::vector<std::int64_t>{13, 14, 15, 16, 17, 18, 1} &&
                  root.mixViewPanel.stack.scope() == "Body",
              "Body chosen in the project view: Body, what it holds, and Main");
        root.arrangement.chooseView(0);
        root.frame();
        check(stackIds().size() == 10 && root.command(AppCommands::ToggleMixView), "the whole project again; closed");
        root.frame();
    }
    // Live's track drag (the Arrangement View: "Tracks can be reordered by selecting and dragging
    // them above or below other tracks"; 18.3: "tracks can be dragged into or out of the group").
    {
        auto &headers = root.arrangement.headers;
        auto at = [&topOf](std::int64_t id, int down) { return juce::Point<float>(60.f, static_cast<float>(topOf(id) + down)); };
        // The window at twice its size, for the PR's picture (ADI_UI_PARITY).
        auto picture = [&root](const char *name) {
            const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
            if (pictures.isEmpty())
                return;
            root.frame();
            juce::Image shot(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics g(shot);
                g.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(g, true);
            }
            const auto file = juce::File(pictures).getChildFile(name);
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(shot, *stream);
        };
        // Drags `id`'s title to `to`; what the line showed on the way, and whether it was drawn.
        bool lineShown = false;
        auto drag = [&](std::int64_t id, juce::Point<float> to, juce::ModifierKeys mods = {}, const char *shots = nullptr) {
            const auto from = at(id, 16);
            headers.mouseDown(pressAt(headers, from, mods));
            headers.mouseDrag(dragAt(headers, from, to, mods));
            const auto shown = headers.drop();
            if (shown) {
                // The line, bright, where the drop would land; or the group outlined.
                juce::Image image(juce::Image::ARGB, headers.getWidth(), headers.getHeight(), true, juce::SoftwareImageType{});
                {
                    juce::Graphics g(image);
                    headers.paintEntireComponent(g, true);
                }
                const int y = shown->into != 0 ? topOf(shown->into) + 1 : shown->y;
                lineShown = image.getPixelAt(headers.nameLeft(shown->depth) + 30, y) == look::textBright;
            }
            if (shots != nullptr)
                picture((std::string(shots) + "-dragging.png").c_str());
            headers.mouseUp(dragAt(headers, from, to, mods));
            root.frame();
            if (shots != nullptr)
                picture((std::string(shots) + "-dropped.png").c_str());
            return shown;
        };
        auto undo = [&root] {
            root.command(AppCommands::Undo);
            root.frame();
        };
        const std::vector<std::pair<std::int64_t, std::uint32_t>> bassFirst{
            {14, 0}, {10, 0}, {11, 1}, {12, 1}, {13, 0}, {15, 1}, {16, 1}, {17, 2}, {18, 2}, {1, 0}};
        headers.mouseMove(pressAt(headers, at(12, 38)));
        const bool splitCursor = headers.getMouseCursor() == juce::MouseCursor::UpDownResizeCursor;
        headers.mouseMove(pressAt(headers, at(12, 16)));
        check(splitCursor && headers.getMouseCursor() == juce::MouseCursor::NormalCursor,
              "the split line at a title's foot shows the resize cursor; the rest of the title does not");
        const int drumsTop = topOf(10);
        auto shown = drag(14, at(10, 6));
        check(shown && shown->to.parent == 0 && shown->to.before == 10 && shown->y == drumsTop && shown->depth == 0 && lineShown &&
                  order() == bassFirst && root.undoTitle(false) == "Undo Move Track" && lanes.selection.track == 14,
              "Bass dragged to Drums' upper half: the line above Drums, and Bass first, one step; still selected");
        undo();
        check(order() == tree && !headers.drop(), "undo puts Bass back in Body; no line once the drag is over");
        shown = drag(17, at(10, 30), {}, "adi-track-move");
        check(shown && shown->to.parent == 10 && shown->to.before == 11 && shown->depth == 1 && lineShown &&
                  order() == std::vector<std::pair<std::int64_t, std::uint32_t>>{{10, 0}, {17, 1}, {11, 1}, {12, 1}, {13, 0},
                                                                                  {14, 1}, {15, 1}, {16, 1}, {18, 2}, {1, 0}},
              "Rises dragged under open Drums: into it, at the top of what it holds, out of Fx And Hits");
        undo();
        shown = drag(18, at(1, 20));
        check(shown && shown->to.parent == 0 && shown->to.before == 0 &&
                  order() == std::vector<std::pair<std::int64_t, std::uint32_t>>{{10, 0}, {11, 1}, {12, 1}, {13, 0}, {14, 1},
                                                                                  {15, 1}, {16, 1}, {17, 2}, {18, 0}, {1, 0}},
              "Hits dragged onto Main at the foot: out of its groups, the last track before Main");
        undo();
        check(order() == tree, "undo puts Hits back in Fx And Hits");
        root.arrangement.toggleFold(10);
        root.frame();
        shown = drag(14, at(10, 20));
        check(shown && shown->into == 10 && shown->to.parent == 10 && shown->to.before == 0 && lineShown,
              "Bass dragged to the middle of folded Drums: the group outlined, at the end of what it holds");
        root.arrangement.toggleFold(10);
        root.frame();
        check(order() == std::vector<std::pair<std::int64_t, std::uint32_t>>{{10, 0}, {11, 1}, {12, 1}, {14, 1}, {13, 0},
                                                                               {15, 1}, {16, 1}, {17, 2}, {18, 2}, {1, 0}},
              "unfolded, Bass is the last track Drums holds");
        undo();
        // Several selected tracks go together; a plain press on one keeps them for the drag.
        lanes.selection.track = 14;
        lanes.selection.toTrack = 15;
        drag(15, at(10, 6));
        check(order() == std::vector<std::pair<std::int64_t, std::uint32_t>>{{14, 0}, {15, 0}, {10, 0}, {11, 1}, {12, 1},
                                                                               {13, 0}, {16, 1}, {17, 2}, {18, 2}, {1, 0}} &&
                  root.undoTitle(false) == "Undo Move Tracks" && lanes.selectedTracks(root.reader()) == std::vector<std::int64_t>{14, 15},
              "Bass and Synths selected, Synths dragged above Drums: both, in their order, one step; still selected");
        undo();
        headers.mouseDown(pressAt(headers, at(15, 16)));
        const bool kept = lanes.selection.toTrack == 15;
        headers.mouseUp(pressAt(headers, at(15, 16)));
        check(kept && lanes.selection.track == 15 && lanes.selection.toTrack == 0,
              "a plain click on Synths among them keeps them while pressed, then selects Synths alone");
        // A group never goes inside itself; the refusal says why.
        drag(13, at(16, 30));
        check(order() == tree && root.error() == "A group can't go inside itself.",
              "Body dragged under Fx And Hits, which it holds: refused, with the reason");
        drag(12, at(12, 10));
        check(order() == tree && root.undoTitle(false) != "Undo Move Track",
              "Hats & Percs dropped on itself: nothing moves, no step");
        // The split line still resizes.
        const auto from = at(12, 38);
        headers.mouseDown(pressAt(headers, from));
        headers.mouseDrag(dragAt(headers, from, from.translated(0, 20)));
        headers.mouseUp(dragAt(headers, from, from.translated(0, 20)));
        check(lanes.heightFor(12) == 60 && order() == tree, "a drag from the split line makes the track taller, and moves none");
        lanes.resizeTrack(12, 40);
        root.frame();
        // The header is one piece, as Live's: the split line runs on under the track controls (the
        // director's R2, 2 November).
        MixerStrip *controls = nullptr;
        for (const auto &s : root.mixer.strips())
            if (s->isVisible() && s->track() == 12)
                controls = s.get();
        check(controls != nullptr, "Hats & Percs has its track controls");
        if (controls != nullptr) {
            const juce::Point<float> foot(20.f, static_cast<float>(controls->getHeight() - 2));
            controls->mouseMove(pressAt(*controls, foot));
            const bool cursor = controls->getMouseCursor() == juce::MouseCursor::UpDownResizeCursor;
            controls->mouseMove(pressAt(*controls, {20.f, 10.f}));
            check(cursor && controls->getMouseCursor() == juce::MouseCursor::NormalCursor,
                  "the split line under the track controls shows the resize cursor; the rest of them does not");
            controls->mouseDown(pressAt(*controls, foot));
            controls->mouseDrag(dragAt(*controls, foot, foot.translated(0, 24)));
            controls->mouseUp(dragAt(*controls, foot, foot.translated(0, 24)));
            check(lanes.heightFor(12) == 64 && order() == tree,
                  "a drag from it makes the track taller, lanes and names with it");
        }
        lanes.resizeTrack(12, 40);
        root.frame();
    }
    // Live's Insert Track (manual 18.2) inside a group: right after the selected track, in its group.
    {
        lanes.selection.track = 11;
        lanes.selection.toTrack = 0;
        lanes.selection.picked.clear();
        check(root.command(AppCommands::InsertAudioTrack), "Insert Audio Track with Clap/Snare/Rimshot selected");
        root.frame();
        check(order() == std::vector<std::pair<std::int64_t, std::uint32_t>>{{10, 0}, {11, 1}, {19, 1}, {12, 1}, {13, 0},
                                                                               {14, 1}, {15, 1}, {16, 1}, {17, 2}, {18, 2}, {1, 0}} &&
                  lanes.selection.track == 19 && root.undoTitle(false) == "Undo Insert Audio Track",
              "the new track in Drums, right after Clap/Snare/Rimshot, Hats & Percs after it; selected; one step");
        if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty()) {
            juce::Image shot(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics g(shot);
                g.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(g, true);
            }
            const auto file = juce::File(pictures).getChildFile("adi-insert-place.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(shot, *stream);
        }
        root.command(AppCommands::Undo);
        root.frame();
        check(order() == tree, "undo removes it");
    }
    // Live scrolls the lanes while a track, a clip or a selection is dragged to their edge, and a
    // mouse held there keeps them going (each repeated drag is one more step).
    {
        auto &headers = root.arrangement.headers;
        auto &canvas = root.arrangement.canvas;
        lanes.resizeTrack(0, 120, true);
        lanes.scrollY = 0;
        root.mark(DirtySet::All);
        root.frame();
        const int bottom = lanes.footTop(root.reader());
        check(lanes.maxScroll(root.reader()) > 0, "the tracks made taller than the lanes");
        auto positionOf = [&order](std::int64_t id) {
            const auto rows = order();
            for (std::size_t i = 0; i < rows.size(); ++i)
                if (rows[i].first == id)
                    return i;
            return rows.size();
        };
        // A track's title held at the foot: the lanes scroll, step by step, and it lands below.
        const juce::Point<float> from(60.f, static_cast<float>(topOf(11) + 16));
        const juce::Point<float> foot(60.f, static_cast<float>(bottom - 4));
        // The window at twice its size, for the PR's picture (ADI_UI_PARITY).
        auto shot = [&root](const char *name) {
            const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
            if (pictures.isEmpty())
                return;
            root.frame();
            juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(g, true);
            }
            const auto file = juce::File(pictures).getChildFile(name);
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(image, *stream);
        };
        headers.mouseDown(pressAt(headers, from));
        headers.mouseDrag(dragAt(headers, from, foot));
        shot("adi-drag-scroll-start.png");
        const int once = lanes.scrollY;
        for (int i = 0; i < 9; ++i)
            headers.mouseDrag(dragAt(headers, from, foot));
        const int held = lanes.scrollY;
        shot("adi-drag-scroll-held.png");
        // The track controls follow the lanes as they scroll: Hats & Percs' row stands level with its lane.
        root.frame();
        bool level = false;
        const int laneY = root.getLocalArea(&canvas, juce::Rectangle<int>(0, laneTop(root, 12), 1, 1)).getY();
        for (const auto &strip : root.mixer.strips())
            if (strip->track() == 12 && strip->isVisible())
                level = std::abs(root.getLocalArea(strip.get(), strip->getLocalBounds()).getY() - laneY) <= 2;
        check(level, "the track controls follow the lanes while a drag scrolls them");
        headers.mouseUp(dragAt(headers, from, foot));
        root.frame();
        check(once > 0 && held > once && positionOf(11) > 2,
              "Clap/Snare/Rimshot's title held at the foot: the lanes scroll on with each repeat, and it lands further down");
        root.command(AppCommands::Undo);
        root.frame();
        check(order() == tree, "undo");
        // Back at the top: a title held above the lanes scrolls them back.
        const auto top = lanes.scrollY;
        const juce::Point<float> start(60.f, static_cast<float>(lanes.footTop(root.reader()) / 2));
        headers.mouseDown(pressAt(headers, start));
        for (int i = 0; i < 5; ++i)
            headers.mouseDrag(dragAt(headers, start, {60.f, -10.f}));
        const int back = lanes.scrollY;
        headers.mouseUp(dragAt(headers, start, {60.f, -10.f}));
        root.frame();
        check(back < top, "a title held above the lanes scrolls them back up");
        if (order() != tree) {
            root.command(AppCommands::Undo);
            root.frame();
        }
        check(order() == tree, "and undo, if it moved");
        // A clip held at the lanes' right edge: they scroll on, and it lands later.
        lanes.scrollY = 0;
        root.frame();
        const auto *claps = root.reader().findTrack(11)->clips.front().get();
        const auto pos = claps->posTicks;
        const juce::Point<float> bar(static_cast<float>(lanes.x(pos)) + 6.f, static_cast<float>(laneTop(root, 11) + 4));
        const juce::Point<float> right(static_cast<float>(canvas.getWidth() - 2), bar.y);
        const double left = lanes.left;
        canvas.mouseDown(pressAt(canvas, bar));
        for (int i = 0; i < 5; ++i)
            canvas.mouseDrag(dragAt(canvas, bar, right));
        const double panned = lanes.left;
        canvas.mouseUp(dragAt(canvas, bar, right));
        root.frame();
        check(panned > left && root.reader().findTrack(11)->clips.front()->posTicks > pos,
              "Claps' bar held at the lanes' right edge: they scroll on, and the clip lands later");
        root.command(AppCommands::Undo);
        root.frame();
        // A time selection dragged to the right edge reaches on as the lanes scroll.
        lanes.left = 0;
        root.frame();
        const juce::Point<float> body(40.f, static_cast<float>(laneTop(root, 12) + 60));
        const juce::Point<float> edge(static_cast<float>(canvas.getWidth() + 10), body.y);
        canvas.mouseDown(pressAt(canvas, body));
        canvas.mouseDrag(dragAt(canvas, body, edge));
        const auto first = lanes.selection.end;
        for (int i = 0; i < 4; ++i)
            canvas.mouseDrag(dragAt(canvas, body, edge));
        canvas.mouseUp(dragAt(canvas, body, edge));
        check(lanes.left > 0 && lanes.selection.end > first,
              "a time selection dragged past the right edge: the lanes scroll on, and it reaches further");
        // A held click that has not moved scrolls nothing, at the edge or not.
        lanes.left = 0;
        lanes.scrollY = 0;
        root.frame();
        const juce::Point<float> rim(static_cast<float>(canvas.getWidth() - 2), static_cast<float>(laneTop(root, 12) + 60));
        canvas.mouseDown(pressAt(canvas, rim));
        canvas.mouseDrag(pressAt(canvas, rim));
        canvas.mouseUp(pressAt(canvas, rim));
        check(lanes.left == 0 && lanes.scrollY == 0, "a held click at the edge that has not moved scrolls nothing");
        lanes.resizeTrack(0, 40, true);
        lanes.left = 0;
        lanes.scrollY = 0;
        root.frame();
    }
    lanes.selection.track = 15;
    // Two changes of signature, for the picture of the ruler and its markers.
    for (const auto &[pos, num, den] : {std::tuple{8 * textproj::kPPQ, 3, 4}, std::tuple{14 * textproj::kPPQ, 7, 8}}) {
        OpRequest sig;
        sig.opType = "project.insertTimeSignature";
        sig.payload = {{"pos", pos}, {"num", num}, {"den", den}};
        sig.label = "Insert Time Signature";
        submitter.submit(std::move(sig));
    }
    lanes.scale = 60;
    render("adi-groups.png");
    lanes.scale = 3.92;
    root.arrangement.toggleFold(16);
    root.arrangement.toggleFold(10);
    render("adi-groups-folded.png");
    root.arrangement.toggleFold(10);
    root.arrangement.toggleFold(16);
    // Live's time signature markers (manual 6.5): a click on one selects it and plays nothing;
    // Ctrl/Cmd+R edits it; Delete removes it; Insert Time Signature Change at the insert marker.
    lanes.scale = 60;
    lanes.left = 0;
    root.mark(DirtySet::All);
    root.frame();
    auto &scrub = root.arrangement.scrub;
    auto meterOf = [&root](std::int64_t pos) {
        for (const auto &m : root.reader().meters())
            if (m.start_ticks == pos)
                return std::to_string(m.numerator) + "/" + std::to_string(m.denominator);
        return std::string{};
    };
    const auto bar3 = 8 * textproj::kPPQ, bar2 = 4 * textproj::kPPQ;
    const float markerX = static_cast<float>(lanes.x(bar3)) + 6.f;
    check(scrub.markerAt(markerX) == bar3 && scrub.markerAt(static_cast<float>(lanes.x(bar2))) == -1,
          "the 3/4 marker is under the mouse at bar 3, and none at bar 2");
    scrub.mouseDown(pressAt(scrub, {markerX, 10.f}));
    check(root.arrangement.selectedMeter == bar3 && !mailbox.desiredPlaying() && lanes.selection.clip == 0,
          "a click on it selects it and plays nothing");
    commands.activate(root);
    juce::ApplicationCommandInfo renameInfo(AppCommands::RenameTrack), deleteInfo(AppCommands::DeleteTrack);
    commands.getCommandInfo(AppCommands::RenameTrack, renameInfo);
    commands.getCommandInfo(AppCommands::DeleteTrack, deleteInfo);
    check(renameInfo.shortName == "Edit Time Signature" && deleteInfo.shortName == "Delete Time Signature Change",
          "the Edit menu's Rename and Delete name the marker");
    root.frame();
    render("adi-meter-selected.png");
    check(root.command(AppCommands::RenameTrack) && scrub.meterEditing() == bar3 && scrub.editor.isVisible() &&
              scrub.editor.getText() == "3/4",
          "Ctrl/Cmd+R: Edit Time Signature, its value in a field over it");
    render("adi-meter-editing.png");
    scrub.editor.setText("5/3", false);
    scrub.editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    check(scrub.editor.isVisible() && meterOf(bar3) == "3/4" && root.error().find("3/4") != std::string::npos,
          "a value Live takes not keeps the field open, and the Status Bar says what to type");
    scrub.editor.setText("5 4", false);
    scrub.editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    root.frame();
    check(!scrub.editor.isVisible() && meterOf(bar3) == "5/4" && submitter.nextUndo()->label == "Edit Time Signature" &&
              root.arrangement.selectedMeter == bar3,
          "5 4 typed, Enter: 5/4, one step, the marker still selected");
    check(root.command(AppCommands::RenameTrack) && scrub.editor.isVisible(), "Ctrl/Cmd+R again");
    scrub.editor.setText("2/2", false);
    scrub.editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    root.frame();
    check(!scrub.editor.isVisible() && meterOf(bar3) == "5/4", "Esc: the field closes, nothing changed");
    const auto markers = root.reader().meters().size();
    check(root.arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "Delete");
    root.frame();
    check(meterOf(bar3).empty() && root.reader().meters().size() == markers - 1 &&
              submitter.nextUndo()->label == "Delete Time Signature Change" && root.arrangement.selectedMeter == -1,
          "Delete removes the selected marker, one step");
    root.command(AppCommands::Undo);
    root.frame();
    check(meterOf(bar3) == "5/4", "undo brings it back");
    lanes.selection.insert = bar2;
    lanes.selection.start = lanes.selection.end = bar2;
    check(root.command(AppCommands::InsertTimeSignatureChange) && scrub.meterEditing() == bar2 &&
              scrub.editor.getText() == "4/4",
          "Insert Time Signature Change: a field at the insert marker, bar 2, with the 4/4 in force there");
    scrub.editor.setText("6/8", false);
    scrub.editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    root.frame();
    check(meterOf(bar2) == "6/8" && submitter.nextUndo()->label == "Insert Time Signature Change" &&
              root.arrangement.selectedMeter == bar2,
          "6/8 typed: a marker at bar 2, one step, selected");
    render("adi-meter-inserted.png");
    lanes.selection.insert = 0;
    check(!root.command(AppCommands::InsertTimeSignatureChange) && !scrub.editor.isVisible() &&
              root.error().find("control bar") != std::string::npos,
          "at the start: refused, the start's signature is the control bar's");
    auto &lanesView = root.arrangement.canvas;
    const juce::Point<float> inLanes{40.f, static_cast<float>(laneTop(root, 11) + 10)};
    lanesView.mouseDown(pressAt(lanesView, inLanes));
    lanesView.mouseUp(pressAt(lanesView, inLanes));
    check(root.arrangement.selectedMeter == -1, "a click in the lanes lets the marker go");
    root.command(AppCommands::Undo);
    root.frame();
    check(meterOf(bar2).empty(), "undo: the inserted marker gone");
    // Moved by the mouse, to the nearest grid line (a quarter at this zoom), or by the arrow keys
    // (manual 6.5): one step each, the marker still selected.
    const auto q = textproj::kPPQ, sevenEighths = 14 * q;
    const auto stepsBefore = submitter.nextUndo()->txnId;
    const juce::Point<float> grab{static_cast<float>(lanes.x(bar3)) + 6.f, 10.f};
    const juce::Point<float> twoLater{grab.x + static_cast<float>(lanes.x(bar3 + 2 * q) - lanes.x(bar3)) + 7.f, 10.f};
    scrub.mouseDown(pressAt(scrub, grab));
    root.frame();
    render("adi-meter-before-move.png");
    scrub.mouseDrag(dragAt(scrub, grab, twoLater));
    check(root.arrangement.selectedMeter == bar3 && scrub.movingTo() == bar3 + 2 * q,
          "the 5/4 marker dragged two quarters on: it would land on the grid line there");
    root.frame();
    render("adi-meter-dragging.png");
    scrub.mouseUp(dragAt(scrub, grab, twoLater));
    root.frame();
    check(meterOf(bar3).empty() && meterOf(bar3 + 2 * q) == "5/4" && submitter.nextUndo()->label == "Move Time Signature Change" &&
              root.arrangement.selectedMeter == bar3 + 2 * q && scrub.movingTo() == -1,
          "let go: it moves there, one step, still selected");
    const juce::Point<float> at10{static_cast<float>(lanes.x(bar3 + 2 * q)) + 6.f, 10.f};
    const juce::Point<float> onto78{static_cast<float>(lanes.x(sevenEighths)) + 6.f, 10.f};
    scrub.mouseDown(pressAt(scrub, at10));
    scrub.mouseDrag(dragAt(scrub, at10, onto78));
    scrub.mouseUp(dragAt(scrub, at10, onto78));
    root.frame();
    check(meterOf(bar3 + 2 * q) == "5/4" && meterOf(sevenEighths) == "7/8" &&
              root.error().find("Another") != std::string::npos,
          "dropped on the 7/8: it stays, and the Status Bar says why");
    const auto insertBefore = lanes.selection.insert;
    check(root.arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)), "right");
    root.frame();
    check(meterOf(bar3 + 3 * q) == "5/4" && root.arrangement.selectedMeter == bar3 + 3 * q &&
              lanes.selection.insert == insertBefore,
          "right moves the selected marker a grid line on, and not the insert marker");
    check(root.arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)), "left");
    root.frame();
    check(meterOf(bar3 + 2 * q) == "5/4", "left moves it back");
    const juce::Point<float> farLeft{-200.f, 10.f};
    scrub.mouseDown(pressAt(scrub, at10));
    scrub.mouseDrag(dragAt(scrub, at10, farLeft));
    check(scrub.movingTo() == q, "dragged past the start: it stops at the first grid line");
    scrub.mouseUp(dragAt(scrub, at10, farLeft));
    root.frame();
    check(meterOf(q) == "5/4", "and lands there");
    check(root.arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)), "left at the first grid line");
    root.frame();
    check(meterOf(q) == "5/4" && root.error().find("control bar") != std::string::npos,
          "it stays: the start is the control bar's");
    for (int i = 0; i < 4; ++i)
        root.command(AppCommands::Undo);
    root.frame();
    check(meterOf(bar3) == "5/4" && submitter.nextUndo()->txnId == stepsBefore,
          "four undos (the two refused moves made none): back at bar 3");
    // Live's fragmentary bar (manual 6.5): 5/4 from bar 3 makes bars of five quarters, so the 7/8 on
    // quarter 14 cuts the second short, from quarter 13; its menu holds Live's two commands, which
    // wait for the time ops.
    auto names = [](const juce::PopupMenu &menu) {
        std::vector<std::pair<std::string, bool>> out;
        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
            if (!it.getItem().isSeparator)
                out.emplace_back(it.getItem().text.toStdString(), it.getItem().isEnabled);
        return out;
    };
    root.arrangement.selectedMeter = -1;
    const auto inFragment = names(scrub.menu(13 * q + q / 2));
    check(inFragment.size() == 4 && inFragment[0].first == "Add Locator" &&
              inFragment[2] == std::pair<std::string, bool>{"Delete Fragmentary Bar Time", false} &&
              inFragment[3] == std::pair<std::string, bool>{"Complete Fragmentary Bar (Insert Time)", false} &&
              root.error().find("time ops") != std::string::npos,
          "the menu on the fragment: Live's two commands, dimmed, and the Status Bar says why");
    check(names(scrub.menu(10 * q)).size() == 2, "elsewhere: Add Locator and Insert Time Signature Change alone");
    lanes.left = 7;
    root.mark(DirtySet::All);
    root.frame();
    render("adi-fragmentary-bar.png");
    lanes.left = 0;
    // The control bar's Time Signature field (manual 6.5): typed, it changes the signature in force
    // at the play location, here the start's, which the project does not store yet.
    auto &bar = root.transport;
    const auto beforeField = submitter.nextUndo()->txnId;
    check(root.timelineTick() < bar3 && meterOf(0) == "4/4", "the play location is before bar 3, in the start's 4/4");
    check(bar.signature.automationLed, "the field's automation LED is lit: the arrangement holds changes");
    bar.signature.onClick();
    check(bar.fieldEditor.isVisible() && bar.fieldEditor.getText() == "4/4",
          "a click on the field: a field over it holding 4/4");
    auto typeSignature = [&](const char *text) {
        bar.fieldEditor.setText(text, false);
        bar.fieldEditor.onReturnKey();
        root.frame();
    };
    typeSignature("3/4");
    check(!bar.fieldEditor.isVisible() && meterOf(0) == "3/4" && bar.signature.text == "3 / 4" &&
              submitter.nextUndo()->label == "Edit Time Signature",
          "3/4 typed: the start's signature, one step, and the field shows it");
    render("adi-signature-field.png");
    bar.signature.onClick();
    typeSignature("6/8");
    check(meterOf(0) == "6/8", "6/8: the stored 3/4 replaced");
    bar.signature.onClick();
    typeSignature("4/4");
    bar.signature.onClick();
    typeSignature("2/4");
    check(meterOf(0) == "2/4" && submitter.nextUndo()->opCount == 2,
          "4/4 stored, then 2/4: the insert refused by the stored 4/4, the 4/4 replaced, one step");
    bar.signature.onClick();
    typeSignature("2/3");
    check(bar.fieldEditor.isVisible() && meterOf(0) == "2/4" && root.error().find("3/4") != std::string::npos,
          "a value Live takes not keeps the field open, and the Status Bar says what to type");
    bar.fieldEditor.onEscapeKey();
    check(!bar.fieldEditor.isVisible(), "Esc closes it");
    for (int i = 0; i < 4; ++i)
        root.command(AppCommands::Undo);
    root.frame();
    check(meterOf(0) == "4/4" && submitter.nextUndo()->txnId == beforeField,
          "four undos, one for each value, and none for the refused insert: as before");
    // The Tempo field (manual 9.1.1), the same way: 20 to 999 BPM, at the play location.
    bar.tempo.onClick();
    check(bar.editing() == &bar.tempo && bar.fieldEditor.getText() == "120.00",
          "a click on the Tempo field: a field over it holding 120.00");
    auto typeTempo = [&](const char *text) {
        bar.fieldEditor.setText(text, false);
        bar.fieldEditor.onReturnKey();
        root.frame();
    };
    typeTempo("128");
    check(bar.editing() == nullptr && std::abs(root.bpm() - 128) < 1e-9 && bar.tempo.text == "128.00" &&
              submitter.nextUndo()->label == "Change Tempo",
          "128 typed: the start's tempo, one step, and the field shows it");
    render("adi-tempo-field.png");
    bar.tempo.onClick();
    typeTempo("120");
    bar.tempo.onClick();
    typeTempo("97.5");
    check(std::abs(root.bpm() - 97.5) < 1e-9 && submitter.nextUndo()->opCount == 2,
          "120 stored, then 97.5: the insert refused by the stored 120, the 120 replaced, one step");
    bar.tempo.onClick();
    typeTempo("1000");
    check(bar.editing() == &bar.tempo && std::abs(root.bpm() - 97.5) < 1e-9 &&
              root.error().find("20 to 999") != std::string::npos,
          "1000 keeps the field open, and the Status Bar says what to type");
    bar.fieldEditor.onEscapeKey();
    for (int i = 0; i < 3; ++i)
        root.command(AppCommands::Undo);
    root.frame();
    check(std::abs(root.bpm() - 120) < 1e-9 && submitter.nextUndo()->txnId == beforeField,
          "three undos: 120 again, as before");
    // Tap Tempo (manual 9.1.2): from the second tap the tempo follows; a run is one step.
    check(root.tapTempo(1000), "a first tap");
    root.frame();
    check(std::abs(root.bpm() - 120) < 1e-9 && submitter.nextUndo()->txnId == beforeField, "one tap changes nothing");
    root.tapTempo(1600);
    root.frame();
    check(std::abs(root.bpm() - 100) < 1e-9 && submitter.nextUndo()->label == "Tap Tempo", "a second 0.6 s on: 100 BPM");
    const auto runStep = submitter.nextUndo()->txnId;
    root.tapTempo(2200);
    root.tapTempo(2700);
    root.frame();
    render("adi-tap-tempo.png");
    check(std::abs(root.bpm() - 105.88) < 1e-9 && submitter.nextUndo()->label == "Tap Tempo",
          "two more: 1.7 s over three beats, 105.88, the run's step replaced");
    bar.tap.onClick();
    root.frame();
    check(root.taps().taps() == 1, "the Tap button taps (a new run, so long after)");
    root.command(AppCommands::Undo);
    root.frame();
    check(std::abs(root.bpm() - 120) < 1e-9 && submitter.nextUndo()->txnId == beforeField && runStep != beforeField,
          "one undo takes the whole run back: 120");
    // The Arrangement Position field (manual 6.1): typed, it moves the insert marker; up and down step
    // a bar. Here 5/4 from bar 3 (quarter 8), and bar 4 cut short by the 7/8 on quarter 14.
    bar.position.onClick();
    check(bar.editing() == &bar.position && bar.fieldEditor.getText() == "1.1.1",
          "a click on the position: a field holding 1.1.1");
    auto typePosition = [&](const char *text) {
        bar.fieldEditor.setText(text, false);
        bar.fieldEditor.onReturnKey();
        root.frame();
    };
    typePosition("2.3");
    check(bar.editing() == nullptr && lanes.selection.insert == 6 * q && lanes.selection.marked &&
              lanes.selection.start == lanes.selection.end && submitter.nextUndo()->txnId == beforeField,
          "2.3 typed: the insert marker on bar 2's third beat, quarter 6; no undo step");
    {
        // The driver takes the locate, as the audio thread would; the field shows the play position.
        engine::Transport driver;
        mailbox.drain(driver);
        root.mark(DirtySet::All);
        root.frame();
        check(!root.playing() && bar.position.text == "2.  3.  1", "the playhead there too, and the field shows 2. 3. 1");
    }
    render("adi-position-field.png");
    bar.position.onClick();
    typePosition("3.5");
    check(lanes.selection.insert == 12 * q, "3.5: bar 3's fifth beat in 5/4, quarter 12");
    bar.position.onClick();
    typePosition("4.2");
    check(bar.editing() == &bar.position && lanes.selection.insert == 12 * q &&
              root.error().find("no beat 2") != std::string::npos,
          "4.2: bar 4 ends at the 7/8 before its second beat: refused, the field kept open, the reason said");
    bar.fieldEditor.onEscapeKey();
    check(bar.position.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && lanes.selection.insert == 13 * q,
          "up: a bar on, where bar 4 has no fifth beat, so its start, quarter 13");
    check(bar.position.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) && lanes.selection.insert == 8 * q,
          "down: bar 3's start again");
    check(submitter.nextUndo()->txnId == beforeField, "none of it an undo step");
}
// The sends (ADR-0203 d3) on Live's default set: one per return on every track's controls, Live's
// value boxes on the row under volume and pan, and its knobs under "Sends" in the Mixer View.
void sendKnobs(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "sends.adi", err);
    check(store != nullptr, "the sends' project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Sends')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    bool madeAll = true;
    auto make = [&submitter, &madeAll](std::int64_t id, const char *kind, const char *name, std::int64_t index) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", name}, {"index", index}};
        r.label = "Create track";
        madeAll = submitter.submit(std::move(r)).ok && madeAll;
    };
    make(1, "midi", "# MIDI", 0);
    make(2, "midi", "# MIDI", 1);
    make(3, "audio", "# Audio", 2);
    make(4, "audio", "# Audio", 3);
    make(5, "return", "A Reverb", 4);
    make(6, "return", "B Delay", 5);
    make(7, "master", "Main", 6);
    check(madeAll, "Live's default set: two MIDI tracks, two audio tracks, two returns and Main");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "sends");
    root.setSize(1440, 900);
    root.arrangement.geometry.selection.track = 3;
    root.frame();
    root.frame();
    const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
    auto render = [&root, folder](const char *name) {
        if (folder.isEmpty())
            return;
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true,
                          juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile(name);
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
    };
    auto rowOf = [](MixerPanel &panel, std::int64_t track) -> MixerStrip * {
        for (const auto &strip : panel.strips())
            if (strip->isVisible() && strip->track() == track)
                return strip.get();
        return nullptr;
    };
    auto *audio = rowOf(root.mixer, 3);
    check(audio != nullptr && audio->sendKnobs().size() == 2 && audio->sendsTo(0) == 5 && audio->sendsTo(1) == 6,
          "3 Audio has a send for each return, A then B");
    if (audio == nullptr || audio->sendKnobs().size() != 2)
        return;
    auto &a = *audio->sendKnobs()[0];
    auto &b = *audio->sendKnobs()[1];
    auto takesClicks = [](juce::Component &c) {
        bool self = false, children = false;
        c.getInterceptsMouseClicks(self, children);
        return self;
    };
    const int x = audio->showsInOut() ? MixerPanel::kInOutWidth : 0;
    check(a.isVisible() && a.getBounds() == juce::Rectangle<int>(x + 4, 37, 29, 13) &&
              b.getBounds() == juce::Rectangle<int>(x + 37, 37, 29, 13) && a.getTextFromValue(a.getValue()) == "-inf" &&
              a.getTitle() == "Send A" && b.getTitle() == "Send B",
          "Live's two -inf boxes on the row under volume and pan, as its capture");
    {
        // No row has a button of its own for a Mix View function (ADR-0243).
        bool none = audio->switches().size() == 3;
        for (auto *child : audio->getChildren())
            none = none && child->getTitle() != "ADI-Q" && child->getTitle() != "ADI-L";
        check(none, "a row has the Track Activator, Solo and Arm, and no button for a Mix View function");
    }
    auto *main = rowOf(root.mixer, 7);
    auto *reverb = rowOf(root.mixer, 5);
    check(main != nullptr && main->sendKnobs().empty(), "Main has no sends");
    // Live's MIDI tracks with no instrument (look-live11 difference row 3): their switches, no volume,
    // pan or sends.
    auto *firstMidi = rowOf(root.mixer, 1);
    check(firstMidi != nullptr && firstMidi->isMidi() && !firstMidi->hasAudio() && firstMidi->active.isVisible() &&
              !firstMidi->volume.isVisible() && !firstMidi->pan.isVisible() && firstMidi->sendKnobs().empty(),
          "1 MIDI: its switches, no volume, pan or sends");
    check(!audio->isMidi() && audio->hasAudio() && audio->volume.isVisible(), "3 Audio keeps its mixer");
    // An instrument on 2 MIDI gives it volume, pan and a send per return.
    auto withSynth = std::make_shared<ParameterPublication>();
    DevicePanelData synth;
    synth.id = 31;
    synth.track = 2;
    synth.name = "Synth";
    synth.instrument = true;
    withSynth->push_back(synth);
    root.devices.publication = [withSynth] { return withSynth; };
    root.frame();
    auto *secondMidi = rowOf(root.mixer, 2);
    check(secondMidi != nullptr && secondMidi->isMidi() && secondMidi->hasAudio() && secondMidi->volume.isVisible() &&
              secondMidi->sendKnobs().size() == 2 && !firstMidi->hasAudio(),
          "an instrument on 2 MIDI gives it volume, pan and its sends; 1 MIDI stays without");
    render("adi-midi-rows.png");
    check(reverb != nullptr && reverb->sendKnobs().size() == 2 && !reverb->sendOn(0) && !reverb->sendOn(1) &&
              !takesClicks(*reverb->sendKnobs()[1]) && reverb->sendKnobs()[1]->getAlpha() < 1.f &&
              audio->sendOn(0) && takesClicks(a),
          "a return's sends are off until Enable Send; a track's are on");
    // DF-W35 (a): a track's send with no row reads on, and unticking Enable Send switches it off: a
    // row, fully down as the knob shows it, written switched off, one step; undo gives the tick back.
    {
        audio->enableSend(0, false);
        root.frame();
        const auto *off = findSend(root.reader().sends(), 3, 5);
        check(off != nullptr && !off->enabled && off->gainDb == kSendSilenceDb && !audio->sendOn(0) &&
                  submitter.nextUndo() && submitter.nextUndo()->label == "Disable Send" &&
                  submitter.nextUndo()->opCount == 3,
              "Disable Send on 3 Audio's send A with no row: the row, fully down and off, one step");
        audio->enableSend(0, false);
        check(root.error() == "The send is off already.", "unticking it again only says so");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        check(findSend(root.reader().sends(), 3, 5) == nullptr && audio->sendOn(0), "undone: no row, the send reads on");
    }

    // A typed value commits at once: the row and its level, one undo step.
    audio->sendKnobs()[0]->setValue(-12, juce::sendNotificationSync);
    root.frame();
    const auto *route = findSend(root.reader().sends(), 3, 5);
    // A typed value passes the knob's 0.01 dB step from its -70 dB foot, which an FMA rounds once
    // (Apple Clang on arm64) and two roundings do not: -12 within a hair, not to the bit.
    check(route != nullptr && std::abs(route->gainDb + 12.0) < 1.e-9 && a.getTextFromValue(a.getValue()) == "-12" &&
              submitter.nextUndo() && submitter.nextUndo()->label == "Change Send Level" &&
              submitter.nextUndo()->opCount == 2,
          "3 Audio's send A at -12 dB: a new row and its level, one step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(findSend(root.reader().sends(), 3, 5) == nullptr && a.getTextFromValue(a.getValue()) == "-inf",
          "the send is gone and its box reads -inf again");
    check(root.command(AppCommands::Redo), "redo");
    root.frame();
    check(a.getTextFromValue(a.getValue()) == "-12", "and back at -12 dB");

    // A return's Pre/Post is the return's own (#405, N42): B Delay, which no send reaches yet, takes
    // Pre as one op and one undo step, and its row lights from `TrackNode::returnSendTap`.
    auto *delay = rowOf(root.mixer, 6);
    check(delay != nullptr && delay->prePost.isVisible() && delay->prePost.getButtonText() == "Post" &&
              delay->prePost.getToggleState() && root.reader().findTrack(6) != nullptr &&
              root.reader().findTrack(6)->returnSendTap && !root.reader().findTrack(6)->returnSendTap->explicitChoice,
          "B Delay's row reads Post, no choice held, no send into it");
    if (delay == nullptr)
        return;
    const auto stepBefore = submitter.nextUndo() ? submitter.nextUndo()->txnId : 0;
    delay->prePost.onClick();
    root.frame();
    check(delay->prePost.getButtonText() == "Pre" && !delay->prePost.getToggleState() && root.error().empty() &&
              root.reader().findTrack(6)->returnSendTap->explicitChoice && root.reader().findTrack(6)->returnSendTap->preFader &&
              submitter.nextUndo() && submitter.nextUndo()->txnId != stepBefore &&
              submitter.nextUndo()->label == "Change Pre/Post" && submitter.nextUndo()->opCount == 1,
          "Pre on B Delay with no send into it: one op, one undo step, the row lit from the return's own switch");
    render("adi-return-pre-empty.png");

    // Live's Solo in Place (manual 18.6; B5, N23): the Solo button's menu shows it ticked from the
    // project's one field, on by default (#412). Unticked, it writes one `mixer.setSoloInPlace`
    // outside undo, saved with the project, and the session re-reads the project (afterEdit); the
    // Options menu's entry reads the same field and puts it back the same way.
    {
        auto soloItem = [audio] {
            const auto menu = audio->soloMenu();
            juce::PopupMenu::MenuItemIterator it(menu);
            return it.next() ? it.getItem() : juce::PopupMenu::Item{};
        };
        auto optionsTicked = [&commands, &root] {
            commands.activate(root);
            juce::ApplicationCommandInfo info(AppCommands::SoloInPlace);
            commands.getCommandInfo(AppCommands::SoloInPlace, info);
            return (info.flags & juce::ApplicationCommandInfo::isTicked) != 0;
        };
        auto undoStep = [&submitter] { return submitter.nextUndo() ? submitter.nextUndo()->txnId : 0; };
        const auto item = soloItem();
        check(item.text == "Solo in Place" && item.isTicked && item.isEnabled && item.action != nullptr &&
                  root.reader().soloInPlace() && readCueSettings(*store).soloInPlace && optionsTicked(),
              "the Solo button's menu: Solo in Place, ticked from the project's field, on by default; the Options "
              "menu's entry with it");
        if (folder.isNotEmpty())
            renderMenu(root.getLookAndFeel(), audio->soloMenu(), juce::File(folder).getChildFile("adi-solo-menu.png"));
        const auto stepBeforeSip = undoStep();
        const auto generationBefore = view.generation();
        int synchronised = 0;
        root.afterEdit = [&synchronised] { ++synchronised; };
        if (item.action)
            item.action();
        root.frame();
        check(!root.reader().soloInPlace() && !readCueSettings(*store).soloInPlace && root.error().empty() &&
                  view.generation() != generationBefore && synchronised == 1 && undoStep() == stepBeforeSip &&
                  submitter.nextUndo()->label == "Change Pre/Post",
              "unticked: the op is in the project, the session re-read it, the undo stack is as it was");
        check(!soloItem().isTicked && !optionsTicked(), "the Solo button's menu and the Options menu's entry both read it off");
        check(root.command(AppCommands::SoloInPlace), "the Options menu's entry, picked");
        root.frame();
        check(root.reader().soloInPlace() && readCueSettings(*store).soloInPlace && soloItem().isTicked && optionsTicked() &&
                  synchronised == 2 && undoStep() == stepBeforeSip,
              "puts it back: on again in both menus, the session re-read again, still no undo step");
        check(root.command(AppCommands::SoloInPlace) && root.command(AppCommands::SoloInPlace), "off and on again");
        root.frame();
        check(root.reader().soloInPlace() && synchronised == 4 && undoStep() == stepBeforeSip,
              "each a re-read, none an undo step");
        root.afterEdit = nullptr;
    }
    // The right-click half of it: a press and release with the popup modifier on the Solo switch
    // opens its menu, never puts the switch down and solos nothing (JUCE's Button takes any
    // release after a press as a click), where a plain press and release still solos the track.
    {
        auto isSoloed = [&root](std::int64_t id) {
            for (const auto &t : root.reader().tracks())
                if (t && t->id == id)
                    return t->soloed;
            return false;
        };
        auto undoHead = [&submitter] { return submitter.nextUndo() ? submitter.nextUndo()->txnId : 0; };
        int opened = 0;
        auto menu = std::move(audio->solo.onMenu);
        audio->solo.onMenu = [&opened] { ++opened; };
        const auto at = audio->solo.getLocalBounds().getCentre().toFloat();
        const auto headBeforeClicks = undoHead();
        const auto right = juce::ModifierKeys::rightButtonModifier;
        audio->solo.mouseDown(pressAt(audio->solo, at, right));
        const bool downOnRight = audio->solo.isDown();
        audio->solo.mouseUp(pressAt(audio->solo, at, right));
        root.frame();
        check(opened == 1 && !downOnRight && !audio->solo.isDown() && !isSoloed(3) && undoHead() == headBeforeClicks &&
                  root.error().empty(),
              "a right-click on 3 Audio's Solo: the menu once, the switch never down, nothing soloed, no undo step");
        audio->solo.onMenu = std::move(menu);
        const auto left = juce::ModifierKeys::leftButtonModifier;
        audio->solo.mouseDown(pressAt(audio->solo, at, left));
        audio->solo.mouseUp(pressAt(audio->solo, at, left));
        root.frame();
        check(opened == 1 && isSoloed(3) && submitter.nextUndo().has_value() && submitter.nextUndo()->label == "Solo Track",
              "a left-click still solos it, one undo step, the menu not opened");
        check(root.command(AppCommands::Undo), "undone");
        root.frame();
        check(!isSoloed(3) && undoHead() == headBeforeClicks, "3 Audio unsoloed, the undo stack as it was");
    }

    // Enable Send on A Reverb's send B: on, at full alpha, taking clicks; made after B's Pre, the new
    // send taps before the fader with nothing added to its batch.
    reverb->enableSend(1, true);
    root.frame();
    check(reverb->sendOn(1) && !reverb->sendOn(0) && takesClicks(*reverb->sendKnobs()[1]) &&
              findSend(root.reader().sends(), 5, 6) != nullptr && findSend(root.reader().sends(), 5, 6)->enabled &&
              findSend(root.reader().sends(), 5, 6)->preFader && submitter.nextUndo()->label == "Enable Send" &&
              submitter.nextUndo()->opCount == 2,
          "Enable Send: A Reverb feeds B Delay, fully down until turned up, before the fader as B's switch says");
    reverb->enableSend(1, true);
    check(root.error() == "The send is on already.", "enabling it again only says so");
    // B back to Post: the send it now has moves after the fader, one step.
    delay->prePost.onClick();
    root.frame();
    check(delay->prePost.getButtonText() == "Post" && delay->prePost.getToggleState() &&
              !findSend(root.reader().sends(), 5, 6)->preFader && root.reader().findTrack(6)->returnSendTap->explicitChoice &&
              submitter.nextUndo()->opCount == 1,
          "Post again on B Delay moves its send after the fader; the choice stays the return's");

    // Enable All Sends on B Delay (§18.4): both of its sends on, A and itself, fully down, one step.
    check(!delay->sendOn(0) && !delay->sendOn(1), "B Delay's sends start off");
    {
        delay->enableAllSends();
        root.frame();
        check(delay->sendOn(0) && delay->sendOn(1) && findSend(root.reader().sends(), 6, 5) != nullptr &&
                  findSend(root.reader().sends(), 6, 6) != nullptr && submitter.nextUndo()->label == "Enable All Sends",
              "Enable All Sends switches every send of B Delay on, one undo step");
        render("adi-enable-all.png");
        delay->enableAllSends();
        check(root.error() == "Every send of this return is on already.", "and again only says so");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
    }
    // Live's return and Main rows: a return's activator shows its letter and its Pre/Post sits where
    // a track has Arm; Main has no activator, Solo or Arm, its volume where the activator is.
    check(reverb->kind() == "return" && reverb->active.getButtonText() == "A" && reverb->prePost.isVisible() &&
              !reverb->arm.isVisible() && reverb->prePost.getButtonText() == "Post" && reverb->prePost.getToggleState(),
          "A Reverb's row: \"A\", Solo, and Post lit where Arm is");
    check(main->kind() == "master" && !main->active.isVisible() && !main->solo.isVisible() && !main->arm.isVisible() &&
              !main->prePost.isVisible() && main->volume.getY() == 3,
          "Main's row: no activator, Solo or Arm; its volume on the first row");
    // Pre on A: one op the engine spreads over every send into A (3 Audio's), one undo step; the row
    // lights from the return's own switch.
    reverb->prePost.onClick();
    root.frame();
    check(reverb->prePost.getButtonText() == "Pre" && !reverb->prePost.getToggleState() &&
              findSend(root.reader().sends(), 3, 5)->preFader && root.reader().findTrack(5)->returnSendTap->explicitChoice &&
              root.reader().findTrack(5)->returnSendTap->preFader && submitter.nextUndo()->label == "Change Pre/Post" &&
              submitter.nextUndo()->opCount == 1,
          "Pre/Post thrown: 3 Audio's send A taps before the fader, one op, the row lit from the return's switch");

    render("adi-sends-rows.png");
    // The Mixer View: Live's knobs under "Sends", A on the left and B lower on the right; the mixer
    // under them.
    check(root.command(AppCommands::ToggleMixerView), "the Mixer View opens");
    root.resizeMixerView(320);
    root.mark(DirtySet::All);
    root.frame();
    auto *column = rowOf(root.mixerView, 3);
    check(column != nullptr && column->sendKnobs().size() == 2 && column->sendsShown() == 2,
          "3 Audio's strip shows both sends");
    if (column != nullptr && column->sendsShown() == 2) {
        const auto ka = column->sendKnobs()[0]->getBounds(), kb = column->sendKnobs()[1]->getBounds();
        check(ka == juce::Rectangle<int>(10, 25, 32, 32) && kb == juce::Rectangle<int>(51, 47, 32, 32) &&
                  column->sendKnobs()[0]->getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag,
              "knob A on the left, B to its right and 22 pt lower, as Live's");
        check(column->volume.getY() == 19 + 72 + 6 && column->peakHold.getY() == 19 + 72 + 5,
              "the mixer begins under the sends");
        check(std::abs(column->sendKnobs()[0]->getValue() + 12.0) < 1.e-9,
              "and send A reads the same row as the track controls");
        auto *mainColumn = rowOf(root.mixerView, 7);
        check(mainColumn != nullptr && mainColumn->sendKnobs().empty() && mainColumn->sendsShown() == 2 &&
                  mainColumn->volume.getY() == column->volume.getY(),
              "Main has no knobs, but keeps the Sends section's room: the mixers line up");
        // Live's Mixer: Main's Sends section holds each return's Pre/Post; Main has no activator.
        if (mainColumn != nullptr && mainColumn->returnSwitches().size() == 2) {
            auto &aSwitch = *mainColumn->returnSwitches()[0];
            auto &bSwitch = *mainColumn->returnSwitches()[1];
            check(aSwitch.getButtonText() == "Pre" && !aSwitch.getToggleState() && bSwitch.getButtonText() == "Post" &&
                      bSwitch.getToggleState() && !mainColumn->active.isVisible() &&
                      root.reader().findTrack(5)->returnSendTap->preFader && !root.reader().findTrack(6)->returnSendTap->preFader,
                  "Main's strip shows A's Pre and B's Post where a track has its knobs, each from its return's own switch");
            aSwitch.onClick();
            root.frame();
            check(aSwitch.getButtonText() == "Post" && aSwitch.getToggleState() && !findSend(root.reader().sends(), 3, 5)->preFader &&
                      !root.reader().findTrack(5)->returnSendTap->preFader && submitter.nextUndo()->opCount == 1,
                  "and A's switch, thrown there, is one op that moves the send back after the fader");
        } else
            check(false, "Main's strip has a Pre/Post switch per return");
    }
    // A short Mixer View keeps the mixer's room and leaves out the sends that do not fit.
    root.resizeMixerView(200);
    root.mark(DirtySet::All);
    root.frame();
    column = rowOf(root.mixerView, 3);
    check(column != nullptr && column->sendsShown() < 2 && !column->sendKnobs()[1]->isVisible() &&
              column->getHeight() - column->peakHold.getY() + 5 >= 110,
          "short, the strip shows fewer sends and keeps its mixer");
    root.resizeMixerView(320);
    root.mark(DirtySet::All);
    root.frame();
    render("adi-sends.png");
    // No routing id left (N39): a routing row at INT64_MAX, then 3 Audio's send B, which has no row,
    // turned up in the Mixer View. Nothing lands: the knob goes back fully down, no undo step, and the
    // Status Bar says why. The row undone, the knob commits a row with the id after the highest.
    {
        OpRequest r;
        r.opType = "routing.connect";
        r.payload = {{"id", std::numeric_limits<std::int64_t>::max()}, {"srcKind", "track"}, {"src", 3},
                     {"dstKind", "track"}, {"dst", 4}, {"kind", "sidechain"}};
        r.label = "Connect";
        check(submitter.submit(std::move(r)).ok, "a sidechain row at INT64_MAX");
        root.frame();
        column = rowOf(root.mixerView, 3);
        const auto stepAtTop = submitter.nextUndo() ? submitter.nextUndo()->txnId : 0;
        check(column != nullptr && column->sendKnobs().size() == 2 && findSend(root.reader().sends(), 3, 6) == nullptr &&
                  root.reader().get() != nullptr && root.reader().get()->maxRoutingId == std::numeric_limits<std::int64_t>::max(),
              "3 Audio's send B has no row, and the project's highest routing id is the largest it can hold");
        if (column != nullptr && column->sendKnobs().size() == 2) {
            column->sendKnobs()[1]->setValue(-6, juce::sendNotificationSync);
            root.frame();
            check(root.error() == kSendNoIdLeft && findSend(root.reader().sends(), 3, 6) == nullptr &&
                      column->sendKnobs()[1]->getValue() <= kSendFloorDb && submitter.nextUndo()->txnId == stepAtTop,
                  "turned up with no id left: refused, the knob back fully down, no undo step, the Status Bar says why");
            render("adi-send-no-id.png");
            check(root.command(AppCommands::Undo), "undo the INT64_MAX row");
            root.frame();
            column = rowOf(root.mixerView, 3);
            const auto highest = root.reader().get()->maxRoutingId;
            check(column != nullptr && highest < std::numeric_limits<std::int64_t>::max(), "the highest id is the sends' again");
            if (column != nullptr) {
                column->sendKnobs()[1]->setValue(-6, juce::sendNotificationSync);
                root.frame();
                const auto *sendB = findSend(root.reader().sends(), 3, 6);
                check(sendB != nullptr && sendB->id == highest + 1 && submitter.nextUndo()->label == "Change Send Level" &&
                          submitter.nextUndo()->opCount == 2,
                      "the row gone, the knob commits 3 Audio's send B with the id after the highest, one step");
                check(root.command(AppCommands::Undo), "undo");
                root.frame();
                column = rowOf(root.mixerView, 3);
            }
        }
    }
    // Hide/Show Sends (§42.1, Ctrl/Cmd+Alt+S; Live 12's Arrangement Track Controls): the sends out of
    // the track controls, ADI-Q and ADI-L moving up under volume and pan. The Mixer View keeps its
    // own (Live 12's Mixer Controls), its strips losing theirs with that toggle alone.
    const auto sendsKey = juce::KeyPress('s', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0);
    check(root.keyPressed(sendsKey) && !root.state().sends && root.state().mixerSends, "Ctrl/Cmd+Alt+S hides the track controls' sends");
    column = rowOf(root.mixerView, 3);
    auto *mainStrip = rowOf(root.mixerView, 7);
    check(!a.isVisible() && !b.isVisible(), "the row's -inf boxes go");
    check(column != nullptr && column->sendsShown() == 2 && column->sendKnobs()[0]->isVisible(),
          "the Mixer View strips keep theirs");
    check(root.command(AppCommands::ToggleMixerSends) && !root.state().mixerSends && root.mixerOptionsMenu().getNumItems() > 0,
          "the Mixer Controls' Sends hides the Mixer View's");
    column = rowOf(root.mixerView, 3);
    mainStrip = rowOf(root.mixerView, 7);
    check(column != nullptr && column->sendsShown() == 0 && !column->sendKnobs()[0]->isVisible() &&
              column->volume.getY() == 19 + 6 && mainStrip != nullptr && mainStrip->returnSwitches().size() == 2 &&
              !mainStrip->returnSwitches()[0]->isVisible(),
          "the Mixer View strips lose their Sends section, Main's Pre/Post switches with it");
    render("adi-sends-hidden.png");
    check(root.keyPressed(sendsKey) && root.state().sends && a.isVisible() && a.getY() == 37,
          "Ctrl/Cmd+Alt+S shows the track controls' again");
    column = rowOf(root.mixerView, 3);
    check(column != nullptr && column->sendsShown() == 0 && root.command(AppCommands::ToggleMixerSends) &&
              root.state().mixerSends && column->sendsShown() == 2 && column->sendKnobs()[0]->isVisible(),
          "and the Mixer Controls' Sends the Mixer View's");
    // Live 12's Mixer View Control options (manual 18.1): In/Out, Sends, Volume, Track Options,
    // Crossfader, Performance Impact, then Return Tracks; what ADI has not built is dimmed.
    auto mixerOptions = [&root] { return menuLine(root.mixerOptionsMenu()); };
    check(mixerOptions() == "In/Out, Sends *, Volume * -, Track Options, Crossfader -, Performance Impact - | Return Tracks *",
          "the Mixer View's options in Live's order and groups (manual 18.1, its figure), In/Out and Track Options off");
    // Its In/Out: Live 12's section under the name and over the sends; as many sends as still
    // leave the mixer its room, then the rest when the strip is taller.
    check(root.command(AppCommands::ToggleMixerInOut) && root.state().mixerInOut && mixerOptions().startsWith("In/Out *"),
          "the Mixer's In/Out shows, and its item is ticked");
    column = rowOf(root.mixerView, 3);
    check(column != nullptr && column->showsMixerInOut() && column->sendsShown() < 2 &&
              (column->sendsShown() == 0 || column->sendKnobs()[0]->getY() == 19 + MixerStrip::kInOutHeight + 6),
          "at 320 points the strip shows its In/Out over the sends, and leaves out the sends that do not fit");
    root.resizeMixerView(440);
    root.mark(DirtySet::All);
    root.frame();
    column = rowOf(root.mixerView, 3);
    mainStrip = rowOf(root.mixerView, 7);
    check(column != nullptr && column->showsMixerInOut() && column->sendsShown() == 2 &&
              column->volume.getY() == 19 + MixerStrip::kInOutHeight + 72 + 6 && mainStrip != nullptr &&
              mainStrip->showsMixerInOut() && mainStrip->returnSwitches()[0]->isVisible(),
          "taller, both sends come back under it, Main's Pre/Post switches with them");
    render("adi-mixer-inout.png");
    root.resizeMixerView(200);
    root.mark(DirtySet::All);
    root.frame();
    column = rowOf(root.mixerView, 3);
    check(column != nullptr && !column->showsMixerInOut(), "short, the strip leaves its In/Out out to keep the mixer");
    check(root.command(AppCommands::ToggleMixerInOut) && !root.state().mixerInOut, "and the In/Out hides again");
    root.resizeMixerView(320);
    root.mark(DirtySet::All);
    root.frame();
    // Live's Track Options (manual 18.7): the Mixer Controls' item shows the Track Delay section
    // under each strip's mixer, Main's too (Master Reference 8.1); the box reads ms to two places,
    // typed or dragged, a negative value pre-delays, and each change is one undo step written in
    // samples at the project's rate (N22, `TrackNode::delaySamples`).
    auto trackControls = [&root] { return menuLine(root.trackControlsMenu()); };
    // The box holds JUCE's snapped double, `start + interval * floor(...)`: Apple clang on arm64
    // contracts that to a fused multiply-add, and -1000 + 0.01 * 101000 comes out 10.000000000000021,
    // still "10.00" and still the 441st sample, so the checks read the box on its hundredth grid,
    // never bit-exactly.
    auto boxAt = [](const juce::Slider &box, double ms) { return std::abs(box.getValue() - ms) < 1e-9; };
    check(trackControls().endsWith("Volume * -, Track Options"), "the Arrangement Track Controls' Track Options, off");
    // The selection was set on the geometry alone; published, as a click publishes it, so each
    // delay step's undo puts it back (ADR-0021) rather than the session's older empty one.
    root.publishSelection();
    check(root.command(AppCommands::ToggleMixerTrackOptions) && root.state().mixerTrackOptions && !root.state().trackOptions &&
              mixerOptions() == "In/Out, Sends *, Volume * -, Track Options *, Crossfader -, Performance Impact - | Return Tracks *" &&
              trackControls().endsWith("Track Options"),
          "the Mixer Controls' Track Options shows the Mixer View's Track Delay, ticked; the track controls' stays off");
    column = rowOf(root.mixerView, 3);
    mainStrip = rowOf(root.mixerView, 7);
    check(column != nullptr && column->showsTrackDelay() && column->delay.isVisible() && column->sendsShown() == 2 &&
              column->delay.getY() == column->getHeight() - MixerStrip::kDelayHeight + 19 &&
              column->arm.getBottom() <= column->getHeight() - MixerStrip::kDelayHeight &&
              column->delay.getTextFromValue(column->delay.getValue()) == "0.00" && column->delay.getTitle() == "Track Delay" &&
              mainStrip != nullptr && mainStrip->showsTrackDelay() && mainStrip->delay.isVisible(),
          "the section under the mixer, which moves up for it: 0.00 on 3 Audio and on Main, both sends kept");
    if (column != nullptr && column->delay.isVisible()) {
        const auto stepUndelayed = submitter.nextUndo() ? submitter.nextUndo()->txnId : 0;
        column->delay.setValue(column->delay.getValueFromText("10 ms"), juce::sendNotificationSync);
        root.frame();
        column = rowOf(root.mixerView, 3);
        const auto *delayed = root.reader().findTrack(3);
        const auto stepTen = submitter.nextUndo() ? submitter.nextUndo()->txnId : 0;
        check(column != nullptr && delayed != nullptr && root.reader().sampleRate() == 44100 && delayed->delaySamples == 441 &&
                  stepTen != stepUndelayed && submitter.nextUndo()->label == "Change Track Delay" &&
                  submitter.nextUndo()->opCount == 1 && boxAt(column->delay, 10.) &&
                  column->delay.getTextFromValue(column->delay.getValue()) == "10.00",
              "10 ms typed at 44.1 kHz writes 441 samples in one undo step and reads back 10.00 ms");
        if (column != nullptr) {
            column->delay.setValue(column->delay.getValueFromText("-2.5"), juce::sendNotificationSync);
            root.frame();
            column = rowOf(root.mixerView, 3);
            delayed = root.reader().findTrack(3);
            check(column != nullptr && delayed != nullptr && delayed->delaySamples == -110 &&
                      column->delay.getTextFromValue(column->delay.getValue()) == "-2.49",
                  "-2.5 ms pre-delays: -110 samples on the grid, read back as -2.49 ms");
            render("adi-track-delay.png");
            check(root.command(AppCommands::Undo), "undo the pre-delay");
            root.frame();
            column = rowOf(root.mixerView, 3);
            delayed = root.reader().findTrack(3);
            check(column != nullptr && delayed != nullptr && delayed->delaySamples == 441 &&
                      column->delay.getTextFromValue(column->delay.getValue()) == "10.00",
                  "undone, 3 Audio reads 10.00 ms again");
        }
        if (column != nullptr) {
            // The box's own grid is a hundredth: 10.004 snaps to the 10.00 it holds and nothing
            // is committed.
            column->delay.setValue(10.004, juce::sendNotificationSync);
            root.frame();
            column = rowOf(root.mixerView, 3);
            check(column != nullptr && submitter.nextUndo()->txnId == stepTen && boxAt(column->delay, 10.),
                  "10.004 ms is the box's 10.00: nothing committed");
        }
        if (column != nullptr) {
            // 10.01 ms the box takes, but at 44.1 kHz it is the 441st sample again (441.44): the
            // helper writes no op and the box goes back to the grid's 10.00.
            column->delay.setValue(column->delay.getValueFromText("10.01"), juce::sendNotificationSync);
            root.frame();
            column = rowOf(root.mixerView, 3);
            const auto *same = root.reader().findTrack(3);
            check(column != nullptr && same != nullptr && same->delaySamples == 441 && submitter.nextUndo()->txnId == stepTen &&
                      column->delay.getTextFromValue(column->delay.getValue()) == "10.00",
                  "10.01 ms typed is the same sample on the grid: nothing written, the box back at 10.00");
        }
        if (column != nullptr) {
            // Dragged up or down, as Live's value fields are (manual 6.3, 7.1), not sideways: 20 px
            // up from 10.00 is 12.00 ms at 0.1 ms a pixel (PROVISIONAL, M1-06), 529 samples at
            // 44.1 kHz, written once at the drag's end.
            const auto centre = column->delay.getLocalBounds().getCentre().toFloat();
            const auto up = centre.translated(0.f, -20.f);
            column->delay.mouseDown(pressAt(column->delay, centre));
            column->delay.mouseDrag(dragAt(column->delay, centre, up));
            const bool held = column->dragging() && boxAt(column->delay, 12.) &&
                              root.reader().findTrack(3) != nullptr && root.reader().findTrack(3)->delaySamples == 441;
            column->delay.mouseUp(dragAt(column->delay, centre, up));
            root.frame();
            column = rowOf(root.mixerView, 3);
            const auto *dragged = root.reader().findTrack(3);
            check(held && column != nullptr && dragged != nullptr && dragged->delaySamples == 529 &&
                      submitter.nextUndo()->txnId != stepTen && submitter.nextUndo()->label == "Change Track Delay" &&
                      submitter.nextUndo()->opCount == 1 &&
                      column->delay.getTextFromValue(column->delay.getValue()) == "12.00",
                  "20 px up drags 10.00 to 12.00 ms, nothing written until the drag ends, then 529 samples in one step");
            check(root.command(AppCommands::Undo), "undo the drag");
            root.frame();
            column = rowOf(root.mixerView, 3);
            check(column != nullptr && submitter.nextUndo()->txnId == stepTen && root.reader().findTrack(3) != nullptr &&
                      root.reader().findTrack(3)->delaySamples == 441,
                  "undone, 3 Audio is at its 441 samples again");
        }
    }
    check(root.command(AppCommands::ToggleMixerTrackOptions) && !root.state().mixerTrackOptions, "Track Options off again");
    column = rowOf(root.mixerView, 3);
    check(column != nullptr && !column->showsTrackDelay() && !column->delay.isVisible() &&
              column->arm.getBottom() == column->getHeight() - 4,
          "the section goes and the mixer is back at the strip's foot");
    check(root.command(AppCommands::ToggleMixerView), "the Mixer View closes");
    // The track controls' own Track Options (the D selector, Live 11's; View › Arrangement Track
    // Controls): the box on the row after ADI-Q and ADI-L, reading the same delay. 3 Audio's row is
    // brought into view first, and the lanes put back where they were after.
    const int scrollBefore = root.arrangement.geometry.scrollY;
    root.mixer.reveal(root.arrangement.geometry.rows(root.reader()).find(3));
    root.frame();
    auto *audioRow = rowOf(root.mixer, 3);
    check(audioRow != nullptr && !root.namesBar.delay.getToggleState() && !audioRow->delay.isVisible() &&
              root.command(AppCommands::ToggleTrackOptions) && root.state().trackOptions && !root.state().mixerTrackOptions &&
              root.namesBar.delay.getToggleState() && trackControls().endsWith("Track Options *"),
          "Arrangement Track Controls › Track Options shows the rows' Track Delay; D lights, the Mixer View's stays off");
    root.frame();
    audioRow = rowOf(root.mixer, 3);
    if (audioRow != nullptr) {
        const int rx = audioRow->showsInOut() ? MixerPanel::kInOutWidth : 0;
        check(audioRow->delay.isVisible() && audioRow->delay.getBounds() == juce::Rectangle<int>(rx + 4, 54, 45, 13) &&
                  audioRow->delay.getTextFromValue(audioRow->delay.getValue()) == "10.00",
              "3 Audio's row shows its 10.00 ms on the row after the sends");
    } else
        check(false, "3 Audio's row in view");
    render("adi-track-delay-rows.png");
    root.namesBar.delay.onClick();
    audioRow = rowOf(root.mixer, 3);
    check(!root.state().trackOptions && !root.namesBar.delay.getToggleState() && audioRow != nullptr && !audioRow->delay.isVisible(),
          "D hides it again");
    root.arrangement.geometry.scrollY = scrollBefore;
    root.arrangement.changed();
    root.frame();
    check(root.command(AppCommands::Undo), "undo the 10 ms");
    root.frame();
    check(root.reader().findTrack(3) != nullptr && root.reader().findTrack(3)->delaySamples == 0, "3 Audio plays undelayed");
    // Hide/Show Return Tracks (§18.4, Ctrl/Cmd+Alt+R): the returns out of the lanes and the controls.
    auto shownLanes = [&root] {
        std::vector<std::int64_t> out;
        for (const auto row : root.arrangement.geometry.rows(root.reader()))
            if (root.arrangement.geometry.shown(row.track->id))
                out.push_back(row.track->id);
        return out;
    };
    check(root.keyPressed(juce::KeyPress('r', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)) &&
              root.state().hideReturns && shownLanes() == std::vector<std::int64_t>{1, 2, 3, 4, 7},
          "Ctrl/Cmd+Alt+R hides the returns");
    root.frame();
    render("adi-returns-hidden.png");
    // Insert Return Track (§18.4, Ctrl/Cmd+Alt+T): C Return after B Delay, selected, the returns shown
    // again; every track gets a send to it.
    check(root.keyPressed(juce::KeyPress('t', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)),
          "Ctrl/Cmd+Alt+T");
    root.frame();
    const auto *inserted = root.reader().findTrack(8);
    check(inserted != nullptr && inserted->kind == "return" && inserted->name == "C Return" && !root.state().hideReturns &&
              shownLanes() == std::vector<std::int64_t>{1, 2, 3, 4, 5, 6, 8, 7} &&
              root.arrangement.geometry.selection.track == 8 && submitter.nextUndo()->label == "Insert Return Track",
          "Insert Return Track makes C Return after the others, selected, the returns shown again");
    if (auto *third = rowOf(root.mixer, 3))
        check(third->sendKnobs().size() == 3 && third->sendsTo(2) == 8, "and every track has a send to it");
    render("adi-return-inserted.png");
    // Live's Solo rules (manual 18.1, §42.19): S solos the selected track; a Solo switch solos its
    // track and unsolos the others (Exclusive Solo, on by default); Ctrl/Cmd adds.
    auto soloed = [&root] {
        std::vector<std::int64_t> out;
        for (const auto &t : root.reader().tracks())
            if (t->soloed)
                out.push_back(t->id);
        return out;
    };
    // The track controls realise rows for the lanes on screen only: 1 and 2 MIDI here.
    root.arrangement.geometry.selection.track = 1;
    root.arrangement.geometry.selection.toTrack = 0;
    check(root.keyPressed(juce::KeyPress('s', 0, 0)), "S");
    root.frame();
    check(soloed() == std::vector<std::int64_t>{1} && submitter.nextUndo()->label == "Solo Track",
          "S solos the selected track, one step");
    auto *second = rowOf(root.mixer, 2);
    check(second != nullptr, "2 MIDI's row is on screen");
    if (second != nullptr)
        second->solo.onClick();
    root.frame();
    check(soloed() == std::vector<std::int64_t>{2} && submitter.nextUndo()->opCount == 2,
          "2 MIDI's Solo solos it and unsolos 1 MIDI, one step");
    juce::ModifierKeys::currentModifiers = juce::ModifierKeys(juce::ModifierKeys::commandModifier);
    if (auto *first = rowOf(root.mixer, 1))
        first->solo.onClick();
    juce::ModifierKeys::currentModifiers = juce::ModifierKeys();
    root.frame();
    check(soloed() == std::vector<std::int64_t>{1, 2}, "Ctrl/Cmd+click adds 1 MIDI");
    render("adi-solo-rules.png");
    check(root.keyPressed(juce::KeyPress('s', 0, 0)), "S again");
    root.frame();
    check(soloed() == std::vector<std::int64_t>{2}, "S on the soloed, selected 1 MIDI unsolos it alone");
    for (int i = 0; i < 4; ++i)
        root.command(AppCommands::Undo);
    root.frame();
    check(soloed().empty(), "four undos: nothing soloed");
    // Live's Track Activator rules (manual 18.1, §42.19): the selected tracks together; 0 on the
    // selection; F1…F8 the first eight tracks, each alone.
    auto muted = [&root] {
        std::vector<std::int64_t> out;
        for (const auto &t : root.reader().tracks())
            if (t->muted)
                out.push_back(t->id);
        return out;
    };
    root.arrangement.geometry.selection.track = 1;
    root.arrangement.geometry.selection.toTrack = 2;
    if (auto *first = rowOf(root.mixer, 1))
        first->active.onClick();
    root.frame();
    check(muted() == std::vector<std::int64_t>{1, 2} && submitter.nextUndo()->label == "Deactivate Track" &&
              submitter.nextUndo()->opCount == 2,
          "1 MIDI's activator, with 1 and 2 MIDI selected, deactivates both, one step");
    check(root.keyPressed(juce::KeyPress(juce::KeyPress::F3Key, 0, 0)), "F3");
    root.frame();
    check(muted() == std::vector<std::int64_t>{1, 2, 3}, "F3 deactivates the third track, alone");
    render("adi-activators.png");
    root.arrangement.geometry.selection.toTrack = 0;
    check(root.keyPressed(juce::KeyPress('0', 0, 0)), "0");
    root.frame();
    check(muted() == std::vector<std::int64_t>{2, 3} && submitter.nextUndo()->label == "Activate Track",
          "0 activates the selected 1 MIDI");
    for (int i = 0; i < 3; ++i)
        root.command(AppCommands::Undo);
    root.frame();
    check(muted().empty(), "three undos: every track active");
    // Live's momentary latching (§42.25): S, F1…F8 and Z held 500 ms or more go back when let go; a
    // tap stays. The key's repeats while it is held do nothing.
    auto lastStep = [&submitter] {
        const auto step = submitter.nextUndo();
        return step ? step->txnId : std::int64_t{0};
    };
    root.letGoUnseen();
    root.arrangement.geometry.selection.track = 1;
    root.arrangement.geometry.selection.toTrack = 0;
    const auto beforeLatch = lastStep();
    check(root.keyPressed(juce::KeyPress('s', 0, 0)) && root.latch().held() && root.latch().key() == 's',
          "S goes down: the window times it");
    root.frame();
    render("adi-latch-held.png");
    check(soloed() == std::vector<std::int64_t>{1}, "S held solos 1 MIDI while it is held");
    check(root.latchKey(0, 's', false, static_cast<std::int64_t>(juce::Time::getMillisecondCounter()) + 600),
          "S let go after 600 ms");
    root.frame();
    check(soloed().empty() && lastStep() == beforeLatch && !root.latch().held(),
          "1 MIDI is unsoloed, the step gone");
    render("adi-latch-released.png");
    check(root.latchKey(AppCommands::SoloSelected, 's', true, 10000) &&
              root.latchKey(AppCommands::SoloSelected, 's', true, 10040) &&
              root.latchKey(AppCommands::SoloSelected, 's', true, 10400),
          "S down, and the key's repeats");
    root.frame();
    check(soloed() == std::vector<std::int64_t>{1} && submitter.nextUndo()->label == "Solo Track" &&
              submitter.nextUndo()->txnId != beforeLatch,
          "the repeats while it is held solo nothing more: one step");
    check(!root.latchKey(0, 's', false, 10499), "let go within 500 ms: a tap");
    root.frame();
    check(soloed() == std::vector<std::int64_t>{1}, "1 MIDI stays soloed");
    root.command(AppCommands::Undo);
    root.frame();
    check(root.latchKey(AppCommands::ActivateTrack1 + 2, juce::KeyPress::F3Key, true, 11000), "F3 down");
    root.frame();
    check(muted() == std::vector<std::int64_t>{3}, "F3 held deactivates the third track while it is held");
    check(root.latchKey(0, juce::KeyPress::F3Key, false, 11500), "F3 let go after 500 ms");
    root.frame();
    check(muted().empty() && lastStep() == beforeLatch, "the third track is active again");
    check(root.latchKey(AppCommands::SoloSelected, 's', true, 12000), "S down again");
    root.frame();
    if (auto *two = rowOf(root.mixer, 2))
        two->solo.onClick();
    root.frame();
    check(!root.latchKey(0, 's', false, 12800), "a solo clicked while S is held: let go, nothing goes back");
    root.frame();
    check(soloed() == std::vector<std::int64_t>{2}, "the click's solo stays");
    root.command(AppCommands::Undo);
    root.command(AppCommands::Undo);
    root.frame();
    check(soloed().empty() && lastStep() == beforeLatch, "two undos: nothing soloed");
    root.arrangement.geometry.selection.start = 0;
    root.arrangement.geometry.selection.end = 4 * textproj::kPPQ;
    const double zoomScale = root.arrangement.geometry.scale, zoomLeft = root.arrangement.geometry.left;
    check(root.latchKey(AppCommands::ZoomToSelection, 'z', true, 13000) && root.arrangement.geometry.scale != zoomScale,
          "Z down zooms to the time selection");
    check(root.latchKey(0, 'z', false, 13700) && root.arrangement.geometry.scale == zoomScale &&
              root.arrangement.geometry.left == zoomLeft,
          "let go after 700 ms: the zoom goes back");
    check(root.latchKey(AppCommands::ZoomToSelection, 'z', true, 14000) && !root.latchKey(0, 'z', false, 14100) &&
              root.arrangement.geometry.scale != zoomScale,
          "a tap of Z stays zoomed");
    root.command(AppCommands::ZoomBack);
    root.arrangement.geometry.selection.start = root.arrangement.geometry.selection.end = 0;
    check(root.arrangement.geometry.scale == zoomScale, "Zoom Back, as before");
    // Live 12 manual, Live Concepts: "Double-clicking a track's title bar brings up the Device View".
    check(root.command(AppCommands::ToggleDeviceView) && !root.state().docked, "the Device View hidden");
    auto &headers = root.arrangement.headers;
    const juce::Point<float> title{20.f, static_cast<float>(laneTop(root, 2) + 8)};
    headers.mouseDown(pressAt(headers, title));
    headers.mouseUp(pressAt(headers, title));
    headers.mouseDoubleClick(pressAt(headers, title));
    check(root.state().docked && root.arrangement.geometry.selection.track == 2,
          "a double-click on 2 MIDI's title selects it and brings up the Device View");
    render("adi-title-double-click.png");
    // The Mixer View strip's title does the same.
    check(root.command(AppCommands::ToggleDeviceView) && !root.state().docked, "hidden again");
    check(root.command(AppCommands::ToggleMixerView), "the Mixer View opens");
    root.resizeMixerView(320);
    root.mark(DirtySet::All);
    root.frame();
    if (auto *strip = rowOf(root.mixerView, 3)) {
        strip->mouseDoubleClick(pressAt(*strip, {20.f, 8.f}));
        check(root.state().docked && root.arrangement.geometry.selection.track == 3,
              "a double-click on 3 Audio's strip title selects it and brings up the Device View");
        check(root.command(AppCommands::ToggleDeviceView) && !root.state().docked, "hidden once more");
        strip->mouseDoubleClick(pressAt(*strip, {20.f, 200.f}));
        check(!root.state().docked, "below the title, a double-click leaves the Device View hidden");
        check(root.command(AppCommands::ToggleDeviceView) && root.state().docked, "and shown again");
    } else
        check(false, "3 Audio's Mixer View strip");
    check(root.command(AppCommands::ToggleMixerView), "the Mixer View closes");
    // The sections on the bar over the names repeat the View menu, as Live's selectors do: R hides the
    // returns, I-O the In/Out, S the sends; each is lit while its part shows.
    check(root.namesBar.returns.getToggleState() && root.namesBar.inOut.getToggleState() &&
              root.namesBar.sends.getToggleState() == root.state().sends &&
              root.namesBar.mixer.getToggleState() && !root.namesBar.delay.getToggleState(),
          "I-O, S, R and M lit, D not, as the sheet draws them");
    {
        const bool sends = root.state().sends;
        root.namesBar.sends.onClick();
        check(root.state().sends != sends && root.namesBar.sends.getToggleState() == root.state().sends,
              "S hides or shows the sends, and is lit while they show");
        root.namesBar.sends.onClick();
        check(root.state().sends == sends && root.namesBar.sends.getToggleState() == sends, "and back");
    }
    // The bar's parts in its order, each inside it, none overlapping the next: Flip, Show, the sections,
    // H and W, Automation Mode, Lock Envelopes.
    {
        auto &bar = root.namesBar;
        int edge = 0;
        bool ordered = true;
        for (juce::Component *c : std::initializer_list<juce::Component *>{
                 &bar.flip, &bar.show, &bar.inOut, &bar.sends, &bar.returns, &bar.mixer, &bar.delay, &bar.height,
                 &bar.width, &bar.automation, &bar.lock}) {
            ordered = ordered && c->isVisible() && c->getX() >= edge && c->getRight() <= bar.getWidth() &&
                      c->getWidth() > 0 && c->getY() >= 0 && c->getBottom() <= bar.getHeight();
            edge = c->getRight();
        }
        check(ordered && bar.show.getWidth() >= 60, "the bar's parts sit in the sheet's order, inside it");
        // H and W are Live's Optimize Arrangement Width and Height: what their commands do.
        auto &geometry = root.arrangement.geometry;
        root.command(AppCommands::OptimizeWidth);
        const double fitted = geometry.scale;
        geometry.scale = fitted * 3;
        bar.width.onClick();
        root.frame();
        check(geometry.scale == fitted, "W fits the song across the lanes, as Optimize Arrangement Width");
        root.command(AppCommands::OptimizeHeight);
        const auto measured = geometry.selection.track;
        const int tall = geometry.heightFor(measured);
        root.command(AppCommands::ShrinkTracks);
        const int shrunk = geometry.heightFor(measured);
        bar.height.onClick();
        root.frame();
        check(tall > 0 && shrunk != tall && geometry.heightFor(measured) == tall,
              "H fills the lanes again after the selected track shrank, as Optimize Arrangement Height");
    }
    root.namesBar.returns.onClick();
    check(root.state().hideReturns && !root.namesBar.returns.getToggleState() &&
              shownLanes() == std::vector<std::int64_t>{1, 2, 3, 4, 7},
          "R hides the returns and goes dark");
    root.namesBar.returns.onClick();
    root.namesBar.inOut.onClick();
    check(!root.state().hideReturns && root.namesBar.returns.getToggleState() && !root.state().inOut &&
              !root.namesBar.inOut.getToggleState() && !rowOf(root.mixer, 1)->showsInOut(),
          "R shows them again; I-O hides the In/Out");
    root.namesBar.inOut.onClick();
    check(root.state().inOut && root.namesBar.inOut.getToggleState(), "and shows it again");
    // Live drags the returns among themselves, at the foot: B above A. Their letters and the sends
    // follow their places.
    {
        root.mark(DirtySet::All);
        root.frame();
        const juce::Point<float> from{20.f, static_cast<float>(laneTop(root, 6) + 8)};
        const juce::Point<float> to{20.f, static_cast<float>(laneTop(root, 5) + 3)};
        headers.mouseDown(pressAt(headers, from));
        headers.mouseDrag(dragAt(headers, from, to));
        const auto shown = headers.drop();
        headers.mouseUp(dragAt(headers, from, to));
        root.mark(DirtySet::All);
        root.frame();
        auto place = [&root](std::int64_t id) {
            const auto lanesNow = laneOrder(root);
            return std::find(lanesNow.begin(), lanesNow.end(), id) - lanesNow.begin();
        };
        // The first track row on screen with its sends (A, B and the C inserted above): they
        // follow the returns' order.
        auto firstSend = [&root]() -> std::int64_t {
            for (const auto &strip : root.mixer.strips())
                if (strip->isVisible() && strip->track() <= 4 && !strip->sendKnobs().empty())
                    return strip->sendsTo(0);
            return 0;
        };
        auto *moved = rowOf(root.mixer, 6);
        check(shown && shown->to.parent == 0 && shown->to.before == 5 && place(6) < place(5) && place(5) < place(7) &&
                  submitter.nextUndo()->label == "Move Track" && moved != nullptr &&
                  moved->active.getButtonText() == "A" && firstSend() == 6,
              "B Delay's title dragged above A Reverb's: first of the returns, lettered A, its send first; Main last");
        render("adi-return-move.png");
        root.command(AppCommands::Undo);
        root.mark(DirtySet::All);
        root.frame();
        check(place(5) < place(6) && firstSend() == 5, "undo puts A Reverb first again");
        // A return dragged up over the tracks stays among the returns, first.
        headers.mouseDown(pressAt(headers, from));
        headers.mouseDrag(dragAt(headers, from, {20.f, 10.f}));
        const auto over = headers.drop();
        headers.mouseUp(dragAt(headers, from, {20.f, 10.f}));
        root.frame();
        check(over && over->to.before == 5 && place(6) < place(5) && place(6) > place(4),
              "B Delay dragged over the tracks: still a return, first of them");
        root.command(AppCommands::Undo);
        root.frame();
    }
}
// Live's file drop (manual 6.1): an audio file on an audio track's lane makes a clip there, named
// after the file, on the nearest grid line; on a MIDI track it is refused; below the tracks it
// makes a new audio track. Each drop is one undo step (#308).
// D4: the browser's devices (Live 12 manual 4.1, 4.10, 41.4; N15's catalogue part, #395). Audio
// Effects lists Live's seven folders with ADI's built-in effects, Pd Devices ADI's own; an entry this
// build cannot load is dimmed and says why; double-click or Enter loads onto the selected track in
// front of the device chosen in the strip; a drag drops onto a lane's track or into the chain; each
// load is one undo step, the reference and the chain made in it.
void browserDevices(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "browser-devices.adi", true, {}, error);
    check(document != nullptr, "a project for the browser's devices");
    if (!document)
        return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "devices");
    UiPeerRoot surfacePeer;
    surfacePeer.setSize(1600, 1000);surfacePeer.addAndMakeVisible(root);
    surfacePeer.setVisible(true);surfacePeer.addToDesktop(0);
    root.afterEdit = [&] { document->synchronise(); };
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const std::vector<std::int64_t> &ids) {
        document->setParameterDemand(owner, ids);
    };
    root.loadBuiltin = [&](const std::string &uid, std::int64_t track, std::optional<std::int64_t> before) {
        const bool ok = document->insertBuiltin(uid, track, before);
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.setSize(1440, 900);
    const auto refresh = [&] {
        document->tick(0);
        root.frame();
        root.frame();
    };
    // 3 Audio, track 4: no chain of its own yet.
    root.arrangement.geometry.selection.track = 4;
    refresh();
    auto &browser = root.browser;
    int audioEffects = -1, pdDevices = -1, plugIns = -1;
    for (std::size_t i = 0; i < browser.sidebar().size(); ++i) {
        const auto &e = browser.sidebar()[i];
        if (e.name == "Audio Effects")
            audioEffects = static_cast<int>(i);
        if (e.name == "Pd Devices")
            pdDevices = static_cast<int>(i);
        if (e.name == "Plug-Ins")
            plugIns = static_cast<int>(i);
    }
    browser.choose(audioEffects);
    std::vector<juce::String> folders;
    bool dimmedRight = true;
    int rowOf = -1;
    const auto find = [&](const char *name) {
        for (std::size_t i = 0; i < browser.rows().size(); ++i)
            if (browser.rows()[i].device != nullptr && browser.rows()[i].device->name == name)
                return static_cast<int>(i);
        return -1;
    };
    for (const auto &r : browser.rows()) {
        if (r.heading.isNotEmpty())
            folders.push_back(r.heading);
        // Every entry loads but Hybrid Reverb with Pd on; none with it off.
#if ADI_WITH_PD
        const bool expected = r.device == nullptr || r.device->name != "Hybrid Reverb";
#else
        const bool expected = r.device == nullptr;
#endif
        if (r.device != nullptr && BrowserPanel::unavailable(*r.device).empty() != expected)
            dimmedRight = false;
    }
    check(folders == std::vector<juce::String>{"EQ & Filters", "Delay & Loop", "Dynamics", "Drive & Color",
                                               "Pitch & Modulation", "Reverb & Resonance", "Utilities"} &&
              dimmedRight && find("Auto Filter") >= 0 && find("Chord Comb") < 0,
          "Audio Effects lists Live's seven folders with their effects, each dimmed when this build cannot load it");
    rowOf = find("Hybrid Reverb");
    check(rowOf >= 0 && !browser.load(rowOf) && root.error().find("not built in ADI yet") != std::string::npos,
          "Hybrid Reverb is dimmed always, and a double-click says why");
    browser.choose(pdDevices);
    check(browser.rows().size() == 3 && browser.rows()[0].device != nullptr && browser.rows()[1].device != nullptr &&
              browser.rows()[2].device != nullptr,
          "Pd Devices lists ADI's own: ADI-Shaper, Chord Comb and Color Cab");
    // Plug-Ins (DF-W9): outside the app nothing scans, and it says where they list.
    browser.choose(plugIns);
    check(root.error().find("list in the application") != std::string::npos && browser.rows().empty(),
          "Plug-Ins outside the app says the plug-ins list in the application");
    {
        // The loader's VST3 lines as `knownPlugins` writes them, and a line of another format.
        const auto echo = parseKnownVst3("vst3  VST3-Echo-1a2b-3c4d  Echo -- Zeta Audio 1.2.0  (/x/Echo.vst3)");
        const auto bare = parseKnownVst3("vst3  VST3-Hiss-1  Hiss --  0.9  (/x/Hiss.vst3)");
        check(echo && echo->uid == "VST3-Echo-1a2b-3c4d" && echo->name == "Echo" && echo->vendor == "Zeta Audio" &&
                  echo->version == "1.2.0" && echo->path == "/x/Echo.vst3" &&
                  echo->kind == PluginEntry::Kind::Unknown && bare && bare->name == "Hiss" && bare->vendor.empty() &&
                  !parseKnownVst3("clap  org.x  X -- Y 1  (/x.clap)") && !parseKnownVst3("vst3  broken"),
              "a VST3 line of the loader reads as its uid, name, vendor, version and file");
        check(clapKind(true, "instrument synthesizer") == PluginEntry::Kind::Instrument &&
                  clapKind(false, "note-effect utility") == PluginEntry::Kind::MidiEffect &&
                  clapKind(false, "audio-effect delay") == PluginEntry::Kind::Effect,
              "a CLAP is an instrument, a MIDI effect (a note effect) or an audio effect");
        PluginListing found;
        found.plugins = {*echo,
                         {"clap", "org.adi.glass", "Glass Piano", "Northwind Audio", "1.0", "/x/glass.clap",
                          PluginEntry::Kind::Instrument},
                         {"clap", "org.adi.arp", "Arpeggiate", "Northwind Audio", "1.0", "/x/arp.clap",
                          PluginEntry::Kind::MidiEffect},
                         *bare};
        found.failed = 1;
        // The app's scan as the Browser sees it (DF-W34): choosing Plug-Ins asks for a scan and returns at
        // once; the list fills when the scan's state arrives.
        int asked = 0;
        PluginScan scan;
        browser.scanPlugins = [&] {
            ++asked;
            scan.phase = PluginScan::Phase::Scanning;
            browser.setPluginScan(scan);
        };
        root.say({});
        browser.choose(plugIns);
        check(asked == 1 && browser.rows().size() == 1 && browser.rows()[0].heading == "Scanning plug-ins…" &&
                  root.error() == "Scanning plug-ins…",
              "choosing Plug-Ins asks for a scan and returns at once; the list and the Status Bar say it scans");
        scan.candidates = 150;
        browser.setPluginScan(scan);
        check(browser.rows().size() == 1 && browser.rows()[0].heading == "Scanning plug-ins: 150 files…",
              "the files the scan probes are counted on the list's line");
        scan.phase = PluginScan::Phase::Done;
        scan.candidates = 0;
        scan.listing = found;
        browser.setPluginScan(scan);
        std::vector<std::pair<juce::String, int>> layout;
        for (const auto &r : browser.rows())
            layout.emplace_back(r.plugin != nullptr ? juce::String(r.plugin->name) : r.heading, r.depth);
        const std::vector<std::pair<juce::String, int>> expected{
            {"CLAP", 0}, {"Northwind Audio", 1}, {"Arpeggiate", 2}, {"Glass Piano", 2},
            {"VST3", 0}, {"Other", 1},           {"Hiss", 2},       {"Zeta Audio", 1}, {"Echo", 2}};
        check(layout == expected,
              "Plug-Ins lists a folder per format, then per vendor, the plug-ins by name; no vendor goes in Other");
        check(root.error() == "1 plug-in file could not be scanned; the rest are listed.",
              "a file a complete scan could not read is counted; the rest are listed");
        browser.choose(plugIns);
        check(asked == 1 && browser.rows().size() == expected.size(),
              "choosing Plug-Ins again lists what was found and scans nothing");
        // A scan running over a listed catalogue: its line heads the list, and a count during it changes
        // only that line, so the selection stays (DF-W34 on N58).
        scan.phase = PluginScan::Phase::Scanning;
        scan.candidates = 150;
        scan.completed = 0;
        ++scan.listed;
        browser.setPluginScan(scan);
        check(browser.rows().size() == expected.size() + 1 && browser.rows()[0].heading == "Scanning plug-ins: 150 files…" &&
                  browser.rows()[1].heading == "CLAP",
              "while a scan runs its line heads what was listed before");
        browser.list.selectRow(3);
        scan.completed = 12;
        browser.setPluginScan(scan);
        check(browser.list.getSelectedRow() == 3 && browser.rows()[0].heading == "Scanning plug-ins: 12 of 150 files…",
              "a count changes the line only; the selected plug-in stays selected");
        scan.phase = PluginScan::Phase::Done;
        scan.candidates = scan.completed = 0;
        ++scan.listed;
        browser.setPluginScan(scan);
        check(browser.rows().size() == expected.size(), "the scan ended, its line goes");
        const auto rowNamed = [&browser](const char *name) {
            for (std::size_t i = 0; i < browser.rows().size(); ++i)
                if (browser.rows()[i].plugin != nullptr && browser.rows()[i].plugin->name == name)
                    return static_cast<int>(i);
            return -1;
        };
        const int glass = rowNamed("Glass Piano");
        check(glass >= 0 && !browser.load(glass) && root.error().find("application's plug-in host") != std::string::npos,
              "outside the application a double-click on a plug-in says it needs the application's host");
        // DF-W9's load on N57: the reference handed over, its kind the subtype that places it.
        std::vector<std::tuple<rows::PluginRef, std::int64_t, bool>> loaded;
        root.loadPlugin = [&loaded](rows::PluginRef ref, std::int64_t track, std::optional<std::int64_t>, bool guessed) {
            loaded.emplace_back(std::move(ref), track, guessed);
            return true;
        };
        std::int64_t audioTrack = 0, midiTrack = 0;
        for (const auto &t : root.reader().tracks())
            if (t && t->kind == "audio" && audioTrack == 0)
                audioTrack = t->id;
            else if (t && t->kind == "midi" && midiTrack == 0)
                midiTrack = t->id;
        root.arrangement.geometry.selection.track = midiTrack;
        check(midiTrack != 0 && browser.load(glass) && loaded.size() == 1 && std::get<1>(loaded[0]) == midiTrack &&
                  std::get<0>(loaded[0]).format == "clap" && std::get<0>(loaded[0]).uid == "org.adi.glass" &&
                  std::get<0>(loaded[0]).subtype == "instrument" && std::get<0>(loaded[0]).pathHint == "/x/glass.clap" &&
                  std::get<0>(loaded[0]).vendor == "Northwind Audio" && !std::get<2>(loaded[0]),
              "a CLAP instrument loads onto the selected track as an instrument, with its identity and file, not guessed");
        const int echoRow = rowNamed("Echo");
        root.arrangement.geometry.selection.track = audioTrack;
        check(audioTrack != 0 && browser.load(echoRow) && loaded.size() == 2 && std::get<0>(loaded[1]).format == "vst3" &&
                  std::get<0>(loaded[1]).subtype == "effect" && std::get<2>(loaded[1]),
              "a VST3 of unknown kind onto an audio track is guessed an effect");
        root.arrangement.geometry.selection.track = midiTrack;
        check(browser.load(echoRow) && loaded.size() == 3 && std::get<0>(loaded[2]).subtype == "instrument" &&
                  std::get<2>(loaded[2]),
              "onto a MIDI track, an instrument");
        // A plug-in the project already holds keeps its stored kind (one reference per plug-in).
        document->store().db().exec("INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(990,'vst3','VST3-Echo-1a2b-3c4d','Echo','effect')");
        check(storedSubtype(document->store(), "vst3", "VST3-Echo-1a2b-3c4d") == std::optional<std::string>("effect") &&
                  !storedSubtype(document->store(), "vst3", "VST3-Hiss-1"),
              "the project's stored kind for a plug-in it holds, and none for one it does not");
        document->store().db().exec("DELETE FROM plugin_refs WHERE id = 990");
        root.arrangement.geometry.selection.track = 0;
        check(!browser.load(echoRow) && loaded.size() == 3 && root.error().rfind("Select a track", 0) == 0,
              "with no track selected it says to select one");
        root.loadPlugin = nullptr;
        browser.search.setText("zeta", false);
        browser.search.onTextChange();
        layout.clear();
        for (const auto &r : browser.rows())
            layout.emplace_back(r.plugin != nullptr ? juce::String(r.plugin->name) : r.heading, r.depth);
        check(asked == 1 && layout == std::vector<std::pair<juce::String, int>>{{"VST3", 0}, {"Zeta Audio", 1}, {"Echo", 2}},
              "the search keeps a plug-in by its vendor too, and asks for no scan");
        browser.search.setText({}, false);
        browser.search.onTextChange();
        // DF-W34 (c): one folder per vendor ignoring case, spelled as the first in sort order; no vendor
        // and "other" share one folder.
        PluginScan spelled;
        spelled.phase = PluginScan::Phase::Done;
        spelled.listing.plugins = {{"vst3", "a", "Pro-C 2", "FabFilter", "1", "/a.vst3"},
                                   {"vst3", "b", "Pro-L 2", "Fabfilter", "1", "/b.vst3"},
                                   {"vst3", "c", "Pro-Q 4", "FabFilter", "1", "/c.vst3"},
                                   {"vst3", "d", "Hum", "", "1", "/d.vst3"},
                                   {"vst3", "e", "Fizz", "other", "1", "/e.vst3"}};
        browser.setPluginScan(spelled);
        layout.clear();
        for (const auto &r : browser.rows())
            layout.emplace_back(r.plugin != nullptr ? juce::String(r.plugin->name) : r.heading, r.depth);
        check(layout == std::vector<std::pair<juce::String, int>>{{"VST3", 0},    {"FabFilter", 1}, {"Pro-C 2", 2},
                                                                    {"Pro-L 2", 2}, {"Pro-Q 4", 2},   {"other", 1},
                                                                    {"Fizz", 2},    {"Hum", 2}},
              "one vendor spelled two ways is one folder, and no vendor goes in with one named other");
        scan.listing = {};
        browser.setPluginScan(scan);
        browser.choose(plugIns);
        check(browser.rows().empty() && root.error().rfind("No VST3 or CLAP plug-ins were found", 0) == 0,
              "with none found by a complete scan, it says where to add their folders");
        // DF-W34 (b): a scan that stopped early is named, with why, and never worded as "the rest".
        scan.listing.complete = false;
        scan::Report missing;
        missing.complete = false;
        missing.failures.push_back({"/x/Foo.vst3", std::string(kHelperUnavailable), scan::State::Unavailable});
        applyReport(scan.listing, missing);
        browser.setPluginScan(scan);
        browser.choose(plugIns);
        check(root.error() == "The plug-in scan stopped early: the plug-in scanner could not run (its helper, "
                              "adi_plugin_scanner, is missing or could not start). Nothing could be listed. Rescan "
                              "from the Settings' Plug-Ins page." &&
                  asked == 1,
              "a scan that stopped early says why and where to rescan, and choosing Plug-Ins does not rescan");
        scan.listing.plugins = found.plugins;
        browser.setPluginScan(scan);
        browser.choose(plugIns);
        check(root.error().find("The list may be incomplete.") != std::string::npos &&
                  root.error().find("the rest are listed") == std::string::npos,
              "with plug-ins kept from before, a stopped scan says the list may be incomplete");
        browser.scanPlugins = nullptr;
        browser.setPluginScan({});
    }
    {
        // The loader's report read into the listing (DF-W34 b).
        scan::Report service;
        service.complete = false;
        service.failures.push_back({"", "the registry could not be opened", scan::State::Unavailable});
        PluginListing l;
        applyReport(l, service);
        check(!l.complete && l.failed == 0 && l.stopped == "the plug-in scanner could not run (the registry could not be opened)",
              "a failure with no file is the scanner's own, not a plug-in's");
        scan::Report helper; // the helper missing or blocked: the probe names the file it was asked to read
        helper.complete = false;
        helper.failures.push_back({"/x/Foo.vst3", std::string(kHelperUnavailable), scan::State::Unavailable});
        applyReport(l, helper);
        check(!l.complete && l.stopped.find("the plug-in scanner could not run") == 0 &&
                  l.stopped.find("Foo.vst3") == std::string::npos,
              "a helper that could not start is the scanner's failure, not the file's");
        scan::Report file;
        file.complete = false;
        file.failures.push_back({"/x/A.vst3", "quarantined", scan::State::Quarantined});
        file.failures.push_back({"/x/B.vst3", "bad metadata", scan::State::Invalid});
        applyReport(l, file);
        check(!l.complete && l.failed == 0 && l.stopped == "/x/B.vst3 stopped the scan (bad metadata)",
              "the file whose failure ended the scan is named");
        scan::Report cancelled;
        cancelled.complete = false;
        cancelled.cancelled = true;
        applyReport(l, cancelled);
        check(!l.complete && l.stopped == "the scan was stopped before it finished", "a cancelled scan says it was stopped");
        scan::Report done;
        done.failures.push_back({"/x/A.vst3", "crashed", scan::State::Failed});
        done.failures.push_back({"/x/C.clap", "quarantined", scan::State::Quarantined});
        applyReport(l, done);
        check(l.complete && l.stopped.empty() && l.failed == 2, "a complete scan counts the files it could not read");
    }
    {
        // The catalogue's service (DF-W34 on N58): it starts the loader's discovery jobs, reads their
        // progress and adopts each that has finished, on the owner; a stand-in for the loader's job API.
        using Outcome = DiscoveryEnd::Outcome;
        std::uint64_t nextJob = 1, cancelled = 0;
        std::vector<DiscoveryMode> started;
        DiscoveryProgressRead progress;
        std::optional<DiscoveryEnd> result;
        PluginListing catalogue;
        catalogue.plugins = {{"vst3", "VST3-Glass-1", "Glass Synth", "Northwind Audio", "1.0", "/x/Glass.vst3",
                              PluginEntry::Kind::Instrument},
                             {"clap", "org.zeta.echo", "Echo", "Zeta Audio", "2", "/x/Echo.clap", PluginEntry::Kind::Effect}};
        std::string refusal;
        DiscoveryPort port;
        port.start = [&](DiscoveryMode mode) {
            DiscoveryAdmission a;
            if (!refusal.empty()) {
                a.refusal = refusal;
                return a;
            }
            a.job = nextJob++;
            started.push_back(mode);
            return a;
        };
        port.progress = [&] { return progress; };
        port.adopt = [&](std::uint64_t job) {
            return job + 1 == nextJob ? std::exchange(result, std::nullopt) : std::nullopt;
        };
        port.cancel = [&](std::uint64_t job) {
            cancelled = job;
            return true;
        };
        port.listing = [&] { return catalogue; };
        const auto finish = [&](Outcome outcome, scan::Report report = {}) {
            DiscoveryEnd e;
            e.outcome = outcome;
            e.report = std::move(report);
            result = std::move(e);
        };
        PluginCatalogueService service(port);
        int changes = 0;
        service.onChange = [&] { ++changes; };
        service.showCatalogue();
        check(service.state().listing.plugins.size() == 2 && service.state().phase == PluginScan::Phase::Done &&
                  service.state().listing.plugins[0].kind == PluginEntry::Kind::Instrument && started.empty(),
              "the stored catalogue is listed at once, a VST3 with its kind from the catalogue (N58), no job yet");
        service.request(DiscoveryMode::Incremental);
        check(service.running() && started == std::vector{DiscoveryMode::Incremental} &&
                  service.state().phase == PluginScan::Phase::Scanning && service.state().listing.plugins.size() == 2,
              "an Incremental discovery starts and returns at once; the list stays meanwhile");
        progress.job = service.job();
        progress.hasTotal = true;
        progress.total = 150;
        progress.completed = 12;
        service.poll();
        check(service.running() && pluginScanStatus(service.state()) == "Scanning plug-ins: 12 of 150 files…",
              "its progress is read on the owner and said");
        catalogue.plugins.pop_back();
        finish(Outcome::Published);
        service.poll();
        check(!service.running() && service.state().phase == PluginScan::Phase::Done &&
                  service.state().listing.plugins.size() == 1 && service.state().listing.complete,
              "once finished it is adopted, and the catalogue it published is listed");
        // Rescan during an Incremental: the Incremental stops quietly, and the Refresh follows.
        service.request(DiscoveryMode::Incremental);
        const auto first = service.job();
        service.request(DiscoveryMode::Refresh);
        check(cancelled == first && service.job() == first, "Rescan stops a running Incremental first");
        finish(Outcome::Cancelled);
        service.poll();
        check(service.running() && started.back() == DiscoveryMode::Refresh && service.state().listing.complete &&
                  service.state().phase == PluginScan::Phase::Scanning,
              "then the Refresh starts; a stop the app made is not named as the scan stopping early");
        service.request(DiscoveryMode::Incremental);
        check(cancelled == first, "an Incremental asked for during a Refresh waits for it; nothing is stopped");
        finish(Outcome::Published);
        service.poll();
        check(service.running() && started.back() == DiscoveryMode::Incremental, "and follows it");
        // A project opened while a job runs waits for the job's adoption, so it finds what the job found;
        // nothing is stopped for it. A folder change leaving the job stale makes it wait for the rerun.
        int opened = 0;
        bool jobBesideOpen = true;
        std::uint64_t listedAtOpen = 0;
        const auto running = service.job();
        const auto cancelledBefore = cancelled;
        service.settleThen([&] {
            ++opened;
            jobBesideOpen = service.running();
            listedAtOpen = service.state().listed;
        });
        check(opened == 0 && cancelled == cancelledBefore && service.job() == running,
              "an open during a job waits for it, and stops nothing");
        finish(Outcome::Stale);
        service.poll();
        check(opened == 0 && service.running() && started.back() == DiscoveryMode::Incremental && started.size() == 5,
              "a stale job runs again on the new folders, and the open waits for that run");
        const auto listedBefore = service.state().listed;
        catalogue.plugins.push_back({"clap", "org.new.one", "New One", "New", "1", "/x/New.clap", PluginEntry::Kind::Effect});
        finish(Outcome::Published);
        service.poll();
        check(opened == 1 && !jobBesideOpen && listedAtOpen > listedBefore && !service.running() &&
                  service.state().listing.plugins.size() == 2,
              "it opens once the job is adopted, with no job beside it, the plug-in the job found listed");
        service.settleThen([&] { ++opened; });
        check(opened == 2, "with no job running, an open runs at once");
        // The open runs between jobs: after the running job's adoption, before the job queued behind it.
        service.request(DiscoveryMode::Incremental);
        jobBesideOpen = true;
        service.settleThen([&] {
            ++opened;
            jobBesideOpen = service.running();
        });
        service.request(DiscoveryMode::Incremental); // queued behind the running one, which goes on
        finish(Outcome::Published);
        service.poll();
        check(opened == 3 && !jobBesideOpen && service.running(), "the open runs before the queued job starts");
        finish(Outcome::Published);
        service.poll();
        // An open waiting on a job that a Rescan stops waits for the Refresh, which publishes what it finds.
        service.request(DiscoveryMode::Incremental);
        service.settleThen([&] { ++opened; });
        service.request(DiscoveryMode::Refresh);
        finish(Outcome::Cancelled);
        service.poll();
        check(opened == 3 && service.running() && started.back() == DiscoveryMode::Refresh,
              "the stopped job published nothing: the open waits for the Refresh");
        finish(Outcome::Published);
        service.poll();
        check(opened == 4 && !service.running(), "and runs once the Refresh is adopted");
        // A Rescan whose stop comes too late (the Incremental had already finished): the Incremental is
        // adopted as it ended, and the Refresh follows once, not twice.
        bool acceptCancel = false;
        port.cancel = [&](std::uint64_t job) {
            cancelled = job;
            return acceptCancel;
        };
        PluginCatalogueService late(port);
        late.request(DiscoveryMode::Incremental);
        late.request(DiscoveryMode::Refresh);
        const auto startedBefore = started.size();
        finish(Outcome::Published);
        late.poll();
        check(late.running() && started.size() == startedBefore + 1 && started.back() == DiscoveryMode::Refresh &&
                  late.state().listing.complete,
              "a refused stop: the finished job is adopted, then the Refresh runs");
        finish(Outcome::Published);
        late.poll();
        check(!late.running() && started.size() == startedBefore + 1, "and nothing runs twice");
        late.stop();
        // A count during a job changes no list: the Browser keeps its rows and selection.
        service.request(DiscoveryMode::Incremental);
        const auto listedDuring = service.state().listed;
        progress.job = service.job();
        progress.completed = 40;
        service.poll();
        check(service.state().listed == listedDuring && service.state().completed == 40,
              "a progress count leaves the list's count as it was");
        finish(Outcome::Published);
        service.poll();
        // A job that failed: what was listed stays, and why it stopped is named.
        service.request(DiscoveryMode::Refresh);
        scan::Report helper;
        helper.complete = false;
        helper.failures.push_back({"", "the helper could not start", scan::State::Unavailable});
        finish(Outcome::Failed, helper);
        service.poll();
        check(!service.running() && !service.state().listing.complete && service.state().listing.plugins.size() == 2 &&
                  service.state().listing.stopped == "the plug-in scanner could not run (the helper could not start)",
              "a failed job keeps the list and names why it stopped");
        // The person's own cancel (none here yet) and a job that could not start.
        refusal = "the plug-in scanner could not run (helper missing)";
        service.request(DiscoveryMode::Refresh);
        check(!service.running() && service.state().phase == PluginScan::Phase::Done &&
                  service.state().listing.stopped == "the plug-in scanner could not run (helper missing)",
              "a job that could not start is named, and nothing runs");
        refusal.clear();
        // Quit: the running job is cancelled, nothing more starts, and the waiting opens are dropped.
        service.request(DiscoveryMode::Incremental);
        const auto last = service.job();
        service.settleThen([&] { ++opened; });
        service.stop();
        finish(Outcome::Cancelled);
        service.poll();
        service.request(DiscoveryMode::Refresh);
        check(cancelled == last && !service.running() && opened == 4 && started.back() == DiscoveryMode::Incremental,
              "Quit cancels the job; nothing starts after it and no waiting open runs");
        check(changes > 0, "the owner was told of each change");
    }
    browser.choose(audioEffects);
    const auto chainOf = [&](std::int64_t track) {
        std::vector<std::string> out;
        SQLite::Statement q(document->store().db(),
                            "SELECT d.name FROM devices d JOIN device_chains c ON c.id = d.chain_id "
                            "WHERE c.track_id = ? ORDER BY d.ord, d.id");
        q.bind(1, track);
        while (q.executeStep())
            out.push_back(q.getColumn(0).getString());
        return out;
    };
    [[maybe_unused]] const auto chains = [&](std::int64_t track) {
        return document->store().db().execAndGet("SELECT count(*) FROM device_chains WHERE track_id = " +
                                                 std::to_string(track)).getInt();
    };
#if ADI_WITH_PD
    check(browser.load(find("Auto Filter")), "a double-click on Auto Filter loads it");
    refresh();
    check(chainOf(4) == std::vector<std::string>{"Auto Filter"} && chains(4) == 1 &&
              document->ops().nextUndo() && document->ops().nextUndo()->label == "Insert Auto Filter",
          "onto 3 Audio, its chain made with it: one step, Insert Auto Filter");
    check(root.command(AppCommands::Undo), "undo");
    refresh();
    check(chainOf(4).empty() && chains(4) == 0, "one undo takes the device and the chain away together");
    check(browser.load(find("Utility")), "Utility");
    refresh();
    check(root.devices.panels.size() == 1, "the strip shows it");
    // In front of the device chosen in the strip, though the browser now has the focus.
    root.devices.chose(root.devices.panels[0]->id);
    check(browser.load(find("EQ Eight")), "EQ Eight with Utility chosen");
    refresh();
    check(chainOf(4) == std::vector<std::string>{"EQ Eight", "Utility"}, "goes in front of it");
    // A drag: into the chain past the last device, and onto another track's lane.
    const juce::DragAndDropTarget::SourceDetails reverb(
        BrowserPanel::deviceDescription(*findBuiltin("org.adidaw.builtin.reverb")), nullptr,
        {root.devices.getWidth() - 20, root.devices.getHeight() / 2});
    check(root.devices.isInterestedInDragSource(reverb), "the strip takes a dragged device");
    root.devices.itemDropped(reverb);
    refresh();
    check(chainOf(4) == std::vector<std::string>{"EQ Eight", "Utility", "Reverb"}, "dropped past the last device: at the end");
    auto &canvas = root.arrangement.canvas;
    const juce::DragAndDropTarget::SourceDetails onLane(
        BrowserPanel::deviceDescription(*findBuiltin("org.adidaw.builtin.delay")), nullptr,
        {40, laneTop(root, 3) + 10});
    check(canvas.isInterestedInDragSource(onLane), "the lanes take a dragged device");
    const auto clipsBefore = document->store().db().execAndGet("SELECT count(*) FROM clips").getInt();
    canvas.itemDropped(onLane);
    refresh();
    check(chainOf(3) == std::vector<std::string>{"Delay"} &&
              document->store().db().execAndGet("SELECT count(*) FROM clips").getInt() == clipsBefore,
          "dropped on 2 MIDI's lane: onto that track, never a clip in the lane");
#else
    check(!browser.load(find("Auto Filter")) && root.error().find("leaves out") != std::string::npos &&
              chainOf(4).empty(),
          "with Pd off, Auto Filter is refused with the reason, and nothing is written");
#endif
    // With no track selected a load says to select one (the root's own refusal, Pd or not).
    check(!root.insertDevice("org.adidaw.builtin.utility", 0, std::nullopt) &&
              root.error().find("Select a track") != std::string::npos,
          "with no track selected, a load says to select one");
    // Below the tracks a dragged device says where to drop it, and loads nothing.
    {
        const juce::DragAndDropTarget::SourceDetails below(
            BrowserPanel::deviceDescription(*findBuiltin("org.adidaw.builtin.utility")), nullptr,
            {40, root.arrangement.canvas.getHeight() - 2});
        if (root.arrangement.geometry.trackAt(root.reader(), root.arrangement.canvas.getHeight() - 2) == nullptr) {
            root.arrangement.canvas.itemDropped(below);
            check(root.error().find("on a track's lane") != std::string::npos, "a device dropped below the tracks says to drop it on a lane");
        }
    }
    // The keyboard never lands on a folder's heading: into the list, the first entry is a device.
    browser.choose(audioEffects);
    browser.search.setText("reverb", false);
    browser.search.onTextChange();
    browser.searchKeys().keyPressed(juce::KeyPress(juce::KeyPress::downKey), &browser.search);
    const int picked = browser.list.getSelectedRow();
    check(picked >= 0 && browser.rows()[static_cast<std::size_t>(picked)].device != nullptr &&
              browser.rows()[static_cast<std::size_t>(picked)].device->name == "Hybrid Reverb",
          "search, then Down: the first entry is selected, never the folder's heading");
    browser.list.selectRow(0);
    check(browser.list.getSelectedRow() > 0 && browser.rows()[static_cast<std::size_t>(browser.list.getSelectedRow())].heading.isEmpty(),
          "a heading clicked or stepped onto passes the selection to an entry");
    browser.search.setText({}, false);
    browser.search.onTextChange();
    // The browser with Audio Effects chosen, for the picture beside Live 12's.
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        browser.choose(audioEffects);
        browser.list.selectRow(1);
        juce::Image picture(juce::Image::ARGB, 2 * browser.getWidth(), 2 * browser.getHeight(), true,
                            juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            browser.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-browser-devices.png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(picture, out);
    }
    document->release();
}
// E1: Split at the insert marker (Ctrl/Cmd+E; Live 12 manual 6.12, 41.5), and Delete and 0 over a
// selection that crosses a clip (6.9; N17, #393): one undo step each, from the keys.
void splitAndRangeEdits(AppCommands &commands, settings::AppSettings &settings, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "split.adi", true, {}, error);
    check(document != nullptr, "a project for Split");
    if (!document)
        return;
    const auto bar = textproj::kWhole;
    std::vector<OpRequest> make(2);
    make[0].opType = "clip.create";
    make[0].payload = {{"id", 50}, {"track", 2}, {"kind", "midi"}, {"pos", 0}, {"length", 2 * bar}};
    make[1].opType = "clip.create";
    make[1].payload = {{"id", 51}, {"track", 3}, {"kind", "midi"}, {"pos", 0}, {"length", 2 * bar}};
    check(document->ops().submit(std::move(make)).ok && document->synchronise(), "a two-bar clip on 1 MIDI and on 2 MIDI");
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "split");
    root.afterEdit = [&] { document->synchronise(); };
    root.setSize(1440, 900);
    root.frame();
    commands.activate(root);
    const auto clipsOf = [&](std::int64_t track) {
        std::vector<std::pair<std::int64_t, std::int64_t>> out; // (pos, length)
        if (const auto *t = root.reader().findTrack(track))
            for (const auto &c : t->clips)
                out.emplace_back(c->posTicks, c->lengthTicks);
        std::sort(out.begin(), out.end());
        return out;
    };
    auto &sel = root.arrangement.geometry.selection;
    // Ctrl/Cmd+E inside a clip, editing.splitAllWithNoSelection off: the selected track's clip, two
    // clips, one undo step.
    std::string why;
    check(settings.set("editing.splitAllWithNoSelection", false, {}, why) && (commands.reload(), !commands.splitAllWithNoSelection()),
          "editing.splitAllWithNoSelection off");
    sel = {};
    sel.track = 2;
    sel.insert = bar;
    sel.marked = true;
    const juce::KeyPress split('e', juce::ModifierKeys::commandModifier, 0);
    check(root.keyPressed(split), "Ctrl/Cmd+E");
    root.frame();
    check(clipsOf(2) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, bar}, {bar, bar}} && clipsOf(3).size() == 1 &&
              document->ops().nextUndo() && document->ops().nextUndo()->label == "Split Clip",
          "Ctrl/Cmd+E at bar 2 on 1 MIDI splits its clip there: two clips, one step; 2 MIDI is untouched");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(clipsOf(2).size() == 1, "one undo makes it whole again");
    // Both tracks selected (the selection's far track), the setting still off: each one's clip.
    sel = {};
    sel.track = 2;
    sel.toTrack = 3;
    sel.insert = bar;
    sel.marked = true;
    check(root.command(AppCommands::Split), "Split over two selected tracks");
    root.frame();
    check(clipsOf(2).size() == 2 && clipsOf(3).size() == 2 && root.error().find("one undo step each") != std::string::npos,
          "each selected track's clip splits, and the Status Bar says each is its own step");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "two undos");
    root.frame();
    check(settings.set("editing.splitAllWithNoSelection", true, {}, why) && (commands.reload(), commands.splitAllWithNoSelection()),
          "editing.splitAllWithNoSelection back on (its default)");
    // The window as drawn, for the PR's picture (ADI_UI_PARITY).
    const auto picture = [&](const char *name) {
        const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
        if (folder.isEmpty())
            return;
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile(name);
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(image, out);
    };
    // A clicked clip carries its own span as the selection: Split cuts it at the marker.
    sel = {};
    sel.track = 2;
    sel.clip = 50;
    sel.start = 0;
    sel.end = 2 * bar;
    sel.insert = bar;
    sel.marked = true;
    root.frame();
    picture("adi-split-before.png");
    check(root.keyPressed(split), "Ctrl/Cmd+E with a clip clicked");
    root.frame();
    picture("adi-split-after.png");
    check(clipsOf(2) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, bar}, {bar, bar}} && clipsOf(3).size() == 1,
          "a clicked clip splits at the marker, not refused as a time selection; the other track's clip stays whole");
    check(sel.clip == 50 && sel.start == 0 && sel.end == bar, "the chosen clip stays chosen as the left piece");
    sel.insert = bar / 2;
    check(root.keyPressed(split), "Ctrl/Cmd+E again, the marker in the left piece");
    root.frame();
    check(clipsOf(2).size() == 3, "the left piece splits again");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "two undos");
    root.frame();
    // No clip chosen, the setting on (its default): every track's clip under the marker, though a
    // lane click chose a track.
    sel = {};
    sel.track = 2;
    sel.insert = bar;
    sel.marked = true;
    check(root.command(AppCommands::Split), "Split with no clip chosen");
    root.frame();
    check(clipsOf(2).size() == 2 && clipsOf(3).size() == 2, "splits the clips under the marker on every track");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "two undos");
    root.frame();
    // A track the lanes do not show (the project view leaves it out) is passed over.
    root.arrangement.geometry.hidden = {3};
    sel = {};
    sel.track = 2;
    sel.insert = bar;
    sel.marked = true;
    check(root.command(AppCommands::Split), "Split with 2 MIDI left out of the view");
    root.frame();
    check(clipsOf(2).size() == 2 && clipsOf(3).size() == 1, "only the shown track's clip splits");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // Split on a time selection cuts at its edges: inside one clip, the two-point split, one step.
    sel = {};
    sel.track = 2;
    sel.start = bar / 2;
    sel.end = bar;
    root.frame();
    picture("adi-split-selection-before.png");
    check(root.command(AppCommands::Split), "Split on a selection inside 1 MIDI's clip");
    root.frame();
    picture("adi-split-selection-after.png");
    check(clipsOf(2) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, bar / 2}, {bar / 2, bar / 2}, {bar, bar}} &&
              clipsOf(3).size() == 1,
          "three clips at the selection's edges; the unselected track is untouched");
    check(sel.clip != 0 && sel.clip != 50, "the part split out is selected, as Live leaves it");
    check(root.arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)), "Right moves the selected part");
    root.frame();
    check(clipsOf(2).size() == 3 && clipsOf(2)[1].first > bar / 2, "the part split out moved right by the grid");
    check(root.command(AppCommands::Undo), "undo the move");
    root.frame();
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(clipsOf(2).size() == 1, "one undo makes it whole");
    // A selection with no clip edge-crossing says there is nothing to split.
    sel = {};
    sel.track = 2;
    sel.start = 3 * bar;
    sel.end = 4 * bar;
    check(!root.command(AppCommands::Split) && root.error().find("nothing to split") != std::string::npos,
          "a selection crossing no clip says there is nothing to split");
    // A marker outside every clip says where to put it.
    sel = {};
    sel.track = 2;
    sel.insert = 4 * bar;
    sel.marked = true;
    check(!root.command(AppCommands::Split) && root.error().find("inside a clip") != std::string::npos,
          "a marker outside the clips says to place it inside one");
    // Delete over the clip's second half on 1 MIDI trims it there.
    auto &canvas = root.arrangement.canvas;
    sel = {};
    sel.track = 2;
    sel.start = bar;
    sel.end = 3 * bar;
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "Delete over bars 2 to 3");
    root.frame();
    check(clipsOf(2) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, bar}}, "the clip is trimmed at the selection");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // Delete over a selection inside the clip cuts the middle out, one step.
    sel = {};
    sel.track = 2;
    sel.start = bar / 2;
    sel.end = bar;
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "Delete inside 1 MIDI's clip");
    root.frame();
    check(clipsOf(2) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, bar / 2}, {bar, bar}},
          "the middle is cut out; the two ends stay as clips");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(clipsOf(2).size() == 1, "one undo makes it whole");
    // 0 over 2 MIDI's clip's right edge: split there, the right part deactivated, one step.
    sel = {};
    sel.track = 3;
    sel.start = bar;
    sel.end = 3 * bar;
    check(canvas.keyPressed(juce::KeyPress('0')), "0 over the clip's right edge");
    root.frame();
    bool rightOff = false, leftOn = false;
    if (const auto *t = root.reader().findTrack(3))
        for (const auto &c : t->clips) {
            if (c->posTicks == bar)
                rightOff = c->muted;
            if (c->posTicks == 0)
                leftOn = !c->muted;
        }
    check(clipsOf(3).size() == 2 && rightOff && leftOn, "split at bar 2, the right part deactivated");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(clipsOf(3).size() == 1, "one undo puts the whole clip back");
    // 0 over the edge of a clip already deactivated activates the part inside: 0 toggles.
    {
        OpRequest off;
        off.opType = "clip.setMute";
        off.payload = {{"id", 51}, {"muted", true}};
        off.label = "Deactivate Clip";
        check(document->ops().submit(std::move(off)).ok && document->synchronise(), "2 MIDI's clip deactivated");
        root.frame();
    }
    sel = {};
    sel.track = 3;
    sel.start = bar;
    sel.end = 3 * bar;
    check(canvas.keyPressed(juce::KeyPress('0')), "0 over the deactivated clip's right edge");
    root.frame();
    bool rightOn = false, leftOff = false;
    if (const auto *t = root.reader().findTrack(3))
        for (const auto &c : t->clips) {
            if (c->posTicks == bar)
                rightOn = !c->muted;
            if (c->posTicks == 0)
                leftOff = c->muted;
        }
    check(clipsOf(3).size() == 2 && rightOn && leftOff,
          "split at bar 2, the part inside activated, the part outside still off");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "undo the split and the deactivation");
    root.frame();
    // When the editing policy has the engine refuse to trim, Delete still removes the whole clips
    // inside and says why the crossing clip stays.
    {
        OpRequest inside;
        inside.opType = "clip.create";
        inside.payload = {{"id", 52}, {"track", 2}, {"kind", "midi"}, {"pos", 3 * bar}, {"length", bar / 2}};
        check(document->ops().submit(std::move(inside)).ok && document->synchronise(), "a short clip in bar 4 on 1 MIDI");
        root.frame();
    }
    check(settings.set("editing.mutedClipsAsDeleted", true, {}, why), "editing.mutedClipsAsDeleted on");
    sel = {};
    sel.track = 2;
    sel.start = bar;
    sel.end = 4 * bar;
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "Delete over bars 2 to 4 under the refusing policy");
    root.frame();
    check(clipsOf(2) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 2 * bar}} &&
              root.error().find("stay whole") != std::string::npos,
          "the clip inside goes; the crossing clip stays whole and the Status Bar says why");
    settings.set("editing.mutedClipsAsDeleted", false, {}, why);
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // 0 inside one clip: the two-point split, the middle deactivated, one step.
    sel = {};
    sel.track = 3;
    sel.start = bar / 2;
    sel.end = bar;
    check(canvas.keyPressed(juce::KeyPress('0')), "0 inside 2 MIDI's clip");
    root.frame();
    bool middleOff = false, endsOn = true;
    if (const auto *t = root.reader().findTrack(3))
        for (const auto &c : t->clips) {
            if (c->posTicks == bar / 2)
                middleOff = c->muted;
            else
                endsOn = endsOn && !c->muted;
        }
    check(clipsOf(3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, bar / 2}, {bar / 2, bar / 2}, {bar, bar}} &&
              middleOff && endsOn,
          "three clips, the middle one deactivated");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(clipsOf(3).size() == 1, "one undo puts the whole clip back");
    commands.detach(root);
    document->release();
}
// D5: Color Cab's Impulse tile (MR 9.8; N28, #414). The tile is drawn on Color Cab's panel only,
// beside its controls, with no wrench (a built-in has no plug-in window). A file dropped assigns the
// slot and Clear empties it, each one undo step, with Pd; with Pd off the drop says the device does
// not run in this build.
void colorCabTile(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "color-cab-tile.adi", true, {}, error);
    check(document != nullptr, "a project for Color Cab's tile");
    if (!document)
        return;
    document->store().db().exec("INSERT INTO plugin_refs(id,format,uid,name) VALUES(40,'internal',"
                                "'org.adidaw.builtin.color-cab','Color Cab')");
    std::vector<OpRequest> make(2);
    make[0].opType = "chain.create";
    make[0].payload = {{"id", 40}, {"track", 4}, {"ord", 0}, {"name", "Devices"}};
    make[1].opType = "device.insert";
    make[1].payload = {{"id", 40}, {"chain", 40}, {"ord", 0}, {"ref", 40}, {"name", "Color Cab"}, {"latency", 0}};
    check(document->ops().submit(std::move(make)).ok && document->synchronise(), "Color Cab on 3 Audio");
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "colorcab");
    UiPeerRoot surfacePeer;
    surfacePeer.setSize(1600, 1000);surfacePeer.addAndMakeVisible(root);
    surfacePeer.setVisible(true);surfacePeer.addToDesktop(0);
    root.afterEdit = [&] { document->synchronise(); };
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const std::vector<std::int64_t> &ids) {
        document->setParameterDemand(owner, ids);
    };
    root.devices.assignSample = [&](std::int64_t device, std::int32_t slot, const std::string &path) {
        const bool ok = document->assignSample(device, slot, media::pathFromUtf8(path), 7);
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.devices.clearSample = [&](std::int64_t device, std::int32_t slot) {
        const bool ok = document->clearSample(device, slot);
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.setSize(1440, 900);
    root.arrangement.geometry.selection.track = 4;
    const auto refresh = [&] {
        document->tick(0);
        root.frame();
        root.frame();
    };
    refresh();
    auto &panels = root.devices.panels;
    check(panels.size() == 1 && panels[0]->sample != nullptr && panels[0]->sample->isVisible() &&
              panels[0]->sample->slotName == "Impulse" && panels[0]->sample->state() == SampleTile::State::Empty &&
              !panels[0]->sample->clear.isVisible() && !panels[0]->wrench.isVisible(),
          "Color Cab's panel draws its Impulse tile, empty, with no Clear and no wrench");
    check(panels[0]->sample->getTooltip().contains("Drop a sample") &&
              panels[0]->sample->getX() >= (panels[0]->controls.empty() ? 0 : panels[0]->controls.back()->getRight()),
          "the empty tile hints where to drop, and stands beside the controls, not over them");
    const auto wav = dir / "cab-impulse.wav";
    {
        std::vector<float> silence(256);
        audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Float32);
        writer.write(silence.data(), 256);
    }
    const auto path = juce::String::fromUTF8(media::pathUtf8(std::filesystem::path(wav).make_preferred()).c_str());
#if ADI_WITH_PD
    check(panels[0]->sample->drop(path), "a file dropped on the tile");
    refresh();
    check(panels[0]->sample->state() == SampleTile::State::Loaded && panels[0]->sample->clear.isVisible() &&
              document->ops().nextUndo() && document->ops().nextUndo()->label == "Assign device sample",
          "assigns Impulse in one step; the tile shows the sample and offers Clear");
    std::filesystem::rename(wav, dir / "cab-impulse-moved.wav");
    document->view().refresh(document->store());
    root.frame();
    check(panels[0]->sample->state() == SampleTile::State::Missing, "its file moved: the tile shows it missing");
    panels[0]->sample->clear.onClick();
    refresh();
    check(panels[0]->sample->state() == SampleTile::State::Empty && !panels[0]->sample->clear.isVisible(),
          "Clear empties it, one step");
#else
    check(!panels[0]->sample->drop(path) && !root.error().empty() &&
              panels[0]->sample->state() == SampleTile::State::Empty,
          "with Pd off a drop says the device does not run in this build, and the slot stays empty");
#endif
    // A file from the browser's Samples is the same drop.
    const juce::DragAndDropTarget::SourceDetails fromBrowser("adi-sample:" + path, nullptr, {10, 10});
    check(panels[0]->sample->isInterestedInDragSource(fromBrowser) &&
              !panels[0]->sample->isInterestedInDragSource(juce::DragAndDropTarget::SourceDetails("other", nullptr, {10, 10})),
          "the tile takes a sample dragged from the browser, and nothing else");
    root.say("");
    panels[0]->sample->itemDropped(fromBrowser);
#if ADI_WITH_PD
    // The file was moved away above: the drop reaches the engine, which says why it cannot read it.
    check(!root.error().empty(), "a sample dropped from the browser reaches the assignment, its refusal said");
#else
    check(!root.error().empty(), "a sample dropped from the browser reaches the assignment, and Pd off is said");
#endif
    // The tile as drawn, for the picture.
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * panels[0]->getWidth(), 2 * panels[0]->getHeight(), true,
                            juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            panels[0]->paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-color-cab-tile.png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(picture, out);
    }
    document->release();
}
// E2: Cut, Copy, Paste and Duplicate in the Arrangement (Live 12 manual 6.9, 41.5; N17).
void clipClipboard(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "clipboard.adi", true, {}, error);
    check(document != nullptr, "a project for the clipboard");
    if (!document)
        return;
    const auto bar = textproj::kWhole;
    std::vector<OpRequest> make(2);
    make[0].opType = "clip.create";
    make[0].payload = {{"id", 50}, {"track", 2}, {"kind", "midi"}, {"pos", 0}, {"length", bar}};
    make[1].opType = "clip.create";
    make[1].payload = {{"id", 51}, {"track", 3}, {"kind", "midi"}, {"pos", 0}, {"length", bar / 2}};
    check(document->ops().submit(std::move(make)).ok && document->synchronise(), "a bar on 1 MIDI, half a bar on 2 MIDI");
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "clipboard");
    root.afterEdit = [&] { document->synchronise(); };
    root.setSize(1440, 900);
    root.frame();
    commands.activate(root);
    commands.clipClipboard().held = std::monostate{};
    using Clips = std::vector<std::pair<std::int64_t, std::int64_t>>;
    const auto clipsOf = [&](std::int64_t track) {
        Clips out; // (pos, length)
        if (const auto *t = root.reader().findTrack(track))
            for (const auto &c : t->clips)
                out.emplace_back(c->posTicks, c->lengthTicks);
        std::sort(out.begin(), out.end());
        return out;
    };
    auto &sel = root.arrangement.geometry.selection;
    const auto clickClip = [&](std::int64_t track, std::int64_t clip, std::int64_t pos, std::int64_t length) {
        sel = {};
        sel.track = track;
        sel.clip = clip;
        sel.start = sel.insert = pos;
        sel.end = pos + length;
        sel.marked = true;
    };
    const auto marker = [&](std::int64_t track, std::int64_t at) {
        sel = {};
        sel.track = track;
        sel.start = sel.end = sel.insert = at;
        sel.marked = true;
    };
    const juce::KeyPress copyKey('c', juce::ModifierKeys::commandModifier, 0), cutKey('x', juce::ModifierKeys::commandModifier, 0),
        pasteKey('v', juce::ModifierKeys::commandModifier, 0), duplicateKey('d', juce::ModifierKeys::commandModifier, 0);
    // Nothing held: Paste says what to do.
    marker(3, 2 * bar);
    check(!root.command(AppCommands::Paste) && !root.error().empty(), "Paste with nothing copied says to copy first");
    // Copy a clip, Paste on another track at the marker: a new clip, one undo step, chosen.
    clickClip(2, 50, 0, bar);
    check(root.keyPressed(copyKey) && !commands.clipClipboard().empty(), "Ctrl/Cmd+C on 1 MIDI's clip");
    root.frame();
    check(clipsOf(2) == Clips{{0, bar}}, "Copy leaves the clip where it is");
    marker(3, 2 * bar);
    const auto before = document->ops().nextUndo();
    check(root.keyPressed(pasteKey), "Ctrl/Cmd+V on 2 MIDI at bar 3");
    root.frame();
    check(clipsOf(3) == Clips{{0, bar / 2}, {2 * bar, bar}}, "the copy lands on 2 MIDI at the marker");
    check(sel.clip != 0 && sel.clip != 50 && sel.track == 3 && sel.start == 2 * bar && sel.end == 3 * bar,
          "the pasted clip, with a new id, is chosen");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(clipsOf(3) == Clips{{0, bar / 2}} && document->ops().nextUndo() && before &&
              document->ops().nextUndo()->txnId == before->txnId,
          "one undo takes the paste back");
    // Cut a clip: gone, and its copy held; Paste brings it back elsewhere.
    clickClip(2, 50, 0, bar);
    check(root.keyPressed(cutKey), "Ctrl/Cmd+X on 1 MIDI's clip");
    root.frame();
    check(clipsOf(2).empty(), "Cut removes the clip");
    marker(3, 2 * bar);
    check(root.keyPressed(pasteKey), "Paste the cut clip on 2 MIDI");
    root.frame();
    check(clipsOf(3) == Clips{{0, bar / 2}, {2 * bar, bar}}, "the cut clip lands on 2 MIDI");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "undo the paste and the cut");
    root.frame();
    check(clipsOf(2) == Clips{{0, bar}} && clipsOf(3) == Clips{{0, bar / 2}}, "both as they were");
    // The window as drawn, for the PR's picture (ADI_UI_PARITY).
    const auto picture = [&](const char *name) {
        const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
        if (folder.isEmpty())
            return;
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile(name);
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(image, out);
    };
    // A range cut across two tracks and pasted at bar 3 comes back in its order.
    sel = {};
    sel.track = 2;
    sel.toTrack = 3;
    sel.start = 0;
    sel.end = bar;
    root.frame();
    picture("adi-clipboard-range.png");
    check(root.keyPressed(cutKey), "Ctrl/Cmd+X over bar 1 on both tracks");
    root.frame();
    check(clipsOf(2).empty() && clipsOf(3).empty(), "the range is cut from both tracks");
    marker(2, 2 * bar);
    check(root.keyPressed(pasteKey), "Paste the range at bar 3 from 1 MIDI");
    root.frame();
    check(clipsOf(2) == Clips{{2 * bar, bar}} && clipsOf(3) == Clips{{2 * bar, bar / 2}},
          "each track's material comes back on the track in its place, in order");
    check(sel.track == 2 && sel.toTrack == 3 && sel.start == 2 * bar && sel.end == 3 * bar && sel.clip == 0,
          "the pasted range is the time selection");
    picture("adi-clipboard-pasted.png");
    check(root.command(AppCommands::Undo), "undo the paste");
    root.frame();
    // From a track with too few below it, the range does not fit.
    // MIDI material does not go on audio tracks; from the last track, a two-track range does not fit.
    marker(4, 4 * bar);
    check(!root.command(AppCommands::Paste) && root.error().find("MIDI track") != std::string::npos &&
              clipsOf(4).empty() && clipsOf(5).empty(),
          "a MIDI range onto the audio tracks is refused, and says why");
    marker(5, 4 * bar);
    check(!root.command(AppCommands::Paste) && root.error().find("reaches 2 tracks") != std::string::npos,
          "a two-track range from the last track says it does not fit");
    // A track that cannot hold clips (a return) takes no paste.
    marker(6, 4 * bar);
    check(!root.command(AppCommands::Paste) && root.error().find("audio or MIDI track") != std::string::npos,
          "a return selected: Paste says to select an audio or MIDI track");
    check(root.command(AppCommands::Undo), "undo the cut");
    root.frame();
    check(clipsOf(2) == Clips{{0, bar}} && clipsOf(3) == Clips{{0, bar / 2}}, "both as they were again");
    // A clip's copy: MIDI does not go on an audio track.
    clickClip(2, 50, 0, bar);
    check(root.command(AppCommands::Copy) && std::holds_alternative<clip_edit::ClipCopy>(commands.clipClipboard().held),
          "copy 1 MIDI's clip: the clipboard holds a clip");
    marker(4, 0);
    check(!root.command(AppCommands::Paste) && root.error().find("The copied clip goes on a MIDI track") != std::string::npos &&
              clipsOf(4).empty(),
          "a MIDI clip onto an audio track is refused, and says why");
    // A Copy that works clears that refusal from the Status Bar.
    clickClip(2, 50, 0, bar);
    check(root.command(AppCommands::Copy) && root.error().empty(), "a Copy that works clears the Status Bar");
    // A time selection only on tracks that cannot hold clips has nothing to copy.
    sel = {};
    sel.track = 6;
    sel.start = 0;
    sel.end = bar;
    check(!root.canClipClipboard(AdiRootComponent::ClipCommand::Copy), "a time selection on a return offers no Copy");
    // Duplicate a time selection: the copy after it, and the selection on the copy.
    sel = {};
    sel.track = 2;
    sel.start = 0;
    sel.end = bar;
    check(root.keyPressed(duplicateKey), "Ctrl/Cmd+D over bar 1 on 1 MIDI");
    root.frame();
    check(clipsOf(2) == Clips{{0, bar}, {bar, bar}}, "the copy is placed at the selection's end");
    check(sel.start == bar && sel.end == 2 * bar, "the selection moves onto the copy");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // Duplicate a clip: right after it, chosen.
    clickClip(3, 51, 0, bar / 2);
    check(root.keyPressed(duplicateKey), "Ctrl/Cmd+D on 2 MIDI's clip");
    root.frame();
    check(clipsOf(3) == Clips{{0, bar / 2}, {bar / 2, bar / 2}} && sel.clip != 51 && sel.start == bar / 2,
          "the duplicate follows the clip and is chosen");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // An upward selection (anchored on 2 MIDI, reaching 1 MIDI) pastes from its top track, in order.
    sel = {};
    sel.track = 3;
    sel.toTrack = 2;
    sel.start = 0;
    sel.end = bar;
    check(root.command(AppCommands::Copy), "copy an upward selection over bar 1");
    marker(2, 2 * bar);
    sel.track = 3;
    sel.toTrack = 2;
    check(root.command(AppCommands::Paste), "paste with an upward selection");
    root.frame();
    check(clipsOf(2) == Clips{{0, bar}, {2 * bar, bar}} && clipsOf(3) == Clips{{0, bar / 2}, {2 * bar, bar / 2}},
          "1 MIDI's material back on 1 MIDI, 2 MIDI's on 2 MIDI: from the top track, not the anchor");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // A track the lanes do not show is left out of a range's copy, as Paste leaves it out.
    root.arrangement.geometry.hidden = {3};
    sel = {};
    sel.track = 2;
    sel.toTrack = 4;
    sel.start = 0;
    sel.end = bar;
    check(root.command(AppCommands::Copy) &&
              std::get<clip_edit::RangeCopy>(commands.clipClipboard().held).selection.tracks == std::vector<std::int64_t>{2, 4},
          "a hidden track between the selection's ends is not in the copy");
    root.arrangement.geometry.hidden.clear();
    // A copy from another project says so, not a kind it reads off this project's tracks.
    std::get<clip_edit::RangeCopy>(commands.clipClipboard().held).sourceFile = "/elsewhere/other.adi";
    marker(2, 2 * bar);
    check(!root.command(AppCommands::Paste) && root.error().find("another project") != std::string::npos,
          "a copy from another project is refused, and says so");
    // Cut the newest clip (the largest id) and paste it back: a new identity, not the copy's own.
    clickClip(3, 51, 0, bar / 2);
    check(root.keyPressed(cutKey), "Ctrl/Cmd+X on 2 MIDI's clip, the newest");
    root.frame();
    marker(3, bar);
    check(root.keyPressed(pasteKey), "paste the newest clip back");
    root.frame();
    check(clipsOf(3) == Clips{{bar, bar / 2}} && sel.clip > 51, "it lands, with an id past the cut one's");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "undo the paste and the cut");
    root.frame();
    // A clip's selection widened to another track is a time selection: Copy takes the range.
    clickClip(2, 50, 0, bar);
    sel.toTrack = 3;
    check(root.command(AppCommands::Copy) && std::holds_alternative<clip_edit::RangeCopy>(commands.clipClipboard().held),
          "a clip's span on two tracks copies as a range");
    // A Duplicate over empty time works, and clears an earlier refusal.
    marker(6, 0);
    root.command(AppCommands::Paste);
    check(!root.error().empty(), "a refusal on the Status Bar");
    sel = {};
    sel.track = 2;
    sel.start = 6 * bar;
    sel.end = 7 * bar;
    check(root.command(AppCommands::Duplicate) && root.error().empty(), "an empty Duplicate clears the Status Bar");
    // With nothing chosen, Copy says what to choose.
    marker(2, 0);
    check(!root.command(AppCommands::Copy) && !root.error().empty(), "Copy with no clip or time selection says what to select");
    commands.clipClipboard().held = std::monostate{};
    commands.detach(root);
    document->release();
}
// E5: ADI's import prompt (Live 12 manual 9.2; ADR-0132 d6): a drop asks while
// `editing.importOptions` is "ask", its choices all off by default and kept as the last ones.
void importPrompt(AppCommands &commands, settings::AppSettings &settings, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "import-prompt.adi", true, {}, error);
    check(document != nullptr, "a project for the import prompt");
    if (!document)
        return;
    const auto wav = dir / "Prompted.wav";
    {
        std::vector<float> samples(24000, .2f);
        audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "import-prompt");
    root.afterEdit = [&] { document->synchronise(); };
    root.importAudio = [&](const std::string &path, AudioImport &out, std::string &why) {
        if (document->prepareImport(path, 1'700'000'000'000'000, out))
            return true;
        why = document->error();
        return false;
    };
    // As the app's worker does, here on the spot.
    std::vector<ImportChoices> asked;
    root.importMaterial = [&](const std::string &path, std::int64_t track, std::int64_t pos, const ImportChoices &choices,
                              AdiRootComponent::MaterialDone done) {
        asked.push_back(choices);
        auto job = document->materialJob(path, track, pos, choices, 1'700'000'000'000'000);
        auto prepared = ProjectDocument::prepareMaterial(job);
        const auto clip = document->commitMaterial(prepared, job);
        document->synchronise();
        done(clip, clip != 0 ? std::string{} : document->error());
        return true;
    };
    root.setSize(1440, 900);
    root.frame();
    commands.activate(root);
    std::string why;
    // A fresh settings file: every choice off, and the prompt asks.
    {
        settings::AppSettings fresh(appdata::App::Daw, dir / "import-settings.json");
        check(importChoicesFrom(fresh) == ImportChoices{} && importAsks(fresh), "the prompt's defaults are all off, and it asks");
    }
    settings.set("editing.importOptions", std::string("ask"), {}, why);
    check(keepImportChoices(settings, ImportChoices{}, why), "the choices start off");
    const auto audioTrack = [&] {
        for (const auto &t : root.reader().tracks())
            if (t->kind == "audio")
                return t->id;
        return std::int64_t{0};
    }();
    const auto clipsOn = [&] {
        const auto *t = root.reader().findTrack(audioTrack);
        return t != nullptr ? t->clips.size() : std::size_t{0};
    };
    // A drop asks first, every choice off.
    check(root.dropAudio(wav.string(), audioTrack, 0, "Prompted") && root.importPrompt() != nullptr &&
              root.importPrompt()->isVisible() && clipsOn() == 0,
          "a drop opens the prompt and imports nothing yet");
    if (auto *prompt = root.importPrompt()) {
        check(prompt->choices() == ImportChoices{} && !prompt->mode.isEnabled(), "every choice off; the Warp mode waits for Warp");
        prompt->copy.setToggleState(true, juce::dontSendNotification);
        prompt->warp.setToggleState(true, juce::sendNotificationSync);
        check(prompt->mode.isEnabled(), "Warp on offers its mode");
        prompt->warp.setToggleState(false, juce::sendNotificationSync);
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
            juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics g(picture);
                g.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(g, true);
            }
            const auto file = juce::File(folder).getChildFile("adi-import-prompt.png");
            file.deleteFile();
            juce::FileOutputStream out(file);
            juce::PNGImageFormat().writeImageToStream(picture, out);
        }
        prompt->answer(true); // as the Import button does (a button click is asynchronous)
    }
    root.frame();
    check(root.importPrompt() == nullptr && asked.size() == 1 && asked[0].copyIn && !asked[0].warp && clipsOn() == 1,
          "Import: the file is imported with Copy on, as one clip");
    check(settings.get("files.copyImportedMedia") == "always", "the choice is kept as the last one");
    // The next drop asks again, with the last choices; Cancel imports nothing.
    check(root.dropAudio(wav.string(), audioTrack, 4 * textproj::kWhole, "Prompted") && root.importPrompt() != nullptr &&
              root.importPrompt()->choices().copyIn,
          "the prompt opens again with the last choices");
    if (auto *prompt = root.importPrompt())
        prompt->answer(false); // as Cancel does
    root.frame();
    check(root.importPrompt() == nullptr && asked.size() == 1 && clipsOn() == 1, "Cancel imports nothing");
    // Not asking: the last choices are used straight away.
    settings.set("editing.importOptions", std::string("never"), {}, why);
    const bool silent = root.dropAudio(wav.string(), audioTrack, 4 * textproj::kWhole, "Prompted");
    root.frame();
    check(silent && root.importPrompt() == nullptr && asked.size() == 2 && asked[1].copyIn && clipsOn() == 2,
          "with the prompt off, the last choices import at once");
    // All off and not asking: the plain import, as before E5.
    keepImportChoices(settings, ImportChoices{}, why);
    const bool plain = root.dropAudio(wav.string(), audioTrack, 8 * textproj::kWhole, "Prompted");
    root.frame();
    check(plain && asked.size() == 2 && clipsOn() == 3, "every choice off: the plain import, no material import");
    // The prompt's keys, and a second drop while it is open.
    settings.set("editing.importOptions", std::string("ask"), {}, why);
    check(root.dropAudio(wav.string(), audioTrack, 12 * textproj::kWhole, "Prompted") && root.importPrompt() != nullptr,
          "the prompt again");
    check(!root.dropAudio(wav.string(), audioTrack, 16 * textproj::kWhole, "Prompted") &&
              root.error() == "Answer the import prompt first.",
          "a second drop waits for the open prompt, and says so");
    if (auto *prompt = root.importPrompt()) {
        check(prompt->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)) && root.importPrompt() != nullptr && clipsOn() == 3,
              "another key stays with the prompt: Delete deletes nothing behind it");
        prompt->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    }
    root.frame();
    check(root.importPrompt() == nullptr && clipsOn() == 3, "Escape cancels");
    check(root.dropAudio(wav.string(), audioTrack, 12 * textproj::kWhole, "Prompted") && root.importPrompt() != nullptr,
          "and again");
    if (auto *prompt = root.importPrompt())
        prompt->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    root.frame();
    check(root.importPrompt() == nullptr && clipsOn() == 4, "Return imports");
    // A refused file leaves nothing: Warp on a file with no beat, dropped below the tracks.
    settings.set("editing.importOptions", std::string("never"), {}, why);
    ImportChoices warped;
    warped.warp = true;
    keepImportChoices(settings, warped, why);
    const auto trackCount = root.reader().tracks().size();
    const auto headBefore = document->ops().nextUndo();
    root.dropAudio(wav.string(), 0, 0, "Prompted");
    root.frame();
    const auto headAfter = document->ops().nextUndo();
    check(root.reader().tracks().size() == trackCount && headBefore && headAfter && headAfter->txnId == headBefore->txnId &&
              root.error().find("no steady beat") != std::string::npos && !root.importing(),
          "a refused file below the tracks leaves no track and no undo step, and says why");
    // Several files: the first refused stops the rest, and the Status Bar says so.
    root.dropAudioFiles({{wav.string(), "Prompted"}, {wav.string(), "Prompted"}}, audioTrack, 20 * textproj::kWhole, false);
    root.frame();
    check(clipsOn() == 4 && root.error().find("(0 files imported; 1 not tried.)") != std::string::npos,
          "a refusal part-way through says what landed and what was not tried");
    // The worker busy: the drop says so.
    auto material = root.importMaterial;
    root.importMaterial = [](const std::string &, std::int64_t, std::int64_t, const ImportChoices &,
                             AdiRootComponent::MaterialDone) { return false; };
    root.dropAudio(wav.string(), audioTrack, 20 * textproj::kWhole, "Prompted");
    check(!root.importing() && root.error().find("still being prepared") != std::string::npos && clipsOn() == 4,
          "a worker still busy: the drop says to drop it again");
    root.importMaterial = material;
    // Copy set to "ask": the prompt asks even with the import options off, and "ask" stays.
    keepImportChoices(settings, ImportChoices{}, why);
    settings.set("files.copyImportedMedia", std::string("ask"), {}, why);
    check(importAsks(settings) && root.dropAudio(wav.string(), audioTrack, 20 * textproj::kWhole, "Prompted") &&
              root.importPrompt() != nullptr,
          "Copy on \"ask\" asks on every drop");
    if (auto *prompt = root.importPrompt())
        prompt->answer(true);
    root.frame();
    check(settings.get("files.copyImportedMedia") == "ask" && clipsOn() == 5, "and the answer leaves \"ask\" as it was");
    keepImportChoices(settings, ImportChoices{}, why);
    settings.set("files.copyImportedMedia", std::string("never"), {}, why);
    settings.set("editing.importOptions", std::string("ask"), {}, why);
    settings.save(why);
    commands.detach(root);
    document->release();
}
// E3: the …Time commands and Insert Silence (Live 12 manual 6.11, 41.5; N17): across every track,
// locators and the loop moving with the time.
void timeCommands(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "time.adi", true, {}, error);
    check(document != nullptr, "a project for the …Time commands");
    if (!document)
        return;
    const auto bar = textproj::kWhole;
    std::vector<OpRequest> make(3);
    make[0].opType = "clip.create";
    make[0].payload = {{"id", 50}, {"track", 2}, {"kind", "midi"}, {"pos", 2 * bar}, {"length", bar}};
    make[1].opType = "clip.create";
    make[1].payload = {{"id", 51}, {"track", 3}, {"kind", "midi"}, {"pos", 4 * bar}, {"length", bar}};
    make[2].opType = "marker.create";
    make[2].payload = {{"id", 70}, {"pos", 4 * bar}, {"kind", "position"}, {"name", "1"}};
    check(document->ops().submit(std::move(make)).ok && document->synchronise(),
          "a clip at bar 3 on 1 MIDI, one at bar 5 on 2 MIDI, a locator at bar 5");
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "time");
    root.afterEdit = [&] { document->synchronise(); };
    root.setSize(1440, 900);
    root.frame();
    commands.activate(root);
    commands.clipClipboard().held = std::monostate{};
    using Clips = std::vector<std::pair<std::int64_t, std::int64_t>>;
    const auto clipsOf = [&](std::int64_t track) {
        Clips out;
        if (const auto *t = root.reader().findTrack(track))
            for (const auto &c : t->clips)
                out.emplace_back(c->posTicks, c->lengthTicks);
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto locator = [&] {
        const auto all = locatorsOf(*document->view().current());
        return all.empty() ? std::int64_t{-1} : all.front().pos;
    };
    auto &sel = root.arrangement.geometry.selection;
    const auto select = [&](std::int64_t a, std::int64_t b) {
        sel = {};
        sel.track = 2;
        sel.start = sel.insert = a;
        sel.end = b;
        sel.marked = true;
    };
    // The loop over bars 5 to 6.
    select(4 * bar, 5 * bar);
    check(root.command(AppCommands::LoopSelection), "loop bars 5 to 6");
    root.frame();
    const auto loopStart = [&] { return root.reader().transport().loopStartTicks; };
    check(loopStart() == 4 * bar, "the loop starts at bar 5");
    // Insert Silence over bars 2 to 4: every later clip, the locator and the loop two bars on, one step.
    select(bar, 3 * bar);
    const juce::KeyPress insertKey('i', juce::ModifierKeys::commandModifier, 0);
    check(root.keyPressed(insertKey), "Ctrl/Cmd+I over bars 2 to 4");
    root.frame();
    check(clipsOf(2) == Clips{{4 * bar, bar}} && clipsOf(3) == Clips{{6 * bar, bar}},
          "both tracks' later clips move two bars on");
    check(locator() == 6 * bar && loopStart() == 6 * bar, "the locator and the loop move two bars on");
    check(sel.start == bar && sel.end == 3 * bar, "the silence stays selected");
    const auto step = document->ops().nextUndo();
    // Delete Time pulls them back.
    check(root.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey, juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)),
          "Ctrl/Cmd+Shift+Delete over the silence");
    root.frame();
    check(clipsOf(2) == Clips{{2 * bar, bar}} && clipsOf(3) == Clips{{4 * bar, bar}} && locator() == 4 * bar &&
              loopStart() == 4 * bar,
          "Delete Time pulls every clip, the locator and the loop back");
    check(root.command(AppCommands::Undo), "undo Delete Time");
    root.frame();
    check(step && document->ops().nextUndo() && document->ops().nextUndo()->txnId == step->txnId,
          "Insert Silence was one undo step");
    check(root.command(AppCommands::Undo), "undo Insert Silence");
    root.frame();
    check(clipsOf(2) == Clips{{2 * bar, bar}} && locator() == 4 * bar, "as before");
    // The window as drawn, for the PR's picture (ADI_UI_PARITY).
    const auto picture = [&](const char *name) {
        const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
        if (folder.isEmpty())
            return;
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile(name);
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(image, out);
    };
    // Cut Time over bars 3 to 4, then Paste Time at the same place, restores the arrangement.
    select(2 * bar, 3 * bar);
    root.frame();
    picture("adi-cut-time-before.png");
    check(root.keyPressed(juce::KeyPress('x', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)),
          "Ctrl/Cmd+Shift+X over bar 3");
    root.frame();
    picture("adi-cut-time-after.png");
    check(clipsOf(2).empty() && clipsOf(3) == Clips{{3 * bar, bar}} && locator() == 3 * bar,
          "the bar is cut out of every track; later material, the locator and the loop close up");
    check(std::holds_alternative<clip_edit::TimeCopy>(commands.clipClipboard().held), "the clipboard holds the time");
    select(2 * bar, 2 * bar);
    check(root.keyPressed(juce::KeyPress('v', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)),
          "Ctrl/Cmd+Shift+V at bar 3");
    root.frame();
    check(clipsOf(2) == Clips{{2 * bar, bar}} && clipsOf(3) == Clips{{4 * bar, bar}} && locator() == 4 * bar &&
              loopStart() == 4 * bar,
          "Paste Time at the same place restores the arrangement");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "undo the paste and the cut");
    root.frame();
    // Copy Time and Duplicate Time.
    commands.clipClipboard().held = std::monostate{};
    select(2 * bar, 3 * bar);
    check(root.keyPressed(juce::KeyPress('c', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) &&
              std::holds_alternative<clip_edit::TimeCopy>(commands.clipClipboard().held) && root.error().empty() &&
              clipsOf(2) == Clips{{2 * bar, bar}},
          "Copy Time holds the time and leaves the arrangement as it was");
    // A plain Paste of the Time copy: its material back on its own tracks at the marker, no time inserted.
    select(6 * bar, 6 * bar);
    check(root.keyPressed(juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0)), "Ctrl/Cmd+V of the Time copy at bar 7");
    root.frame();
    check(clipsOf(2) == Clips{{2 * bar, bar}, {6 * bar, bar}} && clipsOf(3) == Clips{{4 * bar, bar}} && locator() == 4 * bar &&
              root.error().empty(),
          "the bar's clip lands on 1 MIDI at bar 7; nothing later moves");
    check(root.command(AppCommands::Undo), "undo the paste");
    root.frame();
    // A Time copy taken at another sample rate says to copy again.
    std::get<clip_edit::TimeCopy>(commands.clipClipboard().held).referenceRate += 1.0;
    select(2 * bar, 2 * bar);
    check(!root.command(AppCommands::PasteTime) && root.error().find("sample rate changed") != std::string::npos,
          "Paste Time after a rate change says to copy again");
    std::get<clip_edit::TimeCopy>(commands.clipClipboard().held).referenceRate -= 1.0;
    select(2 * bar, 3 * bar);
    check(root.keyPressed(juce::KeyPress('d', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)),
          "Ctrl/Cmd+Shift+D over bar 3");
    root.frame();
    check(clipsOf(2) == Clips{{2 * bar, bar}, {3 * bar, bar}} && clipsOf(3) == Clips{{5 * bar, bar}} && locator() == 5 * bar,
          "Duplicate Time inserts a copy of the bar after it; later material moves on");
    check(sel.start == 3 * bar && sel.end == 4 * bar, "the selection moves onto the copy");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // Nothing selected: the …Time commands say what to do.
    select(bar, bar);
    check(!root.command(AppCommands::DeleteTime) && !root.error().empty(), "Delete Time with no time selected says so");
    commands.clipClipboard().held = std::monostate{};
    commands.detach(root);
    document->release();
}
void audioDrop(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "drop.adi", err);
    check(store != nullptr, "the drop's project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Drop')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    bool madeAll = true;
    for (const auto &[id, kind, name] : {std::tuple{1, "audio", "Drums"}, std::tuple{2, "midi", "Keys"}}) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", name}};
        r.label = "Create track";
        madeAll = submitter.submit(std::move(r)).ok && madeAll;
    }
    check(madeAll, "an audio track and a MIDI track");
    // One second at 48 kHz: two beats at the default 120 BPM.
    const auto wav = dir / "Kick 808.wav";
    {
        std::vector<float> samples(48000, .25f);
        audio::WavWriter writer(wav, 48000, 1, audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "drop");
    // What the document does (ProjectDocument::prepareImport), on this scene's store.
    root.importAudio = [&store](const std::string &path, AudioImport &out, std::string &why) {
        OpRequest captured;
        const auto id = std::max(out.firstFreeId,
                                 store->db().execAndGet("SELECT COALESCE(MAX(id), 0) + 1 FROM media_files").getInt64());
        const auto r = media::prepareImport(*store, media::pathFromUtf8(path), id, 1, captured);
        if (!r.ok || !r.audio) {
            why = r.ok ? "not audio" : r.error;
            return false;
        }
        out.mediaId = r.mediaId;
        out.sampleRate = r.audio->sampleRate;
        out.frames = r.audio->frames;
        out.media = std::move(captured);
        return true;
    };
    root.setSize(1200, 720);
    root.frame();
    root.frame();
    auto &geometry = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    const auto path = juce::String(wav.string());
    // A little past bar 2: the clip starts on its line.
    const auto bar2 = textproj::kWhole;
    const int x = static_cast<int>(geometry.x(bar2) + 3);
    canvas.filesDropped(juce::StringArray{path}, x, laneTop(root, 1) + 10);
    root.frame();
    const auto *drums = root.reader().findTrack(1);
    check(drums != nullptr && drums->clips.size() == 1 && drums->clips[0]->name == "Kick 808" &&
              drums->clips[0]->posTicks == bar2 && std::abs(drums->clips[0]->lengthTicks - 2 * textproj::kPPQ) <= 1,
          "dropped on Drums near bar 2: a clip named after the file, on bar 2, two beats long");
    check(geometry.selection.track == 1 && geometry.selection.clip == drums->clips[0]->id &&
              submitter.nextUndo() && submitter.nextUndo()->label == "Insert Audio Clip" &&
              submitter.nextUndo()->opCount == 3,
          "the clip is selected; one undo step");
    geometry.selection.clip = 0;
    check(root.command(AppCommands::SelectAll) && geometry.selection.clip == drums->clips[0]->id &&
              geometry.selection.toTrack == 0 && geometry.selection.start == bar2,
          "Select All with one clip selects that clip");
    canvas.filesDropped(juce::StringArray{path}, x, laneTop(root, 2) + 10);
    root.frame();
    check(root.reader().findTrack(2)->clips.empty() && root.error().find("audio track") != std::string::npos,
          "on the MIDI track: refused, and the Status Bar says where it goes");
    // Below the tracks, the drop area: a new audio track holding the clip; the pool row is reused.
    const int below = laneTop(root, 2) + geometry.heightFor(2) + 20;
    check(geometry.trackAt(root.reader(), below) == nullptr, "below the tracks is the drop area");
    canvas.filesDropped(juce::StringArray{path}, x, below);
    root.frame();
    const auto *added = root.reader().findTrack(3);
    check(added != nullptr && added->kind == "audio" && added->clips.size() == 1 && geometry.selection.track == 3 &&
              store->db().execAndGet("SELECT count(*) FROM media_files").getInt() == 1,
          "the drop area: a new audio track with the clip, selected; the file pooled once");
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true,
                          juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-audio-drop.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
    }
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(root.reader().findTrack(3) == nullptr && root.reader().findTrack(1)->clips.size() == 1,
          "takes the new track away; the first clip stays");
    // A file that is not audio is refused with the reason.
    const auto text = dir / "notes.txt";
    {
        std::ofstream(text) << "not audio";
    }
    canvas.filesDropped(juce::StringArray{juce::String(text.string())}, x, laneTop(root, 1) + 10);
    root.frame();
    check(root.reader().findTrack(1)->clips.size() == 1 && !root.error().empty(), "a text file is refused, with the reason");
    // Several files at once, as Live places them: one after another on the track; with Ctrl/Cmd, each
    // on a track of its own. One undo step for them all.
    const auto snare = dir / "Snare 909.wav";
    {
        // Half a second: one beat at 120 BPM.
        std::vector<float> samples(24000, .25f);
        audio::WavWriter writer(snare, 48000, 1, audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    const auto snarePath = juce::String(snare.string());
    check(canvas.isInterestedInFileDrag(juce::StringArray{path, snarePath}), "the lanes take several files");
    const auto bar3 = 2 * textproj::kWhole;
    const int x3 = static_cast<int>(geometry.x(bar3) + 3);
    auto clipsOn = [&root](std::int64_t track) {
        std::vector<std::pair<std::string, std::int64_t>> out;
        if (const auto *t = root.reader().findTrack(track))
            for (const auto &c : t->clips)
                out.emplace_back(c->name, c->posTicks);
        return out;
    };
    using Clips = std::vector<std::pair<std::string, std::int64_t>>;
    // The window at twice its size, for the PR's picture (ADI_UI_PARITY).
    auto shot = [&root](const char *name) {
        const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
        if (pictures.isEmpty())
            return;
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(pictures).getChildFile(name);
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
    };
    canvas.filesDropped(juce::StringArray{path, snarePath}, x3, laneTop(root, 1) + 10);
    root.frame();
    shot("adi-multi-drop.png");
    check(clipsOn(1) == Clips{{"Kick 808", bar2}, {"Kick 808", bar3}, {"Snare 909", bar3 + 2 * textproj::kPPQ}} &&
              submitter.nextUndo() && submitter.nextUndo()->label == "Insert Audio Clip" &&
              store->db().execAndGet("SELECT count(*) FROM media_files").getInt() == 2 &&
              geometry.selection.clip == root.reader().findTrack(1)->clips[1]->id && geometry.selection.start == bar3 &&
              geometry.selection.end == bar3 + 2 * textproj::kPPQ,
          "Kick and Snare dropped on Drums at bar 3: one after the other, the Snare pooled, the first selected with its "
          "time, one step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(clipsOn(1).size() == 1 && store->db().execAndGet("SELECT count(*) FROM media_files").getInt() == 1,
          "takes both away, and the Snare's pool row");
    check(root.dropAudioFiles({{path.toStdString(), "Kick 808"}, {snarePath.toStdString(), "Snare 909"}}, 1, bar3, true),
          "with Ctrl/Cmd held");
    root.frame();
    shot("adi-multi-drop-separate.png");
    const auto *own = root.reader().findTrack(3);
    check(clipsOn(1) == Clips{{"Kick 808", bar2}, {"Kick 808", bar3}} && own != nullptr && own->kind == "audio" &&
              clipsOn(3) == Clips{{"Snare 909", bar3}},
          "Kick on Drums, the Snare on a new audio track of its own, both at bar 3");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // The same file twice below the tracks: one new track, the two one after the other, pooled once.
    canvas.filesDropped(juce::StringArray{snarePath, snarePath}, x3, below);
    root.frame();
    check(clipsOn(3) == Clips{{"Snare 909", bar3}, {"Snare 909", bar3 + textproj::kPPQ}} &&
              store->db().execAndGet("SELECT count(*) FROM media_files").getInt() == 2,
          "the Snare dropped twice below the tracks: a new track, one after the other, pooled once");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();

    // The browser's Samples and Places (D1; Live 12 manual 4.1, 4.6, 4.7; N15's index part): a folder
    // registered and scanned into the library index (#102) lists its audio files, and not its others;
    // right goes from the sidebar into the list, left back; the search narrows it; a row dragged onto a
    // lane is the audio drop.
    {
        const auto library = dir / "Library";
        std::filesystem::create_directories(library / "Drums");
        for (const auto &[name, value] : {std::pair{"Drums/Clap.wav", .2f}, std::pair{"Bass Hit.wav", .4f}}) {
            std::vector<float> samples(4800, value);
            audio::WavWriter writer(library / name, 48000, 1, audio::WavFormat::Pcm16);
            writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
        }
        std::ofstream(library / "notes.txt") << "not audio";
        std::string why;
        auto index = library::Index::open(dir / "library.sqlite", why, false);
        check(index != nullptr, "a library index");
        if (!index)
            return;
        // An added folder whose name is a sidebar label's: told apart by its volume, not its name.
        const auto addedFolder = dir / "Samples";
        std::filesystem::create_directories(addedFolder);
        {
            std::vector<float> samples(4800, .3f);
            audio::WavWriter writer(addedFolder / "Rim.wav", 48000, 1, audio::WavFormat::Pcm16);
            writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
        }
        library::MarkerVolumeProvider volumes;
        const std::vector<LibraryFolder> folders{{"User Library", library, true}, {"Samples", addedFolder, false},
                                                 {"Drums", library / "Drums", false}};
        auto listing = scanLibrary(*index, folders, volumes);
        check(listing.error.empty() && listing.places.size() == 3 && listing.samples.size() == 3 &&
                  listing.samples[0].name == "Bass Hit.wav" && listing.samples[1].name == "Clap.wav" &&
                  listing.samples[2].name == "Rim.wav" && listing.places[0].userLibrary,
              "the scan lists the three audio files once each by name (Clap.wav under two folders), and no text file");
        std::atomic<bool> stopNow{true};
        check(scanLibrary(*index, folders, volumes, &stopNow).places.empty(), "a stop before the first folder scans none");
        auto &browser = root.browser;
        browser.setListing(listing);
        int samples = -1, user = -1;
        for (std::size_t i = 0; i < browser.sidebar().size(); ++i) {
            if (browser.sidebar()[i].samples)
                samples = static_cast<int>(i);
            if (browser.sidebar()[i].name == "User Library")
                user = static_cast<int>(i);
        }
        int addedSamples = -1, addedDrums = -1, drumsLabel = -1;
        for (std::size_t i = 0; i < browser.sidebar().size(); ++i) {
            const auto &e = browser.sidebar()[i];
            if (e.name == "Samples" && !e.samples)
                addedSamples = static_cast<int>(i);
            if (e.name == "Drums" && !e.place.empty())
                addedDrums = static_cast<int>(i);
            if (e.name == "Drums" && e.place.empty() && !e.heading)
                drumsLabel = static_cast<int>(i);
        }
        root.say({});
        browser.choose(samples);
        check(browser.shown().size() == 3 && browser.shown()[0]->name == "Bass Hit.wav" && root.error().empty() &&
                  browser.list.getNumRowsOnScreen() > 0,
              "Samples lists all three, and says nothing");
        browser.choose(user);
        check(browser.shown().size() == 2, "the User Library place lists its two, its Drums folder's included");
        browser.choose(addedSamples);
        check(addedSamples > user && browser.shown().size() == 1 && browser.shown()[0]->name == "Rim.wav",
              "an addedFolder folder named Samples is a Place of its own, listing its one file");
        browser.choose(addedDrums);
        check(browser.shown().size() == 1 && browser.shown()[0]->name == "Clap.wav",
              "an addedFolder Drums folder inside the User Library lists the file under it");
        browser.choose(drumsLabel);
        check(browser.shown().empty() && root.error().find("devices lane") != std::string::npos,
              "the Drums category stays the catalogue's, whatever a folder is called, and says what it waits for");
        browser.setListing(listing, true);
        check(browser.selected() == drumsLabel, "a new listing keeps the label chosen");
        browser.setListing(listing, false);
        browser.choose(samples);
        check(root.error().find("still being indexed") != std::string::npos,
              "while the first scan runs, the Status Bar says so");
        browser.setListing(listing, true);
        browser.choose(user);
        check(browser.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && browser.list.getSelectedRow() == 0,
              "right goes into the list, its first row selected");
        check(browser.list.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) && browser.list.getSelectedRow() == 1,
              "down moves to the next row");
        browser.choose(user);
        // JUCE posts a typed change; the test runs the editor's callback as the message loop would.
        browser.search.setText("clap", false);
        browser.search.onTextChange();
        check(browser.shown().size() == 1 && browser.shown()[0]->name == "Clap.wav", "the search narrows the list");
        browser.list.deselectAllRows();
        check(browser.searchKeys().keyPressed(juce::KeyPress(juce::KeyPress::downKey), &browser.search) &&
                  browser.list.getSelectedRow() == 0,
              "the down arrow in the search goes to its results, the first selected");
        browser.search.setText({}, false);
        browser.search.onTextChange();
        // Dragged onto Drums at bar 5: the audio drop, one clip, one step.
        const auto drag = BrowserPanel::dragDescription({browser.shown()[1]});
        const auto bar5 = 4 * textproj::kWhole;
        const juce::Point<int> at{static_cast<int>(geometry.x(bar5)) + 3, laneTop(root, 1) + 10};
        const auto carried = BrowserPanel::droppedFiles(drag);
        check(carried.size() == 1 && juce::File::createFileWithoutCheckingPath(carried[0]).getFileName() == "Clap.wav",
              "the drag carries the sample's native path, its file name Clap.wav on every system");
        juce::DragAndDropTarget::SourceDetails details(drag, nullptr, at);
        check(canvas.isInterestedInDragSource(details) &&
                  !canvas.isInterestedInDragSource(juce::DragAndDropTarget::SourceDetails("something else", nullptr, at)),
              "the lanes take a browser sample, and nothing else dragged");
        const auto before = clipsOn(1).size();
        canvas.itemDropped(details);
        root.frame();
        const auto after = clipsOn(1);
        check(after.size() == before + 1 && after.back().first == "Clap" && after.back().second == bar5 &&
                  submitter.nextUndo() && submitter.nextUndo()->label == "Insert Audio Clip",
              "Clap.wav dropped from the browser on Drums at bar 5: one clip there, one step, as a file from the desktop");
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
            browser.choose(samples);
            browser.list.selectRow(1);
            root.frame();
            juce::Image picture(juce::Image::ARGB, 2 * browser.getWidth(), 2 * browser.getHeight(), true,
                                juce::SoftwareImageType{});
            {
                juce::Graphics g(picture);
                g.addTransform(juce::AffineTransform::scale(2.f));
                browser.paintEntireComponent(g, true);
            }
            const auto file = juce::File(folder).getChildFile("adi-browser-samples.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
        }
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        browser.setListing({});
    }
}
// The Clip View (STEP-7-PLAN 7.7, ADR-0240): a double-click on a clip opens it above the Device View;
// it shows the clip's properties and its notes on the song's time axis, follows the project through
// edits, undo and deletion, and its activator is one undo step. Its picture masks every text.
void clipViewReference(ClipViewPanel &panel, const char *name, bool write) {
    juce::Image image(juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true, juce::SoftwareImageType{});
    {
        juce::Graphics g(image);
        panel.paintEntireComponent(g, true);
    }
    {
        // Each platform's fonts differ: every place the panel draws text is blacked out, with two
        // pixels around it, since a glyph's edge can overhang the box it is drawn in.
        juce::Graphics g(image);
        g.setColour(juce::Colours::black);
        for (const auto &r : panel.textAreas())
            g.fillRect(r.expanded(2));
    }
    const auto file = juce::File(ADI_UI_REFERENCES).getChildFile(juce::String(name) + ".png");
    if (write) {
        file.deleteFile();
        auto stream = file.createOutputStream();
        check(stream != nullptr, "write the Clip View's reference");
        if (stream)
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
        return;
    }
    const auto expected = juce::ImageFileFormat::loadFrom(file);
    check(expected.isValid(), "the Clip View's reference exists");
    if (!expected.isValid())
        return;
    int differences = expected.getWidth() == image.getWidth() && expected.getHeight() == image.getHeight() ? 0 : -1;
    if (differences == 0)
        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
                if (image.getPixelAt(x, y) != expected.getPixelAt(x, y))
                    ++differences;
    check(differences == 0, name);
    if (differences != 0) {
        std::printf("%s: %d differing pixels\n", name, differences);
        const auto diagnostic = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(name) + "-masked-actual.png");
        diagnostic.deleteFile();
        if (auto stream = diagnostic.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
    }
}
void clipView(AppCommands &commands, const std::filesystem::path &dir, bool write) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "clip-view.adi", err);
    check(store != nullptr, "the Clip View's project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clip View')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole, beat = textproj::kPPQ;
    auto op = [](const char *type, Payload payload, const char *label) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(payload);
        r.label = label;
        return r;
    };
    bool made = submitter.submit(op("track.create", {{"id", 1}, {"kind", "midi"}, {"name", "Chords"}, {"index", 0}}, "Create track")).ok;
    made = submitter.submit(op("track.create", {{"id", 2}, {"kind", "audio"}, {"name", "Rise"}, {"index", 1}}, "Create track")).ok && made;
    // A two-bar MIDI clip in bar 2: C3 for a beat, E3 a beat later for two, a quiet G3 in its second bar.
    made = submitter.submit(op("clip.create", {{"id", 11}, {"track", 1}, {"kind", "midi"}, {"name", "Chords"}, {"pos", bar}, {"length", 2 * bar}}, "Create clip")).ok && made;
    made = submitter.submit(std::vector<OpRequest>{
               op("note.insert", {{"clip", 11}, {"note", 1}, {"start", 0}, {"dur", beat}, {"key", 60}, {"vel", 100}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 2}, {"start", beat}, {"dur", 2 * beat}, {"key", 64}, {"vel", 127}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 3}, {"start", bar}, {"dur", beat}, {"key", 67}, {"vel", 30}}, "Add Note")}).ok && made;
    made = submitter.submit(op("clip.create", {{"id", 12}, {"track", 1}, {"kind", "midi"}, {"name", "Empty"}, {"pos", 4 * bar}, {"length", bar}}, "Create clip")).ok && made;
    check(made, "a MIDI track with a two-bar clip of three notes and an empty clip, and an audio track");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "clip-view");
    commands.activate(root);
    root.setSize(1400, 900);
    for (std::int64_t id = 1; id <= 2; ++id)
        root.arrangement.geometry.resizeTrack(id, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    auto &panel = root.clipView;
    const auto x = [&geo](std::int64_t tick) { return static_cast<float>(geo.x(tick)); };
    const auto lane = [&root](std::int64_t id) { return static_cast<float>(laneTop(root, id) + 30); };

    // A MIDI clip shows its notes in the Arrangement (DF-W21): where a note sounds, the clip's body
    // holds its bar; where none does, and in an empty clip, the body is plain.
    {
        juce::Image image(juce::Image::ARGB, canvas.getWidth(), canvas.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            canvas.paintEntireComponent(g, true);
        }
        const int top = laneTop(root, 1) + 14, bottom = laneTop(root, 1) + 44;
        const auto marks = [&](std::int64_t tick) {
            const int column = static_cast<int>(geo.x(tick));
            int count = 0;
            for (int y = top + 1; y < bottom; ++y)
                count += image.getPixelAt(column, y) != image.getPixelAt(column, top) ? 1 : 0;
            return count;
        };
        check(marks(bar + beat / 2) > 0 && marks(bar + beat + beat / 2) > 0 && marks(2 * bar + beat / 2) > 0,
              "a MIDI clip draws a bar where each of its three notes sounds");
        check(marks(bar + 3 * beat + beat / 2) == 0, "and nothing where no note sounds");
        check(marks(4 * bar + beat / 2) == 0, "an empty MIDI clip's body is plain");
    }

    // Hidden until asked for; the Device View is where it was.
    const auto deviceAlone = root.deviceArea();
    check(!root.state().clipView && root.clipArea().isEmpty() && !panel.isVisible() && !root.clipResizer.isVisible(),
          "the Clip View is hidden until a clip is opened");

    // A double-click on the clip selects it and opens the view above the Device View.
    canvas.mouseDoubleClick(pressAt(canvas, {x(bar) + 20, lane(1)}));
    root.frame();
    const auto clipArea = root.clipArea(), deviceArea = root.deviceArea();
    check(root.state().clipView && panel.isVisible() && geo.selection.clip == 11 && panel.clip() == 11,
          "a double-click on a clip selects it and opens the Clip View on it");
    check(!clipArea.isEmpty() && clipArea.getBottom() + look::gap == deviceArea.getY() &&
              clipArea.getX() == deviceArea.getX() && clipArea.getWidth() == deviceArea.getWidth() &&
              deviceArea.getBottom() == deviceAlone.getBottom() && clipArea.getHeight() >= AdiRootComponent::kClipViewFloor,
          "it stacks above the Device View, a gap between them, the Device View still at the window's foot");
    check(root.arrangement.getBottom() <= clipArea.getY(), "and the arrangement ends above it");
    check(root.status.clipToggle.getToggleState(), "the Status Bar's Clip View Selector is lit");
    juce::ApplicationCommandInfo info(AppCommands::ToggleClipView);
    commands.getCommandInfo(AppCommands::ToggleClipView, info);
    check((info.flags & juce::ApplicationCommandInfo::isTicked) != 0 && info.defaultKeypresses.size() == 1 &&
              info.defaultKeypresses[0] ==
                  juce::KeyPress('3', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0),
          "the View menu's Clip View is ticked, on Ctrl/Cmd+Alt+3");

    // What it reads: the clip's kind and name, its markers as Live's fields read them, its notes.
    const auto *d = panel.details();
    check(d != nullptr && panel.midi() && !panel.audio() && d->name == "Chords" && d->notes.size() == 3 &&
              panel.notes.isVisible() && !panel.sample.isVisible() && panel.problem().empty(),
          "a MIDI clip: its name, its three notes, and the Note Editor");
    if (d == nullptr)
        return;
    const auto r = panel.readings();
    check(r.start == "1. 1. 1" && r.end == "3. 1. 1" && r.position == "1. 1. 1" && r.length == "2. 0. 0" &&
              r.signature == "4 / 4" && !r.loop,
          "Start 1.1.1, End 3.1.1, Length 2.0.0, 4/4, the loop off");

    // The notes on the song's time axis and the note ruler: the editor opened on the whole clip and
    // on its notes.
    auto &editor = panel.notes;
    const auto area = editor.notesArea();
    const auto c3 = editor.boxOf(d->notes[0]), e3 = editor.boxOf(d->notes[1]), g3 = editor.boxOf(d->notes[2]);
    check(c3 && e3 && g3, "every note has a box");
    if (!c3 || !e3 || !g3)
        return;
    check(std::abs(c3->x - (area.getX() + editor.time.x(bar))) < 0.01 &&
              std::abs(e3->x - (area.getX() + editor.time.x(bar + beat))) < 0.01 &&
              std::abs(g3->x - (area.getX() + editor.time.x(2 * bar))) < 0.01,
          "a note starts where the clip's position puts it in the song");
    check(std::abs(e3->width - 2 * c3->width) < 0.01 && c3->width > 4, "a two-beat note is twice as wide as a one-beat note");
    check(g3->y < e3->y && e3->y < c3->y && c3->y - e3->y == 4 * editor.keys.keyHeight &&
              e3->y - g3->y == 3 * editor.keys.keyHeight,
          "higher keys are higher up, a key track for each semitone");
    check(c3->x >= area.getX() && g3->x + g3->width <= area.getRight() && g3->y >= area.getY() &&
              c3->y + c3->height <= area.getBottom(),
          "opened, the whole clip and all its notes are in view");
    check(editor.time.x(bar) == 0. && editor.time.x(3 * bar) <= area.getWidth(), "the clip starts at the editor's left edge");
    check(!editor.velocityArea().isEmpty() && editor.velocityArea().getY() == area.getBottom(),
          "the Velocity lane is under the key tracks");

    clipViewReference(panel, "clip-view-midi", write);
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-clip-view.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }

    // The wheel scrolls the key tracks; with Alt it zooms them; with Ctrl/Cmd it zooms time.
    juce::MouseWheelDetails wheel;
    wheel.deltaX = 0.f;
    wheel.deltaY = -0.1f;
    wheel.isReversed = wheel.isSmooth = wheel.isInertial = false;
    const juce::Point<float> inNotes(static_cast<float>(area.getCentreX()), static_cast<float>(area.getCentreY()));
    const int scrolled = editor.keys.scrollY;
    editor.mouseWheelMove(pressAt(editor, inNotes), wheel);
    check(editor.keys.scrollY == scrolled + 18, "the wheel scrolls the key tracks");
    editor.mouseWheelMove(pressAt(editor, inNotes, juce::ModifierKeys(juce::ModifierKeys::altModifier)), wheel);
    check(editor.keys.keyHeight == KeyTracks::kDefaultHeight - 2, "Alt and the wheel zoom the key tracks");
    const double scale = editor.time.scale;
    wheel.deltaY = 0.5f;
    editor.mouseWheelMove(pressAt(editor, inNotes, juce::ModifierKeys(juce::ModifierKeys::commandModifier)), wheel);
    check(editor.time.scale == 2 * scale, "Ctrl/Cmd and the wheel zoom time");

    // The Clip Activator: one click, one undo step, and Undo brings the clip back on.
    const auto dot = panel.activatorArea().getCentre().toFloat();
    panel.mouseDown(pressAt(panel, dot));
    root.frame();
    check(panel.details() != nullptr && panel.details()->muted && submitter.nextUndo()->label == "Deactivate Clip" &&
              submitter.nextUndo()->opCount == 1,
          "the activator deactivates the clip in one undo step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(panel.details() != nullptr && !panel.details()->muted, "and Undo activates it again");

    // It follows the project: a note added by any edit is read, and gone again after Undo.
    check(submitter.submit(op("note.insert", {{"clip", 11}, {"note", 4}, {"start", bar + beat}, {"dur", beat}, {"key", 72}}, "Add Note")).ok,
          "a fourth note is added");
    root.frame();
    check(panel.details() != nullptr && panel.details()->notes.size() == 4, "the view reads it on the next frame");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(panel.details() != nullptr && panel.details()->notes.size() == 3, "and after Undo it shows three again");

    // With nothing selected the shown clip stays; another clip replaces it; a deleted clip leaves none.
    canvas.mouseDown(pressAt(canvas, {x(8 * bar), lane(2)}));
    canvas.mouseUp(pressAt(canvas, {x(8 * bar), lane(2)}));
    root.frame();
    check(geo.selection.clip == 0 && panel.clip() == 11, "a click on an empty lane selects no clip, and the view keeps its clip");
    canvas.mouseDoubleClick(pressAt(canvas, {x(4 * bar) + 20, lane(1)}));
    root.frame();
    check(panel.clip() == 12 && panel.details() != nullptr && panel.details()->notes.empty() && panel.readings().length == "1. 0. 0",
          "a double-click on the empty clip shows it: no notes, one bar");
    const auto c3Row = editor.keys.y(60);
    check(c3Row && *c3Row >= 0 && *c3Row + editor.keys.keyHeight <= editor.notesArea().getHeight(),
          "an empty clip opens around middle C");
    check(submitter.submit(op("clip.delete", {{"id", 12}}, "Delete Clip")).ok, "the clip is deleted");
    root.frame();
    check(panel.clip() == 0 && panel.details() == nullptr && !editor.isVisible(), "deleted, the view shows no clip");

    // Its top edge: dragged up it grows, the arrangement giving way; dragged to the foot it closes.
    auto &resizer = root.clipResizer;
    const int before = root.clipArea().getHeight();
    resizer.mouseDown(pressAt(resizer, {100.f, 3.f}));
    resizer.mouseDrag(dragAt(resizer, {100.f, 3.f}, {100.f, -57.f}));
    resizer.mouseUp(pressAt(resizer, {100.f, -57.f}));
    check(root.clipArea().getHeight() == before + 60 && root.state().clipViewHeight == before + 60 &&
              ViewStateStore(*store).load("clip-view").clipViewHeight == before + 60,
          "dragging its top edge up grows it, and the height is kept");
    resizer.mouseDown(pressAt(resizer, {100.f, 3.f}));
    resizer.mouseDrag(dragAt(resizer, {100.f, 3.f}, {100.f, 600.f}));
    check(root.state().clipView && root.clipArea().getHeight() == AdiRootComponent::kClipViewFloor,
          "dragged down it stops at its floor while the mouse is held");
    resizer.mouseUp(pressAt(resizer, {100.f, 600.f}));
    check(!root.state().clipView && root.clipArea().isEmpty() && root.deviceArea() == deviceAlone &&
              !ViewStateStore(*store).load("clip-view").clipView,
          "let go at the foot it closes, and the Device View is as it was");

    // The Status Bar's selector and the command open it again.
    root.status.clipToggle.onClick();
    check(root.state().clipView && ViewStateStore(*store).load("clip-view").clipView, "the Clip View Selector opens it, and that is kept");
    check(root.command(AppCommands::ToggleClipView) && !root.state().clipView, "and the command hides it");
}
// The Clip View's editing (STEP-7-PLAN 7.7 step 3): a MIDI clip is made from the window (a double-click
// on an empty MIDI lane, Insert Empty MIDI Clip(s) on a time selection), and its notes are added,
// moved, resized, given velocities and deleted with the mouse and the keys, each gesture one undo
// step, a drag previewed while it is held, and a refused op leaving the notes as they were.
void clipViewNotes(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "clip-view-notes.adi", err);
    check(store != nullptr, "the note editing project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clip View Notes')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole, beat = textproj::kPPQ;
    auto op = [](const char *type, Payload payload, const char *label) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(payload);
        r.label = label;
        return r;
    };
    bool made = submitter.submit(op("track.create", {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}, {"index", 0}}, "Create track")).ok;
    made = submitter.submit(op("track.create", {{"id", 2}, {"kind", "audio"}, {"name", "Audio"}, {"index", 1}}, "Create track")).ok && made;
    check(made, "a MIDI track and an audio track, no clips");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "clip-view-notes");
    commands.activate(root);
    root.setSize(1400, 900);
    for (std::int64_t id = 1; id <= 2; ++id)
        root.arrangement.geometry.resizeTrack(id, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    auto &panel = root.clipView;
    auto &ed = panel.notes;
    const auto x = [&geo](std::int64_t tick) { return static_cast<float>(geo.x(tick)); };
    const auto lane = [&root](std::int64_t id) { return static_cast<float>(laneTop(root, id) + 30); };

    // A double-click on an empty MIDI lane makes a clip of the bar under the pointer and opens it.
    canvas.mouseDoubleClick(pressAt(canvas, {x(2 * bar) + 30, lane(1)}));
    root.frame();
    const auto *made1 = panel.details();
    check(root.state().clipView && made1 != nullptr && panel.midi() && made1->posTicks == 2 * bar &&
              made1->lengthTicks == bar && made1->notes.empty() && geo.selection.clip == made1->id &&
              submitter.nextUndo()->label == "Insert MIDI Clip" && submitter.nextUndo()->opCount == 1,
          "a double-click on an empty MIDI lane makes a one-bar clip there, selects and opens it, one undo step");
    if (made1 == nullptr)
        return;
    const auto clipId = made1->id;
    const auto before = submitter.nextUndo()->txnId;
    canvas.mouseDoubleClick(pressAt(canvas, {x(2 * bar) + 30, lane(2)}));
    root.frame();
    check(submitter.nextUndo()->txnId == before, "on an audio track's empty lane it makes nothing");

    // Insert Empty MIDI Clip(s): across the time selection on the selected MIDI track; refused over a clip.
    geo.selection = {};
    geo.selection.track = 1;
    geo.selection.start = 4 * bar;
    geo.selection.end = 6 * bar;
    check(root.command(AppCommands::InsertMidiClip), "Insert Empty MIDI Clip(s) on a two-bar selection");
    root.frame();
    check(panel.details() != nullptr && panel.details()->id != clipId && panel.details()->posTicks == 4 * bar &&
              panel.details()->lengthTicks == 2 * bar && geo.selection.clip == panel.details()->id,
          "makes a two-bar clip there, selected and shown");
    geo.selection.clip = 0;
    geo.selection.start = 5 * bar;
    geo.selection.end = 7 * bar;
    const auto beforeRefusal = submitter.nextUndo()->txnId;
    check(!root.command(AppCommands::InsertMidiClip) && submitter.nextUndo()->txnId == beforeRefusal && !root.error().empty(),
          "over a clip it is refused, with the reason in the Status Bar");
    geo.selection.start = geo.selection.end = 9 * bar;
    check(!root.command(AppCommands::InsertMidiClip), "and it needs a time selection");

    // Back on the first clip: its bar across the editor, a quarter-note grid, opened around middle C.
    canvas.mouseDoubleClick(pressAt(canvas, {x(2 * bar) + 30, lane(1)}));
    root.frame();
    check(panel.clip() == clipId && ed.gridTicks() == beat, "the first clip again, on a quarter-note grid");
    const auto area = ed.notesArea();
    const int kh = ed.keys.keyHeight;
    const int step = static_cast<int>(std::lround(ed.time.scale)); // a quarter note, in pixels
    const auto xAt = [&](std::int64_t tick) { return static_cast<float>(area.getX() + static_cast<int>(ed.time.x(tick))); };
    const auto yOf = [&](int key) { return static_cast<float>(area.getY() + *ed.keys.y(key) + kh / 2); };
    const auto note = [&](std::size_t i) { return panel.details()->notes.at(i); };
    const auto centre = [&](std::size_t i) {
        const auto box = *ed.boxOf(note(i));
        return juce::Point<float>(static_cast<float>(static_cast<int>(box.x + box.width / 2)),
                                  static_cast<float>(box.y + box.height / 2));
    };
    const auto undoIs = [&](const char *label, int ops) {
        return submitter.nextUndo()->label == label && submitter.nextUndo()->opCount == ops;
    };

    // A double-click on empty space adds a note a grid step long, snapped to the grid, selected.
    ed.mouseDoubleClick(pressAt(ed, {xAt(2 * bar + beat) + 10.f, yOf(64)}));
    root.frame();
    check(panel.details()->notes.size() == 1 && note(0).startTicks == beat && note(0).durationTicks == beat &&
              note(0).key == 64 && note(0).velocity == 100 && ed.selected() == std::vector<std::uint64_t>{note(0).id} &&
              undoIs("Add Note", 1),
          "a double-click adds E3 on the second beat, a beat long, velocity 100, selected, one undo step");

    // Dragged by its body: a grid step later and two keys up, previewed while held, one step on release.
    auto from = centre(0);
    ed.mouseDown(pressAt(ed, from));
    ed.mouseDrag(dragAt(ed, from, {from.x + static_cast<float>(step), from.y - static_cast<float>(2 * kh)}));
    check(ed.shown(note(0)).startTicks == 2 * beat && ed.shown(note(0)).key == 66 && note(0).startTicks == beat &&
              undoIs("Add Note", 1),
          "while the drag is held the note is drawn where it would land, and nothing is written");
    ed.mouseUp(pressAt(ed, {from.x + static_cast<float>(step), from.y - static_cast<float>(2 * kh)}));
    root.frame();
    check(note(0).startTicks == 2 * beat && note(0).key == 66 && undoIs("Move Note", 1),
          "released, it has moved a beat later and two keys up, one undo step");

    // Its end edge dragged a step right: twice as long.
    auto box = *ed.boxOf(note(0));
    from = {static_cast<float>(static_cast<int>(box.x + box.width) - 2), static_cast<float>(box.y + box.height / 2)};
    ed.mouseDown(pressAt(ed, from));
    ed.mouseDrag(dragAt(ed, from, {from.x + static_cast<float>(step), from.y}));
    ed.mouseUp(pressAt(ed, {from.x + static_cast<float>(step), from.y}));
    root.frame();
    check(note(0).startTicks == 2 * beat && note(0).durationTicks == 2 * beat && undoIs("Resize Note", 1),
          "its end edge dragged a step right makes it two beats long");

    // Its start edge dragged a step right: it starts later and still ends where it did; one Undo
    // takes both back.
    box = *ed.boxOf(note(0));
    from = {static_cast<float>(static_cast<int>(box.x) + 1), static_cast<float>(box.y + box.height / 2)};
    ed.mouseDown(pressAt(ed, from));
    ed.mouseDrag(dragAt(ed, from, {from.x + static_cast<float>(step), from.y}));
    ed.mouseUp(pressAt(ed, {from.x + static_cast<float>(step), from.y}));
    root.frame();
    check(note(0).startTicks == 3 * beat && note(0).durationTicks == beat && undoIs("Resize Note", 2),
          "its start edge dragged a step right: it starts a beat later and ends where it did");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(note(0).startTicks == 2 * beat && note(0).durationTicks == 2 * beat, "and one Undo puts both back");

    // The Velocity lane: the note's marker dragged down.
    const auto vel = ed.velocityArea().withTrimmedTop(1);
    const double vTop = vel.getY() + 5, vHeight = vel.getHeight() - 10;
    const float markerX = static_cast<float>(vel.getX() + static_cast<int>(ed.time.x(2 * bar + note(0).startTicks)));
    const int downTo = static_cast<int>(velocityY(50, vTop, vHeight));
    const juce::Point<float> marker(markerX, static_cast<float>(static_cast<int>(velocityY(100, vTop, vHeight))));
    ed.mouseDown(pressAt(ed, marker));
    ed.mouseDrag(dragAt(ed, marker, {markerX, static_cast<float>(downTo)}));
    ed.mouseUp(pressAt(ed, {markerX, static_cast<float>(downTo)}));
    root.frame();
    check(note(0).velocity == velocityAt(downTo, vTop, vHeight) && note(0).velocity < 60 && note(0).velocity > 40 &&
              undoIs("Change Velocity", 1),
          "its velocity marker dragged down to about 50 sets its velocity, one undo step");

    // A second note; Ctrl/Cmd+A selects both; the arrows move them together, one step for both.
    ed.mouseDoubleClick(pressAt(ed, {xAt(2 * bar) + 10.f, yOf(60)}));
    root.frame();
    check(panel.details()->notes.size() == 2, "a second note, C3 on the first beat");
    check(ed.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)) && ed.selected().size() == 2,
          "Ctrl/Cmd+A selects both");
    const auto keysBefore = std::pair<int, int>(note(0).key, note(1).key);
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::upKey)), "Up");
    root.frame();
    check(note(0).key == keysBefore.first + 1 && note(1).key == keysBefore.second + 1 && undoIs("Move Notes", 2),
          "Up moves both a key up, one undo step");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::downKey)), "Down");
    root.frame();
    check(note(0).key == keysBefore.first && note(1).key == keysBefore.second, "Down moves both back");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)), "Shift+Up");
    root.frame();
    check(note(0).key == keysBefore.first + 12 && note(1).key == keysBefore.second + 12, "Shift+Up moves both an octave up");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0)), "Shift+Down");
    root.frame();
    check(note(0).key == keysBefore.first && note(1).key == keysBefore.second, "Shift+Down moves both an octave down again");
    const auto startsBefore = std::pair<std::int64_t, std::int64_t>(note(0).startTicks, note(1).startTicks);
    const auto lengthsBefore = std::pair<std::int64_t, std::int64_t>(note(0).durationTicks, note(1).durationTicks);
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)), "Right");
    root.frame();
    check(note(0).startTicks == startsBefore.first + beat && note(1).startTicks == startsBefore.second + beat,
          "Right moves both a grid step later");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)), "Left");
    root.frame();
    check(note(0).startTicks == startsBefore.first && note(1).startTicks == startsBefore.second, "Left moves both back");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0)), "Shift+Right");
    root.frame();
    check(note(0).durationTicks == lengthsBefore.first + beat && note(1).durationTicks == lengthsBefore.second + beat &&
              undoIs("Resize Notes", 2),
          "Shift+Right makes both a grid step longer, one undo step");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier, 0)), "Shift+Left");
    root.frame();
    check(note(0).durationTicks == lengthsBefore.first && note(1).durationTicks == lengthsBefore.second,
          "Shift+Left makes both a grid step shorter");

    // 0 deactivates the selected notes, one undo step; again, it activates them (manual 10.5.7).
    check(ed.keyPressed(juce::KeyPress('0')), "0");
    root.frame();
    check(!note(0).active && !note(1).active && undoIs("Deactivate Notes", 2), "0 deactivates both notes, one undo step");
    check(ed.keyPressed(juce::KeyPress('0')), "0 again");
    root.frame();
    check(note(0).active && note(1).active && undoIs("Activate Notes", 2), "and 0 again activates them");

    // A click on empty space selects nothing; a drag from empty space selects what it touches.
    const juce::Point<float> empty(xAt(2 * bar) + 4.f, static_cast<float>(area.getY() + 2));
    ed.mouseDown(pressAt(ed, empty));
    ed.mouseUp(pressAt(ed, empty));
    check(ed.selected().empty(), "a click on empty space selects nothing");
    ed.mouseDown(pressAt(ed, empty));
    ed.mouseDrag(dragAt(ed, empty, {static_cast<float>(area.getRight() - 2), static_cast<float>(area.getBottom() - 2)}));
    ed.mouseUp(pressAt(ed, {static_cast<float>(area.getRight() - 2), static_cast<float>(area.getBottom() - 2)}));
    check(ed.selected().size() == 2, "a drag across the editor selects both notes");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && ed.selected().empty(), "Esc deselects");

    // A double-click on a note deletes it.
    const auto kept = note(1).id;
    ed.mouseDoubleClick(pressAt(ed, centre(0)));
    root.frame();
    check(panel.details()->notes.size() == 1 && note(0).id == kept && undoIs("Delete Note", 1),
          "a double-click on a note deletes it, one undo step");

    // Delete with nothing selected does nothing and stays in the editor; with the note selected it
    // deletes it. A note already gone (deleted elsewhere before this view's next frame) is refused by
    // the engine: nothing is written, and the Status Bar says why.
    const auto beforeDelete = submitter.nextUndo()->txnId;
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)) && submitter.nextUndo()->txnId == beforeDelete,
          "Delete with no note selected is the editor's and does nothing");
    ed.select({kept});
    check(submitter.submit(op("note.delete", {{"clip", clipId}, {"note", kept}}, "Delete Note")).ok,
          "the note is deleted behind the view's back");
    const auto behind = submitter.nextUndo()->txnId;
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)) && submitter.nextUndo()->txnId == behind &&
              !root.error().empty(),
          "deleting it again is refused: no step is written and the Status Bar says why");
    root.frame();
    check(panel.details()->notes.empty() && ed.selected().empty(), "the view then shows the clip as it is, nothing selected");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(panel.details()->notes.size() == 1, "and Undo brings the note back into the view");
}
// The Clip View's Main panel (worklist G2; manual 8.2): Start, End, the Loop switch, the loop's Position
// and Length and the clip's Signature act, typed, dragged or through Live's Set buttons, each one of
// the clip's own ops and one undo step; what the engine would refuse is refused with a reason; the
// Note Editor shows the loop brace.
void clipViewFields(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "clip-view-fields.adi", err);
    check(store != nullptr, "the clip fields project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clip View Fields')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole, beat = textproj::kPPQ;
    auto op = [](const char *type, Payload payload, const char *label) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(payload);
        r.label = label;
        return r;
    };
    bool made = submitter.submit(op("track.create", {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}, {"index", 0}}, "Create track")).ok;
    made = submitter.submit(op("clip.create", {{"id", 11}, {"track", 1}, {"kind", "midi"}, {"name", "Loop"}, {"pos", bar}, {"length", 2 * bar}}, "Create clip")).ok && made;
    check(made, "a MIDI track with a two-bar clip in bar 2");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "clip-view-fields");
    commands.activate(root);
    root.setSize(1400, 900);
    root.arrangement.geometry.resizeTrack(1, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    auto &panel = root.clipView;
    using Field = ClipViewPanel::Field;
    canvas.mouseDoubleClick(pressAt(canvas, {static_cast<float>(geo.x(bar)) + 20.f, static_cast<float>(laneTop(root, 1) + 30)}));
    root.frame();
    check(panel.clip() == 11 && panel.readings().start == "1. 1. 1" && panel.readings().end == "3. 1. 1" &&
              panel.readings().position == "1. 1. 1" && panel.readings().length == "2. 0. 0" && !panel.readings().loop,
          "the clip as made: Start 1.1.1, End 3.1.1, the loop off over its two bars");
    const auto undoIs = [&](const char *label) {
        return submitter.nextUndo()->label == label && submitter.nextUndo()->opCount == 1;
    };
    const auto details = [&]() -> const engine::ClipDetails & { return *panel.details(); };
    const auto centreOf = [](juce::Rectangle<int> r) { return r.getCentre().toFloat(); };

    // Typed: End, then Start; a Start past the End is refused, and so is what is not a position.
    check(panel.typeField(Field::End, "2.1.1") && panel.readings().end == "2. 1. 1" && endMarkerOf(details()) == bar &&
              undoIs("Set Clip End"),
          "End typed as 2.1.1: one bar in, one undo step");
    check(panel.typeField(Field::Start, "1.2") && panel.readings().start == "1. 2. 1" &&
              details().startMarkerTicks == beat && undoIs("Set Clip Start"),
          "Start typed as 1.2: the second beat, one undo step");
    const auto beforeRefusal = submitter.nextUndo()->txnId;
    check(!panel.typeField(Field::Start, "5") && submitter.nextUndo()->txnId == beforeRefusal && !root.error().empty() &&
              details().startMarkerTicks == beat,
          "a Start past the End is refused, nothing written, the reason in the Status Bar");
    check(!panel.typeField(Field::Start, "soon") && !panel.typeField(Field::Length, "long") &&
              !panel.typeField(Field::Signature, "waltz") && submitter.nextUndo()->txnId == beforeRefusal,
          "what is not a position, a length or a signature is refused");

    // A click on a field opens it for typing: Enter keeps the value, Esc leaves the field as it was.
    panel.mouseDown(pressAt(panel, centreOf(panel.fieldBox(Field::End))));
    panel.mouseUp(pressAt(panel, centreOf(panel.fieldBox(Field::End))));
    check(panel.editing() == Field::End && panel.fieldEditor().isVisible() && panel.fieldEditor().getText() == "2.1.1" &&
              panel.fieldEditor().getBounds() == panel.fieldBox(Field::End),
          "a click on End opens its editor over the field, holding 2.1.1");
    panel.fieldEditor().setText("3.1.1", false);
    panel.fieldEditor().onReturnKey();
    check(panel.editing() == Field::None && !panel.fieldEditor().isVisible() && panel.readings().end == "3. 1. 1" &&
              undoIs("Set Clip End"),
          "Enter keeps what was typed and closes the editor");
    panel.mouseDown(pressAt(panel, centreOf(panel.fieldBox(Field::End))));
    panel.mouseUp(pressAt(panel, centreOf(panel.fieldBox(Field::End))));
    panel.fieldEditor().setText("9.1.1", false);
    const auto beforeEscape = submitter.nextUndo()->txnId;
    panel.fieldEditor().onEscapeKey();
    check(panel.editing() == Field::None && panel.readings().end == "3. 1. 1" && submitter.nextUndo()->txnId == beforeEscape,
          "Esc closes it and leaves the field as it was");

    // Dragged: the bars part of Position, up two steps; it reads the new value while held and is one
    // step when let go.
    const auto position = panel.fieldBox(Field::Position);
    const juce::Point<float> bars(static_cast<float>(position.getX() + 4), static_cast<float>(position.getCentreY()));
    panel.mouseDown(pressAt(panel, bars));
    panel.mouseDrag(dragAt(panel, bars, {bars.x, bars.y - 8.f}));
    check(panel.readings().position == "3. 1. 1" && details().loopStartTicks == 0 && submitter.nextUndo()->txnId == beforeEscape,
          "while Position's bars are dragged up two steps it reads 3.1.1, and nothing is written");
    panel.mouseUp(pressAt(panel, {bars.x, bars.y - 8.f}));
    check(panel.readings().position == "3. 1. 1" && details().loopStartTicks == 2 * bar && panel.editing() == Field::None &&
              undoIs("Set Loop Position"),
          "let go, the loop starts at 3.1.1: one undo step, and no editor opens after a drag");
    // The sixteenths part of Length, down one step.
    const auto length = panel.fieldBox(Field::Length);
    const juce::Point<float> sixteenths(static_cast<float>(length.getRight() - 4), static_cast<float>(length.getCentreY()));
    const auto lengthBefore = loopLengthOf(details());
    panel.mouseDown(pressAt(panel, sixteenths));
    panel.mouseDrag(dragAt(panel, sixteenths, {sixteenths.x, sixteenths.y + 4.f}));
    panel.mouseUp(pressAt(panel, {sixteenths.x, sixteenths.y + 4.f}));
    check(loopLengthOf(details()) == lengthBefore - beat / 4 && undoIs("Set Loop Length"),
          "Length's sixteenths dragged down a step: a sixteenth shorter");
    check(panel.typeField(Field::Length, "1.2.0") && loopLengthOf(details()) == bar + 2 * beat &&
              panel.readings().length == "1. 2. 0" && undoIs("Set Loop Length"),
          "Length typed as 1.2.0: a bar and two beats");

    // The Loop switch, and the loop brace in the Note Editor's ruler.
    auto &ed = panel.notes;
    panel.mouseDown(pressAt(panel, centreOf(panel.loopBox())));
    panel.mouseUp(pressAt(panel, centreOf(panel.loopBox())));
    check(details().loopEnabled && panel.readings().loop && undoIs("Activate Loop") && details().loopStartTicks == 2 * bar &&
              loopLengthOf(details()) == bar + 2 * beat,
          "the Loop switch turns the loop on, its window as it was set, one undo step");
    const auto brace = ed.loopBrace();
    const auto window = ClipWindow::of(details());
    check(brace && brace->getStart() == ed.rulerArea().getX() + static_cast<int>(ed.time.x(window.toArrangement(2 * bar))) &&
              brace->getEnd() ==
                  ed.rulerArea().getX() + static_cast<int>(ed.time.x(window.toArrangement(2 * bar + bar + 2 * beat))),
          "the Note Editor's ruler shows the loop brace over the loop's window");
    panel.mouseDown(pressAt(panel, centreOf(panel.loopBox())));
    panel.mouseUp(pressAt(panel, centreOf(panel.loopBox())));
    check(!details().loopEnabled && undoIs("Deactivate Loop") && loopLengthOf(details()) == bar + 2 * beat,
          "and off again, keeping its window");

    // Live's Set buttons take the arrangement's insert marker, in the clip's own time.
    geo.selection.insert = bar + 6 * beat;
    panel.mouseDown(pressAt(panel, centreOf(panel.setBox(Field::End))));
    panel.mouseUp(pressAt(panel, centreOf(panel.setBox(Field::End))));
    check(endMarkerOf(details()) == details().startMarkerTicks + 6 * beat && undoIs("Set Clip End") &&
              panel.editing() == Field::None,
          "End's Set puts the End marker at the insert marker, one undo step");
    geo.selection.insert = bar + 2 * beat;
    check(panel.setFieldHere(Field::Start) && details().startMarkerTicks == window.toSource(bar + 2 * beat) &&
              undoIs("Set Clip Start"),
          "Start's Set puts the Start marker there");

    // On the loop, Live's Set also switches it on: Set Position moves its start to the marker.
    geo.selection.insert = bar + 3 * beat;
    const auto loopHere = ClipWindow::of(details()).toSource(bar + 3 * beat);
    check(!details().loopEnabled && panel.setFieldHere(Field::Position) && details().loopEnabled &&
              details().loopStartTicks == loopHere && undoIs("Set Loop Position"),
          "the loop's Set Position moves its start to the marker and switches the loop on, one undo step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // The Signature: typed, dragged by its denominator, refused when it is not one.
    check(panel.typeField(Field::Signature, "3/4") && panel.readings().signature == "3 / 4" && details().signature.local &&
              details().signature.numerator == 3 && undoIs("Change Clip Time Signature"),
          "the Signature typed as 3/4 is the clip's own, one undo step");
    const auto signature = panel.fieldBox(Field::Signature);
    const juce::Point<float> denominator(static_cast<float>(signature.getRight() - 4), static_cast<float>(signature.getCentreY()));
    panel.mouseDown(pressAt(panel, denominator));
    panel.mouseDrag(dragAt(panel, denominator, {denominator.x, denominator.y - 4.f}));
    check(panel.readings().signature == "3 / 8", "its denominator dragged up a step reads 3 / 8 while held");
    panel.mouseUp(pressAt(panel, {denominator.x, denominator.y - 4.f}));
    check(details().signature.denominator == 8 && undoIs("Change Clip Time Signature"), "and is 3/8 when let go");
    const auto beforeBad = submitter.nextUndo()->txnId;
    check(!panel.typeField(Field::Signature, "3/5") && submitter.nextUndo()->txnId == beforeBad && !root.error().empty(),
          "3/5 is not a time signature: refused with the reason");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "two undos");
    root.frame();
    check(!details().signature.local && panel.readings().signature == "4 / 4", "take the clip back to the song's 4/4");
}
// The Clip View's Pitch and Time panel and its Scale chooser (STEP-7-PLAN 7.7 step 4; manual 8.5, 8.2.5):
// a MIDI clip has the panel beside the Main panel; Fit to Scale, Invert, Reverse, x2, /2 and Legato act on
// the selected notes, or on all when none is selected, one undo step each; Transpose is dragged or typed;
// the Scale field's choosers set the clip's own scale.
void clipViewTools(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "clip-view-tools.adi", err);
    check(store != nullptr, "the clip tools project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clip View Tools')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole, beat = textproj::kPPQ;
    auto op = [](const char *type, Payload payload, const char *label) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(payload);
        r.label = label;
        return r;
    };
    bool made = submitter.submit(op("track.create", {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}, {"index", 0}}, "Create track")).ok;
    made = submitter.submit(op("track.create", {{"id", 2}, {"kind", "audio"}, {"name", "Audio"}, {"index", 1}}, "Create track")).ok && made;
    made = submitter.submit(op("clip.create", {{"id", 11}, {"track", 1}, {"kind", "midi"}, {"name", "Tune"}, {"pos", bar}, {"length", 2 * bar}}, "Create clip")).ok && made;
    made = submitter.submit(op("clip.create", {{"id", 12}, {"track", 2}, {"kind", "audio"}, {"name", "Take"}, {"pos", bar}, {"length", bar}}, "Create clip")).ok && made;
    // C3, E3 and F#3, a beat each, one after the other.
    made = submitter.submit(std::vector<OpRequest>{
               op("note.insert", {{"clip", 11}, {"note", 1}, {"start", 0}, {"dur", beat}, {"key", 60}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 2}, {"start", beat}, {"dur", beat}, {"key", 64}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 3}, {"start", 2 * beat}, {"dur", beat}, {"key", 66}}, "Add Note")}).ok && made;
    check(made, "a MIDI clip of three notes and an audio clip");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "clip-view-tools");
    commands.activate(root);
    root.setSize(1400, 900);
    for (std::int64_t id = 1; id <= 2; ++id)
        root.arrangement.geometry.resizeTrack(id, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    auto &panel = root.clipView;
    auto &ed = panel.notes;
    using Field = ClipViewPanel::Field;
    using Tool = ClipViewPanel::Tool;
    const auto open = [&](std::int64_t track) {
        canvas.mouseDoubleClick(pressAt(canvas, {static_cast<float>(geo.x(bar)) + 20.f, static_cast<float>(laneTop(root, track) + 30)}));
        root.frame();
    };
    const auto click = [&](juce::Rectangle<int> box) {
        panel.mouseDown(pressAt(panel, box.getCentre().toFloat()));
        panel.mouseUp(pressAt(panel, box.getCentre().toFloat()));
    };
    const auto note = [&](std::uint64_t id) {
        for (const auto &n : panel.details()->notes)
            if (n.id == id)
                return n;
        return engine::ClipNote{};
    };
    const auto undoIs = [&](const char *label, int ops) {
        return submitter.nextUndo()->label == label && submitter.nextUndo()->opCount == ops;
    };

    // An audio clip has the Main panel only; a MIDI clip has the Pitch and Time panel beside it.
    open(2);
    check(panel.audio() && panel.toolsArea().isEmpty() && panel.panelWidth() == ClipViewPanel::kPanelWidth &&
              panel.sample.getX() == ClipViewPanel::kPanelWidth + 1 && panel.fieldAt(panel.fieldBox(Field::Scale).getCentre()) == Field::None,
          "an audio clip: the Main panel only, the Sample Editor right beside it, no Scale field");
    open(1);
    const int both = ClipViewPanel::kPanelWidth + ClipViewPanel::kToolsWidth;
    check(panel.midi() && !panel.toolsArea().isEmpty() && panel.panelWidth() == both && ed.getX() == both + 1 &&
              panel.readings().range == "C3 - F#3" && panel.readings().transpose == "0 st" && panel.readings().scale == "C Major",
          "a MIDI clip: the Pitch and Time panel, the Note Editor beside both; the range C3 - F#3, C Major");
    const auto fit = ed.boxOf(note(3));
    check(fit && fit->x + fit->width <= ed.notesArea().getRight(), "and the editor still opens with the whole clip in view");

    // Fit to Scale: F#3 is not in C major, and goes to F3, the lower of its two neighbours.
    click(panel.toolBox(Tool::FitToScale));
    check(note(1).key == 60 && note(2).key == 64 && note(3).key == 65 && undoIs("Fit to Scale", 1),
          "Fit to Scale moves F#3 to F3 and leaves the others, one undo step");
    // Invert, between the lowest and the highest.
    click(panel.toolBox(Tool::Invert));
    check(note(1).key == 65 && note(2).key == 61 && note(3).key == 60 && undoIs("Invert Notes", 3), "Invert mirrors the three in pitch");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // Reverse, between the first start and the last end.
    click(panel.toolBox(Tool::Reverse));
    check(note(1).startTicks == 2 * beat && note(2).startTicks == beat && note(3).startTicks == 0 && undoIs("Reverse Notes", 2),
          "Reverse mirrors them in time: the last is first");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // x2 and /2.
    click(panel.toolBox(Tool::Double));
    check(note(1).startTicks == 0 && note(1).durationTicks == 2 * beat && note(2).startTicks == 2 * beat &&
              note(3).startTicks == 4 * beat && note(3).durationTicks == 2 * beat && undoIs("Stretch Notes", 5),
          "x2 doubles the starts and the lengths from the first note, one undo step");
    click(panel.toolBox(Tool::Halve));
    check(note(2).startTicks == beat && note(3).startTicks == 2 * beat && note(3).durationTicks == beat && undoIs("Stretch Notes", 5),
          "/2 halves them again");
    // Legato: with the middle note gone, the first lasts until the last begins.
    const auto beforeLegato = submitter.nextUndo()->txnId;
    check(panel.runTool(Tool::Legato) && submitter.nextUndo()->txnId == beforeLegato, "Legato on notes that already touch writes nothing");
    ed.select({2});
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "the middle note deleted");
    click(panel.toolBox(Tool::Legato));
    check(note(1).durationTicks == 2 * beat && note(3).durationTicks == beat && undoIs("Legato", 1),
          "Legato makes the first note last until the next begins");

    // Transpose: dragged, it reads the semitones while held and moves the notes when let go; typed too.
    const auto semitones = panel.fieldBox(Field::Transpose);
    const juce::Point<float> grab(static_cast<float>(semitones.getCentreX()), static_cast<float>(semitones.getCentreY()));
    panel.mouseDown(pressAt(panel, grab));
    panel.mouseDrag(dragAt(panel, grab, {grab.x, grab.y - 12.f}));
    check(panel.readings().transpose == "+3 st" && note(1).key == 60, "Transpose dragged up three steps reads +3 st, nothing written yet");
    panel.mouseUp(pressAt(panel, {grab.x, grab.y - 12.f}));
    check(note(1).key == 63 && note(3).key == 68 && panel.readings().transpose == "0 st" && panel.readings().range == "D#3 - G#3" &&
              undoIs("Transpose Notes", 2),
          "let go, both notes are three semitones up, one undo step, and the field reads 0 st again");
    check(panel.typeField(Field::Transpose, "-3") && note(1).key == 60 && note(3).key == 65, "typed as -3, they are back");
    const auto beforeBad = submitter.nextUndo()->txnId;
    check(!panel.typeField(Field::Transpose, "up") && !panel.typeField(Field::Transpose, "3-") &&
              submitter.nextUndo()->txnId == beforeBad,
          "what is not a number of semitones is refused");
    // With a note selected, the utilities act on it alone, and the range reads it alone.
    ed.select({3});
    check(panel.readings().range == "F3 - F3" && panel.transpose(12) && note(3).key == 77 && note(1).key == 60 &&
              undoIs("Transpose Notes", 1) && ed.selected() == std::vector<std::uint64_t>{3},
          "with one note selected, Transpose moves that note alone and it stays selected");
    ed.select({});

    // The Scale field's choosers: twelve roots, the catalogue's scales; a choice is the clip's own scale.
    check(panel.scaleMenu(true).getNumItems() == 12 && panel.scaleMenu(false).getNumItems() == static_cast<int>(kScales.size()),
          "the Scale field's choosers: twelve root notes, and the scales");
    check(panel.chooseScale(true, 3) && panel.readings().scale == "D Major" && panel.details()->scale.local &&
              panel.details()->scale.root == 2 && undoIs("Change Clip Scale", 1),
          "the root chosen as D: D Major, the clip's own, one undo step");
    check(panel.chooseScale(false, 11) && panel.readings().scale == "D Minor Blues" && panel.details()->scale.id == "minor-blues" &&
              panel.details()->scale.mask == findScale("minor-blues")->mask,
          "the scale chosen as Minor Blues: D Minor Blues");
    check(!panel.chooseScale(true, 13) && !panel.chooseScale(false, 0) && !panel.chooseScale(false, 99), "an item the chooser does not have does nothing");
    check(panel.transpose(1) && note(1).key == 61 && note(3).key == 78, "a semitone up, neither note is in D Minor Blues");
    click(panel.toolBox(Tool::FitToScale));
    check(inScale(note(1).key, 2, panel.details()->scale.mask) && inScale(note(3).key, 2, panel.details()->scale.mask) &&
              undoIs("Fit to Scale", 2),
          "Fit to Scale then fits the notes to D Minor Blues");

    // ADI's additions from Cubase's Key Editor (ADR-0241 d2): chord editing and the length functions.
    ed.select({});
    check(submitter.submit(std::vector<OpRequest>{op("note.delete", {{"clip", 11}, {"note", 1}}, "Delete Note"),
                                                  op("note.delete", {{"clip", 11}, {"note", 3}}, "Delete Note"),
                                                  op("note.insert", {{"clip", 11}, {"note", 20}, {"start", 0}, {"dur", 3 * beat}, {"key", 60}, {"vel", 90}}, "Add Note")}).ok,
          "the clip holds one C3, three beats long");
    root.frame();
    panel.setToolsTab(2);
    check(panel.chordMenu().getNumItems() == static_cast<int>(kChordTypes.size()) && !panel.chordBox().isEmpty() &&
              std::string(panel.chordType().name) == "Major",
          "the chord type's chooser lists the chord types, Major first");
    check(panel.chooseChord(7) && std::string(panel.chordType().name) == "Major 7" && !panel.chooseChord(0) && !panel.chooseChord(99),
          "a chord type is chosen from it");
    ed.select({20});
    click(panel.toolBox(Tool::Chord));
    check(panel.details()->notes.size() == 4 && ed.selected().size() == 4 && undoIs("Insert Chord", 3),
          "Add Chord builds C major 7 on the selected note: three notes added, all four selected, one undo step");
    int low = 127, high = 0;
    for (const auto &n : panel.details()->notes) {
        low = std::min(low, static_cast<int>(n.key));
        high = std::max(high, static_cast<int>(n.key));
    }
    check(low == 60 && high == 71, "from C3 up to B3");
    click(panel.toolBox(Tool::InvertUp));
    low = 127;
    high = 0;
    for (const auto &n : panel.details()->notes) {
        low = std::min(low, static_cast<int>(n.key));
        high = std::max(high, static_cast<int>(n.key));
    }
    check(low == 64 && high == 72 && undoIs("Invert Chord Up", 1), "Inversion Up moves the C up an octave: E3 to C4");
    click(panel.toolBox(Tool::InvertDown));
    check(note(20).key == 60 && undoIs("Invert Chord Down", 1), "Inversion Down brings it back");
    click(panel.toolBox(Tool::Drop2));
    low = 127;
    for (const auto &n : panel.details()->notes)
        low = std::min(low, static_cast<int>(n.key));
    check(low == 55 && undoIs("Drop 2", 1), "Drop 2 takes the G down an octave");
    panel.setToolsTab(0);
    click(panel.toolBox(Tool::FixedLength));
    bool allStep = true;
    for (const auto &n : panel.details()->notes)
        allStep = allStep && n.durationTicks == ed.gridTicks();
    check(allStep && undoIs("Set Length", 4), "Set Length, its Duration on Grid, makes the four notes a grid step long");
    ed.select({});
    check(submitter.submit(op("note.insert", {{"clip", 11}, {"note", 30}, {"start", beat / 2}, {"dur", beat}, {"key", 60}}, "Add Note")).ok,
          "a second C3 begins while the first still sounds");
    root.frame();
    panel.setToolsTab(2);
    click(panel.toolBox(Tool::OverlapsMono));
    panel.setToolsTab(0);
    check(note(20).durationTicks == beat / 2 && undoIs("Delete Overlaps (Mono)", 1), "Overlaps Mono cuts the first C3 where the second begins");

    // ADI's Functions tab (ADR-0242 d1): the velocity functions, Delete Doubles and Restrict Polyphony.
    check(panel.toolsTab() == 0 && panel.toolBox(Tool::VelocityAdd).isEmpty() && !panel.toolBox(Tool::Invert).isEmpty(),
          "the tools panel opens on Pitch and Time");
    click(panel.tabBox(1));
    check(panel.toolsTab() == 1 && panel.toolBox(Tool::Invert).isEmpty() && panel.rangeBox().isEmpty() &&
              panel.fieldBox(Field::Transpose).isEmpty() && !panel.toolBox(Tool::VelocityAdd).isEmpty() &&
              !panel.fieldBox(Field::VelocityAmount).isEmpty(),
          "a click on Functions shows its tools and fields, and hides Pitch and Time's");
    {
        std::vector<juce::Rectangle<int>> boxes;
        for (const auto tool : {Tool::VelocityAdd, Tool::VelocityScale, Tool::VelocityLimit, Tool::DeleteDoubles, Tool::RestrictPolyphony})
            boxes.push_back(panel.toolBox(tool));
        for (const auto field : {Field::VelocityAmount, Field::VelocityPercent, Field::VelocityLow, Field::VelocityHigh, Field::Voices})
            boxes.push_back(panel.fieldBox(field));
        bool apart = true;
        for (std::size_t i = 0; i < boxes.size(); ++i)
            for (std::size_t j = i + 1; j < boxes.size(); ++j)
                apart = apart && !boxes[i].isEmpty() && !boxes[i].intersects(boxes[j]) && panel.toolsArea().contains(boxes[j]);
        check(apart, "its five buttons and five fields each have their own place in the panel");
    }
    for (const auto &n : std::vector<engine::ClipNote>(panel.details()->notes))
        check(submitter.submit(op("note.delete", {{"clip", 11}, {"note", n.id}}, "Delete Note")).ok, "the clip is cleared");
    check(submitter.submit(std::vector<OpRequest>{
              op("note.insert", {{"clip", 11}, {"note", 50}, {"start", 0}, {"dur", 4 * beat}, {"key", 60}, {"vel", 100}}, "Add Note"),
              op("note.insert", {{"clip", 11}, {"note", 51}, {"start", beat}, {"dur", 4 * beat}, {"key", 62}, {"vel", 40}}, "Add Note"),
              op("note.insert", {{"clip", 11}, {"note", 52}, {"start", 2 * beat}, {"dur", beat}, {"key", 64}, {"vel", 120}}, "Add Note")}).ok,
          "three notes that pile up: C3 at 100, D3 at 40, E3 at 120");
    root.frame();
    ed.select({});
    click(panel.toolBox(Tool::VelocityAdd));
    check(note(50).velocity == 110 && note(51).velocity == 50 && note(52).velocity == 127 && undoIs("Add Velocity", 3),
          "Add, with none selected, raises every velocity by the amount in its field, 10: one undo step");
    check(panel.typeField(Field::VelocityAmount, "-20") && panel.velocityAmount == -20, "the amount typed as -20");
    ed.select({50});
    click(panel.toolBox(Tool::VelocityAdd));
    check(note(50).velocity == 90 && note(51).velocity == 50 && undoIs("Subtract Velocity", 1), "and only the selected note drops by 20");
    const auto beforeTyping = submitter.nextUndo()->txnId;
    check(!panel.typeField(Field::VelocityAmount, "loud") && panel.velocityAmount == -20 && !root.error().empty() &&
              panel.typeField(Field::VelocityAmount, "900") && panel.velocityAmount == 127 && submitter.nextUndo()->txnId == beforeTyping,
          "a word is refused; a number out of range is brought inside it; typing an amount is no undo step");
    ed.select({});
    click(panel.toolBox(Tool::VelocityScale));
    check(note(50).velocity == 45 && note(51).velocity == 25 && note(52).velocity == 64 && undoIs("Compress Velocities", 3),
          "Scale at 50 per cent halves them");
    // A field dragged: four pixels a step.
    {
        const auto box = panel.fieldBox(Field::VelocityLow).getCentre().toFloat();
        panel.mouseDown(pressAt(panel, box));
        panel.mouseDrag(pressAt(panel, box.translated(0.f, -120.f)));
        panel.mouseUp(pressAt(panel, box.translated(0.f, -120.f)));
    }
    check(panel.velocityLow == 31 && submitter.nextUndo()->label == "Compress Velocities", "the low limit dragged up by thirty steps: 31");
    check(panel.typeField(Field::VelocityHigh, "60") && panel.velocityHigh == 60, "the high limit typed");
    click(panel.toolBox(Tool::VelocityLimit));
    check(note(50).velocity == 45 && note(51).velocity == 31 && note(52).velocity == 60 && undoIs("Limit Velocities", 2),
          "Limit brings the two outside 31 to 60 to its ends");
    check(panel.typeField(Field::Voices, "2") && panel.voices == 2, "two voices");
    click(panel.toolBox(Tool::RestrictPolyphony));
    check(note(50).durationTicks == 2 * beat && note(51).durationTicks == 4 * beat && undoIs("Restrict Polyphony", 1),
          "Restrict Polyphony: where the third note begins, the first is cut to end there");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(note(50).durationTicks == 4 * beat, "and Undo gives it back");
    const bool doubled = submitter.submit(op("note.insert", {{"clip", 11}, {"note", 53}, {"start", 0}, {"dur", beat}, {"key", 60}, {"vel", 90}}, "Add Note")).ok;
    root.frame();
    if (doubled) {
        click(panel.toolBox(Tool::DeleteDoubles));
        check(panel.details()->notes.size() == 3 && note(50).id == 50 && note(53).id == 0 && undoIs("Delete Doubles", 1),
              "Delete Doubles removes the second C3 that begins with the first");
    }
    const auto beforeNone = submitter.nextUndo()->txnId;
    click(panel.toolBox(Tool::DeleteDoubles));
    check(submitter.nextUndo()->txnId == beforeNone, "with no doubles, it writes nothing");
    click(panel.tabBox(0));
    check(panel.toolsTab() == 0 && !panel.toolBox(Tool::Invert).isEmpty() && panel.toolBox(Tool::VelocityAdd).isEmpty(),
          "and a click on Pitch and Time brings its tools back");
    click(panel.tabBox(1));

    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-clip-view-tools.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
}
// The Note Editor's Fold, copies and zoom, and the playhead (DF-W2; manual 10.2, 10.5.3, 10.5.4): Fold to
// Notes shows only the key tracks in use and follows the notes; Ctrl (Alt/Option on the Mac) and a drag
// copy the selected notes; Ctrl/Cmd+D duplicates them after themselves; Z zooms to the selection and X
// goes back; the playhead crosses the editor while the song plays.
void clipViewMore(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "clip-view-more.adi", err);
    check(store != nullptr, "the Fold and copies project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clip View More')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole, beat = textproj::kPPQ;
    auto op = [](const char *type, Payload payload, const char *label) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(payload);
        r.label = label;
        return r;
    };
    bool made = submitter.submit(op("track.create", {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}, {"index", 0}}, "Create track")).ok;
    made = submitter.submit(op("clip.create", {{"id", 11}, {"track", 1}, {"kind", "midi"}, {"name", "Riff"}, {"pos", 0}, {"length", 2 * bar}}, "Create clip")).ok && made;
    // C3 on the first beat and E3 on the second, a beat each, in a two-bar clip at the song's start.
    made = submitter.submit(std::vector<OpRequest>{
               op("note.insert", {{"clip", 11}, {"note", 1}, {"start", 0}, {"dur", beat}, {"key", 60}, {"vel", 90}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 2}, {"start", beat}, {"dur", beat}, {"key", 64}, {"vel", 70}}, "Add Note")}).ok && made;
    check(made, "a two-bar MIDI clip with C3 and E3");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "clip-view-more");
    commands.activate(root);
    root.setSize(1400, 900);
    root.arrangement.geometry.resizeTrack(1, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    auto &panel = root.clipView;
    auto &ed = panel.notes;
    canvas.mouseDoubleClick(pressAt(canvas, {static_cast<float>(geo.x(0)) + 20.f, static_cast<float>(laneTop(root, 1) + 30)}));
    root.frame();
    check(panel.clip() == 11 && panel.details()->notes.size() == 2, "the clip is open with its two notes");
    const auto note = [&](std::uint64_t id) {
        for (const auto &n : panel.details()->notes)
            if (n.id == id)
                return n;
        return engine::ClipNote{};
    };
    const auto undoIs = [&](const char *label, int ops) {
        return submitter.nextUndo()->label == label && submitter.nextUndo()->opCount == ops;
    };
    const int kh = ed.keys.keyHeight;

    // Fold: the header's switch shows only the keys in use; it follows a note added; F unfolds.
    check(!ed.folded() && ed.keys.rowCount() == 128 && !panel.foldBox().isEmpty(), "unfolded, all 128 key tracks; the Fold switch is in the header");
    panel.mouseDown(pressAt(panel, panel.foldBox().getCentre().toFloat()));
    panel.mouseUp(pressAt(panel, panel.foldBox().getCentre().toFloat()));
    const auto c3 = ed.boxOf(note(1)), e3 = ed.boxOf(note(2));
    check(ed.folded() && ed.keys.rowCount() == 2 && c3 && e3 && e3->y == ed.notesArea().getY() && c3->y == e3->y + kh,
          "the Fold switch leaves the two key tracks in use, one under the other from the top");
    check(submitter.submit(op("note.insert", {{"clip", 11}, {"note", 3}, {"start", 2 * beat}, {"dur", beat}, {"key", 67}}, "Add Note")).ok,
          "a G3 is added");
    root.frame();
    check(ed.folded() && ed.keys.rowCount() == 3 && ed.keys.rowOf(67) == 0, "folded, the editor follows: three key tracks, G3 on top");
    check(ed.keyPressed(juce::KeyPress('f')) && !ed.folded() && ed.keys.rowCount() == 128, "F unfolds");
    const auto back = ed.boxOf(note(3));
    check(back && back->y >= ed.notesArea().getY() && back->y + back->height <= ed.notesArea().getBottom(),
          "and the notes are still in view");
    check(root.command(AppCommands::Undo), "undo the G3");
    root.frame();

    // Ctrl (Alt/Option on the Mac) and a drag copy: the original stays, the copy lands and is selected.
#if JUCE_MAC
    const juce::ModifierKeys copyKey(juce::ModifierKeys::altModifier);
#else
    const juce::ModifierKeys copyKey(juce::ModifierKeys::ctrlModifier);
#endif
    const auto box = *ed.boxOf(note(2));
    const juce::Point<float> from(static_cast<float>(static_cast<int>(box.x + box.width / 2)), static_cast<float>(box.y + box.height / 2));
    const float step = static_cast<float>(std::lround(ed.time.scale));
    ed.mouseDown(pressAt(ed, from));
    ed.mouseDrag(dragAt(ed, from, {from.x + 2 * step, from.y - static_cast<float>(3 * kh)}, copyKey));
    check(ed.copying() && ed.shown(note(2)).startTicks == 3 * beat && ed.shown(note(2)).key == 67,
          "with the copy key held, the drag shows where the copy would land");
    ed.mouseUp(pressAt(ed, {from.x + 2 * step, from.y - static_cast<float>(3 * kh)}, copyKey));
    check(panel.details()->notes.size() == 3 && note(2).startTicks == beat && note(2).key == 64 && ed.selected().size() == 1 &&
              note(ed.selected().front()).startTicks == 3 * beat && note(ed.selected().front()).key == 67 &&
              note(ed.selected().front()).velocity == 70 && undoIs("Copy Note", 1),
          "let go, E3 is where it was and its copy, G3 two beats later with the same velocity, is selected: one undo step");
    check(root.command(AppCommands::Undo), "undo the copy");
    root.frame();
    check(panel.details()->notes.size() == 2, "and it is gone");

    // Duplicate: the selected notes again, right after themselves.
    check(ed.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)) && ed.selected().size() == 2, "both selected");
    check(ed.keyPressed(juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)), "Ctrl/Cmd+D");
    check(panel.details()->notes.size() == 4 && ed.selected().size() == 2 && undoIs("Duplicate Notes", 2), "two copies, selected, one undo step");
    bool placed = true;
    for (const auto id : ed.selected())
        placed = placed && (note(id).startTicks == 2 * beat || note(id).startTicks == 3 * beat) && (note(id).key == 60 || note(id).key == 64);
    check(placed, "the copies start where the originals' two beats end");
    ed.select({});
    const auto beforeNone = submitter.nextUndo()->txnId;
    check(ed.keyPressed(juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)) && submitter.nextUndo()->txnId == beforeNone &&
              panel.details()->notes.size() == 4,
          "with no note selected, Ctrl/Cmd+D is still the editor's and duplicates nothing");

    // Z zooms to the selected note; X goes back.
    const double scale = ed.time.scale, left = ed.time.left;
    ed.select({2});
    check(ed.keyPressed(juce::KeyPress('z')), "Z");
    const auto zoomed = ed.boxOf(note(2));
    check(ed.time.scale > 3 * scale && zoomed && zoomed->x == ed.notesArea().getX() && zoomed->width > ed.notesArea().getWidth() * 0.9,
          "Z fills the editor with the selected note");
    check(ed.keyPressed(juce::KeyPress('x')) && ed.time.scale == scale && ed.time.left == left, "X goes back to the view before");

    // Draw Mode (B, or the header's switch): a stroke across empty space draws a note at each grid step on
    // the key it began on, one undo step; steps a note already fills are skipped; a click on a note deletes it.
    check(!ed.drawMode() && ed.keyPressed(juce::KeyPress('b')) && ed.drawMode() && !panel.drawBox().isEmpty(), "B turns Draw Mode on");
    const auto area = ed.notesArea();
    const auto xAt = [&](std::int64_t tick) { return static_cast<float>(area.getX() + static_cast<int>(ed.time.x(tick))); };
    const auto yOf = [&](int key) { return static_cast<float>(area.getY() + *ed.keys.y(key) + kh / 2); };
    const auto countBefore = panel.details()->notes.size();
    ed.mouseDown(pressAt(ed, {xAt(4 * beat) + 5.f, yOf(67)}));
    ed.mouseDrag(dragAt(ed, {xAt(4 * beat) + 5.f, yOf(67)}, {xAt(6 * beat) + 5.f, yOf(62)}));
    check(ed.drawnSteps() == std::vector<std::int64_t>({4 * beat, 5 * beat, 6 * beat}) && panel.details()->notes.size() == countBefore,
          "a stroke across three grid steps shows three notes to come, on the key it began on, nothing written yet");
    ed.mouseUp(pressAt(ed, {xAt(6 * beat) + 5.f, yOf(62)}));
    int drawn = 0;
    for (const auto &n : panel.details()->notes)
        if (n.key == 67 && n.durationTicks == beat && (n.startTicks == 4 * beat || n.startTicks == 5 * beat || n.startTicks == 6 * beat))
            ++drawn;
    check(panel.details()->notes.size() == countBefore + 3 && drawn == 3 && ed.selected().size() == 3 && undoIs("Draw Notes", 3),
          "let go, three G3s a grid step long, selected, one undo step");
    ed.mouseDown(pressAt(ed, {xAt(0) + 5.f, yOf(60)}));
    ed.mouseUp(pressAt(ed, {xAt(0) + 5.f, yOf(60)}));
    check(panel.details()->notes.size() == countBefore + 2 && undoIs("Delete Note", 1), "in Draw Mode a click on a note deletes it");
    check(root.command(AppCommands::Undo), "undo the delete");
    root.frame();
    ed.mouseDown(pressAt(ed, {xAt(beat) + 5.f, yOf(60)}));
    ed.mouseDrag(dragAt(ed, {xAt(beat) + 5.f, yOf(60)}, {xAt(2 * beat) + 5.f, yOf(60)}));
    ed.mouseUp(pressAt(ed, {xAt(2 * beat) + 5.f, yOf(60)}));
    check(panel.details()->notes.size() == countBefore + 4 && undoIs("Draw Notes", 1),
          "a stroke over a step a note already fills draws only the empty step");
    const auto beforeDouble = submitter.nextUndo()->txnId;
    ed.mouseDoubleClick(pressAt(ed, {xAt(7 * beat) + 5.f, yOf(62)}));
    check(submitter.nextUndo()->txnId == beforeDouble, "a double-click adds nothing of its own in Draw Mode");
    panel.mouseDown(pressAt(panel, panel.drawBox().getCentre().toFloat()));
    panel.mouseUp(pressAt(panel, panel.drawBox().getCentre().toFloat()));
    check(!ed.drawMode(), "the header's switch turns it off");

    // Live's overlap rule, as measured: a note placed over another on its key survives whole; the older is cut or removed.
    ed.select({});
    check(submitter.submit(std::vector<OpRequest>{op("note.insert", {{"clip", 11}, {"note", 40}, {"start", 0}, {"dur", 4 * beat}, {"key", 72}}, "Add Note"),
                                                  op("note.insert", {{"clip", 11}, {"note", 41}, {"start", 5 * beat}, {"dur", beat}, {"key", 72}}, "Add Note")}).ok,
          "a long C4 from the start, and a short one later");
    root.frame();
    ed.select({41});
    for (int i = 0; i < 3; ++i)
        check(ed.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)), "Left");
    check(note(41).startTicks == 2 * beat && note(40).durationTicks == 2 * beat && submitter.nextUndo()->opCount == 2 &&
              submitter.nextUndo()->label == "Move Note",
          "moved into the long note, the short one survives whole and the long one now ends where it begins: one undo step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(note(40).durationTicks == 3 * beat && note(41).startTicks == 3 * beat,
          "and Undo takes back the last step: the short note a beat later, the long one a beat longer");
    check(submitter.submit(std::vector<OpRequest>{op("note.delete", {{"clip", 11}, {"note", 40}}, "Delete Note"),
                                                  op("note.delete", {{"clip", 11}, {"note", 41}}, "Delete Note")}).ok,
          "both removed again");
    root.frame();
    ed.select({});

    // Quantize (Ctrl/Cmd+U), Split and Chop (Ctrl/Cmd+E) and Join (Ctrl/Cmd+J).
    ed.select({});
    check(submitter.submit(op("note.insert", {{"clip", 11}, {"note", 50}, {"start", 4 * beat + beat / 8}, {"dur", 3 * beat}, {"key", 76}}, "Add Note")).ok,
          "an E4 a little after the fifth beat, three beats long");
    root.frame();
    ed.select({50});
    check(ed.keyPressed(juce::KeyPress('u', juce::ModifierKeys::commandModifier, 0)) && note(50).startTicks == 4 * beat &&
              note(50).durationTicks == 3 * beat && undoIs("Quantize Notes", 1),
          "Ctrl/Cmd+U puts its start on the grid and keeps its length, one undo step");
    check(ed.keyPressed(juce::KeyPress('e', juce::ModifierKeys::commandModifier, 0)) && ed.selected().size() == 3 &&
              note(50).durationTicks == beat && undoIs("Chop Notes", 3),
          "Ctrl/Cmd+E with the note selected chops it on the grid: three notes of a beat, selected, one undo step");
    check(ed.keyPressed(juce::KeyPress('j', juce::ModifierKeys::commandModifier, 0)) && ed.selected() == std::vector<std::uint64_t>{50} &&
              note(50).durationTicks == 3 * beat && undoIs("Join Notes", 3),
          "Ctrl/Cmd+J joins them into one again");
    const juce::Point<float> cutAt(xAt(5 * beat) + 4.f, static_cast<float>(area.getY() + 3));
    ed.mouseDown(pressAt(ed, cutAt));
    ed.mouseUp(pressAt(ed, cutAt));
    const auto countBeforeSplit = panel.details()->notes.size();
    check(ed.insertMarker() == 5 * beat && ed.keyPressed(juce::KeyPress('e', juce::ModifierKeys::commandModifier, 0)) &&
              note(50).durationTicks == beat && panel.details()->notes.size() > countBeforeSplit &&
              submitter.nextUndo()->label == "Split Notes",
          "with no note selected, Ctrl/Cmd+E splits every note under the insert marker there");
    check(root.command(AppCommands::Undo), "undo the split");
    root.frame();
    check(submitter.submit(op("note.delete", {{"clip", 11}, {"note", 50}}, "Delete Note")).ok, "the E4 removed");
    root.frame();
    ed.select({});

    // Cut, Copy and Paste of notes, and the insert marker (manual 10.5.2, 10.5.4).
    ed.select({});
    const auto clipCount = panel.details()->notes.size();
    check(panel.noteClipboard.empty() && ed.keyPressed(juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0)) &&
              panel.details()->notes.size() == clipCount,
          "Paste with nothing copied is the editor's and does nothing");
    ed.select({1, 2});
    check(ed.keyPressed(juce::KeyPress('c', juce::ModifierKeys::commandModifier, 0)) && panel.noteClipboard.size() == 2 &&
              panel.noteClipboard[0].startTicks == 0 && panel.noteClipboard[1].startTicks == note(2).startTicks - note(1).startTicks,
          "Copy keeps the two selected notes, each counted from the first of them");
    const juce::Point<float> marker(xAt(6 * beat) + 4.f, static_cast<float>(area.getY() + 3));
    ed.mouseDown(pressAt(ed, marker));
    ed.mouseUp(pressAt(ed, marker));
    check(ed.insertMarker() == 6 * beat && ed.selected().empty(), "a click on empty space puts the insert marker there, on the grid");
    check(ed.keyPressed(juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0)) && panel.details()->notes.size() == clipCount + 2 &&
              ed.selected().size() == 2 && undoIs("Paste Notes", 2),
          "Paste puts them in, selected, one undo step");
    bool landed = true;
    std::int64_t firstPasted = 0;
    for (const auto id : ed.selected())
        firstPasted = firstPasted == 0 ? note(id).startTicks : std::min(firstPasted, note(id).startTicks);
    for (const auto id : ed.selected())
        landed = landed && (note(id).startTicks == 6 * beat || note(id).startTicks == 6 * beat + panel.noteClipboard[1].startTicks);
    check(landed && firstPasted == 6 * beat, "the earliest pasted note is on the insert marker, the other as far after it as before");
    const auto pasted = ed.selected();
    check(ed.keyPressed(juce::KeyPress('x', juce::ModifierKeys::commandModifier, 0)) && panel.details()->notes.size() == clipCount &&
              undoIs("Cut Notes", 2) && panel.noteClipboard.size() == 2 && ed.selected().empty(),
          "Cut takes the selected notes out and keeps them for a paste, one undo step");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "the cut and the paste undone");
    root.frame();
    check(panel.details()->notes.size() == clipCount, "and the clip is as it was");
    ed.select({});

    // The Velocity lane's line tools (ADR-0241 d2, from Cubase): with Line chosen, a stroke across the lane
    // gives every note under it the line's value there; shown while held, one undo step.
    check(ed.laneShape() == LaneShape::Pointer && ed.shapeMenu().getNumItems() == 7 && !ed.shapeBox().isEmpty(),
          "the lane's chooser offers Pointer and six line tools");
    check(ed.chooseShape(2) && ed.laneShape() == LaneShape::Line && !ed.chooseShape(0) && !ed.chooseShape(8), "Line is chosen");
    const auto vLane = ed.velocityArea().withTrimmedTop(1);
    const double vTop = vLane.getY() + 5, vHeight = vLane.getHeight() - 10;
    const int yHigh = static_cast<int>(velocityY(120, vTop, vHeight)), yLow = static_cast<int>(velocityY(20, vTop, vHeight));
    const juce::Point<float> strokeFrom(static_cast<float>(vLane.getX() + static_cast<int>(ed.time.x(0))), static_cast<float>(yHigh));
    const juce::Point<float> strokeTo(static_cast<float>(vLane.getX() + static_cast<int>(ed.time.x(6 * beat)) + 1), static_cast<float>(yLow));
    std::size_t under = 0;
    for (const auto &n : panel.details()->notes)
        if (n.startTicks >= 0 && n.startTicks <= 6 * beat)
            ++under;
    const auto beforeStroke = submitter.nextUndo()->txnId;
    ed.mouseDown(pressAt(ed, strokeFrom));
    ed.mouseDrag(dragAt(ed, strokeFrom, strokeTo));
    check(ed.shapedVelocities().size() == under && under >= 6 && submitter.nextUndo()->txnId == beforeStroke,
          "while the stroke is held every note under it has a value to come, and nothing is written");
    const auto preview = ed.shapedVelocities();
    ed.mouseUp(pressAt(ed, strokeTo));
    bool matches = true, falls = true;
    int firstValue = 0, lastValue = 0;
    for (const auto &[id, value] : preview) {
        matches = matches && note(id).velocity == value;
        if (note(id).startTicks == 0)
            firstValue = value;
        if (note(id).startTicks == 6 * beat)
            lastValue = value;
        for (const auto &[other, otherValue] : preview)
            if (note(other).startTicks > note(id).startTicks)
                falls = falls && otherValue <= value;
    }
    check(matches && falls && firstValue == velocityAt(yHigh, vTop, vHeight) && lastValue >= velocityAt(yLow, vTop, vHeight) &&
              lastValue <= velocityAt(yLow, vTop, vHeight) + 2 && submitter.nextUndo()->label == "Draw Velocities" &&
              submitter.nextUndo()->txnId != beforeStroke,
          "let go, the velocities fall along the line from about 120 to about 20, one undo step");
    check(root.command(AppCommands::Undo), "undo the stroke");
    root.frame();
    check(ed.chooseShape(6), "Square is chosen");
    ed.mouseDown(pressAt(ed, {strokeFrom.x, static_cast<float>(static_cast<int>(velocityY(64, vTop, vHeight)))}));
    ed.mouseDrag(dragAt(ed, strokeFrom, {strokeTo.x, static_cast<float>(static_cast<int>(velocityY(100, vTop, vHeight)))}));
    std::vector<int> levels;
    for (const auto &[id, value] : ed.shapedVelocities())
        if (std::find(levels.begin(), levels.end(), value) == levels.end())
            levels.push_back(value);
    ed.mouseUp(pressAt(ed, {strokeTo.x, static_cast<float>(static_cast<int>(velocityY(100, vTop, vHeight)))}));
    check(levels.size() == 2 && submitter.nextUndo()->label == "Draw Velocities", "a Square stroke leaves two levels of velocity");
    check(root.command(AppCommands::Undo) && ed.chooseShape(1) && ed.laneShape() == LaneShape::Pointer, "undone; Pointer is Live's lane again");
    root.frame();

    // The trim tool (ADR-0241 d2, from Cubase): the header's switch; a stroke down across a note cuts it there, on the grid.
    ed.select({1});
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0)) &&
              ed.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0)) && note(1).durationTicks == 3 * beat,
          "the first C3 is made three beats long");
    panel.mouseDown(pressAt(panel, panel.trimBox().getCentre().toFloat()));
    panel.mouseUp(pressAt(panel, panel.trimBox().getCentre().toFloat()));
    check(ed.trimMode() && !ed.drawMode(), "the header's Trim switch turns the trim tool on");
    const juce::Point<float> cutFrom(xAt(2 * beat) + 3.f, yOf(60) - 3.f), cutTo(xAt(2 * beat) + 3.f, yOf(60) + 3.f);
    ed.mouseDown(pressAt(ed, cutFrom));
    ed.mouseDrag(dragAt(ed, cutFrom, cutTo));
    check(ed.trimmedLengths() == std::vector<std::pair<std::uint64_t, std::int64_t>>{{1, 2 * beat}} && note(1).durationTicks == 3 * beat &&
              ed.shown(note(1)).durationTicks == 2 * beat,
          "a stroke across it just past the third beat shows it cut on the grid, two beats long; nothing written yet");
    ed.mouseUp(pressAt(ed, cutTo));
    check(note(1).durationTicks == 2 * beat && undoIs("Trim Note", 1), "let go, it is two beats long: one undo step");
    panel.mouseDown(pressAt(panel, panel.trimBox().getCentre().toFloat()));
    panel.mouseUp(pressAt(panel, panel.trimBox().getCentre().toFloat()));
    check(!ed.trimMode(), "and the switch turns the tool off");

    // The handles on a selection of velocities (ADR-0241 d2, from Cubase): tilt, scale, compress.
    ed.select({1});
    check(ed.handleBox(LaneHandle::Scale).isEmpty() && ed.handleAt({0, 0}) == LaneHandle::None, "one selected note shows no handles");
    ed.select({1, 2});
    const auto tilt = ed.handleBox(LaneHandle::TiltRight);
    check(!tilt.isEmpty() && !ed.handleBox(LaneHandle::TiltLeft).isEmpty() && !ed.handleBox(LaneHandle::Scale).isEmpty() &&
              !ed.handleBox(LaneHandle::Compress).isEmpty() && ed.handleAt(tilt.getCentre()) == LaneHandle::TiltRight,
          "two selected notes show the four handles over their velocities");
    const auto hLane = ed.velocityArea().withTrimmedTop(1);
    const double hHeight = hLane.getHeight() - 10;
    const int v1 = note(1).velocity, v2 = note(2).velocity;
    const juce::Point<float> grabTilt = tilt.getCentre().toFloat();
    ed.mouseDown(pressAt(ed, grabTilt));
    ed.mouseDrag(dragAt(ed, grabTilt, {grabTilt.x, grabTilt.y - 8.f}));
    const int raised = std::clamp(static_cast<int>(std::lround(v2 + 8. * 127. / hHeight)), 1, 127);
    check(ed.shown(note(1)).velocity == v1 && ed.shown(note(2)).velocity == raised && note(2).velocity == v2,
          "the right tilt handle dragged up raises the later note and leaves the earlier, shown while held");
    ed.mouseUp(pressAt(ed, {grabTilt.x, grabTilt.y - 8.f}));
    check(note(1).velocity == v1 && note(2).velocity == raised && undoIs("Tilt Velocities", 1) && ed.selected().size() == 2,
          "let go, one undo step, and both stay selected");
    const juce::Point<float> grabScale = ed.handleBox(LaneHandle::Scale).getCentre().toFloat();
    const int s1 = note(1).velocity, s2 = note(2).velocity;
    ed.mouseDown(pressAt(ed, grabScale));
    ed.mouseDrag(dragAt(ed, grabScale, {grabScale.x, grabScale.y + 12.f}));
    ed.mouseUp(pressAt(ed, {grabScale.x, grabScale.y + 12.f}));
    check(note(1).velocity < s1 && note(2).velocity < s2 && std::abs(note(1).velocity * s2 - note(2).velocity * s1) <= s1 + s2 &&
              undoIs("Scale Velocities", 2),
          "the Scale handle dragged down lowers both in proportion, one undo step");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "both undone");
    root.frame();
    ed.select({});

    // The playhead: hidden while stopped; while the song plays, a line where the song is.
    check(!ed.playhead.isVisible(), "stopped, the editor shows no playhead");
    engine::Transport driver;
    driver.play(true);
    mailbox.publish(driver);
    root.frame();
    check(root.playing() && ed.playhead.isVisible() &&
              ed.playhead.getX() == ed.notesArea().getX() + static_cast<int>(ed.time.x(root.timelineTick())) &&
              ed.playhead.getWidth() == 1 && ed.playhead.getBottom() == ed.getHeight(),
          "playing, a one-pixel playhead stands where the song is, over the key tracks and the Velocity lane");
    driver.play(false);
    mailbox.publish(driver);
    root.frame();
    check(!ed.playhead.isVisible(), "and it goes when the song stops");
}
// Multi-clip editing (Live 12 manual 10.8; ADR-0242 d5): several selected MIDI clips are shown in one
// Note Editor, each clip's notes in its own colour, a loop bar for each above the ruler; a click on
// another clip's note or bar makes it the clip being edited; Focus (N) greys the others.
// The Note Editor's time selection, the insert marker's keys and the note-selection keys (manual 10.5.2).
void clipViewTime(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "clip-view-time.adi", err);
    check(store != nullptr, "the time selection project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clip View Time')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole, beat = textproj::kPPQ;
    auto op = [](const char *type, Payload payload, const char *label) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(payload);
        r.label = label;
        return r;
    };
    bool made = submitter.submit(op("track.create", {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}, {"index", 0}}, "Create track")).ok;
    made = submitter.submit(op("clip.create", {{"id", 11}, {"track", 1}, {"kind", "midi"}, {"name", "Tune"}, {"pos", bar}, {"length", 2 * bar}}, "Create clip")).ok && made;
    // In the song: C3 from beat 4 to 5, E3 from 5 to 6, a long C3 from 6 to 8 and a G3 from 6 to 7.
    made = submitter.submit(std::vector<OpRequest>{
               op("note.insert", {{"clip", 11}, {"note", 1}, {"start", 0}, {"dur", beat}, {"key", 60}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 2}, {"start", beat}, {"dur", beat}, {"key", 64}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 3}, {"start", 2 * beat}, {"dur", 2 * beat}, {"key", 60}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 4}, {"start", 2 * beat}, {"dur", beat}, {"key", 67}}, "Add Note")}).ok && made;
    check(made, "a MIDI clip of four notes");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "clip-view-time");
    commands.activate(root);
    root.setSize(1400, 900);
    root.arrangement.geometry.resizeTrack(1, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    auto &panel = root.clipView;
    auto &ed = panel.notes;
    canvas.mouseDoubleClick(pressAt(canvas, {static_cast<float>(geo.x(bar)) + 20.f, static_cast<float>(laneTop(root, 1) + 30)}));
    root.frame();
    check(panel.details() != nullptr && panel.details()->notes.size() == 4, "the clip is open in the Note Editor");
    if (panel.details() == nullptr)
        return;
    const auto area = ed.notesArea();
    const auto g = ed.gridTicks();
    const auto xAt = [&](std::int64_t tick) { return static_cast<float>(area.getX()) + static_cast<float>(ed.time.x(tick)); };
    const auto note = [&](std::uint64_t id) {
        for (const auto &n : panel.details()->notes)
            if (n.id == id)
                return n;
        return engine::ClipNote{};
    };
    using Ids = std::vector<std::uint64_t>;
    using Span = std::optional<std::pair<std::int64_t, std::int64_t>>;
    const auto key = [](int code, int mods = 0) { return juce::KeyPress(code, juce::ModifierKeys(mods), 0); };
    const int shift = juce::ModifierKeys::shiftModifier, alt = juce::ModifierKeys::altModifier, command = juce::ModifierKeys::commandModifier;
    const int left = juce::KeyPress::leftKey, right = juce::KeyPress::rightKey, up = juce::KeyPress::upKey, down = juce::KeyPress::downKey;
    check(g > 0 && beat % g == 0, "the editing grid divides the beat");

    // The insert marker and the arrows.
    const juce::Point<float> empty(xAt(6 * beat) + 4.f, static_cast<float>(area.getY() + 3));
    ed.mouseDown(pressAt(ed, empty));
    ed.mouseUp(pressAt(ed, empty));
    check(ed.insertMarker() == 6 * beat && !ed.timeSelection(), "a click on empty space places the insert marker; no time is selected");
    check(ed.keyPressed(key(right)) && ed.insertMarker() == 6 * beat + g, "Right moves the insert marker a grid step on");
    check(ed.keyPressed(key(left)) && ed.keyPressed(key(left)) && ed.insertMarker() == 6 * beat - g, "and Left, back");
    const auto beforeKeys = submitter.nextUndo()->txnId;
    check(ed.keyPressed(key(right, shift)) && ed.keyPressed(key(right, shift)) && ed.timeSelection() == Span({6 * beat - g, 6 * beat + g}) &&
              ed.insertMarker() == 6 * beat - g,
          "Shift and Right select time from the insert marker, a grid step each");
    check(ed.keyPressed(key(left, shift)) && ed.timeSelection() == Span({6 * beat - g, 6 * beat}), "Shift and Left narrow it");
    check(ed.keyPressed(key(left, shift)) && !ed.timeSelection() && ed.keyPressed(key(left, shift)) &&
              ed.timeSelection() == Span({6 * beat - 2 * g, 6 * beat - g}),
          "narrowed to nothing it is a point again, and one more step selects the time before the marker");
    check(ed.keyPressed(key(right)) && !ed.timeSelection() && ed.insertMarker() == 6 * beat - g,
          "Right alone collapses the time selection to its end");
    ed.selectTime(5 * beat, 6 * beat);
    check(ed.keyPressed(key(left)) && !ed.timeSelection() && ed.insertMarker() == 5 * beat, "and Left alone, to its start");
    check(ed.keyPressed(key(juce::KeyPress::homeKey)) && ed.insertMarker() == bar && ed.keyPressed(key(left)) && ed.insertMarker() == bar,
          "Home takes the marker to the clip's start, and Left goes no further");
    check(ed.keyPressed(key(juce::KeyPress::endKey)) && ed.insertMarker() == 3 * bar && ed.keyPressed(key(right)) && ed.insertMarker() == 3 * bar,
          "End takes it to the clip's end, and Right goes no further");
    check(submitter.nextUndo()->txnId == beforeKeys, "none of it is an undo step");

    // A drag across empty space selects the time it spans and the notes it touches.
    const juce::Point<float> from(xAt(4 * beat) + 2.f, static_cast<float>(area.getY() + 2));
    const juce::Point<float> to(xAt(6 * beat) - 2.f, static_cast<float>(area.getBottom() - 2));
    ed.mouseDown(pressAt(ed, from));
    ed.mouseDrag(dragAt(ed, from, to));
    ed.mouseUp(dragAt(ed, from, to));
    check(ed.timeSelection() == Span({4 * beat, 6 * beat}) && ed.selected() == Ids({1, 2}),
          "a drag from beat 4 to beat 6 selects that time, on the grid, and the two notes inside it");
    check(ed.keyPressed(key(juce::KeyPress::returnKey)) && ed.selected().empty() && ed.timeSelection() == Span({4 * beat, 6 * beat}),
          "Enter leaves the time selected and lets the notes go");
    check(ed.keyPressed(key(juce::KeyPress::returnKey)) && ed.selected() == Ids({1, 2}) && ed.timeSelection() == Span({4 * beat, 6 * beat}),
          "Enter again selects the notes that begin inside it");
    check(ed.keyPressed(key(juce::KeyPress::escapeKey)) && ed.selected().empty() && !ed.timeSelection() && ed.insertMarker() == 4 * beat,
          "Esc collapses both; the insert marker stays at the start");
    check(!ed.keyPressed(key(juce::KeyPress::returnKey)), "with neither notes nor time selected, Enter is not the editor's");
    ed.select({3});
    check(ed.keyPressed(key(juce::KeyPress::returnKey)) && ed.selected().empty() && ed.timeSelection() == Span({6 * beat, 8 * beat}),
          "with a note selected and no time, Enter selects the time the note occupies");
    // A click on a note collapses the time selection.
    if (const auto box = ed.boxOf(note(2))) {
        const juce::Point<float> on(static_cast<float>(box->x + box->width / 2), static_cast<float>(box->y + box->height / 2));
        ed.mouseDown(pressAt(ed, on));
        ed.mouseUp(pressAt(ed, on));
    }
    check(ed.selected() == Ids({2}) && !ed.timeSelection(), "a click on a note selects it and collapses the time selection");

    // Split at the time selection's edges.
    ed.select({});
    ed.selectTime(4 * beat + beat / 2, 7 * beat);
    check(ed.keyPressed(key('e', command)) && panel.details()->notes.size() == 6 && note(1).durationTicks == beat / 2 &&
              note(3).durationTicks == beat && note(4).durationTicks == beat && note(2).durationTicks == beat &&
              submitter.nextUndo()->label == "Split Notes" && submitter.nextUndo()->opCount == 4,
          "Ctrl+E with time selected and no note cuts the notes crossing its two edges: one undo step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(panel.details()->notes.size() == 4 && note(3).durationTicks == 2 * beat, "and Undo joins them again");

    // Paste lands on the time selection's start, whichever way it was drawn.
    ed.select({2});
    check(ed.keyPressed(key('c', command)), "E3 copied");
    ed.select({});
    ed.selectTime(7 * beat, 6 * beat);
    check(ed.timeSelection() == Span({6 * beat, 7 * beat}) && ed.keyPressed(key('v', command)) && ed.selected().size() == 1 &&
              note(ed.selected().front()).startTicks == 2 * beat && note(ed.selected().front()).key == 64,
          "Paste puts it at the time selection's start");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();

    // Z zooms to the time selection; X goes back.
    ed.select({});
    ed.selectTime(5 * beat, 6 * beat);
    const double scaleBefore = ed.time.scale;
    check(ed.keyPressed(key('z')) && std::abs(ed.time.x(5 * beat)) < 1.0 && ed.time.x(6 * beat) > area.getWidth() * 0.9 &&
              ed.time.x(6 * beat) <= area.getWidth(),
          "Z fills the editor with the selected time");
    check(ed.keyPressed(key('x')) && std::abs(ed.time.scale - scaleBefore) < 1e-9, "and X goes back");

    // Alt and the arrows walk the notes.
    check(ed.keyPressed(key(juce::KeyPress::escapeKey)) || true, "nothing selected");
    ed.keyPressed(key(juce::KeyPress::homeKey));
    check(ed.keyPressed(key(down, alt)) && ed.selected() == Ids({1}), "Alt and Down with no note selected takes the note nearest the insert marker");
    check(ed.keyPressed(key(down, alt)) && ed.selected() == Ids({2}) && ed.keyPressed(key(down, alt)) && ed.selected() == Ids({3}) &&
              ed.keyPressed(key(down, alt)) && ed.selected() == Ids({4}),
          "again, the next note in time; of two that begin together, the lower first");
    check(ed.keyPressed(key(down, alt)) && ed.selected() == Ids({4}), "at the last note it stays");
    check(ed.keyPressed(key(up, alt)) && ed.selected() == Ids({3}), "Alt and Up, the previous note");
    check(ed.keyPressed(key(up, alt | shift)) && ed.selected() == Ids({3, 2}), "with Shift, the previous note joins the selection");
    ed.select({3});
    check(ed.keyPressed(key(left, alt)) && ed.selected() == Ids({1}) && ed.keyPressed(key(right, alt)) && ed.selected() == Ids({3}) &&
              ed.keyPressed(key(right, alt)) && ed.selected() == Ids({3}),
          "Alt and Left or Right go to the previous and next note on the same key");
    check(submitter.nextUndo()->txnId == beforeKeys, "walking the notes moves none of them");
    // With no note selected, Alt and Left or Right take the insert marker to the next note edge.
    ed.select({});
    ed.keyPressed(key(juce::KeyPress::homeKey));
    check(ed.keyPressed(key(right, alt)) && ed.insertMarker() == 5 * beat && ed.keyPressed(key(right, alt)) && ed.insertMarker() == 6 * beat &&
              ed.keyPressed(key(right, alt)) && ed.insertMarker() == 7 * beat && ed.keyPressed(key(right, alt)) && ed.insertMarker() == 8 * beat &&
              ed.keyPressed(key(right, alt)) && ed.insertMarker() == 8 * beat,
          "Alt and Right: the insert marker goes from note edge to note edge, and stops at the last");
    check(ed.keyPressed(key(left, alt | shift)) && ed.timeSelection() == Span({7 * beat, 8 * beat}),
          "with Shift, the time selection reaches to the previous note edge");

    // Invert Selection.
    ed.keyPressed(key(juce::KeyPress::escapeKey));
    ed.select({1});
    check(ed.keyPressed(key('a', command | shift)) && ed.selected() == Ids({2, 3, 4}), "Ctrl+Shift+A selects every other note instead");

    // The piano's keys select their key tracks.
    const auto pianoKey = [&](int k) {
        return juce::Point<float>(5.f, static_cast<float>(ed.keysArea().getY() + ed.keys.y(k).value_or(0) + 2));
    };
    ed.mouseDown(pressAt(ed, pianoKey(60)));
    ed.mouseUp(pressAt(ed, pianoKey(60)));
    check(ed.selected() == Ids({1, 3}), "a click on the piano's C3 selects the notes on that key");
    ed.mouseDown(pressAt(ed, pianoKey(64), juce::ModifierKeys(shift)));
    ed.mouseUp(pressAt(ed, pianoKey(64), juce::ModifierKeys(shift)));
    check(ed.selected() == Ids({1, 3, 2}), "Shift and a click on E3 adds that key's notes");
    ed.mouseDown(pressAt(ed, pianoKey(60), juce::ModifierKeys(shift)));
    ed.mouseUp(pressAt(ed, pianoKey(60), juce::ModifierKeys(shift)));
    check(ed.selected() == Ids({2}), "Shift and a click on a key whose notes are all selected takes them out");
    ed.mouseDown(pressAt(ed, pianoKey(61)));
    ed.mouseUp(pressAt(ed, pianoKey(61)));
    check(ed.selected().empty() && submitter.nextUndo()->txnId == beforeKeys, "a click on a key with no notes selects none; no selecting is an undo step");

    // With a note selected the arrows still move it.
    ed.select({2});
    check(ed.keyPressed(key(right)) && note(2).startTicks == beat + g && submitter.nextUndo()->label == "Move Note",
          "with a note selected, Right still moves the note, not the marker");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();

    // The ruler's handles (manual 8.8, 8.9): the loop brace along its top, the Start and End markers along its foot.
    {
        using Part = NoteEditor::RulerPart;
        const auto details = [&]() -> const engine::ClipDetails & { return *panel.details(); };
        const auto ruler = ed.rulerArea();
        const float top = static_cast<float>(ruler.getY() + 3), foot = static_cast<float>(ruler.getBottom() - 3);
        const auto part = [&](float x, float y) { return ed.rulerPartAt(juce::Point<float>(x, y).roundToInt()); };
        const auto drag = [&](juce::Point<float> a, juce::Point<float> b, bool release = true) {
            ed.mouseDown(pressAt(ed, a));
            ed.mouseDrag(dragAt(ed, a, b));
            if (release)
                ed.mouseUp(dragAt(ed, a, b));
        };
        const auto undoIs = [&](const char *label, int ops) {
            return submitter.nextUndo()->label == label && submitter.nextUndo()->opCount == ops;
        };
        // The clip never looped: its loop is its whole length, from beat 4 to beat 12 of the song.
        check(!details().loopEnabled && loopLengthOf(details()) == 8 * beat && details().startMarkerTicks == 0 && endMarkerOf(details()) == 8 * beat,
              "the clip's loop and its markers span its eight beats");
        check(part(xAt(4 * beat), top) == Part::LoopStart && part(xAt(12 * beat), top) == Part::LoopEnd && part(xAt(8 * beat), top) == Part::Loop &&
                  part(xAt(4 * beat) + 1.f, foot) == Part::StartMarker && part(xAt(12 * beat) - 1.f, foot) == Part::EndMarker &&
                  part(xAt(8 * beat), foot) == Part::None && ed.rulerSelection() == Part::None,
              "the brace's ends and its bar are in the ruler's upper half, the two markers in its lower");
        const auto marker = ed.insertMarker();
        // The brace's end, dragged.
        drag({xAt(12 * beat), top}, {xAt(8 * beat) + 2.f, top}, false);
        check(ed.loopBrace() && std::abs(ed.loopBrace()->getEnd() - static_cast<int>(xAt(8 * beat))) <= 1 && loopLengthOf(details()) == 8 * beat,
              "while its end is held the brace follows the pointer, on the grid, and nothing is written");
        ed.mouseUp(dragAt(ed, {xAt(12 * beat), top}, {xAt(8 * beat) + 2.f, top}));
        check(details().loopLengthTicks == 4 * beat && details().loopStartTicks == 0 && !details().loopEnabled && undoIs("Set Loop Length", 1) &&
                  ed.rulerSelection() == Part::Loop,
              "released, the loop is four beats long: one undo step; the switch stays off and the brace is selected");
        // The bar, dragged, moves it; its start, dragged, keeps its end.
        drag({xAt(6 * beat), top}, {xAt(7 * beat), top});
        check(details().loopStartTicks == beat && details().loopLengthTicks == 4 * beat && undoIs("Set Loop Position", 1),
              "the brace dragged by its bar moves a beat on, its length kept");
        drag({xAt(5 * beat), top}, {xAt(6 * beat), top});
        check(details().loopStartTicks == 2 * beat && details().loopLengthTicks == 3 * beat && undoIs("Set Loop Position", 1),
              "its start dragged a beat on leaves its end where it was");
        // The selected brace's keys, as the Arrangement's.
        check(ed.keyPressed(key(right)) && details().loopStartTicks == 2 * beat + g && details().loopLengthTicks == 3 * beat &&
                  ed.keyPressed(key(left)) && details().loopStartTicks == 2 * beat,
              "Right and Left nudge the selected brace by the grid");
        check(ed.keyPressed(key(up)) && details().loopStartTicks == 5 * beat && ed.keyPressed(key(down)) && details().loopStartTicks == 2 * beat,
              "Up and Down move it by its length");
        check(ed.keyPressed(key(right, command)) && details().loopLengthTicks == 3 * beat + g && ed.keyPressed(key(left, command)) &&
                  details().loopLengthTicks == 3 * beat && details().loopStartTicks == 2 * beat,
              "Ctrl and Right or Left lengthen and shorten it by the grid");
        check(ed.keyPressed(key(up, command)) && details().loopLengthTicks == 6 * beat && ed.keyPressed(key(down, command)) &&
                  details().loopLengthTicks == 3 * beat && undoIs("Set Loop Length", 1),
              "Ctrl and Up or Down double and halve it");
        check(ed.insertMarker() == marker && note(2).startTicks == beat, "the arrows were the brace's: no note and no insert marker moved");
        check(ed.keyPressed(key(juce::KeyPress::escapeKey)) && ed.rulerSelection() == Part::None, "Esc lets the brace go");

        // The End marker, dragged and by keys.
        drag({xAt(12 * beat) - 1.f, foot}, {xAt(10 * beat), foot}, false);
        check(std::abs(ed.markerX(true) - static_cast<int>(xAt(10 * beat))) <= 1 && endMarkerOf(details()) == 8 * beat,
              "while the End marker is held it follows the pointer");
        ed.mouseUp(dragAt(ed, {xAt(12 * beat) - 1.f, foot}, {xAt(10 * beat), foot}));
        check(endMarkerOf(details()) == 6 * beat && undoIs("Set Clip End", 1) && ed.rulerSelection() == Part::EndMarker,
              "released at beat 10 of the song, the clip's End is its sixth beat: one undo step, the marker selected");
        check(ed.keyPressed(key(right)) && endMarkerOf(details()) == 6 * beat + g && ed.keyPressed(key(left)) && endMarkerOf(details()) == 6 * beat,
              "Right and Left move the selected marker by the grid");
        check(ed.keyPressed(key(right, alt)) && details().startMarkerTicks == g && endMarkerOf(details()) == 6 * beat + g &&
                  undoIs("Move Clip Region", 2),
              "with Alt both markers move together: one undo step");
        check(ed.keyPressed(key(left, alt)) && details().startMarkerTicks == 0 && endMarkerOf(details()) == 6 * beat, "and back");
        // The Start marker.
        drag({xAt(4 * beat) + 1.f, foot}, {xAt(5 * beat), foot});
        check(details().startMarkerTicks == beat && undoIs("Set Clip Start", 1) && ed.rulerSelection() == Part::StartMarker,
              "the Start marker dragged a beat on: the clip starts a beat into its notes");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        check(details().startMarkerTicks == 0, "and Undo puts it back");
        const auto beforeRefusal = submitter.nextUndo()->txnId;
        ed.mouseDown(pressAt(ed, {xAt(4 * beat) + 1.f, foot}));
        ed.mouseUp(pressAt(ed, {xAt(4 * beat) + 1.f, foot}));
        check(ed.rulerSelection() == Part::StartMarker && submitter.nextUndo()->txnId == beforeRefusal, "a click on a marker selects it and writes nothing");
        check(ed.keyPressed(key(left)) && details().startMarkerTicks == 0 && !root.error().empty() && submitter.nextUndo()->txnId == beforeRefusal,
              "Left at the clip's beginning is refused, with the reason");
        const juce::Point<float> away(xAt(9 * beat) + 4.f, static_cast<float>(area.getY() + 3));
        ed.mouseDown(pressAt(ed, away));
        ed.mouseUp(pressAt(ed, away));
        check(ed.rulerSelection() == Part::None && ed.insertMarker() == 9 * beat, "a click in the notes lets the marker go");
    }

    // The Velocity lane as Live's (manual 10.5.12).
    {
        const auto lane = ed.velocityArea().withTrimmedTop(1);
        const double vTop = lane.getY() + 5, vHeight = lane.getHeight() - 10;
        const auto yOf = [&](int velocity) { return static_cast<float>(static_cast<int>(velocityY(velocity, vTop, vHeight))); };
        const auto mark = [&](std::int64_t tick, int velocity) {
            return juce::Point<float>(static_cast<float>(lane.getX() + static_cast<int>(ed.time.x(tick))), yOf(velocity));
        };
        const auto undoIs = [&](const char *label, int ops) {
            return submitter.nextUndo()->label == label && submitter.nextUndo()->opCount == ops;
        };
#if JUCE_MAC
        const juce::ModifierKeys bypass(command);
#else
        const juce::ModifierKeys bypass(alt);
#endif
        check(!lane.isEmpty() && note(1).velocity == 100, "the lane is shown; the notes are at 100");
        check(submitter.submit(op("note.setVelocity", {{"clip", 11}, {"note", 2}, {"vel", 40}}, "Change Velocity")).ok, "E3 set to 40");
        root.frame();
        ed.select({});
        check(ed.velocityReadout().isEmpty(), "with no note selected the lane's header reads nothing");
        ed.mouseDown(pressAt(ed, mark(4 * beat, 100)));
        ed.mouseUp(pressAt(ed, mark(4 * beat, 100)));
        check(ed.selected() == Ids({1}) && ed.velocityReadout() == "100", "a click on a marker selects its note; the header reads its velocity");
        ed.mouseDown(pressAt(ed, mark(5 * beat, 40), juce::ModifierKeys(shift)));
        ed.mouseUp(pressAt(ed, mark(5 * beat, 40), juce::ModifierKeys(shift)));
        check(ed.selected() == Ids({1, 2}) && ed.velocityReadout() == "40 - 100", "Shift and a click adds a marker; the header reads the range");
        ed.mouseDown(pressAt(ed, mark(5 * beat, 40), juce::ModifierKeys(shift)));
        ed.mouseUp(pressAt(ed, mark(5 * beat, 40), juce::ModifierKeys(shift)));
        check(ed.selected() == Ids({1}), "and again takes it out");
        ed.select({1, 2});
        // The lower marker dragged to the top takes the higher one there first, where it waits; back down, they are one value.
        const auto grab = mark(5 * beat, 40);
        const juce::Point<float> top(grab.x, yOf(127)), middle(grab.x, yOf(64));
        const auto before = submitter.nextUndo()->txnId;
        ed.mouseDown(pressAt(ed, grab));
        ed.mouseDrag(dragAt(ed, grab, top));
        check(ed.velocityReadout() == "127" && note(1).velocity == 100 && submitter.nextUndo()->txnId == before,
              "the lower marker dragged to the top: both read 127 while held, and nothing is written");
        ed.mouseDrag(dragAt(ed, grab, middle));
        ed.mouseUp(dragAt(ed, grab, middle));
        const int level = velocityAt(middle.y, vTop, vHeight);
        check(note(1).velocity == level && note(2).velocity == level && level > 55 && level < 75 && undoIs("Change Velocity", 2) &&
                  ed.selected() == Ids({1, 2}),
              "brought back down, both have the one value under the pointer: one undo step");
        // Typed.
        check(ed.keyPressed(key('9')) && ed.keyPressed(key('0')) && ed.velocityReadout() == "90" && note(1).velocity == level,
              "digits typed with notes selected show in the header, and nothing is written yet");
        check(ed.keyPressed(key(juce::KeyPress::returnKey)) && note(1).velocity == 90 && note(2).velocity == 90 && undoIs("Set Velocity", 2) &&
                  ed.velocityReadout() == "90" && ed.selected() == Ids({1, 2}) && !ed.timeSelection(),
              "Enter sets both notes to 90: one undo step; Enter did not swap to a time selection");
        check(ed.keyPressed(key('2')) && ed.keyPressed(key('0')) && ed.keyPressed(key('0')) && ed.keyPressed(key(juce::KeyPress::returnKey)) &&
                  note(1).velocity == 127,
              "200 typed is 127");
        check(ed.keyPressed(key('5')) && ed.keyPressed(key('5')) && ed.keyPressed(key(juce::KeyPress::backspaceKey)) && ed.velocityReadout() == "5" &&
                  panel.details()->notes.size() == 4,
              "Backspace takes a typed digit back; it deletes no note");
        check(ed.keyPressed(key(juce::KeyPress::escapeKey)) && ed.velocityReadout() == "127" && ed.selected() == Ids({1, 2}),
              "Esc drops what was typed and keeps the selection");
        check(ed.keyPressed(key('0')) && !note(1).active && ed.velocityReadout() == "127" && ed.keyPressed(key('0')) && note(1).active,
              "a 0 typed first is still the deactivate key");
        // Ctrl/Cmd and up or down.
        check(ed.keyPressed(key(down, command)) && note(1).velocity == 117 && note(2).velocity == 117 && undoIs("Change Velocity", 2),
              "Ctrl and Down take 10 off the selected notes' velocity");
        check(ed.keyPressed(key(up, command | shift)) && note(1).velocity == 118, "with Shift, Up adds 1");
        // Alt (Cmd on the Mac) and a vertical drag on a note.
        ed.select({});
        if (const auto box = ed.boxOf(note(2))) {
            const juce::Point<float> on(static_cast<float>(box->x + box->width / 2), static_cast<float>(box->y + box->height / 2));
            const juce::Point<float> higher(on.x + 30.f, on.y - 5.f);
            ed.mouseDown(pressAt(ed, on, bypass));
            ed.mouseDrag(dragAt(ed, on, higher, bypass));
            check(ed.velocityReadout() == "123", "Alt and a drag up on a note: its velocity rises a value a pixel, shown while held");
            ed.mouseUp(dragAt(ed, on, higher, bypass));
        }
        check(note(2).velocity == 123 && note(2).startTicks == beat && note(2).key == 64 && undoIs("Change Velocity", 1) && ed.selected() == Ids({2}),
              "released: the velocity is set and the note has not moved");

        // Draw Mode in the lane (manual 10.5.12.1).
        ed.select({});
        ed.setDrawMode(true);
        const juce::Point<float> from1(mark(4 * beat, 30).x + 2.f, yOf(30)), to1(mark(6 * beat, 30).x + 2.f, yOf(30));
        ed.mouseDown(pressAt(ed, from1));
        ed.mouseDrag(dragAt(ed, from1, to1));
        ed.mouseUp(dragAt(ed, from1, to1));
        const int drawn = velocityAt(from1.y, vTop, vHeight);
        check(note(1).velocity == drawn && note(2).velocity == drawn && note(3).velocity == drawn && note(4).velocity == drawn && drawn > 25 &&
                  drawn < 35 && undoIs("Draw Velocities", 4) && ed.selected().empty(),
              "with Draw Mode on, a stroke along the lane gives every note of the grid steps it crosses its height: one undo step");
        ed.select({1});
        const juce::Point<float> from2(from1.x, yOf(80)), to2(to1.x, yOf(80));
        ed.mouseDown(pressAt(ed, from2));
        ed.mouseDrag(dragAt(ed, from2, to2));
        ed.mouseUp(dragAt(ed, from2, to2));
        check(note(1).velocity == velocityAt(from2.y, vTop, vHeight) && note(2).velocity == drawn && note(3).velocity == drawn &&
                  undoIs("Draw Velocities", 1) && ed.selected() == Ids({1}),
              "with a note selected the stroke changes only that note, and it stays selected");
        ed.select({});
        const juce::Point<float> single(mark(5 * beat, 100).x, yOf(100));
        ed.mouseDown(pressAt(ed, single, bypass));
        ed.mouseUp(pressAt(ed, single, bypass));
        check(note(2).velocity == velocityAt(single.y, vTop, vHeight) && note(3).velocity == drawn && undoIs("Draw Velocities", 1),
              "with snapping bypassed, a press sets the one marker under the pointer");
        ed.setDrawMode(false);
    }

    // Live's Velocity Controls under the lanes (manual 10.5.12): Randomize with its amount, and the Ramp.
    {
        using Field = ClipViewPanel::Field;
        const auto undoIs = [&](const char *label, int ops) {
            return submitter.nextUndo()->label == label && submitter.nextUndo()->opCount == ops;
        };
        const auto click = [&](juce::Rectangle<int> box) {
            panel.mouseDown(pressAt(panel, box.getCentre().toFloat()));
            panel.mouseUp(pressAt(panel, box.getCentre().toFloat()));
        };
        const auto strip = panel.velocityStripArea();
        check(!strip.isEmpty() && strip.getBottom() == panel.getHeight() && ed.getBottom() == strip.getY() &&
                  strip.contains(panel.randomizeBox()) && strip.contains(panel.fieldBox(Field::RandomAmount)) &&
                  strip.contains(panel.fieldBox(Field::RampStart)) && strip.contains(panel.fieldBox(Field::RampEnd)) &&
                  !panel.fieldBox(Field::RampStart).intersects(panel.fieldBox(Field::RampEnd)) &&
                  !panel.randomizeBox().intersects(panel.fieldBox(Field::RandomAmount)),
              "the velocity controls are a strip under the editor's lanes: Randomize, its amount, and the Ramp's two values");
        check(submitter.submit(std::vector<OpRequest>{op("note.setVelocity", {{"clip", 11}, {"note", 1}, {"vel", 100}}, "Change Velocity"),
                                                      op("note.setVelocity", {{"clip", 11}, {"note", 2}, {"vel", 100}}, "Change Velocity"),
                                                      op("note.setVelocity", {{"clip", 11}, {"note", 3}, {"vel", 100}}, "Change Velocity"),
                                                      op("note.setVelocity", {{"clip", 11}, {"note", 4}, {"vel", 100}}, "Change Velocity")}).ok,
              "every note back at 100");
        root.frame();
        // Randomize, with the randomness a test's own: up by the amount, then down, in turn.
        int turn = 0;
        panel.randomOffset = [&turn](int amount) { return ++turn % 2 == 1 ? amount : -amount; };
        ed.select({1, 2});
        click(panel.randomizeBox());
        check(note(1).velocity == 120 && note(2).velocity == 80 && note(3).velocity == 100 && undoIs("Randomize Velocities", 2) &&
                  ed.selected() == Ids({1, 2}),
              "Randomize moves each selected note's velocity by up to the amount, 20: one undo step");
        check(panel.typeField(Field::RandomAmount, "5") && panel.randomAmount == 5 && note(1).velocity == 125 && note(2).velocity == 75 &&
                  undoIs("Randomize Velocities", 2),
              "the amount typed, with Enter, randomizes again by the new amount");
        panel.randomOffset = nullptr;
        ed.select({});
        const auto beforeRandom = submitter.nextUndo()->txnId;
        click(panel.randomizeBox());
        bool within = true;
        const int was[] = {125, 75, 100, 100};
        for (std::uint64_t id = 1; id <= 4; ++id)
            within = within && std::abs(static_cast<int>(note(id).velocity) - was[id - 1]) <= 5;
        check(within, "with none selected every note is randomized, each within the amount");
        if (submitter.nextUndo()->txnId != beforeRandom)
            check(root.command(AppCommands::Undo), "undo");
        root.frame();
        // The Ramp.
        check(panel.typeField(Field::RampStart, "40") && panel.rampStart == 40 && note(1).velocity == 40 && note(2).velocity == 60 &&
                  note(3).velocity == 80 && note(4).velocity == 100 && undoIs("Ramp Velocities", 3),
              "the Ramp's start typed as 40: the notes rise evenly from 40 at the first to 100 at the last, in the order they begin");
        {
            const auto box = panel.fieldBox(Field::RampEnd).getCentre().toFloat();
            panel.mouseDown(pressAt(panel, box));
            panel.mouseDrag(dragAt(panel, box, box.translated(0.f, 160.f)));
            panel.mouseUp(dragAt(panel, box, box.translated(0.f, 160.f)));
        }
        check(panel.rampEnd == 60 && note(1).velocity == 40 && note(4).velocity == 60 && note(2).velocity == 47 && note(3).velocity == 53 &&
                  undoIs("Ramp Velocities", 3),
              "the Ramp's end dragged down to 60 ramps again: one undo step");
        ed.select({2, 3});
        check(panel.typeField(Field::RampStart, "10") && note(2).velocity == 10 && note(3).velocity == 60 && note(1).velocity == 40 &&
                  ed.selected() == Ids({2, 3}),
              "with notes selected the ramp runs over them alone");
        check(!panel.typeField(Field::RampEnd, "loud") && panel.rampEnd == 60 && !root.error().empty(), "a word is refused");
        ed.select({});
    }

    // Scale Mode in the editor (manual 8.2.4, 10.6): its switch, Fold to Scale (G) and Highlight Scale (K).
    {
        const auto details = [&]() -> const engine::ClipDetails & { return *panel.details(); };
        const auto undoIs = [&](const char *label, int ops) {
            return submitter.nextUndo()->label == label && submitter.nextUndo()->opCount == ops;
        };
        const auto click = [&](juce::Rectangle<int> box) {
            panel.mouseDown(pressAt(panel, box.getCentre().toFloat()));
            panel.mouseUp(pressAt(panel, box.getCentre().toFloat()));
        };
        ed.select({});
        check(!details().scale.local && !ed.scaleOn() && !panel.scaleModeBox().isEmpty() && !panel.scaleFoldBox().isEmpty() &&
                  !panel.highlightBox().isEmpty() && !panel.scaleFoldBox().intersects(panel.trimBox()) &&
                  !panel.scaleFoldBox().intersects(panel.highlightBox()) && panel.headerArea().contains(panel.highlightBox()),
              "the clip holds no scale of its own; the header has a Scale and a Highlight switch, the Main panel a Scale Mode switch");
        check(ed.keyPressed(key('g')) && !ed.scaleFolded() && !ed.keys.isFolded(), "G with no scale of the clip's own folds nothing");
        click(panel.scaleModeBox());
        check(details().scale.local && ed.scaleOn() && details().scale.root == 0 && undoIs("Activate Scale", 1),
              "the Scale Mode switch gives the clip the scale it shows, C Major, as its own: one undo step");
        check(ed.keyPressed(key('g')) && ed.scaleFolded() && ed.keys.isFolded() && ed.keys.rowCount() == 75 && ed.keys.rowOf(60) && ed.keys.rowOf(62) &&
                  !ed.keys.rowOf(61) && ed.keys.y(60).value_or(-1) >= 0 && ed.keys.y(67).value_or(-1) >= 0,
              "G folds the editor to the scale's 75 key tracks: no C#3; the notes stay in view");
        check(submitter.submit(op("note.insert", {{"clip", 11}, {"note", 70}, {"start", 5 * beat}, {"dur", beat}, {"key", 61}}, "Add Note")).ok,
              "a C#3, outside the scale");
        root.frame();
        check(ed.keys.rowCount() == 76 && ed.keys.rowOf(61), "a key track outside the scale that holds a note is shown too");
        check(ed.keyPressed(key('f')) && ed.folded() && ed.keys.rowCount() == 4 && ed.keyPressed(key('f')) && !ed.folded() && ed.keys.rowCount() == 76,
              "Fold to Notes over it shows the four keys in use; switched off, the scale's fold is back");
        click(panel.scaleFoldBox());
        check(!ed.scaleFolded() && !ed.keys.isFolded() && ed.keys.y(60).value_or(-1) >= 0, "the header's Scale switch unfolds; the notes are in view");
        check(ed.highlightScale && ed.keyPressed(key('k')) && !ed.highlightScale, "K switches Highlight Scale off");
        click(panel.highlightBox());
        check(ed.highlightScale, "and the header's Highlight switch, on again");
        click(panel.scaleFoldBox());
        check(ed.scaleFolded(), "folded to the scale again");
        click(panel.scaleModeBox());
        check(!details().scale.local && !ed.scaleOn() && !ed.scaleFolded() && !ed.keys.isFolded() && undoIs("Deactivate Scale", 1),
              "Scale Mode switched off: the clip holds no scale, and the fold to it is gone");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        check(details().scale.local && ed.scaleOn(), "Undo gives the clip its scale back");
    }

    // The Status Bar reads the selected notes (manual 3.2; the text as measured on Live).
    {
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        ed.select({});
        root.frame();
        const auto before = root.selectionText();
        check(!before.startsWith("Note Selection") && ed.statusText(root.reader().meters()).isEmpty(),
              "with no note and no time selected in the editor, the Status Bar reads the arrangement's selection");
        check(submitter.submit(std::vector<OpRequest>{op("note.setVelocity", {{"clip", 11}, {"note", 1}, {"vel", 100}}, "Change Velocity"),
                                                      op("note.setVelocity", {{"clip", 11}, {"note", 4}, {"vel", 64}}, "Change Velocity")}).ok,
              "C3 at 100, G3 at 64");
        root.frame();
        ed.select({1});
        root.frame();
        check(root.selectionText() == "Note Selection   Time: 2.1.1 - 2.2.1 (0.1.0)   Pitch: C3 (60)   Velocity: 100   Probability: 100%",
              "one note selected: its time in the song with its length, its pitch with its number, its velocity and probability");
        ed.select({1, 4});
        root.frame();
        check(root.selectionText() == "Note Selection   Time: 2.1.1 - 2.4.1 (0.3.0)   Pitch: C3 - G3   Velocity: 64 - 100   Probability: 100%",
              "two notes: from the first one's start to the last one's end, and their pitch and velocity as ranges");
        ed.select({});
        ed.selectTime(5 * beat, 7 * beat);
        root.frame();
        check(root.selectionText() == "Time Selection   Start: 2.2.1   End: 2.4.1   Length: 0.2.0", "time selected in the editor: its start, end and length");
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        root.frame();
        check(root.selectionText() == before, "and with neither, the arrangement's again");
    }

    // Find and Select Notes (manual 10.5.3): the header's Find switch, the filter bar, the eight filters.
    {
        using Field = ClipViewPanel::Field;
        const auto click = [&](juce::Rectangle<int> box) {
            panel.mouseDown(pressAt(panel, box.getCentre().toFloat()));
            panel.mouseUp(pressAt(panel, box.getCentre().toFloat()));
        };
        const auto found = [&] {
            auto ids = ed.selected();
            std::sort(ids.begin(), ids.end());
            return ids;
        };
        const auto letGo = [&] {
            const juce::Point<float> empty(xAt(9 * beat) + 4.f, static_cast<float>(ed.notesArea().getY() + 3));
            ed.mouseDown(pressAt(ed, empty));
            ed.mouseUp(pressAt(ed, empty));
        };
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        ed.select({});
        const int top = ed.getY();
        check(!panel.filtersShown() && panel.filterArea().isEmpty() && !panel.findBox().isEmpty() && panel.headerArea().contains(panel.findBox()) &&
                  !panel.findBox().intersects(panel.highlightBox()),
              "the header has a Find switch; the filter bar is hidden");
        click(panel.findBox());
        check(panel.filtersShown() && ed.getY() == top + ClipViewPanel::kStripHeight && ed.getY() == panel.filterArea().getBottom() &&
                  panel.filter() == NoteFilter::Pitch && !panel.filters.any() && panel.filterMenu().getNumItems() == 8 &&
                  panel.filterArea().contains(panel.filterChooserBox()) && panel.filterArea().contains(panel.filterSelectBox()) &&
                  !panel.filterInvertBox().intersects(panel.filterSelectBox()) && panel.filterToggleCount() == 12 &&
                  panel.filterToggleBox(11).getRight() < panel.filterInvertBox().getX(),
              "the Find switch shows the filter bar under the header, the editor below it: the chooser of eight filters, Pitch's twelve switches, Invert and Select");
        check(ed.keyPressed(key('f', command | shift)) && !panel.filtersShown() && ed.getY() == top && ed.keyPressed(key('f', command | shift)) &&
                  panel.filtersShown(),
              "Ctrl+Shift+F in the editor hides the bar and shows it again");
        // Pitch.
        click(panel.filterSelectBox());
        check(found() == Ids({1, 3}) && panel.filters.applied[0], "Select with the Pitch filter on C selects the Cs; the filter is applied");
        click(panel.filterToggleBox(4));
        check(found() == Ids({1, 2, 3}) && panel.filterToggle(4), "E switched on: the Es are selected with them at once");
        click(panel.filterInvertBox());
        check(found() == Ids({4, 70}), "Invert selects the notes that are neither");
        // A second filter narrows the first.
        check(panel.chooseFilter(8) && panel.filter() == NoteFilter::Velocity && panel.filterToggleCount() == 0 &&
                  !panel.fieldBox(Field::FilterA).isEmpty() && !panel.fieldBox(Field::FilterB).isEmpty() && panel.fieldBox(Field::FilterC).isEmpty() &&
                  panel.typeField(Field::FilterA, "90") && panel.filters.velocityMin == 90 && found() == Ids({70}) && panel.filters.applied[7],
              "the Velocity filter with its lowest typed as 90, while Pitch is still applied: the notes both find");
        letGo();
        check(!panel.filters.any() && ed.selected().empty() && panel.filtersShown(), "a plain click in the editor lets every filter go and deselects; the bar stays");
        // Count.
        check(panel.chooseFilter(5), "Count");
        click(panel.filterSelectBox());
        check(found() == Ids({1, 3, 4}), "Count, every second: notes that begin together count as one");
        check(panel.typeField(Field::FilterB, "1") && found() == Ids({2, 70}), "its Offset typed as 1: the others");
        // Time.
        letGo();
        check(panel.chooseFilter(2) && panel.typeField(Field::FilterA, "1") && found() == Ids({2}), "Time: from beat 1 of the clip, one beat long");
        check(panel.typeField(Field::FilterB, "2") && found() == Ids({2, 3, 4}), "two beats long");
        check(panel.typeField(Field::FilterA, "0") && panel.typeField(Field::FilterB, "1") && panel.typeField(Field::FilterC, "2") &&
                  found() == Ids({1, 3, 4}),
              "one beat from the start, repeated every two beats");
        // Duration.
        letGo();
        check(panel.chooseFilter(6) && panel.typeField(Field::FilterA, "5") && panel.typeField(Field::FilterB, "8") && found() == Ids({3}),
              "Duration: five to eight sixteenths finds the two-beat note");
        // Scale.
        letGo();
        check(panel.chooseFilter(7) && panel.filterToggleCount() == 1 && panel.filterToggle(0), "Scale, on the clip's own scale");
        click(panel.filterSelectBox());
        check(found() == Ids({1, 2, 3, 4}), "every note in C Major: not the C#");
        // Condition.
        letGo();
        check(panel.chooseFilter(4) && panel.filterToggleCount() == 3, "Condition");
        click(panel.filterSelectBox());
        check(found() == Ids({1, 2, 3, 4, 70}), "Active: every note that is not deactivated");
        click(panel.filterToggleBox(0));
        check(found().empty() && !panel.filterToggle(0), "with no condition on, none");
        // Chance.
        letGo();
        check(panel.chooseFilter(3) && panel.typeField(Field::FilterB, "99") && found().empty() && panel.typeField(Field::FilterB, "100") &&
                  found().size() == 5,
              "Chance: up to 99 per cent finds none of these; up to 100, all");
        check(!panel.typeField(Field::FilterA, "often") && !root.error().empty(), "a word is refused");
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
            panel.chooseFilter(1);
            click(panel.filterSelectBox());
            juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics gr(picture);
                gr.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(gr, true);
            }
            const auto file = juce::File(folder).getChildFile("adi-clip-view-find.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
        }
        click(panel.findBox());
        check(!panel.filtersShown() && !panel.filters.any() && ed.getY() == top, "the Find switch hides the bar and lets the filters go");
        ed.select({});
    }

    // The Quantize settings (manual 10.5.11), at the foot of the Functions tab.
    {
        using Field = ClipViewPanel::Field;
        using Tool = ClipViewPanel::Tool;
        const auto click = [&](juce::Rectangle<int> box) {
            panel.mouseDown(pressAt(panel, box.getCentre().toFloat()));
            panel.mouseUp(pressAt(panel, box.getCentre().toFloat()));
        };
        const auto undoIs = [&](const char *label, int ops) {
            return submitter.nextUndo()->label == label && submitter.nextUndo()->opCount == ops;
        };
        panel.setToolsTab(0);
        check(ed.keyPressed(key('u', command | shift)) && panel.toolsTab() == 1, "Ctrl+Shift+U shows the Quantize settings: the Functions tab");
        {
            const std::vector<juce::Rectangle<int>> boxes = {panel.toolBox(Tool::RestrictPolyphony), panel.toolBox(Tool::Quantize),
                                                             panel.quantizeChooserBox(),             panel.quantizeStartBox(),
                                                             panel.quantizeEndBox(),                 panel.fieldBox(Field::QuantizeAmount),
                                                             panel.fieldBox(Field::QuantizeSwing)};
            bool apart = true;
            for (std::size_t i = 0; i < boxes.size(); ++i)
                for (std::size_t j = i + 1; j < boxes.size(); ++j)
                    apart = apart && !boxes[i].isEmpty() && !boxes[i].intersects(boxes[j]) && panel.toolsArea().contains(boxes[j]);
            check(apart, "the Quantize button, its value chooser, Start, End, the amount and swing each have their own place under the note functions");
        }
        check(panel.quantizeValue() == 0 && panel.quantizeAmount == 100 && panel.quantizeSwing == 0 && panel.quantizeStarts && !panel.quantizeEnds &&
                  panel.quantizeGrid() == ed.gridTicks() && panel.quantizeMenu().getNumItems() == 8,
              "as Live's defaults: to the editing grid, starts, in full");
        // A note an eighth of a beat late.
        check(submitter.submit(op("note.insert", {{"clip", 11}, {"note", 80}, {"start", 6 * beat + beat / 8}, {"dur", beat}, {"key", 72}}, "Add Note")).ok,
              "a C4 an eighth of a beat after beat 6 of the clip");
        root.frame();
        check(panel.chooseQuantize(2) && panel.quantizeGrid() == beat, "the value chosen as 1/4");
        ed.select({80});
        click(panel.toolBox(Tool::Quantize));
        check(note(80).startTicks == 6 * beat && note(80).durationTicks == beat && undoIs("Quantize Notes", 1),
              "the Quantize button moves the note's start to the beat, its length kept: one undo step");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        check(panel.typeField(Field::QuantizeAmount, "50") && panel.quantizeAmount == 50 && ed.keyPressed(key('u', command)) &&
                  note(80).startTicks == 6 * beat + beat / 16,
              "with the amount at 50 per cent, Ctrl+U moves it half the way");
        check(root.command(AppCommands::Undo) && panel.typeField(Field::QuantizeAmount, "100"), "undo; in full again");
        root.frame();
        click(panel.quantizeEndBox());
        click(panel.quantizeStartBox());
        check(panel.quantizeEnds && !panel.quantizeStarts && ed.keyPressed(key('u', command)) && note(80).startTicks == 6 * beat + beat / 8 &&
                  note(80).startTicks + note(80).durationTicks == 7 * beat,
              "End alone: the note's end goes to the beat and its start stays");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        click(panel.quantizeEndBox());
        click(panel.quantizeStartBox());
        check(panel.quantizeStarts && !panel.quantizeEnds, "starts alone again");
        // Swing (ADI's addition): every other division is delayed.
        check(panel.typeField(Field::QuantizeSwing, "50") && panel.quantizeSwing == 50, "swing at 50 per cent");
        ed.select({2, 3});
        check(ed.keyPressed(key('u', command)) && note(2).startTicks > beat && note(2).startTicks <= beat + beat / 2 && note(3).startTicks == 2 * beat &&
                  undoIs("Quantize Notes", 1),
              "a note on the second beat is delayed, by no more than half a beat; one on the third stays");
        check(root.command(AppCommands::Undo) && panel.typeField(Field::QuantizeSwing, "0") && panel.chooseQuantize(1), "undo; no swing; the editing grid again");
        root.frame();
        check(note(2).startTicks == beat, "and the note is back on its beat");
        ed.select({});
        panel.setToolsTab(0);
    }

    // Note colours (ADR-0242 d1, N1; from Cubase): by the clip (Live's, the default), velocity, pitch, channel or scale degree.
    {
        using Colours = NoteEditor::NoteColours;
        const auto paler = [&](int velocity) {
            return panel.colour().interpolatedWith(look::textBright, 0.55f * static_cast<float>(127 - velocity) / 126.f);
        };
        check(ed.noteColours == Colours::Clip && !panel.coloursBox().isEmpty() && panel.headerArea().contains(panel.coloursBox()) &&
                  !panel.coloursBox().intersects(panel.findBox()) && panel.coloursMenu().getNumItems() == 5,
              "the header has a chooser of what the notes are coloured by: five ways, the clip's first");
        check(ed.noteFill(note(1)) == paler(note(1).velocity) && ed.noteFill(note(2)) == paler(note(2).velocity) &&
                  ed.noteFill(note(1)) != ed.noteFill(note(2)),
              "by default, as Live: the clip's colour, paler as the velocity falls");
        check(panel.chooseColours(2) && ed.noteColours == Colours::Velocity && ed.noteFill(note(1)) != ed.noteFill(note(2)) &&
                  ed.noteFill(note(1)) == ed.noteFill(note(70)) && ed.noteFill(note(1)) != paler(note(1).velocity),
              "by velocity: notes of one velocity share a colour, others differ, whatever the clip's colour");
        check(panel.chooseColours(3) && ed.noteFill(note(1)) == ed.noteFill(note(3)) && ed.noteFill(note(1)) == ed.noteFill(note(80)) &&
                  ed.noteFill(note(1)) != ed.noteFill(note(2)) && ed.noteFill(note(2)) != ed.noteFill(note(4)),
              "by pitch: every C has one colour in any octave; E and G have their own");
        check(panel.chooseColours(4) && ed.noteFill(note(1)) == ed.noteFill(note(2)) && ed.noteFill(note(1)) == ed.noteFill(note(70)),
              "by channel: these notes are all on one channel, and one colour");
        check(panel.chooseColours(5) && ed.noteFill(note(70)) == look::noteOutOfScale && ed.noteFill(note(1)) == ed.noteFill(note(80)) &&
                  ed.noteFill(note(1)) != ed.noteFill(note(2)) && ed.noteFill(note(2)) != ed.noteFill(note(4)),
              "by scale degree: the root, the third and the fifth each have a colour; the C# outside C Major is dark grey");
        ed.select({2});
        check(ed.keyPressed(key('0')) && !note(2).active && ed.noteFill(note(2)) == look::mutedClip && panel.chooseColours(2) &&
                  ed.noteFill(note(2)) == look::mutedClip,
              "a deactivated note is grey whatever the notes are coloured by");
        check(ed.keyPressed(key('0')) && note(2).active && panel.chooseColours(1) && ed.noteColours == Colours::Clip && !panel.chooseColours(9),
              "activated again; the clip's colours again");
        ed.select({});
    }

    // Getting about in the editor, as Live's (manual 8.7, 10.2).
    {
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        ed.select({});
        const auto notesView = ed.notesArea();
        const auto ruler = ed.rulerArea();
        // Page Up and Down.
        const int scrolled = ed.keys.scrollY;
        check(ed.keyPressed(key(juce::KeyPress::pageUpKey)) && ed.keys.scrollY == scrolled - 12 * ed.keys.keyHeight &&
                  ed.keyPressed(key(juce::KeyPress::pageDownKey)) && ed.keys.scrollY == scrolled,
              "Page Up and Page Down scroll the key tracks an octave");
        check(ed.keyPressed(key(juce::KeyPress::pageDownKey, shift)) && ed.keys.scrollY == scrolled + ed.keys.keyHeight &&
                  ed.keyPressed(key(juce::KeyPress::pageUpKey, shift)) && ed.keys.scrollY == scrolled,
              "with Shift, one key track");
        const double timeLeft = ed.time.left;
        check(ed.keyPressed(key(juce::KeyPress::pageDownKey, command)) && ed.time.left > timeLeft && ed.keyPressed(key(juce::KeyPress::pageUpKey, command)) &&
                  std::abs(ed.time.left - timeLeft) < 1e-9,
              "with Ctrl, a page sideways and back");
        // The grid's keys act on the editor's own grid.
        const auto arrangementGrid = geo.grid;
        const auto grid = ed.gridTicks();
        check(ed.keyPressed(key('1', command)) && ed.gridTicks() == grid / 2 && ed.keyPressed(key('2', command)) && ed.gridTicks() == grid,
              "Ctrl+1 narrows the editing grid and Ctrl+2 widens it");
        check(ed.keyPressed(key('3', command)) && ed.time.grid.triplet && ed.gridTicks() == grid * 2 / 3 && ed.keyPressed(key('3', command)) &&
                  !ed.time.grid.triplet,
              "Ctrl+3 switches the triplet grid");
        check(ed.keyPressed(key('4', command)) && !ed.time.grid.snap && ed.keyPressed(key('4', command)) && ed.time.grid.snap,
              "Ctrl+4 switches snapping");
        check(ed.keyPressed(key('5', command)) && ed.time.grid.fixed && ed.gridTicks() == grid && ed.keyPressed(key('5', command)) &&
                  !ed.time.grid.fixed && geo.grid == arrangementGrid,
              "Ctrl+5 fixes the grid at the width it has; the arrangement's grid is left alone");
        // The header's grid chooser.
        check(!panel.gridBox().isEmpty() && panel.headerArea().contains(panel.gridBox()) && panel.chooseGrid(16) && ed.time.grid.fixed &&
                  ed.gridTicks() == beat / 2 && panel.readings().grid == "1/8",
              "the header's grid chooser: Fixed Grid, 1/8");
        check(panel.chooseGrid(30) && ed.time.grid.triplet && panel.chooseGrid(30) && panel.chooseGrid(31) && !ed.time.grid.snap && panel.chooseGrid(31) &&
                  panel.chooseGrid(3) && !ed.time.grid.fixed && ed.time.grid.narrow == 0 && ed.gridTicks() == grid && !panel.chooseGrid(99),
              "its Triplet Grid and Snap to Grid; and the adaptive grid's Medium, as it was");
        // Zoom in time; W and H.
        const double scale = ed.time.scale;
        check(ed.keyPressed(key('+')) && ed.time.scale > scale && ed.keyPressed(key('-')) && std::abs(ed.time.scale - scale) < 1e-6,
              "+ zooms in time and - out again");
        ed.keyPressed(key('+'));
        ed.keyPressed(key('+'));
        check(ed.keyPressed(key('w')) && ed.time.x(bar) >= -0.5 && ed.time.x(bar) < 1.0 && ed.time.x(3 * bar) <= notesView.getWidth() &&
                  ed.time.x(3 * bar) > notesView.getWidth() * 0.9,
              "W fits the clip to the editor's width");
        ed.keys.keyHeight = 30;
        check(ed.keyPressed(key('h')) && ed.keys.keyHeight < 30 && ed.keys.y(72).value_or(-1) >= 0 &&
                  ed.keys.y(60).value_or(100000) + ed.keys.keyHeight <= notesView.getHeight(),
              "H fits the notes, C3 to C4, to the editor's height");
        // The time ruler dragged, away from its handles: sideways scrolls, down zooms in.
        const juce::Point<float> grab(xAt(8 * beat), static_cast<float>(ruler.getBottom() - 3));
        check(ed.rulerPartAt(grab.roundToInt()) == NoteEditor::RulerPart::None, "a place on the ruler with no handle");
        const double leftBefore = ed.time.left, zoomBefore = ed.time.scale;
        ed.mouseDown(pressAt(ed, grab));
        ed.mouseDrag(dragAt(ed, grab, {grab.x - 40.f, grab.y}));
        check(ed.time.left > leftBefore && std::abs(ed.time.scale - zoomBefore) < 1e-9, "dragged left, the editor scrolls on in time");
        ed.mouseDrag(dragAt(ed, grab, {grab.x - 40.f, grab.y + 30.f}));
        ed.mouseUp(dragAt(ed, grab, {grab.x - 40.f, grab.y + 30.f}));
        check(ed.time.scale > zoomBefore && ed.rulerSelection() == NoteEditor::RulerPart::None, "dragged down, it zooms in");
        ed.keyPressed(key('w'));
        // The piano dragged: up and down scrolls, sideways zooms the key tracks.
        const juce::Point<float> piano(5.f, static_cast<float>(ed.keysArea().getY() + 40));
        const int scrollBefore = ed.keys.scrollY, heightBefore = ed.keys.keyHeight;
        ed.mouseDown(pressAt(ed, piano));
        ed.mouseDrag(dragAt(ed, piano, {piano.x, piano.y - 24.f}));
        check(ed.keys.scrollY == scrollBefore + 24 && ed.keys.keyHeight == heightBefore, "the piano dragged up scrolls the key tracks");
        ed.mouseDrag(dragAt(ed, piano, {piano.x + 18.f, piano.y - 24.f}));
        ed.mouseUp(dragAt(ed, piano, {piano.x + 18.f, piano.y - 24.f}));
        check(ed.keys.keyHeight == heightBefore + 3, "dragged sideways, the key tracks grow a step every six pixels");
        // A double-click on either ruler zooms to the notes.
        ed.select({});
        ed.keyPressed(key('h'));
        ed.keyPressed(key('+'));
        ed.mouseDoubleClick(pressAt(ed, grab));
        check(ed.keys.y(60).value_or(-1) >= 0 && ed.keys.y(72).value_or(-1) >= 0 && ed.time.x(bar) >= -0.5 && ed.time.x(bar) < 2.0,
              "a double-click on the time ruler zooms to the notes");
        check(ed.keyPressed(key('x')), "X goes back");
        // Full height.
        const int height = root.clipArea().getHeight();
        check(ed.keyPressed(key('e', command | alt)) && root.clipViewFull() && root.clipArea().getHeight() > height + 100 &&
                  ed.keyPressed(key('e', command | alt)) && !root.clipViewFull() && root.clipArea().getHeight() == height,
              "Ctrl+Alt+E gives the Clip View its full height, and again the height it had");
        // Follow: the editor turns its page with the playhead.
        if (!root.state().follow)
            root.command(AppCommands::Follow);
        ed.time.left = 400;
        engine::Transport driver;
        driver.play(true);
        mailbox.publish(driver);
        root.frame();
        check(root.playing() && root.state().follow && ed.playhead.isVisible() && ed.time.x(root.timelineTick()) >= 0 &&
                  ed.time.x(root.timelineTick()) < ed.notesArea().getWidth(),
              "playing with Follow on, the editor turns to the page the playhead is on");
        ed.mouseWheelMove(pressAt(ed, {static_cast<float>(notesView.getCentreX()), static_cast<float>(notesView.getCentreY())}, juce::ModifierKeys(shift)),
                          juce::MouseWheelDetails{0.f, -1.f, false, false, false});
        const double scrolledTo = ed.time.left;
        root.frame();
        check(root.arrangement.followPaused && std::abs(ed.time.left - scrolledTo) < 1e-9, "a scroll in time pauses Follow: the editor stays where it was put");
        driver.play(false);
        mailbox.publish(driver);
        root.frame();
        ed.keyPressed(key('w'));
    }

    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        ed.select({});
        ed.selectTime(5 * beat, 7 * beat);
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics gr(picture);
            gr.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(gr, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-clip-view-time.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
}

// Time inside the clip (manual 10.7): Crop Clip, Duplicate, Delete and Insert Time, Duplicate Loop.
void clipViewClipTime(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "clip-view-clip-time.adi", err);
    check(store != nullptr, "the clip time project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clip View Clip Time')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole, beat = textproj::kPPQ;
    auto op = [](const char *type, Payload payload, const char *label) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(payload);
        r.label = label;
        return r;
    };
    bool made = submitter.submit(op("track.create", {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}, {"index", 0}}, "Create track")).ok;
    made = submitter.submit(op("clip.create", {{"id", 11}, {"track", 1}, {"kind", "midi"}, {"name", "Tune"}, {"pos", bar}, {"length", 2 * bar}}, "Create clip")).ok && made;
    // In the clip's own beats: C3 at 0, D3 at 1, a two-beat E3 at 2, F3 at 5 and G3 at 7.
    made = submitter.submit(std::vector<OpRequest>{
               op("note.insert", {{"clip", 11}, {"note", 1}, {"start", 0}, {"dur", beat}, {"key", 60}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 2}, {"start", beat}, {"dur", beat}, {"key", 62}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 3}, {"start", 2 * beat}, {"dur", 2 * beat}, {"key", 64}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 4}, {"start", 5 * beat}, {"dur", beat}, {"key", 65}}, "Add Note"),
               op("note.insert", {{"clip", 11}, {"note", 5}, {"start", 7 * beat}, {"dur", beat}, {"key", 67}}, "Add Note")}).ok && made;
    check(made, "a MIDI clip of five notes");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "clip-view-clip-time");
    commands.activate(root);
    root.setSize(1400, 900);
    root.arrangement.geometry.resizeTrack(1, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    auto &panel = root.clipView;
    auto &ed = panel.notes;
    canvas.mouseDoubleClick(pressAt(canvas, {static_cast<float>(geo.x(bar)) + 20.f, static_cast<float>(laneTop(root, 1) + 30)}));
    root.frame();
    check(panel.details() != nullptr && panel.details()->notes.size() == 5, "the clip is open in the Note Editor");
    if (panel.details() == nullptr)
        return;
    const auto details = [&]() -> const engine::ClipDetails & { return *panel.details(); };
    const auto note = [&](std::uint64_t id) {
        for (const auto &n : details().notes)
            if (n.id == id)
                return n;
        return engine::ClipNote{};
    };
    const auto undoIs = [&](const char *label) { return submitter.nextUndo()->label == label; };
    const auto undo = [&] {
        const bool done = root.command(AppCommands::Undo);
        root.frame();
        return done;
    };
    const auto key = [](int code, int mods = 0) { return juce::KeyPress(code, juce::ModifierKeys(mods), 0); };
    const int shift = juce::ModifierKeys::shiftModifier, command = juce::ModifierKeys::commandModifier;
    const auto song = [&](std::int64_t clipBeats) { return bar + clipBeats; }; // the clip starts at the song's second bar

    // The time commands, over the clip's beats 2 to 4.
    ed.selectTime(song(2 * beat), song(4 * beat));
    check(ed.keyPressed(key('d', command | shift)) && details().notes.size() == 6 && note(4).startTicks == 7 * beat && note(5).startTicks == 9 * beat &&
              note(3).startTicks == 2 * beat && note(2).startTicks == beat && undoIs("Duplicate Time") && details().startMarkerTicks == 0 &&
              endMarkerOf(details()) == 8 * beat,
          "Ctrl+Shift+D with time selected: the E3 in it is copied after it and the later notes move on two beats; the clip's markers stay");
    bool copyAtFour = false;
    for (const auto &n : details().notes)
        copyAtFour = copyAtFour || (n.id > 5 && n.key == 64 && n.startTicks == 4 * beat && n.durationTicks == 2 * beat);
    check(copyAtFour && undo() && details().notes.size() == 5 && note(4).startTicks == 5 * beat, "the copy begins where the time ended; one Undo takes it all back");
    check(ed.keyPressed(key('i', command)) && details().notes.size() == 5 && note(3).startTicks == 4 * beat && note(4).startTicks == 7 * beat &&
              note(2).startTicks == beat && undoIs("Insert Time") && undo(),
          "Ctrl+I: as much empty time before the selection; the notes from its start on move on");
    check(ed.keyPressed(key(juce::KeyPress::deleteKey, command | shift)) && details().notes.size() == 4 && note(3).id == 0 &&
              note(4).startTicks == 3 * beat && note(5).startTicks == 5 * beat && undoIs("Delete Time") && !ed.timeSelection() &&
              ed.insertMarker() == song(2 * beat),
          "Ctrl+Shift+Delete: the time and the note in it go, later notes move back; the insert marker stands where it was");
    check(undo() && details().notes.size() == 5 && note(3).durationTicks == 2 * beat, "undo");
    const auto before = submitter.nextUndo()->txnId;
    ed.keyPressed(key(juce::KeyPress::escapeKey));
    check(!ed.timeSelection() && ed.keyPressed(key(juce::KeyPress::deleteKey, command | shift)) && submitter.nextUndo()->txnId == before,
          "with no time selected, Ctrl+Shift+Delete deletes nothing");

    // Crop Clip.
    ed.selectTime(song(beat), song(6 * beat));
    check(ed.keyPressed(key('j', command | shift)) && details().notes.size() == 3 && note(1).id == 0 && note(5).id == 0 && note(2).startTicks == beat &&
              undoIs("Crop Clip") && undo(),
          "Ctrl+Shift+J with time selected crops to it: the notes outside go");
    ed.keyPressed(key(juce::KeyPress::escapeKey));
    check(ed.keyPressed(key('j', command | shift)) && submitter.nextUndo()->txnId == before, "with no time selected and every note between the markers, nothing to crop");
    check(submitter.submit(op("clip.setEndMarker", {{"id", 11}, {"ticks", 5 * beat + beat / 2}}, "Set Clip End")).ok, "the End marker in the middle of the F3");
    root.frame();
    check(!details().loopEnabled && ed.keyPressed(key('j', command | shift)) && details().notes.size() == 4 && note(5).id == 0 &&
              note(4).durationTicks == beat && undoIs("Crop Clip") && undo(),
          "the loop off: the note past the End marker goes, and the one crossing it is kept whole, as measured on Live");
    check(submitter.submit(op("clip.setLoop", {{"id", 11}, {"on", true}, {"start", 3 * beat}, {"length", 2 * beat + beat / 2}}, "Set Loop Position")).ok,
          "the loop on, from the middle of the E3 to the middle of the F3");
    root.frame();
    check(ed.keyPressed(key('j', command | shift)) && details().notes.size() == 2 && note(3).startTicks == 3 * beat && note(3).durationTicks == beat &&
              note(4).startTicks == 5 * beat && note(4).durationTicks == beat / 2 && undoIs("Crop Clip") && undo(),
          "the loop on: the notes outside it go, and the two crossing its edges are cut there");

    // Duplicate Loop: Ctrl+D with the brace selected.
    const auto ruler = ed.rulerArea();
    const juce::Point<float> brace(static_cast<float>(ruler.getX()) + static_cast<float>(ed.time.x(song(4 * beat))), static_cast<float>(ruler.getY() + 3));
    ed.mouseDown(pressAt(ed, brace));
    ed.mouseUp(pressAt(ed, brace));
    check(ed.rulerSelection() == NoteEditor::RulerPart::Loop && details().notes.size() == 5, "the loop brace selected");
    {
        ed.contextOverride = true;
        juce::ApplicationCommandInfo duplicateInfo(AppCommands::Duplicate);
        commands.getCommandInfo(AppCommands::Duplicate, duplicateInfo);
        check(duplicateInfo.shortName == "Duplicate Loop" && (duplicateInfo.flags & juce::ApplicationCommandInfo::isDisabled) == 0,
              "with the brace selected, the Edit menu's Duplicate reads Duplicate Loop");
        ed.contextOverride.reset();
    }
    check(ed.keyPressed(key('d', command)) && details().loopLengthTicks == 5 * beat && details().loopStartTicks == 3 * beat && details().loopEnabled &&
              details().notes.size() == 6 && note(5).startTicks == 9 * beat + beat / 2 && note(4).startTicks == 5 * beat && undoIs("Duplicate Loop") &&
              submitter.nextUndo()->opCount == 3,
          "Ctrl+D doubles the loop, copies the note that begins in it and moves the later note on by the loop's length: one undo step");
    bool copied = false;
    for (const auto &n : details().notes)
        copied = copied || (n.id > 5 && n.key == 65 && n.startTicks == 7 * beat + beat / 2);
    const auto loopEnd = static_cast<int>(ed.time.x(song(8 * beat)));
    check(copied && loopEnd <= ed.notesArea().getWidth() && ed.time.x(song(3 * beat)) >= 0, "the copy is a loop's length after its note, and the whole loop is in view");
    check(undo() && details().loopLengthTicks == 2 * beat + beat / 2 && details().notes.size() == 5 && note(5).startTicks == 7 * beat,
          "one Undo gives back the loop and the notes");
    ed.select({4});
    check(ed.rulerSelection() == NoteEditor::RulerPart::Loop && ed.keyPressed(key(juce::KeyPress::escapeKey)), "the brace let go");
    check(ed.keyPressed(key('d', command)) && details().loopLengthTicks == 2 * beat + beat / 2 && undoIs("Duplicate Note"),
          "without the brace selected Ctrl+D duplicates the selected note, as before");

    // Pitch and Time as Live's (manual 10.5.9): Interval and Add Interval, the Stretch control, Duration and Set Length;
    // and ADI's chord and overlap tools on their own tab.
    {
        using Field = ClipViewPanel::Field;
        using Tool = ClipViewPanel::Tool;
        const auto click = [&](juce::Rectangle<int> box) {
            panel.mouseDown(pressAt(panel, box.getCentre().toFloat()));
            panel.mouseUp(pressAt(panel, box.getCentre().toFloat()));
        };
        const auto apart = [&](const std::vector<juce::Rectangle<int>> &boxes) {
            bool ok = true;
            for (std::size_t i = 0; i < boxes.size(); ++i) {
                ok = ok && !boxes[i].isEmpty() && panel.toolsArea().contains(boxes[i]);
                for (std::size_t j = i + 1; j < boxes.size(); ++j)
                    ok = ok && !boxes[i].intersects(boxes[j]);
            }
            return ok;
        };
        check(undo() && details().notes.size() == 5, "the duplicate taken back: five notes");
        panel.setToolsTab(0);
        check(apart({panel.tabBox(0), panel.tabBox(1), panel.tabBox(2)}) && panel.tabBox(3).isEmpty() && panel.toolsTab() == 0,
              "the tools panel has three tabs: Pitch and Time, Functions and Chords");
        check(apart({panel.rangeBox(), panel.fieldBox(Field::Transpose), panel.toolBox(Tool::FitToScale), panel.toolBox(Tool::Invert),
                     panel.toolBox(Tool::Reverse), panel.fieldBox(Field::Interval), panel.toolBox(Tool::AddInterval), panel.fieldBox(Field::Stretch),
                     panel.toolBox(Tool::Double), panel.toolBox(Tool::Halve), panel.durationBox(), panel.toolBox(Tool::FixedLength),
                     panel.toolBox(Tool::Legato)}) &&
                  panel.toolBox(Tool::Chord).isEmpty() && panel.toolBox(Tool::OverlapsMono).isEmpty() && panel.chordBox().isEmpty(),
              "Pitch and Time holds Live's tools, each in its own place: the range, Transpose, Fit to Scale, Invert, Reverse, the Interval size and Add "
              "Interval, the Stretch control with x2 and /2, the Duration chooser and Set Length, and Legato");
        click(panel.tabBox(2));
        check(panel.toolsTab() == 2 && apart({panel.chordBox(), panel.toolBox(Tool::Chord), panel.toolBox(Tool::InvertUp), panel.toolBox(Tool::InvertDown),
                                              panel.toolBox(Tool::Drop2), panel.toolBox(Tool::Drop3), panel.toolBox(Tool::OverlapsMono),
                                              panel.toolBox(Tool::OverlapsPoly)}) &&
                  panel.toolBox(Tool::Invert).isEmpty() && panel.rangeBox().isEmpty() && panel.fieldBox(Field::Interval).isEmpty(),
              "the Chords tab holds ADI's chord tools and the overlap functions");
        click(panel.tabBox(0));

        // Add Interval.
        ed.select({1});
        click(panel.toolBox(Tool::AddInterval));
        check(panel.intervalSize == 7 && details().notes.size() == 6 && ed.selected().size() == 1 && note(ed.selected().front()).key == 67 &&
                  note(ed.selected().front()).startTicks == 0 && undoIs("Add Interval"),
              "Add Interval puts a note a fifth above the selected C3 and selects it: one undo step");
        check(panel.typeField(Field::Interval, "-12") && panel.intervalSize == -12 && details().notes.size() == 6, "the size typed as -12; typing adds nothing");
        ed.select({});
        click(panel.toolBox(Tool::AddInterval));
        check(details().notes.size() == 12 && ed.selected().size() == 6 && undo() && details().notes.size() == 6,
              "with no note selected Add Interval works on every note: six more, an octave below");
        click(panel.scaleModeBox());
        check(details().scale.local && panel.typeField(Field::Interval, "2"), "Scale Mode on, in C Major; the size 2");
        ed.select({1});
        click(panel.toolBox(Tool::AddInterval));
        check(ed.selected().size() == 1 && note(ed.selected().front()).key == 64 && note(ed.selected().front()).startTicks == 0,
              "with the scale on the size counts its degrees: two above C3 is E3");
        check(undo() && undo() && !details().scale.local && undo() && details().notes.size() == 5, "undone: the note, the scale, and the fifth");

        // The Stretch control.
        ed.select({3});
        check(panel.typeField(Field::Stretch, "150") && note(3).durationTicks == 3 * beat && note(3).startTicks == 2 * beat && undoIs("Stretch Notes"),
              "Stretch typed as 150 makes the selected two-beat note three beats long");
        {
            const auto box = panel.fieldBox(Field::Stretch).getCentre().toFloat();
            panel.mouseDown(pressAt(panel, box));
            panel.mouseDrag(dragAt(panel, box, box.translated(0.f, 40.f)));
            panel.mouseUp(dragAt(panel, box, box.translated(0.f, 40.f)));
        }
        check(note(3).durationTicks == 3 * beat / 2 && undoIs("Stretch Notes"), "dragged down ten steps it is 50 per cent: a beat and a half");
        const auto beforeBad = submitter.nextUndo()->txnId;
        check(!panel.typeField(Field::Stretch, "5") && !panel.typeField(Field::Stretch, "fast") && !root.error().empty() &&
                  submitter.nextUndo()->txnId == beforeBad && panel.typeField(Field::Stretch, "100") && submitter.nextUndo()->txnId == beforeBad,
              "under 10 per cent and a word are refused; 100 per cent changes nothing");
        check(undo() && undo() && note(3).durationTicks == 2 * beat, "both stretches undone");

        // Duration and Set Length.
        check(panel.duration() == 1 && panel.durationMenu().getNumItems() == 8 && panel.durationTicks() == ed.gridTicks() && panel.chooseDuration(6) &&
                  panel.durationTicks() == beat && !panel.chooseDuration(0) && !panel.chooseDuration(9),
              "the Duration chooser: the editing grid's step first, then fixed lengths; 1/4 chosen");
        ed.select({3});
        click(panel.toolBox(Tool::FixedLength));
        check(note(3).durationTicks == beat && note(4).durationTicks == beat && undoIs("Set Length") && submitter.nextUndo()->opCount == 1,
              "Set Length gives the selected note the chosen length");
        check(panel.chooseDuration(5), "1/8");
        ed.select({});
        click(panel.toolBox(Tool::FixedLength));
        bool allEighths = true;
        for (const auto &n : details().notes)
            allEighths = allEighths && n.durationTicks == beat / 2;
        check(allEighths && submitter.nextUndo()->opCount == 5 && undo() && undo() && note(3).durationTicks == 2 * beat && panel.chooseDuration(2),
              "with no note selected, every note; undone, and the Duration back on Grid");
    }

    // MIDI Note Stretch (manual 10.5.6): the pair of markers under the ruler.
    {
        using Part = NoteEditor::StretchPart;
        using Ids = std::vector<std::uint64_t>;
        using Span = std::optional<std::pair<std::int64_t, std::int64_t>>;
        const auto area = ed.notesArea();
        const auto at = [&](std::int64_t songTick, float dx = 0.f) {
            return juce::Point<float>(static_cast<float>(area.getX()) + static_cast<float>(ed.time.x(songTick)) + dx,
                                      static_cast<float>(ed.stretchArea().getY() + 3));
        };
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        ed.select({});
        check(!ed.stretchSpan() && ed.stretchPartAt(at(song(2 * beat)).roundToInt()) == Part::None, "with nothing selected there are no stretch markers");
        ed.select({1});
        check(!ed.stretchSpan(), "nor with one note");
        // C3 at 0, D3 at 1 and the two-beat E3 at 2: the clip's first four beats.
        ed.select({1, 2, 3});
        check(ed.stretchSpan() == Span({song(0), song(4 * beat)}) && ed.stretchPartAt(at(song(0)).roundToInt()) == Part::Start &&
                  ed.stretchPartAt(at(song(4 * beat)).roundToInt()) == Part::End && ed.stretchPartAt(at(song(2 * beat)).roundToInt()) == Part::Middle &&
                  ed.stretchPartAt(at(song(5 * beat)).roundToInt()) == Part::None &&
                  ed.stretchPartAt(juce::Point<float>(at(song(4 * beat)).x, static_cast<float>(area.getY() + 2)).roundToInt()) == Part::None &&
                  ed.stretchArea().getBottom() == area.getY() && ed.stretchArea().getY() == ed.rulerArea().getBottom(),
              "with several notes selected a pair of markers stands at the selection's start and end, in their own strip between the ruler and the key tracks");
        // The End marker dragged from beat 4 to beat 6: half as long again. The F3 at 5 is not selected and stays.
        const auto unwritten = submitter.nextUndo()->txnId;
        ed.mouseDown(pressAt(ed, at(song(4 * beat))));
        ed.mouseDrag(dragAt(ed, at(song(4 * beat)), at(song(6 * beat), 2.f)));
        check(ed.stretchSpan() == Span({song(0), song(6 * beat)}) && submitter.nextUndo()->txnId == unwritten && note(3).startTicks == 2 * beat,
              "while the End marker is held the pair follows it, on the grid, and nothing is written");
        ed.mouseUp(dragAt(ed, at(song(4 * beat)), at(song(6 * beat), 2.f)));
        check(note(1).startTicks == 0 && note(1).durationTicks == 3 * beat / 2 && note(2).startTicks == 3 * beat / 2 && note(2).durationTicks == 3 * beat / 2 &&
                  note(3).startTicks == 3 * beat && note(3).durationTicks == 3 * beat && note(4).startTicks == 5 * beat && undoIs("Stretch Notes") &&
                  ed.selected() == Ids({1, 2, 3}) && ed.stretchSpan() == Span({song(0), song(6 * beat)}),
              "released, the selected notes are stretched in proportion: one undo step; the note not selected stays; the markers are at the new ends");
        check(undo() && note(3).startTicks == 2 * beat && note(3).durationTicks == 2 * beat, "undo");
        // The Start marker dragged past the End marker mirrors the notes.
        ed.mouseDown(pressAt(ed, at(song(0))));
        ed.mouseDrag(dragAt(ed, at(song(0)), at(song(8 * beat))));
        ed.mouseUp(dragAt(ed, at(song(0)), at(song(8 * beat))));
        check(note(1).startTicks == 7 * beat && note(2).startTicks == 6 * beat && note(3).startTicks == 4 * beat && note(3).durationTicks == 2 * beat &&
                  note(1).durationTicks == beat && undoIs("Stretch Notes"),
              "the Start marker dragged to the far side of the End marker mirrors the notes about it: the first is the last");
        check(undo() && note(1).startTicks == 0, "undo");
        // A drag begun between the two: the pseudo marker. From beat 2 to beat 3.
        ed.mouseDown(pressAt(ed, at(song(2 * beat))));
        ed.mouseDrag(dragAt(ed, at(song(2 * beat)), at(song(3 * beat))));
        ed.mouseUp(dragAt(ed, at(song(2 * beat)), at(song(3 * beat))));
        check(note(1).startTicks == 0 && note(1).durationTicks == 3 * beat / 2 && note(2).startTicks == 3 * beat / 2 && note(3).startTicks == 3 * beat &&
                  note(3).durationTicks == beat && ed.stretchSpan() == Span({song(0), song(4 * beat)}),
              "a drag from between the markers: what was before the place stretches, what was after it shrinks, and the pair's ends stay");
        check(undo() && note(3).durationTicks == 2 * beat, "undo");
        // Time selected over notes, with no note selected.
        ed.select({});
        ed.selectTime(song(0), song(2 * beat));
        check(ed.stretchSpan() == Span({song(0), song(2 * beat)}), "with time selected over notes the markers stand at the time's ends");
        ed.mouseDown(pressAt(ed, at(song(2 * beat))));
        ed.mouseDrag(dragAt(ed, at(song(2 * beat)), at(song(beat))));
        ed.mouseUp(dragAt(ed, at(song(2 * beat)), at(song(beat))));
        check(note(1).durationTicks == beat / 2 && note(2).startTicks == beat / 2 && note(2).durationTicks == beat / 2 && note(3).startTicks == 2 * beat &&
                  ed.timeSelection() == Span({song(0), song(beat)}) && ed.selected().empty(),
              "its End marker dragged in to half: the notes that begin in the time are squeezed, and the time selection is as long as the pair");
        check(undo() && note(2).startTicks == beat, "undo");
        ed.keyPressed(key(juce::KeyPress::escapeKey));
    }

    // The editor's settings are kept with the window: the grid, Highlight Scale, the notes' colours, the Quantize settings.
    {
        using Field = ClipViewPanel::Field;
        const auto kept = [&] { return ViewStateStore(*store).load("clip-view-clip-time"); };
        check(kept().noteGridNarrow == 0 && kept().highlightScale && kept().noteColours == 0 && kept().quantizeAmount == 100 && kept().quantizeStarts,
              "a window that changed none of the editor's settings holds their defaults");
        check(ed.keyPressed(key('1', command)) && kept().noteGridNarrow == 1 && ed.keyPressed(key('3', command)) && kept().noteGridTriplet &&
                  ed.keyPressed(key('4', command)) && !kept().noteGridSnap,
              "the grid's keys in the editor are kept as they are pressed");
        check(panel.chooseGrid(16) && kept().noteGridFixed && kept().noteGridFixedQuarters == 0.5, "and the grid's chooser");
        check(ed.keyPressed(key('k')) && !kept().highlightScale && panel.chooseColours(3) && kept().noteColours == 2, "Highlight Scale and the notes' colours");
        panel.setToolsTab(1);
        panel.mouseDown(pressAt(panel, panel.quantizeEndBox().getCentre().toFloat()));
        panel.mouseUp(pressAt(panel, panel.quantizeEndBox().getCentre().toFloat()));
        check(panel.chooseQuantize(5) && panel.typeField(Field::QuantizeAmount, "60") && panel.typeField(Field::QuantizeSwing, "25") &&
                  kept().quantizeValue == 4 && kept().quantizeAmount == 60 && kept().quantizeSwing == 25 && kept().quantizeEnds && kept().quantizeStarts,
              "the Quantize settings: the value, the amount, swing, and which ends move");
        // A window opening on that state takes them up.
        const auto saved = kept();
        WindowState fresh;
        panel.applyState(fresh);
        check(ed.time.grid.snap && !ed.time.grid.triplet && ed.highlightScale && ed.noteColours == NoteEditor::NoteColours::Clip && panel.quantizeAmount == 100 &&
                  panel.quantizeValue() == 0 && !panel.quantizeEnds,
              "the defaults put back in the editor");
        panel.applyState(saved);
        check(!ed.time.grid.snap && ed.time.grid.triplet && ed.time.grid.fixed && ed.time.grid.fixedQuarters == 0.5 && !ed.highlightScale &&
                  ed.noteColours == NoteEditor::NoteColours::Pitch && panel.quantizeValue() == 4 && panel.quantizeAmount == 60 && panel.quantizeSwing == 25 &&
                  panel.quantizeEnds && panel.quantizeStarts,
              "a window opening on the kept state has every one of them back");
        panel.applyState(fresh);
        panel.keepSettings();
        panel.setToolsTab(0);
    }

    // The Clip View Selector in the Status Bar (manual 8.7): a drag scrolls and zooms the editor; its view is framed.
    {
        auto &statusBar = root.status;
        auto &toggle = statusBar.clipToggle;
        ed.fitWidth();
        root.frame();
        const auto whole = statusBar.clipViewFrame();
        const auto inner = statusBar.secondField().reduced(5, 4).toFloat();
        check(root.state().clipView && !whole.isEmpty() && std::abs(whole.getX() - inner.getX()) < 1.5f && whole.getWidth() > inner.getWidth() * 0.9f,
              "with the whole clip in the editor, the selector's frame spans the whole field");
        ed.keyPressed(key('+'));
        ed.keyPressed(key('+'));
        ed.keyPressed(key('+'));
        root.frame();
        const auto zoomed = statusBar.clipViewFrame();
        check(zoomed.getWidth() < whole.getWidth() * 0.7f, "zoomed in, the frame is as much narrower as the editor shows less");
        const juce::Point<float> start(40.f, 8.f);
        const double left0 = ed.time.left, scale0 = ed.time.scale;
        statusBar.mouseDown(pressAt(toggle, start));
        statusBar.mouseDrag(dragAt(toggle, start, {start.x + 20.f, start.y}));
        check(ed.time.left > left0 && std::abs(ed.time.scale - scale0) < 1e-9 && root.arrangement.followPaused,
              "a drag to the right in the selector scrolls the editor on, by that share of the clip; it pauses Follow");
        const double moved = (ed.time.left - left0) * static_cast<double>(beat);
        check(std::abs(moved - 20. / static_cast<double>(inner.getWidth()) * static_cast<double>(2 * bar)) < static_cast<double>(beat) / 16,
              "twenty pixels of the field are that share of the clip's two bars");
        statusBar.mouseDrag(dragAt(toggle, start, {start.x + 20.f, start.y + 15.f}));
        check(ed.time.scale > scale0, "dragged down, it zooms in");
        statusBar.clipClicked();
        check(root.state().clipView, "letting go after a drag is not the click that hides the view");
        statusBar.mouseDown(pressAt(toggle, start));
        statusBar.clipClicked();
        check(!root.state().clipView, "a press let go where it began still hides the Clip View");
        statusBar.clipClicked();
        root.frame();
        check(root.state().clipView && panel.details() != nullptr, "and shows it again");
        ed.fitWidth();
    }

    // The editor's context menu: its commands under a right-click.
    {
        using Ids = std::vector<std::uint64_t>;
        const auto enabled = [&](int id) {
            const auto menu = ed.contextMenu();
            for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                if (it.getItem().itemID == id)
                    return it.getItem().isEnabled;
            return false;
        };
        const auto named = [&](int id) {
            const auto menu = ed.contextMenu();
            for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                if (it.getItem().itemID == id)
                    return it.getItem().text;
            return juce::String();
        };
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        ed.select({});
        check(!enabled(NoteEditor::kCut) && !enabled(NoteEditor::kDelete) && !enabled(NoteEditor::kJoin) && !enabled(NoteEditor::kDuplicateTime) &&
                  enabled(NoteEditor::kSelectAll) && enabled(NoteEditor::kQuantize) && enabled(NoteEditor::kCrop) &&
                  named(NoteEditor::kCrop) == "Crop Clip" && !enabled(NoteEditor::kFoldToScale),
              "with nothing selected the menu greys what needs notes or time, and Fold to Scale without a scale of the clip's own");
        // A right-click on a note that is not selected selects it.
        if (const auto box = ed.boxOf(note(2))) {
            // The step before the menu opens; the test opens no menu window.
            ed.contextClick(juce::Point<double>(box->x + box->width / 2, box->y + box->height / 2).roundToInt());
        }
        check(ed.selected() == Ids({2}) && enabled(NoteEditor::kCut) && enabled(NoteEditor::kDeactivate) && named(NoteEditor::kDeactivate) == "Deactivate Note(s)" &&
                  named(NoteEditor::kSplit) == "Chop Notes on Grid",
              "a right-click on a note selects it; the menu's note commands are live");
        check(ed.chooseContextItem(NoteEditor::kDeactivate) && !note(2).active && named(NoteEditor::kDeactivate) == "Activate Note(s)" &&
                  ed.chooseContextItem(NoteEditor::kDeactivate) && note(2).active,
              "Deactivate Note(s) from the menu, and Activate again");
        check(ed.chooseContextItem(NoteEditor::kCopy) && panel.noteClipboard.size() == 1 && ed.chooseContextItem(NoteEditor::kSelectAll) &&
                  ed.selected().size() == details().notes.size() && ed.chooseContextItem(NoteEditor::kInvertSelection) && ed.selected().empty(),
              "Copy, Select All and Invert Selection");
        ed.selectTime(song(0), song(2 * beat));
        check(enabled(NoteEditor::kDuplicateTime) && named(NoteEditor::kCrop) == "Crop Clip to Time Selection" &&
                  named(NoteEditor::kSplit) == "Split Notes at Time Selection" && ed.chooseContextItem(NoteEditor::kInsertTime) && undoIs("Insert Time") && undo(),
              "with time selected the time commands are live, and Crop and Split name the time selection");
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        const auto narrow = ed.time.grid.narrow;
        check(ed.chooseContextItem(NoteEditor::kGridItems + 31) && !ed.time.grid.snap && ed.chooseContextItem(NoteEditor::kGridItems + 31) && ed.time.grid.snap &&
                  ed.time.grid.narrow == narrow && ed.chooseContextItem(NoteEditor::kFoldToNotes) && ed.folded() &&
                  ed.chooseContextItem(NoteEditor::kFoldToNotes) && !ed.folded() && ed.chooseContextItem(NoteEditor::kHighlightScale) &&
                  ed.chooseContextItem(NoteEditor::kHighlightScale) && ed.chooseContextItem(NoteEditor::kQuantizeSettings) && panel.toolsTab() == 1 &&
                  !ed.chooseContextItem(999),
              "the grid's submenu, the folds, Highlight Scales and Quantize Settings from the menu");
        panel.setToolsTab(0);
        ed.select({});
    }

    // The divider above the Velocity lane resizes it (manual 10.1); its height is kept with the window.
    {
        const auto lane = ed.velocityArea();
        const auto keysBefore = ed.notesArea().getHeight();
        check(lane.getHeight() == NoteEditor::kVelocityHeight && ed.laneDivider().getCentreY() == lane.getY() && ed.laneHeight() == 56,
              "the lane opens 56 high, with its divider on the line above it");
        const juce::Point<float> grip(300.f, static_cast<float>(lane.getY()));
        ed.mouseDown(pressAt(ed, grip));
        ed.mouseDrag(dragAt(ed, grip, {grip.x, grip.y - 30.f}));
        check(ed.velocityArea().getHeight() == 86 && ed.notesArea().getHeight() == keysBefore - 30 && ed.selected().empty(),
              "the divider dragged up thirty pixels: the lane is that much higher and the key tracks that much lower");
        ed.mouseUp(dragAt(ed, grip, {grip.x, grip.y - 30.f}));
        check(ViewStateStore(*store).load("clip-view-clip-time").noteLaneHeight == 86, "let go, the height is kept with the window");
        ed.mouseDown(pressAt(ed, {grip.x, grip.y - 30.f}));
        ed.mouseDrag(dragAt(ed, {grip.x, grip.y - 30.f}, {grip.x, grip.y - 2000.f}));
        check(ed.notesArea().getHeight() == NoteEditor::kKeysFloor, "dragged to the top, the key tracks keep their floor");
        ed.mouseDrag(dragAt(ed, {grip.x, grip.y - 30.f}, {grip.x, grip.y + 2000.f}));
        ed.mouseUp(dragAt(ed, {grip.x, grip.y - 30.f}, {grip.x, grip.y + 2000.f}));
        check(ed.velocityArea().getHeight() == NoteEditor::kLaneFloor, "dragged to the foot, the lane keeps the height its header needs");
        ed.setLaneHeight(NoteEditor::kVelocityHeight);
        panel.keepSettings();
        check(ed.velocityArea().getHeight() == 56 && ed.notesArea().getHeight() == keysBefore, "and back at 56");
    }

    // Note spelling, from the piano ruler's menu (manual 10.6).
    {
        const auto kept = [&] { return ViewStateStore(*store).load("clip-view-clip-time").noteSpelling; };
        ed.select({});
        check(submitter.submit(op("note.insert", {{"clip", 11}, {"note", 90}, {"start", 6 * beat}, {"dur", beat}, {"key", 70}}, "Add Note")).ok, "an A#3");
        root.frame();
        ed.select({90});
        root.frame();
        check(ed.spelling == NoteSpelling::Sharps && ed.spellingMenu().getNumItems() == 5 && root.selectionText().contains("Pitch: A#3 (70)") &&
                  panel.readings().range == "A#3 - A#3",
              "by default a black key is spelled with a sharp: in the Status Bar and in the Pitch and Time range");
        check(ed.chooseSpelling(2) && ed.named(70) == "Bb3" && root.selectionText().contains("Pitch: Bb3 (70)") && panel.readings().range == "Bb3 - Bb3" &&
                  kept() == 1,
              "Flats chosen from the piano's menu: the same note reads Bb3 everywhere, and the choice is kept with the window");
        check(ed.chooseSpelling(3) && ed.named(70) == "A#/Bb3" && ed.chooseSpelling(5) && ed.named(70) == "70" && ed.named(60) == "60" &&
                  ed.chooseSpelling(4) && ed.named(70) == "A#3" && !ed.chooseSpelling(0) && !ed.chooseSpelling(6),
              "both, MIDI note numbers, and Auto, which follows the clip's root: C, so sharps");
        check(ed.chooseSpelling(1) && kept() == 0 && undo() && details().notes.size() == 5, "sharps again; the test note taken back");
        ed.select({});
    }

    // A double-click on the title folds the panels (manual 8.1.1).
    {
        using Field = ClipViewPanel::Field;
        const int open = ClipViewPanel::kPanelWidth + ClipViewPanel::kToolsWidth;
        check(!panel.panelsFolded() && panel.panelWidth() == open && ed.getX() == open + 1, "the panels are open: the Main panel, the tools, then the editor");
        const juce::Point<float> onTitle(80.f, 8.f);
        panel.mouseDoubleClick(pressAt(panel, onTitle));
        check(panel.panelsFolded() && panel.panelWidth() == ClipViewPanel::kFoldedWidth && ed.getX() == ClipViewPanel::kFoldedWidth + 1 &&
                  panel.toolsArea().isEmpty() && panel.fieldBox(Field::Start).isEmpty() && panel.loopBox().isEmpty() && panel.scaleModeBox().isEmpty() &&
                  panel.setBox(Field::Start).isEmpty() && panel.fieldAt({80, 60}) == Field::None &&
                  panel.titleArea().getHeight() == panel.getHeight() && !panel.velocityStripArea().isEmpty() &&
                  ViewStateStore(*store).load("clip-view-clip-time").clipPanelsFolded,
              "a double-click on the title folds them to a strip: the editor takes their place, none of their controls is there, and it is kept with the window");
        const auto beforeFold = submitter.nextUndo()->txnId;
        panel.mouseDown(pressAt(panel, {80.f, 60.f}));
        panel.mouseUp(pressAt(panel, {80.f, 60.f}));
        check(submitter.nextUndo()->txnId == beforeFold, "a click where a field used to be does nothing");
        panel.mouseDoubleClick(pressAt(panel, {10.f, 100.f}));
        check(!panel.panelsFolded() && panel.panelWidth() == open && ed.getX() == open + 1 && !panel.fieldBox(Field::Start).isEmpty() &&
                  !ViewStateStore(*store).load("clip-view-clip-time").clipPanelsFolded,
              "a double-click on the strip brings them back");
        const bool muted = details().muted;
        panel.mouseDoubleClick(pressAt(panel, panel.activatorArea().getCentre().toFloat()));
        check(!panel.panelsFolded() && details().muted == muted, "a double-click on the activator folds nothing");
    }

    // The window's Edit menu reaches the editor while it has the keyboard (manual 10.5).
    {
        const auto arrangementSelection = geo.selection.clip;
        ed.select({2});
        check(!ed.hasContext() && !root.noteEditorHasContext(), "a headless editor holds no keyboard: the Edit menu is the arrangement's");
        ed.contextOverride = true;
        check(root.noteEditorHasContext() && root.command(AppCommands::Copy) && panel.noteClipboard.size() == 1 && panel.noteClipboard[0].key == 62,
              "with the editor in use, Edit > Copy copies the selected note");
        check(root.command(AppCommands::SelectAll) && ed.selected().size() == details().notes.size() && geo.selection.clip == arrangementSelection,
              "Edit > Select All selects the editor's notes and leaves the arrangement's selection");
        ed.select({2});
        check(root.command(AppCommands::Duplicate) && details().notes.size() == 6 && undoIs("Duplicate Note") && undo(),
              "Edit > Duplicate duplicates the selected note");
        ed.select({2});
        check(root.command(AppCommands::Cut) && details().notes.size() == 4 && undoIs("Cut Note"), "Edit > Cut takes it out");
        ed.selectTime(song(beat), song(beat));
        check(root.command(AppCommands::Paste) && details().notes.size() == 5 && undoIs("Paste Note") && ed.selected().size() == 1 &&
                  note(ed.selected().front()).startTicks == beat && note(ed.selected().front()).key == 62,
              "Edit > Paste puts it back at the insert marker");
        check(undo() && undo() && details().notes.size() == 5 && note(2).startTicks == beat, "the paste and the cut undone: the clip is as it was");
        ed.contextOverride = false;
        const auto selectedBefore = ed.selected();
        root.command(AppCommands::SelectAll);
        check(ed.selected() == selectedBefore, "with the editor not in use, Edit > Select All is the arrangement's again");
        ed.contextOverride.reset();
        ed.select({});
    }

    // The editor's other commands in the window's Edit, View and Options menus (manual 10.5 and on).
    {
        using C = AppCommands;
        const auto info = [&](int id) {
            juce::ApplicationCommandInfo i(id);
            commands.getCommandInfo(id, i);
            return i;
        };
        const auto active = [&](int id) { return (info(id).flags & juce::ApplicationCommandInfo::isDisabled) == 0; };
        const auto ticked = [&](int id) { return (info(id).flags & juce::ApplicationCommandInfo::isTicked) != 0; };
        commands.activate(root);
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        ed.select({2});
        // With the keyboard elsewhere.
        check(!root.noteEditorHasContext() && root.noteEditorShown() && !active(C::InvertSelection) && !active(C::Quantize) && !active(C::QuantizeSettings) &&
                  !active(C::CropClip) && !root.command(C::InvertSelection) && !root.command(C::CropClip) && ed.selected() == std::vector<std::uint64_t>{2} &&
                  info(C::DeleteTrack).shortName != "Delete" && info(C::Split).shortName == "Split",
              "with the keyboard elsewhere the editor's Edit commands are greyed and do nothing, and Delete and Split are the arrangement's");
        const bool wasFolded = ed.folded(), wasHighlit = ed.highlightScale, wasDrawing = ed.drawMode();
        check(active(C::FoldToNotes) && ticked(C::FoldToNotes) == wasFolded && root.command(C::FoldToNotes) && ed.folded() != wasFolded &&
                  ticked(C::FoldToNotes) != wasFolded && root.command(C::FoldToNotes) && ed.folded() == wasFolded,
              "View > Fold to Notes folds the editor on screen, with the keyboard anywhere, and is ticked while it is folded");
        check(active(C::FoldToScale) == ed.scaleOn() && active(C::HighlightScales) && ticked(C::HighlightScales) == wasHighlit &&
                  root.command(C::HighlightScales) && ed.highlightScale != wasHighlit && root.command(C::HighlightScales) && ed.highlightScale == wasHighlit,
              "Fold to Scale is greyed without a scale of the clip's own; Highlight Scales is thrown and ticked the same way");
        check(active(C::DrawMode) && ticked(C::DrawMode) == wasDrawing && root.command(C::DrawMode) && ed.drawMode() != wasDrawing && ticked(C::DrawMode) != wasDrawing &&
                  root.command(C::DrawMode) && ed.drawMode() == wasDrawing,
              "Options > Draw Mode switches the editor's Draw Mode and is ticked while it is on");
        check(!commands.key(key('f'), root) && ed.folded() == wasFolded && !commands.key(key('b'), root) && ed.drawMode() == wasDrawing &&
                  !commands.key(key('k'), root) && ed.highlightScale == wasHighlit,
              "F, B and K pressed outside the editor are no command's: nothing folds or switches");
        const int height = root.clipArea().getHeight();
        check(active(C::ExpandClipView) && !ticked(C::ExpandClipView) && root.command(C::ExpandClipView) && root.clipViewFull() && ticked(C::ExpandClipView) &&
                  root.clipArea().getHeight() > height && commands.key(key('e', command | juce::ModifierKeys::altModifier), root) && !root.clipViewFull() &&
                  root.clipArea().getHeight() == height,
              "View > Expand Clip View gives it its full height, ticked; Ctrl+Alt+E from outside the editor gives the height back");
        // With the editor in use.
        ed.contextOverride = true;
        const auto tracks = view.current()->tracks.size();
        check(info(C::DeleteTrack).shortName == "Delete" && active(C::DeleteTrack) && root.command(C::DeleteTrack) && details().notes.size() == 4 &&
                  note(2).id == 0 && undoIs("Delete Note") && view.current()->tracks.size() == tracks && undo() && details().notes.size() == 5,
              "with the editor in use, Edit > Delete deletes the selected note and never the track");
        ed.select({2});
        check(active(C::InvertSelection) && root.command(C::InvertSelection) && ed.selected().size() == 4 && !ed.isSelected(2),
              "Edit > Invert Selection selects the other four notes");
        ed.select({2});
        panel.setToolsTab(0);
        check(active(C::Quantize) && active(C::QuantizeSettings) && root.command(C::QuantizeSettings) && panel.toolsTab() == 1,
              "Quantize is offered, and Quantize Settings shows the Functions tab");
        panel.setToolsTab(0);
        check(info(C::Split).shortName == "Chop Notes on Grid" && info(C::CropClip).shortName == "Crop Clip" && active(C::CropClip) && !active(C::DuplicateTime) &&
                  !active(C::DeleteTime) && !active(C::InsertSilence) && !active(C::CutTime) && !active(C::CopyTime) && !active(C::PasteTime) &&
                  info(C::RenameTrack).shortName == "Rename" && active(C::RenameTrack),
              "with a note selected Split reads Chop Notes on Grid; with no time selected the Time commands are greyed; Rename is the clip's");
        ed.select({});
        ed.selectTime(song(2 * beat), song(4 * beat));
        check(info(C::Split).shortName == "Split Notes at Time Selection" && info(C::CropClip).shortName == "Crop Clip to Time Selection" &&
                  active(C::DuplicateTime) && active(C::DeleteTime) && info(C::InsertSilence).shortName == "Insert Time" && active(C::InsertSilence) &&
                  !active(C::CutTime) && !root.command(C::CutTime),
              "with time selected: Split and Crop read for it, Duplicate, Delete and Insert Time are the editor's, and Cut Time is not");
        const auto arrangementInsert = geo.selection.insert;
        check(root.command(C::DuplicateTime) && undoIs("Duplicate Time") && note(4).startTicks == 7 * beat && geo.selection.insert == arrangementInsert && undo() &&
                  note(4).startTicks == 5 * beat,
              "Edit > Duplicate Time works inside the clip: the later notes move on by the selection, and the arrangement is left alone");
        const bool triplet = ed.time.grid.triplet, arrangementTriplet = geo.grid.triplet, snap = ed.time.grid.snap;
        check(root.command(C::TripletGrid) && ed.time.grid.triplet != triplet && geo.grid.triplet == arrangementTriplet && ticked(C::TripletGrid) != triplet &&
                  root.command(C::TripletGrid) && ed.time.grid.triplet == triplet && ticked(C::SnapToGrid) == snap,
              "Options > Triplet Grid switches the editor's own grid and is ticked from it; the arrangement's grid stays");
        ed.contextOverride = false;
        check(info(C::InsertSilence).shortName == "Insert Silence" && ticked(C::TripletGrid) == arrangementTriplet && info(C::RenameTrack).shortName != "Rename Locator",
              "with the editor not in use the same entries are the arrangement's again");
        ed.contextOverride.reset();
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        ed.select({});
    }

    // The panels side by side, stacked, or as the Clip View's height allows (manual 8.1.2).
    {
        using C = AppCommands;
        using Panels = ClipViewPanel::Panels;
        using Field = ClipViewPanel::Field;
        const int side = ClipViewPanel::kPanelWidth + ClipViewPanel::kToolsWidth, column = ClipViewPanel::kToolsWidth;
        const auto arranged = [&] { return ViewStateStore(*store).load("clip-view-clip-time").clipPanelsArrangement; };
        const auto ticked = [&](int id) {
            juce::ApplicationCommandInfo i(id);
            commands.getCommandInfo(id, i);
            return (i.flags & juce::ApplicationCommandInfo::isTicked) != 0;
        };
        commands.activate(root);
        check(panel.arrangement() == Panels::Automatic && panel.getHeight() < ClipViewPanel::kStackedHeight && !panel.panelsVertical() &&
                  panel.panelWidth() == side && ticked(C::ArrangePanelsAutomatically) && !ticked(C::ArrangePanelsVertically),
              "the panels follow the Clip View's height by default: at this height, side by side");
        check(root.command(C::ArrangePanelsVertically) && panel.panelsVertical() && panel.panelWidth() == column && ed.getX() == column + 1 &&
                  panel.toolsArea().getX() == 0 && panel.toolsArea().getY() == panel.mainArea().getBottom() + 1 &&
                  panel.tabBox(0).getY() > panel.fieldBox(Field::Signature).getBottom() && panel.fieldBox(Field::End).getRight() <= column &&
                  panel.titleArea().getWidth() == column && arranged() == 2 && ticked(C::ArrangePanelsVertically) && !ticked(C::ArrangePanelsAutomatically),
              "View > Arrange Clip View Panels Vertically stacks them in one column, the tools under the Main panel; the editor takes the room; kept with the window");
        panel.mouseDown(pressAt(panel, panel.tabBox(1).getCentre().toFloat()));
        check(panel.toolsTab() == 1 && !panel.toolBox(ClipViewPanel::Tool::VelocityAdd).isEmpty() &&
                  panel.toolBox(ClipViewPanel::Tool::VelocityAdd).getY() > panel.mainArea().getBottom(),
              "stacked, a click on a tab still shows that tab's tools, now under the Main panel");
        panel.setToolsTab(0);
        // A stacked column higher than the Clip View scrolls under the clip's title.
        {
            using Tool = ClipViewPanel::Tool;
            const auto wheel = [&](float by) {
                panel.mouseWheelMove(pressAt(panel, panel.columnArea().getCentre().toFloat()), juce::MouseWheelDetails{0.f, by, false, false, false});
            };
            // Each tab's column ends a margin under the lowest thing on it.
            bool fitted = true;
            for (int tab = 0; tab < ClipViewPanel::kToolTabs; ++tab) {
                panel.setToolsTab(tab);
                int lowest = tab == 1 ? panel.fieldBox(Field::QuantizeAmount).getBottom() : 0;
                for (const auto tool : ClipViewPanel::kTools)
                    lowest = std::max(lowest, panel.toolBox(tool).getBottom());
                fitted = fitted && lowest + 5 == panel.columnHeight();
            }
            check(fitted && panel.columnScroll() == 0, "the column is as high as the title, the Main panel and the shown tab's tools down to the lowest of them");
            panel.setToolsTab(1);
            const int limit = panel.columnScrollLimit();
            const auto startBox = panel.fieldBox(Field::Start);
            const auto editorBounds = ed.getBounds();
            check(limit > 0 && limit == panel.columnHeight() - panel.getHeight() && panel.fieldBox(Field::QuantizeAmount).getBottom() > panel.getHeight() &&
                      !panel.scrollThumb().isEmpty() && panel.scrollThumb().getY() == panel.columnArea().getY() &&
                      panel.scrollThumb().getX() >= panel.toolBox(Tool::VelocityAdd).getRight() && panel.scrollThumb().getRight() <= panel.panelsEdge().getX(),
                  "stacked at this height the column is higher than the view: its foot is out of sight, and a thumb stands between the fields and the panels' edge");
            wheel(-0.25f);
            check(panel.columnScroll() == 45 && panel.fieldBox(Field::Start).getY() == startBox.getY() - 45 && panel.fieldBox(Field::Start).getX() == startBox.getX() &&
                      panel.tabBox(1).getY() == panel.mainArea().getBottom() + 1 && panel.titleArea().getY() == 0 && panel.columnArea().getY() == ClipViewPanel::kTitleHeight &&
                      ed.getBounds() == editorBounds && panel.scrollThumb().getY() > panel.columnArea().getY(),
                  "the wheel over the column moves the Main panel and the tools together; the title and the editor stay, and the thumb follows");
            wheel(-10.f);
            check(panel.columnScroll() == limit && panel.fieldBox(Field::QuantizeAmount).getBottom() + 5 == panel.getHeight() &&
                      panel.scrollThumb().getBottom() == panel.columnArea().getBottom(),
                  "the wheel stops at the column's foot, now whole in view, the thumb at the foot of its run");
            wheel(10.f);
            check(panel.columnScroll() == 0 && panel.fieldBox(Field::Start) == startBox, "and at its head");
            // The title covers what is scrolled under it.
            const bool looped = details().loopEnabled;
            const auto undoBefore = submitter.nextUndo()->txnId;
            const bool under = panel.scrollColumn(panel.loopBox().getY() - 2) && panel.titleArea().contains(panel.loopBox().getCentre());
            panel.mouseDown(pressAt(panel, panel.loopBox().getCentre().toFloat()));
            panel.mouseUp(pressAt(panel, panel.loopBox().getCentre().toFloat()));
            check(under && details().loopEnabled == looped && submitter.nextUndo()->txnId == undoBefore,
                  "a click on the title does not reach the Loop switch scrolled under it");
            panel.scrollColumn(0);
            // The thumb: a drag moves the column, a click beside it turns a page.
            const auto thumbAt = panel.scrollThumb().getCentre().toFloat();
            panel.mouseDown(pressAt(panel, thumbAt));
            panel.mouseDrag(dragAt(panel, thumbAt, {thumbAt.x, thumbAt.y + 500.f}));
            const bool atFoot = panel.columnScroll() == limit;
            panel.mouseDrag(dragAt(panel, thumbAt, {thumbAt.x, thumbAt.y}));
            const bool backAtHead = panel.columnScroll() == 0;
            panel.mouseUp(dragAt(panel, thumbAt, thumbAt));
            check(atFoot && backAtHead && panel.arrangement() == Panels::Vertical && submitter.nextUndo()->txnId == undoBefore,
                  "the thumb dragged down brings the foot, dragged back the head; nothing is edited and the panels stay stacked");
            const juce::Point<float> below{thumbAt.x, static_cast<float>(panel.getHeight() - 3)};
            panel.mouseDown(pressAt(panel, below));
            panel.mouseUp(pressAt(panel, below));
            check(panel.columnScroll() == std::min(limit, panel.columnArea().getHeight()), "a click below the thumb turns a page down");
            // A field cut off at the foot comes whole into view when it opens.
            panel.scrollColumn(0);
            panel.scrollColumn(panel.fieldBox(Field::QuantizeAmount).getBottom() - panel.getHeight() - 8);
            const auto cut = panel.fieldBox(Field::QuantizeAmount);
            const juce::Point<float> visiblePart{static_cast<float>(cut.getCentreX()), static_cast<float>(cut.getY() + 3)};
            panel.mouseDown(pressAt(panel, visiblePart));
            panel.mouseUp(pressAt(panel, visiblePart));
            check(cut.getBottom() == panel.getHeight() + 8 && panel.editing() == Field::QuantizeAmount && panel.fieldEditor().isVisible() &&
                      panel.fieldBox(Field::QuantizeAmount).getBottom() == panel.getHeight() - 2 && panel.fieldEditor().getBounds() == panel.fieldBox(Field::QuantizeAmount),
                  "a click on a field cut off at the foot brings it whole into view and opens it there");
            wheel(-0.25f);
            check(panel.editing() == Field::None && !panel.fieldEditor().isVisible(), "the open field closes when the column moves under it");
            // A shorter tab, and the panels side by side: the place stays inside the column.
            panel.scrollColumn(limit);
            panel.setToolsTab(2);
            check(panel.columnScrollLimit() < limit && panel.columnScroll() == panel.columnScrollLimit() &&
                      panel.toolBox(Tool::OverlapsMono).getBottom() + 5 == panel.getHeight(),
                  "a shorter tab chosen at the foot of a longer one ends at its own foot");
            // The stacked column scrolled, for the PR's picture (ADI_UI_PARITY).
            if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
                panel.setToolsTab(1);
                panel.scrollColumn(panel.columnScrollLimit() / 2);
                root.frame();
                juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
                {
                    juce::Graphics gr(picture);
                    gr.addTransform(juce::AffineTransform::scale(2.f));
                    root.paintEntireComponent(gr, true);
                }
                const auto file = juce::File(folder).getChildFile("adi-clip-view-scrolled.png");
                file.deleteFile();
                if (auto stream = file.createOutputStream())
                    juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
            }
            panel.setToolsTab(1);
            panel.scrollColumn(40);
            root.command(C::ArrangePanelsHorizontally);
            const bool sideBySide = panel.columnScrollLimit() == 0 && panel.columnScroll() == 0 && panel.scrollThumb().isEmpty() && panel.columnArea().isEmpty() &&
                                    panel.mainArea().getY() == ClipViewPanel::kTitleHeight && panel.toolsArea().getY() == 0;
            wheel(-1.f);
            check(sideBySide && panel.columnScroll() == 0 && panel.mainArea().getY() == ClipViewPanel::kTitleHeight,
                  "side by side nothing scrolls: no thumb, and the wheel leaves the panels where they stand");
            root.command(C::ArrangePanelsVertically);
            panel.scrollColumn(0);
            panel.setToolsTab(0);
        }
        const auto pull = [&](float by) {
            const auto edge = panel.panelsEdge().getCentre().toFloat();
            panel.mouseDown(pressAt(panel, edge));
            panel.mouseDrag(dragAt(panel, edge, {edge.x + by, edge.y}));
            panel.mouseUp(dragAt(panel, edge, {edge.x + by, edge.y}));
        };
        pull(10.f);
        check(panel.arrangement() == Panels::Vertical, "a short pull on the panels' right edge changes nothing");
        pull(40.f);
        check(panel.arrangement() == Panels::Horizontal && !panel.panelsVertical() && panel.panelWidth() == side && ed.getX() == side + 1 && arranged() == 1 &&
                  ticked(C::ArrangePanelsHorizontally),
              "the edge pulled to the right stands the panels side by side");
        pull(-40.f);
        check(panel.arrangement() == Panels::Vertical && panel.panelWidth() == column && arranged() == 2, "pulled to the left it stacks them");
        check(panel.chooseTitleItem(2) && panel.arrangement() == Panels::Automatic && !panel.panelsVertical() && panel.panelWidth() == side && arranged() == 0,
              "Arrange Clip View Panels Automatically, from the title's menu: at this height side by side again");
        check(root.command(C::ExpandClipView) && panel.getHeight() >= ClipViewPanel::kStackedHeight && panel.panelsVertical() && ed.getX() == column + 1 &&
                  root.command(C::ExpandClipView) && !panel.panelsVertical() && ed.getX() == side + 1,
              "at its full height the Clip View is high enough for both: they stack, and stand side by side again at the height it had");
        // The Clip View at its full height, its panels stacked, for the PR's picture (ADI_UI_PARITY).
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
            root.command(C::ExpandClipView);
            root.frame();
            juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics gr(picture);
                gr.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(gr, true);
            }
            const auto file = juce::File(folder).getChildFile("adi-clip-view-stacked.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
            root.command(C::ExpandClipView);
            root.frame();
        }
        check(panel.chooseTitleItem(2) && panel.arrangement() == Panels::Horizontal && panel.chooseTitleItem(2) && panel.arrangement() == Panels::Automatic,
              "unticked, Automatically leaves the panels as they stand; ticked again, the height decides");
        check(root.command(C::ArrangePanelsHorizontally) && panel.arrangement() == Panels::Horizontal && root.command(C::ArrangePanelsAutomatically) &&
                  panel.arrangement() == Panels::Automatic && arranged() == 0,
              "the View menu's Horizontally and Automatically choose the same");
        check(panel.chooseTitleItem(3) && panel.panelsFolded() && panel.panelsEdge().isEmpty() && panel.chooseTitleItem(3) && !panel.panelsFolded(),
              "Fold, in the title's menu, folds the panels and brings them back");
        // The Set buttons' keys (manual 41.8): Ctrl+F9 to F12, at the arrangement's insert marker while stopped.
        {
            const auto insertBefore = geo.selection.insert;
            const auto startBefore = details().startMarkerTicks;
            geo.selection.insert = song(beat);
            check(ed.keyPressed(key(juce::KeyPress::F9Key, command)) && details().startMarkerTicks == beat && undo() && details().startMarkerTicks == startBefore,
                  "Ctrl+F9 sets the Start marker where the insert marker stands, as the panel's Set does");
            geo.selection.insert = song(2 * beat);
            check(ed.keyPressed(key(juce::KeyPress::F10Key, command)) && details().loopEnabled && details().loopStartTicks == 2 * beat && undo(),
                  "Ctrl+F10 sets the loop's start there and switches the loop on");
            geo.selection.insert = insertBefore;
        }
        // Navigate > Clip View (manual 41.2): a hidden Clip View is shown first.
        check(root.state().clipView && root.command(C::ToggleClipView) && !root.state().clipView && root.command(C::FocusClipView) &&
                  root.state().clipView && panel.details() != nullptr,
              "Navigate > Clip View shows a hidden Clip View, its clip still in it, and gives its editor the keyboard");
    }

    // Fit to Time Range (manual 10.5.5): Ctrl+Alt+J, the menus, the Duration chooser's Time Range.
    {
        using C = AppCommands;
        const int alt = juce::ModifierKeys::altModifier;
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        ed.select({});
        check(!ed.canFitToRange() && !ed.contextEntry(NoteEditor::kFitToRange).enabled && !ed.fitToRange(),
              "with no note and no time selected there is nothing to fit");
        // The D3 (beat 1, a beat long) and the two-beat E3 (beat 2): together they span beats 1 to 4.
        ed.select({2, 3});
        const auto unfitted = submitter.nextUndo()->txnId;
        check(ed.canFitToRange() && ed.keyPressed(key('j', command | alt)) && note(2).startTicks == beat && note(2).durationTicks == 3 * beat &&
                  note(3).startTicks == beat && note(3).durationTicks == 3 * beat && note(2).key == 62 && note(3).key == 64 && undoIs("Fit to Time Range") &&
                  submitter.nextUndo()->txnId != unfitted && ed.selected() == std::vector<std::uint64_t>({2, 3}),
              "Ctrl+Alt+J with two notes selected: both begin where the first began and end where the last ended, on their own keys, one undo step");
        check(undo() && note(2).durationTicks == beat && note(3).startTicks == 2 * beat && note(3).durationTicks == 2 * beat, "one Undo gives both back");
        // A time selection is the range: the clip's beats 4 to 6, with the F3 (beat 5) selected.
        ed.select({4});
        ed.selectTime(song(4 * beat), song(6 * beat));
        ed.select({4});
        check(ed.timeSelection().has_value() && ed.chooseContextItem(NoteEditor::kFitToRange) && note(4).startTicks == 4 * beat && note(4).durationTicks == 2 * beat &&
                  undoIs("Fit to Time Range") && undo(),
              "with time selected the selected note takes the selection's start and end; from the editor's menu");
        ed.select({});
        ed.selectTime(song(4 * beat), song(6 * beat));
        check(!ed.anySelected() && ed.fitToRange() && note(4).startTicks == 4 * beat && note(4).durationTicks == 2 * beat && note(5).startTicks == 7 * beat && undo(),
              "with time selected and no note, the notes that begin in it are fitted; the others stay");
        // The Edit menu's entry, with the editor in use.
        ed.keyPressed(key(juce::KeyPress::escapeKey));
        ed.select({2, 3});
        ed.contextOverride = true;
        juce::ApplicationCommandInfo fit(C::FitToTimeRange);
        commands.getCommandInfo(C::FitToTimeRange, fit);
        check(fit.shortName == "Fit to Time Range" && (fit.flags & juce::ApplicationCommandInfo::isDisabled) == 0 && root.command(C::FitToTimeRange) &&
                  note(3).startTicks == beat && undoIs("Fit to Time Range") && undo(),
              "Edit > Fit to Time Range does the same while the editor is in use");
        ed.contextOverride = false;
        check(!root.command(C::FitToTimeRange) && note(3).startTicks == 2 * beat, "and nothing outside it");
        ed.contextOverride.reset();
        // The Duration chooser's Time Range, applied by Set Length.
        panel.setToolsTab(0);
        check(panel.chooseDuration(1) && panel.duration() == 0 && panel.runTool(ClipViewPanel::Tool::FixedLength) && note(2).durationTicks == 3 * beat &&
                  note(3).startTicks == beat && undoIs("Fit to Time Range") && undo() && panel.chooseDuration(2),
              "Time Range in the Duration chooser: Set Length fits the selected notes to the range");
        ed.select({});
    }
}

void clipViewMulti(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "clip-view-multi.adi", err);
    check(store != nullptr, "the multi-clip project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clip View Multi')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole, beat = textproj::kPPQ;
    auto op = [](const char *type, Payload payload, const char *label) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(payload);
        r.label = label;
        return r;
    };
    bool made = true;
    for (const auto &[id, name] : {std::pair<std::int64_t, const char *>{1, "Keys"}, {2, "Bass"}, {3, "Drums"}})
        made = submitter.submit(op("track.create", {{"id", id}, {"kind", id == 3 ? "audio" : "midi"}, {"name", name}, {"index", id - 1}}, "Create track")).ok && made;
    // Keys: two bars from the start, a C3. Bass: two bars from bar 2, an E2 on its first beat. An audio clip.
    made = submitter.submit(op("clip.create", {{"id", 11}, {"track", 1}, {"kind", "midi"}, {"name", "Chords"}, {"pos", 0}, {"length", 2 * bar}}, "Create clip")).ok && made;
    made = submitter.submit(op("clip.create", {{"id", 12}, {"track", 2}, {"kind", "midi"}, {"name", "Line"}, {"pos", bar}, {"length", 2 * bar}}, "Create clip")).ok && made;
    made = submitter.submit(op("clip.create", {{"id", 13}, {"track", 3}, {"kind", "audio"}, {"name", "Loop"}, {"pos", 0}, {"length", bar}}, "Create clip")).ok && made;
    made = submitter.submit(std::vector<OpRequest>{
               op("note.insert", {{"clip", 11}, {"note", 1}, {"start", 0}, {"dur", beat}, {"key", 60}}, "Add Note"),
               op("note.insert", {{"clip", 12}, {"note", 1}, {"start", 0}, {"dur", beat}, {"key", 52}}, "Add Note")}).ok && made;
    check(made, "two MIDI tracks with a clip each, and an audio clip");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "clip-view-multi");
    commands.activate(root);
    root.setSize(1400, 900);
    for (std::int64_t id = 1; id <= 3; ++id)
        root.arrangement.geometry.resizeTrack(id, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    auto &panel = root.clipView;
    auto &ed = panel.notes;
    canvas.mouseDoubleClick(pressAt(canvas, {static_cast<float>(geo.x(0)) + 20.f, static_cast<float>(laneTop(root, 1) + 30)}));
    root.frame();
    check(panel.clip() == 11 && panel.others().empty() && ed.barsHeight() == 0 && panel.focusBox().isEmpty() &&
              ed.rulerArea().getY() == 0,
          "one clip open: no loop bars, no Focus switch");
    const auto oneScale = ed.time.scale;

    // A second MIDI clip and an audio clip picked with it: both MIDI clips are shown, the audio clip is not.
    geo.selection.pickedClips = {12, 13};
    root.frame();
    check(panel.clip() == 11 && panel.others().size() == 1 && panel.others()[0].details.id == 12 &&
              panel.others()[0].colour != panel.colour(),
          "with a second MIDI clip selected, both are shown, each in its own track's colour; the edited clip stays");
    check(ed.barsHeight() == 2 * NoteEditor::kLoopBarHeight + 2 && ed.rulerArea().getY() == ed.barsHeight() &&
              ed.notesArea().getY() == ed.barsHeight() + NoteEditor::kRulerHeight + NoteEditor::kStretchHeight && !panel.focusBox().isEmpty(),
          "a loop bar for each clip sits above the ruler, the editor moves down under them, and the Focus switch appears");
    const auto own = ed.loopBar(0), other = ed.loopBar(1);
    check(own.getX() == NoteEditor::kKeysWidth + static_cast<int>(ed.time.x(0)) &&
              other.getX() == NoteEditor::kKeysWidth + static_cast<int>(ed.time.x(bar)) && other.getY() > own.getY() &&
              std::abs(other.getWidth() - own.getWidth()) <= 1,
          "each bar lies across its clip's place in the song");
    check(ed.time.scale < oneScale && ed.time.x(0) == 0. && ed.time.x(3 * bar) <= ed.notesArea().getWidth(),
          "the editor shows from the first clip's start to the last clip's end");
    const auto &line = panel.others()[0].details;
    const auto low = ed.boxOf(line.notes[0], line);
    const auto high = ed.boxOf(panel.details()->notes[0]);
    check(low && high && std::abs(low->x - (ed.notesArea().getX() + ed.time.x(bar))) < 0.01 && low->y > high->y,
          "the other clip's note is drawn where its own clip puts it in the song");

    // A click on the other clip's note makes that clip the one being edited, the note selected; edits go to it.
    const auto before = submitter.nextUndo()->txnId;
    const juce::Point<float> onLow(static_cast<float>(static_cast<int>(low->x + low->width / 2)), static_cast<float>(low->y + low->height / 2));
    ed.mouseDown(pressAt(ed, onLow));
    ed.mouseUp(pressAt(ed, onLow));
    check(panel.clip() == 12 && panel.others().size() == 1 && panel.others()[0].details.id == 11 &&
              ed.selected() == std::vector<std::uint64_t>{1} && submitter.nextUndo()->txnId == before &&
              std::string(panel.details()->name) == "Line",
          "a click on the other clip's note makes that clip the edited one, its note selected, nothing written");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && panel.details()->notes[0].key == 53 &&
              panel.others()[0].details.notes[0].key == 60 && submitter.nextUndo()->label == "Move Note",
          "Up then moves that clip's note and leaves the other clip's");
    // Its loop bar does the same for the first clip.
    const auto barOfFirst = ed.loopBar(1).getCentre().toFloat();
    ed.mouseDown(pressAt(ed, barOfFirst));
    ed.mouseUp(pressAt(ed, barOfFirst));
    check(panel.clip() == 11 && panel.others()[0].details.id == 12, "a click on a clip's loop bar makes it the edited clip");

    // With Focus off, notes of both clips are selected and edited in one gesture, one undo step.
    const auto mine = [&]() -> const engine::ClipNote & { return panel.details()->notes[0]; };
    const auto theirs = [&]() -> const engine::ClipNote & { return panel.others()[0].details.notes[0]; };
    const auto all = juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0);
    check(ed.gridTicks() == beat && ed.keyPressed(all) && ed.selected() == std::vector<std::uint64_t>{1} &&
              ed.selectedIn(12) == std::vector<std::uint64_t>{1} && ed.anySelected(),
          "Ctrl/Cmd+A selects the notes of both clips");
    const auto mineStart = mine().startTicks, theirStart = theirs().startTicks;
    const int mineKey = mine().key, theirKey = theirs().key;
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && mine().startTicks == mineStart + beat &&
              theirs().startTicks == theirStart + beat && submitter.nextUndo()->opCount == 2,
          "Right moves both clips' notes a grid step later, as one undo step of two ops");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)) && mine().key == mineKey + 12 &&
              theirs().key == theirKey + 12 && ed.selectedIn(12).size() == 1,
          "Shift+Up moves both an octave up, and both stay selected");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "two undos");
    root.frame();
    check(mine().startTicks == mineStart && theirs().startTicks == theirStart && mine().key == mineKey && theirs().key == theirKey,
          "put both clips' notes back");
    // A drag of the edited clip's note takes the other clip's selected note with it.
    check(ed.keyPressed(all), "both selected again");
    const auto grabBox = *ed.boxOf(mine());
    const juce::Point<float> grab(static_cast<float>(static_cast<int>(grabBox.x + grabBox.width / 2)), static_cast<float>(grabBox.y + grabBox.height / 2));
    const float stepPx = static_cast<float>(std::lround(ed.time.scale));
    ed.mouseDown(pressAt(ed, grab));
    ed.mouseDrag(dragAt(ed, grab, {grab.x + stepPx, grab.y}));
    check(ed.shown(mine()).startTicks == mineStart + beat && ed.shownIn(theirs(), 12).startTicks == theirStart + beat &&
              theirs().startTicks == theirStart,
          "while the drag is held both clips' notes are drawn a step later, nothing written");
    ed.mouseUp(pressAt(ed, {grab.x + stepPx, grab.y}));
    check(mine().startTicks == mineStart + beat && theirs().startTicks == theirStart + beat && submitter.nextUndo()->opCount == 2,
          "let go, both have moved: one undo step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // Delete takes the selected notes of both clips; one Undo brings both back.
    check(ed.keyPressed(all) && ed.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)) && panel.details()->notes.empty() &&
              panel.others()[0].details.notes.empty() && submitter.nextUndo()->opCount == 2 && !ed.anySelected(),
          "Delete removes the selected notes of both clips, one undo step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(panel.details()->notes.size() == 1 && panel.others()[0].details.notes.size() == 1, "and one Undo brings both back");
    // Shift and a click on another clip's note adds it to the selection without changing the edited clip.
    const auto theirBox = *ed.boxOf(theirs(), panel.others()[0].details);
    const juce::Point<float> onTheirs(static_cast<float>(static_cast<int>(theirBox.x + theirBox.width / 2)),
                                      static_cast<float>(theirBox.y + theirBox.height / 2));
    const juce::ModifierKeys shiftKey(juce::ModifierKeys::shiftModifier);
    ed.mouseDown(pressAt(ed, onTheirs, shiftKey));
    ed.mouseUp(pressAt(ed, onTheirs, shiftKey));
    check(panel.clip() == 11 && ed.selectedIn(12) == std::vector<std::uint64_t>{1} && ed.selected().empty(),
          "Shift and a click on the other clip's note selects it; the edited clip stays");
    ed.mouseDown(pressAt(ed, onTheirs, shiftKey));
    ed.mouseUp(pressAt(ed, onTheirs, shiftKey));
    check(ed.selectedIn(12).empty() && !ed.anySelected(), "and again takes it out");
    // With Focus on, only the edited clip's notes are selected and edited.
    panel.setFocus(true);
    check(ed.keyPressed(all) && ed.selected().size() == 1 && ed.selectedIn(12).empty(), "with Focus on, Ctrl/Cmd+A selects the edited clip's notes only");
    check(ed.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && mine().startTicks == mineStart + beat && theirs().startTicks == theirStart &&
              submitter.nextUndo()->opCount == 1,
          "and Right moves them only");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    panel.setFocus(false);
    ed.select({});

    // Focus: N, or the header's switch.
    check(!panel.focus() && ed.keyPressed(juce::KeyPress('n')) && panel.focus(), "N turns Focus on");
    panel.mouseDown(pressAt(panel, panel.focusBox().getCentre().toFloat()));
    panel.mouseUp(pressAt(panel, panel.focusBox().getCentre().toFloat()));
    check(!panel.focus(), "and the header's Focus switch turns it off");

    // Fold keeps the key tracks either clip uses.
    ed.setFold(true);
    check(ed.keys.rowCount() == 2 && ed.keys.rowOf(60) == 0 && ed.keys.rowOf(53) == 1, "Fold keeps the key tracks any shown clip uses");
    ed.setFold(false);

    // The panels act on every shown clip; the title counts them; notes copied from several go back each into its own (manual 10.8).
    {
        using Field = ClipViewPanel::Field;
        const auto edited = [&]() -> const engine::ClipDetails & { return *panel.details(); };
        const auto another = [&]() -> const engine::ClipDetails & { return panel.others()[0].details; };
        const auto command = juce::ModifierKeys::commandModifier;
        ed.select({});
        check(panel.others().size() == 1 && panel.readings().title == "2 MIDI Clips" && !panel.readings().loopMixed && panel.readings().scale != "*",
              "with two clips shown the title reads their number; they agree on looping and on the scale");
        const bool looped = edited().loopEnabled;
        check(panel.toggleLoop() && edited().loopEnabled == !looped && another().loopEnabled == !looped && submitter.nextUndo()->opCount == 2,
              "the Loop switch switches both clips: one undo step");
        check(submitter.submit(op("clip.setLoop", {{"id", another().id}, {"on", false}}, "Deactivate Loop")).ok, "the other clip's loop switched off alone");
        root.frame();
        if (edited().loopEnabled) {
            check(panel.readings().loopMixed, "one looped and one not: the Loop button is half lit");
            check(panel.toggleLoop() && !edited().loopEnabled && !another().loopEnabled && !panel.readings().loopMixed && submitter.nextUndo()->opCount == 1,
                  "switched off, only the clip that still looped is written");
        }
        check(panel.typeField(Field::Signature, "3/4") && edited().signature.numerator == 3 && another().signature.numerator == 3 &&
                  another().signature.local && submitter.nextUndo()->opCount == 2,
              "a time signature typed is both clips'");
        check(panel.chooseScale(true, 3) && edited().scale.root == 2 && another().scale.root == 2 && another().scale.local && edited().scale.local,
              "a root chosen is both clips'");
        check(submitter.submit(op("clip.setScale", {{"id", another().id}, {"root", 7}, {"scale", "major"}, {"mask", 2741}}, "Change Clip Scale")).ok,
              "the other clip given a scale of its own");
        root.frame();
        check(panel.readings().scale == "*", "the clips disagree on the scale: an asterisk");
        check(panel.chooseScale(false, 1) && another().scale.root == edited().scale.root && another().scale.id == edited().scale.id &&
                  panel.readings().scale != "*",
              "a scale chosen brings them together again");
        panel.mouseDown(pressAt(panel, panel.scaleModeBox().getCentre().toFloat()));
        panel.mouseUp(pressAt(panel, panel.scaleModeBox().getCentre().toFloat()));
        check(!edited().scale.local && !another().scale.local && submitter.nextUndo()->label == "Deactivate Scale" && submitter.nextUndo()->opCount == 2,
              "Scale Mode switched off is switched off in both");

        // Copy from both clips; paste goes back into both.
        check(ed.keyPressed(juce::KeyPress('a', command, 0)) && ed.anySelected() && ed.keyPressed(juce::KeyPress('c', command, 0)) &&
                  panel.noteClipboard.size() == edited().notes.size() + another().notes.size() &&
                  panel.noteClipboardClips.size() == panel.noteClipboard.size() && panel.noteClipboardShown == panel.shownClips(),
              "every note of both clips copied, each remembered with its clip");
        const auto ownCount = edited().notes.size(), otherCount = another().notes.size();
        const auto firstClip = edited().id, secondClip = another().id;
        ed.selectTime(2 * bar + beat, 2 * bar + beat);
        check(ed.keyPressed(juce::KeyPress('v', command, 0)) && edited().notes.size() == 2 * ownCount && another().notes.size() == 2 * otherCount &&
                  submitter.nextUndo()->label == "Paste Notes" && ed.selected().size() == ownCount,
              "Paste puts each note back into its own clip, at the insert marker: one undo step, the pasted notes selected");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        check(edited().notes.size() == ownCount && another().notes.size() == otherCount, "one Undo takes both clips' pasted notes back");
        // After another clip is chosen, the paste goes into that one.
        check(panel.activate(secondClip) && edited().id == secondClip, "the other clip chosen as the one being edited");
        ed.selectTime(2 * bar + beat, 2 * bar + beat);
        check(ed.keyPressed(juce::KeyPress('v', command, 0)) && edited().notes.size() == otherCount + ownCount + otherCount && another().notes.size() == ownCount,
              "now Paste puts every copied note into the clip being edited");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        check(panel.activate(firstClip) && edited().id == firstClip, "the first clip is the edited one again");
        ed.select({});
    }

    // The title bar's context menu (manual 8.1.1, 10.8): Rename and the colour, for every shown clip.
    {
        const auto edited = [&]() -> const engine::ClipDetails & { return *panel.details(); };
        const auto another = [&]() -> const engine::ClipDetails & { return panel.others()[0].details; };
        check(panel.titleMenu().getNumItems() == 4 && panel.chooseTitleItem(1),
              "the title's menu: Rename, Color, Arrange Clip View Panels Automatically and Fold; Rename opens the name for typing");
        check(panel.typeField(ClipViewPanel::Field::Name, "  Verse A ") && edited().name == "Verse A" && another().name == "Verse A" &&
                  submitter.nextUndo()->label == "Rename Clip" && submitter.nextUndo()->opCount == 2 && panel.readings().title == "2 MIDI Clips",
              "a name typed names both shown clips, without the spaces at its ends: one undo step");
        const auto renamed = submitter.nextUndo()->txnId;
        check(panel.rename("Verse A") && submitter.nextUndo()->txnId == renamed, "the same name again writes nothing");
        check(panel.chooseTitleItem(105) && edited().color == 5 && another().color == 5 && submitter.nextUndo()->label == "Change Clip Color" &&
                  submitter.nextUndo()->opCount == 2 && !panel.chooseTitleItem(100 + ClipViewPanel::kTitleColours) && !panel.chooseTitleItem(4),
              "a colour chosen colours both: one undo step");
        root.frame();
        check(panel.colour() == look::paletteColour(5) && panel.others()[0].colour == look::paletteColour(5), "and the editor draws both clips in it");
    }

    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-clip-view-multi.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }

    // With only one clip selected again, the editor is as it was.
    geo.selection.pickedClips.clear();
    root.frame();
    check(panel.clip() == 11 && panel.others().empty() && ed.barsHeight() == 0 && ed.rulerArea().getY() == 0,
          "one clip selected again: one clip shown, no loop bars");
    check(!ed.keyPressed(juce::KeyPress('n')), "and N is not the editor's with one clip");
    check(panel.readings().title == "Verse A" && panel.rename("") && panel.details()->name.empty() && panel.readings().title == "Keys",
          "one clip shown: the title is its name; a clip with no name reads its track's");
}
// Live's time selection across tracks (manual 6.9): a drag across lanes reaches every track between,
// Shift+click and Shift+Down extend it, it is drawn on every selected lane, and Delete and 0 act on
// the clips inside it on all of them, as one undo step each.
// Live's clip drag across tracks (Live 12 manual 6.7, "to another song position or track"): a clip
// dragged by its bar follows the lane under the mouse when that track can take it, MIDI to MIDI and
// audio to audio, and lands there as one undo step; over a track that cannot, it stays on the last
// that could.
void clipToTrack(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "clip-to-track.adi", err);
    check(store != nullptr, "the clip drag's project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Clip To Track')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole;
    bool madeAll = true;
    for (const auto &[id, kind] : {std::pair<std::int64_t, const char *>{1, "midi"}, {2, "midi"}, {3, "audio"}}) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", kind == std::string("midi") ? "# MIDI" : "# Audio"}, {"index", id - 1}};
        r.label = "Create track";
        madeAll = submitter.submit(std::move(r)).ok && madeAll;
    }
    OpRequest clip;
    clip.opType = "clip.create";
    clip.payload = {{"id", 11}, {"track", 1}, {"kind", "midi"}, {"pos", bar}, {"length", bar}};
    clip.label = "Create clip";
    madeAll = submitter.submit(std::move(clip)).ok && madeAll;
    check(madeAll, "two MIDI tracks and an audio track; a MIDI clip in bar 2 of the first");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "clip-to-track");
    root.setSize(1200, 720);
    for (std::int64_t id = 1; id <= 3; ++id)
        root.arrangement.geometry.resizeTrack(id, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    const auto x = [&geo](std::int64_t tick) { return static_cast<float>(geo.x(tick)); };
    const auto barOf = [&root](std::int64_t id) { return static_cast<float>(laneTop(root, id) + 5); };
    const auto lane = [&root](std::int64_t id) { return static_cast<float>(laneTop(root, id) + 30); };
    auto trackOf = [&root](std::int64_t clipId) -> std::int64_t {
        for (const auto &t : root.reader().tracks())
            for (const auto &c : t->clips)
                if (c->id == clipId)
                    return t->id;
        return 0;
    };
    // Its bar picked, then dragged down onto 2 MIDI's lane: drawn there while dragging, landing there
    // on release, at the same place in time.
    canvas.mouseDown(pressAt(canvas, {x(bar) + 6, barOf(1)}));
    canvas.mouseUp(pressAt(canvas, {x(bar) + 6, barOf(1)}));
    canvas.mouseDown(pressAt(canvas, {x(bar) + 6, barOf(1)}));
    canvas.mouseDrag(pressAt(canvas, {x(bar) + 6, lane(2)}));
    check(canvas.movingClip() == 11 && canvas.movingTrack() == 2, "dragged over 2 MIDI, the clip would land there");
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-clip-to-track.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    // Over the audio track it stays on 2 MIDI: Live keeps MIDI clips on MIDI tracks.
    canvas.mouseDrag(pressAt(canvas, {x(bar) + 6, lane(3)}));
    check(canvas.movingTrack() == 2, "over the audio track it stays on the last MIDI track");
    canvas.mouseUp(pressAt(canvas, {x(bar) + 6, lane(3)}));
    root.frame();
    check(trackOf(11) == 2 && root.reader().findTrack(2)->clips.front()->posTicks == bar &&
              submitter.nextUndo()->label == "Move Clip" && submitter.nextUndo()->opCount == 1 &&
              geo.selection.track == 2 && geo.selection.clip == 11,
          "released, the clip is on 2 MIDI at bar 2, one undo step, and the selection follows it");
    // Back up to 1 MIDI and a bar later: track and time in one step.
    canvas.mouseDown(pressAt(canvas, {x(bar) + 6, barOf(2)}));
    canvas.mouseDrag(pressAt(canvas, {x(2 * bar) + 6, barOf(1)}));
    canvas.mouseUp(pressAt(canvas, {x(2 * bar) + 6, barOf(1)}));
    root.frame();
    check(trackOf(11) == 1 && root.reader().findTrack(1)->clips.front()->posTicks == 2 * bar,
          "dragged up and a bar on, it moves track and time in one step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(trackOf(11) == 2 && root.reader().findTrack(2)->clips.front()->posTicks == bar, "and undo puts it back");
}
void rangeSelection(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "range.adi", err);
    check(store != nullptr, "the range's project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Range')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    const auto bar = textproj::kWhole;
    bool madeAll = true;
    for (std::int64_t id = 1; id <= 4; ++id) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", "midi"}, {"name", "# MIDI"}, {"index", id - 1}};
        r.label = "Create track";
        madeAll = submitter.submit(std::move(r)).ok && madeAll;
    }
    // A clip in bar 2 on each of the first three tracks; the fourth's runs on past bar 3.
    for (std::int64_t id = 1; id <= 4; ++id) {
        OpRequest r;
        r.opType = "clip.create";
        r.payload = {{"id", 10 + id}, {"track", id}, {"kind", "midi"}, {"pos", bar}, {"length", id == 4 ? 2 * bar : bar}};
        r.label = "Create clip";
        madeAll = submitter.submit(std::move(r)).ok && madeAll;
    }
    check(madeAll, "four MIDI tracks, a clip on each");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "range");
    root.setSize(1200, 720);
    for (std::int64_t id = 1; id <= 4; ++id)
        root.arrangement.geometry.resizeTrack(id, 48);
    root.mark(DirtySet::All);
    root.frame();
    auto &geo = root.arrangement.geometry;
    auto &canvas = root.arrangement.canvas;
    const auto x = [&geo](std::int64_t tick) { return static_cast<float>(geo.x(tick)); };
    const auto lane = [&root](std::int64_t id) { return static_cast<float>(laneTop(root, id) + 30); };
    // From bar 1 on track 1, dragged to bar 3 on track 3: bars 1 to 3 on tracks 1, 2 and 3. A forward
    // selection snaps up to the next grid line, so the mouse lets go just short of bar 3's.
    canvas.mouseDown(pressAt(canvas, {x(0) + 1, lane(1)}));
    canvas.mouseDrag(pressAt(canvas, {x(2 * bar) - 1, lane(3)}));
    canvas.mouseUp(pressAt(canvas, {x(2 * bar) - 1, lane(3)}));
    root.frame();
    check(geo.selection.track == 1 && geo.selection.toTrack == 3 && geo.selection.start == 0 &&
              geo.selection.end == 2 * bar && geo.inSelection(root.reader(), 2) && !geo.inSelection(root.reader(), 4),
          "dragged across lanes, the selection reaches every track between");
    // Drawn on every selected lane, not on the fourth.
    juce::Image image(juce::Image::ARGB, canvas.getWidth(), canvas.getHeight(), true, juce::SoftwareImageType{});
    {
        juce::Graphics g(image);
        canvas.paintEntireComponent(g, true);
    }
    const int px = static_cast<int>(x(bar / 2));
    const auto lit = image.getPixelAt(px, static_cast<int>(lane(2))), unlit = image.getPixelAt(px, static_cast<int>(lane(4)));
    check(lit != unlit && image.getPixelAt(px, static_cast<int>(lane(1))) == lit &&
              image.getPixelAt(px, static_cast<int>(lane(3))) == lit,
          "the time selection is drawn on each selected track's lane, as Live's");
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-range.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    // 0 deactivates the three clips inside; the fourth track is not selected.
    check(canvas.keyPressed(juce::KeyPress('0')), "0");
    root.frame();
    auto clipOf = [&root](std::int64_t track) { return root.reader().findTrack(track)->clips.front(); };
    check(clipOf(1)->muted && clipOf(2)->muted && clipOf(3)->muted && !clipOf(4)->muted &&
              submitter.nextUndo()->label == "Deactivate Clips" && submitter.nextUndo()->opCount == 3,
          "0 deactivates the material on every selected track, one undo step");
    // Shift+Down reaches the fourth track; its clip crosses the end, so 0 leaves it and says why.
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0)) &&
              geo.selection.toTrack == 4 && geo.selection.track == 1,
          "Shift+Down reaches one track further");
    check(canvas.keyPressed(juce::KeyPress('0')), "0 again");
    root.frame();
    check(!clipOf(1)->muted && !clipOf(4)->muted && root.error().find("1 clip crosses") != std::string::npos,
          "the three inside are activated; the one across the edge stays, and the Status Bar says why");
    // Shift+click on track 2 at bar 2 pulls the selection back to tracks 1 and 2, bars 1 to 2.
    canvas.mouseDown(pressAt(canvas, {x(bar) - 1, lane(2)}, juce::ModifierKeys::shiftModifier));
    canvas.mouseUp(pressAt(canvas, {x(bar) - 1, lane(2)}, juce::ModifierKeys::shiftModifier));
    check(geo.selection.track == 1 && geo.selection.toTrack == 2 && geo.selection.end == bar,
          "Shift+click extends or shortens the selection, across tracks too");
    // Delete on bars 1 to 3 of tracks 1 to 3: their clips go, as one step.
    canvas.mouseDown(pressAt(canvas, {x(0) + 1, lane(1)}));
    canvas.mouseDrag(pressAt(canvas, {x(2 * bar) - 1, lane(3)}));
    canvas.mouseUp(pressAt(canvas, {x(2 * bar) - 1, lane(3)}));
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "Delete");
    root.frame();
    check(root.reader().findTrack(1)->clips.empty() && root.reader().findTrack(3)->clips.empty() &&
              root.reader().findTrack(4)->clips.size() == 1 && submitter.nextUndo()->label == "Delete Clips",
          "Delete removes the material on every selected track");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(root.reader().findTrack(1)->clips.size() == 1 && root.reader().findTrack(2)->clips.size() == 1,
          "brings it all back");
    // Select All (Ctrl/Cmd+A, §42.5): all the material, every track holding clips, from the earliest
    // clip's start to the latest's end; a text field with the focus keeps its own Ctrl/Cmd+A.
    check(root.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)) && geo.selection.track == 1 &&
              geo.selection.toTrack == 4 && geo.selection.start == bar && geo.selection.end == 3 * bar &&
              geo.selection.clip == 0 && geo.inSelection(root.reader(), 3),
          "Select All selects every track's material, bar 2 to the fourth track's end");
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        root.frame();
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-select-all.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    check(canvas.keyPressed(juce::KeyPress('0')), "0 on all of it");
    root.frame();
    check(clipOf(1)->muted && clipOf(4)->muted && submitter.nextUndo()->opCount == 4, "deactivates every clip, one step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // A plain click starts over: one track.
    canvas.mouseDown(pressAt(canvas, {x(bar) + 1, lane(4)}));
    canvas.mouseUp(pressAt(canvas, {x(bar) + 1, lane(4)}));
    check(geo.selection.track == 4 && geo.selection.toTrack == 0 && !geo.inSelection(root.reader(), 1),
          "a plain click selects in one track again");
    // Live's "Select Multiple Items" (Ctrl/Cmd+click, §42.5) on clips: picked one by one, apart from
    // one another; 0 and Delete act on them all, one undo step each; a plain click lets them go.
    {
        const auto pick = juce::ModifierKeys::commandModifier;
        auto clickClip = [&](std::int64_t track, juce::ModifierKeys mods = {}) {
            const juce::Point<float> at(x(bar) + 10, lane(track));
            canvas.mouseDown(pressAt(canvas, at, mods));
            canvas.mouseUp(pressAt(canvas, at, mods));
        };
        clickClip(1);
        clickClip(3, pick);
        root.frame();
        check(geo.selection.clip == 11 && geo.selection.pickedClips == std::vector<std::int64_t>{13} &&
                  geo.clipSelected(13) && !geo.clipSelected(12),
              "the clip on track 1 clicked, then Ctrl/Cmd+click on track 3's: both selected, track 2's not");
        if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty()) {
            juce::Image shot(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics g(shot);
                g.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(g, true);
            }
            const auto file = juce::File(pictures).getChildFile("adi-picked-clips.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(shot, *stream);
        }
        check(canvas.keyPressed(juce::KeyPress('0')), "0");
        root.frame();
        check(clipOf(1)->muted && clipOf(3)->muted && !clipOf(2)->muted && submitter.nextUndo()->label == "Deactivate Clips" &&
                  submitter.nextUndo()->opCount == 2,
              "0 deactivates both picked clips, one step; the one between stays");
        check(canvas.keyPressed(juce::KeyPress('0')), "0 again");
        root.frame();
        check(!clipOf(1)->muted && !clipOf(3)->muted && submitter.nextUndo()->label == "Activate Clips", "activates them");
        clickClip(3, pick);
        check(geo.selection.clip == 11 && geo.selection.pickedClips.empty(), "Ctrl/Cmd+click on track 3's again lets it go");
        clickClip(2, pick);
        clickClip(4, pick);
        check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "Delete");
        root.frame();
        check(root.reader().findTrack(1)->clips.empty() && root.reader().findTrack(2)->clips.empty() &&
                  root.reader().findTrack(4)->clips.empty() && root.reader().findTrack(3)->clips.size() == 1 &&
                  submitter.nextUndo()->label == "Delete Clips" && geo.selection.clip == 0 && geo.selection.pickedClips.empty(),
              "Delete removes the three picked, one step; the one left out stays, and nothing is selected");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        check(root.reader().findTrack(1)->clips.size() == 1 && root.reader().findTrack(2)->clips.size() == 1 &&
                  root.reader().findTrack(4)->clips.size() == 1,
              "brings the three back");
        clickClip(2);
        check(geo.selection.clip == 12 && geo.selection.pickedClips.empty(), "a plain click selects one clip again");
        // Dragged together (§6.7): the picked clips follow the one dragged by its bar, in time, on
        // their own tracks; one undo step. A press on a picked clip's bar without a drag selects it alone.
        clickClip(1);
        clickClip(3, pick);
        const juce::Point<float> grab(x(bar) + 10, static_cast<float>(laneTop(root, 3) + 4));
        const juce::Point<float> drop(x(2 * bar) + 10, grab.y);
        canvas.mouseDown(pressAt(canvas, grab));
        canvas.mouseDrag(dragAt(canvas, grab, drop));
        const bool along = geo.selection.pickedClips == std::vector<std::int64_t>{13} && canvas.movingClip() == 13;
        if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty()) {
            juce::Image shot(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics g(shot);
                g.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(g, true);
            }
            const auto file = juce::File(pictures).getChildFile("adi-picked-clips-drag.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(shot, *stream);
        }
        canvas.mouseUp(dragAt(canvas, grab, drop));
        root.frame();
        check(along && clipOf(1)->posTicks == 2 * bar && clipOf(3)->posTicks == 2 * bar && clipOf(2)->posTicks == bar &&
                  submitter.nextUndo()->label == "Move Clips",
              "track 3's clip dragged a bar later by its bar, track 1's picked: both move a bar, one step");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        check(clipOf(1)->posTicks == bar && clipOf(3)->posTicks == bar, "puts them back");
        clickClip(1);
        clickClip(3, pick);
        canvas.mouseDown(pressAt(canvas, grab));
        canvas.mouseUp(pressAt(canvas, grab));
        check(geo.selection.clip == 13 && geo.selection.pickedClips.empty(),
              "a press on a picked clip's bar without a drag selects that clip alone");
        // Across tracks, each by the same rows: track 1's and 2's dragged a row down by track 2's bar.
        clickClip(1);
        clickClip(2, pick);
        const juce::Point<float> grab2(x(bar) + 10, static_cast<float>(laneTop(root, 2) + 4));
        const juce::Point<float> below(x(bar) + 10, static_cast<float>(laneTop(root, 3) + 4));
        canvas.mouseDown(pressAt(canvas, grab2));
        canvas.mouseDrag(dragAt(canvas, grab2, below));
        if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty()) {
            juce::Image shot(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics g(shot);
                g.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(g, true);
            }
            const auto file = juce::File(pictures).getChildFile("adi-picked-clips-across.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(shot, *stream);
        }
        canvas.mouseUp(dragAt(canvas, grab2, below));
        root.frame();
        auto onTrack = [&root](std::int64_t track) {
            std::vector<std::int64_t> out;
            for (const auto &c : root.reader().findTrack(track)->clips)
                out.push_back(c->id);
            std::sort(out.begin(), out.end());
            return out;
        };
        check(onTrack(1).empty() && onTrack(2) == std::vector<std::int64_t>{11} && onTrack(3) == std::vector<std::int64_t>{12} &&
                  submitter.nextUndo()->label == "Move Clips" && geo.selection.track == 3,
              "both a row down: track 1's on track 2, track 2's on track 3, one step; 13, which 12 now covers whole, "
              "goes, as in Live (DF-W28); the selection follows the one dragged");
        // DF-W28: a clip moved partly over another of its new track cuts the covered part off; one Undo puts
        // both back.
        check(root.command(AppCommands::Undo), "undo the row-down move");
        root.frame();
        check(onTrack(1) == std::vector<std::int64_t>{11} && onTrack(2) == std::vector<std::int64_t>{12},
              "puts them back, 13 with them");
        const auto *thirteen = [&]() -> const engine::ClipNode * {
            for (const auto &c : root.reader().findTrack(3)->clips)
                if (c->id == 13)
                    return c.get();
            return nullptr;
        }();
        check(thirteen != nullptr, "13 is back on track 3");
        if (thirteen != nullptr) {
            const auto start = thirteen->posTicks, length = thirteen->lengthTicks;
            check(root.placeClips({{12, 3, start + length / 2}}, "Move Clip"), "12 onto the second half of 13");
            root.frame();
            const engine::ClipNode *after = nullptr;
            for (const auto &c : root.reader().findTrack(3)->clips)
                if (c->id == 13)
                    after = c.get();
            check(after != nullptr && after->posTicks == start && after->lengthTicks == length / 2,
                  "13 keeps the part 12 does not cover; its covered half goes");
            check(root.command(AppCommands::Undo), "undo");
            root.frame();
            bool whole = false;
            for (const auto &c : root.reader().findTrack(3)->clips)
                whole = whole || (c->id == 13 && c->posTicks == start && c->lengthTicks == length);
            check(whole, "one Undo gives 13 back whole");
            // Where the planner does not place yet (here a track with an automation lane, AU-05), the clip
            // moves as before, keeping what it lands on, and the Status Bar says why.
            automation::LaneSpec lane;
            lane.id = 77;
            lane.ownerKind = "track";
            lane.ownerId = 3;
            lane.paramRef = "volume";
            lane.paramName = "Volume";
            check(submitter.submit(automation::createLane(lane)).ok, "an automation lane on track 3");
            root.frame();
            check(root.placeClips({{12, 3, start + length / 2}}, "Move Clip") &&
                      root.error().rfind("Moved, keeping what the clips land on", 0) == 0,
                  "with an automation lane the move still goes, saying it keeps what it lands on");
            root.frame();
            bool kept = false, moved = false;
            for (const auto &c : root.reader().findTrack(3)->clips) {
                kept = kept || (c->id == 13 && c->lengthTicks == length);
                moved = moved || (c->id == 12 && c->posTicks == start + length / 2);
            }
            check(kept && moved, "12 moved, 13 kept whole");
            check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "undo the move and the lane");
            root.frame();
        }
        // A row up, track 1's would leave the lanes: they keep their tracks, and move in time.
        clickClip(1);
        clickClip(2, pick);
        const juce::Point<float> above(x(2 * bar) + 10, static_cast<float>(laneTop(root, 1) + 4));
        canvas.mouseDown(pressAt(canvas, grab2));
        canvas.mouseDrag(dragAt(canvas, grab2, above));
        canvas.mouseUp(dragAt(canvas, grab2, above));
        root.frame();
        check(onTrack(1) == std::vector<std::int64_t>{11} && onTrack(2) == std::vector<std::int64_t>{12} &&
                  clipOf(1)->posTicks == 2 * bar,
              "a row up, track 1's would leave the lanes: they keep their tracks and move in time");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        clickClip(4);
    }
    // Live's Ctrl/Cmd+click on track titles picks tracks apart from one another; Solo and Group Tracks
    // take them all; a plain click lets them go.
    auto &headers = root.arrangement.headers;
    const auto title = [&root](std::int64_t id) { return juce::Point<float>{20.f, static_cast<float>(laneTop(root, id) + 8)}; };
    const auto cmd = juce::ModifierKeys::commandModifier;
    headers.mouseDown(pressAt(headers, title(1)));
    headers.mouseUp(pressAt(headers, title(1)));
    headers.mouseDown(pressAt(headers, title(3), cmd));
    headers.mouseUp(pressAt(headers, title(3), cmd));
    check(geo.selectedTracks(root.reader()) == std::vector<std::int64_t>{1, 3} && !geo.inSelection(root.reader(), 2),
          "1 MIDI, then Ctrl/Cmd+click on 3: the two selected, 2 not");
    if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty()) {
        root.frame();
        juce::Image picked(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picked);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(pictures).getChildFile("adi-picked-tracks.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picked, *stream);
    }
    check(root.keyPressed(juce::KeyPress('s', 0, 0)), "S");
    root.frame();
    auto soloedOnes = [&root] {
        std::vector<std::int64_t> out;
        for (const auto &t : root.reader().tracks())
            if (t->soloed)
                out.push_back(t->id);
        return out;
    };
    check(soloedOnes() == std::vector<std::int64_t>{1, 3}, "S solos both picked tracks, one step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(root.keyPressed(juce::KeyPress('g', cmd, 0)), "Ctrl/Cmd+G");
    root.frame();
    const auto *group = root.reader().findTrack(5);
    check(group != nullptr && group->kind == "group" && root.reader().findTrack(1)->parent == 5 &&
              root.reader().findTrack(3)->parent == 5 && root.reader().findTrack(2)->parent == 0,
          "Group Tracks puts the two picked tracks in one group, 2 MIDI left outside");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // Clicks: a plain press on one of several selected tracks selects it alone on release.
    auto click = [&headers](juce::Point<float> at, juce::ModifierKeys mods = {}) {
        headers.mouseDown(pressAt(headers, at, mods));
        headers.mouseUp(pressAt(headers, at, mods));
    };
    click(title(1));
    click(title(3), cmd);
    click(title(3), cmd);
    check(geo.selectedTracks(root.reader()) == std::vector<std::int64_t>{1}, "Ctrl/Cmd+click again lets 3 go");
    click(title(3), cmd);
    click(title(2));
    check(geo.selectedTracks(root.reader()) == std::vector<std::int64_t>{2} && geo.selection.picked.empty(),
          "a plain click on 2 selects it alone");
}
// Live's foot (Live 12 manual 6.1, its demo arrangement): the returns and Main pinned at the
// bottom of the lanes, below the drop area, whatever the scroll.
void footRows(AppCommands &commands, const std::filesystem::path &dir) {
    StoreError err{};
    auto store = Store::createEmpty(dir / "foot.adi", err);
    check(store != nullptr, "the foot's project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Foot')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    // Main and the returns made first, at index 0, so only the order puts them at the foot.
    bool madeAll = true;
    auto make = [&submitter, &madeAll](std::int64_t id, const char *kind, const std::string &name, std::int64_t index) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", name}, {"index", index}};
        r.label = "Create track";
        madeAll = submitter.submit(std::move(r)).ok && madeAll;
    };
    make(1, "master", "Main", 0);
    make(2, "return", "A Reverb", 0);
    make(3, "return", "B Delay", 0);
    for (std::int64_t i = 0; i < 12; ++i)
        make(10 + i, "audio", "Track " + std::to_string(i + 1), i);
    check(madeAll, "twelve tracks, two returns and Main");
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "foot");
    root.setSize(1200, 720);
    root.frame();
    check(!root.command(AppCommands::SelectAll) && root.error() == "There is nothing to select.",
          "Select All with no clips says there is nothing to select");
    root.arrangement.geometry.resizeTrack(0, 48, true);
    root.mark(DirtySet::All);
    root.frame();
    auto &lanes = root.arrangement.geometry;
    const auto rows = laneOrder(root);
    check(rows.size() == 15 && rows[11] == 21 && rows[12] == 2 && rows[13] == 3 && rows[14] == 1,
          "the tracks, then the returns, then Main, as Live's");
    const int lanesHeight = root.arrangement.canvas.getHeight();
    check(lanes.lanesHeight == lanesHeight && lanes.footHeight(root.reader()) == 3 * 48,
          "the foot is the returns and Main, the lanes' height known to the geometry");
    const int footY = lanesHeight - 3 * 48;
    check(laneTop(root, 2) == footY && laneTop(root, 3) == footY + 48 && laneTop(root, 1) == footY + 96,
          "they stand at the bottom of the lanes");
    check(lanes.tracksHeight(root.reader()) == 12 * 48 && 12 * 48 > footY,
          "twelve tracks run on under the foot");
    // A point in the foot is the foot's, though Track 9 or so is scrolled under it.
    check(lanes.trackAt(root.reader(), footY + 5)->id == 2 && lanes.trackAt(root.reader(), lanesHeight - 5)->id == 1,
          "a point in the foot finds a return or Main, never the track under it");
    check(lanes.trackAt(root.reader(), footY - 5) != nullptr && lanes.trackAt(root.reader(), footY - 5)->id != 2,
          "just above it, a track");
    // Scrolled: the tracks move, the foot does not.
    lanes.scrollY = 200;
    root.arrangement.changed();
    root.frame();
    check(laneTop(root, 10) == -200 && laneTop(root, 1) == footY + 96 && laneTop(root, 2) == footY,
          "scrolled, the tracks move and the foot stays");
    // A row scrolled partly above the lanes: its track controls show no more than its lane does,
    // and take no click above the lanes (window/controls-clip).
    {
        lanes.scrollY = 24;
        root.arrangement.changed();
        root.frame();
        const int top = root.arrangement.topHeight();
        const MixerStrip *first = nullptr;
        for (const auto &strip : root.mixer.strips())
            if (strip->isVisible() && strip->track() == 10)
                first = strip.get();
        auto &panel = root.mixer;
        check(first != nullptr && first->getY() == top - 24 && panel.rowsBand().getStart() == top,
              "Track 1's controls half above the lanes, as its lane is");
        auto *strip = const_cast<MixerStrip *>(first);
        check(strip != nullptr && !strip->hitTest(20, 10) && strip->hitTest(20, 30) &&
                  panel.getComponentAt(20, top - 14) != strip,
              "above the lanes a click is not the row's; level with them it is");
        juce::Image image(juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            panel.paintEntireComponent(g, true);
        }
        check(image.getPixelAt(20, top - 10) == image.getPixelAt(20, 2) &&
                  image.getPixelAt(20, top + 10) != image.getPixelAt(20, 2),
              "the frame's band stands in front of the row above the lanes; the row shows below");
    }
    lanes.scrollY = 100000;
    check(lanes.maxScroll(root.reader()) == 12 * 48 - ArrangementGeometry::kMinTrackHeight,
          "the tracks scroll no further than their last sliver");
    lanes.scrollY = lanes.maxScroll(root.reader());
    root.arrangement.changed();
    root.frame();
    // The track controls follow: the foot's strips at the foot, in front of any partly under it.
    const int bodyTop = root.arrangement.topHeight();
    const MixerStrip *mainStrip = nullptr;
    int shownUnder = 0;
    for (const auto &strip : root.mixer.strips()) {
        if (!strip->isVisible())
            continue;
        if (strip->track() == 1)
            mainStrip = strip.get();
        else if (strip->track() >= 10 && strip->getY() >= bodyTop + footY)
            ++shownUnder;
    }
    check(mainStrip != nullptr && mainStrip->getY() == bodyTop + footY + 96 && shownUnder == 0,
          "Main's track controls at the foot; no track's realised under it");
    // Down from the last track goes into the foot, and the foot never scrolls.
    lanes.scrollY = 0;
    lanes.selection.track = 21;
    check(root.arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) && lanes.selection.track == 2,
          "down from the last track selects the first return");
    root.arrangement.changed();
    root.frame();
    check(laneTop(root, 2) == footY, "and the foot is where it was");
    // A track's own colour (ADR-0211): an index into Live's palette, drawn on its title, its lane's
    // clips and its track controls; without one, or outside the palette, the colour of its place.
    {
        auto headerPixel = [&root](std::int64_t id) {
            auto &headers = root.arrangement.headers;
            juce::Image image(juce::Image::ARGB, headers.getWidth(), headers.getHeight(), true,
                              juce::SoftwareImageType{});
            {
                juce::Graphics g(image);
                headers.paintEntireComponent(g, true);
            }
            return image.getPixelAt(headers.nameLeft(0) + 1, laneTop(root, id) + 30);
        };
        lanes.scrollY = 0;
        root.arrangement.changed();
        root.frame();
        const auto byPlace = root.arrangement.colourFor(1);
        check(byPlace == juce::Colour(0xffeb4cce) && headerPixel(11) == byPlace,
              "Track 2 has no colour of its own: the colour of its place");
        check(root.submitEdit("track.setColor", {{"id", 11}, {"color", 26}}, "Recolour Track"), "Track 2 recoloured");
        root.arrangement.changed();
        root.frame();
        // Until the seventy are measured, index 26 stands in as the fourteen's 26 % 14 = 12th.
        const juce::Colour own(0xff92a7ff);
        check(root.arrangement.colourFor(1) == own && headerPixel(11) == own,
              "its own colour, index 26 of Live's palette, on its title");
        check(root.command(AppCommands::ToggleMixerView), "the Mixer View opens");
        root.resizeMixerView(220);
        root.mark(DirtySet::All);
        root.frame();
        MixerStrip *column = nullptr;
        for (const auto &strip : root.mixerView.strips())
            if (strip->isVisible() && strip->track() == 11)
                column = strip.get();
        juce::Colour nameBar;
        if (column != nullptr) {
            juce::Image image(juce::Image::ARGB, column->getWidth(), column->getHeight(), true,
                              juce::SoftwareImageType{});
            {
                juce::Graphics g(image);
                column->paintEntireComponent(g, true);
            }
            nameBar = image.getPixelAt(column->getWidth() - 6, 10);
        }
        check(column != nullptr && nameBar == own, "and on its strip's name in the Mixer View");
        check(root.command(AppCommands::ToggleMixerView), "the Mixer View closes");
        check(root.submitEdit("track.setColor", {{"id", 11}, {"color", 0xff0000}}, "Recolour Track"),
              "a value outside the palette, as an RGB colour would be");
        root.arrangement.changed();
        root.frame();
        check(root.arrangement.colourFor(1) == byPlace, "is not a colour of the palette: the colour of its place");
        check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "two undos");
        root.arrangement.changed();
        root.frame();
        check(root.arrangement.colourFor(1) == byPlace && headerPixel(11) == byPlace, "and it has no colour of its own again");
        const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
        if (folder.isNotEmpty()) {
            // Four tracks in colours of their own, the rest by place, for the picture.
            int edits = 0;
            for (const auto &[id, index] : {std::pair{10, 7}, std::pair{11, 26}, std::pair{13, 47}, std::pair{14, 3}})
                edits += root.submitEdit("track.setColor", {{"id", id}, {"color", index}}, "Recolour Track") ? 1 : 0;
            lanes.selection.track = 11;
            root.arrangement.changed();
            root.mark(DirtySet::All);
            root.frame();
            juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true,
                              juce::SoftwareImageType{});
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(g, true);
            }
            const auto file = juce::File(folder).getChildFile("adi-track-colour.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(image, *stream);
            for (int k = 0; k < edits; ++k)
                root.command(AppCommands::Undo);
            root.arrangement.changed();
            root.frame();
        }
    }
    const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
    if (folder.isNotEmpty()) {
        // The last tracks, the drop area under them, and the foot, as Live's figure.
        lanes.scrollY = lanes.tracksHeight(root.reader()) - 3 * 48;
        lanes.selection.track = 20;
        root.arrangement.changed();
        root.mark(DirtySet::All);
        root.frame();
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-foot.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
        // Half a row scrolled above the lanes: the names, the lanes and the track controls cut
        // at the same line.
        lanes.scrollY = 24;
        root.arrangement.changed();
        root.mark(DirtySet::All);
        root.frame();
        juce::Image cut(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(cut);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto cutFile = juce::File(folder).getChildFile("adi-controls-clip.png");
        cutFile.deleteFile();
        if (auto stream = cutFile.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(cut, *stream);
    }
}
// New, in the window: Live's default Set as the store makes it (#307, ADR-0209 §10 d10), the
// tracks then Main, and nothing to undo.
void newProject(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "new.adi", true, {}, error);
    check(document != nullptr, "New opens a project");
    if (!document)
        return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "new");
    root.setSize(1440, 900);
    root.frame();
    root.frame();
    check(laneOrder(root) == std::vector<std::int64_t>{2, 3, 4, 5, 6, 7, 1} &&
              root.reader().findTrack(2)->name == "1 MIDI" && root.reader().findTrack(5)->name == "4 Audio" &&
              root.reader().findTrack(7)->name == "B Delay",
          "New shows Live's default Set: 1 MIDI, 2 MIDI, 3 Audio, 4 Audio, A Reverb, B Delay, then Main (N41)");
    check(!root.canUndo(false) && root.undoTitle(false) == "Nothing to Undo" && root.undoTitle(true) == "Nothing to Redo",
          "and has nothing to undo: the Edit menu's items read Nothing to Undo and Nothing to Redo, as Live 12's (LK-02)");
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-new.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    document->release();
}
// Live's Undo History (A5; Live 12 manual 5.4.2): Ctrl/Cmd+Alt+Z lists the history newest first, the
// undone steps greyed; Enter on a selected step restores it; a step on another branch is shown with
// its branch, and a Take Snapshot name beside its step.
void undoHistoryWindow(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "history.adi", true, {}, error);
    check(document != nullptr, "a new project for the Undo History");
    if (!document)
        return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "history");
    int synchronised = 0;
    root.afterEdit = [&] {
        ++synchronised;
        document->synchronise();
    };
    root.setSize(1440, 900);
    root.frame();
    for (const auto &[id, name] : {std::pair{2, "Keys"}, std::pair{3, "Pads"}, std::pair{4, "Drums"}}) {
        OpRequest r;
        r.opType = "track.rename";
        r.payload = {{"id", id}, {"name", name}};
        r.label = std::string("Rename ") + name;
        document->ops().submit(std::move(r));
    }
    check(document->takeSnapshot("Before drums", 1'700'000'000'000'000, 0).ok, "a snapshot after the three renames");
    check(root.command(AppCommands::Undo), "undo Rename Drums");
    root.frame();
    auto &history = root.undoHistory;
    history.setSize(320, 420);
    check(root.keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)),
          "Ctrl/Cmd+Alt+Z");
    const auto &rows = history.rows();
    // New's project is the history's root (#307), listed last as Live lists "Open Live Set".
    check(rows.size() == 4 && rows[0].label == "Rename Drums" && rows[1].label == "Rename Pads" &&
              rows[2].label == "Rename Keys" && rows[3].root && rows[3].label == history.rootLabel && !rows[3].current,
          "the Undo History lists the steps newest first, the project as opened last");
    check(!rows[0].applied && rows[1].applied && rows[1].current && rows[2].applied && history.list.getSelectedRow() == 1,
          "the undone step is greyed; the current one selected");
    check(rows[0].detail.find("Before drums") != std::string::npos,
          "the snapshot taken after Rename Drums stands beside it");
    // Enter on the step two down restores it: one refresh, the session re-read.
    history.list.selectRow(2);
    const auto generation = document->view().generation();
    check(history.list.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)), "Enter");
    root.frame();
    check(root.reader().findTrack(3)->name != "Pads" && root.reader().findTrack(2)->name == "Keys" &&
              document->view().generation() == generation + 1 && synchronised == 2 && history.rows()[2].current &&
              !history.rows()[1].applied,
          "Enter restores the selected step: Rename Pads undone, one refresh, the list following");
    // A click on the greyed newest step redoes down to it.
    history.restoreRow(0);
    root.frame();
    check(root.reader().findTrack(4)->name == "Drums" && history.rows()[0].current && history.rows()[0].applied,
          "a click on the greyed newest step redoes down to it");
    // The last row puts the project back as it was opened: every step undone, all greyed.
    check(history.restoreRow(3), "the project as opened");
    root.frame();
    check(root.reader().findTrack(2)->name != "Keys" && history.rows()[3].current && !history.rows()[2].applied &&
              root.undoTitle(false) == "Nothing to Undo",
          "restoring the first action undoes every step: Nothing to Undo, every step greyed");
    check(history.restoreRow(0), "and the newest again");
    root.frame();
    check(root.reader().findTrack(4)->name == "Drums", "redoes all three");
    // An edit after an undo starts a branch: the old step is shown on its branch.
    root.command(AppCommands::Undo);
    {
        OpRequest r;
        r.opType = "track.rename";
        r.payload = {{"id", 5}, {"name", "Bass"}};
        r.label = "Rename Bass";
        document->ops().submit(std::move(r));
    }
    root.frame();
    history.refresh();
    bool branchShown = false;
    int drumsRow = -1;
    for (std::size_t i = 0; i < history.rows().size(); ++i)
        if (history.rows()[i].label == "Rename Drums") {
            drumsRow = static_cast<int>(i);
            branchShown = history.rows()[i].otherBranch && history.rows()[i].detail.rfind("on ", 0) == 0;
        }
    check(history.rows()[0].label == "Rename Bass" && branchShown, "Rename Drums is shown on its own branch");
    check(history.restoreRow(drumsRow), "restoring it");
    root.frame();
    check(root.reader().findTrack(4)->name == "Drums" && root.reader().findTrack(5)->name != "Bass",
          "switches to its branch");
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        history.refresh();
        juce::Image picture(juce::Image::ARGB, 2 * history.getWidth(), 2 * history.getHeight(), true,
                            juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            history.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-undo-history.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    // A restored step takes a locator with it, and the locator's selection: Set reads Set and adds again.
    const auto located = [&] { return locatorsOf(*document->view().current()); };
    check(root.command(AppCommands::AddLocator), "Add Locator");
    root.frame();
    history.refresh();
    check(located().size() == 1 && root.arrangement.selectedLocator != 0 && history.rows()[0].label == "Add Locator",
          "a locator added and selected, the newest step");
    check(history.restoreRow(1), "the step before it");
    root.frame();
    check(located().empty() && root.arrangement.selectedLocator == 0,
          "the step before the Add Locator restored: the locator and its selection are gone");
    check(root.setLocator() && located().size() == 1, "Set adds again after the restore");
    root.afterEdit = nullptr;
    document->release();
}
// Live's locators (C3, N25; Live 12 manual 6.4, 6.9; MR 3.3): Set, which reads Del on a selected
// locator; Previous and Next, to the arrangement's start or end past the first or the last; a click
// jumps, a double-click while stopped plays; Ctrl/Cmd+R renames, Edit Info Text, the arrows move it,
// Delete removes it; Loop to Next Locator; Ctrl+arrows step to locators too.
void locators(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "locators.adi", true, {}, error);
    check(document != nullptr, "a new project for the locators");
    if (!document)
        return;
    const auto q = textproj::kPPQ, bar = 4 * q;
    {
        OpRequest clip;
        clip.opType = "clip.create";
        clip.payload = {{"id", 1}, {"track", 2}, {"kind", "midi"}, {"name", "Chords"}, {"pos", 0}, {"length", 8 * bar}};
        clip.label = "Insert MIDI Clip";
        check(document->ops().submit(std::move(clip)).ok && document->synchronise(), "a clip of eight bars on 1 MIDI");
    }
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "locators");
    root.afterEdit = [&] { document->synchronise(); };
    root.setSize(1440, 900);
    auto &lanes = root.arrangement.geometry;
    auto &sel = lanes.selection;
    auto &arrangement = root.arrangement;
    lanes.scale = 30;
    lanes.left = 0;
    root.frame();
    root.frame();
    const auto located = [&] { return locatorsOf(*document->view().current()); };
    const auto step = [&] { return document->ops().nextUndo() ? document->ops().nextUndo()->txnId : 0; };
    auto click = [&](ArrangementView::TopButton which) {
        const auto at = arrangement.topButton(which).getCentre();
        arrangement.mouseDown(pressAt(arrangement, at));
        root.frame();
    };

    // Set while stopped (manual 6.4): a locator at the insert marker, one undo step, selected.
    sel.track = 2;
    sel.insert = sel.start = sel.end = 2 * bar;
    sel.marked = true;
    const auto before = step();
    click(ArrangementView::TopButton::Set);
    check(located().size() == 1 && located()[0].pos == 2 * bar && located()[0].name == "1" && step() != before &&
              document->ops().nextUndo()->label == "Add Locator" && document->ops().nextUndo()->opCount == 1 &&
              arrangement.selectedLocator == located()[0].id,
          "Set while stopped adds a locator at the insert marker, named 1, in one undo step, and selects it");
    // With it selected, Set reads Del and deletes it.
    click(ArrangementView::TopButton::Set);
    check(located().empty() && document->ops().nextUndo()->label == "Delete Locator" && arrangement.selectedLocator == 0,
          "with that locator selected, Set deletes it");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(located().size() == 1 && located()[0].pos == 2 * bar, "undo brings it back");
    // A time selection's start, while stopped.
    arrangement.selectedLocator = 0;
    sel.start = 5 * bar;
    sel.end = 6 * bar;
    sel.insert = 5 * bar;
    check(root.command(AppCommands::AddLocator), "Add Locator, the Create menu's");
    root.frame();
    check(located().size() == 2 && located()[1].pos == 5 * bar && located()[1].name == "2",
          "Add Locator over a time selection: at its start, named 2");

    // Previous and Next.
    arrangement.selectedLocator = 0;
    sel.insert = sel.start = sel.end = 0;
    click(ArrangementView::TopButton::Next);
    check(sel.insert == 2 * bar && sel.start == sel.end, "Next from the start: the first locator");
    click(ArrangementView::TopButton::Next);
    check(sel.insert == 5 * bar, "Next: the second");
    click(ArrangementView::TopButton::Next);
    check(sel.insert == 8 * bar && root.arrangementEnd() == 8 * bar, "Next past the last: the arrangement's end (the clip's)");
    click(ArrangementView::TopButton::Previous);
    click(ArrangementView::TopButton::Previous);
    click(ArrangementView::TopButton::Previous);
    check(sel.insert == 0, "Previous past the first: the arrangement's start");

    // A click on a locator selects it and jumps there; a drag moves it.
    auto &scrub = arrangement.scrub;
    const auto first = located()[0];
    const juce::Point<float> onFirst{static_cast<float>(lanes.x(first.pos)) + 4.f, 14.f};
    check(scrub.locatorAt(onFirst.x) == first.id, "the first locator is under the mouse");
    scrub.mouseDown(pressAt(scrub, onFirst));
    scrub.mouseUp(pressAt(scrub, onFirst));
    root.frame();
    check(arrangement.selectedLocator == first.id && sel.insert == first.pos && !document->mailbox().desiredPlaying(),
          "a click selects it and jumps there, playing nothing");
    // Ctrl/Cmd+R renames it.
    check(root.command(AppCommands::RenameTrack) && scrub.locatorEditing() == first.id && scrub.editor.isVisible() &&
              scrub.editor.getText() == "1",
          "Ctrl/Cmd+R on it opens a field over it holding its name");
    scrub.editor.setText("Verse", false);
    scrub.editor.onCommit();
    root.frame();
    check(located()[0].name == "Verse" && !scrub.editor.isVisible() && document->ops().nextUndo()->label == "Rename Locator",
          "Enter renames it, one step");
    check(root.command(AppCommands::EditInfoText) && scrub.editor.isVisible() && scrub.editor.getText().isEmpty(),
          "Edit Info Text opens its info text");
    scrub.editor.setText("Big drums in", false);
    scrub.editor.onCommit();
    root.frame();
    check(located()[0].note == "Big drums in" && document->ops().nextUndo()->label == "Edit Info Text",
          "the info text is the marker's note, one step");
    // The right arrow moves it one grid step, one undo step.
    const auto beforeMove = step();
    check(arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)), "right");
    root.frame();
    check(located()[0].pos == lanes.nextGridLine(first.pos, true) && step() != beforeMove &&
              document->ops().nextUndo()->label == "Move Locator" && arrangement.selectedLocator == first.id &&
              sel.insert == first.pos,
          "the right arrow moves the selected locator one grid step, one undo step, the insert marker staying");
    check(root.command(AppCommands::Undo), "undo the move");
    root.frame();
    // Dragged three bars on, onto the other locator.
    const juce::Point<float> twoOn{onFirst.x + static_cast<float>(lanes.x(3 * bar) - lanes.x(0)), 14.f};
    scrub.mouseDown(pressAt(scrub, onFirst));
    scrub.mouseDrag(dragAt(scrub, onFirst, twoOn));
    check(scrub.locatorMovingTo() == first.pos + 3 * bar, "dragged three bars on: it would land where the other locator stands");
    scrub.mouseUp(dragAt(scrub, onFirst, twoOn));
    root.frame();
    check(located().size() == 2 && located()[0].pos == 2 * bar && root.error().find("Another") != std::string::npos,
          "onto the other locator: it stays, and the Status Bar says why");

    // Loop to Next Locator: from it to the next.
    check(root.loopToNextLocator(first.id), "Loop to Next Locator");
    root.frame();
    check(root.reader().transport().loopEnabled && root.reader().transport().loopStartTicks == 2 * bar &&
              root.reader().transport().loopEndTicks == 5 * bar,
          "the loop on from the locator to the next, outside undo");

    // Ctrl+arrows (Option on the Mac) step to the locators as well as the selected track's clip edges.
    arrangement.selectedLocator = 0;
    sel.track = 2;
    sel.insert = sel.start = sel.end = 0;
#if JUCE_MAC
    const auto edgeMods = juce::ModifierKeys::altModifier;
#else
    const auto edgeMods = juce::ModifierKeys::ctrlModifier;
#endif
    check(arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, edgeMods, 0)) && sel.insert == 2 * bar,
          "Ctrl+right from the start stops at the first locator before the clip's end");

    // A double-click on a locator while stopped selects it and plays from it.
    const auto second = located()[1];
    const juce::Point<float> onSecond{static_cast<float>(lanes.x(second.pos)) + 4.f, 14.f};
    scrub.mouseDoubleClick(pressAt(scrub, onSecond));
    check(arrangement.selectedLocator == second.id && document->mailbox().desiredPlaying(),
          "a double-click while stopped selects it and plays from it");
    document->mailbox().post(TransportMailbox::Command::Stop);
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        arrangement.selectedLocator = first.id;
        root.mark(DirtySet::All);
        root.frame();
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-locators.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    // Delete removes the selected locator.
    arrangement.selectedLocator = second.id;
    check(arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "Delete");
    root.frame();
    check(located().size() == 1 && arrangement.selectedLocator == 0 && document->ops().nextUndo()->label == "Delete Locator",
          "Delete removes the selected locator, one step");

    // One thing selected at a time: the brace pressed lets a locator go, so Delete cannot reach it.
    const auto pressBrace = [&] {
        lanes.scale = 30; // the jumps above scrolled the lanes
        lanes.left = 0;
        root.mark(DirtySet::All);
        root.frame();
        const auto &loop = root.reader().transport(); // Loop to Next Locator above moved it
        const juce::Point<float> onBrace{static_cast<float>(lanes.x((loop.loopStartTicks + loop.loopEndTicks) / 2)), 3.f};
        check(scrub.bracePart(onBrace) == ScrubArea::BracePart::Bar, "the brace's bar is under the press");
        scrub.mouseDown(pressAt(scrub, onBrace));
        scrub.mouseUp(pressAt(scrub, onBrace));
    };
    arrangement.selectedLocator = first.id;
    pressBrace();
    check(arrangement.loopSelected && arrangement.selectedLocator == 0, "the brace pressed lets the selected locator go");
    const auto beforeDelete = step();
    arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    root.frame();
    check(located().size() == 1 && located()[0].id == first.id, "locator, brace, Delete: the locator stays");
    if (step() != beforeDelete)
        root.command(AppCommands::Undo); // Delete took the material the brace selected; put it back
    root.frame();

    // And the other way: Set with the brace selected leaves the new locator the selection; the arrows move it.
    pressBrace();
    sel.insert = sel.start = sel.end = first.pos + bar; // the brace starts on the first locator, and two cannot share a place
    click(ArrangementView::TopButton::Set);
    const auto loopBefore = root.reader().transport();
    check(located().size() == 2 && !arrangement.loopSelected && arrangement.selectedLocator != 0 &&
              arrangement.selectedLocator != first.id,
          "brace, Set: the new locator is selected and the brace let go");
    const auto added = arrangement.selectedLocator;
    const auto posOf = [&](std::int64_t id) {
        const auto all = located();
        const auto *l = locatorById(all, id);
        return l != nullptr ? l->pos : std::int64_t{-1};
    };
    const auto addedAt = posOf(added);
    check(arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)), "right");
    root.frame();
    check(posOf(added) > addedAt &&
              root.reader().transport().loopStartTicks == loopBefore.loopStartTicks &&
              root.reader().transport().loopEndTicks == loopBefore.loopEndTicks,
          "brace, Set, right: the locator moves, not the brace");

    // Undo takes the locator away, and its selection with it: Set reads Set again and adds.
    arrangement.selectedLocator = 0;
    sel.insert = sel.start = sel.end = 7 * bar;
    const auto count = located().size();
    click(ArrangementView::TopButton::Set);
    check(located().size() == count + 1 && arrangement.selectedLocator != 0, "Set adds one, selected");
    check(root.command(AppCommands::Undo), "undo the Set");
    root.frame();
    check(located().size() == count && arrangement.selectedLocator == 0, "Undo: the locator and its selection are gone");
    click(ArrangementView::TopButton::Set);
    check(located().size() == count + 1 && arrangement.selectedLocator != 0, "Set adds again after the Undo");
    root.afterEdit = nullptr;
    document->release();
}
// Live's audio clips draw their waveforms (C4, N40; Live 12 manual 6.1, chapter 9): a stereo file
// of decaying bursts dropped on 3 Audio, its peaks built on the document's worker, each channel in
// a band of the clip's body, and nothing past the clip's source window once the clip is longer.
// E4: Live's audio clip fades (Live 12 manual 6.8; N17): the handles, Create Fade and Crossfade,
// Delete Fades, F.
// The window as the director judges it (win's note of 18 November): one picture at 1920 x 1080, real
// size, on a project of real parts: a drum pattern, held chords, a bass line and a melody as MIDI, and
// two audio files imported through the window's own drop, so their waveforms are the files'. Made
// only when ADI_UI_SHOWCASE names a folder; it writes adi-showcase.png there.
void showcase(AppCommands &commands, const std::filesystem::path &dir) {
    const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_SHOWCASE", {});
    if (folder.isEmpty())
        return;
    std::string error;
    auto document = ProjectDocument::open(dir / "Showcase.adi", true, {}, error);
    check(document != nullptr, "the showcase project opens");
    if (!document)
        return;
    const std::int64_t bar = textproj::kWhole, beat = bar / 4, eighth = beat / 2, sixteenth = beat / 4;
    std::vector<OpRequest> ops;
    const auto add = [&](const char *type, Payload payload, const char *label) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(payload);
        r.label = label;
        ops.push_back(std::move(r));
    };
    // Live's default set (two MIDI and two audio tracks) named for the song, and two MIDI tracks more.
    add("track.rename", {{"id", 2}, {"name", "Drums"}}, "Rename");
    add("track.rename", {{"id", 3}, {"name", "Chords"}}, "Rename");
    add("track.rename", {{"id", 4}, {"name", "Drum Loop"}}, "Rename");
    add("track.rename", {{"id", 5}, {"name", "Pad"}}, "Rename");
    add("track.create", {{"id", 20}, {"kind", "midi"}, {"name", "Bass"}}, "Insert MIDI Track");
    add("track.create", {{"id", 21}, {"kind", "midi"}, {"name", "Melody"}}, "Insert MIDI Track");
    std::int64_t noteId = 1;
    const auto note = [&](std::int64_t clip, std::int64_t start, std::int64_t length, int key, int velocity) {
        add("note.insert", {{"clip", clip}, {"note", noteId++}, {"start", start}, {"dur", length}, {"key", key}, {"vel", velocity}},
            "Add Note");
    };
    const auto clip = [&](std::int64_t id, std::int64_t track, std::int64_t pos, std::int64_t length, const char *name) {
        add("clip.create", {{"id", id}, {"track", track}, {"kind", "midi"}, {"pos", pos}, {"length", length}}, "Insert MIDI Clip");
        add("clip.setName", {{"id", id}, {"name", name}}, "Rename");
    };
    // Drums, eight bars: kick on one and three (and the and of three every other bar), snare on two and
    // four, closed hats on the eighths, an open hat closing each bar, a fill in the last.
    clip(100, 2, 0, 8 * bar, "Beat");
    for (int b = 0; b < 8; ++b) {
        const std::int64_t at = b * bar;
        note(100, at, sixteenth, 36, 118);
        note(100, at + 2 * beat, sixteenth, 36, 110);
        if (b % 2 == 1)
            note(100, at + 2 * beat + eighth, sixteenth, 36, 92);
        note(100, at + beat, sixteenth, 38, 112);
        note(100, at + 3 * beat, sixteenth, 38, 116);
        for (int e = 0; e < 7; ++e)
            note(100, at + e * eighth, sixteenth, 42, e % 2 == 0 ? 96 : 70);
        note(100, at + 7 * eighth, eighth, 46, 88);
        if (b == 7)
            for (int f = 0; f < 4; ++f)
                note(100, at + 3 * beat + f * sixteenth, sixteenth, f < 2 ? 45 : 47, 90 + 6 * f);
    }
    // Chords, held a bar each: Am, F, C, G, twice.
    clip(101, 3, 0, 8 * bar, "Chords");
    const std::array<std::array<int, 4>, 4> chords{{{57, 60, 64, 69}, {53, 57, 60, 65}, {48, 55, 60, 64}, {55, 59, 62, 67}}};
    for (int b = 0; b < 8; ++b)
        for (const int key : chords[static_cast<std::size_t>(b % 4)])
            note(101, b * bar, bar - sixteenth, key, 84);
    // Bass, on the chords' roots: the root on the beat, the octave and the fifth between.
    clip(102, 20, 0, 8 * bar, "Bass");
    const std::array<int, 4> roots{45, 41, 48, 43};
    for (int b = 0; b < 8; ++b) {
        const int root = roots[static_cast<std::size_t>(b % 4)];
        const std::array<std::pair<std::int64_t, int>, 6> line{
            {{0, root}, {beat + eighth, root}, {2 * beat, root + 12}, {2 * beat + eighth, root + 7}, {3 * beat, root},
             {3 * beat + eighth, root + 7}}};
        for (const auto &[at, key] : line)
            note(102, b * bar + at, eighth, key, 104);
    }
    // Melody, a phrase in A minor over four bars, answered in the next four.
    clip(103, 21, 0, 8 * bar, "Lead");
    const std::array<std::tuple<std::int64_t, std::int64_t, int>, 13> phrase{{{0, beat, 76},
                                                                               {beat, eighth, 74},
                                                                               {beat + eighth, eighth, 72},
                                                                               {2 * beat, 2 * beat, 69},
                                                                               {bar, beat + eighth, 72},
                                                                               {bar + beat + eighth, eighth, 74},
                                                                               {bar + 2 * beat, 2 * beat, 76},
                                                                               {2 * bar, eighth, 79},
                                                                               {2 * bar + eighth, eighth, 76},
                                                                               {2 * bar + beat, beat, 74},
                                                                               {2 * bar + 2 * beat, 2 * beat, 72},
                                                                               {3 * bar, 3 * beat, 71},
                                                                               {3 * bar + 3 * beat, beat, 69}}};
    for (int half = 0; half < 2; ++half)
        for (const auto &[at, length, key] : phrase)
            note(103, half * 4 * bar + at, length, half == 1 && at >= 3 * bar ? key - 2 : key, 100);
    check(document->ops().submit(std::move(ops)).ok && document->synchronise(), "the song's parts are in");
    // Two audio files, made here and dropped through the window: a two-bar drum loop at 120 BPM and an
    // eight-bar pad, each with the shape of the real thing (a kick's sweep, a snare's noise, hats; a
    // chord's swell).
    const auto samples = dir / "Samples";
    std::filesystem::create_directories(samples);
    const double rate = 48000;
    juce::Random random(7);
    const auto write = [&](const std::filesystem::path &file, const std::vector<float> &stereo) {
        audio::WavWriter writer(file, 48000, 2, audio::WavFormat::Pcm16);
        writer.write(stereo.data(), static_cast<std::uint32_t>(stereo.size() / 2));
    };
    {
        const int frames = static_cast<int>(4 * rate);
        std::vector<float> out(static_cast<std::size_t>(frames) * 2, 0.f);
        const auto hit = [&](double at, double length, auto voice) {
            const int start = static_cast<int>(at * rate), count = static_cast<int>(length * rate);
            for (int i = 0; i < count && start + i < frames; ++i) {
                const double t = i / rate;
                const float v = static_cast<float>(voice(t));
                out[static_cast<std::size_t>(start + i) * 2] += v;
                out[static_cast<std::size_t>(start + i) * 2 + 1] += v;
            }
        };
        const double spb = 0.5;
        for (int b = 0; b < 8; ++b) {
            const double at = b * spb;
            if (b % 2 == 0)
                hit(at, 0.4, [](double t) { return 0.9 * std::exp(-t * 9) * std::sin(2 * juce::MathConstants<double>::pi * (55 + 90 * std::exp(-t * 30)) * t); });
            else
                hit(at, 0.25, [&](double t) { return 0.6 * std::exp(-t * 16) * (random.nextDouble() * 2 - 1) + 0.25 * std::exp(-t * 20) * std::sin(2 * juce::MathConstants<double>::pi * 190 * t); });
            for (int e = 0; e < 2; ++e)
                hit(at + e * spb / 2, 0.06, [&](double t) { return (e == 0 ? 0.25 : 0.15) * std::exp(-t * 70) * (random.nextDouble() * 2 - 1); });
        }
        write(samples / "Drum Loop.wav", out);
    }
    {
        const int frames = static_cast<int>(16 * rate);
        std::vector<float> out(static_cast<std::size_t>(frames) * 2, 0.f);
        const std::array<std::array<double, 3>, 4> notes{{{220, 261.6, 329.6}, {174.6, 220, 261.6}, {130.8, 196, 261.6}, {196, 246.9, 293.7}}};
        for (int i = 0; i < frames; ++i) {
            const double t = i / rate, inBar = std::fmod(t, 2.0);
            const auto &chord = notes[static_cast<std::size_t>(static_cast<int>(t / 2.0) % 4)];
            const double swell = std::min(1.0, inBar / 0.6) * (inBar > 1.7 ? (2.0 - inBar) / 0.3 : 1.0);
            double v = 0;
            for (const double f : chord)
                for (int h = 1; h <= 3; ++h)
                    v += std::sin(2 * juce::MathConstants<double>::pi * f * h * t) / (h * 4.0);
            out[static_cast<std::size_t>(i) * 2] = static_cast<float>(0.5 * swell * v);
            out[static_cast<std::size_t>(i) * 2 + 1] = static_cast<float>(0.5 * swell * v * 0.97);
        }
        write(samples / "Pad.wav", out);
    }
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "showcase");
    root.afterEdit = [&] { document->synchronise(); };
    root.importAudio = [&](const std::string &path, AudioImport &out, std::string &why) {
        if (document->prepareImport(path, 1'700'000'000'000'000, out))
            return true;
        why = document->error();
        return false;
    };
    root.arrangement.waveforms = &document->waveforms();
    root.setSize(1920, 1080);
    root.frame();
    commands.activate(root);
    for (int b = 0; b < 8; b += 2)
        check(root.dropAudio((samples / "Drum Loop.wav").string(), 4, b * bar, "Drum Loop"), "the drum loop dropped");
    check(root.dropAudio((samples / "Pad.wav").string(), 5, 0, "Pad"), "the pad dropped");
    root.frame();
    // The waveforms come from the document's worker: wait for every audio clip's peaks.
    for (int tries = 0; tries < 200; ++tries) {
        bool all = true;
        for (const auto &t : root.reader().tracks())
            for (const auto &c : t->clips)
                if (c->audio && document->waveforms().peaks(*c->audio) == nullptr)
                    all = false;
        if (all)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // The Melody track chosen, its two devices in the Device View, the arrangement fitted to the song.
    auto publication = std::make_shared<ParameterPublication>();
    struct Knob {
        const char *name, *text;
        double value;
    };
    const std::array<std::pair<const char *, std::vector<Knob>>, 2> devices{
        {{"Auto Filter",
          {{"Freq", "1.20 kHz", .62}, {"Res", "38 %", .38}, {"Attack", "6.00 ms", .2}, {"Release", "200 ms", .5},
           {"Amount", "+12", .6}, {"Rate", "1/8", .45}, {"Phase", "180°", 1}}},
         {"Echo", {{"Time", "1/8", .4}, {"Feedback", "42 %", .42}, {"Dry/Wet", "24 %", .24}}}}};
    std::int64_t deviceId = 91;
    for (const auto &[name, knobs] : devices) {
        DevicePanelData data;
        data.id = deviceId++;
        data.track = 21;
        data.name = name;
        for (const auto &k : knobs) {
            panel::Record record;
            record.id = record.name = k.name;
            record.playing = record.stored = k.value;
            record.shape = panel::Shape::Continuous;
            record.text = k.text;
            data.records.push_back(record);
            data.resolved.entries.push_back({record.id, false});
        }
        panel::finalize(data.records);
        publication->push_back(data);
    }
    root.devices.publication = [publication] { return publication; };
    root.arrangement.geometry.selection.track = 21;
    root.command(AppCommands::OptimizeWidth);
    root.command(AppCommands::OptimizeHeight);
    // No time selection over the song: the insert marker at its start, so every clip shows its own colour.
    auto &sel = root.arrangement.geometry.selection;
    sel.start = sel.end = sel.insert = 0;
    sel.marked = true;
    sel.track = 21;
    root.frame();
    root.frame();
    juce::Image image(juce::Image::ARGB, 1920, 1080, true, juce::SoftwareImageType{});
    {
        juce::Graphics g(image);
        root.paintEntireComponent(g, true);
    }
    const auto save = [&folder](const juce::Image &picture, const char *name) {
        const auto file = juce::File(folder).getChildFile(name);
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(picture, out);
    };
    save(image, "adi-showcase.png");
    // The Arrangement sheet's groups (DF-W25): the drums in a group, the loop in a group inside it, the
    // chords and the pad in a second group; Bass and Melody stand alone between them.
    {
        std::vector<OpRequest> grouping;
        const auto op = [&grouping](const char *type, Payload payload) {
            OpRequest r;
            r.opType = type;
            r.payload = std::move(payload);
            r.label = "Group Tracks";
            grouping.push_back(std::move(r));
        };
        op("track.create", {{"id", 30}, {"kind", "group"}, {"name", "Drums"}, {"index", 0}});
        op("track.rename", {{"id", 2}, {"name", "Kit"}});
        op("track.setParent", {{"id", 2}, {"parent", 30}});
        op("track.create", {{"id", 31}, {"kind", "group"}, {"name", "Loops"}, {"parent", 30}, {"index", 1}});
        op("track.setParent", {{"id", 4}, {"parent", 31}});
        op("track.create", {{"id", 32}, {"kind", "group"}, {"name", "Keys"}, {"index", 0}});
        op("track.setParent", {{"id", 3}, {"parent", 32}});
        op("track.setParent", {{"id", 5}, {"parent", 32}});
        std::int64_t place = 0;
        for (const std::int64_t id : {30, 20, 21, 32})
            op("track.reorder", {{"id", id}, {"index", place++}});
        place = 0;
        for (const std::int64_t id : {2, 31})
            op("track.reorder", {{"id", id}, {"index", place++}});
        check(document->ops().submit(std::move(grouping)).ok && document->synchronise(), "the song's tracks grouped");
    }
    root.frame();
    root.command(AppCommands::OptimizeHeight);
    sel.track = 21;
    root.frame();
    root.frame();
    juce::Image grouped(juce::Image::ARGB, 1920, 1080, true, juce::SoftwareImageType{});
    {
        juce::Graphics g(grouped);
        root.paintEntireComponent(g, true);
    }
    save(grouped, "adi-arrangement.png");
    // The Browser's Plug-Ins (DF-W9), as the app lists what its loader found: two formats, their vendors,
    // instruments, effects and a MIDI effect (invented names), with Melody selected and a synth chosen.
    {
        using K = PluginEntry::Kind;
        PluginListing found;
        found.plugins = {
            {"clap", "com.northwind.glass", "Glass Piano", "Northwind Audio", "1.4.2", "/p/Glass Piano.clap", K::Instrument},
            {"clap", "com.northwind.arc", "Arc Synth", "Northwind Audio", "2.0.1", "/p/Arc Synth.clap", K::Instrument},
            {"clap", "com.northwind.chordz", "Chord Weaver", "Northwind Audio", "1.1.0", "/p/Chord Weaver.clap", K::MidiEffect},
            {"clap", "org.tidewater.plate", "Plate Room", "Tidewater DSP", "3.2.0", "/p/Plate Room.clap", K::Effect},
            {"clap", "org.tidewater.tape", "Tape Echo", "Tidewater DSP", "3.2.0", "/p/Tape Echo.clap", K::Effect},
            {"vst3", "VST3-Saturator X-5a1c", "Saturator X", "Copperline", "1.8", "/p/Saturator X.vst3", K::Unknown},
            {"vst3", "VST3-Bus Glue-77e0", "Bus Glue", "Copperline", "1.8", "/p/Bus Glue.vst3", K::Unknown},
            {"vst3", "VST3-Strings 1-0b3d", "Chamber Strings", "Harbor Instruments", "4.0.3", "/p/Chamber Strings.vst3",
             K::Unknown},
            {"vst3", "VST3-Low End-91aa", "Low End", "Harbor Instruments", "4.0.3", "/p/Low End.vst3", K::Unknown}};
        PluginScan scanned;
        scanned.phase = PluginScan::Phase::Done;
        scanned.listing = found;
        root.browser.setPluginScan(scanned);
        for (std::size_t i = 0; i < root.browser.sidebar().size(); ++i)
            if (root.browser.sidebar()[i].name == "Plug-Ins")
                root.browser.choose(static_cast<int>(i));
        for (std::size_t i = 0; i < root.browser.rows().size(); ++i)
            if (root.browser.rows()[i].plugin != nullptr && root.browser.rows()[i].plugin->name == "Arc Synth")
                root.browser.list.selectRow(static_cast<int>(i));
        check(root.browser.rows().size() == 15, "the showcase's Plug-Ins: two formats, four vendors, nine plug-ins");
        root.frame();
        root.frame();
        juce::Image plugIns(juce::Image::ARGB, 1920, 1080, true, juce::SoftwareImageType{});
        {
            juce::Graphics g(plugIns);
            root.paintEntireComponent(g, true);
        }
        save(plugIns, "adi-plugins.png");
    }
    commands.detach(root);
    document->release();
}
void clipFades(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "fades.adi", true, {}, error);
    check(document != nullptr, "a new project for the fades");
    if (!document)
        return;
    const auto wav = dir / "Pad.wav";
    {
        std::vector<float> samples(2 * 96000);
        for (std::size_t i = 0; i < 96000; ++i) {
            const double v = .6 * std::sin(static_cast<double>(i) * 2 * 3.14159265358979 * 110 / 48000.);
            samples[2 * i] = samples[2 * i + 1] = static_cast<float>(v);
        }
        audio::WavWriter writer(wav, 48000, 2, audio::WavFormat::Pcm16);
        writer.write(samples.data(), 96000);
    }
    const auto bar = textproj::kWhole;
    for (const auto at : {std::int64_t{0}, bar}) {
        AudioImport import;
        check(document->prepareImport(wav.string(), 1'700'000'000'000'000, import), "the file is read");
        auto edit = dropAudio(*document->view().current(), 4, at, "Pad", import);
        check(edit.refusal.empty() && document->ops().submit(std::move(edit.ops)).ok && document->synchronise(),
              "a bar of Pad dropped on 3 Audio");
    }
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "fades");
    root.afterEdit = [&] { document->synchronise(); };
    root.arrangement.waveforms = &document->waveforms();
    root.setSize(1440, 900);
    auto &lanes = root.arrangement.geometry;
    lanes.scale = 40;
    lanes.left = 0;
    root.frame();
    root.frame();
    commands.activate(root);
    auto &canvas = root.arrangement.canvas;
    const auto clips = [&] {
        std::vector<const engine::ClipNode *> out;
        if (const auto *t = root.reader().findTrack(4))
            for (const auto &c : t->clips)
                out.push_back(c.get());
        std::sort(out.begin(), out.end(), [](auto *a, auto *b) { return a->posTicks < b->posTicks; });
        return out;
    };
    check(clips().size() == 2 && clips()[0]->audio && clips()[1]->posTicks == bar, "two adjacent bars of audio");
    if (clips().size() != 2)
        return;
    const auto first = clips()[0]->id, second = clips()[1]->id;
    auto &sel = lanes.selection;
    // No handles until the clip is under the mouse, chosen, or F is held.
    sel = {};
    sel.track = 2;
    using Handle = ArrangementCanvas::FadeHandle;
    check(!canvas.fadeHandlePoint(Handle{first, 4, true, false}), "no handles on a clip that is not chosen");
    check(canvas.keyPressed(juce::KeyPress('f')) && canvas.showFades &&
              canvas.fadeHandlePoint(Handle{first, 4, true, false}) && canvas.fadeHandlePoint(Handle{second, 4, false, false}),
          "F held: every audio clip's peak handles show");
    canvas.showFades = false;
    // Choose the first clip; drag its fade-in peak a quarter of a bar in: one clip.setFades, one step.
    sel.track = 4;
    sel.clip = first;
    sel.start = 0;
    sel.end = bar;
    root.frame();
    const auto peak = canvas.fadeHandlePoint(Handle{first, 4, true, false});
    check(peak.has_value() && canvas.fadeHandleAt(static_cast<int>(peak->x), static_cast<int>(peak->y)) ==
                                  std::optional<Handle>(Handle{first, 4, true, false}),
          "the chosen clip's fade-in square sits at its start, at the top of its body, and is hit there");
    if (!peak)
        return;
    const juce::Point<float> to(static_cast<float>(lanes.x(bar / 4)), peak->y);
    canvas.mouseDown(pressAt(canvas, *peak));
    canvas.mouseDrag(dragAt(canvas, *peak, to));
    check(canvas.fadeDragging().has_value(), "the drag holds the handle");
    canvas.mouseUp(dragAt(canvas, *peak, to));
    root.frame();
    const auto near = [](std::int64_t a, std::int64_t b) { return std::abs(a - b) <= textproj::kPPQ / 16; };
    check(near(clips()[0]->fadeInTicks, bar / 4) && clips()[0]->fadeOutTicks == 0 && clips()[1]->fadeInTicks == 0 &&
              document->ops().nextUndo() && document->ops().nextUndo()->label == "Change Fade",
          "the fade in is a quarter of a bar, one undo step, the other clip untouched");
    // The render shows the fade: the body under the fade is darker at the clip's start.
    {
        juce::Image image(juce::Image::RGB, canvas.getWidth(), canvas.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            canvas.paintEntireComponent(g, true);
        }
        const int x0 = static_cast<int>(lanes.x(bar / 64)), x1 = static_cast<int>(lanes.x(bar / 2));
        const int y = static_cast<int>(peak->y) + 6;
        check(image.getPixelAt(x0, y).getBrightness() + 0.05f < image.getPixelAt(x1, y).getBrightness(),
              "the faded start of the clip is drawn darker than its middle, over the selection's tint");
    }
    // The curve's diamond, dragged up, writes the fade's tension.
    if (const auto diamond = canvas.fadeHandlePoint(Handle{first, 4, true, true})) {
        const juce::Point<float> up(diamond->x, diamond->y - 30.f);
        canvas.mouseDown(pressAt(canvas, *diamond));
        canvas.mouseDrag(dragAt(canvas, *diamond, up));
        canvas.mouseUp(dragAt(canvas, *diamond, up));
        root.frame();
        const auto bent = engine::clipFadeShape(0.5, clips()[0]->fadeInCurve, clips()[0]->fadeInTension);
        check(clips()[0]->fadeInTension.has_value() && std::abs(*clips()[0]->fadeInTension) > 0.2 &&
                  clips()[0]->fadeInCurve == kBentFadeCurve && std::abs(bent - 0.5) > 0.05 &&
                  document->ops().nextUndo()->label == "Change Fade Curve",
              "the diamond dragged up bends the curve, one step");
        check(root.command(AppCommands::Undo), "undo the curve");
        root.frame();
    } else {
        check(false, "the fade's diamond is shown");
    }
    // The chosen fade-in square and Delete: the fade goes.
    canvas.mouseDown(pressAt(canvas, *canvas.fadeHandlePoint(Handle{first, 4, true, false})));
    canvas.mouseUp(pressAt(canvas, *canvas.fadeHandlePoint(Handle{first, 4, true, false})));
    check(canvas.selectedFade().has_value() && canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "Delete on the chosen fade");
    root.frame();
    check(clips()[0]->fadeInTicks == 0 && clips().size() == 2, "the fade is deleted; the clip stays");
    check(root.command(AppCommands::Undo) && root.command(AppCommands::Undo), "undo the delete and the drag");
    root.frame();
    // Both clips chosen: dragging the first clip's fade-out square gives both the same fade out, one step.
    sel = {};
    sel.track = 4;
    sel.clip = first;
    sel.start = 0;
    sel.end = bar;
    sel.pickedClips = {second};
    root.frame();
    if (const auto outPeak = canvas.fadeHandlePoint(Handle{first, 4, false, false})) {
        const juce::Point<float> in(static_cast<float>(lanes.x(bar - bar / 8)), outPeak->y);
        canvas.mouseDown(pressAt(canvas, *outPeak));
        canvas.mouseDrag(dragAt(canvas, *outPeak, in));
        canvas.mouseUp(dragAt(canvas, *outPeak, in));
        root.frame();
        check(near(clips()[0]->fadeOutTicks, bar / 8) && clips()[1]->fadeOutTicks == clips()[0]->fadeOutTicks &&
                  document->ops().nextUndo()->label == "Change Fade",
              "both chosen clips take the same fade out");
        check(root.command(AppCommands::Undo), "one undo");
        root.frame();
        check(clips()[0]->fadeOutTicks == 0 && clips()[1]->fadeOutTicks == 0, "takes both back");
    } else {
        check(false, "the chosen clip's fade-out square is shown");
    }
    // A fade square clicked, then another clip chosen: Delete is the new selection's, not the old fade's.
    sel = {};
    sel.track = 4;
    sel.clip = first;
    sel.start = 0;
    sel.end = bar;
    root.frame();
    {
        const auto square = *canvas.fadeHandlePoint(Handle{first, 4, true, false});
        canvas.mouseDown(pressAt(canvas, square));
        canvas.mouseUp(pressAt(canvas, square));
    }
    sel.clip = second;
    sel.start = bar;
    sel.end = 2 * bar;
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)), "Delete with the second clip chosen");
    root.frame();
    check(clips().size() == 1 && clips()[0]->id == first, "the chosen clip is deleted, as without a fade handle clicked");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    // Create Fade over the boundary and past the second clip's end: only a fade out, so not a crossfade.
    sel = {};
    sel.track = 4;
    sel.start = bar - bar / 4;
    sel.end = 3 * bar;
    check(!root.selectionCrossfades(), "a selection past the right clip's end is no crossfade");
    // Create Crossfade over the two clips' boundary: both fades in one step.
    sel = {};
    sel.track = 4;
    sel.start = bar - bar / 4;
    sel.end = bar + bar / 4;
    check(root.selectionCrossfades(), "the selection crosses the boundary: Create Crossfade");
    check(root.keyPressed(juce::KeyPress('f', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)),
          "Ctrl/Cmd+Alt+F over the boundary");
    root.frame();
    const auto step = document->ops().nextUndo();
    check(clips()[0]->fadeOutTicks == bar / 4 && clips()[1]->fadeInTicks == bar / 4 && step &&
              step->label == "Create Crossfade",
          "the left clip fades out and the right one in, meeting at the boundary");
    check(root.command(AppCommands::Undo), "one undo");
    root.frame();
    check(clips()[0]->fadeOutTicks == 0 && clips()[1]->fadeInTicks == 0, "takes both back: one step");
    // Create Fade In over the first clip's start.
    sel.start = 0;
    sel.end = bar / 2;
    check(!root.selectionCrossfades() && root.command(AppCommands::CreateFade), "Create Fade In over the first half bar");
    root.frame();
    check(clips()[0]->fadeInTicks == bar / 2 && clips()[0]->fadeOutTicks == 0, "a half-bar fade in");
    // Delete Fades/Crossfades on the time selection's clips.
    sel.end = 2 * bar;
    check(root.keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey, juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)),
          "Ctrl/Cmd+Alt+Backspace over both clips");
    root.frame();
    check(clips()[0]->fadeInTicks == 0 && document->ops().nextUndo()->label == "Delete Fades", "the fades are deleted");
    // A selection inside a clip holds neither edge: refused, and says so.
    sel.start = bar / 4;
    sel.end = bar / 2;
    check(!root.command(AppCommands::CreateFade) && !root.error().empty(), "a selection inside a clip has no fade to make");
    // For the picture beside Live 12's: a curved fade in, a crossfade at the boundary, a fade out,
    // the handles shown by F, the waveforms drawn.
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        std::vector<OpRequest> fades;
        for (const auto &[id, in, out, tension] :
             {std::tuple{first, bar / 3, bar / 4, 0.6}, std::tuple{second, bar / 4, bar / 2, 0.0}}) {
            OpRequest r;
            r.opType = "clip.setFades";
            r.payload = {{"id", id}, {"in", {{"ticks", in}, {"tension", tension}, {"curve", kBentFadeCurve}}}, {"out", {{"ticks", out}}}};
            fades.push_back(std::move(r));
        }
        document->ops().submit(std::move(fades));
        document->synchronise();
        sel = {};
        sel.track = 2;
        canvas.showFades = true;
        auto &waves = document->waveforms();
        root.frame();
        juce::Image warm(juce::Image::RGB, canvas.getWidth(), canvas.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(warm);
            canvas.paintEntireComponent(g, true);
        }
        for (int i = 0; i < 2000 && !(waves.idle() && waves.builds() > 0); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        root.frame();
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-fades.png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(picture, out);
        canvas.showFades = false;
    }
    commands.detach(root);
    document->release();
}
void clipWaveforms(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "waves.adi", true, {}, error);
    check(document != nullptr, "a new project for the waveforms");
    if (!document)
        return;
    // Two seconds at 48 kHz, stereo: a burst on each beat, the right channel quieter.
    const auto wav = dir / "Break.wav";
    {
        std::vector<float> samples(2 * 96000);
        for (std::size_t i = 0; i < 96000; ++i) {
            const double t = static_cast<double>(i % 24000) / 48000.;
            const double v = std::sin(2 * 3.14159265358979 * 80 * t) * std::exp(-t * 9) +
                             .15 * std::sin(static_cast<double>(i) * .9) * std::exp(-t * 30);
            samples[2 * i] = static_cast<float>(v * .9);
            samples[2 * i + 1] = static_cast<float>(v * .55);
        }
        audio::WavWriter writer(wav, 48000, 2, audio::WavFormat::Pcm16);
        writer.write(samples.data(), 96000);
    }
    AudioImport import;
    check(document->prepareImport(wav.string(), 1'700'000'000'000'000, import), "the file is read");
    auto edit = dropAudio(*document->view().current(), 4, 0, "Break", import);
    check(edit.refusal.empty() && document->ops().submit(std::move(edit.ops)).ok && document->synchronise(),
          "dropped on 3 Audio at bar 1: four beats at 120 BPM");
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "waves");
    root.afterEdit = [&] { document->synchronise(); };
    root.arrangement.waveforms = &document->waveforms();
    root.setSize(1440, 900);
    auto &lanes = root.arrangement.geometry;
    lanes.scale = 40;
    lanes.left = 0;
    lanes.selection.track = 4;
    root.frame();
    root.frame();
    auto &canvas = root.arrangement.canvas;
    auto paintCanvas = [&canvas] {
        juce::Image image(juce::Image::RGB, canvas.getWidth(), canvas.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            canvas.paintEntireComponent(g, true);
        }
        return image;
    };
    auto &waves = document->waveforms();
    check(waves.builds() == 0 && waves.idle(), "nothing asked before the clip is painted");
    paintCanvas(); // the first paint asks for the file's peaks and draws the clip without them
    for (int i = 0; i < 2000 && !(waves.idle() && waves.builds() > 0); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto seenBefore = root.arrangement.waveformsSeen;
    root.frame();
    check(waves.builds() == 1 && waves.problem().empty() && root.arrangement.waveformsSeen != seenBefore,
          "the worker built the file's peaks, and the next frame saw them land");
    const auto *clip = root.reader().findTrack(4)->clips.empty() ? nullptr : root.reader().findTrack(4)->clips[0].get();
    check(clip != nullptr && clip->audio && clip->audio->sourceLengthFrames == 96000, "the clip and its source window");
    if (clip == nullptr)
        return;
    // The clip twice as long as its file: its body past the source window holds no waveform.
    check(document->ops().submit([&] {
        OpRequest r;
        r.opType = "clip.resize";
        r.payload = {{"id", clip->id}, {"pos", 0}, {"length", 2 * clip->lengthTicks}};
        r.label = "Resize Clip";
        return r;
    }()).ok && document->synchronise(), "the clip lengthened past its file's end");
    root.frame();
    clip = root.reader().findTrack(4)->clips[0].get();
    const auto image = paintCanvas();
    const int top = laneTop(root, 4) + 1 + ArrangementCanvas::kClipBarHeight + 2,
              bottom = laneTop(root, 4) + lanes.heightFor(4) - 5;
    const int sourceEnd = static_cast<int>(lanes.x(clip->lengthTicks / 2)), clipEnd = static_cast<int>(lanes.x(clip->lengthTicks));
    // A waveform pixel is much darker than the clip's body around it.
    const auto body = image.getPixelAt(sourceEnd + 20, top + 1).getBrightness();
    auto darkIn = [&](int x0, int x1) {
        int dark = 0;
        for (int x = std::max(0, x0); x < std::min(x1, image.getWidth()); ++x)
            for (int y = top; y < bottom; ++y)
                if (image.getPixelAt(x, y).getBrightness() < body - .2f)
                    ++dark;
        return dark;
    };
    const int inside = darkIn(static_cast<int>(lanes.x(0)) + 2, sourceEnd - 1), past = darkIn(sourceEnd + 2, clipEnd - 2);
    check(inside > 200 && past == 0, "waveform pixels inside the clip's body over its file, none past its source window");
    if (inside <= 200 || past != 0)
        std::printf("waveform pixels: %d inside, %d past\n", inside, past);
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-waveforms.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    root.afterEdit = nullptr;
    document->release();
}
// Live's metronome and Arrangement Loop (C2, N19; Live 12 manual 6.6, 19.4.1, §42.8, §42.16,
// §42.20): O and the control bar's switches, Loop Selection, the brace dragged and moved by its keys,
// the Loop Start and Loop Length fields, and editing.linkLoopToSelection. Each writes the transport's
// saved state (#383) outside undo, and the session re-reads the project after it.
void loopMetronome(AppCommands &commands, settings::AppSettings &settings, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "loop.adi", true, {}, error);
    check(document != nullptr, "a new project for the loop");
    if (!document)
        return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "loop");
    int synchronised = 0;
    root.afterEdit = [&] {
        ++synchronised;
        document->synchronise();
    };
    root.setSize(1440, 900);
    root.frame();
    root.frame();
    auto &bar = root.transport;
    auto &lanes = root.arrangement.geometry;
    auto &sel = lanes.selection;
    const auto q = textproj::kPPQ, barTicks = 4 * q;
    const auto stored = [&] { return readTransportSettings(document->store()); };
    const auto undoTitle = root.undoTitle(false);
    check(!bar.metronome.getToggleState() && !bar.loop.getToggleState() && bar.loopStart.text == "1.  1.  1" &&
              bar.loopLength.text == "4.  0.  0" && root.reader().transport().loopEndTicks == 4 * barTicks,
          "a new Set: the metronome and the loop off, the brace over the first four bars (#383's default)");

    // O (§42.20): the metronome on, outside undo; the switch lights from the snapshot.
    check(root.keyPressed(juce::KeyPress('o')), "O reaches the command layer");
    root.frame();
    check(root.reader().transport().metronome && stored().metronome && bar.metronome.getToggleState() &&
              root.undoTitle(false) == undoTitle && synchronised == 1 && root.error().empty(),
          "O writes transport.setMetronome: on in the project and lit, the undo stack as it was, the session re-read");
    bar.metronome.onClick();
    root.frame();
    check(!stored().metronome && !bar.metronome.getToggleState() && root.undoTitle(false) == undoTitle,
          "the switch clicked: off again, no undo step");

    // Loop Selection (Ctrl/Cmd+L, manual 6.6) over a time selection from bar 3 to bar 5.
    sel.track = 2;
    sel.toTrack = 3;
    sel.start = 2 * barTicks;
    sel.end = 4 * barTicks;
    sel.clip = 0;
    check(root.keyPressed(juce::KeyPress('l', juce::ModifierKeys::commandModifier, 0)), "Ctrl/Cmd+L");
    root.frame();
    check(stored().loopEnabled && stored().loopStartTicks == 2 * barTicks && stored().loopEndTicks == 4 * barTicks &&
              bar.loop.getToggleState() && bar.loopStart.text == "3.  1.  1" && bar.loopLength.text == "2.  0.  0" &&
              root.undoTitle(false) == undoTitle,
          "Ctrl/Cmd+L over bars 3 to 5 writes transport.setLoop with the selection's start and end, the loop on and "
          "its fields reading them; no undo step");
    check(root.command(AppCommands::LoopSelection), "again, the brace already there");
    root.frame();
    check(!stored().loopEnabled && !bar.loop.getToggleState() && stored().loopStartTicks == 2 * barTicks,
          "toggles the loop off, the brace kept");
    bar.loop.onClick();
    root.frame();
    check(stored().loopEnabled && bar.loop.getToggleState(), "the Loop switch clicked: on again");

    // The brace dragged by its right edge (manual 6.6) to the nearest grid line.
    lanes.scale = 30;
    lanes.left = 0;
    root.mark(DirtySet::All);
    root.frame();
    auto &scrub = root.arrangement.scrub;
    const float endX = static_cast<float>(lanes.x(4 * barTicks));
    const juce::Point<float> grab{endX - 2.f, 3.f}, onto{endX + static_cast<float>(lanes.x(q + q / 2) - lanes.x(0)) + 3.f, 3.f};
    check(scrub.bracePart(grab) == ScrubArea::BracePart::End &&
              scrub.bracePart({static_cast<float>(lanes.x(3 * barTicks)), 3.f}) == ScrubArea::BracePart::Bar &&
              scrub.bracePart({static_cast<float>(lanes.x(2 * barTicks)) + 2.f, 3.f}) == ScrubArea::BracePart::Start &&
              scrub.bracePart({endX - 2.f, 14.f}) == ScrubArea::BracePart::None,
          "the brace's edges and bar are under the mouse along the band's top, and not below it");
    scrub.mouseDown(pressAt(scrub, grab));
    check(root.arrangement.loopSelected && sel.start == 2 * barTicks && sel.end == 4 * barTicks && sel.clip == 0 &&
              !document->mailbox().desiredPlaying(),
          "a press on the brace selects it and the material in the loop (Select Loop), and plays nothing");
    scrub.mouseDrag(dragAt(scrub, grab, onto));
    const auto raw = 4 * barTicks + lanes.tick(onto.x) - lanes.tick(grab.x);
    const auto down = lanes.snap(raw, false), up = lanes.snap(raw, true);
    const auto expected = raw - down <= up - raw ? down : up;
    check(scrub.braceMoving() && scrub.braceMoving()->end == expected && expected != 4 * barTicks &&
              stored().loopEndTicks == 4 * barTicks,
          "dragged a quarter and a half on: the end would land on the nearest grid line, nothing written yet");
    root.frame();
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-loop-dragging.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    scrub.mouseUp(dragAt(scrub, grab, onto));
    root.frame();
    check(stored().loopEndTicks == expected && stored().loopStartTicks == 2 * barTicks && stored().loopEnabled &&
              !scrub.braceMoving() && sel.end == expected && root.undoTitle(false) == undoTitle,
          "let go: the loop ends on that grid line, its start kept, the selection with it, no undo step");

    // Its keys while it is selected (manual 6.6, §42.8).
    const auto length = expected - 2 * barTicks;
    auto &canvas = root.arrangement.canvas;
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)), "right");
    root.frame();
    const auto nudged = lanes.nextGridLine(2 * barTicks, true);
    check(stored().loopStartTicks == nudged && stored().loopEndTicks == nudged + length && sel.start == nudged,
          "right nudges the brace a grid line on, its length kept");
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::upKey)), "up");
    root.frame();
    check(stored().loopStartTicks == nudged + length && stored().loopEndTicks == nudged + 2 * length,
          "up moves it on by its length");
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::commandModifier, 0)), "Ctrl/Cmd+up");
    root.frame();
    check(stored().loopStartTicks == nudged + length && stored().loopEndTicks == nudged + 3 * length,
          "Ctrl/Cmd+up doubles it");
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::commandModifier, 0)), "Ctrl/Cmd+down");
    root.frame();
    check(stored().loopEndTicks == nudged + 2 * length, "Ctrl/Cmd+down halves it");
    const auto endBefore = stored().loopEndTicks;
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::leftKey, juce::ModifierKeys::commandModifier, 0)), "Ctrl/Cmd+left");
    root.frame();
    check(stored().loopEndTicks == lanes.nextGridLine(endBefore, false) && stored().loopStartTicks == nudged + length,
          "Ctrl/Cmd+left shortens it by the grid, its start kept");
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) && canvas.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)),
          "down, then left");
    root.frame();
    const auto shortened = lanes.nextGridLine(endBefore, false) - (nudged + length);
    check(stored().loopStartTicks == lanes.nextGridLine(nudged + length - shortened, false) &&
              stored().loopEndTicks - stored().loopStartTicks == shortened && root.undoTitle(false) == undoTitle,
          "down moves it back by its (shortened) length and left nudges it back a grid line; none an undo step");

    // The Loop Start and Loop Length fields typed (manual 6.6).
    bar.loopLength.onClick();
    check(bar.editing() == &bar.loopLength && bar.fieldEditor.isVisible(), "a click on Loop Length opens its field");
    bar.fieldEditor.setText("2.0.0", false);
    bar.fieldEditor.onReturnKey();
    root.frame();
    const auto start = stored().loopStartTicks;
    check(!bar.fieldEditor.isVisible() && stored().loopEndTicks == start + 2 * barTicks && bar.loopLength.text == "2.  0.  0",
          "a typed Loop Length of 2.0.0: the end is the start plus two bars");
    bar.loopStart.onClick();
    check(bar.editing() == &bar.loopStart && bar.fieldEditor.getText() == bar.loopStart.text.removeCharacters(" "),
          "a click on Loop Start opens its field, holding the start");
    bar.fieldEditor.setText("9.1.1", false);
    bar.fieldEditor.onReturnKey();
    root.frame();
    check(stored().loopStartTicks == 8 * barTicks && stored().loopEndTicks == 10 * barTicks && bar.loopStart.text == "9.  1.  1",
          "a typed Loop Start of 9.1.1: the brace starts at bar 9, its two bars kept");
    bar.loopLength.onClick();
    bar.fieldEditor.setText("0.0.0", false);
    bar.fieldEditor.onReturnKey();
    check(bar.fieldEditor.isVisible() && root.error().find("length") != std::string::npos &&
              stored().loopEndTicks == 10 * barTicks,
          "no length: refused, the field stays open and the Status Bar says why");
    bar.fieldEditor.onEscapeKey();

    // A click in the lanes lets the brace go: the arrows move the insert marker again.
    const juce::Point<float> inLanes{40.f, static_cast<float>(laneTop(root, 2) + 10)};
    canvas.mouseDown(pressAt(canvas, inLanes));
    canvas.mouseUp(pressAt(canvas, inLanes));
    check(!root.arrangement.loopSelected, "a click in the lanes lets the brace go");

    // editing.linkLoopToSelection (REAPER's), off by default: on, a time selection made with Shift and
    // the arrows moves the brace to it, the loop's switch as it was.
    std::string refusal;
    const bool linked = settings.set("editing.linkLoopToSelection", true, {}, refusal);
    commands.reload();
    sel.track = 2;
    sel.toTrack = 0;
    sel.clip = 0;
    sel.insert = sel.start = sel.end = 12 * barTicks;
    check(linked && commands.linkLoopToSelection() &&
              canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0)),
          "Shift+right with the setting on");
    root.frame();
    check(stored().loopStartTicks == 12 * barTicks && stored().loopEndTicks == sel.end && sel.end > 12 * barTicks &&
              stored().loopEnabled,
          "the brace follows the time selection made, the loop still on");
    settings.set("editing.linkLoopToSelection", false, {}, refusal);
    commands.reload();
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0)), "again, off");
    root.frame();
    check(stored().loopEndTicks != sel.end, "off: the brace stays where it was");
    check(root.undoTitle(false) == undoTitle, "after all of it, the undo stack is as it was");
    root.afterEdit = nullptr;
    document->release();
}
// A track's recording options (C6, N31; MR V.2 §9; Live 12 manual 18.8): a MIDI track's Rec-Q and
// Play-Q on its header, Keep Monitoring Latency from a right-click on In or Auto, each one undo step,
// read back from `TrackNode::recording`; Insert MIDI Track carries the registry's defaults.
void recordingOptions(AppCommands &commands, settings::AppSettings &settings, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "recording.adi", true, {}, error);
    check(document != nullptr, "a new project for the recording options");
    if (!document)
        return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "recording");
    root.afterEdit = [&] { document->synchronise(); };
    root.setSize(1440, 900);
    root.frame();
    root.frame();
    auto &headers = root.arrangement.headers;
    const auto *midi = root.reader().findTrack(2);
    check(midi != nullptr && recordQuantizeText(midi->recording) == "1/16" && playQuantizeText(midi->recording) == "Off" &&
              !headers.quantizeBox(0, 0, root.arrangement.geometry.heightFor(2), true).isEmpty(),
          "1 MIDI's header shows Rec-Q 1/16 and Play-Q Off");
    auto pick = [](const juce::PopupMenu &menu, const juce::String &text) {
        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
            if (it.getItem().text == text && it.getItem().action) {
                it.getItem().action();
                return true;
            }
        return false;
    };
    const auto step = [&] { return document->ops().nextUndo() ? document->ops().nextUndo()->txnId : 0; };
    const auto before = step();
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        renderMenu(look::liveLook(), headers.quantizeMenu(2, true), juce::File(folder).getChildFile("adi-rec-q-menu.png"));
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-recording-options.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    check(pick(headers.quantizeMenu(2, true), "1/8"), "Rec-Q's menu, 1/8");
    root.frame();
    midi = root.reader().findTrack(2);
    check(midi->recording.recordEnabled && midi->recording.recordGrid == "1/8" && step() != before &&
              document->ops().nextUndo()->label == "Change Record Quantization" && document->ops().nextUndo()->opCount == 1,
          "Rec-Q writes track.setRecordQuantize, one undo step, read back from the snapshot");
    check(pick(headers.quantizeMenu(2, true), "Free"), "Rec-Q Free");
    root.frame();
    check(!root.reader().findTrack(2)->recording.recordEnabled && recordQuantizeText(root.reader().findTrack(2)->recording) == "Free",
          "and Free turns it off");
    check(pick(headers.quantizeMenu(2, false), "1/4"), "Play-Q 1/4");
    root.frame();
    check(root.reader().findTrack(2)->recording.playEnabled && root.reader().findTrack(2)->recording.playGrid == "1/4" &&
              document->ops().nextUndo()->label == "Change Live Quantization",
          "Play-Q writes track.setPlayQuantize, one step");
    check(root.command(AppCommands::Undo), "undo");
    root.frame();
    check(!root.reader().findTrack(2)->recording.playEnabled, "undo puts Play-Q back off");
    // Keep Monitoring Latency from 3 Audio's In or Auto button.
    MixerStrip *audio = nullptr;
    for (const auto &strip : root.mixer.strips())
        if (strip->track() == 4)
            audio = strip.get();
    check(audio != nullptr, "3 Audio's row");
    if (audio != nullptr) {
        check(!audio->showsInOut() || audio->onMonitorInOrAuto({30, 40}), "a right-click on Auto is the monitor's");
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty())
            renderMenu(look::liveLook(), audio->monitorMenu(), juce::File(folder).getChildFile("adi-monitor-menu.png"));
        check(pick(audio->monitorMenu(), "Keep Monitoring Latency in Recorded Audio"), "its menu's one item");
        root.frame();
        check(!root.reader().findTrack(4)->recording.keepMonitoringLatency &&
                  document->ops().nextUndo()->label == "Keep Monitoring Latency",
              "Keep Monitoring Latency off, one step, read back");
    }
    // Live 12's Keep Latency toggle under Track Delay in the Mixer View's Track Options (manual 18.8).
    root.command(AppCommands::ToggleMixerView);
    if (!root.state().mixerTrackOptions)
        root.command(AppCommands::ToggleMixerTrackOptions);
    const bool inOutWasOn = root.state().mixerInOut;
    if (!inOutWasOn)
        root.command(AppCommands::ToggleMixerInOut);
    root.frame();
    MixerStrip *wide = nullptr;
    for (const auto &strip : root.mixerView.strips())
        if (strip->track() == 4)
            wide = strip.get();
    check(wide != nullptr && wide->keepLatency.isVisible() && !wide->keepLatency.getToggleState(),
          "the Mixer View's Track Options: 3 Audio's Keep Latency, unlit as the project holds it");
    if (wide != nullptr) {
        wide->keepLatency.onClick();
        root.frame();
        check(root.reader().findTrack(4)->recording.keepMonitoringLatency && wide->keepLatency.getToggleState(),
              "clicked: on again, lit from the snapshot");
        // The Mixer View's own In/Out section draws In and Auto 59 px under the name (19): a
        // right-click there is the monitor's too, as the toggle's tooltip says; Audio From is not.
        // A strip tall enough for the In/Out section, Track Options and the mixer together.
        const auto keptHeight = wide->getHeight();
        wide->setSize(wide->getWidth(), 360);
        check(wide->onMonitorInOrAuto({10, 19 + 65}) && wide->onMonitorInOrAuto({35, 19 + 65}) &&
                  !wide->onMonitorInOrAuto({10, 19 + 20}),
              "a right-click on In or Auto in the Mixer View's In/Out section is the monitor's");
        wide->setSize(wide->getWidth(), keptHeight);
    }
    if (!inOutWasOn)
        root.command(AppCommands::ToggleMixerInOut);
    root.command(AppCommands::ToggleMixerTrackOptions);
    root.command(AppCommands::ToggleMixerView);
    root.frame();
    // Insert MIDI Track takes the registry's defaults.
    std::string why;
    check(settings.set("record.quantize", "1/32", {}, why), "record.quantize 1/32");
    commands.reload();
    root.arrangement.geometry.selection.track = 2;
    check(root.command(AppCommands::InsertMidiTrack), "Insert MIDI Track");
    root.frame();
    const auto *added = root.reader().findTrack(root.arrangement.geometry.selection.track);
    check(added != nullptr && added->kind == "midi" && added->recording.recordGrid == "1/32",
          "the new MIDI track records quantized to 1/32, the registry's default");
    settings.set("record.quantize", "1/16", {}, why);
    commands.reload();
    root.afterEdit = nullptr;
    document->release();
}
// A parameter drag in the device strip is one undo step (N7, worklist A3; ADR-0124 d4, ADR-0181
// d5; MR 9.1): the real ParameterControl, through the document's gesture, into the engine's window
// producer. The plug-in is the loader's headless Knobs, on 1 MIDI.
// D2: Live's device clipboard (manual 23.2.1, 41.5; N18, #394). Cut, Copy, Paste and Duplicate on
// a device the project stores, from the keys through the peer, with the app's clipboard wired as
// ui_main.cpp wires it; a click past the last device chooses the chain's end.
void deviceClipboard(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "device-clipboard.adi", true, test::knobsLoader(), error);
    check(document != nullptr, "a project for the device clipboard");
    if (!document)
        return;
    document->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                                test::kKnobsUid + "','UI knobs')");
    std::vector<OpRequest> make(2);
    make[0].opType = "chain.create";
    make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
    make[1].opType = "device.insert";
    make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
    check(document->ops().submit(std::move(make)).ok && document->synchronise(), "a device on 1 MIDI to copy");
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "clipboard");
    UiPeerRoot host;
    host.setSize(1600, 1000);host.addAndMakeVisible(root);
    host.setVisible(true);host.addToDesktop(0);
    root.afterEdit = [&] { document->synchronise(); };
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const std::vector<std::int64_t> &ids) {
        document->setParameterDemand(owner, ids);
    };
    root.devices.submit = [&](const std::string &op, Payload p) {
        const bool ok = document->deviceAction(op, std::move(p));
        if (!ok)
            root.say(document->error());
        return ok;
    };
    std::optional<DeviceCopy> clipboard;
    root.devices.copy = [&](std::int64_t id, bool cut) {
        DeviceCopy copy;
        bool ok = document->copyDevice(id, copy);
        if (ok) {
            clipboard = std::move(copy);
            if (cut)
                ok = document->deviceAction("device.remove", {{"id", id}});
        }
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.devices.paste = [&](std::int64_t track, std::optional<std::int64_t> before) {
        const bool ok = clipboard && document->pasteDevice(*clipboard, track, before);
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.devices.duplicate = [&](std::int64_t id, std::int64_t track, std::optional<std::int64_t> before) {
        DeviceCopy copy;
        const bool ok = document->copyDevice(id, copy) && document->pasteDevice(copy, track, before);
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.devices.holdsCopy = [&] { return clipboard.has_value(); };
    root.setSize(1440, 900);
    root.arrangement.geometry.selection.track = 2;
    const auto refresh = [&] {
        document->tick(0);
        root.frame();
        root.frame();
    };
    refresh();
    const auto chain = [&] {
        std::vector<std::int64_t> out;
        SQLite::Statement q(document->store().db(), "SELECT id FROM devices WHERE chain_id = 3 ORDER BY ord, id");
        while (q.executeStep())
            out.push_back(q.getColumn(0).getInt64());
        return out;
    };
    const auto label = [&] { return document->ops().nextUndo() ? document->ops().nextUndo()->label : std::string(); };
    using Clip = DeviceChainStrip::Clip;
    check(root.devices.panels.size() == 1 && !root.devices.canClipboard(Clip::Copy) &&
              !root.devices.canClipboard(Clip::Paste) && !root.command(AppCommands::Copy) &&
              root.error().find("Select a device") != std::string::npos,
          "with no device chosen, Copy has nothing to act on and the Status Bar says so");
    auto *peer = host.getPeer();
    const auto key = [&](char c) {
        return peer != nullptr && peer->handleKeyPress(juce::KeyPress(c, juce::ModifierKeys::commandModifier, 0));
    };
    const auto focus = [&](std::size_t i) {
        if (i >= root.devices.panels.size())
            return false;
        root.devices.panels[i]->grabKeyboardFocus();
        return root.devices.panels[i]->hasKeyboardFocus(true);
    };
    // While a device's name is being edited the keys are the editor's: no device is chosen.
    check(focus(0) && root.devices.panels[0]->beginRename() && !root.devices.focusedDevice() &&
              !root.devices.canClipboard(Clip::Duplicate),
          "while its name is edited, the device is not the clipboard's: Ctrl/Cmd+D stays the editor's");
    root.devices.panels[0]->rename.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    const auto opsBefore = document->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
    check(focus(0) && key('c') && clipboard.has_value() && chain() == std::vector<std::int64_t>{3} &&
              document->store().db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBefore,
          "Ctrl/Cmd+C on the focused device copies it, and writes nothing");
    check(focus(0) && key('v'), "Ctrl/Cmd+V on the focused device pastes");
    refresh();
    auto order = chain();
    check(order.size() == 2 && order[0] > 3 && order[1] == 3 && label() == "Paste Device" &&
              document->store()
                      .db()
                      .execAndGet("SELECT name FROM devices WHERE id = " + std::to_string(order[0]))
                      .getString() == "UI knobs" &&
              root.devices.panels.size() == 2,
          "a new device in front of it, with the next free id and the copy's name: one step, Paste Device");
    check(root.command(AppCommands::Undo), "undo");
    refresh();
    check(chain() == std::vector<std::int64_t>{3}, "undo takes the pasted device away in one step");
    const auto held = clipboard->insert.dump();
    check(focus(0) && key('d'), "Ctrl/Cmd+D on the focused device duplicates");
    refresh();
    order = chain();
    check(order.size() == 2 && order[0] == 3 && order[1] > 3 && clipboard->insert.dump() == held,
          "the duplicate goes after it, at the chain's end, and the clipboard keeps what it held");
    const auto duplicateId = order[1];
    check(focus(1) && key('x'), "Ctrl/Cmd+X on the duplicate cuts it");
    refresh();
    check(chain() == std::vector<std::int64_t>{3} && clipboard->insert.value("name", "") == "UI knobs" &&
              clipboard->insert.value("id", std::int64_t{0}) == duplicateId,
          "cut: the device goes, and the clipboard holds it");
    // A click past the last device chooses the chain's end; Paste lands there.
    const auto past = host.getLocalPoint(&root.devices, juce::Point<int>{root.devices.panels.back()->getRight() + 40,
                                                                          root.devices.getHeight() / 2})
                          .toFloat();
    juce::int64 when = 200000;
    peer->handleMouseEvent(juce::MouseInputSource::InputSourceType::mouse, past, juce::ModifierKeys(), 1.f, 0.f,
                           when += 100);
    peer->handleMouseEvent(juce::MouseInputSource::InputSourceType::mouse, past,
                           juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.f, 0.f, when += 100);
    peer->handleMouseEvent(juce::MouseInputSource::InputSourceType::mouse, past, juce::ModifierKeys(), 1.f, 0.f,
                           when += 100);
    check(root.devices.endChosen() && !root.devices.focusedDevice() && root.devices.canClipboard(Clip::Paste) &&
              !root.devices.canClipboard(Clip::Copy),
          "a click past the last device chooses the chain's end: Paste can go there, Copy has no device");
    check(key('v'), "Ctrl/Cmd+V at the chain's end");
    refresh();
    order = chain();
    check(order.size() == 2 && order[0] == 3 && order[1] > 3 && label() == "Paste Device",
          "the cut device comes back at the chain's end, a new id, one step");
    // The device's menu and the strip with its end chosen, for the picture beside Live 12's.
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        renderMenu(look::liveLook(), root.devices.panels[0]->commands(),
                   juce::File(folder).getChildFile("adi-device-menu.png"));
        root.devices.grabKeyboardFocus();
        juce::Image picture(juce::Image::ARGB, 2 * root.devices.getWidth(), 2 * root.devices.getHeight(), true,
                            juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.devices.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-device-strip.png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(picture, out);
    }
    // A copy from one project does not paste into another: the engine's refusal, in the Status Bar.
    {
        auto other = ProjectDocument::open(dir / "device-clipboard-other.adi", true, {}, error);
        check(other != nullptr, "a second project");
        if (other) {
            check(!other->pasteDevice(*clipboard, 2, std::nullopt) &&
                      other->error().find("different project") != std::string::npos,
                  "a copy from another project is refused, with the engine's reason");
            other->release();
        }
    }
    host.removeChildComponent(&root);
    document->release();
}
// D3: a plug-in's own window (Live 12 manual 23.3, 23.3.1.1, 41.4; N8, #391), with a fake opener in
// the app's place: the wrench on plug-in devices only; a click opens the editor in a floating window
// at its own size, never scaled, and a second closes it; one window at a time unless Ctrl/Cmd or
// Multiple Plug-In Windows; Ctrl/Cmd+Alt+P hides and shows them all; Auto-Hide shows only the
// selected track's; the opener's error and the editor's reach the Status Bar.
void pluginWindows(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "plugin-windows.adi", true, test::knobsLoader(), error);
    check(document != nullptr, "a project for the plug-in windows");
    if (!document)
        return;
    document->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                                test::kKnobsUid + "','UI knobs')");
    std::vector<OpRequest> make(4);
    make[0].opType = "chain.create";
    make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
    make[1].opType = "device.insert";
    make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
    make[2].opType = "device.insert";
    make[2].payload = {{"id", 4}, {"chain", 3}, {"ord", 1}, {"name", "Rack"}, {"latency", 0}};
    make[3].opType = "device.insert";
    make[3].payload = {{"id", 5}, {"chain", 3}, {"ord", 2}, {"ref", 3}, {"name", "UI knobs 2"}, {"latency", 0}};
    check(document->ops().submit(std::move(make)).ok && document->synchronise(),
          "two plug-ins and a device of no plug-in on 1 MIDI");
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "plugins");
    UiPeerRoot surfacePeer;
    surfacePeer.setSize(1600, 1000);surfacePeer.addAndMakeVisible(root);
    surfacePeer.setVisible(true);surfacePeer.addToDesktop(0);
    root.afterEdit = [&] { document->synchronise(); };
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const std::vector<std::int64_t> &ids) {
        document->setParameterDemand(owner, ids);
    };
    root.setSize(1440, 900);
    root.arrangement.geometry.selection.track = 2;
    const auto refresh = [&] {
        document->tick(0);
        root.frame();
        root.frame();
    };
    refresh();
    FloatingHost host(root, commands, document->views(), document->view(), [](std::int64_t) { return true; }, false);
    int opened = 0;
    std::string refuse, editorError;
    bool editorGone = false;
    check(!root.togglePluginWindow(3, false) && root.error().find("in the application") != std::string::npos,
          "with no opener wired, the wrench says plug-in windows open in the application");
    root.pluginHost = &host;
    root.pluginEditor = [&](std::int64_t, std::string &why) {
        AdiRootComponent::PluginEditorView view;
        if (!refuse.empty()) {
            why = refuse;
            return view;
        }
        ++opened;
        auto editor = std::make_unique<juce::Component>();
        editor->setSize(320, 200);
        view.view = std::move(editor);
        view.frame = [&] { return editorError; };
        view.gone = [&] { return editorGone; };
        return view;
    };
    auto &panels = root.devices.panels;
    check(panels.size() == 3 && panels[0]->wrench.isVisible() && !panels[1]->wrench.isVisible() &&
              panels[2]->wrench.isVisible(),
          "the wrench shows on the plug-ins' title bars, not on a device of no plug-in");
    const auto key3 = AdiRootComponent::pluginWindowKey(3), key5 = AdiRootComponent::pluginWindowKey(5);
    check(panels[0]->wrench.getX() < panels[0]->title.getX() && panels[0]->title.getX() > panels[1]->title.getX(),
          "the wrench sits before the name, after the fold switch, as Live's");
    panels[0]->wrench.onClick();
    root.frame();
    // The strip with a plug-in's window open, for the picture beside Live 12's.
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.devices.getWidth(), 2 * root.devices.getHeight(), true,
                            juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.devices.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-plugin-wrench.png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(picture, out);
    }
    check(opened == 1 && host.contains(key3) && host.content(key3) != nullptr &&
              host.content(key3)->getWidth() == 320 && host.content(key3)->getHeight() == 200 &&
              !host.zoom(key3, true) && host.alwaysOnTop(key3) && panels[0]->wrench.getToggleState(),
          "a click opens the editor at the plug-in's own size, never scaled, on top, the wrench lit");
    {
        auto *window = dynamic_cast<juce::DocumentWindow *>(host.window(key3));
        const auto *track = root.reader().findTrack(2);
        check(window != nullptr && track != nullptr && window->getName() == juce::String(track->name + "/UI knobs") &&
                  !window->isResizable() && window->getContentComponentBorder().addedTo(host.content(key3)->getBounds()).getWidth() == window->getWidth(),
              "its window is titled track/device, fits the plug-in, and does not resize");
    }
    panels[0]->wrench.onClick();
    root.frame();
    check(!host.contains(key3) && !panels[0]->wrench.getToggleState(), "a second click closes it");
    // One at a time; Ctrl/Cmd keeps the others open.
    check(root.togglePluginWindow(3, false) && root.togglePluginWindow(5, false) && !host.contains(key3) &&
              host.contains(key5),
          "opening another plug-in's window closes the first (Multiple Plug-In Windows off)");
    check(root.togglePluginWindow(3, true) && host.contains(key3) && host.contains(key5),
          "with Ctrl/Cmd both stay open");
    // A plug-in with no editor refuses before any other window closes.
    root.togglePluginWindow(5, true);
    refuse = "This VST3 has no editor";
    check(!root.togglePluginWindow(5, false) && host.contains(key3) && root.error() == refuse,
          "a refusal leaves the open plug-in window open");
    refuse.clear();
    check(root.togglePluginWindow(5, true) && host.contains(key3) && host.contains(key5), "both open again");
    // Ctrl/Cmd+Alt+P hides and shows them all.
    const juce::KeyPress hideAll('p', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0);
    juce::ApplicationCommandInfo info(AppCommands::TogglePluginWindows);
    commands.activate(root);
    commands.getCommandInfo(AppCommands::TogglePluginWindows, info);
    check(info.shortName == "Hide Plug-In Windows" && info.defaultKeypresses.contains(hideAll),
          "View > Hide Plug-In Windows on Ctrl/Cmd+Alt+P while they show");
    check(root.keyPressed(hideAll) && !root.pluginWindowsShown() && !host.shown(key3) && !host.shown(key5) &&
              host.contains(key3),
          "Ctrl/Cmd+Alt+P hides every plug-in window, open still");
    commands.getCommandInfo(AppCommands::TogglePluginWindows, info);
    root.frame();
    check(info.shortName == "Show Plug-In Windows" && !host.shown(key3), "then the menu offers to show them");
    check(root.keyPressed(hideAll) && root.pluginWindowsShown() && host.shown(key3) && host.shown(key5),
          "and again shows them where they stood");
    // Auto-Hide Plug-In Windows (on by default): only the selected track's show.
    root.arrangement.geometry.selection.track = 3;
    root.frame();
    check(!host.shown(key3) && !host.shown(key5) && host.contains(key3), "another track selected: they hide");
    root.arrangement.geometry.selection.track = 2;
    root.frame();
    check(host.shown(key3) && host.shown(key5), "their track selected again: they show");
    // A window opened on purpose while they are hidden shows, and the others with it.
    check(root.keyPressed(hideAll) && !root.pluginWindowsShown(), "hidden");
    check(root.togglePluginWindow(5, true) && !host.contains(key5), "a wrench click closes the hidden one");
    check(root.togglePluginWindow(5, true) && root.pluginWindowsShown() && host.shown(key5) && host.shown(key3),
          "and opening one on purpose shows it, and the others Show/Hide had hidden");
    // Errors reach the Status Bar: the editor's, on its window's clock, and the opener's.
    editorError = "Plug-in refused to show its editor";
    host.frame(key3);
    check(root.error() == editorError, "an editor's error reaches the Status Bar");
    editorGone = true;
    host.frame(key3);
    check(root.error().find("closed its window") != std::string::npos && !host.shown(key3),
          "an editor the plug-in dropped hides its window at once and says so");
    host.collect();
    root.frame();
    check(!host.contains(key3) && host.contains(key5) && !panels[0]->wrench.getToggleState(),
          "and the next collect closes it, the wrench unlit");
    editorGone = false;
    host.closeAll(false);
    refuse = "This VST3 has no editor";
    check(!root.togglePluginWindow(3, false) && root.error() == refuse && !host.contains(key3),
          "an opener's refusal reaches the Status Bar, and no window opens");
    commands.detach(root);
    root.pluginHost = nullptr;
    document->release();
}
void stripParamUndo(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "strip-undo.adi", true, test::knobsLoader(), error);
    check(document != nullptr, "a project whose plug-ins the loader makes");
    if (!document)
        return;
    document->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'clap','") +
                                test::kKnobsUid + "','UI knobs')");
    std::vector<OpRequest> make(2);
    make[0].opType = "chain.create";
    make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
    make[1].opType = "device.insert";
    make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
    check(document->ops().submit(std::move(make)).ok && document->synchronise(), "a device on 1 MIDI");
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "strip");
    UiPeerRoot surfacePeer;
    surfacePeer.setSize(1600, 1000);surfacePeer.addAndMakeVisible(root);
    surfacePeer.setVisible(true);surfacePeer.addToDesktop(0);
    root.afterEdit = [&] { document->synchronise(); };
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const std::vector<std::int64_t> &ids) {
        document->setParameterDemand(owner, ids);
    };
    // As ui_main.cpp wires it: a refusal's reason goes to the Status Bar.
    root.devices.gesture = [&](std::int64_t id, const std::string &param, engine::ParamEventKind kind,
                               double value) {
        const bool ok = document->parameterGesture(id, param, kind, value);
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.setSize(1440, 900);
    root.arrangement.geometry.selection.track = 2;
    document->tick(0); // the timer's frame publishes the feed
    root.frame();
    root.frame();
    const bool shown = root.devices.panels.size() == 1 && root.devices.panels[0]->controls.size() == 2 &&
                       root.devices.panels[0]->controls[0]->slider.isVisible();
    check(shown, "the strip shows the device's two controls, from the document's feed");
    if (!shown) {
        document->release();
        return;
    }
    auto &cutoff = root.devices.panels[0]->controls[0]->slider;
    auto *knobs = dynamic_cast<test::Knobs *>(document->session().instanceFor(3));
    const auto titleBefore = root.undoTitle(false);
    cutoff.onDragStart();
    cutoff.setValue(.65, juce::sendNotificationSync);
    cutoff.setValue(.8, juce::sendNotificationSync);
    check(knobs != nullptr && std::abs(knobs->getParam("cutoff").normalized - .8) < 1e-9 &&
              root.undoTitle(false) == titleBefore && root.error().empty(),
          "while the mouse is down the knob moves the plug-in at once and nothing is written");
    cutoff.onDragEnd();
    document->tick(0);
    root.frame();
    check(root.undoTitle(false) == "Undo UI knobs: Cutoff" && root.error().empty() &&
              std::abs(root.devices.panels[0]->controls[0]->slider.getValue() - .8) < 1e-9,
          "mouse up: ONE undo step, named for the device and its control, and the knob stays where it was left");
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-strip-param-undo.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    auto rowsOf = [&] {
        std::size_t rows = 0;
        for (const auto &p : document->session().model().pluginParams)
            rows += p.deviceId == 3 ? 1 : 0;
        return rows;
    };
    auto rowValue = [&] {
        for (const auto &p : document->session().model().pluginParams)
            if (p.deviceId == 3 && p.paramId == "cutoff")
                return p.normalized;
        return -1.0;
    };
    check(root.devices.panels[0]->controls[0]->getWantsKeyboardFocus() && !cutoff.getWantsKeyboardFocus() &&
              root.devices.panels[0]->controls[0]->getNumChildComponents() > 0,
          "a click on the knob lands the focus on its control: the slider declines it (manual 41.6's keys need it)");
    // Live 12 manual 41.6: Delete on a control is Return to Default, where the device's Delete is
    // Delete (row 6). Through the peer, as the key arrives: the focused control takes it.
    {
        UiPeerRoot host;
        host.setSize(root.getWidth(), root.getHeight());
        host.addAndMakeVisible(root);
        host.setVisible(true);
        host.addToDesktop(0);
        std::vector<std::string> submitted;
        root.devices.submit = [&](const std::string &op, Payload) {
            submitted.push_back(op);
            return true;
        };
        auto &control = *root.devices.panels[0]->controls[0];
        control.grabKeyboardFocus();
        auto *peer = host.getPeer();
        const juce::KeyPress del(juce::KeyPress::deleteKey);
        check(peer != nullptr && control.hasKeyboardFocus(false) && peer->handleKeyPress(del) && submitted.empty() &&
                  knobs != nullptr && std::abs(knobs->getParam("cutoff").normalized - .5) < 1e-9 &&
                  root.devices.panels.size() == 1 && root.error().empty(),
              "Delete on the focused knob returns the parameter to the plug-in's default and removes no device");
        document->tick(0);
        root.frame();
        check(root.undoTitle(false) == "Undo UI knobs: Cutoff" && rowsOf() == 1 && std::abs(rowValue() - .5) < 1e-9 &&
                  std::abs(cutoff.getValue() - .5) < 1e-9,
              "Return to Default is one undo step of its own, and the knob shows the default");
        root.devices.panels[0]->grabKeyboardFocus();
        check(root.devices.panels[0]->hasKeyboardFocus(false) && peer->handleKeyPress(del) &&
                  submitted == std::vector<std::string>{"device.remove"},
              "Delete on the focused device is still Delete");
        root.devices.submit = nullptr;
        host.removeChildComponent(&root);
        host.removeFromDesktop();
    }
    check(root.command(AppCommands::Undo) && root.undoTitle(false) == "Undo UI knobs: Cutoff" && rowsOf() == 1 &&
              std::abs(rowValue() - .8) < 1e-9,
          "one Undo takes Return to Default back to where the drag left the parameter");
    check(root.command(AppCommands::Undo) && root.undoTitle(false) == titleBefore && rowsOf() == 0,
          "and one more takes the whole drag back: the parameter is never touched again, as before it (ADR-0124 d4)");
    document->release();
}
// Re-Enable Automation lit, and Automation Arm (N26, worklist C1; Live 12 manual 25.1 and 25.4;
// MR 6.1, audit rows 97 and 133): the control bar's two switches over the document's session,
// wired as ui_main.cpp wires them. A3's device with a lane on its Cutoff, under the numeric id a
// lane's param_ref must parse as; a drag on the knob overrides the lane (ADR-0162).
void automationArm(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "automation-arm.adi", true, test::knobsLoader("7"), error);
    check(document != nullptr, "a project whose plug-ins the loader makes");
    if (!document)
        return;
    document->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'vst3','") +
                                test::kKnobsUid + "','UI knobs')");
    std::vector<OpRequest> make(2);
    make[0].opType = "chain.create";
    make[0].payload = {{"id", 3}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
    make[1].opType = "device.insert";
    make[1].payload = {{"id", 3}, {"chain", 3}, {"ord", 0}, {"ref", 3}, {"name", "UI knobs"}, {"latency", 0}};
    automation::LaneSpec spec;
    spec.id = 1;
    spec.ownerKind = "device";
    spec.ownerId = 3;
    spec.paramRef = "7";
    spec.paramName = "Cutoff";
    spec.defaultValue = .5;
    make.push_back(automation::createLane(spec));
    check(document->ops().submit(std::move(make)).ok && document->synchronise(), "a device on 1 MIDI with a lane on its Cutoff");
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "arm");
    UiPeerRoot surfacePeer;
    surfacePeer.setSize(1600, 1000);surfacePeer.addAndMakeVisible(root);
    surfacePeer.setVisible(true);surfacePeer.addToDesktop(0);
    root.afterEdit = [&] { document->synchronise(); };
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const std::vector<std::int64_t> &ids) {
        document->setParameterDemand(owner, ids);
    };
    root.devices.gesture = [&](std::int64_t id, const std::string &param, engine::ParamEventKind kind,
                               double value) {
        const bool ok = document->parameterGesture(id, param, kind, value);
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.reenableAutomation = [&] {
        const bool ok = document->reenableAutomation();
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.automationArmed = [&] { return document->automationArmed(); };
    root.armAutomation = [&](bool armed) { document->setAutomationArmed(armed); };
    root.setSize(1440, 900);
    root.arrangement.geometry.selection.track = 2;
    document->tick(0); // the timer's frame publishes the feed
    root.frame();
    root.frame();
    auto &reEnable = root.transport.reEnable;
    auto &arm = root.transport.automationArm;
    check(!reEnable.getToggleState() && arm.getToggleState() && reEnable.getTitle() == "Re-Enable Automation" &&
              arm.getTitle() == "Automation Arm",
          "nothing overridden: Re-Enable Automation is unlit, and Automation Arm is on as Live's new Set");
    const bool shown = root.devices.panels.size() == 1 && root.devices.panels[0]->controls.size() == 2 &&
                       root.devices.panels[0]->controls[0]->slider.isVisible();
    check(shown, "the strip shows the automated device's two controls");
    if (!shown) {
        document->release();
        return;
    }
    auto &cutoff = root.devices.panels[0]->controls[0]->slider;
    const auto titleBefore = root.undoTitle(false);
    cutoff.onDragStart();
    cutoff.setValue(.8, juce::sendNotificationSync);
    root.frame();
    check(reEnable.getToggleState() && root.undoTitle(false) == titleBefore && root.error().empty(),
          "with the mouse still down, the moved knob's override lights Re-Enable Automation (manual 25.4), nothing written");
    cutoff.onDragEnd();
    document->tick(0);
    root.frame();
    check(reEnable.getToggleState() && root.undoTitle(false) == "Undo UI knobs: Cutoff",
          "lit after the gesture commits its one undo step");
    if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
        juce::Image picture(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(picture);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-automation-arm.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(picture, *stream);
    }
    const auto titleOverridden = root.undoTitle(false);
    reEnable.onClick();
    root.frame();
    check(!reEnable.getToggleState() && root.undoTitle(false) == titleOverridden && root.error().empty() &&
              !document->session().automationOverridden(),
          "a click follows every envelope again: unlit, no undo step, nothing said");
    reEnable.onClick();
    root.frame();
    check(!reEnable.getToggleState() && root.error().empty(), "with nothing overridden a click changes nothing");
    arm.onClick();
    root.frame();
    check(!arm.getToggleState() && !document->automationArmed() && root.error().empty(),
          "Automation Arm clicked: off on the switch and in the session (manual 25.1)");
    arm.onClick();
    root.frame();
    check(arm.getToggleState() && document->automationArmed(), "and on again");
    // A root with no session (the hooks unset) says so instead of toggling.
    root.reenableAutomation = nullptr;
    root.armAutomation = nullptr;
    root.automationArmed = nullptr;
    reEnable.onClick();
    check(root.error() == "Re-Enable Automation: no session holds automation to re-enable.",
          "without a session, Re-Enable Automation says so");
    arm.onClick();
    root.frame();
    check(root.error() == "Automation Arm: no session to arm." && arm.getToggleState(),
          "and Automation Arm says so, showing Live's default, on");
    document->release();
}
// Arm, monitoring and Arrangement Record (MX-09; Live 12 manual 21.1, 21.4): the strips' Arm and the
// monitoring switch, and the Control Bar's Record and F9, wired as ui_main.cpp wires them, over a
// document whose callback the test drives with an input.
void armAndRecord(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "arm-record.adi", true, {}, error);
    check(document != nullptr, "a project to arm and record in");
    if (!document)
        return;
    document->prepare(48000, 64);
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "record");
    UiPeerRoot surfacePeer;
    surfacePeer.setSize(1600, 1000);
    surfacePeer.addAndMakeVisible(root);
    surfacePeer.setVisible(true);
    surfacePeer.addToDesktop(0);
    root.afterEdit = [&] { document->synchronise(); };
    root.readRecordStates = [&] { return document->recordStates(); };
    root.startRecording = [&](std::int64_t insertTick, bool playing) {
        const bool ok = document->startRecording(insertTick, 2, playing);
        if (!ok)
            root.say(document->error());
        return ok;
    };
    root.stopRecording = [&] { document->stopRecording(); };
    root.recording = [&] { return document->recording(); };
    root.setSize(1440, 900);
    // The application's frame: the timer's tick, the rows handed over, the window's frame.
    std::int64_t now = 0;
    const auto frame = [&] {
        document->tick(++now);
        root.setRecordStates(document->recordStates());
        root.frame();
    };
    frame();
    const auto stripOf = [&](MixerPanel &panel, std::int64_t id) -> MixerStrip * {
        for (const auto &strip : panel.strips())
            if (strip->track() == id)
                return strip.get();
        return nullptr;
    };
    // 3 Audio's row and 2 MIDI's in the side Mixer (4 Audio is below the fold at this size).
    auto *three = stripOf(root.mixer, 4), *midi = stripOf(root.mixer, 3);
    check(three != nullptr && midi != nullptr && !three->arm.getToggleState(), "3 Audio and 2 MIDI, unarmed");
    if (three == nullptr || midi == nullptr) {
        document->release();
        return;
    }
    const auto states = [&] { return document->recordStates(); };
    check(states().at(4).monitor == Monitor::Off, "New's tracks store monitoring Off (Live's default is Auto: AWAITING 20)");
    three->arm.onClick();
    frame();
    check(states().at(4).armed && three->arm.getToggleState() && root.error().empty(),
          "a click on 3 Audio's Arm arms it (track.setArm), lit from the rows");
    midi->arm.onClick();
    frame();
    check(states().at(3).armed && !states().at(4).armed && midi->arm.getToggleState() && !three->arm.getToggleState(),
          "Exclusive Arm: arming 2 MIDI disarms 3 Audio");
    check(root.armTrack(4, true) && root.armTrack(5, true), "Ctrl/Cmd adds 3 Audio and 4 Audio");
    frame();
    check(states().at(3).armed && states().at(4).armed && states().at(5).armed && three->arm.getToggleState(),
          "with Ctrl/Cmd all three stay armed");
    check(!root.armTrack(1, false) && root.error().find("no Arm") != std::string::npos, "Main has no Arm");
    // The monitoring switch: a click on In, Auto or Off in the Mixer View's In/Out section.
    root.command(AppCommands::ToggleMixerView);
    const bool inOutWasOn = root.state().mixerInOut;
    if (!inOutWasOn)
        root.command(AppCommands::ToggleMixerInOut);
    frame();
    if (auto *wide = stripOf(root.mixerView, 4)) {
        const auto keptHeight = wide->getHeight();
        wide->setSize(wide->getWidth(), 360);
        check(wide->monitorAt({10, 19 + 65}) == Monitor::In && wide->monitorAt({35, 19 + 65}) == Monitor::Auto &&
                  wide->monitorAt({58, 19 + 65}) == Monitor::Off && !wide->monitorAt({10, 19 + 20}),
              "In, Auto and Off each where the switch draws them, Audio From not");
        wide->setSize(wide->getWidth(), keptHeight);
    } else
        check(false, "3 Audio's strip in the Mixer View");
    if (!inOutWasOn)
        root.command(AppCommands::ToggleMixerInOut);
    root.command(AppCommands::ToggleMixerView);
    check(root.setMonitor(4, Monitor::In), "3 Audio's monitoring to In");
    frame();
    check(states().at(4).monitor == Monitor::In && states().at(5).monitor == Monitor::Off &&
              root.recordState(4).monitor == Monitor::In,
          "track.setMonitorMode In on 3 Audio alone");
    check(root.setMonitor(4, Monitor::In) && root.error().empty(), "the same choice again is accepted");
    frame();
    check(root.command(AppCommands::Undo), "undo");
    frame();
    check(states().at(4).monitor == Monitor::Off, "the same choice again changed nothing: one Undo puts 3 Audio back on Off");
    check(root.command(AppCommands::Redo), "redo");
    frame();
    check(!root.setMonitor(1, Monitor::In) && root.error().find("no input") != std::string::npos, "Main has no monitoring");
    // Arrangement Record: F9 records both armed audio tracks; 2 MIDI, armed too, records nothing yet.
    juce::ApplicationCommandInfo recordInfo(AppCommands::ArrangementRecord);
    commands.getCommandInfo(AppCommands::ArrangementRecord, recordInfo);
    check(recordInfo.shortName == "Arrangement Record" &&
              recordInfo.defaultKeypresses.contains(juce::KeyPress(juce::KeyPress::F9Key)),
          "Arrangement Record is named as Live's and on F9 (manual 21.1)");
    auto &record = root.transport.record;
    check(record.getTitle() == "Arrangement Record" && !record.getToggleState(), "the Record switch, unlit");
    record.onClick();
    frame();
    check(document->recording() && record.getToggleState() && root.error() == "", "Record records; lit");
    std::array<float, 64> left{}, right{}, outL{}, outR{};
    const float *inputs[]{left.data(), right.data()};
    float *outputs[]{outL.data(), outR.data()};
    engine::AudioIo io;
    io.in = inputs;
    io.numIn = 2;
    io.out = outputs;
    io.numOut = 2;
    io.frames = 64;
    std::int64_t stream = 0;
    const auto block = [&] {
        for (std::size_t i = 0; i < 64; ++i) {
            left[i] = .25f;
            right[i] = -.25f;
        }
        io.streamTimeSamples = stream;
        document->process(io);
        stream += 64;
    };
    for (int b = 0; b < 16; ++b)
        block();
    check(root.command(AppCommands::ArrangementRecord), "F9's command again");
    frame();
    check(!document->recording() && !record.getToggleState() && document->mailbox().desiredPlaying(),
          "stops the recording; the transport plays on, as Live's");
    for (int i = 0; i < 5000 && document->landingTake(); ++i) {
        block();
        document->tick(++now);
        if (document->landingTake())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto landed = document->takeLanded();
    check(landed && landed->ok && landed->clips.size() == 2 && landed->message == "Recorded 2 takes.",
          "both armed tracks' takes land together");
    frame();
    check(root.reader().findTrack(4)->clips.size() == 1 && root.reader().findTrack(5)->clips.size() == 1,
          "a clip on each armed track");
    check(root.command(AppCommands::Undo), "one Undo");
    frame();
    check(root.reader().findTrack(4)->clips.empty() && root.reader().findTrack(5)->clips.empty(),
          "takes both takes back");
    // Punch-in: Record while playing records from the playhead, and Stop ends the take with the transport.
    check(root.command(AppCommands::ArrangementRecord) && document->recording() && document->mailbox().desiredPlaying(),
          "Record while playing records; the transport keeps rolling");
    for (int b = 0; b < 8; ++b)
        block();
    check(root.command(AppCommands::Stop), "Stop mid-take");
    check(!document->recording() && !document->mailbox().desiredPlaying(), "Stop ends the recording and the transport");
    for (int i = 0; i < 5000 && document->landingTake(); ++i) {
        block();
        document->tick(++now);
        if (document->landingTake())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    landed = document->takeLanded();
    check(landed && landed->ok && landed->clips.size() == 2, "the punched-in takes land");
    frame();
    // A Count-In is playing for the window: Space during it stops, it does not start again.
    {
        OpRequest countIn;
        countIn.opType = "transport.setCountIn";
        countIn.payload = {{"bars", 1}};
        check(document->ops().submit(countIn).ok && document->synchronise(), "Count-In of one bar");
        frame();
        block();
        check(root.command(AppCommands::ArrangementRecord) && document->recording(), "Record from stopped, with a Count-In");
        block();
        check(document->mailbox().playing() && document->session().transport().countInRemaining() > 0,
              "during the Count-In the window reads the transport as playing");
        check(root.command(AppCommands::PlayStop) && !document->recording() && !document->mailbox().desiredPlaying(),
              "Space during the Count-In stops it and the recording");
        block();
        check(!document->mailbox().playing() && document->session().transport().countInRemaining() == 0,
              "the Count-In is cancelled");
        for (int i = 0; i < 5000 && document->landingTake(); ++i) {
            block();
            document->tick(++now);
            if (document->landingTake())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        (void)document->takeLanded();
    }
    // With the loop on, Record says it waits for take lanes.
    {
        OpRequest loop;
        loop.opType = "transport.setLoop";
        loop.payload = {{"on", true}, {"start", 0}, {"end", 3840}};
        check(document->ops().submit(loop).ok && document->synchronise(), "the loop on");
        frame();
        check(!root.command(AppCommands::ArrangementRecord) && !document->recording() &&
                  root.error().find("loop") != std::string::npos,
              "Record with the loop on is refused, saying why");
    }
    // Without the application's hooks Record says where it runs.
    root.startRecording = nullptr;
    check(!root.command(AppCommands::ArrangementRecord) && root.error().find("in the application") != std::string::npos,
          "outside the application Record says it records there");
    // MIDI Arrangement Overdub: the window's own switch.
    auto &overdub = root.transport.overdub;
    overdub.onClick();
    frame();
    check(root.midiOverdub() && overdub.getToggleState(), "Overdub on, lit");
    overdub.onClick();
    frame();
    check(!root.midiOverdub() && !overdub.getToggleState(), "and off");
    document->release();
}
// EX-11, the Export page (Live 12 manual 5.2.1): the choices in Live's order, what the renderer cannot do
// yet refused with its reason, the range from the selection, the loop or the song, and the job the choices
// make, rendered to a WAV by the engine's renderer.
void exportPage(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "export.adi", true, {}, error);
    check(document != nullptr, "a project to export");
    if (!document)
        return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "export");
    root.setSize(1440, 900);
    root.frame();
    // The command: on Ctrl/Cmd+Shift+R, named as Live's; outside the application it says where it runs.
    juce::ApplicationCommandInfo info(AppCommands::ExportAudio);
    commands.getCommandInfo(AppCommands::ExportAudio, info);
    check(info.shortName == "Export Audio/Video..." &&
              info.defaultKeypresses.contains(juce::KeyPress('r', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)),
          "Export Audio/Video, on Ctrl/Cmd+Shift+R");
    check(!root.command(AppCommands::ExportAudio) && root.error() == "Export runs in the application.",
          "outside the application it says where it runs");
    std::optional<ExportChoices> asked;
    root.exportAudio = [&](const ExportChoices &c) { asked = c; };
    // With nothing selected: the loop brace, whether the loop is on or not (New's brace: four bars), Main,
    // at the project's rate.
    const auto &transport = root.reader().transport();
    check(root.command(AppCommands::ExportAudio) && asked && !transport.loopEnabled &&
              asked->startTicks == transport.loopStartTicks &&
              asked->lengthTicks == transport.loopEndTicks - transport.loopStartTicks && asked->lengthTicks == 4 * textproj::kWhole &&
              asked->track == 0 && asked->sampleRate == root.reader().sampleRate(),
          "the loop brace with the loop off, Main, at the project's rate");
    auto &sel = root.arrangement.geometry.selection;
    sel.start = 3840;
    sel.end = 3840 * 5;
    check(root.command(AppCommands::ExportAudio) && asked->startTicks == 3840 && asked->lengthTicks == 3840 * 4,
          "a time selection is the range, as Live's");
    sel.start = 3840 * 5;
    sel.end = 3840;
    check(root.command(AppCommands::ExportAudio) && asked->startTicks == 3840 && asked->lengthTicks == 3840 * 4,
          "a selection made right to left is the same range");
    sel.start = sel.end = 0;
    // The page: Live's rows in Live's order, Main and the tracks to render.
    ExportChoices c = *asked;
    c.lengthTicks = textproj::kWhole * 2; // two 4/4 bars
    ExportPage page(c, {{4, "3 Audio"}, {6, "A Reverb"}}, root.reader().meters());
    std::vector<juce::String> titles;
    for (auto *control : std::initializer_list<juce::Component *>{&page.renderedTrack, &page.start, &page.length,
                                                                   &page.includeReturnAndMain, &page.renderAsLoop,
                                                                   &page.convertToMono, &page.normalize,
                                                                   &page.createAnalysisFile, &page.sampleRate,
                                                                   &page.encodePcm, &page.fileType, &page.bitDepth,
                                                                   &page.dither, &page.encodeMp3, &page.video})
        titles.push_back(control->getTitle());
    check(titles == std::vector<juce::String>{"Rendered Track", "Render Start", "Render Length",
                                              "Include Return and Main Effects", "Render as Loop", "Convert to Mono",
                                              "Normalize", "Create Analysis File", "Sample Rate", "Encode PCM",
                                              "File Type", "Bit Depth", "Dither Options", "Encode MP3", "Export Video"},
          "Live's choices in Live's order");
    check(page.renderedTrack.getNumItems() == 3 && page.renderedTrack.getText() == "Main" &&
              page.start.getText() == "1.1.1" && page.length.getText() == "2.0.0" && page.refusals().empty() &&
              page.exportButton.isEnabled(),
          "Main, from 1.1.1 for 2.0.0: Export is offered");
    {
        // The range as filled is kept exact while its fields are unchanged; the project's rate is offered
        // when the list lacks it.
        ExportChoices odd = c;
        odd.lengthTicks = textproj::kWhole + 7;
        odd.sampleRate = 192000;
        ExportPage exact(odd, {}, root.reader().meters());
        check(exact.choices().lengthTicks == textproj::kWhole + 7 && exact.choices().sampleRate == 192000 &&
                  exact.sampleRate.getText() == "192000",
              "an unedited range stays exact past the sixteenths it shows, and 192 kHz stays 192 kHz");
    }
    page.encodeMp3.setToggleState(true, juce::dontSendNotification);
    page.encodeMp3.onClick();
    check(!page.exportButton.isEnabled() && page.refusals().size() == 1 &&
              page.refusals()[0].find("MP3 waits") != std::string::npos,
          "MP3 on: Export waits, saying why");
    page.encodeMp3.setToggleState(false, juce::dontSendNotification);
    page.encodeMp3.onClick();
    page.renderedTrack.setSelectedId(2, juce::sendNotificationSync);
    check(page.choices().track == 4 && !page.exportButton.isEnabled() &&
              page.refusals()[0].find("Include Return and Main Effects off") != std::string::npos,
          "a track through the returns and Main waits; the reason says what renders now");
    page.includeReturnAndMain.setToggleState(false, juce::dontSendNotification);
    page.includeReturnAndMain.onClick();
    check(page.exportButton.isEnabled(), "with the effects off, the track's own signal renders");
    page.length.setText("one bar", false);
    page.length.onTextChange();
    check(!page.exportButton.isEnabled() && page.refusals()[0].find("Render Length is not a length") != std::string::npos,
          "a length that is not one is refused");
    page.length.setText("1.2.0", false);
    page.length.onTextChange();
    check(page.exportButton.isEnabled() && page.choices().lengthTicks == textproj::kWhole + textproj::kWhole / 2,
          "1.2.0 is a bar and two beats");
    std::optional<ExportChoices> exported;
    page.onExport = [&](const ExportChoices &e) { exported = e; };
    page.exportButton.onClick();
    check(exported && exported->track == 4 && !exported->includeReturnAndMain, "Export hands over the choices");
    // The job, and a real render of the project: Main for one bar, 16-bit, at 44.1 kHz.
    ExportChoices one;
    one.lengthTicks = textproj::kWhole;
    one.sampleRate = 44100;
    one.bitDepth = 16;
    one.dither = "No Dither";
    one.normalize = true;
    const auto now = document->view().current();
    const auto tempo = now ? now->tempo : nullptr;
    check(tempo != nullptr, "the project's tempo map");
    if (!tempo) {
        document->release();
        return;
    }
    auto job = exportJob(one, document->store().path(), dir / "Song {mix}.wav", *tempo);
    const auto expected = exportSample(*tempo, textproj::kWhole, 44100);
    check(job.directory == dir && job.nameTemplate == "Song {{mix}}" && job.beginSample == 0 && job.endSample == expected &&
              job.session.sampleRate == 44100 && job.encoding.format == audio::WavFormat::Pcm16 &&
              job.encoding.dither == audio::WavDither::None && job.normalization.mode == render::NormalizeMode::Peak &&
              job.source.trackId == 0,
          "the job: the chosen folder and name, the range at the chosen rate, 16-bit, no dither, Normalize");
    one.normalize = false; // a silent song has no peak to normalize
    // New's Set holds Reverb and Delay, which a build without Pd cannot load, and the renderer renders
    // only a project whose every device loads: an empty project here.
    {
        StoreError made{};
        auto empty = Store::createEmpty(dir / "export-empty.adi", made);
        check(empty != nullptr, "an empty project to render");
        if (empty)
            empty->db().exec("INSERT INTO project(id,name) VALUES(1,'Export'); INSERT INTO tracks(id,kind,name) VALUES(1,'master','Main')");
    }
    job = exportJob(one, dir / "export-empty.adi", dir / "Song {mix}.wav", *tempo);
    const auto result = render::renderWav(job, {});
    if (result.state != render::JobState::Completed)
        std::printf("render: %s\n", result.error.c_str());
    check(result.state == render::JobState::Completed && result.output == dir / "Song {mix}.wav" &&
              std::filesystem::exists(result.output) && result.framesWritten == static_cast<std::uint64_t>(expected),
          "the renderer writes the chosen file, the whole range");
    document->release();
}
// MX-23 and SRF-02 from the strip: the fader's own drag is the document's live gesture, one undo step at
// the release, the selected tracks moving together by the same move, Esc putting them back.
void mixerDragLive(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "drag-live.adi", true, {}, error);
    check(document != nullptr, "a project for the strip's drags");
    if (!document)
        return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "drag");
    root.afterEdit = [&] { document->synchronise(); };
    root.beginMixerDrag = [&](const gesture::RelativeScalarSelection &selection, std::optional<double> value,
                              const std::string &label) {
        return document->beginMixerDrag(selection, value, label);
    };
    root.mixerDrag = [&](double value) { return document->mixerDrag(value); };
    root.endMixerDrag = [&] { return document->endMixerDrag(); };
    root.cancelMixerDrag = [&] { document->cancelMixerDrag(); };
    UiPeerRoot surfacePeer;
    surfacePeer.setSize(1600, 1000);
    surfacePeer.addAndMakeVisible(root);
    surfacePeer.setVisible(true);
    surfacePeer.addToDesktop(0);
    root.setSize(1440, 900);
    root.frame();
    // A drag is the mouse held down: the strip goes live only then (a typed value or a double-click does not).
    const auto press = [] { juce::ModifierKeys::currentModifiers = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier); };
    const auto letGo = [] { juce::ModifierKeys::currentModifiers = juce::ModifierKeys(); };
    MixerStrip *three = nullptr, *midi = nullptr;
    for (const auto &strip : root.mixer.strips()) {
        if (strip->track() == 4)
            three = strip.get();
        if (strip->track() == 3)
            midi = strip.get();
    }
    check(three != nullptr && midi != nullptr, "3 Audio's and 2 MIDI's rows");
    if (three == nullptr || midi == nullptr) {
        document->release();
        return;
    }
    const auto volumeOf = [&](std::int64_t id) {
        SQLite::Statement q(document->store().db(), "SELECT volume_db FROM mixer_strip WHERE track_id = ?");
        q.bind(1, id);
        return q.executeStep() ? q.getColumn(0).getDouble() : 999.0;
    };
    const auto opCount = [&] { return document->store().db().execAndGet("SELECT count(*) FROM ops").getInt(); };
    const auto before = opCount();
    // A drag: start, three moves, release.
    press();
    three->volume.onDragStart();
    for (const double db : {-3.0, -6.0, -9.0}) {
        three->volume.setValue(db, juce::dontSendNotification);
        three->volume.onValueChange();
    }
    check(document->mixerDragging() && opCount() == before && volumeOf(4) == 0,
          "while the fader moves nothing is written; the document's gesture holds it");
    letGo();
    three->volume.onDragEnd();
    root.frame();
    check(!document->mixerDragging() && opCount() == before + 1 && std::abs(volumeOf(4) + 9) < 1e-6 &&
              root.undoTitle(false) == "Undo Change Track Volume",
          "released, one op and one undo step, at the value let go");
    // Two tracks selected, the drag on one of them: both move by the same amount.
    auto &sel = root.arrangement.geometry.selection;
    sel.track = 3;
    sel.toTrack = 4;
    root.frame();
    press();
    midi->volume.onDragStart();
    midi->volume.setValue(-6.0, juce::dontSendNotification);
    midi->volume.onValueChange();
    letGo();
    midi->volume.onDragEnd();
    root.frame();
    check(std::abs(volumeOf(3) + 6) < 1e-6 && std::abs(volumeOf(4) + 15) < 1e-6 && document->ops().nextUndo() &&
              document->ops().nextUndo()->opCount == 2 && document->ops().nextUndo()->label == "Change Track Volume",
          "with 2 MIDI and 3 Audio selected, a drag on 2 MIDI moves both by -6 dB, in one step");
    check(root.command(AppCommands::Undo) && std::abs(volumeOf(3)) < 1e-6 && std::abs(volumeOf(4) + 9) < 1e-6,
          "one Undo takes both back");
    sel.track = sel.toTrack = 0;
    root.frame();
    // Esc while dragging puts the value back, and the release commits nothing.
    const auto stepBefore = document->ops().nextUndo() ? document->ops().nextUndo()->txnId : 0;
    press();
    three->volume.onDragStart();
    three->volume.setValue(-30.0, juce::dontSendNotification);
    three->volume.onValueChange();
    auto *focused = juce::Component::getCurrentlyFocusedComponent();
    check(focused == three && focused->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && !document->mixerDragging(),
          "the dragged strip has the keys, and Esc cancels the drag");
    root.frame();
    letGo();
    three->volume.onDragEnd();
    root.frame();
    check(std::abs(three->volume.getValue() + 9) < 1e-6, "the fader shows the value it was put back to");
    check((document->ops().nextUndo() ? document->ops().nextUndo()->txnId : 0) == stepBefore && std::abs(volumeOf(4) + 9) < 1e-6,
          "and the release after it writes nothing");
    // Pan, the same way.
    press();
    three->pan.onDragStart();
    three->pan.setValue(-.5, juce::dontSendNotification);
    three->pan.onValueChange();
    letGo();
    three->pan.onDragEnd();
    root.frame();
    check(root.undoTitle(false) == "Undo Change Track Panning" &&
              std::abs(document->store().db().execAndGet("SELECT pan FROM mixer_strip WHERE track_id = 4").getDouble() + .5) < 1e-9,
          "a pan drag is one step too");
    // A click with no move on an off-grid value, two tracks selected: nothing is written.
    {
        OpRequest odd;
        odd.opType = "mixer.setVolume";
        odd.payload = {{"id", 4}, {"db", -4.7321}};
        check(document->ops().submit(odd).ok && document->synchronise(), "3 Audio at -4.7321 dB");
        root.frame();
        sel.track = 3;
        sel.toTrack = 4;
        root.frame();
        const auto step = document->ops().nextUndo() ? document->ops().nextUndo()->txnId : 0;
        const auto two = volumeOf(3);
        press();
        three->volume.onDragStart();
        letGo();
        three->volume.onDragEnd();
        root.frame();
        check((document->ops().nextUndo() ? document->ops().nextUndo()->txnId : 0) == step && volumeOf(3) == two &&
                  std::abs(volumeOf(4) + 4.7321) < 1e-9,
              "a click on the fader that does not move writes nothing, on either selected track");
        sel.track = sel.toTrack = 0;
        root.frame();
    }
    // A typed value or a double-click is not a drag: it sets the clicked control alone, at once.
    three->volume.onDragStart();
    three->volume.setValue(-3.0, juce::dontSendNotification);
    three->volume.onValueChange();
    three->volume.onDragEnd();
    root.frame();
    check(!document->mixerDragging() && std::abs(volumeOf(4) + 3) < 1e-6, "without the mouse held, the value commits at the end");
    // Without the live gesture, the release commits as before.
    root.beginMixerDrag = nullptr;
    press();
    three->volume.onDragStart();
    three->volume.setValue(-12.0, juce::dontSendNotification);
    three->volume.onValueChange();
    letGo();
    three->volume.onDragEnd();
    root.frame();
    check(std::abs(volumeOf(4) + 12) < 1e-6, "without the live gesture the release commits the value");
    document->release();
}
// AU-01: Automation Mode in the Arrangement (Live 12 manual 25.2, 25.3): a track's Volume envelope on its
// lane; a click adds a breakpoint (the lane made in the same step), a drag moves one, a double-click or
// Delete removes one; each one undo step.
void automationMode(AppCommands &commands, const std::filesystem::path &dir) {
    std::string error;
    auto document = ProjectDocument::open(dir / "automation-mode.adi", true, {}, error);
    check(document != nullptr, "a project to automate");
    if (!document)
        return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands, document->views(),
                          document->sessionState(), "automate");
    UiPeerRoot surfacePeer;
    surfacePeer.setSize(1600, 1000);
    surfacePeer.addAndMakeVisible(root);
    surfacePeer.setVisible(true);
    surfacePeer.addToDesktop(0);
    root.afterEdit = [&] { document->synchronise(); };
    root.setSize(1440, 900);
    root.frame();
    auto &canvas = root.arrangement.canvas;
    check(!root.automationMode(), "Automation Mode is off in a new window");
    root.namesBar.automation.onClick();
    root.frame();
    check(root.automationMode() && root.namesBar.automation.getToggleState(), "the bar's switch turns it on, lit");
    const auto bodies = canvas.laneBodies();
    const ArrangementCanvas::LaneBody *audio = nullptr;
    for (const auto &b : bodies)
        if (b.track->id == 4)
            audio = &b;
    check(audio != nullptr, "3 Audio's lane");
    if (audio == nullptr) {
        document->release();
        return;
    }
    const auto laneOf = [&] { return trackEnvelope(*document->view().current(), 4); };
    const float midY = (audio->top + audio->bottom) / 2;
    const float barX = static_cast<float>(root.arrangement.geometry.x(textproj::kWhole));
    canvas.mouseDown(pressAt(canvas, {barX + 1, midY}));
    canvas.mouseUp(pressAt(canvas, {barX + 1, midY}));
    root.frame();
    auto env = laneOf();
    check(env.lane != nullptr && env.data != nullptr && env.data->points.size() == 1 &&
              env.data->points[0].time == textproj::kWhole && std::abs(env.data->points[0].value + 32) < 1.5 &&
              root.undoTitle(false) == "Undo Add Automation Point",
          "a click on the lane adds a breakpoint on the grid at the value there, the Volume lane with it, one step");
    check(env.lane->paramRef == "volume" && env.lane->valueDomain == "real" && env.lane->ownerKind == "track",
          "the lane is the track's Volume in dB, as the engine plays it");
    check(env.data->points[0].curve == 1, "a new breakpoint is linear to the next, as the lane draws it");
    // Drag it a bar later and up.
    const auto pointAt = juce::Point<float>(barX, (audio->top + audio->bottom) / 2);
    const auto hit = canvas.automationPointAt(pointAt);
    check(hit && hit->track == 4, "the breakpoint is found where it is drawn");
    const float twoBars = static_cast<float>(root.arrangement.geometry.x(2 * textproj::kWhole));
    const float upper = audio->top + (audio->bottom - audio->top) * .25f;
    canvas.mouseDown(pressAt(canvas, pointAt));
    canvas.mouseDrag(dragAt(canvas, pointAt, {twoBars + 1, upper}));
    check(laneOf().data->points[0].time == textproj::kWhole, "while it is dragged nothing is written");
    canvas.mouseUp(dragAt(canvas, pointAt, {twoBars + 1, upper}));
    root.frame();
    env = laneOf();
    check(env.data->points.size() == 1 && env.data->points[0].time == 2 * textproj::kWhole &&
              env.data->points[0].value > -32 && root.undoTitle(false) == "Undo Move Automation Point",
          "let go, it is a bar later and higher: one step");
    check(root.command(AppCommands::Undo) && laneOf().data->points[0].time == textproj::kWhole, "Undo puts it back");
    check(root.command(AppCommands::Redo), "and Redo moves it again");
    root.frame();
    // A double-click on it deletes it.
    const auto moved = canvas.automationPointAt(
        {twoBars, audio->bottom - static_cast<float>((laneOf().data->points[0].value + 70) / 76) * (audio->bottom - audio->top)});
    check(moved.has_value(), "the moved breakpoint is found where it is drawn now");
    if (moved) {
        canvas.mouseDoubleClick(pressAt(canvas, {twoBars, audio->bottom - static_cast<float>((laneOf().data->points[0].value + 70) / 76) * (audio->bottom - audio->top)}));
        root.frame();
        check(laneOf().lane == nullptr && root.undoTitle(false) == "Undo Delete Automation Point",
              "a double-click on the last breakpoint deletes it and its lane, so the fader plays again: one step");
    }
    // Chosen, Delete takes it away.
    canvas.mouseDown(pressAt(canvas, {barX + 1, midY}));
    canvas.mouseUp(pressAt(canvas, {barX + 1, midY}));
    root.frame();
    canvas.mouseDown(pressAt(canvas, {barX, midY}));
    canvas.mouseUp(pressAt(canvas, {barX, midY}));
    check(canvas.chosenAutomationPoint().has_value(), "a click on a breakpoint chooses it");
    check(canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)) && laneOf().lane == nullptr,
          "Delete takes the chosen breakpoint away");
    // The lane's foot is silence, as the fader's.
    canvas.mouseDown(pressAt(canvas, {barX + 1, audio->scaleBottom}));
    canvas.mouseUp(pressAt(canvas, {barX + 1, audio->scaleBottom}));
    root.frame();
    check(laneOf().data != nullptr && laneOf().data->points.size() == 1 && laneOf().data->points[0].value == -150,
          "a breakpoint at the lane's foot is silence (-150 dB, as the fader writes it)");
    root.namesBar.automation.onClick();
    check(!root.automationMode(), "and the switch turns Automation Mode off");
    document->release();
}
// The Mix View (ADR-0209, ruled 1 November): F4 and the control bar's Mix button open it in place
// of the arrangement; its tabs in the draft's order, STACK first; kept per window.
void mixViewShell(AppCommands &commands, const std::filesystem::path &dir, bool headlessOnly = false) {
    std::fputs("[shell] mix fixture entered\n", stderr);
    StoreError err{};
    auto store = Store::createEmpty(dir / "mixview.adi", err);
    check(store != nullptr, "the Mix View's project opens");
    if (!store)
        return;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Mix')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    ViewStateStore persistence(*store);
    SessionState session(*store);
    bool madeAll = true;
    std::int64_t index = 0;
    for (const auto &[id, kind, name] : {std::tuple{1, "master", "Main"}, std::tuple{10, "audio", "Kick"},
                                         std::tuple{11, "audio", "Bass"}, std::tuple{12, "midi", "Lead"}}) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", name}, {"index", index++}};
        r.label = "Create track";
        madeAll = submitter.submit(std::move(r)).ok && madeAll;
    }
    check(madeAll, "Kick, Bass, Lead and Main");
    // Keep the three long-lived roots off the Debug test thread stack.
    const auto rootStorage = std::make_unique<AdiRootComponent>(view, submitter, mailbox, commands,
                                                               persistence, session, "mix");
    auto &root = *rootStorage;
    std::fputs("[shell] mix fixture root constructed\n", stderr);
    root.setSize(1440, 900);
    root.mark(DirtySet::All);
    root.frame();
    auto &panel = root.mixViewPanel;
    const auto lanes = root.arrangement.getBounds();
    const auto controls = root.mixerArea();
    check(!panel.isVisible() && root.arrangement.isVisible() && !root.state().mixView,
          "closed by default: the arrangement as Live's");
    commands.activate(root);
    juce::ApplicationCommandInfo info(AppCommands::ToggleMixView);
    commands.getCommandInfo(AppCommands::ToggleMixView, info);
    check(info.shortName == "Mix View" && info.defaultKeypresses.size() == 1 &&
              info.defaultKeypresses[0] == juce::KeyPress(juce::KeyPress::F4Key) &&
              (info.flags & juce::ApplicationCommandInfo::isTicked) == 0,
          "the View menu's Mix View, on F4 (ADR-0209 d9)");
    check(root.keyPressed(juce::KeyPress(juce::KeyPress::F4Key)), "F4 opens it");
    root.frame();
    check(panel.stack.model().windowView().channelScope == stack::ChannelScope::WithTools &&
              panel.stack.channelScope.getSelectedId() == 1 && panel.stack.model().shownCount() == 0 &&
              panel.stack.emptyMessage().contains("All channels") && panel.stack.emptyMessage().contains("+ Function"),
          "STACK starts with tool channels and explains how to add the first tool");
    {
        const auto selectionBefore = root.arrangement.geometry.selection;
        const auto opsBefore = store->db().execAndGet("SELECT count(*) FROM ops").getInt();
        panel.stack.focus(999);
        check(panel.stack.model().windowView().channelScope == stack::ChannelScope::WithTools &&
                  panel.stack.channelScope.getSelectedId() == 1,
              "an invalid channel shortcut leaves the default tool scope unchanged");
        root.arrangement.geometry.selection.track = 10;
        check(root.command(AppCommands::ShowChannelEq) && panel.stack.focused() == 10 &&
                  panel.stack.model().shownIndex(10) &&
                  panel.stack.model().windowView().channelScope == stack::ChannelScope::AllChannels &&
                  panel.stack.channelScope.getSelectedId() == 2,
              "an explicit channel shortcut visibly selects All channels and lands on an empty chain");
        check(store->db().execAndGet("SELECT count(*) FROM ops").getInt() == opsBefore &&
                  panel.stack.model().moduleKindCount() == 0,
              "landing before the first tool creates no processor or undo operation");
        panel.stack.channelScope.setSelectedId(1, juce::sendNotificationSync);
        root.arrangement.geometry.selection = selectionBefore;
        root.publishSelection();
    }
    {
        const auto previousPublication = root.devices.publication;
        const auto selectionBefore = root.arrangement.geometry.selection;
        const auto undoBefore = root.undoTitle(false);
        auto publication = std::make_shared<ParameterPublication>();
        const auto add = [&](std::int64_t id, std::int64_t track, const char *name, const char *uid, bool plugin) {
            DevicePanelData data;
            data.id = id; data.track = track; data.chain = track; data.instance = static_cast<std::uint64_t>(id);
            data.name = name; data.uid = uid; data.plugin = plugin;
            data.available = false; data.enabled = false;
            publication->push_back(std::move(data));
        };
        add(101, 10, "Before", "ordinary", true);
        add(102, 10, "Renamed shaping tool", "org.adidaw.mix.eq", false);
        add(103, 10, "After", "ordinary", true);
        add(104, 11, "EQ", "org.adidaw.mix.eq", true);
        add(105, 1, "Finisher", "org.adidaw.mix.limiter", false);
        root.devices.publication = [&] { return publication; };
        auto &surface = panel.stack;
        const auto time = static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes());
        surface.frameAt(root.reader(), time);
        check(surface.model().shownCount() == 2 && surface.model().shownTrack(0) == 10 &&
                  surface.model().shownTrack(1) == 1,
              "native identity includes renamed bypassed unavailable tools; a plug-in spoofing its UID stays excluded");
        std::vector<std::int64_t> order;
        if (const auto row = surface.model().shownIndex(10))
            surface.model().forEachChainItem(*row, [&](const stack::ItemGeo &item) { order.push_back(item.ref.device); });
        check(order == std::vector<std::int64_t>{101, 102, 103},
              "eligible channel retains every actual insert in chain order");
        const auto original = surface.model().chainRef(10, 102);
        surface.focus(10);
        check(surface.focused() == 10 &&
                  surface.model().windowView().channelScope == stack::ChannelScope::WithTools &&
                  surface.channelScope.getSelectedId() == 1,
              "an eligible native channel shortcut preserves With tools");
        root.arrangement.geometry.selection = selectionBefore;
        root.publishSelection();
        publication = std::make_shared<ParameterPublication>(*publication);
        (*publication)[1].name = "Another display name";
        (*publication)[1].enabled = true;
        (*publication)[1].available = true;
        surface.frameAt(root.reader(), time + 1);
        check(surface.model().shownCount() == 2 && surface.model().chainRef(10, 102) == original,
              "rename bypass and load-state updates retain native membership and device identity");
        publication = std::make_shared<ParameterPublication>(*publication);
        (*publication)[1].plugin = true;
        surface.frameAt(root.reader(), time + 2);
        check(surface.model().shownCount() == 1 && surface.model().shownTrack(0) == 1,
              "format identity changes invalidate membership even with a native-looking UID");
        publication = std::make_shared<ParameterPublication>(*publication);
        (*publication)[4].uid = "ordinary";
        surface.frameAt(root.reader(), time + 3);
        check(surface.model().shownCount() == 0, "last native UID disappearing removes its channel from With tools");
        surface.channelScope.setSelectedId(2, juce::sendNotificationSync);
        check(surface.model().shownCount() == 4 && surface.model().moduleKindCount() == 0,
              "All channels restores empty and ordinary chains without reserved modules");
        surface.search.setText("Bass", false);
        surface.search.onTextChange();
        check(surface.model().shownCount() == 1 && surface.model().shownTrack(0) == 11 &&
                  root.arrangement.geometry.selection == selectionBefore && root.undoTitle(false) == undoBefore,
              "scope and search are presentation only and never select or edit all matches");
        surface.search.setText({}, false);
        surface.search.onTextChange();
        root.devices.publication = previousPublication ? previousPublication
            : [] { return std::shared_ptr<const ParameterPublication>{}; };
        surface.frameAt(root.reader(), time + 4);
        root.devices.publication = previousPublication;
        const auto originalBounds = surface.getBounds();
        for (const int width : {900, 720, 600, 520, 360, 320}) {
            surface.setSize(width, 460);
            bool contained = true;
            const std::array<juce::Component *, 6> toolbar{&surface.channelScope, &surface.search, &surface.rowSize,
                                                         &surface.autoZoom, &surface.pinsOnly, &surface.navigator};
            for (const auto *control : toolbar)
                contained = contained && surface.getLocalBounds().contains(control->getBounds()) &&
                    control->getBottom() <= surface.display().getY();
            for (std::size_t i = 0; i < toolbar.size(); ++i)
                for (std::size_t j = i + 1; j < toolbar.size(); ++j)
                    contained = contained && !toolbar[i]->getBounds().intersects(toolbar[j]->getBounds());
            check(contained, "scope/search and existing toolbar controls fit narrow widths without overlap");
        }
        surface.setBounds(originalBounds);
    }
    panel.stack.channelScope.setSelectedId(2, juce::sendNotificationSync);
    check(root.state().mixView && panel.isVisible() && !root.arrangement.isVisible() && !root.mixer.isVisible() &&
              panel.getBounds() == lanes.getUnion(controls),
          "in place of the arrangement and its track controls");
    check(root.browser.isVisible() && root.strip.isVisible() && root.devices.isVisible(),
          "the browser, the strip down the edge and the Detail View stay");
    check(root.transport.mix.getToggleState(), "the control bar's Mix button is lit");
    commands.getCommandInfo(AppCommands::ToggleMixView, info);
    check((info.flags & juce::ApplicationCommandInfo::isTicked) != 0, "and the menu's item ticked");
    // The tabs: the draft's five, in its order and names; STACK shown first, P1's only tab.
    bool named = true;
    for (int i = 0; i < MixViewPanel::kTabCount; ++i)
        named = named && panel.tabs[static_cast<std::size_t>(i)]->getButtonText() ==
                             MixViewPanel::kTabNames[static_cast<std::size_t>(i)];
    check(named && panel.tabs[1]->getButtonText() == "STACK", "OVERVIEW, STACK, SCOPE & PHASE, SPECTROGRAM, CTZ CASCADE");
    check(panel.tab() == MixViewPanel::Tab::Stack && panel.tabs[1]->getToggleState() && panel.stack.isVisible() &&
              panel.stack.model().shownCount() == 4,
          "STACK shows, lit, with a row for each channel");
    check(panel.stack.getBounds() == panel.body() && panel.body().getY() >= MixViewPanel::kHeaderHeight,
          "under the tabs");
    {
        const auto before = panel.getBounds();
        check(!root.state().mixAnalyzerDock && !panel.analyzerDockShown(), "right analyzer dock is opt-in by default");
        panel.setSize(1000, 650);
        check(panel.setAnalyzerDock(true) && panel.setAnalyzerWidth(360), "show and size the optional analyzer dock");
        panel.frame(root.reader());
        check(panel.analyzerDockShown() && panel.stack.getWidth() >= MixViewPanel::kStackMinimumWidth &&
                  !panel.stack.getBounds().intersects(panel.analyzerDock.getBounds()) &&
                  panel.analyzerDock.getWidth() == 360,
              "dock sits beside actual channel tools and preserves their editing width");
        check(persistence.load("mix").mixAnalyzerDock && persistence.load("mix").mixAnalyzerWidth == 360,
              "dock visibility and width use window view persistence without an audio edit");
        for (const int width : {400, 570, 900}) {
            panel.setSize(width, 650); panel.frame(root.reader());
            bool fits = panel.getLocalBounds().contains(panel.analyzerToggle.getBounds()) &&
                        panel.getLocalBounds().contains(panel.floating.getBounds()) &&
                        !panel.analyzerToggle.getBounds().intersects(panel.floating.getBounds());
            for (std::size_t i = 0; i < panel.tabs.size(); ++i) {
                fits = fits && panel.getLocalBounds().contains(panel.tabs[i]->getBounds()) &&
                    !panel.tabs[i]->getBounds().intersects(panel.analyzerToggle.getBounds()) &&
                    !panel.tabs[i]->getBounds().intersects(panel.floating.getBounds());
                for (std::size_t j = i + 1; j < panel.tabs.size(); ++j)
                    fits = fits && !panel.tabs[i]->getBounds().intersects(panel.tabs[j]->getBounds());
            }
            check(fits && (!panel.analyzerDockShown() || panel.stack.getWidth() >= MixViewPanel::kStackMinimumWidth),
                  "tabs, dock toggle and float wrap without overlap while channel editing keeps its minimum");
        }
        panel.setSize(400, 650); panel.frame(root.reader());
        check(root.state().mixAnalyzerDock && !panel.analyzerDockShown() && panel.stack.getBounds() == panel.body(),
              "narrow layout suppresses requested dock and gives the whole body back to channel tools");
        const auto zoom = root.state().zoom; root.state().zoom = 0;
        check(!panel.setAnalyzerDock(false) && root.state().mixAnalyzerDock,
              "failed preference persistence rolls back dock visibility instead of changing the view");
        root.state().zoom = zoom;
        check(panel.setAnalyzerDock(false), "restore default hidden dock after geometry checks");
        panel.setBounds(before); panel.frame(root.reader());
    }
    {
        const auto operations = value(*store, "SELECT count(*) FROM ops");
        panel.analysis.setMode(SharedSpectrumView::Mode::Spectrogram);
        panel.analysis.paletteBox.setSelectedId(static_cast<int>(palettes::find("viridis")) + 1, juce::sendNotificationSync);
        panel.analysis.reverseButton.setToggleState(true, juce::dontSendNotification); panel.analysis.reverseButton.onClick();
        root.frame();
        check(root.state().analysisPalette == "viridis" && root.state().analysisPaletteReverse &&
                  persistence.load("mix").analysisPalette == "viridis" && persistence.load("mix").analysisPaletteReverse,
              "palette controls save the parent's per-window visual choice");
        check(root.infoSpectrum.palette() == "viridis" && root.infoSpectrum.paletteReversed() &&
                  root.infoSpectrum.mode() == panel.analysis.mode() && root.infoSpectrum.point() == panel.analysis.point(),
              "compact Info follows the parent's analyzer mode, source point and palette");
        check(value(*store, "SELECT count(*) FROM ops") == operations, "palette choices never create audio edits or undo operations");
        panel.analysis.slopeBox.setSelectedId(2, juce::sendNotificationSync); root.frame();
        check(root.state().analysisSpectrumSlope == "tilt45" && persistence.load("mix").analysisSpectrumSlope == "tilt45" &&
                  root.infoSpectrum.spectrumSlope() == SpectrumSlope::Tilt45 && value(*store, "SELECT count(*) FROM ops") == operations,
              "Spectrum slope persists as view state and compact Info follows without audio edits");
        panel.analysis.slopeBox.setSelectedId(1, juce::sendNotificationSync);
        panel.analysis.paletteBox.setSelectedId(1, juce::sendNotificationSync);
        panel.analysis.reverseButton.setToggleState(false, juce::dontSendNotification); panel.analysis.reverseButton.onClick();
        panel.analysis.setMode(SharedSpectrumView::Mode::Spectrum); root.frame();
    }
    panel.tabs[0]->onClick();
    root.frame();
    check(root.state().mixViewTab == 0 && panel.tab() == MixViewPanel::Tab::Overview && !panel.stack.isVisible() &&
              panel.tabs[0]->getToggleState() && !panel.tabs[1]->getToggleState(),
          "a click on OVERVIEW shows its shared-analysis view");
    check(persistence.load("mix", {}).mixViewTab == 0 && persistence.load("mix", {}).mixView,
          "the tab and the view are kept with the window's view (ui_view)");
    check(root.chooseMixViewTab(1), "back to STACK");
    root.frame();
    // A steady frame with the Mix View open builds nothing.
    root.frame();
    allocations = 0;
    audit = true;
    root.frame();
    audit = false;
    check(allocations == 0, "a steady frame with the Mix View open allocates zero");
    // The rows follow the tracks.
    OpRequest pad;
    pad.opType = "track.create";
    pad.payload = {{"id", 13}, {"kind", "midi"}, {"name", "Pad"}, {"index", 3}};
    pad.label = "Create track";
    check(submitter.submit(std::move(pad)).ok, "a new track");
    root.frame();
    check(panel.stack.model().shownCount() == 5, "and STACK has a model row for it at the next frame");
    // The visible surface uses the same model for layout, hit testing and selection.
    {
        auto &surface = panel.stack;
        auto &model = surface.model();
        const auto base = static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes());
        auto rowOf = [&surface](std::int64_t id) -> StackRow * {
            for (const auto &r : surface.rows()) if (r->track() == id) return r.get();
            return nullptr;
        };
        auto *kick = rowOf(10);
        const auto kickIndex = model.shownIndex(10);
        check(kick && kickIndex && kick->getHeight() == model.windowView().baseHeight &&
                  model.lane(*kickIndex).count == 0 && model.moduleKindCount() == 0,
              "actual-chain rows use the model's base height; no reserved slot columns");
        if (kickIndex) {
            const auto before = model.row(*kickIndex);
            model.pointerMove(8, before.top + before.height / 2, false, base);
            surface.frameAt(root.reader(), base + 61);
            surface.frameAt(root.reader(), base + 230);
            kick = rowOf(10);
            const auto enlarged = model.row(*model.shownIndex(10));
            check(kick && model.enlarged().track == 10 && kick->zoomed() &&
                      kick->getHeight() == static_cast<int>(std::round(enlarged.height)) &&
                      enlarged.height > model.windowView().baseHeight,
                  "dwell and easing enlarge the hovered model row, rendered at its model height");
            const auto chip = model.chip(*model.shownIndex(11)).chip;
            const auto hit = model.hitTest(chip.x + chip.w / 2, chip.y + chip.h / 2);
            check(hit.row.track == 11 && hit.part == stack::HitPart::Chip,
                  "the visible chip and model hit test address the same stable track");
            model.pointerExit(base + 240);
            surface.frameAt(root.reader(), base + 800);
            check(model.enlarged().track == 0, "leave deadline restores the list instead of immediate row jumps");
        }
        if (auto *bass = rowOf(11)) {
            const auto chip = model.chip(*model.shownIndex(11)).chip;
            const auto at = juce::Point<float>(static_cast<float>(chip.x + chip.w / 2),
                                              static_cast<float>(chip.y - model.row(*model.shownIndex(11)).top + chip.h / 2));
            bass->mouseDown(pressAt(*bass, at));
            surface.mouseUp(pressAt(*bass, at));
            root.frame();
            check(root.arrangement.geometry.selection.track == 11 && model.selection() == 11,
                  "the actual chip click publishes Bass through the one track selection");
        }
        const auto ref = model.rowRef(10);
        model.pin(ref, {}, base + 810);
        check(model.pinned(10), "the model owns pins");
        model.setSearch("Bass", base + 820);
        surface.frameAt(root.reader(), base + 820);
        check(model.shownCount() == 1 && model.shownTrack(0) == 11,
              "the visible list follows model filtering, not an independent placeholder list");
        model.setSearch("", base + 830);
        surface.frameAt(root.reader(), base + 830);
        check(model.shownCount() == 5 && model.pinned(10), "clearing search restores the scope and preserves pins");
        {
            const auto seenChip = model.chip(*model.shownIndex(10)).chip;
            const auto seenPoint = surface.listArea().getPosition().toFloat() +
                juce::Point<float>(static_cast<float>(seenChip.x + seenChip.w / 2),
                                   static_cast<float>(seenChip.y + seenChip.h / 2));
            OpRequest moved;
            moved.opType = "track.reorder";
            moved.payload = {{"id", 10}, {"index", 99}};
            moved.label = "Move track between frames";
            check(submitter.submit(std::move(moved)).ok, "reorder a track after presentation and before a click");
            surface.mouseDown(pressAt(surface, seenPoint));
            surface.mouseUp(pressAt(surface, seenPoint));
            check(root.arrangement.geometry.selection.track == 10 && surface.focused() == 10,
                  "a click retains the seen track identity instead of hitting unseen reordered geometry");
            root.frame();
            check(model.shownTrack(0) != 10, "the next frame then presents the new track order");
            check(root.command(AppCommands::Undo), "undo the between-frame reorder");
            root.frame();
            check(model.shownTrack(0) == 10, "undo restores that order through the same model adapter");
        }
        check(root.chooseMixViewTab(0), "on another tab");
        root.arrangement.geometry.selection.track = 12;
        check(root.command(AppCommands::ShowChannelEq) && root.state().mixViewTab == 1 && surface.focused() == 12,
              "the existing channel command lands on the real model row");
        check(surface.scope() == "Project", "the rows follow the project scope");
    }
    // The ★ Reference (ADR-0209 §4, d9, d11): its strip in the Mix View, hidden from the project view
    // until [Reference Track] shows it, [REF] and Shift+R, a tap to latch, a hold to audition.
    {
        commands.activate(root);
        juce::ApplicationCommandInfo refInfo(AppCommands::PlayReference);
        commands.getCommandInfo(AppCommands::PlayReference, refInfo);
        juce::ApplicationCommandInfo renameInfo(AppCommands::RenameTrack);
        commands.getCommandInfo(AppCommands::RenameTrack, renameInfo);
        check(refInfo.defaultKeypresses.size() == 1 &&
                  refInfo.defaultKeypresses[0] == juce::KeyPress('r', juce::ModifierKeys::shiftModifier, 0) &&
                  renameInfo.defaultKeypresses[0] == juce::KeyPress('r', juce::ModifierKeys::commandModifier, 0),
              "Play Reference on Shift+R; Ctrl/Cmd+R stays Live's Rename (d9)");
        check(root.referenceTrack() == 0 && panel.referenceText() == "None yet",
              "the stub names no Reference track until the format carries a track's role (N38)");
        check(root.keyPressed(juce::KeyPress('r', juce::ModifierKeys::shiftModifier, 0)) &&
                  root.error().find("No track is the Reference") != std::string::npos,
              "Shift+R with none says so");
        root.referenceKey(true, 0);
        root.referenceKey(false, 50);
        // A provider that names Lead, as the snapshot's will once tracks carry their role.
        struct Named final : MixViewProvider {
            ChannelSlots slots(const engine::TrackNode &) const noexcept override { return {}; }
            bool live() const noexcept override { return false; }
            std::int64_t reference(const std::vector<std::shared_ptr<const engine::TrackNode>> &tracks) const noexcept override {
                for (const auto &t : tracks)
                    if (t->id == 12)
                        return 12;
                return 0;
            }
        } leadIsReference;
        const auto &own = root.mixProvider();
        root.setMixProvider(leadIsReference);
        root.frame();
        auto inStack = [&panel](std::int64_t id) {
            for (const auto &r : panel.stack.rows())
                if (r->track() == id)
                    return true;
            return false;
        };
        check(root.referenceTrack() == 12 && panel.referenceText() == "Lead" && !root.arrangement.geometry.inView(12) &&
                  !inStack(12) && inStack(11),
              "Lead the Reference: named in the strip, hidden from the project view and from STACK's rows");
        panel.eye.onClick();
        root.frame();
        check(root.state().showReference && root.arrangement.geometry.inView(12) && panel.eye.getToggleState() &&
                  persistence.load("mix", {}).showReference,
              "[Reference Track] shows it, lit, kept with the window's view");
        check(root.command(AppCommands::ToggleReferenceTrack) && !root.arrangement.geometry.inView(12),
              "and hides it again");
        // [REF] and Shift+R: a tap latches, a second tap lets go, a hold auditions.
        check(root.referenceKey(true, 1000) && root.reference().on() && root.reference().held(), "down: it plays");
        check(!root.referenceKey(false, 1100) && root.reference().latched() &&
                  root.error().find("cue path") != std::string::npos,
              "a tap under 200 ms latches it; the Status Bar says it waits for the cue path (N23)");
        root.frame();
        check(panel.ref.getToggleState(), "[REF] lit while it is on");
        if (const auto out = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); !headlessOnly && out.isNotEmpty()) {
            // Lead with two clips, the Reference latched, for the picture; undone after.
            int made = 0;
            for (const auto &[clipId, pos, length] : {std::tuple{900, 0, 8}, std::tuple{901, 10, 6}}) {
                OpRequest clip;
                clip.opType = "clip.create";
                clip.payload = {{"id", clipId}, {"track", 12}, {"kind", "audio"}, {"name", "Reference mix"},
                                {"pos", pos * textproj::kPPQ}, {"length", length * textproj::kPPQ}};
                clip.label = "Create clip";
                made += submitter.submit(std::move(clip)).ok ? 1 : 0;
            }
            root.mark(DirtySet::All);
            root.frame();
            juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true,
                              juce::SoftwareImageType{});
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(2.f));
                root.paintEntireComponent(g, true);
            }
            const auto file = juce::File(out).getChildFile("adi-mix-view-reference.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(image, *stream);
            for (int k = 0; k < made; ++k)
                root.command(AppCommands::Undo);
            root.frame();
        }
        check(!root.referenceKey(true, 2000) && root.referenceKey(false, 2050) && !root.reference().on(),
              "a second tap lets it go");
        check(root.referenceKey(true, 3000) && root.referenceKey(false, 3400) && !root.reference().on(),
              "a hold auditions it only while held");
        check(root.keyPressed(juce::KeyPress('r', juce::ModifierKeys::shiftModifier, 0)) && root.reference().latched(),
              "Shift+R from the keyboard, let go at once: a tap, latched");
        check(root.keyPressed(juce::KeyPress('r', juce::ModifierKeys::shiftModifier, 0)) && !root.reference().on(),
              "and again, off");
        root.setMixProvider(own);
        root.frame();
        check(root.referenceTrack() == 0 && root.arrangement.geometry.inView(12), "back to the stub: Lead shows");
    }
    // A second window keeps its own.
    const auto other = std::make_unique<AdiRootComponent>(view, submitter, mailbox, commands,
                                                         persistence, session, "mix-other");
    other->setSize(1200, 720);
    other->frame();
    check(!other->mixViewPanel.isVisible() && other->arrangement.isVisible(), "a second window keeps its own view");
    root.transport.mix.onClick();
    root.frame();
    check(!root.state().mixView && !panel.isVisible() && root.arrangement.isVisible() && root.mixer.isVisible() &&
              root.arrangement.getBounds() == lanes && !root.transport.mix.getToggleState(),
          "the Mix button closes it: the arrangement and its track controls where they were");
    // While it is closed the project changes; opened, it shows the change on its first picture, before
    // any frame (the director's R3: no flash of what it held before).
    {
        const auto before = panel.stack.model().shownCount();
        OpRequest padTrack;
        padTrack.opType = "track.create";
        padTrack.payload = {{"id", 15}, {"kind", "midi"}, {"name", "Pad"}, {"index", 4}};
        padTrack.label = "Create track";
        check(submitter.submit(std::move(padTrack)).ok, "a track while the Mix View is closed");
        root.frame();
        const bool stale = panel.stack.model().shownCount() == before;
        root.transport.mix.onClick();
        check(stale && panel.isVisible() && panel.stack.model().shownCount() == before + 1,
              "opened, it is drawn from the project as it is now, before the next frame");
        check(root.command(AppCommands::Undo), "the track undone");
        root.transport.mix.onClick();
        root.frame();
    }
    std::fputs("[shell] mix fixture docking start\n", stderr);
    // In a window of its own, onto any display (ADR-0209 d11, ADR-0180 d3): the same panel moves
    // into the floating host's window and back.
    {
        FloatingHost host(root, commands, persistence, view, [](std::int64_t) { return true; }, false);
        root.applicationCommand = [&host, &root](int id) {
            if (id == AppCommands::DetachMixView)
                return detachMixView(host, root, !root.state().mixViewDetached);
            if (id == AppCommands::ToggleMixView)
                return toggleMixViewWindow(host, root);
            return false;
        };
        check(root.command(AppCommands::ToggleMixView) && panel.isVisible() && panel.getParentComponent() == &root,
              "F4: docked first");
        panel.floating.onClick();
        root.frame();
        check(root.state().mixView && root.state().mixViewDetached && host.content("Mix View") == &panel &&
                  panel.getParentComponent() != &root && root.arrangement.isVisible() && root.mixer.isVisible(),
              "its window button moves it into a window; the arrangement comes back in the centre");
        // The director's R3 (2 November): its first window stands where it stood docked, at its size.
        const auto dock = root.localAreaToGlobal(root.mixViewDock());
        const auto *window = host.window("Mix View");
        check(window != nullptr && !dock.isEmpty() &&
                  panel.getBounds().translated(window->getX(), window->getY()) == dock && window->getY() < dock.getY(),
              "its first window opens over the place it stood docked, at its size, its title bar above");
        host.frame("Mix View");
        check(panel.floating.getToggleState() && root.transport.mix.getToggleState(),
              "the window button is lit while it is in a window, and so is the Mix field");
        commands.getCommandInfo(AppCommands::DetachMixView, info);
        check(info.shortName == "Mix View in a Window" && (info.flags & juce::ApplicationCommandInfo::isTicked) != 0,
              "the View menu's Mix View in a Window, ticked");
        OpRequest keys;
        keys.opType = "track.create";
        keys.payload = {{"id", 14}, {"kind", "midi"}, {"name", "Keys"}, {"index", 4}};
        keys.label = "Create track";
        check(submitter.submit(std::move(keys)).ok, "another track");
        host.frame("Mix View");
        check(panel.stack.model().shownCount() == 6 && host.drains("Mix View") == 2, "the window's own clock frames it");
        root.frame();
        // Moved and resized, it keeps that place (on a desktop with a display to keep it on).
        const juce::Rectangle<int> moved(180, 140, 760, 430);
        const bool displays = !headlessOnly && !currentMonitors().empty();
        if (displays)
            host.window("Mix View")->setBounds(moved);
        check(root.keyPressed(juce::KeyPress(juce::KeyPress::F4Key)) && host.count() == 0 &&
                  panel.getParentComponent() == &root && !panel.isVisible() && !root.state().mixView &&
                  root.state().mixViewDetached,
              "F4 closes its window; it will open there again");
        check(root.keyPressed(juce::KeyPress(juce::KeyPress::F4Key)) && host.content("Mix View") == &panel &&
                  (!displays || host.window("Mix View")->getBounds() == moved),
              "and F4 opens it there again, where it was left and at that size");
        // Docked by its window button, it is back in the centre, drawn; out again, its window is where
        // it was.
        panel.floating.onClick();
        check(host.count() == 0 && panel.getParentComponent() == &root && panel.isVisible() &&
                  panel.getBounds() == root.mixViewArea() && root.mixViewArea() == root.mixViewDock() &&
                  !root.arrangement.isVisible() && root.state().mixView && !root.state().mixViewDetached,
              "its window button docks it where it stood, open");
        panel.floating.onClick();
        check(host.content("Mix View") == &panel && (!displays || host.window("Mix View")->getBounds() == moved),
              "and puts it back in its window where it was left");
        check(root.keyPressed(juce::KeyPress(juce::KeyPress::F4Key)) && !root.state().mixView &&
                  root.keyPressed(juce::KeyPress(juce::KeyPress::F4Key)) && root.state().mixView &&
                  host.content("Mix View") == &panel,
              "F4 toggles it both ways in its window");
        // Its own close button: the host collects the window, and the Mix View is closed.
        host.close("Mix View");
        syncMixView(host, root);
        check(!root.state().mixView && root.state().mixViewDetached && !panel.isVisible(),
              "a window closed by its own button closes the Mix View");
        check(root.setMixView(true, true) && host.count() == 0, "kept open in a window, as a project reopens");
        syncMixView(host, root, true);
        check(host.content("Mix View") == &panel, "at startup it opens in its window again");
        check(persistence.load("mix", {}).mixViewDetached, "where it opens is kept with the window's view");
        if (const auto out = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); !headlessOnly && out.isNotEmpty()) {
            // The main window with the arrangement back, and the Mix View's window, for the picture.
            host.frame("Mix View");
            root.mark(DirtySet::All);
            root.frame();
            auto save = [&out](juce::Component &c, const char *name) {
                juce::Image image(juce::Image::ARGB, 2 * c.getWidth(), 2 * c.getHeight(), true,
                                  juce::SoftwareImageType{});
                {
                    juce::Graphics g(image);
                    g.addTransform(juce::AffineTransform::scale(2.f));
                    c.paintEntireComponent(g, true);
                }
                const auto file = juce::File(out).getChildFile(name);
                file.deleteFile();
                if (auto stream = file.createOutputStream())
                    juce::PNGImageFormat{}.writeImageToStream(image, *stream);
            };
            save(root, "adi-mix-view-main.png");
            save(panel, "adi-mix-view-window.png");
        }
        panel.floating.onClick();
        root.frame();
        check(host.count() == 0 && panel.getParentComponent() == &root && panel.isVisible() &&
                  !root.arrangement.isVisible() && root.state().mixView && !root.state().mixViewDetached &&
                  panel.getBounds() == lanes.getUnion(controls),
              "the button again docks it back in the centre");
        check(root.command(AppCommands::ToggleMixView) && !panel.isVisible() && root.arrangement.isVisible(), "and F4 closes it");
        root.applicationCommand = nullptr;
    }
    std::fputs("[shell] mix fixture device document start\n", stderr);
    // A real document and its shared feed, using only the headless Knobs fixture.
    {
        std::string error;
        auto document = ProjectDocument::open(dir / "mixview-devices.adi", true, test::knobsLoader(), error);
        check(document != nullptr, "a document for actual-chain Mix View controls");
        if (document) {
            document->store().db().exec(std::string("INSERT INTO plugin_refs(id,format,uid,name) VALUES(30,'clap','") +
                                        test::kKnobsUid + "','UI knobs')");
            std::vector<OpRequest> make(3);
            make[0].opType = "chain.create";
            make[0].payload = {{"id", 30}, {"track", 2}, {"ord", 0}, {"name", "Devices"}};
            for (std::size_t i = 1; i < make.size(); ++i) {
                make[i].opType = "device.insert";
                make[i].payload = {{"id", 29 + static_cast<std::int64_t>(i)}, {"chain", 30},
                                   {"ord", static_cast<std::int64_t>(i - 1)}, {"ref", 30},
                                   {"name", i == 1 ? "First knobs" : "Second knobs"}, {"latency", 0}};
            }
            check(document->ops().submit(std::move(make)).ok && document->synchronise(),
                  "two actual ordered devices on one track");
            const auto actualStorage = std::make_unique<AdiRootComponent>(document->view(), document->ops(),
                document->mailbox(), commands, document->views(), document->sessionState(), "mix-devices");
            auto &actual = *actualStorage;
            std::fputs("[shell] mix fixture device root constructed\n", stderr);
            // This test peer overrides createNewPeer with UiFakePeer: no OS window, input or display.
            UiPeerRoot fake;
            fake.setSize(1440, 900);
            fake.addAndMakeVisible(actual);
            fake.setVisible(true);
            fake.addToDesktop(0);
            check(dynamic_cast<UiFakePeer *>(fake.getPeer()) && fake.getPeer()->getNativeHandle() == nullptr,
                  "Mix View focus dispatch uses a fake peer with no native window");
            actual.afterEdit = [&] { document->synchronise(); };
            actual.devices.publication = [&] { return document->parameterFeed().current(); };
            actual.devices.parameterDemand = [&](const void *owner, const std::vector<std::int64_t> &ids) {
                document->setParameterDemand(owner, ids);
            };
            actual.devices.submit = [&](const std::string &op, Payload p) {
                return document->deviceAction(op, std::move(p));
            };
            int begins = 0, ends = 0;
            actual.devices.guardedGesture = [&](std::int64_t id, const std::string &parameter,
                  engine::ParamEventKind kind, double v, const engine::DeviceRuntimeToken &token, std::uint64_t stamp) {
                if (kind == engine::ParamEventKind::Begin) ++begins;
                if (kind == engine::ParamEventKind::End) ++ends;
                return document->parameterGestureExpected(id, parameter, kind, v, token, stamp);
            };
            actual.devices.targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &token,
                                                std::uint64_t stamp) {
                return document->deviceTargetCurrent(id, token, stamp);
            };
            std::int64_t copiedDevice = 0;
            actual.devices.copy = [&](std::int64_t id, bool) { copiedDevice = id; return true; };
            actual.setSize(1440, 900);
            actual.arrangement.geometry.selection.track = 2;
            document->tick(0);
            actual.frame();
            check(actual.setMixView(true, false), "open actual chains");
            actual.mixViewPanel.stack.channelScope.setSelectedId(2, juce::sendNotificationSync);
            actual.frame();
            auto &surface = actual.mixViewPanel.stack;
            auto findPanel = [&](std::int64_t id) -> DevicePanelView * {
                const auto visit = [&](auto &&self, juce::Component &component) -> DevicePanelView * {
                    if (auto *p = dynamic_cast<DevicePanelView *>(&component); p && p->id == id) return p;
                    for (int i = 0; i < component.getNumChildComponents(); ++i)
                        if (auto *p = self(self, *component.getChildComponent(i))) return p;
                    return nullptr;
                };
                return visit(visit, surface);
            };
            const auto row = surface.model().shownIndex(2);
            std::vector<std::int64_t> order;
            if (row) surface.model().forEachChainItem(*row, [&](const stack::ItemGeo &i) { order.push_back(i.ref.device); });
            auto *first = findPanel(30);
            check(row && order == std::vector<std::int64_t>{30, 31} && first && first->controls.size() == 2,
                  "the visible model and generic panels show actual device order and the shared parameter feed");
            if (first && first->controls.size() == 2) {
                const auto stable = surface.model().chainRef(2, 30);
                const auto itemTime = static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes());
                surface.model().focusItem(stable, itemTime);
                surface.frameAt(actual.reader(), itemTime + 80);
                stack::ItemGeo inspected, sibling;
                if (const auto at = surface.model().shownIndex(2))
                    surface.model().forEachChainItem(*at, [&](const auto &item) {
                        if (item.ref.device == 30) inspected = item;
                        if (item.ref.device == 31) sibling = item;
                    });
                check(findPanel(30) == first && inspected.emphasized && inspected.height > sibling.height &&
                          first->getHeight() == static_cast<int>(std::round(inspected.height)) - 4 &&
                          findPanel(31) && findPanel(31)->getHeight() == static_cast<int>(std::round(sibling.height)) - 4,
                      "item emphasis uses independent model bounds while retaining both actual device panels");
                auto editPoseTime = itemTime + 200;
                for (const auto kind : {DevicePanelView::Interaction::Text, DevicePanelView::Interaction::Menu}) {
                    surface.model().focusItem(surface.model().chainRef(2, 31), editPoseTime);
                    surface.frameAt(actual.reader(), editPoseTime + 60);
                    surface.model().focusItem(stable, editPoseTime + 60);
                    surface.frameAt(actual.reader(), editPoseTime + 100);
                    const auto displayed = first->getBounds();
                    surface.model().advance(editPoseTime + 107);
                    const bool captured = first->beginInteraction(kind);
                    surface.frameAt(actual.reader(), editPoseTime + 120);
                    check(captured && findPanel(30) == first && first->getBounds() == displayed,
                          "Text/Menu capture uses the last presented child rectangle after unseen t+7 movement");
                    first->endInteraction(kind);
                    surface.frameAt(actual.reader(), editPoseTime + 120);
                    check(first->getBounds() == displayed, "Text/Menu release starts from the same displayed child bounds");
                    editPoseTime += 300;
                }
                const auto dragTime = itemTime + 660;
                surface.frameAt(actual.reader(), dragTime);
                const auto heldItemBounds = first->getBounds();
                surface.model().advance(dragTime + 7); // model events may advance between visible frames
                auto *knobs = dynamic_cast<test::Knobs *>(document->session().instanceFor(30));
                const auto beforeUndo = actual.undoTitle(false);
                auto &slider = first->controls[0]->slider;
                slider.onDragStart();
                slider.setValue(.7, juce::sendNotificationSync);
                slider.setValue(.8, juce::sendNotificationSync);
                check(surface.model().capture().has_value() && begins == 1 && ends == 0 && knobs &&
                          std::abs(knobs->getParam("cutoff").normalized - .8) < 1e-9 &&
                          actual.undoTitle(false) == beforeUndo && actual.arrangement.geometry.selection.track == 2,
                      "a tile drag captures its stable target, edits the existing runtime and writes no intermediate undo");
                document->tick(10);
                actual.frame();
                check(findPanel(30) == first && surface.model().chainRef(2, 30) == stable &&
                          surface.model().capture().has_value() && first->getBounds() == heldItemBounds,
                      "new live parameter publications preserve the tile, capture identity and intermediate item rectangle");
                slider.onDragEnd();
                document->tick(20);
                actual.frame();
                check(!surface.model().capture() && begins == 1 && ends == 1 &&
                          actual.undoTitle(false) == "Undo UI knobs: Cutoff" && first->getBounds() == heldItemBounds,
                      "tile release balances one host gesture/undo and resumes from the last presented bounds");
                check(actual.command(AppCommands::Undo), "undo the tile drag");
                document->tick(30); actual.frame();
                check(knobs && std::abs(knobs->getParam("cutoff").normalized - .5) < 1e-9 &&
                          actual.undoTitle(false) == beforeUndo,
                      "one undo restores the untouched parameter through the shared authority");
                std::fputs("[shell] mix fixture compact activator start\n", stderr);
                const auto compactTime = std::max(dragTime + 20,
                    static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes()));
                surface.model().setAutoZoom(false, compactTime);
                surface.model().setBaseHeight(44, compactTime);
                surface.frameAt(actual.reader(), compactTime + 200);
                stack::ItemGeo folded;
                if (const auto i = surface.model().shownIndex(2))
                    surface.model().forEachChainItem(*i, [&](const auto &item) { if (item.ref.device == 30) folded = item; });
                check(folded.pill && !findPanel(30), "compact tiers fold a plug-in without leaving hidden controls active");
                if (folded.pill) {
                    const auto geo = surface.model().row(*surface.model().shownIndex(2));
                    const auto p = surface.listArea().getPosition().toFloat() +
                        juce::Point<float>(static_cast<float>(folded.x + folded.width / 2), static_cast<float>(geo.top + 6));
                    surface.mouseDown(pressAt(surface, p));
                    surface.mouseUp(pressAt(surface, p));
                    document->tick(35); actual.frame();
                    check(value(document->store(), "SELECT enabled FROM devices WHERE id=30") == "0" &&
                              surface.model().chainRef(2, 30) == stable,
                          "the folded tile's activator switches the exact device off without changing identity");
                    check(actual.command(AppCommands::Undo), "undo the folded activator");
                    document->tick(36); actual.frame();
                    check(value(document->store(), "SELECT enabled FROM devices WHERE id=30") == "1",
                          "one undo restores the folded device activator");
                    stack::ItemGeo secondPill;
                    surface.model().forEachChainItem(*surface.model().shownIndex(2), [&](const auto &item) {
                        if (item.ref.device == 31) secondPill = item;
                    });
                    surface.model().scrollLane(surface.model().rowRef(2), 1, compactTime + 201);
                    surface.model().advance(compactTime + 208); // no new component presentation
                    const auto click = surface.listArea().getPosition().toFloat() + juce::Point<float>(
                        static_cast<float>(secondPill.x + secondPill.width - 2), static_cast<float>(secondPill.top + 6));
                    surface.mouseDown(pressAt(surface, click)); surface.mouseUp(pressAt(surface, click));
                    document->tick(37); actual.frame();
                    check(secondPill.pill && value(document->store(), "SELECT enabled FROM devices WHERE id=31") == "0" &&
                              value(document->store(), "SELECT enabled FROM devices WHERE id=30") == "1",
                          "a pill click hits the last painted stable device despite unseen lane motion between frames");
                    check(actual.command(AppCommands::Undo), "undo only the displayed second pill's activator");
                    document->tick(38); actual.frame();
                }
                std::fputs("[shell] mix fixture compact activator returned\n", stderr);
                const auto restoreTime = std::max(compactTime + 210,
                    static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes()));
                surface.model().setBaseHeight(100, restoreTime);
                surface.model().setAutoZoom(true, restoreTime);
                surface.model().focusItem(stable, restoreTime);
                surface.model().scrollLane(surface.model().rowRef(2), -1, restoreTime);
                surface.frameAt(actual.reader(), restoreTime + surface.model().constants().heightAnimMs +
                    surface.model().constants().laneScrollMs + 1);
                first = findPanel(30);
                if (!first) {
                    const auto at = surface.model().shownIndex(2);
                    std::printf("item fixture: row=%d base=%d focused=%lld emphasized=%lld\n", at.has_value(),
                        surface.model().windowView().baseHeight, static_cast<long long>(surface.model().focus().item.device),
                        static_cast<long long>(surface.model().emphasizedItem().device));
                    if (at) {
                        const auto lane = surface.model().lane(*at);
                        std::printf("lane mode=%u first=%u offset=%.3f width=%.3f\n", static_cast<unsigned>(lane.mode), lane.first, lane.offset, lane.width);
                        surface.model().forEachChainItem(*at, [](const auto &item) {
                            std::printf("device=%lld pill=%d x=%.3f width=%.3f height=%.3f\n", static_cast<long long>(item.ref.device), item.pill, item.x, item.width, item.height);
                        });
                    }
                }
                check(first && first->controls.size() == 2,
                      "pill-navigation fixture restores the original lane/focus so all downstream control regressions still execute");
                if (first) {
                    first->grabKeyboardFocus();
                    check(surface.hasDeviceContext() && actual.command(AppCommands::Copy) && copiedDevice == 30,
                          "global Copy dispatch keeps the Mix View's device context");
                    const auto toolbarUndo = actual.undoTitle(false);
                    const auto deviceCount = value(document->store(), "SELECT count(*) FROM devices");
                    const auto trackCount = value(document->store(), "SELECT count(*) FROM tracks");
                    surface.channelScope.grabKeyboardFocus();
                    commands.activate(actual);
                    check(surface.hasToolbarFocus() && !surface.hasDeviceContext(),
                          "the channel selector owns focus without inheriting the last device target");
                    bool blocked = true;
                    for (const int command : {AppCommands::Cut, AppCommands::Copy, AppCommands::Paste,
                                               AppCommands::Duplicate, AppCommands::RenameTrack, AppCommands::DeleteTrack}) {
                        juce::ApplicationCommandInfo commandInfo(command);
                        commands.getCommandInfo(command, commandInfo);
                        blocked = blocked && !actual.command(command) &&
                            (commandInfo.flags & juce::ApplicationCommandInfo::isDisabled) != 0;
                    }
                    check(blocked && surface.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)) &&
                              surface.keyPressed(juce::KeyPress('x', juce::ModifierKeys::commandModifier, 0)) &&
                              !surface.deviceCommand(AppCommands::DeleteTrack) &&
                              value(document->store(), "SELECT count(*) FROM devices") == deviceCount &&
                              value(document->store(), "SELECT count(*) FROM tracks") == trackCount &&
                              actual.undoTitle(false) == toolbarUndo,
                          "toolbar menu and direct edit keys cannot mutate the stale device or current track");
                    surface.search.grabKeyboardFocus();
                    surface.search.setText("abc", false);
                    surface.search.selectAll();
                    surface.search.keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey));
                    check(surface.search.getText().isEmpty() && surface.hasToolbarFocus(),
                          "search keeps native local text editing while global device actions are blocked");
                    surface.search.setText({}, false);
                    surface.search.onTextChange();
                    first->grabKeyboardFocus();
                    check(actual.command(AppCommands::RenameTrack) && first->rename.isVisible() &&
                              surface.model().capture().has_value(),
                          "global Rename opens the exact device editor rather than the track editor");
                    first->rename.setText("Renamed knobs", false);
                    first->rename.onCommit();
                    document->tick(37); actual.frame();
                    check(value(document->store(), "SELECT name FROM devices WHERE id=30") == "Renamed knobs" &&
                              !surface.model().capture() && surface.model().chainRef(2, 30) == stable,
                          "name commit uses the existing undo path and preserves device identity");
                    check(actual.command(AppCommands::Undo), "undo embedded device rename");
                    document->tick(38); actual.frame();
                    first = findPanel(30);
                    if (first && first->beginRename()) {
                        first->rename.setText("Discarded name", false);
                        first->rename.onCancel();
                    }
                    check(value(document->store(), "SELECT name FROM devices WHERE id=30") == "First knobs" &&
                              !surface.model().capture(),
                          "name cancellation writes no replacement value and releases capture");
                }
                first = findPanel(30);
                if (first) {
                    check(first->beginInteraction(DevicePanelView::Interaction::Menu), "a tile menu captures its target");
                    auto oldMenu = first->commands();
                    surface.cancelInteractions();
                    check(!surface.model().capture() && first->beginInteraction(DevicePanelView::Interaction::Menu),
                          "owner cancellation resets local menu state so reopening acquires fresh retention");
                    const auto reopened = surface.model().capture();
                    auto invoke = [](juce::PopupMenu &menu, const char *name) {
                        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                            if (it.getItem().text == name && it.getItem().action) {
                                it.getItem().action();
                                return true;
                            }
                        return false;
                    };
                    check(invoke(oldMenu, "Rename") && !first->rename.isVisible() &&
                              surface.model().capture() == reopened,
                          "a stale menu action cannot mutate or end the reopened menu's capture");
                    auto freshMenu = first->commands();
                    check(invoke(freshMenu, "Rename") && first->rename.isVisible() &&
                              surface.model().capture() && surface.model().capture()->kind == stack::CaptureKind::Text,
                          "a current menu action ends its own retention and opens the exact name editor");
                    surface.cancelInteractions();
                    check(!first->rename.isVisible() && !surface.model().capture() && first->beginRename(),
                          "owner cancellation resets local text state and permits a fresh name capture");
                    first->rename.onCancel();
                    check(!surface.model().capture() && first->beginInteraction(DevicePanelView::Interaction::Menu),
                          "cancelling that name editor permits another clean menu capture");
                    surface.search.setText("nothing-matches", false);
                    surface.search.onTextChange();
                    actual.frame();
                    check(surface.model().capture().has_value() && surface.model().shownIndex(2).has_value(),
                          "filtering cannot discard the row of an open menu");
                    first->endInteraction(DevicePanelView::Interaction::Menu);
                    actual.frame();
                    check(!surface.model().capture() && !surface.model().shownIndex(2),
                          "dismissing the menu releases retention and the filter applies");
                    surface.search.clear();
                    surface.search.onTextChange();
                    actual.frame();
                }
                first = findPanel(30);
                if (first && !first->controls.empty()) {
                    first->controls[0]->slider.onDragStart();
                    first->controls[0]->slider.setValue(.6, juce::sendNotificationSync);
                    check(actual.setMixView(false, false), "close Mix View during a drag");
                    document->tick(40); actual.frame();
                    check(!surface.model().capture() && begins == 2 && ends == 2,
                          "closing commits/releases the held gesture exactly once");
                    actual.setMixView(true, false); actual.frame();
                    first = findPanel(30);
                    check(first && first->targetValid(), "reopen presents a current target");
                    if (first && knobs && !first->controls.empty()) {
                        auto &held = first->controls[0]->slider;
                        held.onDragStart();
                        held.setValue(.65, juce::sendNotificationSync);
                        knobs->refuse = true;
                        held.setValue(.7, juce::sendNotificationSync);
                        check(!surface.model().capture() && begins == 3 && ends == 3 &&
                                  std::abs(knobs->getParam("cutoff").normalized - .65) < 1e-9,
                              "a rejected value releases capture once and keeps the last accepted parameter value");
                        knobs->refuse = false;
                        held.onDragEnd();
                        check(ends == 3, "late mouse-up after rejection cannot end a second gesture");
                        document->tick(45); actual.frame();
                        first = findPanel(30);
                    }
                    if (first && !first->controls.empty()) {
                        auto &held = first->controls[0]->slider;
                        const int beganBefore = begins, endedBefore = ends;
                        held.onDragStart();
                        held.setValue(.66, juce::sendNotificationSync);
                        held.onDragStart(); // reset/new control begins before the old release arrives
                        held.setValue(.68, juce::sendNotificationSync);
                        held.onDragEnd();
                        check(begins == beganBefore + 2 && ends == endedBefore + 2 && !surface.model().capture(),
                              "a second Begin ends the exact previous gesture before installing its new owner");
                        held.onDragEnd();
                        check(ends == endedBefore + 2, "a late release after the superseded drag is ignored");
                        document->tick(46); actual.frame();
                        first = findPanel(30);
                    }
                    if (first) {
                        const auto journalCount = [&] {
                            return document->store().db().execAndGet("SELECT count(*) FROM ops").getInt64();
                        };
                        const auto keysLeaveRows = [&](const char *label) {
                            const auto before = journalCount();
                            const auto countDevices = value(document->store(), "SELECT count(*) FROM devices");
                            const auto countTracks = value(document->store(), "SELECT count(*) FROM tracks");
                            const auto undo = actual.undoTitle(false);
                            const int beforeSets = knobs ? knobs->sets : 0;
                            bool consumed = true;
                            for (const auto key : {juce::KeyPress::deleteKey, juce::KeyPress::backspaceKey}) {
                                const juce::KeyPress press(key);
                                if (!first->controls.empty()) {
                                    const bool knobHandled = first->controls[0]->keyPressed(press);
                                    // Model JUCE's parent bubbling so a regressed knob guard
                                    // would reach a valid panel's destructive action.
                                    if (!knobHandled) (void)first->keyPressed(press);
                                    consumed = knobHandled && consumed;
                                }
                                if (!first->targetValid()) consumed = first->keyPressed(press) && consumed;
                            }
                            check(consumed && journalCount() == before && actual.undoTitle(false) == undo &&
                                      value(document->store(), "SELECT count(*) FROM devices") == countDevices &&
                                      value(document->store(), "SELECT count(*) FROM tracks") == countTracks &&
                                      (!knobs || knobs->sets == beforeSets), label);
                        };
                        if (!first->controls.empty()) {
                            auto &control = *first->controls[0];
                            const auto accept = control.parameterGesture;
                            control.parameterGesture = [](const std::string &, engine::ParamEventKind, double) {
                                return false;
                            };
                            keysLeaveRows("Delete and Backspace on a refused knob reset are consumed without deleting or writing");
                            control.parameterGesture = accept;
                            panel::Record missing;
                            missing.id = "cutoff"; missing.name = "Cutoff"; missing.missing = true;
                            ParameterControl unavailable(actual.devices, 30, missing, .5);
                            const auto before = journalCount();
                            check(unavailable.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)) &&
                                      unavailable.keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey)) &&
                                      journalCount() == before,
                                  "both reset keys consume a missing parameter with no journal write");
                        }
                        // Keep the old tile alive while delete/undo changes its runtime between frames.
                        check(document->deviceAction("device.remove", {{"id", 30}}),
                              "remove the actual device between surface frames");
                        check(!first->targetValid(), "an old tile rejects the removed runtime before the next frame");
                        keysLeaveRows("both reset keys on removed control and panel consume stale targets without escaped deletion");
                        check(actual.command(AppCommands::Undo), "undo device removal");
                        document->tick(50);
                        check(!first->targetValid(), "undo with the same row id cannot revive a stale runtime handle");
                        keysLeaveRows("both reset keys after same-id undo cannot call the restored runtime or delete its row");
                        actual.frame();
                        check(findPanel(30) && surface.model().chainRef(2, 30) != stable,
                              "the next frame creates a fresh tile generation for the restored device");
                    }
                }
            }
            actual.mixViewPanel.stack.cancelInteractions();
            // End can synchronously destroy or re-present the originating surface.
            // This fixture still uses only the document's headless Knobs devices and null peer.
            const auto savedPublication = actual.devices.publication;
            const auto savedGesture = actual.devices.guardedGesture;
            for (int mode = 0; mode < 2; ++mode) {
                std::unique_ptr<StackTab> transient = std::make_unique<StackTab>(actual);
                actual.addAndMakeVisible(*transient);
                transient->setBounds(surface.getBounds());
                transient->channelScope.setSelectedId(2, juce::sendNotificationSync);
                const auto time = static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes()) + 1000;
                std::shared_ptr<const ParameterPublication> supplied = document->parameterFeed().current();
                actual.devices.publication = [&] { return supplied; };
                transient->frameAt(actual.reader(), time);
                const auto find = [&](auto &&self, juce::Component &component) -> DevicePanelView * {
                    if (auto *p = dynamic_cast<DevicePanelView *>(&component); p && p->id == 30) return p;
                    for (int i = 0; i < component.getNumChildComponents(); ++i)
                        if (auto *p = self(self, *component.getChildComponent(i))) return p;
                    return nullptr;
                };
                auto *observedPanel = find(find, *transient);
                bool reenter = false;
                int endCalls = 0;
                juce::Rectangle<int> nestedBounds;
                bool nestedPresented = false;
                actual.devices.guardedGesture = [&](std::int64_t id, const std::string &parameter,
                        engine::ParamEventKind kind, double v, const engine::DeviceRuntimeToken &token, std::uint64_t stamp) {
                    const bool accepted = savedGesture(id, parameter, kind, v, token, stamp);
                    if (kind == engine::ParamEventKind::End && reenter) {
                        reenter = false; ++endCalls;
                        if (mode == 0) transient.reset();
                        else {
                            auto replacement = std::make_shared<ParameterPublication>(*document->parameterFeed().current());
                            std::reverse(replacement->begin(), replacement->end());
                            for (auto &device : *replacement) if (device.id == 30) device.name = "Nested pose";
                            supplied = replacement;
                            if (const auto cap = transient->model().capture()) transient->model().endCapture(*cap, time + 20);
                            transient->model().focusItem(transient->model().chainRef(2, 31), time + 20);
                            transient->frameAt(actual.reader(), time + 220);
                            if (const auto *nested = find(find, *transient)) {
                                nestedBounds = nested->getBounds(); nestedPresented = true;
                            }
                        }
                    }
                    return accepted;
                };
                const auto gesture = observedPanel ? observedPanel->parameterGesture : decltype(observedPanel->parameterGesture){};
                const bool began = gesture && gesture("cutoff", engine::ParamEventKind::Begin, .5);
                auto changed = std::make_shared<ParameterPublication>(*supplied);
                for (auto &device : *changed) if (device.id == 30) device.available = false;
                supplied = changed; reenter = true;
                if (transient) transient->frameAt(actual.reader(), time + 10);
                check(began && endCalls == 1 && (mode == 0 ? !transient : transient && !transient->model().capture()),
                      mode == 0 ? "End during row presentation can synchronously destroy the surface without resuming a dead walker"
                                : "nested End presentation replaces the outer pose without committing a partial snapshot");
                if (mode == 1 && transient) {
                    auto *nested = find(find, *transient);
                    std::vector<std::int64_t> nestedOrder;
                    if (const auto at = transient->model().shownIndex(2))
                        transient->model().forEachChainItem(*at, [&](const auto &item) { nestedOrder.push_back(item.ref.device); });
                    check(nestedPresented && nested && nested->title.getButtonText() == "Nested pose" &&
                              nested->getBounds() == nestedBounds && nestedOrder == std::vector<std::int64_t>{31, 30},
                          "outer presentation cannot overwrite nested reordered child bounds or replacement labels");
                    const bool capture = nested && nested->beginInteraction(DevicePanelView::Interaction::Menu);
                    transient->frameAt(actual.reader(), time + 227);
                    nested = find(find, *transient);
                    check(capture && nested && nested->getBounds() == nestedBounds,
                          "a subsequent capture consumes the complete nested pose rather than an abandoned outer snapshot");
                    if (nested) nested->endInteraction(DevicePanelView::Interaction::Menu);
                }
                transient.reset();
                actual.devices.guardedGesture = savedGesture;
                actual.devices.publication = savedPublication;
            }
            // Publication getters are external callbacks too. Exercise both entry
            // points with the same null peer, independently of gesture End callbacks.
            for (int entry = 0; entry < 2; ++entry) {
                for (int mode = 0; mode < 3; ++mode) {
                    std::unique_ptr<StackTab> transient = std::make_unique<StackTab>(actual);
                    actual.addAndMakeVisible(*transient);
                    transient->setBounds(surface.getBounds());
                    transient->channelScope.setSelectedId(2, juce::sendNotificationSync);
                    const auto time = static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes()) + 2000;
                    auto initial = std::make_shared<ParameterPublication>(*document->parameterFeed().current());
                    for (auto &device : *initial) if (device.id == 30) device.name = "Abandoned getter";
                    std::shared_ptr<const ParameterPublication> supplied = initial;
                    actual.devices.publication = [&] { return supplied; };
                    transient->frameAt(actual.reader(), time);
                    const auto find = [&](auto &&self, juce::Component &component) -> DevicePanelView * {
                        if (auto *p = dynamic_cast<DevicePanelView *>(&component); p && p->id == 30) return p;
                        for (int i = 0; i < component.getNumChildComponents(); ++i)
                            if (auto *p = self(self, *component.getChildComponent(i))) return p;
                        return nullptr;
                    };
                    auto replacement = std::make_shared<ParameterPublication>(*initial);
                    if (mode == 1) std::reverse(replacement->begin(), replacement->end());
                    for (auto &device : *replacement) if (device.id == 30) device.name = "Nested getter pose";
                    bool armed = true, nestedPresented = false;
                    int callbacks = 0;
                    juce::Rectangle<int> nestedBounds;
                    actual.devices.publication = [&]() -> std::shared_ptr<const ParameterPublication> {
                        const auto result = supplied;
                        if (armed) {
                            armed = false; ++callbacks;
                            if (mode == 0) transient.reset();
                            else {
                                supplied = replacement;
                                transient->model().focusItem(transient->model().chainRef(2, 31), time + 20);
                                if (mode == 1) transient->frameAt(actual.reader(), time + 220);
                                else {
                                    transient->model().advance(time + 220);
                                    transient->resized();
                                }
                                if (const auto *nested = find(find, *transient)) {
                                    nestedBounds = nested->getBounds(); nestedPresented = true;
                                }
                            }
                        }
                        return result; // deliberately return the older publication after nested work
                    };
                    if (entry == 0) transient->frameAt(actual.reader(), time + 10);
                    else transient->resized();
                    check(callbacks == 1 && (mode == 0 ? !transient : transient != nullptr),
                          entry == 0 ? "a frame getter may synchronously destroy or reenter its null-peer surface"
                                     : "a post-demand layout getter may synchronously destroy or reenter its null-peer surface");
                    if (mode != 0 && transient) {
                        auto *nested = find(find, *transient);
                        std::vector<std::int64_t> nestedOrder;
                        if (const auto at = transient->model().shownIndex(2))
                            transient->model().forEachChainItem(*at, [&](const auto &item) { nestedOrder.push_back(item.ref.device); });
                        const auto expectedOrder = mode == 1 ? std::vector<std::int64_t>{31, 30}
                                                             : std::vector<std::int64_t>{30, 31};
                        check(nestedPresented && nested && nested->title.getButtonText() == "Nested getter pose" &&
                                  nested->getBounds() == nestedBounds && nestedOrder == expectedOrder,
                              entry == 0 ? "an abandoned frame getter cannot overwrite nested frame or layout presentation"
                                         : "an abandoned layout getter cannot overwrite nested frame or layout presentation");
                        // With no further getter, the next layout must still use the
                        // nested record storage rather than a late outer assignment.
                        actual.devices.publication = {};
                        transient->resized();
                        nested = find(find, *transient);
                        check(nested && nested->title.getButtonText() == "Nested getter pose",
                              "getter reentry retains the nested publication for a subsequent callback-free layout");
                    }
                    transient.reset();
                    actual.devices.publication = savedPublication;
                }
            }
            // Dock/header/narrow layout changes never relocate a held device control.
            const auto mixBounds = actual.mixViewPanel.getBounds();
            for (int kind = 0; kind < 3; ++kind) {
                auto &mix = actual.mixViewPanel;
                mix.setAnalyzerDock(false); mix.setSize(900, 650);
                const auto stamp = static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes()) + 10000 + kind * 1000;
                surface.model().setAutoZoom(false, stamp); surface.model().setBaseHeight(150, stamp);
                surface.model().focusItem(surface.model().chainRef(2, 30), stamp);
                surface.frameAt(actual.reader(), stamp + 400);
                auto *heldPanel = findPanel(30);
                check(heldPanel && !heldPanel->controls.empty(), "dock capture fixture has a live actual device control");
                if (!heldPanel || heldPanel->controls.empty()) continue;
                const auto child = heldPanel->getBounds(), wholeStack = surface.getBounds(), body = mix.body();
                bool captured = false;
                if (kind == 0) { heldPanel->controls[0]->slider.onDragStart(); captured = surface.model().capture().has_value(); }
                else captured = heldPanel->beginInteraction(kind == 1 ? DevicePanelView::Interaction::Text : DevicePanelView::Interaction::Menu);
                mix.setAnalyzerDock(true); mix.setAnalyzerWidth(300); mix.setAnalyzerWidth(420);
                mix.setSize(400, 650); mix.frame(actual.reader());
                check(captured && surface.model().capture() && surface.getBounds() == wholeStack && mix.body() == body &&
                          heldPanel->getBounds() == child && !mix.analyzerDockShown(),
                      "Drag/Text/Menu hold keeps whole Stack, header origin and child geometry while dock changes queue");
                if (kind == 0) heldPanel->controls[0]->slider.onDragEnd();
                else heldPanel->endInteraction(kind == 1 ? DevicePanelView::Interaction::Text : DevicePanelView::Interaction::Menu);
                mix.frame(actual.reader());
                check(!surface.model().capture() && !mix.analyzerDockShown() && surface.getBounds() == mix.body() &&
                          actual.state().mixAnalyzerWidth == 420,
                      "release applies the last requested narrow layout without showing hidden analyzer demand");
                mix.setSize(900, 650); mix.frame(actual.reader());
                check(mix.analyzerDockShown() && mix.analyzerDock.getWidth() == 420 &&
                          surface.getWidth() >= MixViewPanel::kStackMinimumWidth,
                      "widening after release restores the remembered dock beside channel tools");
                mix.setAnalyzerDock(false); mix.frame(actual.reader());
            }
            actual.mixViewPanel.setBounds(mixBounds); actual.mixViewPanel.frame(actual.reader());
            {
                auto &mix = actual.mixViewPanel;
                const auto savedBroker = actual.sharedAnalysis;
                auto tap = std::make_shared<engine::ScopeTap>(48000, SharedAnalysis::kTapSeconds);
                int acquired = 0, released = 0;
                auto broker = std::make_shared<SharedAnalysis>(
                    [&](std::int64_t, engine::ScopePoint) { ++acquired; return tap; },
                    [&](std::int64_t, engine::ScopePoint) { ++released; }, SharedAnalysis::Mode::Manual);
                actual.sharedAnalysis = broker;
                mix.setSize(900, 650); mix.setAnalyzerDock(true); mix.frame(actual.reader());
                const int chromeHeight = mix.getHeight() - mix.body().getHeight();
                mix.setSize(900, chromeHeight + MixViewPanel::kDockMinimumHeight - 1); mix.frame(actual.reader());
                check(mix.body().getHeight() == 419 && actual.state().mixAnalyzerDock && !mix.analyzerDockShown() &&
                          broker->stats().consumers == 0,
                      "requested dock below 420 px body height releases all its demand and preserves the preference");
                mix.setSize(900, chromeHeight + MixViewPanel::kDockMinimumHeight); mix.frame(actual.reader());
                check(mix.body().getHeight() == 420 && mix.analyzerDockShown() && broker->stats().consumers == 3 &&
                          surface.getWidth() >= MixViewPanel::kStackMinimumWidth,
                      "420 px admission restores all three shared views without taking channel editing width");
                std::array<float, 1024> signal{};
                for (std::size_t i = 0; i < signal.size(); ++i)
                    signal[i] = .5f * std::sin(static_cast<float>(i) * .1f);
                tap->write(signal.data(), signal.data(), static_cast<std::int32_t>(signal.size()), 0, true);
                broker->pump(); mix.frame(actual.reader());
                auto &dock = mix.analyzerDock;
                const auto picture = dock.spectrum.snapshot();
                check(picture && dock.meters.snapshot() == picture && dock.phase.snapshot() == picture,
                      "dock rollback fixture has a shared real publication before freezing");
                dock.freezeButton.setToggleState(true, juce::dontSendNotification); dock.freezeButton.onClick();
                const auto before = broker->stats(); const int oldAcquire = acquired, oldRelease = released;
                const auto oldZoom = actual.state().zoom; actual.state().zoom = 0;
                dock.pointBox.setSelectedId(2, juce::sendNotificationSync);
                dock.channelBox.setSelectedId(4, juce::sendNotificationSync);
                dock.sourceBox.setSelectedId(2, juce::sendNotificationSync);
                dock.widthBox.setSelectedId(240, juce::sendNotificationSync);
                mix.frame(actual.reader());
                check(dock.point() == 0 && dock.channel() == 0 && dock.source() == 0 && dock.requestedWidth() == 420 &&
                          dock.spectrum.snapshot() == picture && dock.phase.snapshot() == picture && dock.meters.snapshot() == picture &&
                          broker->stats().consumers == before.consumers && broker->stats().domains == before.domains &&
                          acquired == oldAcquire && released == oldRelease,
                      "failed source/tap/channel/width saves restore controls without discarding frozen pictures or touching demand");
                actual.state().zoom = oldZoom;
                const auto oldOps = value(document->store(), "SELECT count(*) FROM ops");
                const auto oldTracks = value(document->store(), "SELECT count(*) FROM tracks");
                const auto oldDevices = value(document->store(), "SELECT count(*) FROM devices");
                const auto oldUndo = actual.undoTitle(false);
                commands.activate(actual);
                bool blocked = true;
                for (auto *control : std::array<juce::Component *, 8>{&mix.analyzerToggle, &dock.sourceBox, &dock.pointBox,
                        &dock.channelBox, &dock.widthBox, &dock.freezeButton, &dock.resetButton, &dock.expandButton}) {
                    control->grabKeyboardFocus();
                    blocked = blocked && mix.hasAnalyzerControlFocus();
                    for (const int id : {AppCommands::Cut, AppCommands::Copy, AppCommands::Paste, AppCommands::Duplicate,
                                          AppCommands::RenameTrack, AppCommands::DeleteTrack}) {
                        juce::ApplicationCommandInfo commandInfo(id); commands.getCommandInfo(id, commandInfo);
                        blocked = blocked && !actual.command(id) &&
                            (commandInfo.flags & juce::ApplicationCommandInfo::isDisabled) != 0;
                    }
                }
                check(blocked && value(document->store(), "SELECT count(*) FROM ops") == oldOps &&
                          value(document->store(), "SELECT count(*) FROM tracks") == oldTracks &&
                          value(document->store(), "SELECT count(*) FROM devices") == oldDevices && actual.undoTitle(false) == oldUndo,
                      "analyzer controls protect menu and direct track/device actions without changing audio edits or Undo");
                actual.chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Analysis));
                mix.analysis.setMode(SharedSpectrumView::Mode::Spectrogram);
                mix.analysis.paletteBox.setSelectedId(static_cast<int>(palettes::find("viridis")) + 1, juce::sendNotificationSync);
                mix.analysis.reverseButton.setToggleState(true, juce::dontSendNotification); mix.analysis.reverseButton.onClick();
                mix.frame(actual.reader());
                tap->write(signal.data(), signal.data(), static_cast<std::int32_t>(signal.size()), 1024, true);
                broker->pump(); mix.frame(actual.reader());
                mix.analysis.freezeButton.setToggleState(true, juce::dontSendNotification); mix.analysis.freezeButton.onClick();
                const auto spectralPicture = mix.analysis.view.snapshot(); const auto spectralBefore = broker->stats();
                const auto image = [&] {
                    juce::Image picture(juce::Image::RGB, mix.analysis.view.getWidth(), mix.analysis.view.getHeight(), true);
                    juce::Graphics graphics(picture); mix.analysis.view.paint(graphics); return picture;
                };
                const auto beforeImage = image();
                check(spectralPicture && mix.analysis.view.frozen(), "palette rollback fixture retains a frozen spectrogram");
                actual.state().zoom = 0;
                mix.analysis.paletteBox.setSelectedId(static_cast<int>(palettes::find("plasma")) + 1, juce::sendNotificationSync);
                mix.analysis.reverseButton.setToggleState(false, juce::dontSendNotification); mix.analysis.reverseButton.onClick();
                const auto afterImage = image(); bool pixelsEqual = beforeImage.getBounds() == afterImage.getBounds();
                for (int y = 0; y < beforeImage.getHeight(); ++y) for (int x = 0; x < beforeImage.getWidth(); ++x)
                    pixelsEqual = pixelsEqual && beforeImage.getPixelAt(x, y) == afterImage.getPixelAt(x, y);
                check(mix.analysis.view.palette() == "viridis" && mix.analysis.view.paletteReversed() &&
                          mix.analysis.requestedPalette() == "viridis" && mix.analysis.reverseButton.getToggleState() &&
                          mix.analysis.view.snapshot() == spectralPicture && pixelsEqual &&
                          broker->stats().transforms == spectralBefore.transforms && broker->stats().consumers == spectralBefore.consumers,
                      "failed palette/reverse saves preserve frozen measurement, pixels and analysis demand");
                actual.state().zoom = oldZoom;
                mix.analysis.view.freeze(false); mix.analysis.setMode(SharedSpectrumView::Mode::Spectrum);
                mix.analysis.slopeBox.setSelectedId(2, juce::sendNotificationSync);
                mix.frame(actual.reader()); tap->write(signal.data(), signal.data(), static_cast<std::int32_t>(signal.size()), 2048, true);
                broker->pump(); mix.frame(actual.reader()); mix.analysis.view.freeze(true);
                const auto slopePicture = mix.analysis.view.snapshot();
                const auto slopeImage = mix.analysis.view.createComponentSnapshot(mix.analysis.view.getLocalBounds(), false);
                check(slopePicture && mix.analysis.view.spectrumSlope() == SpectrumSlope::Tilt45,
                      "slope rollback fixture contains a retained real Frozen Spectrum");
                const auto slopeBefore = broker->stats(); actual.state().zoom = 0;
                mix.analysis.slopeBox.setSelectedId(1, juce::sendNotificationSync);
                const auto slopeAfter = mix.analysis.view.createComponentSnapshot(mix.analysis.view.getLocalBounds(), false);
                bool slopePixelsEqual = slopeImage.getBounds() == slopeAfter.getBounds();
                for (int y = 0; y < slopeImage.getHeight() && slopePixelsEqual; ++y)
                    for (int x = 0; x < slopeImage.getWidth(); ++x)
                        slopePixelsEqual = slopePixelsEqual && slopeImage.getPixelAt(x, y) == slopeAfter.getPixelAt(x, y);
                check(mix.analysis.requestedSpectrumSlope() == SpectrumSlope::Tilt45 &&
                          actual.state().analysisSpectrumSlope == "tilt45" && mix.analysis.view.snapshot() == slopePicture &&
                          slopePixelsEqual && broker->stats().consumers == slopeBefore.consumers &&
                          broker->stats().transforms == slopeBefore.transforms,
                      "failed slope save restores only controls and retains Frozen projection, measurement and demand");
                actual.state().zoom = oldZoom;
                {
                    // A separate software component tree reproduces floating
                    // key forwarding without creating a native window.
                    AnalysisPane detached;
                    fake.addAndMakeVisible(detached); detached.setBounds(0, 0, 500, 420);
                    detached.setMode(SharedSpectrumView::Mode::Spectrogram);
                    detached.setPalette(mix.analysis.view.palette(), mix.analysis.view.paletteReversed());
                    detached.setSpectrumSlope(mix.analysis.view.spectrumSlope());
                    detached.slopeBox.setSelectedId(1, juce::sendNotificationSync);
                    check(detached.view.spectrumSlope() == SpectrumSlope::Flat && mix.analysis.view.spectrumSlope() == SpectrumSlope::Tilt45 &&
                              actual.state().analysisSpectrumSlope == "tilt45",
                          "floating slope starts from the parent then remains a lifetime-local view choice");
                    detached.paletteBox.setSelectedId(static_cast<int>(palettes::find("magma")) + 1, juce::sendNotificationSync);
                    check(detached.view.palette() == "magma" && mix.analysis.view.palette() == "viridis" &&
                              actual.state().analysisPalette == "viridis",
                          "detached palette choice remains local after copying the parent presentation");
                    bool protectedContext = true;
                    commands.activate(actual);
                    for (auto *control : std::array<juce::Component *, 10>{&detached.modeBox, &detached.sourceBox,
                            &detached.pointBox, &detached.channelBox, &detached.paletteBox, &detached.slopeBox, &detached.reverseButton,
                            &detached.freezeButton, &detached.resetButton, &detached.expandButton}) {
                        detached.setMode(control == &detached.slopeBox ? SharedSpectrumView::Mode::Spectrum : SharedSpectrumView::Mode::Spectrogram);
                        control->grabKeyboardFocus();
                        protectedContext = protectedContext && AnalysisPane::anyControlsFocused();
                        for (const int id : {AppCommands::Cut, AppCommands::Copy, AppCommands::Paste,
                                              AppCommands::Duplicate, AppCommands::RenameTrack, AppCommands::DeleteTrack}) {
                            juce::ApplicationCommandInfo analysisCommandInfo(id); commands.getCommandInfo(id, analysisCommandInfo);
                            protectedContext = protectedContext && !actual.command(id) &&
                                (analysisCommandInfo.flags & juce::ApplicationCommandInfo::isDisabled) != 0;
                        }
                    }
                    check(protectedContext && value(document->store(), "SELECT count(*) FROM ops") == oldOps &&
                              value(document->store(), "SELECT count(*) FROM tracks") == oldTracks &&
                              value(document->store(), "SELECT count(*) FROM devices") == oldDevices &&
                              actual.undoTitle(false) == oldUndo,
                          "detached analyzer control keys/menu cannot edit the stale main track or device context");
                }
                actual.chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Stack)); mix.frame(actual.reader());
                mix.analysis.freezeButton.setToggleState(false, juce::dontSendNotification); mix.analysis.freezeButton.onClick();
                dock.freezeButton.setToggleState(false, juce::dontSendNotification); dock.freezeButton.onClick();
                actual.sharedAnalysis = savedBroker; mix.setAnalyzerDock(false); mix.frame(actual.reader());
                mix.setBounds(mixBounds); mix.frame(actual.reader()); surface.grabKeyboardFocus();
            }
            actual.mixViewPanel.stack.suspendParameters(true);
            actual.devices.suspendParameters(true);
            actual.afterEdit = nullptr;
            fake.removeChildComponent(&actual);
            fake.removeFromDesktop();
            std::fputs("[shell] mix fixture demand peers start\n", stderr);
            // Exercise the real main/floating visibility hooks with fake peers.
            // Neither DocumentWindow constructs a native peer in this fixture.
            std::map<const void *, std::vector<std::int64_t>> demands;
            {
                auto owned = std::make_unique<AdiRootComponent>(document->view(), document->ops(), document->mailbox(),
                    commands, document->views(), document->sessionState(), "parameter-demand");
                auto &observed = *owned;
                observed.devices.publication = [&] { return document->parameterFeed().current(); };
                observed.devices.parameterDemand = [&](const void *owner, const std::vector<std::int64_t> &ids) {
                    if (ids.empty()) demands.erase(owner); else demands[owner] = ids;
                    document->setParameterDemand(owner, ids);
                };
                observed.setSize(1440, 900);
                observed.arrangement.geometry.selection.track = 2;
                AdiWindow window(std::move(owned), commands, false);
                UiPeerRoot mainPeer;
                mainPeer.setSize(1600, 1000);
                mainPeer.addAndMakeVisible(window);
                mainPeer.setVisible(true);mainPeer.addToDesktop(0);
                check(dynamic_cast<UiFakePeer *>(mainPeer.getPeer()) && window.getPeer() &&
                          window.getPeer()->getNativeHandle() == nullptr,
                      "parameter visibility fixture has only a software peer");
                observed.frame();
                check(observed.setMixView(true, false), "parameter demand opens the docked Stack");
                auto &stackSurface = observed.mixViewPanel.stack;
                const auto time = static_cast<stack::TimeMs>(juce::Time::getMillisecondCounterHiRes());
                stackSurface.model().setAutoZoom(false, time);stackSurface.model().setBaseHeight(140, time);
                observed.frame();stackSurface.frameAt(observed.reader(), time + 200);
                const auto has = [&](const void *owner) {
                    const auto at = demands.find(owner);
                    return at != demands.end() && !at->second.empty();
                };
                check(stackSurface.model().windowView().channelScope == stack::ChannelScope::WithTools &&
                          !stackSurface.model().shownIndex(2) && has(&observed.devices) && !has(&stackSurface),
                      "With tools omits generic plug-in rows and requests no Stack parameters");
                stackSurface.channelScope.setSelectedId(2, juce::sendNotificationSync);
                observed.frame();stackSurface.frameAt(observed.reader(), time + 200);
                check(stackSurface.model().windowView().channelScope == stack::ChannelScope::AllChannels &&
                          stackSurface.model().shownIndex(2) && has(&observed.devices) && has(&stackSurface),
                      "device strip and realised Stack controls own independent parameter demand");
                window.minimisationStateChanged(true);observed.frame();
                check(demands.empty(), "minimizing the main window releases both attached parameter consumers");
                window.minimisationStateChanged(false);observed.frame();stackSurface.frameAt(observed.reader(), time + 220);
                check(has(&observed.devices) && has(&stackSurface), "restoring the main window refreshes both visible consumers");
                FloatingHost host(observed, commands, document->views(), document->view(),
                    [](std::int64_t) { return true; }, false);
                check(detachMixView(host, observed, true), "parameter demand detaches Stack to its own software window");
                UiPeerRoot floatingPeer;
                floatingPeer.setSize(1600, 1000);
                auto *floating = host.window("Mix View");
                if (floating) {
                    floatingPeer.addAndMakeVisible(*floating);floatingPeer.setVisible(true);floatingPeer.addToDesktop(0);
                    host.frame("Mix View");stackSurface.frameAt(observed.reader(), time + 240);
                    check(has(&stackSurface), "detached visible Stack requests its own parameters");
                    window.minimisationStateChanged(true);host.frame("Mix View");
                    check(!has(&observed.devices) && has(&stackSurface),
                          "main minimization preserves the independent floating Stack consumer");
                    floating->minimisationStateChanged(true);host.frame("Mix View");
                    check(demands.empty(), "floating minimization releases its last parameter demand");
                    floating->minimisationStateChanged(false);host.frame("Mix View");
                    check(!has(&observed.devices) && has(&stackSurface),
                          "floating restore works while the main window remains minimized");
                    host.show("Mix View", false);host.frame("Mix View");
                    check(demands.empty(), "explicit floating hide releases demand without a native visibility callback");
                    host.show("Mix View", true);host.frame("Mix View");
                    check(has(&stackSurface), "explicit floating show restores demand");
                    check(detachMixView(host, observed, false), "dock Stack back into the minimized main window");
                    observed.frame();
                    check(demands.empty(), "docking into minimized main does not revive hidden parameter work");
                    window.minimisationStateChanged(false);observed.frame();
                    check(has(&observed.devices) && has(&stackSurface), "main restore after redocking refreshes current controls");
                    floatingPeer.removeFromDesktop();
                } else check(false, "detached parameter window exists");
                check(observed.setMixView(false, false), "close the parameter-demand Stack");
                check(!has(&stackSurface) && has(&observed.devices), "closing Stack keeps the visible device strip's demand");
                mainPeer.removeChildComponent(&window);mainPeer.removeFromDesktop();
            }
            check(demands.empty(), "surface destruction releases all parameter demand owners");
            document->release();
        }
    }
    {
        // Publication admission may reject Values, but an End still has to reach the
        // document's exact-old-owner cancellation path after a runtime/stamp change.
        DeviceChainStrip gateway(root);
        auto publication = std::make_shared<ParameterPublication>();
        DevicePanelData expected;
        expected.id = 700;
        expected.instance = 71;
        expected.available = true;
        publication->push_back(expected);
        gateway.publication = [&] { return publication; };
        int started = 0, ended = 0, values = 0;
        gateway.guardedGesture = [&](std::int64_t id, const std::string &parameter, engine::ParamEventKind kind,
                                      double, const engine::DeviceRuntimeToken &, std::uint64_t stamp) {
            check(id == 700 && parameter == "cutoff" && stamp == 71,
                  "the gesture gateway forwards the original target, parameter and stamp");
            if (kind == engine::ParamEventKind::Begin) { ++started; return true; }
            if (kind == engine::ParamEventKind::Value) ++values;
            if (kind == engine::ParamEventKind::End) ++ended;
            return false; // a real document retires the old owner without setting the new runtime
        };
        check(gateway.editExpected(expected, "cutoff", engine::ParamEventKind::Begin, .5) && started == 1,
              "an admitted Begin reaches the guarded authority");
        publication = std::make_shared<ParameterPublication>(*publication);
        publication->front().instance = 72;
        check(!gateway.editExpected(expected, "cutoff", engine::ParamEventKind::Value, .9) && values == 0,
              "a changed publication refuses stale Values before any SDK gesture");
        check(!gateway.editExpected(expected, "cutoff", engine::ParamEventKind::End, .5) && ended == 1,
              "one stale End reaches exact-owner cleanup despite the changed publication");
        expected.name = "Retained knobs";
        adi::panel::Record record;
        record.id = "cutoff";
        record.name = "Cutoff";
        record.playing = .5;
        expected.records.push_back(record);
        expected.declared.push_back({"cutoff", "Cutoff", true});
        expected.resolved.entries.push_back({"cutoff", false});
        DevicePanelView retained(gateway, expected);
        bool valid = true;
        int panelBegins = 0, panelEnds = 0;
        retained.targetValid = [&] { return valid; };
        retained.parameterGesture = [&](const std::string &, engine::ParamEventKind kind, double) {
            if (kind == engine::ParamEventKind::Begin) ++panelBegins;
            if (kind == engine::ParamEventKind::End) ++panelEnds;
            return true;
        };
        check(retained.controls.size() == 1, "a retained panel for the first gesture gateway");
        if (!retained.controls.empty()) {
            auto &held = retained.controls.front()->slider;
            held.onDragStart();
            valid = false;
            held.onDragEnd();
            check(panelBegins == 1 && panelEnds == 1,
                  "panel End still reaches its original owner after target validation turns stale");
        }
    }
    std::fputs("[shell] mix fixture seek checks start\n", stderr);
    adi::uitest::mixSeekChecks(commands, dir, check);
    std::fputs("[shell] mix fixture seek checks returned\n", stderr);
    const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
    if (!headlessOnly && folder.isNotEmpty()) {
        root.command(AppCommands::ToggleMixView);
        root.mark(DirtySet::All);
        root.frame();
        // STACK with Bass's row open under the mouse, for the picture.
        root.mixViewPanel.stack.hover(11);
        juce::Image image(juce::Image::ARGB, 2 * root.getWidth(), 2 * root.getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(2.f));
            root.paintEntireComponent(g, true);
        }
        const auto file = juce::File(folder).getChildFile("adi-mix-view.png");
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat{}.writeImageToStream(image, *stream);
        root.command(AppCommands::ToggleMixView);
    }
}
} // namespace
int main(int argc, char **argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    const bool write = argc == 2 && std::string(argv[1]) == "--write-references";
    test::TempDirectory temp("ui_shell", "root");
    StoreError err{};
    auto store = Store::createEmpty(temp.path() / "p.adi", err);
    if (!store)
        return 2;
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Shell test')");
    engine::ProjectView view;
    view.refresh(*store);
    OpSubmitter submitter(*store, view);
    TransportMailbox mailbox;
    engine::Transport driver;
    settings::AppSettings settings(appdata::App::Daw, temp.path() / "settings.json");
    AppCommands commands(settings);
    const auto trace = [](const char *name, const auto &fixture) {
        std::fprintf(stderr, "[shell] begin %s\n", name);
        fixture();
        std::fprintf(stderr, "[shell] end %s\n", name);
    };
    if (argc == 2 && std::string(argv[1]) == "--arrangement-components-only") {
        // These fixtures render components in software and do not create a native peer.
        trace("liveParity", [&] { liveParity(commands, temp.path(), false); });
        trace("groupTracks", [&] { groupTracks(commands, temp.path()); });
        trace("sendKnobs", [&] { sendKnobs(commands, temp.path()); });
        std::printf("%d checks, %d failures (Arrangement components)\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--mixview-only") {
        // Deliberately before every UiPeerRoot/addToDesktop/reference() branch below.
        // Fake plug-ins and UiFakePeer only; no native window, desktop input/capture or audio device.
        mixViewShell(commands, temp.path(), true);
        std::printf("%d checks, %d failures (Mix View fake-peer components)\n", checks, failures);
        return failures ? 1 : 0;
    }
    ViewStateStore persistence(*store);
    SessionState session(*store);
    AdiRootComponent a(view, submitter, mailbox, commands, persistence, session, "main"),
        b(view, submitter, mailbox, commands, persistence, session, "floating");
    a.setSize(960, 540);
    b.setSize(960, 540);
    a.frame();
    b.frame();
    check(a.reader().get() == b.reader().get() && a.generation() == b.generation(),
          "windows initially present same generation");
    check(a.bpm() == 120, "TransportBar reads initial BPM from ProjectView");
    check(a.transport.play.getTitle() == "Play" && a.transport.stop.getTitle() == "Stop" &&
              a.transport.play.glyph == BarItem::Glyph::Play,
          "the transport's buttons are Live's glyphs, named for the Info View");
    reference(a, "root-default", write);
    // The theme and density tables (DF-W24). Dark is the default; Mid grey keeps the colours of the
    // window as built before the sheets (step 1 matched that golden pixel for pixel; since the
    // shared controls of step 3, root-default-midgrey.png is the Mid grey window with them). Each other theme and Open
    // draw a window too.
    {
        check(look::themeId() == ThemeId::Dark && look::densityId() == DensityId::Compact &&
                  look::panel == juce::Colour(themeFor(ThemeId::Dark).panel) && look::controlBarHeight == 35,
              "the window starts in Dark and Compact");
        check(look::apply(ThemeId::MidGrey, DensityId::Compact) && look::panel == juce::Colour(0xff8f8f8f) &&
                  look::liveLook().findColour(juce::ComboBox::backgroundColourId) == look::chooser,
              "a theme set reaches the look:: colours and the JUCE controls' own");
        AdiRootComponent grey(view, submitter, mailbox, commands, persistence, session, "grey");
        grey.setSize(960, 540);
        grey.frame();
        reference(grey, "root-default-midgrey", write);
        for (const auto id : kThemes)
            for (const auto density : kDensities) {
                look::apply(id, density);
                AdiRootComponent other(view, submitter, mailbox, commands, persistence, session, "other");
                other.setSize(960, 540);
                other.frame();
                juce::Image image(juce::Image::ARGB, 960, 540, true, juce::SoftwareImageType{});
                {
                    juce::Graphics g(image);
                    other.paintEntireComponent(g, true);
                }
                check(image.getPixelAt(2, 300) == look::frame || image.getPixelAt(2, 300) == look::panel ||
                          image.getPixelAt(2, 300) == look::band,
                      "another theme and density draw the window in their own colours");
                check(other.transport.getHeight() == look::controlBarHeight,
                      "the Control Bar takes the density's height");
                // The PR's picture (ADI_UI_PARITY): each theme and density, at twice the size.
                if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
                    juce::Image twice(juce::Image::ARGB, 1920, 1080, true, juce::SoftwareImageType{});
                    {
                        juce::Graphics g(twice);
                        g.addTransform(juce::AffineTransform::scale(2.f));
                        other.paintEntireComponent(g, true);
                    }
                    const auto file = juce::File(folder).getChildFile(
                        juce::String("adi-theme-") + juce::String(std::string(themeName(id))) + "-" +
                        juce::String(std::string(densityName(density))) + ".png");
                    file.deleteFile();
                    juce::FileOutputStream out(file);
                    juce::PNGImageFormat().writeImageToStream(twice, out);
                }
            }
        check(!look::apply(ThemeId::MixView, DensityId::Open), "setting the same pair again changes nothing");
        // The shared controls (DF-W24 step 3; language.png parts B, C, D): one painter each.
        look::apply(ThemeId::Dark, DensityId::Compact);
        {
            using controls::ButtonState;
            using controls::Lit;
            check(controls::stateOf(false, false, false, true) == ButtonState::Rest &&
                      controls::stateOf(false, true, false, true) == ButtonState::Over &&
                      controls::stateOf(false, true, true, true) == ButtonState::Pressed &&
                      controls::stateOf(true, true, true, true) == ButtonState::On &&
                      controls::stateOf(true, false, false, true, Lit::Record) == ButtonState::OnRecord &&
                      controls::stateOf(true, false, false, true, Lit::Value) == ButtonState::OnValue &&
                      controls::stateOf(false, true, true, false) == ButtonState::Disabled &&
                      controls::stateOf(true, true, true, false) == ButtonState::DisabledOn,
                  "a button's state: rest, pointer over, pressed, on (in the accent, the record's red or the value's "
                  "blue), disabled, and disabled while on");
            // A disabled switch that is on still reads as on (a Settings switch of another scope).
            {
                juce::Image off(juce::Image::ARGB, 40, 20, true, juce::SoftwareImageType{}), on = off.createCopy();
                {
                    juce::Graphics g(off);
                    controls::button(g, {0, 0, 40, 20}, ButtonState::Disabled);
                }
                {
                    juce::Graphics g(on);
                    controls::button(g, {0, 0, 40, 20}, ButtonState::DisabledOn);
                }
                check(off.getPixelAt(20, 3) != on.getPixelAt(20, 3), "a disabled switch on and off look apart");
            }
            // A Solo made in one theme is drawn in the next theme's blue: its kind is its own, not its colour.
            {
                juce::TextButton solo("S");
                solo.setLookAndFeel(&look::liveLook());
                solo.setColour(juce::TextButton::buttonOnColourId, look::value);
                solo.getProperties().set("adi.lit", "value");
                solo.setToggleState(true, juce::dontSendNotification);
                solo.setSize(30, 20);
                look::apply(ThemeId::Light, DensityId::Compact);
                juce::Image shot(juce::Image::ARGB, 30, 20, true, juce::SoftwareImageType{});
                {
                    juce::Graphics g(shot);
                    solo.paintEntireComponent(g, true);
                }
                check(shot.getPixelAt(15, 3) == look::value, "a lit Solo takes the theme's value blue after a theme change");
                look::apply(ThemeId::Dark, DensityId::Compact);
                solo.setLookAndFeel(nullptr);
            }
            check(controls::labelColour(ButtonState::On) == look::ink && controls::labelColour(ButtonState::OnRecord) == look::ink &&
                      controls::labelColour(ButtonState::Rest) == look::text &&
                      controls::labelColour(ButtonState::Disabled) == look::textFaint,
                  "a lit button's label is the ink, a resting one's the text, a disabled one's faint");
            const juce::Rectangle<float> monitor(0, 0, 70, 13);
            const juce::StringArray io{"In", "Auto", "Off"};
            const auto in = controls::oneOfCell(monitor, io, 0), autoCell = controls::oneOfCell(monitor, io, 1),
                       off = controls::oneOfCell(monitor, io, 2);
            check(autoCell.getWidth() > in.getWidth() && in.getRight() == autoCell.getX() &&
                      std::abs(off.getRight() - monitor.getRight()) < 0.01f,
                  "one of three: each choice as wide as its label wants, the three filling the box");
            // A button drawn at rest and on: the sheet's colours, at its middle.
            juce::Image cell(juce::Image::ARGB, 40, 20, true, juce::SoftwareImageType{});
            {
                juce::Graphics g(cell);
                controls::button(g, {0, 0, 40, 20}, ButtonState::Rest);
            }
            const bool rest = cell.getPixelAt(20, 3) == look::buttonRest;
            {
                juce::Graphics g(cell);
                controls::button(g, {0, 0, 40, 20}, ButtonState::OnRecord);
            }
            check(rest && cell.getPixelAt(20, 3) == look::record, "a button at rest and on for record, in the sheet's colours");
            // The PR's picture: every control in every state, as the sheet's parts B, C and D lay them out.
            if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); folder.isNotEmpty()) {
                juce::Image sheet(juce::Image::ARGB, 1920, 300, true, juce::SoftwareImageType{});
                {
                    juce::Graphics g(sheet);
                    g.fillAll(look::frame);
                    g.setFont(look::font(look::fontBody));
                    const auto caption = [&](float cx, float y, const char *t) {
                        g.setColour(look::textDim);
                        g.drawText(t, juce::Rectangle<float>(cx - 50, y, 100, 14), juce::Justification::centred);
                    };
                    float x = 24;
                    for (const auto &[state, name] : std::vector<std::pair<ButtonState, const char *>>{
                             {ButtonState::Rest, "Rest"}, {ButtonState::Over, "Pointer over"}, {ButtonState::Pressed, "Pressed"},
                             {ButtonState::On, "On"}, {ButtonState::OnRecord, "On, record"}, {ButtonState::OnValue, "On, value"},
                             {ButtonState::Disabled, "Disabled"}}) {
                        controls::button(g, {x, 34, 74, 28}, state, "Solo");
                        caption(x + 37, 70, name);
                        x += 86;
                    }
                    controls::group(g, {24, 110, 150, 30});
                    controls::separator(g, 74, 110, 30);
                    controls::separator(g, 124, 110, 30);
                    controls::button(g, {540, 113, 34, 24}, ButtonState::On, "1");
                    controls::button(g, {578, 113, 26, 24}, ButtonState::Rest, "S");
                    controls::button(g, {608, 113, 26, 24}, ButtonState::Rest);
                    caption(587, 146, "Track: on, Solo, Arm");
                    float kx = 686;
                    const auto knob = [&](float r, float v, controls::KnobLook k, const char *name) {
                        controls::ringKnob(g, {kx, 62}, r, v, k);
                        caption(kx, 96, name);
                        kx += r * 2 + 40;
                    };
                    knob(look::knobSmall, 0.55f, {}, "Small");
                    knob(look::knobMedium, 0.55f, {}, "Medium");
                    knob(look::knobLarge, 0.55f, {}, "Large");
                    controls::KnobLook over;
                    over.over = true;
                    knob(17, 0.3f, over, "pointer over");
                    controls::KnobLook centre;
                    centre.fromCentre = true;
                    knob(17, 0.72f, centre, "from centre");
                    controls::KnobLook automated;
                    automated.automated = true;
                    knob(17, 0.6f, automated, "automated");
                    controls::KnobLook modulated;
                    modulated.modulation = std::make_pair(0.3f, 0.7f);
                    knob(17, 0.45f, modulated, "modulated");
                    controls::KnobLook disabled;
                    disabled.disabled = true;
                    knob(17, 0.5f, disabled, "disabled");
                    controls::KnobLook second;
                    second.colour = look::accent;
                    knob(17, 0.64f, second, "second colour");
                    const float dx = 1380;
                    controls::valueBox(g, {dx, 34, 70, 22}, "-6.0 dB", 0.57f);
                    caption(dx + 35, 62, "Value box");
                    controls::chooser(g, {dx + 86, 34, 104, 22}, "Ext. In");
                    caption(dx + 138, 62, "Chooser");
                    controls::oneOf(g, {dx + 206, 34, 96, 22}, {"In", "Auto", "Off"}, 1);
                    caption(dx + 254, 62, "One of three");
                    controls::toggleSwitch(g, {dx + 318, 36, 32, 18}, true);
                    controls::toggleSwitch(g, {dx + 360, 36, 32, 18}, false);
                    caption(dx + 355, 62, "Switch");
                    controls::tabs(g, {dx, 100, 392, 30}, {"Notes", "Envelopes", "MPE", "Expression"}, 0);
                    caption(dx + 196, 138, "Tabs");
                }
                const auto file = juce::File(folder).getChildFile("adi-shared-controls.png");
                file.deleteFile();
                juce::FileOutputStream out(file);
                juce::PNGImageFormat().writeImageToStream(sheet, out);
            }
        }
        // Back to a density the settings must move, so the check below tests it.
        look::apply(ThemeId::MixView, DensityId::Open);
        // The settings choose them: the registry's word for the theme, the density's key once it is there.
        std::string why;
        settings.set("lookfeel.theme", std::string("light"), {}, why);
        commands.reload();
        check(look::themeId() == ThemeId::Light && look::densityId() == DensityId::Compact,
              "lookfeel.theme \"light\" chooses Light; no density key reads as Compact");
        settings.set("lookfeel.theme", std::string("os"), {}, why);
        commands.reload();
        check(look::themeId() == ThemeId::Dark, "and \"os\", the registry's default, reads as Dark");
        look::apply(ThemeId::Dark, DensityId::Compact);
    }
    check(submitter.submit(track(1)).ok, "commit between window frames");
    const auto before = b.generation();
    a.frame();
    check(a.generation() > before && b.generation() == before,
          "windows can differ by one presentation frame");
    check(a.reader().trackCount() == 1 && b.reader().trackCount() == 0,
          "all reads within one root use its presented snapshot");
    // b's frame is stale, but the action must undo the actual latest edit.
    check(b.command(AppCommands::Undo), "undo from stale window uses current history");
    check(view.current()->tracks.empty(), "stale-window undo removes current track");
    b.frame();
    a.frame();
    check(a.reader().get() == b.reader().get(), "windows converge on next frames");
    check(b.command(AppCommands::Redo), "redo via OpSubmitter");
    a.frame();
    b.frame();
    check(a.reader().trackCount() == 1, "redo refreshes ProjectView");
    check(a.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)),
          "synthesised Space reaches app command layer");
    check(!driver.playing(), "UI command does not touch driver state");
    mailbox.drain(driver);
    a.frame();
    check(driver.playing() && a.playing(), "driver starts and frame reads feedback");
    reference(a, "root-playing", write);
    check(a.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)), "queue stop");
    check(a.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)),
          "queue immediate play before driver drains");
    mailbox.drain(driver);
    check(driver.playing(), "two rapid toggles preserve order");
    check(!b.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)),
          "Esc stops nothing: in Live it closes a window or dialog");
    check(b.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)), "other window routes stop");
    mailbox.drain(driver);
    b.frame();
    check(!driver.playing() && !b.playing(), "stop feedback");
    const auto opCount = store->db().execAndGet("SELECT count(*) FROM ops").getInt();
    for (int i = 0; i < 16; ++i)
        mailbox.post(TransportMailbox::Command::Stop);
    check(!a.command(AppCommands::PlayStop), "queue overflow reported");
    check(store->db().execAndGet("SELECT count(*) FROM ops").getInt() == opCount,
          "queue refusal does not journal a command");
    mailbox.drain(driver);
    std::string why;
    check(settings.set("shortcuts.preset", "cubase", {}, why), "change app preset");
    commands.reload();
    check(a.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)),
          "transport shortcut remains available after app preset reload");
    mailbox.drain(driver);
    check(a.keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)),
          "preset undo key");
    check(a.keyPressed(juce::KeyPress(
              'z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)),
          "Cubase redo key");
#if !JUCE_MAC
    check(commands.manager.getKeyMappings()->findCommandForKeyPress(
              juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0)) == 0,
          "old Live redo mapping removed");
#endif
    a.state().zoom = 1.25;
    check(a.persist(), "view zoom persisted for arrangement consumer");
    check(persistence.load("main").zoom == 1.25 && persistence.load("floating").zoom == 1,
          "per-window zoom persistence");
    check(a.command(AppCommands::SwapPanels), "swap command");
    check(a.state().panels.slots()[0].panel == Panel::Mixer &&
              a.state().panels.find(Panel::Browser)->width == DesktopDefaults{}.browser,
          "panel widths follow identity");
    a.frame();
    reference(a, "root-swapped", write);
    {
        // The director's layout C (2026-10-28): names, track controls, lanes; the browser on the
        // right; Live's strip at the window's left edge, so the track controls meet the lanes.
        // Live's sides: lanes, names, track controls, the strip at the right edge.
        const auto at = a.arrangement.getPosition();
        const auto namesC = a.arrangement.headers.getBounds() + at;
        const auto lanesC = a.arrangement.canvas.getBounds() + at;
        check(a.arrangement.namesFirst() && a.namesBar.flip.getToggleState() &&
                  a.stripArea().getX() == look::gap && a.stripArea().getRight() <= namesC.getX() &&
                  namesC.getRight() <= a.mixerArea().getX() &&
                  lanesC.getX() - a.mixerArea().getRight() <= ArrangementView::kLeftMargin &&
                  lanesC.getRight() <= a.browserArea().getX() &&
                  a.arrangement.ruler.getX() + at.x == lanesC.getX(),
              "flipped: the strip at the left edge, the names, the track controls against the lanes");
        // The Arrangement sheet's place for the flip (DF-W25): first on the bar over the track names,
        // which spans the names, their controls and the strip, level with the beat ruler.
        auto onTheBar = [&a] {
            const auto bar = a.namesBar.getBounds();
            const auto names = a.arrangement.headers.getBounds() + a.arrangement.getPosition();
            const auto r = a.namesBar.flip.getBounds() + bar.getPosition();
            return a.namesBar.isVisible() && bar.getX() <= std::min(names.getX(), a.stripArea().getX()) &&
                   bar.getRight() >= std::max(a.mixerArea().getRight(), a.stripArea().getRight()) &&
                   bar.getY() == a.arrangement.ruler.getY() + a.arrangement.getY() && r.getX() < names.getRight() &&
                   bar.contains(r) && a.namesBar.flip.getTooltip() == "Flip sides";
        };
        check(onTheBar(), "the flip leads the bar over the names, level with the ruler, and says Flip sides");
        a.namesBar.flip.onClick();
        a.frame();
        const auto atL = a.arrangement.getPosition();
        const auto namesL = a.arrangement.headers.getBounds() + atL;
        const auto lanesL = a.arrangement.canvas.getBounds() + atL;
        check(!a.arrangement.namesFirst() && !a.namesBar.flip.getToggleState() &&
                  a.state().panels.slots()[0].panel == Panel::Browser &&
                  a.browserArea().getRight() <= lanesL.getX() &&
                  lanesL.getRight() <= namesL.getX() &&
                  namesL.getRight() <= a.mixerArea().getX() &&
                  a.mixerArea().getRight() == a.stripArea().getX() && onTheBar(),
              "the arrows put Live's sides back: lanes, names, track controls, the strip at the edge");
        // With the track controls hidden, the strip goes with them and the arrangement takes
        // their place, up to the window's edge.
        a.command(AppCommands::ToggleMixer);
        a.frame();
        check(!a.mixer.isVisible() && !a.strip.isVisible() &&
                  a.arrangement.getRight() == a.getWidth() - look::gap,
              "hidden track controls take the strip with them; the arrangement reaches the edge");
        // The bar over the names alone keeps Flip and Show, Show wide enough to read, and leaves out
        // what does not fit, every part it keeps inside it.
        {
            auto &bar = a.namesBar;
            const auto names = a.arrangement.headers.getBounds() + a.arrangement.getPosition();
            bool inside = true;
            for (int i = 0; i < bar.getNumChildComponents(); ++i)
                if (const auto *c = bar.getChildComponent(i); c->isVisible())
                    inside = inside && c->getX() >= 0 && c->getRight() <= bar.getWidth();
            check(bar.isVisible() && bar.getX() == names.getX() && bar.getWidth() == names.getWidth() &&
                      bar.flip.isVisible() && bar.show.isVisible() && bar.show.getWidth() >= 60 &&
                      !bar.lock.isVisible() && inside,
                  "the bar over the names alone keeps Flip and Show (its view's name alone) and leaves out what does not fit");
        }
        a.command(AppCommands::ToggleMixer);
        a.frame();
        a.namesBar.flip.onClick();
        a.frame();
        check(a.arrangement.namesFirst() && a.state().panels.slots()[0].panel == Panel::Mixer,
              "and swaps them again");
    }
    {
        // Bitwig's Use Large Track Height (the director, 2026-10-28): the strip's button halves
        // every track, and the track controls' rows with them; a track resized while they are
        // small keeps that size when they grow again.
        auto &lanes = a.arrangement.geometry;
        const auto rows = laneOrder(a);
        const std::int64_t first = rows.empty() ? 0 : rows.front();
        const int full = lanes.heightFor(first);
        auto ticked = [&commands, &a] {
            commands.activate(a);
            juce::ApplicationCommandInfo info(AppCommands::LargeTracks);
            commands.getCommandInfo(AppCommands::LargeTracks, info);
            return (info.flags & juce::ApplicationCommandInfo::isTicked) != 0 &&
                   info.shortName == "Use Large Track Height";
        };
        auto rowsFollow = [&a, &lanes] {
            for (const auto &row : a.mixer.strips())
                if (row->isVisible() && row->getHeight() != lanes.heightFor(row->track()))
                    return false;
            return true;
        };
        const auto at = a.strip.tracks.getBounds() + a.strip.getPosition();
        check(first != 0 && a.state().largeTracks && a.strip.tracks.getToggleState() && ticked() &&
                  at.getY() >= a.namesBar.getBottom() && a.stripArea().contains(at),
              "large tracks by default: the button lit on the strip under the bar, the View menu ticked");
        a.strip.tracks.onClick();
        a.frame();
        check(!a.state().largeTracks && !a.strip.tracks.getToggleState() && !ticked() &&
                  lanes.heightFor(first) == std::max(ArrangementGeometry::kMinTrackHeight, full / 2) &&
                  rowsFollow(),
              "off, every track shows at half its height, the track controls' rows with it");
        lanes.resizeTrack(first, 30);
        a.arrangement.changed();
        a.frame();
        a.strip.tracks.onClick();
        a.frame();
        check(a.state().largeTracks && lanes.heightFor(first) == 60 && rowsFollow(),
              "a track resized to 30 while small comes back at 60");
        lanes.resizeTrack(first, full);
        a.arrangement.changed();
        a.frame();
    }
    a.setSize(400, 540);
    const auto order = a.state().panels.slots()[0].panel;
    check(!a.command(AppCommands::SwapPanels) && a.state().panels.slots()[0].panel == order,
          "too narrow swap refuses without mutation");
    a.setSize(960, 540);
    a.frame();
    b.frame();
    a.mark(DirtySet::Transport);
    a.mark(DirtySet::Transport);
    a.frame();
    b.frame();
    check(a.lastDrain() == DirtySet::Transport && b.lastDrain() == 0,
          "dirty set coalesces per window");
    allocations = 0;
    audit = true;
    for (int i = 0; i < 1000; ++i) {
        a.mark(DirtySet::Transport);
        a.frame();
        b.frame();
    }
    audit = false;
    check(allocations == 0, "steady and dirty frame paths allocate zero");
    check(submitter.submit(track(2)).ok, "new model for changed frame audit");
    // A new track realises its mixer row once; that is the one allocation a new
    // generation may make. A new generation with the same rows makes none.
    a.frame();
    b.frame();
    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id", 2}, {"name", "Renamed"}};
    check(submitter.submit(std::move(rename)).ok, "a new generation with the same rows");
    allocations = 0;
    audit = true;
    a.frame();
    b.frame();
    audit = false;
    check(allocations == 0, "generation-change frame allocates zero");
    OpRequest tempo;
    tempo.opType = "project.insertTempoEvent";
    tempo.payload = {{"pos", 0}, {"bpm", 90.0}, {"curve", 0}};
    check(submitter.submit(std::move(tempo)).ok, "tempo edit through OpSubmitter");
    a.frame();
    check(a.bpm() == 90 && b.bpm() == 120, "BPM follows each window's own frame");
    b.frame();
    check(b.bpm() == 90, "second window catches up with committed tempo");
    allocations = 0;
    audit = true;
    mailbox.post(TransportMailbox::Command::Stop);
    mailbox.drain(driver);
    audit = false;
    check(allocations == 0, "driver command handoff allocates zero");
    auto ro = Store::open(temp.path() / "p.adi", err, true);
    ViewStateStore readonly(*ro);
    engine::ProjectView roView;
    roView.refresh(*ro);
    OpSubmitter roSubmit(*ro, roView);
    TransportMailbox roMailbox;
    SessionState roSession(*ro);
    AdiRootComponent readRoot(roView, roSubmit, roMailbox, commands, readonly, roSession, "main");
    const auto roOrder = readRoot.state().panels.slots()[0].panel;
    check(!readRoot.command(AppCommands::SwapPanels) &&
              readRoot.state().panels.slots()[0].panel == roOrder,
          "failed persistence rolls back visible order");
    // Selection is the session's, one per project (ADR-0021, UI-ARCHITECTURE §2).
    a.arrangement.geometry.selection.track = 2;
    a.arrangement.geometry.selection.start = 0;
    a.arrangement.geometry.selection.end = 960;
    a.publishSelection();
    b.frame();
    check(b.arrangement.geometry.selection == a.arrangement.geometry.selection,
          "every window presents the session's one selection");
    check(SessionState(*store).selection().track == 2 &&
              store->db().execAndGet("SELECT count(*) FROM ops WHERE op_type LIKE 'select%'")
                      .getInt() == 0,
          "it is kept in session_state, not in the op log");
    readRoot.arrangement.geometry.selection.track = 2;
    readRoot.publishSelection();
    check(roSession.selection().track == 2, "a read-only project still selects, in memory");
    // The first frame after a selection changes does work (the device strip follows the
    // selected track); the frames after it, with nothing changed, do none.
    a.frame();
    b.frame();
    allocations = 0;
    audit = true;
    a.frame();
    b.frame();
    audit = false;
    check(allocations == 0, "an unchanged selection costs a frame nothing");
    // The rulings since 29 September (ADR-0203 d6, d7, d12): Live's view keys,
    // the strip's floor, the Info View and Status Bar, the AI tab, tooltips.
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto alt = juce::ModifierKeys::altModifier;
    a.setSize(1200, 720);
    const int floor = DesktopDefaults{}.deviceFloor;
    check(floor == look::detailHeight && a.state().deviceFloor == floor &&
              a.deviceArea().getHeight() == floor,
          "the device strip starts at its floor, Live 11's Device View");
    a.state().setDeviceHeight(40);
    check(a.state().deviceHeight == floor, "the strip never goes below its floor");
    a.state().setDeviceHeight(300);
    a.resized();
    check(a.deviceArea().getHeight() == 300, "the strip grows upward");
    a.setSize(1200, 300);
    check(a.deviceArea().getHeight() == floor, "a short window keeps the floor");
    a.setSize(1200, 720);
    check(a.keyPressed(juce::KeyPress('b', cmd | alt, 0)) &&
              a.state().panels.find(Panel::Browser)->collapsed,
          "Ctrl/Cmd+Alt+B hides the browser");
    check(persistence.load("main").panels.find(Panel::Browser)->collapsed,
          "browser visibility persists in ui_view");
    check(!a.aiTab.isVisible() && a.browserArea().isEmpty(), "the AI tab goes with the browser");
    check(a.keyPressed(juce::KeyPress('5', cmd | alt, 0)) &&
              !a.state().panels.find(Panel::Browser)->collapsed,
          "Ctrl/Cmd+Alt+5 shows it again");
    check(a.command(AppCommands::ToggleMixer) && a.state().panels.find(Panel::Mixer)->collapsed,
          "the View menu hides the track controls' panel");
    check(a.command(AppCommands::ToggleMixer), "and shows it");
    check(a.keyPressed(juce::KeyPress('4', cmd | alt, 0)) && !a.state().docked &&
              a.deviceArea().isEmpty(),
          "Ctrl/Cmd+Alt+4 hides the Device View");
    check(a.keyPressed(juce::KeyPress('4', cmd | alt, 0)) && a.state().docked &&
              a.deviceArea().getHeight() == 300,
          "and shows it at its own height");
    check(!a.state().aiChatOpen && a.aiTab.isVisible() && a.chatArea().getHeight() == 20 &&
              a.chatArea().getBottom() ==
                  a.browserArea().getBottom() - BrowserPanel::kBarHeight - look::border,
          "the AI chat is a collapsed tab at the foot of the browser's panes");
    a.aiTab.onClick();
    check(a.state().aiChatOpen && a.chatArea().getHeight() > 20 &&
              persistence.load("main").aiChatOpen,
          "the tab opens and is remembered per window");
    a.aiTab.onClick();
    check(commands.infoView() && a.infoArea().getX() == look::gap &&
              a.infoArea().getBottom() == a.status.getY() &&
              a.infoArea().getRight() + look::gap == a.deviceArea().getX(),
          "the Info View is on, bottom-left, beside the Device View");
    a.hover(&a.transport.play);
    check(a.infoText().contains("Play") && a.infoText().contains("Space"),
          "the Info View describes the element under the mouse");
    a.hover(nullptr);
    check(a.infoText().isEmpty(), "and clears when nothing is under it");
    {
        // Each platform's fonts wrap a description differently: one longer than the panel is
        // cut at the panel's foot, never drawn over its edge or the Status Bar below.
        auto render = [&a] {
            juce::Image image(juce::Image::ARGB, a.getWidth(), a.getHeight(), true,
                              juce::SoftwareImageType{});
            juce::Graphics g(image);
            a.paintEntireComponent(g, true);
            return image;
        };
        const auto quiet = render();
        const auto saved = a.arrangement.canvas.getTooltip();
        a.arrangement.canvas.setTooltip(juce::String::repeatedString("A description that goes on. ", 80));
        a.hover(&a.arrangement.canvas);
        const auto busy = render();
        a.arrangement.canvas.setTooltip(saved);
        a.hover(nullptr);
        const auto inner = a.infoArea().reduced(look::border);
        int outside = 0;
        for (int y = 0; y < busy.getHeight(); ++y)
            for (int x = 0; x < busy.getWidth(); ++x)
                if (!inner.contains(x, y) && busy.getPixelAt(x, y) != quiet.getPixelAt(x, y))
                    ++outside;
        check(outside == 0, "a long description stays inside the Info View");
    }
    check(a.keyPressed(juce::KeyPress('?', juce::ModifierKeys::shiftModifier, 0)) &&
              !commands.infoView() && a.infoArea().isEmpty() && a.deviceArea().getX() == look::gap,
          "Shift+? hides the Info View");
    b.frame();
    check(b.infoArea().isEmpty(), "the Info View is app-scoped: every window follows");
    check(settings.get("lookfeel.showInfoView") == false, "and the App setting records it");
    a.status.infoToggle.onClick();
    check(commands.infoView() && !a.infoArea().isEmpty(), "the Status Bar's toggle shows it");
    a.setSize(400, 540);
    check(!a.command(AppCommands::SwapPanels) && !a.error().empty(),
          "a refused swap says why in the Status Bar");
    check(a.status.getBounds() ==
              juce::Rectangle<int>(0, 540 - look::statusBarHeight, 400, look::statusBarHeight),
          "the Status Bar runs along the window's foot");
    a.setSize(1200, 720);
    check(a.command(AppCommands::PlayStop) && a.error().empty(),
          "the next success clears the message");
    mailbox.drain(driver);
    check(a.command(AppCommands::Stop), "stop button command");
    mailbox.drain(driver);
    ZoomHost host(std::make_unique<AdiRootComponent>(view, submitter, mailbox, commands,
                                                     persistence, session, "zoomed"));
    host.setSize(1200, 720);
    auto &zoomed = host.root();
    check(zoomed.keyPressed(juce::KeyPress('=', cmd, 0)) && zoomed.interfaceZoom() == 1.25f,
          "Ctrl/Cmd++ zooms this window in");
    check(zoomed.getWidth() == 960 && zoomed.getHeight() == 576,
          "the root lays out in logical units under the window's zoom");
    check(a.interfaceZoom() == 1.f, "other windows keep their own zoom");
    check(zoomed.keyPressed(juce::KeyPress('-', cmd, 0)) &&
              zoomed.keyPressed(juce::KeyPress('-', cmd, 0)) && zoomed.interfaceZoom() == .75f,
          "Ctrl/Cmd+- zooms out");
    {
        // Kept per window and per display (window.zoom, N4), saved at once, and read when the
        // window opens again on that display.
        const auto display = displayAt(zoomed.getScreenBounds().getCentre());
        settings::AppSettings reread(appdata::App::Daw, settings.file());
        check(commands.keptZoom("zoomed", display) == .75f && !commands.keptZoom("tips", display) &&
                  settings::windowZoom(reread, "zoomed", display) == .75 &&
                  settings.get("window.zoom").contains(settings::windowZoomKey("zoomed", display)),
              "each window's zoom is kept for its display in the App settings, saved at once");
        AdiWindow again(std::make_unique<AdiRootComponent>(view, submitter, mailbox, commands, persistence, session,
                                                           "zoomed"),
                        commands, false);
        again.setBounds(40, 40, 1200, 720);
        AdiWindow other(std::make_unique<AdiRootComponent>(view, submitter, mailbox, commands, persistence, session,
                                                           "fresh"),
                        commands, false);
        other.setBounds(40, 40, 1200, 720);
        check(displayAt(again.root().getScreenBounds().getCentre()) == display &&
                  again.root().interfaceZoom() == .75f && other.root().interfaceZoom() == 1.f,
              "a window opens at the zoom kept for it on its display; one with none kept at 100 %");
    }
    check(zoomed.deviceArea().getHeight() == floor, "the floor is in zoom-independent units");
    AdiWindow window(std::make_unique<AdiRootComponent>(view, submitter, mailbox, commands,
                                                        persistence, session, "tips"),
                     commands, false);
    check(window.tooltips() != nullptr && window.tooltipDelayMs() == 600,
          "floating tooltips are on, after 600 ms");
    check(settings.set("lookfeel.showTooltips", false, {}, why), "switch tooltips off");
    commands.reload();
    AdiWindow quiet(std::make_unique<AdiRootComponent>(view, submitter, mailbox, commands,
                                                       persistence, session, "quiet"),
                    commands, false);
    check(quiet.tooltips() == nullptr, "the setting turns them off for new windows");
    // The View menu's toggles (§42.1): Full Screen, the Overview and In/Out.
    {
        AdiWindow viewWindow(std::make_unique<AdiRootComponent>(view, submitter, mailbox, commands, persistence,
                                                                session, "view-toggles"),
                             commands, false);
        viewWindow.setSize(1200, 760);
        auto &viewRoot = viewWindow.root();
        viewRoot.frame();
#if JUCE_MAC
        const juce::KeyPress fullScreenKey('f', juce::ModifierKeys::ctrlModifier | cmd, 0);
#else
        const juce::KeyPress fullScreenKey(juce::KeyPress::F11Key);
#endif
        // Filling the screen needs a window on it, which a peerless test has not: the key is
        // mapped, and the command refuses here and says why (seen working in the app itself).
        check(commands.manager.getKeyMappings()->findCommandForKeyPress(fullScreenKey) == AppCommands::FullScreen &&
                  viewRoot.keyPressed(fullScreenKey) && !viewWindow.isFullScreen() && !viewRoot.fullScreen() &&
                  !viewRoot.error().empty(),
              "Full Screen is F11 (Ctrl+Cmd+F on the Mac); a window off the screen refuses it and says why");
        auto &lanesView = viewRoot.arrangement;
        check(lanesView.overview.isVisible() && lanesView.canvas.getY() == ArrangementView::kTopHeight,
              "the Overview is shown by default");
        check(viewRoot.keyPressed(juce::KeyPress('o', cmd | alt, 0)) && !lanesView.overview.isVisible() &&
                  lanesView.ruler.getY() == 0 &&
                  lanesView.canvas.getY() == ArrangementView::kTopHeight - ArrangementView::kOverviewHeight &&
                  !persistence.load("view-toggles").overview,
              "Ctrl/Cmd+Alt+O hides the Overview: the ruler and the lanes move up, kept with the window");
        viewRoot.frame();
        const MixerStrip *firstRow = nullptr;
        for (const auto &row : viewRoot.mixer.strips())
            if (row->isVisible() && firstRow == nullptr)
                firstRow = row.get();
        check(firstRow != nullptr && firstRow->getY() == lanesView.canvas.getY() - lanesView.geometry.scrollY,
              "the track controls' rows stay level with the lanes");
        const int activatorWith = firstRow != nullptr ? firstRow->active.getX() : 0;
        const int controlsWith = viewRoot.mixer.getWidth();
        check(firstRow != nullptr && firstRow->showsInOut() && viewRoot.keyPressed(juce::KeyPress('i', cmd | alt, 0)) &&
                  !firstRow->showsInOut() && firstRow->active.getX() == activatorWith - MixerPanel::kInOutWidth &&
                  viewRoot.mixer.getWidth() == controlsWith - MixerPanel::kInOutWidth &&
                  !persistence.load("view-toggles").inOut,
              "Ctrl/Cmd+Alt+I hides the track controls' In/Out: the panel narrows by its column");
        // Live's editing grid (§6.10, §42.13): the commands on Ctrl/Cmd+1 to 5, kept with the
        // window; the step and its label; snapping.
        {
            auto &lanes = lanesView.geometry;
            const auto ppq = textproj::kPPQ;
            lanes.scale = 48;
            check(lanes.grid == ArrangementGeometry::Grid{} && lanes.gridQuarters() == 1 &&
                      gridLabel(lanes.gridQuarters(), false) == "1/4" && lanes.snap(ppq * 3 / 2, false) == ppq &&
                      lanes.snap(ppq * 3 / 2, true) == 2 * ppq && lanesView.snaps(juce::ModifierKeys()),
                  "the grid by default: zoom-adaptive, a quarter at this zoom, snapping");
            check(viewRoot.keyPressed(juce::KeyPress('1', cmd, 0)) && lanes.gridQuarters() == .5 &&
                      persistence.load("view-toggles").gridNarrow == 1,
                  "Ctrl/Cmd+1 narrows it, kept with the window");
            check(viewRoot.keyPressed(juce::KeyPress('3', cmd, 0)) && lanes.grid.triplet &&
                      std::abs(lanes.gridQuarters() - 1. / 3) < 1e-12 &&
                      gridLabel(lanes.gridQuarters(), true) == "1/8T",
                  "Ctrl/Cmd+3 makes it triplets: 1/8T");
            check(viewRoot.keyPressed(juce::KeyPress('5', cmd, 0)) && lanes.grid.fixed &&
                      lanes.grid.fixedQuarters == .5,
                  "Ctrl/Cmd+5 fixes it at the spacing on screen");
            lanes.scale = 4;
            check(std::abs(lanes.gridQuarters() - 1. / 3) < 1e-12, "a fixed grid keeps its step at any zoom");
            check(viewRoot.keyPressed(juce::KeyPress('2', cmd, 0)) && lanes.grid.fixedQuarters == 1,
                  "Ctrl/Cmd+2 widens it");
            check(viewRoot.keyPressed(juce::KeyPress('4', cmd, 0)) && !lanes.grid.snap &&
                      !lanesView.snaps(juce::ModifierKeys()) &&
                      persistence.load("view-toggles").gridSnap == false,
                  "Ctrl/Cmd+4 switches snapping off");
#if JUCE_MAC
            const juce::ModifierKeys bypass(juce::ModifierKeys::commandModifier);
#else
            const juce::ModifierKeys bypass(juce::ModifierKeys::altModifier);
#endif
            check(lanesView.snaps(bypass), "with snapping off, Alt (Cmd on the Mac) snaps while held");
            check(gridLabel(4, false) == "1 Bar" && gridLabel(8, false) == "2 Bars" && gridLabel(.25, false) == "1/16",
                  "the spacing reads as Live's: bars, then fractions");
            // The lanes' menu names the widths (§6.10); the keys stay within them.
            lanes.grid = {};
            lanes.scale = 48;
            auto gridMenu = [&lanesView] {
                juce::PopupMenu menu;
                lanesView.addGridItems(menu);
                return menu;
            };
            auto sub = [](const juce::PopupMenu &menu, const char *name) -> const juce::PopupMenu * {
                for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                    if (it.getItem().text == name)
                        return it.getItem().subMenu.get();
                return nullptr;
            };
            // Each item's name, with " *" when it is ticked.
            auto names = [](const juce::PopupMenu *menu) {
                juce::StringArray out;
                if (menu != nullptr)
                    for (juce::PopupMenu::MenuItemIterator it(*menu); it.next();)
                        out.add(it.getItem().text + (it.getItem().isTicked ? " *" : ""));
                return out;
            };
            auto pick = [](const juce::PopupMenu *menu, const char *name) {
                if (menu != nullptr)
                    for (juce::PopupMenu::MenuItemIterator it(*menu); it.next();)
                        if (it.getItem().text == name && it.getItem().action) {
                            it.getItem().action();
                            return true;
                        }
                return false;
            };
            {
                const auto menu = gridMenu();
                std::vector<int> ids;
                for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                    ids.push_back(it.getItem().itemID);
                check(names(sub(menu, "Adaptive Grid")) ==
                          juce::StringArray{"Widest", "Wide", "Medium *", "Narrow", "Narrowest"},
                      "the lanes' menu names the adaptive widths, Medium ticked by default");
                check(names(sub(menu, "Fixed Grid")) == juce::StringArray{"8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2",
                                                                          "1/4", "1/8", "1/16", "1/32"},
                      "and the fixed widths, 8 Bars to 1/32, none ticked");
                check(ids == std::vector<int>{0, 0, AppCommands::TripletGrid, AppCommands::SnapToGrid},
                      "then Triplet Grid and Snap to Grid");
                check(pick(sub(menu, "Fixed Grid"), "1/8") && lanes.grid.fixed && lanes.gridQuarters() == .5 &&
                          persistence.load("view-toggles").gridFixed &&
                          persistence.load("view-toggles").gridFixedQuarters == .5,
                      "choosing 1/8 fixes the grid at an eighth, kept with the window");
            }
            check(names(sub(gridMenu(), "Fixed Grid"))[6] == "1/8 *" &&
                      !names(sub(gridMenu(), "Adaptive Grid")).joinIntoString(",").contains("*"),
                  "the menu ticks the width in use, and only it");
            check(viewRoot.keyPressed(juce::KeyPress('1', cmd, 0)) && viewRoot.keyPressed(juce::KeyPress('1', cmd, 0)) &&
                      viewRoot.keyPressed(juce::KeyPress('1', cmd, 0)) && lanes.grid.fixedQuarters == .125,
                  "Ctrl/Cmd+1 narrows a fixed grid no further than 1/32");
            check(pick(sub(gridMenu(), "Adaptive Grid"), "Narrowest") && !lanes.grid.fixed && lanes.grid.narrow == 2 &&
                      lanes.gridQuarters() == .25 && persistence.load("view-toggles").gridNarrow == 2,
                  "choosing Narrowest makes it adaptive again, two steps narrower");
            check(viewRoot.keyPressed(juce::KeyPress('1', cmd, 0)) && lanes.grid.narrow == 2,
                  "and Ctrl/Cmd+1 narrows it no further");
            for (int i = 0; i < 5; ++i)
                viewRoot.keyPressed(juce::KeyPress('2', cmd, 0));
            check(lanes.grid.narrow == -2 && names(sub(gridMenu(), "Adaptive Grid"))[0] == "Widest *",
                  "Ctrl/Cmd+2 widens it as far as Widest");
            lanes.grid = {};
            lanes.scale = 48;
            lanesView.changed();
        }
        check(viewRoot.command(AppCommands::ToggleOverview) && viewRoot.command(AppCommands::ToggleInOut) &&
                  lanesView.overview.isVisible() && firstRow->showsInOut() &&
                  viewRoot.mixer.getWidth() == controlsWith &&
                  lanesView.canvas.getY() == ArrangementView::kTopHeight,
              "and both come back");
    }
    // An open window follows at its next frame (a change in Settings applies at once).
    window.followSettings();
    check(window.tooltips() == nullptr, "and off in an open window at its next frame");
    check(settings.set("lookfeel.showTooltips", true, {}, why) &&
              settings.set("lookfeel.tooltipDelayMs", 750, {}, why),
          "switch tooltips on again, slower");
    commands.reload();
    quiet.followSettings();
    window.followSettings();
    check(quiet.tooltips() != nullptr && quiet.tooltipDelayMs() == 750 && window.tooltipDelayMs() == 750,
          "on again in an open window, with the new delay");
    check(settings.set("lookfeel.showTooltips", false, {}, why) &&
              settings.set("lookfeel.tooltipDelayMs", 600, {}, why),
          "tooltips off and back to 600 ms for the checks after");
    commands.reload();
    // Live's menu bar in Live's order, each menu listing only the commands ADI has (§42.28).
    {
        auto *bar = window.menu();
        const auto names = bar->getMenuBarNames();
        auto items = [&](const char *name) {
            std::vector<int> ids;
            const auto menu = bar->getMenuForIndex(names.indexOf(name), name);
            for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                ids.push_back(it.getItem().isSeparator ? 0 : it.getItem().itemID);
            return ids;
        };
        using C = AppCommands;
#if JUCE_MAC
        check(names == juce::StringArray{"File", "Edit", "Create", "View", "Options", "Navigate"},
              "the Mac's menu bar: File, Edit, Create, View, Options, Navigate");
        check(items("Options") == std::vector<int>{C::Follow, 0, C::DrawMode, 0, C::NarrowGrid, C::WidenGrid, C::TripletGrid,
                                                   C::SnapToGrid, C::FixedGrid, 0, C::SoloInPlace},
              "the Options menu: Follow (§6.2), Draw Mode (§10.4.1), the grid (§6.10), then Solo in Place (§18.6)");
        check(items("File") == std::vector<int>{C::NewProject, C::OpenProject, 0, C::TakeSnapshot, C::SaveAs,
                                                C::SaveCopy, C::CollectAllAndSave, C::CollectAndExport, 0, C::ExportAudio},
              "the File menu: Save As, then Save a Copy and Collect All and Save (5.4.1, 5.7), ADI's "
              "Collect and Export after them, then Export Audio/Video (5.2.1); Quit is the application menu's on the Mac");
#else
        check(names == juce::StringArray{"File", "Edit", "Create", "View", "Options", "Navigate"},
              "the menu bar: File, Edit, Create, View, Options, Navigate");
        check(items("File") == std::vector<int>{C::NewProject, C::OpenProject, 0, C::TakeSnapshot, C::SaveAs,
                                                C::SaveCopy, C::CollectAllAndSave, C::CollectAndExport, 0, C::ExportAudio, 0,
                                                C::Quit},
              "the File menu: Save As, then Save a Copy and Collect All and Save (5.4.1, 5.7), ADI's "
              "Collect and Export after them, then Export Audio/Video (5.2.1); it ends with Quit and no longer holds Settings");
        check(items("Options") == std::vector<int>{C::Follow, 0, C::DrawMode, 0, C::NarrowGrid, C::WidenGrid, C::TripletGrid,
                                                   C::SnapToGrid, C::FixedGrid, 0, C::SoloInPlace, 0, C::Settings},
              "the Options menu: Follow (§6.2), Draw Mode (§10.4.1), the grid (§6.10), Solo in Place (§18.6), then Settings off the Mac "
              "(§2.3)");
#endif
        // The new ids' names through getCommandInfo: `names[id - PlayStop]` is a table by position, so
        // an entry out of order mislabels a menu item with no compile error. B5 (Solo in Place) landed
        // first and B4 (window/track-delay) extends the same tail: SoloInPlace, then the two Track
        // Options toggles, ToggleMixerTrackOptions the last id; this check reads the tail as aligned.
        {
            juce::ApplicationCommandInfo solo(C::SoloInPlace), rows(C::ToggleTrackOptions), mixer(C::ToggleMixerTrackOptions);
            commands.getCommandInfo(C::SoloInPlace, solo);
            commands.getCommandInfo(C::ToggleTrackOptions, rows);
            commands.getCommandInfo(C::ToggleMixerTrackOptions, mixer);
            juce::ApplicationCommandInfo metronome(C::Metronome), loop(C::LoopSelection), selectLoop(C::SelectLoop);
            commands.getCommandInfo(C::Metronome, metronome);
            commands.getCommandInfo(C::LoopSelection, loop);
            commands.getCommandInfo(C::SelectLoop, selectLoop);
            juce::ApplicationCommandInfo copy(C::SaveCopy), collect(C::CollectAllAndSave), exportZip(C::CollectAndExport);
            commands.getCommandInfo(C::SaveCopy, copy);
            commands.getCommandInfo(C::CollectAllAndSave, collect);
            commands.getCommandInfo(C::CollectAndExport, exportZip);
            juce::ApplicationCommandInfo cut(C::Cut), copyInfo(C::Copy), paste(C::Paste), duplicate(C::Duplicate);
            commands.getCommandInfo(C::Cut, cut);
            commands.getCommandInfo(C::Copy, copyInfo);
            commands.getCommandInfo(C::Paste, paste);
            commands.getCommandInfo(C::Duplicate, duplicate);
            const auto keyed = [](const juce::ApplicationCommandInfo &info, char key) {
                return info.defaultKeypresses.contains(juce::KeyPress(key, juce::ModifierKeys::commandModifier, 0));
            };
            check(solo.shortName == "Solo in Place" && rows.shortName == "Track Options" &&
                      mixer.shortName == "Mixer Track Options" && metronome.shortName == "Metronome" &&
                      loop.shortName == "Loop Selection" && selectLoop.shortName == "Select Loop" &&
                      copy.shortName == "Save a Copy..." && collect.shortName == "Collect All and Save" &&
                      exportZip.shortName == "Collect and Export..." && cut.shortName == "Cut" && keyed(cut, 'x') &&
                      copyInfo.shortName == "Copy" && keyed(copyInfo, 'c') && paste.shortName == "Paste" &&
                      keyed(paste, 'v') && duplicate.shortName == "Duplicate" && keyed(duplicate, 'd') &&
                      C::ToggleClipView == C::Split + 1 && C::InsertMidiClip == C::ToggleClipView + 1 &&
                      C::CutTime == C::InsertMidiClip + 1 && C::CreateFade == C::InsertSilence + 1 &&
                      C::InvertSelection == C::DeleteFades + 1 && C::ArrangePanelsAutomatically == C::DrawMode + 1 &&
                      C::FocusClipView == C::ArrangePanelsVertically + 1 && C::FitToTimeRange == C::FocusClipView + 1 &&
                      C::ArrangementRecord == C::FitToTimeRange + 1 && C::ExportAudio == C::ArrangementRecord + 1 &&
                      C::LastId == C::ExportAudio,
                  "AppCommands' tail: Solo in Place, Track Options, Mixer Track Options, Metronome, Loop Selection, "
                  "Select Loop, Save a Copy, Collect All and Save, Collect and Export, Cut, Copy, Paste, Duplicate, "
                  "Show/Hide Plug-In Windows, Split, Clip View, Insert Empty MIDI Clip(s), the …Time commands and "
                  "Insert Silence, then Create Fade In/Out and Delete Fades/Crossfades, then the Note Editor's nine, each named");
            // The Note Editor's commands, the last ids: named in the table's order, each on the editor's key.
            {
                const auto named = [&](int id) {
                    juce::ApplicationCommandInfo i(id);
                    commands.getCommandInfo(id, i);
                    return i;
                };
                const auto on = [&](int id, int code, int mods) { return named(id).defaultKeypresses.contains(juce::KeyPress(code, juce::ModifierKeys(mods), 0)); };
                const int ctrl = juce::ModifierKeys::commandModifier, shifted = juce::ModifierKeys::shiftModifier, alted = juce::ModifierKeys::altModifier;
                check(named(C::InvertSelection).shortName == "Invert Selection" && on(C::InvertSelection, 'a', ctrl | shifted) &&
                          named(C::Quantize).shortName == "Quantize" && on(C::Quantize, 'u', ctrl) &&
                          named(C::QuantizeSettings).shortName == "Quantize Settings..." && on(C::QuantizeSettings, 'u', ctrl | shifted) &&
                          named(C::CropClip).shortName == "Crop Clip" && on(C::CropClip, 'j', ctrl | shifted) &&
                          named(C::ExpandClipView).shortName == "Expand Clip View" && on(C::ExpandClipView, 'e', ctrl | alted) &&
                          named(C::FoldToNotes).shortName == "Fold to Notes" && on(C::FoldToNotes, 'f', 0) &&
                          named(C::FoldToScale).shortName == "Fold to Scale" && on(C::FoldToScale, 'g', 0) &&
                          named(C::HighlightScales).shortName == "Highlight Scales" && on(C::HighlightScales, 'k', 0) &&
                          named(C::DrawMode).shortName == "Draw Mode" && on(C::DrawMode, 'b', 0) &&
                          named(C::ArrangePanelsAutomatically).shortName == "Arrange Clip View Panels Automatically" &&
                          named(C::ArrangePanelsHorizontally).shortName == "Arrange Clip View Panels Horizontally" &&
                          named(C::ArrangePanelsVertically).shortName == "Arrange Clip View Panels Vertically" &&
                          named(C::FocusClipView).shortName == "Clip View" && on(C::FocusClipView, '3', alted) &&
                          named(C::FitToTimeRange).shortName == "Fit to Time Range" && on(C::FitToTimeRange, 'j', ctrl | alted),
                      "Invert Selection, Quantize, Quantize Settings, Crop Clip, Expand Clip View, Fold to Notes, Fold to Scale, "
                      "Highlight Scales and Draw Mode are named, each on Live's key; then the three that arrange the Clip View's panels, "
                      "and the Navigate menu's Clip View on Alt/Option+3");
            }
            juce::ApplicationCommandInfo clipViewInfo(C::ToggleClipView);
            commands.getCommandInfo(C::ToggleClipView, clipViewInfo);
            juce::ApplicationCommandInfo midiClipInfo(C::InsertMidiClip);
            commands.getCommandInfo(C::InsertMidiClip, midiClipInfo);
            check(clipViewInfo.shortName == "Clip View" && midiClipInfo.shortName == "Insert Empty MIDI Clip(s)" &&
                      midiClipInfo.defaultKeypresses.contains(juce::KeyPress(
                          'm', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)),
                  "Clip View and Insert Empty MIDI Clip(s) are named, the second on Ctrl/Cmd+Shift+M");
            juce::ApplicationCommandInfo cutTime(C::CutTime), insert(C::InsertSilence), deleteTime(C::DeleteTime);
            commands.getCommandInfo(C::CutTime, cutTime);
            commands.getCommandInfo(C::InsertSilence, insert);
            commands.getCommandInfo(C::DeleteTime, deleteTime);
            check(cutTime.shortName == "Cut Time" &&
                      cutTime.defaultKeypresses.contains(juce::KeyPress('x', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) &&
                      insert.shortName == "Insert Silence" && keyed(insert, 'i') && deleteTime.shortName == "Delete Time",
                  "Cut Time on Ctrl/Cmd+Shift+X, Insert Silence on Ctrl/Cmd+I, Delete Time named (manual 41.5)");
        }
        // The File menu as drawn, for A4's picture (Save a Copy, Collect All and Save, Collect and Export).
        if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty())
            renderMenu(look::liveLook(), bar->getMenuForIndex(names.indexOf("File"), "File"),
                       juce::File(pictures).getChildFile("adi-file-menu.png"));
        // The Options menu as drawn, with Solo in Place ticked, for the picture beside Live 12's.
        if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty())
            renderMenu(look::liveLook(), bar->getMenuForIndex(names.indexOf("Options"), "Options"),
                       juce::File(pictures).getChildFile("adi-options-menu.png"));
        check(items("Edit") == std::vector<int>{C::Undo, C::Redo, 0, C::Cut, C::Copy, C::Paste, C::Duplicate, C::DeleteTrack, 0,
                                                C::CutTime, C::CopyTime, C::PasteTime, C::DuplicateTime, C::DeleteTime,
                                                C::InsertSilence, 0, C::RenameTrack, C::EditInfoText,
                                                C::SelectAll, C::InvertSelection, 0, C::LoopSelection, C::SelectLoop, 0, C::Split, C::CropClip, 0,
                                                C::Quantize, C::QuantizeSettings, C::FitToTimeRange, 0, C::GroupTracks, C::UngroupTracks},
              "the Edit menu: Cut, Copy, Paste and Duplicate, then Delete (41.5), then the …Time commands and Insert Silence "
              "(6.11), before Rename (§18.2), Select All and Invert Selection, then Loop Selection and Select Loop (§6.6), then "
              "Split and Crop Clip, the clip commands (6.9, 10.7.1), then Quantize, its settings (10.5.8) and Fit to Time Range (10.5.5), then Group and "
              "Ungroup Tracks");
        // The Edit menu as drawn after a Delete Track, for the picture beside Live 12's (the director's
        // LK-02 capture): its Undo item names the step, as Live's "Undo Change Value" does, and its
        // Redo item reads "Nothing to Redo", as Live's does with nothing to redo. Worklist F1: what
        // undo brings back is pinned in the seams test; this is the menu that offers it. The picture
        // is of ADI's default key set, Live's (Redo Ctrl+Y off the Mac, §41.5): the app preset was
        // switched to Cubase above and is put back before the render.
        {
            auto &edits = window.root();
            std::int64_t victim = 0;
            for (const auto &t : view.current()->tracks)
                if (victim == 0 && (t->kind == "audio" || t->kind == "midi") && t->parent == 0)
                    victim = t->id;
            const auto countBefore = view.current()->tracks.size();
            commands.activate(edits); // the Edit menu names the step of the active root
            edits.arrangement.geometry.selection.track = victim;
            check(victim != 0 && edits.command(C::DeleteTrack) && view.current()->tracks.size() + 1 == countBefore &&
                      edits.undoTitle(false) == "Undo Delete Track" && edits.undoTitle(true) == "Nothing to Redo",
                  "Delete Track from the menu bar's window: the Undo item names the step, Redo has nothing");
            check(settings.set("shortcuts.preset", "live", {}, why), "the Live key set back for the picture");
            commands.reload();
#if !JUCE_MAC
            check(commands.manager.getKeyMappings()->findCommandForKeyPress(
                      juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0)) == C::Redo,
                  "Live's Redo key, Ctrl+Y (§41.5), is back with the preset");
#endif
            if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty())
                renderMenu(look::liveLook(), bar->getMenuForIndex(names.indexOf("Edit"), "Edit"),
                           juce::File(pictures).getChildFile("adi-edit-menu.png"));
            check(edits.command(C::Undo) && view.current()->tracks.size() == countBefore &&
                      view.current()->findTrack(victim) != nullptr,
                  "and undo brings the track back");
        }
        check(items("Create") == std::vector<int>{C::InsertAudioTrack, C::InsertMidiTrack, C::InsertReturnTrack, 0,
                                                  C::InsertMidiClip, 0, C::InsertTimeSignatureChange, 0, C::AddLocator,
                                                  C::DeleteLocator, 0, C::CreateFade, C::DeleteFades},
              "the Create menu inserts tracks, returns too (§18.4), then an empty MIDI clip, then a time signature "
              "change (§6.5), then Add and Delete Locator (§6.4), then Create Fade In/Out and Delete "
              "Fades/Crossfades (§6.8)");
        check(items("Navigate") == std::vector<int>{C::FocusControlBar, C::FocusArrangement, C::FocusClipView,
                                                    C::FocusDeviceView, C::FocusBrowser, 0,
                                                    C::NextNeighbor, C::PreviousNeighbor, 0,
                                                    C::TabMovesFocus, C::WrapTabNavigation},
              "the Navigate menu: the views ADI has, the neighbours, then Tab's two switches (§41.1.1)");
        check(!names.contains("Help"), "a menu with no command yet is left out");
        // Live 12's View menu: the track controls' and the Mixer View's controls in two submenus.
        {
            const auto viewMenu = bar->getMenuForIndex(names.indexOf("View"), "View");
            juce::StringArray submenus;
            std::vector<int> flat;
            juce::String trackControls, mixerControls;
            // A ticked item is not marked here: the submenus' groups and dimmed items are the point.
            auto texts = [](const juce::PopupMenu &menu) { return menuLine(menu, false); };
            for (juce::PopupMenu::MenuItemIterator it(viewMenu); it.next();) {
                const auto &item = it.getItem();
                if (item.subMenu != nullptr) {
                    submenus.add(item.text);
                    (item.text == "Mixer Controls" ? mixerControls : trackControls) = texts(*item.subMenu);
                } else
                    flat.push_back(item.itemID);
            }
            auto has = [&flat](int id) { return std::find(flat.begin(), flat.end(), id) != flat.end(); };
            const auto at = [&flat](int id) { return std::find(flat.begin(), flat.end(), id) - flat.begin(); };
            check(has(C::ExpandClipView) && at(C::ExpandClipView) == at(C::ToggleClipView) + 1 && has(C::FoldToNotes) &&
                      at(C::FoldToScale) == at(C::FoldToNotes) + 1 && at(C::HighlightScales) == at(C::FoldToScale) + 1 &&
                      has(C::ArrangePanelsAutomatically) && at(C::ArrangePanelsHorizontally) == at(C::ArrangePanelsAutomatically) + 1 &&
                      at(C::ArrangePanelsVertically) == at(C::ArrangePanelsHorizontally) + 1,
                  "the View menu: Expand Clip View after Clip View (10.2), the three that arrange the Clip View's panels (8.1.2), "
                  "and the Note Editor's Fold to Notes, Fold to Scale and Highlight Scales (10.6)");
            // Live 12's groups (manual 6.1, the Mixer Controls submenu's figure): In/Out, Sends,
            // Volume | Track Options, Crossfader, Performance Impact | Return Tracks.
            check(submenus == juce::StringArray{"Arrangement Track Controls", "Mixer Controls"} &&
                      trackControls == "Hide/Show Track Controls | In/Out, Sends, Volume -, Track Options" &&
                      mixerControls ==
                          "In/Out, Sends, Volume - | Track Options, Crossfader -, Performance Impact - | Return Tracks" &&
                      !has(C::ToggleInOut) && !has(C::ToggleSends) && !has(C::ToggleReturnTracks) && !has(C::ToggleMixer),
                  "the View menu holds Live 12's Arrangement Track Controls and Mixer Controls submenus in Live's "
                  "groups, their toggles no longer loose in it");
        }
        // Live on Windows (§42.28): Alt and a menu's first letter opens that menu.
        check(menuForKey(*bar, juce::KeyPress('f', alt, 0)) == names.indexOf("File") &&
                  menuForKey(*bar, juce::KeyPress('E', alt, 0)) == names.indexOf("Edit") &&
                  menuForKey(*bar, juce::KeyPress('n', alt, 0)) == names.indexOf("Navigate"),
              "Alt and a menu's first letter names that menu");
        check(menuForKey(*bar, juce::KeyPress('f', 0, 0)) < 0 &&
                  menuForKey(*bar, juce::KeyPress('f', cmd | alt, 0)) < 0 &&
                  menuForKey(*bar, juce::KeyPress('h', alt, 0)) < 0 &&
                  menuForKey(*bar, juce::KeyPress('2', alt, 0)) < 0,
              "only Alt alone opens a menu, and only one that is there");
        const auto appMenu = applicationMenu(commands);
        juce::PopupMenu::MenuItemIterator first(appMenu);
        check(first.next() && first.getItem().itemID == C::Settings && !first.next(),
              "the Mac's application menu holds Settings, as Live's (§2.3)");
    }
    // Step 7.2's application half (#198): a real document, its engine and its keys.
    std::string documentError;
    // The scene opens a project as an earlier build made it: Main by a first "New project" step,
    // the floor undo stops at, and nothing else. New itself, Live's default Set at the history's
    // root (#307), is the document test's.
    {
        StoreError made{};
        auto earlier = Store::create(temp.path() / "integrated.adi", made);
        check(earlier != nullptr, "the integrated scene's project is made");
        if (earlier) {
            engine::ProjectView earlierView;
            earlierView.refresh(*earlier);
            OpSubmitter earlierOps(*earlier, earlierView);
            OpRequest main;
            main.opType = "track.create";
            main.payload = {{"id", 1}, {"kind", "master"}, {"name", "Main"}};
            main.label = kNewProjectLabel;
            check(earlierOps.submit(std::move(main)).ok, "with Main, by its first step");
        }
    }
    auto document = ProjectDocument::open(temp.path() / "integrated.adi", false, {}, documentError);
    check(document != nullptr, "application document opens");
    if (document) {
        AdiRootComponent integrated(document->view(), document->ops(), document->mailbox(),
                                    commands, document->views(), document->sessionState(),
                                    "main");
        int snapshots = 0, saveAs = 0, quits = 0;
        integrated.afterEdit = [&] { document->synchronise(); };
        integrated.applicationCommand = [&](int id) {
            if (id == AppCommands::Quit) {
                ++quits;
                return true;
            }
            if (id == AppCommands::SaveAs) {
                // As the application does once its chooser has a file (Live 41.3; N3): the
                // document carries on in the new file; the Status Bar says nothing on success,
                // as after New and Open (only a warning), and why on a refusal.
                ++saveAs;
                const bool saved = document->saveAs(temp.path() / "integrated-as.adi");
                integrated.say(saved ? document->warning() : document->error());
                return saved;
            }
            if (id != AppCommands::TakeSnapshot)
                return false;
            ++snapshots;
            return document->takeSnapshot({}, 0, 0).ok;
        };
        check(!integrated.canUndo(false) && integrated.undoTitle(false) == "Nothing to Undo",
              "a new project has nothing to undo, as Live: the item reads Nothing to Undo");
        check(document->ops().submit(track(2)).ok && document->synchronise(),
              "application edit reaches engine");
        check(integrated.undoTitle(false) == "Undo Create track",
              "the Edit menu names the step it would undo");
        commands.activate(integrated);
        juce::ApplicationCommandInfo undoInfo(AppCommands::Undo);
        commands.getCommandInfo(AppCommands::Undo, undoInfo);
        check(undoInfo.shortName == "Undo Create track" &&
                  (undoInfo.flags & juce::ApplicationCommandInfo::isDisabled) == 0,
              "and the command's own info carries it");
        check(integrated.keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)),
              "application undo key");
        // Quit (§41.3): every edit is already in the file, so nothing is asked.
        juce::ApplicationCommandInfo quitInfo(AppCommands::Quit);
        commands.getCommandInfo(AppCommands::Quit, quitInfo);
#if JUCE_MAC
        check(quitInfo.shortName == "Quit" && quitInfo.defaultKeypresses.isEmpty() &&
                  integrated.command(AppCommands::Quit) && quits == 1,
              "Quit reaches the application, asking nothing; Cmd+Q is the application menu's");
#else
        check(quitInfo.shortName == "Quit" &&
                  integrated.keyPressed(juce::KeyPress('q', juce::ModifierKeys::commandModifier, 0)) &&
                  quits == 1,
              "Ctrl+Q quits, asking nothing");
#endif
        check(document->session().model().tracks.size() == 1,
              "undo callback updates Session synchronously");
        check(!integrated.command(AppCommands::Undo) && !integrated.error().empty() &&
                  document->session().model().tracks.size() == 1,
              "undo stops at the project's creation and says so");
        check(integrated.command(AppCommands::Redo) &&
                  document->session().model().tracks.size() == 2,
              "redo callback restores engine track");
        document->prepare(48000, 64);
        std::array<float, 64> left{}, right{};
        float *channels[]{left.data(), right.data()};
        engine::AudioIo io;
        io.out = channels;
        io.numOut = 2;
        io.frames = 64;
        check(integrated.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)),
              "application play key");
        document->process(io);
        integrated.frame();
        check(integrated.playing() && document->mailbox().position() == 64,
              "TransportBar reflects actual engine playback");
        check(integrated.command(AppCommands::Stop), "application stop");
        document->process(io);
        integrated.frame();
        check(!integrated.playing() && document->mailbox().position() == 64,
              "engine stop reaches bar");
        const auto opsBeforeSnapshot =
            document->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
        check(integrated.keyPressed(juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0)) &&
                  snapshots == 1,
              "Ctrl/Cmd+S takes a snapshot (ADR-0203 d6), not a save");
        check(document->store().db().execAndGet("SELECT count(*) FROM ops").getInt() ==
                  opsBeforeSnapshot,
              "and writes no op");
        // Save As (Live 12 manual 5.4.1, 41.3): the window at 1440 x 900 points for the PR's
        // picture, the Status Bar along its foot (ADI_UI_PARITY).
        integrated.setSize(1440, 900);
        auto shotIntegrated = [&integrated](const char *name) {
            const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
            if (pictures.isEmpty())
                return;
            juce::Image image(juce::Image::ARGB, 2 * integrated.getWidth(), 2 * integrated.getHeight(),
                              true, juce::SoftwareImageType{});
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(2.f));
                integrated.paintEntireComponent(g, true);
            }
            const auto file = juce::File(pictures).getChildFile(name);
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(image, *stream);
        };
        const auto saveAsKey = juce::KeyPress(
            's', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0);
        integrated.say("a message a success clears");
        check(integrated.keyPressed(saveAsKey) && saveAs == 1 && integrated.error().empty() &&
                  document->name() == "integrated-as" &&
                  std::filesystem::equivalent(document->store().path(), temp.path() / "integrated-as.adi") &&
                  document->session().model().tracks.size() == 2,
              "Ctrl/Cmd+Shift+S saves as: the document carries on in the new file, the Status Bar "
              "cleared (Live 41.3)");
        integrated.frame();
        shotIntegrated("adi-save-as.png");
        check(integrated.keyPressed(saveAsKey) && saveAs == 2 &&
                  integrated.error().find("already exists") != std::string::npos &&
                  document->name() == "integrated-as",
              "the same file again is refused, the Status Bar saying why; nothing moves");
        integrated.frame();
        shotIntegrated("adi-save-as-refused.png");
        integrated.say({});
        OpRequest tempoChange;
        tempoChange.opType = "project.insertTempoEvent";
        tempoChange.payload = {{"pos", 2 * textproj::kPPQ}, {"bpm", 60.0}, {"curve", 0}};
        check(document->ops().submit(tempoChange).ok && document->synchronise(),
              "tempo change for hardware-rate test");
        document->prepare(96000, 64);
        document->session().transport().locate(72000); // offline driver, 0.75 seconds
        document->process(io);
        integrated.frame();
        check(integrated.bpm() == 120, "TransportBar uses granted device rate at tempo boundaries");
        // Step 7.3 (#199): the arrangement, its keys and its one-component canvas.
        auto &arrangement = integrated.arrangement;
        integrated.setSize(1200, 720);
        const auto cmdKey = juce::ModifierKeys::commandModifier;
        arrangement.geometry.selection.track = 2;
        check(integrated.keyPressed(juce::KeyPress('t', cmdKey, 0)),
              "Ctrl/Cmd+T inserts an audio track");
        check(value(document->store(), "SELECT kind||' '||name FROM tracks WHERE id=3") ==
                      "audio # Audio" &&
                  arrangement.geometry.selection.track == 3,
              "after the selected track, numbered by position, and selected");
        if (!integrated.error().empty())
            std::printf("status: %s\n", integrated.error().c_str());
        check(integrated.keyPressed(
                  juce::KeyPress('t', cmdKey | juce::ModifierKeys::shiftModifier, 0)) &&
                  value(document->store(), "SELECT kind FROM tracks WHERE id=4") == "midi",
              "Ctrl/Cmd+Shift+T inserts a MIDI track");
        check(integrated.undoTitle(false) == "Undo Insert MIDI Track",
              "an insertion is one named undo step");
        check(integrated.command(AppCommands::DeleteTrack) &&
                  document->store().db().execAndGet("SELECT count(*) FROM tracks WHERE id=4")
                          .getInt() == 0,
              "Delete Track removes the selected track through its op");
        check(integrated.command(AppCommands::Undo) && document->synchronise(),
              "and undo brings it back");
        integrated.frame();
        check(arrangement.geometry.selection.track == 4 &&
                  document->sessionState().selection().track == 4,
              "and undo selects it again, from the step's sel_before (ADR-0021)");
        integrated.frame();
        // Rename (Ctrl/Cmd+R, the Edit menu; Live 12 manual 18.2).
        check(displayName("#-Audio", 3) == "3-Audio" && displayName("##Drums", 7) == "07Drums" &&
                  displayName("Bass", 2) == "Bass",
              "a name that begins with # shows the track's number, a # per digit");
        arrangement.geometry.selection.track = 2;
        auto &headers = arrangement.headers;
        check(integrated.keyPressed(juce::KeyPress('r', cmdKey, 0)) && headers.renaming() == 2 &&
                  headers.editor.isVisible() && headers.editor.getText() == "Track",
              "Ctrl/Cmd+R edits the selected track's name in its title bar");
        headers.editor.setText("Lead", false);
        check(headers.editor.keyPressed(juce::KeyPress(juce::KeyPress::tabKey)) &&
                  value(document->store(), "SELECT name FROM tracks WHERE id=2") == "Lead" &&
                  headers.renaming() == 3 && headers.editor.getText() == "# Audio",
              "Tab commits and moves on to the next track's title");
        headers.editor.setText("Gone", false);
        check(headers.editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) &&
                  value(document->store(), "SELECT name FROM tracks WHERE id=3") == "# Audio" &&
                  headers.renaming() == 0 && !headers.editor.isVisible(),
              "Esc cancels and leaves the name as it was");
        check(integrated.undoTitle(false) == "Undo Rename Track",
              "a rename is one named undo step");
        check(integrated.command(AppCommands::Undo) &&
                  value(document->store(), "SELECT name FROM tracks WHERE id=2") == "Track",
              "and undo restores the name");
        arrangement.geometry.selection.track = 0;
        check(!integrated.command(AppCommands::RenameTrack) && !integrated.error().empty(),
              "with no track selected, Rename says why");
        integrated.frame();
        OpRequest clip;
        clip.opType = "clip.create";
        clip.payload = {{"id", 1},         {"track", 2},
                        {"kind", "audio"}, {"name", "Rendered clip"},
                        {"pos", 0},        {"length", 4 * textproj::kPPQ}};
        check(document->ops().submit(clip).ok, "arrangement clip created through op");
        integrated.frame();
        const auto *hitTrack = arrangement.geometry.trackAt(integrated.reader(), 4);
        const auto *mainRow = arrangement.geometry.trackAt(integrated.reader(), laneTop(integrated, 1) + 4);
        const auto rowsInOrder = laneOrder(integrated);
        check(mainRow && mainRow->id == 1 && hitTrack && hitTrack->id == 2 && !rowsInOrder.empty() &&
                  rowsInOrder.back() == 1,
              "every track is a row; Main last, as Live draws it");
        check(hitTrack && arrangement.geometry.hit(*hitTrack, 0) &&
                  !arrangement.geometry.hit(*hitTrack, 4 * textproj::kPPQ),
              "hit-test includes start and excludes end");
        // The canvas's accessibility layer (UI-ARCHITECTURE §6): a child per clip on screen.
        integrated.mark(DirtySet::All);
        integrated.frame();
        ClipAccessible *readable = nullptr;
        std::size_t shown = 0;
        for (const auto &child : arrangement.canvas.accessible())
            if (child->isVisible()) {
                ++shown;
                if (child->clip == 1)
                    readable = child.get();
            }
        bool clicksHere = true, clicksBelow = true;
        if (readable)
            readable->getInterceptsMouseClicks(clicksHere, clicksBelow);
        check(readable != nullptr && shown == 1 && !clicksHere && !clicksBelow &&
                  readable->getParentComponent() == &arrangement.canvas,
              "each clip on screen has an accessible child the mouse passes through");
        auto handler = readable ? readable->createAccessibilityHandler() : nullptr;
        check(handler && handler->getRole() == juce::AccessibilityRole::listItem &&
                  handler->getTitle().startsWith("Track, Rendered clip, at bar 1 beat 1") &&
                  handler->getTitle().contains("4.00 beats"),
              "a screen reader hears the clip's track, its name, its start and its length");
        arrangement.geometry.selection.track = 0;
        arrangement.geometry.selection.clip = 0;
        arrangement.changed();
        check(handler && handler->getActions().invoke(juce::AccessibilityActionType::press) &&
                  arrangement.geometry.selection.clip == 1 &&
                  document->sessionState().selection().clip == 1,
              "its press action selects the clip, in the session");
        handler.reset();
#if JUCE_MAC
        const int clipEdgeKey = juce::ModifierKeys::altModifier;
#else
        const int clipEdgeKey = juce::ModifierKeys::ctrlModifier;
#endif
        {
            auto &sel = arrangement.geometry.selection;
            sel.clip = 0;
            sel.insert = sel.start = sel.end = 0;
            sel.marked = true;
        }
        check(arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, clipEdgeKey, 0)) &&
                  arrangement.geometry.selection.insert == 4 * textproj::kPPQ &&
                  arrangement.canvas.lastAnnouncement.contains("Insert Mark 2.1.1"),
              "Ctrl/Option+Right takes the insert marker to the clip's end, and says where (§41.4.1.3)");
        check(arrangement.canvas.keyPressed(juce::KeyPress(
                  juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier | clipEdgeKey, 0)) &&
                  arrangement.geometry.selection.clip == 1 &&
                  arrangement.canvas.lastAnnouncement.contains("Rendered clip, at bar 1 beat 1"),
              "with Shift it selects back to the clip's start, which selects the clip, and says which");
        arrangement.geometry.left = 100;
        arrangement.changed();
        shown = 0;
        for (const auto &child : arrangement.canvas.accessible())
            if (child->isVisible())
                ++shown;
        check(shown == 0, "a clip scrolled off screen has no accessible child");
        arrangement.geometry.left = 0;
        arrangement.changed();
        // Bars in the project's meters (#299): with 3/4 from the start, the control bar shows it and
        // the Status Bar counts in it; undo puts 4/4 back.
        {
            OpRequest sig;
            sig.opType = "project.insertTimeSignature";
            sig.payload = {{"pos", 0}, {"num", 3}, {"den", 4}};
            sig.label = "Insert Time Signature";
            check(document->ops().submit(sig).ok, "3/4 from the start");
            integrated.mark(DirtySet::All);
            integrated.frame();
            auto &sel = arrangement.geometry.selection;
            const auto selWas = sel;
            sel.clip = 0;
            sel.marked = true;
            sel.insert = sel.start = sel.end = 7 * textproj::kPPQ + textproj::kPPQ / 2;
            const auto mark = integrated.selectionText();
            sel.start = 0;
            sel.end = 4 * textproj::kPPQ;
            const auto span = integrated.selectionText();
            check(integrated.transport.signature.text == "3 / 4" && !integrated.transport.signature.automationLed &&
                      mark.contains("Insert Mark 3.2.3") && span.contains("Length: 1.1.0"),
                  "the signature and the positions in the project's meters: 3/4 makes seven and a half quarters 3.2.3; "
                  "no change of signature, so no automation LED");
            sel = selWas;
            check(integrated.command(AppCommands::Undo), "undo the signature");
            integrated.mark(DirtySet::All);
            integrated.frame();
            check(integrated.transport.signature.text == "4 / 4", "and the control bar shows 4/4 again");
        }
        check(arrangement.headers.getX() > arrangement.canvas.getX() &&
                  arrangement.headers.getRight() == arrangement.getWidth() &&
                  arrangement.canvas.getX() == ArrangementView::kLeftMargin,
              "the track headers sit to the right of the clips, as Live's");
        arrangement.geometry.selection.clip = 1;
        arrangement.geometry.selection.start = 0;
        arrangement.geometry.selection.end = 4 * textproj::kPPQ;
        const double beforeScale = arrangement.geometry.scale;
        check(integrated.keyPressed(juce::KeyPress('z')),
              "synthesised Z reaches zoom-to-selection");
        check(arrangement.geometry.scale != beforeScale, "Z changes visible range");
        check(integrated.keyPressed(juce::KeyPress('x')) &&
                  arrangement.geometry.scale == beforeScale,
              "X restores previous zoom");
        const auto middle = arrangement.geometry.anchor(0);
        const auto middleX = arrangement.geometry.x(middle);
        check(integrated.keyPressed(juce::KeyPress('=')) &&
                  arrangement.geometry.scale > beforeScale &&
                  std::abs(arrangement.geometry.x(middle) - middleX) < 1,
              "+ zooms around the selection's middle, which stays put");
        check(integrated.keyPressed(juce::KeyPress('-')) &&
                  std::abs(arrangement.geometry.scale - beforeScale) < 1e-9,
              "- zooms back out around it");
        auto geometry = arrangement.geometry;
        geometry.selection.start = geometry.selection.end = 0;
        geometry.selection.marked = false;
        check(geometry.anchor(777) == 777, "with no selection or marker, the playhead anchors");
        const auto anchor = geometry.tick(70);
        geometry.zoom(2., 70);
        check(std::abs(geometry.tick(70) - anchor) <= 1, "pointer-anchored zoom preserves time");
        check(commands.zoomOnSelection(), "Ctrl/Cmd+wheel zooms around the selection by default");
        check(integrated.keyPressed(juce::KeyPress('w')) && arrangement.geometry.left == 0,
              "W optimises the width");
        check(integrated.keyPressed(juce::KeyPress('h')) && arrangement.geometry.heights.empty(),
              "H optimises the height");
        arrangement.geometry.resizeTrack(0, 64, true);
        arrangement.geometry.selection.insert = 0;
        arrangement.locate();
        document->process(io);
        integrated.frame();
        const auto paints = arrangement.canvasPaints;
        allocations = 0;
        audit = true;
        for (int i = 0; i < 100; ++i) {
            document->mailbox().post(TransportMailbox::Command::Locate, i * 100);
            document->process(io);
            integrated.frame();
        }
        audit = false;
        check(allocations == 0, "moving arrangement playhead frame allocates zero");
        check(arrangement.canvasPaints == paints && arrangement.playhead.getWidth() == 1 &&
                  !arrangement.playhead.hitTest(0, 1),
              "one-pixel playhead never repaints canvas and ignores hits");
        arrangement.geometry.selection.start = arrangement.geometry.selection.end = 0;
        arrangement.geometry.wheelHeight(integrated.reader(), 2, 8);
        check(arrangement.geometry.heightFor(2) == 72 && arrangement.geometry.height == 64,
              "Alt-wheel changes only target track");
        arrangement.changed();
        check(document->views().load("main").laneHeights.at(2) == 72,
              "track height persists in ui_view");
        arrangement.geometry.resizeTrack(2, 64);
        arrangement.changed();
        const auto opsBeforeDrop =
            document->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
        arrangement.canvas.filesDropped(
            juce::StringArray{juce::File::getCurrentWorkingDirectory().getChildFile("no-such-kick.wav").getFullPathName()},
            10, 70);
        check(!integrated.error().empty() &&
                  document->store().db().execAndGet("SELECT count(*) FROM ops").getInt() ==
                      opsBeforeDrop,
              "a drop the window can't import says why and writes nothing");
        {
            UiPeerRoot peerRoot;
            peerRoot.setSize(integrated.getWidth(), integrated.getHeight());
            peerRoot.addAndMakeVisible(integrated);
            peerRoot.setVisible(true);
            peerRoot.addToDesktop(0);
            auto *peer = peerRoot.getPeer();
            check(peer != nullptr, "fake peer created without OS window");
            const auto origin =
                peerRoot.getLocalPoint(&arrangement.canvas, juce::Point<int>{10, laneTop(integrated, 2) + 35})
                    .toFloat();
            juce::int64 when = 100000;
            auto mouse = [&](juce::Point<float> point, int mods) {
                peer->handleMouseEvent(juce::MouseInputSource::InputSourceType::mouse, point,
                                       juce::ModifierKeys(mods), 1.f, 0.f, when += 100);
            };
            arrangement.geometry.selection.clip = 0;
            mouse(origin, 0);
            mouse(origin, juce::ModifierKeys::leftButtonModifier);
            mouse(origin, 0);
            check(arrangement.geometry.selection.clip == 1 && arrangement.geometry.selection.marked,
                  "peer dispatch hit-tests, selects the painted clip and places the marker");
            mouse(origin, juce::ModifierKeys::leftButtonModifier);
            mouse(origin.translated(48, 0), juce::ModifierKeys::leftButtonModifier);
            mouse(origin.translated(48, 0), 0);
            // Live's grid (§6.10): the selection covers whole grid cells while snapping.
            check(arrangement.geometry.grid.snap &&
                      arrangement.geometry.selection.end == arrangement.geometry.snap(arrangement.geometry.tick(58), true) &&
                      arrangement.geometry.selection.end % static_cast<std::int64_t>(
                                                               arrangement.geometry.gridQuarters() * textproj::kPPQ) == 0,
                  "peer drag capture updates time selection, to the grid");
#if JUCE_MAC
            const int bypassKey = juce::ModifierKeys::commandModifier;
#else
            const int bypassKey = juce::ModifierKeys::altModifier;
#endif
            // A mouse event takes its keys from the keyboard's state, not from the event.
            when += 1000;
            juce::ModifierKeys::currentModifiers = juce::ModifierKeys(bypassKey);
            mouse(origin, bypassKey);
            mouse(origin, juce::ModifierKeys::leftButtonModifier | bypassKey);
            mouse(origin.translated(48, 0), juce::ModifierKeys::leftButtonModifier | bypassKey);
            mouse(origin.translated(48, 0), bypassKey);
            juce::ModifierKeys::currentModifiers = juce::ModifierKeys();
            check(arrangement.geometry.selection.end == arrangement.geometry.tick(58),
                  "Alt (Cmd on the Mac) bypasses the grid while dragging");
            // Live's clip editing (§6.7, §6.9): a clip moves by its bar, to the grid; Delete
            // deletes it and 0 deactivates it; each is one undo step.
            {
                auto &geo = arrangement.geometry;
                std::int64_t clipTrack = 0, pos0 = 0;
                int barTop = -geo.scrollY;
                for (const auto row : geo.rows(integrated.reader())) {
                    const auto *tr = row.track;
                    for (const auto &c : tr->clips)
                        if (c->id == 1) {
                            clipTrack = tr->id;
                            pos0 = c->posTicks;
                        }
                    if (clipTrack != 0)
                        break;
                    barTop += geo.heightFor(tr->id);
                }
                auto clipAt = [&]() -> const engine::ClipNode * {
                    if (const auto *tr = integrated.reader().findTrack(clipTrack))
                        for (const auto &c : tr->clips)
                            if (c->id == 1)
                                return c.get();
                    return nullptr;
                };
                const auto bar = peerRoot
                                     .getLocalPoint(&arrangement.canvas,
                                                    juce::Point<int>{static_cast<int>(geo.x(pos0)) + 4, barTop + 5})
                                     .toFloat();
                const auto gridTicks = static_cast<std::int64_t>(geo.gridQuarters() * textproj::kPPQ);
                const auto expected = geo.snap(pos0 + geo.tick(100) - geo.tick(0), false);
                when += 1000;
                mouse(bar, 0);
                mouse(bar, juce::ModifierKeys::leftButtonModifier);
                mouse(bar.translated(100, 0), juce::ModifierKeys::leftButtonModifier);
                const bool previewed = arrangement.canvas.movingClip() == 1 && arrangement.canvas.movingTo() != pos0;
                mouse(bar.translated(100, 0), 0);
                integrated.frame();
                const auto *moved = clipAt();
                check(clipTrack != 0 && previewed && moved != nullptr && moved->posTicks % gridTicks == 0 &&
                          std::llabs(moved->posTicks - expected) <= gridTicks &&
                          geo.selection.start == moved->posTicks && integrated.undoTitle(false) == "Undo Move Clip",
                      "dragging a clip's bar moves it, to the grid, as one undo step named Move Clip");
                const bool movedBack = integrated.command(AppCommands::Undo);
                integrated.frame();
                check(movedBack && clipAt() != nullptr && clipAt()->posTicks == pos0, "and Undo puts it back");
                when += 1000;
                mouse(bar, 0);
                mouse(bar, juce::ModifierKeys::leftButtonModifier);
                mouse(bar, 0);
                juce::Component &clipKeys = arrangement.canvas;
                const bool picked = geo.selection.clip == 1;
                const bool deactivated = clipKeys.keyPressed(juce::KeyPress('0'));
                integrated.frame();
                check(picked && deactivated && clipAt() != nullptr && clipAt()->muted &&
                          integrated.undoTitle(false) == "Undo Deactivate Clip",
                      "0 deactivates the selected clip");
                const bool activated = clipKeys.keyPressed(juce::KeyPress('0'));
                integrated.frame();
                check(activated && clipAt() != nullptr && !clipAt()->muted, "and 0 again activates it");
                const bool deleted = clipKeys.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
                integrated.frame();
                check(deleted && clipAt() == nullptr && geo.selection.clip == 0, "Delete deletes the selected clip");
                const bool restored = integrated.command(AppCommands::Undo);
                integrated.frame();
                check(restored && clipAt() != nullptr && clipAt()->posTicks == pos0, "and Undo brings it back");
                // The right edge changes the clip's length, its start and its audio staying put.
                const auto length0 = clipAt() != nullptr ? clipAt()->lengthTicks : 0;
                const auto rightEdge =
                    peerRoot
                        .getLocalPoint(&arrangement.canvas,
                                       juce::Point<int>{static_cast<int>(geo.x(pos0 + length0)) - 2, barTop + 5})
                        .toFloat();
                when += 1000;
                mouse(rightEdge, 0);
                const bool bracket = arrangement.canvas.getMouseCursor() == juce::MouseCursor::LeftRightResizeCursor;
                mouse(rightEdge, juce::ModifierKeys::leftButtonModifier);
                mouse(rightEdge.translated(100, 0), juce::ModifierKeys::leftButtonModifier);
                const bool stretching =
                    arrangement.canvas.resizingClip() == 1 && arrangement.canvas.resizedLength() > length0;
                mouse(rightEdge.translated(100, 0), 0);
                integrated.frame();
                const auto *longer = clipAt();
                check(bracket && stretching && longer != nullptr && longer->posTicks == pos0 &&
                          longer->lengthTicks > length0 && (pos0 + longer->lengthTicks) % gridTicks == 0 &&
                          geo.selection.end == pos0 + longer->lengthTicks &&
                          integrated.undoTitle(false) == "Undo Resize Clip",
                      "dragging a clip's right edge lengthens it to the grid, its start where it was");
                const bool shortened = integrated.command(AppCommands::Undo);
                integrated.frame();
                check(shortened && clipAt() != nullptr && clipAt()->lengthTicks == length0, "and Undo restores its length");
                // Live's Arrangement keys (online §41.4.1.3, §42.16), each edit one undo step.
                {
                    auto press = [&clipKeys](int code, int mods = 0) {
                        return clipKeys.keyPressed(juce::KeyPress(code, juce::ModifierKeys(mods), 0));
                    };
                    auto &sel = geo.selection;
                    auto point = [&sel](std::int64_t tick) {
                        sel.clip = 0;
                        sel.insert = sel.start = sel.end = tick;
                        sel.marked = true;
                    };
                    const auto step = static_cast<std::int64_t>(geo.gridQuarters() * textproj::kPPQ);
                    const auto end0 = pos0 + length0;
                    arrangement.selectClip(clipTrack, 1);
                    const bool nudged = press(juce::KeyPress::rightKey);
                    integrated.frame();
                    check(nudged && clipAt() != nullptr && clipAt()->posTicks == pos0 + step &&
                              sel.start == pos0 + step && sel.clip == 1 &&
                              integrated.undoTitle(false) == "Undo Move Clip",
                          "Right moves the selected clip to the next grid line, one undo step");
                    const bool unNudged = integrated.command(AppCommands::Undo);
                    integrated.frame();
                    check(unNudged && clipAt() != nullptr && clipAt()->posTicks == pos0, "and Undo puts it back");
                    {
                        // Move Clips with Arrow Keys off (§41.1.1): Right collapses the selection to
                        // its end, Left to its start, and nothing moves.
                        std::string refusal;
                        const bool set = settings.set("editing.arrowKeysMoveClips", false, {}, refusal);
                        commands.reload();
                        arrangement.selectClip(clipTrack, 1);
                        const auto undoBefore = integrated.undoTitle(false);
                        const bool collapsed = press(juce::KeyPress::rightKey) && sel.start == sel.end &&
                                               sel.insert == pos0 + length0 && sel.clip == 0;
                        arrangement.selectClip(clipTrack, 1);
                        const bool collapsedLeft = press(juce::KeyPress::leftKey) && sel.insert == pos0 && sel.clip == 0;
                        integrated.frame();
                        check(set && !commands.arrowKeysMoveClips() && collapsed && collapsedLeft &&
                                  clipAt() != nullptr && clipAt()->posTicks == pos0 &&
                                  integrated.undoTitle(false) == undoBefore,
                              "with Move Clips with Arrow Keys off, Right and Left collapse the selection and move nothing");
                        settings.set("editing.arrowKeysMoveClips", true, {}, refusal);
                        commands.reload();
                    }
                    point(end0);
                    check(press(juce::KeyPress::rightKey) && sel.insert == end0 + step && sel.start == sel.end &&
                              sel.clip == 0,
                          "with no selection, Right moves the insert marker by the grid");
                    check(press(juce::KeyPress::leftKey) && sel.insert == end0,
                          "and Left moves it back");
                    check(press(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier) &&
                              press(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier) &&
                              std::min(sel.start, sel.end) == end0 && std::max(sel.start, sel.end) == end0 + 2 * step &&
                              sel.clip == 0,
                          "Shift+Right selects two grid steps from the insert marker");
                    check(press(juce::KeyPress::rightKey) && std::min(sel.start, sel.end) == end0 + step &&
                              std::max(sel.start, sel.end) == end0 + 3 * step,
                          "and Right moves a selection that holds no clip by the grid");
                    sel.start = pos0;
                    sel.end = pos0 + step;
                    integrated.say({});
                    check(step < length0 && press(juce::KeyPress::rightKey) && sel.start == pos0 &&
                              sel.end == pos0 + step && clipAt() != nullptr && clipAt()->posTicks == pos0 &&
                              integrated.error().find("split") != std::string::npos,
                          "a selection holding part of a clip stays, and the Status Bar says it waits for split");
                    point(pos0);
                    integrated.say({});
                    check(press(juce::KeyPress::returnKey) && !arrangement.canvas.keyResizing() &&
                              integrated.error().find("content offset") != std::string::npos,
                          "Enter at a clip's start says moving its start waits for a content offset");
                    point(end0);
                    const auto undoBefore = integrated.undoTitle(false);
                    check(press(juce::KeyPress::returnKey) && arrangement.canvas.keyResizing() &&
                              press(juce::KeyPress::rightKey) && press(juce::KeyPress::rightKey) &&
                              arrangement.canvas.resizedLength() == length0 + 2 * step,
                          "Enter at a clip's end, then Right, lengthens it by the grid");
                    const bool cancelled = press(juce::KeyPress::escapeKey);
                    integrated.frame();
                    check(cancelled && !arrangement.canvas.keyResizing() && clipAt() != nullptr &&
                              clipAt()->lengthTicks == length0 && integrated.undoTitle(false) == undoBefore,
                          "Esc puts it back and makes no undo step");
                    point(end0);
                    const bool kept = press(juce::KeyPress::returnKey) && press(juce::KeyPress::rightKey) &&
                                      press(juce::KeyPress::returnKey);
                    integrated.frame();
                    check(kept && !arrangement.canvas.keyResizing() && clipAt() != nullptr &&
                              clipAt()->posTicks == pos0 && clipAt()->lengthTicks == length0 + step &&
                              sel.insert == end0 + step && integrated.undoTitle(false) == "Undo Resize Clip",
                          "Enter again keeps the new length, one undo step named Resize Clip");
                    const bool unResized = integrated.command(AppCommands::Undo);
                    integrated.frame();
                    check(unResized && clipAt() != nullptr && clipAt()->lengthTicks == length0,
                          "and Undo restores it");
                }
            }
            when += 1000;
            mouse(origin, 0);
            mouse(origin, juce::ModifierKeys::leftButtonModifier);
            mouse(origin.translated(48, 0), juce::ModifierKeys::leftButtonModifier);
            mouse(origin.translated(48, 0), 0);
            // The Overview (Live 12 manual 6.1, ADR-0129's rows).
            auto &overview = arrangement.overview;
            check(overview.getParentComponent() == &integrated &&
                      overview.getY() > integrated.arrangement.getY() &&
                      overview.getBottom() <=
                          integrated.arrangement.getY() + ArrangementView::kOverviewHeight &&
                      overview.getX() == integrated.arrangement.getX() + arrangement.canvas.getX() &&
                      overview.getRight() <= integrated.arrangement.getX() + arrangement.canvas.getRight() &&
                      overview.getRight() >= integrated.arrangement.getX() + arrangement.canvas.getRight() - 8 &&
                      arrangement.ruler.getY() == ArrangementView::kOverviewHeight &&
                      arrangement.canvas.getY() == ArrangementView::kTopHeight,
                  "the Overview sits above the beat-time ruler, over the lanes as the Arrangement sheet draws it");
            arrangement.geometry.left = 1;
            arrangement.geometry.scale = 48;
            arrangement.changed();
            const auto outline = overview.outline();
            const auto overviewPoint =
                peerRoot
                    .getLocalPoint(&overview,
                                   juce::Point<int>{static_cast<int>(outline.getCentreX()), 8})
                    .toFloat();
            const double leftBefore = arrangement.geometry.left;
            const double perPixel = static_cast<double>(overview.span()) / textproj::kPPQ /
                                    overview.getWidth();
            mouse(overviewPoint, 0);
            mouse(overviewPoint, juce::ModifierKeys::leftButtonModifier);
            mouse(overviewPoint.translated(10, 0), juce::ModifierKeys::leftButtonModifier);
            mouse(overviewPoint.translated(10, 0), 0);
            check(std::abs(arrangement.geometry.left - leftBefore - 10 * perPixel) < 1e-6,
                  "dragging the Overview sideways scrolls by the Overview's own scale");
            const double scaleBefore = arrangement.geometry.scale;
            mouse(overviewPoint, juce::ModifierKeys::leftButtonModifier);
            mouse(overviewPoint.translated(0, -20), juce::ModifierKeys::leftButtonModifier);
            mouse(overviewPoint.translated(0, -20), 0);
            check(arrangement.geometry.scale > scaleBefore, "dragging it up zooms in");
            when += 1000;
            for (int click = 0; click < 2; ++click) {
                mouse(overviewPoint, juce::ModifierKeys::leftButtonModifier);
                mouse(overviewPoint, 0);
            }
            check(arrangement.geometry.left == 0 &&
                      std::abs(arrangement.geometry.x(arrangement.geometry.end(
                                   integrated.reader())) -
                               arrangement.canvas.getWidth()) < 1,
                  "a double-click zooms out to the whole arrangement");
            // The scrub area (Live 12 manual 6.1, p. 161): a click plays from that point.
            check(arrangement.scrub.getY() ==
                          ArrangementView::kOverviewHeight + ArrangementView::kRulerHeight &&
                      arrangement.canvas.getY() == ArrangementView::kTopHeight,
                  "the scrub area sits between the ruler and the lanes");
            check(!document->mailbox().desiredPlaying(), "the transport is stopped");
            const auto scrubPoint =
                peerRoot.getLocalPoint(&arrangement.scrub, juce::Point<int>{100, 16}).toFloat(); // under the brace
            const auto target = arrangement.geometry.tick(100);
            when += 1000;
            mouse(scrubPoint, juce::ModifierKeys::leftButtonModifier);
            mouse(scrubPoint, 0);
            document->process(io);
            const auto expected = static_cast<std::int64_t>(
                integrated.current()->tempo->ticksToSeconds(target) *
                document->mailbox().sampleRate());
            check(document->mailbox().playing() &&
                      std::llabs(document->mailbox().position() - expected - io.frames) <= 1,
                  "a click there starts playback from that point");
            const auto later = arrangement.geometry.tick(300);
            check(integrated.playFrom(later), "a click while playing");
            document->process(io);
            const auto jumped = static_cast<std::int64_t>(
                integrated.current()->tempo->ticksToSeconds(later) *
                document->mailbox().sampleRate());
            check(document->mailbox().playing() &&
                      std::llabs(document->mailbox().position() - jumped - io.frames) <= 1,
                  "jumps there and keeps playing");
            // Live's Follow (§6.2): the arrangement keeps the playhead on screen while it plays.
            const auto viewBefore = arrangement.geometry;
            check(!integrated.state().follow && integrated.command(AppCommands::Follow) &&
                      integrated.state().follow && document->views().load("main").follow,
                  "Follow switches on, kept with the window");
            integrated.frame();
            check(integrated.transport.follow.getToggleState(), "the control bar's Follow is lit");
            const int lanes = arrangement.canvas.getWidth();
            auto playAt = [&](std::int64_t tick) {
                integrated.playFrom(tick);
                document->process(io);
                integrated.frame();
            };
            auto playheadX = [&] { return arrangement.geometry.x(integrated.timelineTick()); };
            playAt(arrangement.geometry.tick(lanes + 200));
            check(playheadX() >= 0 && playheadX() < 2,
                  "by page: past the right edge, the view jumps to put the playhead at the left");
            const auto rulerPoint =
                peerRoot.getLocalPoint(&arrangement.ruler, juce::Point<int>{50, 10}).toFloat();
            when += 1000;
            mouse(rulerPoint, 0);
            mouse(rulerPoint, juce::ModifierKeys::leftButtonModifier);
            mouse(rulerPoint, 0);
            const double pausedAt = arrangement.geometry.left;
            playAt(arrangement.geometry.tick(lanes + 200));
            check(arrangement.followPaused && arrangement.geometry.left == pausedAt && playheadX() > lanes,
                  "a click on the beat-time ruler pauses it");
            const auto scrubEnd =
                peerRoot.getLocalPoint(&arrangement.scrub, juce::Point<int>{lanes - 40, 7}).toFloat();
            when += 1000;
            mouse(scrubEnd, 0);
            mouse(scrubEnd, juce::ModifierKeys::leftButtonModifier);
            mouse(scrubEnd, 0);
            document->process(io);
            integrated.frame();
            playAt(arrangement.geometry.tick(lanes + 200));
            check(!arrangement.followPaused && playheadX() >= 0 && playheadX() < 2,
                  "a click in the scrub area starts it again");
            check(settings.set("lookfeel.followMode", "scroll", {}, why), "Follow by scrolling");
            commands.reload();
            playAt(arrangement.geometry.tick(lanes + 200));
            check(std::abs(playheadX() - lanes / 2.) < 2, "by scrolling: the playhead stays in the middle");
            check(settings.set("lookfeel.followMode", "page", {}, why), "and by page again");
            commands.reload();
            const auto selectionBefore = arrangement.geometry.selection;
            check(integrated.command(AppCommands::InsertAudioTrack) && arrangement.followPaused,
                  "an edit pauses it");
            integrated.command(AppCommands::DeleteTrack);
            arrangement.geometry.selection = selectionBefore;
            integrated.publishSelection();
#if JUCE_MAC
            const juce::KeyPress followKey('f', juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier, 0);
#else
            const juce::KeyPress followKey('f', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0);
            check(commands.manager.getKeyMappings()->findCommandForKeyPress(juce::KeyPress(
                      'f', juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier, 0)) ==
                      AppCommands::Follow,
                  "Alt+Shift+F too, the other key the manual gives Windows");
#endif
            check(integrated.keyPressed(followKey) && !integrated.state().follow,
                  "Follow's key switches it off");
            check(integrated.command(AppCommands::Stop), "stop");
            document->process(io);
            integrated.frame();
            check(!arrangement.followPaused, "stopping playback starts Follow again, for when it is on");
            // §42.16's keys that need no op: the insert marker to the playhead, and Alt/Option
            // + and - on the selected track's height.
            check(integrated.timelineTick() > 0 &&
                      integrated.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey, cmd | juce::ModifierKeys::shiftModifier, 0)) &&
                      arrangement.geometry.selection.insert == integrated.timelineTick() &&
                      arrangement.geometry.selection.marked && arrangement.geometry.selection.clip == 0 &&
                      arrangement.geometry.selection.start == arrangement.geometry.selection.end,
                  "Ctrl/Cmd+Shift+Space puts the insert marker where the playhead is");
            const auto heightTrack = laneOrder(integrated).front();
            arrangement.geometry.selection.track = heightTrack;
            const int heightBefore = arrangement.geometry.heightFor(heightTrack);
            check(integrated.keyPressed(juce::KeyPress('=', alt, 0)) &&
                      arrangement.geometry.heightFor(heightTrack) == heightBefore + ArrangementView::kHeightStep &&
                      document->views().load("main").laneHeights.at(heightTrack) ==
                          heightBefore + ArrangementView::kHeightStep,
                  "Alt/Option + makes the selected track taller, kept with the window");
            check(integrated.keyPressed(juce::KeyPress('-', alt, 0)) &&
                      arrangement.geometry.heightFor(heightTrack) == heightBefore,
                  "and Alt/Option - shorter again");
            arrangement.geometry.selection = selectionBefore;
            integrated.publishSelection();
            arrangement.geometry.left = viewBefore.left;
            arrangement.geometry.scale = viewBefore.scale;
            arrangement.changed();
            peerRoot.removeChildComponent(&integrated);
            peerRoot.removeFromDesktop();
        }
        integrated.frame();
        reference(integrated, "arrangement", write);
        // Step 7.4 (#203): the device strip, fed by one publication, in Live's shape.
        auto publication = std::make_shared<ParameterPublication>();
        for (int device = 0; device < 2; ++device) {
            DevicePanelData panelData;
            panelData.id = 11 + device;
            panelData.track = 2;
            panelData.name = device ? "Second device" : "Test device";
            for (int i = 0; i < (device ? 2 : 5); ++i) {
                panel::Record record;
                record.id = std::to_string(i);
                record.name = "Control " + record.id;
                record.playing = .5;
                record.stored = .25;
                record.shape = static_cast<panel::Shape>(i % 3);
                record.stepCount = i % 3 == 1 ? 2 : 4;
                record.text = "Plugin value";
                panelData.records.push_back(record);
                panelData.declared.push_back({record.id, record.name, true});
                panelData.resolved.entries.push_back({record.id, false});
            }
            panel::finalize(panelData.records);
            publication->push_back(panelData);
        }
        integrated.devices.publication = [publication] { return publication; };
        int gestures = 0;
        integrated.devices.gesture = [&](auto, const auto &, auto, double) {
            ++gestures;
            return true;
        };
        arrangement.geometry.selection.track = 2;
        integrated.frame();
        check(integrated.devices.panels.size() == 2 &&
                  integrated.devices.panels[0]->controls.size() == 5,
              "the selected track's chain is presented, every resolved parameter");
        auto &controls = integrated.devices.panels[0]->controls;
        check(controls[0]->slider.isVisible() && controls[1]->toggle.isVisible() &&
                  controls[2]->menu.isVisible(),
              "continuous, switch and menu use distinct controls");
        controls[1]->toggle.setToggleState(true, juce::dontSendNotification);
        controls[1]->toggle.onClick();
        check(gestures == 3, "a switch sends one bounded begin/value/end gesture");
        check(integrated.devices.getHeight() == DesktopDefaults{}.deviceFloor - 2 * look::border &&
                  controls[1]->getX() == controls[0]->getX() &&
                  controls[1]->getY() > controls[0]->getY() &&
                  controls[3]->getX() > controls[0]->getX(),
              "at the floor the controls run in columns that fit the strip's height");
        const auto &panels = integrated.devices.panels;
        check(panels[1]->getX() > panels[0]->getRight() && panels[0]->getY() == 0 &&
                  panels[1]->getY() == 0,
              "devices run left to right, as Live's Device View");
        integrated.devices.fold(11);
        check(document->views().load("main").foldedDevices.contains(11) &&
                  panels[0]->getWidth() == DevicePanelView::kFoldedWidth,
              "a folded device is a narrow bar, remembered outside the history");
        integrated.devices.fold(11);
        // The title bar's menu in Live's order, and Rename typed in the title bar (§23.2.1).
        {
            auto &first = *integrated.devices.panels[0];
            juce::StringArray items;
            std::function<void()> renameAction;
            const auto menu = first.commands();
            for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                if (!it.getItem().isSeparator) {
                    items.add(it.getItem().text);
                    if (it.getItem().text == "Rename")
                        renameAction = it.getItem().action;
                }
            check(items == juce::StringArray{"Cut", "Copy", "Duplicate", "Rename", "Delete", "Fold", "Configure"},
                  "a device's menu in Live 12's order (23.2.1's figure): Cut, Copy and Duplicate; Rename; Delete and "
                  "Fold; then the device's own");
            std::string renameOp;
            Payload renamed;
            integrated.devices.submit = [&](const std::string &op, Payload p) {
                renameOp = op;
                renamed = std::move(p);
                return true;
            };
            if (renameAction)
                renameAction();
            check(first.rename.isVisible() && first.rename.getText() == "Test device" &&
                      first.rename.getBounds() == first.title.getBounds(),
                  "Rename edits the name in the title bar");
            first.rename.setText("Kick", false);
            juce::Component &editor = first.rename;
            check(editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)) &&
                      renameOp == "device.rename" && renamed.value("id", 0) == 11 &&
                      renamed.value("name", std::string{}) == "Kick" && !first.rename.isVisible(),
                  "Enter renames the device, one op");
            renameOp.clear();
            first.beginRename();
            first.rename.setText("Snare", false);
            const bool escaped = editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
            first.beginRename();
            first.rename.setText("  ", false);
            const bool emptied = editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
            check(escaped && emptied && renameOp.empty() && !first.rename.isVisible(),
                  "Esc keeps the name, and so does an empty one");
            integrated.devices.fold(11);
            check(!first.beginRename() && !first.rename.isVisible(), "a folded device has no title to rename");
            integrated.devices.fold(11);
            UiPeerRoot deviceHost;
            deviceHost.setSize(integrated.getWidth(), integrated.getHeight());
            deviceHost.addAndMakeVisible(integrated);
            deviceHost.setVisible(true);
            deviceHost.addToDesktop(0);
            first.grabKeyboardFocus();
            const bool focused = first.hasKeyboardFocus(true);
            check(focused && integrated.command(AppCommands::RenameTrack) && first.rename.isVisible() &&
                      first.rename.hasKeyboardFocus(false) && !integrated.arrangement.headers.editor.isVisible(),
                  "the Edit menu's Rename renames the device with the focus, not the track");
            editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
            // Live's Navigate menu (§41.4.1): Alt, or Option, and a view's number moves the focus.
            check(integrated.keyPressed(juce::KeyPress('4', alt, 0)) && panels[0]->hasKeyboardFocus(false),
                  "Alt/Option+4 moves the focus to the Device View's first device");
            juce::Component &device = *panels[0];
            check(device.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) &&
                      panels[1]->hasKeyboardFocus(false),
                  "the right arrow moves to the next device (§41.4.1.5)");
            juce::Component &second = *panels[1];
            check(second.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)) &&
                      device.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)) &&
                      panels[0]->hasKeyboardFocus(false),
                  "the left arrow moves back, and stays at the first device");
            check(integrated.keyPressed(juce::KeyPress('2', alt, 0)) &&
                      integrated.arrangement.canvas.hasKeyboardFocus(false),
                  "Alt/Option+2 moves the focus to the Arrangement View");
            auto &bar = integrated.transport;
            check(integrated.keyPressed(juce::KeyPress('0', alt, 0)) && bar.link.hasKeyboardFocus(false) &&
                      bar.link.focusRing(),
                  "Alt/Option+0 moves the focus to the Control Bar's first control, ringed");
            bar.stop.grabKeyboardFocus();
            integrated.command(AppCommands::FocusArrangement);
            check(!bar.stop.focusRing() && integrated.command(AppCommands::FocusControlBar) &&
                      bar.stop.hasKeyboardFocus(false) && !bar.link.focusRing(),
                  "the Control Bar's focus returns to the control that last had it");
            auto &browserPanel = integrated.browser;
            check(integrated.keyPressed(juce::KeyPress('5', alt, 0)) &&
                      browserPanel.hasKeyboardFocus(false) && browserPanel.focusRing(),
                  "Alt/Option+5 moves the focus to the browser, ringed");
            juce::Component &browserKeys = browserPanel;
            const juce::KeyPress down(juce::KeyPress::downKey), up(juce::KeyPress::upKey);
            browserKeys.keyPressed(down);
            const int favorites = browserPanel.selected();
            browserKeys.keyPressed(down);
            const int sounds = browserPanel.selected();
            browserKeys.keyPressed(up);
            const int back = browserPanel.selected();
            browserKeys.keyPressed(up);
            check(favorites == 1 && sounds == 4 && back == 1 && browserPanel.selected() == 1 &&
                      integrated.error() == "Favorites is not listed yet.",
                  "the arrows move between the sidebar's labels, past the headings");
            integrated.command(AppCommands::ToggleDeviceView);
            check(!integrated.state().docked && integrated.command(AppCommands::FocusDeviceView) &&
                      integrated.state().docked && panels[0]->hasKeyboardFocus(false),
                  "a hidden Device View is shown, then focused");
            integrated.command(AppCommands::ToggleBrowser);
            check(!integrated.browser.isVisible() && integrated.command(AppCommands::FocusBrowser) &&
                      integrated.browser.isVisible() && browserPanel.hasKeyboardFocus(false),
                  "a hidden browser is shown, then focused");
            // Ctrl/Cmd+F, from anywhere (§41.4.1.6): the browser's search, ready to type over.
            integrated.arrangement.canvas.grabKeyboardFocus();
            integrated.command(AppCommands::ToggleBrowser);
            browserPanel.search.setText("drums", false);
            check(integrated.keyPressed(juce::KeyPress('f', cmd, 0)) && integrated.browser.isVisible() &&
                      browserPanel.search.hasKeyboardFocus(false) &&
                      browserPanel.search.getHighlightedText() == "drums",
                  "Ctrl/Cmd+F shows the browser and puts the focus in its search, its words selected");
            browserPanel.search.clear();
            // Use Tab Key to Move Focus (§41.1.1), through the peer's own key path: off, Live's
            // default, Tab moves nothing; on, it moves the focus on.
            auto *peer = deviceHost.getPeer();
            auto &canvas = integrated.arrangement.canvas;
            const juce::KeyPress tab(juce::KeyPress::tabKey);
            canvas.grabKeyboardFocus();
            check(!commands.tabMovesFocus() && peer != nullptr && peer->handleKeyPress(tab) &&
                      canvas.hasKeyboardFocus(false),
                  "with Use Tab Key to Move Focus off, Tab moves nothing");
            check(integrated.command(AppCommands::TabMovesFocus) && commands.tabMovesFocus() &&
                      settings.get("lookfeel.tabMovesFocus") == true && peer != nullptr &&
                      peer->handleKeyPress(tab) && !canvas.hasKeyboardFocus(false),
                  "switched on from the Navigate menu, it is the setting, and Tab moves the focus on");
            // Wrap Tab Navigation (§41.1.1), on by default: Tab from the last control goes round
            // to the first; off, it stops there, and Shift+Tab stops at the first.
            {
                auto *container = canvas.findKeyboardFocusContainer();
                auto traverser = canvas.createKeyboardFocusTraverser();
                std::vector<juce::Component *> all;
                if (container != nullptr && traverser != nullptr)
                    all = traverser->getAllComponents(container);
                const juce::KeyPress backTab(juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0);
                bool wrapped = false, stays = false, staysBack = false, off = false;
                if (all.size() > 1 && peer != nullptr && integrated.isParentOf(all.back()) &&
                    integrated.isParentOf(all.front())) {
                    all.back()->grabKeyboardFocus();
                    wrapped = commands.wrapTabNavigation() && peer->handleKeyPress(tab) &&
                              all.front()->hasKeyboardFocus(false);
                    off = integrated.command(AppCommands::WrapTabNavigation) && !commands.wrapTabNavigation() &&
                          settings.get("lookfeel.wrapTabNavigation") == false;
                    all.back()->grabKeyboardFocus();
                    stays = peer->handleKeyPress(tab) && all.back()->hasKeyboardFocus(false);
                    all.front()->grabKeyboardFocus();
                    staysBack = peer->handleKeyPress(backTab) && all.front()->hasKeyboardFocus(false);
                    integrated.command(AppCommands::WrapTabNavigation);
                }
                check(wrapped, "with Wrap Tab Navigation on, Live's default, Tab from the last control goes to the first");
                check(off && stays && staysBack,
                      "switched off from the Navigate menu, Tab stops at the last control, Shift+Tab at the first");
                check(commands.wrapTabNavigation(), "and on again");
            }
            check(integrated.command(AppCommands::TabMovesFocus) && !commands.tabMovesFocus(),
                  "and off again");
            // The Mixer's keyboard: Move Focus to Mixer (§42.16), then Next and Previous
            // Neighbor (§41.4.1.2) to the same switch on the track after or before. The selection
            // stays as it is: the device strip, and `controls` below, follow it.
            {
                const auto rows = laneOrder(integrated);
                const auto selected = integrated.arrangement.geometry.selection.track;
                std::size_t at = 0;
                for (std::size_t i = 0; i < rows.size(); ++i)
                    if (rows[i] == selected)
                        at = i;
                const bool forward = at + 1 < rows.size();
                const std::size_t there = forward ? at + 1 : (at > 0 ? at - 1 : at);
                auto focusedRow = [&]() -> MixerStrip * {
                    for (const auto &s : integrated.mixer.strips())
                        for (auto *control : s->switches())
                            if (control->hasKeyboardFocus(false))
                                return s.get();
                    return nullptr;
                };
#if JUCE_MAC
                const auto neighbourMods = juce::ModifierKeys::altModifier;
#else
                const auto neighbourMods = juce::ModifierKeys::commandModifier;
#endif
                const juce::KeyPress nextKey(juce::KeyPress::tabKey, neighbourMods, 0),
                    previousKey(juce::KeyPress::tabKey, neighbourMods | juce::ModifierKeys::shiftModifier, 0);
                const juce::KeyPress mixerKey('m', alt | juce::ModifierKeys::shiftModifier, 0);
                check(integrated.keyPressed(mixerKey) && focusedRow() != nullptr &&
                          focusedRow()->track() == rows[at] && focusedRow()->active.hasKeyboardFocus(false),
                      "Alt/Option+Shift+M moves the focus to the selected track's Track Activator");
                check(rows.size() >= 2 && integrated.keyPressed(forward ? nextKey : previousKey) &&
                          focusedRow() != nullptr && focusedRow()->track() == rows[there] &&
                          focusedRow()->active.hasKeyboardFocus(false),
                      "a Neighbor key: the same switch on the next or previous track");
                if (auto *row = focusedRow())
                    row->solo.grabKeyboardFocus();
                check(integrated.keyPressed(forward ? previousKey : nextKey) && focusedRow() != nullptr &&
                          focusedRow()->track() == rows[at] && focusedRow()->solo.hasKeyboardFocus(false),
                      "and the other one back, on the switch that had the focus");
                check(focusedRow() != nullptr && !focusedRow()->solo.getMouseClickGrabsKeyboardFocus(),
                      "a click on a mixer switch leaves the focus where it was");
                integrated.command(AppCommands::ToggleMixer);
                check(!integrated.mixer.isVisible() && integrated.keyPressed(mixerKey) &&
                          integrated.mixer.isVisible() && focusedRow() != nullptr,
                      "hidden track controls are shown, then focused");
                // Rows taller than the view: a neighbour out of view is scrolled into view, as
                // Live's, from the first track to the last and back.
                auto &lanes = integrated.arrangement.geometry;
                const auto heightsWas = lanes.heights;
                const int scrollWas = lanes.scrollY;
                for (const auto row : rows)
                    lanes.resizeTrack(row, 400);
                integrated.arrangement.changed();
                integrated.frame();
                for (std::size_t k = 0; k < rows.size(); ++k)
                    integrated.keyPressed(previousKey);
                const bool reachedFirst = focusedRow() != nullptr && focusedRow()->track() == rows.front() &&
                                          lanes.scrollY == 0;
                for (std::size_t k = 0; k < rows.size(); ++k)
                    integrated.keyPressed(nextKey);
                // The last track with an activator: Main has none, as Live's, and the focus passes it by.
                std::int64_t lastWithActivator = 0;
                for (const auto row : rows)
                    if (const auto *t = integrated.reader().findTrack(row); t != nullptr && t->kind != "master")
                        lastWithActivator = row;
                check(rows.size() >= 2 && reachedFirst && focusedRow() != nullptr &&
                          focusedRow()->track() == lastWithActivator && lanes.scrollY > 0 &&
                          focusedRow()->getY() >= ArrangementView::kTopHeight,
                      "a neighbour out of view is scrolled into view, its row's top showing; Main, with no activator, is passed by");
                lanes.heights = heightsWas;
                lanes.scrollY = scrollWas;
                integrated.arrangement.changed();
                integrated.frame();
            }
            deviceHost.removeChildComponent(&integrated);
            deviceHost.removeFromDesktop();
            integrated.devices.submit = nullptr;
        }
        integrated.resizeDevices(1);
        check(integrated.state().deviceHeight == DesktopDefaults{}.deviceFloor,
              "the strip's floor comes from the desktop defaults");
        integrated.resizeDevices(10000);
        check(integrated.arrangement.getHeight() >=
                  ArrangementView::kTopHeight + integrated.state().laneHeight,
              "the strip's ceiling leaves the ruler and a row");
        integrated.resizeDevices(260);
        check(integrated.devices.getHeight() == 260 - 2 * look::border &&
                  controls[3]->getX() == controls[0]->getX(),
              "a taller strip holds more rows, so fewer columns");
        integrated.frame();
        reference(integrated, "devices", write);
        allocations = 0;
        audit = true;
        for (int i = 0; i < 50; ++i)
            integrated.frame();
        audit = false;
        check(allocations == 0, "unchanged parameter publication frame allocates zero");
        arrangement.geometry.selection.track = 1;
        integrated.frame();
        check(integrated.devices.panels.empty(),
              "selection changes strip even with same parameter publication");
        check(!document->parameterGesture(99, "0", engine::ParamEventKind::Value, .5),
              "a gesture for no device is refused");
        // Step 7.5 (#204): the floating-window host, built before anything needs two.
        {
            FloatingState saved;
            saved.monitor = "disconnected";
            saved.x = 3200;
            saved.y = -800;
            saved.width = 1400;
            saved.height = 900;
            check(restoreFloating(saved, {{"primary", {0, 0, 1000, 700}}}) ==
                      juce::Rectangle<int>(0, 0, 1000, 700),
                  "lost monitor restores fully on available screen");
            saved.monitor = "left";
            saved.x = -1600;
            saved.y = 100;
            saved.width = 400;
            saved.height = 300;
            check(restoreFloating(
                      saved, {{"primary", {0, 0, 1000, 700}}, {"left", {-1920, 0, 1920, 1080}}})
                          .getX() == -1600,
                  "monitor identity preserves negative desktop coordinates");
            check(settings.set("lookfeel.showTooltips", true, {}, why), "tooltips back on");
            commands.reload();
            bool alive = true;
            juce::Component dock, content;
            dock.addAndMakeVisible(content);
            content.setBounds(10, 20, 300, 180);
            FloatingHost floating(
                integrated, commands, document->views(), document->view(),
                [&](std::int64_t) { return alive; }, false);
            check(floating.detach("test", 11, content, dock), "detach existing component");
            check(floating.content("test") == &content && content.getParentComponent() != &dock,
                  "detach reparents same identity");
            check(floating.hasTooltips("test"), "a floating window has its own tooltips");
            floating.frame("test");
            floating.frame("test");
            check(floating.drains("test") == 2, "window drains on its own clock");
            floating.close("test");
            check(content.getParentComponent() == &dock &&
                      content.getBounds() == juce::Rectangle<int>(10, 20, 300, 180),
                  "dock restores exact component and bounds");
            check(!document->views().loadFloating("test").open, "close clears saved open state");
            floating.detach("deleted", 11, content, dock);
            alive = false;
            floating.frame("deleted");
            floating.collect();
            check(floating.count() == 0 && !document->views().loadFloating("deleted").open,
                  "deleting the device closes its window and remembers it closed");
            alive = true;
            floating.collect();
            check(floating.count() == 0, "undo restores the device, not the window");
            auto tap = std::make_shared<engine::ScopeTap>(48000, 1.);
            std::array<float, 1024> sine{};
            for (std::size_t i = 0; i < sine.size(); ++i)
                sine[i] = static_cast<float>(
                    std::sin(2 * 3.141592653589793 * 32 * static_cast<double>(i) / 1024.));
            tap->write(sine.data(), sine.data(), 1024, 0, true);
            auto model = std::make_shared<AnalyserModel>(tap);
            model->analyseOnce(); // explicit diagnostic demand; no private worker or timer
            tap->write(sine.data(), sine.data(), 1024, 1024, true);
            model->analyseOnce();
            AnalyserView small(model);
            auto big = std::make_unique<AnalyserView>(model);
            auto *bigPtr = big.get();
            check(floating.openView("analyser", 11, std::move(big),
                                [bigPtr](const SnapshotReader &) { bigPtr->frame(); }),
                  "a second view opens through the generic host");
            check(small.model() == bigPtr->model(),
                  "the docked and the big view share one measurement model");
            std::array<float, 513> bins{};
            std::uint64_t revision = 0;
            model->read(bins, revision);
            check(revision > 0 && bins[100] < -90 && std::abs(bins[32]) < .01,
                  "shared analyzer explicit pass calibrates the 0 dB sine without autonomous polling");
            const std::string analyserKey = "analyser";
            floating.frame(analyserKey);
            allocations = 0;
            audit = true;
            for (int i = 0; i < 20; ++i)
                floating.frame(analyserKey);
            audit = false;
            check(allocations == 0, "floating analyser frame allocates zero");
            // Each window's own interface zoom (ADR-0129 d3): Ctrl/Cmd + and - in a floating
            // view's window zoom that window, never the main one.
            const float mainZoom = integrated.interfaceZoom();
            auto *floatWindow = bigPtr->getTopLevelComponent();
            auto *windowKeys = dynamic_cast<juce::KeyListener *>(floatWindow);
            const auto zoomKey = juce::ModifierKeys::commandModifier;
            const bool zoomedIn = windowKeys != nullptr &&
                                  windowKeys->keyPressed(juce::KeyPress('=', zoomKey, 0), floatWindow);
            const auto *frameOf = bigPtr->getParentComponent();
            check(zoomedIn && floating.zoomOf(analyserKey) == 1.25f && integrated.interfaceZoom() == mainZoom &&
                      juce::approximatelyEqual(bigPtr->getTransform().mat00, 1.25f) && frameOf != nullptr &&
                      bigPtr->getWidth() == juce::roundToInt(static_cast<float>(frameOf->getWidth()) / 1.25f),
                  "Ctrl/Cmd + zooms the floating window it is pressed in, not the main one");
            if (windowKeys != nullptr) {
                windowKeys->keyPressed(juce::KeyPress('-', zoomKey, 0), floatWindow);
                windowKeys->keyPressed(juce::KeyPress('-', zoomKey, 0), floatWindow);
            }
            check(floating.zoomOf(analyserKey) == .75f && integrated.interfaceZoom() == mainZoom,
                  "and Ctrl/Cmd - zooms it out");
            for (int i = 0; i < 4; ++i)
                floating.zoom(analyserKey, false);
            check(!floating.zoom(analyserKey, false) && floating.zoomOf(analyserKey) == .5f,
                  "between Live's 50 % and 200 %");
            const bool infoShown = commands.infoView();
            const auto infoKey = juce::KeyPress('/', juce::ModifierKeys::shiftModifier, 0);
            const bool toggled = windowKeys != nullptr && windowKeys->keyPressed(infoKey, floatWindow) &&
                                 commands.infoView() != infoShown;
            check(toggled && windowKeys->keyPressed(infoKey, floatWindow) && commands.infoView() == infoShown,
                  "other keys in it are still the main window's");
            const auto floatDisplay = displayAt(floatWindow->getBounds().getCentre());
            check(commands.keptZoom("floating:analyser", floatDisplay) == .5f,
                  "a floating view's zoom is kept for its display (window.zoom, N4)");
            floating.close("analyser");
            auto again = std::make_unique<AnalyserView>(model);
            check(floating.openView("analyser", 11, std::move(again)) && floating.zoomOf(analyserKey) == .5f,
                  "and it opens again at that zoom");
            floating.close("analyser");
            const auto selectedBeforeAnalysis = integrated.arrangement.geometry.selection.track;
            check(showAnalyzerWindow(floating, integrated, integrated.analysisTrack()) &&
                      integrated.arrangement.geometry.selection.track == selectedBeforeAnalysis,
                  "opening the default Main analyzer preserves the selected editing channel");
            auto *followingAnalysis = dynamic_cast<AnalysisPane *>(floating.content("Analysis"));
            check(followingAnalysis && followingAnalysis->source() == AnalysisPane::Source::Main,
                  "the project-owned analyzer window starts with explicit Main source");
            const auto oldSessionSelection = integrated.session().selection();
            auto detachedSelection = oldSessionSelection;
            for (const auto &track : integrated.reader().tracks())
                if (track->id != oldSessionSelection.track) { detachedSelection.track = track->id; break; }
            check(integrated.session().select(detachedSelection, why), "another window can update the shared channel selection");
            if (followingAnalysis) {
                followingAnalysis->setSource(AnalysisPane::Source::Selected);
                floating.frame("Analysis");
            }
            check(followingAnalysis && followingAnalysis->view.track() == detachedSelection.track,
                  "a detached analyzer follows shared selection without ticking the main window");
            check(integrated.session().select(oldSessionSelection, why), "restore the session selection after detached analyzer test");
            floating.close("Analysis");
        }
        integrated.devices.publication = {};
        arrangement.geometry.selection.track = 2;
        integrated.frame();
        integrated.resizeDevices(169);
        // Step 7.6 (#205): the mixer, its rows in the arrangement's lanes. The meters read the
        // engine's scalar per strip (UI-ARCHITECTURE §9; N9, #381; worklist B3) through the
        // panel's lookup: fixed values here, track 2 at a half-scale sine's peak and RMS, the
        // others silent, so nothing plays and no tap is opened. The clock is the test's.
        const float halfSineRms = std::sqrt(.125f); // .5 / sqrt 2
        engine::StripMeter fixed, other;
        fixed.peak.store(.5f);
        fixed.rms.store(halfSineRms);
        other.peak.store(.25f);
        other.rms.store(.1f);
        const engine::StripMeter *meterOfTwo = &fixed;
        int lookups = 0;
        double clock = 0;
        auto &mixer = integrated.mixer;
        mixer.meter = [&](std::int64_t id) {
            ++lookups;
            return id == 2 ? meterOfTwo : nullptr;
        };
        mixer.clock = [&] { return clock; };
        mixer.resolveMeters();
        integrated.resized();
        integrated.mark(DirtySet::All);
        integrated.frame();
        MixerStrip *row = nullptr;
        for (const auto &s : mixer.strips())
            if (s->isVisible() && s->track() == 2)
                row = s.get();
        check(row != nullptr && mixer.getX() == integrated.arrangement.getRight(),
              "every visible track has a mixer row, beside the headers");
        const int lane = integrated.arrangement.getY() + integrated.arrangement.canvas.getY() + laneTop(integrated, 2);
        check(row && mixer.getY() + row->getY() == lane &&
                  row->getHeight() == arrangement.geometry.heightFor(2),
              "a mixer row sits level with its track's lane, as Live's track controls");
        if (row) {
            MixerStrip *unmetered = nullptr;
            for (const auto &s : mixer.strips())
                if (s->isVisible() && s->track() != 2 && unmetered == nullptr)
                    unmetered = s.get();
            check(row->hasMeter() && row->peak() == .5f && row->rms() == halfSineRms && unmetered != nullptr &&
                      !unmetered->hasMeter() && unmetered->peak() == 0 && unmetered->rms() == 0,
                  "the meter draws the engine's peak and RMS for its strip; a track with no strip yet draws silence");
            // A refresh finds the meter again (a track's strip arrives with the session's next sync,
            // after the row was bound): what the lookup now gives is drawn, five seconds on so the
            // ballistics have let the old values go.
            const int lookupsBefore = lookups;
            meterOfTwo = &other;
            clock = 5;
            integrated.mark(DirtySet::Layout);
            integrated.frame();
            check(lookups > lookupsBefore && row->track() == 2 && row->peak() == .25f && row->rms() == .1f,
                  "after a refresh the meter is resolved again");
            meterOfTwo = &fixed;
            integrated.mark(DirtySet::Layout);
            integrated.frame();
            check(row->peak() == .5f && row->rms() == halfSineRms, "and again, back to the first");
            // The ballistics, the window's (win's placement until Readings M1-02 to M1-04): the
            // engine's value gone, the level falls 24 dB a second; the peak line holds a second first.
            fixed.peak.store(0);
            fixed.rms.store(0);
            clock = 5.5;
            integrated.frame();
            const float twelveDown = std::pow(10.f, -12.f / 20);
            check(std::abs(row->rms() - halfSineRms * twelveDown) < .002f && row->peak() == .5f,
                  "half a second of silence: the level is 12 dB down, the peak line still held");
            clock = 6.5;
            integrated.frame();
            check(std::abs(row->peak() - .5f * twelveDown) < .002f && row->rms() < .01f,
                  "a second and a half: the line, let go after its second, is 12 dB down");
            clock = 15;
            integrated.frame();
            check(row->peak() == 0 && row->rms() == 0, "ten seconds on: the meter is at rest");
            fixed.peak.store(.5f);
            fixed.rms.store(halfSineRms);
            integrated.frame();
            check(row->peak() == .5f && row->rms() == halfSineRms, "a new value is drawn at once: the rise is instant");
            row->volume.onDragStart();
            row->volume.setValue(-6, juce::sendNotificationSync);
            check(std::abs(document->view().current()->findTrack(2)->volumeDb) < .01,
                  "nothing is written while the fader moves");
            row->volume.onDragEnd();
            integrated.frame();
            check(std::abs(document->view().current()->findTrack(2)->volumeDb + 6) < .01 &&
                      integrated.undoTitle(false) == "Undo Change Track Volume",
                  "the fader's gesture commits as one named undo step");
            check(integrated.command(AppCommands::Undo) &&
                      std::abs(document->view().current()->findTrack(2)->volumeDb) < .01,
                  "and undo restores it");
            integrated.frame();
            // The bottom stop reads -inf and writes silence (-150 dB, SPEC §6.9), not the -70 dB
            // it stands on, which is quiet but heard.
            row->volume.setValue(row->volume.getMinimum(), juce::sendNotificationSync);
            integrated.frame();
            check(document->view().current()->findTrack(2)->volumeDb <= -150.0 &&
                      row->volume.getTextFromValue(row->volume.getValue()) == "-inf",
                  "the fader's bottom stop writes silence and reads -inf");
            check(integrated.command(AppCommands::Undo) &&
                      std::abs(document->view().current()->findTrack(2)->volumeDb) < .01,
                  "and undo restores 0 dB");
            integrated.frame();
            check(row->pan.isDoubleClickReturnEnabled() &&
                      row->pan.getDoubleClickReturnValue() == 0,
                  "a double-click resets the pan to centre");
            row->pan.setValue(.4, juce::sendNotificationSync);
            check(std::abs(document->view().current()->findTrack(2)->pan - .4) < .01,
                  "a typed or keyboard pan commits at once");
            row->pan.setValue(0, juce::sendNotificationSync);
            row->active.onClick();
            integrated.frame();
            const auto lanesNow = laneOrder(integrated);
            const auto place = std::find(lanesNow.begin(), lanesNow.end(), 2) - lanesNow.begin() + 1;
            check(document->view().current()->findTrack(2)->muted && !row->active.getToggleState() &&
                      row->active.getButtonText() == juce::String(static_cast<int>(place)),
                  "the Track Activator, numbered by its place as Live's, mutes the track");
            row->active.onClick();
            row->solo.onClick();
            integrated.frame();
            check(document->view().current()->findTrack(2)->soloed, "Solo solos it");
            row->solo.onClick();
            // The Mix View's STACK on a channel is the command's, with the track selected: no strip
            // has a button of its own for it (ADR-0243).
            const auto opsBefore =
                document->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            const bool mixOpen = integrated.state().mixView;
            integrated.arrangement.geometry.selection.track = 2;
            integrated.publishSelection();
            check(row->switches().size() == 3 && integrated.command(AppCommands::ShowChannelEq) &&
                      document->store().db().execAndGet("SELECT count(*) FROM ops").getInt() ==
                          opsBefore,
                  "a strip has the Track Activator, Solo and Arm, and no button for a Mix View function");
            check(integrated.state().mixView && integrated.mixViewPanel.stack.focused() == 2,
                  "the command opens the Mix View's STACK on the selected channel (ADR-0209 d11)");
            if (!mixOpen)
                integrated.command(AppCommands::ToggleMixView);
        }
        for (int i = 0; i < 30; ++i)
            check(integrated.command(AppCommands::InsertAudioTrack), "many tracks");
        integrated.frame();
        integrated.mark(DirtySet::All);
        integrated.frame();
        std::size_t visibleRows = 0;
        for (const auto &s : mixer.strips())
            if (s->isVisible())
                ++visibleRows;
        check(mixer.strips().size() < integrated.reader().tracks().size() &&
                  visibleRows <= static_cast<std::size_t>(mixer.getHeight() / 24 + 2),
              "the mixer realises the visible rows only");
        reference(integrated, "mixer", write);
        allocations = 0;
        audit = true;
        for (int i = 0; i < 50; ++i)
            integrated.frame();
        audit = false;
        check(allocations == 0, "a steady mixer frame, meters included, allocates zero");
        // Live's Mixer View (manual 6.1 p. 162, 18.1; §41.1): vertical strips, left to right.
        // STACK keeps every channel's actual chain in model order, while realizing only the
        // viewport plus retained interactions. The withdrawn reserved-slot runtime is not used.
        {
            const auto selectionBeforeStack = integrated.arrangement.geometry.selection;
            StackTab eqView(integrated);
            eqView.channelScope.setSelectedId(2, juce::sendNotificationSync);
            eqView.setSize(900, 460);
            SnapshotReader eqReader(document->view().current());
            eqView.frame(eqReader);
            const auto all = laneOrder(integrated);
            bool ordered = !all.empty() && eqView.model().shownCount() == all.size();
            for (std::size_t i = 0; ordered && i < all.size(); ++i)
                ordered = eqView.model().shownTrack(i) == all[i];
            check(ordered, "STACK lists every channel, in the arrangement's order");
            check(eqView.model().moduleKindCount() == 0 && !eqView.rows().empty() && eqView.rows().size() < all.size(),
                  "actual chains use no reserved slot columns and realize a bounded viewport");
            allocations = 0;
            audit = true;
            eqView.frame(eqReader);
            audit = false;
            check(allocations == 0, "a frame that changes no channel builds nothing");
            if (eqView.rows().size() >= 2) {
                auto &second = *eqView.rows()[1];
                const auto shownModelTrack = *eqView.model().shownIndex(second.track());
                const auto geo = eqView.model().row(shownModelTrack);
                const auto chip = eqView.model().chip(shownModelTrack).chip;
                const auto at = juce::Point<float>(static_cast<float>(chip.x + chip.w / 2),
                                                   static_cast<float>(chip.y - geo.top + chip.h / 2));
                second.mouseDown(pressAt(second, at));
                eqView.mouseUp(pressAt(second, at));
                check(eqView.focused() == second.track() && arrangement.geometry.selection.track == second.track() &&
                          eqView.model().moduleKindCount() == 0,
                      "a model chip selects its channel without activating any reserved processor");
            }
            MixerStrip *strip = nullptr;
            for (const auto &s : mixer.strips())
                if (s->isVisible() && s->track() != integrated.arrangement.geometry.selection.track)
                    strip = s.get();
            if (strip) {
                const auto selectedBefore = integrated.arrangement.geometry.selection.track;
                const bool mixWas = integrated.state().mixView;
                integrated.arrangement.geometry.selection.track = strip->track();
                integrated.publishSelection();
                check(integrated.command(AppCommands::ShowChannelEq) && integrated.state().mixView &&
                          integrated.state().mixViewTab == 1 &&
                          integrated.mixViewPanel.stack.focused() == strip->track(),
                      "with another track selected the command opens the Mix View on STACK, on its row");
                if (!mixWas)
                    integrated.command(AppCommands::ToggleMixView);
                integrated.arrangement.geometry.selection.track = selectedBefore;
                integrated.publishSelection();
            }
            // Keep this independent selection exercise out of the later Mixer View fixture.
            integrated.arrangement.geometry.selection = selectionBeforeStack;
            integrated.publishSelection();
        }
        auto &mixerView = integrated.mixerView;
        mixerView.meter = mixer.meter;
        mixerView.clock = mixer.clock;
        const int arrangementBefore = integrated.arrangement.getHeight();
        check(!integrated.state().mixerView && !mixerView.isVisible(),
              "the Mixer View starts hidden");
        check(integrated.keyPressed(
                  juce::KeyPress('m', cmdKey | juce::ModifierKeys::altModifier, 0)) &&
                  mixerView.isVisible() &&
                  integrated.arrangement.getHeight() < arrangementBefore &&
                  mixerView.getBottom() == integrated.arrangement.getBottom() +
                                               integrated.mixerViewArea().getHeight() &&
                  integrated.status.mixerToggle.getToggleState(),
              "Ctrl/Cmd+Alt+M opens it at the foot of the arrangement");
        check(document->views().load("main").mixerView, "and the window remembers it");
        integrated.mark(DirtySet::All);
        integrated.frame();
        std::vector<MixerStrip *> columns;
        for (const auto &strip : mixerView.strips())
            if (strip->isVisible())
                columns.push_back(strip.get());
        const auto trackOrder = laneOrder(integrated);
        check(columns.size() >= 2 && columns.size() < trackOrder.size() &&
                  columns[0]->track() == trackOrder[0] &&
                  columns[1]->track() == trackOrder[1] &&
                  columns[1]->getX() == columns[0]->getX() + MixerPanel::kStripWidth,
              "one strip per track, left to right, only those on screen");
        bool numbered = true;
        for (std::size_t i = 0; i < columns.size(); ++i)
            numbered = numbered &&
                       columns[i]->active.getButtonText() == juce::String(static_cast<int>(i) + 1);
        check(numbered, "each activator shows its track's place, whatever the track's id");
        // Live's peak level (manual 18.1): the highest peak since a click, in dB.
        // Track 2 sounds (its tap reads a half-scale sine); the others are silent.
        MixerStrip *sounding = nullptr, *silent = nullptr;
        for (auto *column : columns) {
            if (column->track() == 2)
                sounding = column;
            else if (silent == nullptr)
                silent = column;
        }
        check(sounding != nullptr && silent != nullptr, "a sounding and a silent strip in view");
        if (sounding != nullptr && silent != nullptr) {
            auto &held = sounding->peakHold;
            check(held.isVisible() && std::abs(held.held - .5f) < .01f,
                  "the Mixer View's peak level holds the track's highest peak");
            held.onClick();
            check(held.held == 0, "a click resets it");
            integrated.frame();
            check(std::abs(held.held - .5f) < .01f, "and it holds the next peak it meets");
            check(!silent->peakHold.isVisible() || silent->peakHold.held == 0,
                  "a silent track's peak level stays at -Inf");
            // Over 0 dBFS the Peak Level turns red (manual 18.1: "resettable peak level indicators";
            // Live's goes red once the track has clipped): read in the pill's own pixels, away from
            // its text.
            fixed.peak.store(1.2f);
            integrated.frame();
            juce::Image pill(juce::Image::ARGB, held.getWidth(), held.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics g(pill);
                held.paintEntireComponent(g, true);
            }
            const auto edge = pill.getPixelAt(3, held.getHeight() / 2);
            check(held.held == 1.2f && sounding->peak() == 1.2f && edge.getRed() > 200 && edge.getGreen() < 100 &&
                      edge.getBlue() < 100,
                  "a peak over 0 dBFS turns the Peak Level red");
            // The window at twice its size, for the PR's picture (ADI_UI_PARITY): the strips with
            // 3 Audio's meter sounding and its Peak Level red.
            if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); pictures.isNotEmpty()) {
                juce::Image shot(juce::Image::ARGB, 2 * integrated.getWidth(), 2 * integrated.getHeight(), true,
                                 juce::SoftwareImageType{});
                {
                    juce::Graphics g(shot);
                    g.addTransform(juce::AffineTransform::scale(2.f));
                    integrated.paintEntireComponent(g, true);
                }
                const auto file = juce::File(pictures).getChildFile("adi-strip-meters.png");
                file.deleteFile();
                if (auto stream = file.createOutputStream())
                    juce::PNGImageFormat{}.writeImageToStream(shot, *stream);
            }
            fixed.peak.store(.5f);
            held.onClick();
            clock = 20;
            integrated.frame();
            check(held.held == .5f && sounding->peak() == .5f && sounding->rms() == halfSineRms,
                  "reset and ten seconds on, the strip is back at the engine's value");
        }
        check(std::abs(meterDepth(0) - .06) < 1e-9 && std::abs(meterLevel(meterDepth(-24)) + 24) < 1e-9 &&
                  !columns.empty() &&
                  std::abs(columns[0]->volume.valueToProportionOfLength(0) - (1 - meterDepth(0))) < 1e-9,
              "the fader and the meter share Live's scale");
        check(!columns.empty() &&
                  columns[0]->volume.getSliderStyle() == juce::Slider::LinearVertical &&
                  columns[0]->getHeight() < MixerPanel::kTallHeight &&
                  columns[0]->volume.getTextBoxPosition() == juce::Slider::NoTextBox,
              "a vertical fader, and no numeric field while the strips are short");
        if (columns.size() >= 2) {
            auto &column = *columns[1];
            column.volume.onDragStart();
            column.volume.setValue(-3, juce::sendNotificationSync);
            column.volume.onDragEnd();
            check(std::abs(document->view().current()->findTrack(column.track())->volumeDb + 3) <
                      .01,
                  "its fader edits the track, as the track controls' does");
            check(integrated.command(AppCommands::Undo), "undo it");
        }
        integrated.resizeMixerView(320);
        integrated.frame();
        check(!columns.empty() && columns[0]->getHeight() >= MixerPanel::kTallHeight &&
                  columns[0]->volume.getTextBoxPosition() == juce::Slider::TextBoxBelow,
              "dragged taller, the strips show a numeric volume field");
        integrated.resizeMixerView(10);
        check(integrated.state().mixerViewHeight == 120, "and never go below their floor");
        integrated.resizeMixerView(220);
        integrated.frame();
        allocations = 0;
        audit = true;
        for (int i = 0; i < 20; ++i)
            integrated.frame();
        audit = false;
        check(allocations == 0, "a steady frame with the Mixer View open allocates zero");
        reference(integrated, "mixer-view", write);
        integrated.status.mixerToggle.onClick();
        check(!mixerView.isVisible() && !document->views().load("main").mixerView,
              "the Status Bar's corner toggle hides it again");
        document->release();
    }
    // SRF-05: the MIDI Ports table at the Tempo & MIDI page's head, in place of the record's own row, over
    // the application's port host (on a machine with no MIDI, it lists none and says so).
    {
        MidiPortHost portHost([&settings] { return settings.get("midi.ports"); });
        SettingsPanel panel(settings);
        panel.setSize(560, 680);
        panel.midiPorts = [&]() -> std::unique_ptr<juce::Component> { return std::make_unique<MidiPortsTable>(portHost, settings); };
        panel.showPage("Tempo & MIDI");
        auto *table = dynamic_cast<MidiPortsTable *>(panel.midiPortsTable());
        const bool recordRow = std::any_of(panel.rows().begin(), panel.rows().end(), [](const auto &row) {
            return row->setting.key.rfind("midi.port", 0) == 0;
        });
        check(table != nullptr && table->isVisible() && !recordRow && table->rows().size() == portHost.ports().size(),
              "Tempo & MIDI shows the MIDI Ports table, one row per port the system lists, not the record's text or the per-port rows");
        std::size_t inputs = 0;
        for (const auto &p : portHost.ports())
            inputs += p.input ? 1 : 0;
        check(inputs == 0 || portHost.ports().front().input, "inputs are listed first");
        panel.showPage("Audio");
        check(table != nullptr && !table->isVisible(), "the table shows on its own page only");
        check(portHost.drain({}) == 0 && portHost.dropped() == 0, "with nothing played, nothing to drain");
    }
    {
        // Two ports, as a machine with a keyboard and a synth lists them: the switches write the record.
        std::string refused;
        check(settings.set("midi.ports", settings::Value::object(), {}, refused), "no port records");
        MidiPortHost::Port keys, synth;
        keys.id = "in:port-1";
        keys.name = "Keystation 61";
        keys.input = true;
        synth.id = "out:port-1"; // a two-way port: one identifier, a record per direction
        synth.name = "Synth";
        synth.output = true;
        MidiPortHost portHost([&settings] { return settings.get("midi.ports"); }, {synth, keys});
        int changes = 0;
        portHost.onChange = [&] { ++changes; };
        MidiPortsTable table(portHost, settings);
        std::vector<std::function<void()>> posted;
        table.post = [&posted](std::function<void()> job) { posted.push_back(std::move(job)); };
        table.setSize(520, 200);
        check(table.rows().size() == 2 && table.rows()[0]->id == "in:port-1" && table.rows()[0]->input &&
                  table.rows()[0]->name->getText() == "Input: Keystation 61" &&
                  table.rows()[1]->name->getText() == "Output: Synth",
              "a row per port and direction, inputs first, as Live's table");
        auto &track = *table.rows()[0]->switches[0];
        check(track.getToggleState() == midiPortDefault("track"), "Track shows the row's default");
        track.setToggleState(!track.getToggleState(), juce::dontSendNotification);
        track.onClick();
        check(posted.size() == 1, "the write waits for the click to return");
        for (auto &job : posted)
            job();
        check(midiPortSwitches(settings.get("midi.ports"), "in:port-1").track != midiPortDefault("track") &&
                  midiPortSwitches(settings.get("midi.ports"), "out:port-1").track == midiPortDefault("track") && changes == 1 &&
                  portHost.ports()[0].switches.track != midiPortDefault("track") && table.error().empty(),
              "a click on Track writes the port's record at once, and the host follows");
        table.rebuild();
        check(table.rows()[0]->switches[0]->getToggleState() != midiPortDefault("track"), "the table shows what is kept");
        check(settings.set("midi.ports", settings::Value::object(), {}, refused), "the records cleared again");
    }
    // The Settings window in Live's shape (ADR-0203 d11, Master Reference V.1).
    {
        SettingsPanel panel(settings);
        panel.setSize(560, 680);
        const auto &pages = panel.pageNames();
        bool liveFirst = pages.size() > kLiveTabs.size();
        for (std::size_t i = 0; liveFirst && i < kLiveTabs.size(); ++i)
            liveFirst = pages[i] == kLiveTabs[i];
        check(liveFirst && panel.page() == "Display & Input",
              "Live's ten tabs in Live's order, ADI's own pages after them; it opens on Display & Input");
        // Every setting on exactly one tab; the tabs are the registry's settings, rearranged.
        std::size_t placed = 0;
        bool once = true;
        for (const auto &tab : settingsTabs())
            for (const auto &group : tab.groups)
                for (const auto *setting : group.settings) {
                    ++placed;
                    once = once && tabOf(*setting) == tab.name;
                }
        std::size_t paged = 0;
        for (const auto &s : settings::registry())
            paged += s.page.empty() ? std::size_t{0} : std::size_t{1}; // internal preferences (DEV-23) have no page
        check(once && placed == paged, "every setting is on one tab, once");
        {
            // Live's sections in Live's order (N14), then ADI's own settings on the tab, in the
            // tab's own group: Live 12 manual 2.3.1 lists neither Zoom on Selection, the tooltips
            // nor the knob modes in its sections.
            const auto *display = settingsTab("Display & Input");
            std::vector<std::string> headings;
            for (const auto &group : display->groups)
                headings.push_back(group.heading);
            const std::vector<std::string> live{"Display", "Display Options", "Navigation and Keyboard",
                                                "Mouse and Pen Input", "Display & Input"};
            const auto &own = display->groups.back().settings;
            auto ownHas = [&own](const char *key) {
                return std::find(own.begin(), own.end(), settings::find(key)) != own.end();
            };
            check(headings == live && ownHas("lookfeel.zoomOnSelection") && ownHas("lookfeel.showTooltips") &&
                      ownHas("lookfeel.tooltipDelayMs") && ownHas("lookfeel.knobMode") &&
                      ownHas("lookfeel.sliderMode") && ownHas("lookfeel.valueBoxMode"),
                  "Display & Input: Live's four sections, then ADI's own (Zoom on Selection, tooltips, knob modes)");
            bool ownLast = true;
            // (On Tempo & MIDI the tab's own group holds Live's Master Focus Dial, chase and filter
            // switches, which the registry gives no section, so it keeps their place in Live's order.)
            for (const char *name : {"Theme & Colors", "Audio", "File & Folder", "Plug-Ins"}) {
                const auto &groups = settingsTab(name)->groups;
                for (std::size_t i = 0; i + 1 < groups.size(); ++i)
                    ownLast = ownLast && groups[i].heading != name;
            }
            const auto *licences = settingsTab("Licenses & Updates");
            check(ownLast && settingsTab("Link")->groups.front().heading == "Link" &&
                      licences->groups.front().heading == "Licenses & Updates",
                  "where a tab's own group is ADI's alone it follows Live's sections; one holding Live's settings "
                  "(Link, Licenses & Updates) keeps its place");
        }
        auto onTab = [](const char *key) {
            const auto *setting = settings::find(key);
            return setting != nullptr ? tabOf(*setting) : std::string{};
        };
        check(onTab("lookfeel.language") == "Display & Input" && onTab("lookfeel.theme") == "Theme & Colors" &&
                  onTab("sync.ptpEnabled") == "Link" && onTab("project.sampleRate") == "Audio" &&
                  onTab("audio.keepMonitoringLatency") == "Record, Warp & Launch",
              "a setting Live has sits on the tab where Live has it: Look & Feel split, Link's own");
        const auto *linkTab = settingsTab("Link");
        check(linkTab != nullptr && linkTab->groups.size() == 3 && linkTab->groups[0].heading == "Link" &&
                  linkTab->groups[0].settings.size() == 2 && linkTab->groups[1].heading == "PTP" &&
                  linkTab->groups[2].heading == "LAN Audio (not Link Audio)" && onTab("sync.lanCodec") == "Link",
              "the Link tab: Live's two settings under Link, then PTP, then the LAN audio node, labelled as not Link Audio");
        const auto *licenses = settingsTab("Licenses & Updates");
        check(licenses != nullptr && licenses->groups.size() == 2 && licenses->groups[0].heading == "Licenses & Updates" &&
                  licenses->groups[1].heading == "Privacy",
              "Updates and Privacy become Licenses & Updates, with a Privacy group");
        const auto *editing = settingsTab("Editing");
        check(editing != nullptr && !editing->live &&
                  std::find(pages.begin(), pages.end(), "Editing") - pages.begin() >=
                      static_cast<std::ptrdiff_t>(kLiveTabs.size()),
              "ADI's own pages come after the ten");
        std::size_t onAudio = 0;
        for (const auto &group : settingsTab("Audio")->groups)
            onAudio += group.settings.size();
        panel.showPage("Audio");
        check(panel.page() == "Audio" && panel.rows().size() == onAudio && panel.audioDevices() == nullptr,
              "one page at a time, every setting on its tab");
        {
            // A page's head holds the rows Live has among its own controls (the Audio page's Driver
            // Error Compensation among the latencies); the panel keeps them, out of its list.
            struct Head final : juce::Component, SettingsPageHead {
                bool hold(const std::string &key, SettingRow &row) override {
                    if (key != "audio.driverErrorCompensationMs")
                        return false;
                    addAndMakeVisible(row);
                    return true;
                }
            };
            SettingsPanel headed(settings, [] {
                auto head = std::make_unique<Head>();
                head->setSize(500, 120);
                return head;
            });
            headed.setSize(560, 680);
            headed.showPage("Audio");
            const SettingRow *compensation = nullptr;
            bool othersInList = true;
            for (const auto &row : headed.rows()) {
                if (row->setting.key == "audio.driverErrorCompensationMs")
                    compensation = row.get();
                else if (row->getParentComponent() == headed.audioDevices())
                    othersInList = false;
            }
            check(compensation != nullptr && compensation->getParentComponent() == headed.audioDevices() && othersInList &&
                      compensation->isVisible(),
                  "the head holds Driver Error Compensation's row; the panel keeps it, the rest stay in the list");
            headed.showPage(pages.front() != "Audio" ? pages.front() : pages.back());
            check(!headed.rows().empty() && headed.page() != "Audio" &&
                      std::none_of(headed.rows().begin(), headed.rows().end(),
                               [](const auto &row) { return row->setting.key == "audio.driverErrorCompensationMs"; }),
                  "another page lets it go");
        }
        {
            // The Audio page's own head, over a device manager with no device open: Live's three
            // sections are its own, and the registry's other settings in them stand there, after
            // Live's lines (N14); the panel draws no second heading for them.
            juce::AudioDeviceManager devices;
            AudioDevicePage *page = nullptr;
            SettingsPanel audio(settings, [&page, &devices, &settings] {
                auto head = std::make_unique<AudioDevicePage>(devices, settings);
                page = head.get();
                return head;
            });
            audio.setSize(560, 680);
            audio.showPage("Audio");
            std::vector<juce::String> panelHeadings;
            std::function<void(juce::Component &)> collect = [&](juce::Component &c) {
                for (auto *child : c.getChildren())
                    if (child != page) {
                        if (dynamic_cast<SettingsHeading *>(child) != nullptr)
                            panelHeadings.push_back(child->getTitle());
                        collect(*child);
                    }
            };
            collect(audio);
            bool heldInPage = page != nullptr;
            for (const auto &row : audio.rows())
                if (row->setting.key == "audio.channelConfig")
                    heldInPage = heldInPage && row->getParentComponent() == nullptr;
                else if (std::find(AudioDevicePage::kSections.begin(), AudioDevicePage::kSections.end(),
                                   row->setting.section) != AudioDevicePage::kSections.end())
                    heldInPage = heldInPage && row->getParentComponent() == page && row->isVisible();
            check(heldInPage && panelHeadings == std::vector<juce::String>{"Test", "Audio"},
                  "the Audio page's head holds every row of Audio Device, Sample Rate and Latency; the panel heads "
                  "Test, then ADI's own");
            auto y = [page](const char *name) { return page != nullptr ? page->lineY(name) : -1; };
            check(page != nullptr && 0 == y("Audio Device") && y("Audio Device") < y("Driver Type") &&
                      y("Driver Type") < y("Audio Output Device") &&
                      y("Audio Output Device") < y("Channel Configuration") &&
                      y("Channel Configuration") < y("Sample Rate") && y("Sample Rate") < y("In/Out Sample Rate") &&
                      y("In/Out Sample Rate") < y("Default SR & Pitch Conversion") &&
                      y("Default SR & Pitch Conversion") < y("Latency") && y("Latency") < y("Buffer Size") &&
                      y("Output Latency") < y("Driver Error Compensation") &&
                      y("Driver Error Compensation") < y("Overall Latency") &&
                      page->getHeight() > y("Overall Latency"),
                  "Live's lines first in each section, the held rows after them; Driver Error Compensation "
                  "between Output and Overall Latency");
            const auto compensation = std::find_if(audio.rows().begin(), audio.rows().end(), [](const auto &row) {
                return row->setting.key == "audio.driverErrorCompensationMs";
            });
            check(compensation != audio.rows().end() && (*compensation)->getY() == y("Driver Error Compensation"),
                  "a held row stands at its line");
            // The two pages at twice their size, for the PR's pictures (ADI_UI_PARITY).
            if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
                pictures.isNotEmpty())
                for (const char *tab : {"Audio", "Display & Input"}) {
                    audio.showPage(tab);
                    juce::Image image(juce::Image::ARGB, 2 * audio.getWidth(), 2 * audio.getHeight(), true,
                                      juce::SoftwareImageType{});
                    {
                        juce::Graphics g(image);
                        g.addTransform(juce::AffineTransform::scale(2.f));
                        audio.paintEntireComponent(g, true);
                    }
                    const auto file = juce::File(pictures).getChildFile(
                        juce::String("settings-sections-") + (tab[0] == 'A' ? "audio" : "display") + ".png");
                    file.deleteFile();
                    if (auto stream = file.createOutputStream())
                        juce::PNGImageFormat{}.writeImageToStream(image, *stream);
                }
            audio.showPage("Display & Input");
            audio.showPage("Audio");
            check(page->lineY("Driver Error Compensation") > 0 &&
                      page->lineY("Overall Latency") - page->lineY("Driver Error Compensation") == SettingRow::kHeight,
                  "shown again, the page holds the new rows only, once each");
            // Live's Channel Configuration (N20; Live 12 manual 17.2): its Input Config and Output
            // Config buttons on their line, in place of the record's field; off with no device open.
            check(page->channels.getY() == page->lineY("Channel Configuration") + 3 && page->channels.isVisible() &&
                      !page->channels.input.isEnabled() && !page->channels.output.isEnabled() &&
                      page->configPanel() == nullptr,
                  "Channel Configuration is Live's Input Config and Output Config buttons, off with no device");
        }
        {
            // The dialogs (N20; Live 12 manual 17.2): a device's mono channels and stereo pairs, each
            // a switch and a name; OK hands the record back, Cancel leaves it.
            using settings::ChannelConfig;
            const auto start = settings::defaultChannelConfig(4, 2);
            ChannelConfigPanel dialog(true, start, "Interface");
            std::vector<std::string> labels;
            std::vector<bool> on;
            for (const auto &line : dialog.lines()) {
                labels.push_back(line->toggle.getButtonText().toStdString());
                on.push_back(line->toggle.getToggleState());
            }
            check(dialog.getTitle() == "Input Config" &&
                      labels == std::vector<std::string>{"1", "2", "3", "4", "1/2", "3/4"} &&
                      on == std::vector<bool>{true, true, false, false, true, false} &&
                      dialog.lines()[4]->name.getTextToShowWhenEmpty() == "1/2 (stereo)",
                  "Input Config lists the mono inputs, then the stereo pairs, Live's default on, Live's names shown");
            auto &three = *dialog.lines()[2];
            three.toggle.setToggleState(true, juce::dontSendNotification);
            three.toggle.onClick();
            auto &pair = *dialog.lines()[4];
            pair.name.setText("Vox", false);
            pair.name.onTextChange();
            std::optional<ChannelConfig> handed;
            bool cancelled = false;
            dialog.onOk = [&handed](const ChannelConfig &c) { handed = c; };
            dialog.onCancel = [&cancelled] { cancelled = true; };
            dialog.ok.onClick();
            check(handed && handed->monoInputs[2].on && handed->stereoInputs[0].name == "Vox" &&
                      handed->monoOutputs == start.monoOutputs && handed->activeInputs() == std::set<int>{1, 2, 3},
                  "a switch and a name edited; OK hands back the record with the outputs as they were");
            dialog.cancel.onClick();
            check(cancelled, "Cancel leaves it");
            // The dialog at twice its size, for the director's first look (ADI_UI_PARITY).
            if (const auto pictures = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
                pictures.isNotEmpty()) {
                ChannelConfigPanel shown(true, settings::defaultChannelConfig(8, 2), "Interface");
                juce::Image image(juce::Image::ARGB, 2 * shown.getWidth(), 2 * shown.getHeight(), true,
                                  juce::SoftwareImageType{});
                {
                    juce::Graphics g(image);
                    g.addTransform(juce::AffineTransform::scale(2.f));
                    shown.paintEntireComponent(g, true);
                }
                const auto file = juce::File(pictures).getChildFile("channel-config-input.png");
                file.deleteFile();
                if (auto stream = file.createOutputStream())
                    juce::PNGImageFormat{}.writeImageToStream(image, *stream);
            }
            ChannelConfigPanel outputs(false, start, "Interface");
            check(outputs.getTitle() == "Output Config" && outputs.lines().size() == 3,
                  "Output Config lists the device's outputs: two mono, one pair");
            // Kept with the device, one side at a time; the device opens with the channels in use.
            std::string refused;
            check(channelConfigFor(settings, "Interface", 4, 2) == start &&
                      keepChannelConfig(settings, "Interface", true, *handed, refused),
                  "no record kept: Live's default; the edited inputs are kept");
            auto edited = start;
            edited.monoOutputs[1].on = false;
            edited.monoInputs[0].on = false; // not this dialog's side: left as kept
            settings::AppSettings reread(appdata::App::Daw, settings.file());
            check(keepChannelConfig(settings, "Interface", false, edited, refused) &&
                      channelConfigFor(settings, "Interface", 4, 2).monoInputs == handed->monoInputs &&
                      !channelConfigFor(settings, "Interface", 4, 2).monoOutputs[1].on &&
                      settings::channelConfig(reread, "Interface").has_value(),
                  "the output side kept beside the inputs, each from its own dialog, saved at once");
            const auto bits = channelBits(channelConfigFor(settings, "Interface", 4, 2).activeInputs(), 4);
            check(bits[0] && bits[1] && bits[2] && !bits[3] && channelBits({1, 2, 5}, 4).countNumberOfSetBits() == 2,
                  "the record's channels in use are the device's channel bits, none beyond the device");
        }
        auto rowFor = [&panel](const settings::Setting *s) -> SettingRow * {
            if (s == nullptr)
                return nullptr;
            panel.showPage(tabOf(*s));
            for (const auto &row : panel.rows())
                if (&row->setting == s)
                    return row.get();
            return nullptr;
        };
        const settings::Setting *flag = nullptr, *choice = nullptr, *projectSetting = nullptr;
        for (const auto &s : settings::registry()) {
            if (!flag && s.type == settings::Type::Bool && s.scope == settings::Scope::App)
                flag = &s;
            if (!choice && s.type == settings::Type::Choice && s.scope == settings::Scope::App &&
                s.choices.size() >= 2)
                choice = &s;
            if (!projectSetting && s.scope == settings::Scope::Project)
                projectSetting = &s;
        }
        if (auto *row = rowFor(flag)) {
            auto *button = dynamic_cast<juce::TextButton *>(row->control.get());
            const bool wasOn = settings.get(flag->key).get<bool>();
            if (button) {
                button->setToggleState(!wasOn, juce::dontSendNotification);
                button->onClick();
            }
            settings::AppSettings reread(appdata::App::Daw, settings.file());
            check(button && settings.get(flag->key) == !wasOn && reread.get(flag->key) == !wasOn &&
                      button->getButtonText() == (wasOn ? "Off" : "On"),
                  "a switch applies at once and is written at once, with no OK button");
            panel.defaults.onClick();
            check(settings.get(flag->key) == flag->defaultValue && button &&
                      button->getToggleState() == flag->defaultValue.get<bool>(),
                  "Restore Defaults puts the page back");
        } else
            check(false, "a switch to try");
        // What the application keeps of the settings follows a change at once: the Info View
        // from its switch, through the commands every window reads.
        {
            int changes = 0;
            panel.onChange = [&] {
                ++changes;
                commands.reload();
            };
            const settings::Setting *infoSetting = nullptr;
            for (const auto &s : settings::registry())
                if (s.key == "lookfeel.showInfoView")
                    infoSetting = &s;
            const bool infoWas = commands.infoView();
            auto *infoRow = rowFor(infoSetting);
            auto *infoSwitch = infoRow ? dynamic_cast<juce::TextButton *>(infoRow->control.get()) : nullptr;
            if (infoSwitch) {
                infoSwitch->setToggleState(!infoWas, juce::dontSendNotification);
                infoSwitch->onClick();
            }
            check(infoSwitch && changes == 1 && commands.infoView() == !infoWas,
                  "a change in the Settings window reaches the commands at once, and every window with them");
            if (infoSwitch) {
                infoSwitch->setToggleState(infoWas, juce::dontSendNotification);
                infoSwitch->onClick();
            }
            panel.onChange = nullptr;
        }
        if (auto *row = rowFor(choice)) {
            auto *chooser = dynamic_cast<juce::ComboBox *>(row->control.get());
            if (chooser)
                chooser->setSelectedItemIndex(1, juce::sendNotificationSync);
            check(chooser && settings.get(choice->key) == choice->choices[1], "a chooser applies at once");
            if (chooser)
                chooser->setSelectedItemIndex(0, juce::sendNotificationSync);
        } else
            check(false, "a chooser to try");
        if (auto *row = rowFor(projectSetting))
            check(!row->control->isEnabled(), "a project's setting is shown with its mark, changed by its op");
        const auto pageBefore = panel.page();
        // The editor posts its change as a message, which nothing dispatches here: run what it runs.
        // (Until now this check passed by chance: the page it was on held as many settings as match.)
        panel.search.setText("buffer", true);
        panel.search.onTextChange();
        check(panel.finding() && !panel.rows().empty() &&
                  panel.rows().size() == settings::search("buffer").size(),
              "the Find box lists every matching setting, from every page");
        panel.search.setText({}, true);
        panel.search.onTextChange();
        check(!panel.finding() && panel.page() == pageBefore, "clearing it goes back to the page");
        panel.showPage(pages.front());
        const auto ctrl = juce::ModifierKeys::ctrlModifier;
        check(pages.size() < 2 ||
                  (panel.keyPressed(juce::KeyPress(juce::KeyPress::tabKey, ctrl, 0)) && panel.page() == pages[1] &&
                   panel.keyPressed(
                       juce::KeyPress(juce::KeyPress::tabKey, ctrl | juce::ModifierKeys::shiftModifier, 0)) &&
                   panel.page() == pages.front()),
              "Ctrl+Tab and Ctrl+Shift+Tab move between the pages");
        check(pages.size() < 2 || (panel.pageList.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) &&
                                   panel.page() == pages[1]),
              "with the chooser focused, the arrows do");
        bool closed = false;
        panel.onClose = [&closed] { closed = true; };
        check(panel.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && closed, "Esc closes the window");
        // Restore Defaults puts the open tab back, settings moved onto it from other pages included.
        if (const auto *toggle = settings::find("sync.showLinkToggle")) {
            panel.showPage("Link");
            const auto was = settings.get(toggle->key);
            panel.change(*toggle, !toggle->defaultValue.get<bool>());
            panel.defaults.onClick();
            check(settings.get(toggle->key) == toggle->defaultValue, "Restore Defaults puts the open tab back");
            panel.change(*toggle, was);
        }
        // Taller than the window, the tab column scrolls; the open tab is kept in view.
        panel.pageList.resized();
        panel.showPage(pages.back());
        const auto lastTab = panel.pageList.tabs().back();
        check(panel.pageList.contentHeight() > panel.pageList.getHeight() && panel.pageList.scroll() > 0 &&
                  lastTab.getBottom() <= panel.pageList.getHeight(),
              "the last tab, scrolled into view");
        panel.showPage(pages.front());
        check(panel.pageList.scroll() == 0 && panel.pageList.tabs().front().getY() == 6, "and the first, back at the top");
        const settings::Setting *ranged = nullptr;
        for (const auto &s : settings::registry())
            if (!ranged && s.type == settings::Type::Int && s.scope == settings::Scope::App && s.max > s.min)
                ranged = &s;
        check(ranged && !panel.change(*ranged, static_cast<std::int64_t>(ranged->max) + 1) &&
                  panel.help().isNotEmpty(),
              "a value the registry refuses is not written, and the help line says why");
        // The keyboard's part (manual 40.1.2): with the focus, the arrows switch a switch
        // and step a value box, and Enter switches too.
        if (auto *row = rowFor(flag)) {
            // Through the Component, as the window's key dispatch reaches it.
            juce::Component *control = row->control.get();
            auto *button = dynamic_cast<juce::TextButton *>(control);
            const bool initial = settings.get(flag->key).get<bool>();
            const bool up = button && control->keyPressed(juce::KeyPress(juce::KeyPress::upKey)) &&
                            settings.get(flag->key) == !initial;
            const bool down = button && control->keyPressed(juce::KeyPress(juce::KeyPress::downKey)) &&
                              settings.get(flag->key) == initial;
            const bool enter = button && control->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)) &&
                               settings.get(flag->key) == !initial &&
                               button->getButtonText() == (initial ? "Off" : "On");
            check(up && down && enter, "with the focus, either arrow or Enter switches a switch, at once");
            panel.defaults.onClick();
        }
        if (auto *row = rowFor(ranged)) {
            auto *box = dynamic_cast<juce::Slider *>(row->control.get());
            const auto stored = settings.get(ranged->key).get<std::int64_t>();
            const bool rising = stored < static_cast<std::int64_t>(ranged->max);
            check(box && box->getWantsKeyboardFocus() &&
                      box->keyPressed(juce::KeyPress(rising ? juce::KeyPress::upKey : juce::KeyPress::downKey)) &&
                      settings.get(ranged->key) == stored + (rising ? 1 : -1),
                  "and the arrows step a value box by one, written at once");
            panel.change(*ranged, stored);
            row->refresh();
            UiPeerRoot settingsHost;
            settingsHost.setSize(panel.getWidth(), panel.getHeight());
            settingsHost.addAndMakeVisible(panel);
            settingsHost.setVisible(true);
            settingsHost.addToDesktop(0);
            if (box)
                box->grabKeyboardFocus();
            const bool marked = row->keyFocused();
            row->focusOfChildComponentChanged(juce::Component::focusChangedByMouseClick);
            const bool clicked = !row->keyFocused();
            if (box)
                box->grabKeyboardFocus();
            if (box)
                box->giveAwayKeyboardFocus();
            check(marked && clicked && !row->keyFocused(),
                  "the focus the keyboard gives is marked; a click's focus and none are not");
            settingsHost.removeChildComponent(&panel);
            settingsHost.removeFromDesktop();
        }
        if (const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
            folder.isNotEmpty()) {
            panel.showPage("Audio");
            juce::Image image(juce::Image::ARGB, 2 * panel.getWidth(), 2 * panel.getHeight(), true,
                              juce::SoftwareImageType{});
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(2.f));
                panel.paintEntireComponent(g, true);
            }
            const auto file = juce::File(folder).getChildFile("adi-settings.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(image, *stream);
            // Live 12's manual shows its Settings on the Link tab: ADI's, for the picture beside it.
            panel.showPage("Link");
            panel.setHelp({});
            juce::Image linkImage(juce::Image::ARGB, 2 * panel.getWidth(), 2 * panel.getHeight(), true,
                                  juce::SoftwareImageType{});
            {
                juce::Graphics g(linkImage);
                g.addTransform(juce::AffineTransform::scale(2.f));
                panel.paintEntireComponent(g, true);
            }
            const auto linkFile = juce::File(folder).getChildFile("adi-settings-link.png");
            linkFile.deleteFile();
            if (auto stream = linkFile.createOutputStream())
                juce::PNGImageFormat{}.writeImageToStream(linkImage, *stream);
        }
    }
    trace("liveParity", [&] { liveParity(commands, temp.path(), write); });
    trace("groupTracks", [&] { groupTracks(commands, temp.path()); });
    trace("footRows", [&] { footRows(commands, temp.path()); });
    trace("newProject", [&] { newProject(commands, temp.path()); });
    trace("stripParamUndo", [&] { stripParamUndo(commands, temp.path()); });
    trace("deviceClipboard", [&] { deviceClipboard(commands, temp.path()); });
    trace("pluginWindows", [&] { pluginWindows(commands, temp.path()); });
    trace("automationArm", [&] { automationArm(commands, temp.path()); });
    trace("armAndRecord", [&] { armAndRecord(commands, temp.path()); });
    trace("exportPage", [&] { exportPage(commands, temp.path()); });
    trace("mixerDragLive", [&] { mixerDragLive(commands, temp.path()); });
    trace("automationMode", [&] { automationMode(commands, temp.path()); });
    trace("sendKnobs", [&] { sendKnobs(commands, temp.path()); });
    trace("audioDrop", [&] { audioDrop(commands, temp.path()); });
    trace("importPrompt", [&] { importPrompt(commands, settings, temp.path()); });
    trace("browserDevices", [&] { browserDevices(commands, temp.path()); });
    trace("splitAndRangeEdits", [&] { splitAndRangeEdits(commands, settings, temp.path()); });
    trace("clipClipboard", [&] { clipClipboard(commands, temp.path()); });
    trace("timeCommands", [&] { timeCommands(commands, temp.path()); });
    trace("colorCabTile", [&] { colorCabTile(commands, temp.path()); });
    trace("rangeSelection", [&] { rangeSelection(commands, temp.path()); });
    trace("clipToTrack", [&] { clipToTrack(commands, temp.path()); });
    trace("clipView", [&] { clipView(commands, temp.path(), write); });
    trace("clipViewNotes", [&] { clipViewNotes(commands, temp.path()); });
    trace("clipViewFields", [&] { clipViewFields(commands, temp.path()); });
    trace("clipViewTools", [&] { clipViewTools(commands, temp.path()); });
    trace("clipViewMore", [&] { clipViewMore(commands, temp.path()); });
    trace("clipViewTime", [&] { clipViewTime(commands, temp.path()); });
    trace("clipViewClipTime", [&] { clipViewClipTime(commands, temp.path()); });
    trace("clipViewMulti", [&] { clipViewMulti(commands, temp.path()); });
    trace("mixViewShell", [&] { mixViewShell(commands, temp.path()); });
    trace("recordingOptions", [&] { recordingOptions(commands, settings, temp.path()); });
    trace("undoHistoryWindow", [&] { undoHistoryWindow(commands, temp.path()); });
    trace("clipWaveforms", [&] { clipWaveforms(commands, temp.path()); });
    trace("clipFades", [&] { clipFades(commands, temp.path()); });
    trace("showcase", [&] { showcase(commands, temp.path()); });
    trace("loopMetronome", [&] { loopMetronome(commands, settings, temp.path()); });
    trace("locators", [&] { locators(commands, temp.path()); });
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
