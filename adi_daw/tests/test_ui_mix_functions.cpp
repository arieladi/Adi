// SPDX-License-Identifier: AGPL-3.0-or-later
// Retained function controls and prepared drag targets, with a software peer only.
// No native host, OS window, desktop input, capture or audio device is involved.
#include "../src/juce/ui_shell.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_function_host.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include "ui_fake_peer.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
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
    auto submit = strip.submit;
    strip.submit = [](const std::string &, Payload) { return false; };
    native->enabled.setToggleState(false, juce::dontSendNotification);
    native->enabled.onClick();
    check(native->enabled.getToggleState(), "refused native removal restores its authored active state");
    strip.submit = submit;
    check(strip.setEnabledExpected(publication->back(), false) && op == "device.remove",
          "shared compact/native activation authority removes instead of bypassing");
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->back().enabled = false;
    strip.frame(2); native = strip.panels[1].get(); ordinary = strip.panels[0].get();
    native->enabled.onClick();
    check(op == "device.setEnabled" && payload.at("enabled").get<bool>() &&
              payload.at("id").get<std::int64_t>() == 20,
          "expanded activation recovers an already-disabled native function");
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->back().enabled = true;
    strip.frame(2); native = strip.panels[1].get(); ordinary = strip.panels[0].get();
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
    bool foreignLive = true;
    const auto beforeForeign = additions;
    strip.setVisible(false);
    auto foreign = menuItems(strip.functionMenuFor(3, [&] { return foreignLive; }));
    foreign[1].action();
    check(additions == beforeForeign + 1 && menuItems(strip.functionMenu())[4].ticked,
          "explicit-target add menu works for another visible owner without retargeting the hidden strip");
    foreignLive = false; foreign[1].action();
    check(additions == beforeForeign + 1, "expired foreign add-menu owner cannot add later");
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
void cleanCompressor(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto doc =
        ProjectDocument::open(directory / "clean.adi", true, device::withBuiltinDevices(), error);
    check(doc && doc->addMixFunction(mix::Function::Compressor, 2),
          "real Clean device can be added for component presentation");
    if (!doc)
        return;
    doc->tick(0);
    const auto publication = doc->parameterFeed().current();
    const auto found = std::find_if(publication->begin(), publication->end(), [](const auto &p) {
        return p.uid == "org.adidaw.mix.compressor";
    });
    if (found == publication->end()) {
        check(false, "Clean publication exists");
        return;
    }
    doc->setParameterDemand(doc.get(), {found->id});
    doc->tick(1);
    AdiRootComponent root(doc->view(), doc->ops(), doc->mailbox(), commands, doc->views(),
                          doc->sessionState(), "clean");
    root.setSize(1200, 800);
    UiPeerRoot peer;
    peer.setSize(1200, 800);
    peer.setVisible(true);
    peer.addToDesktop(0);
    peer.addAndMakeVisible(root);
    check(peer.getPeer() && !peer.getPeer()->getNativeHandle(),
          "Clean presentation uses only a software peer");
    auto &strip = root.devices;
    strip.setVisible(true);
    strip.publication = [&] { return doc->parameterFeed().current(); };
    strip.functionAvailability = device::mixFunctionAvailability;
    strip.addFunction = [&](mix::Function kind, std::int64_t track) { return doc->addMixFunction(kind, track); };
    strip.targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &runtime,
                              std::uint64_t stamp) {
        const auto p = doc->parameterFeed().current();
        return std::any_of(p->begin(), p->end(), [&](const auto &d) {
            return d.id == id && d.runtime.sameInstance(runtime) && d.instance == stamp &&
                   d.available;
        });
    };
    strip.guardedGesture = [&](std::int64_t id, const std::string &parameter,
                               engine::ParamEventKind kind, double value,
                               const engine::DeviceRuntimeToken &runtime, std::uint64_t stamp) {
        return doc->parameterGestureExpected(id, parameter, kind, value, runtime, stamp);
    };
    strip.frame(2);
    check(strip.panels.size() == 1, "Clean creates one native device tab");
    if (strip.panels.size() == 1) {
        auto *panel = strip.panels.front().get();
        check(panel->controls.size() == 8 && panel->title.getTooltip().contains("0 samples"),
              "Clean shows eight live controls and real zero latency");
        check(panel->parameterPages.size() == 1 && panel->parameterPages.front().name == "Clean",
              "Clean has no unavailable style pages");
        if (!panel->controls.empty()) {
            auto *control = panel->controls.front().get();
            control->slider.onDragStart();
            control->slider.setValue(.25, juce::sendNotificationSync);
            control->slider.onDragEnd();
            check(doc->session().instanceFor(found->id)->getParam("0").normalized == .25,
                  "visible Clean control reaches the actual core parameter");
            const bool undo = doc->ops().undo().ok && doc->synchronise();
            doc->tick(2);
            check(undo && std::abs(doc->session().instanceFor(found->id)->getParam("0").normalized -
                                   .7) < 1e-6,
                  "visible Clean drag has one common undo");
        }
    }
    root.arrangement.geometry.selection.track = 2;
    root.command(AppCommands::ToggleMixView);
    root.chooseMixViewTab(1);
    root.mark(DirtySet::All);
    root.frame();
    auto &stack = root.mixViewPanel.stack;
    stack.frameAt(root.reader(), 0);
    const auto item = stack.model().chainRef(2, found->id);
    if (!item.row.valid() || stack.rows().empty())
        std::printf("STACK diagnostic: panel=%d stack=%d rows=%zu scope=%d item=%d/%d area=%d,%d error=%s\n",
          root.mixViewPanel.isVisible(), stack.isVisible(), stack.rows().size(),
          static_cast<int>(stack.model().windowView().channelScope), item.row.valid(),static_cast<int>(item.place),
          stack.getWidth(),stack.getHeight(),root.error().c_str());
    check(stack.model().windowView().channelScope == stack::ChannelScope::WithTools &&
              item.row.valid() && item.place == stack::ItemPlace::Chain && !stack.rows().empty(),
          "actual Clean device appears in the default With Tools STACK row");
    const auto bounds = stack.getBounds();
    bool fits = true;
    for (int width : {320, 480, 816, 1200}) {
        stack.setSize(width, 650);
        fits = fits && !stack.addFunction.getBounds().intersects(stack.search.getBounds()) &&
            stack.getLocalBounds().contains(stack.addFunction.getBounds()) &&
            stack.getLocalBounds().contains(stack.search.getBounds());
    }
    stack.setBounds(bounds);
    check(fits, "Mix View add and search controls stay distinct and inside narrow/wide headers");
    strip.frame(3);
    auto add = menuItems(stack.functionMenu());
    check(stack.addFunction.isVisible() && stack.addFunction.isEnabled() && add.size() == 5 &&
              add[1].ticked && !add[1].enabled && add[0].enabled,
          "Mix View offers a selected-channel add control with present and unavailable capabilities explicit");
    if (add.size() == 5) {
        add[0].action(); doc->tick(3);
        const auto after = doc->parameterFeed().current();
        const auto eq = std::find_if(after->begin(), after->end(), [](const auto &d) { return d.uid == "org.adidaw.mix.eq" && d.track == 2; });
        check(eq != after->end() && !menuItems(strip.functionMenu())[0].ticked && root.arrangement.geometry.selection.track == 2,
              "Mix View adds to its selected channel without selecting or altering the device-strip channel");
        const bool undo = doc->ops().undo().ok && doc->synchronise(); doc->tick(4);
        const auto remaining = doc->parameterFeed().current();
        const bool eqGone = eq != after->end() && std::none_of(remaining->begin(), remaining->end(),
            [&](const auto &d) { return d.id == eq->id; });
        const bool cleanKept = std::any_of(remaining->begin(), remaining->end(),
            [&](const auto &d) { return d.id == found->id && d.uid == "org.adidaw.mix.compressor"; });
        check(undo && eqGone && cleanKept, "one Undo removes only the function added in Mix View");
        root.frame(); stack.frameAt(root.reader(), 1);
        auto expired = menuItems(stack.functionMenu());
        const auto count = doc->store().db().execAndGet("SELECT count(*) FROM devices").getInt();
        stack.cancelInteractions();
        if (expired.size() == 5) expired[0].action();
        check(doc->store().db().execAndGet("SELECT count(*) FROM devices").getInt() == count,
              "cancelled Mix View add menu cannot mutate a later publication");
        auto oldSelection = menuItems(stack.functionMenu());
        root.arrangement.geometry.selection.track = 3; stack.frameAt(root.reader(), 2);
        root.arrangement.geometry.selection.track = 2; stack.frameAt(root.reader(), 3);
        if (oldSelection.size() == 5) oldSelection[0].action();
        check(doc->store().db().execAndGet("SELECT count(*) FROM devices").getInt() == count,
              "selection away and back cannot revive an old Mix View add menu");
    }
    // Drive the actual folded-dot handler using generated component events,
    // not desktop input. Its coordinates come from the presented model geometry.
    strip.submit = [&](const std::string &op, Payload p) { return doc->deviceAction(op, std::move(p)); };
    stack.setSize(320, 650);
    stack.model().setAutoZoom(false, 10);
    const auto refresh = [&] {
        doc->tick(10); root.mark(DirtySet::All); root.frame();
        stack.setSize(320, 650); stack.frameAt(root.reader(), 10000);
    };
    const auto clickDot = [&](std::int64_t id) {
        std::optional<stack::ItemGeo> target;
        stack.model().forEachVisibleRow([&](const auto &row) {
            stack.model().forEachChainItem(row.shown, [&](const auto &item) {
                if (item.ref.device == id && item.pill) target = item;
            });
        });
        if (!target) return false;
        const auto at = stack.listArea().getPosition().toFloat() +
            juce::Point<float>{static_cast<float>(target->x + target->width / 2),
                               static_cast<float>(target->top + 8)};
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent event{juce::Desktop::getInstance().getMainMouseSource(), at,
            juce::ModifierKeys::leftButtonModifier, 1.f, 0.f, 0.f, 0.f, 0.f,
            &stack, &stack, now, at, now, 1, false};
        stack.mouseDown(event); stack.mouseUp(event);
        return true;
    };
    const auto enabledInStore = [&](std::int64_t id) -> std::optional<bool> {
        SQLite::Statement query(doc->store().db(), "SELECT enabled FROM devices WHERE id=?");
        query.bind(1, id);
        if (!query.executeStep()) return {};
        return query.getColumn(0).getInt() != 0;
    };
    const bool disabled = doc->deviceAction("device.setEnabled", {{"id", found->id}, {"enabled", false}});
    refresh();
    check(disabled && clickDot(found->id) && enabledInStore(found->id) == true,
          "actual folded native dot recovers a bypassed function without removing it");
    refresh();
    const bool removed = clickDot(found->id);
    check(removed && !enabledInStore(found->id), "actual active native dot removes its authored device row");
    const bool restored = doc->ops().undo().ok && doc->synchronise(); refresh();
    const auto restoredParams = doc->parameterFeed().current();
    check(restored && enabledInStore(found->id) == true &&
              std::any_of(restoredParams->begin(), restoredParams->end(), [&](const auto &d) {
                  return d.id == found->id && d.uid == "org.adidaw.mix.compressor";
              }), "one Undo recovers the active function removed by the actual folded dot");
    peer.removeChildComponent(&root);
    peer.removeFromDesktop();
}
} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    test::TempDirectory temporary("ui_mix_functions", "component");
    settings::AppSettings settings(appdata::App::Daw, temporary.path() / "settings.json");
    AppCommands commands(settings);
    functions(commands, temporary.path());
    parameterPages(commands, temporary.path());
    cleanCompressor(commands, temporary.path());
    std::printf("%d checks, %d failures (software-peer components)\n", checks, failures);
    return failures ? 1 : 0;
}
