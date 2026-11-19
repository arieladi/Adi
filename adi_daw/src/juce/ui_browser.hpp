// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/builtin_catalog.hpp"
#include "adi/ui/library_listing.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <string>
#include <utility>
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
// audio drop. Audio Effects lists Live's folders with ADI's built-in effects, and Pd Devices ADI's
// own (D4, N15's catalogue part): double-click or Enter loads one onto the selected track, in front
// of the device chosen in the strip; a drag drops it onto a lane's track or into the chain. An entry
// this build cannot load is dimmed and says why. The other categories say what they wait for.
class BrowserPanel final : public juce::Component, public juce::SettableTooltipClient, private juce::ListBoxModel {
  public:
    explicit BrowserPanel(AdiRootComponent &);
    ~BrowserPanel() override {
        search.removeKeyListener(&searchKeys_);
        list.setModel(nullptr);
    }
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
        Entry() = default;
        Entry(juce::String n, int g, bool h = false, bool s = false, std::string p = {})
            : name(std::move(n)), glyph(g), heading(h), samples(s), place(std::move(p)) {}
        juce::String name;
        int glyph = 0; // which small icon
        bool heading = false;
        bool samples = false; // the Samples label
        std::string place;    // a Place's volume id, matched by identity: two folders may share a name
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
    // `ready` is false while the first scan still runs: the Status Bar says so, not "set a folder".
    void setListing(LibraryListing, bool ready = true);
    // Where the list must stop, in this panel's coordinates: above the AI tab at the panes' foot.
    void setFootLimit(int y);
    const LibraryListing &listing() const noexcept { return listing_; }
    // Selects a label, as a click on it does.
    void choose(int);
    // The files the list shows now, in its order.
    const std::vector<const LibraryFile *> &shown() const noexcept { return shown_; }
    // A row of the list: a sample, a folder's heading or a built-in device.
    struct Row {
        const LibraryFile *file = nullptr;
        const BuiltinInfo *device = nullptr;
        juce::String heading;
    };
    const std::vector<Row> &rows() const noexcept { return rows_; }
    // Loads the device on `row` (double-click, Enter), or says why it cannot.
    bool load(int row);
    // Why a catalogue entry cannot load in this build; empty when it can.
    static std::string unavailable(const BuiltinInfo &);
    // A device's drag description, and the uid a drop carries ("" when it is not a device).
    static juce::String deviceDescription(const BuiltinInfo &);
    static std::string droppedDevice(const juce::var &description);
    // The drag's description of these files: what the lanes accept as a drop (`dropDescription`).
    static juce::String dragDescription(const std::vector<const LibraryFile *> &);
    static juce::StringArray droppedFiles(const juce::var &description);
    // The search field's own keys: the down arrow goes into the list.
    juce::KeyListener &searchKeys() noexcept { return searchKeys_; }

  private:
    // The list's rows for the label chosen and the search typed.
    void refill();
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics &, int width, int height, bool selected) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int> &rows) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent &) override { load(row); }
    // A folder's heading is never selected: the selection steps past it, the way it was moving.
    void selectedRowsChanged(int lastRowSelected) override;
    int firstItem() const;
    int lastSelected_ = -1;
    void returnKeyPressed(int row) override { load(row); }
    std::vector<Row> rows_;
    LibraryListing listing_;
    bool ready_ = false;
    int footLimit_ = 1 << 20;
    // The search's down arrow goes into the list (§41.4.1.6).
    struct SearchKeys final : juce::KeyListener {
        BrowserPanel &panel;
        explicit SearchKeys(BrowserPanel &p) : panel(p) {}
        bool keyPressed(const juce::KeyPress &k, juce::Component *) override;
    } searchKeys_{*this};
    void enterList();
    std::vector<const LibraryFile *> shown_;
    std::size_t baseEntries_ = 0;
    AdiRootComponent &root_;
    std::vector<Entry> sidebar_;
    int selected_ = -1;
    bool ring_ = false;
    juce::Rectangle<int> sidebarArea_, contentArea_;
};
} // namespace adi::ui
