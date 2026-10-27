// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/juce/ui_shell.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
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
    AdiRootComponent a(view, submitter, mailbox, commands, persistence, "main"),
        b(view, submitter, mailbox, commands, persistence, "floating");
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
    AdiRootComponent readRoot(roView, roSubmit, roMailbox, commands, readonly, "main");
    const auto roOrder = readRoot.state().panels.slots()[0].panel;
    check(!readRoot.command(AppCommands::SwapPanels) &&
              readRoot.state().panels.slots()[0].panel == roOrder,
          "failed persistence rolls back visible order");
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
                                                     persistence, "zoomed"));
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
                                                        persistence, "tips"),
                     commands, false);
    check(window.tooltips() != nullptr && window.tooltipDelayMs() == 600,
          "floating tooltips are on, after 600 ms");
    check(settings.set("lookfeel.showTooltips", false, {}, why), "switch tooltips off");
    commands.reload();
    AdiWindow quiet(std::make_unique<AdiRootComponent>(view, submitter, mailbox, commands,
                                                       persistence, "quiet"),
                    commands, false);
    check(quiet.tooltips() == nullptr, "the setting turns them off for new windows");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
