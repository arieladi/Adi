// SPDX-License-Identifier: AGPL-3.0-or-later
//
// WTS-01 (ADR-0248 d2, d12): the VST3 parts of the one-session acceptance suite. The repo's fixture
// instrument (the `ADI Expression Test` bundle's plain class, `ADI Test Plain`) is found by the app's loader
// the way the Browser's catalogue finds it (`JuceDeviceLoader::discover`), inserted onto a MIDI track from its catalogue row (ui_main.cpp
// `listingOf`, `loadPlugin`), played, and exported. A console app: no window and no plug-in editor
// opens. Step numbers follow `test_session_wts01.cpp`.
#include "wts01_harness.hpp"
#include "../temp_directory.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/scan_catalogue.hpp"
#include "adi/ui/export_choices.hpp"
#include "adi/ui/plugin_listing.hpp"
#include "juce/juce_device_loader.hpp"
#include "juce/vst3_host.hpp"
#include <chrono>
#include <optional>
using namespace adi;
using namespace adi::wts01;
using V = Report::Verdict;
namespace {
void session(Report &report) {
    namespace fs = std::filesystem;
    test::TempDirectory temp("wts01", "vst3");
    // The plug-in folder the Settings name, holding the fixture, and the app's catalogue registry.
    const auto modules = temp.path() / "VST3";
    fs::create_directory(modules);
    const auto fixture = media::pathFromUtf8(ADI_TEST_VST3);
    fs::copy(fixture, modules / fixture.filename(), fs::copy_options::recursive);
    scan::Config config;
    config.registry = temp.path() / "registry.sqlite";
    config.executable = media::pathFromUtf8(ADI_SCAN_HELPER);
    device::JuceDeviceLoader loader(false);
    loader.setScanConfig(config);
    loader.addSearchPath(juce::File(juce::String::fromUTF8(media::pathUtf8(modules).c_str())));
    const auto found = loader.discover(device::DiscoveryMode::Incremental);
    // The Browser's row for it (ui_main.cpp `listingOf`), then the reference the Browser loads (`pluginReference`).
    std::optional<ui::PluginEntry> row;
    std::string names;
    if (found && found->catalogue)
        for (const auto &d : found->catalogue->vst3) {
            const auto id = device::identityOf(d);
            names += id.name + (d.isInstrument ? " (instrument); " : "; ");
            if (!row && id.name == "ADI Test Plain")
                row = ui::PluginEntry{"vst3", id.uid, id.name, id.vendor, id.version, d.fileOrIdentifier.toStdString(),
                                      d.isInstrument ? ui::PluginEntry::Kind::Instrument : ui::PluginEntry::Kind::Effect};
        }
    std::string error;
    ui::RecordingDefaults defaults;
    auto doc = ui::ProjectDocument::open(temp.path() / "WTS01 VST3.adi", true, loader.fn(), error, &defaults);
    if (!row || !doc) {
        report.step("3c", V::Fail,
                    format("discovery %s, catalogue: %s; project %s", found ? "ran" : "did not run", names.c_str(),
                           doc ? "open" : error.c_str()),
                    "the fixture in the catalogue, a New project");
        return;
    }
    doc->prepare(kRate, kFrames);
    Driver driver(*doc);
    // 3c. The plug-in from the Browser onto 2 MIDI (id 3), as `loadPlugin` makes it.
    const auto ref = ui::pluginReference(*row, true);
    const bool inserted = doc->insertPlugin(ref, 3);
    const auto insertError = doc->error();
    const auto dev = scalar(*doc, "SELECT COALESCE(MAX(id), 0) FROM devices");
    const auto *entry = doc->session().entryFor(dev);
    const auto label = doc->ops().nextUndo() ? doc->ops().nextUndo()->label : std::string();
    report.check("3c", inserted && entry && !entry->placeholder && label == "Insert ADI Test Plain" && ref.subtype == "instrument",
                 format("catalogue: %s inserted %s; device %lld %s; undo \"%s\"", names.c_str(),
                        inserted ? "ok" : insertError.c_str(), static_cast<long long>(dev),
                        entry ? (entry->placeholder ? ("a placeholder: " + entry->error).c_str() : "loaded") : "absent",
                        label.c_str()),
                 "the fixture instrument loads on track 3, one step \"Insert ADI Test Plain\"");
    // 3d. Live notes into it: the same gap as OneShot's.
    report.step("3d", V::NotBuilt, "the app's MIDI port rings are drained into the indicators only; no document call",
                "a key played on the armed VST3 instrument is heard at its time",
                "the app consumer from MidiPortHost to Session::enqueueMidiInput (SRF-05 window half); owner window-fn");
    auto snap = doc->view().current();
    auto arm = ui::armTracks(snap->tracks, doc->recordStates(), 3, {3}, true, false);
    const bool armed = arm.refusal.empty() && submitEdits(*doc, std::move(arm.ops));
    const auto blockStart = driver.stream;
    constexpr std::int64_t k = 37;
    const bool queued =
        armed &&
        doc->session().enqueueMidiInput(blockStart + k, engine::Event{0, engine::EventType::NoteOn, 0, 69, 1, 0, 1.0}) &&
        doc->session().enqueueMidiInput(blockStart + k + 12000,
                                        engine::Event{0, engine::EventType::NoteOff, 0, 69, 1, 0, 0.0});
    Stereo heard;
    driver.run(60, &heard);
    std::int64_t first = -1;
    for (std::size_t i = 0; i < heard.size() && first < 0; ++i)
        if (std::abs(heard.c[0][i]) > 1e-4f)
            first = static_cast<std::int64_t>(i);
    // Rising zero crossings over 0.2 s from 10 ms after the onset.
    int rising = 0;
    const auto from = static_cast<std::size_t>(std::max<std::int64_t>(first, 0)) + 480;
    for (std::size_t i = from + 1; i < from + 9600 && i < heard.size(); ++i)
        rising += heard.c[0][i - 1] < 0 && heard.c[0][i] >= 0 ? 1 : 0;
    const double loud = peak(heard, from, from + 9600);
    report.engine("3d", format("armed %d, queued %d; first sound at +%lld frames from the block start (note at +%lld); "
                               "%.1f Hz; peak %.4f",
                               armed ? 1 : 0, queued ? 1 : 0, static_cast<long long>(first), static_cast<long long>(k),
                               rising / .2, loud),
                  "onset at +1 from the block start (the fixture plays events at offset 0, its sine starting at 0); "
                  "440 Hz +-5; peak 0.0713 "
                  "(0.3 x 0.25 x 0.95, pressure 0)");
    // 7d. Export with the VST3 in the project: refused on the Export page before a file is asked for.
    snap = doc->view().current();
    arm = ui::armTracks(snap->tracks, doc->recordStates(), 3, {3}, true, false);
    submitEdits(*doc, std::move(arm.ops));
    ui::ExportChoices choices;
    choices.lengthTicks = 2 * kBarTicks;
    choices.sampleRate = 48000;
    choices.bitDepth = 32;
    choices.dither = "No Dither";
    const bool prechecked = exportPrecheckRefuses(*doc);
    // And the render's own loader rule, had the page let it through.
    const auto through = render::renderWav(ui::exportJob(choices, doc->store().path(), temp.path() / "With VST3.wav",
                                                         *doc->view().current()->tempo),
                                           exportLoader(loader.fn()));
    report.step("7d", V::NotBuilt,
                format("the Export page refuses: %s; the render with its loader: %s", prechecked ? kExportRefusal : "no",
                       through.state == render::JobState::Completed ? "completed" : through.error.c_str()),
                "a project holding a VST3 exports", "the engine's render worker for VST3/CLAP (EX-11 engine half); owner engine");
    // 7e. Without it, the same project exports.
    const bool removed = doc->deviceAction("device.remove", Payload{{"id", dev}});
    const auto after = render::renderWav(ui::exportJob(choices, doc->store().path(), temp.path() / "Without.wav",
                                                       *doc->view().current()->tempo),
                                         exportLoader(loader.fn()));
    report.check("7e", removed && !exportPrecheckRefuses(*doc) && after.state == render::JobState::Completed &&
                           after.framesWritten == 2 * kBarSamples,
                 format("device removed %d; export %s, %llu frames", removed ? 1 : 0,
                        after.state == render::JobState::Completed ? "completed" : after.error.c_str(),
                        static_cast<unsigned long long>(after.framesWritten)),
                 "with the VST3 removed the export completes, 192000 frames");
    doc->release();
    doc->settleRecording();
    doc.reset();
}
} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI juceRuntime;
    Report report;
    try {
        session(report);
    } catch (const std::exception &e) {
        report.step("-", V::Fail, std::string("exception: ") + e.what(), "no exception");
    }
    std::printf("WTS01 vst3: %d FAIL\n", report.failures());
    return report.failures() == 0 ? 0 : 1;
}
