// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/adi_shaper_panel_view.hpp"
#include "juce/builtin_host.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace adi;
using Panel = ui::AdiShaperPanel;
using Phase = Panel::Phase;
constexpr auto uid = "org.adidaw.builtin.adi-shaper";

void require(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}

// The editor window's content as the app's factory builds it (ui_main.cpp), on a project holding
// an ADI-Shaper: its frame runs; it shows a value an op committed and the value Undo put back; it
// keeps its device across an audio restart, and shows a value that changed with no commit once the
// feed re-stamps the device; it is invalid while the device is deleted and valid
// again once Undo restores it; and it is destroyed before the document, as the app closes it.
void run() {
    test::TempDirectory temp("adi_shaper_view", "window");
    std::string error;
    auto doc = ui::ProjectDocument::open(temp.path() / "view.adi", true, device::withBuiltinDevices(), error);
    require(doc && doc->insertBuiltin(uid, 2, {}), "insert shaper for the window");
    doc->prepare(48000, 64);
    doc->tick(0);
    const auto id = doc->store().db().execAndGet("SELECT MAX(id) FROM devices").getInt64();
    const auto sync = [&](int tick) { require(doc->synchronise(), "synchronise project"); doc->tick(tick); };
    {
        ui::AdiShaperPanelView view(*doc, id);
        view.frame();
        const double initial = view.value(1);
        require(view.valid() && view.error().empty() && std::abs(initial - .5) > .01,
                "the window's first frame shows its device");
        {
            ui::AdiShaperPanelSession other(*doc, id);
            auto b = other.binding();
            const std::array<Panel::Change,1> half{{{1, .5}}};
            require(b.parameters(Phase::Begin, {}) && b.parameters(Phase::End, half), "another editor commits a value");
        }
        sync(10);
        view.frame();
        require(std::abs(view.value(1) - .5) < .00001, "the window shows the committed value");
        require(doc->ops().undo().ok, "undo the value");
        sync(20);
        view.frame();
        require(std::abs(view.value(1) - initial) < .00001, "the window shows the value Undo put back");
        // A value no commit carries: the device's row in the feed changes, not the project.
        auto* device = doc->session().instanceFor(id);
        require(device && device->setParam("1", device::ParamValue::fromNormalized(.25)), "another writer moves a value");
        const auto committed = doc->view().committedGeneration();
        doc->prepare(48000, 128);
        view.frame();
        require(view.valid(), "a new block size keeps the window");
        doc->tick(30);
        view.frame();
        require(view.valid() && std::abs(view.value(1) - .25) < .00001 && doc->view().committedGeneration() == committed,
                "the feed's new stamp shows the value with no commit");
        require(device->setParam("1", device::ParamValue::fromNormalized(initial)), "put the value back");
        require(doc->deviceAction("device.remove", {{"id", id}}), "remove the device");
        view.frame();
        require(!view.valid(), "the window is invalid while its device is deleted");
        require(doc->ops().undo().ok, "restore the device");
        sync(40);
        view.frame();
        require(view.valid() && std::abs(view.value(1) - initial) < .00001,
                "the window is valid again once Undo restores its device");
    }
    doc.reset();
}
} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    try { run(); std::puts("ADI-Shaper editor window tests passed"); return 0; }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
