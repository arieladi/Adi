// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mixview.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include <algorithm>
namespace adi::ui {
void FloatButton::paintButton(juce::Graphics &g, bool over, bool) {
    const auto r = getLocalBounds().toFloat();
    look::box(g, r, getToggleState() ? look::accent : over ? look::chooser : look::button);
    // Two boxes, the front one offset down and right: a window over the window.
    g.setColour(look::text);
    const float s = std::min(r.getWidth(), r.getHeight());
    const juce::Rectangle<float> back(r.getX() + s * .25f, r.getY() + s * .22f, s * .42f, s * .34f);
    g.drawRect(back, 1.f);
    const auto front = back.translated(s * .14f, s * .2f);
    g.setColour(getToggleState() ? look::accent : look::button);
    g.fillRect(front);
    g.setColour(look::text);
    g.drawRect(front, 1.f);
}
void ReferenceButton::paintButton(juce::Graphics &g, bool over, bool) {
    const auto r = getLocalBounds().toFloat();
    look::box(g, r, getToggleState() ? look::accent : over ? look::chooser : look::button);
    g.setColour(look::text);
    g.setFont(look::font(11.5f, true));
    g.drawText("REF", r, juce::Justification::centred);
}
void ReferenceButton::mouseDown(const juce::MouseEvent &e) {
    juce::Button::mouseDown(e);
    root_.referenceKey(true, static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
    setToggleState(root_.reference().on(), juce::dontSendNotification);
}
void ReferenceButton::mouseUp(const juce::MouseEvent &e) {
    juce::Button::mouseUp(e);
    root_.referenceKey(false, static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
    setToggleState(root_.reference().on(), juce::dontSendNotification);
}
void EyeButton::paintButton(juce::Graphics &g, bool over, bool) {
    const auto r = getLocalBounds().toFloat();
    look::box(g, r, getToggleState() ? look::accent : over ? look::chooser : look::button);
    // The eye: an almond and its pupil.
    const juce::Rectangle<float> eye(r.getX() + 5, r.getCentreY() - 3.5f, 14, 7);
    juce::Path almond;
    almond.startNewSubPath(eye.getX(), eye.getCentreY());
    almond.quadraticTo(eye.getCentreX(), eye.getY() - 3, eye.getRight(), eye.getCentreY());
    almond.quadraticTo(eye.getCentreX(), eye.getBottom() + 3, eye.getX(), eye.getCentreY());
    g.setColour(look::text);
    g.strokePath(almond, juce::PathStrokeType(1.f));
    g.fillEllipse(eye.getCentreX() - 2, eye.getCentreY() - 2, 4, 4);
    g.setFont(look::font(11.5f));
    g.drawText("Reference Track", r.withTrimmedLeft(24), juce::Justification::centredLeft);
}
MixViewPanel::MixViewPanel(AdiRootComponent &root) : ref(root), stack(root), root_(root) {
    setTitle("Mix View");
    setLookAndFeel(&look::liveLook());
    for (int i = 0; i < kTabCount; ++i) {
        auto button = std::make_unique<juce::TextButton>(kTabNames[static_cast<std::size_t>(i)]);
        button->setTitle(kTabNames[static_cast<std::size_t>(i)]);
        button->setClickingTogglesState(false);
        button->setColour(juce::TextButton::buttonColourId, look::button);
        button->setColour(juce::TextButton::buttonOnColourId, look::accent);
        button->setColour(juce::TextButton::textColourOffId, look::text);
        button->setColour(juce::TextButton::textColourOnId, look::text);
        button->setMouseClickGrabsKeyboardFocus(false);
        button->onClick = [this, i] { root_.chooseMixViewTab(i); };
        addAndMakeVisible(*button);
        tabs[static_cast<std::size_t>(i)] = std::move(button);
    }
    floating.setTooltip("Mix View in a Window. Moves the Mix View to a window of its own, onto any display; "
                        "again, back to the centre.");
    floating.setMouseClickGrabsKeyboardFocus(false);
    floating.onClick = [this] { root_.command(AppCommands::DetachMixView); };
    addAndMakeVisible(floating);
    ref.setTooltip("REF. Plays the Reference track alone, the Main bypassed: hold to audition, tap to latch "
                   "(Shift+R).");
    ref.setMouseClickGrabsKeyboardFocus(false);
    addAndMakeVisible(ref);
    eye.setTooltip("Reference Track. Shows the Reference track in the arrangement; it is hidden by default.");
    eye.setMouseClickGrabsKeyboardFocus(false);
    eye.onClick = [this] { root_.command(AppCommands::ToggleReferenceTrack); };
    addAndMakeVisible(eye);
    addChildComponent(stack);
    addChildComponent(spectrum);
    addChildComponent(analysis);
    addChildComponent(analyzerDock);
    addAndMakeVisible(analyzerToggle);
    analyzerToggle.setMouseClickGrabsKeyboardFocus(false);
    analyzerToggle.onClick = [this] { setAnalyzerDock(analyzerToggle.getToggleState()); };
    analyzerDock.onChange = [this] { persistAnalyzerSettings(); };
    analyzerDock.onExpand = [this](std::int64_t track) { if (root_.openAnalyzer) root_.openAnalyzer(track); };
    analysis.onExpand = [this](std::int64_t track) { if (root_.openAnalyzer) root_.openAnalyzer(track); };
    analysis.onPaletteChange = [this] {
        auto &state = root_.state();
        const auto before = state.analysisPalette; const bool wasReversed = state.analysisPaletteReverse;
        state.analysisPalette = analysis.requestedPalette(); state.analysisPaletteReverse = analysis.reverseButton.getToggleState();
        if (!root_.persist()) {
            state.analysisPalette = before; state.analysisPaletteReverse = wasReversed;
            analysis.restorePaletteControls(before, wasReversed); return;
        }
        analysis.setPalette(state.analysisPalette, state.analysisPaletteReverse);
    };
    analysis.onSpectrumSlopeChange = [this] {
        auto &state = root_.state();
        const auto before = state.analysisSpectrumSlope;
        state.analysisSpectrumSlope = spectrumSlopeId(analysis.requestedSpectrumSlope());
        if (!root_.persist()) {
            state.analysisSpectrumSlope = before;
            analysis.restoreSlopeControls(spectrumSlopeFromId(before)); return;
        }
        analysis.setSpectrumSlope(spectrumSlopeFromId(state.analysisSpectrumSlope));
    };
}
MixViewPanel::~MixViewPanel() { setLookAndFeel(nullptr); }
void MixViewPanel::adoptViewState() {
    ownerReady_ = true;
    const auto &state = root_.state();
    analyzerDock.configure(state.mixAnalyzerSource, state.mixAnalyzerPoint, state.mixAnalyzerChannel,
                           state.mixAnalyzerWidth);
    analysis.setPalette(state.analysisPalette, state.analysisPaletteReverse);
    analysis.setSpectrumSlope(spectrumSlopeFromId(state.analysisSpectrumSlope));
    resized();
}
bool MixViewPanel::hasAnalyzerControlFocus() const {
    return AnalysisPane::anyControlsFocused() || analyzerToggle.hasKeyboardFocus(true) ||
        (analyzerDock.isVisible() && analyzerDock.hasKeyboardFocus(true)) ||
        (analysis.isVisible() && analysis.hasKeyboardFocus(true));
}
MixViewPanel::Tab MixViewPanel::tab() const noexcept {
    return static_cast<Tab>(std::clamp(root_.state().mixViewTab, 0, kTabCount - 1));
}
juce::Rectangle<int> MixViewPanel::body() const noexcept {
    return appliedBody_;
}
juce::Rectangle<int> MixViewPanel::strip() const noexcept {
    return appliedStrip_;
}
juce::Rectangle<int> MixViewPanel::timeline() const noexcept {
    const auto s = strip();
    const int left = eye.getRight() + look::gap;
    return {left, s.getY() + 5, std::max(0, s.getRight() - look::gap - left), s.getHeight() - 10};
}
juce::String MixViewPanel::referenceText() const {
    if (const auto id = root_.referenceTrack(); id != 0)
        if (const auto *t = root_.reader().findTrack(id))
            return juce::String::fromUTF8(t->name.c_str());
    return "None yet";
}
void MixViewPanel::frame(const SnapshotReader &reader) {
    const juce::Component::SafePointer<MixViewPanel> alive(this);
    floating.setToggleState(root_.state().mixViewDetached, juce::dontSendNotification);
    if (ref.getToggleState() != root_.reference().on() || eye.getToggleState() != root_.state().showReference ||
        root_.playing()) {
        ref.setToggleState(root_.reference().on(), juce::dontSendNotification);
        eye.setToggleState(root_.state().showReference, juce::dontSendNotification);
        repaint(strip());
    }
    const int shown = static_cast<int>(tab());
    if (shown != shownTab_) {
        shownTab_ = shown;
        for (int i = 0; i < kTabCount; ++i)
            tabs[static_cast<std::size_t>(i)]->setToggleState(i == shown, juce::dontSendNotification);
        stack.setVisible(tab() == Tab::Stack);
        if (!alive) return;
        spectrum.setVisible(tab() == Tab::Overview);
        if (!alive) return;
        analysis.setVisible(tab() == Tab::Analysis || tab() == Tab::ScopePhase || tab() == Tab::Spectrogram);
        if (!alive) return;
        if (tab() == Tab::ScopePhase) analysis.setMode(SharedSpectrumView::Mode::Phase);
        if (tab() == Tab::Spectrogram) analysis.setMode(SharedSpectrumView::Mode::Spectrogram);
        layoutPending_ = true;
        repaint();
    }
    if (layoutPending_ || appliedDock_ != root_.state().mixAnalyzerDock ||
        appliedWidth_ != root_.state().mixAnalyzerWidth) resized();
    if (!alive) return;
    // STACK's rows follow the tracks; a frame that changes none of them builds nothing.
    if (stack.isVisible()) {
        stack.frame(reader);
        if (!alive) return;
    }
    spectrum.present(root_.sharedAnalysis, root_.analysisTrack(reader));
    spectrum.frame();
    if (!alive) return;
    analysis.frame(root_.sharedAnalysis, root_.analysisTrack(reader), root_.analysisTrack(reader, true));
    if (!alive) return;
    analyzerDock.frame(root_.sharedAnalysis, root_.analysisTrack(reader), root_.analysisTrack(reader, true));
}
bool MixViewPanel::setAnalyzerDock(bool visible) {
    auto &state = root_.state();
    const bool before = state.mixAnalyzerDock;
    state.mixAnalyzerDock = visible;
    if (!root_.persist()) { state.mixAnalyzerDock = before; analyzerToggle.setToggleState(before, juce::dontSendNotification); return false; }
    resized(); return true;
}
bool MixViewPanel::setAnalyzerWidth(int width) {
    auto &state = root_.state();
    const int before = state.mixAnalyzerWidth;
    state.mixAnalyzerWidth = std::clamp(width, kDockMinimumWidth, 420);
    if (!root_.persist()) { state.mixAnalyzerWidth = before; return false; }
    analyzerDock.configure(state.mixAnalyzerSource, state.mixAnalyzerPoint, state.mixAnalyzerChannel,
                           state.mixAnalyzerWidth);
    resized(); return true;
}
bool MixViewPanel::persistAnalyzerSettings() {
    auto &state = root_.state();
    const auto before = std::array{state.mixAnalyzerSource, state.mixAnalyzerPoint, state.mixAnalyzerChannel,
                                   state.mixAnalyzerWidth};
    state.mixAnalyzerSource = analyzerDock.source(); state.mixAnalyzerPoint = analyzerDock.point();
    state.mixAnalyzerChannel = analyzerDock.channel(); state.mixAnalyzerWidth = analyzerDock.requestedWidth();
    if (!root_.persist()) {
        state.mixAnalyzerSource = before[0]; state.mixAnalyzerPoint = before[1];
        state.mixAnalyzerChannel = before[2]; state.mixAnalyzerWidth = before[3];
        analyzerDock.restoreControls(before[0], before[1], before[2], before[3]); return false;
    }
    analyzerDock.configure(state.mixAnalyzerSource, state.mixAnalyzerPoint, state.mixAnalyzerChannel,
                           state.mixAnalyzerWidth);
    resized(); return true;
}
void MixViewPanel::resized() {
    const juce::Component::SafePointer<MixViewPanel> alive(this);
    if (!ownerReady_) { layoutPending_ = true; return; }
    // Preserve the complete applied child/header/canvas geometry while an edit is
    // held. The next owner frame coalesces requested changes after release.
    if (stack.model().capture()) { layoutPending_ = true; return; }
    layoutPending_ = false;
    const auto &state = root_.state();
    appliedDock_ = state.mixAnalyzerDock; appliedWidth_ = state.mixAnalyzerWidth;
    headerHeight_ = getWidth() >= kHeaderSingleRowWidth ? kHeaderHeight : kHeaderHeight * 2;
    appliedBody_ = getLocalBounds().withTrimmedTop(headerHeight_ + kStripHeight).reduced(look::border);
    appliedStrip_ = {look::border, headerHeight_, std::max(0, getWidth() - 2 * look::border), kStripHeight - 2};
    analyzerToggle.setToggleState(appliedDock_, juce::dontSendNotification);
    // The tabs along the top, from the left, as Live's view choosers sit on its band.
    int x = look::gap;
    const int y = (kHeaderHeight - look::fieldHeight) / 2;
    const int tabsWidth = std::max(0, getWidth() - look::gap * 2 -
        (headerHeight_ == kHeaderHeight ? 120 : 26));
    int remaining = std::min(500, tabsWidth);
    for (int i = 0; i < kTabCount; ++i) {
        const int nominal = kTabWidths[static_cast<std::size_t>(i)];
        const int w = std::min(remaining, std::max(0, nominal * std::min(500, tabsWidth) / 500));
        tabs[static_cast<std::size_t>(i)]->setBounds(x, y, w, look::fieldHeight);
        x += w + 2; remaining = std::max(0, remaining - w - 2);
    }
    floating.setBounds(std::max(0, getWidth() - look::gap - 22), y, std::min(22, getWidth()), look::fieldHeight);
    analyzerToggle.setBounds(headerHeight_ == kHeaderHeight ? std::max(x, floating.getX() - 98) : look::gap,
                             y + (headerHeight_ == kHeaderHeight ? 0 : kHeaderHeight),
                             std::min(94, std::max(0, getWidth() - look::gap * 2)), look::fieldHeight);
    const auto s = strip();
    const int sy = s.getY() + (s.getHeight() - look::fieldHeight) / 2;
    ref.setBounds(s.getX() + 210, sy, 38, look::fieldHeight);
    eye.setBounds(ref.getRight() + 6, sy, 124, look::fieldHeight);
    auto content = body();
    const bool beside = tab() == Tab::Stack || tab() == Tab::Overview;
    const bool fits = content.getWidth() >= kStackMinimumWidth + kDockGap + kDockMinimumWidth &&
                      content.getHeight() >= kDockMinimumHeight;
    analyzerDock.setVisible(appliedDock_ && beside && fits);
    if (!alive) return;
    if (analyzerDock.isVisible()) {
        const int width = std::min(appliedWidth_, content.getWidth() - kStackMinimumWidth - kDockGap);
        analyzerDock.setBounds(content.removeFromRight(width)); content.removeFromRight(kDockGap);
        if (!alive) return;
    }
    analyzerToggle.setTooltip(appliedDock_ && beside && !fits ?
        "Analyzer dock requested. Enlarge this view to retain 320 px for channel tools and 240 by 420 px for analyzers." :
        "Show or hide Meters, Phase and Spectrum beside STACK or OVERVIEW. Analysis remains a separate tab.");
    stack.setBounds(content);
    if (!alive) return;
    spectrum.setBounds(content);
    if (!alive) return;
    analysis.setBounds(body());
}
void MixViewPanel::paint(juce::Graphics &g) {
    // Live's panel: the dark band with its rounded 4 pt edge, the tabs on it, the body inside.
    g.setColour(look::band);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 5.f);
    const auto inner = body();
    g.setColour(look::frame);
    g.fillRect(inner);
    // The ★ Reference strip: its name, [REF] and [Reference Track], then the track's clips against
    // the arrangement's length, with the playhead.
    const auto s = strip();
    g.setColour(look::panel);
    g.fillRect(s);
    g.setColour(look::text);
    g.setFont(look::font(12.f, true));
    const juce::String star = juce::String(juce::CharPointer_UTF8("\xe2\x98\x85"));
    g.drawText(star + " Reference", s.getX() + 8, s.getY(), 96, s.getHeight(), juce::Justification::centredLeft);
    g.setFont(look::font(12.f));
    const auto reference = root_.referenceTrack();
    g.setColour(reference != 0 ? look::text : look::textDim);
    g.drawText(referenceText(), s.getX() + 104, s.getY(), 100, s.getHeight(), juce::Justification::centredLeft, true);
    const auto line = timeline();
    g.setColour(look::lane);
    g.fillRect(line);
    const auto *track = reference != 0 ? root_.reader().findTrack(reference) : nullptr;
    if (track == nullptr) {
        g.setColour(look::textDim);
        g.setFont(look::font(11.5f));
        g.drawText("A track's Reference role arrives with the format (N38); its route with the cue path (N23).",
                   line.reduced(6, 0), juce::Justification::centredLeft, true);
    } else if (line.getWidth() > 0) {
        const auto end = static_cast<double>(root_.arrangement.geometry.end(root_.reader()));
        const auto rows = root_.arrangement.geometry.rows(root_.reader());
        const float lx = static_cast<float>(line.getX()), lw = static_cast<float>(line.getWidth());
        auto xOf = [&](std::int64_t tick) { return static_cast<float>(static_cast<double>(tick) / end) * lw; };
        g.setColour(root_.arrangement.colourFor(rows.find(reference)));
        for (const auto &c : track->clips)
            g.fillRect(lx + xOf(c->posTicks), static_cast<float>(line.getY() + 2), std::max(1.f, xOf(c->lengthTicks)),
                       static_cast<float>(line.getHeight() - 4));
        const auto px = lx + xOf(root_.timelineTick());
        g.setColour(look::text);
        g.fillRect(px, static_cast<float>(line.getY()), 1.f, static_cast<float>(line.getHeight()));
    }
    if (tab() != Tab::Cascade)
        return;
    // A tab of P2 (ADR-0209 d11): drawn empty, saying when it comes.
    g.setColour(look::textDim);
    g.setFont(look::font(12.f));
    g.drawText(juce::String(kTabNames[static_cast<std::size_t>(tab())]) +
                   " is not built yet: it comes after the first window (ADR-0209 d11).",
               inner, juce::Justification::centred);
}
} // namespace adi::ui
