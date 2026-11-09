// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_shell.hpp"
#include "adi/ui/meter_edits.hpp"
#include "adi/ui/meters.hpp"
#include "adi/ui/tempo_edits.hpp"
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <vector>
namespace adi::ui {
namespace {
// A letter's key is down whichever case its code names.
bool keyIsDown(int code) {
    return juce::KeyPress::isKeyCurrentlyDown(code) ||
           (code > 0 && code < 128 && std::isalpha(code) &&
            (juce::KeyPress::isKeyCurrentlyDown(std::toupper(code)) ||
             juce::KeyPress::isKeyCurrentlyDown(std::tolower(code))));
}
// Live's measures, in points at 100 % (ui_look.hpp): the control bar, the Status Bar,
// the AI tab, the Info View's width and the browser's preview strip.
constexpr int kTransportHeight = look::controlBarHeight, kStatusHeight = look::statusBarHeight,
              kTabHeight = 20;
constexpr int kInfoWidth = 200, kPreviewHeight = 27;
// Interface zoom steps per window. PROVISIONAL: Live's own list is to be
// measured (ADR-0203 d26); 100 % is the start and the steps are symmetric.
constexpr std::array<float, 7> kZoomSteps{.5f, .75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f};
bool flag(const settings::AppSettings &s, const char *key, bool fallback) {
    const auto v = s.get(key);
    return v.is_boolean() ? v.get<bool>() : fallback;
}
} // namespace
AppCommands::AppCommands(settings::AppSettings &settings) : settings_(settings) {
    manager.setFirstCommandTarget(this);
    reload();
}
void AppCommands::reload() {
    live_ = settings_.get("shortcuts.preset") == "live";
    infoView_ = flag(settings_, "lookfeel.showInfoView", true);
    tooltips_ = flag(settings_, "lookfeel.showTooltips", true);
    const auto delay = settings_.get("lookfeel.tooltipDelayMs");
    tooltipDelayMs_ = delay.is_number_integer() ? delay.get<int>() : 600;
    zoomOnSelection_ = flag(settings_, "lookfeel.zoomOnSelection", true);
    tabMovesFocus_ = flag(settings_, "lookfeel.tabMovesFocus", false);
    wrapTab_ = flag(settings_, "lookfeel.wrapTabNavigation", true);
    arrowKeysMoveClips_ = flag(settings_, "editing.arrowKeysMoveClips", true);
    followByPage_ = settings_.get("lookfeel.followMode") != "scroll";
    exclusiveSolo_ = flag(settings_, "record.exclusiveSolo", true);
    linkLoop_ = flag(settings_, "editing.linkLoopToSelection", false);
    // A preset changes default keys, so the old registrations go first.
    manager.clearCommands();
    manager.registerAllCommandsForTarget(this);
    manager.getKeyMappings()->resetToDefaultMappings();
}
bool AppCommands::setInfoView(bool on, std::string &why) {
    if (!settings_.set("lookfeel.showInfoView", on, {}, why) || !settings_.save(why))
        return false;
    infoView_ = on;
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
bool AppCommands::setWrapTabNavigation(bool on, std::string &why) {
    if (!settings_.set("lookfeel.wrapTabNavigation", on, {}, why) || !settings_.save(why))
        return false;
    wrapTab_ = on;
    return true;
}
void AppCommands::getAllCommands(juce::Array<juce::CommandID> &ids) {
    for (int id = PlayStop; id <= LastId; ++id)
        ids.add(id);
}
void AppCommands::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo &info) {
    if (id < PlayStop || id > LastId) {
        info.setActive(false);
        return;
    }
    static const char *names[] = {"Play / stop",         "Stop",
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
                                  "Undo History"};
    info.setInfo(names[id - PlayStop], names[id - PlayStop], "ADI", 0);
    if (id == Undo || id == Redo) {
        const bool redo = id == Redo;
        info.setInfo(active_ ? juce::String(active_->undoTitle(redo))
                             : juce::String(names[id - PlayStop]),
                     names[id - PlayStop], "ADI", 0);
        info.setActive(active_ != nullptr && active_->canUndo(redo));
    }
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto alt = juce::ModifierKeys::altModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    // Live's Activate/Deactivate Track 1…8 (§42.19): F1 to F8.
    if (id >= ActivateTrack1 && id <= ActivateTrack8) {
        static const std::array<int, 8> keys{juce::KeyPress::F1Key, juce::KeyPress::F2Key, juce::KeyPress::F3Key,
                                             juce::KeyPress::F4Key, juce::KeyPress::F5Key, juce::KeyPress::F6Key,
                                             juce::KeyPress::F7Key, juce::KeyPress::F8Key};
        info.addDefaultKeypress(keys[static_cast<std::size_t>(id - ActivateTrack1)], 0);
        info.setActive(active_ != nullptr);
        return;
    }
    // Live 12 manual §41.1, §41.5, §41.9 and §41.20. Esc stops nothing: in Live
    // it closes a window or a dialog, and the window lane builds Live's keys.
    switch (id) {
    case PlayStop:
        info.addDefaultKeypress(juce::KeyPress::spaceKey, 0);
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
    case ToggleDeviceView:
        info.addDefaultKeypress('4', cmd | alt);
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
    // §41.3. Ctrl/Cmd+S takes a named snapshot rather than saving, a deviation the
    // director ruled (ADR-0203 d6, d12); Save As is Live's, a true one.
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
    case DeleteTrack:
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
        info.setActive(active_ != nullptr && active_->arrangement.geometry.selection.track != 0);
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
        info.setActive(active_ != nullptr && active_->arrangement.geometry.selection.track != 0);
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
    case ToggleReferenceTrack: // ADR-0209 §4, d11: the reference track, hidden by default
        info.setTicked(active_ != nullptr && active_->state().showReference);
        break;
    case PlayReference: // ADR-0209 d9: [REF] and Shift+R; Ctrl+R stays Rename
        info.addDefaultKeypress('r', shift);
        info.setTicked(active_ != nullptr && active_->reference().on());
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
    case SoloInPlace: // manual 18.6: the Options menu's entry, ticked from the project's field (#412)
        info.setTicked(active_ != nullptr && active_->reader().soloInPlace());
        info.setActive(active_ != nullptr);
        break;
    case DeactivateSelected: // §42.19: Deactivate Selected Track, 0 (the Arrangement takes 0 for clips first)
        info.addDefaultKeypress('0', 0);
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
        info.setActive(snapshot != nullptr && (id == GroupTracks ? canGroup(snapshot->tracks, track)
                                                                 : canUngroup(snapshot->tracks, track)));
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
    case FocusDeviceView:
        info.addDefaultKeypress('4', alt);
        break;
    case FocusBrowser:
        info.addDefaultKeypress('5', alt);
        break;
    case SearchBrowser: // §41.4.1.6, §42.22
        info.addDefaultKeypress('f', cmd);
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
    default:
        break;
    }
}
bool AppCommands::key(const juce::KeyPress &key, AdiRootComponent &root) {
    activate(root);
    // A latching key runs its command once while it is held (Live 42.25): the root times it. One let
    // go where the window could not see it (the focus elsewhere) goes down anew, and nothing goes back.
    if (const auto id = manager.getKeyMappings()->findCommandForKeyPress(key); AdiRootComponent::latches(id)) {
        if (root.latch().held() && !keyIsDown(root.latch().key()))
            root.letGoUnseen();
        return root.latchKey(id, key.getKeyCode(), true, static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
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
    for (auto *b : {&infoToggle, &detailToggle}) {
        addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    }
    infoToggle.setTitle("Info View");
    infoToggle.setTooltip("Shows or hides the Info View. Shift+?.");
    infoToggle.onClick = [this] { root_.command(AppCommands::ToggleInfoView); };
    detailToggle.setTitle("Detail View");
    detailToggle.setTooltip("Shows or hides the Device View. Ctrl/Cmd+Alt+4.");
    detailToggle.onClick = [this] { root_.command(AppCommands::ToggleDeviceView); };
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
}
void StatusBar::resized() {
    // Live's Status Bar at 100 %: round corner buttons, the message field, a second
    // field, the selected track's box; 22 pt fields with the frame's 8 pt around them.
    const int w = getWidth();
    infoToggle.setBounds(8, 9, 19, 19);
    detailToggle.setBounds(w - 27, 9, 19, 19);
    mixerToggle.setBounds(w - 86, 11, 36, 16);
    mixerOptions.setBounds(w - 49, 11, 8, 16);
}
void StatusBar::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    const int w = getWidth();
    const juce::Rectangle<int> message(35, 8, std::max(0, w - 419), 22);
    const juce::Rectangle<int> second(w - 376, 8, 206, 22), track(w - 162, 8, 124, 22);
    for (auto r : {message, second, track}) {
        g.setColour(look::band);
        g.fillRect(r);
        g.setColour(look::panel);
        g.fillRect(r.reduced(3));
    }
    // The Mixer View Control's arrow, pointing down to its menu.
    look::triangle(g, mixerOptions.getBounds().toFloat().withSizeKeepingCentre(7.f, 5.f),
                   juce::MathConstants<float>::halfPi, look::text);
    // The corner buttons: dark and round, with Live's light triangles.
    for (auto *b : {&infoToggle, &detailToggle}) {
        const auto c = b->getBounds().toFloat();
        g.setColour(look::band);
        g.fillEllipse(c);
        look::triangle(g, c.reduced(5.f), -juce::MathConstants<float>::halfPi, look::panel);
    }
    const bool error = !root_.error().empty();
    g.setColour(look::text);
    g.setFont(look::font(12.5f));
    g.drawText(error ? juce::String(root_.error()) : root_.selectionText(), message.reduced(8, 0),
               juce::Justification::centredLeft);
    // The selected track's name, as Live shows it at the right.
    const auto rows = root_.arrangement.geometry.rows(root_.reader());
    if (const auto i = rows.find(root_.arrangement.geometry.selection.track); i < rows.size()) {
        g.setFont(look::font(11.5f));
        g.drawText(juce::String::fromUTF8(displayName(rows[i].track->name, i + 1).c_str()),
                   track.reduced(6, 0).withWidth(44), juce::Justification::centredLeft);
    }
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
    undoHistory.setLookAndFeel(&look::liveLook());
    undoHistory.read = [this] { return submitter_.timeline(); };
    undoHistory.restore = [this](std::int64_t txn) { return restoreStep(txn); };
    setOpaque(true);
    setWantsKeyboardFocus(true);
    // Tab moves within the window's own controls, and Wrap Tab Navigation wraps or stops at
    // their ends (§41.1.1), whatever else shares the native window.
    setFocusContainerType(FocusContainerType::keyboardFocusContainer);
    addAndMakeVisible(transport);
    addAndMakeVisible(status);
    addAndMakeVisible(browser);
    addAndMakeVisible(aiTab);
    addAndMakeVisible(arrangement);
    addAndMakeVisible(devices);
    addAndMakeVisible(dockResizer);
    addAndMakeVisible(mixer);
    addAndMakeVisible(strip);
    strip.flip.onClick = [this] { command(AppCommands::SwapPanels); };
    strip.tracks.onClick = [this] { command(AppCommands::LargeTracks); };
    // Live's selectors at the strip's foot repeat the View menu's toggles (manual 18.1).
    strip.inOut.onClick = [this] { command(AppCommands::ToggleInOut); };
    strip.returns.onClick = [this] { command(AppCommands::ToggleReturnTracks); };
    strip.delay.onClick = [this] { command(AppCommands::ToggleTrackOptions); };
    // The Overview spans the track headers too, as Live's (ui_arrangement.hpp).
    addAndMakeVisible(arrangement.overview);
    addChildComponent(mixerView);
    addChildComponent(mixerViewResizer);
    addChildComponent(mixViewPanel);
    arrangement.geometry.left = state_.timelineLeft;
    arrangement.geometry.scale = state_.pixelsPerQuarter;
    arrangement.geometry.height = state_.laneHeight;
    arrangement.geometry.heights = state_.laneHeights;
    arrangement.geometry.largeTracks = state_.largeTracks;
    arrangement.geometry.grid = {state_.gridSnap, state_.gridFixed, state_.gridTriplet, state_.gridNarrow,
                                 state_.gridFixedQuarters};
    aiTab.setColour(juce::TextButton::buttonColourId, look::columnHead);
    aiTab.setTitle("AI");
    aiTab.setTooltip("The AI chat. Opens at the foot of the browser.");
    aiTab.onClick = [this] {
        state_.aiChatOpen = !state_.aiChatOpen;
        if (!persist()) {
            state_.aiChatOpen = !state_.aiChatOpen;
            fail(error_);
            return;
        }
        resized();
    };
    reader_.emplace(view_.current());
    setSize(1200, 720);
    frame();
}
AdiRootComponent::~AdiRootComponent() {
    commands_.detach(*this);
    setLookAndFeel(nullptr);
    undoHistory.setLookAndFeel(nullptr);
}
void AdiRootComponent::frame() noexcept {
    // Root owns exactly one reader for the presented frame; descendants borrow it.
    // JUCE may paint after this callback, so it remains alive until the next frame.
    reader_.emplace(view_.current());
    const auto generation = view_.generation();
    if (generation_ != generation) {
        generation_ = generation;
        // The project view's tree follows the project: tracks come and go, groups with them.
        arrangement.applyView();
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
    // The Info View is app-scoped: a toggle in another window shows here too.
    if (infoShown_ != commands_.infoView()) {
        infoShown_ = commands_.infoView();
        resized();
    }
    if (infoShown_)
        hover(juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse());
    // Live's Follow (§6.2): the arrangement keeps the playhead on screen while it plays,
    // unless an edit, a sideways scroll or a click on the ruler paused it.
    if (state_.follow && playing_ && !arrangement.followPaused &&
        arrangement.follow(timelineTick(), commands_.followByPage()))
        dirty_.mark(DirtySet::Content);
    lastDrain_ = dirty_.drain();
    arrangement.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    devices.frame(arrangement.geometry.selection.track);
    mixer.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    if (mixerView.isVisible())
        mixerView.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    // In its own window the floating host drives its frame.
    if (mixViewPanel.isVisible() && mixViewPanel.getParentComponent() == this)
        mixViewPanel.frame(reader());
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
bool AdiRootComponent::locate(std::int64_t tick) {
    // Landing reads the current cell, not this window's presented frame.
    const auto current = view_.current();
    const double rate = mailbox_.sampleRate() > 0 ? mailbox_.sampleRate() : current->sampleRate;
    if (!current->tempo || !mailbox_.hasRoom())
        return false;
    return mailbox_.post(TransportMailbox::Command::Locate,
                         static_cast<std::int64_t>(current->tempo->ticksToSeconds(tick) * rate));
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
    if (!locate(tick))
        return fail("The project has no tempo map to locate in.");
    if (start)
        mailbox_.post(TransportMailbox::Command::Play);
    error_.clear();
    dirty_.mark(DirtySet::Transport | DirtySet::Status);
    return true;
}
double AdiRootComponent::bpm() const noexcept {
    const auto *tempo = reader_->tempo();
    const double rate = sampleRate_ > 0 ? sampleRate_ : reader_->sampleRate();
    return tempo ? tempo->bpmAt(
                       tempo->secondsToTicks(rate > 0 ? static_cast<double>(position_) / rate : 0))
                 : 120.;
}
void AdiRootComponent::resized() {
    const int w = getWidth(), h = getHeight(), gap = look::gap;
    transport.setBounds(0, 0, w, kTransportHeight);
    status.setBounds(0, h - kStatusHeight, w, kStatusHeight);
    const int top = kTransportHeight, bottom = h - kStatusHeight;
    const int body = std::max(0, bottom - top);
    // The detail row: Live's Info View and Device View. The strip never goes below its
    // floor and grows upward only (ADR-0203 d7); it leaves the transport, the ruler and
    // a row when it can.
    const int dock = state_.docked ? std::min(body, std::max(state_.deviceFloor,
                                                             std::min(state_.deviceHeight, body - 100)))
                                   : 0;
    const int row = dock > 0 ? dock : (infoShown_ ? std::min(body, state_.deviceFloor) : 0);
    const int rowY = bottom - row;
    // Live's Info View sits in the bottom-left corner beside the Device View.
    const int infoW = infoShown_ ? std::min(kInfoWidth, w / 2) : 0;
    infoArea_ = infoShown_ ? juce::Rectangle<int>(gap, rowY, infoW, row) : juce::Rectangle<int>();
    const int deviceX = infoShown_ ? infoArea_.getRight() + gap : gap;
    deviceArea_ = dock > 0 ? juce::Rectangle<int>(deviceX, rowY, w - deviceX - gap, dock)
                           : juce::Rectangle<int>();
    // Each panel has Live's dark 4 pt edge, which the root draws; the strip sits inside.
    // Widths belong to panels (ADR-0080 d2). If the window shrinks, share the
    // lost space proportionally above each panel's own minimum; the centre
    // keeps its minimum. Stored preferences survive a temporary resize.
    const auto &slots = state_.panels.slots();
    // Without In/Out, the track controls are narrower by its column, as Live's (§42.1); the
    // stored width is kept for when it comes back.
    auto shownWidth = [this](const PanelGeometry &slot, int width) {
        const bool roomForInOut =
            width >= MixerPanel::kInOutWidth + MixerPanel::kControlsWidth + MixerPanel::kRightStrip;
        return slot.panel == Panel::Mixer && !state_.inOut && roomForInOut ? width - MixerPanel::kInOutWidth
                                                                           : width;
    };
    int left = shownWidth(slots[0], state_.panels.occupied(slots[0]));
    int right = shownWidth(slots[1], state_.panels.occupied(slots[1]));
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
    const int bw = browserLeft ? left : right;
    browserArea_ = bw > 0 ? juce::Rectangle<int>(browserLeft ? gap : w - right, top, bw - gap, mid)
                          : juce::Rectangle<int>();
    browser.setVisible(!browserArea_.isEmpty());
    browser.setBounds(browserArea_);
    // The AI chat is a tab at the browser's foot, collapsed by default, so the
    // column looks as Live's (ADR-0203 d6). Open, it takes the lower part.
    if (browserArea_.isEmpty()) {
        chatArea_ = {};
        aiTab.setVisible(false);
    } else {
        // ADI's addition (Master Reference 3.1): the tab sits at the foot of the
        // browser's panes, above Live's preview strip.
        const int inner = browserArea_.getHeight() - kPreviewHeight - look::border;
        const int chatH = state_.aiChatOpen ? std::max(kTabHeight, inner * 2 / 5) : kTabHeight;
        chatArea_ = juce::Rectangle<int>(browserArea_.getX() + look::border, browserArea_.getY() + inner - chatH,
                                         browserArea_.getWidth() - 2 * look::border, chatH);
        aiTab.setVisible(true);
        aiTab.setBounds(chatArea_.withHeight(kTabHeight));
        browser.setFootLimit(chatArea_.getY() - browserArea_.getY());
    }
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
    strip.flip.setToggleState(!browserLeft, juce::dontSendNotification);
    strip.tracks.setToggleState(state_.largeTracks, juce::dontSendNotification);
    strip.inOut.setToggleState(state_.inOut, juce::dontSendNotification);
    strip.returns.setToggleState(!state_.hideReturns, juce::dontSendNotification);
    strip.delay.setToggleState(state_.trackOptions, juce::dontSendNotification);
    arrangement.setBounds(centreX, top, centreW, mid - mixerViewArea_.getHeight());
    // The Mix View, docked (ADR-0209, UI-ARCHITECTURE §12): in place of the arrangement, its Mixer
    // View and its track controls, which a lane-less view has no use for. The browser, the strip
    // down the edge and the Detail View stay.
    mixViewDock_ = juce::Rectangle<int>(centreX, top, centreW, mid).getUnion(mixerArea_);
    mixViewArea_ = state_.mixView && !state_.mixViewDetached ? mixViewDock_ : juce::Rectangle<int>();
    const bool mixing = !mixViewArea_.isEmpty();
    // In its own window the panel is the window's; docked, it is laid out here. Framed before it shows,
    // so the switch never shows what it held when it was last open (the director's R3).
    if (mixViewPanel.getParentComponent() == this) {
        mixViewPanel.setBounds(mixViewArea_);
        if (mixing && !mixViewPanel.isVisible() && reader_)
            mixViewPanel.frame(reader());
        mixViewPanel.setVisible(mixing);
    }
    arrangement.setVisible(!mixing);
    if (mixing) {
        mixer.setVisible(false);
        mixerView.setVisible(false);
        mixerViewResizer.setVisible(false);
    }
    transport.mix.setToggleState(state_.mixView, juce::dontSendNotification);
    // The Overview runs from the lanes' left edge to Live's H and W buttons.
    const int overviewX = lanesX + ArrangementView::kLeftMargin;
    const int overviewRight = browserLeft && !mixerArea_.isEmpty()
                                  ? mixerArea_.getRight() - 48
                                  : centreX + centreW - 4;
    arrangement.overview.setBounds(overviewX, top + 4, std::max(0, overviewRight - overviewX),
                                   ArrangementView::kOverviewHeight - 4);
    arrangement.overview.setVisible(state_.overview && !mixing);
    devices.setVisible(!deviceArea_.isEmpty());
    dockResizer.setVisible(!deviceArea_.isEmpty());
    devices.setBounds(deviceArea_.reduced(look::border));
    dockResizer.setBounds(deviceArea_.getX(), deviceArea_.getY() - 3, deviceArea_.getWidth(), 6);
    dockResizer.toFront(false);
    dirty_.mark(DirtySet::All);
}
void AdiRootComponent::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    // Live's panels have a dark 4 pt edge with rounded corners.
    auto panel = [&g](juce::Rectangle<int> r) {
        g.setColour(look::band);
        g.fillRoundedRectangle(r.toFloat(), 5.f);
    };
    if (!deviceArea_.isEmpty())
        panel(deviceArea_);
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
        g.setFont(look::font(12.f, true));
        g.drawText(title, titleBar.reduced(5, 0), juce::Justification::centredLeft);
        // Live's body text: small, on a 15 pt line.
        juce::AttributedString words(body);
        words.setFont(look::font(11.f));
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
    if (!chatArea_.isEmpty() && state_.aiChatOpen) {
        g.setColour(look::panel);
        g.fillRect(chatArea_);
    }
}
bool AdiRootComponent::keyPressed(const juce::KeyPress &key) {
    // With Use Tab Key to Move Focus off, Live's default (§41.1.1), Tab and Shift+Tab move
    // nothing. In Live they switch to Session View, which ADI does not have yet.
    const auto mods = key.getModifiers();
    const bool plainTab = key.getKeyCode() == juce::KeyPress::tabKey && !mods.isCommandDown() &&
                          !mods.isCtrlDown() && !mods.isAltDown();
    if (plainTab && !commands_.tabMovesFocus())
        return true;
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
void AdiRootComponent::resizeMixerView(int height) {
    const int body = getHeight() - kTransportHeight - kStatusHeight - deviceArea_.getHeight();
    state_.mixerViewHeight =
        std::clamp(height, 120,
                   std::max(120, body - arrangement.topHeight() - state_.laneHeight));
    resized();
}
void AdiRootComponent::resizeDevices(int height) {
    const int ceiling = getHeight() - kTransportHeight - kStatusHeight - look::gap -
                        arrangement.topHeight() - state_.laneHeight;
    state_.setDeviceHeight(std::min(height, ceiling));
    resized();
}
bool AdiRootComponent::persist() { return persistence_.save(window_, state_, error_); }
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
    menu.addItem(juce::PopupMenu::Item("Volume").setTicked(true).setEnabled(false));
    if (viewMenu)
        menu.addSeparator();
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleMixerTrackOptions, "Track Options");
    menu.addItem(juce::PopupMenu::Item("Crossfader").setEnabled(false));
    menu.addItem(juce::PopupMenu::Item("Performance Impact").setEnabled(false));
    menu.addSeparator();
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleReturnTracks);
    return menu;
}
juce::PopupMenu AdiRootComponent::trackControlsMenu() {
    // Live 12's Arrangement Track Controls submenu (the View menu; manual 6.1: "You can customize
    // which controls are shown"; 18.7: Track Options holds Track Delay): the track controls' In/Out
    // and Sends, with their keys. Volume always shows in ADI; Track Options is the rows' Track Delay
    // box, the D selector's (Live 11's). Live 12's full list is to be measured: its 6.1 says track
    // delays are the mixer's only. First, ADI's side panel of track controls, shown or hidden whole,
    // which the View menu held as "Arrangement Track Controls" before the submenu.
    commands_.activate(*this);
    juce::PopupMenu menu;
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleMixer, "Hide/Show Track Controls");
    menu.addSeparator();
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleInOut);
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleSends);
    menu.addItem(juce::PopupMenu::Item("Volume").setTicked(true).setEnabled(false));
    menu.addCommandItem(&commands_.manager, AppCommands::ToggleTrackOptions);
    return menu;
}
void AdiRootComponent::setMixProvider(const MixViewProvider &provider) {
    mixProvider_ = &provider;
    arrangement.applyView();
    arrangement.firstShownIfHidden();
    dirty_.mark(DirtySet::All);
}
std::int64_t AdiRootComponent::referenceTrack() const noexcept { return mixProvider_->reference(reader().tracks()); }
bool AdiRootComponent::referenceKey(bool down, std::int64_t nowMs) {
    const bool was = reference_.on();
    if (down)
        reference_.press(nowMs);
    else
        reference_.release(nowMs);
    if (reference_.on() == was)
        return false;
    // What sounds is the engine's: the reference on the cue path, the Main bypassed (N23).
    say(!reference_.on()             ? "Reference off."
        : referenceTrack() == 0     ? "No track is the Reference yet: a track's role arrives with the format (N38)."
                                    : "Reference on: it plays on the cue path once the engine routes it (N23).");
    dirty_.mark(DirtySet::Layout);
    return true;
}
bool AdiRootComponent::keyStateChanged(bool isKeyDown) {
    const auto now = static_cast<std::int64_t>(juce::Time::getMillisecondCounter());
    // A latching key let go: after a hold, the window goes back (Live 42.25).
    bool handled = false;
    if (latch_.held() && !keyIsDown(latch_.key()))
        handled = latchKey(0, latch_.key(), false, now);
    // Shift+R let go: a hold of the ★ Reference ends, a tap latches it (ADR-0209 d9).
    if (reference_.held() && !keyIsDown('r'))
        return referenceKey(false, now) || handled;
    return handled || juce::Component::keyStateChanged(isKeyDown);
}
bool AdiRootComponent::latches(int command) noexcept {
    return command == AppCommands::SoloSelected || command == AppCommands::ZoomToSelection ||
           (command >= AppCommands::ActivateTrack1 && command <= AppCommands::ActivateTrack8);
}
bool AdiRootComponent::latchKey(int id, int keyCode, bool down, std::int64_t nowMs) {
    if (down) {
        // The first press acts; a repeat while the key is held does nothing.
        if (!latch_.press(keyCode, id, nowMs))
            return true;
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
    // Only the step the press made, while it is still the latest: an edit since then stays, and so
    // does what the press made under it. Going back pauses no Follow, as an undo from the menu does.
    const auto last = submitter_.nextUndo();
    if (back->step == 0 || !last || last->txnId != back->step)
        return false;
    const bool paused = arrangement.followPaused;
    const bool done = command(AppCommands::Undo);
    arrangement.followPaused = paused;
    return done;
}
bool AdiRootComponent::setMixView(bool open, bool detached) {
    const bool wasOpen = state_.mixView, wasDetached = state_.mixViewDetached;
    state_.mixView = open;
    state_.mixViewDetached = detached;
    if (!persist()) {
        state_.mixView = wasOpen;
        state_.mixViewDetached = wasDetached;
        return fail(error_);
    }
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
    dirty_.mark(DirtySet::Layout);
    return true;
}
void AdiRootComponent::publishSelection() {
    std::string why;
    if (!session_.select(arrangement.geometry.selection, why))
        fail(why);
    selectionGeneration_ = session_.generation();
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
    error_ = std::move(message);
    dirty_.mark(DirtySet::Status);
}
juce::String AdiRootComponent::selectionText() const {
    // Live's Status Bar: the insert marker with its time, or the time selection.
    // Positions and lengths in the project's meters (#299).
    const auto &s = arrangement.geometry.selection;
    const auto &meters = reader_->meters();
    auto where = [&meters](std::int64_t tick) {
        const auto at = barPosition(tick, meters);
        return juce::String(at.bar) + "." + juce::String(at.beat) + "." + juce::String(at.sixteenth);
    };
    auto seconds = [this](std::int64_t tick) {
        const auto *tempo = reader_->tempo();
        const int t = tempo ? static_cast<int>(tempo->ticksToSeconds(tick)) : 0;
        return juce::String(t / 60) + ":" + juce::String(t % 60).paddedLeft('0', 2);
    };
    if (s.start != s.end) {
        const auto a = std::min(s.start, s.end), b = std::max(s.start, s.end);
        const auto length = barSpan(a, b - a, meters);
        return "Time Selection   Start: " + where(a) + "   End: " + where(b) + "   Length: " +
               juce::String(length.bar) + "." + juce::String(length.beat) + "." +
               juce::String(length.sixteenth) + "   (Duration: " + seconds(b - a) + ")";
    }
    return "Insert Mark " + where(s.marked ? s.insert : 0) + " (Time: " +
           seconds(s.marked ? s.insert : 0) + ")";
}
std::string AdiRootComponent::undoTitle(bool redo) const {
    const auto step = redo ? submitter_.nextRedo() : submitter_.nextUndo();
    const std::string verb = redo ? "Redo" : "Undo";
    if (!canUndo(redo))
        return "Nothing to " + verb;
    return verb + (step && !step->label.empty() ? " " + step->label : "");
}
bool AdiRootComponent::fail(std::string message) {
    error_ = std::move(message);
    dirty_.mark(DirtySet::Status);
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
    auto edit = ::adi::ui::insertTrack(current->tracks, arrangement.geometry.selection.track, midi);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    edit.ops.front().selBefore = session_.advisory();
    const auto result = submitter_.submit(std::move(edit.ops));
    if (!result)
        return fail(result.error);
    arrangement.followPaused = true;
    arrangement.geometry.selection.track = edit.select;
    arrangement.geometry.selection.toTrack = 0;
    arrangement.geometry.selection.picked.clear();
    publishSelection();
    return true;
}
bool AdiRootComponent::insertReturnTrack() {
    // From the CURRENT cell (ADR-0201). The new return comes after the others, as every return
    // stands after the tracks (TrackOrder), and takes the next letter, as Live names it.
    const auto current = view_.current();
    if (!current)
        return fail("Inserting a return needs an open project.");
    const auto &tracks = current->tracks;
    std::int64_t id = 1, returns = 0;
    for (const auto &t : tracks) {
        id = std::max(id, t->id + 1);
        if (t->kind == "return")
            ++returns;
    }
    OpRequest request;
    request.opType = "track.create";
    request.payload = {{"id", id},
                       {"kind", "return"},
                       {"name", std::string(1, static_cast<char>('A' + returns % 26)) + " Return"},
                       {"index", static_cast<std::int64_t>(tracks.size())}};
    request.label = "Insert Return Track";
    request.selBefore = session_.advisory();
    const auto result = submitter_.submit(std::move(request));
    if (!result)
        return fail(result.error);
    arrangement.followPaused = true;
    if (state_.hideReturns) {
        state_.hideReturns = false;
        persist();
        strip.returns.setToggleState(true, juce::dontSendNotification);
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
    const auto id = arrangement.geometry.selection.track;
    if (id == 0)
        return fail("Select a track to delete.");
    OpRequest request;
    request.opType = "track.delete";
    request.payload = {{"id", id}};
    request.label = "Delete Track";
    request.selBefore = session_.advisory();
    const auto result = submitter_.submit(std::move(request));
    if (!result)
        return fail(result.error);
    arrangement.followPaused = true;
    arrangement.geometry.selection.track = 0;
    arrangement.geometry.selection.clip = 0;
    arrangement.geometry.selection.pickedClips.clear();
    publishSelection();
    return true;
}
bool AdiRootComponent::dropAudio(const std::string &utf8Path, std::int64_t track, std::int64_t pos,
                                 const std::string &name) {
    return dropAudioFiles({{utf8Path, name}}, track, pos, false);
}
bool AdiRootComponent::dropAudioFiles(const std::vector<std::pair<std::string, std::string>> &files,
                                      std::int64_t track, std::int64_t pos, bool separate) {
    // Where it lands is checked before the files are read; the batch is built from the CURRENT cell.
    if (!view_.current())
        return fail("Dropping audio needs an open project.");
    if (auto refusal = audioDropRefusal(*view_.current(), track); !refusal.empty())
        return fail(std::move(refusal));
    if (!importAudio)
        return fail("Dropping audio needs an open project.");
    // Each file captured before the batch lands: a new pool row takes an id past the ones the files
    // before it took, and a file dropped twice is captured once.
    std::vector<DroppedAudio> dropped;
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
            if (!file.import.media.opType.empty())
                nextId = std::max(nextId, file.import.mediaId + 1);
        }
        dropped.push_back(std::move(file));
    }
    auto edit = adi::ui::dropAudio(*view_.current(), track, pos, std::move(dropped), separate);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
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
bool AdiRootComponent::groupTracks(bool ungroup) {
    // From the CURRENT cell, as every action is; the batch is one undo step, labelled as Live's
    // Edit menu names it.
    const auto current = view_.current();
    // Group Tracks takes every selected track (Shift+click on the headers, or a selection across
    // tracks); Ungroup the selected group.
    const auto id = arrangement.geometry.selection.track;
    auto edit = ungroup ? ungroupTrack(current->tracks, id)
                        : ::adi::ui::groupTracks(current->tracks, arrangement.geometry.selectedTracks(reader()));
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
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
bool AdiRootComponent::restoreStep(std::int64_t txnId) {
    const auto result = submitter_.restore(txnId);
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
    auto &sel = arrangement.geometry.selection;
    sel.insert = sel.start = sel.end = tick;
    sel.clip = 0;
    sel.pickedClips.clear();
    sel.marked = true;
    arrangement.locate();
    publishSelection();
    arrangement.changed();
    return true;
}
bool AdiRootComponent::addLocatorAt(std::int64_t pos) {
    const auto current = view_.current();
    if (!current)
        return fail("Locators need an open project.");
    auto edit = addLocator(*current, pos);
    if (!edit.refusal.empty())
        return fail(std::move(edit.refusal));
    if (!submitEdits(std::move(edit.ops)))
        return false;
    arrangement.selectedLocator = edit.id;
    arrangement.selectedMeter = -1;
    arrangement.loopSelected = false; // the new locator is the selection; the arrows go to it
    arrangement.scrub.repaint();
    return true;
}
bool AdiRootComponent::setLocator() {
    // Set Locator (manual 6.4): "the Set Locator button becomes the Delete Locator button for any
    // selected locators". Live quantizes it to the global launch quantization, which arrives with
    // the Session View's launch settings (ADR-0203 d10): unquantized here.
    if (arrangement.selectedLocator != 0)
        return deleteLocator(arrangement.selectedLocator);
    const auto &sel = arrangement.geometry.selection;
    const auto at = playing_ ? timelineTick() : sel.start != sel.end ? std::min(sel.start, sel.end) : sel.insert;
    return addLocatorAt(at);
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
    // The run's step gives way to this tap's, while it is still the latest: one step for the run.
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
namespace {
// The insert marker, a point, at `tick`, and the playhead with it.
void placeInsertMarker(ArrangementView &arrangement, std::int64_t tick) {
    auto &sel = arrangement.geometry.selection;
    sel.insert = sel.start = sel.end = tick;
    sel.clip = 0;
    sel.pickedClips.clear();
    sel.marked = true;
    arrangement.locate();
}
} // namespace
bool AdiRootComponent::setPositionAt(const std::string &text) {
    const auto at = parsePosition(text);
    if (!at)
        return fail("Type a position such as 5.2.3: bar, beat and sixteenth.");
    const auto tick = tickOf(*at, reader().meters());
    if (!tick)
        return fail("Bar " + std::to_string(at->bar) + " has no beat " + std::to_string(at->beat) + ", sixteenth " +
                    std::to_string(at->sixteenth) + " in its time signature.");
    placeInsertMarker(arrangement, *tick);
    publishSelection();
    arrangement.changed();
    return true;
}
bool AdiRootComponent::stepPosition(int bars) {
    const auto &meters = reader().meters();
    const auto from = barPosition(playing_ ? timelineTick() : arrangement.geometry.selection.insert, meters);
    const BarPosition to{std::max<std::int64_t>(1, from.bar + bars), from.beat, from.sixteenth};
    auto tick = tickOf(to, meters);
    if (!tick)
        tick = tickOf({to.bar, 1, 1}, meters); // a beat that bar does not have: its start
    if (!tick)
        return false;
    placeInsertMarker(arrangement, *tick);
    publishSelection();
    arrangement.changed();
    return true;
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
bool AdiRootComponent::showDeviceView() { return state_.docked || command(AppCommands::ToggleDeviceView); }
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
        arrangement.canvas.grabKeyboardFocus();
        return true;
    case AppCommands::FocusDeviceView:
        if (!state_.docked && !command(AppCommands::ToggleDeviceView))
            return false;
        if (devices.panels.empty())
            return fail("The selected track has no devices.");
        devices.panels.front()->grabKeyboardFocus();
        return true;
    case AppCommands::FocusBrowser:
    case AppCommands::SearchBrowser:
        if (state_.panels.find(Panel::Browser)->collapsed && !command(AppCommands::ToggleBrowser))
            return false;
        if (id == AppCommands::FocusBrowser) {
            browser.grabKeyboardFocus();
            return true;
        }
        // Ctrl/Cmd+F, from anywhere: the search field, its words selected to type over.
        browser.search.grabKeyboardFocus();
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
bool AdiRootComponent::command(int id) {
    // The application's own: files and Settings.
    // In a window of its own, the Mix View is the application's to open and close (its floating
    // host); docked, the root's.
    if (id == AppCommands::DetachMixView || (id == AppCommands::ToggleMixView && state_.mixViewDetached))
        return applicationCommand && applicationCommand(id);
    if ((id >= AppCommands::NewProject && id <= AppCommands::Settings) || id == AppCommands::Quit)
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
    switch (id) {
    case AppCommands::PlayStop:
    case AppCommands::Stop:
    case AppCommands::ContinuePlay: {
        // Space plays from the insert marker, Shift+Space from where it stopped.
        const bool play = id != AppCommands::Stop && !mailbox_.desiredPlaying();
        const bool fromMarker = play && id == AppCommands::PlayStop;
        if (!mailbox_.hasRoom(fromMarker ? 2 : 1))
            return fail("Transport command queue is full");
        OpRequest request;
        request.opType = play ? "transport.play" : "transport.stop";
        request.payload = Payload::object();
        const auto result = submitter_.submit(std::move(request));
        if (!result)
            return fail(result.error);
        if (fromMarker)
            locate(arrangement.geometry.selection.insert);
        mailbox_.post(play ? TransportMailbox::Command::Play : TransportMailbox::Command::Stop);
        break;
    }
    case AppCommands::Undo:
    case AppCommands::Redo: {
        const auto result = id == AppCommands::Undo ? submitter_.undo() : submitter_.redo();
        if (!result.ok)
            return fail(result.error);
        arrangement.followPaused = true;
        // Undo puts back what was selected before the undone step (ADR-0021).
        if (id == AppCommands::Undo && result.selBefore)
            session_.restore(*result.selBefore);
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
        if (!chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Stack)))
            return false;
        if (!state_.mixView) {
            if (state_.mixViewDetached ? !(applicationCommand && applicationCommand(AppCommands::ToggleMixView))
                                       : !setMixView(true, false))
                return false;
        }
        mixViewPanel.frame(reader());
        mixViewPanel.stack.focus(arrangement.geometry.selection.track);
        break;
    case AppCommands::ToggleReferenceTrack:
        state_.showReference = !state_.showReference;
        if (!persist()) {
            state_.showReference = !state_.showReference;
            return fail(error_);
        }
        arrangement.applyView();
        arrangement.firstShownIfHidden();
        break;
    case AppCommands::PlayReference:
        // The key goes down; keyStateChanged lets it go. From a menu, a tap.
        referenceKey(true, static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
        if (!juce::KeyPress::isKeyCurrentlyDown('r') && !juce::KeyPress::isKeyCurrentlyDown('R'))
            referenceKey(false, static_cast<std::int64_t>(juce::Time::getMillisecondCounter()));
        return true;
    case AppCommands::ToggleMixView:
        state_.mixView = !state_.mixView;
        if (!persist()) {
            state_.mixView = !state_.mixView;
            return fail(error_);
        }
        break;
    case AppCommands::ToggleDeviceView:
        state_.docked = !state_.docked;
        if (!persist()) {
            state_.docked = !state_.docked;
            return fail(error_);
        }
        break;
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
    case AppCommands::ToggleMixerInOut:
    case AppCommands::ToggleMixerSends:
    case AppCommands::ToggleTrackOptions:
    case AppCommands::ToggleMixerTrackOptions: {
        auto &shown = id == AppCommands::ToggleOverview  ? state_.overview
                      : id == AppCommands::ToggleInOut   ? state_.inOut
                      : id == AppCommands::ToggleSends   ? state_.sends
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
        strip.inOut.setToggleState(state_.inOut, juce::dontSendNotification);
        strip.delay.setToggleState(state_.trackOptions, juce::dontSendNotification);
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
        std::string why;
        if (!commands_.setInfoView(!commands_.infoView(), why))
            return fail(why);
        infoShown_ = commands_.infoView();
        break;
    }
    case AppCommands::TabMovesFocus: {
        std::string why;
        if (!commands_.setTabMovesFocus(!commands_.tabMovesFocus(), why))
            return fail(why);
        break;
    }
    case AppCommands::WrapTabNavigation: {
        std::string why;
        if (!commands_.setWrapTabNavigation(!commands_.wrapTabNavigation(), why))
            return fail(why);
        break;
    }
    case AppCommands::InsertAudioTrack:
    case AppCommands::InsertMidiTrack:
        if (!insertTrack(id == AppCommands::InsertMidiTrack))
            return false;
        break;
    case AppCommands::DeleteTrack:
        if (arrangement.selectedLocator != 0)
            return deleteLocator(arrangement.selectedLocator);
        if (arrangement.selectedMeter >= 0)
            return deleteMeter(arrangement.selectedMeter);
        if (!deleteTrack())
            return false;
        break;
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
    case AppCommands::GroupTracks:
    case AppCommands::UngroupTracks:
        if (!groupTracks(id == AppCommands::UngroupTracks))
            return false;
        break;
    case AppCommands::SelectAll:
        if (!arrangement.selectAll())
            return false;
        break;
    case AppCommands::SoloSelected:
        if (!soloTrack(arrangement.geometry.selection.track, false))
            return false;
        break;
    case AppCommands::SoloInPlace:
        // Complete on its own, as the Solo button's menu is: the session's re-read, the Status Bar
        // and the marks are setSoloInPlace's, so the tail below is not run a second time.
        return toggleSoloInPlace();
    case AppCommands::UndoHistory:
        // The app puts the list in a window of its own (ui_main.cpp); headless, it is read as it is.
        undoHistory.refresh();
        if (applicationCommand)
            applicationCommand(AppCommands::UndoHistory);
        if (undoHistory.isShowing())
            undoHistory.list.grabKeyboardFocus();
        return true;
    // The metronome and the loop are complete on their own too (submitTransport).
    case AppCommands::Metronome:
        return toggleMetronome();
    case AppCommands::LoopSelection:
        return loopSelection();
    case AppCommands::SelectLoop:
        return arrangement.selectLoop();
    case AppCommands::AddLocator:
        return setLocator(); // the Create menu's Add Locator, at the play position as Set
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
        strip.returns.setToggleState(!state_.hideReturns, juce::dontSendNotification);
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
        return arrangement.headers.beginRename(arrangement.geometry.selection.track) ||
               fail("Select a track to rename.");
    case AppCommands::FocusControlBar:
    case AppCommands::FocusArrangement:
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
constexpr int kTrackControlsMenu = -1, kMixerControlsMenu = -2;
const std::vector<MenuEntries> &menuBar() {
    using C = AppCommands;
    static const std::vector<MenuEntries> bar{
#if JUCE_MAC
        {"File", {C::NewProject, C::OpenProject, 0, C::TakeSnapshot, C::SaveAs}},
#else
        {"File", {C::NewProject, C::OpenProject, 0, C::TakeSnapshot, C::SaveAs, 0, C::Quit}},
#endif
        // Tracks are deleted with the Edit menu's Delete (§18.2), which follows Cut, Copy,
        // Paste and Duplicate; those wait for their ops.
        // Loop Selection and Select Loop (manual 6.6, §42.8) after Select All; their place in Live 12's
        // Edit menu is to be read on Live.
        {"Edit", {C::Undo, C::Redo, 0, C::DeleteTrack, 0, C::RenameTrack, C::EditInfoText, C::SelectAll, 0, C::LoopSelection,
                  C::SelectLoop, 0,
                  C::GroupTracks, C::UngroupTracks}},
        // Add Locator and Delete Locator (manual 6.4) after Insert Time Signature Change; their place
        // in Live 12's Create menu is to be read on Live.
        {"Create", {C::InsertAudioTrack, C::InsertMidiTrack, C::InsertReturnTrack, 0, C::InsertTimeSignatureChange, 0,
                    C::AddLocator, C::DeleteLocator}},
        // The show and hide toggles in §42.1's order, ADI's Channel EQ after the Mixer.
        {"View",
         {C::FullScreen, 0, C::ToggleBrowser, C::ToggleOverview, kTrackControlsMenu, kMixerControlsMenu,
          C::ToggleMixerView, C::ShowChannelEq, C::ToggleMixView, C::DetachMixView, C::ToggleReferenceTrack,
          C::PlayReference, C::ToggleDeviceView,
          C::ToggleInfoView, C::UndoHistory, 0, C::ZoomIn, C::ZoomOut, 0,
          C::ZoomToSelection, C::ZoomBack, C::OptimizeWidth, C::OptimizeHeight, C::LargeTracks, 0,
          C::SwapPanels}},
        // Follow (§6.2) and the grid's commands (§6.10) are the Options menu's; so is Solo in Place
        // (§18.6), in its own group after the grid's, as Live 12's menu places it (read on Live 12,
        // 5 November: Solo Switches, Cue Switches, Solo in Place, before Settings), and Settings on
        // Windows (§2.3). Solo Switches and Cue Switches come with the cue path (B6).
#if JUCE_MAC
        {"Options",
         {C::Follow, 0, C::NarrowGrid, C::WidenGrid, C::TripletGrid, C::SnapToGrid, C::FixedGrid, 0,
          C::SoloInPlace}},
#else
        {"Options",
         {C::Follow, 0, C::NarrowGrid, C::WidenGrid, C::TripletGrid, C::SnapToGrid, C::FixedGrid, 0,
          C::SoloInPlace, 0, C::Settings}},
#endif
        // The views ADI has, in Live's order (§41.4.1): Session View, Clip View, the Groove
        // Pool and the Learn View come with those views; the Mixer with its keyboard.
        {"Navigate",
         {C::FocusControlBar, C::FocusArrangement, C::FocusDeviceView, C::FocusBrowser, 0,
          C::NextNeighbor, C::PreviousNeighbor, 0, C::TabMovesFocus,
          C::WrapTabNavigation}},
        {"Help", {}}};
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
                    else
                        menu.addCommandItem(&commands.manager, id);
                }
        return menu;
    }
    void menuItemSelected(int, int) override {}

  private:
    AdiRootComponent &root;
    AppCommands &commands;
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
    : DocumentWindow("ADI", juce::Colour(0xff171a20), DocumentWindow::allButtons, onDesktop),
      root_(root.get()), commands_(commands), clock_(this, [this](double) {
          followSettings();
          root_->frame();
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
void AdiWindow::moved() {
    DocumentWindow::moved();
    root_->followDisplay();
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
void followTooltips(juce::Component &window, const AppCommands &commands,
                    std::unique_ptr<juce::TooltipWindow> &tips, int &delayMs) {
    if (!commands.tooltips()) {
        tips.reset();
        return;
    }
    if (tips == nullptr) {
        delayMs = commands.tooltipDelayMs();
        tips = std::make_unique<juce::TooltipWindow>(&window, delayMs);
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
} // namespace adi::ui
