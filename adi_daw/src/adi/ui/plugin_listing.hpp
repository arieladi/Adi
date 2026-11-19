// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Browser's Plug-Ins (DF-W9; Live 12 manual 4.1, the browser's Plug-Ins label): the VST3 and CLAP
// plug-ins the app's loader found, in folders by format and then by vendor, each a name and a kind.
// No JUCE: the app fills it from the loader (`JuceDeviceLoader::knownPlugins`, `ClapHost::plugins`),
// tests fill it by hand, and the Browser lays it out.
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
    // (a VST3 whose kind the loader's line does not carry: N58).
    enum class Kind { Unknown, Instrument, Effect, MidiEffect };
    std::string format; // "vst3" or "clap", as `plugin_refs.format` names them
    std::string uid;    // VST3: JUCE's identifier string; CLAP: the descriptor's id
    std::string name, vendor, version;
    std::string path; // the file or bundle the scan found it in
    Kind kind = Kind::Unknown;
};
struct PluginListing {
    std::vector<PluginEntry> plugins;
    std::size_t failed = 0; // files the scan could not read; the rest are listed
    std::string error;      // the scan stopped, or could not run at all
    bool scanning = false;  // the first scan is still to run: the list comes when it is done
};
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
    // One folder per vendor whatever its spelling's case (FabFilter and Fabfilter are one maker), named
    // by the first spelling in the sort's order (DF-W34 c).
    std::vector<PluginGroup> out;
    std::string folderKey;
    for (const auto *p : kept) {
        const auto key = lowerCase(vendorOf(p));
        if (out.empty() || out.back().format != p->format || folderKey != key) {
            out.push_back({p->format, vendorOf(p), {}});
            folderKey = key;
        }
        out.back().entries.push_back(p);
    }
    return out;
}
// What a scan found, as the Browser lists it: the loader's VST3 lines, its CLAP plug-ins, and its
// report. A scan that stopped part way is named by its own failure: one with no file is the scan
// service's (the helper, the registry), not a plug-in's (DF-W34 b).
inline PluginListing listingOf(const std::vector<std::string> &vst3Lines, std::vector<PluginEntry> clap,
                               const scan::Report &report) {
    PluginListing out;
    for (const auto &line : vst3Lines)
        if (auto entry = parseKnownVst3(line))
            out.plugins.push_back(std::move(*entry));
    for (auto &entry : clap)
        out.plugins.push_back(std::move(entry));
    out.failed = 0;
    for (const auto &f : report.failures)
        if (!f.path.empty())
            ++out.failed;
    if (report.cancelled)
        out.error = "the scan was cancelled.";
    else if (!report.complete) {
        const scan::Failure *why = nullptr;
        for (const auto &f : report.failures)
            if (why == nullptr || (f.path.empty() && !why->path.empty()))
                why = &f;
        out.error = why == nullptr || why->error.empty() ? std::string("the scan stopped part way.")
                    : why->path.empty()                 ? "the scan service failed: " + why->error
                                                        : why->path + ": " + why->error;
    }
    return out;
}
// A format's folder name, as Live writes its own.
inline std::string formatFolder(const std::string &format) {
    return format == "vst3" ? "VST3" : format == "clap" ? "CLAP" : format;
}
} // namespace adi::ui
