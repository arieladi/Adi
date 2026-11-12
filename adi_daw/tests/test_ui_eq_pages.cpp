// SPDX-License-Identifier: AGPL-3.0-or-later
// Native EQ presentation using synthetic descriptors and a software-only peer.
// No native window, desktop input, audio device or reference plug-in is used.
#include "../src/juce/ui_shell.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include "ui_fake_peer.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace adi;
using namespace adi::ui;
namespace {
int checks = 0, failures = 0;
void check(bool okay, const char *what) {
    ++checks;
    if (!okay) { ++failures; std::printf("FAIL %s\n", what); }
}
DevicePanelData eqData(std::int64_t id, bool plugin = false) {
    DevicePanelData data;
    data.id = id; data.track = 2; data.chain = 22; data.instance = 91;
    data.available = true; data.name = "EQ"; data.uid = "org.adidaw.mix.eq"; data.plugin = plugin;
    const auto add = [&](std::string parameter, std::int32_t band, const char *section, const char *name, bool writable) {
        data.declared.push_back({parameter, name, writable, writable});
        panel::Record record;
        record.id = parameter; record.name = name; record.playing = record.stored = .25; record.text = "Current value";
        data.records.push_back(record); data.defaults[parameter] = .25;
        data.parameterGroups[parameter] = {"eq", "EQ", false, band, section, name};
    };
    for (int i = 0; i < 10; ++i) add("global-" + std::to_string(i), -1, "global", "Global", true);
    for (std::int32_t band = 0; band < 24; ++band)
        for (int field = 0; field < 30; ++field) {
            const char *section = field < 8 ? "basic" : field < 17 ? "dynamics" : field < 26 ? "detector" : "spectral";
            const char *name = field < 8 ? "Basic" : field < 17 ? "Dynamics" : field < 26 ? "Detector" : "Spectral";
            add("band-" + std::to_string(band) + "-" + std::to_string(field), band, section, name, field != 17);
            auto &record = data.records.back();
            if (field == 1) {
                record.shape = panel::Shape::Menu; record.stepCount = 11; record.playing = record.stored = .2;
                record.choiceLabels = {"Bell", "Low Shelf", "High Cut", "High Shelf", "Low Cut", "Notch",
                                       "Band Pass", "Tilt Shelf", "Flat Tilt", "All Pass", "Flat Gain"};
            } else if (field == 2) {
                record.shape = panel::Shape::Menu; record.stepCount = 3; record.choiceLabels = {"First", ""};
            }
        }
    // Simulate the ordinary default-panel threshold. Native pages must remain
    // usable without silently relying on the first 64 descriptor ordinals.
    data.resolved.modifiableCount = data.declared.size(); data.resolved.showsConfigureHint = true;
    return data;
}
struct Event { std::int64_t device; std::string parameter; engine::ParamEventKind kind; double value; std::uint64_t instance; };
void exercise(AppCommands &commands, const std::filesystem::path &directory) {
    std::string error;
    auto document = ProjectDocument::open(directory / "eq-pages.adi", true, {}, error);
    check(document != nullptr, "presentation fixture opens without a host");
    if (!document) return;
    AdiRootComponent root(document->view(), document->ops(), document->mailbox(), commands,
                          document->views(), document->sessionState(), "eq-pages");
    root.setSize(1200, 800);
    UiPeerRoot peer;
    peer.setSize(1200, 800); peer.setVisible(true); peer.addToDesktop(0); peer.addAndMakeVisible(root);
    check(peer.getPeer() && !peer.getPeer()->getNativeHandle(), "component peer has no OS window");
    auto publication = std::make_shared<ParameterPublication>(); publication->push_back(eqData(40));
    auto &strip = root.devices;
    strip.setVisible(true); strip.publication = [&] { return publication; };
    strip.targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &, std::uint64_t instance) {
        return std::any_of(publication->begin(), publication->end(), [&](const auto &data) {
            return data.id == id && data.instance == instance && data.available;
        });
    };
    std::vector<Event> events;
    strip.guardedGesture = [&](std::int64_t id, const std::string &parameter, engine::ParamEventKind kind,
                                double value, const engine::DeviceRuntimeToken &, std::uint64_t instance) {
        events.push_back({id, parameter, kind, value, instance}); return true;
    };
    int actions = 0;
    strip.submit = [&](const std::string &, Payload) { ++actions; return true; };
    strip.frame(2);
    auto *panel = strip.panels.front().get();
    const auto findControl = [&](const std::string &id) -> ParameterControl * {
        const auto at = std::find_if(panel->controls.begin(), panel->controls.end(), [&](const auto &control) { return control->parameter == id; });
        return at == panel->controls.end() ? nullptr : at->get();
    };
    check(publication->front().declared.size() == 730 && panel->controls.size() == 8 && panel->functionBand.getNumItems() == 24,
          "730 descriptors construct only one basic band while all 24 bands remain reachable");
    check(panel->parameterPages.size() == 3 && panel->parameterPages[0].id == "eq" &&
              panel->parameterPages[1].id == "carve" && panel->parameterPages[2].id == "focus",
          "native EQ has separate EQ, Carve and Focus presentation pages");
    check(!panel->configureButton.isVisible() && panel->selectedFunctionSection() == "basic" &&
              panel->selectedFunctionBand() == 0 && panel->functionSection.getNumItems() == 5,
          "native EQ uses explicit band and section selectors instead of generic Configure");
    const auto *shape = findControl("band-0-1");
    check(shape && shape->menu.getItemText(0) == "Bell" && shape->menu.getItemText(2) == "High Cut" &&
              shape->menu.getSelectedId() == 3 && shape->menu.getText() == "High Cut",
          "named shape choices identify the current selected value without displaying ordinal numbers");
    const auto *partialLabels = findControl("band-0-2");
    check(partialLabels && partialLabels->menu.getItemText(0) == "First" && partialLabels->menu.getItemText(1) == "2" &&
              partialLabels->menu.getItemText(2) == "3", "missing or empty choice labels retain the ordinary numeric fallback");
    check(panel->chooseFunctionBand(23) && findControl("band-23-0") && panel->controls.size() == 8,
          "last band is available beyond the aggregate 64-control threshold");
    check(panel->controlsMatch(publication->front()), "STACK validates the visible EQ control set rather than all descriptor ordinals");
    auto changed = publication->front();
    std::erase_if(changed.records, [](const auto &record) { return record.id == "band-23-0"; });
    check(!panel->controlsMatch(changed), "removing a displayed field invalidates the held inline control set");
    check(!panel->chooseFunctionBand(24) && !panel->chooseFunctionBand(-1) && !panel->chooseFunctionSection("absent"),
          "invalid presentation selections cannot alias a different band or section");
    check(events.empty() && actions == 0, "selection alone sends no parameter, state or journal writes");
    auto *shapeEdit = findControl("band-23-1");
    shapeEdit->menu.setSelectedId(5, juce::sendNotificationSync);
    check(events.size() == 3 && events[0].kind == engine::ParamEventKind::Begin && events[1].parameter == "band-23-1" &&
              events[1].value == .4 && events[2].kind == engine::ParamEventKind::End,
          "named menu choices preserve normalized step order and one common balanced gesture");
    events.clear();
    publication = std::make_shared<ParameterPublication>(*publication);
    for (auto &record : publication->front().records)
        if (record.id == "band-23-1") record.choiceLabels[2] = "High Cut revised";
    strip.frame(2);
    check(findControl("band-23-1")->menu.getItemText(2) == "High Cut revised" &&
              findControl("band-23-1")->menu.getSelectedId() == 3 && events.empty(),
          "label refresh updates a retained control and selected choice without a parameter write");
    auto *control = findControl("band-23-0");
    if (!control) return;
    const auto delayed = control->parameterGesture;
    control->slider.onDragStart(); control->slider.setValue(.6, juce::sendNotificationSync);
    panel->chooseFunctionBand(0);
    check(events.size() == 3 && events.back().parameter == "band-23-0" && events.back().kind == engine::ParamEventKind::End &&
              events.back().value == .6, "band selection ends the exact held parameter at its last accepted value");
    check(!delayed("band-23-0", engine::ParamEventKind::Value, .9), "a disposed band control cannot resume a delayed value");
    panel->chooseFunctionBand(23); control = findControl("band-23-0");
    control->slider.onDragStart();
    check(!delayed("band-23-0", engine::ParamEventKind::End, .9), "a disposed control cannot end a newly acquired same-id gesture");
    control->slider.onDragEnd();
    check(events.size() == 5 && events.back().kind == engine::ParamEventKind::End,
          "replacement control keeps a balanced independent gesture");
    events.clear();
    check(panel->chooseFunctionSection("global") && panel->controls.size() == 10 && findControl("global-9") && !panel->functionBand.isEnabled(),
          "global controls stay accessible without duplicating them on every band section");
    check(panel->chooseFunctionSection("detector") && panel->controls.size() == 9 && findControl("band-23-17") &&
              !findControl("band-23-17")->isEnabled(), "unsupported linked edit remains visible at its real value but read-only");
    findControl("band-23-17")->slider.onDragStart();
    check(events.empty(), "read-only detector field cannot claim a working edit gesture");
    for (const auto *page : {"carve", "focus"}) {
        check(panel->chooseParameterPage(page) && panel->controls.empty() && !panel->parameterPageUnavailableReason().empty() &&
                  !panel->functionBand.isVisible() && !panel->functionSection.isVisible(),
              "deferred processing page names its missing dependencies and exposes no controls");
        check(!panel->setParameterPageEnabled(page, true) && events.empty() && actions == 0,
              "deferred page cannot enable fake processing or apply an analysis result");
    }
    panel->chooseParameterPage("eq");
    check(panel->selectedFunctionBand() == 23 && panel->selectedFunctionSection() == "detector" && panel->controls.size() == 9,
          "returning from deferred pages preserves the chosen band and section");
    publication = std::make_shared<ParameterPublication>(*publication);
    std::reverse(publication->front().records.begin(), publication->front().records.end());
    std::reverse(publication->front().declared.begin(), publication->front().declared.end());
    strip.frame(2);
    check(strip.panels.front().get() == panel && panel->selectedFunctionBand() == 23 && panel->selectedFunctionSection() == "detector" &&
              findControl("band-23-25") && panel->controls.size() == 9,
          "descriptor reorder and refreshed publication preserve stable band identity");
    publication = std::make_shared<ParameterPublication>(*publication);
    publication->insert(publication->begin(), eqData(41, true));
    strip.frame(2);
    check(strip.panels.size() == 2 && strip.panels[1].get() == panel && panel->selectedFunctionBand() == 23 &&
              panel->selectedFunctionSection() == "detector", "adding an adjacent device retains native EQ presentation identity");
    auto *ordinary = strip.panels[0].get();
    check(!ordinary->nativeFunction() && ordinary->parameterPages.size() == 1 && ordinary->controls.empty() &&
              !ordinary->functionBand.isVisible() && ordinary->configureButton.isVisible(),
          "ordinary plug-in UID coincidence keeps generic Configure and no native EQ band UI or deferred pages");
    events.clear(); control = findControl("band-23-25"); control->slider.onDragStart();
    publication = std::make_shared<ParameterPublication>(*publication);
    std::reverse(publication->begin(), publication->end()); strip.frame(2);
    check(strip.panels.front().get() == panel && events.size() == 2 && events.back().kind == engine::ParamEventKind::End &&
              events.back().parameter == "band-23-25" && events.back().instance == 91,
          "chain reorder keeps selection and closes the exact retained edit owner");
    check(panel->selectedFunctionBand() == 23 && panel->selectedFunctionSection() == "detector" && actions == 0,
          "chain reorder never changes processing through presentation state");
    events.clear();
    publication = std::make_shared<ParameterPublication>(*publication); ++publication->front().instance;
    check(!panel->chooseFunctionBand(0) && !panel->chooseParameterPage("carve") && events.empty(),
          "stale runtime selection refuses delayed presentation callbacks");
    strip.frame(2);
    check(strip.panels.front()->selectedFunctionBand() == 0 && strip.panels.front()->selectedFunctionSection() == "basic",
          "a new runtime begins a fresh presentation instead of inheriting another instance's selection");
    peer.removeChildComponent(&root); peer.removeFromDesktop();
}
void destructionDuringFrame(AppCommands &commands, const std::filesystem::path &directory, bool reorder) {
    std::string error;
    auto document = ProjectDocument::open(directory / (reorder ? "eq-destroy-reorder.adi" : "eq-destroy-present.adi"), true, {}, error);
    check(document != nullptr, "destruction fixture opens without a host");
    if (!document) return;
    auto root = std::make_unique<AdiRootComponent>(document->view(), document->ops(), document->mailbox(), commands,
                                                  document->views(), document->sessionState(), "eq-destruction");
    root->setSize(1200, 800);
    UiPeerRoot peer;
    peer.setSize(1200, 800); peer.setVisible(true); peer.addToDesktop(0); peer.addAndMakeVisible(*root);
    const juce::Component::SafePointer<AdiRootComponent> observed(root.get());
    auto publication = std::make_shared<ParameterPublication>();
    publication->push_back(eqData(60)); publication->push_back(eqData(61, true));
    auto *strip = &root->devices;
    strip->setVisible(true); strip->publication = [&] { return publication; };
    strip->targetCurrent = [&](std::int64_t id, const engine::DeviceRuntimeToken &, std::uint64_t instance) {
        return std::any_of(publication->begin(), publication->end(), [&](const auto &data) {
            return data.id == id && data.instance == instance && data.available;
        });
    };
    int begins = 0, ends = 0;
    bool destroy = false;
    strip->guardedGesture = [&](std::int64_t, const std::string &, engine::ParamEventKind kind,
                                 double, const engine::DeviceRuntimeToken &, std::uint64_t) {
        if (kind == engine::ParamEventKind::Begin) ++begins;
        if (kind == engine::ParamEventKind::End) {
            ++ends;
            if (destroy) root.reset();
        }
        return true;
    };
    strip->frame(2);
    auto *control = strip->panels.front()->controls.front().get();
    control->slider.onDragStart();
    check(begins == 1, "destruction fixture acquires one real retained edit owner");
    destroy = true;
    publication = std::make_shared<ParameterPublication>(*publication);
    if (reorder) std::reverse(publication->begin(), publication->end());
    else std::erase_if(publication->front().records, [](const auto &record) { return record.id == "band-0-0"; });
    strip->frame(2); // End destroys root/strip synchronously inside this frame.
    check(!root && !observed && ends == 1 && begins == 1,
          reorder ? "reconciliation End can destroy the strip without local panels or a post-callback dereference"
                  : "presentation-rebuild End can destroy the strip without resuming the outer frame");
    peer.removeFromDesktop();
}
} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    test::TempDirectory temporary("ui_eq_pages", "component");
    settings::AppSettings settings(appdata::App::Daw, temporary.path() / "settings.json");
    AppCommands commands(settings);
    exercise(commands, temporary.path());
    destructionDuringFrame(commands, temporary.path(), true);
    destructionDuringFrame(commands, temporary.path(), false);
    std::printf("%d checks, %d failures (software-peer EQ presentation)\n", checks, failures);
    return failures ? 1 : 0;
}
