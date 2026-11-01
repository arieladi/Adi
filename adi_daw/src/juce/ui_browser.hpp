// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
// Live's browser (manual 3.3, 13; Master Reference 3.1): the search field, the
// sidebar of Collections, Categories and Places, and the content pane. The content
// itself is LANES §3b item 2's next piece; until then the pane shows its header.
class BrowserPanel final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit BrowserPanel(AdiRootComponent &);
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    struct Entry {
        juce::String name;
        int glyph; // which small icon
        bool heading = false;
    };
    const std::vector<Entry> &sidebar() const noexcept { return sidebar_; }
    int selected() const noexcept { return selected_; }
    juce::Rectangle<int> sidebarArea() const noexcept { return sidebarArea_; }
    juce::Rectangle<int> contentArea() const noexcept { return contentArea_; }
    // Live 11's browser at 100 %: the round buttons' column, the sidebar, a row, the
    // search and preview rows, the dark edge.
    static constexpr int kStripWidth = 20, kSidebarWidth = 113, kRowHeight = 19, kBarHeight = 27;
    juce::TextEditor search;

  private:
    AdiRootComponent &root_;
    std::vector<Entry> sidebar_;
    int selected_ = -1;
    juce::Rectangle<int> sidebarArea_, contentArea_;
};
} // namespace adi::ui
