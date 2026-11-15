// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../src/juce/ui_analysis.hpp"
#include "ui_fake_peer.hpp"
#include <cmath>
#include <cstdio>
#include <memory>
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
} // namespace
int main() {
    juce::ScopedJuceInitialiser_GUI initialiser;
    try { sharingAndVisibility(); sourceAndOwnerLifetime(); parentDestruction(); modesSourcesAndFreeze(); pendingSourceLease(); }
    catch (const std::exception &e) { ++failures; std::printf("FAIL exception: %s\n", e.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
