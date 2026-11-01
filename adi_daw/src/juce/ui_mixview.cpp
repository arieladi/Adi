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
MixViewPanel::MixViewPanel(AdiRootComponent &root) : stack(root), root_(root) {
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
    addChildComponent(stack);
}
MixViewPanel::~MixViewPanel() { setLookAndFeel(nullptr); }
MixViewPanel::Tab MixViewPanel::tab() const noexcept {
    return static_cast<Tab>(std::clamp(root_.state().mixViewTab, 0, kTabCount - 1));
}
juce::Rectangle<int> MixViewPanel::body() const noexcept {
    return getLocalBounds().withTrimmedTop(kHeaderHeight).reduced(look::border);
}
void MixViewPanel::frame(const SnapshotReader &reader) {
    floating.setToggleState(root_.state().mixViewDetached, juce::dontSendNotification);
    const int shown = static_cast<int>(tab());
    if (shown != shownTab_) {
        shownTab_ = shown;
        for (int i = 0; i < kTabCount; ++i)
            tabs[static_cast<std::size_t>(i)]->setToggleState(i == shown, juce::dontSendNotification);
        stack.setVisible(tab() == Tab::Stack);
        repaint();
    }
    // STACK's rows follow the tracks; a frame that changes none of them builds nothing.
    if (stack.isVisible())
        stack.frame(reader);
}
void MixViewPanel::resized() {
    // The tabs along the top, from the left, as Live's view choosers sit on its band.
    int x = look::gap;
    const int y = (kHeaderHeight - look::fieldHeight) / 2;
    for (int i = 0; i < kTabCount; ++i) {
        const int w = kTabWidths[static_cast<std::size_t>(i)];
        tabs[static_cast<std::size_t>(i)]->setBounds(x, y, w, look::fieldHeight);
        x += w + 2;
    }
    floating.setBounds(getWidth() - look::gap - 22, y, 22, look::fieldHeight);
    stack.setBounds(body());
}
void MixViewPanel::paint(juce::Graphics &g) {
    // Live's panel: the dark band with its rounded 4 pt edge, the tabs on it, the body inside.
    g.setColour(look::band);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 5.f);
    const auto inner = body();
    g.setColour(look::frame);
    g.fillRect(inner);
    if (tab() == Tab::Stack)
        return;
    // A tab of P2 (ADR-0209 d11): drawn empty, saying when it comes.
    g.setColour(look::textDim);
    g.setFont(look::font(12.f));
    g.drawText(juce::String(kTabNames[static_cast<std::size_t>(tab())]) +
                   " is not built yet: it comes after the first window (ADR-0209 d11).",
               inner, juce::Justification::centred);
}
} // namespace adi::ui
