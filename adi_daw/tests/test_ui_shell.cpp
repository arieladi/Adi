// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/juce/ui_floating.hpp"
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
        for (const auto &strip : root.mixer.strips()) {
            if (!strip->isVisible())
                continue;
            for (juce::Component *c : std::initializer_list<juce::Component *>{
                     &strip->active, &strip->solo, &strip->eq, &strip->limiter})
                g.fillRect(root.getLocalArea(c, c->getLocalBounds()));
            const auto v = root.getLocalArea(&strip->volume, strip->volume.getLocalBounds());
            g.fillRect(v.getRight() - 50, v.getY(), 50, v.getHeight());
        }
        for (const auto &device : root.devices.panels) {
            const auto b = root.getLocalArea(device.get(), device->getLocalBounds());
            g.fillRect(b.getX() + 22, b.getY() + 1, std::max(0, b.getWidth() - 23), 20);
            for (const auto &c : device->controls) {
                const auto cb = root.getLocalArea(c.get(), c->getLocalBounds());
                g.fillRect(cb.getX(), cb.getY(), cb.getWidth(), 14);
                g.fillRect(cb.getX(), cb.getY() + 32, cb.getWidth(), 14);
                if (c->menu.isVisible())
                    g.fillRect(cb.getX() + 2, cb.getY() + 14, cb.getWidth() - 4, 18);
            }
        }
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
    check(a.transport.play.getButtonText() == "Play / stop" &&
              a.transport.stop.getButtonText() == "Stop",
          "button labels");
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
              a.state().panels.find(Panel::Browser)->width == 240,
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
    check(a.state().deviceFloor == 169 && a.deviceArea().getHeight() == 169,
          "the device strip starts at its floor");
    a.state().setDeviceHeight(40);
    check(a.state().deviceHeight == 169, "the strip never goes below its floor");
    a.state().setDeviceHeight(300);
    a.resized();
    check(a.deviceArea().getHeight() == 300, "the strip grows upward");
    a.setSize(1200, 300);
    check(a.deviceArea().getHeight() == 169, "a short window keeps the floor");
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
    check(a.keyPressed(juce::KeyPress('m', cmd | alt, 0)) &&
              a.state().panels.find(Panel::Mixer)->collapsed,
          "Ctrl/Cmd+Alt+M hides the mixer");
    check(a.keyPressed(juce::KeyPress('m', cmd | alt, 0)), "and shows it");
    check(a.keyPressed(juce::KeyPress('4', cmd | alt, 0)) && !a.state().docked &&
              a.deviceArea().isEmpty(),
          "Ctrl/Cmd+Alt+4 hides the Device View");
    check(a.keyPressed(juce::KeyPress('4', cmd | alt, 0)) && a.state().docked &&
              a.deviceArea().getHeight() == 300,
          "and shows it at its own height");
    check(!a.state().aiChatOpen && a.aiTab.isVisible() && a.chatArea().getHeight() == 24 &&
              a.chatArea().getBottom() == a.browserArea().getBottom(),
          "the AI chat is a collapsed tab at the browser's foot");
    a.aiTab.onClick();
    check(a.state().aiChatOpen && a.chatArea().getHeight() > 24 &&
              persistence.load("main").aiChatOpen,
          "the tab opens and is remembered per window");
    a.aiTab.onClick();
    check(commands.infoView() && a.infoArea().getX() == 0 &&
              a.infoArea().getBottom() == a.status.getY() &&
              a.infoArea().getRight() == a.deviceArea().getX(),
          "the Info View is on, bottom-left, beside the Device View");
    a.hover(&a.transport.play);
    check(a.infoText().contains("Play") && a.infoText().contains("Space"),
          "the Info View describes the element under the mouse");
    a.hover(nullptr);
    check(a.infoText().isEmpty(), "and clears when nothing is under it");
    check(a.keyPressed(juce::KeyPress('?', juce::ModifierKeys::shiftModifier, 0)) &&
              !commands.infoView() && a.infoArea().isEmpty() && a.deviceArea().getX() == 0,
          "Shift+? hides the Info View");
    b.frame();
    check(b.infoArea().isEmpty(), "the Info View is app-scoped: every window follows");
    check(settings.get("lookfeel.showInfoView") == false, "and the App setting records it");
    a.status.infoToggle.onClick();
    check(commands.infoView() && !a.infoArea().isEmpty(), "the Status Bar's toggle shows it");
    a.setSize(400, 540);
    check(!a.command(AppCommands::SwapPanels) && !a.error().empty(),
          "a refused swap says why in the Status Bar");
    check(a.status.getBounds() == juce::Rectangle<int>(0, 540 - 22, 400, 22),
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
    check(zoomed.deviceArea().getHeight() == 169, "the floor is in zoom-independent units");
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
    // Step 7.2's application half (#198): a real document, its engine and its keys.
    std::string documentError;
    auto document = ProjectDocument::open(temp.path() / "integrated.adi", true, {}, documentError);
    check(document != nullptr, "application document opens");
    if (document) {
        AdiRootComponent integrated(document->view(), document->ops(), document->mailbox(),
                                    commands, document->views(), document->sessionState(),
                                    "main");
        int snapshots = 0, saveAs = 0;
        integrated.afterEdit = [&] { document->synchronise(); };
        integrated.applicationCommand = [&](int id) {
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
                      "audio #-Audio" &&
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
                  headers.renaming() == 3 && headers.editor.getText() == "#-Audio",
              "Tab commits and moves on to the next track's title");
        headers.editor.setText("Gone", false);
        check(headers.editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) &&
                  value(document->store(), "SELECT name FROM tracks WHERE id=3") == "#-Audio" &&
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
        const auto *hitTrack = arrangement.geometry.trackAt(integrated.reader(), 64 + 4);
        check(mainRow && mainRow->id == 1 && hitTrack && hitTrack->id == 2,
              "every track is a row, in the snapshot's order");
        check(hitTrack && arrangement.geometry.hit(*hitTrack, 0) &&
                  !arrangement.geometry.hit(*hitTrack, 4 * textproj::kPPQ),
              "hit-test includes start and excludes end");
        check(arrangement.headers.getX() > arrangement.canvas.getX() &&
                  arrangement.headers.getRight() == arrangement.getWidth() &&
                  arrangement.canvas.getX() == 0,
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
            check(overview.getY() == 0 &&
                      arrangement.ruler.getY() == ArrangementView::kOverviewHeight &&
                      arrangement.canvas.getY() == ArrangementView::kTopHeight,
                  "the Overview sits above the beat-time ruler");
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
        check(integrated.devices.getHeight() == 168 && controls[1]->getX() == controls[0]->getX() &&
                  controls[1]->getY() > controls[0]->getY() &&
                  controls[2]->getX() > controls[0]->getX(),
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
        integrated.resizeDevices(1);
        check(integrated.state().deviceHeight == DesktopDefaults{}.deviceFloor,
              "the strip's floor comes from the desktop defaults");
        integrated.resizeDevices(10000);
        check(integrated.arrangement.getHeight() >=
                  ArrangementView::kTopHeight + integrated.state().laneHeight,
              "the strip's ceiling leaves the ruler and a row");
        integrated.resizeDevices(240);
        check(integrated.devices.getHeight() == 239 &&
                  controls[2]->getX() == controls[0]->getX(),
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
        check(row != nullptr && mixer.getX() == integrated.arrangement.getRight() + 1,
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
                  "the Track Activator, numbered as Live's, mutes the track");
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
        document->release();
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
