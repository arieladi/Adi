// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mixview.hpp"
#include "ui_controls.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include "adi/ui/meters.hpp"
#include <algorithm>
#include <cmath>
namespace adi::ui {
void FloatButton::paintButton(juce::Graphics &g, bool over, bool) {
    const auto r = getLocalBounds().toFloat();
    look::box(g, r, getToggleState() ? look::accent : over ? look::chooser : look::button);
    // Two boxes, the front one offset down and right: a window over the window.
    g.setColour(look::text);
    const float s = std::min(r.getWidth(), r.getHeight());
    const juce::Rectangle<float> back(r.getX() + s * .25f, r.getY() + s * .22f, s * .42f, s * .34f);
    g.drawRect(back, look::hairline);
    const auto front = back.translated(s * .14f, s * .2f);
    g.setColour(getToggleState() ? look::accent : look::button);
    g.fillRect(front);
    g.setColour(look::text);
    g.drawRect(front, look::hairline);
}
void ReferenceButton::paintButton(juce::Graphics &g, bool over, bool down) {
    controls::button(g, getLocalBounds().toFloat(), controls::stateOf(false, over, down, isEnabled()), "REF");
}
void EyeButton::paintButton(juce::Graphics &g, bool over, bool) {
    const auto r = getLocalBounds().toFloat();
    if (isEnabled())
        look::box(g, r, getToggleState() ? look::accent : over ? look::chooser : look::button);
    else
        controls::button(g, r, controls::ButtonState::Disabled);
    // The eye: an almond and its pupil.
    const juce::Rectangle<float> eye(r.getX() + 5, r.getCentreY() - 3.5f, 14, 7);
    juce::Path almond;
    almond.startNewSubPath(eye.getX(), eye.getCentreY());
    almond.quadraticTo(eye.getCentreX(), eye.getY() - 3, eye.getRight(), eye.getCentreY());
    almond.quadraticTo(eye.getCentreX(), eye.getBottom() + 3, eye.getX(), eye.getCentreY());
    g.setColour(isEnabled() ? look::text : controls::labelColour(controls::ButtonState::Disabled));
    g.strokePath(almond, juce::PathStrokeType(1.f));
    g.fillEllipse(eye.getCentreX() - 2, eye.getCentreY() - 2, 4, 4);
    g.setFont(look::font(11.5f));
    g.drawText("Reference Track", r.withTrimmedLeft(24), juce::Justification::centredLeft);
}
MixSongStrip::MixSongStrip(AdiRootComponent& root) : root_(root) {
    setTitle("Song overview and ruler");
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setOpaque(true);
}
int MixSongStrip::headX(std::int64_t tick) const noexcept {
    const auto position = static_cast<double>(std::clamp(tick, std::int64_t{}, span_)) / static_cast<double>(span_);
    return look::gap + juce::roundToInt(position * std::max(0, getWidth() - 2 * look::gap - 1));
}
std::int64_t MixSongStrip::tickAt(float x) const noexcept {
    const auto width = std::max(1, getWidth() - 2 * look::gap - 1);
    const auto position = std::clamp((static_cast<double>(x) - look::gap) / width, 0., 1.);
    return position == 1. ? span_ : std::llround(position * static_cast<double>(span_));
}
void MixSongStrip::drawSong(juce::Graphics& g, const SnapshotReader& reader) {
    g.fillAll(look::frame);
    const int rulerY = std::max(0, getHeight() - look::mixSongRulerHeight);
    const auto rows = root_.arrangement.geometry.rows(reader);
    const float lane = static_cast<float>(std::max(0, rulerY - look::border))
                     / static_cast<float>(std::max<std::size_t>(1, rows.size()));
    {
        juce::Graphics::ScopedSaveState clips(g);
        g.reduceClipRegion(0, 0, getWidth(), rulerY);
        for (const auto row : rows) {
            g.setColour(row.track->color ? look::paletteColour(*row.track->color) : look::trackColour(row.position));
            for (const auto& clip : row.track->clips) {
                const auto x = headX(clip->posTicks), right = headX(clip->posTicks + clip->lengthTicks);
                g.fillRect(static_cast<float>(x), look::hairline * 2 + lane * static_cast<float>(row.position),
                           static_cast<float>(std::max(1, right - x)), std::max(look::hairline, lane - look::hairline));
            }
        }
    }
    const auto& transport = reader.transport();
    const int braceHeight = std::min(look::mixSongLoopBraceHeight, getHeight() - rulerY);
    if (transport.loopEndTicks > transport.loopStartTicks) {
        const auto left = headX(transport.loopStartTicks), right = headX(transport.loopEndTicks);
        if (transport.loopEnabled) {
            g.setColour(look::loopBand);
            g.fillRect(left, 0, right - left, rulerY);
        }
        g.setColour(transport.loopEnabled ? look::loopOn : look::loopOff);
        g.fillRect(left, rulerY, right - left, braceHeight);
    }
    const auto lastBar = barPosition(span_, reader.meters()).bar;
    const auto slots = std::max(1, getWidth() / look::mixSongLabelSpacing);
    std::int64_t stride = 1;
    while (lastBar / stride > slots) stride *= 2;
    g.setFont(look::font(look::fontSmall));
    for (std::int64_t bar = 1; bar <= lastBar; bar += stride) {
        const auto tick = tickOf({bar, 1, 1}, reader.meters());
        if (!tick) continue;
        const auto x = headX(*tick);
        g.setColour(look::gridLine);
        g.drawVerticalLine(x, static_cast<float>(rulerY + braceHeight), static_cast<float>(getHeight()));
        g.setColour(look::textDim);
        g.drawText(juce::String(static_cast<juce::int64>(bar)), x + look::border, rulerY + braceHeight,
                   look::mixSongLabelSpacing - look::border, getHeight() - rulerY - braceHeight,
                   juce::Justification::centredLeft);
    }
}
void MixSongStrip::frame(const SnapshotReader& reader, std::int64_t tick) {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    if (theme_ != look::themeId() || density_ != look::densityId() || generation_ != root_.generation() ||
        snapshot_ != reader.get() || map_.getWidth() != getWidth() || map_.getHeight() != getHeight()) {
        theme_ = look::themeId();
        density_ = look::densityId();
        generation_ = root_.generation();
        snapshot_ = reader.get();
        span_ = std::max(root_.arrangement.geometry.end(reader), reader.transport().loopEndTicks);
        map_ = juce::Image(juce::Image::RGB, getWidth(), getHeight(), false, juce::SoftwareImageType{});
        juce::Graphics g(map_);
        drawSong(g, reader);
        repaint();
    }
    moveHead(headX(seeking_ ? previewTick_ : tick));
}
void MixSongStrip::moveHead(int x) {
    if (x == head_) return;
    const int radius = look::border + juce::roundToInt(look::line);
    if (head_ >= 0) repaint(head_ - radius, 0, 2 * radius + 1, getHeight());
    head_ = x;
    repaint(head_ - radius, 0, 2 * radius + 1, getHeight());
}
void MixSongStrip::paint(juce::Graphics& g) {
    if (map_.isValid()) g.drawImageAt(map_, 0, 0);
    else g.fillAll(look::frame);
    if (head_ < 0) return;
    const auto x = static_cast<float>(head_), radius = static_cast<float>(look::border);
    g.setColour(look::text);
    g.drawLine(x, 0.f, x, static_cast<float>(getHeight()), look::line);
    juce::Path marker;
    marker.addTriangle(x - radius, 0.f, x + radius, 0.f, x, 2.f * radius);
    g.fillPath(marker);
}
void MixSongStrip::mouseDown(const juce::MouseEvent& e) {
    seeking_ = e.mods.isLeftButtonDown();
    if (seeking_) mouseDrag(e);
}
void MixSongStrip::mouseDrag(const juce::MouseEvent& e) {
    if (!seeking_) return;
    previewTick_ = tickAt(e.position.x);
    moveHead(headX(previewTick_));
}
void MixSongStrip::mouseUp(const juce::MouseEvent& e) {
    if (!seeking_) return;
    cancelSeek();
    if (!root_.playFrom(tickAt(e.position.x))) moveHead(headX(root_.timelineTick()));
}
MixViewPanel::MixViewPanel(AdiRootComponent &root)
    : song(root), stack(root), channels(root), focusOwner_(std::make_unique<MixFocus>(root)),
      focusPage(*focusOwner_), root_(root) {
    setTitle("Mix View");
    addAndMakeVisible(song);
    setLookAndFeel(&look::liveLook());
    for (int i = 0; i < kTabCount; ++i) {
        auto button = std::make_unique<juce::TextButton>(kTabNames[static_cast<std::size_t>(i)]);
        button->setTitle(kTabNames[static_cast<std::size_t>(i)]);
        button->setClickingTogglesState(false);
        button->setMouseClickGrabsKeyboardFocus(false);
        button->onClick = [this, i] { root_.chooseMixViewTab(i); };
        addChildComponent(*button);
        button->setVisible(std::any_of(pages.begin(), pages.end(), [i](Tab page) { return static_cast<int>(page) == i; }));
        tabs[static_cast<std::size_t>(i)] = std::move(button);
    }
    refreshTheme();
    floating.setTooltip("Mix View in a Window. Moves the Mix View to a window of its own, onto any display; "
                        "again, back to the centre.");
    floating.setMouseClickGrabsKeyboardFocus(false);
    floating.onClick = [this] { root_.command(AppCommands::DetachMixView); };
    addAndMakeVisible(floating);
    ref.setEnabled(false);
    ref.setTooltip(AdiRootComponent::kNoReferenceTrack);
    ref.setMouseClickGrabsKeyboardFocus(false);
    addAndMakeVisible(ref);
    eye.setEnabled(false);
    eye.setTooltip(AdiRootComponent::kNoReferenceTrack);
    eye.setMouseClickGrabsKeyboardFocus(false);
    eye.onClick = [this] { root_.command(AppCommands::ToggleReferenceTrack); };
    addAndMakeVisible(eye);
    addChildComponent(stack);
    addChildComponent(channels);
    addChildComponent(mainMeter);
    addChildComponent(focusPage);
    focusPage.onEdit = [this](std::int64_t id) {
        if (tab() == Tab::Focus && stack.isShowing() && focusPage.contains(id) && stack.model().shownIndex(id))
            stack.focus(id);
    };
    focusPage.onMembersChanged = [this] {
        if (!ownerReady_ || tab() != Tab::Focus) return;
        const auto ids = focusPage.channelIds();
        channels.setFocusMembers(ids, focusPage.mainTrack());
        stack.filterChannels(ids, true);
    };
    channels.onChoose = [this](std::int64_t id) {
        const juce::Component::SafePointer<MixViewPanel> alive(this);
        auto &selection = root_.arrangement.geometry.selection;
        selection.track = id;
        selection.toTrack = 0;
        selection.picked.clear();
        selection.clip = 0;
        selection.pickedClips.clear();
        root_.publishSelection();
        if (!alive) return;
        if (tab() == Tab::Stack && stack.isVisible()) stack.focus(id);
        else if (tab() == Tab::Focus) focusPage.select(id);
    };
    channels.onFilter = [this](auto tracks) {
        if (tab() == Tab::Stack) stack.filterChannels(std::move(tracks));
    };
    channels.onOpenInfo = [this] {
        if (!stack.model().capture()) root_.chooseMixViewTab(static_cast<int>(Tab::Analysis));
    };
    addChildComponent(analysis);
    addChildComponent(analyzerDock);
    addAndMakeVisible(analyzerToggle);
    analyzerToggle.setMouseClickGrabsKeyboardFocus(false);
    analyzerToggle.onClick = [this] { setAnalyzerDock(analyzerToggle.getToggleState()); };
    analyzerDock.onChange = [this] { persistAnalyzerSettings(); };
    analyzerDock.onExpand = [this](std::int64_t track) { if (root_.openAnalyzer) root_.openAnalyzer(track); };
    analysis.detail.onExpand = [this](std::int64_t track) { if (root_.openAnalyzer) root_.openAnalyzer(track); };
    analysis.detail.onPaletteChange = [this] {
        auto &detail = analysis.detail;
        auto &state = root_.state();
        const auto before = state.analysisPalette; const bool wasReversed = state.analysisPaletteReverse;
        state.analysisPalette = detail.requestedPalette(); state.analysisPaletteReverse = detail.reverseButton.getToggleState();
        if (!root_.persist()) {
            state.analysisPalette = before; state.analysisPaletteReverse = wasReversed;
            detail.restorePaletteControls(before, wasReversed); return;
        }
        detail.setPalette(state.analysisPalette, state.analysisPaletteReverse);
    };
    analysis.detail.onSpectrumSlopeChange = [this] {
        auto &detail = analysis.detail;
        auto &state = root_.state();
        const auto before = state.analysisSpectrumSlope;
        state.analysisSpectrumSlope = spectrumSlopeId(detail.requestedSpectrumSlope());
        if (!root_.persist()) {
            state.analysisSpectrumSlope = before;
            detail.restoreSlopeControls(spectrumSlopeFromId(before)); return;
        }
        applySpectrumSlope(spectrumSlopeFromId(state.analysisSpectrumSlope));
    };
}
MixViewPanel::~MixViewPanel() { setLookAndFeel(nullptr); }
void MixViewPanel::adoptViewState() {
    ownerReady_ = true;
    shownStoredTab_ = -1;
    stack.adoptViewState();
    const auto &state = root_.state();
    analyzerDock.configure(state.mixAnalyzerSource, state.mixAnalyzerPoint, state.mixAnalyzerChannel,
                           state.mixAnalyzerWidth);
    analysis.detail.setPalette(state.analysisPalette, state.analysisPaletteReverse);
    applySpectrumSlope(spectrumSlopeFromId(state.analysisSpectrumSlope));
    resized();
}
void MixViewPanel::applySpectrumSlope(SpectrumSlope slope) {
    analysis.detail.setSpectrumSlope(slope);
    analyzerDock.spectrum.setSpectrumSlope(slope);
}
bool MixViewPanel::hasAnalyzerControlFocus() const {
    return AnalysisPane::anyControlsFocused() || analyzerToggle.hasKeyboardFocus(true) ||
        (mainMeter.isVisible() && mainMeter.hasKeyboardFocus(true)) ||
        (analyzerDock.isVisible() && analyzerDock.hasKeyboardFocus(true)) ||
        (analysis.isVisible() && analysis.hasKeyboardFocus(true));
}
bool MixViewPanel::focusChannelSearch() {
    if (!root_.state().mixView || !root_.chooseMixViewTab(static_cast<int>(Tab::Stack))) return false;
    const juce::Component::SafePointer<MixViewPanel> alive(this);
    frame(root_.reader());
    if (!alive) return false;
    auto &search = channels.isVisible() ? channels.search : stack.search;
    focusIfShowing(search);
    if (alive) search.selectAll();
    return true;
}
MixViewPanel::Tab MixViewPanel::tab() const noexcept {
    const auto saved = static_cast<Tab>(std::clamp(root_.state().mixViewTab, 0, kTabCount - 1));
    return saved == Tab::Overview || saved == Tab::Cascade ? Tab::Analysis : saved;
}
juce::Rectangle<int> MixViewPanel::body() const noexcept {
    return appliedBody_;
}
juce::Rectangle<int> MixViewPanel::strip() const noexcept {
    return appliedStrip_;
}
juce::Rectangle<int> MixViewPanel::timeline() const noexcept {
    const auto s = strip();
    const auto &measures = densityFor(appliedDensity_);
    const int left = eye.getRight() + measures.gap;
    const int inset = measures.border + juce::roundToInt(measures.hairline);
    return {left, s.getY() + inset, std::max(0, s.getRight() - measures.gap - left), std::max(0, s.getHeight() - 2 * inset)};
}
juce::String MixViewPanel::referenceText() const {
    if (const auto id = root_.referenceTrack(); id != 0)
        if (const auto name = shownName(root_.arrangement.geometry, root_.reader(), id); !name.empty())
            return juce::String::fromUTF8(name.c_str()); // as its header shows it (walk-B B13)
    return "None yet";
}
void MixViewPanel::refreshTheme() {
    const auto theme = look::themeId();
    if (appliedTheme_ == theme) return;
    appliedTheme_ = theme;
    for (auto &button : tabs) {
        button->setColour(juce::TextButton::buttonColourId, look::button);
        button->setColour(juce::TextButton::buttonOnColourId, look::accent);
        button->setColour(juce::TextButton::textColourOffId, look::text);
        button->setColour(juce::TextButton::textColourOnId, look::text);
    }
    channels.list.setColour(juce::ListBox::backgroundColourId, look::panel);
    repaint();
}
void MixViewPanel::frame(const SnapshotReader &reader) {
    const juce::Component::SafePointer<MixViewPanel> alive(this);
    refreshTheme();
    floating.setToggleState(root_.state().mixViewDetached, juce::dontSendNotification);
    const bool assigned = root_.referenceTrack() != 0;
    if (referenceAssigned_ != assigned) {
        referenceAssigned_ = assigned;
        eye.setEnabled(assigned);
        if (!alive) return;
        eye.setTooltip(assigned ? "Show or hide the Reference track in the Arrangement." : AdiRootComponent::kNoReferenceTrack);
        ref.setTooltip(assigned ? AdiRootComponent::kReferencePlaybackUnavailable : AdiRootComponent::kNoReferenceTrack);
        repaint(strip());
    }
    const bool referenceShown = assigned && root_.state().showReference;
    if (eye.getToggleState() != referenceShown || root_.playing()) {
        eye.setToggleState(referenceShown, juce::dontSendNotification);
        repaint(strip());
    }
    const int stored = std::clamp(root_.state().mixViewTab, 0, kTabCount - 1);
    if (stored != shownStoredTab_) {
        shownStoredTab_ = stored;
        const bool editing = tab() == Tab::Stack || tab() == Tab::Focus;
        for (const auto page : pages) {
            const bool selected = page == tab() || (page == Tab::Analysis && !editing);
            tabs[static_cast<std::size_t>(page)]->setToggleState(selected, juce::dontSendNotification);
        }
        stack.setVisible(editing);
        if (!alive) return;
        if (tab() == Tab::Focus) {
            const auto ids = focusPage.channelIds();
            channels.setFocusMembers(ids, focusPage.mainTrack());
            stack.filterChannels(ids, true);
        } else {
            channels.setFocusMembers({});
            if (tab() == Tab::Stack) stack.filterChannels(channels.filterMembership());
        }
        if (!alive) return;
        focusPage.setVisible(tab() == Tab::Focus);
        if (!alive) return;
        // Legacy Overview and Analysers share the same page. Only entering
        // Overview selects Spectrum so later subpage choices remain local.
        if (stored == static_cast<int>(Tab::Overview)) {
            analysis.setPage(MixAnalysers::Page::Spectrum);
            if (!alive) return;
        }
        analysis.setVisible(tab() == Tab::Analysis || tab() == Tab::ScopePhase || tab() == Tab::Spectrogram);
        if (!alive) return;
        if (tab() == Tab::ScopePhase) analysis.setMode(SharedSpectrumView::Mode::Phase);
        if (tab() == Tab::Spectrogram) analysis.setMode(SharedSpectrumView::Mode::Spectrogram);
        layoutPending_ = true;
        repaint();
    }
    if (layoutPending_ || appliedDensity_ != look::densityId() || appliedDock_ != root_.state().mixAnalyzerDock ||
        song.getHeight() != std::min(look::mixSongHeight, getHeight() / 5) ||
        appliedWidth_ != root_.state().mixAnalyzerWidth) resized();
    if (!alive) return;
    song.frame(reader, root_.timelineTick());
    channels.frame(reader);
    if (!alive) return;
    if (focusPage.isVisible()) {
        focusPage.frame(reader);
        if (!alive) return;
    }
    // STACK's rows follow the tracks; a frame that changes none of them builds nothing.
    if (stack.isVisible()) {
        stack.frame(reader);
        if (!alive) return;
    }
    analysis.frame(root_.sharedAnalysis, reader, root_.analysisTrack(reader), root_.analysisTrack(reader, true),
                   root_.projectViewGeneration());
    if (!alive) return;
    analyzerDock.frame(root_.sharedAnalysis, root_.analysisTrack(reader), root_.analysisTrack(reader, true));
    if (!alive) return;
    if (mainMeter.isVisible()) mainMeter.frame(root_.sharedAnalysis, root_.analysisTrack(reader), mainMeterSource.lock());
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
    appliedDensity_ = look::densityId();
    const auto &state = root_.state();
    appliedDock_ = state.mixAnalyzerDock; appliedWidth_ = state.mixAnalyzerWidth;
    const bool singleRow = getWidth() >= look::mixHeaderSingleRowWidth;
    const int headerHeight = look::mixHeaderHeight * (singleRow ? 1 : 2);
    auto remainingArea = getLocalBounds();
    song.setBounds(remainingArea.removeFromTop(std::min(look::mixSongHeight, getHeight() / 5)));
    appliedBody_ = remainingArea.withTrimmedTop(headerHeight + look::mixReferenceHeight).reduced(look::border);
    appliedStrip_ = {look::border, song.getBottom() + headerHeight, std::max(0, getWidth() - 2 * look::border),
                     look::mixReferenceHeight - look::border / 2};
    analyzerToggle.setToggleState(appliedDock_, juce::dontSendNotification);
    int x = look::gap;
    const int y = song.getBottom() + (look::mixHeaderHeight - look::fieldHeight) / 2;
    const int tabsWidth = std::max(0, getWidth() - look::gap * 3 -
        look::mixFloatButtonWidth - look::border - (singleRow ? look::mixDockButtonWidth : 0));
    const int pageWidth = std::min(look::mixPageWidth, tabsWidth / static_cast<int>(pages.size()));
    for (const auto page : pages) {
        tabs[static_cast<std::size_t>(page)]->setBounds(x, y, pageWidth, look::fieldHeight);
        x += pageWidth + look::border;
    }
    floating.setBounds(std::max(0, getWidth() - look::gap - look::mixFloatButtonWidth), y,
                       std::min(look::mixFloatButtonWidth, getWidth()), look::fieldHeight);
    analyzerToggle.setBounds(singleRow ? std::max(x, floating.getX() - look::mixDockButtonWidth - look::border) : look::gap,
                             y + (singleRow ? 0 : look::mixHeaderHeight),
                             std::min(look::mixDockButtonWidth, std::max(0, getWidth() - look::gap * 2)), look::fieldHeight);
    const auto s = strip();
    const int sy = s.getY() + (s.getHeight() - look::fieldHeight) / 2;
    const int refTextWidth = look::mixReferenceLabelWidth + look::mixReferenceNameWidth + 2 * look::gap - look::border / 2;
    ref.setBounds(s.getX() + refTextWidth, sy, look::mixReferenceButtonWidth, look::fieldHeight);
    eye.setBounds(ref.getRight() + look::gap - look::border / 2, sy, look::mixReferenceVisibilityWidth, look::fieldHeight);
    auto content = body();
    const int roomForMeter = content.getWidth() - kStackMinimumWidth - look::gap;
    const bool showMeter = roomForMeter >= look::mixMeterMinWidth && content.getHeight() >= look::mixMeterMinHeight;
    mainMeter.setVisible(showMeter);
    if (!alive) return;
    if (showMeter) {
        const int preferred = std::clamp(getWidth() / 8, look::mixMeterMinWidth, look::mixMeterMaxWidth);
        mainMeter.setBounds(content.removeFromRight(std::min(roomForMeter, preferred)));
        content.removeFromRight(look::gap);
        if (!alive) return;
    }
    const bool beside = tab() == Tab::Stack;
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
        "Show or hide Meters, Phase and Spectrum beside Stack.");
    const int navWidth = std::clamp(getWidth() / 6, look::mixChannelMinWidth, look::mixChannelMaxWidth);
    const bool showChannels = content.getWidth() >= kStackMinimumWidth + navWidth + look::gap &&
                              content.getHeight() >= look::mixInfoHeight + 6 * look::mixChannelRowHeight;
    channels.setVisible(showChannels);
    if (!alive) return;
    if (showChannels) {
        channels.setBounds(content.removeFromLeft(navWidth));
        content.removeFromLeft(look::gap);
        if (!alive) return;
    }
    if (tab() == Tab::Focus) {
        auto chains = content;
        focusPage.setBounds(chains.removeFromTop(chains.getHeight() * 3 / 5));
        if (!alive) return;
        stack.setBounds(chains.withTrimmedTop(look::gap));
    } else {
        stack.setBounds(content);
    }
    if (!alive) return;
    analysis.setBounds(content);
}
void MixViewPanel::paint(juce::Graphics &g) {
    const auto &measures = densityFor(appliedDensity_);
    g.setColour(look::band);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), measures.radiusControl);
    const auto inner = body();
    g.setColour(look::frame);
    g.fillRect(inner);
    // The ★ Reference strip: its name, [REF] and [Reference Track], then the track's clips against
    // the arrangement's length, with the playhead.
    const auto s = strip();
    g.setColour(look::panel);
    g.fillRect(s);
    g.setColour(look::text);
    g.setFont(look::font(measures.fontField, true));
    const juce::String star = juce::String(juce::CharPointer_UTF8("\xe2\x98\x85"));
    const int labelX = s.getX() + measures.gap;
    g.drawText(star + " Reference", labelX, s.getY(), measures.mixReferenceLabelWidth, s.getHeight(),
               juce::Justification::centredLeft);
    g.setFont(look::font(measures.fontField));
    const auto reference = root_.referenceTrack();
    g.setColour(reference != 0 ? look::text : look::textDim);
    g.drawText(referenceText(), labelX + measures.mixReferenceLabelWidth, s.getY(), measures.mixReferenceNameWidth,
               s.getHeight(), juce::Justification::centredLeft, true);
    const auto line = timeline();
    g.setColour(look::lane);
    g.fillRect(line);
    const auto *track = reference != 0 ? root_.reader().findTrack(reference) : nullptr;
    if (track == nullptr) {
        g.setColour(look::textDim);
        g.setFont(look::font(measures.fontBody));
        g.drawText(AdiRootComponent::kNoReferenceTrack,
                   line.reduced(measures.gap - measures.border / 2, 0), juce::Justification::centredLeft, true);
    } else if (line.getWidth() > 0) {
        const auto end = static_cast<double>(root_.arrangement.geometry.end(root_.reader()));
        const auto rows = root_.arrangement.geometry.rows(root_.reader());
        const float lx = static_cast<float>(line.getX()), lw = static_cast<float>(line.getWidth());
        auto xOf = [&](std::int64_t tick) { return static_cast<float>(static_cast<double>(tick) / end) * lw; };
        g.setColour(root_.arrangement.colourFor(rows.find(reference)));
        const auto clip = line.reduced(0, measures.border / 2).toFloat();
        for (const auto &c : track->clips)
            g.fillRect(lx + xOf(c->posTicks), clip.getY(), std::max(measures.hairline, xOf(c->lengthTicks)), clip.getHeight());
        const auto px = lx + xOf(root_.timelineTick());
        g.setColour(look::text);
        g.fillRect(px, static_cast<float>(line.getY()), measures.hairline, static_cast<float>(line.getHeight()));
    }
}
} // namespace adi::ui
