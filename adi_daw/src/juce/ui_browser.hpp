// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/builtin_catalog.hpp"
#include "adi/device_presets.hpp"
#include "adi/media/peaks.hpp"
#include "adi/ui/library_query.hpp"
#include "adi/ui/plugin_scan.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace adi::settings { class AppSettings; }
namespace adi::ui {
class AdiRootComponent;
// The browser's content list (Live 12 manual 4.1): its rows take up and down, and left goes back to
// the sidebar (41.22).
class BrowserList final : public juce::ListBox {
  public:
    std::function<bool(int, bool)> onFolderArrow;
    std::function<void()> onLeft;
    std::function<bool()> onEscape;
    std::function<bool(const juce::KeyPress &)> onPreviewKey;
    std::function<bool(const juce::KeyPress &)> onFavoriteKey;
    bool keyPressed(const juce::KeyPress &k) override {
        if (k == juce::KeyPress::escapeKey && onEscape && onEscape())
            return true;
        if ((k == juce::KeyPress::leftKey || k == juce::KeyPress::rightKey) && onFolderArrow &&
            onFolderArrow(getSelectedRow(), k == juce::KeyPress::rightKey))
            return true;
        if (onFavoriteKey && onFavoriteKey(k))
            return true;
        if (onPreviewKey && onPreviewKey(k))
            return true;
        if (k.isKeyCode(juce::KeyPress::returnKey) && k.getModifiers().isAnyModifierKeyDown())
            return false; // Modified Return belongs to commands, not the list's load action.
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
// audio drop. Instruments lists ADI's own instruments (OneShot, Sampler; DF-W48), Audio Effects Live's
// folders with ADI's built-in effects, and Pd Devices ADI's own (D4, N15's catalogue part):
// double-click or Enter loads one onto the selected track, in front of the device chosen in the strip
// (an instrument onto a MIDI track only, in place of its instrument); a drag drops it onto a lane's
// track or into the chain. An entry
// this build cannot load is dimmed and says why. Plug-Ins lists the VST3 and CLAP plug-ins the app's
// loader found, in folders by format and vendor (DF-W9), from the app's scan off the message thread
// (DF-W34); double-click, Enter and drag load through the engine's insert (N57).
// Sounds lists the user's saved device presets (SC-85).
class BrowserPanel final : public juce::Component, public juce::SettableTooltipClient,
                           public juce::FileDragAndDropTarget, private juce::ListBoxModel {
  public:
    explicit BrowserPanel(AdiRootComponent &);
    ~BrowserPanel() override {
        search.removeKeyListener(&searchKeys_);
        list.setModel(nullptr);
    }
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &, int, int) override;
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
    // The logical row before clipping to the scrolling sidebar pane.
    juce::Rectangle<int> sidebarRowBounds(int row) const noexcept;
    juce::Rectangle<int> contentArea() const noexcept { return contentArea_; }
    bool hasInfoAt(juce::Point<int>) const;
    juce::Rectangle<int> nameHeaderArea() const noexcept { return contentArea_.withHeight(kRowHeight); }
    static juce::Rectangle<int> hideButtonArea() noexcept { return {0, 0, 19, 20}; }
    // Live 11's browser at 100 %: the round buttons' column, the sidebar, a row, the
    // search and preview rows, the dark edge.
    static constexpr int kStripWidth = 20, kSidebarWidth = 113, kRowHeight = 19, kBarHeight = 27;
    juce::TextEditor search;
    BrowserList list;
    class PreviewToggle final : public juce::Button {
      public:
        PreviewToggle();
        void paintButton(juce::Graphics &, bool over, bool down) override;
    } previewToggle;
    juce::Rectangle<int> previewArea() const noexcept;
    juce::Rectangle<int> previewWaveformArea() const noexcept;
    const LibraryFile *selectedSample() const noexcept;
    void syncPreviewToggle();
    bool previewKey(const juce::KeyPress &);
    bool previewSelected(double fraction = 0.);
    using PreviewPeaks = std::vector<media::PeakBin>;
    void setPreviewDisplay(std::shared_ptr<const PreviewPeaks>, std::optional<double> position,
                           bool canRepeat = false);
    // What the library index holds, from the app's indexing worker: the Places join the sidebar
    // after Current Project, before Add Folder (Live's added folders), and the list shows again.
    // `ready` is false while the first scan still runs: the Status Bar says so, not "set a folder".
    void setLibrarySnapshot(std::shared_ptr<const BrowserLibrarySnapshot>, bool ready = true);
    void setCurrentProjectSnapshot(std::shared_ptr<const BrowserLibrarySnapshot>);
    void setListing(LibraryListing, bool ready = true);
    const LibraryListing &listing() const noexcept;
    void restoreLabel(settings::AppSettings &);
    // Selects a label, as a click on it does.
    void choose(int);
    // Selects this exact sample, with a temporary Current Project row for an unindexed source.
    void revealSample(std::filesystem::path);
    std::function<void()> requestAddFolder;
    std::function<void(std::span<const std::filesystem::path>)> addFolders;
    std::function<void(const std::filesystem::path &)> removeFolder;
    std::function<void()> requestLibraryRescan;
    // The app's file-manager action and the popup can be captured without a native peer.
    std::function<void(const std::filesystem::path &)> revealInFileManager;
    std::function<void(juce::PopupMenu, juce::Point<int>)> onContextMenu;
    void searchAll();
    // The files the list shows now, in its order.
    const std::vector<const LibraryFile *> &shown() const noexcept { return shown_; }
    // A row of the list: a library file or folder, a heading, a device or a saved preset.
    struct Row {
        const LibraryFile *file = nullptr;
        const BuiltinInfo *device = nullptr;
        juce::String heading;
        const PluginEntry *plugin = nullptr;
        int depth = 0;
        const preset::Entry *savedPreset = nullptr;
        juce::String tooltip{};
        const BrowserLibrarySnapshot::Folder *folder = nullptr;
        bool folderOpen = false;
    };
    std::function<void(juce::PopupMenu &, const Row &)> appendFavoritesToMenu;
    const std::vector<Row> &rows() const noexcept { return rows_; }
    bool canMarkSelection() const noexcept;
    bool toggleSelectionMark(unsigned slot);

    // Activates the row (double-click or Enter).
    bool load(int row);
    void setDevicePresets(std::vector<preset::Entry>);
    std::function<bool(const std::filesystem::path &)> loadDevicePreset;
    struct HotSwapFilter {
        std::string subtype, targetName;
        bool samples = false;
    };
    struct HotSwapChoice {
        enum class Kind { Builtin, Plugin, Preset, Sample };
        Kind kind;
        std::string uid;
        std::optional<PluginEntry> plugin;
        std::filesystem::path path;
    };
    void setHotSwap(std::optional<HotSwapFilter>);
    bool hotSwapActive() const noexcept { return hotSwap_.has_value(); }
    std::function<bool(const HotSwapChoice &)> swapChoice;
    std::function<void()> leaveHotSwap;
    // Asks the app to scan the plug-ins when Plug-Ins is chosen and no scan has begun; unset outside the
    // app. It returns at once: the scan runs on the app's worker (DF-W34).
    std::function<void()> scanPlugins;
    // The app's scan, each time it changes: Plug-Ins refills when it is shown, and the Status Bar says a
    // scan's start and end while it is.
    void setPluginScan(const PluginScan &);
    const PluginListing &plugins() const noexcept { return pluginScan_.listing; }
    const PluginScan &pluginScan() const noexcept { return pluginScan_; }
    // What a load of a plug-in says where nothing can load it (outside the application).
    static std::string pluginWaits(const PluginEntry &);
    // Why a catalogue entry cannot load in this build; empty when it can.
    static std::string unavailable(const BuiltinInfo &);
    // A device's drag description, and the uid a drop carries ("" when it is not a device).
    static juce::String deviceDescription(const BuiltinInfo &);
    static std::string droppedDevice(const juce::var &description);
    bool acceptsDeviceDrop(const juce::DragAndDropTarget::SourceDetails &) const;
    bool loadDroppedDevice(const juce::DragAndDropTarget::SourceDetails &, std::int64_t track,
                           std::optional<std::int64_t> before = {});
    // The drag's description of these files: what the lanes accept as a drop (`dropDescription`).
    static juce::String dragDescription(const std::vector<const LibraryFile *> &);
    static juce::StringArray droppedFiles(const juce::var &description);
    // The search field's own keys: Down enters the list and Escape clears the search.
    juce::KeyListener &searchKeys() noexcept { return searchKeys_; }

  private:
    void setSidebarScroll(double);
    void revealSidebarSelection();
    bool setCurrentProjectLabelVisible(bool);
    void selectRevealedSampleLabel();
    void clearRevealedSample();
    int sidebarCursor() const noexcept;
    void sidebarClick(int row, const juce::MouseEvent &);
    void presentContextMenu(juce::PopupMenu, const juce::MouseEvent &);
    void addRevealItem(juce::PopupMenu &, const std::filesystem::path &, bool builtin = false);
    void addRescanItem(juce::PopupMenu &);
    void searchEdited();
    bool clearSearch();
    void syncSearchClearButton();
    class SearchClearButton final : public juce::Button {
      public:
        SearchClearButton();
        void paintButton(juce::Graphics &, bool over, bool down) override;
    } searchClear_;
    const juce::BorderSize<int> searchBorder_ = search.getBorder();
    const PluginEntry *draggedPlugin(const juce::DragAndDropTarget::SourceDetails &) const;
    bool loadPluginEntry(const PluginEntry &, std::int64_t track, std::optional<std::int64_t> before);
    bool cancelHotSwap();
    bool loadHotSwap(const Row &);
    std::string labelKey(int) const;
    int findLabel(std::string_view) const;
    // The list's rows for the label chosen and the search typed.
    void refill(int selectRow = -1, bool keepListPosition = false);
    void refillRows();
    void sortRows();
    // The rows are borrowed only while constructing a menu. Actions own the identity.
    void appendContextFavorites(juce::PopupMenu &, const Row &);
    void refillFavorites(const LibraryQuery &);
    void loadFavoriteMarks();
    void updateMarkedSamples();
    bool favoritesShown() const noexcept;
    const Row *favoriteSelection() const noexcept;
    static std::string favoriteKey(const Row &);
    unsigned favoriteMarks(const Row &) const;
    bool toggleFavoriteMark(const std::string &identity, unsigned slot);

    void appendPlaceRows(std::span<const BrowserLibrarySnapshot::Folder>, std::size_t folder, int depth,
                         const std::set<std::filesystem::path> &open);
    bool folderArrow(int row, bool right);
    bool toggleFolder(int row);
    bool hitFolderDisclosure(int row, int x) const;
    int getNumRows() override;
    juce::String getNameForRow(int row) override;
    void paintListBoxItem(int row, juce::Graphics &, int width, int height, bool selected) override;
    juce::String getTooltipForRow(int row) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int> &rows) override;
    void listBoxItemClicked(int row, const juce::MouseEvent &) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent &) override;
    // A static heading is never selected: the selection steps past it, the way it was moving.
    void selectedRowsChanged(int lastRowSelected) override;
    void updatePreviewSelection();
    void setPreviewRowShown(bool);
    std::filesystem::path previewSelection_;
    std::shared_ptr<const PreviewPeaks> previewPeaks_;
    std::optional<double> previewPosition_;
    bool previewRowShown_ = false;
    bool canRepeatPreview_ = false;
    std::optional<bool> nameSortAscending_;
    int firstItem() const;
    int lastSelected_ = -1;
    bool restoringListSelection_ = false;
    void returnKeyPressed(int row) override { load(row); }
    std::vector<Row> rows_;
    std::map<std::string, unsigned, std::less<>> favoriteMarks_;
    std::map<std::filesystem::path, unsigned> sampleMarks_;
    std::vector<const LibraryFile *> markedSamples_;

    std::optional<HotSwapFilter> hotSwap_;
    juce::String idleSearch_, swapSearch_;
    std::map<std::string, std::vector<preset::Entry>> devicePresets_;
    std::shared_ptr<const BrowserLibrarySnapshot> library_;
    std::shared_ptr<const BrowserLibrarySnapshot> projectLibrary_;
    std::optional<LibraryFile> revealedSample_;
    std::map<std::string, std::set<std::filesystem::path>> openFolders_;
    bool ready_ = false;
    PluginScan pluginScan_;
    bool pluginsShown() const;
    void startSearchPluginScan();
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
    settings::AppSettings *labelSettings_ = nullptr;
    std::string pendingLabel_;
    double sidebarScroll_ = 0.;
    bool addFolderFocused_ = false;
    bool ring_ = false;
    juce::Rectangle<int> sidebarArea_, contentArea_;
};
} // namespace adi::ui
