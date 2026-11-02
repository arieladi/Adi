// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/juce/ui_floating.hpp"
#include "../src/juce/ui_settings.hpp"
#include "../src/juce/ui_shell.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/ui/send_edits.hpp"
#include "temp_directory.hpp"
#include "ui_fake_peer.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <thread>
#include <tuple>
#include <vector>
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
        for (const auto track : laneOrder(root))
            if (const int top = root.arrangement.geometry.rowTop(root.reader(), track); top >= 0)
                g.fillRect(headers.getX() + 6, headers.getY() + top + 4, std::max(0, headers.getWidth() - 8), 24);
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
                     &strip->active, &strip->solo, &strip->eq, &strip->limiter, &strip->volume,
                     &strip->pan})
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
    if (differences)
        std::printf("%s: %d differing pixels\n", name, differences);
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
    check(before.size() == 5 && before[0].first == "Project" && before[0].second && before[1].first == "Body" &&
              before[2].first == "Fx" && !before[2].second && !root.arrangement.projectView.getToggleState(),
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
    check(root.state().viewGroup == 3 && root.arrangement.projectView.getToggleState() &&
              lanes.hidden == std::vector<std::int64_t>{2} && lanes.heightFor(2) == 0 && passesOver &&
              lanes.selection.track == 3 && items()[2].second,
          "Fx chosen: Body is left out of the lanes, Main kept, the button lit, the selection on Fx");
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
    check(root.state().viewGroup == 0 && lanes.hidden.empty() && !root.arrangement.projectView.getToggleState(),
          "a chosen group that is gone gives way to the whole project");
    choose("Body");
    check(root.state().viewGroup == 2 && lanes.hidden.empty(), "a group at the top shows everything inside it");
    choose("Project");
    check(root.state().viewGroup == 0 && lanes.hidden.empty() && !root.arrangement.projectView.getToggleState() &&
              shownRows(root.mixer) == std::vector<std::int64_t>{1, 2, 3, 4},
          "Project shows every track again, the button unlit");
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
        check(canvas.keyPressed(juce::KeyPress('u')) && root.state().foldedGroups == std::set<std::int64_t>{13},
              "U on a track that is not a group folds nothing");
        // A view of a group inside a folded one shows it: the folded group is left out.
        root.arrangement.chooseView(16);
        check(lanes.heightFor(16) > 0 && lanes.heightFor(17) > 0 && lanes.heightFor(13) == 0 && lanes.heightFor(1) > 0,
              "Fx And Hits viewed alone inside folded Body: it shows, with its tracks and Main");
        root.arrangement.chooseView(0);
        check(canvas.keyPressed(juce::KeyPress('u', juce::ModifierKeys::altModifier, 0)) &&
                  root.state().foldedGroups.empty() && lanes.heightFor(14) > 0,
              "Alt+U unfolds every group");
    }
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
        root.frame();
        auto stackIds = [&root] {
            std::vector<std::int64_t> out;
            for (const auto &r : root.mixViewPanel.stack.rows())
                out.push_back(r->track());
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
                // The line, white, where the drop would land; or the group outlined.
                juce::Image image(juce::Image::ARGB, headers.getWidth(), headers.getHeight(), true, juce::SoftwareImageType{});
                {
                    juce::Graphics g(image);
                    headers.paintEntireComponent(g, true);
                }
                const int y = shown->into != 0 ? topOf(shown->into) + 1 : shown->y;
                lineShown = image.getPixelAt(headers.nameLeft(shown->depth) + 30, y) == juce::Colours::white;
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
    check(inFragment.size() == 3 && inFragment[1] == std::pair<std::string, bool>{"Delete Fragmentary Bar Time", false} &&
              inFragment[2] == std::pair<std::string, bool>{"Complete Fragmentary Bar (Insert Time)", false} &&
              root.error().find("time ops") != std::string::npos,
          "the menu on the fragment: Live's two commands, dimmed, and the Status Bar says why");
    check(names(scrub.menu(10 * q)).size() == 1, "elsewhere: Insert Time Signature Change alone");
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
    check(audio->eq.getY() == 54 && audio->limiter.getY() == 54, "ADI-Q and ADI-L on the row after the sends");
    auto *main = rowOf(root.mixer, 7);
    auto *reverb = rowOf(root.mixer, 5);
    check(main != nullptr && main->sendKnobs().empty(), "Main has no sends");
    // Live's MIDI tracks with no instrument (look-live11 difference row 3): their switches, no volume,
    // pan or sends; ADI-Q and ADI-L on the row where volume and pan would be.
    auto *firstMidi = rowOf(root.mixer, 1);
    check(firstMidi != nullptr && firstMidi->isMidi() && !firstMidi->hasAudio() && firstMidi->active.isVisible() &&
              !firstMidi->volume.isVisible() && !firstMidi->pan.isVisible() && firstMidi->sendKnobs().empty() &&
              firstMidi->eq.getY() == 20,
          "1 MIDI: its switches, no volume, pan or sends; ADI-Q and ADI-L on the second row");
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

    // Enable Send on A Reverb's send B: on, at full alpha, taking clicks.
    reverb->enableSend(1, true);
    root.frame();
    check(reverb->sendOn(1) && !reverb->sendOn(0) && takesClicks(*reverb->sendKnobs()[1]) &&
              findSend(root.reader().sends(), 5, 6) != nullptr && findSend(root.reader().sends(), 5, 6)->enabled,
          "Enable Send: A Reverb feeds B Delay, fully down until turned up");
    reverb->enableSend(1, true);
    check(root.error() == "The send is on already.", "enabling it again only says so");

    // Enable All Sends on B Delay (§18.4): both of its sends on, A and itself, fully down, one step.
    auto *delay = rowOf(root.mixer, 6);
    check(delay != nullptr && !delay->sendOn(0) && !delay->sendOn(1), "B Delay's sends start off");
    if (delay != nullptr) {
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
    // Pre moves every send into A before the fader (3 Audio's), one undo step.
    reverb->prePost.onClick();
    root.frame();
    check(reverb->prePost.getButtonText() == "Pre" && !reverb->prePost.getToggleState() &&
              findSend(root.reader().sends(), 3, 5)->preFader && submitter.nextUndo()->label == "Change Pre/Post",
          "Pre/Post thrown: 3 Audio's send A taps before the fader");

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
        check(ka == juce::Rectangle<int>(10, 46, 32, 32) && kb == juce::Rectangle<int>(51, 68, 32, 32) &&
                  column->sendKnobs()[0]->getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag,
              "knob A on the left, B to its right and 22 pt lower, as Live's");
        check(column->volume.getY() == 40 + 72 + 6 && column->peakHold.getY() == 40 + 72 + 5,
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
            check(aSwitch.getButtonText() == "Pre" && mainColumn->returnSwitches()[1]->getButtonText() == "Post" &&
                      !mainColumn->active.isVisible(),
                  "Main's strip shows A's Pre and B's Post where a track has its knobs");
            aSwitch.onClick();
            root.frame();
            check(aSwitch.getButtonText() == "Post" && !findSend(root.reader().sends(), 3, 5)->preFader,
                  "and A's switch, thrown there, moves the send back after the fader");
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
    // Hide/Show Sends (§42.1, Ctrl/Cmd+Alt+S; Live 12's Arrangement Track Controls): the sends out of
    // the track controls, ADI-Q and ADI-L moving up under volume and pan. The Mixer View keeps its
    // own (Live 12's Mixer Controls), its strips losing theirs with that toggle alone.
    const auto sendsKey = juce::KeyPress('s', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0);
    check(root.keyPressed(sendsKey) && !root.state().sends && root.state().mixerSends, "Ctrl/Cmd+Alt+S hides the track controls' sends");
    column = rowOf(root.mixerView, 3);
    auto *mainStrip = rowOf(root.mixerView, 7);
    check(!a.isVisible() && !b.isVisible() && audio->eq.getY() == 37 && audio->limiter.getY() == 37,
          "the row's -inf boxes go; ADI-Q and ADI-L take their row");
    check(column != nullptr && column->sendsShown() == 2 && column->sendKnobs()[0]->isVisible(),
          "the Mixer View strips keep theirs");
    check(root.command(AppCommands::ToggleMixerSends) && !root.state().mixerSends && root.mixerOptionsMenu().getNumItems() > 0,
          "the Mixer Controls' Sends hides the Mixer View's");
    column = rowOf(root.mixerView, 3);
    mainStrip = rowOf(root.mixerView, 7);
    check(column != nullptr && column->sendsShown() == 0 && !column->sendKnobs()[0]->isVisible() &&
              column->volume.getY() == 40 + 6 && mainStrip != nullptr && mainStrip->returnSwitches().size() == 2 &&
              !mainStrip->returnSwitches()[0]->isVisible(),
          "the Mixer View strips lose their Sends section, Main's Pre/Post switches with it");
    render("adi-sends-hidden.png");
    check(root.keyPressed(sendsKey) && root.state().sends && a.isVisible() && audio->eq.getY() == 54,
          "Ctrl/Cmd+Alt+S shows the track controls' again");
    column = rowOf(root.mixerView, 3);
    check(column != nullptr && column->sendsShown() == 0 && root.command(AppCommands::ToggleMixerSends) &&
              root.state().mixerSends && column->sendsShown() == 2 && column->sendKnobs()[0]->isVisible(),
          "and the Mixer Controls' Sends the Mixer View's");
    // Live 12's Mixer View Control options (manual 18.1): In/Out, Sends, Volume, Track Options,
    // Crossfader, Performance Impact, then Return Tracks; what ADI has not built is dimmed.
    auto mixerOptions = [&root] {
        juce::StringArray out;
        const auto menu = root.mixerOptionsMenu();
        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
            if (!it.getItem().isSeparator)
                out.add(it.getItem().text + (it.getItem().isTicked ? " *" : "") + (it.getItem().isEnabled ? "" : " -"));
        return out.joinIntoString(", ");
    };
    check(mixerOptions() == "In/Out, Sends *, Volume * -, Track Options -, Crossfader -, Performance Impact -, Return Tracks *",
          "the Mixer View's options in Live's order, In/Out off");
    // Its In/Out: Live 12's section under ADI's slots and over the sends; as many sends as still
    // leave the mixer its room, then the rest when the strip is taller.
    check(root.command(AppCommands::ToggleMixerInOut) && root.state().mixerInOut && mixerOptions().startsWith("In/Out *"),
          "the Mixer's In/Out shows, and its item is ticked");
    column = rowOf(root.mixerView, 3);
    check(column != nullptr && column->showsMixerInOut() && column->sendsShown() < 2 &&
              (column->sendsShown() == 0 || column->sendKnobs()[0]->getY() == 40 + MixerStrip::kInOutHeight + 6),
          "at 320 points the strip shows its In/Out over the sends, and leaves out the sends that do not fit");
    root.resizeMixerView(440);
    root.mark(DirtySet::All);
    root.frame();
    column = rowOf(root.mixerView, 3);
    mainStrip = rowOf(root.mixerView, 7);
    check(column != nullptr && column->showsMixerInOut() && column->sendsShown() == 2 &&
              column->volume.getY() == 40 + MixerStrip::kInOutHeight + 72 + 6 && mainStrip != nullptr &&
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
    check(root.command(AppCommands::ToggleMixerView), "the Mixer View closes");
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
    // Live's selectors at the strip's foot repeat the View menu: R hides the returns, I-O the In/Out;
    // each is lit while its part shows.
    check(root.strip.returns.getToggleState() && root.strip.inOut.getToggleState() &&
              root.strip.mixer.getToggleState() && !root.strip.delay.getToggleState(),
          "I-O, R and M lit, D not, as Live's");
    root.strip.returns.onClick();
    check(root.state().hideReturns && !root.strip.returns.getToggleState() &&
              shownLanes() == std::vector<std::int64_t>{1, 2, 3, 4, 7},
          "R hides the returns and goes dark");
    root.strip.returns.onClick();
    root.strip.inOut.onClick();
    check(!root.state().hideReturns && root.strip.returns.getToggleState() && !root.state().inOut &&
              !root.strip.inOut.getToggleState() && !rowOf(root.mixer, 1)->showsInOut(),
          "R shows them again; I-O hides the In/Out");
    root.strip.inOut.onClick();
    check(root.state().inOut && root.strip.inOut.getToggleState(), "and shows it again");
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
                  submitter.nextUndo()->label == "Move Clips" && submitter.nextUndo()->opCount == 2,
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
        check(onTrack(1).empty() && onTrack(2) == std::vector<std::int64_t>{11} && onTrack(3) == std::vector<std::int64_t>{12, 13} &&
                  submitter.nextUndo()->label == "Move Clips" && geo.selection.track == 3,
              "both a row down: track 1's on track 2, track 2's on track 3, one step; the selection follows the one dragged");
        check(root.command(AppCommands::Undo), "undo");
        root.frame();
        check(onTrack(1) == std::vector<std::int64_t>{11} && onTrack(2) == std::vector<std::int64_t>{12}, "puts them back");
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
// The Mix View (ADR-0209, ruled 1 November): F4 and the control bar's Mix button open it in place
// of the arrangement; its tabs in the draft's order, STACK first; kept per window.
void mixViewShell(AppCommands &commands, const std::filesystem::path &dir) {
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
    AdiRootComponent root(view, submitter, mailbox, commands, persistence, session, "mix");
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
              panel.stack.rows().size() == 4,
          "STACK shows, lit, with a row for each channel");
    check(panel.stack.getBounds() == panel.body() && panel.body().getY() >= MixViewPanel::kHeaderHeight,
          "under the tabs");
    panel.tabs[0]->onClick();
    root.frame();
    check(root.state().mixViewTab == 0 && panel.tab() == MixViewPanel::Tab::Overview && !panel.stack.isVisible() &&
              panel.tabs[0]->getToggleState() && !panel.tabs[1]->getToggleState(),
          "a click on OVERVIEW shows it, drawn empty until after the first window (d11)");
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
    check(panel.stack.rows().size() == 5, "and STACK has a row for it at the next frame");
    // STACK (ADR-0209 d11; Pro-Q's instance list): the row under the mouse opens, dormant slots are
    // flat dimmed lines, a slot says what it waits for, and the window's group decides the rows.
    {
        auto &stack = panel.stack;
        auto rowOf = [&stack](std::int64_t id) -> StackRow * {
            for (const auto &r : stack.rows())
                if (r->track() == id)
                    return r.get();
            return nullptr;
        };
        auto *kick = rowOf(10), *bass = rowOf(11);
        check(kick != nullptr && bass != nullptr && kick->getHeight() == StackRow::kHeight &&
                  bass->getY() == kick->getBottom(),
              "rows of one height, one under another");
        kick->mouseEnter(pressAt(*kick, {40.f, 10.f}));
        check(stack.hovered() == 10 && kick->zoomed() && kick->getHeight() == StackRow::kZoomedHeight &&
                  bass->getY() == kick->getBottom(),
              "the row under the mouse opens, the rows below move down");
        check(kick->eqArea().getHeight() > 120 && kick->sidechainArea().getRight() < kick->limiterArea().getX() &&
                  kick->eqArea().getRight() < kick->sidechainArea().getX(),
              "ADI-Q's graph to edit, the sidechain slot and ADI-L as drawers beside it");
        kick->mouseExit(pressAt(*kick, {40.f, 10.f}));
        check(stack.hovered() == 0 && kick->getHeight() == StackRow::kHeight, "and closes when the mouse leaves");
        // Dormant: the 0 dB line dimmed, never the track's colour.
        auto pixelAt = [](juce::Component &c, juce::Point<int> at) {
            juce::Image image(juce::Image::ARGB, c.getWidth(), c.getHeight(), true, juce::SoftwareImageType{});
            {
                juce::Graphics g(image);
                c.paintEntireComponent(g, true);
            }
            return image.getPixelAt(at.x, at.y);
        };
        const auto flat = juce::Point<int>(kick->eqArea().getCentreX(), kick->eqArea().getCentreY());
        const auto kickColour = root.arrangement.colourFor(0);
        check(pixelAt(*kick, flat) != kickColour && !stack.provider().live(), "a dormant ADI-Q: a flat line, dimmed");
        // Engaged, as the engine's slots will report: the line in the track's colour, the drawer lit.
        struct Engaged final : MixViewProvider {
            ChannelSlots slots(const engine::TrackNode &t) const noexcept override {
                ChannelSlots s;
                s.eq = t.id == 10;
                s.sidechain = t.id == 10 ? SidechainMode::Rmsc : SidechainMode::Off;
                return s;
            }
            bool live() const noexcept override { return true; }
        } engaged;
        const auto &own = root.mixProvider(); // the root's stub, which outlives this block
        root.setMixProvider(engaged);
        const auto lit = kick->sidechainArea().getTopLeft() + juce::Point<int>(2, 2);
        check(pixelAt(*kick, flat) == kickColour && pixelAt(*kick, lit) != pixelAt(*bass, lit),
              "engaged: Kick's line in its colour, its sidechain drawer lit, Bass's not");
        root.setMixProvider(own);
        // A slot says what it waits for; elsewhere a click selects the channel's track.
        bass->mouseDown(pressAt(*bass, bass->sidechainArea().getCentre().toFloat()));
        check(stack.focused() == 11 && root.error().find("sidechain") != std::string::npos,
              "a click on the sidechain slot focuses Bass and says it waits for the engine's slots");
        bass->mouseDown(pressAt(*bass, {40.f, 10.f}));
        check(root.arrangement.geometry.selection.track == 11, "a click on its name selects Bass's track");
        // The View menu's Channel EQ, and a track row's Q, open STACK on the selected track.
        check(root.chooseMixViewTab(0), "on another tab");
        root.arrangement.geometry.selection.track = 12;
        check(root.command(AppCommands::ShowChannelEq) && root.state().mixViewTab == 1 && stack.focused() == 12,
              "Channel EQ opens STACK, Lead's row focused (ADR-0204 d1's one window)");
        check(stack.scope() == "Project", "the rows: the whole project, the project view showing it");
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
        if (const auto out = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); out.isNotEmpty()) {
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
    AdiRootComponent other(view, submitter, mailbox, commands, persistence, session, "mix-other");
    other.setSize(1200, 720);
    other.frame();
    check(!other.mixViewPanel.isVisible() && other.arrangement.isVisible(), "a second window keeps its own view");
    root.transport.mix.onClick();
    root.frame();
    check(!root.state().mixView && !panel.isVisible() && root.arrangement.isVisible() && root.mixer.isVisible() &&
              root.arrangement.getBounds() == lanes && !root.transport.mix.getToggleState(),
          "the Mix button closes it: the arrangement and its track controls where they were");
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
        check(panel.stack.rows().size() == 6 && host.drains("Mix View") == 2, "the window's own clock frames it");
        root.frame();
        check(root.keyPressed(juce::KeyPress(juce::KeyPress::F4Key)) && host.count() == 0 &&
                  panel.getParentComponent() == &root && !panel.isVisible() && !root.state().mixView &&
                  root.state().mixViewDetached,
              "F4 closes its window; it will open there again");
        check(root.keyPressed(juce::KeyPress(juce::KeyPress::F4Key)) && host.content("Mix View") == &panel,
              "and F4 opens it there again");
        // Its own close button: the host collects the window, and the Mix View is closed.
        host.close("Mix View");
        syncMixView(host, root);
        check(!root.state().mixView && root.state().mixViewDetached && !panel.isVisible(),
              "a window closed by its own button closes the Mix View");
        check(root.setMixView(true, true) && host.count() == 0, "kept open in a window, as a project reopens");
        syncMixView(host, root, true);
        check(host.content("Mix View") == &panel, "at startup it opens in its window again");
        check(persistence.load("mix", {}).mixViewDetached, "where it opens is kept with the window's view");
        if (const auto out = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {}); out.isNotEmpty()) {
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
    const auto folder = juce::SystemStats::getEnvironmentVariable("ADI_UI_PARITY", {});
    if (folder.isNotEmpty()) {
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
        check(a.arrangement.namesFirst() && a.strip.flip.getToggleState() &&
                  a.stripArea().getX() == look::gap && a.stripArea().getRight() <= namesC.getX() &&
                  namesC.getRight() <= a.mixerArea().getX() &&
                  lanesC.getX() - a.mixerArea().getRight() <= ArrangementView::kLeftMargin &&
                  lanesC.getRight() <= a.browserArea().getX() &&
                  a.arrangement.ruler.getX() + at.x == lanesC.getX(),
              "flipped: the strip at the left edge, the names, the track controls against the lanes");
        // The director's place for the arrows (2026-10-28): under the strip's two view switches.
        auto underSwitches = [&a] {
            const auto r = a.strip.flip.getBounds() + a.strip.getPosition();
            return a.strip.isVisible() && a.stripArea().contains(r) && r.getY() >= a.stripArea().getY() + 44 &&
                   r.getBottom() <= a.stripArea().getY() + a.arrangement.topHeight() &&
                   a.strip.flip.getTooltip() == "Flip sides";
        };
        check(underSwitches(), "the flip arrows sit under the strip's view switches and say Flip sides");
        a.strip.flip.onClick();
        a.frame();
        const auto atL = a.arrangement.getPosition();
        const auto namesL = a.arrangement.headers.getBounds() + atL;
        const auto lanesL = a.arrangement.canvas.getBounds() + atL;
        check(!a.arrangement.namesFirst() && !a.strip.flip.getToggleState() &&
                  a.state().panels.slots()[0].panel == Panel::Browser &&
                  a.browserArea().getRight() <= lanesL.getX() &&
                  lanesL.getRight() <= namesL.getX() &&
                  namesL.getRight() <= a.mixerArea().getX() &&
                  a.mixerArea().getRight() == a.stripArea().getX() && underSwitches(),
              "the arrows put Live's sides back: lanes, names, track controls, the strip at the edge");
        // With the track controls hidden, the strip goes with them and the arrangement takes
        // their place, up to the window's edge.
        a.command(AppCommands::ToggleMixer);
        a.frame();
        check(!a.mixer.isVisible() && !a.strip.isVisible() &&
                  a.arrangement.getRight() == a.getWidth() - look::gap,
              "hidden track controls take the strip with them; the arrangement reaches the edge");
        a.command(AppCommands::ToggleMixer);
        a.frame();
        a.strip.flip.onClick();
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
                  at.getY() >= (a.strip.flip.getBounds() + a.strip.getPosition()).getBottom() &&
                  a.stripArea().contains(at),
              "large tracks by default: the button lit under the flip arrows, the View menu ticked");
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
        check(items("Options") == std::vector<int>{C::Follow, 0, C::NarrowGrid, C::WidenGrid, C::TripletGrid,
                                                   C::SnapToGrid, C::FixedGrid},
              "the Options menu: Follow (§6.2), then the grid (§6.10)");
        check(items("File") == std::vector<int>{C::NewProject, C::OpenProject, 0,
                                                C::TakeSnapshot, C::SaveAs},
              "the File menu; Quit is the application menu's on the Mac");
#else
        check(names == juce::StringArray{"File", "Edit", "Create", "View", "Options", "Navigate"},
              "the menu bar: File, Edit, Create, View, Options, Navigate");
        check(items("File") == std::vector<int>{C::NewProject, C::OpenProject, 0,
                                                C::TakeSnapshot, C::SaveAs, 0, C::Quit},
              "the File menu ends with Quit and no longer holds Settings");
        check(items("Options") == std::vector<int>{C::Follow, 0, C::NarrowGrid, C::WidenGrid, C::TripletGrid,
                                                   C::SnapToGrid, C::FixedGrid, 0, C::Settings},
              "the Options menu: Follow (§6.2), the grid (§6.10), then Settings off the Mac (§2.3)");
#endif
        check(items("Edit") == std::vector<int>{C::Undo, C::Redo, 0, C::DeleteTrack, 0,
                                                C::RenameTrack, C::SelectAll, 0, C::GroupTracks, C::UngroupTracks},
              "the Edit menu deletes the selected track, before Rename (§18.2) and Select All, then Group and "
              "Ungroup Tracks");
        check(items("Create") == std::vector<int>{C::InsertAudioTrack, C::InsertMidiTrack, C::InsertReturnTrack, 0,
                                                  C::InsertTimeSignatureChange},
              "the Create menu inserts tracks, returns too (§18.4), then a time signature change (§6.5)");
        check(items("Navigate") == std::vector<int>{C::FocusControlBar, C::FocusArrangement,
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
            auto texts = [](const juce::PopupMenu &menu) {
                juce::StringArray out;
                for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
                    if (!it.getItem().isSeparator)
                        out.add(it.getItem().text + (it.getItem().isEnabled ? "" : " -"));
                return out.joinIntoString(", ");
            };
            for (juce::PopupMenu::MenuItemIterator it(viewMenu); it.next();) {
                const auto &item = it.getItem();
                if (item.subMenu != nullptr) {
                    submenus.add(item.text);
                    (item.text == "Mixer Controls" ? mixerControls : trackControls) = texts(*item.subMenu);
                } else
                    flat.push_back(item.itemID);
            }
            auto has = [&flat](int id) { return std::find(flat.begin(), flat.end(), id) != flat.end(); };
            check(submenus == juce::StringArray{"Arrangement Track Controls", "Mixer Controls"} &&
                      trackControls == "Hide/Show Track Controls, In/Out, Sends, Volume -, Track Options -" &&
                      mixerControls ==
                          "In/Out, Sends, Volume -, Track Options -, Crossfader -, Performance Impact -, Return Tracks" &&
                      !has(C::ToggleInOut) && !has(C::ToggleSends) && !has(C::ToggleReturnTracks) && !has(C::ToggleMixer),
                  "the View menu holds Live 12's Arrangement Track Controls and Mixer Controls submenus, "
                  "their toggles no longer loose in it");
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
    auto document = ProjectDocument::open(temp.path() / "integrated.adi", true, {}, documentError);
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
                ++saveAs;
                integrated.say("Save As waits for the engine's (ADR-0203 d6)");
                return false;
            }
            if (id != AppCommands::TakeSnapshot)
                return false;
            ++snapshots;
            return document->takeSnapshot({}, 0, 0).ok;
        };
        check(!integrated.canUndo(false) && integrated.undoTitle(false) == "Undo",
              "a new project has nothing to undo, as Live");
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
        check(integrated.keyPressed(juce::KeyPress('s', juce::ModifierKeys::commandModifier |
                                                            juce::ModifierKeys::shiftModifier,
                                                   0)) &&
                  saveAs == 1 && !integrated.error().empty(),
              "Save As reaches the application, which says why it cannot yet");
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
                      overview.getRight() > integrated.arrangement.getRight() &&
                      arrangement.ruler.getY() == ArrangementView::kOverviewHeight &&
                      arrangement.canvas.getY() == ArrangementView::kTopHeight,
                  "the Overview sits above the beat-time ruler, across the track headers as Live's");
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
                peerRoot.getLocalPoint(&arrangement.scrub, juce::Point<int>{100, 7}).toFloat();
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
            check(items == juce::StringArray{"Delete", "Rename", "Fold", "Configure"},
                  "a device's menu: Delete, Rename, then the device's own, in Live's order");
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
            for (int retry = 0; retry < 100 && !model->read(bins, revision); ++retry)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(revision > 0 && bins[100] < -90 && std::abs(bins[32]) < .01,
                  "analyser worker calibrated floor before 0 dB sine");
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
            floating.close("analyser");
        }
        integrated.devices.publication = {};
        arrangement.geometry.selection.track = 2;
        integrated.frame();
        integrated.resizeDevices(169);
        // Step 7.6 (#205): the mixer, its rows in the arrangement's lanes.
        auto meterTap = std::make_shared<engine::ScopeTap>(48000, .1);
        {
            std::array<float, 1024> half{};
            for (std::size_t i = 0; i < half.size(); ++i)
                half[i] = .5f * static_cast<float>(std::sin(2 * 3.141592653589793 * 16 *
                                                            static_cast<double>(i) / 1024.));
            meterTap->write(half.data(), half.data(), 1024, 0, true);
        }
        auto &mixer = integrated.mixer;
        mixer.tap = [&](std::int64_t id) {
            return id == 2 ? std::shared_ptr<const engine::ScopeTap>(meterTap) : nullptr;
        };
        mixer.retap();
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
            check(std::abs(row->peak() - .5f) < .01f && row->rms() > .3f && row->rms() < .4f,
                  "the meter reads the track's tap: peak and RMS");
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
            const auto opsBefore =
                document->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            const bool mixOpen = integrated.state().mixView;
            row->eq.onClick();
            row->limiter.onClick();
            check(!row->eq.getToggleState() && !row->limiter.getToggleState() &&
                      !integrated.error().empty() &&
                      document->store().db().execAndGet("SELECT count(*) FROM ops").getInt() ==
                          opsBefore,
                  "the ADI-Q and ADI-L slots are on every strip, present and off");
            check(integrated.state().mixView && integrated.mixViewPanel.stack.focused() == 2,
                  "and a slot opens the Mix View's STACK on its channel (ADR-0204 d1, ADR-0209 d11)");
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
        // ADI's one channel window (ADR-0204 d1), the Mix View's STACK (ADR-0209 d11): every
        // channel's ADI-Q, sidechain slot and ADI-L in one view.
        {
            StackTab eqView(integrated);
            eqView.setSize(900, 460);
            SnapshotReader eqReader(document->view().current());
            eqView.frame(eqReader);
            const auto all = laneOrder(integrated);
            bool ordered = !all.empty() && eqView.rows().size() == all.size();
            for (std::size_t i = 0; ordered && i < all.size(); ++i)
                ordered = eqView.rows()[i]->track() == all[i];
            check(ordered, "STACK lists every channel, in the arrangement's order");
            bool modes = !eqView.rows().empty();
            for (const auto &eqRow : eqView.rows())
                modes = modes && std::string(eqRow->mode()) == (eqRow->track() == 1 ? "Full" : "Light");
            check(modes, "ADI-L is full on Main's row and light on every other channel's");
            allocations = 0;
            audit = true;
            eqView.frame(eqReader);
            audit = false;
            check(allocations == 0, "a frame that changes no channel builds nothing");
            if (eqView.rows().size() >= 2) {
                auto &second = *eqView.rows()[1];
                second.mouseDown(pressAt(second, second.eqArea().getCentre().toFloat()));
                check(eqView.focused() == second.track() && !integrated.error().empty() &&
                          !integrated.mixProvider().slots(*integrated.reader().findTrack(second.track())).eq,
                      "a click on a slot focuses its channel, says what it waits for, and the slot stays off");
            }
            MixerStrip *strip = nullptr;
            for (const auto &s : mixer.strips())
                if (s->isVisible() && s->track() != integrated.arrangement.geometry.selection.track)
                    strip = s.get();
            if (strip) {
                const auto selectedBefore = integrated.arrangement.geometry.selection.track;
                const bool mixWas = integrated.state().mixView;
                strip->eq.onClick();
                check(integrated.arrangement.geometry.selection.track == strip->track() &&
                          integrated.state().mixView && integrated.state().mixViewTab == 1 &&
                          integrated.mixViewPanel.stack.focused() == strip->track(),
                      "a channel row's Q selects its track and opens the Mix View on STACK, on its row");
                if (!mixWas)
                    integrated.command(AppCommands::ToggleMixView);
                integrated.arrangement.geometry.selection.track = selectedBefore;
                integrated.publishSelection();
            }
        }
        auto &mixerView = integrated.mixerView;
        mixerView.tap = mixer.tap;
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
    // The Settings window in Live's shape (ADR-0203 d11, Master Reference V.1).
    {
        SettingsPanel panel(settings);
        panel.setSize(560, 680);
        const auto pages = settings::pages();
        check(!pages.empty() && panel.pageNames() == pages && panel.page() == pages.front(),
              "the Settings window lists the registry's pages in its order and opens on the first");
        std::size_t onAudio = 0;
        for (const auto &s : settings::registry())
            if (s.page == "Audio")
                ++onAudio;
        panel.showPage("Audio");
        check(panel.page() == "Audio" && panel.rows().size() == onAudio && panel.audioDevices() == nullptr,
              "one page at a time, every setting on it in the registry's order");
        auto rowFor = [&panel](const settings::Setting *s) -> SettingRow * {
            if (s == nullptr)
                return nullptr;
            panel.showPage(s->page);
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
        panel.search.setText("buffer", true);
        check(panel.finding() && !panel.rows().empty() &&
                  panel.rows().size() == settings::search("buffer").size(),
              "the Find box lists every matching setting, from every page");
        panel.search.setText({}, true);
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
        }
    }
    liveParity(commands, temp.path(), write);
    groupTracks(commands, temp.path());
    footRows(commands, temp.path());
    sendKnobs(commands, temp.path());
    audioDrop(commands, temp.path());
    rangeSelection(commands, temp.path());
    clipToTrack(commands, temp.path());
    mixViewShell(commands, temp.path());
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
