// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/juce/ui_analysis.hpp"
#include "../src/juce/ui_look.hpp"
#include "../src/juce/ui_mix_meter.hpp"
#include "../src/juce/ui_mix_analysers.hpp"
#include "ui_fake_peer.hpp"
#include "adi/engine/main_loudness.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <limits>
#include <numbers>
#include <vector>

using adi::engine::ScopePoint;
using adi::engine::ScopeTap;
using adi::ui::SharedAnalysis;
using adi::ui::SharedSpectrumView;
namespace adi::ui {
struct SharedSpectrumViewTestAccess {
    static auto capture(const SharedSpectrumView &view) { return view.captureShown_; }
    static std::size_t offsetCount(const SharedSpectrumView &view) { return view.spectrumProjection_.offsets().size(); }
};
}
namespace {
int checks = 0, failures = 0;
void check(bool value, const char *what) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", what); }
}
class Peer final : public UiFakePeer {
  public:
    explicit Peer(juce::Component &owner) : UiFakePeer(owner) {}
    bool isMinimised() const override { return minimized; }
    bool minimized = false;
};
class Root final : public juce::Component {
  public:
#if JUCE_VERSION >= 0x090005
    juce::ComponentPeer *createNewPeer(const juce::DesktopWindowOptions &) override { fake = new Peer(*this); return fake; }
#else
    juce::ComponentPeer *createNewPeer(int, void *) override { fake = new Peer(*this); return fake; }
#endif
    Peer *fake = nullptr;
};
struct Fixture {
    std::shared_ptr<ScopeTap> tap = std::make_shared<ScopeTap>(48000, SharedAnalysis::kTapSeconds);
    int acquired = 0, released = 0;
    std::shared_ptr<SharedAnalysis> broker = std::make_shared<SharedAnalysis>(
        [this](std::int64_t, ScopePoint) { ++acquired; return tap; },
        [this](std::int64_t, ScopePoint) { ++released; }, SharedAnalysis::Mode::Manual);
    void write(std::int64_t stamp = 0) {
        std::vector<float> samples(1024);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = .5f * static_cast<float>(std::sin(2 * std::numbers::pi * 16 * static_cast<double>(i) / 1024));
        tap->write(samples.data(), samples.data(), static_cast<std::int32_t>(samples.size()), stamp, true);
    }
};
bool originalRasterPixels(SharedSpectrumView &, const SharedAnalysis::Result &);
void sharingAndVisibility() {
    Fixture f;
    Root root;
    root.setBounds(0, 0, 640, 360);
    SharedSpectrumView full, compact;
    full.setBounds(0, 0, 300, 180);
    compact.setBounds(310, 0, 220, 180);
    full.present(f.broker, 1); compact.present(f.broker, 1);
    full.setVisible(true); compact.setVisible(true);
    full.frame(); compact.frame();
    check(f.acquired == 0 && !full.subscribed() && !compact.subscribed(),
          "offscreen components without a peer acquire no analysis");
    root.setVisible(true);
    root.addToDesktop(0); // Root creates only UiFakePeer: no OS window, display or native input.
    check(root.getPeer() && !root.getPeer()->getNativeHandle(), "test root has only a software peer");
    root.addAndMakeVisible(full); root.addAndMakeVisible(compact);
    full.frame(); compact.frame();
    check(full.subscribed() && compact.subscribed() && f.acquired == 1 &&
          f.broker->stats().domains == 1 && f.broker->stats().consumers == 2,
          "full and boxed components share one compatible analysis domain");
    check(full.status() == SharedSpectrumView::Status::Waiting && !full.snapshot(),
          "unwritten source shows waiting rather than an invented spectrum");
    f.write(); f.broker->pump(); full.frame(); compact.frame();
    check(full.status() == SharedSpectrumView::Status::Ready && full.snapshot() &&
          full.snapshot() == compact.snapshot(), "both components observe the exact immutable result");
    check(full.snapshot() && full.snapshot()->request.point == ScopePoint::PostFader && full.snapshot()->request.fftSize == 1024 &&
          full.snapshot()->request.channel == SharedAnalysis::Channel::Mid &&
          full.snapshot()->request.window == adi::dsp::Window::Hann,
          "full and compact use the declared shared calibration and tap");
    const auto repaints = root.fake->repaints;
    const auto snapshot = compact.snapshot();
    compact.frame();
    check(compact.snapshot() == snapshot && root.fake->repaints == repaints,
          "unchanged publication does not repaint");
    full.setVisible(false);
    check(!full.subscribed() && !full.snapshot() && compact.subscribed() && f.released == 0,
          "hiding the full view preserves boxed demand");
    root.setVisible(false);
    check(!compact.subscribed() && f.released == 1 && f.broker->stats().consumers == 0,
          "ancestor hide releases final demand without waiting for another frame");
    root.setVisible(true);
    compact.frame();
    check(compact.subscribed() && f.acquired == 2, "visible ancestor restores its visible child demand");
    compact.setSize(0, 0);
    check(!compact.subscribed() && f.released == 2, "empty component bounds release demand immediately");
    compact.setBounds(310, 0, 220, 180);
    check(compact.subscribed(), "nonempty visible bounds restore demand");
    compact.setTopLeftPosition(700, 0);
    check(!compact.subscribed() && f.released == 3, "fully clipped component acquires no hidden analysis");
    compact.setTopLeftPosition(310, 0);
    root.fake->minimized = true;
    compact.frame();
    check(!compact.subscribed() && f.released == 4, "a minimized owner releases demand when its frame is polled");
    root.fake->minimized = false;
    compact.frame();
    compact.suspend(true);
    check(!compact.subscribed() && f.released == 5, "owner suspension releases demand even if VBlank stops");
    compact.frame();
    check(!compact.subscribed(), "suspension survives frame calls until owner restores it");
    compact.suspend(false);
    check(compact.subscribed(), "owner restore reacquires only the visible view");
    root.removeChildComponent(&compact);
    check(!compact.subscribed() && f.released == 6, "removing a component from its peer hierarchy releases demand");
}
void sourceAndOwnerLifetime() {
    Fixture f;
    Root root;
    root.setBounds(0, 0, 640, 360); root.setVisible(true); root.addToDesktop(0);
    SharedSpectrumView view;
    view.setBounds(0, 0, 240, 180);
    root.addAndMakeVisible(view);
    view.present(f.broker, 1);
    f.write(); f.broker->pump(); view.frame();
    const auto old = view.snapshot();
    view.present(f.broker, 1);
    check(view.snapshot() == old && f.acquired == 1, "repeated presentation does not recreate a consumer");
    f.broker->refreshSources(true);
    view.frame();
    check(!view.snapshot() && view.status() == SharedSpectrumView::Status::Waiting,
          "seek invalidation clears the displayed source generation");
    f.write(100000); f.broker->pump(); view.frame();
    check(old && view.snapshot() && view.snapshot()->sourceEpoch > old->sourceEpoch,
          "new source generation replaces the displayed snapshot");
    const auto acquiredBeforeTrackChange = f.acquired, releasedBeforeTrackChange = f.released;
    view.present(f.broker, 2);
    check(f.released == releasedBeforeTrackChange + 1 && f.acquired == acquiredBeforeTrackChange + 1 &&
          !view.snapshot() && view.track() == 2,
          "track change releases old demand and cannot show the former track's result");
    f.tap.reset(); f.broker->refreshSources(); view.frame();
    check(view.subscribed() && !view.snapshot() && view.status() == SharedSpectrumView::Status::Unavailable,
          "missing analysis source is explicitly unavailable");
    f.broker->shutdown(); view.frame();
    check(!view.subscribed() && !view.snapshot() && view.status() == SharedSpectrumView::Status::Unavailable,
          "document revocation clears a view retaining the broker");
    view.present({}, 0);
    check(view.status() == SharedSpectrumView::Status::Detached && !view.subscribed(),
          "clearing the document leaves no analysis demand");
}
void parentDestruction() {
    Fixture f;
    Root root;
    root.setBounds(0, 0, 640, 360); root.setVisible(true); root.addToDesktop(0);
    SharedSpectrumView view;
    view.present(f.broker, 1); view.setBounds(0, 0, 240, 180);
    auto parent = std::make_unique<juce::Component>();
    parent->setBounds(0, 0, 300, 200);
    root.addAndMakeVisible(*parent); parent->addAndMakeVisible(view);
    check(view.subscribed(), "nested visible component holds demand");
    parent.reset();
    check(!view.subscribed() && f.released == 1,
          "ancestor destruction safely releases the child without a later frame");
}
void modesSourcesAndFreeze() {
    Fixture f;
    Root root;
    root.setBounds(0, 0, 900, 500); root.setVisible(true); root.addToDesktop(0);
    adi::ui::AnalysisPane pane;
    pane.setBounds(0, 0, 600, 400); root.addAndMakeVisible(pane);
    pane.frame(f.broker, 1, 2);
    check(pane.source() == adi::ui::AnalysisPane::Source::Main && pane.view.track() == 1,
          "analyzer opens on Main, independently of current channel selection");
    SharedSpectrumView compact;
    compact.setBounds(610, 0, 240, 200); root.addAndMakeVisible(compact); compact.present(f.broker, 1);
    pane.setMode(SharedSpectrumView::Mode::Spectrogram);
    f.write(); f.broker->pump(); pane.frame(f.broker, 1, 2); compact.frame();
    check(f.broker->stats().domains == 1 && pane.view.snapshot() == compact.snapshot() &&
          pane.view.snapshot()->columns.size() == 1, "spectrogram and spectrum share the same calibrated transform/publication");
    const auto otherView = compact.snapshot();
    pane.resetButton.onClick(); pane.frame(f.broker, 1, 2); compact.frame();
    check(!pane.view.snapshot() && compact.snapshot() == otherView,
          "Reset clears only this view's interval and waits for a new publication without resetting shared consumers");
    compact.setVisible(false);
    pane.setMode(SharedSpectrumView::Mode::Meters);
    const auto before = f.broker->stats().transforms;
    f.write(1024); f.broker->pump(); pane.frame(f.broker, 1, 2);
    check(pane.view.snapshot() && pane.view.snapshot()->binsDbfs.empty() &&
          f.broker->stats().transforms == before && !pane.channelBox.isEnabled(),
          "meter-only view publishes sample statistics without FFT or misleading spectrum controls");
    const auto frozen = pane.view.snapshot();
    pane.freezeButton.setToggleState(true, juce::dontSendNotification); pane.freezeButton.onClick();
    check(pane.view.frozen() && !pane.view.subscribed() && pane.view.snapshot() == frozen &&
          f.broker->stats().consumers == 0, "Freeze retains one immutable picture and releases all live analysis demand");
    pane.freezeButton.setToggleState(false, juce::dontSendNotification); pane.freezeButton.onClick();
    check(pane.view.subscribed() && !pane.view.snapshot(), "Resume waits for fresh audio instead of replaying hidden history");
    pane.setSource(adi::ui::AnalysisPane::Source::Selected); pane.frame(f.broker, 1, 2);
    check(pane.view.track() == 2 && !pane.view.snapshot(), "explicit Selected source follows current channel identity");
    pane.frame(f.broker, 1, 3);
    check(pane.view.track() == 3 && !pane.view.snapshot(), "selection replacement clears the old channel publication");
    pane.setPoint(ScopePoint::PreFader);
    pane.setMode(SharedSpectrumView::Mode::Scope);
    f.write(2048); f.broker->pump(); pane.frame(f.broker, 1, 3);
    check(pane.view.snapshot() && pane.view.snapshot()->scopeLeft.size() == 1024 &&
          pane.view.snapshot()->request.point == ScopePoint::PreFader, "Scope uses the selected explicit tap and contiguous raw samples");
    int expanded = 0;
    pane.onExpand = [&](std::int64_t track) { expanded = static_cast<int>(track); };
    pane.expandButton.onClick();
    check(expanded == 3, "window action carries exactly the shown channel identity");
    for (const auto mode : {SharedSpectrumView::Mode::Spectrum, SharedSpectrumView::Mode::Scope,
                            SharedSpectrumView::Mode::Meters, SharedSpectrumView::Mode::Phase,
                            SharedSpectrumView::Mode::Spectrogram}) {
        pane.setMode(mode); f.write(4096); f.broker->pump(); pane.frame(f.broker, 1, 3);
        juce::Image image(juce::Image::RGB, 600, 375, true);
        juce::Graphics graphics(image); pane.view.paint(graphics); // software-only component render, never desktop capture
        check(pane.view.snapshot() && pane.view.mode() == mode, "each implemented mode consumes a real publication and paints offscreen");
    }
    root.setVisible(false);
    check(!pane.view.subscribed() && f.broker->stats().consumers == 0, "hiding analyzer controls releases mode demand without a frame");
}
void pendingSourceLease() {
    std::shared_ptr<ScopeTap> tap;
    int releases = 0, resolutions = 0;
    auto broker = std::make_shared<SharedAnalysis>([&](adi::engine::ScopeSource, double) {
        ++resolutions; return tap;
    }, [&](adi::engine::ScopeSource) { ++releases; });
    Root root;
    root.setBounds(0, 0, 640, 360); root.setVisible(true); root.addToDesktop(0);
    SharedSpectrumView view;
    view.setBounds(0, 0, 300, 180); root.addAndMakeVisible(view); view.present(broker, 1);
    view.frame();
    check(view.status() == SharedSpectrumView::Status::Unavailable && resolutions >= 2 &&
          broker->stats().workerPasses == 0, "a pending visible lease resolves on frames without starting unavailable analysis jobs");
    view.setVisible(false);
    check(releases == 1 && broker->stats().consumers == 0,
          "hiding a never-available source releases its pending endpoint lease exactly once");
    view.setVisible(true); view.frame();
    tap = std::make_shared<ScopeTap>(48000, .25);
    view.frame();
    check(view.subscribed() && view.status() == SharedSpectrumView::Status::Waiting,
          "a visible pending source becomes ready for fresh audio without a project edit or another available view");
    view.setVisible(false);
    check(releases == 2 && broker->stats().consumers == 0, "resolved pending source also releases at final demand");
}
void cappedFrozenHistoryAndIdleReset() {
    auto tap = std::make_shared<ScopeTap>(48000, 1.);
    constexpr std::size_t bytes = 513 * sizeof(float);
    auto broker = std::make_shared<SharedAnalysis>([&](std::int64_t, ScopePoint) { return tap; },
        [](std::int64_t, ScopePoint) {}, SharedAnalysis::Mode::Manual, bytes);
    Root root; root.setBounds(0, 0, 400, 340); root.setVisible(true); root.addToDesktop(0);
    SharedSpectrumView live, frozen;
    live.setMode(SharedSpectrumView::Mode::Spectrogram); frozen.setMode(SharedSpectrumView::Mode::Spectrogram);
    // Exactly one raster pixel per hop and one raster pixel per logarithmic row.
    live.setBounds(0, 0, 156, 311); frozen.setBounds(180, 0, 156, 311);
    root.addAndMakeVisible(live); root.addAndMakeVisible(frozen);
    live.present(broker, 1); frozen.present(broker, 1);
    const auto write = [&](int frames, std::int64_t stamp) {
        std::vector<float> samples(static_cast<std::size_t>(frames));
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = .5f * static_cast<float>(std::sin(2 * std::numbers::pi * 16 * static_cast<double>(i) / 1024));
        tap->write(samples.data(), samples.data(), frames, stamp, true);
    };
    write(1024, 0); broker->pump(); live.frame(); frozen.frame(); frozen.freeze(true);
    check(live.snapshot() && live.snapshot() == frozen.snapshot() && broker->stats().historyBytes == bytes,
          "two views and Freeze retain one charged immutable history payload");
    write(256, 1024); broker->pump(); live.frame();
    write(256, 1280); broker->pump(); live.frame();
    check(live.snapshot()->historyLimited && live.snapshot()->historySerial == 3 && live.snapshot()->columns.empty() &&
          frozen.snapshot()->historySerial == 1 && broker->stats().historyBytes == bytes,
          "frozen renderer retention caps live history honestly without overwriting its picture");
    frozen.setVisible(false);
    const auto before = broker->stats();
    broker->pump(); live.frame();
    check(live.snapshot() && !live.snapshot()->historyLimited && live.snapshot()->columns.size() == 1 &&
          live.snapshot()->columns.back().serial == 3 && broker->stats().transforms == before.transforms,
          "hiding the frozen picture frees capacity and resumes a pending hop without source audio or FFT");
    juce::Image image(juce::Image::RGB, 156, 311, true);
    { juce::Graphics graphics(image); live.paint(graphics); }
    bool missingBlank = true, currentVisible = false;
    const auto background = image.getPixelAt(0, 0);
    for (int y = 21; y < 277; ++y) {
        missingBlank = missingBlank && image.getPixelAt(131, y) == background;
        currentVisible = currentVisible || image.getPixelAt(132, y) != background;
    }
    check(missingBlank && currentVisible,
          "software spectrogram leaves the missing hop blank instead of compressing history time");
    check(originalRasterPixels(live, *live.snapshot()), "default capped/gapped raster pixels match the original display law");
    live.clear();
    SharedAnalysis::Request scope{1}; scope.outputs = SharedAnalysis::Scope;
    auto extra = broker->acquire(scope); broker->pump(); live.frame();
    check(!live.snapshot() && live.status() == SharedSpectrumView::Status::Waiting,
          "local Reset cannot redisplay the old interval after an idle output-demand publication");
    const auto idle = broker->stats();
    for (int i = 0; i < 8; ++i) { broker->pump(); live.frame(); }
    check(broker->stats().analysisWindows == idle.analysisWindows && broker->stats().workerPasses == idle.workerPasses,
          "visible idle component frames have no repeated analysis or aggregation");
    extra.reset(); root.setVisible(false);
    check(broker->stats().consumers == 0 && broker->stats().spectralPlans == 0 && broker->stats().historyBytes == 0,
          "hidden analysis and released frozen views leave no spectral plan or history backing");
}
void mainReadoutFormatProjection() {
    adi::engine::MainLoudness meter;
    check(!adi::ui::readMainMeterSnapshot(meter, 1, 48000), "unprepared Main service cannot provide a display snapshot");
    check(meter.prepare(48000, 2) && !adi::ui::readMainMeterSnapshot(meter, 1, 48000),
          "prepared format is not presented before audio adopts its identity");
    std::array<float, 256> samples{}; samples.fill(.25f);
    std::array<float *, 2> output{samples.data(), samples.data()};
    adi::engine::AudioIo io; io.out = output.data(); io.numOut = 2; io.frames = 256;
    meter.process(io);
    const auto first = adi::ui::readMainMeterSnapshot(meter, 1, 48000);
    check(first && first->formatSequence == meter.preparedFormatSequence() &&
          std::isfinite(first->truePeakDb), "Main projection reads the adopted format and existing held true peak");
    check(meter.prepare(44100, 2) && !adi::ui::readMainMeterSnapshot(meter, 1, 44100) &&
          std::isfinite(meter.readout().truePeakDb.load()),
          "old-format held values are suppressed while a new prepared format awaits adoption");
    meter.process({});
    const auto second = adi::ui::readMainMeterSnapshot(meter, 1, 44100);
    check(second && first && second->formatSequence != first->formatSequence &&
          std::isinf(second->momentaryLufs) && std::isinf(second->truePeakDb),
          "new adopted format exposes cleared service values without UI reset or meter processing");
    check(!adi::ui::readMainMeterSnapshot(meter, 0, 44100) &&
          !adi::ui::readMainMeterSnapshot(meter, 1, std::numeric_limits<double>::quiet_NaN()),
          "invalid presentation identity/rate cannot claim Main results");
}
void mainMetersSharedPresentation() {
    Fixture f;
    Root root; root.setBounds(0, 0, 900, 500); root.setVisible(true); root.addToDesktop(0);
    Root floating; floating.setBounds(0, 0, 400, 300); floating.setVisible(true); floating.addToDesktop(0);
    SharedSpectrumView full, info, window;
    full.setBounds(0, 0, 400, 240); info.setBounds(410, 0, 240, 196); window.setBounds(0, 0, 350, 230);
    root.addAndMakeVisible(full); root.addAndMakeVisible(info); floating.addAndMakeVisible(window);
    adi::ui::MainMeterSnapshot values;
    values.track = 1; values.sampleRate = 48000; values.formatSequence = 1;
    values.momentaryLufs = -14.2f; values.shortTermLufs = -15.3f; values.integratedLufs = -16.4f; values.truePeakDb = -.7f;
    int reads = 0; bool ready = true;
    std::shared_ptr<const adi::ui::MainMeterSource> source = std::make_shared<adi::ui::MainMeterSource>(
        adi::ui::MainMeterSource{1, [&]() -> std::optional<adi::ui::MainMeterSnapshot> {
            ++reads;
            if (!ready) return {};
            return values;
        }});
    for (auto *view : {&full, &info, &window}) { view->setMainMeterSource(source); view->present(f.broker, 1); }
    f.write(); f.broker->pump(); full.frame(); info.frame(); window.frame();
    check(reads == 0 && !full.mainMeterSnapshot(), "default Spectrum performs no Main readout callbacks");
    const auto paint = [](SharedSpectrumView &view) {
        juce::Image image(juce::Image::RGB, view.getWidth(), view.getHeight(), true);
        juce::Graphics graphics(image); view.paint(graphics); return image;
    };
    const auto spectrumWithBinding = paint(full);
    full.setMainMeterSource({});
    const auto spectrumWithoutBinding = paint(full);
    bool samePixels = true;
    for (int y = 0; y < full.getHeight(); ++y) for (int x = 0; x < full.getWidth(); ++x)
        samePixels = samePixels && spectrumWithBinding.getPixelAt(x, y) == spectrumWithoutBinding.getPixelAt(x, y);
    check(samePixels, "default Spectrum software image is unchanged by the optional Main binding");
    full.setMainMeterSource(source);
    for (auto *view : {&full, &info, &window}) view->setMode(SharedSpectrumView::Mode::Meters);
    const auto transforms = f.broker->stats().transforms;
    f.write(1024); f.broker->pump(); full.frame(); info.frame(); window.frame();
    check(full.mainMeterSnapshot() == values && info.mainMeterSnapshot() == values && window.mainMeterSnapshot() == values &&
          full.snapshot() == info.snapshot() && info.snapshot() == window.snapshot(),
          "full, boxed Info and floating renderers reuse one supplied Main readout and sample publication");
    check(f.broker->stats().transforms == transforms && f.broker->stats().domains == 1 && f.acquired == 1,
          "Main readouts add no FFT, source cursor or tap to compatible Meters views");
    const auto text = full.mainMeterText();
    check(text.contains("Main aggregate programme / pre-cue") && text.contains("M -14.2") &&
          text.contains("S -15.3") && text.contains("I -16.4 LUFS") && text.contains("TP (held) -0.7 dBTP"),
          "Main text names aggregate pre-cue programme, independent LUFS windows and held dBTP at one decimal");
    (void)paint(full); (void)paint(info); (void)paint(window);
    const int minimisedReads = reads; root.fake->minimized = true; floating.fake->minimized = true;
    full.frame(); info.frame(); window.frame();
    check(reads == minimisedReads && !full.mainMeterSnapshot() && !window.mainMeterSnapshot(),
          "minimised owning windows release Main presentation without invoking its reader");
    root.fake->minimized = false; floating.fake->minimized = false;
    full.frame(); info.frame(); window.frame(); f.write(2048); f.broker->pump(); full.frame(); info.frame(); window.frame();
    const auto beforeFreeze = full.mainMeterSnapshot(); full.freeze(true);
    const int frozenReads = reads; values.integratedLufs = -12.5f;
    full.frame(); check(reads == frozenReads && full.mainMeterSnapshot() == beforeFreeze,
                        "Freeze keeps only a local Main picture without polling or pausing the shared service");
    info.frame(); check(info.mainMeterSnapshot() == values, "another live view keeps reading while its peer is frozen");
    const auto peer = info.mainMeterSnapshot(); full.clear(); full.frame();
    check(!full.mainMeterSnapshot() && info.mainMeterSnapshot() == peer,
          "Reset clears one frozen presentation without altering another view or Main values");
    full.freeze(false); full.frame();
    check(!full.mainMeterSnapshot(), "resume after local clear waits for a new sample interval");
    f.write(3072); f.broker->pump(); full.frame();
    check(full.mainMeterSnapshot() == values, "fresh interval resumes the ongoing held Main measurement");
    info.present(f.broker, 2); f.broker->pump(); const auto selectedReads = reads; info.frame();
    check(reads == selectedReads && !info.mainMeterSnapshot(), "Selected non-Main source never polls or displays Main loudness");
    for (const auto point : {ScopePoint::PreFader, ScopePoint::ChainInput, ScopePoint::DeviceInput, ScopePoint::DeviceOutput}) {
        full.setSource(point, point == ScopePoint::DeviceInput || point == ScopePoint::DeviceOutput ? 12 : 0);
        f.broker->pump(); const int wrongPointReads = reads; full.frame();
        check(reads == wrongPointReads && !full.mainMeterSnapshot(), "non-post-Main/device points cannot display programme loudness");
    }
    full.setSource(ScopePoint::PostFader); f.broker->pump(); full.frame();
    ready = false; full.frame(); check(!full.mainMeterSnapshot(), "unavailable owner projection clears old Main values");
    ready = true; values.formatSequence = 0; full.frame();
    check(!full.mainMeterSnapshot(), "unadopted format sequence cannot present old numeric values");
    values.formatSequence = 2; values.sampleRate = 44100; full.frame();
    check(!full.mainMeterSnapshot(), "readout rate must match the current source publication");
    f.tap = std::make_shared<ScopeTap>(44100, SharedAnalysis::kTapSeconds); f.broker->refreshSources(true);
    f.write(); f.broker->pump(); full.frame();
    // The first owner frame observes the replacement activation. Supply an
    // interval containing post-activation audio before checking its projection.
    f.write(1024); f.broker->pump(); full.frame();
    check(full.mainMeterSnapshot() == values && full.snapshot()->sampleRate == 44100,
          "new source format accepts only its matching adopted Main projection");
    values.momentaryLufs = values.shortTermLufs = values.integratedLufs = values.truePeakDb = -std::numeric_limits<float>::infinity();
    full.frame();
    check(full.mainMeterText().contains("M --") && full.mainMeterText().contains("TP (held) -- dBTP") &&
          !full.mainMeterText().containsIgnoreCase("warming"),
          "negative infinity is shown honestly without claiming silence is necessarily an unwarmed window");
    values.truePeakDb = std::numeric_limits<float>::quiet_NaN(); full.frame();
    check(!full.mainMeterSnapshot(), "invalid readout numbers never leak into the display");
    values.truePeakDb = -.5f; full.frame(); full.freeze(true);
    Fixture replacement;
    replacement.tap = std::make_shared<ScopeTap>(44100, SharedAnalysis::kTapSeconds);
    auto newValues = values;
    newValues.integratedLufs = -20;
    int newReads = 0;
    auto newSource = std::make_shared<adi::ui::MainMeterSource>(adi::ui::MainMeterSource{
        1, [&]() -> std::optional<adi::ui::MainMeterSnapshot> { ++newReads; return newValues; }});
    full.setMainMeterSource(newSource);
    check(full.frozen() && !full.mainMeterSnapshot() && newValues.track == values.track &&
          newValues.sampleRate == values.sampleRate && newValues.formatSequence == values.formatSequence,
          "new document binding invalidates frozen Main values even when all numeric source/format identities match");
    full.present(replacement.broker, 1); full.freeze(false); full.frame();
    check(!full.mainMeterSnapshot(), "document/source replacement cannot retain its old frozen Main numbers");
    replacement.write(); replacement.broker->pump(); full.frame();
    check(full.mainMeterSnapshot() == newValues && newReads == 1,
          "replacement document reads only its new Main identity and owner callback");
    full.freeze(true); newSource.reset(); full.frame();
    check(!full.mainMeterSnapshot(), "revoking the document callback clears a retained frozen Main picture");
    source.reset();
    const int hiddenReads = reads, hiddenNewReads = newReads; root.setVisible(false); floating.setVisible(false);
    full.frame(); info.frame(); window.frame();
    check(reads == hiddenReads && newReads == hiddenNewReads && f.broker->stats().consumers == 0 &&
          replacement.broker->stats().consumers == 0, "hidden full/Info/floating views poll no Main values or analysis");
    full.present({}, 0);
}

void permanentMainMeter() {
    using adi::ui::MixOutputMeter;
    Fixture f;
    std::vector<MixOutputMeter::Audition> calls;
    Root root;
    juce::Component owner;
    MixOutputMeter meter;
    root.setSize(640, 560);
    root.addAndMakeVisible(owner);
    owner.setBounds(root.getLocalBounds());
    owner.addAndMakeVisible(meter);
    meter.setBounds(0, 0, 152, 520);
    root.setVisible(true);
    root.addToDesktop(0);
    int reads = 0;
    const auto source = std::make_shared<adi::ui::MainMeterSource>(adi::ui::MainMeterSource{
        1, [&]() -> std::optional<adi::ui::MainMeterSnapshot> {
            ++reads;
            return adi::ui::MainMeterSnapshot{1, 48000., 1, -16.f, -16.f, -16.f, -.9f};
        }});
    meter.frame(f.broker, 1, source);
    check(meter.subscribed() && !meter.reading() && !meter.listenBox.isVisible() && reads == 0 &&
              f.broker->stats().spectralPlans == 0,
          "visible Main meter subscribes once without FFT or an unowned listen action");
    f.write(); f.broker->pump(); meter.frame(f.broker, 1, source);
    const auto initial = meter.reading();
    check(initial && std::abs(initial->reading.maximumPeak[0] - .5) < 1.e-6 && meter.loudness() && reads == 1,
          "Main meter publishes the stereo envelope and projects one existing loudness snapshot");
    const auto beforeScale = f.broker->stats();
    meter.scaleBox.setSelectedId(3, juce::sendNotificationSync);
    meter.frame(f.broker, 1, source);
    const auto afterScale = f.broker->stats();
    check(meter.reading() == initial && afterScale.publications == beforeScale.publications &&
              afterScale.spectralPlans == beforeScale.spectralPlans,
          "K scale relabels the same reading without a new publication or plan");
    const auto releaseMouse = [&](juce::Point<float> down, juce::Point<float> up, juce::ModifierKeys buttons,
                                  bool dragged = false) {
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent event{juce::Desktop::getInstance().getMainMouseSource(), up, buttons,
                                    1.f, 0.f, 0.f, 0.f, 0.f, &meter, &meter, now, down, now, 1, dragged};
        meter.mouseUp(event);
    };
    const juce::Point<float> plotClick{76.f, 160.f};
    const juce::Point<float> controlsClick = meter.weightingBox.getBounds().getCentre().toFloat();
    {
        auto otherReader = f.broker->acquireStereoMeter(1, {});
        check(otherReader.readStereoMeter() == initial, "another same-profile reader shares the Main interval");
        releaseMouse(plotClick, plotClick, juce::ModifierKeys::rightButtonModifier);
        releaseMouse(plotClick, plotClick, juce::ModifierKeys::leftButtonModifier, true);
        releaseMouse(plotClick, controlsClick, juce::ModifierKeys::leftButtonModifier);
        releaseMouse(controlsClick, plotClick, juce::ModifierKeys::leftButtonModifier);
        check(meter.reading() == initial && otherReader.readStereoMeter() == initial,
              "only a primary click within the plot restarts the meter");
        const auto beforeReset = reads;
        releaseMouse(plotClick, plotClick, juce::ModifierKeys::leftButtonModifier);
        meter.frame(f.broker, 1, source);
        check(!meter.reading() && !meter.loudness() && !otherReader.readStereoMeter() && reads == beforeReset,
              "a click on the meter restarts the shared interval without reading or resetting Main loudness");
        const auto afterClick = f.broker->stats();
        check(meter.subscribed() && afterClick.consumers == 2 && afterClick.domains == 1 &&
                  afterClick.spectralPlans == 0 && f.acquired == 1 && f.released == 0,
              "meter click keeps both observers on one live tap without an FFT");
        f.write(1024); f.broker->pump(); meter.frame(f.broker, 1, source);
        check(meter.reading() && meter.reading() == otherReader.readStereoMeter() && meter.loudness() && reads == beforeReset + 1,
              "both observers resume at the first new result and Main loudness is projected again");
    }
    meter.channelBox.setSelectedId(2, juce::sendNotificationSync);
    meter.weightingBox.setSelectedId(2, juce::sendNotificationSync);
    meter.frame(f.broker, 1, source);
    check(!meter.reading() && meter.scaleBox.getSelectedId() == 3, "M/S and A weighting start a fresh measurement, retaining its scale");
    f.write(2048); f.broker->pump(); meter.frame(f.broker, 1, source);
    check(meter.reading() && meter.reading()->reading.maximumPeak[1] == 0.,
          "in-phase stereo shows no Side signal after changing the meter profile");
    meter.resetButton.onClick();
    meter.frame(f.broker, 1, source);
    check(!meter.reading() && !meter.loudness(), "Reset drops only the meter interval and its compact readout");
    meter.setAuditionSink([&](auto mode) { calls.push_back(mode); });
    meter.listenBox.setSelectedId(5, juce::sendNotificationSync);
    meter.weightingBox.setSelectedId(3, juce::sendNotificationSync);
    check(calls == std::vector{MixOutputMeter::Audition::Side}, "passive weighting leaves other monitor modes alone");
    meter.listenBox.setSelectedId(6, juce::sendNotificationSync);
    meter.weightingBox.setSelectedId(4, juce::sendNotificationSync);
    meter.weightingBox.setSelectedId(1, juce::sendNotificationSync);
    check(meter.listenBox.getSelectedId() == 6 &&
              calls == std::vector{MixOutputMeter::Audition::Side, MixOutputMeter::Audition::FilterC,
                                   MixOutputMeter::Audition::FilterK, MixOutputMeter::Audition::FilterFlat},
          "Filter Listen follows the active weighting while keeping one selected Listen item");
    meter.frame(f.broker, 1, source);
    check(meter.subscribed(), "the changed weighting acquires a meter lease before its owner hides");
    owner.setVisible(false);
    check(!meter.subscribed() && f.broker->stats().consumers == 0 &&
              calls.size() == 5 && calls.back() == MixOutputMeter::Audition::Stereo && meter.listenBox.getSelectedId() == 1,
          "an ancestor hide releases the meter and ends Filter Listen");
    const auto hiddenReads = reads;
    f.write(3072); f.broker->pump(); meter.frame(f.broker, 1, source);
    check(!meter.reading() && reads == hiddenReads && f.broker->stats().consumers == 0,
          "hidden meter frames make no new analysis demand and poll no loudness");
    owner.setVisible(true);
    meter.suspend(true); meter.frame(f.broker, 1, source);
    check(!meter.subscribed(), "explicit minimisation suspends even when the owner component remains visible");
    meter.suspend(false); meter.frame(f.broker, 1, source);
    check(meter.subscribed() && !meter.reading(), "restored meter waits for fresh data");
    meter.setAuditionSink({});
    root.removeFromDesktop();
}

void parkedMainLoudnessProjection() {
    using namespace adi::ui;
    Fixture f;
    Root root;
    MixOutputMeter meter;
    root.setSize(240, 520);
    meter.setBounds(root.getLocalBounds());
    root.addAndMakeVisible(meter);
    root.setVisible(true); root.addToDesktop(0);
    int reads = 0;
    MainMeterSnapshot snapshot{1, 48000., 1, -10.f, -12.f, -13.f, -.8f};
    const auto source = std::make_shared<MainMeterSource>(MainMeterSource{
        1, [&] { ++reads; return std::optional(snapshot); }});
    meter.frame(f.broker, 1, source);
    f.tap->write(static_cast<const float *>(nullptr), nullptr, 480, 0, false, 1);
    f.broker->pump(); meter.frame(f.broker, 1, source);
    const auto envelope = meter.reading();
    check(envelope && meter.loudness() == snapshot && reads == 1,
          "parked fixture presents the existing independent Main loudness");
    const auto stable = f.broker->stats();
    f.tap->write(static_cast<const float *>(nullptr), nullptr, 480, 0, false, 1);
    f.broker->pump();
    const auto beforeTruePeak = root.fake->repaints;
    snapshot.truePeakDb = -.4f;
    meter.frame(f.broker, 1, source);
    check(meter.reading() == envelope && meter.loudness() == snapshot && reads == 2 &&
              root.fake->repaints > beforeTruePeak,
          "held true peak refreshes independently of the parked stereo publication");
    const auto beforeMomentary = root.fake->repaints;
    snapshot.momentaryLufs = -std::numeric_limits<float>::infinity();
    meter.frame(f.broker, 1, source);
    check(meter.reading() == envelope && meter.loudness() == snapshot && reads == 3 &&
              root.fake->repaints > beforeMomentary,
          "the parked meter's ghost momentary value clears when Main loudness reaches silence");
    const auto stablePaint = root.fake->repaints;
    meter.frame(f.broker, 1, source);
    const auto after = f.broker->stats();
    check(reads == 4 && root.fake->repaints == stablePaint && after.publications == stable.publications &&
              after.workerPasses == stable.workerPasses && after.cursorReads == stable.cursorReads &&
              after.spectralPlans == 0,
          "an unchanged loudness frame makes no paint or analysis work");
    snapshot.sampleRate = 44100.;
    meter.frame(f.broker, 1, source);
    check(meter.reading() == envelope && !meter.loudness() && reads == 5, "a stale loudness format clears the overlay");
    meter.suspend(true); meter.frame(f.broker, 1, source);
    check(!meter.reading() && !meter.loudness() && reads == 5 && f.broker->stats().consumers == 0,
          "suspending an eligible meter releases its lease and stops loudness polling");
    root.removeFromDesktop();
}

void compactAnalyzerDock() {
    Fixture f;
    Root root;
    root.setBounds(0, 0, 520, 620); root.setVisible(true); root.addToDesktop(0);
    adi::ui::AnalysisDock dock;
    root.addAndMakeVisible(dock); dock.setBounds(0, 0, 240, 600);
    dock.frame(f.broker, 1, 2);
    check(dock.source() == 0 && dock.point() == 0 && dock.channel() == 0 &&
              dock.meters.track() == 1 && dock.phase.track() == 1 && dock.spectrum.track() == 1,
          "compact dock starts on Main/Post Mixer with one shared channel convention");
    const auto stats = f.broker->stats();
    check(stats.consumers == 3 && stats.domains == 1 && stats.taps == 1 && f.acquired == 1,
          "stacked meter/phase/spectrum request one compatible domain and tap");
    f.write(); f.broker->pump(); dock.frame(f.broker, 1, 2);
    const auto picture = dock.spectrum.snapshot();
    check(picture && dock.meters.snapshot() == picture && dock.phase.snapshot() == picture &&
              (picture->outputs & (SharedAnalysis::Meters | SharedAnalysis::Phase | SharedAnalysis::Spectrum)) ==
                  (SharedAnalysis::Meters | SharedAnalysis::Phase | SharedAnalysis::Spectrum),
          "all dock layers consume the same published interval and output-mask union");
    bool contained = true;
    const std::array<juce::Component *, 7> controls{&dock.sourceBox, &dock.pointBox, &dock.channelBox, &dock.widthBox,
        &dock.freezeButton, &dock.resetButton, &dock.expandButton};
    for (std::size_t i = 0; i < controls.size(); ++i) {
        contained = contained && dock.getLocalBounds().contains(controls[i]->getBounds());
        for (std::size_t j = i + 1; j < controls.size(); ++j)
            contained = contained && !controls[i]->getBounds().intersects(controls[j]->getBounds());
    }
    check(contained && dock.meters.getBottom() < dock.phase.getY() && dock.phase.getBottom() < dock.spectrum.getY(),
          "compact controls and three labeled projections fit 240 px without overlap");
    dock.setSize(240, 420);
    check(dock.meters.getHeight() >= 104 && dock.phase.getHeight() >= 66 && dock.spectrum.getHeight() >= 66,
          "minimum admitted dock height retains space for Main footer and each existing plot");
    int reads = 0;
    auto source = std::make_shared<adi::ui::MainMeterSource>(adi::ui::MainMeterSource{1,
        [&]() -> std::optional<adi::ui::MainMeterSnapshot> {
            ++reads; return adi::ui::MainMeterSnapshot{1, 48000, 1, -12, -13, -14, -1}; }});
    dock.meters.setMainMeterSource(source); dock.frame(f.broker, 1, 2);
    check(dock.meters.mainMeterSnapshot() && !dock.phase.mainMeterSnapshot() && !dock.spectrum.mainMeterSnapshot() && reads == 1,
          "only the dock meter projects the existing aggregate Main loudness");
    dock.freezeButton.setToggleState(true, juce::dontSendNotification); dock.freezeButton.onClick();
    check(dock.meters.frozen() && dock.phase.frozen() && dock.spectrum.frozen() && f.broker->stats().consumers == 0,
          "dock Freeze releases all three local demands");
    dock.resetButton.onClick();
    check(!dock.meters.snapshot() && !dock.phase.snapshot() && !dock.spectrum.snapshot() && reads == 1,
          "dock Reset clears only its local pictures without invoking Main reset or reads");
    dock.freezeButton.setToggleState(false, juce::dontSendNotification); dock.freezeButton.onClick();
    dock.configure(1, 2, 3, 360); dock.frame(f.broker, 1, 2);
    check(dock.meters.track() == 2 && dock.spectrum.point() == ScopePoint::ChainInput &&
              dock.spectrum.channel() == SharedAnalysis::Channel::Right && !dock.meters.mainMeterSnapshot() && reads == 1,
          "Selected/pre-FX/right shares a new source without Main-only loudness");
    dock.setVisible(false);
    check(f.broker->stats().consumers == 0 && !dock.meters.subscribed() && !dock.phase.subscribed() && !dock.spectrum.subscribed(),
          "hiding dock releases every lease immediately without another frame");
    dock.setVisible(true); dock.frame(f.broker, 1, 2); dock.suspend(true);
    check(f.broker->stats().consumers == 0, "minimizing its owner suspends all dock modes");
    dock.suspend(false); dock.frame(f.broker, 1, 2);
    check(f.broker->stats().consumers == 3 && f.broker->stats().domains == 1, "restoring reacquires one shared dock domain");
    source.reset(); dock.configure(0, 0, 0, 300); dock.frame(f.broker, 1, 2);
    check(!dock.meters.mainMeterSnapshot(), "expired document callback cannot survive in a dock readout");
    dock.frame({}, 0, 0);
    check(f.broker->stats().consumers == 0 && !dock.spectrum.snapshot(), "document replacement releases the old dock publication");
}

juce::Image softwarePicture(SharedSpectrumView &view) {
    juce::Image image(juce::Image::RGB, view.getWidth(), view.getHeight(), true);
    juce::Graphics graphics(image); view.paint(graphics); return image;
}
bool samePixels(const juce::Image &a, const juce::Image &b) {
    if (a.getBounds() != b.getBounds()) return false;
    for (int y = 0; y < a.getHeight(); ++y) for (int x = 0; x < a.getWidth(); ++x)
        if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) return false;
    return true;
}
void paletteMenuTheme() {
    using namespace adi::ui;
    Root root;
    AnalysisPane pane;
    root.setBounds(0, 0, 500, 420);
    root.setVisible(true);
    root.addToDesktop(0);
    root.addAndMakeVisible(pane);
    pane.setBounds(root.getLocalBounds());
    pane.setMode(SharedSpectrumView::Mode::Spectrogram);
    pane.setPalette("rocket", true);

    const auto swatch = [&pane](std::size_t index) {
        const auto id = static_cast<int>(index) + 1;
        for (juce::PopupMenu::MenuItemIterator item(*pane.paletteBox.getRootMenu()); item.next();) {
            if (item.getItem().itemID != id) continue;
            if (const auto *preview = dynamic_cast<const juce::DrawableImage *>(item.getItem().image.get()))
                return preview->getImage();
        }
        return juce::Image{};
    };
    const auto oldTheme = look::themeId();
    const auto density = look::densityId();
    const auto initialDefault = swatch(palettes::defaultIndex);
    const auto rocket = palettes::find("rocket");
    const auto initialStock = swatch(rocket);
    check(initialDefault.isValid() && initialDefault.getPixelAt(0, 0) == look::display && initialStock.isValid(),
          "an open analyser pane uses the current theme's default and a fixed stock swatch");

    look::apply(oldTheme == ThemeId::Light ? ThemeId::Dark : ThemeId::Light, density);
    pane.frame({}, 0, 0);
    const auto currentDefault = swatch(palettes::defaultIndex);
    check(currentDefault.isValid() && currentDefault.getPixelAt(0, 0) == look::display &&
              !samePixels(initialDefault, currentDefault) && swatch(rocket) == initialStock &&
              pane.requestedPalette() == "rocket" && pane.reverseButton.getToggleState(),
          "changing theme refreshes only the default thumbnail and preserves the palette selection");
    const auto repaints = root.fake->repaints;
    pane.frame({}, 0, 0);
    pane.frame({}, 0, 0);
    check(swatch(palettes::defaultIndex) == currentDefault && root.fake->repaints == repaints,
          "unchanged frames reuse the same menu image without repainting");
    look::apply(oldTheme, density);
    pane.frame({}, 0, 0);
    check(swatch(palettes::defaultIndex) == initialDefault, "returning to a theme reuses its original default thumbnail");
}
// Independent pixel oracle for the original ADI rendering law (01992652).
// Fixed view bounds below map one displayed hop/row to one software pixel.
bool originalRasterPixels(SharedSpectrumView &view, const SharedAnalysis::Result &result) {
    const auto image = softwarePicture(view);
    const auto top = std::min(20000., result.sampleRate * .5);
    for (int x = 0; x < 128; ++x) for (int y = 0; y < 256; ++y) {
        auto expected = adi::ui::look::display;
        const auto distance = static_cast<std::uint64_t>(127 - x);
        if (distance <= result.historySerial) {
            const auto serial = result.historySerial - distance;
            for (const auto &column : result.columns) if (column.serial == serial && column.binsDbfs) {
                const int row = 255 - y;
                const auto low = 20. * std::pow(top / 20., static_cast<double>(row) / 256);
                const auto high = 20. * std::pow(top / 20., static_cast<double>(row + 1) / 256);
                const auto first = std::max<std::size_t>(1, static_cast<std::size_t>(low * result.request.fftSize / result.sampleRate));
                const auto last = std::max(first + 1, static_cast<std::size_t>(high * result.request.fftSize / result.sampleRate) + 1);
                float peak = -96;
                for (auto bin = first; bin < std::min(column.binsDbfs->size(), last); ++bin)
                    peak = std::max(peak, (*column.binsDbfs)[bin]);
                const auto index = static_cast<int>(std::clamp((peak + 96.f) / 96.f, 0.f, 1.f) * 255.f);
                const float t = static_cast<float>(index) / 255.f;
                expected = adi::ui::look::display.interpolatedWith(adi::ui::look::value, t)
                    .interpolatedWith(adi::ui::look::accent, std::max(0.f, (t - .7f) / .3f));
            }
        }
        if (image.getPixelAt(x + 5, y + 21) != expected) return false;
    }
    return true;
}
void palettesAndFrozenRaster() {
    using namespace adi::ui;
    check(palettes::count == 47 && palettes::id(0) == "adi" && palettes::find("missing") == 0,
          "palette inventory includes original ADI plus all46 public families with safe fallback");
    bool lookup = true;
    for (std::size_t p = 1; p < palettes::count; ++p) {
        lookup = lookup && palettes::find(palettes::id(p)) == p;
        for (std::size_t q = p + 1; q < palettes::count; ++q) lookup = lookup && palettes::id(p) != palettes::id(q);
        for (int i = 0; i < 256; ++i)
            lookup = lookup && analysisPaletteColour(p, static_cast<std::uint8_t>(i), true) ==
                analysisPaletteColour(p, static_cast<std::uint8_t>(255 - i));
    }
    check(lookup, "every family has stable unique identity and exact reverse table indexing");
    check(palettes::rgb(palettes::find("viridis"), 0) == 0x440154 &&
              palettes::rgb(palettes::find("viridis"), 255) == 0xfde725 &&
              palettes::rgb(palettes::find("gray"), 0) == 0 && palettes::rgb(palettes::find("gray"), 255) == 0xffffff,
          "public viridis and gray endpoints retain their known RGB8 values");
    bool original = true;
    for (int i = -100; i <= 1100; ++i) {
        const float db = -96.f + static_cast<float>(i) * 96.f / 1000.f;
        const auto index = static_cast<std::uint8_t>(std::clamp((db + 96.f) / 96.f, 0.f, 1.f) * 255.f);
        const float t = static_cast<float>(index) / 255.f;
        const auto expected = look::display.interpolatedWith(look::value, t)
            .interpolatedWith(look::accent, std::max(0.f, (t - .7f) / .3f));
        original = original && analysisPaletteColour(0, index) == expected;
    }
    check(original, "default colors preserve the original quantized law for1201 arbitrary dB values and clipping");
    Fixture f;
    Root root; root.setBounds(0, 0, 500, 420); root.setVisible(true); root.addToDesktop(0);
    SharedSpectrumView view; view.setMode(SharedSpectrumView::Mode::Spectrogram);
    root.addAndMakeVisible(view); view.setBounds(0, 0, 156, 311); view.present(f.broker, 1); view.frame();
    bool pixels = true;
    for (int block = 0; block < 4; ++block) {
        f.write(block * 1024); f.broker->pump(); view.frame();
        pixels = pixels && view.snapshot() && originalRasterPixels(view, *view.snapshot());
    }
    check(pixels, "actual default raster pixels match old frequency aggregation/color/gap law across advancing columns");
    const auto snapshot = view.snapshot(); view.freeze(true);
    const auto before = f.broker->stats(); const auto picture = softwarePicture(view);
    view.setPalette("viridis"); const auto colored = softwarePicture(view);
    check(!samePixels(picture, colored) && view.snapshot() == snapshot && view.frozen(),
          "palette selection recolors the existing frozen raster without replacing its measurement");
    view.setPalette("gray", true); const auto gray = softwarePicture(view);
    check(!samePixels(colored, gray) && gray.getPixelAt(5, 21) == look::display,
          "reverse and grayscale recolor valid pixels while missing history remains background");
    view.setPalette("adi");
    check(samePixels(picture, softwarePicture(view)), "returning to original palette restores every frozen displayed pixel");
    const auto oldTheme = look::themeId();
    const auto density = look::densityId();
    look::apply(oldTheme == ThemeId::Light ? ThemeId::Dark : ThemeId::Light, density);
    view.frame();
    check(snapshot && analysisPaletteColour(0, 0) == look::display && !samePixels(picture, softwarePicture(view)) &&
              originalRasterPixels(view, *snapshot) && view.snapshot() == snapshot,
          "a theme switch recolors gaps and signal in a frozen raster without changing its publication");
    look::apply(oldTheme, density);
    view.frame();
    check(samePixels(picture, softwarePicture(view)), "restoring the theme restores every frozen pixel");
    for (int i = 0; i < 4; ++i) { view.frame(); (void)softwarePicture(view); }
    const auto after = f.broker->stats();
    check(after.transforms == before.transforms && after.analysisWindows == before.analysisWindows &&
              after.cursorReads == before.cursorReads && after.workerPasses == before.workerPasses &&
              after.domains == before.domains && after.taps == before.taps && after.frameRequests == before.frameRequests &&
              after.historyBytes == before.historyBytes && after.consumers == 0,
          "palette changes/unchanged frozen paints create no FFT, reads, subscriptions or history work");
    view.freeze(false); f.tap = std::make_shared<ScopeTap>(44100, SharedAnalysis::kTapSeconds);
    f.broker->refreshSources(true); f.write(); f.broker->pump(); view.frame();
    f.write(1024); f.broker->pump(); view.frame();
    check(view.snapshot() && view.snapshot()->sampleRate == 44100 && originalRasterPixels(view, *view.snapshot()),
          "format/source epoch replacement removes old pixels and retains default calibration");
    AnalysisPane pane;
    pane.setMode(SharedSpectrumView::Mode::Spectrogram); pane.setPalette("rocket", true);
    check(pane.paletteBox.getNumItems() == 47 && pane.requestedPalette() == "rocket" && pane.reverseButton.getToggleState(),
          "palette menu shows every family and selected/reversed state");
    for (const int width : {240, 332, 500}) {
        pane.setSize(width, 420);
        const std::array<juce::Component *, 10> controls{&pane.modeBox, &pane.sourceBox, &pane.pointBox, &pane.channelBox,
            &pane.paletteBox, &pane.slopeBox, &pane.reverseButton, &pane.freezeButton, &pane.resetButton, &pane.expandButton};
        bool fits = true;
        for (std::size_t i = 0; i < controls.size(); ++i) {
            fits = fits && pane.getLocalBounds().contains(controls[i]->getBounds()) &&
                controls[i]->getBottom() <= pane.view.getY();
            for (std::size_t j = i + 1; j < controls.size(); ++j)
                fits = fits && !controls[i]->getBounds().intersects(controls[j]->getBounds());
        }
        check(fits && pane.view.getHeight() > 250, "analysis controls wrap responsively with useful plot space");
    }
    view.setVisible(false);
    check(f.broker->stats().consumers == 0, "palette-enabled spectrogram still releases hidden demand");
}
void restartedRasterDomain() {
    for (const bool useFreeze : {false, true}) {
        Fixture f;
        Root root; root.setBounds(0, 0, 400, 340); root.setVisible(true); root.addToDesktop(0);
        SharedSpectrumView view; view.setMode(SharedSpectrumView::Mode::Spectrogram);
        root.addAndMakeVisible(view); view.setBounds(0, 0, 156, 311); view.present(f.broker, 1); view.frame();
        f.write(); f.broker->pump(); view.frame();
        const auto first = view.snapshot(); const auto firstImage = softwarePicture(view);
        if (useFreeze) { view.freeze(true); view.freeze(false); }
        else { view.setVisible(false); view.setVisible(true); }
        view.frame(); // renew the sole cursor before supplying fresh audio
        std::array<float, 1024> silence{};
        f.tap->write(silence.data(), silence.data(), static_cast<std::int32_t>(silence.size()), 1024, true);
        f.broker->pump(); view.frame();
        const auto second = view.snapshot();
        check(first && second && first != second && first->sourceEpoch == second->sourceEpoch &&
                  first->revision == second->revision && first->historySerial == second->historySerial &&
                  !first->columns.empty() && !second->columns.empty() &&
                  *first->columns.back().binsDbfs != *second->columns.back().binsDbfs,
              "restarted sole spectrogram domain reuses counters on the same tap but carries fresh different audio");
        check(second && !samePixels(firstImage, softwarePicture(view)) && originalRasterPixels(view, *second),
              "hide or Freeze/Resume cannot reuse old raster pixels when new-domain counters repeat");
    }
}

void writeFrames(Fixture &fixture, int frames, bool parked = false) {
    std::vector<float> samples(static_cast<std::size_t>(frames));
    const auto start = fixture.tap->written();
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = .5f * static_cast<float>(std::sin(2 * std::numbers::pi * 16 *
            static_cast<double>(start + static_cast<std::int64_t>(i)) / 1024));
    fixture.tap->write(samples.data(), samples.data(), frames, parked ? 0 : start, !parked);
}

void freshActionsWithSharedConsumer() {
    for (const auto mode : {SharedSpectrumView::Mode::Spectrum, SharedSpectrumView::Mode::Scope,
                           SharedSpectrumView::Mode::Meters, SharedSpectrumView::Mode::Phase,
                           SharedSpectrumView::Mode::Spectrogram}) {
        for (int action = 0; action < 4; ++action) {
            Fixture f;
            Root root; root.setBounds(0, 0, 400, 340); root.setVisible(true); root.addToDesktop(0);
            auto keeper = f.broker->acquire({1});
            writeFrames(f, 1024, true); f.broker->pump();
            const auto initial = keeper.read(); const auto beforeInitial = f.broker->stats();
            SharedSpectrumView view; view.setMode(mode); view.setBounds(0, 0, 300, 300);
            root.addAndMakeVisible(view); view.present(f.broker, 1);
            f.broker->pump(); view.frame();
            check(initial && view.snapshot() && view.snapshot()->firstSequence == initial->firstSequence &&
                      f.broker->stats().analysisWindows == beforeInitial.analysisWindows &&
                      f.broker->stats().transforms == beforeInitial.transforms,
                  "a new view may reuse a correct compatible initial stopped interval without new audio or FFT");
            const auto picture = view.snapshot();
            if (action == 0) view.freeze(true);
            else if (action == 1) view.setVisible(false);
            else if (action == 3) view.suspend(true);
            writeFrames(f, 255, true); // Entire backlog exists BEFORE the explicit fresh action.
            const auto watermark = f.tap->written();
            if (action == 0) view.freeze(false);
            else if (action == 1) view.setVisible(true);
            else if (action == 2) view.clear();
            else view.suspend(false);
            f.broker->pump(); view.frame();
            check(!view.snapshot() && view.status() == SharedSpectrumView::Status::Waiting && keeper.read(),
                  "Resume, reshow, Clear and unsuspend reject cached or output-republished pre-action intervals");
            const auto idle = f.broker->stats();
            for (int i = 0; i < 3; ++i) { f.broker->pump(); view.frame(); }
            check(!view.snapshot() && f.broker->stats().analysisWindows == idle.analysisWindows &&
                      f.broker->stats().transforms == idle.transforms,
                  "fresh-data waiting is idle and does not promote a cached result on another owner frame");
            writeFrames(f, 1, true); f.broker->pump(); view.frame();
            const auto fresh = view.snapshot();
            check(fresh && fresh != picture && fresh->firstSequence + fresh->request.fftSize > watermark &&
                      fresh->firstStamp == 0 && fresh == keeper.read(),
                  "one newly written sample admits a completed hop even with a parked transport stamp");
            const auto resources = f.broker->stats();
            check(resources.consumers == 2 && resources.domains == 1 && resources.taps == 1 &&
                      resources.spectralPlans == 1,
                  "freshness actions retain one compatible domain, tap, cursor and spectral plan");
        }
    }
}

void backlogAndLifecycleFreshness() {
    Fixture f;
    Root root; root.setBounds(0, 0, 500, 340); root.setVisible(true); root.addToDesktop(0);
    auto keeper = f.broker->acquire({1});
    SharedSpectrumView view; view.setBounds(0, 0, 300, 300); root.addAndMakeVisible(view); view.present(f.broker, 1);
    writeFrames(f, 1024); f.broker->pump(); view.frame();
    check(view.snapshot() != nullptr, "backlog fixture begins with a real displayed interval");
    view.freeze(true); writeFrames(f, 4096); view.freeze(false);
    const auto backlogEnd = f.tap->written();
    for (int i = 0; i < 4; ++i) { f.broker->pump(); view.frame(); }
    check(!view.snapshot() && keeper.read() &&
              keeper.read()->firstSequence + keeper.read()->request.fftSize == backlogEnd,
          "processing a complete pre-Resume backlog cannot satisfy the sample-position barrier");
    writeFrames(f, 256); f.broker->pump(); view.frame();
    check(view.snapshot() && view.snapshot()->firstSequence + view.snapshot()->request.fftSize > backlogEnd,
          "the first completed post-Resume hop replaces the waiting picture");

    const auto reshow = [&](auto hide, auto show) {
        hide(); writeFrames(f, 256); f.broker->pump(); show(); view.frame();
        check(!view.snapshot() && keeper.read(), "reshow rejects another consumer's newer but pre-show cached interval");
        writeFrames(f, 256); f.broker->pump(); view.frame();
        check(view.snapshot() && view.snapshot() == keeper.read(), "fresh post-show audio restores the same shared publication");
    };
    reshow([&] { root.setVisible(false); }, [&] { root.setVisible(true); });
    reshow([&] { view.setTopLeftPosition(600, 0); }, [&] { view.setTopLeftPosition(0, 0); });
    reshow([&] { root.removeChildComponent(&view); }, [&] { root.addAndMakeVisible(view); });

    const auto previous = view.snapshot();
    f.tap = std::make_shared<ScopeTap>(48000, SharedAnalysis::kTapSeconds);
    writeFrames(f, 1024); f.broker->refreshSources(); view.frame();
    check(!view.snapshot() && view.status() == SharedSpectrumView::Status::Waiting,
          "tap replacement clears the old generation before its new admission");
    writeFrames(f, 1024); f.broker->pump(); view.frame();
    if (!view.snapshot()) { writeFrames(f, 256); f.broker->pump(); view.frame(); }
    check(previous && view.snapshot() && previous->generation != view.snapshot()->generation,
          "a replacement generation admits only after fresh replacement-source audio");
    auto parent = std::make_unique<juce::Component>(); parent->setBounds(0, 0, 400, 320);
    root.addAndMakeVisible(*parent); parent->addAndMakeVisible(view);
    writeFrames(f, 256); f.broker->pump(); view.frame();
    check(view.snapshot() != nullptr, "ancestor-retirement fixture begins from a current displayed picture");
    parent.reset(); writeFrames(f, 256); f.broker->pump(); root.addAndMakeVisible(view); view.frame();
    check(!view.snapshot() && keeper.read(),
          "reattachment after ancestor destruction rejects retained pre-reattachment cached data");
    writeFrames(f, 256); f.broker->pump(); view.frame();
    check(view.snapshot() && view.snapshot() == keeper.read(),
          "post-reattachment source audio restores the shared picture without a new domain");
    view.freeze(true); const auto held = view.snapshot(); f.broker->shutdown(); view.frame();
    check(held && !view.snapshot() && !view.subscribed() && view.status() == SharedSpectrumView::Status::Unavailable &&
              f.broker->stats().consumers == 0,
          "broker shutdown revokes even a lease-free Frozen publication without creating demand");
}

void spectrumSlopeProjection() {
    using namespace adi::ui;
    constexpr std::array<std::array<double, 2>, 5> points{{{250., -9.}, {500., -4.5}, {1000., 0.}, {2000., 4.5}, {4000., 9.}}};
    for (const auto &point : points)
        check(spectrumDisplayOffset(SpectrumSlope::Tilt45, point[0]) == point[1] &&
                  spectrumDisplayOffset(SpectrumSlope::Flat, point[0]) == 0,
              "display slope has the explicit octave/pivot law while Flat remains exact zero");
    check(spectrumDisplayOffset(SpectrumSlope::Tilt45, 1000.) == 0 &&
              spectrumDisplayOffset(SpectrumSlope::Tilt45, 0.) == 0 &&
              spectrumDisplayOffset(SpectrumSlope::Tilt45, -1.) == 0 &&
              spectrumDisplayOffset(SpectrumSlope::Tilt45, std::numeric_limits<double>::infinity()) == 0 &&
              spectrumDisplayOffset(SpectrumSlope::Tilt45, std::numeric_limits<double>::quiet_NaN()) == 0,
          "pivot is exact and DC/nonpositive/nonfinite frequencies never enter a logarithm");
    SpectrumSlopeCache cache;
    check(!cache.prepare(SpectrumSlope::Flat, 48000, 1024, 513) && cache.offsets().empty(),
          "the default Flat projection allocates no offset storage");
    check(cache.prepare(SpectrumSlope::Tilt45, 64000, 1024, 513) && cache.offsets().size() == 513 &&
              cache.offsets()[0] == 0 && cache.offsets()[16] == 0,
          "prepared slope uses actual Fs/N bin frequencies including unmodified DC and exact pivot");
    const auto *storage = cache.offsets().data();
    check(!cache.prepare(SpectrumSlope::Tilt45, 64000, 1024, 513) && cache.offsets().data() == storage,
          "unchanged projection setup returns before allocation or logarithm recomputation");
    const auto old = cache.offsets()[32];
    check(cache.prepare(SpectrumSlope::Tilt45, 128000, 1024, 513) && cache.offsets()[32] == old + 4.5f,
          "rate replacement updates the display-frequency offsets");
    check(cache.prepare(SpectrumSlope::Tilt45, 128000, 2048, 1025) && cache.offsets()[32] == old,
          "FFT-size replacement restores the corresponding calibrated bin frequencies");
    check(cache.prepare(SpectrumSlope::Tilt45, 48000, 65536, 32769) && cache.offsets().size() == 32769 &&
              cache.prepare(SpectrumSlope::Tilt45, 48000, 65568, 32785) && cache.offsets().empty(),
          "projection cache obeys the existing maximum FFT bound and rejects oversized metadata");
    check(spectrumSlopeFromId("unknown") == SpectrumSlope::Flat && !validSpectrumSlopeId("unknown") &&
              spectrumSlopeId(SpectrumSlope::Tilt45) == "tilt45",
          "stable slope IDs validate persistence and fall back honestly for unknown input");

    Fixture f;
    Root root; root.setBounds(0, 0, 660, 340); root.setVisible(true); root.addToDesktop(0);
    SharedSpectrumView full, info;
    full.setBounds(0, 0, 300, 300); info.setBounds(330, 0, 300, 300);
    root.addAndMakeVisible(full); root.addAndMakeVisible(info); full.present(f.broker, 1); info.present(f.broker, 1);
    writeFrames(f, 1024); f.broker->pump(); full.frame(); info.frame();
    const auto original = softwarePicture(full), compact = softwarePicture(info);
    const auto snapshot = full.snapshot();
    check(snapshot && snapshot == info.snapshot() && samePixels(original, compact),
          "slope fixture begins with exact shared calibrated data and identical Flat pictures");
    const auto raw = snapshot ? snapshot->binsDbfs : std::vector<float>{};
    full.freeze(true); const auto frozenFlat = softwarePicture(full); const auto before = f.broker->stats();
    full.setSpectrumSlope(SpectrumSlope::Tilt45); const auto tilted = softwarePicture(full);
    check(snapshot && !samePixels(frozenFlat, tilted) && full.snapshot() == snapshot && full.frozen() &&
              snapshot->binsDbfs == raw && samePixels(compact, softwarePicture(info)),
          "Frozen Spectrum reprojects locally without mutating its retained bins or another consumer");
    for (int i = 0; i < 4; ++i) { full.frame(); (void)softwarePicture(full); }
    const auto after = f.broker->stats();
    check(after.transforms == before.transforms && after.analysisWindows == before.analysisWindows &&
              after.publications == before.publications && after.cursorReads == before.cursorReads &&
              after.consumers == before.consumers && after.domains == before.domains && after.taps == before.taps,
          "slope changes and unchanged Frozen frames create no measurement, transform or demand work");
    full.setSpectrumSlope(SpectrumSlope::Flat);
    check(samePixels(frozenFlat, softwarePicture(full)) && full.snapshot() == snapshot && snapshot->binsDbfs == raw,
          "returning Flat restores the exact original pixels and raw measurement");
    full.setSpectrumSlope(SpectrumSlope::Tilt45); full.freeze(false); writeFrames(f, 256); f.broker->pump(); full.frame(); info.frame();
    check(full.snapshot() && full.snapshot() == info.snapshot() && f.broker->stats().domains == 1 &&
              f.broker->stats().taps == 1 && f.broker->stats().spectralPlans == 1,
          "differing live display slopes still share one compatible result, source and FFT");
    full.setMode(SharedSpectrumView::Mode::Meters); f.broker->pump(); full.frame();
    check(full.snapshot() && full.snapshot()->outputs & SharedAnalysis::Meters,
          "meter-isolation oracle uses a real shared meter publication");
    const auto meterFlat = softwarePicture(full); full.setSpectrumSlope(SpectrumSlope::Flat);
    check(samePixels(meterFlat, softwarePicture(full)), "Spectrum slope never modifies raw sample peak/RMS presentation");
    check(SharedSpectrumViewTestAccess::offsetCount(full) == 0,
          "choosing Flat releases prior offsets immediately even outside Spectrum mode");
    full.setMode(SharedSpectrumView::Mode::Spectrum); f.broker->pump(); full.frame(); full.setSpectrumSlope(SpectrumSlope::Tilt45);
    check(SharedSpectrumViewTestAccess::offsetCount(full) == 513, "visible tilted Spectrum owns a prepared bounded projection");
    full.setVisible(false); full.setSpectrumSlope(SpectrumSlope::Flat);
    check(SharedSpectrumViewTestAccess::offsetCount(full) == 0,
          "choosing Flat while hidden releases the prepared projection without a future frame");

    AnalysisPane pane; pane.setSpectrumSlope(SpectrumSlope::Tilt45);
    for (const auto mode : {SharedSpectrumView::Mode::Spectrum, SharedSpectrumView::Mode::Scope,
                           SharedSpectrumView::Mode::Meters, SharedSpectrumView::Mode::Phase, SharedSpectrumView::Mode::Spectrogram}) {
        pane.setMode(mode);
        check(pane.slopeBox.isEnabled() == (mode == SharedSpectrumView::Mode::Spectrum) &&
                  pane.view.spectrumSlope() == SpectrumSlope::Tilt45 && pane.requestedSpectrumSlope() == SpectrumSlope::Tilt45,
              "slope control is Spectrum-only while retaining its local preference across other modes");
    }
}

void classicScopeSurface() {
    using namespace adi::ui;
    using Trigger = adi::dsp::TriggeredScope::Trigger;
    const ClassicScopeControls factory;
    const auto defaults = factory.settings();
    check(defaults.columns == 627 && defaults.trigger == Trigger::Free && defaults.threshold == 0 &&
              defaults.gain == 1 && defaults.holdoffSamples == 100 &&
              std::abs(defaults.columnsPerSample - std::pow(10., -2.25)) < 1.e-12 &&
              !factory.right && !factory.sync && !defaults.removeDc,
          "Classic scope uses the pinned factory face and capture settings");

    ClassicScopePanel face;
    face.setBounds(0, 0, 825, 300);
    face.setVisible(true);
    auto picture = std::make_shared<SharedAnalysis::CaptureResult>();
    picture->sampleRate = 48000;
    picture->previous = {{.25f, .25f, 1, 1}, {.25f, .25f, 2, 2}};
    picture->current = {{.5f, .5f, 7, 7}};
    const auto raster = [&] {
        juce::Image image(juce::Image::RGB, 825, 300, true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            face.paintEntireComponent(g, true);
        }
        return image;
    };
    const auto pixel = [](const juce::Image &image, int x, int y) {
        return image.getPixelAt(38 + x, 16 + y);
    };
    auto controls = factory;
    const auto ink = juce::Colour(118, 118, 118);
    face.present(controls, false, picture, 0);
    const auto progressive = raster();
    check(pixel(progressive, 0, 67) == ink && pixel(progressive, 1, 100) == ink,
          "progressive redraw keeps the preceding sweep tail");
    controls.sync = true;
    face.present(controls, false, picture, 0);
    const auto sync = raster();
    check(pixel(sync, 0, 100) == ink && pixel(sync, 0, 67) != ink, "Sync redraw uses only the previous sweep");
    controls.sync = false;
    face.present(controls, false, picture, 7);
    const auto fresh = raster();
    check(pixel(fresh, 0, 67) == ink && pixel(fresh, 1, 100) != ink,
          "the local freshness fence omits pre-resume columns");

    bool heldFace = false;
    face.onChange = [&](const ClassicScopeControls &next) { controls = next; face.present(controls, heldFace, picture, 7); };
    face.onFreeze = [&](bool held) { heldFace = held; face.present(controls, heldFace, picture, 7); };
    const auto mouse = [](juce::Component &c, juce::Point<float> p, int mods) {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent{juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(mods),
                               1.f, 0.f, 0.f, 0.f, 0.f, &c, &c, now, p, now, 1, false};
    };
    const auto press = [&](int x, int y, const char *title) {
        auto *control = face.getComponentAt(x, y);
        if (!control || control->getTitle() != title) return false;
        const auto event = mouse(*control, control->getLocalBounds().toFloat().getCentre(),
                                 juce::ModifierKeys::leftButtonModifier);
        control->mouseDown(event);
        control->mouseUp(event);
        return true;
    };
    for (auto trigger : {Trigger::Rising, Trigger::Falling, Trigger::Internal, Trigger::Free})
        check(press(730, 100, "Trigger mode") && controls.trigger == trigger,
              "the original trigger control cycles through all four modes");
    check(press(760, 262, "Left or right channel") && controls.right, "the original channel control selects Right");
    check(press(705, 262, "DC removal") && controls.removeDc, "the original DC control edits acquisition");
    check(press(705, 225, "Sync redraw") && controls.sync, "the original Sync control selects the completed sweep");
    check(press(767, 225, "Freeze") && heldFace && press(767, 225, "Freeze") && !heldFace,
          "the original Freeze control toggles local hold");
    auto *timeKnob = dynamic_cast<juce::Slider *>(face.getComponentAt(713, 48));
    if (timeKnob) timeKnob->setValue(.6, juce::sendNotificationSync);
    check(timeKnob && std::abs(controls.time - .6f) < 1.e-6f, "the original Time knob changes the capture profile");
    auto *wave = face.getComponentAt(158, 206);
    const auto before = pixel(raster(), 120, 190);
    if (wave) {
        const auto left = mouse(*wave, {120.f, 102.f}, juce::ModifierKeys::leftButtonModifier);
        wave->mouseDown(left);
        wave->mouseUp(left);
    }
    const auto ruler = pixel(raster(), 120, 190);
    check(wave && wave->getTitle() == "Waveform ruler" && ruler == juce::Colour(10, 10, 10) && ruler != before,
          "a left press places the sticky ruler independently of the waveform");
    if (wave) wave->mouseUp(mouse(*wave, {120.f, 102.f}, juce::ModifierKeys::rightButtonModifier));
    check(pixel(raster(), 120, 190) == before, "a right press clears the ruler");

    Fixture f;
    Root root;
    root.setBounds(0, 0, 970, 720);
    Root detached;
    detached.setBounds(0, 0, 920, 420);
    MixAnalysers pages;
    pages.setBounds(root.getLocalBounds());
    AnalysisPane pane;
    pane.setBounds(detached.getLocalBounds());
    root.addAndMakeVisible(pages);
    detached.addAndMakeVisible(pane);
    for (auto *peer : {&root, &detached}) {
        peer->setVisible(true);
        peer->addToDesktop(0);
    }
    const SnapshotReader reader{std::make_shared<adi::engine::Snapshot>()};
    pages.setPage(MixAnalysers::Page::Scope);
    pane.modeBox.setSelectedId(static_cast<int>(SharedSpectrumView::Mode::ClassicScope) + 1, juce::sendNotificationSync);
    pages.frame(f.broker, reader, 1, 2);
    pane.frame(f.broker, 1, 2);
    check(pages.detail.mode() == SharedSpectrumView::Mode::ClassicScope && !pages.detail.channelBox.isVisible() &&
              pane.modeBox.getText() == "s(M)exoscope" && !pane.channelBox.isVisible() && !pane.freezeButton.isVisible(),
          "the full and detached Scope views use the original controls");
    f.write(); f.broker->pump(); pages.detail.view.frame(); pane.view.frame();
    const auto capture = [](const SharedSpectrumView &v) { return SharedSpectrumViewTestAccess::capture(v); };
    const auto first = capture(pane.view);
    check(first && first == capture(pages.detail.view) && !first->current.empty() &&
              f.broker->stats().transforms == 0 && f.broker->stats().domains == 1 && f.acquired == 1,
          "compatible full and detached scopes share one immutable capture without an FFT");
    controls = factory;
    controls.sync = true;
    pane.view.setClassicControls(controls);
    check(capture(pane.view) == first && f.acquired == 1, "Sync redraw does not restart the shared cursor");

    pane.view.freeze(true);
    f.write(1024); f.broker->pump(); pages.detail.view.frame(); pane.view.frame();
    check(first && capture(pane.view) == first && capture(pages.detail.view) != first && !pane.view.subscribed(),
          "Freeze retains the local trace and releases only that view's live demand");
    pane.view.freeze(false);
    pane.view.frame();
    check(!capture(pane.view), "Resume rejects pre-resume audio even when another view still owns the capture");
    f.write(2048); f.broker->pump(); pane.view.frame();
    const auto resumed = capture(pane.view);
    check(resumed && !resumed->current.empty() && resumed->current.back().firstSequence >= 2048,
          "Resume accepts data after the physical tap fence");

    pane.view.clear();
    pane.view.frame();
    check(!capture(pane.view) && capture(pages.detail.view), "Reset clears this scope and leaves its shared peer alone");
    controls.right = true;
    pane.view.setClassicControls(controls);
    check(f.broker->stats().domains == 2 && f.acquired == 1, "a different lane shares the tap but owns its own sweep");
    detached.setVisible(false);
    check(!pane.view.subscribed() && f.broker->stats().consumers == 1 && f.released == 0,
          "hiding the detached parent drops demand without waiting for another frame");
    detached.setVisible(true);
    pane.suspend(true);
    pane.frame(f.broker, 1, 2);
    check(!pane.view.subscribed() && f.broker->stats().consumers == 1, "minimising the detached view prevents reacquisition");
    pane.suspend(false);
    check(pane.view.subscribed() && f.broker->stats().consumers == 2, "restoring the detached view reacquires demand");
    detached.removeChildComponent(&pane);
    auto retuned = factory;
    retuned.time = .65f;
    pages.detail.view.setClassicControls(retuned);
    check(f.broker->stats().consumers == 1 && f.broker->stats().domains == 1 && f.acquired == 1 && f.released == 0,
          "retuning the final view replaces its sweep without tearing down the physical tap");
    root.setVisible(false);
    check(!pane.view.subscribed() && !pages.detail.view.subscribed() &&
              f.broker->stats().consumers == 0 && f.released == 1,
          "detaching and hiding the final views release the shared tap");
}

void mixAnalyserPages() {
    using namespace adi::ui;
    using Page = MixAnalysers::Page;
    using Track = adi::engine::TrackNode;
    constexpr std::int64_t largeTrackId = 1LL << 42;
    const auto channel = [](std::int64_t id, std::string name, std::string kind = "audio") {
        auto node = std::make_shared<Track>();
        node->id = id;
        node->name = std::move(name);
        node->kind = std::move(kind);
        return node;
    };
    const auto main = channel(1, "Main", "master");
    const auto first = channel(2, "Bass");
    const auto fixed = channel(largeTrackId, "Guitar");
    // ProjectView leaves Snapshot::seq at zero even as authored channels change.
    const auto project = [&](std::shared_ptr<Track> other) {
        auto state = std::make_shared<adi::engine::Snapshot>();
        state->tracks = {main, first};
        if (other) state->tracks.push_back(std::move(other));
        return state;
    };

    const SnapshotReader reader{project(fixed)};
    Fixture f, replacement;
    Root root;
    root.setBounds(0, 0, 1040, 620);
    MixAnalysers pages;
    pages.setBounds(root.getLocalBounds());
    root.addAndMakeVisible(pages);
    pages.frame(f.broker, reader, main->id, first->id);
    check(f.acquired == 0 && f.broker->stats().consumers == 0,
          "analysers outside a visible window acquire no taps");
    root.setVisible(true);
    root.addToDesktop(0);

    auto meter = std::make_shared<MainMeterSource>();
    meter->track = main->id;
    int loudnessReads = 0;
    meter->read = [&] {
        ++loudnessReads;
        return MainMeterSnapshot{main->id, 48000., 1, -17.f, -19.f, -20.f, -2.f};
    };
    pages.setMainMeterSource(meter);
    pages.frame(f.broker, reader, main->id, first->id);
    const auto all = f.broker->stats();
    check(pages.page() == Page::All && pages.pageBox.getNumItems() == static_cast<int>(Page::Compare) + 1 &&
              pages.sourceBox.getNumItems() == 4 && pages.sourceBox.getSelectedId() == 1,
          "analysers default to All and Main and offer each non-Main channel once");
    check(all.taps == 3 && all.domains == 3 && all.consumers == 8 && all.spectralPlans == 1,
          "All shares one Main FFT across spectral views and leases readout rows without their own FFT");
    f.write();
    check(f.broker->pump() == 1, "All uses one transform for the compatible Main spectrum and spectrogram");
    pages.frame(f.broker, reader, main->id, first->id);
    check(loudnessReads > 0, "All reads Main loudness through the persistent measurement");

    for (const auto &[page, mode] : std::array{
         std::pair{Page::Spectrogram, SharedSpectrumView::Mode::Spectrogram},
         std::pair{Page::Spectrum, SharedSpectrumView::Mode::Spectrum},
         std::pair{Page::Scope, SharedSpectrumView::Mode::ClassicScope},
         std::pair{Page::Phase, SharedSpectrumView::Mode::Phase},
         std::pair{Page::Loudness, SharedSpectrumView::Mode::Meters}}) {
        pages.pageBox.setSelectedId(static_cast<int>(page) + 1, juce::sendNotificationSync);
        pages.frame(f.broker, reader, main->id, first->id);
        const auto individual = f.broker->stats();
        check(pages.page() == page && pages.detail.mode() == mode && pages.detail.isShowing() &&
                  !pages.detail.modeBox.isVisible() && !pages.detail.sourceBox.isVisible() &&
                  pages.detail.view.subscribed() && individual.consumers == 1 && individual.taps == 1,
              "each individual analyser page keeps only its actual display subscribed");
    }
    f.write(1024); f.broker->pump();
    pages.frame(f.broker, reader, main->id, first->id);
    check(pages.detail.view.mainMeterSnapshot().has_value(), "the Main Loudness page displays native Main LUFS");

    pages.sourceBox.setSelectedId(2, juce::sendNotificationSync);
    const auto priorLoudnessReads = loudnessReads;
    pages.frame(f.broker, reader, main->id, first->id);
    check(pages.detail.view.track() == first->id, "Follow selection first follows Bass");
    pages.frame(f.broker, reader, main->id, fixed->id);
    check(pages.detail.view.track() == fixed->id && f.broker->stats().taps == 1,
          "Follow selection retargets without retaining the old tap");
    f.write(2048); f.broker->pump();
    pages.frame(f.broker, reader, main->id, fixed->id);
    check(!pages.detail.view.mainMeterSnapshot() && loudnessReads == priorLoudnessReads,
          "a channel Loudness page never reads or attributes Main loudness");

    const int fixedMenuId = pages.sourceBox.getItemId(3);
    pages.sourceBox.setSelectedId(fixedMenuId, juce::sendNotificationSync);
    const SnapshotReader renamed{project(channel(largeTrackId, "Lead guitar"))};
    pages.frame(f.broker, renamed, main->id, first->id);
    check(pages.detail.view.track() == largeTrackId && pages.sourceBox.getSelectedId() == fixedMenuId &&
              pages.sourceBox.getText() == "Lead guitar",
          "fixed channel uses a stable 64-bit track identity through selection changes and renames");
    const SnapshotReader removed{project({})};
    check(removed.seq() == renamed.seq() && pages.target(removed, main->id, first->id) == main->id,
          "an unstamped changed snapshot cannot resolve a stale fixed source before a frame");
    pages.frame(f.broker, removed, main->id, first->id, 24);
    check(pages.detail.view.track() == main->id && pages.sourceBox.getSelectedId() == 1 &&
              pages.sourceBox.getNumItems() == 3 && f.broker->stats().taps == 1,
          "removing a fixed channel in an unstamped snapshot restores Main and releases the removed tap");

    pages.setPage(Page::Analysis);
    pages.frame(f.broker, reader, main->id, first->id);
    const auto table = f.broker->stats();
    check(!pages.sourceBox.isVisible() && !pages.detail.isVisible() && table.consumers == 3 &&
              table.taps == 3 && table.spectralPlans == 0,
          "Analysis shows per-channel readings without a spectrogram or spectrum plan");
    {
        SharedAnalysis::Request meterRequest{main->id};
        meterRequest.outputs = SharedAnalysis::Meters;
        auto sharedMeter = f.broker->acquire(meterRequest);
        f.write(3072);
        f.broker->pump();
        const auto result = sharedMeter.read();
        const auto shared = f.broker->stats();
        check(result && result->outputs == SharedAnalysis::Meters && result->scopeLeft.empty() &&
                  result->scopeRight.empty() && shared.consumers == 4 && shared.domains == 3 && shared.taps == 3,
              "the table shares the Main meter window without adding Phase or requesting raw samples");
    }
    pages.setVisible(false);
    check(f.broker->stats().consumers == 0 && f.broker->stats().taps == 0,
          "hiding Analysers releases the full measurement table without another frame");
    pages.setVisible(true);
    pages.frame(f.broker, reader, main->id, first->id);
    const auto beforeAncestorHide = f.broker->stats();
    root.setVisible(false);
    pages.frame(f.broker, reader, main->id, first->id);
    check(f.broker->stats().consumers == 0 && f.broker->stats().taps == 0 &&
              f.broker->stats().frameRequests == beforeAncestorHide.frameRequests,
          "hiding the workspace ancestor releases the table and a hidden frame requests no work");
    root.setVisible(true);
    pages.frame(f.broker, reader, main->id, first->id);
    pages.suspend(true);
    pages.frame(f.broker, reader, main->id, first->id);
    check(f.broker->stats().consumers == 0, "suspending Analysers stops demand and prevents reacquisition while minimized");
    pages.suspend(false);
    pages.frame(f.broker, reader, main->id, first->id);
    check(f.broker->stats().consumers == 3, "restore reacquires only the visible table");

    pages.setPage(Page::Scope);
    pages.sourceBox.setSelectedId(pages.sourceBox.getItemId(3), juce::sendNotificationSync);
    pages.frame(replacement.broker, reader, main->id, first->id);
    check(pages.sourceBox.getSelectedId() == 1 && pages.detail.view.track() == main->id &&
              f.broker->stats().taps == 0 && replacement.broker->stats().taps == 1,
          "replacing the project cannot retain a fixed source even if the new project has the same track IDs");
}

void compareAndAveragePages() {
    using namespace adi::ui;
    using Page = MixAnalysers::Page;
    using Comparison = AnalysisComparison::Status;
    constexpr std::int64_t main = 91, guitar = (std::int64_t{1} << 34) + 3;
    const auto project = [](std::uint64_t sequence, const char *name) {
        auto snapshot = std::make_shared<adi::engine::Snapshot>();
        snapshot->seq = sequence;
        auto master = std::make_shared<adi::engine::TrackNode>();
        master->id = main; master->kind = "master"; master->name = "Main";
        snapshot->tracks.push_back(master);
        if (name) {
            auto instrument = std::make_shared<adi::engine::TrackNode>();
            instrument->id = guitar; instrument->kind = "audio"; instrument->name = name;
            snapshot->tracks.push_back(instrument);
        }
        return snapshot;
    };
    const SnapshotReader original{project(1, "Guitar")};
    const SnapshotReader renamed{project(2, "Lead guitar")};
    const SnapshotReader removed{project(3, nullptr)};
    auto first = std::make_shared<ScopeTap>(48000., 1.);
    auto second = std::make_shared<ScopeTap>(48000., 1.);
    auto broker = std::make_shared<SharedAnalysis>(
        [&](std::int64_t track, ScopePoint) { return track == main ? first : second; },
        [](std::int64_t, ScopePoint) {}, SharedAnalysis::Mode::Manual);
    const auto write = [](ScopeTap &tap, float level, std::int64_t stamp, std::uint64_t revision) {
        std::array<float, 1024> samples;
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = level * static_cast<float>(std::sin(2. * std::numbers::pi * static_cast<double>(i) / 64.));
        tap.write(samples.data(), samples.data(), static_cast<std::int32_t>(samples.size()), stamp, true, revision);
    };

    Root root;
    MixAnalysers pages;
    root.setBounds(0, 0, 740, 660);
    root.setVisible(true); root.addToDesktop(0);
    root.addAndMakeVisible(pages);
    pages.setBounds(root.getLocalBounds());
    pages.setPage(Page::Compare);
    pages.frame(broker, original, main, guitar);
    check(root.getPeer() && !root.getPeer()->getNativeHandle() && broker->stats().consumers == 0 &&
              pages.compareA.isShowing() && pages.compareB.isShowing() && !pages.detail.isVisible(),
          "Compare uses a software peer and acquires only an explicitly chosen pair");
    pages.compareA.setSelectedId(pages.compareA.getItemId(0), juce::sendNotificationSync);
    pages.compareB.setSelectedId(pages.compareB.getItemId(1), juce::sendNotificationSync);
    pages.frame(broker, original, main, guitar);
    check(broker->stats().consumers == 2 && broker->stats().taps == 2, "Compare subscribes to two independent channel taps");
    write(*first, .64f, 0, 1); write(*second, .32f, 0, 1); broker->pump();
    pages.frame(broker, original, main, guitar);
    auto pair = pages.comparisonView();
    check(pair.status == Comparison::Independent && pair.a.metadata && pair.b.metadata &&
              pair.a.metadata->endpoint.request.track == main && pair.b.metadata->endpoint.request.track == guitar &&
              pair.a.binsDbfs[16] > pair.b.binsDbfs[16],
          "Compare presents the two calibrated broker windows on their explicit channels");
    const auto displayShowsBothTraces = [&] {
        auto *compare = pages.compareA.getParentComponent();
        if (!compare || !pair.a.metadata || !pair.b.metadata) return false;
        juce::Image image(juce::Image::RGB, compare->getWidth(), compare->getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics graphics(image);
            compare->paint(graphics);
        }
        const auto plot = compare->getLocalBounds().reduced(look::border).withTrimmedTop(look::fieldHeight)
            .reduced(look::gap).withTrimmedTop(look::fieldHeight).withTrimmedBottom(look::fieldHeight).toFloat();
        const auto hasSegment = [&](std::span<const float> bins, juce::Colour colour) {
            if (bins.size() <= 16) return false;
            const auto colourDistance = [](juce::Colour a, juce::Colour b) {
                return std::abs(a.getRed() - b.getRed()) + std::abs(a.getGreen() - b.getGreen()) +
                    std::abs(a.getBlue() - b.getBlue());
            };
            // Software rendering can split a thin line across two pixels. Accept
            // part of its colour, while still rejecting an unpainted plot column.
            const int maximumDistance = colourDistance(colour, look::display) * 7 / 10;
            const auto x = [&](int bin) {
                return plot.getX() + plot.getWidth() * static_cast<float>(std::log(bin * 48000. / 1024. / 20.) / std::log(1000.));
            };
            const auto y = [&](std::size_t bin) {
                return plot.getY() + plot.getHeight() * std::clamp(bins[bin] / -96.f, 0.f, 1.f);
            };
            for (float fraction : {.1f, .3f, .5f, .7f, .9f}) {
                const int px = juce::roundToInt(std::lerp(x(15), x(16), fraction));
                const int py = juce::roundToInt(std::lerp(y(15), y(16), fraction));
                bool visible = false;
                for (int dy = -2; dy <= 2; ++dy) {
                    const auto actual = image.getPixelAt(px, py + dy);
                    visible |= colourDistance(actual, colour) <= maximumDistance;
                }
                if (!visible) return false;
            }
            return true;
        };
        return hasSegment(pair.a.binsDbfs, look::value) && hasSegment(pair.b.binsDbfs, look::accent);
    };
    check(displayShowsBothTraces(), "both Compare paths draw continuous segments between consecutive measured bins");
    write(*first, .4f, 1024, 2); broker->pump();
    pages.frame(broker, original, main, main);
    check(pages.comparisonView().status == Comparison::Unavailable,
          "a live overlay waits when the two tap publications disagree on the audio transport revision");
    write(*second, .2f, 1024, 2); broker->pump();
    const int priorBMenu = pages.compareB.getSelectedId();
    pages.frame(broker, renamed, main, main);
    pair = pages.comparisonView();
    check(pair.status == Comparison::Independent && pages.compareB.getSelectedId() == priorBMenu &&
              pages.compareB.getText() == "Lead guitar" && pair.b.metadata->endpoint.request.track == guitar,
          "renaming B and moving selection keeps its authored 64-bit channel and publication");
    const auto reference = pair.b.binsDbfs.empty() ? 0.f : pair.b.binsDbfs[16];
    pages.holdB.onClick();
    check(pages.comparisonView().status == Comparison::Held && broker->stats().taps == 1,
          "holding B retains its actual bins and releases its live tap");
    write(*first, .1f, 2048, 3); broker->pump();
    pages.frame(broker, renamed, main, main);
    pair = pages.comparisonView();
    check(pair.status == Comparison::Held && pair.b.binsDbfs[16] == reference &&
              pair.a.metadata->transportRevision == 3,
          "a deliberately held reference survives a new live transport revision");
    root.setVisible(false);
    check(broker->stats().consumers == 0 && !pages.comparisonView().visible && pages.comparisonView().referenceHeld,
          "ancestor hide releases live Compare demand without a frame and retains the deliberate B reference");
    root.setVisible(true);
    pages.frame(broker, renamed, main, main);
    check(!pages.comparisonView().a.metadata && pages.comparisonView().referenceHeld,
          "showing Compare waits for a fresh A window");
    pages.clearCompare.onClick();
    check(!pages.comparisonView().referenceHeld && !pages.comparisonView().a.metadata &&
              !pages.comparisonView().b.metadata && broker->stats().consumers == 2,
          "clearing the reference fences both sources and keeps explicit endpoints");
    pages.frame(broker, removed, main, main);
    check(pages.compareB.getSelectedId() == 0 && broker->stats().taps == 0,
          "deleting B drops the pair without following the next available channel");

    pages.setPage(Page::Average);
    pages.frame(broker, original, main, guitar);
    SharedAnalysis::Request rawRequest{main};
    auto raw = broker->acquire(rawRequest);
    auto averageRequest = rawRequest;
    averageRequest.outputs = SharedAnalysis::Average1s;
    auto publishedAverage = broker->acquire(averageRequest);
    // Prime the exponential average after its zero-state reset before comparing
    // its response to a sudden drop with the latest unsmoothed window.
    constexpr int warmBlocks = 64;
    for (int block = 0; block < warmBlocks; ++block) {
        write(*first, .8f, 3072 + block * 1024, 4); broker->pump();
        pages.frame(broker, original, main, guitar);
    }
    write(*first, .1f, 3072 + warmBlocks * 1024, 4); broker->pump();
    pages.frame(broker, original, main, guitar);
    const auto average = publishedAverage.readAverage();
    const auto rawWindow = raw.read();
    const auto bins = pages.detail.view.spectrumBins();
    check(average && average->status == SharedAnalysis::AverageStatus::Ready && average->binsDbfs &&
              pages.detail.view.snapshot() == average->window && std::ranges::equal(bins, *average->binsDbfs) &&
              rawWindow && bins.size() > 16 && bins[16] > rawWindow->binsDbfs[16] &&
              broker->stats().taps == 1 && broker->stats().spectralPlans == 1,
          "the visible 1 s Average draws actual smoothed bins and shares a single transform with Raw");
    pages.detail.view.clear();
    pages.frame(broker, original, main, guitar);
    check(pages.detail.view.spectrumBins().empty() && publishedAverage.readAverage() == average,
          "clearing the display gates its next average without resetting another reader");
    pages.setPage(Page::Compare);
    check(broker->stats().consumers == 2, "leaving Average releases only the page's own lease");
    raw.reset(); publishedAverage.reset();
    root.removeFromDesktop();
}

} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI initialiser;
    try { sharingAndVisibility(); sourceAndOwnerLifetime(); parentDestruction(); modesSourcesAndFreeze(); pendingSourceLease(); cappedFrozenHistoryAndIdleReset();
          mainReadoutFormatProjection(); mainMetersSharedPresentation(); permanentMainMeter(); parkedMainLoudnessProjection();
          compactAnalyzerDock(); paletteMenuTheme(); palettesAndFrozenRaster(); restartedRasterDomain();
          freshActionsWithSharedConsumer(); backlogAndLifecycleFreshness(); spectrumSlopeProjection(); classicScopeSurface(); mixAnalyserPages(); compareAndAveragePages(); }
    catch (const std::exception &e) { ++failures; std::printf("FAIL exception: %s\n", e.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
