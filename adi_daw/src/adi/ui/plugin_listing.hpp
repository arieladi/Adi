// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Browser's Plug-Ins (DF-W9; Live 12 manual 4.1, the browser's Plug-Ins label): the VST3 and CLAP
// plug-ins the app's loader found, in folders by format and then by vendor, each a name and a kind.
// No JUCE: the app fills it from the loader (`JuceDeviceLoader::knownPlugins`, `ClapHost::plugins`) on
// the scan's worker (`plugin_scan.hpp`, DF-W34), tests fill it by hand, and the Browser lays it out.
#pragma once
#include "adi/plugin_scanner.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
struct PluginEntry {
    // What a plug-in is, for the row's icon: an instrument, an audio effect, a MIDI effect, or not known
    // (a VST3 whose kind the loader's line does not carry until engine-win's catalogue, N58).
    enum class Kind { Unknown, Instrument, Effect, MidiEffect };
    std::string format; // "vst3" or "clap", as `plugin_refs.format` names them
    std::string uid;    // VST3: JUCE's identifier string; CLAP: the descriptor's id
    std::string name, vendor, version;
    std::string path; // the file or bundle the scan found it in
    Kind kind = Kind::Unknown;
};
struct PluginListing {
    std::vector<PluginEntry> plugins;
    std::size_t failed = 0; // files a complete scan could not read; the rest are listed
    bool complete = true;   // false: the scan stopped early (`stopped` says why); the list may be short
    std::string stopped;    // why the scan stopped early, a sentence's middle ("the plug-in scanner ...")
};
// `Scanner::probe`'s words when its helper could not start or was cancelled (src/adi/plugin_scanner.cpp).
inline constexpr std::string_view kHelperUnavailable = "scanner helper unavailable or scan cancelled";
// Why a scan stopped early, from the loader's report (DF-W34 b): the failure that ended it, the scanner
// itself when its path is empty (the registry could not open) or its helper could not start, else that
// file. A
// scan cancelled by the app says so. Empty for a scan that completed.
inline std::string stoppedReason(const scan::Report &report) {
    if (report.complete && !report.cancelled)
        return {};
    if (report.cancelled)
        return "the scan was stopped before it finished";
    for (auto it = report.failures.rbegin(); it != report.failures.rend(); ++it)
        if (it->state == scan::State::Unavailable || it->state == scan::State::Invalid ||
            it->state == scan::State::Cancelled) {
            const auto error = it->error.empty() ? std::string("no reason given") : it->error;
            if (it->path.empty())
                return "the plug-in scanner could not run (" + error + ")";
            // The helper that could not start (missing, blocked) is the scanner's failure, though the
            // probe names the file it was asked to read (`Scanner::probe`'s own words; a scan the app
            // cancelled is caught above).
            if (it->state == scan::State::Unavailable && it->error == kHelperUnavailable)
                return "the plug-in scanner could not run (its helper, adi_plugin_scanner, is missing or could "
                       "not start)";
            return it->path + " stopped the scan (" + error + ")";
        }
    return "the scan did not finish";
}
// The listing's state from the loader's report: the per-file failures of a complete scan are counted
// (the rest are listed); a scan that stopped early is named, never counted as "the rest".
inline void applyReport(PluginListing &listing, const scan::Report &report) {
    listing.complete = report.complete && !report.cancelled;
    listing.stopped = stoppedReason(report);
    listing.failed = listing.complete ? report.failures.size() : 0;
}
// A plug-in's kind from its CLAP descriptor: an instrument, a note effect (a MIDI effect) or an effect.
inline PluginEntry::Kind clapKind(bool instrument, std::string_view features) {
    if (instrument)
        return PluginEntry::Kind::Instrument;
    return features.find("note-effect") != std::string_view::npos ? PluginEntry::Kind::MidiEffect
                                                                   : PluginEntry::Kind::Effect;
}
// One VST3 line of `JuceDeviceLoader::knownPlugins()`: "vst3  <uid>  <name> -- <vendor> <version>  (<path>)".
// Nothing for a line of another format, or one not in that shape.
inline std::optional<PluginEntry> parseKnownVst3(const std::string &line) {
    const std::string_view text(line);
    constexpr std::string_view kSep = "  ";
    if (text.substr(0, 6) != "vst3  ")
        return std::nullopt;
    const auto uidEnd = text.find(kSep, 6);
    const auto pathStart = text.rfind("  (");
    if (uidEnd == std::string_view::npos || pathStart == std::string_view::npos || pathStart <= uidEnd ||
        text.back() != ')')
        return std::nullopt;
    PluginEntry e;
    e.format = "vst3";
    e.uid = std::string(text.substr(6, uidEnd - 6));
    e.path = std::string(text.substr(pathStart + 3, text.size() - pathStart - 4));
    const auto middle = text.substr(uidEnd + kSep.size(), pathStart - uidEnd - kSep.size());
    const auto dash = middle.rfind(" -- ");
    if (e.uid.empty() || dash == std::string_view::npos)
        return std::nullopt;
    e.name = std::string(middle.substr(0, dash));
    auto rest = middle.substr(dash + 4);
    // The version is the last word; what is before it is the vendor, which may hold spaces or be empty.
    const auto space = rest.rfind(' ');
    if (space == std::string_view::npos)
        e.version = std::string(rest);
    else {
        e.vendor = std::string(rest.substr(0, space));
        e.version = std::string(rest.substr(space + 1));
    }
    return e;
}
// The Browser's folders: one per format (CLAP, then VST3, in Live's alphabetical order of its formats'
// folders), and in each one per vendor, its plug-ins by name; those whose name or vendor holds `query`
// (lower case; empty keeps all). A folder left empty by the query is left out.
struct PluginGroup {
    std::string format, vendor;
    std::vector<const PluginEntry *> entries;
};
inline std::string lowerCase(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
// A plug-in with no vendor goes in this folder.
inline constexpr const char *kNoVendor = "Other";
inline std::vector<PluginGroup> pluginGroups(const PluginListing &listing, const std::string &query) {
    std::vector<const PluginEntry *> kept;
    for (const auto &p : listing.plugins)
        if (query.empty() || lowerCase(p.name).find(query) != std::string::npos ||
            lowerCase(p.vendor).find(query) != std::string::npos)
            kept.push_back(&p);
    const auto vendorOf = [](const PluginEntry *p) { return p->vendor.empty() ? std::string(kNoVendor) : p->vendor; };
    std::stable_sort(kept.begin(), kept.end(), [&vendorOf](const PluginEntry *a, const PluginEntry *b) {
        if (a->format != b->format)
            return a->format < b->format;
        if (const auto va = lowerCase(vendorOf(a)), vb = lowerCase(vendorOf(b)); va != vb)
            return va < vb;
        return lowerCase(a->name) < lowerCase(b->name);
    });
    // One folder per vendor ignoring case (DF-W34 c), named as the first of its plug-ins in sort order
    // spells it: (FabFilter, Fabfilter) is one folder, and no vendor goes in with one named "other".
    std::vector<PluginGroup> out;
    std::string key;
    for (const auto *p : kept) {
        auto next = lowerCase(vendorOf(p));
        if (out.empty() || out.back().format != p->format || key != next) {
            out.push_back({p->format, vendorOf(p), {}});
            key = std::move(next);
        }
        out.back().entries.push_back(p);
    }
    return out;
}
// A format's folder name, as Live writes its own.
inline std::string formatFolder(const std::string &format) {
    return format == "vst3" ? "VST3" : format == "clap" ? "CLAP" : format;
}
} // namespace adi::ui
