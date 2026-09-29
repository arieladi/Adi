// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
// A peer that exercises JUCE's real input dispatch without an OS window/display.
class UiFakePeer final : public juce::ComponentPeer {
  public:
    explicit UiFakePeer(juce::Component &c) : ComponentPeer(c, 0), bounds(c.getBounds()) {}
    void *getNativeHandle() const override { return nullptr; }
    void setVisible(bool v) override { visible = v; }
    void setTitle(const juce::String &) override {}
    void setBounds(const juce::Rectangle<int> &b, bool) override { bounds = b; }
    juce::Rectangle<int> getBounds() const override { return bounds; }
    using ComponentPeer::globalToLocal;
    using ComponentPeer::localToGlobal;
    juce::Point<float> localToGlobal(juce::Point<float> p) override { return p; }
    juce::Point<float> globalToLocal(juce::Point<float> p) override { return p; }
    void setMinimised(bool) override {}
    bool isMinimised() const override { return false; }
    bool isShowing() const override { return visible; }
    void setFullScreen(bool) override {}
    bool isFullScreen() const override { return false; }
    void setIcon(const juce::Image &) override {}
    bool contains(juce::Point<int> p, bool) const override {
        return bounds.withPosition(0, 0).contains(p);
    }
    OptionalBorderSize getFrameSizeIfPresent() const override {
        return OptionalBorderSize(juce::BorderSize<int>{});
    }
    juce::BorderSize<int> getFrameSize() const override { return {}; }
    bool setAlwaysOnTop(bool) override { return false; }
    void toFront(bool) override {}
    void toBehind(juce::ComponentPeer *) override {}
    bool isFocused() const override { return true; }
    void grabFocus() override {}
    void repaint(const juce::Rectangle<int> &) override { ++repaints; }
    void performAnyPendingRepaintsNow() override {}
    void setAlpha(float) override {}
    juce::StringArray getAvailableRenderingEngines() override { return {"software"}; }
    void textInputRequired(juce::Point<int>, juce::TextInputTarget &) override {}
    unsigned repaints = 0;

  private:
    juce::Rectangle<int> bounds;
    bool visible = true;
};
class UiPeerRoot final : public juce::Component {
  public:
    juce::ComponentPeer *createNewPeer(int, void *) override { return new UiFakePeer(*this); }
};
