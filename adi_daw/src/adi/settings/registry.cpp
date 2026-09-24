// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/settings/registry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace adi::settings {
namespace {

Setting make(std::string key, Type type, std::string page, std::string label, std::string help,
             Value dflt, bool agent = false) {
    Setting s;
    s.key = std::move(key);
    s.type = type;
    s.page = std::move(page);
    s.label = std::move(label);
    s.help = std::move(help);
    s.defaultValue = std::move(dflt);
    s.agentMayChange = agent;
    return s;
}

Setting choice(std::string key, std::string page, std::string label, std::string help,
               std::vector<Value> choices, Value dflt, bool agent = false) {
    Setting s = make(std::move(key), Type::Choice, std::move(page), std::move(label),
                     std::move(help), std::move(dflt), agent);
    s.choices = std::move(choices);
    return s;
}

Setting ranged(std::string key, Type type, std::string page, std::string label, std::string help,
               Value dflt, double lo, double hi, bool agent = false) {
    Setting s = make(std::move(key), type, std::move(page), std::move(label), std::move(help),
                     std::move(dflt), agent);
    s.min = lo;
    s.max = hi;
    return s;
}

Setting project(std::string key, Type type, std::string label, std::string help, Value dflt,
                std::string op) {
    Setting s = make(std::move(key), type, "Project", std::move(label), std::move(help),
                     std::move(dflt));
    s.scope = Scope::Project;
    s.op = std::move(op);
    return s;
}

Setting device(std::string key, Type type, std::string page, std::string label, std::string help,
               Value dflt, std::vector<Value> choices = {}) {
    Setting s = make(std::move(key), type, std::move(page), std::move(label), std::move(help),
                     std::move(dflt));
    s.scope = Scope::Device;
    s.choices = std::move(choices);
    return s;
}

// ADR-0156: every row of docs/SETTINGS-CATALOGUE.md that is a setting ADI has.
// catalogue.cpp says which row each key answers, and why the others are absent.
void catalogueSettings(std::vector<Setting>& r) {
    const std::string lf = "Look & Feel";
    // --- Display & Input (catalogue II §1) ----------------------------------------------------
    r.push_back(make("lookfeel.focusOutline", Type::Bool, lf, "Outline view in focus",
                     "Outline the view that has keyboard focus, with the view indicator of ADR-0112.", true));
    r.push_back(choice("lookfeel.scrollBars", lf, "Scroll bars", "Show scroll bars always, or only on hover.",
                       {"always", "hover"}, "always"));
    r.push_back(choice("lookfeel.followMode", lf, "Follow behaviour",
                       "How a view follows the playhead: by page or by scrolling, per view "
                       "(Bitwig's playhead follow mode).",
                       {"page", "scroll"}, "page"));
    r.push_back(make("lookfeel.showLabels", Type::Bool, lf, "Show interface labels",
                     "Show the labels beside controls.", true));
    r.push_back(make("lookfeel.tabMovesFocus", Type::Bool, lf, "Tab key moves focus",
                     "The Tab key moves keyboard focus between controls.", false));
    r.push_back(make("lookfeel.wrapTabNavigation", Type::Bool, lf, "Wrap Tab navigation",
                     "Tab from the last control returns to the first.", true));
    r.push_back(make("editing.arrowKeysMoveClips", Type::Bool, lf, "Move clips with arrow keys",
                     "The arrow keys move the selected clips, not only the selection.", true));
    r.push_back(make("lookfeel.penTabletMode", Type::Bool, lf, "Pen tablet mode",
                     "Absolute pen input; the cursor is not repositioned while a knob moves "
                     "(REAPER's pen-safe mode, folded in).",
                     false));
    r.push_back(make("lookfeel.permanentScrubAreas", Type::Bool, lf, "Permanent scrub areas",
                     "Clicking in the scrub area above a track always scrubs.", true));
    r.push_back(choice("lookfeel.valueBoxMode", lf, "Value box",
                       "A value box edits by dragging, or opens its text on click (R-09). Live's is drag.",
                       {"drag", "text"}, "drag"));
    r.push_back(choice("lookfeel.sliderMode", lf, "Slider mode",
                       "How a slider takes a click: jump to it, touch, ramp or relative (R-09).",
                       {"jump", "touch", "ramp", "relative"}, "jump"));
    r.push_back(choice("lookfeel.wheelChangesParameters", lf, "Mouse wheel changes parameters",
                       "Whether the mouse wheel moves a control under it. Default: not in "
                       "scrollable areas, so scrolling never nudges a fader.",
                       {"never", "notInScrollableAreas", "always"}, "notInScrollableAreas"));
    r.push_back(make("lookfeel.showTooltips", Type::Bool, lf, "Floating tooltips",
                     "Show floating tooltips after the delay; the Info View is instant (ADR-0131).", true));

    // --- Theme & Colors (catalogue II §2) -----------------------------------------------------
    r.push_back(choice("lookfeel.paletteTone", lf, "Palette tone", "The theme's tone.",
                       {"neutral", "cool", "warm"}, "neutral"));
    r.push_back(ranged("lookfeel.contrast", Type::Int, lf, "Contrast",
                       "Interface contrast, a slider rather than a high-contrast switch (Bitwig).",
                       0, 0, 100));
    r.push_back(ranged("lookfeel.gridIntensity", Type::Int, lf, "Grid line intensity",
                       "How strongly grid lines are drawn.", 50, 0, 100));
    r.push_back(ranged("lookfeel.brightness", Type::Int, lf, "Brightness", "Interface brightness.", 50, 0, 100));
    r.push_back(ranged("lookfeel.colorIntensity", Type::Int, lf, "Colour intensity",
                       "Saturation of track and clip colours.", 50, 0, 100));
    r.push_back(ranged("lookfeel.hue", Type::Int, lf, "Hue", "Rotates the theme's hues, in degrees.", 0, -180, 180));
    r.push_back(choice("lookfeel.autoTrackColor", lf, "Auto-assign track colours",
                       "Cubase's five modes. Live's on is previous + 1, its off is default.",
                       {"default", "previous", "previousPlusOne", "lastApplied", "random"}, "previousPlusOne"));
    r.push_back(choice("lookfeel.clipColor", lf, "Clip colour", "New clips take the track's colour, or a random one.",
                       {"track", "random"}, "track"));
    r.push_back(make("lookfeel.accentColor", Type::Text, lf, "Accent colour",
                     "One colour that marks the selection (Cubase). Empty: the theme's own.", ""));
    for (const char* kind : {"audio", "midi", "instrument", "group", "return", "master", "vca"})
        r.push_back(make(std::string("lookfeel.trackColor.") + kind, Type::Text, lf,
                         std::string("Default colour: ") + kind + " tracks",
                         "The colour a new track of this kind gets (Cubase). Empty: the theme's.", ""));
    r.push_back(make("lookfeel.waveformPerceptual", Type::Bool, lf, "Perceptual waveforms",
                     "Draw waveforms on a perceptual scale (Bitwig).", false));
    r.push_back(ranged("lookfeel.waveformBrightness", Type::Int, lf, "Waveform brightness",
                       "Brightness of waveforms in clips (Cubase).", 50, 0, 100));
    r.push_back(choice("lookfeel.showVolumeCurves", lf, "Event volume curves",
                       "Show a clip's volume curve always or on hover (ADR-0115).", {"always", "hover"}, "hover"));
    r.push_back(choice("lookfeel.showFades", lf, "Fades", "Show clip fades always or on hover (ADR-0115).",
                       {"always", "hover"}, "always"));
    r.push_back(choice("lookfeel.volumeHandle", lf, "Clip volume handle",
                       "Show the clip volume handle always, never or on hover (ADR-0115).",
                       {"always", "never", "hover"}, "hover"));
    r.push_back(ranged("lookfeel.meterPeakHoldMs", Type::Int, lf, "Meter peak hold time",
                       "Milliseconds a peak is held (R-10).", 1500, 0, 30000));
    r.push_back(ranged("lookfeel.meterDecayDbPerSecond", Type::Real, lf, "Meter fall-back rate",
                       "How fast a meter falls, in dB per second (R-10).", 20.0, 1, 200));
    r.push_back(make("lookfeel.meterStickyClip", Type::Bool, lf, "Sticky clip indicators",
                     "A clip light stays on until clicked (R-10).", true));
    r.push_back(ranged("lookfeel.meterFloorDb", Type::Int, lf, "Meter floor",
                       "The lowest level a meter shows, in dB (R-10).", -70, -144, -20));
    r.push_back(make("lookfeel.hiDpi", Type::Bool, lf, "HiDPI",
                     "Draw at the display's full resolution (REAPER's HiDPI mode).", true));
    r.push_back(choice("lookfeel.dialogPosition", lf, "Dialog position",
                       "Where dialogs open: centred on the window, or at the mouse.", {"center", "mouse"}, "center"));

    // --- Audio (catalogue II §3) ----------------------------------------------------------------
    r.push_back(make("audio.inputDevice", Type::Text, "Audio", "Audio input device",
                     "The input device. Empty: use the system device.", ""));
    r.push_back(make("audio.outputDevice", Type::Text, "Audio", "Audio output device",
                     "The output device. Empty: use the system device.", ""));
    r.push_back(device("audio.channelConfig", Type::Text, "Audio", "Channel configuration",
                       "The device's enabled inputs and outputs and their names (Live's Input and "
                       "Output Config). Stored with the device.",
                       ""));
    r.push_back(device("audio.outputRoles", Type::Text, "Audio", "Output roles",
                       "Each output pair's role: speakers, headphones or output (Bitwig). The cue "
                       "bus plays to the headphones (R-11). Stored with the device.",
                       ""));
    r.push_back(choice("audio.sampleRate", "Audio", "Sample rate",
                       "The rate the device is opened at: 44.1 to 768 kHz, nothing lower "
                       "(ADR-0157). A project at another rate raises the mismatch bar.",
                       sampleRateLadder(), 48000));
    r.push_back(choice("audio.srcQuality", "Audio", "Sample-rate conversion",
                       "Default quality of sample-rate and pitch conversion, as Live.", {"normal", "high"}, "high"));
    r.push_back(ranged("audio.driverErrorCompensationMs", Type::Real, "Audio", "Driver error compensation",
                       "Milliseconds added to what the driver reports; Measure round trip fills it in.",
                       0.0, -100, 100));
    r.push_back(make("audio.testTone", Type::Bool, "Audio", "Test tone", "Play a sine tone to the output.", false));
    r.push_back(ranged("audio.testToneVolumeDb", Type::Real, "Audio", "Tone volume",
                       "The test tone's level, in dB.", -36.0, -70, 0));
    r.push_back(ranged("audio.testToneFrequencyHz", Type::Int, "Audio", "Tone frequency",
                       "The test tone's frequency, in Hz.", 440, 20, 20000));
    r.push_back(ranged("audio.cpuSimulatorPercent", Type::Int, "Audio", "CPU usage simulator",
                       "A synthetic load, the benchmark's (BENCHMARKS.md), to test how far the "
                       "buffer holds.",
                       0, 0, 100));
    r.push_back(make("audio.loopbackSource", Type::Text, "Audio", "Loopback input",
                     "The master or a bus offered as a recordable input (ADR-0106). Empty: none.", ""));
    r.push_back(choice("audio.releaseDeviceInBackground", "Audio", "Release the device in the background",
                       "Close the audio device when ADI is in the background. Exclusive-mode "
                       "drivers need this (REAPER).",
                       {"never", "whenStopped", "always"}, "never"));
    r.push_back(make("audio.fallbackToSystemDevice", Type::Bool, "Audio", "Fall back to the system device",
                     "When a device cannot be opened, open the system device silently. ADI "
                     "always warns; this decides whether it also falls back.",
                     false));
    r.push_back(ranged("audio.recordPdcThresholdMs", Type::Real, "Audio", "Delay compensation threshold when recording",
                       "Plug-ins with more latency than this are bypassed on armed tracks while "
                       "recording; the mix is otherwise fully compensated (R-12).",
                       10.0, 0, 500));
    r.push_back(make("audio.autoMute", Type::Bool, "Audio", "Automatic mute at a limit",
                     "Mute the output when it passes the limit: the one setting between a "
                     "feedback patch and a hearing test (REAPER). Off by default.",
                     false));
    r.push_back(ranged("audio.autoMuteLimitDb", Type::Real, "Audio", "Automatic mute limit",
                       "The level, in dBFS, above which the output mutes.", 12.0, 0, 24));
    r.push_back(make("audio.metronomeOutput", Type::Text, "Audio", "Metronome output",
                     "The output the metronome plays to. Empty: the main output.", ""));

    // --- File & Folder (catalogue II §6) ----------------------------------------------------------
    const std::string ff = "File & Folder";
    r.push_back(make("files.sampleEditor", Type::Path, ff, "Sample editor",
                     "The external editor Edit opens a sample in.", ""));
    r.push_back(choice("files.sampleEditorOpens", ff, "Sample editor opens",
                       "Open a copy of the sample, or the file in place.", {"copy", "inPlace"}, "copy"));
    r.push_back(ranged("cache.maxSizeMb", Type::Int, ff, "Maximum cache size",
                       "Decoding cache: decoded audio, analysis and peaks. The least recently "
                       "used goes first, never a file in use (ADR-0156). In MB.",
                       10240, 0, 16777216));
    r.push_back(ranged("cache.minFreeSpaceMb", Type::Int, ff, "Minimum free space",
                       "Decoding cache: the free space kept on its disk; files are evicted to "
                       "keep it (ADR-0156). In MB.",
                       2048, 0, 16777216));
    r.push_back(make("cache.folder", Type::Path, ff, "Cache folder",
                     "Where the decoding cache lives. Empty: the application cache folder (ADR-0149).", ""));
    r.push_back(make("files.projectsFolder", Type::Path, ff, "Default projects folder",
                     "Where new projects are saved (REAPER).", ""));
    r.push_back(make("files.renderFolder", Type::Path, ff, "Default render folder",
                     "Where exports are written.", ""));
    r.push_back(choice("files.copyImportedMedia", ff, "Copy imported media into the project",
                       "Always, ask, or never, in Live's Collect Files wording. Default: ask.",
                       {"always", "ask", "never"}, "ask"));
    r.push_back(choice("files.openOnStart", ff, "Open on start",
                       "What opens when ADI starts: nothing, the last project, or a template.",
                       {"nothing", "lastProject", "template"}, "template"));
    r.push_back(make("files.startTemplate", Type::Path, ff, "Start-up template",
                     "The project a new project starts from. Empty: the built-in one.", ""));
    r.push_back(make("files.defaultAuthor", Type::Text, ff, "Default author",
                     "The author new projects are given (Cubase).", ""));

    // --- Library (catalogue II §7) ---------------------------------------------------------------
    r.push_back(choice("library.collectOnExport", "Library", "Collect files on export",
                       "Same three, same wording as Live.", {"always", "ask", "never"}, "ask"));
    r.push_back(make("library.backgroundIndexing", Type::Bool, "Library", "Index content folders in the background",
                     "Keep the browser's index current while ADI runs.", true));
    r.push_back(ranged("library.maxResults", Type::Int, "Library", "Maximum browser results",
                       "The most results a browser search shows.", 10000, 100, 1000000));

    // --- Plug-ins (catalogue II §8) --------------------------------------------------------------
    r.push_back(make("plugins.useVst3SystemFolders", Type::Bool, "Plug-ins", "Use VST3 system folders",
                     "Scan the format's default locations, as well as the custom folders.", true));
    r.push_back(make("plugins.autoOpenWindows", Type::Bool, "Plug-ins", "Auto-open plug-in windows",
                     "Open a plug-in's window when it is added.", true));
    r.push_back(make("plugins.multipleWindows", Type::Bool, "Plug-ins", "Multiple plug-in windows",
                     "Allow more than one plug-in window open at once.", false));
    r.push_back(make("plugins.autoHideWindows", Type::Bool, "Plug-ins", "Auto-hide plug-in windows",
                     "Show only the selected track's plug-in windows.", true));
    r.push_back(make("plugins.editorsAlwaysOnTop", Type::Bool, "Plug-ins", "Plug-in windows always on top",
                     "Third-party plug-in windows float above ADI (ADR-0076).", true));
    r.push_back(device("plugins.windowScale", Type::Choice, "Plug-ins", "Plug-in window scaling",
                       "An override for the one plug-in that draws wrong on HiDPI: 100, 150 or "
                       "200 %. Stored with the device.",
                       "auto", {"auto", 100, 150, 200}));
    r.push_back(device("plugins.ignoreBroadcasts", Type::Bool, "Plug-ins", "Ignore parameter broadcasts",
                       "For the plug-in that floods: its unsolicited parameter changes are "
                       "dropped instead of becoming ops (ADR-0110). Stored with the device.",
                       false));

    // --- Record, Warp & Launch (catalogue II §9) ----------------------------------------------------
    r.push_back(make("record.writeBext", Type::Bool, "Record", "Broadcast Wave (bext)",
                     "Write timecode, description and originator into recordings (ADR-0132 d2).", false));
    r.push_back(make("record.writeIxml", Type::Bool, "Record", "iXML", "Write an iXML chunk into recordings.", false));
    r.push_back(make("record.exclusiveArm", Type::Bool, "Record", "Exclusive arm",
                     "Arming a track disarms the others.", true));
    r.push_back(make("record.exclusiveSolo", Type::Bool, "Record", "Exclusive solo",
                     "Soloing a track unsolos the others.", true));
    r.push_back(choice("record.clipUpdateRate", "Record", "Clip update rate",
                       "How often changes to a playing clip take effect.",
                       {"none", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"}, "1/16"));
    r.push_back(make("record.startPlaybackWithRecord", Type::Bool, "Record", "Start playback with record",
                     "Pressing Record also starts playback.", true));
    r.push_back(choice("record.shortSampleMode", "Record", "Loop/warp short samples",
                       "Whether a short sample imports looped and warped.", {"auto", "on", "off"}, "auto"));
    r.push_back(choice("record.defaultWarpMode", "Record", "Default warp mode",
                       "The warp mode new clips get. ADI's stretch is its own (ADR-0061); the "
                       "names follow Live.",
                       {"beats", "tones", "texture", "re-pitch", "complex", "complexPro"}, "beats"));
    r.push_back(make("record.warpDetectTempoChanges", Type::Bool, "Record", "Warp: detect tempo changes",
                     "When Warp runs on a raw clip, follow tempo changes rather than assume one "
                     "tempo (ADR-0132 d7).",
                     false));
    r.push_back(choice("record.warpInsertFrom", "Record", "Warp: insert from",
                       "When Warp runs, 1.1.1 goes on the first beat or at the sample start (ADR-0132 d7).",
                       {"firstBeat", "sampleStart"}, "firstBeat"));
    r.push_back(choice("record.autoArmOnSelect", "Record", "Auto-arm on select",
                       "Which tracks arm when selected (Bitwig).", {"none", "instrumentAndMidi", "all"},
                       "instrumentAndMidi"));
    r.push_back(make("record.discardIncompleteTakes", Type::Bool, "Record", "Discard incomplete takes",
                     "In loop recording, drop a first or last take shorter than the threshold (REAPER).", true));
    r.push_back(ranged("record.completeTakePercent", Type::Int, "Record", "Complete take threshold",
                       "The share of the loop a take must cover to be kept, in percent.", 50, 0, 100));
    r.push_back(make("record.snapMidiPartsToBars", Type::Bool, "Record", "Snap MIDI clips to bars",
                     "A recorded MIDI clip starts and ends on bars, as Live's do (Cubase).", true));

    // --- MIDI (catalogue II §5) ------------------------------------------------------------------
    r.push_back(make("midi.controlSurfaceScripts", Type::PathList, "MIDI", "Controller script folders",
                     "Folders of controller extensions, Bitwig's model (R-14).", Value::array()));
    r.push_back(choice("midi.takeoverMode", "MIDI", "Takeover mode",
                       "How a controller takes over a value it does not match; overridable per controller.",
                       {"none", "pickup", "valueScaling"}, "pickup"));
    r.push_back(make("midi.focusDialPort", Type::Text, "MIDI", "Master focus dial: port",
                     "The port of the one encoder that controls whatever is under the mouse, or "
                     "the last touched plug-in parameter (ADR-0130). Empty: not bound.",
                     ""));
    r.push_back(ranged("midi.focusDialChannel", Type::Int, "MIDI", "Master focus dial: channel",
                       "The encoder's MIDI channel (ADR-0130).", 1, 1, 16));
    r.push_back(ranged("midi.focusDialCc", Type::Int, "MIDI", "Master focus dial: CC",
                       "The encoder's controller number (ADR-0130).", 0, 0, 127));
    r.push_back(choice("midi.focusDialMode", "MIDI", "Master focus dial: encoder mode",
                       "How the encoder encodes a turn (ADR-0130).",
                       {"absolute", "relativeTwosComplement", "relativeBinaryOffset", "relativeSignBit"},
                       "relativeTwosComplement"));
    r.push_back(device("midi.port.track", Type::Bool, "MIDI", "Port: Track",
                       "The port feeds tracks. Stored per port.", false));
    r.push_back(device("midi.port.sync", Type::Bool, "MIDI", "Port: Sync", "The port sends or takes sync. Stored per port.", false));
    r.push_back(device("midi.port.remote", Type::Bool, "MIDI", "Port: Remote",
                       "The port may be MIDI-mapped. Stored per port.", false));
    r.push_back(device("midi.port.mpe", Type::Bool, "MIDI", "Port: MPE",
                       "The port carries MPE. The route per plug-in is ADR-0134 d7's. Stored per port.", false));
    r.push_back(device("midi.port.clockSyncDelayMs", Type::Real, "MIDI", "Port: clock sync delay",
                       "Milliseconds of clock offset, in and out (Bitwig). Stored per port.", 0.0));
    r.push_back(device("midi.port.syncType", Type::Choice, "MIDI", "Port: sync type",
                       "MIDI clock or MIDI timecode. Stored per port.", "clock", {"clock", "mtc"}));
    r.push_back(device("midi.port.mtcFrameRate", Type::Choice, "MIDI", "Port: MTC frame rate",
                       "Stored per port.", "25", {"24", "25", "29.97df", "30"}));
    r.push_back(device("midi.port.mtcStartOffset", Type::Text, "MIDI", "Port: MTC start offset",
                       "Timecode at the song start, hh:mm:ss:ff. Stored per port.", "00:00:00:00"));
    r.push_back(make("midi.resetOnStop", Type::Bool, "MIDI", "Reset on stop",
                     "Send reset events when the transport stops (Cubase).", true));
    r.push_back(make("midi.insertResetAfterRecord", Type::Bool, "MIDI", "Insert reset events after record",
                     "End a recording with reset events, so a held pedal does not stick (Cubase).", true));
    r.push_back(make("midi.recordFilter", Type::Text, "MIDI", "MIDI filter: record",
                     "Message types and channels never recorded, comma-separated (Cubase).", ""));
    r.push_back(make("midi.thruFilter", Type::Text, "MIDI", "MIDI filter: thru",
                     "Message types and channels never echoed, comma-separated (Cubase).", ""));
    r.push_back(make("midi.extendEarlyNotes", Type::Bool, "MIDI", "Play notes that start before the clip",
                     "A note a few ticks before a clip starts still sounds (Cubase).", true));

    // --- Editing (catalogue III: Cubase and REAPER) ---------------------------------------------
    r.push_back(make("editing.autoSelectUnderCursor", Type::Bool, "Editing", "Select clips under the cursor",
                     "Clips under the edit cursor are selected (Cubase, R-21).", false));
    r.push_back(make("editing.deleteOverlaps", Type::Bool, "Editing", "Delete overlaps",
                     "A clip moved onto another replaces the overlapped part, as in Live (R-21).", true));
    r.push_back(make("editing.clipsGetTrackNames", Type::Bool, "Editing", "Clips get track names",
                     "A clip moved to another track takes its name (Cubase, R-21).", false));
    r.push_back(make("editing.trackFollowsClipSelection", Type::Bool, "Editing", "Track selection follows clips",
                     "Selecting a clip selects its track (R-21).", true));
    r.push_back(make("editing.automationFollowsClips", Type::Bool, "Editing", "Automation follows clips",
                     "Moving a clip moves the automation under it. On by default (R-21).", true));
    r.push_back(ranged("editing.dragDelayMs", Type::Int, "Editing", "Drag delay",
                       "Milliseconds before a click becomes a drag, so a click never moves a clip.", 150, 0, 1000));
    r.push_back(make("editing.wheelForVolumeAndFades", Type::Bool, "Editing", "Mouse wheel for clip volume and fades",
                     "The wheel over a clip's handle changes its volume or fade (ADR-0115).", true));
    r.push_back(make("editing.mutedClipsAsDeleted", Type::Bool, "Editing", "Treat muted clips as deleted",
                     "Muted clips are ignored by edits and overlaps (Cubase).", false));
    r.push_back(choice("editing.importOptions", "Editing", "Audio import options",
                       "Show the import options, or use the last ones.", {"ask", "always", "never"}, "ask"));
    r.push_back(choice("editing.bounceReplaces", "Editing", "On bounce",
                       "A bounce replaces the clips it was made from, keeps them, or asks.",
                       {"ask", "replace", "keep"}, "ask"));
    r.push_back(make("editing.zoomInRuler", Type::Bool, "Editing", "Zoom while locating in the ruler",
                     "Dragging vertically in the ruler zooms, as in Live.", true));
    r.push_back(choice("editing.editorOpensIn", "Editing", "Editor opens in",
                       "A double-click opens the editor in the lower zone or a window (ADR-0063).",
                       {"lowerZone", "window"}, "lowerZone"));
    r.push_back(make("editing.editorFollowsSelection", Type::Bool, "Editing", "Editor follows selection",
                     "The editor shows the selected clip.", true));
    r.push_back(make("editing.cursorFollowsSelection", Type::Bool, "Editing", "Edit cursor follows selection",
                     "Selecting moves the edit cursor to the selection (REAPER).", true));
    r.push_back(make("editing.linkLoopToSelection", Type::Bool, "Editing", "Link loop to time selection",
                     "The loop follows the time selection (REAPER).", false));
    r.push_back(ranged("editing.transientSensitivity", Type::Int, "Editing", "Transient sensitivity",
                       "How readily transients are detected (REAPER).", 50, 0, 100));
    r.push_back(make("editing.rippleLockedClips", Type::Bool, "Editing", "Ripple moves locked clips",
                     "Ripple editing also moves locked clips (REAPER).", false));
    r.push_back(make("editing.splitAllWithNoSelection", Type::Bool, "Editing", "Split all at the cursor",
                     "With nothing selected, Split splits every track at the cursor (REAPER).", true));

    // --- Mixing and new tracks (catalogue III) -------------------------------------------------
    r.push_back(ranged("mixing.defaultSendLevelDb", Type::Real, "Mixing", "Default send level",
                       "The level a new send starts at, in dB (Cubase; ADR-0072).", 0.0, -144, 6));
    r.push_back(make("mixing.groupMuteMutesSources", Type::Bool, "Mixing", "Group mute mutes sources",
                     "Muting a group mutes the tracks that feed it (Cubase; ADR-0044).", true));
    r.push_back(make("mixing.soloInFront", Type::Bool, "Mixing", "Solo in front",
                     "Soloing leaves the rest of the mix audible, lowered (REAPER).", false));
    r.push_back(ranged("mixing.soloInFrontDb", Type::Real, "Mixing", "Solo in front level",
                       "How far the rest of the mix is lowered, in dB.", -18.0, -60, 0));
    r.push_back(make("mixing.soloBus", Type::Bool, "Mixing", "Solo through a dedicated bus",
                     "Soloed tracks play through a solo bus (REAPER).", false));
    r.push_back(ranged("newtrack.faderGainDb", Type::Real, "New track", "Fader",
                       "The fader a new track starts at, in dB (REAPER's track defaults, R-26).", 0.0, -144, 6));
    r.push_back(make("newtrack.showInMixer", Type::Bool, "New track", "Show in mixer",
                     "A new track shows in the mixer (R-26).", true));
    r.push_back(choice("newtrack.meterMode", "New track", "Meter",
                       "The meter a new track shows (R-26).", {"peak", "rms", "peakAndRms"}, "peak"));
    r.push_back(ranged("newtrack.sendLevelDb", Type::Real, "New track", "Send level",
                       "The send level a new track's sends start at, in dB (R-26).", 0.0, -144, 6));
    r.push_back(make("newtrack.showEnvelopes", Type::Bool, "New track", "Show automation lanes",
                     "A new track opens with its automation lanes shown (R-26).", false));

    // --- Transport (catalogue III) ------------------------------------------------------------
    r.push_back(ranged("transport.windSpeed", Type::Real, "Transport", "Wind speed",
                       "How fast fast-forward and rewind move, in bars per second (Cubase).", 4.0, 0.25, 64));
    r.push_back(make("transport.locateOnClick", Type::Bool, "Transport", "Locate on click in empty space",
                     "A click in empty track space moves the playhead, as in Live.", true));
    r.push_back(choice("transport.seekOnClick", "Transport", "Seek on click",
                       "Where a click moves the playhead during playback (REAPER).",
                       {"ruler", "rulerAndEmptyArea", "anywhere"}, "rulerAndEmptyArea"));
    r.push_back(ranged("transport.runFxWhenStoppedMs", Type::Int, "Transport", "Run effects when stopped",
                       "Milliseconds effects keep running after stop, so tails ring out (REAPER). 0: stop at once.",
                       500, 0, 60000));
    r.push_back(make("transport.stopAtProjectEnd", Type::Bool, "Transport", "Stop at the end of the project",
                     "Playback stops after the last clip (REAPER).", false));
    r.push_back(make("transport.commitFieldsAfterPause", Type::Bool, "Transport", "Commit fields after a pause",
                     "A transport field commits one second after typing stops (REAPER).", true));

    // --- Sync (catalogue II §4, IV §2) ---------------------------------------------------------
    r.push_back(make("sync.linkEnabled", Type::Bool, "Sync", "Link", "Join an Ableton Link session.", false));
    r.push_back(make("sync.showLinkToggle", Type::Bool, "Sync", "Show Link toggle",
                     "Show the Link switch in the control bar.", false));
    r.push_back(make("sync.linkStartStopSync", Type::Bool, "Sync", "Link start/stop sync",
                     "Share start and stop with Link peers.", false));
    r.push_back(choice("sync.clockSource", "Sync", "Sync in", "Where tempo and position come from (Bitwig).",
                       {"internal", "midiClock", "link"}, "internal"));
    r.push_back(ranged("sync.tempoResponsiveness", Type::Int, "Sync", "Incoming tempo responsiveness",
                       "How quickly the tempo follows incoming MIDI clock (Bitwig).", 50, 0, 100));
    r.push_back(ranged("sync.lanJitterBufferMs", Type::Int, "Sync", "LAN audio: jitter buffer",
                       "Default for new LAN audio nodes (ADR-0126); each node keeps its own.", 40, 5, 500));
    r.push_back(choice("sync.lanCodec", "Sync", "LAN audio: codec", "Default for new LAN audio nodes (ADR-0126).",
                       {"pcm", "opus"}, "opus"));
    r.push_back(ranged("sync.lanBitrateKbps", Type::Int, "Sync", "LAN audio: bitrate",
                       "Default for new LAN audio nodes, in kbit/s (ADR-0126).", 256, 32, 1536));
    r.push_back(make("sync.ptpEnabled", Type::Bool, "Sync", "PTP",
                     "PTP as a shared timebase and a measurement across AudioGridder servers (ADR-0107).", false));
    r.push_back(ranged("sync.ptpDomain", Type::Int, "Sync", "PTP domain", "The PTP domain listened to.", 0, 0, 127));
    r.push_back(make("sync.ptpAdapters", Type::Text, "Sync", "PTP network adapters",
                     "The adapters PTP runs on, comma-separated. Empty: none.", ""));
    r.push_back(make("sync.ptpGrandmasters", Type::Text, "Sync", "PTP grandmasters",
                     "The grandmasters allowed, by IP and MAC address, comma-separated. "
                     "Announcements from anything else are ignored (ADR-0145 d7).",
                     ""));
    r.push_back(make("sync.audioGridderDiscovery", Type::Bool, "Sync", "Find AudioGridder servers",
                     "Find remote plug-in servers by mDNS/Bonjour (ADR-0053, ADR-0145 d6).", true));

    // --- Engine (catalogue IV §1) ------------------------------------------------------------------
    r.push_back(ranged("engine.compensationHeadroomSamples", Type::Int, "Engine", "Compensation headroom",
                       "The ring headroom the graph reserves, so a latency change is a tap move "
                       "and not a rebuild (ADR-0088). Advanced.",
                       8192, 512, 65536));

    // --- Updates (catalogue II §10) ----------------------------------------------------------------
    r.push_back(make("updates.checkOnLaunch", Type::Bool, "Updates", "Check for updates",
                     "Check on launch and show the release notes; the download is manual.", true));
    r.push_back(choice("updates.channel", "Updates", "Release channel", "Stable, or beta releases too (Bitwig).",
                       {"stable", "beta"}, "stable"));
    r.push_back(make("privacy.diagnosticLog", Type::Bool, "Privacy", "Write a diagnostic log",
                     "A local log for support; nothing is sent (R-20).", false));

    // --- AI agent (catalogue IV §3) ------------------------------------------------------------------
    r.push_back(ranged("agent.maxOpsPerMinute", Type::Int, "AI", "Rate cap",
                       "Ops per minute the agent may commit at Apply tier (AI-AGENT 6).", 600, 1, 100000));
    r.push_back(make("agent.endpoint", Type::Text, "AI", "Model endpoint",
                     "Which model service, local RPC or remote. The key is kept in the OS "
                     "keychain, never here.",
                     ""));
    r.push_back(choice("agent.asyncServices", "AI", "Stem separation and other services",
                       "Run async AI jobs locally or remotely (ADR-0064).", {"local", "remote"}, "local"));
    r.push_back(make("agent.resultsFolder", Type::Path, "AI", "Results folder",
                     "Where async AI jobs keep their results (ADR-0064). Empty: the cache.", ""));

    // --- Windows and views (catalogue IV §4) ---------------------------------------------------
    r.push_back(choice("windows.defaultView", "Windows", "Default view",
                       "The view a project opens in: Main, Macro or Micro (ADR-0112).",
                       {"main", "macro", "micro"}, "main"));
    r.push_back(make("windows.rememberFloatingPerDisplay", Type::Bool, "Windows", "Remember floating panels",
                     "Floating panels reopen where they were, per display (ADR-0063).", true));
    r.push_back(make("windows.snapshotName", Type::Text, "Windows", "Snapshot name",
                     "The default name File > Take Snapshot offers (ADR-0128).", "[Project] [YYYY-MM-DD HH:MM]"));
    r.push_back(make("devices.pdAnalyserOpen", Type::Bool, "Devices", "Pd analyser view open",
                     "A Pd device's analyser view opens with it (ADR-0116).", false));

    // --- Music (catalogue IV §5) ---------------------------------------------------------------
    r.push_back(make("music.defaultTuning", Type::Text, "Music", "Default tuning",
                     "The tuning new projects start with: 12-TET, or a maqam or other table (ADR-0103).",
                     "12-TET"));
    r.push_back(choice("music.pitchNaming", "Music", "Pitch naming",
                       "English, German (B as H: the German and Hebrew markets read H) or Solfège.",
                       {"english", "german", "solfege"}, "english"));
    r.push_back(choice("music.middleC", "Music", "Middle C",
                       "Which octave number middle C gets.", {"C3", "C4"}, "C3"));
    r.push_back(ranged("music.defaultTempo", Type::Real, "Music", "Default tempo",
                       "The tempo new projects start at, in BPM.", 120.0, 20, 999));
    r.push_back(make("music.defaultTimeSignature", Type::Text, "Music", "Default time signature",
                     "The time signature new projects start in.", "4/4"));

    // --- Devices and DSP (catalogue IV §6) ------------------------------------------------------
    r.push_back(make("devices.pdExternalsFolders", Type::PathList, "Devices", "Pd externals",
                     "Folders Pure Data loads externals from (ADR-0145 d8).", Value::array()));
    r.push_back(make("devices.pdPatchFolder", Type::Path, "Devices", "Pd patch folder",
                     "Where new Pd patches are saved (ADR-0145 d8).", ""));
    r.push_back(ranged("devices.freezeTailSeconds", Type::Real, "Devices", "Freeze tail",
                       "Seconds of tail a freeze renders after the last clip (ADR-0059).", 4.0, 0, 120));
    r.push_back(make("devices.freezeMutedClipsSilent", Type::Bool, "Devices", "Muted clips freeze silent",
                     "Muted clips render as silence in a freeze (ADR-0059, REAPER).", true));
    r.push_back(choice("shortcuts.preset", "Shortcuts", "Key map",
                       "Keys named after the three references, so a Cubase user has Cubase's keys "
                       "on day one (R-25).",
                       {"live", "cubase", "bitwig"}, "live"));
    r.push_back(make("shortcuts.computerMidiKeyboard", Type::Bool, "Shortcuts", "Computer MIDI keyboard",
                     "The computer keyboard plays notes (Bitwig, Live).", false));
}

std::vector<Setting> build() {
    std::vector<Setting> r;
    // --- Look & Feel: the agent's whitelist lives mostly here (ADR-0125 d2) --
    r.push_back(choice("lookfeel.theme", "Look & Feel", "Theme",
                       "Colour theme of the whole application; by default it follows the "
                       "operating system. Themes are files, so one can be shared.",
                       {"os", "dark", "light"}, "os", true));
    r.push_back(choice("lookfeel.language", "Look & Feel", "Language",
                       "Interface language. Hebrew runs right to left in text containers only "
                       "(ADR-0125 d4).",
                       {"en", "he"}, "en"));
    r.push_back(make("lookfeel.zoomOnSelection", Type::Bool, "Look & Feel", "Zoom on Selection",
                     "Ctrl/Cmd + wheel zooms around the selection, as Live does, instead of "
                     "around the cursor (ADR-0145 d2).",
                     false, true));
    r.push_back(choice("lookfeel.knobMode", "Look & Feel", "Knob drag mode",
                       "How a knob follows the mouse. Vertical is Live's (R-09).",
                       {"vertical", "horizontal", "rotary"}, "vertical", true));
    r.push_back(ranged("lookfeel.tooltipDelayMs", Type::Int, "Look & Feel", "Tooltip delay",
                       "Milliseconds before a floating tooltip appears, 500 to 800 "
                       "(ADR-0131 d1).",
                       600, 500, 800, true));
    r.push_back(make("lookfeel.followPlayhead", Type::Bool, "Look & Feel", "Follow playhead",
                     "Scroll the arrangement to keep the playhead in view.", true, true));
    r.push_back(make("lookfeel.showInfoView", Type::Bool, "Look & Feel", "Info View",
                     "Show the docked Info View, which describes what the mouse is over "
                     "(ADR-0131 d1).",
                     true, true));
    r.push_back(make("lookfeel.showMeterPeakHold", Type::Bool, "Look & Feel", "Meter peak hold",
                     "Hold the highest meter level for a moment (R-10).", true, true));

    // --- Audio: never the agent's (ADR-0125 d2) -----------------------------
    r.push_back(choice("audio.driverType", "Audio", "Driver type",
                       "The audio driver. ASIO is opened only through JUCE's wrapper; there is "
                       "no raw bypass (ADR-0145 d5). Empty means the system default.",
                       {"", "ASIO", "WASAPI", "DirectSound", "CoreAudio", "ALSA", "PipeWire/JACK"},
                       ""));
    r.push_back(choice("audio.bufferSize", "Audio", "Buffer size",
                       "Samples per block, chosen by hand: 64 to 4096. There is no 32 and no "
                       "automatic switching; a driver that grants less still runs and is shown "
                       "as granted (ADR-0145 d5).",
                       {64, 128, 256, 512, 1024, 2048, 4096}, 256));
    r.push_back(make("audio.silentResampling", Type::Bool, "Audio", "Resample silently",
                     "When the project and the hardware disagree on sample rate, resample "
                     "without showing the mismatch bar (ADR-0145 d3).",
                     false));
    r.push_back(make("audio.keepMonitoringLatency", Type::Bool, "Audio",
                     "Keep monitoring latency in recordings",
                     "Record what you heard, latency included, as Live does (ADR-0132 d10).",
                     true));
    r.push_back(choice("audio.linuxBackend", "Audio", "Linux backend",
                       "ALSA directly, or PipeWire through its JACK interface (ADR-0145 d4).",
                       {"ALSA", "PipeWire/JACK"}, "PipeWire/JACK"));
    r.push_back(make("audio.jackTransportSync", Type::Bool, "Audio", "JACK transport sync",
                     "Follow and drive the JACK transport (ADR-0145 d4).", false));

    // --- Record ----------------------------------------------------------------
    r.push_back(choice("record.quantize", "Record", "Record quantize",
                       "The quantize new recordings get, inherited by new tracks "
                       "(ADR-0132 d8).",
                       {"off", "1/4", "1/8", "1/16", "1/32"}, "1/16"));
    r.push_back(ranged("record.retrospectiveSeconds", Type::Int, "Record",
                       "Retrospective capture length",
                       "Seconds of armed or monitored input kept for Capture (ADR-0132 d9).",
                       30, 1, 600));
    r.push_back(make("record.importAutoWarp", Type::Bool, "Record", "Warp imported audio",
                     "Warp audio on import. Off by default: warp when you ask (ADR-0132 d6).",
                     false));
    r.push_back(make("record.importEdgeFades", Type::Bool, "Record", "Fade clip edges on import",
                     "Add short fades to imported clips. Off by default (ADR-0132 d6).", false));

    // --- MIDI and controllers ------------------------------------------------------------
    r.push_back(make("midi.chaseControllers", Type::Bool, "MIDI", "Chase controllers",
                     "Send the controller values in effect when playback starts mid-song (R-15).",
                     true));
    r.push_back(make("midi.chasePitchBend", Type::Bool, "MIDI", "Chase pitch bend",
                     "Send the pitch bend in effect when playback starts mid-song (R-15).", true));
    r.push_back(make("midi.chaseProgramChange", Type::Bool, "MIDI", "Chase program change",
                     "Send the program in effect when playback starts mid-song (R-15).", true));
    r.push_back(make("midi.autoAddControllers", Type::Bool, "MIDI", "Add controllers automatically",
                     "Recognise a connected control surface and set it up (R-14).", true));

    // --- Editing ------------------------------------------------------------------------
    r.push_back(choice("editing.modifierPreset", "Editing", "Modifier keys",
                       "Which DAW's modifier keys editing follows (R-22).",
                       {"live", "cubase", "bitwig"}, "live", true));
    r.push_back(make("editing.snapToGrid", Type::Bool, "Editing", "Snap to grid",
                     "Moves and resizes snap to the grid by default.", true, true));

    // --- Plug-ins: paths and scanning are never the agent's --------------------------------
    r.push_back(make("plugins.preferClap", Type::Bool, "Plug-ins", "Prefer CLAP",
                     "When a plug-in exists as CLAP and VST3, load the CLAP (R-18).", true));
    r.push_back(make("plugins.scanOutOfProcess", Type::Bool, "Plug-ins", "Scan out of process",
                     "Scan plug-ins in a separate process, with quarantine and a blocklist "
                     "(R-17).",
                     true));
    r.push_back(make("plugins.vst3Folders", Type::PathList, "Plug-ins", "VST3 folders",
                     "Extra folders scanned for VST3 plug-ins.", Value::array()));
    r.push_back(make("plugins.clapFolders", Type::PathList, "Plug-ins", "CLAP folders",
                     "Extra folders scanned for CLAP plug-ins, custom folders on Linux included "
                     "(ADR-0145 d4).",
                     Value::array()));
    r.push_back(make("plugins.lv2Folders", Type::PathList, "Plug-ins", "LV2 folders",
                     "Extra folders scanned for LV2 plug-ins on Linux (ADR-0145 d4).",
                     Value::array()));

    // --- Library: paths are roles in a bundle (R-05) -------------------------------------
    r.push_back(make("library.userLibrary", Type::Path, "Library", "User library",
                     "Your own presets, samples and clips.", ""));
    r.push_back(make("library.contentFolders", Type::PathList, "Library", "Content folders",
                     "Sample and content folders the browser shows.", Value::array()));
    r.push_back(make("library.recordFolder", Type::Path, "Library", "Default record folder",
                     "Where a new project records audio before it is saved.", ""));

    // --- Privacy: opt-in, off (R-20) ---------------------------------------------------------
    r.push_back(make("privacy.crashReports", Type::Bool, "Privacy", "Send crash reports",
                     "Opt in to sending crash reports. Off by default (R-20).", false));
    r.push_back(make("privacy.usageStatistics", Type::Bool, "Privacy", "Send usage statistics",
                     "Opt in to anonymous usage statistics. Off by default (R-20).", false));

    // --- AI -----------------------------------------------------------------------------------------
    r.push_back(choice("agent.tier", "AI", "Agent tier",
                       "What the agent may do: observe, propose a changeset, or apply "
                       "(AI-AGENT 2). The agent can never change this itself.",
                       {"observe", "propose", "apply"}, "propose"));
    r.push_back(ranged("agent.maxOpsPerRequest", Type::Int, "AI", "Changes per request",
                       "The most ops one agent request may contain (AI-AGENT 6.6).",
                       256, 1, 10000));

    catalogueSettings(r);

    // --- Project: rows in the .adi, changed by ops (R-03, R-26) -------------------------------
    r.push_back(project("project.name", Type::Text, "Project name",
                        "The project's name, shown in the title bar and snapshot names.", "",
                        "project.setName"));
    Setting rate = project("project.sampleRate", Type::Choice, "Sample rate",
                           "The project's sample rate, 44.1 to 768 kHz and nothing lower "
                           "(ADR-0157); the hardware follows it or resamples (ADR-0145 d3).",
                           48000, "project.setSampleRate");
    rate.choices = sampleRateLadder();
    r.push_back(std::move(rate));
    r.push_back(project("project.frameRate", Type::Text, "Frame rate",
                        "Video frame rate for timecode.", "25/1", "project.setFrameRate"));
    r.push_back(project("project.timecodeOrigin", Type::Int, "Timecode origin",
                        "Nanoseconds of timecode at the project's start.", 0,
                        "project.setTimecodeOrigin"));
    return r;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

const char* toString(Type t) {
    switch (t) {
        case Type::Bool:     return "bool";
        case Type::Int:      return "int";
        case Type::Real:     return "real";
        case Type::Text:     return "text";
        case Type::Choice:   return "choice";
        case Type::Path:     return "path";
        case Type::PathList: return "path list";
    }
    return "?";
}

const char* toString(Scope s) {
    switch (s) {
        case Scope::App:     return "App";
        case Scope::Project: return "Project";
        case Scope::Device:  return "Device";
    }
    return "?";
}

std::vector<Value> sampleRateLadder() {
    return {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};
}

const std::vector<Setting>& registry() {
    static const std::vector<Setting> all = build();
    return all;
}

const Setting* find(std::string_view key) {
    for (const auto& s : registry())
        if (s.key == key) return &s;
    return nullptr;
}

std::vector<std::string> pages() {
    std::vector<std::string> out;
    for (const auto& s : registry())
        if (std::find(out.begin(), out.end(), s.page) == out.end()) out.push_back(s.page);
    return out;
}

std::vector<const Setting*> search(std::string_view query) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : lower(query)) {
        if (c == ' ') {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    if (!word.empty()) words.push_back(word);

    std::vector<const Setting*> out;
    if (words.empty()) return out;
    for (const auto& s : registry()) {
        const std::string hay = lower(s.label + " " + s.help + " " + s.key + " " + s.page);
        bool all = true;
        for (const auto& w : words) all = all && hay.find(w) != std::string::npos;
        if (all) out.push_back(&s);
    }
    return out;
}

bool validate(const Setting& s, const Value& v, std::string& why) {
    const auto inRange = [&](double x) {
        if (s.max > s.min && (x < s.min || x > s.max)) {
            why = s.key + " must be between " + std::to_string(static_cast<long long>(s.min)) +
                  " and " + std::to_string(static_cast<long long>(s.max));
            return false;
        }
        return true;
    };
    switch (s.type) {
        case Type::Bool:
            if (v.is_boolean()) return true;
            why = s.key + " takes true or false";
            return false;
        case Type::Int:
            if (!v.is_number_integer()) { why = s.key + " takes a whole number"; return false; }
            return inRange(static_cast<double>(v.get<long long>()));
        case Type::Real:
            if (!v.is_number() || !std::isfinite(v.get<double>())) {
                why = s.key + " takes a finite number";
                return false;
            }
            return inRange(v.get<double>());
        case Type::Text:
        case Type::Path:
            if (v.is_string()) return true;
            why = s.key + " takes text";
            return false;
        case Type::PathList:
            if (v.is_array() && std::all_of(v.begin(), v.end(), [](const Value& e) { return e.is_string(); }))
                return true;
            why = s.key + " takes a list of folders";
            return false;
        case Type::Choice:
            if (std::find(s.choices.begin(), s.choices.end(), v) != s.choices.end()) return true;
            why = s.key + " does not offer " + v.dump();
            return false;
    }
    why = "unknown type";
    return false;
}

}  // namespace adi::settings
