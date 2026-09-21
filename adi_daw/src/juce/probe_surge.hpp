// SPDX-License-Identifier: GPL-3.0-or-later
//
// Making Surge XT's pressure and timbre AUDIBLE, for the probes (ADR-0100).
//
// An init patch routes neither dimension to anything, so a synth that received
// both perfectly sounds identical to one that received neither. This edits
// Surge's own saved patch so that:
//
//   pressure -> oscillator 1 level: 0.25 + 0.75 x pressure
//               (channel aftertouch, source 4, which Surge feeds per voice in
//               MPE mode and from CLAP's PRESSURE expression; and poly
//               aftertouch, source 3, which plain MIDI's poly pressure drives)
//   timbre   -> a 24 dB low-pass at ~311 Hz, opened 48 semitones by timbre 1
//               (source 29: CC74 per voice in MPE mode, or CLAP's BRIGHTNESS)
//
// The base level is 0.25, not 0, on purpose: a pressure that never arrives
// still makes sound, so "not delivered" reads as "unchanged" and not as
// "silent" -- the two must not be confused.
//
// ONE DIMENSION PER PATCH. The low-pass makes E4 darker AND quieter than C4
// whether or not anything arrived, so a pressure patch with the filter on
// would read that tilt as pressure. The pressure patch leaves the filter off;
// the timbre patch leaves the level alone. And the two-note scene is played
// both ways round -- see `measureDimension`.
//
// Surge's patch format is its own and is GPL-3.0 like this project: a 32-byte
// "sub3" header whose second field is the XML's length, the XML, then
// wavetable data. Modulation is a <modrouting source= depth=> child of the
// parameter it modulates, in that parameter's own units. Source numbers are
// Surge's `modsources` enum (ModulationSource.h); LP 24 dB is sst-filters'
// `fut_lp24` = 2.

#pragma once

#include "adi/blob.hpp"
#include "adi/engine/events.hpp"
#include "juce/probe_audio.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace adi::probe {

inline constexpr int kSurgePolyAftertouch = 3;
inline constexpr int kSurgeChannelAftertouch = 4;
inline constexpr int kSurgeTimbre = 29;
inline constexpr int kSurgeLp24 = 2;

/// Replace the self-closing element `<name .../>` with `replacement`.
inline bool replaceElement(std::string& xml, const std::string& name, const std::string& replacement,
                           std::string& why) {
    const std::string open = "<" + name + " ";
    const std::size_t a = xml.find(open);
    if (a == std::string::npos) { why = "no <" + name + ">"; return false; }
    const std::size_t close = xml.find('>', a);
    if (close == std::string::npos || close == 0 || xml[close - 1] != '/') {
        why = "<" + name + "> is not self-closing -- it already has modulation";
        return false;
    }
    xml.replace(a, close + 1 - a, replacement);
    return true;
}

/// The routing for one dimension, applied to Surge's patch XML.
inline bool routeDimension(std::string& xml, ExpressionDim d, std::string& why) {
    if (d == ExpressionDim::Pressure) {
        const std::string p3 = std::to_string(kSurgePolyAftertouch);
        const std::string p4 = std::to_string(kSurgeChannelAftertouch);
        return replaceElement(xml, "a_level_o1",
                              "<a_level_o1 type=\"2\" value=\"0.25\">"
                              "<modrouting source=\"" + p3 + "\" depth=\"0.75\" />"
                              "<modrouting source=\"" + p4 + "\" depth=\"0.75\" /></a_level_o1>", why);
    }
    if (d == ExpressionDim::Timbre) {
        const std::string t = std::to_string(kSurgeTimbre);
        return replaceElement(xml, "a_filter1_type",
                              "<a_filter1_type type=\"0\" value=\"" + std::to_string(kSurgeLp24) +
                              "\" deactivated=\"0\" />", why) &&
               replaceElement(xml, "a_filter1_cutoff",
                              "<a_filter1_cutoff type=\"2\" value=\"-6.0\" extend_range=\"0\">"
                              "<modrouting source=\"" + t + "\" depth=\"48.0\" /></a_filter1_cutoff>", why);
    }
    why = "only pressure and timbre are routed";
    return false;
}

/// Find Surge's patch anywhere in `blob` -- raw from its CLAP state, or inside
/// whatever a host wrapped it in -- apply the routings, and fix the header's
/// XML length. Bytes after the XML (wavetables, a wrapper's trailer) are kept.
inline bool routeSurgePatch(std::vector<std::uint8_t>& blob, ExpressionDim d, std::string& why) {
    const char tag[4] = {'s', 'u', 'b', '3'};
    std::size_t at = std::string::npos;
    for (std::size_t i = 0; i + 32 <= blob.size(); ++i)
        if (std::memcmp(blob.data() + i, tag, 4) == 0) { at = i; break; }
    if (at == std::string::npos) { why = "no Surge patch (sub3) in the state"; return false; }
    const std::uint32_t xmlSize = static_cast<std::uint32_t>(blob[at + 4]) |
                                  (static_cast<std::uint32_t>(blob[at + 5]) << 8) |
                                  (static_cast<std::uint32_t>(blob[at + 6]) << 16) |
                                  (static_cast<std::uint32_t>(blob[at + 7]) << 24);
    const std::size_t xmlAt = at + 32;
    if (xmlAt + xmlSize > blob.size()) { why = "the patch's XML length runs past the state"; return false; }
    std::string xml(reinterpret_cast<const char*>(blob.data() + xmlAt), xmlSize);
    if (!routeDimension(xml, d, why)) return false;
    std::vector<std::uint8_t> out(blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(xmlAt));
    out.insert(out.end(), xml.begin(), xml.end());
    out.insert(out.end(), blob.begin() + static_cast<std::ptrdiff_t>(xmlAt + xmlSize), blob.end());
    const auto n = static_cast<std::uint32_t>(xml.size());
    for (int b = 0; b < 4; ++b)
        out[at + 4 + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>((n >> (8 * b)) & 0xFF);
    blob.swap(out);
    return true;
}

// --- the scenes and what they measure ---------------------------------------

/// One dimension's scenes: C4 high; C4 low; and two notes together both ways
/// round -- C4 high with E4 low, then C4 low with E4 high. Every value is set
/// at the note's instant, from controller channels 2 and 3.
///
/// HIGH IS 1.0; LOW IS THE ROUTE'S NEUTRAL. For every dimension but MPE's
/// timbre that is 0. MPE does not say whether CC74 is unipolar or bipolar,
/// and Surge XT 1.3.4 reads it BIPOLAR around 64 -- so CC74 0 closes its
/// filter a further 48 semitones and the note all but vanishes (-123 dB was
/// measured), and a brightness ratio of a vanished note is two noise floors.
/// CLAP's BRIGHTNESS is unipolar by definition. On the MPE route timbre's
/// low is therefore 0.5 -- CC74 64, MPE's neutral, the value the router
/// resets a channel to -- which sounds the same cutoff CLAP's 0 does.
struct DimScenes {
    std::vector<engine::Event> high, low, pairA, pairB;
};

inline DimScenes dimScenes(ExpressionDim d, double low = 0.0) {
    auto note = [](std::uint64_t id, int key, int chan) {
        engine::Event e; e.type = engine::EventType::NoteOn; e.noteId = id;
        e.dim = static_cast<std::uint16_t>(key); e.channel = static_cast<std::uint8_t>(chan);
        e.value = 0.8; return e;
    };
    auto set = [d](std::uint64_t id, double v, int chan) {
        engine::Event e; e.type = engine::EventType::NoteExpression; e.noteId = id;
        e.dim = static_cast<std::uint16_t>(d); e.channel = static_cast<std::uint8_t>(chan);
        e.value = v; return e;
    };
    DimScenes s;
    s.high = {note(1, 60, 2), set(1, 1.0, 2)};
    s.low = {note(1, 60, 2), set(1, low, 2)};
    s.pairA = {note(1, 60, 2), set(1, 1.0, 2), note(2, 64, 3), set(2, low, 3)};
    s.pairB = {note(1, 60, 2), set(1, low, 2), note(2, 64, 3), set(2, 1.0, 3)};
    return s;
}

/// What one dimension did, in dB.
///   single  C4 at 1.0 against C4 at 0.0: does the dimension reach a note?
///   pair    half of (C4 - E4 with C4 high) - (C4 - E4 with E4 high): does it
///           reach EACH note on its own? Playing both ways round cancels
///           anything the two keys differ by anyway -- a filter tilt -- and a
///           GLOBAL application (the last value set wins for every note)
///           makes both halves equal, so it reads 0 here while `single` does
///           not.
/// Pressure is measured as the fundamental's level; timbre as brightness, the
/// 3rd harmonic over the fundamental.
struct DimHeard {
    double single = 0.0, pair = 0.0;
    /// The four notes behind the numbers, for when a verdict needs explaining:
    /// C4 high/low alone, then C4 and E4 in pair A (C4 high) and pair B.
    double c4Hi = 0.0, c4Lo = 0.0, c4A = 0.0, e4A = 0.0, c4B = 0.0, e4B = 0.0;
    enum class Verdict { PerNote, NotDelivered, Wrong } verdict = Verdict::Wrong;
};

inline double noteMeasure(ExpressionDim d, const std::vector<float>& a, std::size_t from,
                          std::size_t n, double sr, double key) {
    const double f = toneLevel(a, from, n, sr, midiHz(key));
    if (d == ExpressionDim::Pressure) return dbRel(f, 1.0);
    return dbRel(toneLevel(a, from, n, sr, 3.0 * midiHz(key)), f);
}

/// Delivered per note, or not delivered at all. Anything else -- one note
/// responds and the pair does not, which is a global application -- is Wrong.
inline DimHeard::Verdict judge(double single, double pair) {
    if (single >= 6.0 && pair >= 6.0) return DimHeard::Verdict::PerNote;
    if (std::abs(single) < 3.0 && std::abs(pair) < 3.0) return DimHeard::Verdict::NotDelivered;
    return DimHeard::Verdict::Wrong;
}

inline const char* verdictName(DimHeard::Verdict v) {
    return v == DimHeard::Verdict::PerNote ? "per note"
         : v == DimHeard::Verdict::NotDelivered ? "not delivered" : "WRONG";
}

/// Play one dimension's four scenes through `render` (events -> audio) and judge.
template <typename Render>
DimHeard measureDimension(ExpressionDim d, Render&& render, std::size_t from, std::size_t n, double sr,
                          double low = 0.0) {
    const DimScenes s = dimScenes(d, low);
    const auto hi = render(s.high), lo = render(s.low), a = render(s.pairA), b = render(s.pairB);
    DimHeard h;
    h.c4Hi = noteMeasure(d, hi, from, n, sr, 60);
    h.c4Lo = noteMeasure(d, lo, from, n, sr, 60);
    h.c4A = noteMeasure(d, a, from, n, sr, 60);
    h.e4A = noteMeasure(d, a, from, n, sr, 64);
    h.c4B = noteMeasure(d, b, from, n, sr, 60);
    h.e4B = noteMeasure(d, b, from, n, sr, 64);
    h.single = h.c4Hi - h.c4Lo;
    h.pair = 0.5 * ((h.c4A - h.e4A) - (h.c4B - h.e4B));
    h.verdict = judge(h.single, h.pair);
    return h;
}

// --- baselines ---------------------------------------------------------------
//
// "Delivered per note, or not at all" is the rule for a plugin nobody has
// measured -- whether it reads a route is its own business. For a plugin that
// HAS been measured it is too weak: a defect that quietly stops one route
// delivering reads as "not delivered", which the rule allows. So each probe
// also holds what was measured, keyed by plugin name AND version, and any
// change from it fails. A different version is reported and not judged.

/// One measured result. `what` names the route or dialect and the dimension.
struct Baseline {
    const char* plugin;
    const char* version;
    const char* what;
    const char* verdict;
};

/// The baseline for `plugin`/`version`/`what`, or nullptr when there is none.
inline const char* baselineFor(const Baseline* table, std::size_t n, const std::string& plugin,
                               const std::string& version, const std::string& what) {
    for (std::size_t i = 0; i < n; ++i)
        if (plugin == table[i].plugin && version == table[i].version && what == table[i].what)
            return table[i].verdict;
    return nullptr;
}

/// Whether any baseline exists for this plugin at this version.
inline bool hasBaseline(const Baseline* table, std::size_t n, const std::string& plugin,
                        const std::string& version) {
    for (std::size_t i = 0; i < n; ++i)
        if (plugin == table[i].plugin && version == table[i].version) return true;
    return false;
}

}  // namespace adi::probe
