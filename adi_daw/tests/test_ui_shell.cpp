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
        const auto area = root.arrangement.getBounds();
        g.fillRect(area.getX(), area.getY(), area.getWidth(), 28);
        int laneY = area.getY() + 28 - root.arrangement.geometry.scrollY;
        for (const auto &trackNode : root.reader().tracks()) {
            if (trackNode->kind == "master")
                continue;
            g.fillRect(area.getX() + 4, laneY + 4, std::max(0, area.getWidth() - 8), 24);
            laneY += root.arrangement.geometry.heightFor(trackNode->id);
        }
        for (const auto &device : root.devices.panels) {
            const auto bounds = root.getLocalArea(device.get(), device->getLocalBounds());
            g.fillRect(bounds.getX() + 2, bounds.getY() + 2, std::max(0, bounds.getWidth() - 4),
                       104);
            for (const auto &c : device->controls) {
                const auto b = root.getLocalArea(c.get(), c->getLocalBounds());
                g.fillRect(b.getX(), b.getY(), b.getWidth(), 20);
                g.fillRect(b.getX(), b.getY() + 42, b.getWidth(), 18);
                if (c->menu.isVisible())
                    g.fillRect(b.getX() + 4, b.getY() + 20, b.getWidth() - 8, 22);
            }
        }
        const auto mixerArea=root.mixer.getBounds();
        g.fillRect(mixerArea.getX()+6,mixerArea.getY()+5,mixerArea.getWidth()-12,26);
        for(const auto& strip:root.mixer.strips()) {
            if(!strip->isVisible())continue;
            const auto b=root.getLocalArea(strip.get(),strip->getLocalBounds());
            g.fillRect(b.getX()+2,b.getY()+2,b.getWidth()-4,22);
            g.fillRect(b.getRight()-72,b.getY()+27,54,52);
            g.fillRect(b.getX()+4,b.getY()+86,b.getWidth()-22,24);
        }
        const int dock = root.state().docked ? root.state().deviceHeight : 0;
        g.fillRect(0, root.getHeight() - dock + 10, root.getWidth(), 30);
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
    check(b.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)), "other window routes stop");
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
    commands.reload(settings);
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
    std::string documentError;
    auto document = ProjectDocument::open(temp.path() / "integrated.adi", true, {}, documentError);
    check(document != nullptr, "application document opens");
    if (document) {
        AdiRootComponent integrated(document->view(), document->ops(), document->mailbox(),
                                    commands, document->views(), "main");
        integrated.afterEdit = [&] { document->synchronise(); };
        integrated.applicationCommand = [&](int id) {
            return id == AppCommands::SaveProject && document->save(documentError);
        };
        check(document->ops().submit(track(2)).ok && document->synchronise(),
              "application edit reaches engine");
        check(integrated.undoTitle(false).find("Create track") != std::string::npos,
              "undo menu includes actual history label");
        check(integrated.keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)),
              "application undo key");
        check(document->session().model().tracks.size() == 1,
              "undo callback updates Session synchronously");
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
        check(integrated.keyPressed(juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0)),
              "Save key reaches live project checkpoint");
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
        auto &arrangement = integrated.arrangement;
        arrangement.geometry.selectedTrack = 2;
        OpRequest clip;
        clip.opType = "clip.create";
        clip.payload = {{"id", 1},         {"track", 2},
                        {"kind", "audio"}, {"name", "Rendered clip"},
                        {"pos", 0},        {"length", 4 * textproj::kPPQ}};
        check(document->ops().submit(clip).ok, "arrangement clip created through op");
        integrated.frame();
        const auto *hitTrack = arrangement.geometry.trackAt(integrated.reader(), 4);
        check(hitTrack && hitTrack->id == 2, "header and canvas rows exclude master");
        check(hitTrack && arrangement.geometry.hit(*hitTrack, 0) &&
                  !arrangement.geometry.hit(*hitTrack, 4 * textproj::kPPQ),
              "hit-test includes start and excludes end");
        arrangement.geometry.selectedClip = 1;
        arrangement.geometry.selectionStart = 0;
        arrangement.geometry.selectionEnd = 4 * textproj::kPPQ;
        const double beforeScale = arrangement.geometry.scale;
        check(integrated.keyPressed(juce::KeyPress('z')),
              "synthesised Z reaches fit-selection command");
        check(arrangement.geometry.scale != beforeScale, "Z changes visible range");
        check(integrated.keyPressed(juce::KeyPress('x')) &&
                  arrangement.geometry.scale == beforeScale,
              "X restores previous zoom");
        auto geometry = arrangement.geometry;
        const auto anchor = geometry.tick(70);
        geometry.zoom(2., 70);
        check(std::abs(geometry.tick(70) - anchor) <= 1, "pointer-anchored zoom preserves time");
        arrangement.geometry.insert = 0;
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
        arrangement.geometry.selectionStart = arrangement.geometry.selectionEnd = 0;
        arrangement.geometry.wheelHeight(integrated.reader(), 2, 8);
        check(arrangement.geometry.heightFor(2) == 72 && arrangement.geometry.height == 64,
              "Alt-wheel changes only target track");
        arrangement.changed();
        check(document->views().load("main").laneHeights.at(2) == 72,
              "track height persists in ui_view");
        arrangement.geometry.resizeTrack(2, 64);
        arrangement.changed();
        {
            UiPeerRoot peerRoot;
            peerRoot.setSize(integrated.getWidth(), integrated.getHeight());
            peerRoot.addAndMakeVisible(integrated);
            peerRoot.setVisible(true);
            peerRoot.addToDesktop(0);
            auto *peer = peerRoot.getPeer();
            check(peer != nullptr, "fake peer created without OS window");
            const auto origin =
                peerRoot.getLocalPoint(&arrangement.canvas, juce::Point<int>{10, 35}).toFloat();
            juce::int64 when = 100000;
            auto mouse = [&](juce::Point<float> point, int mods) {
                peer->handleMouseEvent(juce::MouseInputSource::InputSourceType::mouse, point,
                                       juce::ModifierKeys(mods), 1.f, 0.f, when += 100);
            };
            arrangement.geometry.selectedClip = 0;
            mouse(origin, 0);
            mouse(origin, juce::ModifierKeys::leftButtonModifier);
            mouse(origin, 0);
            check(arrangement.geometry.selectedClip == 1,
                  "peer dispatch hit-tests and selects painted clip");
            mouse(origin, juce::ModifierKeys::leftButtonModifier);
            mouse(origin.translated(48, 0), juce::ModifierKeys::leftButtonModifier);
            mouse(origin.translated(48, 0), 0);
            check(arrangement.geometry.selectionEnd == arrangement.geometry.tick(58),
                  "peer drag capture updates time selection");
            peerRoot.removeChildComponent(&integrated);
            peerRoot.removeFromDesktop();
        }
        auto publication = std::make_shared<ParameterPublication>();
        DevicePanelData panel;
        panel.id = 11;
        panel.track = 2;
        panel.name = "Test device";
        for (int i = 0; i < 3; ++i) {
            panel::Record record;
            record.id = std::to_string(i);
            record.name = "Control " + record.id;
            record.playing = .5;
            record.stored = .25;
            record.shape = static_cast<panel::Shape>(i);
            record.stepCount = i == 1 ? 2 : 4;
            record.text = "Plugin value";
            panel.records.push_back(record);
            panel.resolved.entries.push_back({record.id, false});
        }
        panel::finalize(panel.records);
        publication->push_back(panel);
        integrated.devices.publication = [publication] { return publication; };
        int gestures = 0;
        integrated.devices.gesture = [&](auto, const auto &, auto, double) {
            ++gestures;
            return true;
        };
        integrated.frame();
        check(integrated.devices.panels.size() == 1 &&
                  integrated.devices.panels[0]->controls.size() == 3,
              "selected chain presents all three declared shapes");
        auto &controls = integrated.devices.panels[0]->controls;
        check(controls[0]->slider.isVisible() && controls[1]->toggle.isVisible() &&
                  controls[2]->menu.isVisible(),
              "continuous switch menu use distinct controls");
        controls[1]->toggle.setToggleState(true, juce::dontSendNotification);
        controls[1]->toggle.onClick();
        check(gestures == 3, "switch sends bounded begin/value/end capture");
        integrated.devices.fold(11);
        check(document->views().load("main").foldedDevices.contains(11),
              "fold persists outside history");
        integrated.devices.fold(11);
        integrated.resizeDevices(1);
        check(integrated.state().deviceHeight == DesktopDefaults{}.deviceHeight,
              "strip minimum comes from desktop defaults");
        integrated.resizeDevices(10000);
        check(integrated.arrangement.getHeight() >= 28 + integrated.state().laneHeight,
              "strip ceiling leaves ruler and a row");
        integrated.resizeDevices(240);
        reference(integrated, "devices", write);
        allocations = 0;
        audit = true;
        for (int i = 0; i < 50; ++i)
            integrated.frame();
        audit = false;
        check(allocations == 0, "unchanged parameter publication frame allocates zero");
        arrangement.geometry.selectedTrack = 1;
        integrated.frame();
        check(integrated.devices.panels.empty(),
              "selection changes strip even with same parameter publication");
        arrangement.geometry.selectedTrack = 2;
        integrated.devices.publication = {};
        integrated.frame();
        integrated.state().deviceHeight = 200;
        integrated.resized();
        reference(integrated, "arrangement", write);
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
            bool alive = true;
            juce::Component dock, content;
            dock.addAndMakeVisible(content);
            content.setBounds(10, 20, 300, 180);
            FloatingHost host(
                integrated, commands, document->views(), document->view(),
                [&](std::int64_t) { return alive; }, false);
            check(host.detach("test", 11, content, dock), "detach existing component");
            check(host.content("test") == &content && content.getParentComponent() != &dock,
                  "detach reparents same identity");
            host.frame("test");
            host.frame("test");
            check(host.drains("test") == 2, "window drains on its own clock");
            host.close("test");
            check(content.getParentComponent() == &dock &&
                      content.getBounds() == juce::Rectangle<int>(10, 20, 300, 180),
                  "dock restores exact component and bounds");
            check(!document->views().loadFloating("test").open, "close clears saved open state");
            host.detach("deleted", 11, content, dock);
            alive = false;
            host.frame("deleted");
            host.collect();
            check(host.count() == 0 && !document->views().loadFloating("deleted").open,
                  "device retirement closes and persists closed");
            alive = true;
            host.collect();
            check(host.count() == 0, "undo does not resurrect a window");
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
            check(host.openView("analyser", 11, std::move(big),
                                [bigPtr](const SnapshotReader &) { bigPtr->frame(); }),
                  "analyser second view opens through generic host");
            check(small.model() == bigPtr->model(),
                  "docked and big analyser share one measurement model");
            std::array<float, 513> bins{};
            std::uint64_t revision = 0;
            for (int retry = 0; retry < 30 && !model->read(bins, revision); ++retry)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(revision > 0 && bins[100] < -90 && std::abs(bins[32]) < .01,
                  "analyser worker calibrated floor before 0 dB sine");
            small.setSize(500, 200);
            small.frame();
            juce::Image render(juce::Image::ARGB, 500, 200, true, juce::SoftwareImageType{});
            juce::Graphics g(render);
            small.paintEntireComponent(g, true);
            auto stream = juce::File::getCurrentWorkingDirectory()
                              .getChildFile("analyser-actual.png")
                              .createOutputStream();
            check(stream && juce::PNGImageFormat{}.writeImageToStream(render, *stream),
                  "analyser software render saved to file");
            const std::string analyserKey="analyser";
            host.frame(analyserKey);
            allocations = 0;
            audit = true;
            for (int i = 0; i < 20; ++i)
                host.frame(analyserKey);
            audit = false;
            std::printf("METRIC floating frame allocations %u\n",allocations.load());
            check(allocations == 0, "floating analyser frame allocates zero");
            host.close("analyser");
        }
        integrated.frame();
        auto& mixer=integrated.mixer;
        check(!mixer.strips().empty() && mixer.strips()[0]->track()==integrated.reader().tracks()[0]->id,"mixer follows the arrangement snapshot order");
        auto& strip=*mixer.strips()[0];
        const auto id=strip.track();
        strip.volume.onDragStart();strip.volume.setValue(-12,juce::sendNotificationSync);
        mixer.scroll(100);
        check(strip.track()==id,"scroll cannot retarget an active fader");
        strip.volume.onDragEnd();integrated.frame();
        check(std::abs(document->view().current()->findTrack(id)->volumeDb+12)<.01,"fader commits an undoable op");
        integrated.command(AppCommands::Undo);integrated.frame();
        check(std::abs(document->view().current()->findTrack(id)->volumeDb)<.01,"undo restores fader");
        strip.pan.setValue(.4,juce::sendNotificationSync);integrated.frame();
        check(std::abs(document->view().current()->findTrack(id)->pan-.4)<.01,"pan text or keyboard edit commits");
        strip.pan.setValue(0,juce::sendNotificationSync);integrated.frame();
        strip.active.onClick();integrated.frame();
        check(document->view().current()->findTrack(id)->muted,"activator commits mute");
        strip.active.onClick();integrated.frame();
        check(mixer.strips().size()<=static_cast<std::size_t>(mixer.getHeight()/132+1),"mixer realises only visible span and margin");
        reference(integrated,"mixer",write);
        allocations=0;audit=true;
        for(int i=0;i<50;++i)integrated.frame();
        audit=false;check(allocations==0,"steady mixer frame allocates zero");
        document->release();
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
