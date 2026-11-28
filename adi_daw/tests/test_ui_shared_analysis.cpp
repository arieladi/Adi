// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/juce/ui_analysis.hpp"
#include "../src/juce/ui_colour_waveform.hpp"
#include "../src/juce/ui_filled_spectrum.hpp"
#include "../src/juce/ui_third_octave.hpp"
#include "../src/juce/ui_look.hpp"
#include "../src/juce/ui_mix_meter.hpp"
#include "../src/juce/ui_mix_analysers.hpp"
#include "ui_fake_peer.hpp"
#include "ui_picture_compare.hpp"
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
using adi::uitest::samePicturePixels;
namespace adi::ui {
struct SharedSpectrumViewTestAccess {
    static auto capture(const SharedSpectrumView &view) { return view.captureShown_; }
    static std::size_t offsetCount(const SharedSpectrumView &view) { return view.spectrumProjection_.offsets().size(); }
    static bool hasScaledPlot(const SharedSpectrumView &view) { return static_cast<bool>(view.scaledSpectrum_); }
    static bool hasRaster(const SharedSpectrumView &view) { return static_cast<bool>(view.raster_); }
    static void showFilled(SharedSpectrumView &view, std::shared_ptr<const SharedAnalysis::Result> result) {
        view.shown_ = std::move(result);
    }
    static void pointFilled(SharedSpectrumView &view, juce::Point<int> pointer) { view.filledPointer_ = pointer; }
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
juce::Image softwarePicture(SharedSpectrumView &view) {
    juce::Image image(juce::Image::RGB, view.getWidth(), view.getHeight(), true, juce::SoftwareImageType{});
    {
        juce::Graphics graphics(image);
        view.paint(graphics);
    }
    return image;
}
adi::uitest::ComparisonPicture comparisonPicture(SharedSpectrumView &view) {
    return adi::uitest::pictureForComparison(juce::Image::RGB, view.getWidth(), view.getHeight(),
                                             [&](juce::Graphics &graphics) { view.paint(graphics); });
}
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
    root.addToDesktop(0);
    check(dynamic_cast<UiFakePeer *>(root.getPeer()), "test root uses the software peer");
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
void phaseGoniometer() {
    Fixture fixture;
    Root root;
    root.setBounds(0, 0, 480, 360);
    root.setVisible(true);
    root.addToDesktop(0);
    SharedSpectrumView phase;
    phase.setMode(SharedSpectrumView::Mode::Phase);
    phase.setBounds(0, 0, 320, 240);
    root.addAndMakeVisible(phase);
    phase.present(fixture.broker, 1);

    constexpr std::int32_t frameCount = 1024;
    std::array<float, frameCount> signal{}, left{}, right{};
    for (std::size_t i = 0; i < signal.size(); ++i)
        signal[i] = static_cast<float>(std::sin(2. * std::numbers::pi * static_cast<double>(i % 64) / 64.));
    const auto write = [&](float leftGain, float rightGain, bool advancing = true) {
        for (std::size_t i = 0; i < signal.size(); ++i) {
            left[i] = signal[i] * leftGain;
            right[i] = signal[i] * rightGain;
        }
        fixture.tap->write(left.data(), right.data(), frameCount, advancing ? fixture.tap->written() : 0, advancing, 1);
        fixture.broker->pump();
        phase.frame();
    };
    const auto image = [&] {
        juce::Image pixels(juce::Image::RGB, phase.getWidth(), phase.getHeight(), true, juce::SoftwareImageType{});
        juce::Graphics graphics(pixels);
        phase.paint(graphics);
        return pixels;
    };
    const auto greenTrace = [](const juce::Image &pixels, int atX, int atY) {
        for (int y = atY - 1; y <= atY + 1; ++y)
            for (int x = atX - 1; x <= atX + 1; ++x) {
                const auto sample = pixels.getPixelAt(x, y);
                if (sample.getGreen() >= 48 && sample.getGreen() - sample.getRed() >= 18 &&
                    sample.getBlue() - sample.getRed() >= 9)
                    return true;
            }
        return false;
    };
    constexpr int centreX = 151, centreY = 114;

    write(1.f, 1.f);
    const auto mono = phase.snapshot();
    const auto monoImage = image();
    int centreLinePixels = 0;
    for (int y = centreY - 50; y <= centreY + 50; ++y)
        centreLinePixels += greenTrace(monoImage, centreX, y) ? 1 : 0;
    check(mono && mono->scopeLeft.size() == frameCount && mono->scopeLeft == mono->scopeRight &&
              fixture.broker->stats().transforms == 0 && centreLinePixels >= 95 &&
              !greenTrace(monoImage, centreX - 24, centreY - 24) && !greenTrace(monoImage, centreX + 24, centreY),
          "same-sample mono draws a continuous vertical goniometer trace on the M axis without an FFT");

    write(1.f, 0.f);
    const auto leftImage = image();
    check(greenTrace(leftImage, centreX - 24, centreY - 24) &&
              !greenTrace(leftImage, centreX + 24, centreY - 24) && greenTrace(leftImage, centreX, centreY - 36),
          "hard-left audio draws the upper-left diagonal while the previous mono frame persists");

    write(1.f, -1.f);
    const auto antiPhase = image();
    check(greenTrace(antiPhase, centreX - 48, centreY) && greenTrace(antiPhase, centreX + 48, centreY),
          "opposed channels draw along the horizontal side axis");
    write(0.f, .3f);
    write(0.f, .3f);
    check(!greenTrace(image(), centreX, centreY - 36), "after four newer frames the old mono frame is retired");

    const auto last = phase.snapshot();
    const auto frozen = image();
    phase.freeze(true);
    write(1.f, 1.f);
    check(phase.snapshot() == last && !phase.subscribed() && fixture.broker->stats().consumers == 0 &&
              greenTrace(frozen, centreX - 48, centreY) == greenTrace(image(), centreX - 48, centreY),
          "freezing retains the goniometer picture and releases live capture");
    phase.clear();
    check(!phase.snapshot() && !greenTrace(image(), centreX - 48, centreY), "clearing removes the frozen trace and afterglow");

    phase.freeze(false);
    write(.08f, .08f);
    const auto seeded = image();
    check(greenTrace(seeded, centreX, centreY - 22) && !greenTrace(seeded, centreX, centreY - 42),
          "a quiet first frame uses the reference quarter-scale AGC seed");
    constexpr std::int32_t decaySamples = 340800; // 7.1 s at the fixture's 48 kHz
    fixture.tap->write(static_cast<const float *>(nullptr), nullptr, decaySamples, fixture.tap->written(), true, 1);
    constexpr std::int32_t samplesPerPass = 256 * static_cast<std::int32_t>(SharedAnalysis::kMaxWindowsPerFrame);
    for (int remaining = decaySamples; remaining > 0; remaining -= samplesPerPass) fixture.broker->pump();
    write(.08f, .08f);
    const auto released = image();
    check(greenTrace(released, centreX, centreY - 66) && !greenTrace(released, centreX, centreY - 78),
          "one 7.1-second time constant expands a quiet trace regardless of how often the owner presents it");

    write(1.f, 0.f, false);
    write(1.f, -1.f, false);
    const auto monitoring = image();
    check(greenTrace(monitoring, centreX - 24, centreY - 24) && greenTrace(monitoring, centreX - 48, centreY),
          "monitoring during stopped transport retains the recent left and antiphase traces");
    fixture.tap->write(static_cast<const float *>(nullptr), nullptr, frameCount, 0, false, 1);
    fixture.broker->pump();
    phase.frame();
    const auto quiet = phase.snapshot();
    const auto publications = fixture.broker->stats().publications;
    fixture.tap->write(static_cast<const float *>(nullptr), nullptr, frameCount, 0, false, 1);
    fixture.broker->pump();
    phase.frame();
    const auto parked = image();
    check(quiet && !quiet->advancing && quiet->peak == std::array<float, 2>{} && phase.snapshot() == quiet &&
              fixture.broker->stats().publications == publications &&
              !greenTrace(parked, centreX - 24, centreY - 24) && !greenTrace(parked, centreX - 48, centreY),
          "the final parked-silence publication clears recent traces before the broker stops publishing");
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
        juce::Image image(juce::Image::RGB, 600, 375, true, juce::SoftwareImageType{});
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
    juce::Image image(juce::Image::RGB, 156, 311, true, juce::SoftwareImageType{});
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
    const auto spectrumWithBinding = comparisonPicture(full);
    full.setMainMeterSource({});
    const auto spectrumWithoutBinding = comparisonPicture(full);
    check(samePicturePixels(spectrumWithBinding, spectrumWithoutBinding),
          "default Spectrum software image is unchanged by the optional Main binding");
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
    (void)softwarePicture(full); (void)softwarePicture(info); (void)softwarePicture(window);
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
    check(!meter.reading() && meter.scaleBox.getSelectedId() == 1, "M/S and A weighting start a fresh measurement, retaining its scale");
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
    meter.listenBox.setSelectedId(5, juce::sendNotificationSync);
    owner.setTopLeftPosition(-120, 0);
    check(meter.subscribed() && calls.back() == MixOutputMeter::Audition::Side,
          "a partly visible meter keeps its lease and audition");
    owner.setTopLeftPosition(-160, 0);
    check(!meter.subscribed() && f.broker->stats().consumers == 0 && calls.size() == 7 &&
              calls.back() == MixOutputMeter::Audition::Stereo && meter.listenBox.getSelectedId() == 1,
          "an entirely clipped ancestor releases the meter and ends audition before the next frame");
    const auto clippedReads = reads;
    f.write(4096); f.broker->pump(); meter.frame(f.broker, 1, source);
    check(!meter.reading() && reads == clippedReads && f.broker->stats().consumers == 0,
          "a fully clipped meter neither reads loudness nor holds analysis demand");
    owner.setTopLeftPosition(0, 0);
    meter.frame(f.broker, 1, source);
    check(meter.subscribed() && !meter.reading() && calls.size() == 7,
          "returning to view acquires fresh data without restoring audition");
    meter.setTopLeftPosition(root.getWidth(), 0);
    check(!meter.subscribed() && f.broker->stats().consumers == 0,
          "moving the meter itself out of view also releases its lease immediately");
    meter.setAuditionSink({});
    root.removeFromDesktop();
}

void referenceMainMeterScale() {
    using namespace adi::ui;
    Fixture f;
    Root root;
    MixOutputMeter meter;
    root.setSize(152, 520);
    meter.setBounds(root.getLocalBounds());
    root.addAndMakeVisible(meter);
    root.setVisible(true);
    root.addToDesktop(0);
    meter.weightingBox.setSelectedId(2, juce::sendNotificationSync);
    meter.frame(f.broker, 1);
    f.write(); f.broker->pump(); meter.frame(f.broker, 1);
    const auto weighted = meter.reading();
    meter.scaleBox.setSelectedId(3, juce::sendNotificationSync);
    meter.frame(f.broker, 1);
    check(weighted && !meter.reading() && !meter.weightingBox.isEnabled(),
          "K-14 starts the fixed-time unweighted measurement");
    f.write(1024); f.broker->pump(); meter.frame(f.broker, 1);
    const auto k14 = meter.reading();
    meter.scaleBox.setSelectedId(4, juce::sendNotificationSync);
    meter.frame(f.broker, 1);
    check(k14 && meter.reading() == k14 && !meter.weightingBox.isEnabled(),
          "changing the K reference level keeps the same fixed-time interval");
    const auto paintTop = [&] {
        juce::Image raster(juce::Image::RGB, 152, 520, true, juce::SoftwareImageType{});
        {
            juce::Graphics g(raster);
            meter.paintEntireComponent(g, true);
        }
        return raster.getPixelAt(35, 123);
    };
    check(paintTop() != juce::Colour(look::record), "K bias does not light the raw clip indicator");
    constexpr std::array left{-1.f, 0.f}, right{0.f, 0.f};
    f.tap->write(left.data(), right.data(), static_cast<std::int32_t>(left.size()), 2048, true);
    f.broker->pump(); meter.frame(f.broker, 1);
    check(paintTop() == juce::Colour(look::record), "a negative full-scale sample lights the left clip strip");
    meter.scaleBox.setSelectedId(1, juce::sendNotificationSync);
    meter.frame(f.broker, 1);
    check(meter.weightingBox.isEnabled() && meter.weightingBox.getSelectedId() == 2 && !meter.reading(),
          "dBFS resumes the saved A-weighted profile at the next samples");
    root.removeFromDesktop();
}

void linkedMainMeter() {
    using namespace adi::ui;
    Fixture f;
    Root root;
    MixOutputMeter meter;
    root.setSize(240, 520);
    meter.setBounds(root.getLocalBounds());
    root.addAndMakeVisible(meter);
    root.setVisible(true);
    root.addToDesktop(0); // software peer only
    int auditionCalls = 0;
    meter.setAuditionSink([&](auto) { ++auditionCalls; });
    auto other = f.broker->acquireStereoMeter(1, {});
    meter.frame(f.broker, 1, {});
    check(meter.channelBox.getSelectedId() == 1 && meter.channelBox.getNumItems() == 3 &&
              meter.channelBox.getItemText(2) == "Link", "the Main meter offers a distinct Link mode with Stereo as default");
    meter.channelBox.setSelectedId(3, juce::sendNotificationSync);
    meter.frame(f.broker, 1, {});
    const auto shared = f.broker->stats();
    check(shared.consumers == 2 && shared.domains == 2 && shared.taps == 1 && f.acquired == 1,
          "Link uses its own meter profile and shares the existing physical tap with Stereo");
    f.write();
    f.broker->pump();
    meter.frame(f.broker, 1, {});
    const auto linked = meter.reading();
    const auto stereo = other.readStereoMeter();
    check(linked && linked->reading.linkedDynamicDb && stereo && !stereo->reading.linkedDynamicDb &&
              linked->reading.maximumPeak == stereo->reading.maximumPeak &&
              linked->reading.sineReferencedRms == stereo->reading.sineReferencedRms && auditionCalls == 0,
          "only Link exposes the combined dynamic result and both modes retain the same levels without a monitor change");

    meter.resetButton.onClick();
    meter.frame(f.broker, 1, {});
    check(!meter.reading() && other.readStereoMeter() == stereo,
          "Link Reset clears the linked interval without resetting a Stereo observer");
    meter.channelBox.setSelectedId(1, juce::sendNotificationSync);
    meter.frame(f.broker, 1, {});
    check(meter.reading() == stereo && f.broker->stats().domains == 1 && auditionCalls == 0,
          "changing back to Stereo reuses its existing publication and releases Link work");

    meter.channelBox.setSelectedId(3, juce::sendNotificationSync);
    meter.frame(f.broker, 1, {});
    other.reset();
    root.setVisible(false);
    const auto hidden = f.broker->stats();
    f.write(1024);
    f.broker->pump();
    meter.frame(f.broker, 1, {});
    check(!meter.subscribed() && !meter.reading() && hidden.taps == 0 && f.released == 1 &&
              hidden.workerPasses == f.broker->stats().workerPasses && auditionCalls == 0,
          "an unseen linked meter holds no tap and executes no analysis");
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
    snapshot.shortTermLufs = -24.f;
    snapshot.integratedLufs = -18.f;
    meter.frame(f.broker, 1, source);
    check(reads == 5 && meter.loudness() == snapshot && root.fake->repaints == stablePaint,
          "Main updates not shown on the stereo meter do not repaint its readout");
    snapshot.sampleRate = 44100.;
    meter.frame(f.broker, 1, source);
    check(meter.reading() == envelope && !meter.loudness() && reads == 6, "a stale loudness format clears the overlay");
    meter.suspend(true); meter.frame(f.broker, 1, source);
    check(!meter.reading() && !meter.loudness() && reads == 6 && f.broker->stats().consumers == 0,
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

bool samePixels(const juce::Image &a, const juce::Image &b) {
    if (a.getBounds() != b.getBounds()) return false;
    for (int y = 0; y < a.getHeight(); ++y) for (int x = 0; x < a.getWidth(); ++x)
        if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) return false;
    return true;
}
void wordsMaskedInComparison() {
    const auto draw = [](const char *word, juce::Colour trace) {
        return adi::uitest::pictureForComparison(juce::Image::RGB, 140, 40, [&](juce::Graphics &graphics) {
            graphics.fillAll(juce::Colours::black);
            graphics.setColour(trace);
            graphics.fillRect(12, 26, 110, 3);
            graphics.setColour(juce::Colours::white);
            graphics.setFont(13.f);
            graphics.drawFittedText(word, {0, 0, 140, 20}, juce::Justification::centredLeft, 1);
        });
    };
    const auto before = draw("Short", juce::Colours::red);
    const auto after = draw("A different label", juce::Colours::red);
    bool textPainted = false;
    for (int y = 0; y < 20 && !textPainted; ++y)
        for (int x = 0; x < before.raster.getWidth(); ++x)
            textPainted |= before.raster.getPixelAt(x, y) != juce::Colours::black;
    check(textPainted && samePicturePixels(before, after), "raster comparisons mask the glyphs of both labels");
    check(!samePicturePixels(before, draw("A different label", juce::Colours::green)),
          "raster comparisons still detect changes to the drawn trace");
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
    const juce::SoftwareImageType software;
    const auto initialPixels = software.convert(initialDefault);
    const auto rocket = palettes::find("rocket");
    const auto initialStock = swatch(rocket);
    check(initialPixels.isValid() && initialPixels.getPixelAt(0, 0) == look::display && initialStock.isValid(),
          "an open analyser pane uses the current theme's default and a fixed stock swatch");

    look::apply(oldTheme == ThemeId::Light ? ThemeId::Dark : ThemeId::Light, density);
    pane.frame({}, 0, 0);
    const auto currentDefault = swatch(palettes::defaultIndex);
    const auto currentPixels = software.convert(currentDefault);
    check(currentPixels.isValid() && currentPixels.getPixelAt(0, 0) == look::display &&
              !samePixels(initialPixels, currentPixels) && swatch(rocket) == initialStock &&
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
    const auto before = f.broker->stats(); const auto picture = comparisonPicture(view);
    view.setPalette("viridis"); const auto colored = comparisonPicture(view);
    check(!samePicturePixels(picture, colored) && view.snapshot() == snapshot && view.frozen(),
          "palette selection recolors the existing frozen raster without replacing its measurement");
    view.setPalette("gray", true); const auto gray = comparisonPicture(view);
    check(!samePicturePixels(colored, gray) && gray.raster.getPixelAt(5, 21) == look::display,
          "reverse and grayscale recolor valid pixels while missing history remains background");
    view.setPalette("adi");
    check(samePicturePixels(picture, comparisonPicture(view)), "returning to original palette restores every frozen displayed pixel");
    const auto oldTheme = look::themeId();
    const auto density = look::densityId();
    look::apply(oldTheme == ThemeId::Light ? ThemeId::Dark : ThemeId::Light, density);
    view.frame();
    check(snapshot && analysisPaletteColour(0, 0) == look::display && !samePicturePixels(picture, comparisonPicture(view)) &&
              originalRasterPixels(view, *snapshot) && view.snapshot() == snapshot,
          "a theme switch recolors gaps and signal in a frozen raster without changing its publication");
    look::apply(oldTheme, density);
    view.frame();
    check(samePicturePixels(picture, comparisonPicture(view)), "restoring the theme restores every frozen pixel");
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
        const auto first = view.snapshot(); const auto firstImage = comparisonPicture(view);
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
        check(second && !samePicturePixels(firstImage, comparisonPicture(view)) && originalRasterPixels(view, *second),
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

void clearSpectrogramWhileWorkerAhead() {
    using Mode = SharedSpectrumView::Mode;
    using Motion = SharedSpectrumView::HistoryMotion;
    constexpr int fft = 1024, hop = fft / 4, backlog = 4;
    constexpr int left = 5, top = 21, width = 128, height = 256;
    Fixture f;
    Root root;
    root.setSize(340, 340);
    root.setVisible(true);
    root.addToDesktop(0);
    SharedSpectrumView cleared, sibling;
    cleared.setBounds(0, 0, 156, 311);
    sibling.setBounds(170, 0, 156, 311);
    for (auto *view : {&cleared, &sibling}) {
        view->setMode(Mode::Spectrogram);
        root.addAndMakeVisible(*view);
        view->present(f.broker, 1);
        view->frame();
    }
    check(dynamic_cast<UiFakePeer *>(root.getPeer()) && f.acquired == 1 &&
              f.broker->stats().consumers == 2,
          "both spectrograms use a single tap through a software peer");
    writeFrames(f, fft);
    f.broker->pump();
    cleared.frame();
    sibling.frame();
    const auto displayed = cleared.snapshot();
    check(displayed && displayed == sibling.snapshot() && displayed->columns.size() == 1,
          "both spectrograms initially present one completed window");

    writeFrames(f, backlog * hop);
    f.broker->pump();
    sibling.frame();
    const auto ahead = sibling.snapshot();
    check(displayed && ahead && cleared.snapshot() == displayed &&
              ahead->historySerial == displayed->historySerial + backlog &&
              ahead->columns.size() == 1 + backlog,
          "four real hops are published ahead of the view about to Clear");

    const auto fence = f.tap->written();
    cleared.clear();
    cleared.frame();
    check(!cleared.snapshot() && sibling.snapshot() == ahead && f.broker->stats().consumers == 2,
          "Clear waits for a post-fence input window while the other view keeps its publication");

    writeFrames(f, hop);
    f.broker->pump();
    cleared.frame();
    sibling.frame();
    const auto complete = sibling.snapshot();
    check(complete && ahead && complete->historySerial == ahead->historySerial + 1 &&
              complete->columns.size() == 2 + backlog && originalRasterPixels(sibling, *complete),
          "the sibling still renders all of the pre-Clear and fresh history");
    if (!complete)
        return;

    auto visible = *complete;
    std::erase_if(visible.columns, [fence](const auto &column) {
        return column.firstSequence + fft <= fence;
    });
    check(visible.columns.size() == 1 && visible.columns.front().firstSequence + fft == fence + hop &&
              visible.columns.front().firstSequence < fence,
          "the writer fence admits just the one overlapping window completed after Clear");
    check(cleared.snapshot() && originalRasterPixels(cleared, visible),
          "the cleared Scroll raster shows the fresh column and none of the worker's pre-fence backlog");

    const auto fullScroll = softwarePicture(sibling);
    cleared.setHistoryMotion(Motion::Sweep);
    const auto localSweep = softwarePicture(cleared);
    bool freshAtOrigin = true, lit = false;
    for (int y = 0; y < height; ++y) {
        const auto expected = fullScroll.getPixelAt(left + width - 1, top + y);
        lit = lit || expected != adi::ui::look::display;
        freshAtOrigin = freshAtOrigin && localSweep.getPixelAt(left, top + y) == expected;
        // The next write edge at x = 1 may have the sweep cursor painted on it.
        for (int x = 2; x < width; ++x)
            freshAtOrigin = freshAtOrigin && localSweep.getPixelAt(left + x, top + y) == adi::ui::look::display;
    }
    check(lit && freshAtOrigin, "Sweep restarts at the first column after the writer fence");
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
    const auto original = comparisonPicture(full), compact = comparisonPicture(info);
    const auto snapshot = full.snapshot();
    check(snapshot && snapshot == info.snapshot() && samePicturePixels(original, compact),
          "slope fixture begins with exact shared calibrated data and identical Flat pictures");
    const auto raw = snapshot ? snapshot->binsDbfs : std::vector<float>{};
    full.freeze(true); const auto frozenFlat = comparisonPicture(full); const auto before = f.broker->stats();
    full.setSpectrumSlope(SpectrumSlope::Tilt45); const auto tilted = comparisonPicture(full);
    check(snapshot && !samePicturePixels(frozenFlat, tilted) && full.snapshot() == snapshot && full.frozen() &&
              snapshot->binsDbfs == raw && samePicturePixels(compact, comparisonPicture(info)),
          "Frozen Spectrum reprojects locally without mutating its retained bins or another consumer");
    for (int i = 0; i < 4; ++i) { full.frame(); (void)softwarePicture(full); }
    const auto after = f.broker->stats();
    check(after.transforms == before.transforms && after.analysisWindows == before.analysisWindows &&
              after.publications == before.publications && after.cursorReads == before.cursorReads &&
              after.consumers == before.consumers && after.domains == before.domains && after.taps == before.taps,
          "slope changes and unchanged Frozen frames create no measurement, transform or demand work");
    full.setSpectrumSlope(SpectrumSlope::Flat);
    check(samePicturePixels(frozenFlat, comparisonPicture(full)) && full.snapshot() == snapshot && snapshot->binsDbfs == raw,
          "returning Flat restores the exact original pixels and raw measurement");
    full.setSpectrumSlope(SpectrumSlope::Tilt45); full.freeze(false); writeFrames(f, 256); f.broker->pump(); full.frame(); info.frame();
    check(full.snapshot() && full.snapshot() == info.snapshot() && f.broker->stats().domains == 1 &&
              f.broker->stats().taps == 1 && f.broker->stats().spectralPlans == 1,
          "differing live display slopes still share one compatible result, source and FFT");
    full.setMode(SharedSpectrumView::Mode::Meters); f.broker->pump(); full.frame();
    check(full.snapshot() && full.snapshot()->outputs & SharedAnalysis::Meters,
          "meter-isolation oracle uses a real shared meter publication");
    const auto meterFlat = comparisonPicture(full); full.setSpectrumSlope(SpectrumSlope::Flat);
    check(samePicturePixels(meterFlat, comparisonPicture(full)), "Spectrum slope never modifies raw sample peak/RMS presentation");
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

void filledSpectrumProjection() {
    using namespace adi::ui;
    using namespace filled_spectrum;
    constexpr std::array bins{99.f, 2.f, 8.f, 3.f, 5.f};
    check(std::abs(columnMagnitude(bins, 100., 90., 350.) - 8.f) < 1.e-5f,
          "one display column keeps the strongest FFT bin it covers");
    const auto fraction = (std::sqrt(110. * 140.) - 100.) / 100.;
    check(std::abs(columnMagnitude(bins, 100., 110., 140.) - (2. + 6. * fraction)) < 1.e-5,
          "a narrow column interpolates amplitudes at its log-frequency centre");
    check(std::abs(columnMagnitude(bins, 100., 10., 30.) - 2.f) < 1.e-5f,
          "the DC bin stays off the logarithmic display");
    const auto at = [](double hz, double fractionY) { return cursorAt(std::log(hz / lowHz) / logWidth(), fractionY); };
    const auto a = at(440., .5), b = at(1000., 0.), c = at(261.6255653, 1.);
    check(std::abs(a.hz - 440.) < 1.e-7 && std::string_view(a.note) == "A" && a.octave == 3 && a.cents == 0 &&
              std::abs(a.levelDb + 46.5) < 1.e-7, "cursor in the plot reads 440 Hz as A3 and its y coordinate as level");
    check(std::string_view(b.note) == "B" && b.octave == 4 && b.cents == 21 && std::abs(b.levelDb + 13.) < 1.e-7 &&
              std::string_view(c.note) == "C" && c.octave == 3 && c.cents == 0 && std::abs(c.levelDb + 80.) < 1.e-7,
          "cursor matches the reference pitch convention and calibrated dB endpoints");

    SharedSpectrumView view;
    view.setBounds(0, 0, 400, 200);
    view.setMode(SharedSpectrumView::Mode::FilledSpectrum);
    auto result = std::make_shared<SharedAnalysis::Result>();
    result->sampleRate = 48000.;
    result->request.fftSize = 4096;
    auto spectra = std::make_shared<SharedAnalysis::FilledSpectrumResult>();
    spectra->magnitudes[static_cast<std::size_t>(SharedAnalysis::FilledCurve::RealtimeMaximum)].assign(2049, .01f);
    spectra->magnitudes[static_cast<std::size_t>(SharedAnalysis::FilledCurve::HeldMaximum)].assign(2049, .1f);
    spectra->magnitudes[static_cast<std::size_t>(SharedAnalysis::FilledCurve::Average)].assign(2049, .03f);
    result->filledSpectrum = spectra;
    SharedSpectrumViewTestAccess::showFilled(view, result);
    const auto picture = comparisonPicture(view);
    const auto held = picture.raster.getPixelAt(225, 60), live = picture.raster.getPixelAt(225, 110);
    check(held.getGreen() > held.getRed() && live.getGreen() > held.getGreen(),
          "the software plot draws a darker filled hold behind the brighter live trace");
    view.setFilledSpectrumCurve(SharedAnalysis::FilledCurve::Average);
    check(!samePicturePixels(picture, comparisonPicture(view)) && view.snapshot() == result,
          "selecting another curve redraws the same immutable measurement");
    SharedSpectrumViewTestAccess::pointFilled(view, {222, 100});
    const auto withPointer = comparisonPicture(view);
    SharedSpectrumViewTestAccess::pointFilled(view, {-1, -1});
    check(!samePicturePixels(withPointer, comparisonPicture(view)), "a pointer inside the software plot draws the readout overlay");
}

void thirdOctaveAndPeriodSurface() {
    using namespace adi::ui;
    using namespace third_octave;
    check(centres.size() == 30 && centres.front() == 25. && centres[12] == 400. && centres.back() == 20000.,
          "the display follows all thirty ISO centre frequencies from 25 Hz to 20 kHz");
    constexpr std::array bins{-4.f, -30.f, -12.f, -44.f, -18.f, -22.f, -24.f, -34.f, -7.f, -35.f, -10.f, -13.f, -19.f};
    check(std::abs(bandLevel(bins, 10., 100.) + 10.f) < .01f, "the band retains its highest calibrated bin");
    constexpr std::array lowBins{0.f, -40.f, -20.f, -45.f};
    const auto expected = 20. * std::log10(std::lerp(.01, .1, (31.5 - 25.) / 25.));
    check(std::abs(bandLevel(lowBins, 25., 31.5) - expected) < .01 &&
              bandLevel(lowBins, 25., 25.) < -39.f && bandLevel(lowBins, 25., 250.) < -100.f,
          "narrow bands interpolate linear magnitude, skip DC and respect Nyquist");
    check(std::abs(holdLevel(-24.f, -50.f, .25) + 30.f) < .01f &&
              std::abs(holdLevel(-24.f, -50.f, 2.) + 50.f) < .01f,
          "band hold falls 24 dB per second of source time and stops at the current level");

    Fixture fixture;
    Root root;
    root.setBounds(0, 0, 640, 360);
    root.setVisible(true);
    root.addToDesktop(0);
    SharedSpectrumView view;
    view.setMode(SharedSpectrumView::Mode::ThirdOctaveBands);
    view.setBounds(root.getLocalBounds());
    root.addAndMakeVisible(view);
    view.present(fixture.broker, 1);
    std::array<float, 4096> tone{};
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = static_cast<float>(.3 * std::sin(2. * std::numbers::pi * 440. * static_cast<double>(i) / 48000.));
    fixture.tap->write(tone.data(), tone.data(), static_cast<std::int32_t>(tone.size()), 0, true);
    fixture.broker->pump();
    view.frame();
    const auto shot = view.snapshot();
    check(dynamic_cast<UiFakePeer *>(root.getPeer()) && shot && shot->periodScope &&
              shot->request.fftSize == 4096 && shot->request.channel == SharedAnalysis::Channel::Mid &&
              (shot->outputs & (SharedAnalysis::Spectrum | SharedAnalysis::PeriodScope)) ==
                  (SharedAnalysis::Spectrum | SharedAnalysis::PeriodScope) &&
              shot->periodScope->lag > 0. && std::abs(12800. / shot->periodScope->lag - 440.) < 3.,
          "the fake-peer bands page uses the paired 4096-sample spectrum and a locked A3 scope");
    const auto pixels = comparisonPicture(view);
    const auto cyan = pixels.raster.getPixelAt(217, 102);
    check(cyan.getBlue() > 175 && cyan.getGreen() > 200 && cyan.getRed() < 160,
          "the 400 Hz column is measured and drawn in the left pane");
    const auto scopePixel = [&pixels](int x, int y) {
        for (int dy = -4; dy <= 4; ++dy) {
            const auto colour = pixels.raster.getPixelAt(x, y + dy);
            if (colour.getGreen() > 115 && colour.getGreen() > colour.getRed() && colour.getRed() > colour.getBlue()) return true;
        }
        return false;
    };
    check(scopePixel(551, 146) && scopePixel(607, 230), "the right pane draws one rising, then falling period around its centre line");
    view.clear();
    check(!view.snapshot() && !samePicturePixels(pixels, comparisonPicture(view)), "clearing resets the bands, held markers and scope together");
    root.setVisible(false);
    view.frame();
    check(!view.subscribed() && fixture.broker->stats().consumers == 0, "hidden bands release their passive analysis lease");

    AnalysisPane standalone;
    standalone.modeBox.setSelectedId(static_cast<int>(SharedSpectrumView::Mode::ThirdOctaveBands) + 1, juce::sendNotificationSync);
    check(standalone.mode() == SharedSpectrumView::Mode::ThirdOctaveBands && standalone.modeBox.getText() == "Third-Octave Bands" &&
              !standalone.channelBox.isVisible() && !standalone.slopeBox.isVisible() && !standalone.scaleBox.isVisible(),
          "the detached selector offers the bands and hides controls for its fixed measurement");
}

void colourWaveformSurface() {
    using namespace adi::ui;
    using namespace colour_waveform;
    const auto edges = bandEdges(.8f, {.25f, .5f, .25f});
    check(std::abs(edges[0] - .2f) < 1.e-6f && std::abs(edges[1] - .6f) < 1.e-6f && std::abs(edges[2] - .8f) < 1.e-6f,
          "three colour heights stack their energy shares inside the original peak envelope");
    check(std::abs(columnX(128, 630.f) - 157.5f) < .01f && std::abs(columnX(512, 630.f) - 630.f) < .01f,
          "a half-second occupies one fourth of the two-second canvas, ending at Now");
    adi::dsp::ColourWaveformReading suffix;
    suffix.firstSequence = 100;
    suffix.serial = 3;
    suffix.size = 2;
    check(firstVisibleColumn(suffix, 288, 48000.) == 0 && firstVisibleColumn(suffix, 289, 48000.) == 1 &&
              firstVisibleColumn(suffix, 476, 48000.) == 2,
          "a local clear keeps only complete columns that start at or after the source fence");

    Fixture fixture;
    Root root;
    root.setBounds(0, 0, 640, 360);
    root.setVisible(true);
    root.addToDesktop(0);
    SharedSpectrumView view;
    view.setBounds(root.getLocalBounds());
    view.setMode(SharedSpectrumView::Mode::ColourWaveform);
    root.addAndMakeVisible(view);
    view.present(fixture.broker, 1);
    const auto blank = comparisonPicture(view);
    constexpr int length = 4800;
    std::array<float, length> tone{}, silence{};
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = static_cast<float>(.65 * std::sin(2. * std::numbers::pi * 80. * static_cast<double>(i) / 48000.));
    fixture.tap->write(tone.data(), tone.data(), length, 0, true);
    fixture.broker->pump();
    view.frame();
    const auto current = view.snapshot();
    check(dynamic_cast<UiFakePeer *>(root.getPeer()) && current && current->colourWaveform &&
              current->colourWaveform->size >= 20 && current->colourWaveform->size < 30 &&
              current->request.channel == SharedAnalysis::Channel::Mid && fixture.broker->stats().spectralPlans == 0,
          "the fake-peer waveform consumes only timestamped paired-mono columns at the native source rate");
    const auto hasBlue = [](const juce::Image &picture, int x, int y) {
        const auto pixel = picture.getPixelAt(x, y);
        return pixel.getBlue() > 170 && pixel.getBlue() > pixel.getGreen() && pixel.getGreen() > pixel.getRed();
    };
    const auto fresh = softwarePicture(view);
    check(hasBlue(fresh, 624, 150) && hasBlue(fresh, 624, 212) &&
              fresh.getPixelAt(490, 150) == blank.raster.getPixelAt(490, 150) &&
              fresh.getPixelAt(638, 150) == blank.raster.getPixelAt(638, 150),
          "the captured low band mirrors at the right edge, with empty past and no future trace");
    fixture.tap->write(silence.data(), silence.data(), length, length, true);
    fixture.broker->pump();
    view.frame();
    const auto scrolled = softwarePicture(view);
    check(hasBlue(scrolled, 592, 150) && !hasBlue(scrolled, 624, 150),
          "another 100 ms shifts the earlier peak 32 px to the left and leaves the newest silence at the right");
    const auto retained = view.snapshot();
    view.freeze(true);
    check(retained && view.snapshot() == retained && !view.subscribed() && hasBlue(softwarePicture(view), 592, 150),
          "Freeze keeps the rendered two-second picture and releases its passive demand");
    view.freeze(false);
    SharedAnalysis::Request siblingRequest{1};
    siblingRequest.outputs = SharedAnalysis::ColourWaveform;
    auto sibling = fixture.broker->acquire(siblingRequest);
    fixture.tap->write(tone.data(), tone.data(), length, 2 * length, true);
    fixture.broker->pump();
    view.frame();
    const auto siblingPicture = sibling.read();
    view.clear();
    check(!view.snapshot() && siblingPicture && sibling.read() == siblingPicture,
          "Clear empties the local waveform while its sibling retains audio history");
    fixture.tap->write(silence.data(), silence.data(), length, 3 * length, true);
    fixture.broker->pump();
    view.frame();
    check(view.snapshot() && samePicturePixels(blank, comparisonPicture(view)), "post-Clear silence cannot reveal older colour columns");
    root.setVisible(false);
    view.frame();
    check(!view.subscribed() && fixture.broker->stats().consumers == 1, "hiding the waveform releases its lease alone");

    AnalysisPane standalone;
    standalone.modeBox.setSelectedId(static_cast<int>(SharedSpectrumView::Mode::ColourWaveform) + 1, juce::sendNotificationSync);
    check(standalone.mode() == SharedSpectrumView::Mode::ColourWaveform && standalone.modeBox.getText() == "Colour Waveform" &&
              !standalone.channelBox.isVisible() && !standalone.historyBox.isVisible() && !standalone.paletteBox.isVisible(),
          "the detached analyser offers the colour waveform with its fixed scroll and colour stack");
}

void spectrumDetailScale() {
    using namespace adi::ui;
    using Mode = SharedSpectrumView::Mode;
    using Scale = SharedSpectrumView::SpectrumScale;
    const auto writeTone = [](Fixture &f, int bin, int spread = 0) {
        std::array<float, 1024> samples{};
        const auto amplitude = std::pow(10., -30. / 20.);
        const auto start = f.tap->written();
        for (int side = -spread; side <= spread; ++side) {
            const auto radiansPerSample = 2. * std::numbers::pi * (bin + side * 2) / 1024.;
            for (std::size_t i = 0; i < samples.size(); ++i)
                samples[i] += static_cast<float>(amplitude * std::sin(radiansPerSample * static_cast<double>(i)));
        }
        f.tap->write(samples.data(), samples.data(), static_cast<int>(samples.size()), start, true);
    };
    const auto sameWork = [](const auto &before, const auto &after) {
        return before.transforms == after.transforms && before.analysisWindows == after.analysisWindows &&
            before.publications == after.publications && before.cursorReads == after.cursorReads &&
            before.workerPasses == after.workerPasses && before.consumers == after.consumers &&
            before.domains == after.domains && before.taps == after.taps &&
            before.frameRequests == after.frameRequests && before.spectralPlans == after.spectralPlans;
    };

    Fixture f, lowRate;
    Root root; root.setBounds(0, 0, 680, 720); root.setVisible(true); root.addToDesktop(0);
    AnalysisPane pane; pane.setBounds(0, 0, 640, 360);
    SharedSpectrumView sibling; sibling.setBounds(0, 380, 640, 320);
    root.addAndMakeVisible(pane);
    root.addAndMakeVisible(sibling);
    pane.frame(f.broker, 1, 1); sibling.present(f.broker, 1);
    check(pane.scaleBox.isVisible() && pane.scaleBox.getSelectedId() == 1 &&
              pane.view.spectrumScale() == Scale::Wide && !SharedSpectrumViewTestAccess::hasScaledPlot(pane.view),
          "expanded Spectrum keeps the wide scale by default and allocates no detail geometry");
    writeTone(f, 32); f.broker->pump(); pane.frame(f.broker, 1, 1); sibling.frame();
    const auto source = pane.view.snapshot();
    const auto original = comparisonPicture(pane.view);
    const auto siblingImage = comparisonPicture(sibling);
    const auto before = f.broker->stats();

    pane.scaleBox.setSelectedId(2, juce::dontSendNotification); pane.scaleBox.onChange();
    const auto detail = comparisonPicture(pane.view);
    check(source && source == pane.view.snapshot() && source == sibling.snapshot() && f.acquired == 1 &&
              SharedSpectrumViewTestAccess::hasScaledPlot(pane.view) && !samePicturePixels(original, detail) &&
              samePicturePixels(siblingImage, comparisonPicture(sibling)) && sameWork(before, f.broker->stats()),
          "detail menu reprojects the existing live publication while a wide sibling keeps the same single tap");

    // A coherent -30 dBFS tone lands one fifth down the detail range. Axis labels
    // consume their own strip so the curve and the fill remain in graph space.
    const auto tracePoint = [](const juce::Image &image, double hz, float db) {
        const auto width = static_cast<float>(image.getWidth()) - 10.f - 18.f - look::fontCaption * 3.f;
        const auto height = static_cast<float>(image.getHeight()) - 10.f - 16.f - 29.f - look::fontCaption;
        const auto x = static_cast<float>(std::log(hz / 20.) / std::log(1000.));
        return juce::Point<int>{juce::roundToInt(5.f + (width - 1.f) * x),
            juce::roundToInt(21.f + (height - 1.f) * ((-18.f - db) / 60.f))};
    };
    const auto peak = tracePoint(detail.raster, 1500., -30.f);
    check(detail.raster.getPixelAt(peak.x, peak.y + 12) != look::display &&
              detail.raster.getPixelAt(peak.x, peak.y - 12) == look::display,
          "the tone fill starts at its calibrated detail height rather than at the wide scale height");

    const auto repaints = root.fake->repaints;
    pane.setSpectrumScale(Scale::Detail);
    pane.frame(f.broker, 1, 1);
    check(root.fake->repaints == repaints && sameWork(before, f.broker->stats()) &&
              samePicturePixels(detail, comparisonPicture(pane.view)),
          "unchanged scale, paints and owner frame do no new work or invalidation");
    pane.setSize(420, 360);
    const auto resized = softwarePicture(pane.view);
    const auto resizedPeak = tracePoint(resized, 1500., -30.f);
    check(resized.getPixelAt(resizedPeak.x, resizedPeak.y + 12) != look::display &&
              pane.view.snapshot() == source && sameWork(before, f.broker->stats()),
          "resize transforms the same trace and keeps its calibrated height and broker result");
    pane.setSize(640, 360);

    pane.view.freeze(true);
    const auto frozen = comparisonPicture(pane.view);
    const auto held = f.broker->stats();
    pane.setSpectrumScale(Scale::Wide);
    pane.setSpectrumScale(Scale::Detail);
    check(pane.view.frozen() && pane.view.snapshot() == source && samePicturePixels(frozen, comparisonPicture(pane.view)) &&
              sameWork(held, f.broker->stats()),
          "frozen view can switch scales and return to the same cached picture without renewing demand");

    pane.setMode(Mode::Meters);
    check(!pane.scaleBox.isVisible() && pane.view.spectrumScale() == Scale::Detail &&
              !SharedSpectrumViewTestAccess::hasScaledPlot(pane.view),
          "non-spectrum mode hides the control and releases geometry but remembers the view preference");
    pane.setMode(Mode::SpectrumAverage);
    writeTone(f, 32); f.broker->pump(); pane.frame(f.broker, 1, 1);
    const auto average = pane.view.snapshot();
    const auto averageBins = pane.view.spectrumBins().data();
    const auto averageWork = f.broker->stats();
    pane.setSpectrumScale(Scale::Wide);
    pane.setSpectrumScale(Scale::Detail);
    check(pane.scaleBox.isVisible() && pane.scaleBox.getSelectedId() == 2 && average && averageBins &&
              pane.view.snapshot() == average && pane.view.spectrumBins().data() == averageBins &&
              SharedSpectrumViewTestAccess::hasScaledPlot(pane.view) && sameWork(averageWork, f.broker->stats()),
          "one-second average uses the same local scale without a new averaging interval or transform");
    pane.setMode(Mode::Spectrum); pane.view.freeze(false); sibling.setVisible(false); root.setVisible(false);
    check(!pane.view.subscribed() && !SharedSpectrumViewTestAccess::hasScaledPlot(pane.view) &&
              !pane.view.snapshot() && f.broker->stats().consumers == 0 && f.released == 1,
          "ancestor hide drops all demand and the detail geometry without waiting on a frame");
    const auto hidden = f.broker->stats();
    pane.setSpectrumScale(Scale::Wide); pane.setSpectrumScale(Scale::Detail);
    pane.frame(f.broker, 1, 1); f.broker->pump();
    check(!SharedSpectrumViewTestAccess::hasScaledPlot(pane.view) && sameWork(hidden, f.broker->stats()) && f.acquired == 1,
          "hidden scale changes and frames do not reacquire a tap, analyse, publish or prepare a trace");

    lowRate.tap = std::make_shared<ScopeTap>(32000, SharedAnalysis::kTapSeconds);
    root.setVisible(true); pane.frame(lowRate.broker, 1, 1);
    writeTone(lowRate, 480, 4); lowRate.broker->pump(); pane.frame(lowRate.broker, 1, 1);
    const auto limited = softwarePicture(pane.view);
    const auto available = tracePoint(limited, 15000., -30.f);
    const auto beyond = tracePoint(limited, 18300., -30.f);
    check(pane.view.snapshot() && pane.view.snapshot()->sampleRate == 32000 &&
              limited.getPixelAt(available.x, available.y + 12) != look::display &&
              limited.getPixelAt(beyond.x, beyond.y + 12) == look::display,
          "20 kHz axis is fixed while the trace stops at Nyquist on a lower-rate source");
}

void spectrumExtendedScale() {
    using namespace adi::ui;
    using Scale = SharedSpectrumView::SpectrumScale;
    Fixture f;
    f.tap = std::make_shared<ScopeTap>(32000., SharedAnalysis::kTapSeconds);
    Root root; root.setBounds(0, 0, 680, 720); root.setVisible(true); root.addToDesktop(0);
    AnalysisPane pane; pane.setBounds(0, 0, 640, 360);
    SharedSpectrumView sibling; sibling.setBounds(0, 380, 640, 320);
    root.addAndMakeVisible(pane);
    root.addAndMakeVisible(sibling);
    pane.frame(f.broker, 1, 1); sibling.present(f.broker, 1);
    std::array<float, 1024> samples{};
    const auto amplitude = std::pow(10., -30. / 20.);
    // Spread several coherent tones around 15 kHz so the fill remains visible
    // when the last octave is compressed onto a few screen pixels.
    for (int bin = 472; bin <= 488; bin += 2) {
        const auto radiansPerSample = 2. * std::numbers::pi * bin / 1024.;
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] += static_cast<float>(amplitude * std::sin(radiansPerSample * static_cast<double>(i)));
    }
    f.tap->write(samples.data(), samples.data(), static_cast<int>(samples.size()), 0, true);
    f.broker->pump(); pane.frame(f.broker, 1, 1); sibling.frame();
    const auto source = pane.view.snapshot();
    const auto before = f.broker->stats();
    pane.scaleBox.setSelectedId(3, juce::dontSendNotification); pane.scaleBox.onChange();
    const auto extended = softwarePicture(pane.view);
    const auto unchanged = [&](const auto &stats) {
        const auto now = f.broker->stats();
        return now.transforms == stats.transforms && now.publications == stats.publications &&
            now.cursorReads == stats.cursorReads && now.consumers == stats.consumers && now.taps == stats.taps;
    };
    check(pane.view.spectrumScale() == Scale::Extended && source && source == pane.view.snapshot() &&
              source == sibling.snapshot() && f.acquired == 1 && unchanged(before),
          "Extended projects the same visible immutable result while Wide shares its one analysis");

    const float width = static_cast<float>(extended.getWidth()) - 10.f - 18.f - look::fontCaption * 3.f;
    const float height = static_cast<float>(extended.getHeight()) - 10.f - 16.f - 29.f - look::fontCaption;
    const auto x = [width](double hz) {
        return juce::roundToInt(5.f + (width - 1.f) * static_cast<float>(std::log(hz / 10.) / std::log(3000.)));
    };
    const auto y = [height](float level) { return juce::roundToInt(21.f - (height - 1.f) * level / 100.f); };
    const auto peak = y(-30.f);
    check(source && source->sampleRate == 32000. && extended.getPixelAt(x(15000.), peak - 12) == look::display &&
              extended.getPixelAt(x(15000.), peak + 12) != look::display &&
              extended.getPixelAt(x(23000.), peak + 12) == look::display,
          "Extended fill uses its 30 kHz projection and calibrated ordinate while stopping at Nyquist");
    bool grid = true;
    for (const float level : {-10.f, -20.f, -30.f})
        grid = grid && extended.getPixelAt(x(37.), y(level)) != look::display &&
            extended.getPixelAt(x(37.), y(level) + 2) == look::display;
    check(grid, "Extended paints 10 dB divisions on the shared plot");

    pane.view.freeze(true);
    const auto held = f.broker->stats();
    const auto frozen = comparisonPicture(pane.view);
    pane.setSpectrumScale(Scale::Detail);
    check(pane.view.snapshot() == source && !pane.view.subscribed() && unchanged(held) &&
              !samePicturePixels(frozen, comparisonPicture(pane.view)),
          "switching a frozen Extended spectrum remaps its display without renewing analysis demand");
    pane.setSpectrumScale(Scale::Extended);
    check(samePicturePixels(frozen, comparisonPicture(pane.view)) && unchanged(held),
          "switching back to Extended restores the same frozen pixels and slope");
    root.setVisible(false);
    const auto hidden = f.broker->stats();
    pane.setSpectrumScale(Scale::Detail); pane.setSpectrumScale(Scale::Extended);
    check(!pane.view.subscribed() && !SharedSpectrumViewTestAccess::hasScaledPlot(pane.view) &&
              hidden.consumers == 0 && unchanged(hidden),
          "hidden Extended keeps only its view-local preference");
}

void spectrumDeviceScale() {
    using namespace adi::ui;
    using Scale = SharedSpectrumView::SpectrumScale;
    Fixture f;
    Root root; root.setBounds(0, 0, 660, 380); root.setVisible(true); root.addToDesktop(0);
    AnalysisPane pane; pane.setBounds(0, 0, 640, 360);
    root.addAndMakeVisible(pane);
    pane.frame(f.broker, 1, 1);
    f.write(); f.broker->pump(); pane.frame(f.broker, 1, 1);
    const auto source = pane.view.snapshot();
    const auto before = f.broker->stats();
    pane.scaleBox.setSelectedId(4, juce::dontSendNotification); pane.scaleBox.onChange();
    const auto device = softwarePicture(pane.view);
    const auto after = f.broker->stats();
    check(pane.view.spectrumScale() == Scale::Device && source && pane.view.snapshot() == source &&
              before.consumers == after.consumers && before.taps == after.taps && f.acquired == 1 &&
              before.transforms == after.transforms && before.publications == after.publications,
          "Device uses the selected scale with the same calibrated window, tap and broker work");

    const float left = 5.f + look::fontCaption * 3.f, top = 21.f + look::fontCaption;
    const float width = static_cast<float>(device.getWidth()) - 10.f - 18.f - look::fontCaption * 3.f;
    const float height = static_cast<float>(device.getHeight()) - 10.f - 16.f - 29.f - look::fontCaption;
    const auto x = [=](double hz) {
        return juce::roundToInt(left + (width - 1.f) * static_cast<float>(std::log(hz / 10.) / std::log(2400.)));
    };
    const auto y = [=](float db) { return juce::roundToInt(top - (height - 1.f) * db / 120.f); };
    const auto hasInk = [&](juce::Rectangle<int> area) {
        for (int iy = area.getY(); iy < area.getBottom(); ++iy)
            for (int ix = area.getX(); ix < area.getRight(); ++ix)
                if (device.getPixelAt(ix, iy) != look::display) return true;
        return false;
    };
    const auto peak = y(-6.0206f);
    check(hasInk({x(750.) - 1, peak - 2, 3, 5}) && device.getPixelAt(x(750.), peak + 16) == look::display,
          "Device projects the 750 Hz calibrated peak onto the 120 dB log view with no fill");
    check(hasInk({6, y(-12.f) - 4, 24, 9}) && hasInk({x(1000.) - 10, 22, 20, 9}),
          "Device paints level labels on the left and decade labels along the top");
    bool grid = true;
    for (const float level : {-12.f, -24.f, -36.f})
        grid = grid && device.getPixelAt(x(37.), y(level)) != look::display &&
            device.getPixelAt(x(37.), y(level) + 2) == look::display;
    check(grid, "Device uses 12 dB divisions and preserves the calibrated range");

    pane.setMode(SharedSpectrumView::Mode::SpectrumAverage);
    writeFrames(f, 1024); f.broker->pump(); pane.frame(f.broker, 1, 1);
    const auto measured = pane.view.snapshot();
    const auto *bins = pane.view.spectrumBins().data();
    pane.setSpectrumScale(Scale::Extended); pane.setSpectrumScale(Scale::Device);
    check(pane.scaleBox.getSelectedId() == 4 && measured && bins && measured == pane.view.snapshot() &&
              bins == pane.view.spectrumBins().data() && SharedSpectrumViewTestAccess::hasScaledPlot(pane.view),
          "Device projection also leaves the existing averaged publication intact");
}

void spectrogramHistoryMotion() {
    using namespace adi::ui;
    using Mode = SharedSpectrumView::Mode;
    using Motion = SharedSpectrumView::HistoryMotion;
    constexpr int width = 128, height = 256, left = 5, top = 21;
    // One raster pixel per logical pixel. The only paint addition is the cursor
    // at the next write edge, so exclude that column from the color comparison.
    const auto isRotated = [](const juce::Image &scroll, const juce::Image &sweep, int head) {
        const auto rotation = head + 1;
        const auto cursor = std::min(width - 1, rotation);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (x == cursor) continue;
                const int input = (x + width - rotation) % width;
                if (sweep.getPixelAt(left + x, top + y) != scroll.getPixelAt(left + input, top + y)) return false;
            }
        }
        return true;
    };
    const auto sameWork = [](const auto &before, const auto &after) {
        return before.transforms == after.transforms && before.analysisWindows == after.analysisWindows &&
            before.publications == after.publications && before.cursorReads == after.cursorReads &&
            before.workerPasses == after.workerPasses && before.consumers == after.consumers &&
            before.domains == after.domains && before.taps == after.taps;
    };

    AnalysisPane controls;
    check(!controls.historyBox.isVisible() && controls.view.historyMotion() == Motion::Scroll,
          "other analyser modes keep the history motion control hidden");
    controls.setMode(Mode::Spectrogram);
    check(controls.historyBox.isVisible() && controls.historyBox.getSelectedId() == 1,
          "Spectrogram exposes Scroll as the initial presentation");
    controls.historyBox.setSelectedId(2, juce::dontSendNotification); controls.historyBox.onChange();
    controls.setMode(Mode::Spectrum); controls.setMode(Mode::Spectrogram);
    check(controls.historyBox.getSelectedId() == 2 && controls.view.historyMotion() == Motion::Sweep,
          "the Spectrogram history menu remembers Sweep when switching pages");

    Fixture f;
    Root root; root.setSize(340, 340); root.setVisible(true); root.addToDesktop(0);
    SharedSpectrumView view, sibling, boxed{true};
    view.setBounds(0, 0, 156, 311); sibling.setBounds(170, 0, 156, 311);
    boxed.setBounds(0, 0, width, height);
    for (auto *consumer : {&view, &sibling, &boxed}) {
        consumer->setMode(Mode::Spectrogram);
        root.addAndMakeVisible(*consumer);
        consumer->present(f.broker, 1);
    }
    f.write(); f.broker->pump(); view.frame(); sibling.frame(); boxed.frame();
    const auto standard = softwarePicture(view), docked = softwarePicture(boxed);
    bool sameRaster = boxed.snapshot() && boxed.snapshot() == view.snapshot();
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            sameRaster = sameRaster && docked.getPixelAt(x, y) == standard.getPixelAt(x + left, y + top);
    check(sameRaster, "SC-462: the boxed spectrogram paints the entire raster flush with the display edges");
    boxed.setVisible(false);
    const auto initial = view.snapshot();
    const auto stat = f.broker->stats();
    view.setHistoryMotion(Motion::Sweep);
    const auto first = comparisonPicture(view), scroll = comparisonPicture(sibling);
    check(initial && !initial->columns.empty() && initial->historySerial == initial->columns.front().serial &&
              initial == view.snapshot() && initial == sibling.snapshot() && f.acquired == 1 && sameWork(stat, f.broker->stats()),
          "Scroll and Sweep display the same first immutable history and single compatible analysis");
    check(initial && isRotated(scroll.raster, first.raster, 0) && !samePicturePixels(scroll, first) &&
              first.raster.getPixelAt(left + 1, top + height / 2) != look::display,
          "first published column moves to the left with a visible adjacent cursor");

    const auto repaints = root.fake->repaints;
    view.setHistoryMotion(Motion::Sweep); view.frame();
    check(root.fake->repaints == repaints && sameWork(stat, f.broker->stats()) &&
              samePicturePixels(first, comparisonPicture(view)),
          "stationary publication leaves the sweep and cursor stationary without invalidation");

    const auto origin = initial ? initial->historySerial : 1;
    bool wrapped = false;
    for (int block = 1; block <= 40; ++block) {
        f.write(block * 1024); f.broker->pump(); view.frame(); sibling.frame();
        const auto next = view.snapshot();
        if (!next || next->historySerial < origin + width) continue;
        const auto head = static_cast<int>((next->historySerial - origin) % width);
        wrapped = next == sibling.snapshot() && isRotated(softwarePicture(sibling), softwarePicture(view), head);
        break;
    }
    check(wrapped, "wrapping the sweep preserves both visible runs of the scrolling raster");

    view.freeze(true);
    const auto held = view.snapshot();
    const auto frozenSweep = comparisonPicture(view);
    const auto heldStats = f.broker->stats();
    view.setHistoryMotion(Motion::Scroll); view.setHistoryMotion(Motion::Sweep); view.frame();
    check(held && view.snapshot() == held && !view.subscribed() &&
              samePicturePixels(frozenSweep, comparisonPicture(view)) && sameWork(heldStats, f.broker->stats()),
          "switching history motion on a frozen display preserves its picture and released lease");

    view.freeze(false);
    writeFrames(f, 1024); f.broker->pump(); view.frame(); sibling.frame();
    const auto clearAt = view.snapshot() ? view.snapshot()->historySerial : 0;
    view.clear();
    writeFrames(f, 1024); f.broker->pump(); view.frame(); sibling.frame();
    const auto fresh = view.snapshot();
    const auto clearedSweep = softwarePicture(view);
    view.setHistoryMotion(Motion::Scroll);
    const auto clearedScroll = comparisonPicture(view);
    const auto head = fresh && fresh->historySerial > clearAt
        ? static_cast<int>((fresh->historySerial - clearAt - 1) % width) : width - 1;
    check(fresh && clearAt && head < width - 3 && isRotated(clearedScroll.raster, clearedSweep, head) &&
              clearedSweep.getPixelAt(left + head + 2, top + height / 2) == look::display &&
              sibling.subscribed(),
          "a local history reset starts a new sweep without clearing sibling data or retaining old pixels");
    check(fresh && fresh->columns.size() == width &&
              fresh->columns[width / 2].serial <= clearAt && fresh->columns.back().serial > clearAt,
          "the shared publication retains a full history spanning the local clear boundary");
    const auto clearedRepaints = root.fake->repaints;
    const auto clearedStats = f.broker->stats();
    for (int frame = 0; frame < 8; ++frame) view.frame();
    check(fresh && view.snapshot() == fresh && sibling.snapshot() == fresh &&
              root.fake->repaints == clearedRepaints && sameWork(clearedStats, f.broker->stats()) &&
              samePicturePixels(clearedScroll, comparisonPicture(view)),
          "unchanged post-clear frames retain their raster without invalidation or analysis work");

    root.setVisible(false);
    const auto hidden = f.broker->stats();
    view.setHistoryMotion(Motion::Sweep); view.frame(); f.broker->pump();
    check(hidden.consumers == 0 && !view.subscribed() && !sibling.subscribed() &&
              !SharedSpectrumViewTestAccess::hasRaster(view) && sameWork(hidden, f.broker->stats()) &&
              f.acquired == 1 && f.released == 1,
          "hiding the last observers releases their raster and acquisition and hidden settings do no analysis work");
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
              pages.pageBox.getText() == "Classic Scope" && pane.modeBox.getText() == "Classic Scope" &&
              face.getTitle() == "Classic Scope" && !pane.channelBox.isVisible() && !pane.freezeButton.isVisible(),
          "the full and detached Scope views use the original controls");
    check(pages.detail.view.getTitle() == "Classic Scope" && pane.view.getTitle() == "Classic Scope" &&
              pane.view.getTooltip().startsWith("Classic Scope"),
          "the full and detached scope wrapper expose Classic Scope to accessibility and tooltips");
    pane.view.setMode(SharedSpectrumView::Mode::Spectrum);
    check(pane.view.getTitle() == "Channel spectrum" && !pane.view.getTooltip().startsWith("Classic Scope"),
          "switching back restores the spectrum's accessibility description");
    pane.view.setMode(SharedSpectrumView::Mode::ClassicScope);
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

void classicScopeUnchangedTrace() {
    using namespace adi::ui;
    Fixture f;
    Root root;
    ClassicScopePanel face;
    root.setSize(825, 300);
    face.setBounds(root.getLocalBounds());
    root.addAndMakeVisible(face);
    root.setVisible(true);
    root.addToDesktop(0);
    ClassicScopeControls controls;
    controls.time = .3f;
    const auto lease = f.broker->acquireCapture({{1, ScopePoint::PostFader}, SharedAnalysis::Channel::Left, controls.settings()});
    std::array<float, 1024> monitored;
    monitored.fill(.5f);
    const auto feed = [&](std::int32_t frames, std::uint64_t revision, bool monitoring = false) {
        const auto *lane = monitoring ? monitored.data() : nullptr;
        f.tap->write(lane, lane, frames, 100, false, revision);
        f.broker->pump();
        const auto next = lease.read();
        face.present(controls, false, next, 0);
        return next;
    };

    const auto first = feed(1024, 5);
    const auto progressivePaints = root.fake->repaints;
    const auto second = feed(1024, 5);
    const auto quiet = f.tap->lastParkedSilence();
    check(first && second && first->currentSerial != second->currentSerial && quiet.begin == 0 && quiet.end == 2048 &&
              root.fake->repaints == progressivePaints,
          "progressive Classic keeps consuming parked sweeps without repainting the same flat trace");

    controls.sync = true;
    face.present(controls, false, second, 0);
    const auto syncPaints = root.fake->repaints;
    const auto third = feed(1024, 5);
    check(third && second && third->previousSerial != second->previousSerial && root.fake->repaints == syncPaints,
          "Sync also retains the same painted trace when a new completed sweep has identical extrema");

    const auto live = feed(24, 5, true);
    check(third && live && live->current.size() > third->current.size() &&
              live->current.back().second == .5f && live->previous == third->previous &&
              root.fake->repaints == syncPaints,
          "a stopped monitored input advances the live sweep while Sync displays the unchanged completed sweep");

    controls.sync = false;
    face.present(controls, false, live, 0);
    const auto livePaints = root.fake->repaints;
    juce::Image raster(juce::Image::RGB, 825, 300, true, juce::SoftwareImageType{});
    {
        juce::Graphics g(raster);
        face.paintEntireComponent(g, true);
    }
    const bool showsMonitor = live && live->current.size() >= 2 &&
        raster.getPixelAt(38 + static_cast<int>(live->current.size()) - 2, 16 + 67) == juce::Colour(118, 118, 118);
    check(livePaints > syncPaints && showsMonitor, "returning to progressive redraw shows the retained live sweep");
    const auto afterSeek = feed(10, 6);
    check(live && afterSeek && afterSeek->sourceEpoch > live->sourceEpoch && afterSeek->transportRevision == 6 &&
              afterSeek->previous.empty() && root.fake->repaints > livePaints,
          "a parked seek still clears the old trace and paints the new sweep");
    root.removeFromDesktop();
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
    check(pages.page() == Page::All && pages.pageBox.getNumItems() == static_cast<int>(Page::ColourWaveform) + 1 &&
              pages.sourceBox.getNumItems() == 4 && pages.sourceBox.getSelectedId() == 1,
          "analysers default to All and Main and offer each non-Main channel once");
    check(all.taps == 3 && all.domains == 4 && all.consumers == 8 && all.spectralPlans == 2,
          "All shares the Main tap across two FFT sizes and leases readout rows without their own FFT");
    f.write();
    check(f.broker->pump() == 1, "All can publish a 1024-frame spectrogram while the filled spectrum waits for its block");
    pages.frame(f.broker, reader, main->id, first->id);
    check(loudnessReads > 0, "All reads Main loudness through the persistent measurement");

    for (const auto &[page, mode] : std::array{
         std::pair{Page::Spectrogram, SharedSpectrumView::Mode::Spectrogram},
         std::pair{Page::Spectrum, SharedSpectrumView::Mode::FilledSpectrum},
         std::pair{Page::Scope, SharedSpectrumView::Mode::ClassicScope},
         std::pair{Page::Phase, SharedSpectrumView::Mode::Phase},
         std::pair{Page::ThirdOctaveBands, SharedSpectrumView::Mode::ThirdOctaveBands},
         std::pair{Page::ColourWaveform, SharedSpectrumView::Mode::ColourWaveform},
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
    check(dynamic_cast<UiFakePeer *>(root.getPeer()) && broker->stats().consumers == 0 &&
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
    const auto displayShowsBothTraces = [&](int scaleId) {
        auto *compare = pages.compareA.getParentComponent();
        if (!compare || !pair.a.metadata || !pair.b.metadata) return false;
        juce::Image image(juce::Image::RGB, compare->getWidth(), compare->getHeight(), true, juce::SoftwareImageType{});
        {
            juce::Graphics graphics(image);
            compare->paint(graphics);
        }
        auto plot = compare->getLocalBounds().reduced(look::border).withTrimmedTop(look::fieldHeight)
            .reduced(look::gap).withTrimmedTop(look::fieldHeight).withTrimmedBottom(look::fieldHeight).toFloat();
        struct Axis { double lowHz, highHz; float upper, lower; };
        constexpr std::array axes{Axis{20., 20000., 0.f, -96.f}, Axis{20., 20000., -18.f, -78.f},
                                  Axis{10., 30000., 0.f, -100.f}, Axis{10., 24000., 0.f, -120.f}};
        const auto &axis = axes[static_cast<std::size_t>(scaleId - 1)];
        const float rulerWidth = look::fontCaption * 3.f, rulerHeight = look::fontCaption;
        const bool wide = scaleId == 1;
        if (scaleId == 4) plot = plot.withTrimmedLeft(rulerWidth).withTrimmedTop(rulerHeight);
        else if (!wide) plot = plot.withTrimmedRight(rulerWidth).withTrimmedBottom(rulerHeight);
        const float inset = wide ? 0.f : 1.f;
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
                const auto hz = bin * 48000. / 1024.;
                return plot.getX() + (plot.getWidth() - inset) *
                    static_cast<float>(std::log(hz / axis.lowHz) / std::log(axis.highHz / axis.lowHz));
            };
            const auto y = [&](std::size_t bin) {
                const auto p = std::clamp((axis.upper - bins[bin]) / (axis.upper - axis.lower), 0.f, 1.f);
                return plot.getY() + (plot.getHeight() - inset) * p;
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
    check(pages.compareScale.isShowing() && pages.compareScale.getSelectedId() == 1 && displayShowsBothTraces(1),
          "Wide is the initial Compare projection and both paths draw adjacent measured bins");
    const auto unchanged = [](const auto &before, const auto &after) {
        return before.consumers == after.consumers && before.taps == after.taps && before.domains == after.domains &&
            before.spectralPlans == after.spectralPlans && before.transforms == after.transforms &&
            before.publications == after.publications && before.cursorReads == after.cursorReads &&
            before.frameRequests == after.frameRequests && before.workerPasses == after.workerPasses;
    };
    const auto visible = broker->stats();
    pages.setTopLeftPosition(root.getWidth() / 2, 0);
    check(pages.comparisonView().visible && unchanged(visible, broker->stats()),
          "a partly clipped Compare ancestor keeps its two existing sources");
    pages.setTopLeftPosition(root.getWidth(), 0);
    check(pages.compareA.isShowing() && !pages.comparisonView().visible &&
              broker->stats().consumers == 0 && broker->stats().taps == 0 && broker->stats().spectralPlans == 0,
          "moving a still-visible ancestor completely offscreen releases both Compare sources immediately");
    const auto clipped = broker->stats();
    write(*first, .2f, 1024, 1); write(*second, .15f, 1024, 1);
    pages.frame(broker, original, main, guitar); broker->pump();
    check(unchanged(clipped, broker->stats()), "audio written while Compare is clipped creates no analysis work");
    pages.setTopLeftPosition(0, 0);
    pages.frame(broker, original, main, guitar); broker->pump();
    pages.frame(broker, original, main, guitar);
    pair = pages.comparisonView();
    check(pair.visible && !pair.a.metadata && !pair.b.metadata && broker->stats().consumers == 2,
          "both restored Compare sources fence audio written while the plot was clipped");
    write(*first, .64f, 2048, 1); write(*second, .32f, 2048, 1); broker->pump();
    pages.frame(broker, original, main, guitar);
    pair = pages.comparisonView();
    check(pair.status == Comparison::Independent && pair.a.metadata && pair.b.metadata &&
              pair.a.metadata->firstSequence == 2048 && pair.b.metadata->firstSequence == 2048,
          "both restored Compare traces use the first fresh publication");

    write(*first, .4f, 3072, 2); broker->pump();
    pages.frame(broker, original, main, main);
    check(pages.comparisonView().status == Comparison::Unavailable,
          "a live overlay waits when the two tap publications disagree on the audio transport revision");
    write(*second, .2f, 3072, 2); broker->pump();
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
    write(*first, .1f, 4096, 3); broker->pump();
    pages.frame(broker, renamed, main, main);
    pair = pages.comparisonView();
    check(pair.status == Comparison::Held && pair.b.binsDbfs[16] == reference &&
              pair.a.metadata->transportRevision == 3,
          "a deliberately held reference survives a new live transport revision");
    const auto immutableA = pair.a.binsDbfs.data(), immutableB = pair.b.binsDbfs.data();
    const auto beforeScale = broker->stats();
    for (const int id : {2, 3, 4, 1}) {
        pages.compareScale.setSelectedId(id, juce::sendNotificationSync);
        pair = pages.comparisonView();
        check(pair.status == Comparison::Held && pair.a.binsDbfs.data() == immutableA &&
                  pair.b.binsDbfs.data() == immutableB && unchanged(beforeScale, broker->stats()) &&
                  pages.detail.scaleBox.getSelectedId() == 1 && displayShowsBothTraces(id),
              "Compare scales redraw both calibrated traces without changing the held bins, tap or Spectrum preference");
    }
    root.setVisible(false);
    check(broker->stats().consumers == 0 && !pages.comparisonView().visible && pages.comparisonView().referenceHeld,
          "ancestor hide releases live Compare demand without a frame and retains the deliberate B reference");
    const auto hidden = broker->stats();
    pages.compareScale.setSelectedId(2, juce::sendNotificationSync);
    broker->pump();
    check(!pages.compareScale.isShowing() && pages.comparisonView().referenceHeld && unchanged(hidden, broker->stats()),
          "changing the hidden Compare scale retains Hold B without new work");
    root.setVisible(true);
    pages.frame(broker, renamed, main, main);
    check(!pages.comparisonView().a.metadata && pages.comparisonView().referenceHeld &&
              pages.compareScale.getSelectedId() == 2,
          "showing Compare waits for a fresh A window and retains its local scale");
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
    const auto firstStamp = first->written();
    for (int block = 0; block < warmBlocks; ++block) {
        write(*first, .8f, firstStamp + block * 1024, 4); broker->pump();
        pages.frame(broker, original, main, guitar);
    }
    write(*first, .1f, firstStamp + warmBlocks * 1024, 4); broker->pump();
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

    using Mode = SharedSpectrumView::Mode;
    pages.setPage(Page::Spectrum);
    using Curve = SharedAnalysis::FilledCurve;
    const auto traceId = [](Curve curve) { return static_cast<int>(curve) + 1; };
    pages.frame(broker, original, main, guitar);
    SharedAnalysis::Request filledRequest{main};
    filledRequest.fftSize = 4096;
    filledRequest.outputs = SharedAnalysis::FilledSpectrum;
    auto sibling = broker->acquire(filledRequest);
    const auto peakStart = first->written();
    for (int block = 0; block < 4; ++block) write(*first, .8f, peakStart + block * 1024, 4);
    broker->pump();
    pages.frame(broker, original, main, guitar);
    const auto picture = pages.detail.view.snapshot();
    const auto siblingPeak = sibling.read();
    const auto beforeTrace = broker->stats();
    check(pages.page() == Page::Spectrum && pages.pageBox.getText() == "Spectrum" &&
              pages.spectrumTraceBox.getSelectedId() == traceId(Curve::RealtimeMaximum) &&
              pages.spectrumTraceBox.isShowing() && pages.spectrumTraceBox.getNumItems() == 4 &&
              pages.detail.view.mode() == Mode::FilledSpectrum && pages.detail.view.getTitle() == "Filled Spectrum" &&
              !pages.detail.scaleBox.isShowing(),
          "Spectrum defaults to Realtime Maximum and offers four traces on its fixed grid");
    const auto hasAllCurves = [](const auto &publication) {
        return publication && publication->filledSpectrum && publication->request.fftSize == 4096 &&
            std::ranges::all_of(publication->filledSpectrum->magnitudes, [](const auto &curve) { return curve.size() == 2049; });
    };
    check(hasAllCurves(picture) && hasAllCurves(siblingPeak) && beforeTrace.taps == 1 && beforeTrace.spectralPlans == 1,
          "every display curve is published while both Filled views share the tap and FFT");
    for (auto curve : {Curve::RealtimeAverage, Curve::Average, Curve::HeldMaximum}) {
        pages.spectrumTraceBox.setSelectedId(traceId(curve), juce::sendNotificationSync);
        pages.frame(broker, original, main, guitar);
        const auto afterTrace = broker->stats();
        check(pages.detail.view.filledSpectrumCurve() == curve && pages.detail.view.snapshot() == picture &&
                  afterTrace.transforms == beforeTrace.transforms && afterTrace.consumers == beforeTrace.consumers,
              "switching Filled curves keeps the lease, complete picture and transform count");
    }
    pages.detail.resetButton.onClick();
    pages.frame(broker, original, main, guitar);
    check(!pages.detail.view.snapshot() && sibling.read() == siblingPeak,
          "Clear immediately removes only its own filled spectrum picture");
    for (int block = 0; block < 4; ++block) write(*first, .1f, peakStart + (4 + block) * 1024, 4);
    broker->pump();
    pages.frame(broker, original, main, guitar);
    const auto cleared = pages.detail.view.snapshot();
    const auto siblingAfter = sibling.read();
    const auto held = static_cast<std::size_t>(Curve::HeldMaximum);
    check(hasAllCurves(cleared) && hasAllCurves(siblingAfter) &&
              cleared->filledSpectrum->magnitudes[held][64] < .11f &&
              siblingAfter->filledSpectrum->magnitudes[held][64] > .75f,
          "a complete post-Clear window replaces the old held maximum only for the cleared view");
    pages.setPage(Page::Analysis);
    check(!pages.spectrumTraceBox.isShowing() && broker->stats().consumers == 1,
          "leaving Spectrum hides the trace selector and releases only its lease");
    pages.setPage(Page::Spectrum);
    pages.frame(broker, original, main, guitar);
    check(pages.detail.view.filledSpectrumCurve() == Curve::HeldMaximum && broker->stats().consumers == 2,
          "returning to Spectrum restores the chosen trace");
    pages.setVisible(false);
    check(broker->stats().consumers == 1 && sibling.read() == siblingAfter,
          "hiding Spectrum leaves the sibling picture and lease intact");
    sibling.reset();
    check(broker->stats().consumers == 0 && broker->stats().spectralPlans == 0,
          "hiding the final Filled view releases its FFT demand");

    const auto cumulativeId = static_cast<int>(Mode::SpectrumCumulativeMaximum) + 1;
    AnalysisPane detached;
    detached.setMode(Mode::FilledSpectrum);
    check(detached.modeBox.getText() == "Filled Spectrum", "the detached picker names the Filled Spectrum mode");
    detached.modeBox.setSelectedId(static_cast<int>(Mode::SpectrumMaximum) + 1, juce::sendNotificationSync);
    check(detached.modeBox.getText() == "Realtime Maximum" && detached.view.mode() == Mode::SpectrumMaximum,
          "the detached analyser also exposes Realtime Maximum");
    detached.modeBox.setSelectedId(cumulativeId, juce::sendNotificationSync);
    check(detached.modeBox.getText() == "Cumulative Maximum" && detached.view.getTitle() == "Cumulative Maximum",
          "the detached analyser also exposes the cumulative spectrum trace");
    root.removeFromDesktop();
}

void compareHoldSurvivesInvalidA() {
    using namespace adi::ui;
    constexpr std::int64_t primary = 11, reference = 12;
    auto project = std::make_shared<adi::engine::Snapshot>();
    for (const auto id : {primary, reference}) {
        auto track = std::make_shared<adi::engine::TrackNode>();
        track->id = id;
        track->name = id == primary ? "Programme" : "Reference";
        track->kind = "audio";
        project->tracks.push_back(std::move(track));
    }
    const SnapshotReader snapshot{project};
    auto a = std::make_shared<ScopeTap>(48000., 1.);
    auto b = std::make_shared<ScopeTap>(48000., 1.);
    auto broker = std::make_shared<SharedAnalysis>(
        [&](std::int64_t track, ScopePoint) { return track == primary ? a : b; },
        [](std::int64_t, ScopePoint) {}, SharedAnalysis::Mode::Manual);
    const auto write = [](ScopeTap &tap, float value, std::int64_t stamp) {
        std::array<float, 1024> samples;
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = value * static_cast<float>(std::sin(2. * std::numbers::pi * static_cast<double>(i) / 64.));
        tap.write(samples.data(), samples.data(), static_cast<std::int32_t>(samples.size()), stamp, true, 1);
    };
    Root root;
    MixAnalysers pages;
    root.setSize(640, 360); root.setVisible(true); root.addToDesktop(0);
    root.addAndMakeVisible(pages); pages.setBounds(root.getLocalBounds());
    pages.setPage(MixAnalysers::Page::Compare);
    const auto frame = [&] { pages.frame(broker, snapshot, primary, primary); };
    frame();
    const auto aMenu = pages.compareA.getItemId(0), bMenu = pages.compareB.getItemId(1);
    const auto selectA = [&](int menu) { pages.compareA.setSelectedId(menu, juce::sendNotificationSync); };
    selectA(aMenu);
    pages.compareB.setSelectedId(bMenu, juce::sendNotificationSync);
    write(*a, .3f, 0); write(*b, .6f, 0); broker->pump(); frame();
    pages.holdB.onClick();
    auto pair = pages.comparisonView();
    const std::vector<float> captured(pair.b.binsDbfs.begin(), pair.b.binsDbfs.end());
    const auto capturedWindow = pair.b.metadata ? pair.b.metadata->windowSerial : 0;
    check(pair.referenceHeld && capturedWindow && broker->stats().consumers == 1,
          "the invalid-A test starts with a captured B and only A consuming analysis");

    selectA(bMenu);
    const auto equal = broker->stats();
    pair = pages.comparisonView();
    check(pair.referenceHeld && !pair.visible && !pair.a.metadata && !pair.b.metadata &&
              equal.consumers == 0 && equal.taps == 0,
          "choosing B in the A picker leaves the reference held and releases the former A tap");
    selectA(0);
    write(*a, .1f, 1024); write(*b, .2f, 1024); frame(); broker->pump();
    const auto idle = broker->stats();
    check(pages.comparisonView().referenceHeld && idle.consumers == 0 && idle.taps == 0 &&
              idle.cursorReads == equal.cursorReads && idle.transforms == equal.transforms &&
              idle.publications == equal.publications && idle.frameRequests == equal.frameRequests,
          "an unavailable A keeps the reference without reading buffered samples or scheduling analysis");

    selectA(aMenu);
    broker->pump(); frame();
    pair = pages.comparisonView();
    check(pair.referenceHeld && !pair.a.metadata && pair.b.metadata &&
              pair.b.metadata->windowSerial == capturedWindow && broker->stats().consumers == 1 &&
              broker->stats().taps == 1,
          "choosing A again reacquires only A and fences its older buffered audio");
    write(*a, .4f, 2048); broker->pump(); frame();
    pair = pages.comparisonView();
    check(pair.status == AnalysisComparison::Status::Held && pair.a.metadata && pair.b.metadata &&
              pair.a.metadata->firstSequence == 2048 && pair.b.metadata->windowSerial == capturedWindow &&
              std::ranges::equal(pair.b.binsDbfs, captured),
          "a fresh A pairs with the original B, including every immutable reference bin");

    pages.compareB.setSelectedId(aMenu, juce::sendNotificationSync);
    check(!pages.comparisonView().referenceHeld && broker->stats().consumers == 0,
          "changing B explicitly discards its hold even if the new pair is invalid");
    root.removeFromDesktop();
}

} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI initialiser;
    try { sharingAndVisibility(); sourceAndOwnerLifetime(); parentDestruction(); phaseGoniometer(); modesSourcesAndFreeze(); pendingSourceLease(); cappedFrozenHistoryAndIdleReset();
          mainReadoutFormatProjection(); mainMetersSharedPresentation(); permanentMainMeter(); referenceMainMeterScale();
          linkedMainMeter(); parkedMainLoudnessProjection();
          compactAnalyzerDock(); wordsMaskedInComparison(); paletteMenuTheme(); palettesAndFrozenRaster(); restartedRasterDomain();
          clearSpectrogramWhileWorkerAhead(); freshActionsWithSharedConsumer(); backlogAndLifecycleFreshness(); spectrumSlopeProjection(); filledSpectrumProjection(); thirdOctaveAndPeriodSurface(); colourWaveformSurface(); spectrumDetailScale(); spectrumExtendedScale();
          spectrumDeviceScale(); spectrogramHistoryMotion(); classicScopeSurface();
          classicScopeUnchangedTrace(); mixAnalyserPages(); compareAndAveragePages(); compareHoldSurvivesInvalidA(); }
    catch (const std::exception &e) { ++failures; std::printf("FAIL exception: %s\n", e.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
