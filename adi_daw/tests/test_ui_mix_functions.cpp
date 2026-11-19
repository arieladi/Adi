// SPDX-License-Identifier: AGPL-3.0-or-later
// Retained function controls and prepared drag targets, with a software peer only.
// No native host, OS window, desktop input, capture or audio device is involved.
#include "../src/juce/ui_shell.hpp"
#include "../src/juce/ui_look.hpp"
#include "juce/builtin_host.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include "ui_fake_peer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace adi;
using namespace adi::ui;
namespace {
int checks = 0, failures = 0;
void check(bool value, const char *what) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", what); }
}
struct MenuItem {
    juce::String name;
    bool enabled = false, ticked = false;
    std::function<void()> action;
};
std::vector<MenuItem> menuItems(const juce::PopupMenu &menu) {
    std::vector<MenuItem> items;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
        const auto &item = it.getItem();
        items.push_back({item.text, item.isEnabled, item.isTicked, item.action});
    }
    return items;
}
DevicePanelData makePanel(std::int64_t id, bool plugin, const char *name) {
    DevicePanelData data;
    data.id = id; data.track = 2; data.chain = 22; data.instance = 71;
    data.available = true; data.name = name; data.uid = "org.adidaw.mix.limiter";
    data.plugin = plugin; data.latency = 476;
    data.declared.push_back({"gain", "Gain", true});
    data.resolved.entries.push_back({"gain", false});
    data.resolved.modifiableCount = 1;
    panel::Record record;
    record.id = "gain"; record.name = "Gain"; record.stored = record.playing = .25;
    record.text = "6.0 dB";
    data.records.push_back(record); data.defaults["gain"] = .25;
    return data;
}
struct Event {
    std::int64_t device;
    engine::ParamEventKind kind;
    std::uint64_t instance;
    double value;
    std::string parameter{};
};
void functions(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto document = ProjectDocument::open(directory / "function-tabs.adi", true, {}, error);
    check(document != nullptr, "component project opens without a native host");
    if (!document) return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "function-tabs");
    root.setSize(1200, 800);
    UiPeerRoot peer;
    peer.setSize(1200, 800); peer.setVisible(true); peer.addToDesktop(0);
    check(peer.getPeer() && !peer.getPeer()->getNativeHandle(), "test has only UiFakePeer and no OS window");
    peer.addAndMakeVisible(root);
    auto publication = std::make_shared<ParameterPublication>();
    publication->push_back(makePanel(21, true, "External limiter"));
    publication->push_back(makePanel(20, false, "Limiter"));
    auto &strip = root.devices;
    strip.setVisible(true);
    strip.publication = [&] { return publication; };
    strip.targetCurrent = [&](std::int64_t device, const engine::DeviceRuntimeToken &, std::uint64_t stamp) {
        return std::any_of(publication->begin(), publication->end(), [&](const auto &data) {
            return data.id == device && data.instance == stamp && data.available;
        });
    };
    std::vector<Event> events;
    bool rejectValue = false;
    strip.guardedGesture = [&](std::int64_t device, const std::string &, engine::ParamEventKind kind,
                                double value, const engine::DeviceRuntimeToken &, std::uint64_t stamp) {
        events.push_back({device, kind, stamp, value});
        return kind != engine::ParamEventKind::Value || !rejectValue;
    };
    std::string op;
    Payload payload;
    strip.submit = [&](const std::string &name, Payload data) { op = name; payload = std::move(data); return true; };
    int additions = 0;
    mix::Function added = mix::Function::Eq;
    strip.functionAvailability = [](mix::Function kind) {
        return kind == mix::Function::Limiter || kind == mix::Function::Compressor
            ? mix::Availability{true, {}} : mix::Availability{false, "Core pending"};
    };
    strip.addFunction = [&](mix::Function kind, std::int64_t track) {
        ++additions; added = kind; return track == 2;
    };
    int moves = 0;
    DevicePanelData moved;
    std::optional<std::int64_t> movedBefore;
    strip.moveFunction = [&](const DevicePanelData &expected, std::int64_t track, std::optional<std::int64_t> before) {
        ++moves; moved = expected; movedBefore = before; return track == 2;
    };
    strip.frame(2);
    check(strip.addTab.isVisible() && strip.panels.size() == 2, "chain-end add tab appears beside actual authored devices");
    if (strip.panels.size() != 2) return;
    auto *ordinary = strip.panels[0].get();
    auto *native = strip.panels[1].get();
    check(!ordinary->nativeFunction() && native->nativeFunction() &&
              native->nativeFunction()->kind == mix::Function::Limiter,
          "external UID coincidence remains an ordinary plug-in; native identity is explicit");
    check(native->parameterPages.empty(), "a plain Limiter without group descriptors invents no Clip page");
    {
        DevicePanelView card(strip, publication->back(), true);
        card.setSize((look::workspaceSegmentWidth * 3), look::mixStackRow);
        card.present(publication->back());
        const auto *gain = card.controls.empty() ? nullptr : card.controls.front().get();
        check(gain && gain->isVisible() && gain->getBottom() <= card.getHeight() &&
                  !card.configureButton.isVisible(), "the compact Limiter retains its actual Gain control");
        card.setSize((look::workspaceSegmentWidth * 3) * 2, look::mixStackFocusedRow);
        check(gain && card.controls.front().get() == gain && gain->isVisible(),
              "the expanded card uses the same owned control as the compact card");
    }
    check(!ordinary->beginFunctionDrag(), "ordinary plug-ins cannot enter the native function drag path");
    int duplicates = 0;
    strip.duplicate = [&](std::int64_t, std::int64_t, std::optional<std::int64_t>) { ++duplicates; return true; };
    const auto commandItems = menuItems(native->commands());
    const auto duplicate = std::find_if(commandItems.begin(), commandItems.end(), [](const auto &item) { return item.name == "Duplicate"; });
    check(duplicate != commandItems.end() && !duplicate->enabled &&
              !strip.clipboard(DeviceChainStrip::Clip::Duplicate, native->id) && duplicates == 0,
          "native function duplication cannot evade the one-function-per-channel rule");
    check(native->title.getTooltip().contains("476 samples") && native->title.getTooltip().contains("Native Mix View"),
          "native panel says its real latency and movable function scope");
    auto items = menuItems(strip.functionMenu());
    check(items.size() == 5, "add menu has exactly the five director-confirmed functions");
    if (items.size() != 5) return;
    check(!items[0].enabled && items[0].name.contains("Core pending") && items[1].enabled && !items[1].ticked &&
              items[4].ticked && !items[4].enabled,
          "menu distinguishes absent unavailable cores, addable core and already-present native function");
    auto staleAction = items[1].action;
    strip.cancelFunctionInteractions();
    staleAction();
    check(additions == 0, "cancelled add-menu action cannot insert a function later");
    items = menuItems(strip.functionMenu()); items[1].action();
    check(additions == 1 && added == mix::Function::Compressor, "current enabled menu action calls shared insertion once");
    staleAction = menuItems(strip.functionMenu())[1].action;
    strip.frame(3); strip.frame(2);
    staleAction();
    check(additions == 1, "leaving and returning to the channel cannot revive its prior add menu");
    ordinary = strip.panels[0].get(); native = strip.panels[1].get();
    native->enabled.onClick();
    check(op == "device.remove" && payload.at("id").get<std::int64_t>() == 20,
          "native function deactivation removes it through the shared undoable device op");
    ordinary->enabled.onClick();
    check(op == "device.setEnabled" && payload.at("id").get<std::int64_t>() == 21,
          "ordinary plug-in activator keeps ordinary bypass semantics");
    check(native->beginFunctionDrag() && native->functionDragTarget() &&
              native->functionDragTarget()->id == 20 && native->functionDragTarget()->instance == 71,
          "a function drag retains its authored identity and current instance");
    check(strip.dropFunction(*native, 21) && moves == 1 && moved.id == 20 && moved.instance == 71 && movedBefore == 21 &&
              !native->functionDragTarget(),
          "drop forwards one captured move and clears its drag owner before the callback");
    check(native->beginFunctionDrag(), "a second drag captures the displayed instance");
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->back().instance = 72;
    check(!strip.dropFunction(*native, 21) && moves == 1, "publication replacement rejects an old drag before mutation");
    strip.frame(2);
    native = strip.panels[1].get();
    check(native->presentedInstance() == 72 && !native->functionDragTarget(), "new runtime rebuilds the retained function panel");
    if (native->controls.size() != 1) { check(false, "native descriptor makes its real parameter control"); return; }
    auto *control = native->controls.front().get();
    events.clear();
    control->slider.onDragStart(); control->slider.setValue(.4, juce::sendNotificationSync); control->slider.onDragEnd();
    check(events.size() == 3 && events[0].kind == engine::ParamEventKind::Begin &&
              events[1].kind == engine::ParamEventKind::Value && events[2].kind == engine::ParamEventKind::End &&
              events[0].instance == 72 && events[2].value == .4,
          "one native drag uses the common exact-instance begin/value/end route");
    events.clear();
    control->slider.onDragStart(); control->slider.setValue(.5, juce::sendNotificationSync);
    strip.cancelFunctionInteractions(); control->slider.onDragEnd();
    check(events.size() == 3 && events.back().kind == engine::ParamEventKind::End && events.back().value == .5,
          "owner cancellation closes its gesture once; a late release cannot close another owner");
    events.clear();
    control->slider.onDragStart(); rejectValue = true;
    control->slider.setValue(.6, juce::sendNotificationSync); control->slider.onDragEnd();
    check(events.size() == 3 && events.back().kind == engine::ParamEventKind::End && events.back().value == .5,
          "rejected parameter move closes at its last accepted value");
    rejectValue = false;
    events.clear();
    control->slider.onDragStart();
    publication = std::make_shared<ParameterPublication>(*publication); publication->back().instance = 73;
    strip.cancelFunctionInteractions(); control->slider.onDragEnd();
    check(events.size() == 2 && events.back().kind == engine::ParamEventKind::End && events.back().instance == 72,
          "stale cancellation reaches the retained old owner rather than the replacement runtime");
    strip.frame(2); native = strip.panels[1].get(); control = native->controls.front().get();
    events.clear();
    control->slider.onDragStart(); peer.setVisible(false);
    check(events.size() == 2 && events.back().kind == engine::ParamEventKind::End,
          "ancestor hiding ends a native gesture without a frame or native input");
    peer.removeChildComponent(&root); peer.removeFromDesktop();
}
DevicePanelData makePagedPanel() {
    auto data = makePanel(30, false, "Clip / Limiter");
    data.declared.clear(); data.resolved.entries.clear(); data.records.clear(); data.defaults.clear();
    const auto add = [&](const char *id, const char *name, const char *group, const char *groupName, bool enable, double value) {
        data.declared.push_back({id, name, true}); data.resolved.entries.push_back({id, false});
        panel::Record r; r.id = id; r.name = name; r.playing = r.stored = value;
        if (enable) { r.shape = panel::Shape::Switch; r.stepCount = 2; }
        data.records.push_back(r); data.defaults[id] = value;
        data.parameterGroups[id] = {group, groupName, enable};
    };
    // Existing Limiter ids may come first in the backend; display order still follows audio.
    add("lgain", "Limiter Gain", "limiter", "Limiter", false, .25);
    add("lenable", "Limiter On", "limiter", "Limiter", true, 1.);
    add("cgain", "Clip Gain", "clip", "Clip", false, .25);
    add("cenable", "Clip On", "clip", "Clip", true, 1.);
    add("lexport", "Limiter Export Quality", "limiter", "Limiter", false, 0.);
    data.declared.back().automatable = false;
    data.records.back().shape = panel::Shape::Switch; data.records.back().stepCount = 2;
    add("readonly", "Read-only Status", "limiter", "Limiter", false, .25);
    data.declared.back().modifiable = false; data.declared.back().automatable = false;
    data.resolved.modifiableCount = data.records.size();
    return data;
}
void parameterPages(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto document = ProjectDocument::open(directory / "parameter-pages.adi", true, {}, error);
    check(document != nullptr, "page component project opens headlessly");
    if (!document) return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "parameter-pages");
    root.setSize(1200, 800);
    UiPeerRoot peer;
    peer.setSize(1200, 800); peer.setVisible(true); peer.addToDesktop(0); peer.addAndMakeVisible(root);
    auto publication = std::make_shared<ParameterPublication>(); publication->push_back(makePagedPanel());
    auto &strip = root.devices;
    strip.setVisible(true); strip.publication = [&] { return publication; };
    strip.targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &, std::uint64_t instance) {
        return publication->front().id == id && publication->front().instance == instance && publication->front().available;
    };
    std::vector<Event> events;
    bool reject = false;
    strip.guardedGesture = [&](std::int64_t id, const std::string &parameter, engine::ParamEventKind kind,
                                double value, const engine::DeviceRuntimeToken &, std::uint64_t instance) {
        events.push_back({id, kind, instance, value, parameter});
        if (kind == engine::ParamEventKind::Value && reject) return false;
        if (kind == engine::ParamEventKind::Value) {
            publication = std::make_shared<ParameterPublication>(*publication);
            for (auto &r : publication->front().records) if (r.id == parameter) r.stored = r.playing = value;
        }
        return true;
    };
    int actions = 0;
    strip.submit = [&](const std::string &, Payload) { ++actions; return true; };
    strip.frame(2);
    auto *panel = strip.panels.front().get();
    const auto findControl = [&](const std::string &id) -> ParameterControl * {
        const auto at = std::find_if(panel->controls.begin(), panel->controls.end(), [&](const auto &c) { return c->parameter == id; });
        return at == panel->controls.end() ? nullptr : at->get();
    };
    auto *clip = findControl("cgain"), *limiter = findControl("lgain");
    check(panel->parameterPages.size() == 2 && panel->parameterPages[0].id == "clip" &&
              panel->parameterPages[1].id == "limiter" && panel->selectedParameterPage() == "limiter",
          "only provided pages appear in serial Clip then Limiter order, with Limiter selected by default");
    if (!clip || !limiter || panel->parameterPages.size() != 2) return;
    auto *configuration = findControl("lexport"), *readOnly = findControl("readonly");
    check(configuration && configuration->isEnabled() && readOnly && !readOnly->isEnabled() &&
              !publication->front().declared[4].automatable,
          "explicit configuration is editable without automation eligibility, while read-only fields stay disabled");
    events.clear();
    if (configuration) {
        configuration->toggle.setToggleState(true, juce::dontSendNotification);
        configuration->toggle.onClick();
    }
    check(events.size() == 3 && events[1].parameter == "lexport" && events[1].value == 1.,
          "configuration toggle uses the same exact-target Begin Value End callback");
    events.clear();
    if (readOnly) readOnly->slider.onDragStart();
    check(events.empty(), "disabled read-only parameter cannot start a retained edit callback");
    strip.frame(2);
    check(!clip->isVisible() && limiter->isVisible() && !findControl("cenable")->isVisible() &&
              panel->parameterPages[0].enabled->getToggleState() && panel->parameterPages[1].enabled->getToggleState(),
          "selected page filters controls while both real enable parameters remain on");
    events.clear();
    check(panel->chooseParameterPage("clip") && clip->isVisible() && !limiter->isVisible() && events.empty() && actions == 0,
          "page selection changes presentation without processing, parameter writes or device ops");
    panel->chooseParameterPage("limiter");
    check(panel->setParameterPageEnabled("clip", false), "Clip can switch off independently of selected page");
    strip.frame(2);
    check(events.size() == 3 && events[0].parameter == "cenable" && events[1].value == 0 &&
              events[2].kind == engine::ParamEventKind::End && !panel->parameterPages[0].enabled->getToggleState() &&
              panel->parameterPages[1].enabled->getToggleState() && panel->selectedParameterPage() == "limiter" && actions == 0,
          "page enable uses one common begin/value/end gesture and retains the other stage");
    events.clear(); panel->chooseParameterPage("clip");
    check(events.empty() && !panel->parameterPages[0].enabled->getToggleState() &&
              panel->parameterPages[1].enabled->getToggleState(), "selecting inactive Clip never enables it or changes the active Limiter");
    panel->chooseParameterPage("limiter");
    events.clear(); panel->setParameterPageEnabled("limiter", false); strip.frame(2);
    panel->setParameterPageEnabled("clip", true); strip.frame(2);
    check(panel->parameterPages[0].enabled->getToggleState() && !panel->parameterPages[1].enabled->getToggleState() &&
              events.size() == 6 && events[1].parameter == "lenable" && events[4].parameter == "cenable",
          "both stages can be disabled or enabled independently through real parameter ids");
    panel->chooseParameterPage("clip");
    events.clear(); clip->slider.onDragStart(); clip->slider.setValue(.6, juce::sendNotificationSync);
    panel->chooseParameterPage("limiter"); clip->slider.onDragEnd(); clip->slider.setValue(.7, juce::sendNotificationSync);
    check(events.size() == 3 && events.back().kind == engine::ParamEventKind::End && events.back().parameter == "cgain" &&
              events.back().value == .6, "page change ends the exact held gesture; hidden late callbacks cannot resume it");
    events.clear(); limiter->slider.onDragStart(); panel->chooseParameterPage("clip"); clip->slider.onDragStart();
    limiter->slider.onDragEnd(); clip->slider.setValue(.65, juce::sendNotificationSync); clip->slider.onDragEnd();
    check(events.size() == 5 && events[1].parameter == "lgain" && events[1].kind == engine::ParamEventKind::End &&
              events.back().parameter == "cgain" && events.back().kind == engine::ParamEventKind::End,
          "late release from a hidden page cannot end the new page's gesture");
    events.clear(); reject = true; panel->setParameterPageEnabled("limiter", true); reject = false;
    check(events.size() == 3 && events.back().kind == engine::ParamEventKind::End && events.back().value == 0,
          "rejected page-enable value closes at its last accepted state");
    auto stalePageEnable = panel->parameterPages[0].enabled->onClick;
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->front().parameterGroups["cgain"].groupName = "Clip revised";
    strip.frame(2); events.clear(); stalePageEnable();
    check(events.empty() && panel->parameterPages[0].name == "Clip revised", "superseded page metadata invalidates retained header actions");
    events.clear(); clip->slider.onDragStart(); panel->setVisible(false); clip->slider.onDragEnd();
    check(events.size() == 2 && events.back().kind == engine::ParamEventKind::End,
          "hiding a page panel closes its exact held parameter owner");
    panel->setVisible(true);
    events.clear(); clip->slider.onDragStart(); strip.fold(panel->id); clip->slider.onDragEnd();
    check(events.size() == 2 && events.back().kind == engine::ParamEventKind::End,
          "folding the function closes a held page gesture without waiting for native input");
    strip.fold(panel->id);
    stalePageEnable = panel->parameterPages[0].enabled->onClick;
    publication = std::make_shared<ParameterPublication>(*publication); ++publication->front().instance;
    events.clear(); stalePageEnable();
    check(events.empty(), "page enable rejects a replaced runtime before mutation");
    peer.removeChildComponent(&root); peer.removeFromDesktop();
}
void stackEqSpectrum(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto document = ProjectDocument::open(directory / "stack-eq-spectrum.adi", true, device::withBuiltinDevices(), error);
    check(document && document->addMixFunction(mix::Function::Eq, 2), "Stack spectrum opens a native EQ fixture");
    if (!document) return;
    document->tick(0);
    const auto publication = document->parameterFeed().current();
    const auto eq = std::ranges::find(*publication, "org.adidaw.mix.eq", &DevicePanelData::uid);
    if (eq == publication->end()) return;
    const auto device = eq->id;
    const engine::ScopeSource expected{2, engine::ScopePoint::DeviceOutput, device};
    auto tap = std::make_shared<engine::ScopeTap>(48000, 1.);
    std::vector<engine::ScopeSource> opened, closed;
    auto broker = std::make_shared<SharedAnalysis>(
        [&](engine::ScopeSource source, double) -> std::shared_ptr<const engine::ScopeTap> {
            opened.push_back(source);
            return source == expected ? tap : nullptr;
        }, [&](engine::ScopeSource source) { closed.push_back(source); }, SharedAnalysis::Mode::Manual);
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "stack-eq-spectrum");
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const auto &ids) { document->setParameterDemand(owner, ids); };
    root.sharedAnalysis = broker;
    root.state().mixView = true;
    root.state().mixViewTab = static_cast<int>(MixViewPanel::Tab::Stack);
    root.arrangement.geometry.selection.track = 2;
    UiPeerRoot peer;
    peer.setBounds(0, 0, 1200, 720);
    peer.setVisible(true); peer.addToDesktop(0);
    StackTab surface(root);
    surface.setBounds(peer.getLocalBounds()); peer.addAndMakeVisible(surface);
    surface.filterChannels(std::vector<std::int64_t>{2});
    surface.model().setAutoZoom(false, 0);
    surface.frameAt(root.reader(), 500);
    check(opened == std::vector{expected} && broker->stats().consumers == 1,
          "visible EQ graph demands exactly its stable device output");

    const auto findPanel = [&](const auto &walk, juce::Component &parent) -> DevicePanelView * {
        if (auto *item = dynamic_cast<DevicePanelView *>(&parent); item && item->id == device) return item;
        for (auto *child : parent.getChildren())
            if (auto *found = walk(walk, *child)) return found;
        return nullptr;
    };
    const auto paintPanel = [](DevicePanelView &panel) {
        juce::Image image(juce::Image::RGB, panel.getWidth(), panel.getHeight(), true, juce::SoftwareImageType());
        juce::Graphics g(image);
        panel.paint(g);
        return image;
    };
    auto *panel = findPanel(findPanel, surface);
    check(panel != nullptr, "the spectrum uses the existing EQ card");
    if (!panel) return;
    juce::Component::SafePointer<DevicePanelView> card(panel);
    SharedAnalysis::Request request{expected.track, expected.point};
    request.device = expected.device;
    request.channel = SharedAnalysis::Channel::StereoPower;
    auto sibling = broker->acquire(request);
    check(sibling.available() && broker->stats().consumers == 2 && broker->stats().domains == 1 &&
              broker->stats().spectralPlans == 1 && opened.size() == 1,
          "a compatible sibling shares the same tap, FFT plan and worker domain");

    tap->write(static_cast<const float *>(nullptr), nullptr, 1024, 0, false);
    broker->pump(); surface.frameAt(root.reader(), 520);
    const auto silent = paintPanel(*card);
    std::array<float, 1024> left{}, right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = .5f * static_cast<float>(std::sin(2. * std::numbers::pi * 16. * static_cast<double>(i) / static_cast<double>(left.size())));
        right[i] = -left[i];
    }
    tap->write(left.data(), right.data(), static_cast<std::int32_t>(left.size()), 0, false);
    broker->pump(); surface.frameAt(root.reader(), 540);
    const auto audible = paintPanel(*card);
    const auto result = sibling.read();
    int changedPixels = 0;
    bool withinGraph = true;
    const auto graph = card->eqPreviewBounds().toNearestInt();
    for (int y = 0; y < silent.getHeight(); ++y)
        for (int x = 0; x < silent.getWidth(); ++x)
            if (silent.getPixelAt(x, y) != audible.getPixelAt(x, y)) {
                ++changedPixels;
                withinGraph = withinGraph && graph.contains(x, y);
            }
    if (!result || std::abs(result->binsDbfs[16] + 6.0206f) >= .002f || changedPixels == 0 || !withinGraph)
        std::cout << "Stack spectrum fixture: bin16=" << (result ? result->binsDbfs[16] : -999.f)
                  << ", peak=" << (result ? result->peak[0] : -1.f)
                  << ", changed=" << changedPixels << ", withinGraph=" << withinGraph << '\n';
    check(result && std::abs(result->binsDbfs[16] + 6.0206f) < .002f && changedPixels > 0 && withinGraph,
          "valid antiphase sound after silence changes only pixels clipped to the graph");
    const auto shown = broker->stats();
    const auto width = card->getWidth();
    surface.model().setBaseHeight(122, 550);
    surface.frameAt(root.reader(), 800);
    broker->pump();
    check(card && card->getWidth() != width && broker->stats().transforms == shown.transforms &&
              broker->stats().publications == shown.publications && broker->stats().consumers == shown.consumers,
          "resizing changes graph geometry without requesting a new transform or lease");

    surface.setVisible(false);
    check(broker->stats().consumers == 1 && closed.empty(),
          "hiding the Stack immediately releases its reader while the sibling remains live");
    sibling.reset();
    check(broker->stats().consumers == 0 && closed == std::vector{expected},
          "last hide unbinds the physical EQ tap exactly once");
    surface.setVisible(true);
    surface.frameAt(root.reader(), 850);
    check(broker->stats().consumers == 1, "showing the EQ attaches one new reader");
    surface.filterChannels(std::nullopt);
    surface.setSize(1200, 165);
    surface.frameAt(root.reader(), 900);
    surface.model().scrollTo(surface.model().maxScroll(), 905);
    surface.frameAt(root.reader(), 1200);
    check(broker->stats().consumers == 0 && closed.size() == opened.size(),
          "scrolling all EQ graph pixels outside the viewport unbinds its device tap");
    surface.filterChannels(std::vector<std::int64_t>{2});
    surface.setBounds(peer.getLocalBounds()); surface.frameAt(root.reader(), 1300);
    const auto live = broker->stats();
    surface.suspendParameters(true);
    broker->pump();
    check(live.consumers == 1 && broker->stats().consumers == 0 && broker->stats().workerPasses == live.workerPasses,
          "suspending the window removes its last EQ reader and schedules no idle work");
    peer.removeChildComponent(&surface); peer.removeFromDesktop();
}

void stackLimiterReduction(AppCommands &commands, const std::filesystem::path &directory) {
    namespace l = mix::limiter;
    std::string error;
    auto document = ProjectDocument::open(directory / "stack-limiter-reduction.adi", true,
        device::withBuiltinDevices(), error);
    check(document && document->addMixFunction(mix::Function::Limiter, 2),
          "Stack reduction opens a native limiter fixture");
    if (!document) return;
    document->tick(0);
    const auto publication = document->parameterFeed().current();
    const auto row = std::ranges::find(*publication, "org.adidaw.mix.limiter", &DevicePanelData::uid);
    check(row != publication->end(), "fixture exposes the same exact native runtime to the Stack");
    if (row == publication->end()) return;
    const auto runtime = row->runtime;
    const auto device = row->id;
    auto mailbox = std::make_shared<l::ReductionMailbox>();
    mailbox->setPrepared(true);
    int acquisitions = 0;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "stack-limiter-reduction");
    root.devices.publication = [&] { return document->parameterFeed().current(); };
    root.devices.parameterDemand = [&](const void *owner, const auto &ids) { document->setParameterDemand(owner, ids); };
    root.state().mixView = true;
    root.state().mixViewTab = static_cast<int>(MixViewPanel::Tab::Stack);
    root.arrangement.geometry.selection.track = 2;
    UiPeerRoot peer;
    peer.setBounds(0, 0, 1200, 720);
    peer.setVisible(true); peer.addToDesktop(0);
    StackTab surface(root);
    surface.setBounds(peer.getLocalBounds()); peer.addAndMakeVisible(surface);
    surface.filterChannels(std::vector<std::int64_t>{2});
    surface.model().setAutoZoom(false, 0);
    surface.model().setBaseHeight(184, 0);
    surface.setLimiterObserver([&](const DevicePanelData &target) {
        if (target.id != device || !target.runtime.sameInstance(runtime)) return l::ReductionMailbox::Lease{};
        ++acquisitions;
        return l::ReductionMailbox::observe(mailbox);
    });
    surface.frameAt(root.reader(), 500);
    check(mailbox->observed() && acquisitions == 1, "expanded visible Limiter card acquires one reading lease");
    const auto locate = [&](const auto &walk, juce::Component &parent) -> DevicePanelView * {
        if (auto *card = dynamic_cast<DevicePanelView *>(&parent); card && card->id == device) return card;
        for (auto *child : parent.getChildren())
            if (auto *found = walk(walk, *child)) return found;
        return nullptr;
    };
    auto *found = locate(locate, surface);
    check(found != nullptr, "the reduction plot lives in the existing Stack device card");
    if (!found) return;
    juce::Component::SafePointer<DevicePanelView> card(found);
    const auto paint = [](DevicePanelView &panel) {
        juce::Image image(juce::Image::RGB, panel.getWidth(), panel.getHeight(), true, juce::SoftwareImageType());
        juce::Graphics g(image);
        panel.paint(g);
        return image;
    };
    const auto rightPeakPixels = [](const juce::Image &image) {
        const auto colour = static_cast<juce::Colour>(look::meterPeak);
        const int x = image.getWidth() - 2 * static_cast<int>(look::gap) - 2;
        int pixels = 0;
        for (int y = DevicePanelView::kTitleHeight; y < image.getHeight(); ++y)
            if (image.getPixelAt(x, y) == colour) ++pixels;
        return pixels;
    };
    const auto empty = paint(*card);
    l::ReductionFrame summary;
    summary.renderBlock = 1;
    summary.observedFrames = 128;
    summary.reduction = {.75f, true};
    mailbox->publishSegment(mailbox->observationEpoch(), summary);
    surface.frameAt(root.reader(), 520);
    const auto peak = paint(*card);
    check(rightPeakPixels(peak) > rightPeakPixels(empty) && acquisitions == 1,
          "the latest measured peak paints a right-hand column without reacquiring the source");
    summary.renderBlock = 2;
    summary.reduction = {};
    mailbox->publishSegment(mailbox->observationEpoch(), summary);
    surface.frameAt(root.reader(), 540);
    const auto missing = paint(*card);
    check(rightPeakPixels(missing) == rightPeakPixels(empty) && mailbox->observed(),
          "an unavailable block paints a genuine gap in place of the latest peak");

    surface.model().setBaseHeight(112, 600);
    surface.frameAt(root.reader(), 950);
    check(!mailbox->observed(), "a compact card releases its reduction lease");
    surface.model().setBaseHeight(184, 980);
    surface.frameAt(root.reader(), 1330);
    check(mailbox->observed() && acquisitions == 2, "re-expansion reacquires an empty observation");
    root.devices.fold(device);
    surface.frameAt(root.reader(), 1360);
    check(!mailbox->observed(), "folded Limiter has no reduction subscription");
    root.devices.fold(device);
    surface.frameAt(root.reader(), 1600);
    check(mailbox->observed() && acquisitions == 3, "unfolding reopens one subscription");
    surface.filterChannels(std::nullopt);
    surface.setSize(1200, 165);
    surface.frameAt(root.reader(), 1640);
    surface.model().scrollTo(surface.model().maxScroll(), 1660);
    surface.frameAt(root.reader(), 2090);
    check(!mailbox->observed(), "a retained Limiter card with its plot fully offscreen releases demand");

    surface.filterChannels(std::vector<std::int64_t>{2});
    surface.setBounds(peer.getLocalBounds());
    surface.frameAt(root.reader(), 2410);
    const auto shown = mailbox->observed();
    auto sibling = l::ReductionMailbox::observe(mailbox);
    surface.setVisible(false);
    check(shown && mailbox->observed(), "hiding a Stack reader preserves a compatible sibling");
    sibling.reset();
    check(!mailbox->observed(), "the last hidden view leaves no producer demand");
    peer.removeChildComponent(&surface); peer.removeFromDesktop();
}

} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    test::TempDirectory temporary("ui_mix_functions", "component");
    settings::AppSettings settings(appdata::App::Daw, temporary.path() / "settings.json");
    AppCommands commands(settings);
    functions(commands, temporary.path());
    parameterPages(commands, temporary.path());
    stackEqSpectrum(commands, temporary.path());
    stackLimiterReduction(commands, temporary.path());
    std::printf("%d checks, %d failures (software-peer components)\n", checks, failures);
    return failures ? 1 : 0;
}
