// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/library_listing.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
// The browser's content list (Live 12 manual 4.1): its rows take up and down, and left goes back to
// the sidebar (41.22).
class BrowserList final : public juce::ListBox {
  public:
    std::function<void()> onLeft;
    bool keyPressed(const juce::KeyPress &k) override {
        if (k == juce::KeyPress::leftKey && onLeft) {
            onLeft();
            return true;
        }
        return juce::ListBox::keyPressed(k);
    }
};
// Live's browser (manual 3.3, 4; Master Reference 3.1): the search field, the sidebar of Collections,
// Categories and Places, and the content pane. Samples and the Places list what the library index
// holds (D1, N15's index part): Samples every audio file of the folders the settings name, a Place
// that folder's; the search narrows the list as it is typed. A row dragged onto the lanes is the
// audio drop. The other categories wait for the catalogue and the devices (D4) and say so.
class BrowserPanel final : public juce::Component, public juce::SettableTooltipClient, private juce::ListBoxModel {
  public:
    explicit BrowserPanel(AdiRootComponent &);
    ~BrowserPanel() override { list.setModel(nullptr); }
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    // With the focus, the up and down arrows move between the sidebar's labels (§41.4.1.6).
    bool keyPressed(const juce::KeyPress &) override;
    void focusGained(FocusChangeType) override;
    void focusLost(FocusChangeType) override;
    // Live 12's focus ring round the sidebar, where the keyboard put the focus.
    bool focusRing() const noexcept { return ring_; }
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
    BrowserList list;
    // What the library index holds, from the app's indexing worker: the Places join the sidebar
    // after Current Project, before Add Folder (Live's added folders), and the list shows again.
    void setListing(LibraryListing);
    const LibraryListing &listing() const noexcept { return listing_; }
    // Selects a label, as a click on it does.
    void choose(int);
    // The files the list shows now, in its order.
    const std::vector<const LibraryFile *> &shown() const noexcept { return shown_; }
    // The drag's description of these files: what the lanes accept as a drop (`dropDescription`).
    static juce::String dragDescription(const std::vector<const LibraryFile *> &);
    static juce::StringArray droppedFiles(const juce::var &description);

  private:
    // The list's rows for the label chosen and the search typed.
    void refill();
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics &, int width, int height, bool selected) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int> &rows) override;
    LibraryListing listing_;
    std::vector<const LibraryFile *> shown_;
    std::size_t baseEntries_ = 0;
    AdiRootComponent &root_;
    std::vector<Entry> sidebar_;
    int selected_ = -1;
    bool ring_ = false;
    juce::Rectangle<int> sidebarArea_, contentArea_;
};
} // namespace adi::ui
