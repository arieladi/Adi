// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_browser.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include <algorithm>
namespace adi::ui {
namespace {
enum Glyph { None, Favorite, Sounds, Drums, Instruments, AudioFx, MidiFx, Pd, PlugIns, Clips,
             Samples, Grooves, Templates, Packs, User, Project, AddFolder };
void icon(juce::Graphics &g, int kind, juce::Rectangle<float> r) {
    g.setColour(look::text);
    const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
    juce::Path p;
    switch (kind) {
    case Favorite:
        g.setColour(juce::Colour(0xffe5352b));
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
    default:
        break;
    }
}
} // namespace
BrowserPanel::BrowserPanel(AdiRootComponent &root) : root_(root) {
    // Live's sidebar, its names and order (manual 3.3). "Max for Live" is ADI's Pd tier
    // (Master Reference 9.3), Live's place for the same thing.
    sidebar_ = {{"Collections", None, true}, {"Favorites", Favorite},  {{}, None, true},
                {"Categories", None, true},  {"Sounds", Sounds},       {"Drums", Drums},
                {"Instruments", Instruments}, {"Audio Effects", AudioFx}, {"MIDI Effects", MidiFx},
                {"Pd Devices", Pd},          {"Plug-Ins", PlugIns},    {"Clips", Clips},
                {"Samples", Samples, false, true},        {"Grooves", Grooves},     {"Templates", Templates},
                {{}, None, true},            {"Places", None, true},   {"Packs", Packs},
                {"User Library", User},      {"Current Project", Project},
                {"Add Folder...", AddFolder}};
    setTitle("Browser");
    setTooltip("Live's browser: your sounds, devices, plug-ins and files.");
    setWantsKeyboardFocus(true);
#if JUCE_MAC
    search.setTextToShowWhenEmpty("Search (Cmd + F)", juce::Colour(0xff5a5a5a));
#else
    search.setTextToShowWhenEmpty("Search (Ctrl + F)", juce::Colour(0xff5a5a5a));
#endif
    search.setTitle("Search");
    search.setFont(look::font(12.5f));
    search.setIndents(8, 4);
    search.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    search.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(search);
    baseEntries_ = sidebar_.size();
    // The search narrows the list as it is typed; Enter or the down arrow goes into it (§41.4.1.6).
    search.onTextChange = [this] { refill(); };
    search.onReturnKey = [this] { enterList(); };
    search.addKeyListener(&searchKeys_);
    list.setTitle("Browser content");
    list.setModel(this);
    list.setRowHeight(kRowHeight);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setMultipleSelectionEnabled(true);
    list.onLeft = [this] { grabKeyboardFocus(); };
    addAndMakeVisible(list);
}
void BrowserPanel::enterList() {
    if (firstItem() >= 0) {
        if (list.getSelectedRows().isEmpty())
            list.selectRow(firstItem());
        list.grabKeyboardFocus();
    }
}
int BrowserPanel::firstItem() const {
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].heading.isEmpty())
            return static_cast<int>(i);
    return -1;
}
void BrowserPanel::selectedRowsChanged(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].heading.isEmpty()) {
        lastSelected_ = row;
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
    if (k != juce::KeyPress::downKey)
        return false;
    panel.enterList();
    return true;
}
void BrowserPanel::setFootLimit(int y) {
    footLimit_ = y;
    resized();
}
juce::String BrowserPanel::dragDescription(const std::vector<const LibraryFile *> &files) {
    juce::StringArray paths;
    // The native spelling, as a file dragged from the desktop arrives: JUCE names a file by its last
    // native separator, so a generic path on Windows would name the clip after the whole path.
    for (const auto *f : files) {
        const auto native = std::filesystem::path(f->path).make_preferred().u8string();
        paths.add(juce::String::fromUTF8(reinterpret_cast<const char *>(native.data()), static_cast<int>(native.size())));
    }
    return "adi-sample:" + paths.joinIntoString("\n");
}
juce::StringArray BrowserPanel::droppedFiles(const juce::var &description) {
    const auto text = description.toString();
    if (!text.startsWith("adi-sample:"))
        return {};
    return juce::StringArray::fromLines(text.fromFirstOccurrenceOf("adi-sample:", false, false));
}
void BrowserPanel::setListing(LibraryListing listing, bool ready) {
    // The label chosen stays chosen across a new listing, found again by its name and place.
    const auto keep = selected_ >= 0 ? sidebar_[static_cast<std::size_t>(selected_)] : Entry{};
    listing_ = std::move(listing);
    ready_ = ready;
    // Live's added folders stand among the Places, after Current Project and before Add Folder; the
    // User Library's own label is Live's.
    sidebar_.resize(baseEntries_);
    auto addFolder = sidebar_.back();
    sidebar_.pop_back();
    for (auto &e : sidebar_)
        if (e.name == "User Library" && !e.heading)
            e.place.clear();
    for (const auto &place : listing_.places) {
        if (place.userLibrary) {
            for (auto &e : sidebar_)
                if (e.name == "User Library" && !e.heading)
                    e.place = place.volumeId;
            continue;
        }
        sidebar_.push_back({juce::String::fromUTF8(place.name.c_str()), User, false, false, place.volumeId});
    }
    sidebar_.push_back(addFolder);
    selected_ = -1;
    // The User Library's label is Live's own, whatever folder it names now; any other by its place.
    for (std::size_t i = 0; i < sidebar_.size() && keep.name.isNotEmpty() && selected_ < 0; ++i)
        if (sidebar_[i].name == keep.name && sidebar_[i].heading == keep.heading &&
            (sidebar_[i].place == keep.place || (keep.name == "User Library" && i < baseEntries_)))
            selected_ = static_cast<int>(i);
    refill();
    repaint();
}
void BrowserPanel::refill() {
    shown_.clear();
    const Entry none{};
    const auto &chosen = selected_ >= 0 ? sidebar_[static_cast<std::size_t>(selected_)] : none;
    // Samples, or a Place the listing holds, by its volume; the search alone looks through Samples.
    const bool samples = chosen.samples || (selected_ < 0 && search.getText().isNotEmpty());
    std::filesystem::path root;
    for (const auto &p : listing_.places)
        if (!chosen.place.empty() && p.volumeId == chosen.place)
            root = p.root;
    const auto query = search.getText().trim().toLowerCase();
    // A Place lists every sample under its folder, a folder inside it included.
    const auto under = [&root](const std::filesystem::path &path) {
        const auto rel = path.lexically_relative(root);
        return !rel.empty() && *rel.begin() != "..";
    };
    if (samples || !root.empty())
        for (const auto &f : listing_.samples)
            if ((samples || under(f.path)) &&
                (query.isEmpty() || juce::String::fromUTF8(f.name.c_str()).toLowerCase().contains(query)))
                shown_.push_back(&f);
    rows_.clear();
    for (const auto *f : shown_)
        rows_.push_back({f, nullptr, {}});
    // Audio Effects in Live's folders (manual 4.10), Pd Devices ADI's own, from the catalogue.
    const auto matches = [&query](const BuiltinInfo &d) {
        return query.isEmpty() || juce::String(std::string(d.name)).toLowerCase().contains(query);
    };
    if (!chosen.heading && chosen.name == "Audio Effects") {
        for (const char *folder : {"EQ & Filters", "Delay & Loop", "Dynamics", "Drive & Color", "Pitch & Modulation",
                                   "Reverb & Resonance", "Utilities"}) {
            std::vector<const BuiltinInfo *> in;
            for (const auto &d : builtinCatalog())
                if (d.category == "Audio Effects" && d.group == folder && matches(d))
                    in.push_back(&d);
            if (in.empty())
                continue;
            rows_.push_back({nullptr, nullptr, folder});
            std::sort(in.begin(), in.end(), [](const BuiltinInfo *a, const BuiltinInfo *b) { return a->name < b->name; });
            for (const auto *d : in)
                rows_.push_back({nullptr, d, {}});
        }
    } else if (!chosen.heading && chosen.name == "Pd Devices") {
        for (const auto &d : builtinCatalog())
            if (d.category == "ADI" && matches(d))
                rows_.push_back({nullptr, &d, {}});
    }
    list.updateContent();
    list.deselectAllRows();
    lastSelected_ = -1;
    list.repaint();
}
int BrowserPanel::getNumRows() { return static_cast<int>(rows_.size()); }
void BrowserPanel::paintListBoxItem(int row, juce::Graphics &g, int width, int height, bool selected) {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return;
    const auto &r = rows_[static_cast<std::size_t>(row)];
    if (selected && r.heading.isEmpty())
        g.fillAll(look::selection);
    if (r.heading.isNotEmpty()) {
        // A folder of Live's, as its heading.
        g.setColour(look::textDim);
        g.setFont(look::font(12.f, true));
        g.drawText(r.heading, 6, 0, width - 10, height, juce::Justification::centredLeft, true);
        return;
    }
    const bool device = r.device != nullptr;
    const int indent = device && r.device->category == "Audio Effects" ? 12 : 0;
    icon(g, device ? AudioFx : Samples,
         juce::Rectangle<float>(6.f + static_cast<float>(indent), (static_cast<float>(height) - 13.f) / 2.f, 13.f, 13.f));
    // An entry this build cannot load is dimmed (its reason is said when it is chosen).
    g.setColour(device && !unavailable(*r.device).empty() ? look::textDim : look::text);
    g.setFont(look::font(12.5f));
    const auto name = device ? juce::String(std::string(r.device->name)) : juce::String::fromUTF8(r.file->name.c_str());
    g.drawText(name, 24 + indent, 0, width - 28 - indent, height, juce::Justification::centredLeft, true);
}
juce::var BrowserPanel::getDragSourceDescription(const juce::SparseSet<int> &rows) {
    std::vector<const LibraryFile *> files;
    for (int i = 0; i < rows.size(); ++i)
        if (const int r = rows[i]; r >= 0 && r < static_cast<int>(rows_.size())) {
            const auto &row = rows_[static_cast<std::size_t>(r)];
            // A device drags alone, the first one chosen; one this build cannot load drags nothing.
            if (row.device != nullptr)
                return unavailable(*row.device).empty() ? juce::var(deviceDescription(*row.device)) : juce::var();
            if (row.file != nullptr)
                files.push_back(row.file);
        }
    return files.empty() ? juce::var() : juce::var(dragDescription(files));
}
std::string BrowserPanel::unavailable(const BuiltinInfo &d) {
    if (d.available)
        return {};
    if (d.uid == "org.adidaw.builtin.hybrid-reverb")
        return "Hybrid Reverb is not built in ADI yet.";
    return std::string(d.name) + " runs on ADI's Pd devices, which this build leaves out.";
}
juce::String BrowserPanel::deviceDescription(const BuiltinInfo &d) {
    return "adi-device:" + juce::String(std::string(d.uid));
}
std::string BrowserPanel::droppedDevice(const juce::var &description) {
    const auto text = description.toString();
    if (!text.startsWith("adi-device:"))
        return {};
    const auto uid = text.fromFirstOccurrenceOf("adi-device:", false, false).toStdString();
    return findBuiltin(uid) != nullptr ? uid : std::string();
}
bool BrowserPanel::load(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].device == nullptr)
        return false;
    const auto &d = *rows_[static_cast<std::size_t>(row)].device;
    if (const auto why = unavailable(d); !why.empty()) {
        root_.say(why);
        return false;
    }
    // Onto the selected track, in front of the device chosen in the strip (manual 4.10, 41.4).
    return root_.insertDevice(std::string(d.uid), root_.arrangement.geometry.selection.track,
                              root_.devices.insertPoint());
}
void BrowserPanel::resized() {
    // Live's shape: the search row and the preview row start right of the round
    // buttons; the panes run the panel's width between them, one dark-edged block.
    const auto r = getLocalBounds();
    const int edge = look::border;
    search.setBounds(kStripWidth + edge, edge, std::max(0, r.getWidth() - kStripWidth - 2 * edge), 21);
    const int top = kBarHeight - edge + edge, bottom = r.getHeight() - kBarHeight - edge;
    sidebarArea_ = {edge, top, kSidebarWidth, std::max(0, bottom - top)};
    contentArea_ = {sidebarArea_.getRight() + 2, top,
                    std::max(0, r.getWidth() - sidebarArea_.getRight() - 2 - edge), sidebarArea_.getHeight()};
    // Under the list's header, and above the AI tab at the panes' foot.
    auto rows = contentArea_.withTrimmedTop(19);
    list.setBounds(rows.withBottom(std::max(rows.getY(), std::min(rows.getBottom(), footLimit_))));
}
void BrowserPanel::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    const float w = static_cast<float>(getWidth()), h = static_cast<float>(getHeight());
    const float strip = kStripWidth, bar = kBarHeight;
    // The dark edge: the search row, the panes, the preview row, as one block.
    g.setColour(juce::Colour(0xff444444));
    g.fillRoundedRectangle(strip, 0, w - strip, bar + 4, 5.f);
    g.fillRoundedRectangle(0, bar - 4, w, h - 2 * bar + 8, 5.f);
    g.fillRoundedRectangle(strip, h - bar - 4, w - strip, bar + 4, 5.f);
    // The round buttons: show/hide the browser at the top, the groove pool at the foot.
    for (const float y : {0.5f, h - 19.5f}) {
        g.setColour(look::band);
        g.fillEllipse(0, y, 19, 19);
        look::triangle(g, {7, y + 5, 7, 9}, 0, look::panel);
    }
    // The search field.
    g.setColour(look::panel);
    g.fillRect(search.getBounds());
    for (auto area : {sidebarArea_, contentArea_}) {
        g.setColour(look::panel);
        g.fillRect(area);
    }
    // The rows stop at the pane's foot, under the AI tab, as a list does.
    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(sidebarArea_);
        int y = sidebarArea_.getY() + 1;
        for (std::size_t i = 0; i < sidebar_.size(); ++i) {
            const auto &e = sidebar_[i];
            const juce::Rectangle<int> row(sidebarArea_.getX(), y, sidebarArea_.getWidth(), kRowHeight);
            if (static_cast<int>(i) == selected_) {
                g.setColour(look::selection);
                g.fillRect(row);
            }
            if (e.heading) {
                g.setColour(look::textDim);
                g.setFont(look::font(12.f));
                g.drawText(e.name, row.withTrimmedLeft(5), juce::Justification::centredLeft);
            } else {
                icon(g, e.glyph, row.withTrimmedLeft(8).withWidth(13).toFloat().withSizeKeepingCentre(13, 13));
                g.setColour(look::text);
                g.setFont(look::font(12.5f));
                g.drawText(e.name, row.withTrimmedLeft(25), juce::Justification::centredLeft);
            }
            y += kRowHeight;
        }
    }
    if (ring_) {
        g.setColour(look::accent);
        g.drawRect(sidebarArea_, 1);
    }
    // The content pane's header, as Live's list.
    const juce::Rectangle<int> header(contentArea_.getX(), contentArea_.getY(), contentArea_.getWidth(), 19);
    g.setColour(look::columnHead);
    g.fillRect(header);
    g.setColour(look::text);
    g.setFont(look::font(12.f, true));
    g.drawText("Name", header.withTrimmedLeft(4), juce::Justification::centredLeft);
    look::triangle(g, {static_cast<float>(header.getRight()) - 13, static_cast<float>(header.getY()) + 6, 8, 7},
                   juce::MathConstants<float>::halfPi, look::textDim);
    // The preview row: Live's headphones switch at its left.
    const juce::Rectangle<int> preview(kStripWidth + look::border, getHeight() - kBarHeight + 2,
                                       getWidth() - kStripWidth - 2 * look::border, 21);
    g.setColour(look::panel);
    g.fillRect(preview);
    g.setColour(juce::Colour(0xff2e6fd8));
    g.drawEllipse(juce::Rectangle<float>(11, 11).withCentre(
                      {static_cast<float>(preview.getX()) + 13, static_cast<float>(preview.getCentreY())}),
                  2.f);
}
void BrowserPanel::mouseDown(const juce::MouseEvent &e) {
    if (sidebarArea_.contains(e.getPosition()))
        choose((e.y - sidebarArea_.getY() - 1) / kRowHeight);
}
void BrowserPanel::choose(int i) {
    if (i >= 0 && i < static_cast<int>(sidebar_.size()) && !sidebar_[static_cast<std::size_t>(i)].heading) {
        selected_ = i;
        repaint();
        refill();
        const auto &entry = sidebar_[static_cast<std::size_t>(i)];
        const bool library = entry.samples || entry.name == "User Library" || !entry.place.empty();
        if (entry.name == "Audio Effects" || entry.name == "Pd Devices") {
            // The catalogue's (D4): with Pd off, every entry is dimmed and the Status Bar says why.
            if (!rows_.empty() && std::none_of(rows_.begin(), rows_.end(), [](const Row &r) {
                    return r.device != nullptr && unavailable(*r.device).empty();
                }))
                root_.say("ADI's built-in devices run on its Pd devices, which this build leaves out.");
        } else if (!library)
            // The other categories say what they wait for (D4; N15).
            root_.say(entry.name == "Plug-Ins"
                          ? "Plug-Ins list once the plug-in scanner is in (#438 to #440)."
                      : entry.name == "Instruments" || entry.name == "MIDI Effects" || entry.name == "Sounds" ||
                              entry.name == "Drums"
                          ? (entry.name + " wait for the devices lane's instruments and MIDI effects.").toStdString()
                          : (entry.name + " is not listed yet.").toStdString());
        else if (!ready_)
            root_.say("The library is still being indexed; its samples list when it is done.");
        else if (shown_.empty())
            root_.say(listing_.places.empty() || (!entry.samples && entry.place.empty())
                          ? "Set the User Library or add a folder in the Settings' Library page to list samples."
                          : (entry.name + " holds no audio files.").toStdString());
    }
}
bool BrowserPanel::keyPressed(const juce::KeyPress &k) {
    // Right goes from the sidebar into the list (§41.22), its first row selected.
    if (k == juce::KeyPress::rightKey) {
        enterList();
        return true;
    }
    if (k != juce::KeyPress::upKey && k != juce::KeyPress::downKey)
        return false;
    // The next label up or down, past the headings; at either end the selection stays.
    const int step = k == juce::KeyPress::downKey ? 1 : -1;
    for (int i = selected_ + step; i >= 0 && i < static_cast<int>(sidebar_.size()); i += step)
        if (!sidebar_[static_cast<std::size_t>(i)].heading) {
            choose(i);
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
    repaint();
}
} // namespace adi::ui
