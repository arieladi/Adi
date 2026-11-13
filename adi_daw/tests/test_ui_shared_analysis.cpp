// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/juce/ui_analysis.hpp"
#include "../src/juce/ui_look.hpp"
#include "ui_fake_peer.hpp"
#include "adi/engine/main_loudness.hpp"
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
    for (int i = 0; i < 4; ++i) { view.frame(); (void)softwarePicture(view); }
    const auto after = f.broker->stats();
    check(after.transforms == before.transforms && after.analysisWindows == before.analysisWindows &&
              after.cursorReads == before.cursorReads && after.workerPasses == before.workerPasses &&
              after.domains == before.domains && after.taps == before.taps && after.frameRequests == before.frameRequests &&
              after.historyBytes == before.historyBytes && after.consumers == 0,
          "palette changes/unchanged frozen paints create no FFT, reads, subscriptions or history work");
    view.freeze(false); f.tap = std::make_shared<ScopeTap>(44100, SharedAnalysis::kTapSeconds);
    f.broker->refreshSources(true); f.write(); f.broker->pump(); view.frame();
    check(view.snapshot() && view.snapshot()->sampleRate == 44100 && originalRasterPixels(view, *view.snapshot()),
          "format/source epoch replacement removes old pixels and retains default calibration");
    AnalysisPane pane;
    pane.setMode(SharedSpectrumView::Mode::Spectrogram); pane.setPalette("rocket", true);
    check(pane.paletteBox.getNumItems() == 47 && pane.requestedPalette() == "rocket" && pane.reverseButton.getToggleState(),
          "palette menu shows every family and selected/reversed state");
    for (const int width : {240, 332, 500}) {
        pane.setSize(width, 420);
        const std::array<juce::Component *, 9> controls{&pane.modeBox, &pane.sourceBox, &pane.pointBox, &pane.channelBox,
            &pane.paletteBox, &pane.reverseButton, &pane.freezeButton, &pane.resetButton, &pane.expandButton};
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

} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI initialiser;
    try { sharingAndVisibility(); sourceAndOwnerLifetime(); parentDestruction(); modesSourcesAndFreeze(); pendingSourceLease(); cappedFrozenHistoryAndIdleReset();
          mainReadoutFormatProjection(); mainMetersSharedPresentation(); compactAnalyzerDock(); palettesAndFrozenRaster(); restartedRasterDomain(); }
    catch (const std::exception &e) { ++failures; std::printf("FAIL exception: %s\n", e.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
