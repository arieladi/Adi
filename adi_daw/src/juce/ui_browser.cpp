// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_browser.hpp"
#include "ui_controls.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include <algorithm>
namespace adi::ui {
namespace {
constexpr std::size_t kMaxGramBytes = 3;
std::uint32_t addGramByte(std::uint32_t key, char byte) {
    return (key << 8U) | static_cast<std::uint32_t>(static_cast<unsigned char>(byte));
}
enum Glyph { None, Favorite, Sounds, Drums, Instruments, AudioFx, MidiFx, Pd, PlugIns, Clips,
             Samples, Grooves, Templates, Packs, User, Project, AddFolder };
void icon(juce::Graphics &g, int kind, juce::Rectangle<float> r) {
    g.setColour(look::text);
    const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
    juce::Path p;
    switch (kind) {
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
    default:
        break;
    }
}
} // namespace
BrowserLibrarySnapshot::BrowserLibrarySnapshot(LibraryListing listing) : listing_(std::move(listing)) {
    foldedNames_.reserve(listing_.samples.size());
    for (std::size_t i = 0; i < listing_.samples.size(); ++i) {
        const auto &sample = listing_.samples[i];
        const auto &name = foldedNames_.emplace_back(juce::String::fromUTF8(sample.name.c_str()).toLowerCase().toStdString());
        for (std::size_t start = 0; start < name.size(); ++start) {
            // A leading 1 distinguishes grams of different widths, even for non-ASCII names.
            std::uint32_t key = 1;
            const auto length = std::min(kMaxGramBytes, name.size() - start);
            for (std::size_t offset = 0; offset < length; ++offset) {
                key = addGramByte(key, name[start + offset]);
                auto &posting = postings_[key];
                if (posting.empty() || posting.back() != i)
                    posting.push_back(i);
            }
        }
    }
    // A sample deduplicated by the library index may belong to several overlapping Places.
    // Keep their path-based membership here, so changing the query never compares paths.
    for (const auto &place : listing_.places) {
        if (place.volumeId.empty())
            continue;
        auto &scope = places_[place.volumeId];
        scope.ordered.clear();
        scope.contains.assign(listing_.samples.size(), false);
        if (place.root.empty())
            continue;
        for (std::size_t i = 0; i < listing_.samples.size(); ++i) {
            const auto relative = listing_.samples[i].path.lexically_relative(place.root);
            if (!relative.empty() && *relative.begin() != "..") {
                scope.ordered.push_back(i);
                scope.contains[i] = true;
            }
        }
    }
}
void BrowserLibrarySnapshot::appendSamples(std::string_view query, const std::string *volume,
                                           std::vector<const LibraryFile *> &results) const {
    const PlaceSamples *scope = nullptr;
    if (volume != nullptr) {
        const auto found = places_.find(*volume);
        if (found == places_.end())
            return;
        scope = &found->second;
    }

    const SampleIndices *shortest = nullptr;
    const auto gramSize = std::min(kMaxGramBytes, query.size());
    if (gramSize != 0) {
        for (std::size_t start = 0; start <= query.size() - gramSize; ++start) {
            std::uint32_t key = 1;
            for (const char byte : query.substr(start, gramSize))
                key = addGramByte(key, byte);
            const auto found = postings_.find(key);
            if (found == postings_.end())
                return;
            if (shortest == nullptr || found->second.size() < shortest->size())
                shortest = &found->second;
        }
    }
    // Every posting follows library order. The rarest gram (or a smaller Place) limits
    // how many names need the full substring check. A one-gram query already matches exactly.
    if (scope != nullptr && (shortest == nullptr || scope->ordered.size() < shortest->size())) {
        for (const auto i : scope->ordered)
            if (query.empty() || foldedNames_[i].find(query) != std::string::npos)
                results.push_back(&listing_.samples[i]);
    } else if (shortest != nullptr) {
        const bool exact = gramSize == query.size();
        for (const auto i : *shortest)
            if ((scope == nullptr || scope->contains[i]) && (exact || foldedNames_[i].find(query) != std::string::npos))
                results.push_back(&listing_.samples[i]);
    } else {
        for (const auto &sample : listing_.samples)
            results.push_back(&sample);
    }
}
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
    list.onLeft = [this] { focusIfShowing(*this); };
    list.onEscape = [this] { return cancelHotSwap(); };
    addAndMakeVisible(list);
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
    if (k == juce::KeyPress::escapeKey)
        return panel.cancelHotSwap();
    if (k != juce::KeyPress::downKey)
        return false;
    panel.enterList();
    return true;
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
void BrowserPanel::setLibrarySnapshot(std::shared_ptr<const BrowserLibrarySnapshot> library, bool ready) {
    // The label chosen stays chosen across a new listing, found again by its name and place.
    const auto keep = selected_ >= 0 ? sidebar_[static_cast<std::size_t>(selected_)] : Entry{};
    library_ = std::move(library);
    ready_ = ready;
    // Live's added folders stand among the Places, after Current Project and before Add Folder; the
    // User Library's own label is Live's.
    sidebar_.resize(baseEntries_);
    auto addFolder = sidebar_.back();
    sidebar_.pop_back();
    for (auto &e : sidebar_)
        if (e.name == "User Library" && !e.heading)
            e.place.clear();
    for (const auto &place : listing().places) {
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
void BrowserPanel::setDevicePresets(std::vector<preset::Entry> entries) {
    devicePresets_.clear();
    for (auto &entry : entries)
        devicePresets_[entry.deviceKey].push_back(std::move(entry));
    for (auto &[key, children] : devicePresets_) {
        (void)key;
        std::sort(children.begin(), children.end(), [](const preset::Entry &a, const preset::Entry &b) {
            const int order = juce::String::fromUTF8(a.name.c_str()).compareIgnoreCase(juce::String::fromUTF8(b.name.c_str()));
            return order != 0 ? order < 0 : a.path < b.path;
        });
    }
    refill();
}
void BrowserPanel::setHotSwap(std::optional<HotSwapFilter> filter) {
    if (!hotSwap_ && !filter)
        return;
    if (!hotSwap_) {
        idleSearch_ = search.getText();
        search.setText({}, false);
    } else if (!filter) {
        search.setText(std::exchange(idleSearch_, {}), false);
    }
    hotSwap_ = std::move(filter);
    list.setMultipleSelectionEnabled(!hotSwap_);
    refill();
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
void BrowserPanel::refill() {
    shown_.clear();
    const Entry none{};
    const auto &chosen = !hotSwap_ && selected_ >= 0 ? sidebar_[static_cast<std::size_t>(selected_)] : none;
    const bool swappingDevices = hotSwap_ && !hotSwap_->samples;
    using Role = ::adi::detail::DeviceRole;
    const auto swapRole = swappingDevices ? ::adi::detail::deviceRole(hotSwap_->subtype) : Role::AudioEffect;
    const auto text = search.getText();
    // Samples, or a Place the listing holds, by its volume; the search alone looks through Samples.
    const bool samples = hotSwap_ ? hotSwap_->samples
                                  : chosen.samples || (selected_ < 0 && text.isNotEmpty());
    const auto query = text.trim().toLowerCase();
    if (library_ && (samples || !chosen.place.empty()))
        library_->appendSamples(query.toRawUTF8(), samples ? nullptr : &chosen.place, shown_);
    rows_.clear();
    rows_.reserve(shown_.size());
    for (const auto *f : shown_)
        rows_.push_back({f, nullptr, {}});
    if (swappingDevices && pluginScan_.phase == PluginScan::Phase::Scanning)
        rows_.push_back({nullptr, nullptr, juce::String::fromUTF8(pluginScanStatus(pluginScan_).c_str())});
    // Audio Effects in Live's folders (manual 4.10), Pd Devices ADI's own, from the catalogue.
    const auto matches = [&query](std::string_view name) {
        return query.isEmpty() || juce::String::fromUTF8(name.data(), static_cast<int>(name.size())).containsIgnoreCase(query);
    };
    const auto visiblePresets = [&](std::string_view format, std::string_view uid,
                                    std::optional<std::int64_t> shellId, bool entireDevice) {
        std::vector<const preset::Entry *> out;
        if (!devicePresets_.empty()) {
            const auto found = devicePresets_.find(preset::deviceKey({std::string(format), std::string(uid), shellId}));
            if (found != devicePresets_.end())
                for (const auto &entry : found->second)
                    if (entireDevice || matches(entry.name))
                        out.push_back(&entry);
        }
        return out;
    };
    const auto addPresets = [&](const std::vector<const preset::Entry *> &presets, int depth) {
        for (const auto *entry : presets)
            rows_.push_back({nullptr, nullptr, {}, nullptr, depth, entry});
    };
    const auto addBuiltin = [&](const BuiltinInfo &d, int depth, const char *heading = nullptr) {
        if (swappingDevices && (!d.available || swapRole != (d.instrument ? Role::Instrument : Role::AudioEffect)))
            return false;
        const bool ownMatch = matches(d.name);
        const auto presets = visiblePresets("internal", d.uid, std::nullopt, ownMatch);
        if (!ownMatch && presets.empty())
            return false;
        if (heading != nullptr)
            rows_.push_back({nullptr, nullptr, heading});
        rows_.push_back({nullptr, &d, {}, nullptr, depth});
        addPresets(presets, depth + 1);
        return true;
    };
    if (swappingDevices || (!chosen.heading && chosen.name == "Audio Effects")) {
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
    if (swappingDevices || (!chosen.heading && chosen.name == "Instruments")) {
        // ADI's own instruments, by name, where Live lists its instruments (walk-A A7, DF-W48).
        std::vector<const BuiltinInfo *> in;
        for (const auto &d : builtinCatalog())
            if (d.category == "Instruments")
                in.push_back(&d);
        std::sort(in.begin(), in.end(), [](const BuiltinInfo *a, const BuiltinInfo *b) { return a->name < b->name; });
        for (const auto *d : in)
            addBuiltin(*d, 0);
    }
    if (swappingDevices || (!chosen.heading && chosen.name == "Pd Devices")) {
        for (const auto &d : builtinCatalog())
            if (d.category == "ADI")
                addBuiltin(d, 0);
    }
    if (swappingDevices || (!chosen.heading && chosen.name == "Plug-Ins")) {
        // Live's Plug-Ins: a folder per format, in it a folder per vendor, in that its plug-ins by name.
        std::string format;
        // While a scan runs its line heads the list, above what was listed before it.
        if (!swappingDevices && pluginScan_.phase == PluginScan::Phase::Scanning)
            rows_.push_back({nullptr, nullptr, juce::String::fromUTF8(pluginScanStatus(pluginScan_).c_str()), nullptr, 0});
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
                const bool ownMatch = matches(p->name) || matches(p->vendor);
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
            if (!query.isEmpty() && !name.containsIgnoreCase(query))
                continue;
            const auto heading = (failure.state == scan::State::Quarantined ? "Blocked: " : "Scan failed: ") + name;
            const auto reason = juce::String::fromUTF8(failure.error.c_str());
            const auto tooltip = reason.isEmpty() ? path : path + "\n" + reason;
            rows_.push_back({nullptr, nullptr, heading, nullptr, 0, nullptr, tooltip});
        }
    }
    if (hotSwap_ && rows_.empty())
        rows_.push_back({nullptr, nullptr, samples ? "No matching samples" : "No matching devices or presets"});
    list.updateContent();
    list.deselectAllRows();
    lastSelected_ = -1;
    list.repaint();
}
int BrowserPanel::getNumRows() { return static_cast<int>(rows_.size()); }
juce::String BrowserPanel::getTooltipForRow(int row) {
    return row >= 0 && row < getNumRows() ? rows_[static_cast<std::size_t>(row)].tooltip : juce::String{};
}
void BrowserPanel::paintListBoxItem(int row, juce::Graphics &g, int width, int height, bool selected) {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return;
    const auto &r = rows_[static_cast<std::size_t>(row)];
    if (selected && r.heading.isEmpty())
        g.fillAll(look::selection);
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
        g.drawText(juce::String::fromUTF8(r.plugin->name.c_str()), 24 + in, 0, width - 28 - in, height,
                   juce::Justification::centredLeft, true);
        return;
    }
    const bool device = r.device != nullptr;
    const int indent = 12 * r.depth;
    icon(g, device ? (r.device->category == "Instruments" ? Instruments : AudioFx) : Samples,
         juce::Rectangle<float>(6.f + static_cast<float>(indent), (static_cast<float>(height) - 13.f) / 2.f, 13.f, 13.f));
    // An entry this build cannot load is dimmed (its reason is said when it is chosen).
    g.setColour(device && !unavailable(*r.device).empty() ? look::textDim : look::text);
    g.setFont(look::font(look::fontHeading));
    const auto name = device ? juce::String(std::string(r.device->name)) : juce::String::fromUTF8(r.file->name.c_str());
    g.drawText(name, 24 + indent, 0, width - 28 - indent, height, juce::Justification::centredLeft, true);
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
    return files.empty() ? juce::var() : juce::var(dragDescription(files));
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
    return selected_ >= 0 && selected_ < static_cast<int>(sidebar_.size()) &&
           !sidebar_[static_cast<std::size_t>(selected_)].heading &&
           sidebar_[static_cast<std::size_t>(selected_)].name == "Plug-Ins";
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
        if (!rows_.empty() && rows_.front().plugin == nullptr && rows_.front().device == nullptr) {
            rows_.front().heading = juce::String::fromUTF8(pluginScanStatus(pluginScan_).c_str());
            repaint();
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
    if (row.file)
        return swapChoice({Kind::Sample, {}, {}, row.file->path});
    return false;
}
bool BrowserPanel::load(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return false;
    const auto &entry = rows_[static_cast<std::size_t>(row)];
    if (hotSwap_)
        return loadHotSwap(entry);
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
void BrowserPanel::resized() {
    // Live's shape: the search row and the preview row start right of the round
    // buttons; the panes run the panel's width between them, one dark-edged block.
    const auto r = getLocalBounds();
    const int edge = look::border;
    search.setBounds(kStripWidth + edge, edge, std::max(0, r.getWidth() - kStripWidth - 2 * edge), 21);
    const int top = kBarHeight, bottom = r.getHeight() - kBarHeight - edge;
    sidebarArea_ = {edge, top, kSidebarWidth, std::max(0, bottom - top)};
    contentArea_ = {sidebarArea_.getRight() + 2, top,
                    std::max(0, r.getWidth() - sidebarArea_.getRight() - 2 - edge), sidebarArea_.getHeight()};
    // The list fills the content pane below its header.
    list.setBounds(contentArea_.withTrimmedTop(19));
}
void BrowserPanel::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    const float w = static_cast<float>(getWidth()), h = static_cast<float>(getHeight());
    const float strip = kStripWidth, bar = kBarHeight;
    // The dark edge: the search row, the panes, the preview row, as one block.
    g.setColour(look::browserEdge);
    g.fillRoundedRectangle(strip, 0, w - strip, bar + 4, look::radiusControl);
    g.fillRoundedRectangle(0, bar - 4, w, h - 2 * bar + 8, look::radiusControl);
    g.fillRoundedRectangle(strip, h - bar - 4, w - strip, bar + 4, look::radiusControl);
    // The round buttons: show/hide the browser at the top, the groove pool at the foot.
    // Round, in the shared button's colours at rest (language.png part B).
    for (const float y : {0.5f, h - 19.5f}) {
        controls::roundButton(g, {0, y, 19, 19}, controls::ButtonState::Rest);
        look::triangle(g, {7, y + 5, 7, 9}, 0, controls::labelColour(controls::ButtonState::Rest));
    }
    // The search field.
    g.setColour(look::panel);
    g.fillRect(search.getBounds());
    for (auto area : {sidebarArea_, contentArea_}) {
        g.setColour(look::panel);
        g.fillRect(area);
    }
    // The rows stop at the pane's foot.
    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(sidebarArea_);
        int y = sidebarArea_.getY() + 1;
        for (std::size_t i = 0; i < sidebar_.size(); ++i) {
            const auto &e = sidebar_[i];
            const juce::Rectangle<int> row(sidebarArea_.getX(), y, sidebarArea_.getWidth(), kRowHeight);
            if (!hotSwap_ && static_cast<int>(i) == selected_) {
                g.setColour(look::selection);
                g.fillRect(row);
            }
            if (e.heading) {
                g.setColour(look::textDim);
                g.setFont(look::font(look::fontField));
                g.drawText(e.name, row.withTrimmedLeft(5), juce::Justification::centredLeft);
            } else {
                icon(g, e.glyph, row.withTrimmedLeft(8).withWidth(13).toFloat().withSizeKeepingCentre(13, 13));
                g.setColour(hotSwap_ ? look::textDim : look::text);
                g.setFont(look::font(look::fontHeading));
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
    g.setFont(look::font(look::fontField, true));
    if (hotSwap_) {
        const auto target = juce::String::fromUTF8(hotSwap_->targetName.c_str());
        g.setColour(look::accent);
        g.drawFittedText(target.isEmpty() ? juce::String("Hot-Swap") : "Hot-Swap  " + target,
                         header.reduced(4, 0), juce::Justification::centredLeft, 1);
    } else {
        g.setColour(look::text);
        g.drawText("Name", header.withTrimmedLeft(4), juce::Justification::centredLeft);
        look::triangle(g, {static_cast<float>(header.getRight()) - 13, static_cast<float>(header.getY()) + 6, 8, 7},
                       juce::MathConstants<float>::halfPi, look::textDim);
    }
    // The preview row: Live's headphones switch at its left.
    const juce::Rectangle<int> preview(kStripWidth + look::border, getHeight() - kBarHeight + 2,
                                       getWidth() - kStripWidth - 2 * look::border, 21);
    g.setColour(look::panel);
    g.fillRect(preview);
    // Round, in the shared button's colours at rest, its preview ring kept.
    const auto headphones = juce::Rectangle<float>(11, 11).withCentre(
        {static_cast<float>(preview.getX()) + 13, static_cast<float>(preview.getCentreY())});
    controls::roundButton(g, headphones, controls::ButtonState::Rest);
    g.setColour(look::focus);
    g.drawEllipse(headphones, 2.f);
}
void BrowserPanel::mouseDown(const juce::MouseEvent &e) {
    if (sidebarArea_.contains(e.getPosition()))
        choose((e.y - sidebarArea_.getY() - 1) / kRowHeight);
}
void BrowserPanel::choose(int i) {
    if (hotSwap_)
        return;
    if (i >= 0 && i < static_cast<int>(sidebar_.size()) && !sidebar_[static_cast<std::size_t>(i)].heading) {
        selected_ = i;
        const auto &entry = sidebar_[static_cast<std::size_t>(i)];
        // Plug-Ins lists what the app's scan found; the first time, it asks the app to scan, which runs on
        // a worker and fills the list when it ends (DF-W9, DF-W34): choosing never waits on a scan.
        const bool plugIns = entry.name == "Plug-Ins";
        if (plugIns && scanPlugins && pluginScan_.phase == PluginScan::Phase::NotStarted)
            scanPlugins();
        repaint();
        refill();
        const bool library = entry.samples || entry.name == "User Library" || !entry.place.empty();
        if (entry.name == "Instruments" || entry.name == "Audio Effects" || entry.name == "Pd Devices") {
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
        } else if (!library)
            // The other categories say what they wait for (D4; N15).
            root_.say(entry.name == "MIDI Effects" || entry.name == "Sounds" ||
                              entry.name == "Drums"
                          ? (entry.name + " wait for the devices lane's instruments and MIDI effects.").toStdString()
                          : (entry.name + " is not listed yet.").toStdString());
        else if (!ready_)
            root_.say("The library is still being indexed; its samples list when it is done.");
        else if (shown_.empty())
            root_.say(listing().places.empty() || (!entry.samples && entry.place.empty())
                          ? "Set the User Library or add a folder in the Settings' Library page to list samples."
                          : (entry.name + " holds no audio files.").toStdString());
    }
}
bool BrowserPanel::keyPressed(const juce::KeyPress &k) {
    if (k == juce::KeyPress::escapeKey && cancelHotSwap())
        return true;
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
