// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/juce/ui_channel_eq.hpp"
#include "../src/juce/ui_floating.hpp"
#include "../src/juce/ui_settings.hpp"
#include "../src/juce/ui_shell.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include "ui_fake_peer.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
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
        g.fillRect(area.getX(), area.getY() + ArrangementView::kOverviewHeight, area.getWidth(),
                   ArrangementView::kRulerHeight);
        const auto headers = root.arrangement.headers.getBounds() + area.getPosition();
        int laneY = headers.getY() - root.arrangement.geometry.scrollY;
        for (const auto &trackNode : root.reader().tracks()) {
            g.fillRect(headers.getX() + 6, laneY + 4, std::max(0, headers.getWidth() - 8), 24);
            laneY += root.arrangement.geometry.heightFor(trackNode->id);
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
        g.fillRect(canvas.withTrimmedTop(std::max(0, laneY - canvas.getY())));
        if (root.mixer.isVisible() && !root.mixer.vertical()) {
            const auto m = root.mixer.getBounds();
            g.fillRect(m.getRight() - MixerPanel::kRightStrip - 44, m.getY() + 3, 42, 17);
            g.fillRect(m.withTrimmedLeft(m.getWidth() - MixerPanel::kRightStrip));
        }
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
    auto store = Store::create(dir / "parity.adi", err);
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
              root.mixer.getWidth() == MixerPanel::kFullWidth && root.devices.panels.size() == 2,
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
        ChannelEqView eqView(root);
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
        const auto file = juce::File(folder).getChildFile("adi-channel-eq.png");
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
}
} // namespace
int main(int argc, char **argv) {
    juce::ScopedJuceInitialiser_GUI init;
    const bool write = argc == 2 && std::string(argv[1]) == "--write-references";
    test::TempDirectory temp("ui_shell", "root");
    StoreError err{};
    auto store = Store::create(temp.path() / "p.adi", err);
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
        check(items("Options") == std::vector<int>{C::Follow}, "Follow is the Options menu's (§6.2)");
        check(items("File") == std::vector<int>{C::NewProject, C::OpenProject, 0,
                                                C::TakeSnapshot, C::SaveAs},
              "the File menu; Quit is the application menu's on the Mac");
#else
        check(names == juce::StringArray{"File", "Edit", "Create", "View", "Options", "Navigate"},
              "the menu bar: File, Edit, Create, View, Options, Navigate");
        check(items("File") == std::vector<int>{C::NewProject, C::OpenProject, 0,
                                                C::TakeSnapshot, C::SaveAs, 0, C::Quit},
              "the File menu ends with Quit and no longer holds Settings");
        check(items("Options") == std::vector<int>{C::Follow, 0, C::Settings},
              "the Options menu: Follow (§6.2), then Settings off the Mac (§2.3)");
#endif
        check(items("Edit") == std::vector<int>{C::Undo, C::Redo, 0, C::DeleteTrack, 0,
                                                C::RenameTrack},
              "the Edit menu deletes the selected track, before Rename (§18.2)");
        check(items("Create") == std::vector<int>{C::InsertAudioTrack, C::InsertMidiTrack},
              "the Create menu inserts tracks");
        check(items("Navigate") == std::vector<int>{C::FocusControlBar, C::FocusArrangement,
                                                    C::FocusDeviceView, C::FocusBrowser, 0,
                                                    C::TabMovesFocus},
              "the Navigate menu moves the focus to the views ADI has, then Tab's switch (§41.4.1)");
        check(!names.contains("Help"), "a menu with no command yet is left out");
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
        const auto *mainRow = arrangement.geometry.trackAt(integrated.reader(), 4);
        const auto *hitTrack =
            arrangement.geometry.trackAt(integrated.reader(), arrangement.geometry.height + 4);
        check(mainRow && mainRow->id == 1 && hitTrack && hitTrack->id == 2,
              "every track is a row, in the snapshot's order");
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
                  handler->getTitle().startsWith("Track, clip at bar 1 beat 1") &&
                  handler->getTitle().contains("4.00 beats"),
              "a screen reader hears the clip's track, start and length");
        arrangement.geometry.selection.track = 0;
        arrangement.geometry.selection.clip = 0;
        arrangement.changed();
        check(handler && handler->getActions().invoke(juce::AccessibilityActionType::press) &&
                  arrangement.geometry.selection.clip == 1 &&
                  document->sessionState().selection().clip == 1,
              "its press action selects the clip, in the session");
        handler.reset();
        arrangement.geometry.selection.clip = 0;
        check(arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) &&
                  arrangement.canvas.lastAnnouncement.contains("clip at bar 1 beat 1"),
              "moving the selection by key announces where it landed");
        arrangement.geometry.left = 100;
        arrangement.changed();
        shown = 0;
        for (const auto &child : arrangement.canvas.accessible())
            if (child->isVisible())
                ++shown;
        check(shown == 0, "a clip scrolled off screen has no accessible child");
        arrangement.geometry.left = 0;
        arrangement.changed();
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
        arrangement.canvas.filesDropped(juce::StringArray{"kick.wav"}, 10, 70);
        check(!integrated.error().empty() &&
                  document->store().db().execAndGet("SELECT count(*) FROM ops").getInt() ==
                      opsBeforeDrop,
              "an audio drop says it waits for the engine and writes nothing");
        {
            UiPeerRoot peerRoot;
            peerRoot.setSize(integrated.getWidth(), integrated.getHeight());
            peerRoot.addAndMakeVisible(integrated);
            peerRoot.setVisible(true);
            peerRoot.addToDesktop(0);
            auto *peer = peerRoot.getPeer();
            check(peer != nullptr, "fake peer created without OS window");
            const auto origin =
                peerRoot.getLocalPoint(&arrangement.canvas, juce::Point<int>{10, 64 + 35})
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
            check(arrangement.geometry.selection.end == arrangement.geometry.tick(58),
                  "peer drag capture updates time selection");
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
            const auto heightTrack = integrated.reader().tracks().front()->id;
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
            check(integrated.command(AppCommands::TabMovesFocus) && !commands.tabMovesFocus(),
                  "and off again");
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
        int lane = integrated.arrangement.getY() + integrated.arrangement.canvas.getY() -
                   arrangement.geometry.scrollY;
        for (const auto &t : integrated.reader().tracks()) {
            if (t->id == 2)
                break;
            lane += arrangement.geometry.heightFor(t->id);
        }
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
            check(row->pan.isDoubleClickReturnEnabled() &&
                      row->pan.getDoubleClickReturnValue() == 0,
                  "a double-click resets the pan to centre");
            row->pan.setValue(.4, juce::sendNotificationSync);
            check(std::abs(document->view().current()->findTrack(2)->pan - .4) < .01,
                  "a typed or keyboard pan commits at once");
            row->pan.setValue(0, juce::sendNotificationSync);
            row->active.onClick();
            integrated.frame();
            check(document->view().current()->findTrack(2)->muted &&
                      !row->active.getToggleState() && row->active.getButtonText() == "2",
                  "the Track Activator, numbered by its place as Live's, mutes the track");
            row->active.onClick();
            row->solo.onClick();
            integrated.frame();
            check(document->view().current()->findTrack(2)->soloed, "Solo solos it");
            row->solo.onClick();
            const auto opsBefore =
                document->store().db().execAndGet("SELECT count(*) FROM ops").getInt();
            row->eq.onClick();
            row->limiter.onClick();
            check(!row->eq.getToggleState() && !row->limiter.getToggleState() &&
                      !integrated.error().empty() &&
                      document->store().db().execAndGet("SELECT count(*) FROM ops").getInt() ==
                          opsBefore,
                  "the ADI-Q and ADI-L slots are on every strip, present and off");
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
        // ADI's Channel EQ window (ADR-0204 d1): every channel's ADI-Q and ADI-L in one view.
        {
            ChannelEqView eqView(integrated);
            SnapshotReader eqReader(document->view().current());
            eqView.frame(eqReader);
            const auto &all = integrated.reader().tracks();
            bool ordered = !all.empty() && eqView.rows().size() == all.size();
            for (std::size_t i = 0; ordered && i < all.size(); ++i)
                ordered = eqView.rows()[i]->track() == all[i]->id;
            check(ordered, "the Channel EQ window lists every channel, in the snapshot's order");
            allocations = 0;
            audit = true;
            eqView.frame(eqReader);
            audit = false;
            check(allocations == 0, "a frame that changes no channel builds nothing");
            if (eqView.rows().size() >= 2) {
                auto &second = *eqView.rows()[1];
                second.eq.onClick();
                check(eqView.focused() == second.track() && !integrated.error().empty() &&
                          !second.eq.getToggleState(),
                      "a slot's switch focuses its channel, says what it waits for and stays off");
            }
            MixerStrip *strip = nullptr;
            for (const auto &s : mixer.strips())
                if (s->isVisible() && s->track() != integrated.arrangement.geometry.selection.track)
                    strip = s.get();
            if (strip) {
                const auto selectedBefore = integrated.arrangement.geometry.selection.track;
                strip->eq.onClick();
                check(integrated.arrangement.geometry.selection.track == strip->track() &&
                          !integrated.error().empty(),
                      "a channel row's Q selects its track and opens the window, or says why it cannot");
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
        const auto &trackOrder = integrated.reader().tracks();
        check(columns.size() >= 2 && columns.size() < trackOrder.size() &&
                  columns[0]->track() == trackOrder[0]->id &&
                  columns[1]->track() == trackOrder[1]->id &&
                  columns[1]->getX() == columns[0]->getX() + MixerPanel::kStripWidth,
              "one strip per track, left to right, only those on screen");
        bool numbered = true;
        for (std::size_t i = 0; i < columns.size(); ++i)
            numbered = numbered &&
                       columns[i]->active.getButtonText() == juce::String(static_cast<int>(i) + 1);
        check(numbered, "each activator shows its track's place, whatever the track's id");
        // Live's peak level (manual 18.1): the highest peak since a click, in dB.
        if (columns.size() >= 2) {
            auto &held = columns[1]->peakHold;
            check(columns[1]->track() == 2 && held.isVisible() && std::abs(held.held - .5f) < .01f,
                  "the Mixer View's peak level holds the track's highest peak");
            held.onClick();
            check(held.held == 0, "a click resets it");
            integrated.frame();
            check(std::abs(held.held - .5f) < .01f, "and it holds the next peak it meets");
            check(!columns[0]->peakHold.isVisible() || columns[0]->peakHold.held == 0,
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
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
