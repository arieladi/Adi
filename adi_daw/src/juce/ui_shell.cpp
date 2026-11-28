// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_shell.hpp"
#include "adi/engine/midi_input_route.hpp"
#include "adi/engine/plan.hpp"
#include "adi/ui/playback_problems.hpp"
#include "adi/automation_splice_store.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/track_image.hpp"
#include "adi/media/media_ops.hpp"
#include "ui_computer_midi_keys.hpp"
#include "ui_controls.hpp"
#include "ui_floating.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/row_ids.hpp"
#include "adi/interchange/midi_clip_file.hpp"
#include "adi/clip_image.hpp"
#include "adi/ui/clip_edits.hpp"
#include "adi/ui/cue_edits.hpp"
#include "adi/ui/fade_edits.hpp"
#include "adi/ui/meter_edits.hpp"
#include "adi/ui/meters.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/ui/recording_edits.hpp"
#include "adi/ui/send_edits.hpp"
#include "adi/ui/tempo_edits.hpp"
#include "adi/ui/track_edits.hpp"
#include "adi/store.hpp"
#include "adi/controller_lanes.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <sqlite3.h>
#include <string_view>
#include <vector>
namespace adi::ui {
namespace {
bool deviceOrTrackEditCommand(int id) noexcept {
    return id == AppCommands::Cut || id == AppCommands::Copy || id == AppCommands::Paste ||
           id == AppCommands::Duplicate || id == AppCommands::RenameTrack || id == AppCommands::DeleteTrack;
}
// SC-258 provisional policy pending a Live 12 reading of Undo during Arrangement Record.
constexpr bool kHoldHistoryUntilArrangementTakeLands = true;
constexpr auto kRecordingHistoryMessage = "Undo and Redo are available after the current take finishes.";
constexpr int kRecentProjectLimit = 12;
// Interface zoom steps per window. PROVISIONAL: Live's own list is to be
// measured (ADR-0203 d26); 100 % is the start and the steps are symmetric.
constexpr std::array<float, 7> kZoomSteps{.5f, .75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f};
bool audioDestination(const rows::Track &track, const rows::Track &target) {
    if (track.id == target.id || track.kind == "master" || track.kind == "vca")
        return false;
    if (track.kind == "return" && track.parentId)
        return *track.parentId == target.id && target.kind == "group";
    return target.kind == "master" || target.kind == "audio" || target.kind == "instrument" ||
           target.kind == "group" || target.kind == "return";
}

std::int64_t defaultAudioDestination(const rows::Model &model, const rows::Track &track) {
    if (track.parentId)
        return *track.parentId;
    std::int64_t main = 0;
    for (const auto &target : model.tracks)
        if (target.kind == "master" && (main == 0 || target.id < main))
            main = target.id;
    return main;
}
bool flag(const settings::AppSettings &s, const char *key, bool fallback) {
    const auto v = s.get(key);
    return v.is_boolean() ? v.get<bool>() : fallback;
}
} // namespace
AppCommands::AppCommands(settings::AppSettings &settings) : settings_(settings) {
    recentProjects_.setMaxNumberOfItems(kRecentProjectLimit);
    if (const auto directory = settings_.file().parent_path().u8string(); !directory.empty()) {
        recentProjectsFile_ = juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(directory.c_str())))
                                  .getChildFile("recent-projects.json");
        const auto recent = juce::JSON::parse(recentProjectsFile_);
        if (const auto *paths = recent.getArray()) {
            for (int i = std::min(paths->size(), kRecentProjectLimit); i > 0; --i) {
                const auto &value = (*paths)[i - 1];
                const auto path = value.toString();
                if (value.isString() && juce::File::isAbsolutePath(path))
                    recentProjects_.addFile(juce::File(path));
            }
        }
    }
    manager.setFirstCommandTarget(this);
    reload();
}
void AppCommands::saveRecentProjects() {
    if (recentProjectsFile_ == juce::File{} || !recentProjectsFile_.getParentDirectory().createDirectory().wasOk())
        return;
    juce::Array<juce::var> paths;
    for (int i = 0; i < recentProjects_.getNumFiles(); ++i)
        paths.add(recentProjects_.getFile(i).getFullPathName());
    (void)recentProjectsFile_.replaceWithText(juce::JSON::toString(juce::var(std::move(paths))));
}
void AppCommands::rememberProject(const juce::File &file) {
    if (recentProjects_.getFile(0) == file)
        return;
    recentProjects_.addFile(file);
    saveRecentProjects();
}
void AppCommands::clearRecentProjects(bool onlyUnavailable) {
    if (onlyUnavailable) {
        for (int i = recentProjects_.getNumFiles(); i-- > 0;)
            if (const auto file = recentProjects_.getFile(i); !file.existsAsFile())
                recentProjects_.removeFile(file);
    } else {
        recentProjects_.clear();
    }
    saveRecentProjects();
}
void AppCommands::reload() {
    live_ = settings_.get("shortcuts.preset") == "live";
    infoView_ = flag(settings_, "lookfeel.showInfoView", true);
    browserPreviewEnabled_ = flag(settings_, settings::kBrowserPreviewEnabledKey, settings::kBrowserPreviewStartsEnabled);
    tooltips_ = flag(settings_, "lookfeel.showTooltips", true);
    const auto delay = settings_.get("lookfeel.tooltipDelayMs");
    tooltipDelayMs_ = delay.is_number_integer() ? delay.get<int>() : 600;
    zoomOnSelection_ = flag(settings_, "lookfeel.zoomOnSelection", true);
    multiplePluginWindows_ = flag(settings_, "plugins.multipleWindows", false);
    autoHidePluginWindows_ = flag(settings_, "plugins.autoHideWindows", true);
    autoOpenPluginWindows_ = flag(settings_, "plugins.autoOpenWindows", true);
    pluginWindowsOnTop_ = flag(settings_, "plugins.editorsAlwaysOnTop", true);
    tabMovesFocus_ = flag(settings_, "lookfeel.tabMovesFocus", false);
    delayCompensation_ = flag(settings_, "audio.delayCompensation", true);
    envelopesLocked_ = !flag(settings_, "editing.automationFollowsClips", true);
    wrapTab_ = flag(settings_, "lookfeel.wrapTabNavigation", true);
    arrowKeysMoveClips_ = flag(settings_, "editing.arrowKeysMoveClips", true);
    followByPage_ = settings_.get("lookfeel.followMode") != "scroll";
    exclusiveSolo_ = flag(settings_, "record.exclusiveSolo", true);
    exclusiveArm_ = flag(settings_, "record.exclusiveArm", true);
    startPlaybackWithRecord_ = flag(settings_, "record.startPlaybackWithRecord", true);
    const auto armOnSelect = settings_.get("record.autoArmOnSelect");
    const auto nextArm = armOnSelect == "all" ? SelectionArm::All
                         : armOnSelect == "instrumentAndMidi" ? SelectionArm::Midi : SelectionArm::None;
    const bool armModeChanged = std::exchange(selectionArm_, nextArm) != nextArm;
    if (const auto quantize = settings_.get("record.quantize"); quantize.is_string())
        recordingDefaults_.quantize = quantize.get<std::string>();
    recordingDefaults_.keepMonitoringLatency = flag(settings_, "audio.keepMonitoringLatency", true);
    linkLoop_ = flag(settings_, "editing.linkLoopToSelection", false);
    splitAll_ = flag(settings_, "editing.splitAllWithNoSelection", true);
    // Appearance is shared by the windows. Existing "os" profiles resolve to Dark.
    const auto word = [this](const char *key) {
        const auto v = settings_.get(key);
        return v.is_string() ? v.get<std::string>() : std::string{};
    };
    if (look::apply(themeFromSetting(word("lookfeel.theme")), densityFromSetting(word("lookfeel.density"))))
        for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
            if (auto *c = juce::Desktop::getInstance().getComponent(i))
                c->repaint();
    // A preset changes default keys, so the old registrations go first.
    manager.clearCommands();
    manager.registerAllCommandsForTarget(this);
    manager.getKeyMappings()->resetToDefaultMappings();
    // WIN-04: the user's own keys on top, after every reload, so a Settings change or a preset switch
    // keeps them: a stable id mapped to chord texts, an empty list for none (shortcuts.overrides).
    if (const auto overrides = settings_.get("shortcuts.overrides"); overrides.is_object())
        for (const auto &[stable, chords] : overrides.items()) {
            const int command = commandFor(stable);
            if (command == 0 || !chords.is_array())
                continue;
            auto *mappings = manager.getKeyMappings();
            mappings->clearAllKeyPresses(command);
            for (const auto &chord : chords)
                if (chord.is_string())
                    if (const auto key = parseChord(chord.get<std::string>()))
                        mappings->addKeyPress(command, *key);
        }
    if (active_ && armModeChanged)
        active_->refreshSelectionArming();
}
settings::Value AppCommands::userKeys() const {
    auto out = settings::Value::object();
    const auto *mappings = manager.getKeyMappings();
    for (int id = PlayStop; id <= LastId; ++id) {
        juce::ApplicationCommandInfo info(id);
        const_cast<AppCommands *>(this)->getCommandInfo(id, info);
        std::vector<std::string> mine, theirs;
        for (const auto &k : mappings->getKeyPressesAssignedToCommand(id))
            mine.push_back(chordText(k));
        for (const auto &k : info.defaultKeypresses)
            theirs.push_back(chordText(k));
        std::sort(mine.begin(), mine.end());
        std::sort(theirs.begin(), theirs.end());
        if (mine != theirs)
            out[stableId(id)] = mine;
    }
    return out;
}
bool AppCommands::storeUserKeys(std::string &why) {
    const auto held = settings_.get("shortcuts.overrides");
    auto keys = userKeys();
    if (held.is_object()) {
        const auto *mappings = manager.getKeyMappings();
        for (const auto &[stable, chords] : held.items()) {
            const int command = commandFor(stable);
            // A stable id this build does not know (a later build's command) keeps its keys.
            if (command == 0) {
                keys[stable] = chords;
                continue;
            }
            // A held entry the command still has keeps its place even where it equals the preset's keys,
            // so reload()'s own change messages, or a preset whose keys match, never drop it.
            std::vector<std::string> now, kept;
            for (const auto &k : mappings->getKeyPressesAssignedToCommand(command))
                now.push_back(chordText(k));
            if (chords.is_array())
                for (const auto &chord : chords)
                    if (chord.is_string())
                        kept.push_back(chord.get<std::string>());
            std::sort(now.begin(), now.end());
            std::sort(kept.begin(), kept.end());
            if (now == kept && !keys.contains(stable))
                keys[stable] = chords;
        }
    }
    if (keys == held)
        return true;
    return settings_.set("shortcuts.overrides", keys, {}, why) && settings_.save(why);
}
ShortcutsPage::ShortcutsPage(AppCommands &commands)
    : commands_(commands), editor_(*commands.manager.getKeyMappings(), true) {
    // setColours, not setColour: only it reaches the editor's tree, which fills most of it.
    editor_.setColours(look::panel, look::text);
    addAndMakeVisible(editor_);
    commands_.manager.getKeyMappings()->addChangeListener(this);
    setSize(560, kHeight);
}
ShortcutsPage::~ShortcutsPage() { commands_.manager.getKeyMappings()->removeChangeListener(this); }
void ShortcutsPage::paint(juce::Graphics &g) {
    if (error_.empty())
        return;
    g.setColour(look::textDim);
    g.setFont(look::font(look::fontBody));
    g.drawText(juce::String::fromUTF8(error_.c_str()), getLocalBounds().removeFromBottom(kErrorHeight).reduced(17, 0),
               juce::Justification::centredLeft, true);
}
void ShortcutsPage::resized() { editor_.setBounds(getLocalBounds().withTrimmedBottom(kErrorHeight)); }
void ShortcutsPage::store() {
    std::string why;
    const bool ok = commands_.storeUserKeys(why);
    const std::string said = ok ? std::string() : "Your keys were not saved: " + why;
    if (said != error_) {
        error_ = said;
        repaint();
    }
}
void ShortcutsPage::changeListenerCallback(juce::ChangeBroadcaster *) { store(); }
namespace {
struct StableEntry {
    const char *id;
    const char *category;
};
const StableEntry *stableEntry(int id) noexcept {
    static constexpr auto table = std::to_array<StableEntry>({
        {"transport.play-stop", "Transport"}, // PlayStop
        {"transport.stop", "Transport"}, // Stop
        {"edit.undo", "Edit"}, // Undo
        {"edit.redo", "Edit"}, // Redo
        {"view.swap-panels", "View"}, // SwapPanels
        {"view.browser", "View"}, // ToggleBrowser
        {"arrangement.mixer", "View"}, // ToggleMixer
        {"view.device-view", "View"}, // ToggleDeviceView
        {"view.info-view", "View"}, // ToggleInfoView
        {"view.zoom-in", "View"}, // ZoomIn
        {"view.zoom-out", "View"}, // ZoomOut
        {"file.new-project", "File"}, // NewProject
        {"file.open-project", "File"}, // OpenProject
        {"file.take-snapshot", "File"}, // TakeSnapshot
        {"file.save-as", "File"}, // SaveAs
        {"options.settings", "Options"}, // Settings
        {"arrangement.time-zoom-in", "Arrangement"}, // TimeZoomIn
        {"arrangement.time-zoom-out", "Arrangement"}, // TimeZoomOut
        {"view.zoom-to-selection", "View"}, // ZoomToSelection
        {"view.zoom-back", "View"}, // ZoomBack
        {"view.optimize-width", "View"}, // OptimizeWidth
        {"view.optimize-height", "View"}, // OptimizeHeight
        {"transport.go-start", "Transport"}, // GoStart
        {"transport.continue-play", "Transport"}, // ContinuePlay
        {"create.insert-audio-track", "Create"}, // InsertAudioTrack
        {"create.insert-midi-track", "Create"}, // InsertMidiTrack
        {"edit.delete-track", "Edit"}, // DeleteTrack
        {"edit.rename-track", "Edit"}, // RenameTrack
        {"view.mixer-view", "View"}, // ToggleMixerView
        {"view.show-channel-eq", "View"}, // ShowChannelEq
        {"file.quit", "File"}, // Quit
        {"navigate.focus-control-bar", "Navigate"}, // FocusControlBar
        {"navigate.focus-arrangement", "Navigate"}, // FocusArrangement
        {"navigate.focus-device-view", "Navigate"}, // FocusDeviceView
        {"navigate.focus-browser", "Navigate"}, // FocusBrowser
        {"arrangement.search-browser", "Arrangement"}, // SearchBrowser
        {"navigate.tab-moves-focus", "Navigate"}, // TabMovesFocus
        {"options.follow", "Options"}, // Follow
        {"arrangement.insert-marker-to-playhead", "Arrangement"}, // InsertMarkerToPlayhead
        {"arrangement.grow-tracks", "Arrangement"}, // GrowTracks
        {"arrangement.shrink-tracks", "Arrangement"}, // ShrinkTracks
        {"mixer.focus-mixer", "Mixer"}, // FocusMixer
        {"navigate.next-neighbor", "Navigate"}, // NextNeighbor
        {"navigate.previous-neighbor", "Navigate"}, // PreviousNeighbor
        {"view.full-screen", "View"}, // FullScreen
        {"view.overview", "View"}, // ToggleOverview
        {"arrangement.in-out", "Arrangement"}, // ToggleInOut
        {"options.narrow-grid", "Options"}, // NarrowGrid
        {"options.widen-grid", "Options"}, // WidenGrid
        {"options.triplet-grid", "Options"}, // TripletGrid
        {"options.snap-to-grid", "Options"}, // SnapToGrid
        {"options.fixed-grid", "Options"}, // FixedGrid
        {"navigate.wrap-tab-navigation", "Navigate"}, // WrapTabNavigation
        {"view.large-tracks", "View"}, // LargeTracks
        {"edit.group-tracks", "Edit"}, // GroupTracks
        {"edit.ungroup-tracks", "Edit"}, // UngroupTracks
        {"view.mix-view", "View"}, // ToggleMixView
        {"view.detach-mix-view", "View"}, // DetachMixView
        {"view.reference-track", "View"}, // ToggleReferenceTrack
        {"view.play-reference", "View"}, // PlayReference
        {"edit.select-all", "Edit"}, // SelectAll
        {"create.insert-return-track", "Create"}, // InsertReturnTrack
        {"arrangement.return-tracks", "View"}, // ToggleReturnTracks
        {"arrangement.sends", "View"}, // ToggleSends
        {"arrangement.mixer-in-out", "View"}, // ToggleMixerInOut
        {"mixer.solo-selected", "Mixer"}, // SoloSelected
        {"mixer.deactivate-selected", "Mixer"}, // DeactivateSelected
        {"mixer.activate-track-1", "Mixer"}, // ActivateTrack1
        {"mixer.activate-track-2", "Mixer"}, // ActivateTrack2
        {"mixer.activate-track-3", "Mixer"}, // ActivateTrack3
        {"mixer.activate-track-4", "Mixer"}, // ActivateTrack4
        {"mixer.activate-track-5", "Mixer"}, // ActivateTrack5
        {"mixer.activate-track-6", "Mixer"}, // ActivateTrack6
        {"mixer.activate-track-7", "Mixer"}, // ActivateTrack7
        {"mixer.activate-track-8", "Mixer"}, // ActivateTrack8
        {"arrangement.mixer-sends", "View"}, // ToggleMixerSends
        {"create.insert-time-signature-change", "Create"}, // InsertTimeSignatureChange
        {"options.solo-in-place", "Options"}, // SoloInPlace
        {"arrangement.track-options", "Arrangement"}, // ToggleTrackOptions
        {"arrangement.mixer-track-options", "View"}, // ToggleMixerTrackOptions
        {"transport.metronome", "Transport"}, // Metronome
        {"edit.loop-selection", "Edit"}, // LoopSelection
        {"edit.select-loop", "Edit"}, // SelectLoop
        {"create.add-locator", "Create"}, // AddLocator
        {"create.delete-locator", "Create"}, // DeleteLocator
        {"edit.edit-info-text", "Edit"}, // EditInfoText
        {"view.undo-history", "View"}, // UndoHistory
        {"file.save-copy", "File"}, // SaveCopy
        {"file.collect-all-and-save", "File"}, // CollectAllAndSave
        {"file.collect-and-export", "File"}, // CollectAndExport
        {"edit.cut", "Edit"}, // Cut
        {"edit.copy", "Edit"}, // Copy
        {"edit.paste", "Edit"}, // Paste
        {"edit.duplicate", "Edit"}, // Duplicate
        {"view.plugin-windows", "View"}, // TogglePluginWindows
        {"edit.split", "Edit"}, // Split
        {"view.clip-view", "View"}, // ToggleClipView
        {"create.insert-midi-clip", "Create"}, // InsertMidiClip
        {"edit.cut-time", "Edit"}, // CutTime
        {"edit.copy-time", "Edit"}, // CopyTime
        {"edit.paste-time", "Edit"}, // PasteTime
        {"edit.duplicate-time", "Edit"}, // DuplicateTime
        {"edit.delete-time", "Edit"}, // DeleteTime
        {"edit.insert-silence", "Edit"}, // InsertSilence
        {"create.create-fade", "Create"}, // CreateFade
        {"create.delete-fades", "Create"}, // DeleteFades
        {"edit.invert-selection", "Edit"}, // InvertSelection
        {"edit.quantize", "Edit"}, // Quantize
        {"edit.quantize-settings", "Edit"}, // QuantizeSettings
        {"edit.crop-clip", "Edit"}, // CropClip
        {"view.expand-clip-view", "View"}, // ExpandClipView
        {"view.fold-to-notes", "View"}, // FoldToNotes
        {"view.fold-to-scale", "View"}, // FoldToScale
        {"view.highlight-scales", "View"}, // HighlightScales
        {"options.draw-mode", "Options"}, // DrawMode
        {"view.arrange-panels-automatically", "View"}, // ArrangePanelsAutomatically
        {"view.arrange-panels-horizontally", "View"}, // ArrangePanelsHorizontally
        {"view.arrange-panels-vertically", "View"}, // ArrangePanelsVertically
        {"navigate.focus-clip-view", "Navigate"}, // FocusClipView
        {"edit.fit-to-time-range", "Edit"}, // FitToTimeRange
        {"transport.arrangement-record", "Transport"}, // ArrangementRecord
        {"file.export-audio", "File"}, // ExportAudio
        {"options.delay-compensation", "Options"}, // DelayCompensation
        {"options.solo-switches", "Options"}, // SoloSwitches
        {"options.cue-switches", "Options"}, // CueSwitches
        {"help.manual", "Help"}, // HelpManual
        {"help.keyboard-shortcuts", "Help"}, // KeyboardReference
        {"view.automation-mode", "View"}, // AutomationMode
        {"edit.delete-automation", "Edit"}, // DeleteAutomation
        {"mixer.arm-selected", "Mixer"}, // ArmSelected
        {"arrangement.track-mixer", "View"}, // ToggleTrackMixer
        {"view.switch-detail-view", "View"}, // SwitchDetailView
        {"transport.arrangement-record-alternate", "Transport"}, // ArrangementRecordAlternate
        {"options.computer-midi-keyboard", "Options"}, // ComputerMidiKeyboard
        {"create.import-midi-file", "Create"}, // ImportMidiFile
        {"file.export-midi-clip", "File"}, // ExportMidiClip
        {"help.about", "Help"}, // About
        {"create.import-audio-file", "Create"}, // ImportAudioFile
        {"browser.toggle-preview", "Browser"}, // ToggleBrowserPreview
        {"browser.preview-selected", "Browser"}, // PreviewSelected
        {"browser.hot-swap", "Browser"}, // HotSwap
        {"file.manage-project-files", "File"}, // ManageProjectFiles
        {"browser.toggle-favorite", "Browser"},
        {"browser.toggle-collection-red", "Browser"},
        {"browser.toggle-collection-orange", "Browser"},
        {"browser.toggle-collection-yellow", "Browser"},
        {"browser.toggle-collection-green", "Browser"},
        {"browser.toggle-collection-blue", "Browser"},
        {"browser.toggle-collection-purple", "Browser"},
        {"browser.toggle-collection-gray", "Browser"},
        {"arrangement.reverse-audio", "Arrangement"}, // ReverseAudio
        {"options.lock-envelopes", "Options"}, // LockEnvelopes
        {"file.open-backup", "File"}, // OpenBackup
        {"edit.consolidate", "Edit"}, // ConsolidateMidi
    });
    static_assert(table.size() == AppCommands::LastId - AppCommands::PlayStop + 1);
    const auto at = id - AppCommands::PlayStop;
    return at >= 0 && at < static_cast<int>(table.size()) ? &table[static_cast<std::size_t>(at)] : nullptr;
}
struct NamedKey {
    int code;
    const char *name;
};
const NamedKey kNamedKeys[] = {
    {juce::KeyPress::spaceKey, "Space"},       {juce::KeyPress::escapeKey, "Esc"},
    {juce::KeyPress::returnKey, "Return"},     {juce::KeyPress::tabKey, "Tab"},
    {juce::KeyPress::deleteKey, "Delete"},     {juce::KeyPress::backspaceKey, "Backspace"},
    {juce::KeyPress::insertKey, "Insert"},     {juce::KeyPress::upKey, "Up"},
    {juce::KeyPress::downKey, "Down"},         {juce::KeyPress::leftKey, "Left"},
    {juce::KeyPress::rightKey, "Right"},       {juce::KeyPress::pageUpKey, "PageUp"},
    {juce::KeyPress::pageDownKey, "PageDown"}, {juce::KeyPress::homeKey, "Home"},
    {juce::KeyPress::endKey, "End"},           {juce::KeyPress::F1Key, "F1"},
    {juce::KeyPress::F2Key, "F2"},             {juce::KeyPress::F3Key, "F3"},
    {juce::KeyPress::F4Key, "F4"},             {juce::KeyPress::F5Key, "F5"},
    {juce::KeyPress::F6Key, "F6"},             {juce::KeyPress::F7Key, "F7"},
    {juce::KeyPress::F8Key, "F8"},             {juce::KeyPress::F9Key, "F9"},
    {juce::KeyPress::F10Key, "F10"},           {juce::KeyPress::F11Key, "F11"},
    {juce::KeyPress::F12Key, "F12"},           {juce::KeyPress::numberPadAdd, "Num+"},
    {juce::KeyPress::numberPadSubtract, "Num-"}};
} // namespace
const char *AppCommands::stableId(int id) noexcept {
    const auto *e = stableEntry(id);
    return e != nullptr ? e->id : nullptr;
}
const char *AppCommands::category(int id) noexcept {
    const auto *e = stableEntry(id);
    return e != nullptr ? e->category : "ADI";
}
int AppCommands::commandFor(const std::string &stable) noexcept {
    for (int id = PlayStop; id <= LastId; ++id)
        if (const auto *e = stableEntry(id); e != nullptr && stable == e->id)
            return id;
    return 0;
}
std::string AppCommands::chordText(const juce::KeyPress &k) {
    std::string out;
    const auto mods = k.getModifiers();
    if (mods.isCommandDown())
        out += "Cmd+";
#if JUCE_MAC
    if ((mods.getRawFlags() & juce::ModifierKeys::ctrlModifier) != 0)
        out += "Ctrl+";
#endif
    if (mods.isAltDown())
        out += "Alt+";
    if (mods.isShiftDown())
        out += "Shift+";
    const int code = k.getKeyCode();
    for (const auto &n : kNamedKeys)
        if (n.code == code)
            return out + n.name;
    if (code > 32 && code < 127)
        return out + static_cast<char>(code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code);
    return out + "#" + std::to_string(code);
}
std::optional<juce::KeyPress> AppCommands::parseChord(const std::string &text) {
    int mods = 0;
    std::string rest = text;
    const std::pair<const char *, int> prefixes[] = {{"Cmd+", juce::ModifierKeys::commandModifier},
                                                     {"Ctrl+", juce::ModifierKeys::ctrlModifier},
                                                     {"Alt+", juce::ModifierKeys::altModifier},
                                                     {"Shift+", juce::ModifierKeys::shiftModifier}};
    for (bool more = true; more;) {
        more = false;
        for (const auto &[prefix, flag] : prefixes) {
            const std::string p(prefix);
            if (rest.size() > p.size() && rest.compare(0, p.size(), p) == 0) {
                mods |= flag;
                rest = rest.substr(p.size());
                more = true;
            }
        }
    }
    for (const auto &n : kNamedKeys)
        if (rest == n.name)
            return juce::KeyPress(n.code, juce::ModifierKeys(mods), 0);
    if (rest.size() == 1 && rest[0] > 32 && rest[0] < 127) {
        const char ch = rest[0] >= 'A' && rest[0] <= 'Z' ? static_cast<char>(rest[0] - 'A' + 'a') : rest[0];
        return juce::KeyPress(ch, juce::ModifierKeys(mods), 0);
    }
    if (rest.size() > 1 && rest[0] == '#')
        return juce::KeyPress(std::atoi(rest.c_str() + 1), juce::ModifierKeys(mods), 0);
    return std::nullopt;
}
bool AppCommands::setInfoView(bool on, std::string &why) {
    if (!settings_.set("lookfeel.showInfoView", on, {}, why) || !settings_.save(why))
        return false;
    infoView_ = on;
    return true;
}
bool AppCommands::setBrowserPreviewEnabled(bool on, std::string &why) {
    if (!settings_.set(settings::kBrowserPreviewEnabledKey, on, {}, why) || !settings_.save(why))
        return false;
    browserPreviewEnabled_ = on;
    return true;
}
std::optional<float> AppCommands::keptZoom(const std::string &window, const std::string &display) const {
    const auto kept = settings::windowZoom(settings_, window, display);
    if (!kept)
        return std::nullopt;
    return static_cast<float>(*kept);
}
bool AppCommands::keepZoom(const std::string &window, const std::string &display, float zoom, std::string &why) {
    return settings::setWindowZoom(settings_, window, display, zoom, {}, why) && settings_.save(why);
}
bool AppCommands::setTabMovesFocus(bool on, std::string &why) {
    if (!settings_.set("lookfeel.tabMovesFocus", on, {}, why) || !settings_.save(why))
        return false;
    tabMovesFocus_ = on;
    return true;
}
bool AppCommands::setDelayCompensation(bool on, std::string &why) {
    if (!settings_.set("audio.delayCompensation", on, {}, why))
        return false;
    delayCompensation_ = on; // in force from here, as every Settings change is
    return settings_.save(why);
}
bool AppCommands::setEnvelopesLocked(bool locked, std::string &why) {
    if (!settings_.set("editing.automationFollowsClips", !locked, {}, why))
        return false;
    envelopesLocked_ = locked;
    const bool saved = settings_.save(why);
    manager.commandStatusChanged();
    if (onEnvelopeLockChanged)
        onEnvelopeLockChanged();
    return saved;
}
bool AppCommands::setWrapTabNavigation(bool on, std::string &why) {
    if (!settings_.set("lookfeel.wrapTabNavigation", on, {}, why) || !settings_.save(why))
        return false;
    wrapTab_ = on;
    return true;
}
namespace {
// The Note Editor's entry for a command of the window's menus, 0 when it has none. While the
// editor has the keyboard these commands act in it, as their keys do there: Delete on its notes,
// Duplicate on its notes or, with the loop brace selected, on the loop (manual 8.9), Split as its
// Chop or Split, the Time commands inside the clip (10.7.2).
int noteEditorItem(int id, const NoteEditor &notes) {
    using C = AppCommands;
    switch (id) {
    case C::Cut:
        return NoteEditor::kCut;
    case C::Copy:
        return NoteEditor::kCopy;
    case C::Paste:
        return NoteEditor::kPaste;
    case C::Duplicate:
        return notes.rulerSelection() == NoteEditor::RulerPart::Loop ? NoteEditor::kDuplicateLoop : NoteEditor::kDuplicate;
    case C::DeleteTrack:
        return NoteEditor::kDelete;
    case C::SelectAll:
        return NoteEditor::kSelectAll;
    case C::InvertSelection:
        return NoteEditor::kInvertSelection;
    case C::Quantize:
        return NoteEditor::kQuantize;
    case C::QuantizeSettings:
        return NoteEditor::kQuantizeSettings;
    case C::Split:
        return NoteEditor::kSplit;
    case C::CropClip:
        return NoteEditor::kCrop;
    case C::FitToTimeRange:
        return NoteEditor::kFitToRange;
    case C::DuplicateTime:
        return NoteEditor::kDuplicateTime;
    case C::DeleteTime:
        return NoteEditor::kDeleteTime;
    case C::InsertSilence:
        return NoteEditor::kInsertTime;
    default:
        return 0;
    }
}
} // namespace
void AppCommands::getAllCommands(juce::Array<juce::CommandID> &ids) {
    for (int id = PlayStop; id <= LastId; ++id)
        ids.add(id);
}
void AppCommands::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo &info) {
    if (id < PlayStop || id > LastId) {
        info.setActive(false);
        return;
    }
    static constexpr const char *names[] = {"Play / stop",         "Stop",
                                  "Undo",                "Redo",
                                  "Flip Sides",          "Hide/Show Browser",
                                  "Arrangement Track Controls", "Hide/Show Device View",
                                  "Hide/Show Info View", "Zoom In Window",
                                  "Zoom Out Window",     "New Project",
                                  "Open Project...",     "Take Snapshot...",
                                  "Save Project As...",  "Settings...",
                                  "Zoom In",             "Zoom Out",
                                  "Zoom to Arrangement Time Selection",
                                  "Zoom Back",           "Optimize Arrangement Width",
                                  "Optimize Arrangement Height",
                                  "Move Insert Marker to Beginning",
                                  "Continue Play from Stop Point",
                                  "Insert Audio Track",  "Insert MIDI Track",
                                  "Delete Track",        "Rename",
                                  "Mixer",               "Channel EQ",
                                  "Quit",                "Control Bar",
                                  "Arrangement View",    "Device View",
                                  "Browser",             "Search in Browser",
                                  "Use Tab Key to Move Focus", "Follow",
                                  "Move Insert Marker to Playhead Position",
                                  "Increase Height of Selected Tracks",
                                  "Decrease Height of Selected Tracks",
                                  "Move Focus to Mixer", "Next Neighbor",
                                  "Previous Neighbor", "Full Screen",
                                  "Overview", "In/Out", "Narrow Grid", "Widen Grid",
                                  "Triplet Grid", "Snap to Grid", "Fixed Grid",
                                  "Wrap Tab Navigation", "Use Large Track Height",
                                  "Group Tracks", "Ungroup Tracks", "Mix View", "Mix View in a Window",
                                  "Reference Track", "Play Reference", "Select All", "Insert Return Track",
                                  "Return Tracks", "Sends", "Mixer In/Out", "Solo Selected Tracks",
                                  "Deactivate Selected Track", "Activate/Deactivate Track 1",
                                  "Activate/Deactivate Track 2", "Activate/Deactivate Track 3",
                                  "Activate/Deactivate Track 4", "Activate/Deactivate Track 5",
                                  "Activate/Deactivate Track 6", "Activate/Deactivate Track 7",
                                  "Activate/Deactivate Track 8", "Mixer Sends",
                                  "Insert Time Signature Change", "Solo in Place",
                                  "Track Options", "Mixer Track Options", "Metronome", "Loop Selection",
                                  "Select Loop", "Add Locator", "Delete Locator", "Edit Info Text",
                                  "Undo History", "Save a Copy...", "Collect All and Save",
                                  "Collect and Export...", "Cut", "Copy", "Paste", "Duplicate",
                                  "Show Plug-In Windows", "Split", "Clip View",
                                  "Insert Empty MIDI Clip(s)", "Cut Time", "Copy Time", "Paste Time",
                                  "Duplicate Time", "Delete Time", "Insert Silence", "Create Fade In/Out",
                                  "Delete Fades/Crossfades", "Invert Selection", "Quantize", "Quantize Settings...",
                                  "Crop Clip", "Expand Clip View", "Fold to Notes", "Fold to Scale", "Highlight Scales",
                                  "Draw Mode", "Arrange Clip View Panels Automatically", "Arrange Clip View Panels Horizontally",
                                  "Arrange Clip View Panels Vertically", "Clip View", "Fit to Time Range",
                                  "Arrangement Record", "Export Audio/Video...", "Delay Compensation", "Solo Switches", "Cue Switches",
                                  "Live 12 Manual", "Keyboard Shortcuts", "Automation Mode", "Delete Automation",
                                  "Arm Selected Tracks", "Volume, Pan and Meters", "Switch Clip and Device Views",
                                  "Arrangement Record (Alternate Start)", "Computer MIDI Keyboard",
                                  "Import MIDI File...", "Export MIDI Clip...", "About ADI", "Import Audio File...",
                                  "Browser Preview", "Preview Selected", "Hot-Swap", "Manage Project Files...",
                                  "Toggle Favorite", "Toggle Red Collection",
                                  "Toggle Orange Collection", "Toggle Yellow Collection", "Toggle Green Collection",
                                  "Toggle Blue Collection", "Toggle Purple Collection", "Toggle Gray Collection",
                                  "Reverse Clip(s)", "Lock Envelopes", "Open Backup...", "Consolidate"};
    static_assert(std::size(names) == LastId - PlayStop + 1);
    info.setInfo(names[id - PlayStop], names[id - PlayStop], category(id), 0);
    if (id >= ToggleFavorite && id <= ToggleCollectionGray) {
        if (id != ToggleFavorite)
            info.addDefaultKeypress('1' + id - ToggleCollectionRed, 0);
        info.setActive(active_ && active_->browser.canMarkSelection());
        return;
    }
    if (id == TakeSnapshot && active_ && active_->needsFirstSave)
        info.setInfo("Save Project...", "Name this project", category(id), 0);
    if (id == Undo || id == Redo) {
        const bool redo = id == Redo;
        info.setInfo(active_ ? juce::String(active_->undoTitle(redo))
                             : juce::String(names[id - PlayStop]),
                     names[id - PlayStop], category(id), 0);
        info.setActive(active_ != nullptr && active_->canUndo(redo));
    }
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto alt = juce::ModifierKeys::altModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    // Live's Activate/Deactivate Track 1…8 (§42.19). The commands stay, for a user's own keys, but
    // with no default (WIN-01): the function keys do functions (ADR-0209 d9). F4 is the Mix View's;
    // F3 and F7 stay free, held for the Session View and offline processing.
    if (id >= ActivateTrack1 && id <= ActivateTrack8) {
        info.setActive(active_ != nullptr);
        return;
    }
    // Live 12 manual §41.1, §41.5, §41.9 and §41.20. Esc stops nothing: in Live
    // it closes a window or a dialog, and the window lane builds Live's keys.
    switch (id) {
    case PlayStop:
        info.addDefaultKeypress(juce::KeyPress::spaceKey, 0);
        break;
    case ConsolidateMidi:
        info.addDefaultKeypress('j', cmd);
        info.setActive(active_ != nullptr && active_->canConsolidateMidi());
        break;
    case Undo:
        info.addDefaultKeypress('z', cmd);
        break;
    case Redo:
#if JUCE_MAC
        info.addDefaultKeypress('z', cmd | shift);
#else
        if (live_)
            info.addDefaultKeypress('y', cmd);
        else
            info.addDefaultKeypress('z', cmd | shift);
#endif
        break;
    case ToggleBrowser:
        info.addDefaultKeypress('b', cmd | alt);
        info.addDefaultKeypress('5', cmd | alt);
        break;
    case ToggleMixer:
        // The side panel of track controls; Ctrl/Cmd+Alt+M is the Mixer View's (§41.1).
        break;
    case ToggleMixerView:
        // Live 12: the View menu's Mixer and the toggle in the window's bottom-right
        // corner open the Mixer View in the Arrangement (manual 6.1, p. 162; §41.1).
        info.addDefaultKeypress('m', cmd | alt);
        info.setTicked(active_ != nullptr && active_->state().mixerView);
        break;
    case ToggleClipView:
        // Live 12: the View menu's Clip View and the Clip View Selector at the window's foot
        // (manual 8; §41.1).
        info.addDefaultKeypress('3', cmd | alt);
        info.setTicked(active_ != nullptr && active_->state().clipView);
        break;
    case ToggleDeviceView:
        info.addDefaultKeypress('4', cmd | alt);
        break;
    case SwitchDetailView:
        info.addDefaultKeypress(juce::KeyPress::tabKey, shift);
        info.addDefaultKeypress(juce::KeyPress::F12Key, 0);
        info.setActive(active_ != nullptr && active_->arrangement.isVisible());
        break;
    case ToggleInfoView:
        // Shift+? : one platform reports the shifted character, the other the key.
        info.addDefaultKeypress('?', shift);
        info.addDefaultKeypress('/', shift);
        break;
    case ZoomIn:
        info.addDefaultKeypress('+', cmd);
        info.addDefaultKeypress('+', cmd | shift);
        info.addDefaultKeypress('=', cmd);
        break;
    case ZoomOut:
        info.addDefaultKeypress('-', cmd);
        break;
    // Ctrl/Cmd+S names an unnamed project once, then takes named snapshots
    // (ADR-0258 d3 and ADR-0203 d6, d12).
    case NewProject:
        info.addDefaultKeypress('n', cmd);
        break;
    case OpenProject:
        info.addDefaultKeypress('o', cmd);
        break;
    case TakeSnapshot:
        info.addDefaultKeypress('s', cmd);
        break;
    case SaveAs:
        info.addDefaultKeypress('s', cmd | shift);
        break;
    case Settings:
        info.addDefaultKeypress(',', cmd);
        break;
    // The arrangement: §6.2, §41.9, §41.16, §41.19, §41.20; ADR-0129's gate.
    case TimeZoomIn:
        info.addDefaultKeypress('+', 0);
        info.addDefaultKeypress('+', shift);
        info.addDefaultKeypress('=', 0);
        break;
    case TimeZoomOut:
        info.addDefaultKeypress('-', 0);
        break;
    case ZoomToSelection:
        info.addDefaultKeypress('z', 0);
        break;
    case ZoomBack:
        info.addDefaultKeypress('x', 0);
        break;
    case OptimizeWidth:
        info.addDefaultKeypress('w', 0);
        break;
    case OptimizeHeight:
        info.addDefaultKeypress('h', 0);
        break;
    case GoStart:
        info.addDefaultKeypress(juce::KeyPress::homeKey, 0);
        break;
    case ContinuePlay:
        info.addDefaultKeypress(juce::KeyPress::spaceKey, shift);
        break;
    case InsertAudioTrack:
        info.addDefaultKeypress('t', cmd);
        break;
    case InsertMidiTrack:
        info.addDefaultKeypress('t', cmd | shift);
        break;
    case InsertMidiClip:
        // Live 12's Create menu: Insert Empty MIDI Clip(s), on a time selection (§41.1).
        info.addDefaultKeypress('m', cmd | shift);
        break;
    case ImportAudioFile:
        info.setActive(active_ != nullptr && active_->chooseAudioFile);
        break;
    case DeleteTrack:
        info.addDefaultKeypress(juce::KeyPress::deleteKey, 0);
        info.addDefaultKeypress(juce::KeyPress::backspaceKey, 0);
        if (active_ != nullptr && active_->devices.focusedDevice()) {
            info.shortName = "Delete";
            info.setActive(true);
            break;
        }
        if (active_ != nullptr && active_->arrangement.canvas.hasAutomationSelection()) {
            const auto &selection = active_->arrangement.geometry.selection;
            info.shortName = "Delete Automation Range";
            info.setActive(selection.start != selection.end);
            break;
        }
        // On a selected locator, Delete removes it (Live 12 manual 6.4).
        if (active_ != nullptr && active_->arrangement.selectedLocator != 0) {
            info.shortName = "Delete Locator";
            info.setActive(true);
            break;
        }
        // On a selected time signature marker, Delete removes it (Live 12 manual 6.5).
        if (active_ != nullptr && active_->arrangement.selectedMeter >= 0) {
            info.shortName = "Delete Time Signature Change";
            info.setActive(true);
            break;
        }
        if (active_ != nullptr) {
            const auto &selection = active_->arrangement.geometry.selection;
            const auto &canvas = active_->arrangement.canvas;
            const bool detail = canvas.hasKeyboardFocus(true) && active_->automationMode() &&
                                canvas.chosenAutomationPoint().has_value();
            const bool material = detail || selection.clip != 0 || selection.start != selection.end;
            if (material)
                info.shortName = "Delete";
            info.setActive(material ||
                           (selection.track != 0 && !active_->arrangement.canvas.hasKeyboardFocus(true)));
        } else {
            info.setActive(false);
        }
        break;
    case Quit:
        // §41.3: Ctrl+Q. On the Mac Cmd+Q is the application menu's own Quit, which
        // reaches the same place (systemRequestedQuit).
#if !JUCE_MAC
        info.addDefaultKeypress('q', cmd);
#endif
        break;
    case RenameTrack: // §41.5; on a selected time signature marker, Edit Time Signature (manual 6.5)
        info.addDefaultKeypress('r', cmd);
        if (active_ != nullptr && active_->arrangement.selectedLocator != 0) {
            info.shortName = "Rename Locator";
            info.setActive(true);
            break;
        }
        if (active_ != nullptr && active_->arrangement.selectedMeter >= 0) {
            info.shortName = "Edit Time Signature";
            info.setActive(true);
            break;
        }
        info.setActive(active_ != nullptr && (active_->arrangement.geometry.selection.track != 0 ||
                                              active_->arrangement.geometry.selection.clip != 0));
        break;
    case InsertTimeSignatureChange: // manual 6.5: at the insert marker
        info.setActive(active_ != nullptr);
        break;
    case ToggleMixView:
        // ADR-0209 d9: F4 opens and closes the Mix View; the function keys do functions.
        info.addDefaultKeypress(juce::KeyPress::F4Key, 0);
        info.setTicked(active_ != nullptr && active_->state().mixView);
        break;
    case DetachMixView: // ADR-0209 d11: docked in the centre, or in a window onto any display
        info.setTicked(active_ != nullptr && active_->state().mixViewDetached);
        break;
    case ToggleReferenceTrack: { // ADR-0209 §4, d11: the reference track, hidden by default
        const bool assigned = active_ != nullptr && active_->referenceTrack() != 0;
        info.setActive(assigned);
        info.setTicked(assigned && active_->state().showReference);
        if (!assigned)
            info.description = AdiRootComponent::kNoReferenceTrack;
        break;
    }
    case PlayReference: // ADR-0209 d9: [REF] and Shift+R; Ctrl+R stays Rename
        info.addDefaultKeypress('r', shift);
        info.setActive(false);
        info.description = active_ != nullptr && active_->referenceTrack() != 0
            ? AdiRootComponent::kReferencePlaybackUnavailable : AdiRootComponent::kNoReferenceTrack;
        break;
    case InsertReturnTrack: // §18.4, §42.19: Ctrl/Cmd+Alt+T
        info.addDefaultKeypress('t', cmd | juce::ModifierKeys::altModifier);
        break;
    case ToggleReturnTracks: // §18.4, §42.1: Hide/Show Return Tracks, Ctrl/Cmd+Alt+R
        info.addDefaultKeypress('r', cmd | juce::ModifierKeys::altModifier);
        info.setTicked(active_ != nullptr && !active_->state().hideReturns);
        break;
    case ToggleSends: // §42.1: Hide/Show Sends, Ctrl/Cmd+Alt+S
        info.addDefaultKeypress('s', cmd | juce::ModifierKeys::altModifier);
        info.setTicked(active_ != nullptr && active_->state().sends);
        break;
    case ToggleTrackMixer:
        info.setTicked(active_ != nullptr && active_->state().trackMixer);
        break;
    case ToggleMixerInOut: // Live 12's Mixer Controls In/Out: the Mixer View's own; no key of its own
        info.setTicked(active_ != nullptr && active_->state().mixerInOut);
        break;
    case ToggleMixerSends: // Live 12's Mixer Controls Sends: the Mixer View's own; Ctrl/Cmd+Alt+S is the track controls'
        info.setTicked(active_ != nullptr && active_->state().mixerSends);
        break;
    case ToggleTrackOptions: // Live's Track Options (manual 18.7): the track controls' Track Delay; no key
        info.setTicked(active_ != nullptr && active_->state().trackOptions);
        break;
    case ToggleMixerTrackOptions: // Live 12's Mixer Controls Track Options: the Mixer View's own Track Delay
        info.setTicked(active_ != nullptr && active_->state().mixerTrackOptions);
        break;
    // Live's Edit menu (manual 41.5): Cut, Copy, Paste and Duplicate on Ctrl/Cmd+X, C, V and D.
    // They act on the device with the focus, Paste also at the chain's chosen end (manual 23.2.1);
    // else on the Arrangement's chosen clip or time selection (manual 6.9; E2).
    case Cut:
    case Copy:
    case Paste:
    case Duplicate: {
        info.addDefaultKeypress(id == Cut ? 'x' : id == Copy ? 'c' : id == Paste ? 'v' : 'd', cmd);
        // The Mix View's STACK takes them while a device there has the focus (it does them itself);
        // else the device chain, else the Arrangement.
        using Clip = DeviceChainStrip::Clip;
        const auto clip = id == Cut ? Clip::Cut : id == Copy ? Clip::Copy : id == Paste ? Clip::Paste : Clip::Duplicate;
        const bool stack = active_ && active_->mixViewPanel.stack.hasDeviceContext();
        const bool copied = active_ && active_->devices.holdsCopy && active_->devices.holdsCopy();
        info.setActive(active_ && (stack ? (clip != Clip::Paste || copied) : active_->canClipboard(static_cast<int>(id))));
        break;
    }
    case CreateFade: // manual 6.8: Create > Create Fade In/Out or Create Crossfade, Ctrl/Cmd+Alt+F
        info.addDefaultKeypress('f', cmd | alt);
        if (active_ != nullptr && active_->selectionCrossfades())
            info.shortName = "Create Crossfade";
        info.setActive(active_ != nullptr &&
                       active_->arrangement.geometry.selection.start != active_->arrangement.geometry.selection.end);
        break;
    case DeleteFades: // manual 6.8: Delete Fades/Crossfades, Ctrl/Cmd+Alt+Backspace
        info.addDefaultKeypress(juce::KeyPress::backspaceKey, cmd | alt);
        info.addDefaultKeypress(juce::KeyPress::deleteKey, cmd | alt);
        info.setActive(active_ != nullptr);
        break;
    // Live's …Time commands (manual 6.11, 41.5): Ctrl/Cmd+Shift with X, C, V, D and Delete; Insert
    // Silence on Ctrl/Cmd+I.
    case CutTime:
    case CopyTime:
    case PasteTime:
    case DuplicateTime:
        info.addDefaultKeypress(id == CutTime ? 'x' : id == CopyTime ? 'c' : id == PasteTime ? 'v' : 'd', cmd | shift);
        info.setActive(active_ != nullptr && active_->canTimeCommand(static_cast<int>(id)));
        break;
    case DeleteTime:
        info.addDefaultKeypress(juce::KeyPress::deleteKey, cmd | shift);
        info.addDefaultKeypress(juce::KeyPress::backspaceKey, cmd | shift);
        info.setActive(active_ != nullptr && active_->canTimeCommand(static_cast<int>(id)));
        break;
    case InsertSilence:
        info.addDefaultKeypress('i', cmd);
        info.setActive(active_ != nullptr && active_->canTimeCommand(static_cast<int>(id)));
        break;
    case Split: // manual 6.12, 41.5: Edit > Split, Ctrl/Cmd+E
        info.addDefaultKeypress('e', cmd);
        info.setActive(active_ != nullptr);
        break;
    case ReverseAudio:
        info.addDefaultKeypress('r', 0);
        info.setActive(active_ != nullptr && !active_->noteEditorHasContext() && active_->canReverseAudio());
        break;
    case TogglePluginWindows: // manual 23.3, 41.4: View > Show/Hide Plug-In Windows, Ctrl/Cmd+Alt+P
        info.addDefaultKeypress('p', cmd | alt);
        info.shortName = active_ != nullptr && active_->pluginWindowsShown() ? "Hide Plug-In Windows"
                                                                            : "Show Plug-In Windows";
        info.setActive(active_ != nullptr);
        break;
    case UndoHistory: // manual 5.4.2: View > Undo History, Ctrl/Cmd+Alt+Z
        info.addDefaultKeypress('z', cmd | alt);
        info.setActive(active_ != nullptr);
        break;
    case Metronome: // §42.20: Toggle Metronome, O; lit from the snapshot (#383)
        info.addDefaultKeypress('o', 0);
        info.setTicked(active_ != nullptr && active_->reader().transport().metronome);
        info.setActive(active_ != nullptr);
        break;
    case LoopSelection: // manual 6.6, §42.16: Ctrl/Cmd+L, the Edit menu's
        info.addDefaultKeypress('l', cmd);
        info.setActive(active_ != nullptr);
        break;
    case AddLocator: // manual 6.4: the Create menu's and the scrub area's
        info.setActive(active_ != nullptr);
        break;
    case DeleteLocator: // manual 6.4: the Create menu's, on a selected locator
    case EditInfoText:  // manual 6.4: the Edit menu's, on a selected locator
        info.setActive(active_ != nullptr && active_->arrangement.selectedLocator != 0);
        break;
    case SelectLoop: // §42.8: Select Material in Loop, Ctrl/Cmd+Shift+L; a click on the brace too
        info.addDefaultKeypress('l', cmd | shift);
        info.setActive(active_ != nullptr);
        break;
    case SoloSelected: // §42.19: Solo Selected Tracks, S
        info.addDefaultKeypress('s', 0);
        info.setActive(active_ != nullptr);
        break;
    case ArmSelected:
        info.addDefaultKeypress('c', 0);
        info.setActive(active_ != nullptr);
        break;
    case SoloInPlace: // manual 18.6: the Options menu's entry, ticked from the project's field (#412)
        info.setTicked(active_ != nullptr && active_->reader().soloInPlace());
        info.setActive(active_ != nullptr);
        break;
    case DeactivateSelected: // §42.19: Deactivate Selected Track, 0 (the Arrangement takes 0 for clips first)
        info.addDefaultKeypress('0', 0);
        info.setActive(active_ != nullptr);
        break;
    case AutomationMode: // manual 25.2, 41.7: A, the View menu's; the names bar's switch too
        info.addDefaultKeypress('a', 0);
        info.setTicked(active_ != nullptr && active_->automationMode());
        info.setActive(active_ != nullptr);
        break;
    case LockEnvelopes:
        info.setTicked(envelopesLocked());
        info.setActive(active_ != nullptr);
        break;
    case DeleteAutomation:
        info.addDefaultKeypress(juce::KeyPress::backspaceKey, cmd);
        info.setActive(active_ != nullptr && active_->canDeleteShownAutomation());
        break;
    case ComputerMidiKeyboard:
        info.setInfo("Computer MIDI Keyboard", "Play MIDI notes from the computer keyboard.", "Options", 0);
        info.addDefaultKeypress('m', 0);
        info.setTicked(active_ != nullptr && active_->computerMidiEnabled());
        info.setActive(active_ != nullptr);
        break;
    case SelectAll: // §42.5: Ctrl/Cmd+A; a text field with the focus selects its own text first
        info.addDefaultKeypress('a', cmd);
        info.setActive(active_ != nullptr);
        break;
    case GroupTracks: // §41.5; manual 18.3
    case UngroupTracks: {
        info.addDefaultKeypress('g', id == GroupTracks ? cmd : cmd | shift);
        const auto snapshot = active_ != nullptr ? active_->current() : nullptr;
        const auto track = active_ != nullptr ? active_->arrangement.geometry.selection.track : 0;
        info.setActive(snapshot != nullptr && active_->trackGroupingContext() &&
                       (id == GroupTracks ? canGroup(snapshot->tracks, track) : canUngroup(snapshot->tracks, track)));
        break;
    }
    // Live 12's Navigate menu (online manual §41.4.1, §42.2): Alt, or Option, and the view's
    // number moves the keyboard focus there.
    case FocusControlBar:
        info.addDefaultKeypress('0', alt);
        break;
    case FocusArrangement:
        info.addDefaultKeypress('2', alt);
        break;
    case FocusClipView: // §41.2: the Clip View's editor
        info.addDefaultKeypress('3', alt);
        break;
    case FocusDeviceView:
        info.addDefaultKeypress('4', alt);
        break;
    case FocusBrowser:
        info.addDefaultKeypress('5', alt);
        break;
    case SearchBrowser: // §41.4.1.6, §42.22
        info.addDefaultKeypress('f', cmd);
        break;
    case ToggleBrowserPreview:
        info.setTicked(browserPreviewEnabled_);
        break;
    case PreviewSelected:
        info.addDefaultKeypress(juce::KeyPress::returnKey, shift);
        info.addDefaultKeypress(juce::KeyPress::rightKey, 0);
        info.setActive(active_ && active_->browser.selectedSample() && active_->playSamplePreview);
        break;
    case HotSwap:
        info.addDefaultKeypress('q', 0);
        info.setActive(active_ && active_->devices.hasHotSwapContext());
        break;
    case TabMovesFocus: // §41.1.1: the Navigate menu's switch, also in the Settings
        info.setTicked(tabMovesFocus_);
        break;
    case WrapTabNavigation: // §41.1.1: the Navigate menu's too, and Display & Input's
        info.setTicked(wrapTab_);
        break;
    case LargeTracks: // Bitwig's (Master Reference 10): the strip's button and the View menu
        info.setTicked(active_ != nullptr && active_->state().largeTracks);
        break;
    case Follow:
        // §6.2: the control bar's switch and the Options menu's command. The manual gives
        // Windows two keys, Ctrl+Shift+F (§42.11, §42.12, §42.16) and Alt+Shift+F (§42.9);
        // both are bound until win measures Live. The Mac's is Option+Shift+F throughout.
#if JUCE_MAC
        info.addDefaultKeypress('f', alt | shift);
#else
        info.addDefaultKeypress('f', cmd | shift);
        info.addDefaultKeypress('f', alt | shift);
#endif
        info.setTicked(active_ != nullptr && active_->state().follow);
        break;
    // §42.16's Arrangement View keys that need no op.
    case InsertMarkerToPlayhead:
        info.addDefaultKeypress(juce::KeyPress::spaceKey, cmd | shift);
        break;
    case GrowTracks:
        info.addDefaultKeypress('+', alt);
        info.addDefaultKeypress('+', alt | shift);
        info.addDefaultKeypress('=', alt);
        break;
    case ShrinkTracks:
        info.addDefaultKeypress('-', alt);
        break;
    case FocusMixer: // §42.16
        info.addDefaultKeypress('m', alt | shift);
        break;
    // §41.4.1.2: Ctrl+Tab and Ctrl+Shift+Tab, Option on the Mac.
    case NextNeighbor:
#if JUCE_MAC
        info.addDefaultKeypress(juce::KeyPress::tabKey, alt);
#else
        info.addDefaultKeypress(juce::KeyPress::tabKey, cmd);
#endif
        break;
    case PreviousNeighbor:
#if JUCE_MAC
        info.addDefaultKeypress(juce::KeyPress::tabKey, alt | shift);
#else
        info.addDefaultKeypress(juce::KeyPress::tabKey, cmd | shift);
#endif
        break;
    // The View menu's toggles (§42.1).
    case FullScreen:
#if JUCE_MAC
        info.addDefaultKeypress('f', juce::ModifierKeys::ctrlModifier | cmd);
#else
        info.addDefaultKeypress(juce::KeyPress::F11Key, 0);
#endif
        info.setTicked(active_ != nullptr && active_->fullScreen());
        break;
    case ToggleOverview:
        info.addDefaultKeypress('o', cmd | alt);
        info.setTicked(active_ != nullptr && active_->state().overview);
        break;
    case ToggleInOut:
        info.addDefaultKeypress('i', cmd | alt);
        info.setTicked(active_ != nullptr && active_->state().inOut);
        break;
    // The editing grid (§6.10, §42.13), the Options menu's and the lanes' menu's.
    case NarrowGrid:
        info.addDefaultKeypress('1', cmd);
        break;
    case WidenGrid:
        info.addDefaultKeypress('2', cmd);
        break;
    case TripletGrid:
        info.addDefaultKeypress('3', cmd);
        info.setTicked(active_ != nullptr && active_->arrangement.geometry.grid.triplet);
        break;
    case SnapToGrid:
        info.addDefaultKeypress('4', cmd);
        info.setTicked(active_ != nullptr && active_->arrangement.geometry.grid.snap);
        break;
    case FixedGrid:
        info.addDefaultKeypress('5', cmd);
        info.setTicked(active_ != nullptr && active_->arrangement.geometry.grid.fixed);
        break;
    // The Note Editor's own (manual 10.5.2, 10.5.8, 10.7.1): Invert Selection on Ctrl/Cmd+Shift+A,
    // Quantize on Ctrl/Cmd+U and its settings on Ctrl/Cmd+Shift+U, Crop Clip on Ctrl/Cmd+Shift+J.
    // They act while the editor has the keyboard; what each reads and whether it can act is the
    // editor's entry, below.
    case InvertSelection:
        info.addDefaultKeypress('a', cmd | shift);
        info.setActive(false);
        break;
    case Quantize:
        info.addDefaultKeypress('u', cmd);
        info.setActive(active_ && active_->state().clipView && active_->clipView.audio() &&
                       active_->clipView.sample.hasKeyboardFocus(true) && active_->clipView.sample.canQuantize());
        break;
    case QuantizeSettings:
        info.addDefaultKeypress('u', cmd | shift);
        info.setActive(false);
        break;
    case CropClip:
        info.addDefaultKeypress('j', cmd | shift);
        info.setActive(false);
        break;
    case FitToTimeRange: // manual 10.5.5: the editor's, in the Edit menu too
        info.addDefaultKeypress('j', cmd | alt);
        info.setActive(false);
        break;
    case ArrangementRecord:
    case ArrangementRecordAlternate:
        info.addDefaultKeypress(juce::KeyPress::F9Key, id == ArrangementRecordAlternate ? shift : 0);
        info.setTicked(active_ != nullptr && active_->arrangementRecordEnabled());
        break;
    case DelayCompensation: // manual 18.7: the Options menu's switch, ticked from the setting (MX-04)
        info.setTicked(delayCompensation_);
        break;
    case SoloSwitches: // manual 18.6: the Options menu's pair, one ticked, from the project's field
    case CueSwitches:
        info.setTicked(active_ != nullptr && active_->current() &&
                       active_->current()->cue.cueMode == (id == CueSwitches));
        info.setActive(active_ != nullptr && active_->current() != nullptr);
        break;
    case ImportMidiFile:
        info.setActive(active_ != nullptr && active_->current() != nullptr);
        break;
    case ExportMidiClip:
        info.addDefaultKeypress('e', cmd | shift);
        info.setActive(active_ != nullptr && active_->midiClipForExport() != 0);
        break;
    case ExportAudio: // manual 5.2.1 and §41.3: Ctrl/Cmd+Shift+R
        info.addDefaultKeypress('r', cmd | shift);
        break;
    case ExpandClipView: // manual 10.2; Live 12's View menu: Expand Clip View, Ctrl/Cmd+Alt+E
        info.addDefaultKeypress('e', cmd | alt);
        info.setTicked(active_ != nullptr && active_->clipViewFull());
        info.setActive(active_ != nullptr && active_->state().clipView);
        break;
    case ArrangePanelsAutomatically: // manual 8.1.2: the View menu's three, one of them ticked
    case ArrangePanelsHorizontally:
    case ArrangePanelsVertically:
        info.setTicked(active_ != nullptr && static_cast<int>(active_->clipView.arrangement()) == id - ArrangePanelsAutomatically);
        info.setActive(active_ != nullptr && active_->state().clipView);
        break;
    case DrawMode:
        info.addDefaultKeypress('b', 0);
        info.setActive(active_ != nullptr);
        info.setTicked(active_ != nullptr && active_->drawMode());
        break;
    // The editor's switches (manual 10.4.1, 10.6): F, G and K are the editor's own keys.
    // From a menu they are thrown whenever the editor is on screen.
    case FoldToNotes:
    case FoldToScale:
    case HighlightScales: {
        info.addDefaultKeypress(id == FoldToNotes ? 'f' : id == FoldToScale ? 'g' : 'k', 0);
        const bool shown = active_ != nullptr && active_->noteEditorShown();
        info.setActive(shown);
        if (!shown)
            break;
        const auto &notes = active_->clipView.notes;
        const auto entry = notes.contextEntry(id == FoldToNotes   ? NoteEditor::kFoldToNotes
                                              : id == FoldToScale ? NoteEditor::kFoldToScale
                                                                  : NoteEditor::kHighlightScale);
        info.setActive(entry.enabled);
        info.setTicked(entry.ticked);
        break;
    }
    default:
        break;
    }
    // While the Note Editor has the keyboard, the Edit and Options menus' commands are its own
    // (manual 10.5 and on): each reads as the editor's entry does and is greyed when the editor
    // has nothing for it to act on. The Mix View's STACK, with a device in focus, comes first.
    if (active_ != nullptr && active_->noteEditorHasContext() && !active_->mixViewPanel.stack.hasDeviceContext()) {
        const auto &notes = active_->clipView.notes;
        if (const int item = noteEditorItem(id, notes); item != 0) {
            const auto entry = notes.contextEntry(item);
            info.shortName = entry.text;
            info.setActive(entry.enabled);
            info.setTicked(entry.ticked);
        } else if (id == CutTime || id == CopyTime || id == PasteTime) {
            info.setActive(false); // the editor has Duplicate, Delete and Insert Time (manual 10.7.2)
        } else if (id == RenameTrack) {
            info.shortName = "Rename";
            info.setActive(true);
        } else if (id == TripletGrid) {
            info.setTicked(notes.time.grid.triplet);
        } else if (id == SnapToGrid) {
            info.setTicked(notes.time.grid.snap);
        } else if (id == FixedGrid) {
            info.setTicked(notes.time.grid.fixed);
        }
    }
    if (active_ && (active_->mixViewPanel.stack.hasToolbarFocus() || active_->mixViewPanel.hasAnalyzerControlFocus() ||
                    active_->mixViewPanel.hasChannelControlFocus()) && deviceOrTrackEditCommand(id))
        info.setActive(false);
}
bool AppCommands::key(const juce::KeyPress &pressed, AdiRootComponent &root, bool allowComputerMidi) {
    activate(root);
    const auto *mappings = manager.getKeyMappings();
    if (root.browser.list.hasKeyboardFocus(true)) {
        const auto direct = mappings->findCommandForKeyPress(pressed);
        if (direct >= ToggleFavorite && direct <= ToggleCollectionGray)
            return root.command(direct);
    }
    if (allowComputerMidi && root.playComputerMidiKey(pressed))
        return true;
    const auto key = allowComputerMidi ? root.computerMidiShortcut(pressed) : pressed;
    auto id = mappings->findCommandForKeyPress(key);
    const int physical = computerMidiKeyCode(pressed);
    if (allowComputerMidi && id == 0 && (physical == 'm' || physical == 'M') &&
        mappings->findCommandForKeyPress(juce::KeyPress('m', pressed.getModifiers(), 0)) == ComputerMidiKeyboard)
        id = ComputerMidiKeyboard;
    if (id == ComputerMidiKeyboard)
        return allowComputerMidi && root.pressComputerMidiToggle(physical);
    if (id == PreviewSelected)
        return root.browser.list.hasKeyboardFocus(true) && root.browser.previewKey(key);
    if (id == HotSwap && (!root.devices.hasHotSwapContext() ||
                         (root.transport.keyboard.getToggleState() && key == juce::KeyPress('q'))))
        return false;
    if (id >= ToggleFavorite && id <= ToggleCollectionGray)
        return false;
    if ((id == Undo || id == Redo) && root.recordingHoldsHistory()) {
        // JUCE skips inactive commands. A mapped Undo or Redo key can still say why it is held.
        root.command(id);
        return true;
    }
    if (id == SwitchDetailView && tabMovesFocus_ &&
        key == juce::KeyPress(juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0))
        return false;
    // F, G and K are the Note Editor's own keys: it takes them itself while it has the keyboard.
    // A key that comes here was pressed elsewhere, where they are no command's.
    if (id >= FoldToNotes && id <= HighlightScales)
        return false;
    if ((id == GroupTracks || id == UngroupTracks) && !root.trackGroupingContext())
        return false;
    if (id == DrawMode || id == ArmSelected) {
        const auto *focus = juce::Component::getCurrentlyFocusedComponent();
        if (focus && root.isParentOf(focus) && dynamic_cast<const juce::TextEditor *>(focus))
            return false;
        if (root.transport.keyboard.getToggleState() &&
            pressed == juce::KeyPress(id == DrawMode ? 'b' : 'c'))
            return false;
    }
    // A with the Note Editor's keyboard stays the editor's: Automation Mode is the Arrangement's.
    if (id == AutomationMode && root.clipView.notes.hasContext())
        return false;
    // A latching key runs its command once while it is held (Live 42.25): the root times it. One let
    // go where the window could not see it (the focus elsewhere) goes down anew, and nothing goes back.
    if (AdiRootComponent::latches(id)) {
        if (root.latch().held() && !root.input.down(root.latch().key()))
            root.letGoUnseen();
        return root.latchKey(id, key.getKeyCode(), true, root.input.ms());
    }
    return manager.getKeyMappings()->keyPressed(key, &root);
}
void AppCommands::activate(AdiRootComponent &root) {
    active_ = &root;
    // Re-read the command infos so the Edit menu names the step it would undo.
    manager.commandStatusChanged();
}
void AppCommands::detach(AdiRootComponent &root) {
    if (active_ == &root)
        active_ = nullptr;
}
bool AppCommands::perform(const InvocationInfo &info) {
    // A command the root refuses has still been handled: the Status Bar says why.
    if (active_ == nullptr)
        return false;
    active_->command(info.commandID);
    return true;
}
StatusBar::StatusBar(AdiRootComponent &root) : root_(root) {
    // Live's corners: the Info View's toggle on the left, the Detail View's on the right.
    for (juce::TextButton *b : std::initializer_list<juce::TextButton *>{&infoToggle, &detailToggle}) {
        addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        b->getProperties().set(controls::iconTooltip, true);
    }
    infoToggle.setTitle("Info View");
    infoToggle.setTooltip("Shows or hides the Info View. Shift+?.");
    infoToggle.onClick = [this] { root_.command(AppCommands::ToggleInfoView); };
    detailToggle.setTitle("Detail View");
    detailToggle.setTooltip("Shows or hides the Device View. Ctrl/Cmd+Alt+4. Alt-click shows both views.");
    addAndMakeVisible(clipToggle);
    clipToggle.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    clipToggle.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    clipToggle.setTitle("Clip View");
    clipToggle.setTooltip("Shows or hides the Clip View. Ctrl/Cmd+Alt+3. Alt-click shows both views.");
    clipToggle.setClickingTogglesState(false);
    clipToggle.addMouseListener(this, false);
    addAndMakeVisible(mixerToggle);
    mixerToggle.setTitle("Mixer View");
    mixerToggle.setTooltip("Shows or hides the Mixer. Ctrl/Cmd+Alt+M.");
    mixerToggle.setClickingTogglesState(false);
    mixerToggle.onClick = [this] { root_.command(AppCommands::ToggleMixerView); };
    addAndMakeVisible(mixerOptions);
    mixerOptions.setTitle("Mixer View options");
    mixerOptions.setTooltip("Shows or hides the Mixer's In/Out, Sends and Return Tracks.");
    mixerOptions.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    mixerOptions.onClick = [this] {
        root_.mixerOptionsMenu().showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&mixerOptions));
    };
    addChildComponent(audioSettings);
    audioSettings.setTitle("Audio Settings");
    audioSettings.onClick = [this] {
        if (root_.openAudioSettings)
            root_.openAudioSettings();
    };
    addChildComponent(playbackIssues);
    playbackIssues.setColour(juce::TextButton::textColourOffId, look::error);
    playbackIssues.setTooltip("Shows what cannot play in this project.");
    playbackIssues.onClick = [this] {
        if (playbackProblems_.empty())
            return;
        if (onPlaybackProblems)
            onPlaybackProblems(playbackProblems_);
        else
            root_.showPlaybackProblems();
    };
}
void StatusBar::setAudioDeviceProblem(juce::String problem) {
    audioDeviceProblem_ = std::move(problem);
    audioSettings.setVisible(audioDeviceProblem_.isNotEmpty());
    audioSettings.setTooltip(audioDeviceProblem_);
    resized();
    repaint();
}
bool StatusBar::setPlaybackProblems(const std::vector<std::string> &problems) {
    if (playbackProblems_ == problems)
        return false;
    auto projected = projectPlaybackProblems(problems);
    playbackProblems_ = std::move(projected.reasons);
    root_.arrangement.canvas.setPlaybackProblemIds(std::move(projected.clipIds));
    root_.devices.setPlaybackProblemIds(std::move(projected.deviceIds));
    root_.mixViewPanel.repaint();
    root_.refreshPlaybackProblemList();
    playbackIssues.setVisible(!problems.empty());
    playbackIssueLabel_ = juce::String(problems.size()) + (problems.size() == 1 ? " playback issue" : " playback issues");
    playbackIssues.setTitle(playbackIssueLabel_);
    resized();
    repaint();
    return true;
}
void StatusBar::setMissingCueOutput(std::optional<int> firstChannel) {
    if (firstChannel == missingCueOutput_)
        return;
    missingCueOutput_ = firstChannel;
    cueOutputProblem_ = firstChannel ? "Cue Out " + juce::String(*firstChannel + 1) + "/" +
                                           juce::String(*firstChannel + 2) + " unavailable. Metronome and sample preview are silent."
                                     : juce::String{};
    setTooltip(cueOutputProblem_);
    repaint();
}
juce::String StatusBar::messageText() const {
    auto text = audioDeviceProblem_;
    if (!root_.error().empty()) {
        if (text.isNotEmpty())
            text << "  ";
        text << juce::String(root_.error());
    }
    if (cueOutputProblem_.isNotEmpty()) {
        if (text.isNotEmpty())
            text << "  ";
        text << cueOutputProblem_;
    }
    return text.isNotEmpty() ? text : root_.selectionText();
}
void StatusBar::resized() {
    // Live's Status Bar at 100 %: round corner buttons, the message field, a second
    // field, the selected track's box; 22 pt fields with the frame's 8 pt around them.
    const int w = getWidth();
    infoToggle.setBounds(8, 9, 19, 19);
    detailToggle.setBounds(w - 27, 9, 19, 19);
    clipToggle.setBounds(secondField());
    mixerToggle.setBounds(w - 86, 11, 36, 16);
    mixerOptions.setBounds(w - 49, 11, 8, 16);
    auto actions = messageField().withY(11).withHeight(16).reduced(4, 0);
    const bool showAudio = audioSettings.isVisible(), showIssues = playbackIssues.isVisible();
    if (showAudio) {
        const int availableWidth = showIssues ? actions.getWidth() / 2 : actions.getWidth();
        audioSettings.setBounds(actions.removeFromRight(std::min(98, availableWidth)));
        audioSettings.setButtonText(audioSettings.getWidth() >= 85 ? "Audio Settings" : "Audio");
        actions.removeFromRight(4);
    }
    if (showIssues) {
        playbackIssues.setBounds(actions.removeFromRight(125));
        playbackIssues.setButtonText(playbackIssues.getWidth() >= 110 ? playbackIssueLabel_ : juce::String("Issues"));
    }
}
void StatusBar::DetailButton::clicked(const juce::ModifierKeys &mods) {
    // Letting go after a drag in the Clip View selector only finishes the drag.
    if (clip_ && std::exchange(bar_.selectorDragged_, false))
        return;
    if (mods.isAltDown())
        bar_.root_.setDetailViews(true, true);
    else
        bar_.root_.command(clip_ ? AppCommands::ToggleClipView : AppCommands::ToggleDeviceView);
}
void StatusBar::mouseDown(const juce::MouseEvent &e) {
    if (e.eventComponent != &clipToggle)
        return;
    selectorLast_ = e.getPosition();
    selectorDragged_ = false;
}
void StatusBar::mouseDrag(const juce::MouseEvent &e) {
    if (e.eventComponent != &clipToggle || !root_.state().clipView)
        return;
    const auto at = e.getPosition();
    if (!selectorDragged_ && at.getDistanceFrom(selectorLast_) < 3)
        return;
    selectorDragged_ = true;
    const double width = std::max(1, secondField().reduced(5, 4).getWidth());
    root_.clipView.selectorDrag(static_cast<double>(at.x - selectorLast_.x) / width, at.y - selectorLast_.y);
    selectorLast_ = at;
    repaint();
}
juce::Rectangle<float> StatusBar::clipViewFrame() const {
    const auto *clip = root_.clipView.details();
    if (clip == nullptr || !root_.state().clipView || clip->lengthTicks.value_or(0) <= 0)
        return {};
    const auto inner = secondField().reduced(5, 4).toFloat();
    const auto shown = root_.clipView.shownSpan();
    const double start = static_cast<double>(clip->posTicks.value_or(0)), length = static_cast<double>(*clip->lengthTicks);
    const double from = std::clamp((static_cast<double>(shown.first) - start) / length, 0., 1.);
    const double to = std::clamp((static_cast<double>(shown.second) - start) / length, 0., 1.);
    if (to <= from)
        return {};
    return {inner.getX() + inner.getWidth() * static_cast<float>(from), inner.getY() - 1.f,
            std::max(2.f, inner.getWidth() * static_cast<float>(to - from)), inner.getHeight() + 2.f};
}
void StatusBar::setExportProgress(std::optional<int> percent) {
    if (exportPercent_ == percent)
        return;
    exportPercent_ = percent;
    exportProgressText_ = percent ? "Exporting " + juce::String(*percent) + "%" : juce::String{};
    repaint();
}
void StatusBar::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    const int w = getWidth();
    const auto message = messageField();
    const juce::Rectangle<int> second = secondField(), track(w - 162, 8, 124, 22);
    for (auto r : {message, second, track}) {
        g.setColour(look::band);
        g.fillRect(r);
        g.setColour(look::panel);
        g.fillRect(r.reduced(3));
    }
    // The Mixer View Control's arrow, pointing down to its menu.
    // The Clip View Selector: the shown clip's notes in miniature, in its colour; lit while the
    // Clip View is open.
    controls::button(g, second.reduced(2).toFloat(),
                     controls::stateOf(clipToggle.getToggleState(), clipToggle.isOver(), clipToggle.isDown(),
                                       clipToggle.isEnabled()));
    if (const auto *clip = root_.clipView.details(); clip != nullptr && clip->lengthTicks.value_or(0) > 0) {
        const auto inner = second.reduced(5, 4).toFloat();
        const double length = static_cast<double>(*clip->lengthTicks);
        const auto range = keyRange(clip->notes);
        g.setColour(root_.clipView.colour().darker(0.45f));
        if (!range)
            g.fillRect(inner.withSizeKeepingCentre(inner.getWidth(), 2.f));
        else
            for (const auto &note : clip->notes) {
                const double from = static_cast<double>(note.startTicks - clip->startMarkerTicks) / length;
                const double to = from + static_cast<double>(note.durationTicks) / length;
                if (to <= 0 || from >= 1)
                    continue;
                const float span = static_cast<float>(std::max(1, range->high - range->low));
                const float y = inner.getBottom() - 2.f -
                                (inner.getHeight() - 2.f) * static_cast<float>(note.key - range->low) / span;
                const float x0 = inner.getX() + inner.getWidth() * static_cast<float>(std::max(0., from));
                const float x1 = inner.getX() + inner.getWidth() * static_cast<float>(std::min(1., to));
                g.fillRect(x0, y, std::max(1.f, x1 - x0), 2.f);
            }
    }
    if (const auto frame = clipViewFrame(); !frame.isEmpty()) {
        // The part of the clip the Clip View's editor shows.
        g.setColour(look::text.withAlpha(0.75f));
        g.drawRect(frame, 1.f);
    }
    {
        // As the shared chooser's arrow (language.png part D): a chevron half as tall as it is wide.
        const auto a = mixerOptions.getBounds().toFloat().withSizeKeepingCentre(7.f, 5.f);
        const float top = a.getCentreY() - a.getWidth() / 4;
        juce::Path arrow;
        arrow.startNewSubPath(a.getX(), top);
        arrow.lineTo(a.getCentreX(), top + a.getWidth() / 2);
        arrow.lineTo(a.getRight(), top);
        g.setColour(look::textDim);
        g.strokePath(arrow, juce::PathStrokeType(look::line, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    // The corner buttons: round, in the shared button's colours, with Live's triangles.
    for (juce::TextButton *b : std::initializer_list<juce::TextButton *>{&infoToggle, &detailToggle}) {
        const auto c = b->getBounds().toFloat();
        const auto state = controls::stateOf(false, b->isOver(), b->isDown(), b->isEnabled());
        controls::roundButton(g, c, state);
        look::triangle(g, c.reduced(5.f), -juce::MathConstants<float>::halfPi, controls::labelColour(state));
    }
    g.setColour(look::text);
    g.setFont(look::font(look::fontHeading));
    const auto actionWidth = (audioSettings.isVisible() ? audioSettings.getWidth() + 4 : 0) +
                             (playbackIssues.isVisible() ? playbackIssues.getWidth() + 4 : 0);
    auto textArea = message.reduced(8, 0).withTrimmedRight(actionWidth);
    if (exportProgressText_.isNotEmpty())
        g.drawText(exportProgressText_, textArea.removeFromLeft(128), juce::Justification::centredLeft);
    g.drawText(messageText(), textArea, juce::Justification::centredLeft);
    // The selected track's name, as Live shows it at the right.
    const auto rows = root_.arrangement.geometry.rows(root_.reader());
    if (const auto i = rows.find(root_.arrangement.geometry.selection.track); i < rows.size()) {
        g.setFont(look::font(look::fontBody));
        g.drawText(juce::String::fromUTF8(displayName(rows[i].track->name, rows[i].number()).c_str()),
                   track.reduced(6, 0).withWidth(44), juce::Justification::centredLeft);
    }
}
BrowserResizer::BrowserResizer(AdiRootComponent &root) : root_(root) {
    setTitle("Browser width");
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
}
void BrowserResizer::mouseDown(const juce::MouseEvent &event) {
    if (!event.mods.isLeftButtonDown() || root_.browserArea().isEmpty()) return;
    const auto &panels = root_.state().panels;
    const int direction = panels.slotOf(Panel::Browser) == 0 ? 1 : -1;
    drag_ = Drag{root_.browserArea().getWidth() + look::gap, panels.find(Panel::Browser)->width, direction};
}
void BrowserResizer::mouseDrag(const juce::MouseEvent &event) {
    if (drag_)
        root_.resizeBrowser(drag_->visibleWidth + drag_->direction * event.getDistanceFromDragStartX());
}
void BrowserResizer::mouseUp(const juce::MouseEvent &) {
    if (drag_ && root_.state().panels.find(Panel::Browser)->width != drag_->storedWidth)
        (void)root_.persist();
    drag_.reset();
}

AdiRootComponent::AdiRootComponent(engine::ProjectView &view, OpSubmitter &submitter,
                                   TransportMailbox &mailbox, AppCommands &commands,
                                   ViewStateStore &persistence, SessionState &session,
                                   std::string window, DesktopDefaults defaults)
    : transport(*this), status(*this), arrangement(*this), devices(*this), dockResizer(*this),
      mixer(*this), view_(view), submitter_(submitter), mailbox_(mailbox),
      commands_(commands), persistence_(persistence), session_(session),
      window_(std::move(window)),
      state_(persistence.load(window_, defaults)), infoShown_(commands.infoView()) {
    // Live's look for everything this window holds (ui_look.hpp).
    setLookAndFeel(&look::liveLook());
    browser.syncPreviewToggle();
    undoHistory.setLookAndFeel(&look::liveLook());
    auto& store = submitter_.store();
    const auto opening = store.openingHistory();
    undoHistory.openingHead = opening ? opening->seq : OpJournal(store).headSeq();
    if (!opening && !store.readOnly())
        store.retainOpeningHistory(undoHistory.openingHead);
    undoHistory.openedUtc = juce::Time::currentTimeMillis() * 1000;
    undoHistory.read = [this] { return submitter_.timeline(); };
    undoHistory.restore = [this](std::int64_t txn) {
        return txn == 0 ? restoreHistoryHead(std::nullopt) : restoreStep(txn);
    };
    undoHistory.restoreOpening = [this] { return restoreHistoryHead(undoHistory.openingHead); };
    if (!submitter_.store().readOnly()) {
        undoHistory.revertSnapshot = [this](std::int64_t id) {
            return restoreHistory([this, id] {
                return submitter_.revertSnapshot(id, juce::Time::currentTimeMillis() * 1000);
            }, [this, id] {
                return restoreStoredHistory && restoreStoredHistory(HistorySnapshotTarget{id});
            });
        };
        undoHistory.renameSnapshot = [this](std::int64_t id, const std::string &name) {
            const auto result = History(submitter_.store()).renameSnapshot(id, name);
            return result.ok || fail(result.error);
        };
        undoHistory.removeSnapshot = [this](std::int64_t id) {
            const auto result = History(submitter_.store()).removeSnapshot(id);
            return result.ok || fail(result.error);
        };
    }
    setOpaque(true);
    setWantsKeyboardFocus(true);
    // Tab moves within the window's own controls, and Wrap Tab Navigation wraps or stops at
    // their ends (§41.1.1), whatever else shares the native window.
    setFocusContainerType(FocusContainerType::keyboardFocusContainer);
    addAndMakeVisible(transport);
    addAndMakeVisible(status);
    addAndMakeVisible(browser);
    addChildComponent(browserResizer);
    addAndMakeVisible(arrangement);
    addAndMakeVisible(devices);
    addChildComponent(infoSpectrum);
    applyInfoAnalyserMode();
    addAndMakeVisible(dockResizer);
    addChildComponent(clipView);
    addChildComponent(clipResizer);
    addAndMakeVisible(mixer);
    addAndMakeVisible(strip);
    addAndMakeVisible(namesBar);
    strip.tracks.onClick = [this] { command(AppCommands::LargeTracks); };
    // The bar over the track names (the Arrangement sheet): Flip Sides; the track row's sections, which
    // repeat the View menu's toggles as Live's selectors do (manual 18.1); H and W; Automation Mode
    // and Lock Envelopes.
    namesBar.flip.onClick = [this] { command(AppCommands::SwapPanels); };
    namesBar.inOut.onClick = [this] { command(AppCommands::ToggleInOut); };
    namesBar.sends.onClick = [this] { command(AppCommands::ToggleSends); };
    namesBar.returns.onClick = [this] { command(AppCommands::ToggleReturnTracks); };
    namesBar.mixer.onClick = [this] { command(AppCommands::ToggleTrackMixer); };
    namesBar.delay.onClick = [this] { command(AppCommands::ToggleTrackOptions); };
    namesBar.height.onClick = [this] { command(AppCommands::OptimizeHeight); };
    namesBar.width.onClick = [this] { command(AppCommands::OptimizeWidth); };
    namesBar.automation.onClick = [this] { setAutomationMode(!automationMode_); };
    namesBar.lock.onClick = [this] { command(AppCommands::LockEnvelopes); };
    namesBar.lock.setToggleState(commands_.envelopesLocked(), juce::dontSendNotification);
    // The Overview spans the track headers too, as Live's (ui_arrangement.hpp).
    addAndMakeVisible(arrangement.overview);
    addChildComponent(mixerView);
    addChildComponent(mixerViewResizer);
    addChildComponent(mixViewPanel);
    mixViewPanel.adoptViewState();
    arrangement.geometry.left = state_.timelineLeft;
    arrangement.geometry.scale = state_.pixelsPerQuarter;
    arrangement.geometry.height = state_.laneHeight;
    arrangement.geometry.heights = state_.laneHeights;
    arrangement.geometry.foldedTracks = state_.foldedTracks;
    arrangement.geometry.largeTracks = state_.largeTracks;
    arrangement.geometry.grid = {state_.gridSnap, state_.gridFixed, state_.gridTriplet, state_.gridNarrow,
                                 state_.gridFixedQuarters};
    clipView.applyState(state_);
    reader_.emplace(view_.current());
    setSize(1200, 720);
    frame();
    juce::Desktop::getInstance().addFocusChangeListener(this);
}
AdiRootComponent::~AdiRootComponent() {
    playbackProblemsWindow_.reset();
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    releaseComputerMidiKeys();
    mixViewPanel.stack.persistWindowControls();
    commands_.detach(*this);
    setLookAndFeel(nullptr);
    undoHistory.setLookAndFeel(nullptr);
}
void AdiRootComponent::refreshPlaybackProblemList() {
    if (!playbackProblemsWindow_)
        return;
    auto *list = static_cast<juce::TextEditor *>(playbackProblemsWindow_->getContentComponent());
    juce::StringArray reasons;
    for (const auto &reason : status.playbackProblems_)
        reasons.add(juce::String(reason));
    list->setText(reasons.isEmpty() ? juce::String("No playback problems.") : reasons.joinIntoString("\n\n"), false);
}
void AdiRootComponent::showPlaybackProblems() {
    if (!playbackProblemsWindow_) {
        auto list = std::make_unique<juce::TextEditor>();
        list->setTitle("Playback problem details");
        list->setMultiLine(true, true);
        list->setReadOnly(true);
        list->setCaretVisible(false);
        list->setScrollbarsShown(true);
        list->setSize(540, 260);
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "Playback issues";
        options.content.setOwned(list.release());
        options.componentToCentreAround = getTopLevelComponent();
        options.associatedComponent = getTopLevelComponent();
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = true;
        playbackProblemsWindow_.reset(options.create());
        playbackProblemsWindow_->setLookAndFeel(&look::liveLook());
        refreshPlaybackProblemList();
    }
    playbackProblemsWindow_->setVisible(true);
    playbackProblemsWindow_->toFront(true);
}
bool AdiRootComponent::insertDevice(const std::string &uid, std::int64_t track, std::optional<std::int64_t> before) {
    const auto *info = findBuiltin(uid);
    const std::string name = info != nullptr ? std::string(info->name) : std::string("the device");
    if (!loadBuiltin)
        return fail("Devices load in the application.");
    const auto current = view_.current();
    if (track != 0 && (!current || current->findTrack(track) == nullptr))
        return fail("Select a track to load " + name + " onto.");
    const juce::Component::SafePointer<AdiRootComponent> alive(this);
    if (!loadBuiltin(uid, track, before))
        return false;
    if (alive) {
        say({});
        // The strip updates on the next frame and the browser keeps focus.
        devices.repaint();
    }
    return true;
}
std::string AdiRootComponent::pluginWindowKey(std::int64_t device) { return "plugin:" + std::to_string(device); }
bool AdiRootComponent::pluginWindowOpen(std::int64_t device) const {
    return pluginHost != nullptr && pluginHost->contains(pluginWindowKey(device));
}
bool AdiRootComponent::openPluginWindow(std::int64_t device) {
    if (!pluginWindowOpen(device)) return togglePluginWindow(device, false);
    pluginWindowsShown_ = true;
    commands_.manager.commandStatusChanged();
    followPluginWindows();
    if (auto *window = pluginHost->window(pluginWindowKey(device))) window->toFront(true);
    return true;
}
bool AdiRootComponent::togglePluginWindow(std::int64_t device, bool several) {
    if (pluginHost == nullptr || !pluginEditor)
        return fail("Plug-in windows open in the application.");
    const auto key = pluginWindowKey(device);
    if (pluginHost->contains(key)) {
        pluginHost->close(key);
        return true;
    }
    // The editor first: a plug-in that has none refuses before any other window closes.
    std::string why;
    auto editor = pluginEditor(device, why);
    if (!editor.view)
        return fail(why.empty() ? "This device has no plug-in window." : why);
    // One plug-in window at a time unless Multiple Plug-In Windows is on, or Ctrl/Cmd is held
    // (manual 23.3.1.1).
    if (!several && !commands_.multiplePluginWindows())
        for (const auto &other : pluginHost->keys("plugin:"))
            pluginHost->close(other);
    // Titled as Live titles it: the track, then the device.
    std::string title;
    if (const auto feed = devices.publication ? devices.publication() : nullptr)
        for (const auto &d : *feed)
            if (d.id == device) {
                const auto *t = reader().findTrack(d.track);
                title = (t != nullptr ? shownName(arrangement.geometry, reader(), t->id) + "/" : std::string()) + d.name; // B13
            }
    auto frameEditor = std::move(editor.frame);
    auto gone = std::move(editor.gone);
    const bool resizable = editor.resizable;
    const auto safe = juce::Component::SafePointer<AdiRootComponent>(this);
    // The window's own clock runs the editor's; an error it reports goes to the Status Bar once;
    // an editor the plug-in has dropped takes its window with it, at the next collect.
    auto onFrame = [safe, key, frameEditor, gone, said = std::make_shared<std::string>()](const SnapshotReader &) {
        if (!safe)
            return;
        if (frameEditor) {
            const auto error = frameEditor();
            if (!error.empty() && error != *said) {
                *said = error;
                safe->say(error);
            }
        }
        if (gone && gone() && safe->pluginHost != nullptr) {
            safe->say("The plug-in closed its window.");
            safe->pluginHost->requestClose(key);
        }
    };
    if (!pluginHost->openView(key, device, std::move(editor.view), std::move(onFrame), false, title, resizable))
        return fail("The plug-in window could not open.");
    pluginHost->setAlwaysOnTop(key, commands_.pluginWindowsOnTop());
    // A window opened on purpose shows, and with it the others Show/Hide had hidden.
    if (!pluginWindowsShown_) {
        pluginWindowsShown_ = true;
        commands_.manager.commandStatusChanged();
    }
    followPluginWindows();
    devices.repaint();
    return true;
}
void AdiRootComponent::pluginAdded(std::int64_t device) {
    if (!commands_.autoOpenPluginWindows() || pluginHost == nullptr || !pluginEditor)
        return;
    autoOpen_ = device;
    autoOpenFrames_ = 60;
}
void AdiRootComponent::followPluginWindows() {
    // Auto-Open (WIN-03): the added plug-in's window, once the feed knows its track and name for the
    // title. One the feed never shows within the frames given is let go.
    if (autoOpen_ != 0) {
        const auto feed = devices.publication ? devices.publication() : nullptr;
        bool known = false;
        if (feed)
            for (const auto &d : *feed)
                if (d.id == autoOpen_)
                    known = d.plugin;
        if (known || --autoOpenFrames_ <= 0) {
            const auto device = std::exchange(autoOpen_, 0);
            if (known && pluginHost != nullptr && !pluginHost->contains(pluginWindowKey(device)))
                togglePluginWindow(device, false); // one with no editor says so, as its wrench would
        }
    }
    // Auto-Hide Plug-In Windows (manual 23.3.1.1): only the selected track's show; Show/Hide
    // Plug-In Windows hides them all. Always on top follows the setting.
    if (pluginHost == nullptr)
        return;
    const auto keys = pluginHost->keys("plugin:");
    if (keys.empty())
        return;
    const auto feed = devices.publication ? devices.publication() : nullptr;
    const auto selected = arrangement.geometry.selection.track;
    for (const auto &key : keys) {
        const auto id = std::stoll(key.substr(7));
        std::int64_t track = 0;
        if (feed)
            for (const auto &d : *feed)
                if (d.id == id)
                    track = d.track;
        const bool show = pluginWindowsShown_ && (!commands_.autoHidePluginWindows() || track == selected);
        if (pluginHost->shown(key) != show)
            pluginHost->show(key, show);
        if (pluginHost->alwaysOnTop(key) != commands_.pluginWindowsOnTop())
            pluginHost->setAlwaysOnTop(key, commands_.pluginWindowsOnTop());
    }
}
void AdiRootComponent::vblankFrame() noexcept {
    vblankFrame_ = true;
    frame();
    vblankFrame_ = false;
}
void AdiRootComponent::frame() noexcept {
    browser.syncPreviewToggle();
    // Root owns exactly one reader for the presented frame; descendants borrow it.
    // JUCE may paint after this callback, so it remains alive until the next frame.
    reader_.emplace(view_.current());
    const auto outputs = physicalOutputChannels ? physicalOutputChannels() : std::nullopt;
    std::optional<int> missingCue;
    if (const auto *snapshot = reader_->get(); snapshot != nullptr && outputs &&
                                              outputs->packedPair(snapshot->cue.firstOutputChannel) < 0)
        missingCue = snapshot->cue.firstOutputChannel;
    status.setMissingCueOutput(missingCue);
    if (automationMode_)
        refreshAdditionalAutomationLanes();
    const auto generation = view_.generation();
    if (generation_ != generation) {
        generation_ = generation;
        // The project view's tree follows the project: tracks come and go, groups with them.
        arrangement.applyView();
        // A newly inserted track brought into sight above the foot, now that the rows hold it.
        if (arrangement.revealTrack != 0) {
            arrangement.geometry.reveal(reader(), arrangement.revealTrack);
            arrangement.revealTrack = 0;
        }
        // An open Undo History lists the step every edit, undo or redo made.
        if (undoHistory.isShowing())
            undoHistory.refresh();
        // A selected locator goes with its locator, however it left: Undo, Redo, a restored step, another window.
        if (arrangement.selectedLocator != 0) {
            const auto *snapshot = reader_->get();
            if (snapshot == nullptr || locatorById(locatorsOf(*snapshot), arrangement.selectedLocator) == nullptr) {
                arrangement.selectedLocator = 0;
                arrangement.repaint();
            }
        }
        dirty_.mark(DirtySet::All);
    }
    const bool playing = mailbox_.playing();
    const auto position = mailbox_.position();
    const double rate = mailbox_.sampleRate();
    if (playing != playing_ || position != position_ || rate != sampleRate_) {
        // Follow starts again whenever playback starts or stops (§6.2).
        if (playing != playing_)
            arrangement.followPaused = false;
        if (arrangement.selectedLocator != 0 && (playing != playing_ || !playing))
            arrangement.repaint(arrangement.topButton(ArrangementView::TopButton::Set).getSmallestIntegerContainer());
        sampleRate_ = rate;
        playing_ = playing;
        position_ = position;
        dirty_.mark(DirtySet::Transport);
    }
    // Selection is the session's (ADR-0021): present it again when it changed, from
    // this window or any other.
    if (selectionGeneration_ != session_.generation()) {
        selectionGeneration_ = session_.generation();
        arrangement.geometry.selection = session_.selection();
        dirty_.mark(DirtySet::Layout);
    }
    const bool envelopesLocked = commands_.envelopesLocked();
    if (namesBar.lock.getToggleState() != envelopesLocked)
        namesBar.lock.setToggleState(envelopesLocked, juce::dontSendNotification);
    // The Info View and density are app-scoped and can change in another window.
    const bool showInfo = commands_.infoView();
    if (infoShown_ != showInfo || appliedDensity_ != look::densityId()) {
        const bool openingInfo = showInfo && !infoShown_;
        infoShown_ = showInfo;
        if (openingInfo && state_.panels.find(Panel::Browser)->collapsed)
            (void)toggle(Panel::Browser);
        resized();
    }
    if (!infoArea_.isEmpty())
        hover(juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse());
    infoSpectrum.setVisible(!infoArea_.isEmpty() && infoText_.isEmpty());
    if (infoSpectrum.isVisible()) {
        const auto &analysis = mixViewPanel.analysis;
        const auto &settings = analysis.detail;
        infoSpectrum.setSource(settings.point());
        infoSpectrum.setChannel(settings.view.channel());
        infoSpectrum.setPalette(settings.view.palette(), settings.view.paletteReversed());
        infoSpectrum.setSpectrumSlope(settings.view.spectrumSlope());
        const auto &snapshot = reader();
        infoSpectrum.present(sharedAnalysis, analysis.target(snapshot, analysisTrack(snapshot), analysisTrack(snapshot, true)));
        infoSpectrum.frame();
    }
    // Live's Follow (§6.2): the arrangement keeps the playhead on screen while it plays,
    // unless an edit, a sideways scroll or a click on the ruler paused it.
    if (arrangement.isVisible() && state_.follow && playing_ && !arrangement.followPaused &&
        arrangement.follow(timelineTick(), commands_.followByPage()))
        dirty_.mark(DirtySet::Content);
    // Cross-view selection navigation has a destination. Edits and Undo can restore selections without scrolling.
    const auto selectedTrack = session_.selection().track;
    if (arrangement.isVisible()) {
        if (const auto track = std::exchange(revealInLanes_, 0); track != 0 && track == selectedTrack &&
            arrangement.geometry.reveal(reader(), track, false))
            dirty_.mark(DirtySet::Layout);
    }
    lastDrain_ = dirty_.drain();
    arrangement.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    devices.frame(arrangement.geometry.selection.track);
    // The Clip View follows the selected clip and the project, shown or not: the Status Bar's
    // selector draws the same clip.
    {
        const auto shown = clipView.clip();
        clipView.frame(reader());
        if (shown != clipView.clip() || (lastDrain_ & DirtySet::Content) != 0)
            status.repaint();
    }
    if (vblankFrame_)
        windowFollower_.triggerAsyncUpdate(); // not inside JUCE's vblank listener walk (A28)
    else
        followPluginWindows();
    mixer.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    if (mixerView.isVisible()) {
        if (const auto track = std::exchange(revealInMixer_, 0); track != 0 && track == selectedTrack &&
            arrangement.geometry.inView(track))
            mixerView.reveal(arrangement.geometry.rows(reader()).find(track));
        mixerView.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    }
    // In its own window the floating host drives its frame.
    if (mixViewPanel.isVisible() && mixViewPanel.getParentComponent() == this)
        mixViewPanel.frame(reader());
    transport.position.setCountInPosition(mailbox_.countInPosition());
    if (lastDrain_ & DirtySet::Transport) {
        transport.refresh();
        transport.repaint();
    }
    if (lastDrain_ & DirtySet::Status)
        status.repaint();
    if (lastDrain_ & (DirtySet::Layout | DirtySet::Content))
        repaint();
}
void AdiRootComponent::hover(juce::Component *c) {
    // A boxed analyzer never replaces itself with its own Info tooltip.
    if (c == &infoSpectrum || (c && infoSpectrum.isParentOf(c))) c = nullptr;
    if (c != nullptr && c != this && !isParentOf(c))
        c = nullptr;
    // The nearest element that can describe itself, as Live's Info View does.
    juce::TooltipClient *client = nullptr;
    for (auto *p = c; p != nullptr && p != this; p = p->getParentComponent())
        if ((client = dynamic_cast<juce::TooltipClient *>(p)) != nullptr) {
            c = p;
            break;
        }
    if (c == hovered_)
        return;
    hovered_ = c;
    infoText_ = client ? c->getTitle() + "\n" + client->getTooltip() : juce::String();
    dirty_.mark(DirtySet::Content);
}
std::int64_t AdiRootComponent::timelineTick() const noexcept {
    const auto *tempo = reader_->tempo();
    const double rate = sampleRate_ > 0 ? sampleRate_ : reader_->sampleRate();
    return tempo && rate > 0 ? tempo->secondsToTicks(static_cast<double>(position_) / rate) : 0;
}
bool AdiRootComponent::locate(std::int64_t tick, bool fromInsertMarker) {
    // Landing reads the current cell, not this window's presented frame.
    const auto current = view_.current();
    const double rate = mailbox_.sampleRate() > 0 ? mailbox_.sampleRate() : current->sampleRate;
    if (!current->tempo || !mailbox_.hasRoom())
        return false;
    const auto command = fromInsertMarker ? TransportMailbox::Command::LocateMarker : TransportMailbox::Command::Locate;
    const bool posted = mailbox_.post(command, static_cast<std::int64_t>(current->tempo->ticksToSeconds(tick) * rate));
    return posted;
}
bool AdiRootComponent::playFrom(std::int64_t tick) {
    const bool start = !mailbox_.desiredPlaying();
    if (!mailbox_.hasRoom(start ? 2 : 1))
        return fail("Transport command queue is full");
    if (start) {
        OpRequest request;
        request.opType = "transport.play";
        request.payload = Payload::object();
        if (const auto result = submitter_.submit(std::move(request)); !result)
            return fail(result.error);
    }
    // Scrub playback keeps its explicitly requested position.
    const bool requestedRecording = start && recordPending_;
    const bool recordingFromHere = requestedRecording && beginArrangementRecording(tick, true);
    if (requestedRecording && !recordingFromHere)
        setRecordPending(false);
    if (!locate(tick)) {
        if (recordingFromHere)
            stopRecording();
        return fail("The project has no tempo map to locate in.");
    }
    if (start)
        mailbox_.post(TransportMailbox::Command::Play);
    if (!requestedRecording || recordingFromHere)
        error_.clear();
    dirty_.mark(DirtySet::Transport | DirtySet::Status);
    return true;
}
bool AdiRootComponent::playOrStopFrom(std::int64_t tick) {
    return mailbox_.desiredPlaying() ? command(AppCommands::Stop) : playFrom(tick);
}
double AdiRootComponent::bpm() const noexcept {
    const auto *tempo = reader_->tempo();
    const double rate = sampleRate_ > 0 ? sampleRate_ : reader_->sampleRate();
    return tempo ? tempo->bpmAt(
                       tempo->secondsToTicks(rate > 0 ? static_cast<double>(position_) / rate : 0))
                 : 120.;
}
int AdiRootComponent::shownPanelWidth(const PanelGeometry &panel) const noexcept {
    const int width = state_.panels.occupied(panel);
    const bool roomForInOut = width >= MixerPanel::kInOutWidth + MixerPanel::kControlsWidth + MixerPanel::kRightStrip;
    return panel.panel == Panel::Mixer && !state_.inOut && roomForInOut ? width - MixerPanel::kInOutWidth : width;
}
void AdiRootComponent::resized() {
    appliedDensity_ = look::densityId();
    const int w = getWidth(), h = getHeight(), gap = look::gap;
    transport.setBounds(0, 0, w, look::controlBarHeight);
    status.setBounds(0, h - look::statusBarHeight, w, look::statusBarHeight);
    const int top = look::controlBarHeight, bottom = h - look::statusBarHeight;
    const int body = std::max(0, bottom - top);
    // The docked Mix View is a workspace switch. The Arrangement retains
    // its geometry and panel preferences while the workspace is hidden.
    mixViewDock_ = {0, top, w, body};
    const bool fullMix = state_.mixView && !state_.mixViewDetached;
    mixViewArea_ = fullMix ? mixViewDock_ : juce::Rectangle<int>{};
    transport.mix.setToggleState(state_.mixView, juce::dontSendNotification);
    transport.arrangementPage.setToggleState(!state_.mixView, juce::dontSendNotification);
    if (fullMix) {
        for (auto *child : std::initializer_list<juce::Component*>{
                 &arrangement, &arrangement.overview, &namesBar, &mixer, &strip, &browser, &browserResizer,
                 &mixerView, &mixerViewResizer, &clipView, &clipResizer, &devices, &dockResizer, &infoSpectrum})
            child->setVisible(false);
        browserArea_ = mixerArea_ = mixerViewArea_ = stripArea_ = {};
        clipArea_ = deviceArea_ = infoArea_ = {};
        infoSpectrum.setBounds({});
        if (mixViewPanel.getParentComponent() == this) {
            mixViewPanel.setBounds(mixViewArea_);
            if (!mixViewPanel.isVisible() && reader_) mixViewPanel.frame(reader());
            mixViewPanel.setVisible(true);
        }
        dirty_.mark(DirtySet::All);
        return;
    }
    if (mixViewPanel.getParentComponent() == this)
        mixViewPanel.setVisible(false);
    arrangement.setVisible(true);
    // The detail row below the tracks. The Device View grows upward from its floor
    // (ADR-0203 d7), leaving the ruler and a row when there is room.
    // Live 12's Clip View stacks above the Device View (manual 8); the Device View leaves it its
    // floor, and it takes what it asks for while the arrangement keeps its ruler and a row.
    const int clipFloor = state_.clipView ? kClipViewFloor + gap : 0;
    const int dock = state_.docked ? std::min(body, std::max(state_.deviceFloor,
                                                             std::min(state_.deviceHeight, body - 100 - clipFloor)))
                                   : 0;
    const int clipRoom = std::max(0, body - dock - (dock > 0 ? gap : 0));
    const int clip = state_.clipView
                         ? std::min(clipRoom, std::max(kClipViewFloor, std::min(state_.clipViewHeight, clipRoom - 100)))
                         : 0;
    const int detail = dock + clip + (dock > 0 && clip > 0 ? gap : 0);
    const int rowY = bottom - detail;
    // Each panel has Live's dark 4 pt edge, which the root draws; the strip sits inside.
    // Widths belong to panels (ADR-0080 d2). If the window shrinks, share the
    // lost space proportionally above each panel's own minimum; the centre
    // keeps its minimum. Stored preferences survive a temporary resize.
    const auto &slots = state_.panels.slots();
    // Without In/Out, the track controls are narrower by its column, as Live's (§42.1); the
    // stored width is kept for when it comes back.
    int left = shownPanelWidth(slots[0]);
    int right = shownPanelWidth(slots[1]);
    const int leftMin = slots[0].collapsed ? 0 : slots[0].minWidth;
    const int rightMin = slots[1].collapsed ? 0 : slots[1].minWidth;
    const int available = std::max(0, w - state_.panels.arrangementMinWidth());
    if (left + right > available) {
        const int extra = std::max(0, available - leftMin - rightMin);
        const int wanted = left + right - leftMin - rightMin;
        const int leftExtra =
            wanted > 0 ? static_cast<int>(static_cast<double>(extra) * (left - leftMin) / wanted)
                       : 0;
        left = leftMin + leftExtra;
        right = rightMin + extra - leftExtra;
    }
    const int mid = std::max(0, rowY - gap - top);
    const bool browserLeft = slots[0].panel == Panel::Browser;
    const int bw = browserLeft ? left : right;
    // The Browser column spans the working area. Info takes its width and sits at its foot.
    browserArea_ = bw > gap ? juce::Rectangle<int>(browserLeft ? gap : w - right, top, bw - gap, body)
                            : juce::Rectangle<int>{};
    infoArea_ = {};
    if (infoShown_ && !browserArea_.isEmpty()) {
        // Keep the Browser's search and preview bars usable in a short window.
        const int browserBars = 2 * BrowserPanel::kBarHeight + look::border;
        const int infoHeight = std::min(state_.deviceFloor, std::max(0, body - browserBars - gap));
        if (infoHeight > 0) {
            infoArea_ = browserArea_.removeFromBottom(infoHeight);
            browserArea_.setHeight(browserArea_.getHeight() - gap);
        }
    }
    infoSpectrum.setBounds(infoArea_.reduced(look::border));
    if (infoArea_.isEmpty())
        infoSpectrum.setVisible(false);
    const int detailX = browserLeft ? gap + bw : gap;
    const int detailWidth = std::max(0, w - bw - 2 * gap);
    clipArea_ = clip > 0 ? juce::Rectangle<int>(detailX, rowY, detailWidth, clip) : juce::Rectangle<int>{};
    deviceArea_ = dock > 0 ? juce::Rectangle<int>(detailX, bottom - dock, detailWidth, dock)
                           : juce::Rectangle<int>{};
    // The mixer's columns sit against the arrangement, inside its frame, as Live's
    // track headers do; the browser stands apart, a gap away. Swapped, the track names lead,
    // then the track controls, then the clips, as Bitwig and Cubase have them (the
    // director's ruling of 28 October 2026 on ADR-0080): the controls sit in a gap the
    // arrangement leaves between its names and its lanes.
    const int mw = browserLeft ? right : left;
    // Live's strip down the outer edge is part of the panel's width: right of the track
    // controls, or, flipped, at the window's left edge, so the controls meet the lanes (the
    // director, 28 October 2026).
    const int sw = std::min(mw, MixerPanel::kRightStrip);
    const int namesW =
        browserLeft ? 0 : ArrangementView::headerWidth(std::max(0, w - right - gap - gap - mw));
    mixerArea_ = mw > sw ? juce::Rectangle<int>(browserLeft ? w - right - gap : gap + sw + namesW, top,
                                                mw - sw, mid)
                         : juce::Rectangle<int>();
    stripArea_ = sw > 0 ? juce::Rectangle<int>(browserLeft ? w - right - gap + mw - sw : gap, top, sw, mid)
                        : juce::Rectangle<int>();
    mixer.setVisible(!mixerArea_.isEmpty());
    mixer.setBounds(mixerArea_);
    strip.setVisible(!stripArea_.isEmpty());
    strip.setBounds(stripArea_);
    browser.setVisible(!browserArea_.isEmpty());
    browser.setBounds(browserArea_);
    const int browserEdge = browserLeft ? browserArea_.getRight() : browserArea_.getX();
    browserResizer.setBounds(browserEdge - gap / 2, top, gap, body);
    browserResizer.setVisible(!browserArea_.isEmpty());
    leftWidth_ = left;
    rightWidth_ = right;
    // The arrangement fills what the side panels leave, between the browser's gap and
    // the mixer's columns.
    const int centreX = browserLeft ? left + gap : gap + sw;
    const int centreW = std::max(0, w - right - gap - centreX);
    // Where the lanes' column starts: the arrangement's left edge, or past the names and the
    // track controls when swapped.
    const int lanesX = browserLeft ? centreX : centreX + namesW + mw - sw;
    const int lanesW = std::max(0, centreX + centreW - lanesX);
    arrangement.setNamesFirst(!browserLeft, mw - sw);
    // Live's Mixer View sits at the foot of the arrangement, its full width (§18.1); swapped,
    // under the lanes.
    const int mvh = state_.mixerView
                        ? std::clamp(state_.mixerViewHeight, 120,
                                     std::max(120, mid - arrangement.topHeight() - 64))
                        : 0;
    mixerViewArea_ = mvh > 0 && mid > mvh
                         ? juce::Rectangle<int>(browserLeft ? centreX : lanesX, top + mid - mvh,
                                                browserLeft ? centreW : lanesW, mvh)
                         : juce::Rectangle<int>();
    mixerView.setVisible(!mixerViewArea_.isEmpty());
    mixerViewResizer.setVisible(!mixerViewArea_.isEmpty());
    mixerView.setBounds(mixerViewArea_.withTrimmedTop(1));
    mixerViewResizer.setBounds(mixerViewArea_.getX(), mixerViewArea_.getY() - 3,
                               mixerViewArea_.getWidth(), 6);
    status.mixerToggle.setToggleState(state_.mixerView, juce::dontSendNotification);
    namesBar.flip.setToggleState(!browserLeft, juce::dontSendNotification);
    strip.tracks.setToggleState(state_.largeTracks, juce::dontSendNotification);
    namesBar.inOut.setToggleState(state_.inOut, juce::dontSendNotification);
    namesBar.sends.setToggleState(state_.sends, juce::dontSendNotification);
    namesBar.mixer.setToggleState(state_.trackMixer, juce::dontSendNotification);
    namesBar.returns.setToggleState(!state_.hideReturns, juce::dontSendNotification);
    namesBar.delay.setToggleState(state_.trackOptions, juce::dontSendNotification);
    arrangement.setBounds(centreX, top, centreW, mid - mixerViewArea_.getHeight());
    // The bar over the track names, their controls and the strip, level with the beat ruler (the
    // Arrangement sheet); the strip's head under it.
    {
        const auto names = arrangement.headers.getBounds() + arrangement.getPosition();
        auto span = names.getHorizontalRange();
        for (const auto &r : {mixerArea_, stripArea_})
            if (!r.isEmpty())
                span = span.getUnionWith(r.getHorizontalRange());
        const int barY = top + arrangement.overviewHeight();
        // The sheet's row, 26 high: it ends above Set, which stands at the ruler's foot.
        namesBar.setBounds(span.getStart(), barY, span.getLength(), ArrangementView::kRulerHeight - 3);
        strip.setHead(arrangement.overviewHeight() + ArrangementView::kRulerHeight);
    }
    namesBar.setVisible(namesBar.getWidth() > 0);
    // The Overview runs from the lanes' left edge to Live's H and W buttons.
    const int overviewX = lanesX + ArrangementView::kLeftMargin;
    const int overviewRight = arrangement.getX() + arrangement.canvas.getRight() - 4;
    arrangement.overview.setBounds(overviewX, top + 4, std::max(0, overviewRight - overviewX),
                                   ArrangementView::kOverviewHeight - 4);
    arrangement.overview.setVisible(state_.overview);
    devices.setVisible(!deviceArea_.isEmpty());
    dockResizer.setVisible(!deviceArea_.isEmpty());
    devices.setBounds(deviceArea_.reduced(look::border));
    dockResizer.setBounds(deviceArea_.getX(), deviceArea_.getY() - 3, deviceArea_.getWidth(), 6);
    dockResizer.toFront(false);
    clipView.setVisible(!clipArea_.isEmpty());
    clipResizer.setVisible(!clipArea_.isEmpty());
    clipView.setBounds(clipArea_.reduced(look::border));
    clipResizer.setBounds(clipArea_.getX(), clipArea_.getY() - 3, clipArea_.getWidth(), 6);
    clipResizer.toFront(false);
    status.clipToggle.setToggleState(state_.clipView, juce::dontSendNotification);
    dirty_.mark(DirtySet::All);
}
void AdiRootComponent::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    // Live's panels have a dark 4 pt edge with rounded corners.
    auto panel = [&g](juce::Rectangle<int> r) {
        g.setColour(look::band);
        g.fillRoundedRectangle(r.toFloat(), look::radiusControl);
    };
    if (!deviceArea_.isEmpty())
        panel(deviceArea_);
    if (!clipArea_.isEmpty())
        panel(clipArea_);
    // Live's Info View: a title bar with the element's name, its description below.
    if (!infoArea_.isEmpty()) {
        panel(infoArea_);
        const auto inner = infoArea_.reduced(look::border);
        g.setColour(look::panel);
        g.fillRect(inner);
        const auto titleBar = inner.withHeight(17);
        g.setColour(look::field);
        g.fillRect(titleBar);
        const int split = infoText_.indexOfChar('\n');
        const auto title = split < 0 ? infoText_ : infoText_.substring(0, split);
        const auto body = split < 0 ? juce::String() : infoText_.substring(split + 1);
        g.setColour(look::text);
        g.setFont(look::font(look::fontField, true));
        g.drawText(title, titleBar.reduced(5, 0), juce::Justification::centredLeft);
        // Live's body text: small, on a 15 pt line.
        juce::AttributedString words(body);
        words.setFont(look::font(look::fontLabel));
        words.setColour(look::text);
        words.setLineSpacing(2.5f);
        juce::TextLayout layout;
        layout.createLayout(words, static_cast<float>(inner.getWidth() - 10));
        // A description longer than the panel is cut at its foot, as Live's; it never spills
        // onto the panel's edge below (each platform's fonts wrap it differently).
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(inner);
        layout.draw(g, inner.withTrimmedTop(20).reduced(5, 2).toFloat());
    }
}
void AdiRootComponent::applyInfoAnalyserMode() {
    using Mode = SharedSpectrumView::Mode;
    using Scale = SharedSpectrumView::SpectrumScale;
    switch (state_.infoAnalyserMode) {
    case InfoAnalyserMode::Spectrogram:
        infoSpectrum.setMode(Mode::Spectrogram);
        break;
    case InfoAnalyserMode::FilledSpectrum:
        infoSpectrum.setMode(Mode::FilledSpectrum);
        break;
    case InfoAnalyserMode::LineSpectrum:
        infoSpectrum.setSpectrumScale(Scale::Device);
        infoSpectrum.setMode(Mode::Spectrum);
        break;
    case InfoAnalyserMode::ClassicScope:
        infoSpectrum.setMode(Mode::ClassicScope);
        break;
    }
}
void AdiRootComponent::mouseDown(const juce::MouseEvent &e) {
    const bool inInfo = infoArea_.contains(e.getPosition());
    infoPressed_ = inInfo && e.mods.isLeftButtonDown() && !e.mods.isPopupMenu();
    if (!inInfo || !e.mods.isPopupMenu()) return;
    juce::PopupMenu menu;
    constexpr std::array choices{
        std::pair{InfoAnalyserMode::Spectrogram, "Spectrogram"},
        std::pair{InfoAnalyserMode::FilledSpectrum, "Filled Spectrum"},
        std::pair{InfoAnalyserMode::LineSpectrum, "Line Spectrum"},
        std::pair{InfoAnalyserMode::ClassicScope, "Classic Scope"}};
    const SafePointer<AdiRootComponent> safe(this);
    for (const auto &[mode, name] : choices)
        menu.addItem(name, true, state_.infoAnalyserMode == mode, [safe, mode = mode] { // a named copy: Clang 14 captures no structured binding
            if (!safe || safe->state_.infoAnalyserMode == mode) return;
            const auto previous = std::exchange(safe->state_.infoAnalyserMode, mode);
            if (!safe->persist()) {
                safe->state_.infoAnalyserMode = previous;
                safe->fail(safe->error_);
                return;
            }
            safe->applyInfoAnalyserMode();
        });
    if (onInfoMenu) {
        onInfoMenu(menu);
        return;
    }
    const auto where = e.getScreenPosition();
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea(
        juce::Rectangle<int>(where.x, where.y, 1, 1)));
}
void AdiRootComponent::mouseUp(const juce::MouseEvent &e) {
    if (std::exchange(infoPressed_, false) && infoArea_.contains(e.getPosition()) &&
        !e.mouseWasDraggedSinceMouseDown() && (state_.mixView || setMixView(true, state_.mixViewDetached)))
        chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Analysis));
}
void AdiRootComponent::paintOverChildren(juce::Graphics &g) {
    const auto *focused = juce::Component::getCurrentlyFocusedComponent();
    if (!focused || !isParentOf(focused))
        return;
    const auto containsFocus = [focused](const juce::Component &panel) {
        return panel.isVisible() && (&panel == focused || panel.isParentOf(focused));
    };
    const juce::Component *owner = nullptr;
    if (containsFocus(arrangement.overview) || containsFocus(namesBar))
        owner = &arrangement;
    else
        for (const auto *panel : std::array<const juce::Component *, 8>{
                 &transport, &browser, &arrangement, &mixer, &mixerView, &devices, &clipView, &mixViewPanel})
            if (containsFocus(*panel)) {
                owner = panel;
                break;
            }
    if (owner && owner->getWidth() > 8) {
        g.setColour(look::focus);
        g.fillRect(owner->getX() + 4, owner->getY() + 1, std::min(32, owner->getWidth() - 8), 2);
    }
}
bool AdiRootComponent::keyPressed(const juce::KeyPress &key) {
    // With Use Tab Key to Move Focus off, Shift+Tab switches the Detail View.
    // Tab's Arrangement/Session switch waits for Session View.
    const auto mods = key.getModifiers();
    const bool plainTab = key.getKeyCode() == juce::KeyPress::tabKey && !mods.isCommandDown() &&
                          !mods.isCtrlDown() && !mods.isAltDown();
    if (plainTab && !commands_.tabMovesFocus()) {
        if (mods.isShiftDown())
            commands_.key(key, *this);
        return true;
    }
    // With Wrap Tab Navigation off, Tab stops at the area's last control (Shift+Tab at its
    // first) instead of going round to the other end.
    if (plainTab && !commands_.wrapTabNavigation())
        if (auto *focused = juce::Component::getCurrentlyFocusedComponent();
            focused != nullptr && isParentOf(focused))
            if (auto traverser = focused->createKeyboardFocusTraverser())
                if ((mods.isShiftDown() ? traverser->getPreviousComponent(focused)
                                        : traverser->getNextComponent(focused)) == nullptr)
                    return true;
    return commands_.key(key, *this);
}
namespace {
bool wantsTextInput(juce::Component *focused) {
    const auto *target = dynamic_cast<juce::TextInputTarget *>(focused);
    return target != nullptr && target->isTextInputActive();
}
bool canUseComputerMidiKeys() {
    return !wantsTextInput(juce::Component::getCurrentlyFocusedComponent()) &&
           juce::Component::getCurrentlyModalComponent() == nullptr;
}
} // namespace
bool AdiRootComponent::playComputerMidiKey(const juce::KeyPress &key) {
    if (!computerMidiEnabled_ || key.getModifiers().isAnyModifierKeyDown() || !canUseComputerMidiKeys())
        return false;
    return computerMidi_.press(computerMidiKeyCode(key), [this](const engine::Event &event) {
        if (sendComputerMidi)
            sendComputerMidi(event);
    });
}
juce::KeyPress AdiRootComponent::computerMidiShortcut(const juce::KeyPress &key) const {
    if (computerMidiEnabled_ && key.getModifiers().getRawFlags() == juce::ModifierKeys::shiftModifier &&
        ComputerMidiKeyboard::recognizes(computerMidiKeyCode(key)) &&
        commands_.manager.getKeyMappings()->findCommandForKeyPress(key) == 0 && canUseComputerMidiKeys())
        return juce::KeyPress(key.getKeyCode());
    return key;
}
bool AdiRootComponent::pressComputerMidiToggle(int key) {
    if (!canUseComputerMidiKeys())
        return false;
    if (computerMidiToggleKey_ && computerMidiKeyDown(input, *computerMidiToggleKey_))
        return true;
    computerMidiToggleKey_ = key;
    return command(AppCommands::ComputerMidiKeyboard);
}
void AdiRootComponent::releaseComputerMidiKeys() {
    computerMidi_.releaseAll([this](const engine::Event &event) {
        if (sendComputerMidi)
            sendComputerMidi(event);
    });
}
void AdiRootComponent::globalFocusChanged(juce::Component *focused) {
    repaint();
    if (focused != nullptr && (focused == this || isParentOf(focused)) && !wantsTextInput(focused))
        return;
    releaseComputerMidiKeys();
    computerMidiToggleKey_.reset();
}
void AdiRootComponent::resizeMixerView(int height) {
    const int body = getHeight() - look::controlBarHeight - look::statusBarHeight - deviceArea_.getHeight();
    state_.mixerViewHeight =
        std::clamp(height, 120,
                   std::max(120, body - arrangement.topHeight() - state_.laneHeight));
    resized();
}
void AdiRootComponent::resizeClipView(int height) {
    state_.clipViewHeight = std::clamp(height, kClipViewFloor, 4000);
    resized();
}
bool AdiRootComponent::noteEditorShown() const { return state_.clipView && clipView.midi() && !clipView.envelopesShown() && clipView.details() != nullptr; }
bool AdiRootComponent::noteEditorHasContext() const { return noteEditorShown() && clipView.notes.hasContext(); }
void AdiRootComponent::toggleClipViewFull() {
    if (!state_.clipView)
        return;
    if (clipViewRestore_ > 0) {
        resizeClipView(std::exchange(clipViewRestore_, 0));
        return;
    }
    clipViewRestore_ = state_.clipViewHeight;
    resizeClipView(4000);
}
void AdiRootComponent::resizeDevices(int height) {
    const int ceiling = getHeight() - look::controlBarHeight - look::statusBarHeight - look::gap -
                        arrangement.topHeight() - state_.laneHeight;
    state_.setDeviceHeight(std::min(height, ceiling));
    resized();
}
void AdiRootComponent::resizeBrowser(int width) {
    auto &panels = state_.panels;
    const auto &browserPanel = *panels.find(Panel::Browser);
    const auto &mixerPanel = *panels.find(Panel::Mixer);
    // The mixer keeps its own preference. Account for the hidden In/Out column
    // and the two gutters when enforcing the arrangement's minimum.
    const int hiddenMixerWidth = panels.occupied(mixerPanel) - shownPanelWidth(mixerPanel);
    const int available = getWidth() - 2 * look::gap + hiddenMixerWidth;
    const int maximum = available - panels.arrangementMinWidth() - panels.occupied(mixerPanel);
    if (browserPanel.collapsed || maximum < browserPanel.minWidth) return;
    width = std::clamp(width, browserPanel.minWidth, maximum);
    if (width != browserPanel.width && !panels.setWidth(Panel::Browser, width, available))
        resized();
}
bool AdiRootComponent::persist() {
    if (persistence_.save(window_, state_, error_))
        return true;
    if (onStatusMessage && !error_.empty())
        onStatusMessage(error_);
    dirty_.mark(DirtySet::Status);
    return false;
}
juce::PopupMenu AdiRootComponent::mixerOptionsMenu(bool viewMenu) {
    // Live 12's order (manual 18.1, the Mixer View Control's options menu). Volume always shows in
    // ADI; Track Options is the Mixer View's Track Delay section (manual 18.7); the crossfader and
    // Performance Impact wait on the engine. The View menu's Mixer Controls submenu (manual 6.1,
    // its figure) draws a separator after Volume too; the Mixer View Control's own menu (18.1, its
    // figure) only the one before Return Tracks.
    commands_.activate(*this);
    juce::PopupMenu menu;
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleMixerInOut, "In/Out");
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleMixerSends, "Sends");
    menu.addItem(juce::PopupMenu::Item("Volume (always shown)").setTicked(true).setEnabled(false));
    if (viewMenu)
        menu.addSeparator();
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleMixerTrackOptions, "Track Options");
    menu.addItem(juce::PopupMenu::Item("Crossfader (requires Session View)").setEnabled(false));
    menu.addItem(juce::PopupMenu::Item("Performance Impact (requires per-track CPU timings)").setEnabled(false));
    menu.addSeparator();
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleReturnTracks);
    return menu;
}
juce::PopupMenu AdiRootComponent::trackControlsMenu() {
    // Live 12's Arrangement Track Controls submenu (the View menu; manual 6.1: "You can customize
    // which controls are shown"; 18.7: Track Options holds Track Delay): the track controls' In/Out
    // and Sends, with their keys. Volume is also the M selector; Track Options is the rows' Track Delay
    // box, the D selector's (Live 11's). Live 12's full list is to be measured: its 6.1 says track
    // delays are the mixer's only. First, ADI's side panel of track controls, shown or hidden whole,
    // which the View menu held as "Arrangement Track Controls" before the submenu.
    commands_.activate(*this);
    juce::PopupMenu menu;
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleMixer, "Hide/Show Track Controls");
    menu.addSeparator();
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleInOut);
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleSends);
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleTrackMixer, "Volume");
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleTrackOptions);
    return menu;
}
void AdiRootComponent::setMixProvider(const MixViewProvider &provider) {
    mixProvider_ = &provider;
    arrangement.applyView();
    arrangement.firstShownIfHidden();
    dirty_.mark(DirtySet::All);
}
std::int64_t AdiRootComponent::referenceTrack() const noexcept { return referenceTrack(reader()); }
void AdiRootComponent::addReferenceCommand(juce::PopupMenu &menu, int command) {
    const bool assigned = referenceTrack() != 0;
    juce::String label;
    if (command == AppCommands::PlayReference)
        label = assigned ? "Play Reference (no monitor connection)" : "Play Reference (no track assigned)";
    else if (!assigned)
        label = "Reference Track (no track assigned)";
    menu.addCommandItem(&commands_.manager, command, std::move(label));
}
std::int64_t AdiRootComponent::referenceTrack(const SnapshotReader &snapshot) const noexcept {
    const auto provided = mixProvider_->reference(snapshot.tracks());
    if (provided != 0 && snapshot.findTrack(provided)) return provided;
    if (const auto *current = snapshot.get(); current && snapshot.findTrack(current->cue.projectReferenceTrack))
        return current->cue.projectReferenceTrack;
    return 0;
}
bool AdiRootComponent::keyStateChanged(bool isKeyDown) {
    const auto now = input.ms();
    if (computerMidiToggleKey_ && !computerMidiKeyDown(input, *computerMidiToggleKey_))
        computerMidiToggleKey_.reset();
    const bool releasedMidi = computerMidiEnabled_ && computerMidi_.releaseUp(
        [this](int key) { return computerMidiKeyDown(input, key); },
        [this](const engine::Event &event) { if (sendComputerMidi) sendComputerMidi(event); });
    // A latching key let go: after a hold, the window goes back (Live 42.25).
    bool handled = releasedMidi;
    if (latch_.held() && !input.down(latch_.key()))
        handled = latchKey(0, latch_.key(), false, now) || handled;
    return handled || juce::Component::keyStateChanged(isKeyDown);
}
bool AdiRootComponent::latches(int command) noexcept {
    return command == AppCommands::SoloSelected || command == AppCommands::ZoomToSelection || command == AppCommands::DrawMode ||
           (command >= AppCommands::ActivateTrack1 && command <= AppCommands::ActivateTrack8);
}
bool AdiRootComponent::latchKey(int id, int keyCode, bool down, std::int64_t nowMs) {
    if (down) {
        // The first press acts; a repeat while the key is held does nothing.
        if (!latch_.press(keyCode, id, nowMs))
            return true;
        if (id == AppCommands::DrawMode) {
            drawBeforeLatch_ = drawMode();
            trimBeforeDrawLatch_ = clipView.notes.trimMode();
            return command(id);
        }
        const auto before = submitter_.nextUndo();
        const bool done = command(id);
        if (const auto after = submitter_.nextUndo(); after && (!before || after->txnId != before->txnId))
            latch_.made(after->txnId);
        return done;
    }
    const auto back = latch_.release(keyCode, nowMs);
    if (!back)
        return false;
    if (back->command == AppCommands::ZoomToSelection)
        return command(AppCommands::ZoomBack);
    if (back->command == AppCommands::DrawMode) {
        clipView.notes.setDrawMode(drawBeforeLatch_);
        if (trimBeforeDrawLatch_)
            clipView.notes.setTrimMode(true);
        return true;
    }
    // Only the step the press made, while it is still the latest: an edit since then stays, and so
    // does what the press made under it. Going back pauses no Follow, as an undo from the menu does.
    // A knob edit still held commits first (DF-W29), so the check sees the step that is latest.
    if (settleEdits && !settleEdits())
        return false;
    const auto last = submitter_.nextUndo();
    if (back->step == 0 || !last || last->txnId != back->step)
        return false;
    const bool paused = arrangement.followPaused;
    const bool done = command(AppCommands::Undo);
    arrangement.followPaused = paused;
    return done;
}
std::int64_t AdiRootComponent::analysisTrack(bool selected) const noexcept {
    return analysisTrack(reader(), selected);
}
std::int64_t AdiRootComponent::analysisTrack(const SnapshotReader &snapshot, bool selected) const noexcept {
    if (selected) {
        // A detached window can advance while the main window's clock is suspended. Its
        // source follows the session's newer selection and its own current project snapshot.
        const auto id = selectionGeneration_ != session_.generation() ? session_.selection().track
                                                                     : arrangement.geometry.selection.track;
        if (id != 0 && snapshot.findTrack(id)) return id;
        return 0;
    }
    for (const auto &track : snapshot.tracks())
        if (track->kind == "master") return track->id;
    return 0;
}
bool AdiRootComponent::setMixView(bool open, bool detached) {
    const bool wasOpen = state_.mixView, wasDetached = state_.mixViewDetached;
    const int oldTab = state_.mixViewTab;
    const auto focusMembers = open && !wasOpen ? MixFocus::soloedChannels(SnapshotReader(current()))
                                               : std::vector<std::int64_t>{};
    if (open && !wasOpen)
        state_.mixViewTab = static_cast<int>(focusMembers.size() >= 2 ? MixViewPanel::Tab::Focus : MixViewPanel::Tab::Stack);
    state_.mixView = open;
    state_.mixViewDetached = detached;
    if (!persist()) {
        state_.mixView = wasOpen;
        state_.mixViewDetached = wasDetached;
        state_.mixViewTab = oldTab;
        return fail(error_);
    }
    const juce::Component::SafePointer<AdiRootComponent> alive(this);
    if (open && !wasOpen) mixViewPanel.focusPage.setMembers(focusMembers);
    if (!alive) return false;
    mixViewPanel.focusPage.suspend(!open);
    if (!alive) return false;
    if (wasOpen != open) mixViewPanel.mainMeter.suspend(!open);
    if (!alive) return false;
    if (wasOpen != open || wasDetached != detached) {
        mixViewPanel.song.cancelSeek();
        mixViewPanel.stack.cancelInteractions(); devices.cancelFunctionInteractions();
    }
    mixViewPanel.stack.suspendParameters(!open || (!detached && devices.parametersSuspended()));
    resized();
    return true;
}
bool AdiRootComponent::chooseMixViewTab(int tab) {
    const int was = state_.mixViewTab;
    state_.mixViewTab = std::clamp(tab, 0, MixViewPanel::kTabCount - 1);
    if (!persist()) {
        state_.mixViewTab = was;
        return fail(error_);
    }
    const juce::Component::SafePointer<AdiRootComponent> alive(this);
    if ((was == static_cast<int>(MixViewPanel::Tab::Stack) || was == static_cast<int>(MixViewPanel::Tab::Focus)) &&
        state_.mixViewTab != was) {
        mixViewPanel.stack.cancelInteractions();
        if (!alive) return false;
        devices.cancelFunctionInteractions();
        if (!alive) return false;
        if (was == static_cast<int>(MixViewPanel::Tab::Focus))
            mixViewPanel.focusPage.setVisible(false);
        if (!alive) return false;
    }
    dirty_.mark(DirtySet::Layout);
    return true;
}
void AdiRootComponent::publishSelection(SelectionOrigin origin) {
    const auto before = session_.selection().track;
    std::string why;
    if (!session_.select(arrangement.geometry.selection, why))
        fail(why);
    selectionGeneration_ = session_.generation();
    const auto track = session_.selection().track;
    if (track != before) {
        refreshSelectionArming();
        revealInLanes_ = origin == SelectionOrigin::Mixer ? track : 0;
        revealInMixer_ = origin == SelectionOrigin::Arrangement ? track : 0;
        if (origin != SelectionOrigin::Other)
            dirty_.mark(DirtySet::Layout);
    }
}
bool AdiRootComponent::changeSelectionArming(std::int64_t track) {
    if (!setSelectionArmedTrack || track == selectionArmedTrack_)
        return true;
    if (!setSelectionArmedTrack(track))
        return fail("The selected track's recording arm could not be updated.");
    selectionArmedTrack_ = track;
    setRecordStates(freshRecordStates());
    mark(DirtySet::Content);
    return true;
}
void AdiRootComponent::refreshSelectionArming() {
    const auto mode = commands_.selectionArm();
    const auto current = view_.current();
    const auto *track = current ? current->findTrack(session_.selection().track) : nullptr;
    const bool eligible = track && !track->frozen &&
                          ((mode == AppCommands::SelectionArm::All && armable(*track)) ||
                           (mode == AppCommands::SelectionArm::Midi && isMidiTrack(*track)));
    changeSelectionArming(eligible ? track->id : 0);
}
bool AdiRootComponent::submitEdit(const std::string &op, Payload payload, const char *label) {
    OpRequest request;
    request.opType = op;
    request.payload = std::move(payload);
    request.label = label;
    request.selBefore = session_.advisory();
    return edited(submitter_.submit(std::move(request)));
}
bool AdiRootComponent::submitEdits(std::vector<OpRequest> ops) {
    if (ops.empty())
        return true;
    ops.front().selBefore = session_.advisory();
    return edited(submitter_.submit(std::move(ops)));
}
bool AdiRootComponent::writeControllerPoint(const controllers::Address &address, const controllers::Point &point,
                                            const std::optional<std::int64_t> &expectedHead) {
    const std::array points{point};
    auto prepared = controllers::prepareWrite(editStore(), address.clipId, address.channel, address.controller,
                                              points);
    if (!prepared.ok)
        return fail(prepared.error);
    if ((address.id != 0 && prepared.address.id != address.id) || prepared.expectedHead != expectedHead)
        return fail("The MIDI controller envelope changed during the edit.");
    std::vector requests{std::move(prepared.request)};
    requests.front().selBefore = session_.advisory();
    return edited(submitter_.commitPrepared(requests, [&](Store &store, Actor) {
        prepared.request = std::move(requests.front());
        return controllers::commitPrepared(store, prepared);
    }));
}
bool AdiRootComponent::deleteControllerPoints(const controllers::Lane &lane, std::span<const std::uint64_t> points) {
    if (!lane.channel || !lane.controller || points.empty())
        return false;
    const controllers::Address address{lane.id, lane.clipId, static_cast<int>(*lane.channel),
                                       static_cast<int>(*lane.controller)};
    return submitEdits({controllers::deleteRequest(address, points)});
}
double AdiRootComponent::editRate() const noexcept {
    return sampleRate_ > 0 ? sampleRate_ : (reader_ ? reader_->sampleRate() : 48000.0);
}
bool AdiRootComponent::placeClips(const std::vector<ClipPlace> &places, const char *label) {
    const auto current = view_.current();
    if (!current || places.empty())
        return fail("Select a clip to move.");
    std::vector<edit_plan::Move> moves;
    moves.reserve(places.size());
    for (const auto &place : places)
        moves.push_back({place.clip, {place.track, std::nullopt}, {0, place.position}});
    edit_plan::Options options;
    options.referenceRate = editRate();
    options.policy = edit_plan::capturePolicy(commands_.settings());
    auto prepared = edit_plan::prepareMove(submitter_.store(), moves, options);
    if (!prepared.ok) {
        // Preserve the existing move for the planner's policy exclusions. A failed splice
        // must stop the whole edit before moving any material.
        if (options.policy.deleteOverlaps && !options.policy.mutedClipsAsDeleted &&
            prepared.error != "frozen placement requires TM-FC-04 policy" &&
            prepared.error != "frozen-group placement requires TM-FC-04 policy" &&
            prepared.error != "change or detach the take lane explicitly before moving it" &&
            prepared.error != edit_plan::toString(edit_plan::PlacementError::OverlappingIncoming))
            return commitEdit(std::move(prepared));
        std::vector<OpRequest> ops;
        for (const auto &place : places) {
            OpRequest op;
            op.opType = "clip.move";
            op.payload = {{"id", place.clip}, {"to", place.position}, {"track", place.track}};
            op.label = label;
            ops.push_back(std::move(op));
        }
        const auto why = prepared.error;
        if (!submitEdits(std::move(ops)))
            return false;
        say("Moved, keeping what the clips land on: the engine does not place them here yet (" + why + ").");
        return true;
    }
    if (!prepared.requests.empty())
        prepared.requests.front().label = label;
    return commitEdit(std::move(prepared));
}
std::vector<std::int64_t> AdiRootComponent::copyClips(const std::vector<ClipPlace> &places, const char *label) {
    const auto current = view_.current();
    if (!current || places.empty()) {
        fail("Select a clip to copy.");
        return {};
    }
    std::vector<clip_edit::ClipPlacement> copies;
    std::vector<std::int64_t> ids;
    auto id = audio_drop_detail::newClipId(*current);
    for (const auto &place : places) {
        copies.push_back({place.clip, id, place.track, {0, place.position}});
        ids.push_back(id++);
    }
    auto prepared = clip_edit::prepareDuplicates(submitter_.store(), copies,
                                                 {editRate(), edit_plan::capturePolicy(commands_.settings())});
    if (prepared.ok && !prepared.requests.empty())
        prepared.requests.front().label = label;
    if (!commitEdit(std::move(prepared)))
        return {};
    return ids;
}
bool AdiRootComponent::commitEdit(clip_edit::PreparedEdit prepared, std::optional<OpRequest> appended) {
    if (!prepared.ok)
        return fail(prepared.error.empty() ? "The edit could not be prepared." : prepared.error);
    if (prepared.requests.empty())
        return true;
    prepared.requests.front().selBefore = session_.advisory();
    return edited(submitter_.commitPrepared(
        std::move(prepared),
        [](Store &store, clip_edit::PreparedEdit &p, Actor actor) { return clip_edit::commitPrepared(store, p, actor); },
        std::move(appended)));
}
namespace {
// The clip chosen by a click: the selection is that clip's own span (ArrangementView::select).
struct ChosenClip {
    const engine::TrackNode *track = nullptr;
    const engine::ClipNode *clip = nullptr;
};
ChosenClip chosenClip(const engine::Snapshot &current, const Selection &sel) {
    // A selection widened to other tracks (a drag across lanes, Shift or Ctrl/Cmd on a title) is a
    // time selection, whatever clip the press began on.
    if (sel.clip == 0 || (sel.toTrack != 0 && sel.toTrack != sel.track) || !sel.picked.empty())
        return {};
    const auto from = std::min(sel.start, sel.end), to = std::max(sel.start, sel.end);
    for (const auto &t : current.tracks)
        if (t)
            for (const auto &c : t->clips)
                if (c && c->id == sel.clip && from == c->posTicks && to == c->posTicks + c->lengthTicks)
                    return {t.get(), c.get()};
    return {};
}
struct ReverseTargets {
    std::vector<std::int64_t> clips;
    std::optional<std::pair<std::int64_t, std::int64_t>> range;
};
ReverseTargets reverseTargets(const engine::Snapshot &current, const ArrangementGeometry &geometry,
                              const SnapshotReader &reader) {
    ReverseTargets result;
    const auto &sel = geometry.selection;
    const auto [from, to] = std::minmax(sel.start, sel.end);
    if (from != to && !chosenClip(current, sel).clip && sel.pickedClips.empty())
        result.range.emplace(from, to);
    for (const auto &track : current.tracks) {
        if (!track || (result.range && !geometry.inSelection(reader, track->id)))
            continue;
        for (const auto &clip : track->clips) {
            if (!clip)
                continue;
            const bool chosen = result.range ? clip->posTicks < to && clip->posTicks + clip->lengthTicks > from
                                             : geometry.clipSelected(clip->id);
            if (!chosen)
                continue;
            if (!clip->audio)
                return {};
            result.clips.push_back(clip->id);
        }
    }
    return result;
}
bool holdsClips(const engine::TrackNode &t) { return t.kind == "audio" || isMidiTrack(t); }
std::string_view clipTrackKind(const engine::TrackNode &t) { return isMidiTrack(t) ? "midi" : std::string_view(t.kind); }
// A copied clip's kind, as its image holds it ("audio" or "midi").
std::string imageKind(const Payload &image) {
    if (image.contains("row") && image["row"].contains("kind") && image["row"]["kind"].is_string())
        return image["row"]["kind"].get<std::string>();
    return {};
}
std::string kindWord(std::string_view kind) { return kind == "midi" ? "a MIDI" : "an audio"; }
} // namespace
bool AdiRootComponent::deviceClipboardFirst(DeviceChainStrip::Clip clip) const {
    // The device chain's when a device has the keyboard focus (Live's focus is where the command
    // goes); Paste also at the chain's chosen end while the Arrangement holds no copy.
    if (!devices.canClipboard(clip))
        return false;
    return devices.hasKeyboardFocus(true) ||
           (clip == DeviceChainStrip::Clip::Paste && commands_.clipClipboard().empty() &&
            !arrangement.canvas.hasAutomationSelection());
}
bool AdiRootComponent::canClipboard(int id) const {
    using Clip = DeviceChainStrip::Clip;
    const auto clip = id == AppCommands::Cut    ? Clip::Cut
                      : id == AppCommands::Copy ? Clip::Copy
                      : id == AppCommands::Paste ? Clip::Paste
                                                 : Clip::Duplicate;
    const auto command = id == AppCommands::Cut    ? ClipCommand::Cut
                         : id == AppCommands::Copy ? ClipCommand::Copy
                         : id == AppCommands::Paste ? ClipCommand::Paste
                                                    : ClipCommand::Duplicate;
    return devices.canClipboard(clip) || canClipClipboard(command);
}
namespace {
// The project file a clipboard copy names (`sourceFile`), as the engine reads the store's.
std::string projectFile(Store &store) {
    SQLite::Statement row(store.db(), "PRAGMA database_list");
    while (row.executeStep())
        if (row.getColumn(1).getString() == "main")
            return row.getColumn(2).getString();
    return {};
}
} // namespace
std::int64_t AdiRootComponent::pasteOrigin() const {
    // The top selected track that can hold clips, in the lanes' order, whichever way the selection
    // was made (an upward drag keeps the lower track as its anchor).
    for (const auto id : arrangement.geometry.selectedTracks(reader()))
        if (const auto *t = reader().findTrack(id); t != nullptr && arrangement.geometry.shown(id) && holdsClips(*t))
            return id;
    return 0;
}
clip_edit::MidiConsolidation AdiRootComponent::prepareMidiConsolidation() const {
    clip_edit::MidiConsolidation unavailable;
    unavailable.edit.error = "Select at least two whole MIDI clips on the same Arrangement track.";
    const auto current = view_.current();
    const auto &geo = arrangement.geometry;
    const auto &sel = geo.selection;
    if (!current || !arrangement.isVisible() || arrangement.canvas.hasAutomationSelection() ||
        noteEditorHasContext() || sel.track == 0)
        return unavailable;
    if (submitter_.store().readOnly()) {
        unavailable.edit.error = "This project is read-only.";
        return unavailable;
    }
    auto tracks = geo.selectedTracks(reader());
    if (tracks.size() != 1 || !geo.shown(tracks.front()))
        return unavailable;
    const auto *track = current->findTrack(tracks.front());
    if (track == nullptr || (track->kind != "midi" && track->kind != "instrument"))
        return unavailable;
    auto begin = std::min(sel.start, sel.end), end = std::max(sel.start, sel.end);
    std::vector<std::int64_t> picked;
    if (sel.clip != 0) {
        picked = geo.selectedClips();
        if (picked.size() < 2)
            return unavailable;
        begin = std::numeric_limits<std::int64_t>::max();
        end = 0;
        std::size_t found = 0;
        for (const auto &clip : track->clips)
            if (geo.clipSelected(clip->id)) {
                begin = std::min(begin, clip->posTicks);
                end = std::max(end, clip->posTicks + clip->lengthTicks);
                ++found;
            }
        if (found != picked.size())
            return unavailable;
    }
    if (begin >= end)
        return unavailable;
    const edit_plan::Options options{editRate(), edit_plan::capturePolicy(commands_.settings())};
    return clip_edit::prepareMidiConsolidation(submitter_.store(), {std::move(tracks), {0, begin}, {0, end}}, picked, options);
}
bool AdiRootComponent::canConsolidateMidi() const {
    return prepareMidiConsolidation().edit.ok;
}
bool AdiRootComponent::canClipClipboard(ClipCommand command) const {
    const auto current = view_.current();
    if (!current)
        return false;
    const auto &sel = arrangement.geometry.selection;
    const auto &board = commands_.clipClipboard();
    const bool envelopeCopy = std::holds_alternative<AppCommands::ClipClipboard::EnvelopeCopy>(board.held);
    if (arrangement.canvas.hasAutomationSelection())
        return command == ClipCommand::Paste ? envelopeCopy : sel.start != sel.end;
    if (command == ClipCommand::Paste) {
        if (std::holds_alternative<AppCommands::TrackCopy>(board.held))
            return trackClipboardContext();
        return !board.empty() && !envelopeCopy && pasteOrigin() != 0;
    }
    if (trackClipboardContext())
        if (const auto *track = current->findTrack(sel.track); track != nullptr && track->kind != "master")
            return true;
    if (!sel.pickedClips.empty() || chosenClip(*current, sel).clip != nullptr)
        return true;
    if (sel.start == sel.end)
        return false;
    for (const auto id : arrangement.geometry.selectedTracks(reader()))
        if (const auto *t = reader().findTrack(id); t != nullptr && arrangement.geometry.shown(id) && holdsClips(*t))
            return true;
    return false;
}
bool AdiRootComponent::trackClipboardContext() const {
    const auto &sel = arrangement.geometry.selection;
    if (sel.clip != 0 || !sel.pickedClips.empty() || sel.start != sel.end)
        return false;
    const auto *focus = juce::Component::getCurrentlyFocusedComponent();
    return focus == &arrangement.headers ||
           (dynamic_cast<const MixerStrip *>(focus) != nullptr && mixerView.isParentOf(focus));
}
bool AdiRootComponent::trackGroupingContext() const {
    return !noteEditorHasContext() && !devices.hasKeyboardFocus(true) && !mixViewPanel.stack.hasKeyboardFocus(true) &&
           !mixViewPanel.hasAnalyzerControlFocus() && !mixViewPanel.hasChannelControlFocus() &&
           dynamic_cast<const juce::TextEditor *>(juce::Component::getCurrentlyFocusedComponent()) == nullptr;
}
bool AdiRootComponent::trackClipboard(ClipCommand command) {
    using track_edits_detail::children;
    using track_edits_detail::op;
    using track_edits_detail::rank;
    auto &board = commands_.clipClipboard();
    auto &store = submitter_.store();
    auto &sel = arrangement.geometry.selection;
    const auto current = view_.current();
    if (!current || !trackClipboardContext())
        return fail("Select a track by its title to use the track clipboard.");
    const auto *selected = current->findTrack(sel.track);
    try {
        AppCommands::TrackCopy copy;
        if (command == ClipCommand::Paste) {
            const auto *held = std::get_if<AppCommands::TrackCopy>(&board.held);
            if (held == nullptr || held->sourceFile != projectFile(store))
                return fail("Copy the track in this project before pasting it.");
            copy = *held;
        } else {
            if (selected == nullptr || selected->kind == "master")
                return fail("Select a track other than Main.");
            std::string error;
            if (beforeDeleteTrack && !beforeDeleteTrack(error))
                return fail(std::move(error));
            copy = {projectFile(store), selected->kind, selected->name, selected->parent, selected->indexInParent,
                    detail::captureTrackForCopy(store.db(), selected->id)};
            for (const auto &row : copy.contents.at("rows"))
                if (row.at("table") == "tracks" && row.at("before").at("kind") == "master")
                    return fail("The Main track cannot be copied.");
            if (command == ClipCommand::Cut || command == ClipCommand::Copy) {
                if (command == ClipCommand::Cut) {
                    if (!submitEdit("track.delete", {{"id", selected->id}}, "Cut Track"))
                        return false;
                    sel.track = sel.toTrack = 0;
                    sel.picked.clear();
                    publishSelection();
                }
                board.held = std::move(copy);
                error_.clear();
                dirty_.mark(DirtySet::Status);
                commands_.manager.commandStatusChanged();
                return true;
            }
        }

        // The duplicate goes immediately under its source. Without a paste
        // destination, reuse the copied track's original position if possible.
        const bool hasAnchor = selected != nullptr && selected->kind != "master";
        const auto sourceParent = current->findTrack(copy.parent) ? copy.parent : 0;
        const auto parent = hasAnchor ? selected->parent : sourceParent;
        const auto order = children(current->tracks, parent);
        const int newRank = copy.kind == "return" ? 1 : 0;
        const auto end = std::find_if(order.begin(), order.end(), [&](const auto *sibling) { return rank(*sibling) > newRank; });
        auto place = end;
        if (hasAnchor) {
            if (const auto anchor = std::find(order.begin(), end, selected);
                anchor != end && rank(*selected) == newRank)
                place = std::next(anchor);
        } else {
            place = std::find_if(order.begin(), end, [&](const auto *sibling) {
                return rank(*sibling) == newRank && sibling->indexInParent >= copy.index;
            });
        }
        const auto index = static_cast<std::int64_t>(place - order.begin());
        auto contents = detail::copyTrackContents(store.db(), copy.contents, sourceParent, index,
                                                  retainedDeviceId ? retainedDeviceId() : 0);
        const auto id = contents.at("root").get<std::int64_t>();
        const auto label = command == ClipCommand::Duplicate ? "Duplicate Track" : "Paste Track";
        std::vector<OpRequest> requests;
        requests.push_back(op("track.create", {{"id", id}, {"kind", copy.kind}, {"name", copy.name},
                                               {"contents", std::move(contents)}}, label));
        // Moving a pasted track between groups follows the standard automatic
        // Main route and grouped-send rules.
        if (parent != sourceParent)
            requests.push_back(op("track.setParent", {{"id", id}, {"parent", track_edits_detail::parentValue(parent)}}, label));
        for (std::size_t i = 0; i < order.size(); ++i) {
            const auto next = static_cast<std::int64_t>(i) + (static_cast<std::int64_t>(i) >= index ? 1 : 0);
            if (order[i]->indexInParent != next)
                requests.push_back(op("track.reorder", {{"id", order[i]->id}, {"index", next}}, label));
        }
        if (!submitEdits(std::move(requests)))
            return false;
        sel.track = id;
        sel.toTrack = 0;
        sel.picked.clear();
        arrangement.revealTrack = id;
        arrangement.changed();
        return true;
    } catch (const std::exception &e) {
        return fail(e.what());
    }
}
bool AdiRootComponent::deleteAutomationRange() {
    const auto lanes = arrangement.canvas.automationRangeLanes();
    if (!lanes)
        return fail("Select the automation lanes again. A selected control is unavailable.");
    const auto &sel = arrangement.geometry.selection;
    const auto from = std::min(sel.start, sel.end), to = std::max(sel.start, sel.end);
    if (from == to)
        return fail("Select a time range in the automation lane first.");
    auto prepared = edit::prepareDeleteAutomationRange(submitter_.store(), *lanes, from, to, editRate());
    if (!prepared.requests.empty())
        prepared.requests.front().label = "Delete Automation Range";
    return commitEdit(std::move(prepared));
}
bool AdiRootComponent::automationClipboard(ClipCommand command) {
    using EnvelopeCopy = AppCommands::ClipClipboard::EnvelopeCopy;
    const auto lanes = arrangement.canvas.automationRangeLanes();
    if (!lanes)
        return fail("Select the automation lanes again. A selected control is unavailable.");
    const auto &sel = arrangement.geometry.selection;
    const auto from = std::min(sel.start, sel.end), to = std::max(sel.start, sel.end);
    auto &store = submitter_.store();
    auto &board = commands_.clipClipboard();
    const auto rate = editRate();
    const auto paste = [&](const edit::AutomationRangeCopy &copy, std::int64_t at, const char *label) {
        if (at < 0 || copy.ticks <= 0 || copy.ticks > std::numeric_limits<std::int64_t>::max() - at)
            return fail("The automation paste is outside the supported timeline.");
        auto prepared = edit::preparePasteAutomationRange(store, copy, *lanes, at, rate);
        if (!prepared.requests.empty())
            prepared.requests.front().label = label;
        if (!commitEdit(std::move(prepared)))
            return false;
        arrangement.canvas.selectAutomationSpan(at, at + copy.ticks);
        return true;
    };
    if (command == ClipCommand::Paste) {
        const auto *copy = std::get_if<EnvelopeCopy>(&board.held);
        if (!copy)
            return fail("Copy a time range in an automation lane first.");
        if (copy->sourceFile != projectFile(store))
            return fail("The clipboard holds automation from another project. Copy it again in this one.");
        return paste(copy->range, from != to ? from : sel.insert, "Paste Automation Range");
    }
    if (from == to)
        return fail("Select a time range in the automation lane first.");
    auto copy = edit::copyAutomationRange(store, *lanes, from, to, rate);
    if (!copy.ok)
        return fail(copy.error);
    if (command == ClipCommand::Duplicate)
        return paste(copy, to, "Duplicate Automation Range");
    if (command == ClipCommand::Cut) {
        auto prepared = edit::prepareDeleteAutomationRange(store, *lanes, from, to, rate);
        if (!prepared.requests.empty())
            prepared.requests.front().label = "Cut Automation Range";
        if (!commitEdit(std::move(prepared)))
            return false;
    }
    board.held = EnvelopeCopy{std::move(copy), projectFile(store)};
    error_.clear();
    dirty_.mark(DirtySet::Status);
    commands_.manager.commandStatusChanged();
    return true;
}
bool AdiRootComponent::clipClipboard(ClipCommand command) {
    if (arrangement.canvas.hasAutomationSelection())
        return automationClipboard(command);
    const auto current = view_.current();
    if (!current)
        return fail("The clipboard needs an open project.");
    auto &sel = arrangement.geometry.selection;
    auto &board = commands_.clipClipboard();
    if (command == ClipCommand::Paste && std::holds_alternative<AppCommands::TrackCopy>(board.held))
        return trackClipboard(command);
    if (command != ClipCommand::Paste && sel.track != 0 && trackClipboardContext())
        return trackClipboard(command);
    auto &store = submitter_.store();
    const auto rate = editRate();
    const edit_plan::Options options{rate, edit_plan::capturePolicy(commandSettings())};
    const auto from = std::min(sel.start, sel.end), to = std::max(sel.start, sel.end);
    const auto chosen = chosenClip(*current, sel);
    const bool range = chosen.clip == nullptr && from != to;
    clip_edit::RangeSelection selection{arrangement.geometry.selectedTracks(reader()), {0, from}, {0, to}};
    // The time selection's tracks the lanes show that can hold clips, in the lanes' order: the
    // same rule Paste puts a range back by, so a range returns onto the tracks it came from.
    selection.tracks.erase(std::remove_if(selection.tracks.begin(), selection.tracks.end(),
                                          [&](std::int64_t id) {
                                              const auto *t = reader().findTrack(id);
                                              return t == nullptr || !arrangement.geometry.shown(id) || !holdsClips(*t);
                                          }),
                           selection.tracks.end());
    if (command != ClipCommand::Paste && sel.pickedClips.empty() && chosen.clip == nullptr &&
        (!range || selection.tracks.empty()))
        return fail("Select a device, a clip, or a time selection on tracks with clips, first.");
    // A clip chosen after an edit: the new one, at its span in the refreshed snapshot.
    const auto chooseClip = [&](std::int64_t id) {
        if (const auto after = view_.current())
            for (const auto &t : after->tracks)
                if (t)
                    for (const auto &c : t->clips)
                        if (c && c->id == id) {
                            sel.track = t->id;
                            sel.toTrack = 0;
                            sel.clip = id;
                            sel.pickedClips.clear();
                            sel.insert = sel.start = c->posTicks;
                            sel.end = c->posTicks + c->lengthTicks;
                            sel.marked = true;
                        }
    };
    const auto chooseClips = [&](const std::vector<std::int64_t> &ids) {
        chooseClip(ids.front());
        sel.picked.clear();
        sel.pickedClips.assign(std::next(ids.begin()), ids.end());
    };
    const auto timeSelection = [&](std::int64_t first, std::int64_t last, std::int64_t a, std::int64_t b) {
        sel.track = first;
        sel.toTrack = last == first ? 0 : last;
        sel.clip = 0;
        sel.pickedClips.clear();
        sel.insert = sel.start = a;
        sel.end = b;
        sel.marked = true;
    };
    // A command that worked clears an earlier refusal: an empty edit commits nothing, so nothing
    // else would.
    const auto succeeded = [&] {
        error_.clear();
        dirty_.mark(DirtySet::Status);
        arrangement.changed();
    };
    if (command != ClipCommand::Paste && !sel.pickedClips.empty()) {
        const auto ids = arrangement.geometry.selectedClips();
        std::vector<std::int64_t> lanes;
        std::vector<ClipPlace> places;
        std::size_t firstLane = 0, lastLane = 0;
        auto begin = std::numeric_limits<std::int64_t>::max();
        std::int64_t end = 0;
        for (const auto &row : arrangement.geometry.rows(reader())) {
            const auto *track = row.track;
            if (!arrangement.geometry.shown(track->id) || !holdsClips(*track))
                continue;
            const auto lane = lanes.size();
            lanes.push_back(track->id);
            for (const auto &clip : track->clips) {
                if (!clip || !arrangement.geometry.clipSelected(clip->id))
                    continue;
                if (places.empty())
                    firstLane = lane;
                lastLane = lane;
                begin = std::min(begin, clip->posTicks);
                end = std::max(end, clip->posTicks + clip->lengthTicks);
                places.push_back({clip->id, track->id, clip->posTicks});
            }
        }
        if (places.size() != ids.size() || end <= begin)
            return fail("The selected clips are no longer in the Arrangement.");

        if (command == ClipCommand::Duplicate) {
            const auto span = end - begin;
            if (end > std::numeric_limits<std::int64_t>::max() - span)
                return fail("The duplicates would extend past the end of the timeline.");
            for (auto &place : places)
                place.position += span;
            const auto copies = copyClips(places, "Duplicate Clips");
            if (copies.empty())
                return false;
            chooseClips(copies);
        } else {
            clip_edit::RangeSelection group{{}, {0, begin}, {0, end}};
            for (auto lane = firstLane; lane <= lastLane; ++lane)
                group.tracks.push_back(lanes[lane]);
            if (command == ClipCommand::Cut) {
                auto cut = clip_edit::prepareCutClips(store, group, ids, options);
                if (!cut.copy.ok)
                    return fail(cut.copy.error);
                if (!commitEdit(std::move(cut.edit)))
                    return false;
                board.held = std::move(cut.copy);
                timeSelection(group.tracks.front(), group.tracks.front(), begin, begin);
            } else {
                auto copy = clip_edit::copySelectedClips(store, group, ids, options);
                if (!copy.ok)
                    return fail(copy.error);
                board.held = std::move(copy);
            }
            commands_.manager.commandStatusChanged();
        }
        succeeded();
        return true;
    }
    switch (command) {
    case ClipCommand::Copy: {
        if (chosen.clip != nullptr) {
            auto copy = clip_edit::copyClip(store, chosen.clip->id, options);
            if (!copy.ok)
                return fail(copy.error);
            board.held = std::move(copy);
        } else {
            auto copy = clip_edit::copyRange(store, selection, options);
            if (!copy.ok)
                return fail(copy.error);
            board.held = std::move(copy);
        }
        // Copy writes nothing, so nothing else clears an earlier refusal from the Status Bar.
        error_.clear();
        dirty_.mark(DirtySet::Status);
        commands_.manager.commandStatusChanged();
        return true;
    }
    case ClipCommand::Cut: {
        // The copy is kept only once the cut is in, so a refused Cut leaves the clipboard as it was.
        if (chosen.clip != nullptr) {
            auto cut = clip_edit::prepareCutClip(store, chosen.clip->id, options);
            if (!cut.copy.ok)
                return fail(cut.copy.error);
            if (!commitEdit(std::move(cut.edit)))
                return false;
            board.held = std::move(cut.copy);
            timeSelection(chosen.track->id, chosen.track->id, from, from);
        } else {
            auto cut = clip_edit::prepareCutRange(store, selection, options);
            if (!cut.copy.ok)
                return fail(cut.copy.error);
            if (!commitEdit(std::move(cut.edit)))
                return false;
            board.held = std::move(cut.copy);
        }
        commands_.manager.commandStatusChanged();
        succeeded();
        return true;
    }
    case ClipCommand::Paste: {
        if (std::holds_alternative<AppCommands::ClipClipboard::EnvelopeCopy>(board.held))
            return fail("Select an automation lane to paste its envelope.");
        if (board.empty())
            return fail("Copy a clip or a time selection first.");
        // At the insert marker, or a time selection's start.
        const auto at = from != to ? from : sel.marked ? sel.insert : 0;
        // A copy from another project: the engine refuses it, so say so before reading its tracks.
        const auto heldFile = board.sourceFile();
        if (!heldFile.empty() && heldFile != projectFile(store))
            return fail("The clipboard holds a copy from another project: copy again in this one.");
        const auto origin = pasteOrigin();
        const auto *track = reader().findTrack(origin);
        if (track == nullptr || !holdsClips(*track))
            return fail("Select an audio or MIDI track to paste on.");
        if (const auto *copy = std::get_if<clip_edit::ClipCopy>(&board.held)) {
            // Audio on audio tracks, MIDI on MIDI tracks, as Live places them.
            if (const auto kind = imageKind(copy->image); !kind.empty() && kind != clipTrackKind(*track))
                return fail("The copied clip goes on " + kindWord(kind) + " track: " + shownName(arrangement.geometry, reader(), track->id) + " is " +
                            kindWord(clipTrackKind(*track)) + " track.");
            // A new identity, never the copied clip's own: after a Cut of the newest clip, the
            // project's next free id is the one the copy still carries.
            auto id = audio_drop_detail::newClipId(*current);
            if (copy->image.contains("id") && copy->image["id"].is_number_integer())
                id = std::max(id, copy->image["id"].get<std::int64_t>() + 1);
            if (!commitEdit(clip_edit::preparePaste(store, *copy, id, track->id, {0, at}, options)))
                return false;
            chooseClip(id);
        } else {
            // A Time copy's material goes back onto the tracks it came from, at the paste point,
            // without inserting time (Paste Time inserts it). Its track list is the whole Set's,
            // Main and the returns too, so it is not laid out from the selected track.
            if (const auto *time = std::get_if<clip_edit::TimeCopy>(&board.held)) {
                if (!commitEdit(clip_edit::preparePasteRange(store, time->material, time->material.selection.tracks,
                                                             {0, at}, options)))
                    return false;
                timeSelection(origin, origin, at, at + time->ticks);
                succeeded();
                return true;
            }
            const auto &rangeCopy = std::get<clip_edit::RangeCopy>(board.held);
            // As many tracks as the copy came from, in order, from the selected one: the shown
            // tracks that can hold clips.
            const auto wanted = rangeCopy.selection.tracks.size();
            std::vector<std::int64_t> targets;
            bool reached = false;
            for (const auto row : arrangement.geometry.rows(reader())) {
                reached = reached || row.track->id == origin;
                if (reached && targets.size() < wanted && arrangement.geometry.shown(row.track->id) &&
                    holdsClips(*row.track))
                    targets.push_back(row.track->id);
            }
            if (targets.size() < wanted)
                return fail("The copy reaches " + std::to_string(wanted) + " tracks: select a track with " +
                            std::to_string(wanted - 1) + (wanted == 2 ? " more" : " more tracks") +
                            " below it that can hold clips.");
            // Each source track's material onto a track of its kind: the source's kind from the
            // project, else from its copied clips.
            for (std::size_t i = 0; i < wanted; ++i) {
                if (!rangeCopy.clearsRange &&
                    std::none_of(rangeCopy.images.begin(), rangeCopy.images.end(), [i, wanted](const auto &image) {
                        return image.value("trackIndex", wanted) == i;
                    }))
                    continue;
                const auto source = rangeCopy.selection.tracks[i];
                std::string kind;
                if (const auto *t = reader().findTrack(source))
                    kind = clipTrackKind(*t);
                for (const auto &image : rangeCopy.images)
                    if (kind.empty() && image.contains("row") && image["row"].value("track_id", std::int64_t{0}) == source)
                        kind = imageKind(image);
                const auto *target = reader().findTrack(targets[i]);
                if (!kind.empty() && target != nullptr && clipTrackKind(*target) != kind)
                    return fail("The copy's track " + std::to_string(i + 1) + " goes on " + kindWord(kind) +
                                " track: " + shownName(arrangement.geometry, reader(), target->id) + " is " +
                                kindWord(clipTrackKind(*target)) + " track.");
            }
            auto paste = clip_edit::preparePasteRange(store, rangeCopy, targets, {0, at}, options);
            std::vector<std::int64_t> pastedClips;
            if (paste.ok && !rangeCopy.clearsRange) {
                for (const auto &effect : edit_plan::describe(paste).effects)
                    if (effect.kind == edit_plan::EffectKind::Created)
                        for (const auto &clip : effect.after)
                            pastedClips.push_back(clip.id);
                if (pastedClips.empty() || pastedClips.size() != rangeCopy.images.size())
                    return fail("The paste did not identify every copied clip.");
            }
            if (!commitEdit(std::move(paste)))
                return false;
            if (rangeCopy.clearsRange) {
                const auto length = rangeCopy.selection.end.value - rangeCopy.selection.begin.value;
                timeSelection(targets.front(), targets.back(), at, at + length);
            } else {
                chooseClips(pastedClips);
            }
        }
        succeeded();
        return true;
    }
    case ClipCommand::Duplicate: {
        // Right after the selection, which then moves onto the copy (manual 6.9).
        if (chosen.clip != nullptr) {
            const auto id = audio_drop_detail::newClipId(*current);
            if (!commitEdit(clip_edit::prepareDuplicate(store, chosen.clip->id, id, chosen.track->id,
                                                        {0, chosen.clip->posTicks + chosen.clip->lengthTicks}, options)))
                return false;
            chooseClip(id);
        } else {
            if (!commitEdit(clip_edit::prepareDuplicateRange(store, selection, options)))
                return false;
            const auto length = to - from;
            sel.insert = sel.start = to;
            sel.end = to + length;
            sel.clip = 0;
        }
        succeeded();
        return true;
    }
    }
    return false;
}
namespace {
FadeEdit createFadesFor(const engine::Snapshot &current, const Selection &sel, const std::vector<std::int64_t> &tracks) {
    return createFades(current.tracks, tracks, sel.start, sel.end);
}
} // namespace
bool AdiRootComponent::selectionCrossfades() const {
    const auto current = view_.current();
    if (!current)
        return false;
    const auto &sel = arrangement.geometry.selection;
    return createFadesFor(*current, sel, arrangement.geometry.selectedTracks(reader())).crossfade;
}
bool AdiRootComponent::createFades() {
    const auto current = view_.current();
    if (!current)
        return fail("Fades need an open project.");
    const auto &sel = arrangement.geometry.selection;
    auto edit = createFadesFor(*current, sel, arrangement.geometry.selectedTracks(reader()));
    if (!edit.refusal.empty())
        return fail(edit.refusal);
    if (!submitEdits(std::move(edit.ops)))
        return false;
    arrangement.changed();
    return true;
}
bool AdiRootComponent::deleteFades() {
    const auto current = view_.current();
    if (!current)
        return fail("Fades need an open project.");
    const auto &sel = arrangement.geometry.selection;
    // The chosen clip and those picked with it; else the audio clips the time selection holds.
    std::vector<std::int64_t> clips;
    if (sel.clip != 0)
        clips.push_back(sel.clip);
    clips.insert(clips.end(), sel.pickedClips.begin(), sel.pickedClips.end());
    if (clips.empty() && sel.start != sel.end) {
        const auto from = std::min(sel.start, sel.end), to = std::max(sel.start, sel.end);
        for (const auto id : arrangement.geometry.selectedTracks(reader()))
            if (const auto *t = reader().findTrack(id))
                for (const auto &c : t->clips)
                    if (c && c->posTicks < to && c->posTicks + c->lengthTicks > from)
                        clips.push_back(c->id);
    }
    if (clips.empty())
        return fail("Select the clips whose fades to delete.");
    auto edit = ui::deleteFades(current->tracks, clips);
    if (!edit.refusal.empty())
        return fail(edit.refusal);
    if (!submitEdits(std::move(edit.ops)))
        return false;
    arrangement.changed();
    return true;
}
bool AdiRootComponent::canTimeCommand(int id) const {
    if (!view_.current())
        return false;
    const auto &sel = arrangement.geometry.selection;
    if (id == AppCommands::PasteTime) {
        const auto &held = commands_.clipClipboard().held;
        const auto *range = std::get_if<clip_edit::RangeCopy>(&held);
        return (std::holds_alternative<clip_edit::TimeCopy>(held) || (range && range->clearsRange)) &&
               (sel.marked || sel.start != sel.end);
    }
    if (id == AppCommands::InsertSilence && sel.marked && promptInsertSilence)
        return true;
    return sel.start != sel.end;
}
bool AdiRootComponent::insertSilence(std::int64_t at, std::int64_t length) {
    if (at < 0 || length <= 0 || at > std::numeric_limits<std::int64_t>::max() - length)
        return fail("Enter a positive time in bars, beats and sixteenths.");
    if (!commitEdit(clip_edit::prepareInsertSilence(submitter_.store(), at, length, editRate())))
        return false;
    auto &sel = arrangement.geometry.selection;
    sel.clip = 0;
    sel.pickedClips.clear();
    sel.insert = sel.start = at;
    sel.end = at + length;
    sel.marked = true;
    error_.clear();
    dirty_.mark(DirtySet::Status);
    commands_.manager.commandStatusChanged();
    arrangement.changed();
    return true;
}
bool AdiRootComponent::timeCommand(int id) {
    const auto current = view_.current();
    if (!current)
        return fail("The …Time commands need an open project.");
    auto &sel = arrangement.geometry.selection;
    auto &board = commands_.clipClipboard();
    auto &store = submitter_.store();
    const auto rate = editRate();
    const auto from = std::max<std::int64_t>(0, std::min(sel.start, sel.end)), to = std::max(sel.start, sel.end);
    const auto length = to - from;
    if (id == AppCommands::InsertSilence && length == 0 && sel.marked && promptInsertSilence) {
        promptInsertSilence([self = SafePointer<AdiRootComponent>(this), at = sel.insert](auto answer) {
            if (self == nullptr || !answer)
                return;
            const auto snapshot = self->current();
            if (!snapshot)
                return;
            const auto duration = parseLength(*answer);
            if (!duration) {
                self->fail("Enter a positive time in bars, beats and sixteenths, such as 1.0.0.");
                return;
            }
            self->insertSilence(at, lengthTicks(at, *duration, snapshot->meters));
        });
        return true;
    }
    if (id != AppCommands::PasteTime && length <= 0)
        return fail("Select the time to act on first: drag across the lanes.");
    // The selection after an edit: a span of time on the tracks it was on, or the marker.
    const auto select = [&](std::int64_t a, std::int64_t b) {
        sel.clip = 0;
        sel.pickedClips.clear();
        sel.insert = sel.start = a;
        sel.end = b;
        sel.marked = true;
    };
    const auto done = [&] {
        error_.clear();
        dirty_.mark(DirtySet::Status);
        commands_.manager.commandStatusChanged();
        arrangement.changed();
        return true;
    };
    switch (id) {
    case AppCommands::CopyTime: {
        auto copy = clip_edit::copyTime(store, from, to, rate);
        if (!copy.ok)
            return fail(copy.error);
        board.held = std::move(copy);
        return done();
    }
    case AppCommands::CutTime: {
        // The copy is kept only once the cut is in.
        auto cut = clip_edit::prepareCutTime(store, from, to, rate);
        if (!cut.copy.ok)
            return fail(cut.copy.error);
        if (!commitEdit(std::move(cut.edit)))
            return false;
        board.held = std::move(cut.copy);
        select(from, from);
        return done();
    }
    case AppCommands::PasteTime: {
        // At the insert marker, or the selection's start: the copy's time is inserted there.
        const auto at = from != to ? from : sel.marked ? sel.insert : 0;
        if (board.empty())
            return fail("Copy Time first (Ctrl/Cmd+Shift+C).");
        if (const auto file = board.sourceFile(); !file.empty() && file != projectFile(store))
            return fail("The clipboard holds a copy from another project: copy again in this one.");
        if (const auto *time = std::get_if<clip_edit::TimeCopy>(&board.held)) {
            // The engine takes a Time copy only at the rate it was copied at.
            if (time->referenceRate != rate)
                return fail("The sample rate changed since the time was copied: copy it again (Ctrl/Cmd+Shift+C).");
            if (!commitEdit(clip_edit::preparePasteTime(store, *time, at, rate)))
                return false;
            select(at, at + time->ticks);
            return done();
        }
        if (const auto *range = std::get_if<clip_edit::RangeCopy>(&board.held); range && range->clearsRange) {
            // A time range's material, with its length of time inserted across the whole Set first
            // (every track, the locators, the loop); the material lands on the tracks it came from.
            if (!commitEdit(clip_edit::preparePasteRange(store, *range, range->selection.tracks, {0, at}, rate, true)))
                return false;
            select(at, at + (range->selection.end.value - range->selection.begin.value));
            return done();
        }
        return fail("A clip's copy has no time to paste: use Paste (Ctrl/Cmd+V), or Copy Time.");
    }
    case AppCommands::DuplicateTime: {
        if (!commitEdit(clip_edit::prepareDuplicateTime(store, from, to, rate)))
            return false;
        select(to, to + length);
        return done();
    }
    case AppCommands::DeleteTime: {
        if (!commitEdit(clip_edit::prepareDeleteTime(store, from, to, rate)))
            return false;
        select(from, from);
        return done();
    }
    case AppCommands::InsertSilence:
        // The selection's length at its start (manual 6.11); the silence stays selected.
        return insertSilence(from, length);
    default:
        return false;
    }
}
bool AdiRootComponent::canReverseAudio() const {
    const auto current = view_.current();
    return current && !reverseTargets(*current, arrangement.geometry, reader()).clips.empty();
}
bool AdiRootComponent::reverseAudio() {
    const auto current = view_.current();
    if (!current)
        return fail("Reverse needs an open project.");
    const auto targets = reverseTargets(*current, arrangement.geometry, reader());
    return commitEdit(clip_edit::prepareReverse(submitter_.store(), targets.clips, targets.range, editRate()));
}
bool AdiRootComponent::splitAtMarker() {
    const auto current = view_.current();
    if (!current)
        return fail("Splitting needs an open project.");
    const auto &geometry = arrangement.geometry;
    auto &sel = arrangement.geometry.selection;
    const auto from = std::min(sel.start, sel.end), to = std::max(sel.start, sel.end), marker = sel.insert;
    // Clicked clips are cut at the marker. The anchor carries its own span as the selection.
    const bool clipsChosen = !sel.pickedClips.empty() || chosenClip(*current, sel).clip != nullptr;
    const bool timeSelection = from != to && !clipsChosen;
    if (!timeSelection && !sel.marked)
        return fail("Place the insert marker inside a clip to split it.");
    // Each clip and the points it is cut at, strictly inside it (Live 12 manual 6.9): a time
    // selection cuts every clip on its tracks at its edges, one clip at both of them when the
    // selection lies inside it (the two-point split); else the marker cuts the chosen clips, or with
    // no clips chosen the clips under it on the selected tracks, or on every track when
    // `editing.splitAllWithNoSelection` is on.
    std::vector<clip_edit::ClipSplit> cuts;
    std::vector<std::string> cutTracks;
    const auto inside = [](const engine::ClipNode &c, std::int64_t at) {
        return at > c.posTicks && at < c.posTicks + c.lengthTicks;
    };
    const auto selected = geometry.selectedTracks(reader());
    const bool everyTrack = !timeSelection && !clipsChosen && commands_.splitAllWithNoSelection();
    for (const auto &t : current->tracks) {
        if (!t)
            continue;
        // Every track the lanes show: not a track the project view leaves out, the returns while
        // hidden, or the Reference track while hidden (a folded group's tracks are its own, and split
        // with it).
        const bool onTrack = everyTrack ? geometry.inView(t->id)
                                        : std::find(selected.begin(), selected.end(), t->id) != selected.end();
        const auto trackName = shownName(geometry, reader(), t->id); // as its header (B13)
        for (const auto &c : t->clips) {
            if (!c)
                continue;
            if (timeSelection && onTrack) {
                const bool cutStart = inside(*c, from);
                const bool cutEnd = inside(*c, to);
                if (!cutStart && !cutEnd)
                    continue;
                cuts.push_back({c->id, {0, cutStart ? from : to}, {}});
                if (cutStart && cutEnd)
                    cuts.back().second = clip_edit::TimePoint{0, to};
            } else if (!timeSelection && (clipsChosen ? geometry.clipSelected(c->id) : onTrack) && inside(*c, marker)) {
                cuts.push_back({c->id, {0, marker}, {}});
            } else {
                continue;
            }
            cutTracks.push_back(trackName);
        }
    }
    if (cuts.empty())
        return fail(timeSelection ? "No clip crosses the time selection's edges: there is nothing to split."
                                  : "Place the insert marker inside a clip to split it.");
    auto prepared = clip_edit::prepareSplits(submitter_.store(), cuts, editRate());
    const auto done = prepared.edit.requests.size();
    const bool anchorSplit = clipsChosen && std::ranges::any_of(prepared.edit.requests, [&](const auto &request) {
        return request.targetId == sel.clip;
    });
    const auto &refused = prepared.refused;
    std::string refusal;
    if (!refused.empty()) {
        const auto first = std::find_if(cuts.begin(), cuts.end(), [&](const auto &cut) {
            return cut.clipId == refused.front().clipId;
        });
        const auto index = static_cast<std::size_t>(std::distance(cuts.begin(), first));
        refusal = "The clip on " + cutTracks.at(index) + " could not be split. " + refused.front().error;
        if (refused.size() > 1)
            refusal += " Other clips skipped: " + std::to_string(refused.size() - 1) + ".";
    }
    if (!prepared.edit.ok && done == 0 && !refusal.empty())
        return fail(std::move(refusal));
    if (!commitEdit(std::move(prepared.edit)))
        return false;

    // Chosen clips keep their identities as the left pieces. A time selection
    // in one track selects the piece cut out when both edges were inside it.
    if (anchorSplit) {
        sel.start = from;
        sel.end = marker;
    } else if (timeSelection) {
        sel.clip = 0;
        if (selected.size() == 1 && selected.front() == sel.track)
            if (const auto after = view_.current())
                if (const auto *t = track_edits_detail::find(after->tracks, sel.track))
                    for (const auto &c : t->clips)
                        if (c && c->posTicks == from && c->posTicks + c->lengthTicks == to)
                            sel.clip = c->id;
    }

    arrangement.changed();
    if (!refusal.empty() || done > 1) {
        auto message = std::to_string(done) + (done == 1 ? " clip split" : " clips split") + " in one undo step.";
        if (!refusal.empty())
            message += " " + refusal;
        say(std::move(message));
    }
    return true;
}
bool AdiRootComponent::edited(const SubmitResult &result) {
    if (!result)
        return fail(result.error);
    arrangement.followPaused = true; // an edit pauses Follow (§6.2)
    if (afterEdit)
        afterEdit();
    error_.clear();
    dirty_.mark(DirtySet::All);
    return true;
}
void AdiRootComponent::say(std::string message) {
    if (onStatusMessage && !message.empty() && message != error_)
        onStatusMessage(message);
    error_ = std::move(message);
    dirty_.mark(DirtySet::Status);
}
bool AdiRootComponent::canRenderTrack(TrackRenderAction action, const engine::TrackNode &track) const {
    if (!trackRenderDocument_ || trackRenderDocument_->store().readOnly() || track.frozenByGroup != 0)
        return false;
    const bool regular = track.kind == "audio" || isMidiTrack(track);
    const bool cached = track.frozen && track.freezeMediaId &&
                        (track.freezeMode == "wet" || track.freezeMode == "group");
    const bool running = trackRenderDocument_->trackRenderRunning();
    switch (action) {
    case TrackRenderAction::Freeze:
        if (!regular || track.frozen || running)
            return false;
        break;
    case TrackRenderAction::Unfreeze:
        if (!regular || !track.frozen)
            return false;
        break;
    case TrackRenderAction::Flatten:
        if (!regular || !cached)
            return false;
        break;
    case TrackRenderAction::BounceToNewTrack:
        if ((!regular && track.kind != "group") || (track.frozen && !cached) || running)
            return false;
        break;
    case TrackRenderAction::BounceInPlace:
        if (!regular || (track.frozen && !cached) || running)
            return false;
        break;
    }
    SQLite::Statement state(trackRenderDocument_->store().db(),
                            "SELECT locked FROM tracks WHERE id=?");
    state.bind(1, track.id);
    if (!state.executeStep() || state.getColumn(0).getInt() != 0)
        return false;
    if (action == TrackRenderAction::BounceInPlace && !track.frozen) {
        SQLite::Statement incoming(trackRenderDocument_->store().db(),
                                   "SELECT 1 FROM routing WHERE dst_kind='track' AND dst_id=? "
                                   "AND kind IN ('main','send') LIMIT 1");
        incoming.bind(1, track.id);
        return !incoming.executeStep();
    }
    return true;
}
void AdiRootComponent::addTrackRenderItems(juce::PopupMenu &menu, std::int64_t id) {
    const auto current = view_.current();
    const auto *track = current ? current->findTrack(id) : nullptr;
    if (track == nullptr)
        return;
    const bool regular = track->kind == "audio" || isMidiTrack(*track);
    if (!regular && track->kind != "group")
        return;
    menu.addSeparator();
    const juce::Component::SafePointer<AdiRootComponent> safe(this);
    const auto add = [&](const char *name, TrackRenderAction action) {
        menu.addItem(name, canRenderTrack(action, *track), false, [safe, action, id] {
            if (safe)
                safe->renderTrack(action, id);
        });
    };
    if (regular) {
        if (track->frozen)
            add("Unfreeze Track", TrackRenderAction::Unfreeze);
        else
            add("Freeze Track", TrackRenderAction::Freeze);
        add("Flatten Track", TrackRenderAction::Flatten);
    }
    add("Bounce to New Track", TrackRenderAction::BounceToNewTrack);
    if (regular)
        add("Bounce Track in Place", TrackRenderAction::BounceInPlace);
}
bool AdiRootComponent::renderTrack(TrackRenderAction action, std::int64_t id) {
    const auto current = view_.current();
    const auto *track = current ? current->findTrack(id) : nullptr;
    if (track == nullptr || !canRenderTrack(action, *track))
        return fail("That track is no longer available for this command.");
    bool ok = false;
    std::string renderStatus;
    switch (action) {
    case TrackRenderAction::Freeze:
        ok = trackRenderDocument_->freezeTrack(id);
        renderStatus = "Freezing track...";
        break;
    case TrackRenderAction::Unfreeze:
        ok = trackRenderDocument_->unfreezeTrack(id);
        break;
    case TrackRenderAction::Flatten:
        ok = trackRenderDocument_->flattenTrack(id);
        break;
    case TrackRenderAction::BounceToNewTrack:
        ok = trackRenderDocument_->bounceTrackToNewTrack(id);
        renderStatus = "Bouncing to a new track...";
        break;
    case TrackRenderAction::BounceInPlace:
        ok = trackRenderDocument_->bounceTrackInPlace(id);
        if (!track->frozen)
            renderStatus = "Bouncing track in place...";
        break;
    }
    if (!ok)
        return fail(trackRenderDocument_->error());
    say(std::move(renderStatus));
    return true;
}
void AdiRootComponent::sayStaleAddMenu() {
    say("This Add menu is out of date. Reopen it and try again.");
}
juce::String AdiRootComponent::selectionText() const {
    // Live's Status Bar: the insert marker with its time, or the time selection.
    // Positions and lengths in the project's meters (#299).
    const auto &s = arrangement.geometry.selection;
    const auto &meters = reader_->meters();
    // With notes, or time, selected in the Note Editor, the Status Bar reads them (manual 3.2).
    if (state_.clipView && clipView.midi())
        if (const auto text = clipView.notes.statusText(meters); text.isNotEmpty())
            return text;
    auto where = [&meters](std::int64_t tick) {
        const auto at = barPosition(tick, meters);
        return juce::String(at.bar) + "." + juce::String(at.beat) + "." + juce::String(at.sixteenth);
    };
    const auto *tempo = reader_->tempo();
    const auto timeAt = [tempo](std::int64_t tick) { return tempo ? tempo->ticksToSeconds(tick) : 0.; };
    const auto seconds = [](double elapsed) {
        const auto t = static_cast<std::int64_t>(elapsed);
        return juce::String(t / 60) + ":" + juce::String(t % 60).paddedLeft('0', 2);
    };
    if (s.start != s.end) {
        const auto a = std::min(s.start, s.end), b = std::max(s.start, s.end);
        const auto length = barSpan(a, b - a, meters);
        return "Time Selection   Start: " + where(a) + "   End: " + where(b) + "   Length: " +
               juce::String(length.bar) + "." + juce::String(length.beat) + "." +
               juce::String(length.sixteenth) + "   (Duration: " + seconds(timeAt(b) - timeAt(a)) + ")";
    }
    return "Insert Mark " + where(s.marked ? s.insert : 0) + " (Time: " +
           seconds(timeAt(s.marked ? s.insert : 0)) + ")";
}
bool AdiRootComponent::recordingHoldsHistory() const {
    return kHoldHistoryUntilArrangementTakeLands && (isRecording() || (landingRecording && landingRecording()));
}
std::string AdiRootComponent::undoTitle(bool redo) const {
    const std::string verb = redo ? "Redo" : "Undo";
    if (recordingHoldsHistory())
        return verb;
    const auto step = redo ? submitter_.nextRedo() : submitter_.nextUndo();
    if (!canUndo(redo))
        return "Nothing to " + verb;
    // A held knob edit is the step Undo takes back: its own label once committed, the verb meanwhile.
    if (!redo && editsHeld && editsHeld())
        return verb;
    return verb + (step && !step->label.empty() ? " " + step->label : "");
}
bool AdiRootComponent::fail(std::string message) {
    say(std::move(message));
    return false;
}
bool AdiRootComponent::toggle(Panel p) {
    const auto previous = state_.panels;
    const auto *g = state_.panels.find(p);
    if (auto refusal = state_.panels.setCollapsed(p, !g->collapsed, getWidth()))
        return fail(refusal.message);
    if (!persist()) {
        state_.panels = previous;
        return fail(error_);
    }
    return true;
}
bool AdiRootComponent::insertTrack(bool midi) {
    // Live inserts after the selected track and selects the new one. The id is the
    // payload's, never SQLite's (ADR-0021), and it comes from the CURRENT cell: an
    // action lands on the project as it is now, not on this window's last frame. In the selected
    // track's group, right after it; the tracks after it move down one, in the same step.
    const auto current = view_.current();
    if (!current)
        return fail("Inserting a track needs an open project.");
    // The new id from one `RowIds` (DF-E16): past every track and every id a remark still holds.
    RowIds ids(submitter_.store().db());
    auto edit = ::adi::ui::insertTrack(current->tracks, arrangement.geometry.selection.track, midi,
                                       &commands_.recordingDefaults(), ids.next("tracks"));
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    // Its own colour in the same step (ADR-0211 d2): the others keep theirs.
    colourNewTrack(current->tracks, edit.ops, [this](std::int64_t t) { return arrangement.colourIndex(t); });
    edit.ops.front().selBefore = session_.advisory();
    const auto result = submitter_.submit(std::move(edit.ops));
    if (!result)
        return fail(result.error);
    arrangement.followPaused = true;
    arrangement.revealTrack = edit.select; // in sight on the next frame, not under the foot
    arrangement.geometry.selection.track = edit.select;
    arrangement.geometry.selection.toTrack = 0;
    arrangement.geometry.selection.picked.clear();
    arrangement.geometry.selection.clearMaterial();
    publishSelection();
    focusIfShowing(arrangement.headers);
    return true;
}
bool AdiRootComponent::insertMidiClips(const std::vector<std::int64_t> &tracks, std::int64_t start, std::int64_t end) {
    // From the CURRENT cell (ADR-0201); the batch is one undo step, and the first new clip is selected.
    const auto current = view_.current();
    if (!current)
        return fail("Inserting a clip needs an open project.");
    auto edit = ::adi::ui::insertMidiClips(*current, tracks, start, end);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
    auto &sel = arrangement.geometry.selection;
    sel.track = edit.select;
    sel.toTrack = 0;
    sel.picked.clear();
    sel.clip = edit.clip;
    sel.pickedClips.clear();
    sel.start = start;
    sel.end = end;
    publishSelection();
    return true;
}
bool AdiRootComponent::insertReturnTrack(std::int64_t group) {
    // From the CURRENT cell (ADR-0201); the ops, the place and the letter are
    // `adi::ui::insertReturnTrack`'s, one undo step.
    const auto current = view_.current();
    if (!current)
        return fail("Inserting a return needs an open project.");
    RowIds ids(submitter_.store().db()); // DF-E16: never an id a remark still holds
    auto edit = ::adi::ui::insertReturnTrack(current->tracks, group, ids.next("tracks"));
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    const auto id = edit.select;
    edit.ops.front().selBefore = session_.advisory();
    colourNewTrack(current->tracks, edit.ops, [this](std::int64_t t) { return arrangement.colourIndex(t); }); // ADR-0211 d2
    // The tracks' sends to it, fully down, in the same step, so a first drag is heard (walk-A A10).
    const std::string stepLabel = edit.ops.front().label;
    for (auto &send : newReturnSends(*current, id, group, stepLabel.c_str()))
        edit.ops.push_back(std::move(send));
    const auto result = submitter_.submit(std::move(edit.ops));
    if (!result)
        return fail(result.error);
    arrangement.followPaused = true;
    if (state_.hideReturns) {
        state_.hideReturns = false;
        persist();
        namesBar.returns.setToggleState(true, juce::dontSendNotification);
    }
    arrangement.geometry.selection.track = id;
    arrangement.geometry.selection.toTrack = 0;
    arrangement.geometry.selection.picked.clear();
    arrangement.geometry.selection.clip = 0;
    arrangement.geometry.selection.pickedClips.clear();
    publishSelection();
    arrangement.applyView();
    return true;
}
bool AdiRootComponent::deleteTrack() {
    const SnapshotReader current(view_.current());
    const auto selected = arrangement.geometry.selectedTracks(current);
    if (selected.empty())
        return fail("Select a track to delete.");
    std::vector<OpRequest> requests;
    for (const auto id : selected) {
        const auto *track = current.findTrack(id);
        if (track == nullptr)
            return fail("A selected track is gone.");
        if (track->kind == "master")
            return fail("The Main track cannot be deleted.");
        // Deleting a group also removes its descendants.
        bool covered = false;
        auto parentId = track->parent;
        for (std::size_t depth = 0; parentId != 0 && depth < current.tracks().size(); ++depth) {
            if (std::find(selected.begin(), selected.end(), parentId) != selected.end()) {
                covered = true;
                break;
            }
            const auto *parent = current.findTrack(parentId);
            parentId = parent != nullptr ? parent->parent : 0;
        }
        if (!covered)
            requests.push_back(track_edits_detail::op("track.delete", {{"id", id}}, "Delete Track"));
    }
    if (requests.empty())
        return false;
    std::string error;
    if (beforeDeleteTrack && !beforeDeleteTrack(error))
        return fail(std::move(error));
    if (!submitEdits(std::move(requests)))
        return false;
    auto &selection = arrangement.geometry.selection;
    selection.track = selection.toTrack = 0;
    selection.picked.clear();
    selection.clearMaterial();
    publishSelection();
    return true;
}
bool AdiRootComponent::dropMidiFiles(const std::vector<std::pair<std::string, std::string>> &files,
                                     std::int64_t track, std::int64_t position, bool separate) {
    const auto current = view_.current();
    if (!current || files.empty())
        return fail("Choose a MIDI file to import.");
    if (track != 0) {
        const auto *destination = current->findTrack(track);
        if (destination == nullptr || !isMidiTrack(*destination))
            return fail("Drop the MIDI file on a MIDI track or below the tracks.");
    }
    const char *label = "Import MIDI File";
    constexpr std::int64_t limit = std::int64_t{1} << 50;
    try {
        auto &store = submitter_.store();
        CommitOptions options;
        options.expectHead.emplace(OpJournal(store).headSeq());
        // Imported clips and any split survivors share the same ID allocator.
        clip_edit::CopyIds ids(store.db());
        RowIds trackIds(store.db());
        std::vector<OpRequest> batch;
        edit_plan::detail::PlacementBatch overlaps;
        const auto existingTrack = track;
        std::int64_t clearUntil = 0;
        auto tracks = current->tracks;
        auto insertAfter = track;
        auto newTrack = [&]() {
            auto made = ::adi::ui::insertTrack(tracks, insertAfter, true, &commands_.recordingDefaults(), trackIds.next("tracks"));
            for (auto &request : made.ops) {
                if (request.opType == "track.create") {
                    auto added = std::make_shared<engine::TrackNode>();
                    added->id = made.select;
                    added->kind = "midi";
                    const auto &parent = request.payload.at("parent");
                    added->parent = parent.is_null() ? 0 : parent.get<std::int64_t>();
                    added->indexInParent = request.payload.at("index").get<std::int64_t>();
                    tracks.push_back(std::move(added));
                } else if (request.opType == "track.reorder") {
                    const auto id = request.payload.at("id").get<std::int64_t>();
                    for (auto &existing : tracks)
                        if (existing != nullptr && existing->id == id) {
                            auto ordered = std::make_shared<engine::TrackNode>(*existing);
                            ordered->indexInParent = request.payload.at("index").get<std::int64_t>();
                            existing = std::move(ordered);
                            break;
                        }
                }
                request.label = label;
                batch.push_back(std::move(request));
            }
            insertAfter = made.select;
            return made.select;
        };
        std::int64_t firstTrack = 0, firstClip = 0, firstLength = 0;
        position = std::max<std::int64_t>(0, position);
        auto at = position;
        for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
            const auto &[path, name] = files[fileIndex];
            const auto midi = interchange::readMidiClipFile(media::pathFromUtf8(path));
            if (!midi.error.empty())
                return fail(midi.error);
            if (track == 0)
                track = newTrack();
            auto on = separate && fileIndex > 0 ? newTrack() : track;
            const auto first = separate ? position : at;
            std::int64_t longest = 0;
            for (std::size_t channelTrack = 0; channelTrack < midi.tracks.size(); ++channelTrack) {
                const auto &source = midi.tracks[channelTrack];
                if (channelTrack != 0)
                    on = newTrack();
                if (first > limit || source.length > limit - first)
                    return fail("The MIDI file extends past the Arrangement time range.");
                const auto clip = ids.next("clips");
                const auto title = midi.tracks.size() == 1 ? name : name + " " + std::to_string(channelTrack + 1);
                batch.push_back(interchange::createImportedMidiClip(ids, source, on, clip, first, title));
                if (existingTrack != 0 && on == existingTrack) {
                    const auto end = first + source.length;
                    overlaps.clear.push_back({{on, std::nullopt}, {first, end}});
                    clearUntil = std::max(clearUntil, end);
                }
                longest = std::max(longest, source.length);
                if (firstClip == 0) {
                    firstTrack = on;
                    firstClip = clip;
                    firstLength = source.length;
                }
            }
            at = first + longest;
        }
        if (!overlaps.clear.empty()) {
            // Replace only the landed spans on the existing main lane. Track
            // automation stays where it was, as for a recording.
            const std::array targets{existingTrack};
            auto scope = edit_plan::materialScope(targets, edit_plan::PlacementClock::Ticks,
                                                   {position, clearUntil});
            scope.domains = edit_plan::Domains::Clips;
            edit_plan::Options placement{editRate(), {}};
            placement.policy.automationFollowsClips = false;
            auto splice = edit_plan::detail::prepareCaptured(store, scope, std::move(overlaps), placement, ids, label);
            if (!splice.ok)
                return fail(splice.error);
            batch.insert(batch.begin(), std::make_move_iterator(splice.requests.begin()),
                         std::make_move_iterator(splice.requests.end()));
        }
        batch.front().selBefore = session_.advisory();
        if (!edited(submitter_.commitPrepared(batch, [&](Store &destination, Actor) {
                return OpJournal(destination).commit(batch, options);
            })))
            return false;
        auto &selection = arrangement.geometry.selection;
        selection.track = firstTrack;
        selection.toTrack = 0;
        selection.clip = firstClip;
        selection.start = position;
        selection.end = position + firstLength;
        selection.picked.clear();
        selection.pickedClips.clear();
        arrangement.revealTrack = firstTrack;
        publishSelection();
        return true;
    } catch (const std::exception &exception) {
        return fail(exception.what());
    }
}
std::int64_t AdiRootComponent::midiClipForExport() const noexcept {
    const auto &selection = arrangement.geometry.selection;
    if (selection.clip == 0 || std::any_of(selection.pickedClips.begin(), selection.pickedClips.end(),
                                          [&](auto clip) { return clip != selection.clip; }))
        return 0;
    const auto current = view_.current();
    if (!current)
        return 0;
    const auto matches = [&](const auto &clip) { return clip != nullptr && !clip->audio && clip->id == selection.clip; };
    for (const auto &track : current->tracks)
        if (track != nullptr && isMidiTrack(*track) &&
            (std::any_of(track->clips.begin(), track->clips.end(), matches) ||
             std::any_of(track->takeClips.begin(), track->takeClips.end(), matches)))
            return selection.clip;
    return 0;
}
bool AdiRootComponent::dropAudio(const std::string &utf8Path, std::int64_t track, std::int64_t pos,
                                 const std::string &name) {
    return dropAudioFiles({{utf8Path, name}}, track, pos, false);
}
bool AdiRootComponent::dropAudioPaths(const juce::StringArray &paths, std::int64_t track, std::int64_t pos,
                                     bool separate) {
    if (paths.isEmpty())
        return false;
    std::vector<std::pair<std::string, std::string>> files;
    // The clip's name is the file's name without its ending. Read through std::filesystem: a path
    // can arrive written with forward slashes (ADI's own Browser hands them so), and on Windows an
    // unchecked juce::File takes such a path whole for the name.
    for (const auto &path : paths) {
        const auto utf8 = path.toStdString();
        files.emplace_back(utf8, media::pathUtf8(media::pathFromUtf8(utf8).stem()));
    }
    return dropAudioFiles(files, track, pos, separate);
}
bool AdiRootComponent::sampleInstrumentTarget(std::int64_t track) const {
    const auto current = view_.current();
    const auto *target = current ? current->findTrack(track) : nullptr;
    return target && isMidiTrack(*target);
}
bool AdiRootComponent::loadSampleAsInstrument(std::int64_t track, const std::filesystem::path &path) {
    if (!sampleInstrumentTarget(track))
        return fail("Choose a MIDI track to load the sample as an instrument.");
    if (importing_ || importPrompt_)
        return fail("Finish the current import before loading another sample.");
    if (!loadSampleInstrument)
        return fail("Loading a sample as an instrument needs the application.");
    std::string why;
    if (!loadSampleInstrument(track, path, why))
        return fail(why.empty() ? "The sample could not be loaded as an instrument." : std::move(why));
    say({});
    return true;
}
bool AdiRootComponent::dropAudioFiles(const std::vector<std::pair<std::string, std::string>> &files,
                                      std::int64_t track, std::int64_t pos, bool separate) {
    // Where it lands is checked before anything is asked or read.
    const auto current = view_.current();
    if (!current)
        return fail("Dropping audio needs an open project.");
    if (const auto *target = current->findTrack(track); target && isMidiTrack(*target)) {
        if (files.size() != 1)
            return fail("Drop one sample at a time to make an instrument.");
        return loadSampleAsInstrument(track, media::pathFromUtf8(files.front().first));
    }
    if (auto refusal = audioDropRefusal(*current, track); !refusal.empty())
        return fail(std::move(refusal));
    if (importing_)
        return fail("The last drop is still being imported.");
    if (importPrompt_)
        return fail("Answer the import prompt first.");
    // ADI's prompt (E5) while `editing.importOptions` asks and the import with choices is wired;
    // else the last choices.
    const auto last = importChoicesFrom(commands_.settings());
    if (importMaterial && importAsks(commands_.settings())) {
        importPrompt_ = std::make_unique<ImportPrompt>(
            files.size(), last,
            [this, files, track, pos, separate](std::optional<ImportChoices> answer, bool stopAsking) {
                // The prompt goes once it is answered (deleted after its own click returns); the drop
                // goes on with its choices.
                if (importPrompt_) {
                    removeChildComponent(importPrompt_.get());
                    juce::MessageManager::callAsync([prompt = std::shared_ptr<ImportPrompt>(importPrompt_.release())] {});
                }
                if (!answer) {
                    say("Nothing imported.");
                    return;
                }
                // The track may have gone while the prompt was open: the drop's own refusal.
                if (!view_.current()) {
                    fail("Dropping audio needs an open project.");
                    return;
                }
                if (auto refusal = audioDropRefusal(*view_.current(), track); !refusal.empty()) {
                    fail(std::move(refusal));
                    return;
                }
                std::string why;
                const bool kept = keepImportChoices(commands_.writableSettings(), *answer, why,
                                                    juce::Time::currentTimeMillis() * 1000, stopAsking);
                dropWithChoices(files, track, pos, separate, *answer);
                if (!kept) {
                    const auto warning = "The import's choices were used but not saved: " + why;
                    if (importing_)
                        *importing_ = warning;
                    else
                        say(warning);
                }
            });
        addAndMakeVisible(*importPrompt_);
        importPrompt_->setBounds(getLocalBounds().withSizeKeepingCentre(380, 184));
        importPrompt_->toFront(true);
        return true;
    }
    return dropWithChoices(files, track, pos, separate, last);
}
bool AdiRootComponent::dropWithChoices(const std::vector<std::pair<std::string, std::string>> &files,
                                       std::int64_t track, std::int64_t pos, bool separate,
                                       const ImportChoices &choices) {
    if (!choices.any() || !importMaterial)
        return dropPlain(files, track, pos, separate);
    if (files.empty())
        return fail("Drop an audio file.");
    importing_.emplace();
    say(files.size() == 1 ? "Importing " + files.front().second + "..."
                          : "Importing " + std::to_string(files.size()) + " audio files...");
    const bool started = importMaterial(files, track, pos, separate, choices, [this](std::int64_t clip,
                                                                                      std::int64_t on,
                                                                                      const std::string &why) {
        finishImport(clip, on, clip == 0 && why.empty() ? "The files were not imported." : why);
    });
    if (!started && importing_)
        finishImport(0, 0, "The last import is still being prepared. Drop the files again when it ends.");
    return true;
}
void AdiRootComponent::finishImport(std::int64_t first, std::int64_t onTrack, const std::string &why) {
    if (!importing_)
        return;
    std::string message = why;
    if (!importing_->empty())
        message += (message.empty() ? "" : " ") + *importing_;
    importing_.reset();
    if (first != 0) {
        // The first clip selected, as a click on it selects it.
        auto &sel = arrangement.geometry.selection;
        sel.track = onTrack;
        sel.toTrack = 0;
        sel.picked.clear();
        sel.clip = first;
        sel.pickedClips.clear();
        if (const auto now = view_.current())
            if (const auto *t = now->findTrack(onTrack))
                for (const auto &c : t->clips)
                    if (c->id == first) {
                        sel.start = c->posTicks;
                        sel.end = c->posTicks + c->lengthTicks;
                    }
        publishSelection();
    }
    if (!message.empty())
        say(message);
    else
        error_.clear();
    dirty_.mark(DirtySet::Status);
}
bool AdiRootComponent::dropPlain(const std::vector<std::pair<std::string, std::string>> &files,
                                 std::int64_t track, std::int64_t pos, bool separate) {
    // The batch is built from the CURRENT cell.
    if (!importAudio)
        return fail("Dropping audio needs an open project.");
    // Capture each path once. Uncommitted pool rows are shared by content across the batch.
    std::vector<DroppedAudio> dropped;
    std::map<std::string, std::int64_t> pendingMedia;
    std::int64_t nextId = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto &[path, name] = files[i];
        DroppedAudio file{name, {}};
        std::size_t seen = 0;
        while (seen < i && files[seen].first != path)
            ++seen;
        if (seen < i) {
            file.import = dropped[seen].import;
            file.import.media = {};
        } else {
            file.import.firstFreeId = nextId;
            std::string why;
            if (!importAudio(path, file.import, why))
                return fail(why.empty() ? "The file could not be imported." : why);
            if (file.import.media.opType == "media.import") {
                const auto &hash = file.import.media.payload.at("row").at("hash_blake3").get_ref<const std::string &>();
                const auto [entry, inserted] = pendingMedia.try_emplace(hash, file.import.mediaId);
                if (!inserted) {
                    file.import.mediaId = entry->second;
                    file.import.media = {};
                }
            }
            if (!file.import.media.opType.empty())
                nextId = std::max(nextId, file.import.mediaId + 1);
        }
        dropped.push_back(std::move(file));
    }
    auto edit = adi::ui::dropAudio(*view_.current(), track, pos, std::move(dropped), separate,
                                   &commands_.recordingDefaults(), &submitter_.store(), editRate());
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (track != 0) {
        const edit_plan::Options options{editRate(), edit_plan::capturePolicy(commandSettings())};
        auto cleared = clip_edit::prepareAudioDropReplacement(submitter_.store(), track,
            {0, std::max<std::int64_t>(0, pos)}, {0, edit.endOnTarget}, options, edit.lastClip);
        if (!cleared.ok)
            return fail(std::move(cleared.error));
        if (!cleared.requests.empty()) {
            for (auto &request : cleared.requests)
                request.label = edit.ops.front().label;
            cleared.requests.insert(cleared.requests.end(), std::make_move_iterator(edit.ops.begin()),
                                    std::make_move_iterator(edit.ops.end()));
            if (!commitEdit(std::move(cleared)))
                return false;
        } else if (!submitEdits(std::move(edit.ops))) {
            return false;
        }
    } else if (!submitEdits(std::move(edit.ops))) {
        return false;
    }
    // The first clip selected, as a click on it selects it: its track, and its time.
    auto &sel = arrangement.geometry.selection;
    sel.track = edit.select;
    sel.toTrack = 0;
    sel.picked.clear();
    sel.clip = edit.clip;
    sel.pickedClips.clear();
    if (const auto now = view_.current())
        if (const auto *t = now->findTrack(edit.select))
            for (const auto &c : t->clips)
                if (c->id == edit.clip) {
                    sel.start = c->posTicks;
                    sel.end = c->posTicks + c->lengthTicks;
                }
    publishSelection();
    return true;
}
bool AdiRootComponent::groupTracks(bool ungroup, std::int64_t group) {
    // From the CURRENT cell, as every action is; the batch is one undo step, labelled as Live's
    // Edit menu names it.
    const auto current = view_.current();
    // Group Tracks takes every selected track (Shift+click on the headers, or a selection across
    // tracks); Ungroup the selected group.
    const auto id = group != 0 ? group : arrangement.geometry.selection.track;
    auto edit = ungroup ? ungroupTrack(current->tracks, id)
                        : ::adi::ui::groupTracks(current->tracks, arrangement.geometry.selectedTracks(reader()),
                                                 RowIds(submitter_.store().db()).next("tracks"));
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    // A new group's own colour in the same step (ADR-0211 d2); ungrouping creates none.
    colourNewTrack(current->tracks, edit.ops, [this](std::int64_t t) { return arrangement.colourIndex(t); });
    edit.ops.front().selBefore = session_.advisory();
    const auto result = submitter_.submit(std::move(edit.ops));
    if (!result)
        return fail(result.error);
    arrangement.followPaused = true;
    arrangement.geometry.selection.track = edit.select;
    arrangement.geometry.selection.toTrack = 0;
    arrangement.geometry.selection.picked.clear();
    arrangement.geometry.selection.clip = 0;
    arrangement.geometry.selection.pickedClips.clear();
    publishSelection();
    return true;
}
bool AdiRootComponent::soloTrack(std::int64_t id, bool add) {
    // From the CURRENT cell (ADR-0201); the batch is one undo step.
    const auto current = view_.current();
    if (!current)
        return fail("Soloing needs an open project.");
    auto edit = soloTracks(current->tracks, id, arrangement.geometry.selectedTracks(reader()), commands_.exclusiveSolo(), add);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    return edit.ops.empty() || submitEdits(std::move(edit.ops));
}
void AdiRootComponent::selectClip(std::int64_t clip) {
    // As a click on it selects it: its track, and its time.
    const auto now = view_.current();
    if (!now)
        return;
    for (const auto &t : now->tracks)
        if (t)
            for (const auto &c : t->clips)
                if (c->id == clip) {
                    auto &sel = arrangement.geometry.selection;
                    sel.track = t->id;
                    sel.toTrack = 0;
                    sel.picked.clear();
                    sel.clip = clip;
                    sel.pickedClips.clear();
                    sel.start = c->posTicks;
                    sel.end = c->posTicks + c->lengthTicks;
                    publishSelection();
                    return;
                }
}
TrackRecordState AdiRootComponent::recordState(std::int64_t track) const {
    return ::adi::ui::recordState(recordStates_, track);
}
bool AdiRootComponent::armTrack(std::int64_t id, bool add) {
    return toggleArm(id, add);
}
bool AdiRootComponent::toggleArm(std::optional<std::int64_t> clicked, bool add) {
    const auto current = view_.current();
    if (!current)
        return fail("Arming needs an open project.");
    const SnapshotReader snapshot(current);
    auto &selection = arrangement.geometry.selection;
    auto selected = arrangement.geometry.selectedTracks(snapshot);
    auto anchor = clicked.value_or(selection.track);
    if (!clicked) {
        const auto eligible = [&](std::int64_t id) {
            const auto *track = snapshot.findTrack(id);
            return track && armable(*track);
        };
        if (std::find(selected.begin(), selected.end(), anchor) == selected.end() || !eligible(anchor)) {
            const auto first = std::find_if(selected.begin(), selected.end(), eligible);
            if (first == selected.end())
                return false;
            anchor = *first;
        }
    }
    const auto states = freshRecordStates();
    auto edit = armTracks(current->tracks, states, anchor, selected, commands_.exclusiveArm(), add);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    const auto implicit = selectionArmedTrack_;
    const auto disarmsImplicit = [implicit](const OpRequest &request) {
        return request.payload.at("id") == implicit && request.payload.at("armed") == false;
    };
    if (implicit != 0 && std::any_of(edit.ops.begin(), edit.ops.end(), disarmsImplicit)) {
        if (!changeSelectionArming(0))
            return false;
        if (!::adi::ui::recordState(freshRecordStates(), implicit).armed)
            std::erase_if(edit.ops, disarmsImplicit);
    }
    if (!submitEdits(std::move(edit.ops)))
        return false;
    // Publish after submission so Undo restores the selection from before the Arm click.
    if (clicked && !::adi::ui::recordState(states, anchor).armed && selection.track != anchor) {
        if (const auto found = std::find(selected.begin(), selected.end(), anchor); found != selected.end())
            selected.erase(found);
        else
            selected.clear();
        selection.track = anchor;
        selection.toTrack = 0;
        selection.picked = std::move(selected);
        selection.clip = 0;
        selection.pickedClips.clear();
        selection.start = selection.end = selection.insert;
        arrangement.selectedLocator = 0;
        arrangement.selectedMeter = -1;
        arrangement.loopSelected = false;
        publishSelection();
    }
    return true;
}
bool AdiRootComponent::setMonitor(std::int64_t id, std::optional<Monitor> mode) {
    const auto current = view_.current();
    if (!current)
        return fail("Monitoring needs an open project.");
    auto edit = ::adi::ui::setMonitor(current->tracks, freshRecordStates(), id, arrangement.geometry.selectedTracks(reader()), mode);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    return edit.ops.empty() || submitEdits(std::move(edit.ops));
}
std::vector<std::int64_t> AdiRootComponent::audioOutputChoices(std::int64_t id) const {
    const auto model = rows::readModel(submitter_.store());
    const auto source = std::ranges::find(model.tracks, id, &rows::Track::id);
    if (source == model.tracks.end())
        return {};
    const auto plan = engine::planGraph(model);
    const auto start = plan.indexOf(id);
    if (!start)
        return {};
    // A destination already feeding this source would close a loop. A return
    // may have a feedback send to itself, which is outside the graph order.
    std::vector<std::vector<std::size_t>> upstream(plan.nodes.size());
    for (const auto &edge : plan.edges)
        if (!edge.feedback && edge.keyDevice == 0)
            upstream[edge.to].push_back(edge.from);
    std::vector<bool> feedsSource(plan.nodes.size(), false);
    std::vector<std::size_t> visit{*start};
    feedsSource[*start] = true;
    while (!visit.empty()) {
        const auto node = visit.back();
        visit.pop_back();
        for (const auto upstreamNode : upstream[node])
            if (!feedsSource[upstreamNode]) {
                feedsSource[upstreamNode] = true;
                visit.push_back(upstreamNode);
            }
    }
    std::vector<std::int64_t> choices;
    for (const auto &target : model.tracks) {
        const auto index = plan.indexOf(target.id);
        if (index && !feedsSource[*index] && audioDestination(*source, target))
            choices.push_back(target.id);
    }
    std::ranges::sort(choices);
    return choices;
}

bool AdiRootComponent::setTrackMidiOutput(std::int64_t id, std::optional<rows::MidiOutput> output) {
    if (output) {
        const auto ports = midiTrackOutputs ? midiTrackOutputs() : std::vector<MidiPortListing>{};
        if (std::ranges::find(ports, output->port, &MidiPortListing::id) == ports.end())
            return fail("That MIDI output is unavailable. Enable its Track switch in Tempo & MIDI Settings.");
    }
    auto& store = submitter_.store();
    struct Prepared {
        std::vector<OpRequest> requests;
        CommitOptions options;
    } prepared;
    try {
        SQLite::Transaction read(store.db(), SQLite::TransactionBehavior::DEFERRED);
        prepared.options.expectHead.emplace(OpJournal(store).headSeq());
        const auto model = rows::readModel(store);
        const auto source = std::ranges::find(model.tracks, id, &rows::Track::id);
        if (source == model.tracks.end() || (source->kind != "midi" && source->kind != "instrument"))
            return fail("Choose a MIDI track to route to hardware.");
        for (const auto& route : model.routing) {
            if (!route.enabled || route.srcId != id || !route.midiOutput) continue;
            if (route.midiOutput == output) return true;
            prepared.requests.push_back(track_edits_detail::op("routing.disconnect", {{"id", route.id}}, "MIDI To"));
        }
        if (output) {
            RowIds ids(store.db());
            prepared.requests.push_back(track_edits_detail::op("routing.connect",
                {{"id", ids.next("routing")}, {"srcKind", "track"}, {"src", id},
                 {"dstKind", "hw_out"}, {"dst", 0}, {"kind", "main"},
                 {"midiPort", output->port}, {"midiChannel", output->channel}}, "MIDI To"));
        }
        read.commit();
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    if (prepared.requests.empty()) return true;
    prepared.requests.front().selBefore = session_.advisory();
    return edited(submitter_.commitPrepared(std::move(prepared), [](Store& target, Prepared& edit, Actor) {
        return OpJournal(target).commit(edit.requests, edit.options);
    }));
}

bool AdiRootComponent::setTrackAudioOutput(std::int64_t id, std::int64_t destination) {
    struct Prepared {
        std::vector<OpRequest> requests;
        CommitOptions options;
    } prepared;
    auto &store = submitter_.store();
    try {
        const auto baseline = [&] {
            SQLite::Transaction read(store.db(), SQLite::TransactionBehavior::DEFERRED);
            prepared.options.expectHead.emplace(OpJournal(store).headSeq());
            const auto model = rows::readModel(store);
            const auto source = std::ranges::find(model.tracks, id, &rows::Track::id);
            const auto target = std::ranges::find(model.tracks, destination, &rows::Track::id);
            if (source == model.tracks.end() || target == model.tracks.end() || !audioDestination(*source, *target))
                throw std::runtime_error("That Audio To destination is unavailable.");
            const auto defaultDestination = defaultAudioDestination(model, *source);
            std::vector<const rows::Routing *> previous;
            for (const auto &route : model.routing)
                if (route.enabled && route.kind == "main" && route.srcKind == "track" &&
                    route.srcId == id && route.dstKind == "track")
                    previous.push_back(&route);

            const bool unchanged = previous.empty() ? destination == defaultDestination
                : previous.size() == 1 && previous.front()->dstId == destination;
            if (unchanged)
                return engine::planGraph(model);
            for (const auto *route : previous)
                prepared.requests.push_back(track_edits_detail::op("routing.disconnect", {{"id", route->id}}, "Audio To"));
            if (destination != defaultDestination) {
                RowIds ids(store.db());
                prepared.requests.push_back(track_edits_detail::op("routing.connect",
                    {{"id", ids.next("routing")}, {"srcKind", "track"}, {"src", id}, {"dstKind", "track"},
                     {"dst", destination}, {"kind", "main"}}, "Audio To"));
            }
            return engine::planGraph(model);
        }();
        if (prepared.requests.empty())
            return true;
        prepared.requests.front().selBefore = session_.advisory();
        prepared.options.beforeCommit = [&](SQLite::Database &, std::int64_t, std::string &error) {
            const auto candidate = rows::readModel(store);
            const auto plan = engine::planGraph(candidate);
            if (plan.cycle) {
                error = "That Audio To destination would create a routing loop.";
                return false;
            }
            for (const auto &problem : plan.problems)
                if (std::ranges::find(baseline.problems, problem) == baseline.problems.end()) {
                    error = problem;
                    return false;
                }
            return !validateAudioRouting || validateAudioRouting(candidate, error);
        };
        return edited(submitter_.commitPrepared(std::move(prepared), [](Store &target, Prepared &edit, Actor) {
            return OpJournal(target).commit(edit.requests, edit.options);
        }));
    } catch (const std::exception &e) {
        return fail(e.what());
    }
}

bool AdiRootComponent::setAudioInput(std::int64_t id, std::string_view ref) {
    const auto current = view_.current();
    const auto *track = current ? current->findTrack(id) : nullptr;
    if (!track || track->kind != "audio")
        return fail("Audio inputs are available on audio tracks.");
    const auto choices = audioInputs();
    if (ref != kResamplingInput &&
        std::none_of(choices.begin(), choices.end(), [&](const auto &choice) { return choice.ref == ref; }))
        return fail("That external input is unavailable. Open Audio Settings to configure inputs.");
    if (track->inputRef && *track->inputRef == ref)
        return true;
    return submitEdit("track.setInput", {{"id", id}, {"input", std::string(ref)}}, "Audio From");
}
bool AdiRootComponent::setMidiInput(std::int64_t id, std::string_view port, int channel) {
    const auto current = view_.current();
    const auto *track = current ? current->findTrack(id) : nullptr;
    if (!track || !isMidiTrack(*track) || channel < 0 || channel > 16)
        return fail("Choose a valid input on a MIDI track.");
    const auto previous = engine::midiInputChoice(track->inputRef ? std::string_view(*track->inputRef) : std::string_view{});
    if (previous.port == port && previous.channel == channel)
        return true;
    if (!port.empty() && port != previous.port && std::ranges::find(midiInputPorts, port, &MidiPortListing::id) == midiInputPorts.end())
        return fail("That MIDI input is unavailable. Check the port's Track switch in MIDI Settings.");
    return submitEdit("track.setInput", {{"id", id}, {"input", engine::midiInputRef({port, channel})}}, "MIDI From");
}
void AdiRootComponent::setRecordPending(bool pending) {
    if (recordPending_ == pending)
        return;
    recordPending_ = pending;
    mark(DirtySet::Transport);
}
bool AdiRootComponent::toggleRecord(bool alternate) {
    if (recordPending_) {
        setRecordPending(false);
        return true;
    }
    if (!startRecording || !stopRecording || !recording)
        return fail("Recording runs in the application, with an audio device.");
    if (isRecording()) {
        stopRecording();
        mark(DirtySet::Transport);
        return true;
    }
    const bool rolling = mailbox_.desiredPlaying();
    if (!rolling && commands_.startPlaybackWithRecord() == alternate) {
        setRecordPending(true);
        return true;
    }
    return beginArrangementRecording(rolling ? timelineTick() : arrangement.geometry.selection.insert, rolling);
}
bool AdiRootComponent::beginArrangementRecording(std::int64_t takeStart, bool rolling) {
    if (!startRecording || !stopRecording || !recording)
        return fail("Recording runs in the application, with an audio device.");
    if (!rolling && !mailbox_.hasRoom())
        return fail("Transport command queue is full");
    const auto insertTick = rolling ? arrangement.geometry.selection.insert : takeStart;
    if (!startRecording(insertTick, rolling, midiOverdub()))
        return false;
    setRecordPending(false);
    // The armed audio tracks' lanes show the take as it grows.
    if (!capturingAudioTake || capturingAudioTake())
        if (const auto *current = reader().get())
            arrangement.beginLiveTake(takeStart, armedTracks(current->tracks, freshRecordStates()).audio);
    // A recording start plays the transport: the same op Play writes, so the history's transport state
    // agrees.
    if (!rolling) {
        OpRequest request;
        request.opType = "transport.play";
        request.payload = Payload::object();
        (void)submitter_.submit(std::move(request));
    }
    mark(DirtySet::Transport);
    return true;
}
ExportChoices AdiRootComponent::exportDefaults() const {
    // R36: without a time selection or an active loop, export covers the Arrangement.
    ExportChoices c;
    const auto &sel = arrangement.geometry.selection;
    const auto current = view_.current();
    if (sel.end != sel.start) {
        // A selection made right to left is the same range.
        c.startTicks = std::min(sel.start, sel.end);
        c.lengthTicks = std::max(sel.start, sel.end) - c.startTicks;
    } else if (current && current->transport.loopEnabled &&
               current->transport.loopEndTicks > current->transport.loopStartTicks) {
        c.startTicks = current->transport.loopStartTicks;
        c.lengthTicks = current->transport.loopEndTicks - current->transport.loopStartTicks;
    } else {
        c.startTicks = 0;
        c.lengthTicks = arrangementEnd();
    }
    if (current && current->sampleRate > 0)
        c.sampleRate = static_cast<int>(current->sampleRate);
    return c;
}
void AdiRootComponent::setAutomationMode(bool on) {
    automationMode_ = on;
    if (!on) {
        arrangement.canvas.clearAutomationSelection();
        arrangement.canvas.resetAutomationValueInput();
    }
    refreshAdditionalAutomationLanes();
    namesBar.automation.setToggleState(on, juce::dontSendNotification);
    say(on ? "Choose an automation device and parameter on the track header, or click the control itself. "
             "Click the line to add a breakpoint. Click a breakpoint to delete it, or drag it to move it."
           : "");
    arrangement.repaint();
}
std::vector<AutomationTarget> AdiRootComponent::automationChoices(std::int64_t track) const {
    const auto current = view_.current();
    if (!current)
        return {};
    const auto published = automationParameters ? automationParameters(track)
                                                : devices.publication ? devices.publication() : nullptr;
    return ::adi::ui::automationChoices(*current, track, published.get());
}
AutomationTarget AdiRootComponent::automationTarget(std::int64_t track) const {
    const auto selection = state_.automationChoices.find(track);
    if (selection == state_.automationChoices.end())
        return volumeTarget(track);
    const auto current = view_.current();
    const auto *node = current ? current->findTrack(track) : nullptr;
    if (!node)
        return volumeTarget(track);
    const auto &chosen = selection->second;
    const auto matches = [&](const AutomationTarget &target) {
        return target.ownerKind == chosen.ownerKind && target.ownerId == chosen.ownerId &&
               target.paramRef == chosen.paramRef;
    };
    if (chosen.ownerKind == "track" && chosen.ownerId == track) {
        if (chosen.paramRef == "pan")
            return panTarget(track);
        if (chosen.paramRef == "mute" && node->kind != "master")
            return activatorTarget(track);
    }
    if (chosen.ownerKind == "routing") {
        for (const auto &target : ::adi::ui::automationChoices(*current, track, nullptr))
            if (matches(target))
                return target;
    }
    if (chosen.ownerKind == "device")
        if (const auto published = devices.publication ? devices.publication() : nullptr)
            for (const auto &device : *published) {
                if (device.id != chosen.ownerId || device.track != track)
                    continue;
                if (chosen.paramRef == kDeviceActivatorParamRef) {
                    auto target = deviceActivatorTarget(device);
                    if (device.activatorAvailable || envelopeOf(*current, target).lane)
                        return target;
                    break;
                }
                const auto cached = automationTargets_.find(track);
                if (device.metadata || !device.declarations().empty()) {
                    for (const auto &parameter : device.declarations()) {
                        if (cached != automationTargets_.end() && matches(cached->second) &&
                            !cached->second.nativeParamId.empty() && parameter.id != cached->second.nativeParamId)
                            continue;
                        if (auto target = deviceTarget(device, parameter); target && matches(*target))
                            return std::move(*target);
                    }
                    break;
                }
                if (cached != automationTargets_.end() && matches(cached->second)) {
                    auto target = cached->second;
                    target.device = device.name;
                    return target;
                }
                // A reopened project has the address but has yet to resolve the hidden device's
                // descriptor. Keep the result for subsequent frames without a new catalog read.
                for (auto &target : automationChoices(track))
                    if (matches(target)) {
                        automationTargets_.insert_or_assign(track, target);
                        return std::move(target);
                    }
                break;
            }
    return volumeTarget(track);
}
void AdiRootComponent::showAutomation(std::int64_t track, const AutomationTarget &target) {
    const auto current = view_.current();
    if (!current || !current->findTrack(track))
        return;
    auto selected = target;
    if (target.ownerKind == "routing") {
        const auto mixerChoices = ::adi::ui::automationChoices(*current, track, nullptr);
        const auto found = std::find(mixerChoices.begin(), mixerChoices.end(), target);
        if (found == mixerChoices.end())
            return;
        selected = *found;
    }
    if (selected.ownerKind == "device")
        automationTargets_.insert_or_assign(track, selected);
    else
        automationTargets_.erase(track);
    if (!automationMode_)
        setAutomationMode(true);

    bool changed = state_.hiddenAutomationOverlays.erase(track) != 0;
    auto &saved = state_.automationChoices;
    if (selected == volumeTarget(track))
        changed = saved.erase(track) != 0 || changed;
    else {
        const WindowState::AutomationChoice choice{selected.ownerKind, selected.ownerId, selected.paramRef};
        const auto before = saved.find(track);
        if (before == saved.end() || before->second != choice) {
            saved.insert_or_assign(track, choice);
            changed = true;
        }
    }
    if (changed) {
        arrangement.canvas.resetAutomationValueInput();
        if (!persist()) {
            say("Automation choice could not be saved. " + error_);
            arrangement.repaint();
            return;
        }
    }
    say("Automation Mode shows " + (selected.ownerKind == "device" ? selected.device + ": " : std::string()) +
        selected.paramName + ".");
    arrangement.repaint();
}
namespace {
bool sameAutomationLaneStructure(const engine::Snapshot *a, const engine::Snapshot *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    const auto sameTrack = [](const auto &left, const auto &right) {
        return left == right || (left && right && left->id == right->id);
    };
    const auto sameSend = [](const auto &left, const auto &right) {
        return left == right || (left && right && left->id == right->id && left->sourceKind == right->sourceKind &&
                                 left->sourceId == right->sourceId);
    };
    const auto sameLane = [](const auto &left, const auto &right) {
        return left == right || (left && right && left->id == right->id && left->ownerKind == right->ownerKind &&
            left->ownerId == right->ownerId && left->paramRef == right->paramRef && left->paramName == right->paramName &&
            left->visible == right->visible && left->height == right->height && left->valueDomain == right->valueDomain &&
            left->unit == right->unit && left->minValue == right->minValue && left->maxValue == right->maxValue &&
            left->defaultValue == right->defaultValue);
    };
    return std::equal(a->tracks.begin(), a->tracks.end(), b->tracks.begin(), b->tracks.end(), sameTrack) &&
           std::equal(a->sends.begin(), a->sends.end(), b->sends.begin(), b->sends.end(), sameSend) &&
           std::equal(a->automationLanes.begin(), a->automationLanes.end(), b->automationLanes.begin(),
                      b->automationLanes.end(), sameLane);
}

bool sameAutomationLaneOwners(const ParameterPublication *a, const ParameterPublication *b) {
    if (a == b)
        return true;
    return a && b && std::equal(a->begin(), a->end(), b->begin(), b->end(), [](const auto &left, const auto &right) {
        return left.id == right.id && left.track == right.track && left.instance == right.instance &&
               left.name == right.name && left.available == right.available && left.metadata == right.metadata;
    });
}

using AutomationCatalog = std::function<const ParameterPublication *(std::int64_t)>;
std::optional<std::pair<std::int64_t, AutomationTarget>> additionalLaneTarget(
    const engine::Snapshot &snapshot, const ParameterPublication *devices, const engine::AutomationLaneNode &lane,
    const AutomationCatalog &catalogForTrack = {}) {
    auto track = lane.ownerId;
    AutomationTarget target;
    if (lane.ownerKind == "track") {
        if (lane.paramRef == "volume")
            return std::pair{track, volumeTarget(track)};
        if (lane.paramRef == "pan")
            return std::pair{track, panTarget(track)};
        if (lane.paramRef == "mute")
            return std::pair{track, activatorTarget(track)};
    } else if (lane.ownerKind == "routing") {
        const auto send = std::find_if(snapshot.sends.begin(), snapshot.sends.end(), [&](const auto &entry) {
            return entry && entry->id == lane.ownerId && entry->sourceKind == "track";
        });
        if (send == snapshot.sends.end())
            return std::nullopt;
        track = (*send)->sourceId;
        if (lane.paramRef == "gain_db")
            return std::pair{track, sendTarget(lane.ownerId, lane.paramName.empty() ? "Send" : lane.paramName)};
    } else if (lane.ownerKind == "device" && devices) {
        const auto dev = std::find_if(devices->begin(), devices->end(), [&](const auto &entry) {
            return entry.id == lane.ownerId;
        });
        if (dev == devices->end())
            return std::nullopt;
        track = dev->track;
        if (lane.paramRef == kDeviceActivatorParamRef)
            return std::pair{track, deviceActivatorTarget(*dev)};
        const auto findParameter = [&](const DevicePanelData &description) -> std::optional<AutomationTarget> {
            for (const auto &parameter : description.declarations())
                if (auto candidate = deviceTarget(description, parameter); candidate &&
                    candidate->valueDomain == lane.valueDomain &&
                    automation_override_detail::sameParam(lane.ownerKind, lane.paramRef, candidate->paramRef))
                    return candidate;
            return std::nullopt;
        };
        if (auto candidate = findParameter(*dev))
            return std::pair{track, std::move(*candidate)};
        if (dev->available && !dev->metadata && dev->declarations().empty() && catalogForTrack)
            if (const auto *catalog = catalogForTrack(track))
                for (const auto &description : *catalog)
                    if (description.id == dev->id && description.track == track) {
                        if (auto candidate = findParameter(description))
                            return std::pair{track, std::move(*candidate)};
                        break;
                    }
        target.device = dev->name;
    } else
        return std::nullopt;
    target.ownerKind = lane.ownerKind;
    target.ownerId = lane.ownerId;
    target.paramRef = lane.paramRef;
    if (lane.ownerKind == "device") {
        std::uint32_t parameter = 0;
        if (parseDeviceParamRef(lane.paramRef, parameter))
            target.paramRef = std::to_string(parameter);
    }
    target.paramName = lane.paramName.empty() ? lane.paramRef : lane.paramName;
    target.valueDomain = lane.valueDomain;
    target.stepped = lane.valueDomain == "enum";
    target.unit = lane.unit;
    target.defaultValue = lane.defaultValue;
    target.minimum = lane.minValue.value_or(0.);
    target.maximum = lane.maxValue.value_or(1.);
    if (target.maximum <= target.minimum) {
        target.minimum = 0.;
        target.maximum = 1.;
    }
    target.footIsSilence = false;
    return std::pair{track, std::move(target)};
}
} // namespace

void AdiRootComponent::refreshAdditionalAutomationLanes() {
    const auto snapshot = automationMode_ ? view_.current() : nullptr;
    const auto published = snapshot && devices.publication ? devices.publication() : nullptr;
    if (sameAutomationLaneStructure(automationLaneSnapshot_.get(), snapshot.get()) &&
        sameAutomationLaneOwners(automationLanePublication_.get(), published.get())) {
        automationLaneSnapshot_ = snapshot;
        automationLanePublication_ = published;
        return;
    }
    automationLaneSnapshot_ = snapshot;
    automationLanePublication_ = published;
    additionalAutomationLanes_.clear();
    std::map<std::int64_t, std::shared_ptr<const ParameterPublication>> catalogs;
    const AutomationCatalog catalogForTrack = [&](std::int64_t track) -> const ParameterPublication * {
        if (!automationParameters)
            return nullptr;
        const auto [entry, inserted] = catalogs.try_emplace(track);
        if (inserted)
            entry->second = automationParameters(track);
        return entry->second.get();
    };
    if (snapshot)
        for (const auto &lane : snapshot->automationLanes) {
            if (!lane || !lane->visible)
                continue;
            auto entry = additionalLaneTarget(*snapshot, published.get(), *lane, catalogForTrack);
            if (!entry || !snapshot->findTrack(entry->first))
                continue;
            auto &trackLanes = additionalAutomationLanes_[entry->first];
            if (std::any_of(trackLanes.begin(), trackLanes.end(), [&](const auto &existing) {
                    return existing.target == entry->second;
                }))
                continue;
            const auto height = lane->height == 0 ? 42 : static_cast<int>(std::clamp<std::int64_t>(lane->height, 24, 512));
            trackLanes.push_back({std::move(entry->second), height});
        }
    layoutAdditionalAutomationLanes();
}

void AdiRootComponent::layoutAdditionalAutomationLanes() {
    decltype(arrangement.geometry.automationLaneHeights) heights;
    for (const auto &[track, lanes] : additionalAutomationLanes_)
        if (automationLanesExpanded(track)) {
            int height = 0;
            for (const auto &lane : lanes) {
                if (height > std::numeric_limits<int>::max() - ArrangementGeometry::kMaxTrackHeight - lane.height)
                    break;
                height += lane.height;
            }
            heights.emplace(track, height);
        }
    if (arrangement.geometry.automationLaneHeights != heights)
        arrangement.geometry.automationLaneHeights = std::move(heights);
    dirty_.mark(DirtySet::All);
}

const std::vector<AdditionalAutomationLane> &AdiRootComponent::additionalAutomationLanes(std::int64_t track) const {
    static const std::vector<AdditionalAutomationLane> empty;
    const auto found = additionalAutomationLanes_.find(track);
    return found == additionalAutomationLanes_.end() ? empty : found->second;
}

bool AdiRootComponent::automationLanesExpanded(std::int64_t track) const {
    return automationMode_ && !state_.collapsedAutomationLanes.contains(track);
}

bool AdiRootComponent::automationOverlayShown(std::int64_t track) const {
    if (!automationMode_ || state_.hiddenAutomationOverlays.contains(track))
        return false;
    const auto &lanes = additionalAutomationLanes(track);
    if (lanes.empty() || !automationLanesExpanded(track) || arrangement.geometry.foldedTracks.contains(track))
        return true;
    const auto selected = automationTarget(track);
    return std::none_of(lanes.begin(), lanes.end(), [&](const auto &lane) { return lane.target == selected; });
}

bool AdiRootComponent::automationTargetShown(std::int64_t track, const AutomationTarget &target) const {
    if (!automationMode_ || !arrangement.geometry.shown(track) || arrangement.geometry.foldedTracks.contains(track))
        return false;
    if (automationOverlayShown(track) && target == automationTarget(track))
        return true;
    const auto &lanes = additionalAutomationLanes(track);
    return automationLanesExpanded(track) &&
           std::any_of(lanes.begin(), lanes.end(), [&](const auto &lane) { return lane.target == target; });
}

bool AdiRootComponent::addAutomationLane(std::int64_t track, const AutomationTarget &target) {
    const auto snapshot = view_.current();
    if (!snapshot || !snapshot->findTrack(track))
        return fail("That track is gone.");
    auto chosen = automationTarget(track);
    if (chosen != target) {
        const auto choices = automationChoices(track);
        const auto found = std::find(choices.begin(), choices.end(), target);
        if (found == choices.end())
            return fail("That control is no longer available.");
        chosen = *found;
    }
    const auto envelope = envelopeOf(*snapshot, chosen);
    if (!envelope.lane || !envelope.lane->visible) {
        OpRequest request;
        if (envelope.lane)
            request = automation::setLaneVisible(envelope.lane->id, true);
        else {
            RowIds rows(submitter_.store().db());
            auto lane = automationLaneSpec(chosen, automationCurrent(chosen), rows.next("automation_lanes"), true);
            lane.visible = true;
            request = automation::createLane(lane);
        }
        request.label = "Add Automation Lane";
        if (!submitEdits({std::move(request)}))
            return false;
    }
    if (!automationMode_)
        setAutomationMode(true);
    const bool wasCollapsed = state_.collapsedAutomationLanes.erase(track) != 0;
    arrangement.canvas.clearAutomationSelection();
    arrangement.canvas.resetAutomationValueInput();
    refreshAdditionalAutomationLanes();
    if (wasCollapsed) {
        layoutAdditionalAutomationLanes();
        if (!persist())
            say("Automation lane view could not be saved. " + error_);
    }
    return true;
}

bool AdiRootComponent::addAutomatedLanes(std::int64_t track) {
    const auto snapshot = view_.current();
    if (!snapshot || !snapshot->findTrack(track))
        return fail("That track is gone.");
    const auto published = devices.publication ? devices.publication() : nullptr;
    std::vector<OpRequest> requests;
    for (const auto &lane : snapshot->automationLanes) {
        if (!lane || lane->visible || std::none_of(lane->envelopes.begin(), lane->envelopes.end(), [](const auto &data) {
                return data && !data->clipId && !data->points.empty();
            }))
            continue;
        const auto target = additionalLaneTarget(*snapshot, published.get(), *lane);
        if (target && target->first == track && envelopeOf(*snapshot, target->second).lane == lane.get()) {
            auto request = automation::setLaneVisible(lane->id, true);
            request.label = "Add Automated Lanes";
            requests.push_back(std::move(request));
        }
    }
    if (!submitEdits(std::move(requests)))
        return false;
    if (!automationMode_)
        setAutomationMode(true);
    const bool wasCollapsed = state_.collapsedAutomationLanes.erase(track) != 0;
    arrangement.canvas.clearAutomationSelection();
    arrangement.canvas.resetAutomationValueInput();
    refreshAdditionalAutomationLanes();
    if (wasCollapsed) {
        layoutAdditionalAutomationLanes();
        if (!persist())
            say("Automation lane view could not be saved. " + error_);
    }
    return true;
}

bool AdiRootComponent::removeAutomationLane(std::int64_t track, const AutomationTarget &target) {
    const auto snapshot = view_.current();
    const auto &lanes = additionalAutomationLanes(track);
    if (!snapshot || std::none_of(lanes.begin(), lanes.end(), [&](const auto &lane) { return lane.target == target; }))
        return fail("That automation lane is no longer visible.");
    const auto envelope = envelopeOf(*snapshot, target);
    if (!envelope.lane || !envelope.lane->visible)
        return fail("That automation lane is no longer visible.");
    auto request = automation::setLaneVisible(envelope.lane->id, false);
    request.label = "Remove Automation Lane";
    if (!submitEdits({std::move(request)}))
        return false;
    arrangement.canvas.clearAutomationSelection();
    arrangement.canvas.resetAutomationValueInput();
    refreshAdditionalAutomationLanes();
    return true;
}

void AdiRootComponent::toggleAutomationLanes(std::int64_t track) {
    const auto &lanes = additionalAutomationLanes(track);
    if (lanes.empty())
        return;
    const bool collapsing = state_.collapsedAutomationLanes.erase(track) == 0;
    if (collapsing)
        state_.collapsedAutomationLanes.insert(track);
    arrangement.canvas.clearAutomationSelection();
    arrangement.canvas.resetAutomationValueInput();
    layoutAdditionalAutomationLanes();
    if (collapsing && state_.hiddenAutomationOverlays.contains(track)) {
        showAutomation(track, lanes.front().target);
        if (!state_.hiddenAutomationOverlays.contains(track))
            return;
    }
    if (!persist())
        say("Automation lane view could not be saved. " + error_);
}

void AdiRootComponent::hideAutomationOverlay(std::int64_t track) {
    if (!state_.hiddenAutomationOverlays.insert(track).second)
        return;
    arrangement.canvas.clearAutomationSelection();
    arrangement.canvas.resetAutomationValueInput();
    if (!persist())
        say("Automation lane view could not be saved. " + error_);
    arrangement.repaint();
}

std::optional<std::pair<std::int64_t, AutomationTarget>> AdiRootComponent::deviceAutomationTarget(
    std::int64_t device, const std::string &parameter) const {
    const auto published = devices.publication ? devices.publication() : nullptr;
    const auto current = view_.current();
    if (!published || !current)
        return std::nullopt;
    const auto find = [&](const DevicePanelData &data) -> std::optional<std::pair<std::int64_t, AutomationTarget>> {
        for (const auto &entry : data.declarations())
            if (entry.id == parameter) {
                if (auto target = deviceTarget(data, entry))
                    return std::pair{data.track, std::move(*target)};
                break;
            }
        return std::nullopt;
    };
    for (const auto &entry : *published) {
        if (entry.id != device || !current->findTrack(entry.track))
            continue;
        if (parameter == kDeviceActivatorParamRef) {
            if (entry.activatorAvailable)
                return std::pair{entry.track, deviceActivatorTarget(entry)};
            break;
        }
        if (entry.metadata || !entry.declarations().empty() || !automationParameters)
            return find(entry);
        if (const auto declarations = automationParameters(entry.track))
            for (const auto &meta : *declarations)
                if (meta.id == device)
                    return find(meta);
        break;
    }
    return std::nullopt;
}
bool AdiRootComponent::showDeviceAutomation(std::int64_t device, const std::string &parameter) {
    const auto found = deviceAutomationTarget(device, parameter);
    if (!found)
        return fail("That parameter cannot be automated.");
    showAutomation(found->first, found->second);
    return true;
}
double AdiRootComponent::automationCurrent(const AutomationTarget &target) const {
    const auto current = view_.current();
    if (!current)
        return target.minimum;
    if (target.ownerKind == "track") {
        const auto *t = current->findTrack(target.ownerId);
        if (!t)
            return target.minimum;
        if (target.paramRef == "mute")
            return t->muted ? 1.0 : 0.0;
        return target.paramRef == "pan" ? t->pan : t->volumeDb;
    }
    if (target.ownerKind == "routing") {
        for (const auto &s : current->sends)
            if (s && s->id == target.ownerId)
                return s->gainDb;
        return target.minimum;
    }
    // A visible control can supply its current value. Other choices keep
    // their declared default from the metadata that built the chooser.
    if (const auto published = devices.publication ? devices.publication() : nullptr)
        for (const auto &d : *published)
            if (d.id == target.ownerId) {
                if (target.paramRef == kDeviceActivatorParamRef)
                    return d.enabled ? 1.0 : 0.0;
                for (const auto &r : d.parameters())
                    if (r.id == target.nativeParamId)
                        return r.playing;
                break;
            }
    return target.defaultValue;
}
bool AdiRootComponent::addAutomationPoint(std::int64_t track, std::int64_t tick, double value, bool exactValue) {
    return addAutomationPoint(track, automationTarget(track), tick, value, exactValue);
}
bool AdiRootComponent::addAutomationPoint(std::int64_t track, const AutomationTarget &target, std::int64_t tick,
                                          double value, bool exactValue) {
    const auto current = view_.current();
    if (current == nullptr || current->findTrack(track) == nullptr)
        return fail("That track is gone.");
    if (automationMode_ && !automationTargetShown(track, target))
        return fail("That automation lane is no longer visible.");
    const auto envelope = envelopeOf(*current, target);
    if (exactValue) {
        if (!editableEnvelope(envelope))
            return fail("Adding a value requires an envelope in musical time.");
        if (!validExactAutomationValue(envelope, target, value))
            return fail("Enter a finite value in the envelope's range.");
    }
    RowIds rows(submitter_.store().db()); // one allocation context for the edit (DF-E16)
    AutomationIds ids;
    ids.lane = rows.next("automation_lanes");
    ids.data = rows.next("automation_data");
    ids.point = 1;
    return submitEdits(::adi::ui::addAutomationPoint(envelope, target, tick, value,
                                                     automationCurrent(target), ids, exactValue));
}
bool AdiRootComponent::moveAutomationPoint(std::int64_t track, std::uint64_t point, std::int64_t tick, double value,
                                          bool exactValue) {
    return moveAutomationPoint(track, automationTarget(track), point, tick, value, exactValue);
}
bool AdiRootComponent::moveAutomationPoint(std::int64_t track, const AutomationTarget &target, std::uint64_t point,
                                          std::int64_t tick, double value, bool exactValue) {
    const auto current = view_.current();
    if (!current)
        return false;
    if (automationMode_ && !automationTargetShown(track, target))
        return fail("That automation lane is no longer visible.");
    const auto envelope = envelopeOf(*current, target);
    if (exactValue) {
        if (!envelope.lane || !envelope.data)
            return fail("That breakpoint is gone.");
        const auto &points = envelope.data->points;
        const auto found = std::find_if(points.begin(), points.end(), [point](const auto &entry) {
            return entry.id == point;
        });
        if (found == points.end() || found->time != tick)
            return fail("That breakpoint has changed. Open Edit Value again.");
        if (!validExactAutomationValue(envelope, target, value))
            return fail("Enter a finite value in the envelope's range.");
        const auto written = steppedAutomationValue(automationRange(target), value);
        return found->value == value || found->value == written ||
               submitEdits({editAutomationPointValue(envelope, *found, written)});
    }
    auto ops = ::adi::ui::moveAutomationPoint(envelope, target, point, tick, value);
    return !ops.empty() && submitEdits(std::move(ops));
}
bool AdiRootComponent::deleteAutomationPoint(std::int64_t track, std::uint64_t point) {
    return deleteAutomationPoint(track, automationTarget(track), point);
}
bool AdiRootComponent::deleteAutomationPoint(std::int64_t track, const AutomationTarget &target, std::uint64_t point) {
    const auto current = view_.current();
    if (!current)
        return false;
    if (automationMode_ && !automationTargetShown(track, target))
        return fail("That automation lane is no longer visible.");
    auto ops = ::adi::ui::deleteAutomationPoint(envelopeOf(*current, target), point);
    return !ops.empty() && submitEdits(std::move(ops));
}

bool AdiRootComponent::submitAutomationMove(std::vector<OpRequest> requests,
                                          std::span<const TrackEnvelope> envelopes) {
    if (requests.empty())
        return false;
    auto &store = submitter_.store();
    auto &db = store.db();
    try {
        {
            std::optional<SQLite::Transaction> read;
            if (sqlite3_get_autocommit(db.getHandle()) != 0)
                read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
            const auto head = OpJournal(store).headSeq();
            OpTransactionInputs inputs;
            for (const auto &envelope : envelopes) {
                const auto &lane = *envelope.lane;
                const auto &data = *envelope.data;
                const Payload laneKey{{"id", lane.id}};
                const Payload dataKey{{"lane_id", lane.id}, {"clip_id", nullptr}};
                auto laneRows = adi::detail::captureGuardRows(store, "automation_lanes", laneKey, 2);
                auto dataRows = adi::detail::captureGuardRows(store, "automation_data", dataKey, 2);
                if (laneRows.size() != 1 || dataRows.size() != 1)
                    return fail("That automation envelope changed during the drag.");
                const auto &row = laneRows.front();
                const auto &stored = dataRows.front();
                const auto &bytes = stored.at("data").get_binary();
                if (row.at("owner_kind") != lane.ownerKind || row.at("owner_id") != lane.ownerId ||
                    row.at("param_ref") != lane.paramRef || row.at("time_base") != lane.timeBase ||
                    row.at("min_value") != (lane.minValue ? Payload(*lane.minValue) : Payload(nullptr)) ||
                    row.at("max_value") != (lane.maxValue ? Payload(*lane.maxValue) : Payload(nullptr)) ||
                    stored.at("id") != data.id ||
                    !std::equal(bytes.begin(), bytes.end(), data.encoded.begin(), data.encoded.end(),
                                [](std::uint8_t storedByte, std::byte source) {
                                    return storedByte == std::to_integer<std::uint8_t>(source);
                                }))
                    return fail("That automation envelope changed during the drag.");
                inputs.rowGuards.push_back({"automation_lanes", laneKey, std::move(laneRows.front())});
                inputs.rowSetGuards.push_back({"automation_data", dataKey, std::move(dataRows)});
                const Payload rangesKey{{"data_id", data.id}};
                auto ranges = adi::detail::captureGuardRows(store, "automation_segment_ranges", rangesKey);
                inputs.rowSetGuards.push_back({"automation_segment_ranges", rangesKey, std::move(ranges)});
            }
            requests.front().selBefore = session_.advisory();
            const auto captured = CapturedTransactionAccess::seal(appliedSource(store), head, requests,
                                                                   std::move(inputs));
            for (auto &request : requests)
                request.capturedTransaction = captured;
        }
        return edited(submitter_.submit(std::move(requests)));
    } catch (const std::exception &error) {
        return fail(error.what());
    }
}

bool AdiRootComponent::replaceAutomationRange(std::int64_t lane, std::int64_t begin, std::int64_t end,
                                              std::span<const automation::NativeSpliceKnot> knots, const char *label) {
    auto edit = automation::prepareEnvelopeSplice(submitter_.store(), lane, begin, end, knots, editRate(),
                                                  automation::kSplicePointBudget, label, session_.advisory());
    if (!edit.ok)
        return fail(std::move(edit.error));
    return edited(submitter_.submit(std::move(edit.requests)));
}
bool AdiRootComponent::simplifyAutomationRange(std::int64_t lane, std::int64_t begin, std::int64_t end,
                                               double tolerance) {
    auto edit = automation::prepareSimplifyEnvelope(submitter_.store(), lane, begin, end, editRate(),
                                                    tolerance, session_.advisory());
    if (!edit.ok)
        return fail(std::move(edit.error));
    return edit.requests.empty() || edited(submitter_.submit(std::move(edit.requests)));
}
bool AdiRootComponent::transformAutomationRange(std::int64_t lane, const edit::AutomationTransform &transform,
                                                const char *label) {
    auto prepared = edit::prepareAutomationTransform(submitter_.store(), lane, transform, editRate(), label,
                                                     session_.advisory());
    if (!prepared.ok)
        return fail(std::move(prepared.error));
    return !prepared.requests.empty() && edited(submitter_.submit(std::move(prepared.requests)));
}
bool AdiRootComponent::drawAutomationStroke(std::int64_t track, const AutomationTarget &target,
                                           std::int64_t begin, std::int64_t end,
                                           const std::map<std::int64_t, double> &points, bool stepped,
                                           std::uint64_t generation) {
    const auto current = view_.committed();
    if (!automationMode_ || !current || !current->tempo || !current->findTrack(track) ||
        generation != view_.committedGeneration() || !automationTargetShown(track, target))
        return fail("The automation target changed during the stroke.");
    if (points.empty() || begin < 0 || points.begin()->first != begin || points.rbegin()->first >= end)
        return false;
    const auto env = envelopeOf(*current, target);
    if (!editableEnvelope(env))
        return fail("This envelope uses absolute time.");
    const std::uint8_t curve = stepped || target.stepped ? 0 : 1;
    const auto valueAt = [&](double value) { return writtenAutomationValue(target, value); };
    constexpr const char *label = "Draw Automation";
    if (env.lane) {
        // A mouse click at a close zoom can span less than one rendered
        // sample. Give the stroke at least one sample before splicing.
        const auto rate = editRate();
        const auto at = current->tempo->ticksToSeconds(begin) * rate;
        const auto until = current->tempo->ticksToSeconds(end) * rate;
        if (!std::isfinite(at) || !std::isfinite(until) || until > (std::int64_t{1} << 50))
            return fail("The automation stroke is beyond the timeline range.");
        if (std::llround(until) <= std::llround(at))
            end = std::max(end, current->tempo->secondsToTicks(static_cast<double>(std::llround(at) + 1) / rate));
        std::vector<automation::NativeSpliceKnot> knots;
        knots.reserve(points.size() + 1);
        for (const auto &[tick, value] : points)
            knots.push_back({{tick, valueAt(value), 0.f, curve, 0, 0, 0}, {}});
        knots.push_back({{end, knots.back().point.value, 0.f, curve, 0, 0, 0}, {}});
        return replaceAutomationRange(env.lane->id, begin, end, knots, label);
    }

    RowIds rows(submitter_.store().db());
    const auto lane = rows.next("automation_lanes");
    const auto base = valueAt(automationCurrent(target));
    auto knots = Payload::array();
    std::uint64_t pointId = 1;
    auto addPoint = [&](std::int64_t tick, double value, std::uint8_t kind) {
        knots.push_back({{"id", pointId++}, {"time", tick}, {"value", value}, {"curve", kind}, {"tension", 0.0}});
    };
    if (begin > 0)
        addPoint(0, base, 0);
    for (const auto &[tick, value] : points)
        addPoint(tick, valueAt(value), curve);
    if (curve != 0 && points.rbegin()->first < end - 1)
        addPoint(end - 1, valueAt(points.rbegin()->second), 0);
    addPoint(end, base, 0);
    auto create = automation::createLane(automationLaneSpec(target, base, lane));
    auto write = automation::writePoints(rows.next("automation_data"), lane, nullptr, std::move(knots));
    create.label = write.label = label;
    return submitEdits({std::move(create), std::move(write)});
}

namespace {
std::vector<std::int64_t> matchingAutomation(const engine::Snapshot &snapshot, std::string_view kind,
                                             std::int64_t owner, std::string_view param = {}) {
    std::vector<std::int64_t> ids;
    for (const auto &lane : snapshot.automationLanes)
        if (lane && lane->ownerKind == kind && lane->ownerId == owner &&
            (param.empty() || automation_override_detail::sameParam(kind, lane->paramRef, param)))
            ids.push_back(lane->id);
    return ids;
}
}

bool AdiRootComponent::canDeleteShownAutomation() const {
    const auto current = view_.current();
    if (!current || clipView.notes.hasContext() || submitter_.store().readOnly())
        return false;
    const auto target = arrangement.canvas.automationCommandTarget();
    return target && !matchingAutomation(*current, target->ownerKind, target->ownerId, target->paramRef).empty();
}

bool AdiRootComponent::deleteAutomationLanes(const std::vector<std::int64_t> &ids, const char *label) {
    if (ids.empty())
        return false;
    std::vector<OpRequest> requests;
    requests.reserve(ids.size());
    const auto current = view_.current();
    for (const auto id : ids) {
        const auto *lane = current ? current->findAutomationLane(id) : nullptr;
        auto op = lane && lane->visible ? automation::clearLane(id) : automation::deleteLane(id);
        op.label = label;
        requests.push_back(std::move(op));
    }
    return submitEdits(std::move(requests));
}

void AdiRootComponent::addAutomationDeleteItem(juce::PopupMenu &menu, const AutomationTarget &target) {
    const auto current = view_.current();
    if (!current || submitter_.store().readOnly() ||
        matchingAutomation(*current, target.ownerKind, target.ownerId, target.paramRef).empty())
        return;
    menu.addItem("Delete Automation", [safe = SafePointer<AdiRootComponent>(this), target] {
        if (safe)
            if (const auto snapshot = safe->view_.current())
                safe->deleteAutomationLanes(matchingAutomation(*snapshot, target.ownerKind, target.ownerId,
                                                                target.paramRef), "Delete Automation");
    });
}

std::vector<std::int64_t> AdiRootComponent::trackAutomationLanes(std::int64_t track) {
    std::vector<std::int64_t> ids;
    const auto current = view_.current();
    if (!current || !current->findTrack(track) || current->automationLanes.empty())
        return ids;
    SQLite::Statement query(submitter_.store().db(), R"sql(
        WITH RECURSIVE track_devices(id) AS (
            SELECT d.id FROM devices d JOIN device_chains c ON c.id=d.chain_id WHERE c.track_id=?1
            UNION
            SELECT d.id FROM devices d JOIN device_chains c ON c.id=d.chain_id
                JOIN track_devices parent ON parent.id=c.parent_device_id
        )
        SELECT id FROM automation_lanes
        WHERE (owner_kind='track' AND owner_id=?1)
           OR (owner_kind='device' AND owner_id IN track_devices)
           OR (owner_kind='routing' AND owner_id IN (SELECT id FROM routing
                                                    WHERE src_kind='track' AND src_id=?1))
        ORDER BY id
    )sql");
    query.bind(1, track);
    while (query.executeStep())
        ids.push_back(query.getColumn(0).getInt64());
    return ids;
}

void AdiRootComponent::addTrackAutomationDeleteItem(juce::PopupMenu &menu, std::int64_t track) {
    if (submitter_.store().readOnly() || trackAutomationLanes(track).empty())
        return;
    menu.addItem("Delete All Automation", [safe = SafePointer<AdiRootComponent>(this), track] {
        if (safe)
            safe->deleteAutomationLanes(safe->trackAutomationLanes(track), "Delete All Automation");
    });
}

void AdiRootComponent::addDeviceAutomationDeleteItem(juce::PopupMenu &menu, std::int64_t device) {
    const auto current = view_.current();
    if (!current || submitter_.store().readOnly() || matchingAutomation(*current, "device", device).empty())
        return;
    menu.addItem("Delete All Automation", [safe = SafePointer<AdiRootComponent>(this), device] {
        if (safe)
            if (const auto snapshot = safe->view_.current())
                safe->deleteAutomationLanes(matchingAutomation(*snapshot, "device", device), "Delete All Automation");
    });
}

bool AdiRootComponent::setSoloInPlace(bool on) {
    // Live's Solo in Place (manual 18.6): one project field for the Solo button's menu and the
    // Options menu's entry (#412, N23), from the CURRENT cell (ADR-0201). The op is outside undo, as
    // the catalogue says, and saved with the project, so no undo step is made and nothing is said;
    // the session re-reads the project after it (afterEdit), so the next solo sounds as the field
    // says. Follow is not paused: nothing in the arrangement moved.
    const auto current = view_.current();
    if (!current)
        return fail("Solo in Place needs an open project.");
    auto edit = soloInPlaceEdit(*current, on);
    if (edit.ops.empty())
        return true;
    if (const auto result = submitter_.submit(std::move(edit.ops)); !result)
        return fail(result.error);
    if (afterEdit)
        afterEdit();
    error_.clear();
    dirty_.mark(DirtySet::All);
    return true;
}
// What every cue op shares (MX-08): a transport op outside undo, the session re-reading the project
// after it, nothing said; Follow is not paused, nothing in the arrangement moved.
bool AdiRootComponent::submitCue(TrackEdit edit, const char *needs) {
    if (!view_.current())
        return fail(needs);
    if (!edit.refusal.empty())
        return fail(edit.refusal);
    if (edit.ops.empty())
        return true;
    if (const auto result = submitter_.submit(std::move(edit.ops)); !result)
        return fail(result.error);
    if (afterEdit)
        afterEdit();
    error_.clear();
    dirty_.mark(DirtySet::All);
    return true;
}
bool AdiRootComponent::setCueMode(bool cue) {
    const auto current = view_.current();
    return submitCue(current ? cueModeEdit(*current, cue) : TrackEdit{}, "Cue Switches need an open project.");
}
bool AdiRootComponent::cueTrack(std::int64_t track) {
    // A Cue button toggles its own track only: Live's Exclusive Solo is solo's, and whether Live's Cue
    // is exclusive is to be read on Live 12 (PROVISIONAL, MX-08).
    const auto current = view_.current();
    return submitCue(current ? cueTrackEdit(*current, track, !cued(*current, track)) : TrackEdit{},
                     "Cueing needs an open project.");
}
bool AdiRootComponent::setCueVolume(double db) {
    const auto current = view_.current();
    return submitCue(current ? cueVolumeEdit(*current, db) : TrackEdit{}, "Cue Volume needs an open project.");
}
bool AdiRootComponent::setMainOutput(int firstChannel) {
    const auto current = view_.current();
    return submitCue(current ? mainOutputEdit(*current, firstChannel) : TrackEdit{}, "Main Out needs an open project.");
}
bool AdiRootComponent::setCueOutput(int firstChannel) {
    const auto current = view_.current();
    return submitCue(current ? cueOutputEdit(*current, firstChannel) : TrackEdit{}, "Cue Out needs an open project.");
}
bool AdiRootComponent::restoreHistoryHead(std::optional<std::int64_t> head) {
    return restoreHistory([this, head] {
        return submitter_.restoreHistoryHead(head, juce::Time::currentTimeMillis() * 1000);
    }, [this, head] {
        return restoreStoredHistory && restoreStoredHistory(HistoryHeadTarget{head});
    });
}
bool AdiRootComponent::restoreStep(std::int64_t txnId) {
    return restoreHistory([this, txnId] { return submitter_.restore(txnId); }, [this, txnId] {
        if (!restoreStoredHistory)
            return false;
        const auto timeline = submitter_.timeline();
        const auto step = std::find_if(timeline.steps.begin(), timeline.steps.end(), [txnId](const auto &entry) {
            return entry.step.txnId == txnId;
        });
        return step != timeline.steps.end() && restoreStoredHistory(HistoryHeadTarget{step->step.lastSeq});
    });
}
bool AdiRootComponent::restoreHistory(const std::function<History::Result()> &restore,
                                     const std::function<bool()> &reload) {
    if (recordingHoldsHistory())
        return fail(kRecordingHistoryMessage);
    if (settleEdits && !settleEdits())
        return false;
    const auto result = restore();
    if (result.sessionReloadRequired && reload())
        return true;
    if (!result.ok)
        return fail(result.error.empty() ? "That step could not be restored." : result.error);
    arrangement.followPaused = true; // as an undo pauses it
    if (afterEdit)
        afterEdit();
    error_.clear();
    dirty_.mark(DirtySet::All);
    return true;
}
bool AdiRootComponent::submitTransport(LoopEdit edit) {
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (edit.ops.empty())
        return true;
    if (const auto result = submitter_.submit(std::move(edit.ops)); !result)
        return fail(result.error);
    if (afterEdit)
        afterEdit();
    error_.clear();
    dirty_.mark(DirtySet::All);
    return true;
}
bool AdiRootComponent::setMetronome(bool on) {
    // From the CURRENT cell (ADR-0201), so a switch clicked in one window and O in another agree.
    const auto current = view_.current();
    if (!current)
        return fail("The metronome needs an open project.");
    return submitTransport(::adi::ui::setMetronome(current->transport, on));
}
bool AdiRootComponent::toggleMetronome() {
    const auto current = view_.current();
    return setMetronome(!(current && current->transport.metronome));
}
bool AdiRootComponent::toggleMetronomeWhileRecording() {
    const auto current = view_.current();
    if (!current)
        return fail("The metronome needs an open project.");
    return submitTransport(::adi::ui::setMetronomeOptions({{"recordingOnly", !current->transport.metronomeOptions.recordingOnly}}));
}
bool AdiRootComponent::setMetronomeEighthNotes(bool eighthNotes) {
    const auto current = view_.current();
    if (!current)
        return fail("The metronome needs an open project.");
    return current->transport.metronomeOptions.eighthNotes == eighthNotes ||
           submitTransport(::adi::ui::setMetronomeOptions({{"eighthNotes", eighthNotes}}));
}
bool AdiRootComponent::setMetronomeSound(MetronomeSound sound) {
    const auto current = view_.current();
    if (!current)
        return fail("The metronome needs an open project.");
    return current->transport.metronomeOptions.sound == sound ||
           submitTransport(::adi::ui::setMetronomeOptions({{"sound", static_cast<int>(sound)}}));
}
bool AdiRootComponent::setCountIn(int bars) {
    const auto current = view_.current();
    if (!current)
        return fail("Count-in needs an open project.");
    return submitTransport(::adi::ui::setCountIn(current->transport, bars));
}
bool AdiRootComponent::togglePunch(bool start) {
    const auto current = view_.current();
    if (!current)
        return fail("Punch-In and Punch-Out need an open project.");
    const auto &t = current->transport;
    return submitTransport(setPunch(t, start ? !t.punchIn : t.punchIn, start ? t.punchOut : !t.punchOut));
}
bool AdiRootComponent::toggleLoop() {
    const auto current = view_.current();
    if (!current)
        return fail("The loop needs an open project.");
    const auto &t = current->transport;
    return submitTransport(setLoop(t, !t.loopEnabled, loopOf(t)));
}
bool AdiRootComponent::setLoopRange(LoopRange range) {
    const auto current = view_.current();
    if (!current)
        return fail("The loop needs an open project.");
    return submitTransport(setLoop(current->transport, current->transport.loopEnabled, range));
}
bool AdiRootComponent::loopSelection() {
    const auto current = view_.current();
    if (!current)
        return fail("The loop needs an open project.");
    // The time selection, else the selected clips' span; neither, the loop toggles where it is.
    const auto &sel = arrangement.geometry.selection;
    std::optional<LoopRange> selected;
    if (sel.end != sel.start) {
        selected = LoopRange{std::min(sel.start, sel.end), std::max(sel.start, sel.end)};
    } else if (sel.clip != 0) {
        std::int64_t from = 0, to = 0;
        bool any = false;
        for (const auto &t : current->tracks)
            for (const auto &c : t->clips)
                if (c->id == sel.clip || std::find(sel.pickedClips.begin(), sel.pickedClips.end(), c->id) != sel.pickedClips.end()) {
                    from = any ? std::min(from, c->posTicks) : c->posTicks;
                    to = any ? std::max(to, c->posTicks + c->lengthTicks) : c->posTicks + c->lengthTicks;
                    any = true;
                }
        if (any)
            selected = LoopRange{from, to};
    }
    if (!submitTransport(::adi::ui::loopSelection(current->transport, selected)))
        return false;
    arrangement.scrub.repaint();
    return true;
}
bool AdiRootComponent::braceKey(BraceKey key, bool command) {
    const auto current = view_.current();
    if (!current)
        return fail("The loop needs an open project.");
    const auto &geometry = arrangement.geometry;
    const auto to = ::adi::ui::braceKey(loopOf(current->transport), key, command,
                                        [&geometry](std::int64_t tick, bool forward) { return geometry.nextGridLine(tick, forward); });
    return !to || setLoopRange(*to);
}
bool AdiRootComponent::setLoopStartAt(const std::string &text) {
    const auto current = view_.current();
    if (!current)
        return fail("The loop needs an open project.");
    return submitTransport(typeLoopStart(current->transport, text, current->meters));
}
bool AdiRootComponent::setLoopLengthAt(const std::string &text) {
    const auto current = view_.current();
    if (!current)
        return fail("The loop needs an open project.");
    return submitTransport(typeLoopLength(current->transport, text, current->meters));
}
bool AdiRootComponent::linkLoopToSelection() {
    const auto &sel = arrangement.geometry.selection;
    if (!commands_.linkLoopToSelection() || sel.end == sel.start || sel.clip != 0 || !sel.pickedClips.empty())
        return true;
    return setLoopRange({std::min(sel.start, sel.end), std::max(sel.start, sel.end)});
}
std::int64_t AdiRootComponent::arrangementEnd() const {
    const auto current = view_.current();
    std::int64_t end = 0;
    if (!current)
        return end;
    for (const auto &t : current->tracks)
        for (const auto &c : t->clips)
            end = std::max(end, c->posTicks + c->lengthTicks);
    for (const auto &l : locatorsOf(*current))
        end = std::max(end, l.pos);
    return end;
}
bool AdiRootComponent::jumpTo(std::int64_t tick) {
    // While playing, playback jumps there (manual 6.4); while stopped, the insert marker goes there.
    if (playing_) {
        if (!locate(tick))
            return fail("The project has no tempo map to locate in.");
        return true;
    }
    arrangement.placeInsertMarker(tick, true);
    return true;
}
bool AdiRootComponent::addLocatorAt(std::int64_t pos, bool select) {
    const auto current = view_.current();
    if (!current)
        return fail("Locators need an open project.");
    auto edit = addLocator(*current, pos);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
    if (select) {
        arrangement.selectedLocator = edit.id;
        arrangement.selectedMeter = -1;
        arrangement.loopSelected = false;
    }
    arrangement.scrub.repaint();
    return true;
}
std::int64_t AdiRootComponent::stoppedSetLocatorTick() const noexcept {
    const auto &sel = arrangement.geometry.selection;
    return sel.start != sel.end ? std::min(sel.start, sel.end) : sel.insert;
}
bool AdiRootComponent::setLocatorDeletes() const noexcept {
    if (playing_ || arrangement.selectedLocator == 0)
        return false;
    const auto *snapshot = reader_->get();
    const auto *marker = snapshot != nullptr ? snapshot->findMarker(arrangement.selectedLocator) : nullptr;
    // A locator selected at a former play position can stay highlighted after Stop.
    return marker != nullptr && marker->kind == "position" && marker->timeBase == 0 && marker->posTicks &&
           (*marker->posTicks == stoppedSetLocatorTick() || *marker->posTicks == timelineTick());
}
bool AdiRootComponent::setLocator() {
    if (setLocatorDeletes())
        return deleteLocator(arrangement.selectedLocator);
    // The running button marks the playhead and keeps the current selection. Global launch
    // quantization arrives with the Session View's launch settings (ADR-0203 d10).
    return playing_ ? addLocatorAt(timelineTick(), false) : addLocatorAt(stoppedSetLocatorTick());
}
bool AdiRootComponent::jumpLocator(bool forward) {
    const auto current = view_.current();
    if (!current)
        return fail("Locators need an open project.");
    const auto &sel = arrangement.geometry.selection;
    const auto from = playing_ ? timelineTick() : sel.insert;
    const auto to = nextLocator(locatorsOf(*current), from, forward);
    // Past the first or the last locator, the arrangement's start or end (manual 6.4).
    return jumpTo(to ? *to : forward ? arrangementEnd() : 0);
}
bool AdiRootComponent::deleteLocator(std::int64_t id) {
    const auto current = view_.current();
    if (!current)
        return fail("Locators need an open project.");
    auto edit = ::adi::ui::deleteLocator(*current, id);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
    arrangement.selectedLocator = 0;
    arrangement.scrub.repaint();
    return true;
}
bool AdiRootComponent::moveLocator(std::int64_t id, std::int64_t to) {
    const auto current = view_.current();
    if (!current)
        return fail("Locators need an open project.");
    auto edit = ::adi::ui::moveLocator(*current, id, to);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
    arrangement.scrub.repaint();
    return true;
}
bool AdiRootComponent::renameLocator(std::int64_t id, const std::string &name) {
    const auto current = view_.current();
    if (!current)
        return fail("Locators need an open project.");
    auto edit = ::adi::ui::renameLocator(*current, id, name);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    return submitEdits(std::move(edit.ops));
}
bool AdiRootComponent::setLocatorInfo(std::int64_t id, const std::string &text) {
    const auto current = view_.current();
    if (!current)
        return fail("Locators need an open project.");
    auto edit = ::adi::ui::setLocatorInfo(*current, id, text);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    return submitEdits(std::move(edit.ops));
}
bool AdiRootComponent::loopToNextLocator(std::int64_t id) {
    // Loop to Next Locator (manual 6.4): the loop on, from this locator to the next, or to the
    // arrangement's end after the last.
    const auto current = view_.current();
    if (!current)
        return fail("Locators need an open project.");
    const auto locators = locatorsOf(*current);
    const auto *from = locatorById(locators, id);
    if (from == nullptr)
        return fail("Select a locator to loop from.");
    const auto to = nextLocator(locators, from->pos, true).value_or(arrangementEnd());
    if (to <= from->pos)
        return fail("Nothing follows this locator to loop to.");
    return submitTransport(setLoop(current->transport, true, {from->pos, to}));
}
bool AdiRootComponent::setQuantize(std::int64_t track, bool record, const std::string &choice) {
    const auto current = view_.current();
    const auto *t = current ? current->findTrack(track) : nullptr;
    if (t == nullptr)
        return fail("That track is gone.");
    auto edit = ::adi::ui::setQuantize(*t, record, choice);
    return edit.ops.empty() || submitEdits(std::move(edit.ops));
}
bool AdiRootComponent::setKeepMonitoringLatency(std::int64_t track, bool on) {
    const auto current = view_.current();
    const auto *t = current ? current->findTrack(track) : nullptr;
    if (t == nullptr)
        return fail("That track is gone.");
    auto edit = ::adi::ui::setKeepMonitoringLatency(*t, on);
    return edit.ops.empty() || submitEdits(std::move(edit.ops));
}
bool AdiRootComponent::toggleSoloInPlace() {
    // From the CURRENT cell (ADR-0201), not the presented frame's, read at pick time: the Options
    // menu and the Solo button's menu (the Mix View's own window too) share the field, and one
    // left open while another threw it still toggles what the project holds now.
    const auto current = view_.current();
    return setSoloInPlace(!(current && current->cue.soloInPlace));
}
bool AdiRootComponent::activateTrack(std::int64_t id, bool withSelection) {
    // From the CURRENT cell (ADR-0201); the batch is one undo step.
    const auto current = view_.current();
    if (!current)
        return fail("Deactivating needs an open project.");
    auto edit = activateTracks(current->tracks, id,
                               withSelection ? arrangement.geometry.selectedTracks(reader()) : std::vector<std::int64_t>{});
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    return edit.ops.empty() || submitEdits(std::move(edit.ops));
}
bool AdiRootComponent::insertMeter(std::int64_t pos, const std::string &text) {
    const auto current = view_.current();
    if (!current)
        return fail("Time signatures need an open project.");
    auto edit = ::adi::ui::insertMeter(current->meters, pos, text);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
    arrangement.selectedMeter = pos;
    arrangement.scrub.repaint();
    return true;
}
bool AdiRootComponent::editMeter(std::int64_t pos, const std::string &text) {
    const auto current = view_.current();
    if (!current)
        return fail("Time signatures need an open project.");
    auto edit = ::adi::ui::editMeter(current->meters, pos, text);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
    arrangement.selectedMeter = pos;
    arrangement.scrub.repaint();
    return true;
}
bool AdiRootComponent::moveMeter(std::int64_t from, std::int64_t to) {
    const auto current = view_.current();
    if (!current)
        return fail("Time signatures need an open project.");
    auto edit = ::adi::ui::moveMeter(current->meters, from, to);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
    if (to != from)
        arrangement.selectedMeter = to;
    arrangement.scrub.repaint();
    return true;
}
bool AdiRootComponent::setMeterAtPlay(const std::string &text) {
    const auto current = view_.current();
    if (!current)
        return fail("Time signatures need an open project.");
    const auto at = timelineTick();
    const bool surely = startSurelyStored(current->meters);
    auto edit = setMeterAt(current->meters, at, text, surely);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (edit.ops.empty())
        return true;
    // A 4/4 at the start may be stored or the snapshot's own: insert first, and where the project
    // stores one, replace it.
    if (!surely && edit.ops.size() == 1 && edit.ops.front().payload.at("pos").get<std::int64_t>() == 0)
        return submitStartEdit(std::move(edit.ops), setMeterAt(current->meters, at, text, true).ops);
    return submitEdits(std::move(edit.ops));
}
bool AdiRootComponent::tapTempo(std::int64_t nowMs) {
    const auto bpm = tap_.tap(nowMs);
    if (!bpm) {
        tapStep_ = 0;
        tapBpm_ = 0;
        return true;
    }
    const double value = std::round(std::clamp(*bpm, kMinTempo, kMaxTempo) * 100) / 100;
    if (std::abs(value - tapBpm_) < .005)
        return true;
    // The run's step gives way to this tap's, while it is still the latest: one step for the run. A
    // knob edit still held commits first (DF-W29), so the check sees the step that is latest.
    if (settleEdits)
        settleEdits();
    if (const auto last = submitter_.nextUndo(); tapStep_ != 0 && last && last->txnId == tapStep_) {
        const bool paused = arrangement.followPaused;
        command(AppCommands::Undo);
        arrangement.followPaused = paused;
    }
    const auto before = submitter_.nextUndo();
    if (!setTempoAtPlay(juce::String(value, 2).toStdString(), "Tap Tempo"))
        return false;
    tapBpm_ = value;
    if (const auto after = submitter_.nextUndo(); after && (!before || after->txnId != before->txnId))
        tapStep_ = after->txnId;
    return true;
}
bool AdiRootComponent::setPositionAt(const std::string &text) {
    const auto at = parsePosition(text);
    if (!at)
        return fail("Type a position such as 5.2.3: bar, beat and sixteenth.");
    const auto tick = tickOf(*at, reader().meters());
    if (!tick)
        return fail("Bar " + std::to_string(at->bar) + " has no beat " + std::to_string(at->beat) + ", sixteenth " +
                    std::to_string(at->sixteenth) + " in its time signature.");
    arrangement.placeInsertMarker(*tick);
    return true;
}
bool AdiRootComponent::setTempoChange(std::int64_t pos, const std::string &text) {
    const auto bpm = parseTempo(text);
    if (!bpm) return fail("Type a tempo from 20 to 999 BPM, such as 128 or 97.50.");
    if (pos < 0) return fail("A tempo change cannot precede the start of the song.");
    constexpr auto *label = "Change Tempo";
    CommitOptions options;
    std::vector<OpRequest> ops;
    auto &store = submitter_.store();
    Payload payload{{"pos", pos}, {"bpm", *bpm}, {"curve", 0}};
    // The playback snapshot omits row identity and ramp tension. Preserve both when editing an
    // authored event, and guard the read and replacement against another edit of the project.
    try {
        options.expectHead.emplace(History(store).head());
        SQLite::Statement stmt(store.db(), "SELECT id,bpm,curve,tension FROM tempo_map WHERE pos_ticks=?");
        stmt.bind(1, pos);
        if (stmt.executeStep()) {
            if (stmt.getColumn(1).getDouble() == *bpm) return true;
            payload["id"] = stmt.getColumn(0).getInt64();
            payload["curve"] = stmt.getColumn(2).getInt64();
            payload["tension"] = stmt.getColumn(3).getDouble();
            ops.push_back(track_edits_detail::op("project.removeTempoEvent", {{"pos", pos}}, label));
        }
    } catch (const std::exception &e) {
        return fail(e.what());
    }
    ops.push_back(track_edits_detail::op("project.insertTempoEvent", std::move(payload), label));
    ops.front().selBefore = session_.advisory();
    return edited(submitter_.commitPrepared(ops, [&](Store &target, Actor) {
        return OpJournal(target).commit(ops, options);
    }));
}
bool AdiRootComponent::setTempoAtPlay(const std::string &text, const char *label) {
    const auto current = view_.current();
    if (!current || !current->tempo)
        return fail("The tempo needs an open project.");
    const auto at = timelineTick();
    const auto &events = current->tempo->events;
    const bool surely = tempoSurelyStored(events);
    auto edit = setTempoAt(events, at, text, surely, label);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (edit.ops.empty())
        return true;
    // A 120 at the start may be stored or the snapshot's own, as for the signature.
    if (!surely && edit.ops.size() == 1 && edit.ops.front().payload.at("pos").get<std::int64_t>() == 0)
        return submitStartEdit(std::move(edit.ops), setTempoAt(events, at, text, true, label).ops);
    return submitEdits(std::move(edit.ops));
}
bool AdiRootComponent::submitStartEdit(std::vector<OpRequest> tried, std::vector<OpRequest> otherwise) {
    tried.front().selBefore = session_.advisory();
    if (const auto result = submitter_.submit(std::move(tried)); result.ok)
        return edited(result);
    return submitEdits(std::move(otherwise));
}
bool AdiRootComponent::deleteMeter(std::int64_t pos) {
    const auto current = view_.current();
    if (!current)
        return fail("Time signatures need an open project.");
    auto edit = ::adi::ui::deleteMeter(current->meters, pos);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
    arrangement.selectedMeter = -1;
    arrangement.scrub.repaint();
    return true;
}
bool AdiRootComponent::moveTracks(std::int64_t dragged, TrackDrop to) {
    // From the CURRENT cell (ADR-0201); the batch is one undo step, and the selection stays.
    const auto current = view_.current();
    if (!current)
        return fail("Moving tracks needs an open project.");
    auto ids = arrangement.geometry.selectedTracks(reader());
    if (std::find(ids.begin(), ids.end(), dragged) == ids.end())
        ids = {dragged};
    auto edit = ::adi::ui::moveTracks(current->tracks, ids, to);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    return edit.ops.empty() || submitEdits(std::move(edit.ops));
}
bool AdiRootComponent::nudgeSelectedTracks(bool down) {
    const auto current = view_.current();
    if (!current)
        return fail("Moving tracks needs an open project.");
    auto ids = arrangement.geometry.selectedTracks(reader());
    auto edit = ::adi::ui::nudgeTracks(current->tracks, ids, down);
    if (edit.ops.empty())
        return true;
    if (!submitEdits(std::move(edit.ops)))
        return false;

    auto &selection = arrangement.geometry.selection;
    if (selection.toTrack != 0) {
        const SnapshotReader updated(view_.current());
        std::vector<TrackRow> rows;
        rowsFromSnapshot(updated, rows);
        ArrangementGeometry check;
        check.selection = selection;
        check.order.build(updated, rows);
        const auto ranged = check.selectedTracks(updated);
        if (!std::is_permutation(ids.begin(), ids.end(), ranged.begin(), ranged.end())) {
            // When a range would reach different tracks after the move, keep the original IDs.
            selection.toTrack = 0;
            std::erase(ids, selection.track);
            selection.picked = std::move(ids);
            publishSelection(SelectionOrigin::Arrangement);
        }
    }
    return true;
}
bool AdiRootComponent::showDeviceView() { return state_.docked || command(AppCommands::ToggleDeviceView); }
bool AdiRootComponent::showClipView() { return state_.clipView || command(AppCommands::ToggleClipView); }
bool AdiRootComponent::setDetailViews(bool showClip, bool showDevices) {
    const bool previousClip = std::exchange(state_.clipView, showClip);
    const bool previousDevices = std::exchange(state_.docked, showDevices);
    if (!persist()) {
        state_.clipView = previousClip;
        state_.docked = previousDevices;
        return fail(error_);
    }
    if (afterEdit)
        afterEdit();
    error_.clear();
    resized();
    return true;
}
bool AdiRootComponent::fullScreen() const {
    const auto *window = dynamic_cast<const juce::ResizableWindow *>(getTopLevelComponent());
    return window != nullptr && window->isFullScreen();
}
bool AdiRootComponent::focusView(int id) {
    // Live's Navigate menu (§41.4.1): the keyboard focus moves to a view, shown first if it
    // was hidden.
    switch (id) {
    case AppCommands::FocusControlBar:
        transport.focus();
        return true;
    case AppCommands::FocusArrangement:
        focusIfShowing(arrangement.canvas);
        return true;
    case AppCommands::FocusClipView:
        // The editor of the clip shown: the Note Editor or the Sample Editor. With no clip, the
        // view itself, empty as it is.
        if (!showClipView())
            return false;
        if (clipView.details() == nullptr)
            focusIfShowing(clipView);
        else if (clipView.midi())
            focusIfShowing(clipView.envelopesShown() ? static_cast<juce::Component &>(clipView.envelopes)
                                                     : static_cast<juce::Component &>(clipView.notes));
        else
            focusIfShowing(clipView.sample);
        return true;
    case AppCommands::FocusDeviceView:
        if (!state_.docked && !command(AppCommands::ToggleDeviceView))
            return false;
        if (devices.panels.empty())
            return fail("The selected track has no devices.");
        focusIfShowing(*devices.panels.front());
        return true;
    case AppCommands::FocusBrowser:
    case AppCommands::SearchBrowser:
        if (state_.mixView && !state_.mixViewDetached)
            return mixViewPanel.focusChannelSearch();
        if (state_.panels.find(Panel::Browser)->collapsed && !command(AppCommands::ToggleBrowser))
            return false;
        if (id == AppCommands::FocusBrowser) {
            focusIfShowing(browser);
            return true;
        }
        // Ctrl/Cmd+F always searches All, with the library search text ready to type over.
        browser.searchAll();
        focusIfShowing(browser.search);
        browser.search.selectAll();
        return true;
    case AppCommands::FocusMixer: {
        // The Mixer View when it is open, else the track controls, shown first if hidden.
        if (!state_.mixerView && state_.panels.find(Panel::Mixer)->collapsed &&
            !command(AppCommands::ToggleMixer))
            return false;
        auto &panel = state_.mixerView ? mixerView : mixer;
        panel.frame(true);
        return panel.focusTrack(arrangement.geometry.selection.track) ||
               fail("The mixer shows no track.");
    }
    default:
        return false;
    }
}
juce::File AdiRootComponent::writeKeyboardReference() const {
    // Every command, in the order the menus know them, with the keys it has now (a user's own included).
    auto *mappings = commands_.manager.getKeyMappings();
    const auto escape = [](const juce::String &text) {
        return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;");
    };
    juce::String html = "<!doctype html><meta charset=\"utf-8\"><title>ADI Keyboard Shortcuts</title>"
                        "<h1>ADI Keyboard Shortcuts</h1><table>";
    for (int id = AppCommands::PlayStop; id <= AppCommands::LastId; ++id) {
        juce::ApplicationCommandInfo info(id);
        commands_.getCommandInfo(id, info);
        juce::StringArray keys;
        if (mappings != nullptr)
            for (const auto &k : mappings->getKeyPressesAssignedToCommand(id))
                keys.add(k.getTextDescriptionWithIcons());
        html << "<tr><td>" << escape(info.shortName) << "</td><td>" << escape(keys.joinIntoString(", "))
             << "</td></tr>";
    }
    html << "</table>";
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ADI Keyboard Shortcuts.html");
    return file.replaceWithText(html, false, false, "\n") ? file : juce::File();
}
bool AdiRootComponent::command(int id) {
    if ((mixViewPanel.stack.hasToolbarFocus() || mixViewPanel.hasAnalyzerControlFocus() ||
         mixViewPanel.hasChannelControlFocus()) && deviceOrTrackEditCommand(id))
        return false;
    if (id == AppCommands::Undo || id == AppCommands::Redo) {
        if (recordingHoldsHistory())
            return fail(kRecordingHistoryMessage);
        mixViewPanel.stack.cancelInteractions();
        devices.cancelFunctionInteractions();
    }
    if (mixViewPanel.stack.hasDeviceContext()) {
        if (id == AppCommands::Cut || id == AppCommands::Copy || id == AppCommands::Paste ||
            id == AppCommands::Duplicate || id == AppCommands::RenameTrack || id == AppCommands::DeleteTrack)
            return mixViewPanel.stack.deviceCommand(id);
    }
    // The Note Editor, while it has the keyboard, takes the Edit and Options menus' commands for
    // its notes and its own grid (manual 10.5 and on): chosen from a menu, they act where their
    // keys do. Cut, Copy and Paste Time are not the editor's: there they do nothing.
    if (noteEditorHasContext()) {
        if (const int item = noteEditorItem(id, clipView.notes); item != 0)
            return clipView.notes.chooseContextItem(item);
        if (id >= AppCommands::NarrowGrid && id <= AppCommands::FixedGrid) {
            clipView.notes.gridKey(id - AppCommands::NarrowGrid + 1);
            return true;
        }
        if (id == AppCommands::CutTime || id == AppCommands::CopyTime || id == AppCommands::PasteTime)
            return false;
        if (id == AppCommands::RenameTrack)
            return clipView.chooseTitleItem(1);
    }
    if (state_.clipView && clipView.audio() && clipView.sample.hasKeyboardFocus(true)) {
        if (id == AppCommands::PlayStop && !mailbox_.desiredPlaying() && clipView.sample.insert()) {
            clipView.sample.playFromInsert();
            return true;
        }
        if (clipView.sample.command(id))
            return true;
    }
    // The editor's switches, from a menu, whenever it is on screen; the Clip View's full height
    // whenever it is open (manual 10.2).
    if (id >= AppCommands::FoldToNotes && id <= AppCommands::HighlightScales) {
        if (!noteEditorShown())
            return false;
        return clipView.notes.chooseContextItem(id == AppCommands::FoldToNotes   ? NoteEditor::kFoldToNotes
                                                : id == AppCommands::FoldToScale ? NoteEditor::kFoldToScale
                                                                                 : NoteEditor::kHighlightScale);
    }
    if (id >= AppCommands::ArrangePanelsAutomatically && id <= AppCommands::ArrangePanelsVertically) {
        if (!state_.clipView)
            return false;
        clipView.arrange(static_cast<ClipViewPanel::Panels>(id - AppCommands::ArrangePanelsAutomatically));
        return true;
    }
    if (id == AppCommands::ExpandClipView) {
        if (!state_.clipView)
            return false;
        toggleClipViewFull();
        return true;
    }
    // The editor's own Edit commands outside it: nothing to act on.
    if ((id >= AppCommands::InvertSelection && id <= AppCommands::CropClip) || id == AppCommands::FitToTimeRange)
        return false;
    // The application's own: files and Settings.
    // In a window of its own, the Mix View is the application's to open and close (its floating
    // host); docked, the root's.
    if (id == AppCommands::DetachMixView || (id == AppCommands::ToggleMixView && state_.mixViewDetached))
        return applicationCommand && applicationCommand(id);
    if ((id >= AppCommands::NewProject && id <= AppCommands::Settings) || id == AppCommands::Quit ||
        (id >= AppCommands::SaveCopy && id <= AppCommands::CollectAndExport) || id == AppCommands::About ||
        id == AppCommands::ManageProjectFiles || id == AppCommands::OpenBackup)
        return applicationCommand && applicationCommand(id);
    if ((id >= AppCommands::TimeZoomIn && id <= AppCommands::GoStart) ||
        (id >= AppCommands::InsertMarkerToPlayhead && id <= AppCommands::ShrinkTracks) ||
        (id >= AppCommands::NarrowGrid && id <= AppCommands::FixedGrid))
        return arrangement.command(id);
    // F1…F8: the first eight numbered tracks, in the arrangement's order (returns and Main have letters
    // or none), each alone.
    if (id >= AppCommands::ActivateTrack1 && id <= AppCommands::ActivateTrack8) {
        auto n = id - AppCommands::ActivateTrack1;
        for (const auto row : arrangement.geometry.rows(reader()))
            if (row.track->kind != "return" && row.track->kind != "master" && n-- == 0)
                return activateTrack(row.track->id, false);
        return fail("There is no track " + std::to_string(id - AppCommands::ActivateTrack1 + 1) + ".");
    }
    if (id == AppCommands::LargeTracks) {
        const bool done = arrangement.command(id);
        strip.tracks.setToggleState(state_.largeTracks, juce::dontSendNotification);
        return done;
    }
    // No action carries the presented frame into an edit: OpSubmitter resolves
    // current store/history state on arrival, even when this window is a frame behind.
    bool recordStartRefused = false;
    switch (id) {
    case AppCommands::PlayStop:
    case AppCommands::Stop:
    case AppCommands::ContinuePlay: {
        // Space starts at the selection's left edge or at the insert marker. Shift+Space continues from the stop point.
        const bool wasPlaying = mailbox_.desiredPlaying();
        const bool rewind = id == AppCommands::Stop && !wasPlaying;
        const bool play = id != AppCommands::Stop && !wasPlaying;
        const bool fromMarker = play && id == AppCommands::PlayStop;
        // Stop clears a waiting request even if the driver cannot accept another command.
        if (!play) {
            setRecordPending(false);
            if (isRecording() && stopRecording)
                stopRecording();
        }
        if (fromMarker && recordPending_) {
            // The Record command starts at the normal recording point with the saved Count-In.
            if (beginArrangementRecording(arrangement.geometry.selection.insert, false))
                break;
            recordStartRefused = true;
            setRecordPending(false);
        }
        if (!mailbox_.hasRoom(fromMarker ? 2 : 1))
            return fail("Transport command queue is full");
        OpRequest request;
        request.opType = play ? "transport.play" : "transport.stop";
        request.payload = Payload::object();
        const auto result = submitter_.submit(std::move(request));
        if (!result)
            return fail(result.error);
        // Continue keeps the exact stopped sample owned by the driver's mailbox.
        if (play && recordPending_ && !beginArrangementRecording(timelineTick(), true)) {
            recordStartRefused = true;
            setRecordPending(false);
        }
        if (fromMarker) {
            const auto &sel = arrangement.geometry.selection;
            const bool timeSelection = sel.clip == 0 && sel.pickedClips.empty() && sel.start != sel.end;
            locate(timeSelection ? std::min(sel.start, sel.end) : sel.insert);
        }
        using Command = TransportMailbox::Command;
        mailbox_.post(!play ? Command::Stop : fromMarker ? Command::Play : Command::Continue);
        if (rewind)
            arrangement.showInsertMarkerAt(0);
        break;
    }
    case AppCommands::ArrangementRecord:
    case AppCommands::ArrangementRecordAlternate:
        if (!toggleRecord(id == AppCommands::ArrangementRecordAlternate))
            return false;
        break;
    case AppCommands::AutomationMode:
        setAutomationMode(!automationMode_);
        return true;
    case AppCommands::LockEnvelopes: {
        std::string why;
        const bool saved = commands_.setEnvelopesLocked(!commands_.envelopesLocked(), why);
        namesBar.lock.setToggleState(commands_.envelopesLocked(), juce::dontSendNotification);
        if (!saved)
            return fail(why.empty() ? "Lock Envelopes could not be saved." : why);
        say({});
        return true;
    }
    case AppCommands::DeleteAutomation: {
        if (!canDeleteShownAutomation())
            return false;
        const auto target = arrangement.canvas.automationCommandTarget();
        return target && deleteAutomationLanes(matchingAutomation(*view_.current(), target->ownerKind,
                                                                  target->ownerId, target->paramRef),
                                               "Delete Automation");
    }
    case AppCommands::DrawMode:
        clipView.notes.setDrawMode(!drawMode());
        return true;
    case AppCommands::ComputerMidiKeyboard:
        computerMidiEnabled_ = !computerMidiEnabled_;
        if (!computerMidiEnabled_)
            releaseComputerMidiKeys();
        transport.keyboard.setToggleState(computerMidiEnabled_, juce::dontSendNotification);
        commands_.manager.commandStatusChanged();
        mark(DirtySet::Transport);
        return true;
    case AppCommands::HelpManual:
    case AppCommands::KeyboardReference: {
        const auto target = id == AppCommands::HelpManual ? juce::URL(kManualAddress)
                                                          : juce::URL(writeKeyboardReference());
        if (target.isEmpty())
            return fail("The keyboard reference could not be written.");
        if (!(openHelp ? openHelp(target) : target.launchInDefaultBrowser()))
            return fail("The system could not open it.");
        break;
    }
    case AppCommands::ImportMidiFile:
    case AppCommands::ExportMidiClip:
        if (id == AppCommands::ExportMidiClip && midiClipForExport() == 0)
            return fail("Select one MIDI clip to export.");
        return applicationCommand && applicationCommand(id);
    case AppCommands::ExportAudio:
        if (!exportAudio)
            return fail("Export runs in the application.");
        exportAudio(exportDefaults());
        break;
    case AppCommands::Undo:
    case AppCommands::Redo: {
        // A knob's last edit, still held for its quiet interval, is the step the user sees last: it
        // commits first, so Undo takes it back and nothing commits on top of a Redo (DF-W29).
        if (settleEdits && !settleEdits())
            return false;
        const auto result = id == AppCommands::Undo ? submitter_.undo() : submitter_.redo();
        if (!result.ok)
            return fail(result.error);
        arrangement.followPaused = true;
        // Undo puts back what was selected before the undone step (ADR-0021).
        if (id == AppCommands::Undo && result.selBefore) {
            const auto priorTrack = session_.selection().track;
            session_.restore(*result.selBefore);
            if (priorTrack != session_.selection().track)
                refreshSelectionArming();
        }
        break;
    }
    case AppCommands::SwapPanels: {
        const auto previous = state_.panels;
        if (auto refusal = state_.panels.swapSides(getWidth()))
            return fail(refusal.message);
        if (!persist()) {
            state_.panels = previous;
            return fail(error_);
        }
        break;
    }
    case AppCommands::ToggleBrowser:
    case AppCommands::ToggleMixer:
        if (!toggle(id == AppCommands::ToggleBrowser ? Panel::Browser : Panel::Mixer))
            return false;
        break;
    case AppCommands::ToggleMixerView:
        state_.mixerView = !state_.mixerView;
        if (!persist()) {
            state_.mixerView = !state_.mixerView;
            return fail(error_);
        }
        break;
    case AppCommands::ShowChannelEq:
        // ADR-0204 d1's one window is the Mix View's STACK (ADR-0209 d11): opened there, wherever
        // it opens, on the selected track's row.
        if (!state_.mixView) {
            if (state_.mixViewDetached ? !(applicationCommand && applicationCommand(AppCommands::ToggleMixView))
                                       : !setMixView(true, false))
                return false;
        }
        if (!chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Stack)))
            return false;
        mixViewPanel.frame(reader());
        mixViewPanel.stack.focus(arrangement.geometry.selection.track);
        break;
    case AppCommands::ToggleReferenceTrack:
        if (referenceTrack(SnapshotReader(view_.current())) == 0)
            return fail(kNoReferenceTrack);
        state_.showReference = !state_.showReference;
        if (!persist()) {
            state_.showReference = !state_.showReference;
            return fail(error_);
        }
        arrangement.applyView();
        arrangement.firstShownIfHidden();
        break;
    case AppCommands::PlayReference:
        return fail(referenceTrack(SnapshotReader(view_.current())) == 0 ? kNoReferenceTrack : kReferencePlaybackUnavailable);
    case AppCommands::ToggleMixView:
        return setMixView(!state_.mixView, state_.mixViewDetached);
    case AppCommands::ToggleClipView:
        return setDetailViews(!state_.clipView, state_.docked);
    case AppCommands::ToggleDeviceView:
        return setDetailViews(state_.clipView, !state_.docked);
    case AppCommands::SwitchDetailView: {
        if (!arrangement.isVisible())
            return false;
        const bool showClip = !state_.clipView || (state_.docked && devices.hasKeyboardFocus(true));
        return setDetailViews(showClip, !showClip);
    }
    case AppCommands::Follow:
        state_.follow = !state_.follow;
        if (!persist()) {
            state_.follow = !state_.follow;
            return fail(error_);
        }
        arrangement.followPaused = false;
        break;
    case AppCommands::ToggleOverview:
    case AppCommands::ToggleInOut:
    case AppCommands::ToggleSends:
    case AppCommands::ToggleTrackMixer:
    case AppCommands::ToggleMixerInOut:
    case AppCommands::ToggleMixerSends:
    case AppCommands::ToggleTrackOptions:
    case AppCommands::ToggleMixerTrackOptions: {
        auto &shown = id == AppCommands::ToggleOverview  ? state_.overview
                      : id == AppCommands::ToggleInOut   ? state_.inOut
                      : id == AppCommands::ToggleSends   ? state_.sends
                      : id == AppCommands::ToggleTrackMixer ? state_.trackMixer
                      : id == AppCommands::ToggleMixerSends ? state_.mixerSends
                      : id == AppCommands::ToggleTrackOptions ? state_.trackOptions
                      : id == AppCommands::ToggleMixerTrackOptions ? state_.mixerTrackOptions
                                                         : state_.mixerInOut;
        shown = !shown;
        if (!persist()) {
            shown = !shown;
            return fail(error_);
        }
        // The lanes, and the track controls' rows level with them, move with the Overview;
        // each row, and each Mixer View strip, lays its controls out again with or without In/Out
        // and the sends.
        arrangement.resized();
        for (const auto *panel : {&mixer, &mixerView})
            for (const auto &row : panel->strips()) {
                row->resized();
                row->repaint();
            }
        namesBar.inOut.setToggleState(state_.inOut, juce::dontSendNotification);
        namesBar.sends.setToggleState(state_.sends, juce::dontSendNotification);
        namesBar.delay.setToggleState(state_.trackOptions, juce::dontSendNotification);
        break;
    }
    case AppCommands::FullScreen: {
        // Only a window on the screen can fill it: JUCE reads the monitor's area to do so.
        auto *window = dynamic_cast<juce::ResizableWindow *>(getTopLevelComponent());
        if (window == nullptr || !window->isOnDesktop())
            return fail("This view has no window on the screen to fill it with.");
        window->setFullScreen(!window->isFullScreen());
        break;
    }
    case AppCommands::ToggleInfoView: {
        const bool reopening = state_.panels.find(Panel::Browser)->collapsed;
        if (reopening && !toggle(Panel::Browser))
            return false;
        const bool shown = commands_.infoView();
        const bool showInfo = reopening || !shown;
        std::string why;
        if (showInfo != shown && !commands_.setInfoView(showInfo, why)) {
            if (reopening)
                resized();
            return fail(why);
        }
        infoShown_ = commands_.infoView();
        break;
    }
    case AppCommands::ToggleBrowserPreview: {
        const bool enable = !commands_.browserPreviewEnabled();
        std::string why;
        if (!commands_.setBrowserPreviewEnabled(enable, why))
            return fail(why);
        if (!enable && stopSamplePreview)
            stopSamplePreview();
        browser.syncPreviewToggle();
        commands_.manager.commandStatusChanged();
        return true;
    }
    case AppCommands::PreviewSelected:
        return browser.previewSelected();
    case AppCommands::HotSwap:
        return devices.hotSwapFocused();
    case AppCommands::ToggleFavorite:
    case AppCommands::ToggleCollectionRed:
    case AppCommands::ToggleCollectionOrange:
    case AppCommands::ToggleCollectionYellow:
    case AppCommands::ToggleCollectionGreen:
    case AppCommands::ToggleCollectionBlue:
    case AppCommands::ToggleCollectionPurple:
    case AppCommands::ToggleCollectionGray:
        return browser.toggleSelectionMark(static_cast<unsigned>(id - AppCommands::ToggleFavorite));
    case AppCommands::TabMovesFocus: {
        std::string why;
        if (!commands_.setTabMovesFocus(!commands_.tabMovesFocus(), why))
            return fail(why);
        break;
    }
    case AppCommands::DelayCompensation: {
        // Live's switch (manual 18.7): the setting, then the session; no undo step. The strips grey
        // their Track Delay at the next frame.
        // The setting is in force once set, whether or not its file could be written, as the Settings'
        // rows are, so the session follows it either way; a session that refuses keeps its message in
        // the Status Bar (the tail below would clear it), and a file not written is said.
        std::string why;
        const bool saved = commands_.setDelayCompensation(!commands_.delayCompensation(), why);
        if (applyDelayCompensation && !applyDelayCompensation(commands_.delayCompensation()))
            return true;
        if (!saved)
            return fail(why.empty() ? "Delay Compensation could not be changed." : why);
        break;
    }
    case AppCommands::WrapTabNavigation: {
        std::string why;
        if (!commands_.setWrapTabNavigation(!commands_.wrapTabNavigation(), why))
            return fail(why);
        break;
    }
    case AppCommands::InsertMidiClip: {
        const auto &sel = arrangement.geometry.selection;
        if (!insertMidiClips(arrangement.geometry.selectedTracks(reader()), std::min(sel.start, sel.end),
                             std::max(sel.start, sel.end)))
            return false;
        break;
    }
    case AppCommands::ImportAudioFile: {
        const auto &selection = arrangement.geometry.selection;
        const auto *track = reader().findTrack(selection.track);
        if (track == nullptr || track->kind != "audio")
            return fail("Select an audio track to import a file.");
        if (importPrompt_)
            return fail("Answer the import prompt first.");
        if (importing_)
            return fail("The last file is still being imported.");
        if (!chooseAudioFile)
            return fail("The audio file chooser is unavailable.");
        const auto root = juce::Component::SafePointer<AdiRootComponent>(this);
        if (!chooseAudioFile([root, id = track->id, tick = selection.insert](const juce::File &file) {
                if (root != nullptr && file != juce::File{})
                    root->dropAudio(file.getFullPathName().toStdString(), id, tick,
                                    file.getFileNameWithoutExtension().toStdString());
            }))
            return fail("Close the open file chooser before importing audio.");
        return true;
    }
    case AppCommands::InsertAudioTrack:
    case AppCommands::InsertMidiTrack:
        if (!insertTrack(id == AppCommands::InsertMidiTrack))
            return false;
        break;
    case AppCommands::DeleteTrack:
        if (devices.focusedDevice())
            return devices.deleteDevices();
        if (arrangement.canvas.hasAutomationSelection())
            return deleteAutomationRange();
        if (arrangement.selectedLocator != 0)
            return deleteLocator(arrangement.selectedLocator);
        if (arrangement.selectedMeter >= 0)
            return deleteMeter(arrangement.selectedMeter);
        if (arrangement.canvas.hasKeyboardFocus(true) &&
            (arrangement.canvas.selectedFade() || (automationMode() && arrangement.canvas.chosenAutomationPoint())))
            return arrangement.canvas.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
        if (const auto &sel = arrangement.geometry.selection; sel.clip != 0 || sel.start != sel.end)
            return arrangement.canvas.editMaterialSelection(false);
        if (arrangement.canvas.hasKeyboardFocus(true))
            return false;
        return deleteTrack();
    case AppCommands::InsertTimeSignatureChange: {
        // At the insert marker (Live 12 manual 6.5): a field there to type the value; where a marker
        // stands, that marker's value. The start's signature is the control bar's.
        const auto pos = arrangement.geometry.selection.insert;
        const auto current = view_.current();
        if (!current)
            return fail("Time signatures need an open project.");
        if (pos <= 0)
            return fail("The time signature at the start is the control bar's; place the insert marker "
                        "where the change goes.");
        arrangement.selectedMeter = markerAt(current->meters, pos) != 0 ? pos : -1;
        return arrangement.scrub.beginMeterEdit(pos, true);
    }
    case AppCommands::ConsolidateMidi: {
        auto prepared = prepareMidiConsolidation();
        if (!commitEdit(std::move(prepared.edit)))
            return false;
        selectClip(prepared.clip);
        return true;
    }
    case AppCommands::GroupTracks:
    case AppCommands::UngroupTracks:
        if (!trackGroupingContext() || !groupTracks(id == AppCommands::UngroupTracks))
            return false;
        break;
    case AppCommands::SelectAll:
        if (!arrangement.selectAll())
            return false;
        break;
    case AppCommands::SoloSelected:
        // With Cue Switches, S cues the track as its Cue button does (MX-08); complete on its own.
        if (const auto current = view_.current(); current && current->cue.cueMode)
            return cueTrack(arrangement.geometry.selection.track);
        if (!soloTrack(arrangement.geometry.selection.track, false))
            return false;
        break;
    case AppCommands::ArmSelected:
        return toggleArm(std::nullopt, false);
    case AppCommands::SoloSwitches:
    case AppCommands::CueSwitches:
        // Complete on its own, as Solo in Place: setCueMode re-reads the session and says why it refuses.
        return setCueMode(id == AppCommands::CueSwitches);
    case AppCommands::SoloInPlace:
        // Complete on its own, as the Solo button's menu is: the session's re-read, the Status Bar
        // and the marks are setSoloInPlace's, so the tail below is not run a second time.
        return toggleSoloInPlace();
    case AppCommands::Cut:
    case AppCommands::Copy:
    case AppCommands::Paste:
    case AppCommands::Duplicate: {
        using Clip = DeviceChainStrip::Clip;
        const auto clip = id == AppCommands::Cut    ? Clip::Cut
                          : id == AppCommands::Copy ? Clip::Copy
                          : id == AppCommands::Paste ? Clip::Paste
                                                     : Clip::Duplicate;
        if (deviceClipboardFirst(clip))
            return devices.clipboard(clip);
        const auto command = id == AppCommands::Cut    ? ClipCommand::Cut
                             : id == AppCommands::Copy ? ClipCommand::Copy
                             : id == AppCommands::Paste ? ClipCommand::Paste
                                                        : ClipCommand::Duplicate;
        if (!arrangement.canvas.hasAutomationSelection() && !canClipClipboard(command) &&
            clip == Clip::Paste && commands_.clipClipboard().empty())
            return fail("Copy a clip, a time selection or a device first.");
        return clipClipboard(command);
    }
    case AppCommands::CreateFade:
        return createFades();
    case AppCommands::DeleteFades:
        return deleteFades();
    case AppCommands::Split:
        return splitAtMarker();
    case AppCommands::ReverseAudio:
        return reverseAudio();
    case AppCommands::CutTime:
    case AppCommands::CopyTime:
    case AppCommands::PasteTime:
    case AppCommands::DuplicateTime:
    case AppCommands::DeleteTime:
    case AppCommands::InsertSilence:
        return timeCommand(id);
    case AppCommands::TogglePluginWindows:
        // Every plug-in window hidden, or shown again where it stood (Auto-Hide still applies).
        pluginWindowsShown_ = !pluginWindowsShown_;
        followPluginWindows(); // shown again as Auto-Hide allows
        commands_.manager.commandStatusChanged();
        return true;
    case AppCommands::UndoHistory:
        // The app puts the list in a window of its own (ui_main.cpp); headless, it is read as it is.
        undoHistory.refresh();
        if (applicationCommand)
            applicationCommand(AppCommands::UndoHistory);
        if (undoHistory.isShowing())
            focusIfShowing(undoHistory.list);
        return true;
    // The metronome and the loop are complete on their own too (submitTransport).
    case AppCommands::Metronome:
        return toggleMetronome();
    case AppCommands::LoopSelection:
        return loopSelection();
    case AppCommands::SelectLoop:
        return arrangement.selectLoop();
    case AppCommands::AddLocator:
        return addLocatorAt(timelineTick());
    case AppCommands::DeleteLocator:
        return deleteLocator(arrangement.selectedLocator);
    case AppCommands::EditInfoText:
        return arrangement.selectedLocator != 0 ? arrangement.scrub.beginLocatorEdit(arrangement.selectedLocator, true)
                                               : fail("Select a locator to edit its info text.");
    case AppCommands::DeactivateSelected:
        if (!activateTrack(arrangement.geometry.selection.track))
            return false;
        break;
    case AppCommands::InsertReturnTrack:
        if (!insertReturnTrack())
            return false;
        break;
    case AppCommands::ToggleReturnTracks:
        state_.hideReturns = !state_.hideReturns;
        if (!persist()) {
            state_.hideReturns = !state_.hideReturns;
            return fail(error_);
        }
        arrangement.applyView();
        arrangement.firstShownIfHidden();
        namesBar.returns.setToggleState(!state_.hideReturns, juce::dontSendNotification);
        break;
    case AppCommands::RenameTrack:
        // Rename acts on the selection: a selected time signature marker (Edit Time Signature), the
        // device with the focus, else the selected track. The edit itself is its own op when the
        // name is committed.
        if (arrangement.selectedLocator != 0)
            return arrangement.scrub.beginLocatorEdit(arrangement.selectedLocator, false);
        if (arrangement.selectedMeter >= 0)
            return arrangement.scrub.beginMeterEdit(arrangement.selectedMeter, false);
        for (auto &panel : devices.panels)
            if (panel->hasKeyboardFocus(true))
                return panel->beginRename() || fail("Unfold the device to rename it.");
        // A selected clip is renamed in its title bar, as Live (walk-B B14); a click on a track's
        // header lets the clip go, so Rename then acts on the track.
        if (arrangement.geometry.selection.clip != 0)
            return arrangement.canvas.beginClipRename(arrangement.geometry.selection.clip) ||
                   fail("Scroll the clip into view to rename it.");
        return arrangement.headers.beginRename(arrangement.geometry.selection.track) ||
               fail("Select a track to rename.");
    case AppCommands::FocusControlBar:
    case AppCommands::FocusArrangement:
    case AppCommands::FocusClipView:
    case AppCommands::FocusDeviceView:
    case AppCommands::FocusBrowser:
    case AppCommands::SearchBrowser:
    case AppCommands::FocusMixer:
        return focusView(id);
    case AppCommands::NextNeighbor:
    case AppCommands::PreviousNeighbor:
        // From a mixer switch with the focus, in either mixer; elsewhere nothing moves.
        return mixerView.focusNeighbour(id == AppCommands::NextNeighbor) ||
               mixer.focusNeighbour(id == AppCommands::NextNeighbor);
    case AppCommands::ZoomIn:
    case AppCommands::ZoomOut: {
        const auto next = nextZoom(interfaceZoom_, id == AppCommands::ZoomIn);
        if (!next)
            return false;
        interfaceZoom_ = *next;
        if (auto *host = getParentComponent())
            host->resized();
        // Kept for this window on the display it stands on, for the next launch (N4).
        zoomDisplay_ = displayAt(getScreenBounds().getCentre());
        if (std::string why; !commands_.keepZoom(window_, *zoomDisplay_, interfaceZoom_, why)) {
            say("The zoom is not kept for the next launch: " + why);
            return true;
        }
        break;
    }
    default:
        return false;
    }
    if (afterEdit)
        afterEdit();
    if (!recordStartRefused)
        error_.clear();
    resized();
    return true;
}
namespace {
// Live 12's menu bar (manual §42.28), in Live's order, each menu listing only the commands
// ADI has; 0 is a separator. A menu with none is left out: Help, until there is a Learn
// View (§2.2.1). On the Mac, Settings is the application menu's (§2.3). Transport has no
// menu in Live; Space is its key.
struct MenuEntries {
    const char *name;
    std::vector<int> items;
};
// Live 12's View menu submenus, in a menu's items: Arrangement Track Controls and Mixer Controls.
constexpr int kTrackControlsMenu = -1, kMixerControlsMenu = -2, kOpenRecentMenu = -3;
constexpr int kFirstRecentProject = 0x4000;
constexpr int kClearRecentProjects = kFirstRecentProject + kRecentProjectLimit;
constexpr int kRemoveUnavailableProjects = kClearRecentProjects + 1;
const std::vector<MenuEntries> &menuBar() {
    using C = AppCommands;
    static const std::vector<MenuEntries> bar{
#if JUCE_MAC
        {"File", {C::NewProject, C::OpenProject, C::OpenBackup, kOpenRecentMenu, 0, C::TakeSnapshot, C::SaveAs, C::SaveCopy, C::CollectAllAndSave,
                  C::CollectAndExport, C::ManageProjectFiles, 0, C::ExportAudio, C::ExportMidiClip}},
#else
        {"File", {C::NewProject, C::OpenProject, C::OpenBackup, kOpenRecentMenu, 0, C::TakeSnapshot, C::SaveAs, C::SaveCopy, C::CollectAllAndSave,
                  C::CollectAndExport, C::ManageProjectFiles, 0, C::ExportAudio, C::ExportMidiClip, 0, C::Quit}},
#endif
        // Live's Cut, Copy, Paste and Duplicate, then Delete (§18.2, 41.5); the four act on devices.
        // Loop Selection and Select Loop (manual 6.6, §42.8) after Select All; their place in Live 12's
        // Edit menu is to be read on Live. Split (6.9) heads the clip commands' group, after the
        // clipboard's and the selection's, with Crop Clip (10.7.1) and Consolidate.
        // Invert Selection follows Select All, as measured on Live 12; Quantize and its settings
        // (10.5.8) have a group of their own, with Fit to Time Range (10.5.5); the group's place in
        // Live's menu is to be read on Live.
        {"Edit", {C::Undo, C::Redo, 0, C::Cut, C::Copy, C::Paste, C::Duplicate, C::DeleteTrack, C::DeleteAutomation, 0, C::CutTime, C::CopyTime,
                  C::PasteTime, C::DuplicateTime, C::DeleteTime, C::InsertSilence, 0, C::RenameTrack, C::EditInfoText, C::SelectAll,
                  C::InvertSelection, 0, C::LoopSelection, C::SelectLoop, 0, C::Split, C::CropClip, C::ConsolidateMidi, 0,
                  C::Quantize, C::QuantizeSettings, C::FitToTimeRange, 0,
                  C::GroupTracks, C::UngroupTracks}},
        // Add Locator and Delete Locator (manual 6.4) after Insert Time Signature Change, then
        // Create Fade In/Out and Delete Fades/Crossfades (6.8); their place in Live 12's Create
        // menu is to be read on Live.
        {"Create", {C::InsertAudioTrack, C::InsertMidiTrack, C::InsertReturnTrack, 0, C::InsertMidiClip, C::ImportMidiFile,
                    C::ImportAudioFile, 0,
                    C::InsertTimeSignatureChange, 0,
                    C::AddLocator, C::DeleteLocator, 0, C::CreateFade, C::DeleteFades}},
        // The show and hide toggles in §42.1's order, ADI's Channel EQ after the Mixer. Automation Mode
        // (manual 25.2) after them; its place in Live 12's menu is to be read on Live (PROVISIONAL).
        {"View",
         {C::FullScreen, 0, C::ToggleBrowser, C::ToggleOverview, kTrackControlsMenu, kMixerControlsMenu,
          C::ToggleMixerView, C::ShowChannelEq, C::ToggleMixView, C::DetachMixView, C::ToggleReferenceTrack,
          C::PlayReference, C::ToggleClipView, C::ExpandClipView, C::ToggleDeviceView, C::SwitchDetailView, C::TogglePluginWindows,
          C::ToggleInfoView, C::UndoHistory, 0, C::AutomationMode, 0, C::ZoomIn, C::ZoomOut, 0,
          C::ZoomToSelection, C::ZoomBack, C::OptimizeWidth, C::OptimizeHeight, C::LargeTracks, 0,
          C::ArrangePanelsAutomatically, C::ArrangePanelsHorizontally, C::ArrangePanelsVertically, 0,
          C::FoldToNotes, C::FoldToScale, C::HighlightScales, 0,
          C::SwapPanels}},
        // Follow (§6.2) and the grid's commands (§6.10) are the Options menu's; so is Solo in Place
        // (§18.6), in its own group after the grid's, as Live 12's menu places it (read on Live 12,
        // 5 November: Solo Switches, Cue Switches, Solo in Place, before Settings), and Settings on
        // Windows (§2.3). Solo Switches and Cue Switches are the cue path's (B6, MX-08). Delay
        // Compensation (manual 18.7) stands in its own group before them; its place among Live's
        // other Options entries is to be read on Live 12 (PROVISIONAL).
#if JUCE_MAC
        {"Options",
         {C::Follow, 0, C::DrawMode, C::ComputerMidiKeyboard, C::LockEnvelopes, 0,
          C::NarrowGrid, C::WidenGrid, C::TripletGrid, C::SnapToGrid, C::FixedGrid, 0,
          C::DelayCompensation, 0, C::SoloSwitches, C::CueSwitches, C::SoloInPlace}},
#else
        {"Options",
         {C::Follow, 0, C::DrawMode, C::ComputerMidiKeyboard, C::LockEnvelopes, 0,
          C::NarrowGrid, C::WidenGrid, C::TripletGrid, C::SnapToGrid, C::FixedGrid, 0,
          C::DelayCompensation, 0, C::SoloSwitches, C::CueSwitches, C::SoloInPlace, 0, C::Settings}},
#endif
        // The views ADI has, in Live's order (§41.4.1): Session View, the Groove Pool and the
        // Learn View come with those views; the Mixer with its keyboard.
        {"Navigate",
         {C::FocusControlBar, C::FocusArrangement, C::FocusClipView, C::FocusDeviceView, C::FocusBrowser, 0,
          C::NextNeighbor, C::PreviousNeighbor, 0, C::TabMovesFocus,
          C::WrapTabNavigation}},
        // WIN-09: the manual and the keyboard reference, so the Mac's Cmd+? search finds ADI's menus;
        // the Help View (Learn View) joins it later.
        {"Help", {C::HelpManual, C::KeyboardReference, 0, C::About}}};
    return bar;
}
} // namespace
class ShellMenu final : public juce::MenuBarModel {
  public:
    ShellMenu(AdiRootComponent &r, AppCommands &c) : root(r), commands(c) {}
    juce::StringArray getMenuBarNames() override {
        juce::StringArray names;
        for (const auto &m : menuBar())
            if (!m.items.empty())
                names.add(m.name);
        return names;
    }
    juce::PopupMenu getMenuForIndex(int, const juce::String &name) override {
        commands.activate(root);
        juce::PopupMenu menu;
        for (const auto &m : menuBar())
            if (name == m.name)
                for (const int id : m.items) {
                    if (id == 0)
                        menu.addSeparator();
                    else if (id == kTrackControlsMenu)
                        menu.addSubMenu("Arrangement Track Controls", root.trackControlsMenu());
                    else if (id == kMixerControlsMenu)
                        menu.addSubMenu("Mixer Controls", root.mixerOptionsMenu(true));
                    else if (id == AppCommands::ToggleReferenceTrack || id == AppCommands::PlayReference)
                        root.addReferenceCommand(menu, id);
                    else if (id == kOpenRecentMenu) {
                        auto recent = recentMenu();
                        menu.addSubMenu("Open Recent", std::move(recent), !recentItems_.empty());
                    } else
                        menu.addCommandItem(&commands.manager, id);
                }
        return menu;
    }
    void menuItemSelected(int id, int) override {
        if (id == kClearRecentProjects || id == kRemoveUnavailableProjects) {
            commands.clearRecentProjects(id == kRemoveUnavailableProjects);
            menuItemsChanged();
        } else if (const auto index = static_cast<std::size_t>(id - kFirstRecentProject);
                   index < recentItems_.size() && root.openProjectFile) {
            // Opening a project replaces the root and its menu. Keep a local copy of the action and path.
            auto open = root.openProjectFile;
            const auto file = recentItems_[index];
            open(file);
        }
    }

  private:
    AdiRootComponent &root;
    AppCommands &commands;
    std::vector<juce::File> recentItems_;

    juce::PopupMenu recentMenu() {
        juce::PopupMenu menu;
        recentItems_.clear();
        const auto &files = commands.recentProjects();
        for (int i = 0; i < files.getNumFiles(); ++i)
            recentItems_.push_back(files.getFile(i));
        bool unavailable = false;
        for (std::size_t i = 0; i < recentItems_.size(); ++i) {
            const auto &file = recentItems_[i];
            auto name = file.getFileNameWithoutExtension();
            if (std::count_if(recentItems_.begin(), recentItems_.end(), [&name](const auto &other) {
                    return other.getFileNameWithoutExtension() == name;
                }) > 1)
                name = file.getFullPathName();
            const bool present = file.existsAsFile();
            menu.addItem(kFirstRecentProject + static_cast<int>(i), name, present);
            unavailable |= !present;
        }
        if (!recentItems_.empty()) {
            menu.addSeparator();
            menu.addItem(kRemoveUnavailableProjects, "Remove Unavailable Projects", unavailable);
            menu.addItem(kClearRecentProjects, "Clear List");
        }
        return menu;
    }
};
juce::PopupMenu applicationMenu(AppCommands &commands) {
    juce::PopupMenu menu;
    menu.addCommandItem(&commands.manager, AppCommands::Settings);
    return menu;
}
int menuForKey(juce::MenuBarModel &bar, const juce::KeyPress &key) {
    if (key.getModifiers().withoutMouseButtons() != juce::ModifierKeys(juce::ModifierKeys::altModifier))
        return -1;
    const auto letter = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
    const auto names = bar.getMenuBarNames();
    for (int i = 0; i < names.size(); ++i)
        if (juce::CharacterFunctions::toLowerCase(names[i][0]) == letter)
            return i;
    return -1;
}
std::optional<float> nextZoom(float now, bool in) noexcept {
    const auto at = std::find(kZoomSteps.begin(), kZoomSteps.end(), now);
    const auto i = at == kZoomSteps.end() ? 2 : at - kZoomSteps.begin();
    const auto next = in ? i + 1 : i - 1;
    if (next < 0 || next >= static_cast<std::ptrdiff_t>(kZoomSteps.size()))
        return std::nullopt;
    return kZoomSteps[static_cast<std::size_t>(next)];
}
std::string displayId(const juce::Displays::Display &d) {
    const auto a = d.logicalBounds.toNearestInt();
    return std::to_string(a.getX()) + ":" + std::to_string(a.getY()) + ":" + std::to_string(a.getWidth()) + ":" +
           std::to_string(a.getHeight()) + ":" + std::to_string(static_cast<int>(d.dpi));
}
juce::StringArray launchArguments(const juce::String &commandLine) {
    juce::StringArray arguments;
    for (const auto &token : juce::StringArray::fromTokens(commandLine, true))
        if (const auto argument = token.unquoted().trim(); argument.isNotEmpty())
            arguments.add(argument);
    return arguments;
}
bool isTestLaunch(const juce::String &commandLine) {
    for (const auto &t : launchArguments(commandLine))
        if (t == "--smoke-output" || t == "--control-test")
            return true;
    return false;
}
juce::File projectsFolder(const settings::AppSettings &settings) {
    const auto chosen = settings.get("files.projectsFolder");
    auto directory =
        chosen.is_string() && !chosen.get<std::string>().empty()
            ? juce::File(juce::String::fromUTF8(chosen.get<std::string>().c_str()))
            : juce::File(juce::String::fromUTF8(
                             reinterpret_cast<const char *>(settings.file().parent_path().u8string().c_str())))
                  .getChildFile("projects");
    directory.createDirectory();
    return directory;
}
juce::File nextUntitledProject(const settings::AppSettings &settings) {
    const auto parent = projectsFolder(settings);
    for (int index = 1; ; ++index) {
        const auto name = index == 1 ? juce::String("Untitled") : "Untitled(" + juce::String(index) + ")";
        if (parent.getChildFile(name + ".adi").exists())
            continue;
        const auto directory = parent.getChildFile(name);
        const auto path = media::pathFromUtf8(directory.getFullPathName().toStdString());
        std::error_code error;
        if (std::filesystem::create_directory(path, error))
            return directory.getChildFile(name + ".adi");
        if (error && error != std::errc::file_exists)
            return {};
    }
}
juce::File songProjectFile(const juce::File &file) {
    const auto name = file.getFileNameWithoutExtension();
    if (name.isEmpty() || name == "." || name == "..")
        return {};
    const auto parent = file.getParentDirectory();
    if (file.existsAsFile() || parent == parent.getSiblingFile(name))
        return file;
    return parent.getChildFile(name).getChildFile(file.getFileName());
}
std::string displayAt(juce::Point<int> screen) {
    const auto *d = juce::Desktop::getInstance().getDisplays().getDisplayForPoint(screen.toFloat());
    return d != nullptr ? displayId(*d) : std::string();
}
void AdiRootComponent::followDisplay() {
    const auto display = displayAt(getScreenBounds().getCentre());
    if (zoomDisplay_ == display)
        return;
    zoomDisplay_ = display;
    const auto kept = commands_.keptZoom(window_, display);
    if (!kept || juce::approximatelyEqual(*kept, interfaceZoom_))
        return;
    interfaceZoom_ = *kept;
    if (auto *host = getParentComponent())
        host->resized();
}
ZoomHost::ZoomHost(std::unique_ptr<AdiRootComponent> root) : root_(std::move(root)) {
    setOpaque(true);
    addAndMakeVisible(*root_);
    setSize(root_->getWidth(), root_->getHeight());
}
void ZoomHost::resized() {
    const float z = root_->interfaceZoom();
    root_->setTransform(juce::AffineTransform::scale(z));
    root_->setBounds(0, 0, juce::roundToInt(static_cast<float>(getWidth()) / z),
                     juce::roundToInt(static_cast<float>(getHeight()) / z));
}
AdiWindow::AdiWindow(std::unique_ptr<AdiRootComponent> root, AppCommands &commands,
                     bool onDesktop)
    : DocumentWindow("ADI", look::windowBack, DocumentWindow::allButtons, onDesktop),
      root_(root.get()), commands_(commands), clock_(this, [this](double) {
          followSettings();
          root_->vblankFrame();
      }) {
    setContentOwned(new ZoomHost(std::move(root)), true);
    setResizable(true, false);
    setResizeLimits(520, 180, 10000, 10000);
    addKeyListener(this);
    // Floating tooltips are on by default after 600 ms, a deliberate deviation
    // the director confirmed by name (ADR-0203 d12, ADR-0131); one per window.
    followSettings();
    commands_.activate(*root_);
    menu_ = std::make_unique<ShellMenu>(*root_, commands_);
    // On the Mac the menu is the application's, at the top of the screen, as Live's,
    // with Settings in the application menu.
#if JUCE_MAC
    if (onDesktop) {
        const auto appMenu = applicationMenu(commands_);
        juce::MenuBarModel::setMacMainMenu(menu_.get(), &appMenu);
    }
#else
    setMenuBar(menu_.get());
#endif
}
void AdiWindow::visibilityChanged() {
    if (!quiet)
        DocumentWindow::visibilityChanged();
}
void AdiWindow::activeWindowStatusChanged() {
    DocumentWindow::activeWindowStatusChanged();
    if (root_ != nullptr && !isActiveWindow())
        root_->releaseComputerMidiKeys();
}
void AdiWindow::moved() {
    DocumentWindow::moved();
    root_->followDisplay();
    place.moved();
}
void AdiWindow::resized() {
    DocumentWindow::resized();
    place.moved();
}
void AdiWindow::parentSizeChanged() {
    DocumentWindow::parentSizeChanged();
    if (!isFullScreen() && keepReachable(*this))
        place.moved();
}
void AdiWindow::broughtToFront() {
    DocumentWindow::broughtToFront();
    // The owned windows stay above the main window (WIN-03).
    if (root_->pluginHost != nullptr)
        root_->pluginHost->ownerToFront();
}
void AdiWindow::minimisationStateChanged(bool minimised) {
    // The owned windows go down and come back with it (WIN-03).
    if (root_->pluginHost != nullptr)
        root_->pluginHost->ownerMinimised(minimised);
    root_->infoSpectrum.suspend(minimised);
    root_->devices.suspendParameters(minimised);
    if (root_->mixViewPanel.getParentComponent() == root_) {
        root_->mixViewPanel.suspendAnalysis(minimised);
        root_->mixViewPanel.stack.suspendParameters(minimised || !root_->state().mixView);
        if (minimised) root_->mixViewPanel.stack.cancelInteractions();
    }
    if (minimised) root_->devices.cancelFunctionInteractions();
}
AdiWindow::~AdiWindow() {
#if JUCE_MAC
    if (juce::MenuBarModel::getMacMainMenu() == menu_.get())
        juce::MenuBarModel::setMacMainMenu(nullptr);
#else
    setMenuBar(nullptr);
#endif
    removeKeyListener(this);
}
juce::MenuBarModel *AdiWindow::menu() noexcept { return menu_.get(); }
void AdiWindow::followSettings() { followTooltips(*this, commands_, tips_, tipDelayMs_); }
juce::String windowTooltipLabel(const juce::Component &component) {
    return static_cast<bool>(component.getProperties()[controls::iconTooltip]) ? component.getTitle() : juce::String{};
}
namespace {
class IconTooltipWindow final : public juce::TooltipWindow {
  public:
    using juce::TooltipWindow::TooltipWindow;
  private:
    juce::String getTipFor(juce::Component &component) override {
        const auto label = windowTooltipLabel(component);
        // JUCE still owns foreground, modal and button-down suppression.
        return label.isNotEmpty() && juce::TooltipWindow::getTipFor(component).isNotEmpty() ? label : juce::String{};
    }
};
} // namespace
void followTooltips(juce::Component &window, const AppCommands &commands,
                    std::unique_ptr<juce::TooltipWindow> &tips, int &delayMs) {
    if (!commands.tooltips()) {
        tips.reset();
        return;
    }
    if (tips == nullptr) {
        delayMs = commands.tooltipDelayMs();
        tips = std::make_unique<IconTooltipWindow>(&window, delayMs);
    } else if (delayMs != commands.tooltipDelayMs()) {
        delayMs = commands.tooltipDelayMs();
        tips->setMillisecondsBeforeTipAppears(delayMs);
    }
}
bool AdiWindow::keyPressed(const juce::KeyPress &key, juce::Component *) {
#if !JUCE_MAC
    // Alt and a menu's first letter opens it (§42.28); JUCE's menus then take the arrows,
    // to move through a menu and on to its neighbours, and Enter.
    if (const int index = menuForKey(*menu_, key); index >= 0) {
        if (auto *bar = dynamic_cast<juce::MenuBarComponent *>(getMenuBarComponent()))
            bar->showMenu(index);
        return true;
    }
#endif
    return commands_.key(key, *root_);
}
bool AdiWindow::keyStateChanged(bool isKeyDown, juce::Component *) {
    // A held key comes up wherever the focus is: the root lets it go.
    return root_->keyStateChanged(isKeyDown);
}
AdiRootComponent::ImportPrompt::ImportPrompt(std::size_t files, const ImportChoices &choices, Reply reply)
    : title_(files == 1 ? juce::String("Import Audio") : "Import " + juce::String(static_cast<int>(files)) + " Audio Files"),
      answer_(std::move(reply)) {
    for (auto *b : {&warp, &fades, &copy, &stopAsking})
        addAndMakeVisible(*b);
    stopAsking.setTooltip("To show this prompt again, set Audio import options to Ask in Settings > Editing.");
    warp.setToggleState(choices.warp, juce::dontSendNotification);
    fades.setToggleState(choices.edgeFades, juce::dontSendNotification);
    copy.setToggleState(choices.copyIn, juce::dontSendNotification);
    // Live's Warp modes, in Live's order (manual 9.2.1); ADI's stretch is its own (ADR-0061).
    int item = 1;
    for (const auto &choice : kWarpModeChoices) {
        mode.addItem(choice.name, item);
        if (choice.mode == choices.mode)
            mode.setSelectedId(item, juce::dontSendNotification);
        ++item;
    }
    mode.setEnabled(choices.warp);
    warp.onClick = [this] { mode.setEnabled(warp.getToggleState()); };
    addAndMakeVisible(mode);
    addAndMakeVisible(import);
    addAndMakeVisible(cancel);
    import.onClick = [this] { this->answer(true); };
    cancel.onClick = [this] { this->answer(false); };
    setWantsKeyboardFocus(true);
}
ImportChoices AdiRootComponent::ImportPrompt::choices() const {
    ImportChoices out;
    out.warp = warp.getToggleState();
    out.edgeFades = fades.getToggleState();
    out.copyIn = copy.getToggleState();
    if (const auto id = mode.getSelectedId(); id >= 1 && id <= static_cast<int>(kWarpModeChoices.size()))
        out.mode = kWarpModeChoices[static_cast<std::size_t>(id - 1)].mode;
    return out;
}
void AdiRootComponent::ImportPrompt::answer(bool importing) {
    if (!answer_)
        return;
    auto reply = std::move(answer_);
    answer_ = nullptr;
    reply(importing ? std::optional<ImportChoices>(choices()) : std::nullopt,
          importing && stopAsking.getToggleState());
}
bool AdiRootComponent::ImportPrompt::keyPressed(const juce::KeyPress &key) {
    if (key == juce::KeyPress::returnKey) {
        answer(true);
        return true;
    }
    if (key == juce::KeyPress::escapeKey) {
        answer(false);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::tabKey) {
        // Round its own controls, whatever the window's Tab setting.
        std::array<juce::Component *, 7> order{&warp, &mode, &fades, &copy, &stopAsking, &import, &cancel};
        const auto at = std::find(order.begin(), order.end(), getCurrentlyFocusedComponent());
        const auto step = key.getModifiers().isShiftDown() ? order.size() - 1 : 1;
        const auto from = at == order.end() ? (step == 1 ? order.size() - 1 : 0)
                                            : static_cast<std::size_t>(at - order.begin());
        for (std::size_t n = 1; n <= order.size(); ++n) {
            auto *next = order[(from + n * step) % order.size()];
            if (next->isEnabled()) {
                focusIfShowing(*next);
                break;
            }
        }
        return true;
    }
    return true;
}
void AdiRootComponent::ImportPrompt::paint(juce::Graphics &g) {
    // A title bar above the choices.
    g.setColour(look::panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), look::radiusControl);
    g.setColour(look::field);
    g.fillRect(getLocalBounds().withHeight(24).reduced(1, 1));
    g.setColour(look::text);
    g.setFont(look::font(look::fontHeading, true));
    g.drawText(title_, getLocalBounds().withHeight(24).reduced(8, 0), juce::Justification::centredLeft);
    g.setColour(look::separator);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), look::radiusControl, look::hairline);
}
void AdiRootComponent::ImportPrompt::resized() {
    auto r = getLocalBounds().withTrimmedTop(32).reduced(10, 0);
    auto row = r.removeFromTop(26);
    warp.setBounds(row.removeFromLeft(90));
    mode.setBounds(row.removeFromLeft(150).reduced(0, 2));
    fades.setBounds(r.removeFromTop(26));
    copy.setBounds(r.removeFromTop(26));
    stopAsking.setBounds(r.removeFromTop(26));
    auto foot = getLocalBounds().removeFromBottom(40).reduced(10, 8);
    cancel.setBounds(foot.removeFromRight(80));
    foot.removeFromRight(8);
    import.setBounds(foot.removeFromRight(80));
}
void AdiRootComponent::addReenableAutomation(juce::PopupMenu &menu, const engine::AutomationLaneNode *lane) {
    if (lane == nullptr)
        return;
    if (menu.getNumItems() > 0)
        menu.addSeparator();
    const auto id = lane->id;
    menu.addItem("Re-Enable Automation", lane->overridden, false, [safe = SafePointer<AdiRootComponent>(this), id] {
        if (!safe)
            return;
        if (!safe->reenableLane)
            safe->say("Re-Enable Automation needs the project's session.");
        else
            safe->reenableLane(id); // a refusal says why
    });
}
} // namespace adi::ui
