// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/adi_shaper_panel_session.hpp"
#include "adi/dsp/adi_shaper.hpp"
#include "juce/builtin_host.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/history.hpp"
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
constexpr Panel::Key key{0, 1, 0};

void require(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}

float sample(device::DeviceInstance& instance) {
    std::array<float,64> input{}, left{}, right{};
    input.fill(1);
    const float* in[]{input.data(),input.data()};
    float* out[]{left.data(),right.data()};
    engine::NodeIo io;
    io.frames=64; io.channels=2; io.in=in; io.out=out;
    instance.process(io);
    require(left==right && std::isfinite(left[0]), "monitor produced invalid stereo");
    return left[0];
}

void bandTrimInProject() {
    using Core = dsp::AdiShaper;
    test::TempDirectory temp("adi_shaper_band_trim", "fader");
    std::string error;
    auto doc = ui::ProjectDocument::open(temp.path() / "trim.adi", true, device::withBuiltinDevices(), error);
    require(doc && doc->insertBuiltin(uid, 2, {}), "insert shaper for trim");
    doc->prepare(48000, 64);
    doc->tick(0);
    const auto id = doc->store().db().execAndGet("SELECT MAX(id) FROM devices").getInt64();
    ui::AdiShaperPanelSession session(*doc, id);
    auto binding = session.binding();
    auto* device = doc->session().instanceFor(id);
    const int trim = Core::VolumeMidTrim + 1;
    const std::array<Panel::Point,1> quarter{{{0, .25f, -1, Panel::Shape::SourceCorner}}};
    require(device && binding.hasBandTrim && binding.read(trim) == .5, "registered trim defaults to unity");
    require(binding.editWave(key, Phase::Begin, {}) && binding.editWave(key, Phase::Change, quarter) &&
            binding.editWave(key, Phase::End, quarter), "quarter-gain source");
    const auto steps = [&] { return History(doc->store()).readTimeline().steps.size(); };
    const auto start = steps();
    const std::array<Panel::Change,1> boost{{{trim, .75}}};
    require(binding.parameters(Phase::Begin, {}) && binding.parameters(Phase::Change, boost), "preview trim");
    require(std::abs(sample(*device) - 1) < .0001 && steps() == start, "trim reaches audio before release");
    require(binding.parameters(Phase::End, boost) && steps() == start + 1, "one undo step for trim");
    require(doc->ops().undo().ok && doc->synchronise(), "undo trim");
    doc->tick(1);
    session.refresh();
    require(binding.read(trim) == .5 && std::abs(sample(*device) - .25) < .0001, "undo restores gain and fader");
    require(doc->ops().redo().ok && doc->synchronise(), "redo trim");
    doc->tick(2);
    session.refresh();
    require(std::abs(binding.read(trim) - .75) < .00001 && std::abs(sample(*device) - 1) < .0001,
            "redo restores gain and fader");
}
void run() {
    test::TempDirectory temp("adi_shaper_session", "journal");
    const auto file=temp.path()/"song.adi";
    std::string error;
    auto doc=ui::ProjectDocument::open(file,true,device::withBuiltinDevices(),error);
    require(doc && doc->insertBuiltin(uid,2,{}),"insert registered device");
    doc->tick(0);
    const auto id=doc->store().db().execAndGet(
        "SELECT d.id FROM devices d JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE p.uid='org.adidaw.builtin.adi-shaper'").getInt64();
    const auto steps=[&] { return History(doc->store()).readTimeline().steps.size(); };
    const auto sync=[&](int tick) { require(doc->synchronise(),"synchronise project"); doc->tick(tick); };
    const auto count=steps();
    {
        ui::AdiShaperPanelSession session(*doc,id);
        auto b=session.binding();
        auto* device=doc->session().instanceFor(id);
        require(device && b.editWave && b.wave,"project runtime binding");
        device->prepare(48000,64);
        std::array<Panel::Point,2> pts{{{0,.75f,0,Panel::Shape::Hold},{.381f,.4f,.1f,Panel::Shape::Bend}}};
        require(b.editWave(key,Phase::Begin,{}) && b.editWave(key,Phase::Change,pts),"preview first drag");
        require(std::abs(sample(*device)-.75f)<.0001f && steps()==count,"first preview audible before history");
        pts[0].y=.25f;
        require(b.editWave(key,Phase::Change,pts) && std::abs(sample(*device)-.25f)<.0001f,"second live preview");
        require(b.editWave(key,Phase::End,pts) && steps()==count+1,"drag commits one step");
        sync(10);
        require(doc->ops().undo().ok,"undo drag"); sync(20); session.refresh();
        require(b.wave(key).empty() && std::abs(sample(*device)-1)<.0001f,"one undo clears all points in audio");
        require(doc->ops().redo().ok,"redo drag"); sync(30); session.refresh();
        require(b.wave(key).size()==2 && std::abs(sample(*device)-.25f)<.0001f,"redo all points in audio");
        pts[0].y=.6f;
        require(b.editWave(key,Phase::Begin,{}) && b.editWave(key,Phase::Change,pts) && b.editWave(key,Phase::Cancel,{}),"cancel drag");
        require(steps()==count+1 && std::abs(sample(*device)-.25f)<.0001f,"cancel restores without journaling");
        Payload state={{"version",1},{"uid",uid},{"params",{{"999999",.5}}}};
        require(!session.applyPreset(Payload::to_cbor(state)) && steps()==count+1,"reject unknown preset atomically");
        state["params"]=Payload::object();
        require(session.applyPreset(Payload::to_cbor(state),"Flat") && session.presetTitle()=="Flat" && steps()==count+2,"one-step sparse recall");
        sync(40); session.refresh();
        require(b.wave(key).empty() && std::abs(sample(*device)-1)<.0001f,"recall clears omitted slots");
        require(doc->ops().undo().ok,"undo recall"); sync(50); session.refresh();
        require(session.presetTitle().empty() && b.wave(key).size()==2 && std::abs(sample(*device)-.25f)<.0001f,"undo recall restores previous curve");
        const std::array<Panel::Change,1> half{{{1,.5}}}, dry{{{1,0}}};
        const auto previousSteps=steps();
        require(b.parameters(Phase::Begin,{}) && b.parameters(Phase::Change,half) &&
                std::abs(sample(*device)-.625f)<.0001f && b.parameters(Phase::End,half),"master mix drag auditions and commits");
        require(b.parameters(Phase::Begin,{}) && b.parameters(Phase::End,dry) &&
                std::abs(sample(*device)-1)<.0001f && steps()==previousSteps+2,"two successive control edits have separate undo steps");
        require(doc->ops().undo().ok,"undo second control"); sync(55);
        require(std::abs(sample(*device)-.625f)<.0001f,"undo restores exact intermediate control value");
        require(doc->deviceAction("device.remove",{{"id",id}}),"remove target");
        require(!b.parameters(Phase::Begin,{}),"stale binding refuses gesture");
        require(doc->ops().undo().ok,"restore target"); sync(60);
        require(!b.parameters(Phase::Begin,{}),"restored generation cannot be touched by former editor");
    }
    doc.reset();
    doc=ui::ProjectDocument::open(file,false,device::withBuiltinDevices(),error);
    require(static_cast<bool>(doc),"reopen project");
    doc->tick(70);
    ui::AdiShaperPanelSession reopened(*doc,id);
    require(reopened.binding().wave(key).size()==2,"curve survives project reopen");
}
}
int main() {
    try { run(); bandTrimInProject(); std::puts("ADI-Shaper project gesture, audio and recall tests passed"); return 0; }
    catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
