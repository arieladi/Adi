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

std::vector<Setting> build() {
    std::vector<Setting> r;
    // --- Look & Feel: the agent's whitelist lives mostly here (ADR-0125 d2) --
    r.push_back(choice("lookfeel.theme", "Look & Feel", "Theme",
                       "Colour theme of the whole application.", {"dark", "light"}, "dark", true));
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

    // --- Project: rows in the .adi, changed by ops (R-03, R-26) -------------------------------
    r.push_back(project("project.name", Type::Text, "Project name",
                        "The project's name, shown in the title bar and snapshot names.", "",
                        "project.setName"));
    r.push_back(project("project.sampleRate", Type::Int, "Sample rate",
                        "The project's sample rate; the hardware follows it or resamples "
                        "(ADR-0145 d3).",
                        48000, "project.setSampleRate"));
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
