// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_browser.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "ui_controls.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <unordered_set>
namespace adi::ui {
namespace {
constexpr int kSearchClearSlot = 21;
constexpr int kFolderIndent = 12, kFolderDisclosureWidth = 20;
enum Glyph { None, Favorite, Sounds, Drums, Instruments, AudioFx, MidiFx, Pd, PlugIns, Clips,
             Samples, Grooves, Templates, Packs, User, Project, AddFolder, All, Ai };
constexpr auto kLastLabel = "browser.lastLabel";
constexpr auto kAiLabel = "label:AI";
// A proposal's card in the AI mode's pane (browser-2.png, state C): its request, one line under it,
// then its buttons.
constexpr int kCardPad = 6, kCardTitle = 18, kCardDetail = 16, kCardMovedDetail = 30, kCardButtons = 20,
              kCardGap = 8, kCardButtonGap = 4;
int cardHeight(const BrowserPanel::AgentCard &card) {
    using Moved = BrowserPanel::AgentCard::Moved;
    return kCardPad + kCardTitle + (card.moved == Moved::No ? kCardDetail : kCardMovedDetail) + kCardButtonGap +
           kCardButtons + kCardPad;
}
// The buttons a card offers, left to right: Apply only on a current card; Ask again in its place on a
// moved card of ADI's own agent.
std::vector<BrowserPanel::CardButton> cardButtons(const BrowserPanel::AgentCard &card) {
    using B = BrowserPanel::CardButton;
    if (card.moved == BrowserPanel::AgentCard::Moved::No)
        return {B::Apply, B::Reject, B::OpenList};
    if (card.canAskAgain)
        return {B::AskAgain, B::Reject, B::OpenList};
    return {B::Reject, B::OpenList};
}
int cardButtonWidth(BrowserPanel::CardButton b) {
    using B = BrowserPanel::CardButton;
    return b == B::Apply || b == B::Reject ? 50 : 64;
}
const char *cardButtonLabel(BrowserPanel::CardButton b) {
    using B = BrowserPanel::CardButton;
    switch (b) {
    case B::Apply:
        return "Apply";
    case B::Reject:
        return "Reject";
    case B::OpenList:
        return "Open list";
    case B::AskAgain:
        return "Ask again";
    }
    return "";
}
constexpr auto kAllLabel = "label:All";
constexpr auto kFirstLabel = "label:Instruments";
constexpr auto kSamplesLabel = "label:Samples";
constexpr auto kCurrentProjectLabel = "label:Current Project";
constexpr std::string_view kSampleMarkPrefix = "sample:";
constexpr auto kFavoriteSlot = 0u;
constexpr auto kFavoriteMarkSize = 5, kFavoriteMarkPitch = 7, kFavoriteMarkInset = 5;
struct CollectionColour {
    const char *name;
    std::int64_t palette;
};
// Use ADI's existing palette until the Collections sheet is checked.
constexpr std::array<CollectionColour, 7> kCollectionColours{{
    CollectionColour{"Red", 2}, {"Orange", 4}, {"Yellow", 5}, {"Green", 7},
    {"Blue", 11}, {"Purple", 13}, {"Gray", -1},
}};
juce::Colour collectionColour(std::size_t i) {
    const auto palette = kCollectionColours[i].palette;
    return palette >= 0 ? look::paletteColour(palette) : look::textDim;
}
int favoriteMarkWidth(unsigned marks) { return std::popcount(marks) * kFavoriteMarkPitch; }
void paintFavoriteMarks(juce::Graphics &g, unsigned marks, int width, int height) {
    if (marks == 0)
        return;
    int x = width - kFavoriteMarkInset;
    const int y = (height - kFavoriteMarkSize) / 2;
    if ((marks & (1u << kFavoriteSlot)) != 0) {
        x -= kFavoriteMarkPitch;
        g.setColour(look::favourite);
        g.drawRect(x, y, kFavoriteMarkSize, kFavoriteMarkSize);
    }
    for (std::size_t i = 0; i < kCollectionColours.size(); ++i) {
        if ((marks & (2u << i)) != 0) {
            x -= kFavoriteMarkPitch;
            g.setColour(collectionColour(i));
            g.fillRect(x, y, kFavoriteMarkSize, kFavoriteMarkSize);
        }
    }
}

juce::String nativeFilePath(const std::filesystem::path &path) {
    const auto native = std::filesystem::path(path).make_preferred().u8string();
    return juce::String::fromUTF8(reinterpret_cast<const char *>(native.data()), static_cast<int>(native.size()));
}
bool presetNameLess(const preset::Entry &a, const preset::Entry &b) {
    const int order = juce::String::fromUTF8(a.name.c_str()).compareIgnoreCase(juce::String::fromUTF8(b.name.c_str()));
    return order != 0 ? order < 0 : a.path < b.path;
}
juce::String rowName(const BrowserPanel::Row &row) {
    if (row.folder)
        return juce::String::fromUTF8(row.folder->name.c_str());
    if (row.device)
        return juce::String::fromUTF8(row.device->name.data(), static_cast<int>(row.device->name.size()));
    if (row.plugin)
        return juce::String::fromUTF8(row.plugin->name.c_str());
    if (row.savedPreset)
        return juce::String::fromUTF8(row.savedPreset->name.c_str());
    return row.file ? juce::String::fromUTF8(row.file->name.c_str()) : row.heading;
}
void icon(juce::Graphics &g, int kind, juce::Rectangle<float> r, std::optional<juce::Colour> ink = {}) {
    g.setColour(look::text);
    const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
    juce::Path p;
    switch (kind) {
    case All:
        g.drawEllipse(r.reduced(3.f).translated(-1.f, -1.f), 1.f);
        g.drawLine(x + w - 5.f, y + h - 5.f, x + w - 2.f, y + h - 2.f, 1.f);
        break;
    case Favorite:
        g.setColour(look::favourite);
        g.fillRect(r.reduced(3));
        break;
    case Sounds:
        g.fillEllipse(x + 1, y + h - 5, 4, 4);
        g.fillEllipse(x + w - 6, y + h - 6, 4, 4);
        g.fillRect(x + 4, y + 2, 1.2f, h - 5);
        g.fillRect(x + w - 3, y + 1, 1.2f, h - 5);
        g.fillRect(x + 4, y + 1, w - 6, 1.6f);
        break;
    case Drums:
        for (int i = 0; i < 4; ++i)
            g.drawRect(x + 1 + static_cast<float>(i % 2) * 6, y + 1 + static_cast<float>(i / 2) * 6,
                       4.5f, 4.5f, 1.f);
        break;
    case Instruments:
        g.drawEllipse(r.reduced(1.5f), 1.2f);
        g.fillRect(r.getCentreX() - 0.6f, y + 2, 1.2f, h / 2 - 1);
        break;
    case AudioFx:
        for (int i = 0; i < 6; ++i)
            g.fillRect(x + 1.f + static_cast<float>(i) * 2.2f, y + 2.f + static_cast<float>(i % 3) * 2,
                       1.2f, h - 4 - static_cast<float>(i % 3) * 4);
        break;
    case MidiFx:
        for (int i = 0; i < 3; ++i)
            g.fillRect(x + 1, y + 3.f + static_cast<float>(i) * 4, w - 2 - static_cast<float>(i) * 3, 1.3f);
        break;
    case Pd:
    case PlugIns:
        g.drawRect(x + 4, y + 3, w - 6, h - 6, 1.2f);
        g.fillRect(x, r.getCentreY() - 0.6f, 4.f, 1.2f);
        if (kind == PlugIns) {
            g.fillRect(x + w - 2, y + 5, 2.f, 1.2f);
            g.fillRect(x + w - 2, y + h - 6, 2.f, 1.2f);
        }
        break;
    case Clips:
        g.drawRect(r.reduced(1), 1.2f);
        p.addTriangle(x + 5, y + 4, x + w - 4, r.getCentreY(), x + 5, y + h - 4);
        g.fillPath(p);
        break;
    case Samples:
        g.drawRect(r.reduced(1), 1.2f);
        for (int i = 0; i < 5; ++i)
            g.fillRect(x + 3.f + static_cast<float>(i) * 2.f, y + 4.f + static_cast<float>(i % 2) * 2,
                       1.f, h - 8 - static_cast<float>(i % 2) * 4);
        break;
    case Grooves:
        for (int i = 0; i < 2; ++i) {
            juce::Path wave;
            wave.startNewSubPath(x + 1, y + 5.f + static_cast<float>(i) * 4);
            wave.quadraticTo(x + w / 4, y + 2.f + static_cast<float>(i) * 4, x + w / 2,
                             y + 5.f + static_cast<float>(i) * 4);
            wave.quadraticTo(x + 3 * w / 4, y + 8.f + static_cast<float>(i) * 4, x + w - 1,
                             y + 5.f + static_cast<float>(i) * 4);
            g.strokePath(wave, juce::PathStrokeType(1.2f));
        }
        break;
    case Templates:
        g.drawRect(r.reduced(1), 1.2f);
        g.fillRect(x + 1, y + 5, w - 2, 1.2f);
        g.fillRect(x + w / 2, y + 5, 1.2f, h - 6);
        break;
    case Packs:
        g.drawRect(x + 1, y + 4, w - 5, h - 5, 1.2f);
        g.drawRect(x + 4, y + 1, w - 5, h - 5, 1.2f);
        break;
    case User:
        g.drawEllipse(r.getCentreX() - 3, y + 1, 6, 6, 1.2f);
        p.addCentredArc(r.getCentreX(), y + h, w / 2 - 1, h / 2 - 1, 0,
                        -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
        g.strokePath(p, juce::PathStrokeType(1.2f));
        break;
    case Project:
        g.drawRect(r.reduced(1), 1.2f);
        for (int i = 0; i < 2; ++i)
            g.fillRect(x + 4, y + 5.f + static_cast<float>(i) * 3, w - 7, 1.f);
        break;
    case AddFolder:
        g.drawRect(r.reduced(1), 1.2f);
        g.fillRect(r.getCentreX() - 3, r.getCentreY() - 0.6f, 6.f, 1.2f);
        g.fillRect(r.getCentreX() - 0.6f, r.getCentreY() - 3, 1.2f, 6.f);
        break;
    case Ai: {
        // The sheet's four-pointed spark, in the accent; dark on the lit label.
        g.setColour(ink.value_or(look::accent));
        const float cx = r.getCentreX(), cy = r.getCentreY(), reach = w / 2.f - 1.f, waist = 1.6f;
        p.startNewSubPath(cx, cy - reach);
        p.lineTo(cx + waist, cy - waist);
        p.lineTo(cx + reach, cy);
        p.lineTo(cx + waist, cy + waist);
        p.lineTo(cx, cy + reach);
        p.lineTo(cx - waist, cy + waist);
        p.lineTo(cx - reach, cy);
        p.lineTo(cx - waist, cy - waist);
        p.closeSubPath();
        g.fillPath(p);
        break;
    }
    default:
        break;
    }
}
} // namespace
BrowserPanel::BrowserPanel(AdiRootComponent &root) : root_(root) {
    // Live's sidebar, its names and order (manual 3.3). "Max for Live" is ADI's Pd tier
    // (Master Reference 9.3), Live's place for the same thing.
    sidebar_ = {{"Collections", None, true}, {"Favorites", Favorite}, {{}, None, true},
                {"Categories", None, true}, {"All", All}, {"Sounds", Sounds},
                {"Drums", Drums},
                {"Instruments", Instruments}, {"Audio Effects", AudioFx},
                {"Pd Devices", Pd}, {"Plug-Ins", PlugIns}, {"Samples", Samples, false, true},
                {{}, None, true}, {"Places", None, true}, {"User Library", User},
                {"Add Folder...", AddFolder}};
    setTitle("Browser");
    setTooltip("Live's browser: your sounds, devices, plug-ins and files.");
    setWantsKeyboardFocus(true);
#if JUCE_MAC
    search.setTextToShowWhenEmpty("Search (Cmd + F)", look::textFaint);
#else
    search.setTextToShowWhenEmpty("Search (Ctrl + F)", look::textFaint);
#endif
    search.setTitle("Search");
    search.setFont(look::font(look::fontHeading));
    search.setIndents(8, 4);
    search.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    search.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(search);
    addChildComponent(searchClear_);
    searchClear_.onClick = [this] { clearSearch(); };
    baseEntries_ = sidebar_.size();
    // The search narrows the list as it is typed; Enter or the down arrow goes into it (§41.4.1.6).
    search.onTextChange = [this] { searchEdited(); };
    search.onReturnKey = [this] { enterList(); };
    search.addKeyListener(&searchKeys_);
    list.setTitle("Browser content");
    list.setModel(this);
    list.setRowHeight(kRowHeight);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setMultipleSelectionEnabled(true);
    list.onFolderArrow = [this](int row, bool right) { return folderArrow(row, right); };
    list.onLeft = [this] { focusIfShowing(*this); };
    list.onEscape = [this] { return cancelHotSwap(); };
    list.onPreviewKey = [this](const juce::KeyPress &k) { return previewKey(k); };
    list.onFavoriteKey = [this](const juce::KeyPress &key) {
        const auto command = root_.commands().manager.getKeyMappings()->findCommandForKeyPress(key);
        return command >= AppCommands::ToggleFavorite && command <= AppCommands::ToggleCollectionGray &&
               root_.command(command);
    };

    addAndMakeVisible(list);
    appendFavoritesToMenu = [this](juce::PopupMenu &menu, const Row &row) { appendContextFavorites(menu, row); };
    previewToggle.onClick = [this] { root_.command(AppCommands::ToggleBrowserPreview); };
    addChildComponent(previewToggle);
    selected_ = findLabel(kFirstLabel);
    refill();
}
BrowserPanel::SearchClearButton::SearchClearButton() : juce::Button("Clear Search") {
    setTitle("Clear Search");
    setTooltip("Clear the Browser search.");
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
}
void BrowserPanel::SearchClearButton::paintButton(juce::Graphics &g, bool over, bool down) {
    const auto centre = getLocalBounds().toFloat().getCentre();
    constexpr float radius = 3.5f;
    g.setColour(down ? look::textBright : over ? look::text : look::textDim);
    g.drawLine(centre.x - radius, centre.y - radius, centre.x + radius, centre.y + radius, 1.2f);
    g.drawLine(centre.x + radius, centre.y - radius, centre.x - radius, centre.y + radius, 1.2f);
}
void BrowserPanel::syncSearchClearButton() {
    auto field = search.getBounds();
    const bool shown = !search.isEmpty() && field.getWidth() >= 2 * kSearchClearSlot;
    searchClear_.setBounds(shown ? field.removeFromRight(kSearchClearSlot).reduced(1) : juce::Rectangle<int>{});
    searchClear_.setVisible(shown);
    auto border = searchBorder_;
    if (shown)
        border.setRight(border.getRight() + kSearchClearSlot);
    if (search.getBorder() != border)
        search.setBorder(border);
}
void BrowserPanel::searchEdited() {
    clearRevealedSample();
    syncSearchClearButton();
    refill();
    startSearchPluginScan();
}
bool BrowserPanel::clearSearch() {
    if (search.isEmpty())
        return false;
    search.setText({}, false);
    searchEdited();
    return true;
}
BrowserPanel::PreviewToggle::PreviewToggle() : juce::Button("Browser Preview") {
    setTitle("Browser Preview");
    setTooltip("Automatically preview selected audio files through Cue Out.");
}
void BrowserPanel::PreviewToggle::paintButton(juce::Graphics &g, bool over, bool down) {
    const auto ring = juce::Rectangle<float>(11, 11).withCentre(getLocalBounds().getCentre().toFloat());
    const auto state = controls::stateOf(getToggleState(), over, down, isEnabled());
    controls::roundButton(g, ring, state);
    g.setColour(getToggleState() ? look::ink : look::focus);
    g.drawEllipse(ring, 2.f);
    if (hasKeyboardFocus(false)) {
        g.setColour(look::accent);
        g.drawRect(getLocalBounds().reduced(1), 1);
    }
}
const LibraryFile *BrowserPanel::selectedSample() const noexcept {
    const int row = list.getLastRowSelected();
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
        return nullptr;
    const auto *file = rows_[static_cast<std::size_t>(row)].file;
    return file && file->isSample() ? file : nullptr;
}
void BrowserPanel::syncPreviewToggle() {
    const bool enabled = root_.commands().browserPreviewEnabled();
    if (previewToggle.getToggleState() != enabled)
        previewToggle.setToggleState(enabled, juce::dontSendNotification);
}
bool BrowserPanel::previewKey(const juce::KeyPress &key) {
    const auto &commands = root_.commands();
    if (commands.browserPreviewEnabled() || !selectedSample() || !root_.playSamplePreview ||
        commands.manager.getKeyMappings()->findCommandForKeyPress(key) != AppCommands::PreviewSelected)
        return false;
    root_.command(AppCommands::PreviewSelected);
    return true;
}
bool BrowserPanel::previewSelected(double fraction) {
    if (const auto *file = selectedSample(); file && root_.playSamplePreview) {
        canRepeatPreview_ = false;
        root_.playSamplePreview(file->path, fraction);
        return true;
    }
    return false;
}
void BrowserPanel::updatePreviewSelection() {
    const auto *file = selectedSample();
    if (file && root_.playSamplePreview)
        setPreviewRowShown(true);
    const auto path = file ? file->path : std::filesystem::path{};
    if (path == previewSelection_)
        return;
    previewSelection_ = path;
    setPreviewDisplay({}, {}, false);
    if (root_.selectSamplePreview)
        root_.selectSamplePreview(path);
    if (file && root_.commands().browserPreviewEnabled())
        root_.command(AppCommands::PreviewSelected);
    root_.commands().manager.commandStatusChanged();
}
void BrowserPanel::setPreviewRowShown(bool show) {
    if (std::exchange(previewRowShown_, show) != show) {
        resized();
        repaint();
    }
}
void BrowserPanel::setPreviewDisplay(std::shared_ptr<const PreviewPeaks> peaks, std::optional<double> position,
                                     bool canRepeat) {
    canRepeatPreview_ = canRepeat;
    if (peaks != previewPeaks_ || position != previewPosition_) {
        previewPeaks_ = std::move(peaks);
        previewPosition_ = position;
        repaint(previewWaveformArea());
    }
}
void BrowserPanel::enterList() {
    if (firstItem() >= 0) {
        if (list.getSelectedRows().isEmpty())
            list.selectRow(firstItem());
        focusIfShowing(list);
    }
}
int BrowserPanel::firstItem() const {
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].heading.isEmpty())
            return static_cast<int>(i);
    return -1;
}
void BrowserPanel::selectedRowsChanged(int row) {
    if (restoringListSelection_)
        return;
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].heading.isEmpty()) {
        lastSelected_ = row;
        updatePreviewSelection();
        if (labelSettings_)
            root_.commands().manager.commandStatusChanged();
        return;
    }
    // Past the heading, the way the selection moved; at the list's top, down to the first entry.
    const int step = row < lastSelected_ ? -1 : 1;
    for (int r = row + step; r >= 0 && r < static_cast<int>(rows_.size()); r += step)
        if (rows_[static_cast<std::size_t>(r)].heading.isEmpty()) {
            list.selectRow(r);
            return;
        }
    for (int r = row - step; r >= 0 && r < static_cast<int>(rows_.size()); r -= step)
        if (rows_[static_cast<std::size_t>(r)].heading.isEmpty()) {
            list.selectRow(r);
            return;
        }
    list.deselectAllRows();
}
bool BrowserPanel::SearchKeys::keyPressed(const juce::KeyPress &k, juce::Component *) {
    if (k == juce::KeyPress::escapeKey)
        return panel.cancelHotSwap() || panel.clearSearch();
    if (k != juce::KeyPress::downKey)
        return false;
    panel.enterList();
    return true;
}
juce::String BrowserPanel::dragDescription(const std::vector<const LibraryFile *> &files) {
    juce::StringArray paths;
    // The native spelling, as a file dragged from the desktop arrives: JUCE names a file by its last
    // native separator, so a generic path on Windows would name the clip after the whole path.
    for (const auto *file : files)
        if (file->isSample())
            paths.add(nativeFilePath(file->path));
    return paths.isEmpty() ? juce::String{} : "adi-sample:" + paths.joinIntoString("\n");
}
juce::StringArray BrowserPanel::droppedFiles(const juce::var &description) {
    if (!description.isString())
        return {};
    const auto text = description.toString();
    if (!text.startsWith("adi-sample:"))
        return {};
    return juce::StringArray::fromLines(text.fromFirstOccurrenceOf("adi-sample:", false, false));
}
void BrowserPanel::setListing(LibraryListing listing, bool ready) {
    setLibrarySnapshot(std::make_shared<const BrowserLibrarySnapshot>(std::move(listing)), ready);
}
const LibraryListing &BrowserPanel::listing() const noexcept {
    static const LibraryListing empty;
    return library_ ? library_->listing() : empty;
}
std::string BrowserPanel::labelKey(int index) const {
    if (index < 0 || index >= static_cast<int>(sidebar_.size()))
        return {};
    const auto i = static_cast<std::size_t>(index);
    const auto &entry = sidebar_[i];
    if (entry.heading || entry.glyph == AddFolder)
        return {};
    return i < baseEntries_ - 1 ? "label:" + entry.name.toStdString() : "place:" + entry.place;
}
int BrowserPanel::findLabel(std::string_view key) const {
    if (!key.empty())
        for (int i = 0; i < static_cast<int>(sidebar_.size()); ++i)
            if (labelKey(i) == key)
                return i;
    return -1;
}
void BrowserPanel::restoreLabel(settings::AppSettings &settings) {
    labelSettings_ = &settings;
    loadFavoriteMarks();
    const auto saved = settings.get(kLastLabel).get<std::string>();
    selected_ = findLabel(saved.empty() ? kFirstLabel : std::string_view(saved));
    pendingLabel_.clear();
    if (selected_ < 0) {
        if (!ready_ && saved.starts_with("place:"))
            pendingLabel_ = saved;
        selected_ = findLabel(kAllLabel);
    }
    revealSidebarSelection();
    refill();
    repaint();
}
void BrowserPanel::setLibrarySnapshot(std::shared_ptr<const BrowserLibrarySnapshot> library, bool ready) {
    ready_ = ready;
    if (library_ == library) {
        if (ready_) pendingLabel_.clear();
        return;
    }
    const auto priorLabel = labelKey(selected_);
    const auto keep = pendingLabel_.empty() ? labelKey(selected_) : pendingLabel_;
    const bool keepInView = selected_ >= 0 && sidebarArea_.intersects(sidebarRowBounds(selected_));
    // The old rows still borrow these sample and folder identities until the refill finishes.
    const auto priorLibrary = std::exchange(library_, std::move(library));
    updateMarkedSamples();
    // Live's added folders stand among the Places, after Current Project and before Add Folder; the
    // User Library's own label is Live's.
    auto addFolder = std::move(sidebar_.back());
    sidebar_.resize(baseEntries_ - 1);
    for (auto &e : sidebar_)
        if (e.name == "User Library" && !e.heading)
            e.place.clear();
    for (const auto &place : listing().places) {
        if (place.starterDrums)
            continue;
        if (place.userLibrary) {
            for (auto &e : sidebar_)
                if (e.name == "User Library" && !e.heading)
                    e.place = place.volumeId;
            continue;
        }
        sidebar_.push_back({juce::String::fromUTF8(place.name.c_str()), User, false, false, place.volumeId});
    }
    sidebar_.push_back(addFolder);
    selected_ = findLabel(keep);
    if (ready_ || selected_ >= 0)
        pendingLabel_.clear();
    if (selected_ < 0)
        selected_ = findLabel(kAllLabel);
    setSidebarScroll(sidebarScroll_);
    if (keepInView)
        revealSidebarSelection();
    if (revealedSample_ && !hotSwap_)
        selectRevealedSampleLabel();
    const bool sameLabel = labelKey(selected_) == priorLabel;
    refill(-1, hotSwap_ || sameLabel || revealedSample_.has_value());
    if (!hotSwap_ && !sameLabel && !revealedSample_)
        setPreviewRowShown(false);
    repaint();
}
bool BrowserPanel::setCurrentProjectLabelVisible(bool visible) {
    const int currentProject = findLabel(kCurrentProjectLabel);
    if (visible == (currentProject >= 0))
        return false;
    const auto key = labelKey(selected_);
    const bool inView = selected_ >= 0 && sidebarArea_.intersects(sidebarRowBounds(selected_));
    if (visible) {
        sidebar_.insert(sidebar_.begin() + static_cast<std::ptrdiff_t>(baseEntries_ - 1), {"Current Project", Project});
        ++baseEntries_;
    } else {
        sidebar_.erase(sidebar_.begin() + currentProject);
        --baseEntries_;
    }
    selected_ = findLabel(key);
    const bool selectionRemoved = selected_ < 0;
    if (selectionRemoved) {
        selected_ = findLabel(kAllLabel);
        setPreviewRowShown(false);
    }
    setSidebarScroll(sidebarScroll_);
    if (inView)
        revealSidebarSelection();
    repaint();
    return selectionRemoved;
}
void BrowserPanel::selectRevealedSampleLabel() {
    const auto *indexed = library_ ? library_->matchingFileAtPath(revealedSample_->path, LibraryQuery{""}) : nullptr;
    const bool inSamples = indexed && indexed->isSample();
    setCurrentProjectLabelVisible(projectLibrary_ || !inSamples);
    const int label = findLabel(inSamples ? kSamplesLabel : kCurrentProjectLabel);
    if (selected_ != label) {
        selected_ = label;
        revealSidebarSelection();
    }
}
void BrowserPanel::clearRevealedSample() {
    if (revealedSample_) {
        revealedSample_.reset();
        if (!projectLibrary_)
            setCurrentProjectLabelVisible(false);
    }
}
void BrowserPanel::revealSample(std::filesystem::path path) {
    const SafePointer<BrowserPanel> alive(this);
    cancelHotSwap();
    if (!alive || hotSwap_ || path.empty())
        return;
    const auto priorLabel = labelKey(selected_);
    revealedSample_ = LibraryFile{media::pathUtf8(path.filename()), std::move(path), {}};
    addFolderFocused_ = false;
    pendingLabel_.clear();
    selectRevealedSampleLabel();
    revealSidebarSelection();
    if (priorLabel != labelKey(selected_))
        setPreviewRowShown(false);
    search.setText({}, false);
    syncSearchClearButton();
    refill();
    const auto found = std::ranges::find_if(rows_, [this](const Row &row) {
        return row.file && row.file->path == revealedSample_->path;
    });
    if (found != rows_.end())
        list.selectRow(static_cast<int>(found - rows_.begin()));
    repaint();
}
void BrowserPanel::setCurrentProjectSnapshot(std::shared_ptr<const BrowserLibrarySnapshot> library) {
    if (projectLibrary_ == library)
        return;
    // Keep the previous publication alive until the rows that borrowed it have been cleared.
    const auto previous = std::exchange(projectLibrary_, std::move(library));
    updateMarkedSamples();
    bool keepListPosition = true;
    if (revealedSample_ && !hotSwap_)
        selectRevealedSampleLabel();
    else if (!revealedSample_ && static_cast<bool>(previous) != static_cast<bool>(projectLibrary_))
        keepListPosition = !setCurrentProjectLabelVisible(static_cast<bool>(projectLibrary_));
    if (!hotSwap_ && selected_ >= 0) {
        const auto glyph = sidebar_[static_cast<std::size_t>(selected_)].glyph;
        if (glyph == Project || glyph == All || glyph == Favorite)
            refill(-1, keepListPosition);
    }
}
void BrowserPanel::setDevicePresets(std::vector<preset::Entry> entries) {
    devicePresets_.clear();
    for (auto &entry : entries)
        devicePresets_[entry.deviceKey].push_back(std::move(entry));
    for (auto &[key, children] : devicePresets_) {
        (void)key;
        std::sort(children.begin(), children.end(), presetNameLess);
    }
    refill();
}
void BrowserPanel::setHotSwap(std::optional<HotSwapFilter> filter) {
    if (!hotSwap_ && !filter)
        return;
    if (!hotSwap_) {
        idleSearch_ = search.getText();
        search.setText(swapSearch_, false);
    } else if (!filter) {
        swapSearch_ = search.getText();
        search.setText(std::exchange(idleSearch_, {}), false);
    }
    hotSwap_ = std::move(filter);
    syncSearchClearButton();
    list.setMultipleSelectionEnabled(!hotSwap_);
    refill();
    setPreviewRowShown(false);
    repaint();
    if (hotSwap_ && !hotSwap_->samples && scanPlugins && pluginScan_.phase == PluginScan::Phase::NotStarted)
        scanPlugins();
}
bool BrowserPanel::cancelHotSwap() {
    if (!hotSwap_)
        return false;
    if (leaveHotSwap)
        leaveHotSwap();
    else
        setHotSwap(std::nullopt);
    return true;
}
void BrowserPanel::appendPlaceRows(std::span<const BrowserLibrarySnapshot::Folder> folders, std::size_t folder, int depth,
                                   const std::set<std::filesystem::path> &open) {
    const auto &parent = folders[folder];
    for (const auto child : parent.children) {
        const auto &directory = folders[child];
        const auto expanded = open.contains(directory.path);
        rows_.push_back({nullptr, nullptr, {}, nullptr, depth, nullptr, {}, &directory, expanded});
        if (expanded)
            appendPlaceRows(folders, child, depth + 1, open);
    }
    for (const auto index : parent.files) {
        const auto *file = &listing().files[index];
        shown_.push_back(file);
        rows_.push_back({file, nullptr, {}, nullptr, depth});
    }
}
bool BrowserPanel::favoritesShown() const noexcept {
    return !hotSwap_ && selected_ >= 0 && sidebar_[static_cast<std::size_t>(selected_)].glyph == Favorite;
}
const BrowserPanel::Row *BrowserPanel::favoriteSelection() const noexcept {
    const int index = list.getLastRowSelected();
    if (!labelSettings_ || index < 0 || static_cast<std::size_t>(index) >= rows_.size())
        return nullptr;
    const auto &row = rows_[static_cast<std::size_t>(index)];
    return (row.file && row.file->isSample()) || row.device || row.plugin ? &row : nullptr;
}
bool BrowserPanel::canMarkSelection() const noexcept {
    return favoriteSelection() != nullptr;
}
std::string BrowserPanel::favoriteKey(const Row &row) {
    if (row.file && row.file->isSample())
        return std::string(kSampleMarkPrefix) + media::pathUtf8(row.file->path);
    if (row.device)
        return "device:" + std::string(row.device->uid);
    if (row.plugin) {
        const auto &plugin = *row.plugin;
        const auto shell = plugin.shellId ? settings::Value(*plugin.shellId) : settings::Value();
        return "plugin:" + settings::Value::array({plugin.format, plugin.uid, shell}).dump();
    }
    return {};
}
unsigned BrowserPanel::favoriteMarks(const Row &row) const {
    if (row.file && row.file->isSample()) {
        const auto found = sampleMarks_.find(row.file->path);
        return found == sampleMarks_.end() ? 0u : found->second;
    }
    if (favoriteMarks_.empty())
        return 0;
    const auto found = favoriteMarks_.find(favoriteKey(row));
    return found == favoriteMarks_.end() ? 0u : found->second;
}
void BrowserPanel::loadFavoriteMarks() {
    favoriteMarks_.clear();
    sampleMarks_.clear();
    const auto snapshot = labelSettings_->get(settings::kBrowserFavoriteMarksKey);
    for (const auto &[key, value] : snapshot.items()) {
        const auto marks = value.get<unsigned>();
        if (key.starts_with(kSampleMarkPrefix))
            sampleMarks_.emplace(media::pathFromUtf8(key.substr(kSampleMarkPrefix.size())), marks);
        else
            favoriteMarks_.emplace(key, marks);
    }
    updateMarkedSamples();
}
void BrowserPanel::updateMarkedSamples() {
    markedSamples_.clear();
    if (sampleMarks_.empty())
        return;
    std::unordered_set<std::filesystem::path, LibraryPathHash> projectMarks;
    if (projectLibrary_)
        for (const auto &sample : projectLibrary_->listing().files)
            if (sample.isSample() && sampleMarks_.contains(sample.path)) {
                markedSamples_.push_back(&sample);
                projectMarks.insert(sample.path);
            }
    for (const auto &sample : listing().files)
        if (sample.isSample() && sampleMarks_.contains(sample.path) && !projectMarks.contains(sample.path))
            markedSamples_.push_back(&sample);
}
bool BrowserPanel::toggleSelectionMark(unsigned slot) {
    const auto *row = favoriteSelection();
    return row && toggleFavoriteMark(favoriteKey(*row), slot);
}
bool BrowserPanel::toggleFavoriteMark(const std::string &identity, unsigned slot) {
    if (!labelSettings_ || identity.empty() || slot > kCollectionColours.size())
        return false;
    auto snapshot = labelSettings_->get(settings::kBrowserFavoriteMarksKey);
    const auto before = snapshot.value(identity, 0u);
    const auto next = before ^ (1u << slot);
    if (next == 0)
        snapshot.erase(identity);
    else
        snapshot[identity] = next;
    std::string error;
    if (!labelSettings_->set(settings::kBrowserFavoriteMarksKey, snapshot, {}, error)) {
        root_.say(std::move(error));
        return true;
    }
    loadFavoriteMarks();
    if (favoritesShown() && ((before == 0) != (next == 0)))
        refill(-1, true);
    else
        list.repaint();
    root_.commands().manager.commandStatusChanged();
    if (!labelSettings_->save(error))
        root_.say(std::move(error));
    return true;
}
void BrowserPanel::appendContextFavorites(juce::PopupMenu &menu, const Row &row) {
    if (!labelSettings_)
        return;
    const auto key = favoriteKey(row);
    if (key.empty())
        return;
    const auto marks = favoriteMarks(row);
    const auto toggle = [owner = SafePointer<BrowserPanel>(this), key](unsigned slot) {
        if (owner)
            owner->toggleFavoriteMark(key, slot);
    };
    const auto *keys = root_.commands().manager.getKeyMappings();
    const auto add = [&](juce::PopupMenu &to, juce::PopupMenu::Item item, unsigned slot) {
        const auto shortcuts = keys->getKeyPressesAssignedToCommand(AppCommands::ToggleFavorite + static_cast<int>(slot));
        if (!shortcuts.isEmpty())
            item.shortcutKeyDescription = shortcuts[0].getTextDescriptionWithIcons();
        to.addItem(std::move(item).setAction([toggle, slot] { toggle(slot); }));
    };
    const bool favorite = (marks & (1u << kFavoriteSlot)) != 0;
    add(menu, juce::PopupMenu::Item(favorite ? "Remove from Favorites" : "Add to Favorites"), kFavoriteSlot);
    juce::PopupMenu colours;
    for (std::size_t i = 0; i < kCollectionColours.size(); ++i) {
        add(colours, juce::PopupMenu::Item(kCollectionColours[i].name)
                         .setColour(collectionColour(i)).setTicked((marks & (2u << i)) != 0),
            static_cast<unsigned>(i) + 1u);
    }
    menu.addSubMenu("Collection Colors", colours);
}
void BrowserPanel::refillFavorites(const LibraryQuery &query) {
    rows_.clear();
    for (const auto *sample : markedSamples_)
        if (query.matches(sample->name)) {
            shown_.push_back(sample);
            rows_.push_back({sample, nullptr, {}});
        }
    if (favoriteMarks_.empty())
        return;
    for (const auto &device : builtinCatalog()) {
        Row row{nullptr, &device, {}};
        if (favoriteMarks(row) && query.matches(device.name))
            rows_.push_back(row);
    }
    for (const auto &plugin : pluginScan_.listing.plugins) {
        Row row{nullptr, nullptr, {}, &plugin};
        if (favoriteMarks(row) && query.matches(plugin.name, plugin.vendor))
            rows_.push_back(row);
    }
}
namespace {
const std::filesystem::path *browserRowPath(const BrowserPanel::Row &row) {
    if (row.file) return &row.file->path;
    if (row.folder) return &row.folder->path;
    if (row.savedPreset) return &row.savedPreset->path;
    return nullptr;
}
const void *browserCatalogueRow(const BrowserPanel::Row &row) {
    return row.device ? static_cast<const void *>(row.device) : row.plugin;
}
struct BrowserListPosition {
    std::unordered_set<std::filesystem::path, LibraryPathHash> paths;
    std::unordered_set<const void *> catalogue;
    const std::filesystem::path *primaryPath = nullptr;
    const void *primaryCatalogue = nullptr;
    juce::Point<int> scroll;
};
} // namespace
void BrowserPanel::refill(int selectRow, bool keepListPosition) {
    // The AI mode's pane takes the list's place.
    list.setVisible(!agentChosen());
    std::optional<BrowserListPosition> previous;
    if (keepListPosition) {
        auto &position = previous.emplace();
        position.scroll = list.getViewport()->getViewPosition();
        const int primary = list.getLastRowSelected();
        const auto selection = list.getSelectedRows();
        for (int i = 0; i < selection.getNumRanges(); ++i) {
            const auto range = selection.getRange(i);
            for (int row = range.getStart(); row < range.getEnd(); ++row) {
                const auto &item = rows_[static_cast<std::size_t>(row)];
                if (const auto *path = browserRowPath(item)) {
                    position.paths.insert(*path);
                    if (row == primary) position.primaryPath = path;
                } else if (const auto *catalogue = browserCatalogueRow(item)) {
                    position.catalogue.insert(catalogue);
                    if (row == primary) position.primaryCatalogue = catalogue;
                }
            }
        }
    }
    refillRows();
    sortRows();
    if (previous) {
        juce::SparseSet<int> selected;
        int primary = -1;
        if (!previous->paths.empty() || !previous->catalogue.empty())
            for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
                const auto &item = rows_[static_cast<std::size_t>(row)];
                const auto *path = browserRowPath(item);
                const auto *catalogue = browserCatalogueRow(item);
                if ((path && previous->paths.contains(*path)) || (catalogue && previous->catalogue.contains(catalogue))) {
                    selected.addRange({row, row + 1});
                    if ((path && previous->primaryPath && *path == *previous->primaryPath) ||
                        (catalogue && catalogue == previous->primaryCatalogue))
                        primary = row;
                }
            }
        {
            const juce::ScopedValueSetter restoring(restoringListSelection_, true);
            list.updateContent();
            if (primary >= 0) list.selectRow(primary, true);
            list.setSelectedRows(selected, juce::dontSendNotification);
            lastSelected_ = list.getLastRowSelected();
        }
        updatePreviewSelection();
        list.getViewport()->setViewPosition(previous->scroll);
    } else {
        list.updateContent();
        list.deselectAllRows();
        lastSelected_ = -1;
        if (selectRow >= 0)
            list.selectRow(selectRow);
    }
    list.repaint();
}
void BrowserPanel::refillRows() {
    shown_.clear();
    const Entry none{};
    const auto &chosen = !hotSwap_ && selected_ >= 0 ? sidebar_[static_cast<std::size_t>(selected_)] : none;
    const bool all = chosen.glyph == All;
    const bool sounds = chosen.glyph == Sounds;
    const bool swappingDevices = hotSwap_ && !hotSwap_->samples;
    const bool showPlugins = swappingDevices || all || chosen.glyph == PlugIns;
    using Role = ::adi::detail::DeviceRole;
    const auto swapRole = swappingDevices ? ::adi::detail::deviceRole(hotSwap_->subtype) : Role::AudioEffect;
    const LibraryQuery query{search.getText().toStdString()};
    if (favoritesShown()) {
        refillFavorites(query);
        return;
    }
    // Keep the typed results separate from how a panel groups them for display.
    const bool samples = hotSwap_ ? hotSwap_->samples : chosen.samples;
    const bool browsePlace = !samples && query.empty() && !chosen.place.empty();
    LibraryQueryResults hits;
    if (all) {
        hits.files.swap(shown_);
        queryLibrary(query, {library_.get(), builtinCatalog(), pluginScan_.listing.plugins, &devicePresets_,
                             projectLibrary_.get()}, hits);
        hits.files.swap(shown_);
    } else if (chosen.glyph == Project) {
        if (projectLibrary_)
            projectLibrary_->appendSamples(query, nullptr, shown_);
        if (revealedSample_ && std::ranges::none_of(shown_, [this](const LibraryFile *file) {
                return file->path == revealedSample_->path;
            }))
            shown_.push_back(&*revealedSample_);
    } else if (sounds) {
        queryLibrary(query, {nullptr, {}, {}, &devicePresets_}, hits);
        std::ranges::sort(hits.presets, presetNameLess, [](const preset::Entry *entry) -> const preset::Entry & {
            return *entry;
        });
    } else if (library_ && samples) {
        library_->appendSamples(query, nullptr, shown_);
    } else if (library_ && !browsePlace && !chosen.place.empty()) {
        library_->appendFiles(query, &chosen.place, shown_);
    }
    if (library_ && chosen.glyph == Drums) {
        static const std::string drumVolume{kStarterDrumsVolumeId};
        library_->appendSamples(query, &drumVolume, shown_);
    }
    std::unordered_set<const preset::Entry *> unusedPresets(hits.presets.begin(), hits.presets.end());
    rows_.clear();
    rows_.reserve(shown_.size());
    if (showPlugins && pluginScan_.phase == PluginScan::Phase::Scanning)
        rows_.push_back({nullptr, nullptr, juce::String::fromUTF8(pluginScanStatus(pluginScan_).c_str())});
    if (browsePlace && library_) {
        const auto folders = library_->folders(chosen.place);
        if (!folders.empty())
            appendPlaceRows(folders, 0, 0, openFolders_[chosen.place]);
    } else {
        for (const auto *file : shown_)
            rows_.push_back({file, nullptr, {}});
    }
    // Audio Effects in Live's folders (manual 4.10), Pd Devices ADI's own, from the catalogue.
    const auto visiblePresets = [&](std::string_view format, std::string_view uid,
                                    std::optional<std::int64_t> shellId, bool entireDevice) {
        std::vector<const preset::Entry *> out;
        if (!devicePresets_.empty()) {
            const auto found = devicePresets_.find(preset::deviceKey({std::string(format), std::string(uid), shellId}));
            if (found != devicePresets_.end())
                for (const auto &entry : found->second)
                    if (entireDevice || (all ? unusedPresets.contains(&entry) : query.matches(entry.name)))
                        out.push_back(&entry);
        }
        return out;
    };
    const auto addPresets = [&](const std::vector<const preset::Entry *> &presets, int depth) {
        for (const auto *entry : presets) {
            unusedPresets.erase(entry);
            rows_.push_back({nullptr, nullptr, {}, nullptr, depth, entry});
        }
    };
    const auto addBuiltin = [&](const BuiltinInfo &d, int depth, const char *heading = nullptr) {
        if (swappingDevices && (!d.available || swapRole != (d.instrument ? Role::Instrument : Role::AudioEffect)))
            return false;
        const bool ownMatch = all ? std::ranges::binary_search(hits.builtins, &d) : query.matches(d.name);
        const auto presets = visiblePresets("internal", d.uid, std::nullopt, ownMatch);
        if (!ownMatch && presets.empty())
            return false;
        if (heading != nullptr)
            rows_.push_back({nullptr, nullptr, heading});
        rows_.push_back({nullptr, &d, {}, nullptr, depth});
        addPresets(presets, depth + 1);
        return true;
    };
    if (swappingDevices || all || chosen.glyph == AudioFx) {
        for (const char *folder : {"EQ & Filters", "Delay & Loop", "Dynamics", "Drive & Color", "Pitch & Modulation",
                                   "Reverb & Resonance", "Utilities"}) {
            std::vector<const BuiltinInfo *> in;
            for (const auto &d : builtinCatalog())
                if (d.category == "Audio Effects" && d.group == folder)
                    in.push_back(&d);
            std::sort(in.begin(), in.end(), [](const BuiltinInfo *a, const BuiltinInfo *b) { return a->name < b->name; });
            const char *heading = folder;
            for (const auto *d : in)
                if (addBuiltin(*d, 1, heading))
                    heading = nullptr;
        }
    }
    if (swappingDevices || all || chosen.glyph == Instruments) {
        // ADI's own instruments, by name, where Live lists its instruments (walk-A A7, DF-W48).
        std::vector<const BuiltinInfo *> in;
        for (const auto &d : builtinCatalog())
            if (d.category == "Instruments")
                in.push_back(&d);
        std::sort(in.begin(), in.end(), [](const BuiltinInfo *a, const BuiltinInfo *b) { return a->name < b->name; });
        for (const auto *d : in)
            addBuiltin(*d, 0);
    }
    if (swappingDevices || all || chosen.glyph == Pd) {
        for (const auto &d : builtinCatalog())
            if (d.category == "ADI")
                addBuiltin(d, 0);
    }
    if (showPlugins) {
        // Live's Plug-Ins: a folder per format, in it a folder per vendor, in that its plug-ins by name.
        std::string format;
        for (const auto &group : pluginGroups(pluginScan_.listing, {})) {
            bool showVendor = true;
            for (const auto *p : group.entries) {
                if (swappingDevices) {
                    using K = PluginEntry::Kind;
                    if (!((p->kind == K::Instrument && swapRole == Role::Instrument) ||
                          (p->kind == K::MidiEffect && swapRole == Role::MidiEffect) ||
                          (p->kind == K::Effect && swapRole == Role::AudioEffect)))
                        continue;
                }
                const bool ownMatch = all ? std::ranges::binary_search(hits.plugins, p)
                                          : query.matches(p->name, p->vendor);
                const auto presets = visiblePresets(p->format, p->uid, p->shellId, ownMatch);
                if (!ownMatch && presets.empty())
                    continue;
                if (showVendor) {
                    if (group.format != format) {
                        format = group.format;
                        rows_.push_back({nullptr, nullptr, juce::String(formatFolder(format)), nullptr, 0});
                    }
                    rows_.push_back({nullptr, nullptr, juce::String::fromUTF8(group.vendor.c_str()), nullptr, 1});
                    showVendor = false;
                }
                rows_.push_back({nullptr, nullptr, {}, p, 2});
                addPresets(presets, 3);
            }
        }
        for (const auto &failure : pluginScan_.listing.scanFailures) {
            const auto path = juce::String::fromUTF8(failure.path.c_str());
            const auto name = path.substring(path.lastIndexOfAnyOf("/\\") + 1);
            if (!query.matches(name.toStdString()))
                continue;
            const auto heading = (failure.state == scan::State::Quarantined ? "Blocked: "
                                 : failure.state == scan::State::UnreadablePath ? "Skipped: " : "Scan failed: ") + name;
            const auto reason = juce::String::fromUTF8(failure.error.c_str());
            const auto tooltip = reason.isEmpty() ? path : path + "\n" + reason;
            rows_.push_back({nullptr, nullptr, heading, nullptr, 0, nullptr, tooltip});
        }
    }
    for (const auto *entry : hits.presets)
        if (unusedPresets.contains(entry))
            rows_.push_back({nullptr, nullptr, {}, nullptr, 0, entry});
    if (hotSwap_ && rows_.empty())
        rows_.push_back({nullptr, nullptr, samples ? "No matching samples" : "No matching devices or presets"});
}
void BrowserPanel::sortRows() {
    if (!nameSortAscending_ || hotSwap_ || rows_.size() < 2)
        return;
    const auto count = rows_.size();
    std::vector<std::size_t> ends(count);
    std::vector<juce::String> names;
    names.reserve(rows_.size());
    for (const auto &row : rows_)
        names.push_back(rowName(row));
    // Each contiguous branch includes its more deeply indented descendants.
    for (auto index = count; index > 0;) {
        --index;
        auto end = index + 1;
        while (end < count && rows_[end].depth > rows_[index].depth)
            end = ends[end];
        ends[index] = end;
    }
    auto original = std::move(rows_);
    rows_.clear();
    rows_.reserve(original.size());
    shown_.clear();
    const auto append = [&](std::size_t index) {
        if (const auto *file = original[index].file)
            shown_.push_back(file);
        rows_.push_back(std::move(original[index]));
    };
    const auto appendSorted = [&](auto &&self, std::size_t first, std::size_t end) -> void {
        std::vector<std::size_t> siblings;
        for (auto index = first; index < end; index = ends[index])
            siblings.push_back(index);
        std::stable_sort(siblings.begin(), siblings.end(), [&](std::size_t a, std::size_t b) {
            const auto order = names[a].compareIgnoreCase(names[b]);
            return *nameSortAscending_ ? order < 0 : order > 0;
        });
        for (const auto index : siblings) {
            append(index);
            if (index + 1 < ends[index])
                self(self, index + 1, ends[index]);
        }
    };
    // Favorites has no scan progress row, even when it includes plug-ins.
    const bool scanNotice = !favoritesShown() && pluginsShown() && pluginScan_.phase == PluginScan::Phase::Scanning;
    if (scanNotice)
        append(0);
    appendSorted(appendSorted, scanNotice ? 1u : 0u, count);
}
int BrowserPanel::getNumRows() { return static_cast<int>(rows_.size()); }
juce::String BrowserPanel::getNameForRow(int row) {
    if (row >= 0 && row < getNumRows()) {
        const auto &item = rows_[static_cast<std::size_t>(row)];
        if (item.folder != nullptr)
            return juce::String::fromUTF8(item.folder->name.c_str()) + (item.folderOpen ? ", expanded" : ", collapsed");
        if (item.file != nullptr)
            return juce::String::fromUTF8(item.file->name.c_str());
    }
    return juce::ListBoxModel::getNameForRow(row);
}
juce::String BrowserPanel::getTooltipForRow(int row) {
    if (row < 0 || row >= getNumRows())
        return {};
    const auto &item = rows_[static_cast<std::size_t>(row)];
    if (item.file != nullptr)
        return juce::String::fromUTF8(media::pathUtf8(item.file->path).c_str());
    return item.tooltip;
}
void BrowserPanel::paintListBoxItem(int row, juce::Graphics &g, int width, int height, bool selected) {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return;
    const auto &r = rows_[static_cast<std::size_t>(row)];
    if (selected && r.heading.isEmpty())
        g.fillAll(look::selection);
    if (r.folder != nullptr) {
        const int in = kFolderIndent * r.depth;
        const float x = 8.f + static_cast<float>(in), y = static_cast<float>(height) / 2.f;
        juce::Path chevron;
        if (r.folderOpen)
            chevron.addTriangle(x, y - 2.f, x + 8.f, y - 2.f, x + 4.f, y + 3.f);
        else
            chevron.addTriangle(x + 2.f, y - 4.f, x + 2.f, y + 4.f, x + 7.f, y);
        g.setColour(look::text);
        g.fillPath(chevron);
        g.setFont(look::font(look::fontHeading));
        g.drawText(juce::String::fromUTF8(r.folder->name.c_str()), 24 + in, 0, width - 28 - in, height,
                   juce::Justification::centredLeft, true);
        return;
    }
    if (r.heading.isNotEmpty()) {
        // Group headings and notices cannot be selected or loaded.
        g.setColour(r.tooltip.isEmpty() ? look::textDim : look::error);
        g.setFont(look::font(look::fontField, true));
        const int in = 6 + 12 * r.depth;
        g.drawText(r.heading, in, 0, width - in - 4, height, juce::Justification::centredLeft, true);
        return;
    }
    if (r.savedPreset != nullptr) {
        const int in = 12 * r.depth;
        icon(g, Templates, {6.f + static_cast<float>(in), (static_cast<float>(height) - 13.f) / 2.f, 13.f, 13.f});
        const bool canLoad = hotSwap_ ? bool(swapChoice) : bool(loadDevicePreset);
        g.setColour(canLoad ? look::text : look::textDim);
        g.setFont(look::font(look::fontHeading));
        g.drawText(juce::String::fromUTF8(r.savedPreset->name.c_str()), 24 + in, 0, width - 28 - in, height,
                   juce::Justification::centredLeft, true);
        return;
    }
    if (r.plugin != nullptr) {
        // A plug-in, under its vendor: its kind's icon (a VST3's is not known yet: the plug-in icon), its
        // name dimmed where nothing can load it (outside the application).
        const int in = 12 * r.depth;
        using K = PluginEntry::Kind;
        const auto kind = r.plugin->kind;
        icon(g, kind == K::Instrument ? Instruments : kind == K::Effect ? AudioFx : kind == K::MidiEffect ? MidiFx : PlugIns,
             juce::Rectangle<float>(6.f + static_cast<float>(in), (static_cast<float>(height) - 13.f) / 2.f, 13.f, 13.f));
        const bool canLoad = hotSwap_ ? bool(swapChoice) : bool(root_.loadPlugin);
        g.setColour(canLoad ? look::text : look::textDim);
        g.setFont(look::font(look::fontHeading));
        const auto marks = favoriteMarks(r);
        g.drawText(juce::String::fromUTF8(r.plugin->name.c_str()), 24 + in, 0,
                   std::max(0, width - 28 - in - favoriteMarkWidth(marks)), height,
                   juce::Justification::centredLeft, true);
        paintFavoriteMarks(g, marks, width, height);
        return;
    }
    const bool device = r.device != nullptr;
    const int indent = kFolderIndent * r.depth;
    int glyph = Samples;
    if (device)
        glyph = r.device->category == "Instruments" ? Instruments : AudioFx;
    else if (r.file->kind == LibraryFile::Kind::Project)
        glyph = Project;
    else if (r.file->kind == LibraryFile::Kind::Midi)
        glyph = Sounds;
    icon(g, glyph,
         juce::Rectangle<float>(6.f + static_cast<float>(indent), (static_cast<float>(height) - 13.f) / 2.f, 13.f, 13.f));
    // An entry this build cannot load is dimmed (its reason is said when it is chosen).
    g.setColour(device && !unavailable(*r.device).empty() ? look::textDim : look::text);
    g.setFont(look::font(look::fontHeading));
    const auto name = device ? juce::String(std::string(r.device->name)) : juce::String::fromUTF8(r.file->name.c_str());
    const auto marks = favoriteMarks(r);
    g.drawText(name, 24 + indent, 0, std::max(0, width - 28 - indent - favoriteMarkWidth(marks)), height,
               juce::Justification::centredLeft, true);
    paintFavoriteMarks(g, marks, width, height);
}
juce::var BrowserPanel::getDragSourceDescription(const juce::SparseSet<int> &rows) {
    if (hotSwap_)
        return {};
    std::vector<const LibraryFile *> files;
    for (int i = 0; i < rows.size(); ++i)
        if (const int r = rows[i]; r >= 0 && r < static_cast<int>(rows_.size())) {
            const auto &row = rows_[static_cast<std::size_t>(r)];
            if (row.savedPreset != nullptr)
                return {};
            // Carry the catalogue identity. A scan may replace the rows while the drag is running.
            if (row.plugin != nullptr) {
                if (!root_.loadPlugin)
                    return {};
                return juce::var(juce::Array<juce::var>{"adi-plugin", juce::String(row.plugin->format),
                                                       juce::String(row.plugin->uid)});
            }
            // A device drags alone, the first one chosen; one this build cannot load drags nothing.
            if (row.device != nullptr)
                return unavailable(*row.device).empty() ? juce::var(deviceDescription(*row.device)) : juce::var();
            if (row.file != nullptr)
                files.push_back(row.file);
        }
    const auto description = dragDescription(files);
    return description.isEmpty() ? juce::var{} : juce::var(description);
}
std::string BrowserPanel::unavailable(const BuiltinInfo &d) {
    if (d.available)
        return {};
    return std::string(d.name) + " runs on ADI's Pd devices, which this build leaves out.";
}
juce::String BrowserPanel::deviceDescription(const BuiltinInfo &d) {
    return "adi-device:" + juce::String(std::string(d.uid));
}
std::string BrowserPanel::droppedDevice(const juce::var &description) {
    if (!description.isString())
        return {};
    const auto text = description.toString();
    if (!text.startsWith("adi-device:"))
        return {};
    const auto uid = text.fromFirstOccurrenceOf("adi-device:", false, false).toStdString();
    return findBuiltin(uid) != nullptr ? uid : std::string();
}
const PluginEntry *BrowserPanel::draggedPlugin(const juce::DragAndDropTarget::SourceDetails &details) const {
    const auto *parts = details.description.getArray();
    if (details.sourceComponent.get() != &list || parts == nullptr || parts->size() != 3 ||
        !(*parts)[0].isString() || (*parts)[0].toString() != "adi-plugin" ||
        !(*parts)[1].isString() || !(*parts)[2].isString())
        return nullptr;
    const auto format = (*parts)[1].toString().toStdString();
    const auto uid = (*parts)[2].toString().toStdString();
    const auto &entries = pluginScan_.listing.plugins;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const PluginEntry &entry) {
        return entry.format == format && entry.uid == uid;
    });
    return found != entries.end() ? &*found : nullptr;
}
bool BrowserPanel::acceptsDeviceDrop(const juce::DragAndDropTarget::SourceDetails &details) const {
    return !droppedDevice(details.description).empty() || (root_.loadPlugin && draggedPlugin(details) != nullptr);
}
bool BrowserPanel::loadDroppedDevice(const juce::DragAndDropTarget::SourceDetails &details, std::int64_t track,
                                    std::optional<std::int64_t> before) {
    if (const auto uid = droppedDevice(details.description); !uid.empty())
        return root_.insertDevice(uid, track, before);
    if (const auto *plugin = draggedPlugin(details))
        return loadPluginEntry(*plugin, track, before);
    return false;
}
std::string BrowserPanel::pluginWaits(const PluginEntry &p) {
    return "Loading " + p.name + " needs the application's plug-in host.";
}
bool BrowserPanel::pluginsShown() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(sidebar_.size()))
        return false;
    const auto glyph = sidebar_[static_cast<std::size_t>(selected_)].glyph;
    const auto firstPlugin = favoriteMarks_.lower_bound("plugin:");
    return glyph == PlugIns || glyph == All ||
           (glyph == Favorite && firstPlugin != favoriteMarks_.end() && firstPlugin->first.starts_with("plugin:"));
}
void BrowserPanel::startSearchPluginScan() {
    if (!hotSwap_ && scanPlugins && pluginScan_.phase == PluginScan::Phase::NotStarted && pluginsShown())
        scanPlugins();
}
void BrowserPanel::setPluginScan(const PluginScan &scan) {
    const bool began = scan.phase == PluginScan::Phase::Scanning && pluginScan_.phase != PluginScan::Phase::Scanning;
    const bool ended = scan.phase == PluginScan::Phase::Done && pluginScan_.phase == PluginScan::Phase::Scanning;
    // A count during a job, with the same list: only the list's first line changes, so the rows, the
    // selection and the scroll stay as they are.
    const bool countOnly = !began && !ended && scan.phase == PluginScan::Phase::Scanning &&
                           pluginScan_.phase == PluginScan::Phase::Scanning && scan.listed == pluginScan_.listed;
    if (countOnly) {
        pluginScan_.candidates = scan.candidates;
        pluginScan_.completed = scan.completed;
    } else {
        pluginScan_ = scan;
    }
    if (!(hotSwap_ ? !hotSwap_->samples : pluginsShown()))
        return;
    if (countOnly) {
        if (!favoritesShown() && !rows_.empty() && rows_.front().plugin == nullptr && rows_.front().device == nullptr) {
            rows_.front().heading = juce::String::fromUTF8(pluginScanStatus(pluginScan_).c_str());
            list.repaint();
        }
        return;
    }
    refill();
    // The Status Bar says the scan's start and its end while Plug-Ins shows; a count arriving between
    // them only redraws the list's line.
    if (!hotSwap_ && (began || ended))
        root_.say(pluginScanStatus(pluginScan_));
}
bool BrowserPanel::loadPluginEntry(const PluginEntry &p, std::int64_t track, std::optional<std::int64_t> before) {
    if (!root_.loadPlugin) {
        root_.say(pluginWaits(p));
        return false;
    }
    const auto current = root_.current();
    const auto *target = current ? current->findTrack(track) : nullptr;
    if (track != 0 && target == nullptr) {
        root_.say("Select a track to load " + p.name + " onto.");
        return false;
    }
    // N58: with the kind unknown, a MIDI track takes an instrument and an audio track takes an effect.
    // Copy the reference first: the load can refill the list this row points into.
    const bool midi = target && (target->kind == "midi" || target->kind == "instrument");
    return root_.loadPlugin(pluginReference(p, midi), track, before, p.kind == PluginEntry::Kind::Unknown);
}
bool BrowserPanel::loadHotSwap(const Row &row) {
    if (!row.heading.isEmpty())
        return false;
    if (!swapChoice) {
        root_.say("Hot-Swap needs the application.");
        return false;
    }
    // Replacing the device can refill the Browser. Pass an owning choice into the callback.
    using Kind = HotSwapChoice::Kind;
    if (row.device)
        return swapChoice({Kind::Builtin, std::string(row.device->uid), {}, {}});
    if (row.plugin)
        return swapChoice({Kind::Plugin, {}, *row.plugin, {}});
    if (row.savedPreset)
        return swapChoice({Kind::Preset, {}, {}, row.savedPreset->path});
    if (row.file && row.file->isSample())
        return swapChoice({Kind::Sample, {}, {}, row.file->path});
    return false;
}
bool BrowserPanel::load(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return false;
    const auto &entry = rows_[static_cast<std::size_t>(row)];
    if (hotSwap_)
        return loadHotSwap(entry);
    if (entry.folder != nullptr)
        return toggleFolder(row);
    if (const auto *saved = entry.savedPreset) {
        if (!loadDevicePreset) {
            root_.say("Loading a device preset needs the application.");
            return false;
        }
        const auto path = saved->path;
        return loadDevicePreset(path);
    }
    auto track = root_.arrangement.geometry.selection.track;
    if (entry.file) {
        if (entry.file->kind == LibraryFile::Kind::Project) {
            // The application can replace the root while opening the file.
            auto open = root_.openProjectFile;
            if (!open)
                return false;
            const juce::File file(nativeFilePath(entry.file->path));
            open(file);
            return true;
        }
        if (entry.file->kind == LibraryFile::Kind::Midi) {
            const auto path = media::pathUtf8(entry.file->path);
            const auto name = media::pathUtf8(entry.file->path.stem());
            const auto current = root_.current();
            const auto *target = current ? current->findTrack(track) : nullptr;
            return root_.dropMidiFiles({{path, name}}, target && isMidiTrack(*target) ? track : 0,
                                       root_.arrangement.geometry.selection.insert, false);
        }
        if (!entry.file->isSample())
            return false;
        const auto &selection = root_.arrangement.geometry.selection;
        if (track == 0) {
            root_.say("Select a track to load " + entry.file->name + " onto.");
            return false;
        }
        return root_.dropAudio(media::pathUtf8(entry.file->path), track, selection.insert,
                               media::pathUtf8(entry.file->path.stem()));
    }
    const bool instrument = entry.plugin ? entry.plugin->kind == PluginEntry::Kind::Instrument
                                         : entry.device && entry.device->instrument;
    // R50 decides whether a double-click on a known instrument with an audio track chosen creates a MIDI track.
    constexpr bool kNewMidiTrackWhenAudioSelected = false;
    if constexpr (kNewMidiTrackWhenAudioSelected) {
        if (const auto *target = root_.reader().findTrack(track); instrument && target && target->kind == "audio")
            track = 0;
    }
    const auto before = track != 0 ? root_.devices.insertPoint() : std::nullopt;
    if (const auto *p = entry.plugin) {
        if (track == 0 && !instrument && root_.loadPlugin) {
            root_.say("Select a track to load " + p->name + " onto.");
            return false;
        }
        return loadPluginEntry(*p, track, before);
    }
    if (entry.device == nullptr)
        return false;
    const auto &d = *entry.device;
    if (const auto why = unavailable(d); !why.empty()) {
        root_.say(why);
        return false;
    }
    // Onto the selected track, in front of the device chosen in the strip (manual 4.10, 41.4).
    if (track == 0 && !instrument) {
        root_.say("Select a track to load " + std::string(d.name) + " onto.");
        return false;
    }
    return root_.insertDevice(std::string(d.uid), track, before);
}
bool BrowserPanel::toggleFolder(int row) {
    const auto &entry = rows_[static_cast<std::size_t>(row)];
    auto &open = openFolders_[sidebar_[static_cast<std::size_t>(selected_)].place];
    if (entry.folderOpen)
        open.erase(entry.folder->path);
    else
        open.insert(entry.folder->path);
    refill(row);
    return true;
}
bool BrowserPanel::folderArrow(int row, bool right) {
    if (row < 0 || row >= getNumRows())
        return false;
    const auto &entry = rows_[static_cast<std::size_t>(row)];
    if (entry.folder != nullptr && entry.folderOpen != right)
        return toggleFolder(row);
    if (right) {
        if (entry.folder == nullptr)
            return false;
        if (row + 1 < getNumRows() && rows_[static_cast<std::size_t>(row + 1)].depth > entry.depth)
            list.selectRow(row + 1);
        return true;
    }
    if (entry.depth > 0) {
        for (int parent = row - 1; parent >= 0; --parent) {
            const auto &candidate = rows_[static_cast<std::size_t>(parent)];
            if (candidate.depth < entry.depth) {
                if (candidate.folder == nullptr)
                    return false;
                list.selectRow(parent);
                return true;
            }
        }
    }
    return false;
}
bool BrowserPanel::hitFolderDisclosure(int row, int x) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return false;
    const auto &entry = rows_[static_cast<std::size_t>(row)];
    const int indent = kFolderIndent * entry.depth;
    return entry.folder != nullptr && x >= indent && x < indent + kFolderDisclosureWidth;
}
void BrowserPanel::listBoxItemClicked(int row, const juce::MouseEvent &event) {
    if (event.getNumberOfClicks() != 1)
        return;
    if (event.mods.isPopupMenu()) {
        if (row < 0 || row >= getNumRows())
            return;
        const auto &entry = rows_[static_cast<std::size_t>(row)];
        if (!entry.file && !entry.folder && !entry.savedPreset && !entry.plugin && !entry.device)
            return;
        juce::PopupMenu menu;
        std::filesystem::path path;
        if (entry.file)
            path = entry.file->path;
        else if (entry.folder && selected_ >= 0) {
            const auto &places = listing().places;
            const auto &volume = sidebar_[static_cast<std::size_t>(selected_)].place;
            const auto place = std::ranges::find(places, volume, &LibraryPlace::volumeId);
            if (place != places.end())
                path = place->root / entry.folder->path;
        } else if (entry.savedPreset)
            path = entry.savedPreset->path;
        else if (entry.plugin)
            path = media::pathFromUtf8(entry.plugin->path);
        addRevealItem(menu, path, entry.device != nullptr);
        if (entry.file || entry.folder)
            addRescanItem(menu);
        if (appendFavoritesToMenu && !entry.folder)
            appendFavoritesToMenu(menu, entry);
        presentContextMenu(std::move(menu), event);
        return;
    }
    if (hitFolderDisclosure(row, event.x)) {
        toggleFolder(row);
        return;
    }
    // Selection already starts the first audition. Replaying the same row
    // waits until that audition has finished.
    if (row == list.getLastRowSelected() && canRepeatPreview_ && root_.commands().browserPreviewEnabled())
        root_.command(AppCommands::PreviewSelected);
}
void BrowserPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent &event) {
    if (!event.mods.isPopupMenu() && !hitFolderDisclosure(row, event.x))
        load(row);
}
void BrowserPanel::resized() {
    // Live's shape: the search row and the preview row start right of the round
    // buttons; the panes run the panel's width between them, one dark-edged block.
    const auto r = getLocalBounds();
    const int edge = look::border;
    search.setBounds(kStripWidth + edge, edge, std::max(0, r.getWidth() - kStripWidth - 2 * edge), 21);
    syncSearchClearButton();
    const int top = kBarHeight, bottom = r.getHeight() - (previewRowShown_ ? kBarHeight : 0) - edge;
    const bool keepInView = selected_ >= 0 &&
        (sidebarArea_.isEmpty() || sidebarArea_.intersects(sidebarRowBounds(selected_)));
    sidebarArea_ = {edge, top, kSidebarWidth, std::max(0, bottom - top)};
    setSidebarScroll(sidebarScroll_);
    if (keepInView)
        revealSidebarSelection();
    contentArea_ = {sidebarArea_.getRight() + 2, top,
                    std::max(0, r.getWidth() - sidebarArea_.getRight() - 2 - edge), sidebarArea_.getHeight()};
    // The list fills the content pane below its header.
    list.setBounds(contentArea_.withTrimmedTop(19));
    previewToggle.setVisible(previewRowShown_);
    previewToggle.setBounds(previewRowShown_ ? previewArea().withWidth(26) : juce::Rectangle<int>{});
}
juce::Rectangle<int> BrowserPanel::previewArea() const noexcept {
    if (!previewRowShown_)
        return {};
    return {kStripWidth + look::border, getHeight() - kBarHeight + 2,
            std::max(0, getWidth() - kStripWidth - 2 * look::border), 21};
}
juce::Rectangle<int> BrowserPanel::previewWaveformArea() const noexcept {
    return previewArea().withTrimmedLeft(26).reduced(look::border, 3);
}
void BrowserPanel::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    const float w = static_cast<float>(getWidth()), h = static_cast<float>(getHeight());
    const float strip = kStripWidth, bar = kBarHeight;
    // The dark edge: the search row, the panes, the preview row, as one block.
    g.setColour(look::browserEdge);
    g.fillRoundedRectangle(strip, 0, w - strip, bar + 4, look::radiusControl);
    const float footer = previewRowShown_ ? bar : 0.f;
    g.fillRoundedRectangle(0, bar - 4, w, h - bar - footer + 8, look::radiusControl);
    if (previewRowShown_)
        g.fillRoundedRectangle(strip, h - bar - 4, w - strip, bar + 4, look::radiusControl);
    // The top round button hides the Browser. MI-05 will bring in the Groove Pool.
    controls::roundButton(g, {0, 0.5f, 19, 19}, controls::ButtonState::Rest);
    look::triangle(g, {7, 5.5f, 7, 9}, 0, controls::labelColour(controls::ButtonState::Rest));
    // The search field.
    g.setColour(look::panel);
    g.fillRect(search.getBounds());
    for (auto area : {sidebarArea_, contentArea_}) {
        g.setColour(look::panel);
        g.fillRect(area);
    }
    // Draw only the rows that meet the sidebar pane.
    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(sidebarArea_);
        const int first = std::max(0, juce::roundToInt(sidebarScroll_) - 1) / kRowHeight;
        const int highlighted = sidebarCursor();
        for (int i = first; i < static_cast<int>(sidebar_.size()); ++i) {
            const auto row = sidebarRowBounds(i);
            if (row.getY() >= sidebarArea_.getBottom())
                break;
            const auto &e = sidebar_[static_cast<std::size_t>(i)];
            // The sheet lights the chosen AI label in the accent (browser-2.png, states C and D).
            const bool litAi = !hotSwap_ && i == highlighted && e.glyph == Ai;
            if (!hotSwap_ && i == highlighted) {
                g.setColour(litAi ? look::accent : look::selection);
                g.fillRect(row);
            }
            if (e.heading) {
                g.setColour(look::textDim);
                g.setFont(look::font(look::fontField));
                g.drawText(e.name, row.withTrimmedLeft(5), juce::Justification::centredLeft);
            } else {
                icon(g, e.glyph, row.withTrimmedLeft(8).withWidth(13).toFloat().withSizeKeepingCentre(13, 13),
                     litAi ? std::optional<juce::Colour>(look::ink) : std::nullopt);
                g.setColour(litAi ? look::ink : hotSwap_ ? look::textDim : look::text);
                g.setFont(look::font(look::fontHeading));
                g.drawText(e.name, row.withTrimmedLeft(25), juce::Justification::centredLeft);
            }
        }
    }
    if (ring_) {
        g.setColour(look::accent);
        g.drawRect(sidebarArea_, 1);
    }
    // The content pane's header, as Live's list.
    const auto header = nameHeaderArea();
    g.setColour(look::columnHead);
    g.fillRect(header);
    g.setFont(look::font(look::fontField, true));
    if (hotSwap_) {
        const auto target = juce::String::fromUTF8(hotSwap_->targetName.c_str());
        g.setColour(look::accent);
        g.drawFittedText(target.isEmpty() ? juce::String("Hot-Swap") : "Hot-Swap  " + target,
                         header.reduced(4, 0), juce::Justification::centredLeft, 1);
    } else if (agentChosen()) {
        paintAgentPane(g);
    } else {
        g.setColour(look::text);
        g.drawText("Name", header.withTrimmedLeft(4), juce::Justification::centredLeft);
        const auto angle = nameSortAscending_.value_or(true) ? juce::MathConstants<float>::halfPi
                                                           : -juce::MathConstants<float>::halfPi;
        look::triangle(g, {static_cast<float>(header.getRight()) - 13, static_cast<float>(header.getY()) + 6, 8, 7},
                       angle, look::textDim);
    }
    // The preview row: Live's headphones switch at its left.
    const auto preview = previewArea();
    if (preview.isEmpty())
        return;
    g.setColour(look::panel);
    g.fillRect(preview);
    const auto wave = previewWaveformArea();
    if (previewPeaks_ && !previewPeaks_->empty() && !wave.isEmpty()) {
        const auto width = static_cast<std::size_t>(wave.getWidth());
        const auto count = previewPeaks_->size();
        const float middle = static_cast<float>(wave.getY()) + static_cast<float>(wave.getHeight()) / 2.f;
        const float reach = static_cast<float>(wave.getHeight()) / 2.f - 1.f;
        g.setColour(look::textDim);
        for (std::size_t i = 0; i < width; ++i) {
            const auto from = i * count / width;
            const auto to = std::max(from + 1, (i + 1) * count / width);
            auto [low, high] = (*previewPeaks_)[from];
            for (std::size_t b = from + 1; b < to; ++b) {
                low = std::min(low, (*previewPeaks_)[b].minimum);
                high = std::max(high, (*previewPeaks_)[b].maximum);
            }
            const auto top = middle - reach * std::clamp(high, -1.f, 1.f);
            const auto bottom = middle - reach * std::clamp(low, -1.f, 1.f);
            g.fillRect(static_cast<float>(wave.getX()) + static_cast<float>(i), top, 1.f,
                       std::max(1.f, bottom - top));
        }
        if (previewPosition_) {
            const auto x = wave.getX() + juce::roundToInt(std::clamp(*previewPosition_, 0., 1.) * (wave.getWidth() - 1));
            g.setColour(look::accent);
            g.fillRect(x, wave.getY(), 1, wave.getHeight());
        }
    }
}
juce::Rectangle<int> BrowserPanel::sidebarRowBounds(int row) const noexcept {
    return {sidebarArea_.getX(), sidebarArea_.getY() + 1 + row * kRowHeight - juce::roundToInt(sidebarScroll_),
            sidebarArea_.getWidth(), kRowHeight};
}
void BrowserPanel::setSidebarScroll(double requested) {
    if (sidebarArea_.isEmpty())
        return;
    const int extent = 1 + static_cast<int>(sidebar_.size()) * kRowHeight;
    const double maximum = std::max(0, extent - sidebarArea_.getHeight());
    const double next = std::clamp(requested, 0., maximum);
    if (juce::roundToInt(next) != juce::roundToInt(sidebarScroll_))
        repaint(sidebarArea_);
    sidebarScroll_ = next;
}
int BrowserPanel::sidebarCursor() const noexcept {
    return addFolderFocused_ ? static_cast<int>(sidebar_.size()) - 1 : selected_;
}
void BrowserPanel::revealSidebarSelection() {
    const int row = sidebarCursor();
    if (row < 0 || sidebarArea_.isEmpty())
        return;
    const int top = 1 + row * kRowHeight;
    const int height = sidebarArea_.getHeight();
    const double minimumScroll = top + std::min(height, kRowHeight) - height;
    setSidebarScroll(std::clamp(sidebarScroll_, minimumScroll, static_cast<double>(top)));
}
void BrowserPanel::mouseDown(const juce::MouseEvent &e) {
    if (e.mods.isLeftButtonDown() && !e.mods.isPopupMenu()) {
        if (hideButtonArea().contains(e.getPosition())) {
            root_.command(AppCommands::ToggleBrowser);
            return;
        }
        if (!hotSwap_ && nameHeaderArea().contains(e.getPosition())) {
            if (agentChosen())
                return; // the AI mode's head line sorts nothing
            nameSortAscending_ = !nameSortAscending_.value_or(true);
            refill(-1, true);
            repaint(nameHeaderArea());
            return;
        }
    }
    const auto wave = previewWaveformArea();
    if (!e.mods.isPopupMenu() && previewPeaks_ && wave.contains(e.getPosition()) && selectedSample()) {
        previewSelected(static_cast<double>(e.x - wave.getX()) / std::max(1, wave.getWidth() - 1));
        return;
    }
    if (agentChosen() && !e.mods.isPopupMenu() && contentArea_.contains(e.getPosition())) {
        for (std::size_t i = 0; i < agentMode_.cards.size(); ++i)
            for (const auto b : cardButtons(agentMode_.cards[i]))
                if (agentButtonArea(i, b).contains(e.getPosition())) {
                    pressAgentButton(i, b);
                    return;
                }
        return;
    }
    const int y = e.y - sidebarRowBounds(0).getY();
    if (sidebarArea_.contains(e.getPosition()) && y >= 0)
        sidebarClick(y / kRowHeight, e);
}
void BrowserPanel::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) {
    if (sidebarArea_.contains(e.getPosition()) && wheel.deltaY != 0) {
        constexpr double pixelsPerUnit = 300.;
        setSidebarScroll(sidebarScroll_ - pixelsPerUnit * wheel.deltaY);
        return;
    }
    juce::Component::mouseWheelMove(e, wheel);
}
void BrowserPanel::sidebarClick(int row, const juce::MouseEvent &e) {
    if (hotSwap_ || row < 0 || row >= static_cast<int>(sidebar_.size())) return;
    if (!e.mods.isPopupMenu()) {
        choose(row);
        return;
    }
    const auto &entry = sidebar_[static_cast<std::size_t>(row)];
    if (entry.heading)
        return;
    const auto &places = listing().places;
    const auto place = entry.place.empty() ? places.end()
                                          : std::ranges::find(places, entry.place, &LibraryPlace::volumeId);
    juce::PopupMenu menu;
    if (place != places.end()) {
        addRevealItem(menu, place->root);
        if (!place->userLibrary && !place->starterDrums && removeFolder) {
            const SafePointer<BrowserPanel> safe(this);
            menu.addItem("Remove from Places", [safe, path = place->root] {
                if (safe && safe->removeFolder)
                    safe->removeFolder(path);
            });
        }
    }
    if (place != places.end() || entry.glyph == All || entry.samples || entry.glyph == User)
        addRescanItem(menu);
    presentContextMenu(std::move(menu), e);
}
void BrowserPanel::presentContextMenu(juce::PopupMenu menu, const juce::MouseEvent &e) {
    if (menu.getNumItems() == 0)
        return;
    const auto where = e.getScreenPosition();
    if (onContextMenu) {
        onContextMenu(std::move(menu), where);
        return;
    }
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({where.x, where.y, 1, 1}));
}
void BrowserPanel::addRevealItem(juce::PopupMenu &menu, const std::filesystem::path &path, bool builtin) {
    if (!path.is_absolute()) {
        menu.addItem(juce::PopupMenu::Item(builtin ? "Show in File Manager (built-in)"
                                                 : "Show in File Manager (file unavailable)").setEnabled(false));
        return;
    }
    const SafePointer<BrowserPanel> safe(this);
    menu.addItem("Show in File Manager", [safe, path] {
        if (!safe)
            return;
        if (safe->revealInFileManager) {
            safe->revealInFileManager(path);
            return;
        }
        juce::MessageManager::callAsync([safe, path] {
            if (!safe)
                return;
            const juce::File file(juce::String::fromUTF8(media::pathUtf8(path).c_str()));
            if (file.exists())
                file.revealToUser();
            else
                safe->root_.say("The Browser item is no longer on disk.");
        });
    });
}
void BrowserPanel::addRescanItem(juce::PopupMenu &menu) {
    if (!requestLibraryRescan)
        return;
    const SafePointer<BrowserPanel> safe(this);
    menu.addItem("Rescan Library", [safe] {
        if (safe && safe->requestLibraryRescan)
            safe->requestLibraryRescan();
    });
}
namespace {
bool droppedDirectory(const juce::String &file) {
    return juce::File::isAbsolutePath(file) && juce::File(file).isDirectory();
}
} // namespace
bool BrowserPanel::isInterestedInFileDrag(const juce::StringArray &files) {
    return addFolders && std::ranges::any_of(files, droppedDirectory);
}
void BrowserPanel::filesDropped(const juce::StringArray &files, int, int) {
    if (!addFolders) return;
    std::vector<std::filesystem::path> folders;
    for (const auto &file : files)
        if (droppedDirectory(file)) folders.push_back(media::pathFromUtf8(file.toStdString()));
    if (!folders.empty()) addFolders(folders);
}
void BrowserPanel::searchAll() {
    const SafePointer<BrowserPanel> alive(this);
    cancelHotSwap();
    if (!alive)
        return;
    const int all = findLabel(kAllLabel);
    if (selected_ != all)
        choose(all);
    else
        startSearchPluginScan();
}
void BrowserPanel::choose(int i) {
    if (hotSwap_)
        return;
    if (i >= 0 && i < static_cast<int>(sidebar_.size()) && !sidebar_[static_cast<std::size_t>(i)].heading) {
        if (sidebar_[static_cast<std::size_t>(i)].glyph == AddFolder) {
            if (requestAddFolder) requestAddFolder();
            else root_.say("Choose a sample folder in the application.");
            return;
        }
        if (revealedSample_) {
            const auto key = labelKey(i);
            clearRevealedSample();
            i = findLabel(key);
            if (i < 0)
                i = findLabel(kAllLabel);
        }
        const auto &entry = sidebar_[static_cast<std::size_t>(i)];
        addFolderFocused_ = false;
        const bool differentList = selected_ != i;
        selected_ = i;
        pendingLabel_.clear();
        revealSidebarSelection();
        // Plug-Ins lists what the app's scan found; the first time, it asks the app to scan, which runs on
        // a worker and fills the list when it ends (DF-W9, DF-W34): choosing never waits on a scan.
        const bool plugIns = entry.glyph == PlugIns;
        repaint();
        refill();
        if (differentList)
            setPreviewRowShown(false);
        startSearchPluginScan();
        const auto key = labelKey(i);
        if (labelSettings_ && !key.empty() && labelSettings_->get(kLastLabel) != key) {
            std::string why;
            if (!labelSettings_->set(kLastLabel, key, {}, why) || !labelSettings_->save(why)) {
                root_.say(std::move(why));
                return;
            }
        }
        if (entry.glyph == Project && projectLibrary_ && !projectLibrary_->listing().error.empty())
            root_.say(projectLibrary_->listing().error);
        if (entry.glyph == All || entry.glyph == Sounds || entry.glyph == Project || entry.glyph == Favorite)
            return;
        const bool library = entry.samples || entry.glyph == User || entry.glyph == Drums;
        if (!entry.place.empty()) {
            const auto &places = listing().places;
            const auto place = std::ranges::find(places, entry.place, &LibraryPlace::volumeId);
            if (place != places.end() && !place->problem.empty()) {
                root_.say(place->problem);
                return;
            }
        }
        if (entry.glyph == Instruments || entry.glyph == AudioFx || entry.glyph == Pd) {
            // The catalogue's (D4): with Pd off, every entry is dimmed and the Status Bar says why.
            if (!rows_.empty() && std::none_of(rows_.begin(), rows_.end(), [](const Row &r) {
                    return r.device != nullptr && unavailable(*r.device).empty();
                }))
                root_.say("ADI's built-in devices run on its Pd devices, which this build leaves out.");
        } else if (plugIns) {
            if (!scanPlugins)
                root_.say("Plug-ins list in the application, from its plug-in scan.");
            else
                root_.say(pluginScanStatus(pluginScan_));
        } else if (library && !ready_)
            root_.say("The library is still being indexed; its samples list when it is done.");
        else if (library && rows_.empty()) {
            const bool audioOnly = entry.samples || entry.glyph == Drums;
            root_.say(entry.glyph != Drums && (listing().places.empty() || (!entry.samples && entry.place.empty()))
                          ? "Choose Add Folder in the Browser to list samples."
                          : (entry.name + (audioOnly ? " holds no audio files." : " holds no supported files.")).toStdString());
        }
    }
}
bool BrowserPanel::keyPressed(const juce::KeyPress &k) {
    if (k == juce::KeyPress::escapeKey && cancelHotSwap())
        return true;
    if (k == juce::KeyPress::returnKey && addFolderFocused_ && !hotSwap_) {
        choose(static_cast<int>(sidebar_.size()) - 1);
        return true;
    }
    // Right goes from the sidebar into the list (§41.22), its first row selected.
    if (k == juce::KeyPress::rightKey) {
        enterList();
        return true;
    }
    if (k != juce::KeyPress::upKey && k != juce::KeyPress::downKey)
        return false;
    if (hotSwap_) {
        enterList();
        return true;
    }
    // The next label up or down, past the headings; at either end the selection stays.
    const int step = k == juce::KeyPress::downKey ? 1 : -1;
    for (int i = sidebarCursor() + step; i >= 0 && i < static_cast<int>(sidebar_.size()); i += step)
        if (!sidebar_[static_cast<std::size_t>(i)].heading) {
            if (sidebar_[static_cast<std::size_t>(i)].glyph == AddFolder) {
                addFolderFocused_ = true;
                revealSidebarSelection();
                repaint();
            } else {
                choose(i);
            }
            break;
        }
    return true;
}
void BrowserPanel::focusGained(FocusChangeType cause) {
    ring_ = cause != focusChangedByMouseClick;
    repaint();
}
void BrowserPanel::focusLost(FocusChangeType) {
    ring_ = false;
    addFolderFocused_ = false;
    repaint();
}

// --- the AI mode ---------------------------------------------------------------------------
bool BrowserPanel::agentChosen() const noexcept {
    return !hotSwap_ && selected_ >= 0 && static_cast<std::size_t>(selected_) < sidebar_.size() &&
           sidebar_[static_cast<std::size_t>(selected_)].glyph == Ai;
}
bool BrowserPanel::setAgentLabelVisible(bool visible) {
    const int ai = findLabel(kAiLabel);
    if (visible == (ai >= 0))
        return false;
    const auto key = labelKey(selected_);
    if (visible) {
        // ADR-0261 d2: the last label of Categories (the sheet puts it after Templates, which this
        // Browser does not list yet; Samples ends Categories here).
        const int samples = findLabel(kSamplesLabel);
        if (samples < 0)
            return false;
        sidebar_.insert(sidebar_.begin() + static_cast<std::ptrdiff_t>(samples + 1), Entry{"AI", Ai});
        ++baseEntries_;
    } else {
        sidebar_.erase(sidebar_.begin() + static_cast<std::ptrdiff_t>(ai));
        --baseEntries_;
    }
    selected_ = findLabel(key);
    const bool selectionRemoved = selected_ < 0;
    if (selectionRemoved) {
        selected_ = findLabel(kAllLabel);
        setPreviewRowShown(false);
    }
    setSidebarScroll(sidebarScroll_);
    repaint();
    return selectionRemoved;
}
juce::String BrowserPanel::agentCardDetail(const AgentCard &card) {
    using Moved = AgentCard::Moved;
    const auto count = [](std::size_t n, const char *one, const char *many) {
        return juce::String(static_cast<juce::int64>(n)) + " " + (n == 1 ? one : many);
    };
    const auto changes = count(card.changes, "change", "changes");
    switch (card.moved) {
    case Moved::No:
        return changes;
    case Moved::Behind:
        return changes + ". The song moved on: " + count(card.steps, "edit", "edits") + " came after it; " +
               count(card.steps, "Undo makes", "Undos make") + " it current.";
    case Moved::Ahead:
        return changes + ". " + count(card.steps, "step was", "steps were") + " undone since it was made; " +
               count(card.steps, "Redo makes", "Redos make") + " it current.";
    case Moved::Elsewhere:
        return changes + ". The song is on another branch of its history now.";
    case Moved::Unknown:
        return changes + ". Where it stands in the song's history cannot be read.";
    }
    return changes;
}
void BrowserPanel::setAgentMode(AgentMode mode) {
    const bool wasChosen = agentChosen();
    agentMode_ = std::move(mode);
    agentHead_ = juce::String::fromUTF8(agentMode_.head.c_str());
    agentRequests_.clear();
    agentDetails_.clear();
    for (const auto &card : agentMode_.cards) {
        agentRequests_.push_back(juce::String::fromUTF8(card.request.c_str()));
        agentDetails_.push_back(agentCardDetail(card));
    }
    const bool removed = setAgentLabelVisible(agentMode_.shown());
    if (removed || wasChosen != agentChosen())
        refill();
    repaint();
}
juce::Rectangle<int> BrowserPanel::agentCardArea(std::size_t card) const {
    const auto &cards = agentMode_.cards;
    if (card >= cards.size() || contentArea_.isEmpty())
        return {};
    int y = nameHeaderArea().getBottom() + kCardGap;
    for (std::size_t i = 0; i < card; ++i)
        y += cardHeight(cards[i]) + kCardGap;
    const juce::Rectangle<int> area{contentArea_.getX() + kCardPad, y,
                                    std::max(0, contentArea_.getWidth() - 2 * kCardPad), cardHeight(cards[card])};
    return area.getBottom() <= contentArea_.getBottom() ? area : juce::Rectangle<int>{};
}
juce::Rectangle<int> BrowserPanel::agentButtonArea(std::size_t card, CardButton which) const {
    const auto area = agentCardArea(card);
    if (area.isEmpty())
        return {};
    int x = area.getX() + 8;
    const int y = area.getBottom() - kCardPad - kCardButtons;
    for (const auto b : cardButtons(agentMode_.cards[card])) {
        const int width = cardButtonWidth(b);
        if (b == which)
            return juce::Rectangle<int>{x, y, width, kCardButtons}.getIntersection(area);
        x += width + kCardButtonGap;
    }
    return {};
}
bool BrowserPanel::pressAgentButton(std::size_t card, CardButton which) {
    if (agentButtonArea(card, which).isEmpty())
        return false;
    // A copy: the seam may hand the pane new cards before it returns.
    const std::string id = agentMode_.cards[card].id;
    const auto &seam = which == CardButton::Apply      ? onApplyCard
                       : which == CardButton::Reject   ? onRejectCard
                       : which == CardButton::OpenList ? onOpenCardList
                                                       : onAskAgain;
    if (!seam) {
        root_.say("The agent's proposals are reviewed in the application.");
        return false;
    }
    const auto call = seam; // the seam may be replaced while it runs
    call(id);
    return true;
}
void BrowserPanel::paintAgentPane(juce::Graphics &g) {
    // The head line: who answers and how far it may go (the sheet's note 6).
    const auto header = nameHeaderArea();
    g.setColour(look::textDim);
    g.setFont(look::font(look::fontField));
    g.drawFittedText(agentHead_.isNotEmpty() ? agentHead_ : juce::String("Proposals that wait for you"),
                     header.reduced(4, 0), juce::Justification::centredLeft, 1);
    juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(contentArea_.withTrimmedTop(header.getHeight()));
    if (agentMode_.cards.empty()) {
        g.drawFittedText("Nothing waits for your Apply.",
                         contentArea_.withTrimmedTop(header.getHeight()).reduced(kCardPad), juce::Justification::topLeft,
                         2);
        return;
    }
    for (std::size_t i = 0; i < agentMode_.cards.size(); ++i) {
        const auto area = agentCardArea(i);
        if (area.isEmpty())
            break;
        const auto &card = agentMode_.cards[i];
        const bool current = card.moved == AgentCard::Moved::No;
        // A proposed change is a card (note 7), outlined in the accent while Apply can write it.
        g.setColour(look::browserEdge);
        g.fillRoundedRectangle(area.toFloat(), look::radiusControl);
        g.setColour(current ? look::accent : look::textFaint);
        g.drawRoundedRectangle(area.toFloat().reduced(0.5f), look::radiusControl, 1.f);
        auto inner = area.reduced(8, kCardPad);
        g.setColour(look::text);
        g.setFont(look::font(look::fontHeading, true));
        g.drawFittedText(agentRequests_[i], inner.removeFromTop(kCardTitle), juce::Justification::centredLeft, 1);
        g.setColour(look::textDim);
        g.setFont(look::font(look::fontField));
        const int detailHeight = current ? kCardDetail : kCardMovedDetail;
        g.drawFittedText(agentDetails_[i], inner.removeFromTop(detailHeight), juce::Justification::centredLeft,
                         current ? 1 : 2);
        for (const auto b : cardButtons(card)) {
            const auto button = agentButtonArea(i, b);
            if (button.isEmpty())
                continue;
            controls::button(g, button.toFloat(),
                             b == CardButton::Apply ? controls::ButtonState::On : controls::ButtonState::Rest,
                             cardButtonLabel(b), look::fontField);
        }
    }
}
} // namespace adi::ui
