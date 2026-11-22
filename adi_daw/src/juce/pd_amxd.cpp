// SPDX-License-Identifier: GPL-3.0-or-later

#include "juce/pd_amxd.hpp"

#include <nlohmann/json.hpp>

#include "juce/pd_object_walk.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>

namespace adi::device {

namespace {

using nlohmann::json;

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

bool isNumber(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

/// A Max number written as a float ("1.", "0.5", "1e3") rather than an int.
bool isFloatLiteral(const std::string& s) {
    return isNumber(s) && s.find_first_of(".eE") != std::string::npos;
}

/// Pd file escaping for message and comment text.
std::string pdEscape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == ',' || c == ';') { out += " \\"; out += c; out += ' '; }
        else if (c == '$') out += "\\$";
        else out += c;
    }
    return out;
}

std::string number(double v) {
    std::ostringstream s;
    s.precision(10);
    s << v;
    return s.str();
}

struct Box {
    std::string id, maxClass, text;
    std::vector<std::string> words;   ///< newobj text split
    double x = 0, y = 0, width = 0;
    int inlets = 0;
    std::vector<bool> signalOutlet;
    json raw;
    /// Why the box's #-arguments could not be replaced; empty when they were.
    std::string unreplaced;
};

/// What a bpatcher gives the patcher inside it (and that patcher's own [p]
/// subpatchers): its arguments for #1 to #9, and for #0 a name that belongs
/// to this one instance, under the device's $0.
struct Substitution {
    bool active = false;
    std::vector<std::string> args;
    std::string instance;
};

/// A name usable as a Pd canvas name or inside a send name.
std::string plainName(std::string s) {
    for (char& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.')) c = '_';
    return s;
}

struct Line {
    std::string src, dst;
    int outlet = 0, inlet = 0;
};

/// What one Max box becomes: Pd boxes (a subpatcher is one record holding its
/// whole canvas), the connections among them, and where the Max box's inlets
/// and outlets landed. No maps: inlet n is box 0's inlet n, and so on.
struct Fragment {
    std::vector<std::string> records;
    std::vector<std::array<int, 4>> internal;   ///< local src, outlet, local dst, inlet
    std::vector<std::pair<int, int>> inlets, outlets;
    /// Where a CONTROL source lands on a Max inlet that also takes a signal
    /// (biquad~'s coefficient list), when that differs from `inlets`.
    std::map<int, std::pair<int, int>> controlInlets;
    [[nodiscard]] std::pair<int, int> in(int n) const {
        return static_cast<std::size_t>(n) < inlets.size() ? inlets[static_cast<std::size_t>(n)]
                                                           : std::pair<int, int>{0, n};
    }
    [[nodiscard]] std::pair<int, int> out(int n) const {
        return static_cast<std::size_t>(n) < outlets.size() ? outlets[static_cast<std::size_t>(n)]
                                                            : std::pair<int, int>{0, n};
    }
};

std::string at(double x, double y) {
    return std::to_string(static_cast<long>(std::lround(x))) + " " +
           std::to_string(static_cast<long>(std::lround(y)));
}

bool isIntLiteral(const std::string& s) { return isNumber(s) && !isFloatLiteral(s); }

/// One Max patcher -- the device's, or a subpatcher's -- as one Pd canvas.
class Canvas {
public:
    Canvas(AmxdTranslation& out, std::string prefix, std::set<int> signalInlets, int depth, Substitution sub = {})
        : out_(out), prefix_(std::move(prefix)), signalInlets_(std::move(signalInlets)), depth_(depth),
          sub_(std::move(sub)) {}

    /// False when the patcher could not be read; refusals go to `out`.
    bool read(const json& patcher) {
        if (patcher.contains("boxes") && patcher["boxes"].is_array())
            for (const auto& entry : patcher["boxes"]) {
                if (!entry.contains("box") || !entry["box"].is_object()) continue;
                const json& b = entry["box"];
                Box box;
                box.id = b.value("id", "");
                box.maxClass = b.value("maxclass", "");
                box.text = b.value("text", "");
                box.raw = b;
                if (sub_.active && (box.maxClass == "newobj" || box.maxClass == "message"))
                    box.unreplaced = replaceArguments(box.text, box.maxClass == "newobj");
                if (b.contains("patching_rect") && b["patching_rect"].is_array() &&
                    b["patching_rect"].size() >= 2) {
                    box.x = b["patching_rect"][0].get<double>();
                    if (b["patching_rect"].size() >= 3 && b["patching_rect"][2].is_number())
                        box.width = b["patching_rect"][2].get<double>();
                    box.y = b["patching_rect"][1].get<double>();
                }
                if (b.contains("outlettype") && b["outlettype"].is_array())
                    for (const auto& t : b["outlettype"]) {
                        const std::string s = t.is_string() ? t.get<std::string>() : "";
                        box.signalOutlet.push_back(s == "signal" || s == "multichannelsignal");
                    }
                if (b.contains("numinlets") && b["numinlets"].is_number()) box.inlets = b["numinlets"].get<int>();
                if (box.maxClass == "newobj") box.words = words(box.text);
                index_[box.id] = boxes_.size();
                boxes_.push_back(std::move(box));
            }
        if (patcher.contains("lines") && patcher["lines"].is_array())
            for (const auto& entry : patcher["lines"]) {
                if (!entry.contains("patchline")) continue;
                const json& l = entry["patchline"];
                if (!l.contains("source") || !l.contains("destination")) continue;
                Line line;
                line.src = l["source"][0].get<std::string>();
                line.outlet = l["source"][1].get<int>();
                line.dst = l["destination"][0].get<std::string>();
                line.inlet = l["destination"][1].get<int>();
                if (index_.count(line.src) == 0 || index_.count(line.dst) == 0) {
                    out_.error = "a patch line names a box that is not there";
                    return false;
                }
                lines_.push_back(line);
            }
        // Inlet boxes carry a signal when the parent feeds that inlet one;
        // Max numbers a patcher's inlets and outlets left to right, as Pd does.
        inletBoxes_ = sortedByX("inlet");
        outletBoxes_ = sortedByX("outlet");
        for (std::size_t k = 0; k < inletBoxes_.size(); ++k) {
            Box& b = boxes_[index_.at(inletBoxes_[k])];
            b.signalOutlet = {signalInlets_.count(static_cast<int>(k)) != 0};
        }
        // A subpatcher box's outlets: signal where its inner outlet is fed one.
        for (auto& b : boxes_)
            if (isSubpatcher(b) && b.raw.contains("patcher")) {
                AmxdTranslation scratch;
                Canvas probe(scratch, "", signalInletsOf(b), depth_ + 1);
                if (probe.read(b.raw["patcher"])) b.signalOutlet = probe.outletSignals();
            }
        return true;
    }

    /// The canvas's records and connections; empty if anything was refused.
    std::string body() {
        for (const auto& b : boxes_) translate(b);
        if (!out_.refused.empty() || !out_.error.empty()) return {};
        std::ostringstream o, connects;
        std::map<std::string, int> base;
        int n = 0;
        for (const auto& b : boxes_) {
            const auto f = frags_.find(b.id);
            if (f == frags_.end()) continue;
            base[b.id] = n;
            for (const auto& r : f->second.records) o << r << ";\n";
            for (const auto& c : f->second.internal)
                connects << "#X connect " << n + c[0] << " " << c[1] << " " << n + c[2] << " " << c[3] << ";\n";
            n += static_cast<int>(f->second.records.size());
        }
        const auto src = [&](const Line& l) {
            const auto p = frags_.at(l.src).out(l.outlet);
            return std::pair<int, int>{base.at(l.src) + p.first, p.second};
        };
        const auto dst = [&](const Line& l) {
            const Fragment& f = frags_.at(l.dst);
            const auto c = f.controlInlets.find(l.inlet);
            const auto p = c != f.controlInlets.end() && !isSignalSource(l) ? c->second : f.in(l.inlet);
            return std::pair<int, int>{base.at(l.dst) + p.first, p.second};
        };
        std::map<std::pair<std::string, int>, std::vector<const Line*>> fan;
        for (const auto& l : lines_) fan[{l.src, l.outlet}].push_back(&l);
        for (auto& [key, ls] : fan) {
            const Box& from = boxes_[index_.at(key.first)];
            const auto o2 = static_cast<std::size_t>(key.second);
            const bool signal = o2 < from.signalOutlet.size() && from.signalOutlet[o2];
            if (signal || ls.size() == 1) {
                for (const Line* l : ls) {
                    const auto s = src(*l), d = dst(*l);
                    connects << "#X connect " << s.first << " " << s.second << " " << d.first << " " << d.second
                             << ";\n";
                }
                continue;
            }
            // Max: right to left by the x of the destination INLET (its box's
            // left edge plus its place along the box: inlets are spread evenly
            // over its width), so one outlet into two inlets of one box feeds
            // the right inlet first; equal x, lower box first.
            const auto inletX = [&](const Line* l) {
                const Box& d = boxes_[index_.at(l->dst)];
                if (d.inlets < 2) return d.x + static_cast<double>(l->inlet);
                return d.x + std::max(d.width - 7.0, 0.0) * static_cast<double>(l->inlet) / static_cast<double>(d.inlets - 1) +
                       1e-3 * static_cast<double>(l->inlet);
            };
            std::sort(ls.begin(), ls.end(), [&](const Line* a, const Line* b) {
                const double xa = inletX(a), xb = inletX(b);
                if (xa != xb) return xa > xb;
                return boxes_[index_.at(a->dst)].y > boxes_[index_.at(b->dst)].y;
            });
            std::string t = "t";
            for (std::size_t i = 0; i < ls.size(); ++i) t += " a";
            o << "#X obj " << at(from.x, from.y + 25) << " " << t << ";\n";
            const int trig = n++;
            const auto s = src(*ls.front());
            connects << "#X connect " << s.first << " " << s.second << " " << trig << " 0;\n";
            // [t] fires its rightmost outlet first: the first destination.
            for (std::size_t i = 0; i < ls.size(); ++i) {
                const auto d = dst(*ls[i]);
                connects << "#X connect " << trig << " " << (ls.size() - 1 - i) << " " << d.first << " "
                         << d.second << ";\n";
            }
        }
        return o.str() + connects.str();
    }

    [[nodiscard]] std::vector<bool> outletSignals() const {
        std::vector<bool> s;
        for (const auto& id : outletBoxes_) s.push_back(fedSignal(boxes_[index_.at(id)], 0));
        return s;
    }

private:
    static bool isSubpatcher(const Box& b) {
        return (b.maxClass == "newobj" && !b.words.empty() && (b.words[0] == "p" || b.words[0] == "patcher")) ||
               (b.maxClass == "bpatcher" && embedded(b));
    }

    static bool embedded(const Box& b) {
        if (!b.raw.contains("embed")) return false;
        const json& e = b.raw["embed"];
        return (e.is_number() && e.get<double>() != 0) || (e.is_boolean() && e.get<bool>());
    }

    /// Replaces #0 to #9 in a box's text as Max does when a bpatcher's patcher
    /// is loaded: #n by the bpatcher's n-th argument, #0 by the instance's own
    /// name (written as Pd's $0, escaped in an object box's text). Returns why
    /// it could not, or nothing.
    std::string replaceArguments(std::string& text, bool objectBox) const {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const bool hash = text[i] == '#' && i + 1 < text.size() &&
                              std::isdigit(static_cast<unsigned char>(text[i + 1])) != 0;
            if (!hash) {
                out += text[i];
                continue;
            }
            const auto n = static_cast<std::size_t>(text[i + 1] - '0');
            if (n == 0) {
                out += std::string(objectBox ? "\\$0-" : "$0-") + sub_.instance;
            } else if (n <= sub_.args.size()) {
                out += sub_.args[n - 1];
            } else {
                return "#" + std::to_string(n) + " has no argument: the bpatcher gives " +
                       std::to_string(sub_.args.size());
            }
            ++i;
        }
        text = std::move(out);
        return {};
    }

    std::vector<std::string> sortedByX(const char* cls) const {
        std::vector<const Box*> v;
        for (const auto& b : boxes_)
            if (b.maxClass == cls) v.push_back(&b);
        std::stable_sort(v.begin(), v.end(), [](const Box* a, const Box* b) { return a->x < b->x; });
        std::vector<std::string> ids;
        for (const Box* b : v) ids.push_back(b->id);
        return ids;
    }

    std::set<int> signalInletsOf(const Box& b) const {
        std::set<int> s;
        for (const auto& l : lines_)
            if (l.dst == b.id && isSignalSource(l)) s.insert(l.inlet);
        return s;
    }

    bool isSignalSource(const Line& l) const {
        const Box& s = boxes_[index_.at(l.src)];
        const auto o = static_cast<std::size_t>(l.outlet);
        return o < s.signalOutlet.size() && s.signalOutlet[o];
    }

    void refuse(const Box& b, const std::string& cls, std::string why) {
        out_.refused.push_back(AmxdRefusedBox{prefix_ + b.id, cls, b.text, std::move(why)});
    }

    bool connected(const Box& b, bool asSource, int port) const {
        for (const auto& l : lines_)
            if (asSource ? (l.src == b.id && l.outlet == port) : (l.dst == b.id && l.inlet == port))
                return true;
        return false;
    }
    bool fedSignal(const Box& b, int inlet) const {
        for (const auto& l : lines_)
            if (l.dst == b.id && l.inlet == inlet && isSignalSource(l)) return true;
        return false;
    }
    bool anyConnection(const Box& b) const {
        return std::any_of(lines_.begin(), lines_.end(),
                           [&](const Line& l) { return l.src == b.id || l.dst == b.id; });
    }

    void single(const Box& b, const std::string& record) {
        Fragment f;
        f.records = {record};
        frags_[b.id] = std::move(f);
    }
    void obj(const Box& b, const std::string& text) { single(b, "#X obj " + at(b.x, b.y) + " " + text); }

    /// A Max integer box: each listed inlet goes through [i] first; with
    /// `truncateOut`, the result through [i] too (integer division).
    ///
    /// The left inlet takes what Max's does: a list sets both operands
    /// (first left, second right) and computes; "set n" stores n without
    /// output (into the [i]'s cold inlet, which a later bang sends on).
    /// With `divides`, a divisor of 0 divides by 1, as Max's int division
    /// does.
    void integer(const Box& b, const std::string& pdText, int inlets, bool truncateOut, bool divides = false) {
        Fragment f;
        for (int k = 0; k < inlets; ++k) f.records.push_back("#X obj " + at(b.x + 40.0 * k, b.y - 20) + " i");
        const int op = inlets;
        f.records.push_back("#X obj " + at(b.x, b.y) + " " + pdText);
        for (int k = 0; k < inlets; ++k) {
            if (!(divides && k == 1)) f.internal.push_back({k, 0, op, k});
            f.inlets.emplace_back(k, 0);
        }
        f.outlets = {{op, 0}};
        if (truncateOut) {
            f.records.push_back("#X obj " + at(b.x, b.y + 20) + " i");
            f.internal.push_back({op, 0, op + 1, 0});
            f.outlets = {{op + 1, 0}};
        }
        if (divides && inlets == 2) {   // [i] -> [sel 0]: 0 becomes 1, anything else passes
            const int sel = static_cast<int>(f.records.size());
            f.records.push_back("#X obj " + at(b.x + 40, b.y - 10) + " sel 0");
            f.records.push_back("#X msg " + at(b.x + 40, b.y - 5) + " 1");
            f.internal.push_back({1, 0, sel, 0});
            f.internal.push_back({sel, 0, sel + 1, 0});
            f.internal.push_back({sel + 1, 0, op, 1});
            f.internal.push_back({sel, 1, op, 1});
        }
        const int route = static_cast<int>(f.records.size());
        f.records.push_back("#X obj " + at(b.x, b.y - 60) + " route set list");
        f.internal.push_back({route, 0, 0, 1});     // set: stored, no output
        f.internal.push_back({route, 2, 0, 0});     // a number or a bang
        if (inlets == 2) {
            f.records.push_back("#X obj " + at(b.x + 60, b.y - 40) + " unpack f f");
            f.internal.push_back({route, 1, route + 1, 0});
            f.internal.push_back({route + 1, 1, 1, 0});   // unpack fires right to left: the right operand first
            f.internal.push_back({route + 1, 0, 0, 0});
        } else {
            f.internal.push_back({route, 1, 0, 0});
        }
        f.inlets[0] = {route, 0};
        frags_[b.id] = std::move(f);
    }

    void translate(const Box& b) {
        const std::string& mc = b.maxClass;
        if (!b.unreplaced.empty()) return refuse(b, b.words.empty() ? mc : b.words[0], b.unreplaced);
        if (mc == "bpatcher") return bpatcher(b);
        if (mc == "comment" || mc == "live.comment") return single(b, "#X text " + at(b.x, b.y) + " " + pdEscape(b.text));
        if (mc == "panel" || mc == "fpic" || mc == "live.line") {
            if (anyConnection(b)) refuse(b, mc, "a decoration with connections");
            return;
        }
        if (mc == "message") return message(b);
        if (mc == "flonum" || mc == "number") return numberBox(b, mc == "number");
        if (mc == "inlet" || mc == "outlet") {
            const bool signal = mc == "inlet" ? (!b.signalOutlet.empty() && b.signalOutlet[0]) : fedSignal(b, 0);
            return obj(b, mc + (signal ? "~" : ""));
        }
        if (mc == "live.dial" || mc == "live.numbox" || mc == "live.slider" || mc == "live.menu" ||
            mc == "live.tab")
            return liveControl(b);
        if (mc == "live.text") {
            // Toggle mode is a two-state parameter; button mode (mode 0) is a
            // bang with no value, not translated yet.
            if (b.raw.value("mode", 1) == 0) return refuse(b, mc, "button mode is not translated yet");
            return liveControl(b, true);
        }
        if (mc != "newobj") return refuse(b, mc, "no translation for this kind of box yet");
        if (b.words.empty()) return refuse(b, "newobj", "an empty object box");
        if (b.text.find('"') != std::string::npos) return refuse(b, b.words[0], "quoted symbols are not translated yet");

        const std::string& cls = b.words[0];
        const std::vector<std::string> args(b.words.begin() + 1, b.words.end());
        const auto allNumbers = std::all_of(args.begin(), args.end(), isNumber);

        if (cls == "p" || cls == "patcher") return subpatcher(b);
        if (cls == "*~" || cls == "+~" || cls == "-~" || cls == "/~") {
            if (args.size() > 1 || !allNumbers) return refuse(b, cls, "expects one number at most");
            // A signal into the right inlet needs Pd's signal form, which has
            // no argument; otherwise the argument form, whose right inlet
            // takes floats as Max's does.
            if (fedSignal(b, 1)) return obj(b, cls);
            // With no argument Max's [/~] divides by 1 (its reference); the others start at 0.
            return obj(b, cls + " " + (args.empty() ? std::string(cls == "/~" ? "1" : "0") : args[0]));
        }
        if (cls == "cycle~" || cls == "phasor~" || cls == "sig~") {
            if (args.size() > 1 || !allNumbers) return refuse(b, cls, "only a numeric frequency is translated");
            if (cls == "cycle~" && connected(b, false, 1)) return cyclePhase(b, args);
            if (cls == "phasor~" && fedSignal(b, 1))
                return refuse(b, cls, "a signal into its right inlet resets the phase in Max (hard sync); Pd's is a float inlet");
            const std::string text = (cls == "cycle~" ? std::string("osc~") : cls) + (args.empty() ? "" : " " + args[0]);
            if (cls == "sig~" || !controlInto(b, 0)) return obj(b, text);
            // "reset" (cycle~: to a phase 0 to 1; phasor~: to 0) goes to the
            // phase inlet, which in Pd's [osc~] and [phasor~] sets the phase.
            Fragment f;
            f.records = {"#X obj " + at(b.x, b.y) + " " + text, "#X obj " + at(b.x + 60, b.y - 60) + " route reset",
                         "#X obj " + at(b.x + 60, b.y - 40) + " route bang", "#X msg " + at(b.x + 60, b.y - 20) + " 0"};
            f.internal = {{1, 0, 2, 0}, {2, 0, 3, 0}, {3, 0, 0, 1}, {2, 1, 0, 1}, {1, 1, 0, 0}};
            f.inlets = {{0, 0}, {0, 1}};
            f.controlInlets[0] = {1, 0};
            f.outlets = {{0, 0}};
            frags_[b.id] = std::move(f);
            return;
        }
        if (cls == "plugin~" || cls == "plugout~") {
            if (depth_ > 0) return refuse(b, cls, "audio ports belong in the device's own patcher");
            // Max keeps channel arguments only "as a courtesy to Max 4 users":
            // they do not route (its reference). Left is 1, right is 2.
            if (!(args.empty() || (args.size() == 2 && args[0] == "1" && args[1] == "2")))
                return refuse(b, cls, "Max ignores its channel arguments (kept for Max 4 plug-ins); how many ports it then has is not read");
            return obj(b, std::string(cls == "plugin~" ? "adc~" : "dac~") + " 1 2");
        }
        if (cls == "loadbang") return loadBang(b);
        if (cls == "live.thisdevice") {
            // Its left outlet also bangs in Max when a preset is loaded or the
            // device saved; ADI has neither event for a device. Its bang
            // comes in Pd's loadbang order, not once the device is complete.
            if (connected(b, true, 1) || connected(b, true, 2))
                return refuse(b, cls, "only its load bang (left outlet) is translated");
            return loadBang(b);
        }
        if (cls == "metro") {
            if (args.size() != 1 || !isNumber(args[0])) return refuse(b, cls, "only a time in ms is translated");
            return obj(b, "metro " + args[0]);
        }
        // With no arguments Max's [t] has two int outlets (Pd's, two bangs).
        if (cls == "t" || cls == "trigger") return trigger(b, args.empty() ? std::vector<std::string>{"i", "i"} : args);
        if (cls == "line~") return line(b, args);
        if (cls == "prepend" || cls == "append") {
            if (args.empty()) return refuse(b, cls, "nothing to add");
            std::string what;
            for (const auto& a : args) what += " " + a;
            // Max outputs a message whose selector is the first word; Pd's
            // [list prepend] gives a list, which [list trim] turns into that
            // message. "set ..." replaces what is added, without output; a
            // bang is the word "bang" (both as Max's reference says).
            Fragment f;
            f.records = {"#X obj " + at(b.x, b.y - 20) + " route set bang", "#X obj " + at(b.x, b.y) + " list " + cls + what,
                         "#X obj " + at(b.x, b.y + 20) + " list trim", "#X msg " + at(b.x + 80, b.y - 10) + " list bang"};
            f.internal = {{0, 0, 1, 1}, {0, 1, 3, 0}, {3, 0, 1, 0}, {0, 2, 1, 0}, {1, 0, 2, 0}};
            f.inlets = {{0, 0}};
            f.outlets = {{2, 0}};
            frags_[b.id] = std::move(f);
            return;
        }
        if (cls == "f" || cls == "float" || cls == "i" || cls == "int") {
            if (args.size() > 1 || !allNumbers) return refuse(b, cls, "one number at most");
            // "set n" stores n without output, as in Max: into the cold inlet.
            Fragment f;
            f.records = {"#X obj " + at(b.x, b.y - 20) + " route set",
                         "#X obj " + at(b.x, b.y) + " " + cls.substr(0, 1) + (args.empty() ? std::string() : " " + args[0])};
            f.internal = {{0, 0, 1, 1}, {0, 1, 1, 0}};
            f.inlets = {{0, 0}, {1, 1}};
            f.outlets = {{1, 0}};
            frags_[b.id] = std::move(f);
            return;
        }
        if (cls == "delay" || cls == "del") {
            if (args.size() > 1 || !allNumbers) return refuse(b, cls, "only a time in ms is translated");
            return obj(b, "delay" + (args.empty() ? std::string() : " " + args[0]));
        }
        if (cls == "abs~") {
            if (!args.empty()) return refuse(b, cls, "no arguments");
            return obj(b, "abs~");
        }
        if (cls == "!-") return reverseSubtract(b, args);
        if (cls == "gate") return gate(b, args);
        if (cls == "selector~") return selector(b, args);
        if (cls == "slide~") return slide(b, args);
        if (cls == "biquad~") return biquad(b, args);
        if (cls == "pack" || cls == "unpack") return packing(b, cls, args);
        static const std::set<std::string> twoInlet{"+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "min", "max"};
        if (twoInlet.count(cls) != 0 || cls == "abs") {
            if (args.size() > 1 || !allNumbers) return refuse(b, cls, "expects one number at most");
            // Max's [%] works on ints whatever it is given (its reference: a
            // float is "converted to int"), so a float argument does not make
            // it the float form as it does the other operators.
            const bool isFloat = !args.empty() && isFloatLiteral(args[0]) && cls != "%";
            if (isFloat) return obj(b, b.text);
            // Max's integer form: inputs truncated; '/' truncates its result too.
            std::string pdText = cls + (args.empty() || cls == "abs" ? "" : " " + args[0]);
            if (cls == "/" && (args.empty() || std::atoi(args[0].c_str()) == 0)) pdText = "/ 1";   // Max: "/ 0" divides by 1
            return integer(b, pdText, cls == "abs" ? 1 : 2, cls == "/", cls == "/");
        }
        if (cls == "s" || cls == "send" || cls == "r" || cls == "receive") {
            if (args.size() != 1 || isNumber(args[0])) return refuse(b, cls, "one name");
            std::string name = args[0];
            if (name.rfind("---", 0) == 0) name = "\\$0-" + name.substr(3);
            return obj(b, std::string(cls[0] == 's' ? "s " : "r ") + name);
        }
        if (cls == "print") return obj(b, b.text);
        refuse(b, cls, "no translation for this object yet");
    }

    /// Max's [line~] takes a list of (target, ms) pairs and plays them one
    /// after another; a lone number goes there in the time last given to the
    /// right inlet, which then reverts to 0 ("if no time has been specified
    /// since the last target value, the time is considered to be 0", Max's
    /// reference). Pd's [vline~] takes ONE segment and reads a third number as
    /// a delay, so a Max list is walked pair by pair here, each pair sent as
    /// "target ms delay" with the delay accumulated -- the first with delay 0,
    /// which (in vline~) cancels whatever was still scheduled, as a new list
    /// does in Max. The right outlet bangs "when line~ has finished generating
    /// all of its ramps": a [delay] of the list's whole time, rescheduled by
    /// every new input, so a sequence cut short never bangs. A jump (no time at
    /// all) does not bang either; Max's reference is silent on both, and a jump
    /// that retriggered itself would hold Pd's scheduler at one instant (the
    /// devices log's reading 14 asks Max). An argument is the initial value.
    void line(const Box& b, const std::vector<std::string>& args) {
        if (args.size() > 1 || (args.size() == 1 && !isNumber(args[0])))
            return refuse(b, "line~", "one initial value at most");
        std::string name = prefix_ + b.id;
        for (char& c : name)
            if (c == ' ' || c == '/') c = '_';
        const std::string acc = "v \\$0-mxline-" + name;
        const char* body[] = {
            "#X obj 10 10 inlet",                       // 0
            "#X obj 10 40 t b a b",                     // 1  input: acc := 0, walk, time := 0
            "#X msg 80 70 0",                           // 2
            "#X obj 80 100 ",                           // 3  acc := 0 (v)
            "#X obj 10 130 t a b",                      // 4  loop entry: clear the rest
            "#X obj 160 250 list",                      // 5  the rest, stored
            "#X obj 10 160 route bang",                 // 6  an empty rest ends the walk
            "#X obj 10 190 list split 2",               // 7
            "#X obj 120 220 t f b",                     // 8  a lone number: in the stored time
            "#X obj 10 250 t a a",                      // 9  per pair: send, then advance
            "#X obj 120 280 unpack f f",                // 10
            "#X obj 120 340 pack f f f",                // 11 target ms delay
            "#X obj 120 310 t f b",                     // 12
            "#X obj 200 310 ",                          // 13 acc (read)
            "#X obj 120 370 vline~",                    // 14
            "#X obj 120 400 outlet~",                   // 15
            "#X obj 10 280 t b a",                      // 16
            "#X obj 10 310 unpack f f",                 // 17
            "#X obj 10 340 t b f",                      // 18
            "#X obj 10 400 +",                          // 19
            "#X obj 10 370 ",                           // 20 acc (read)
            "#X obj 10 430 ",                           // 21 acc := acc + ms
            "#X obj 300 10 inlet",                      // 22 the ramp time
            "#X obj 300 160 ",                          // 23 acc (read): the walk's whole time
            "#X obj 300 280 delay",                     // 24
            "#X obj 300 400 outlet",                    // 25 the end bang
            "#X obj 220 220 f",                         // 26 the stored time
            "#X obj 120 250 pack f f",                  // 27 target, stored time
            "#X msg 300 70 0",                          // 28 time := 0
            "#X obj 300 190 t f b",                     // 29
            "#X msg 380 220 stop",                      // 30
            "#X obj 300 250 moses 1e-06",               // 31 a jump does not bang
        };
        std::string canvas = "#N canvas 0 50 500 480 max-line~ 0;\n";
        for (std::size_t i = 0; i < std::size(body); ++i) {
            std::string r = body[i];
            if (i == 3 || i == 13 || i == 20 || i == 21 || i == 23) r += acc;
            canvas += r + ";\n";
        }
        std::vector<std::array<int, 4>> wires = {
            {0, 0, 1, 0},   {1, 2, 2, 0},   {2, 0, 3, 0},   {1, 1, 4, 0},   {1, 0, 28, 0},  {28, 0, 26, 1},
            {4, 1, 5, 1},   {4, 0, 6, 0},   {6, 0, 23, 0},  {23, 0, 29, 0}, {29, 1, 30, 0}, {30, 0, 24, 0},
            {29, 0, 31, 0}, {31, 1, 24, 0}, {24, 0, 25, 0}, {6, 1, 7, 0},   {7, 1, 5, 1},   {7, 2, 8, 0},
            {8, 1, 26, 0},  {26, 0, 27, 1}, {8, 0, 27, 0},  {27, 0, 9, 0},  {7, 0, 9, 0},   {9, 1, 10, 0},
            {10, 1, 11, 1}, {10, 0, 12, 0}, {12, 1, 13, 0}, {13, 0, 11, 2}, {12, 0, 11, 0}, {11, 0, 14, 0},
            {14, 0, 15, 0}, {9, 0, 16, 0},  {16, 1, 17, 0}, {16, 0, 5, 0},  {5, 0, 4, 0},   {17, 1, 18, 0},
            {18, 1, 19, 1}, {18, 0, 20, 0}, {20, 0, 19, 0}, {19, 0, 21, 0}, {22, 0, 26, 1},
        };
        if (!args.empty()) {   // the initial value, before anything the parent's loadbang sends
            canvas += "#X obj 400 10 loadbang;\n#X msg 400 40 " + args[0] + ";\n";
            const int load = static_cast<int>(std::size(body));
            wires.push_back({load, 0, load + 1, 0});
            wires.push_back({load + 1, 0, 14, 0});
        }
        for (const auto& w : wires)
            canvas += "#X connect " + std::to_string(w[0]) + " " + std::to_string(w[1]) + " " +
                      std::to_string(w[2]) + " " + std::to_string(w[3]) + ";\n";
        single(b, canvas + "#X restore " + at(b.x, b.y) + " pd max-line~");
    }

    /// Boxes in a row, each feeding the next: inlets on the first, outlet 0
    /// on the last.
    /// Max's [cycle~] takes a phase OFFSET in its right inlet (0 to 1, a float
    /// or a signal), added to its running phase; Pd's [osc~] RESETS its phase
    /// there. So a [cycle~] with that inlet in use is [phasor~] -> [+~] ->
    /// [cos~]: cos(2 pi (phase + offset)), as Max computes it.
    void cyclePhase(const Box& b, const std::vector<std::string>& args) {
        Fragment f;
        f.records = {"#X obj " + at(b.x, b.y) + " phasor~" + (args.empty() ? std::string() : " " + args[0]),
                     "#X obj " + at(b.x, b.y + 20) + (fedSignal(b, 1) ? " +~" : " +~ 0"),
                     "#X obj " + at(b.x, b.y + 40) + " cos~", "#X obj " + at(b.x + 60, b.y - 60) + " route reset",
                     "#X obj " + at(b.x + 60, b.y - 40) + " route bang", "#X msg " + at(b.x + 60, b.y - 20) + " 0"};
        // "reset" sets the running phase (the offset still adds to it).
        f.internal = {{0, 0, 1, 0}, {1, 0, 2, 0}, {3, 0, 4, 0}, {4, 0, 5, 0}, {5, 0, 0, 1}, {4, 1, 0, 1}, {3, 1, 0, 0}};
        f.inlets = {{0, 0}, {1, 1}};
        f.controlInlets[0] = {3, 0};
        f.outlets = {{2, 0}};
        frags_[b.id] = std::move(f);
    }

    /// Whether a control (not a signal) source feeds this inlet.
    bool controlInto(const Box& b, int inlet) const {
        return std::any_of(lines_.begin(), lines_.end(), [&](const Line& l) { return l.dst == b.id && l.inlet == inlet && !isSignalSource(l); });
    }

    /// Whether a message box whose first word is `word` feeds this inlet.
    bool messageInto(const Box& b, int inlet, const std::string& word) const {
        return std::any_of(lines_.begin(), lines_.end(), [&](const Line& l) {
            if (l.dst != b.id || l.inlet != inlet) return false;
            const Box& s = boxes_[index_.at(l.src)];
            const auto w = words(s.text);
            return s.maxClass == "message" && !w.empty() && w[0] == word;
        });
    }

    void chain(const Box& b, const std::vector<std::string>& texts) {
        Fragment f;
        for (std::size_t k = 0; k < texts.size(); ++k) {
            f.records.push_back("#X obj " + at(b.x, b.y + 20.0 * static_cast<double>(k)) + " " + texts[k]);
            if (k > 0) f.internal.push_back({static_cast<int>(k) - 1, 0, static_cast<int>(k), 0});
        }
        f.outlets = {{static_cast<int>(texts.size()) - 1, 0}};
        frags_[b.id] = std::move(f);
    }

    /// Max [!- n]: n minus the input, int unless n is written as a float;
    /// the right inlet sets n. Pd: (-x) + n, with [i] first for ints.
    void reverseSubtract(const Box& b, const std::vector<std::string>& args) {
        if (args.size() > 1 || (args.size() == 1 && !isNumber(args[0]))) return refuse(b, "!-", "one number at most");
        const bool isFloat = !args.empty() && isFloatLiteral(args[0]);
        const std::string n = args.empty() ? "0" : args[0];
        Fragment f;
        f.records.push_back("#X obj " + at(b.x, b.y - 20) + (isFloat ? " f" : " i"));   // a list never reaches [* -1]
        const int neg = static_cast<int>(f.records.size());
        f.records.push_back("#X obj " + at(b.x, b.y) + " * -1");
        const int add = neg + 1;
        f.records.push_back("#X obj " + at(b.x, b.y + 20) + " + " + n);
        f.internal.push_back({0, 0, neg, 0});
        if (!isFloat) {
            f.records.push_back("#X obj " + at(b.x + 40, b.y - 20) + " i");
            f.internal.push_back({add + 1, 0, add, 1});
            f.inlets = {{0, 0}, {add + 1, 0}};
        } else {
            f.inlets = {{0, 0}, {add, 1}};
        }
        f.internal.push_back({neg, 0, add, 0});
        f.outlets = {{add, 0}};
        frags_[b.id] = std::move(f);
    }

    /// Max [gate n init]: the left inlet picks an outlet (0 closes it; above n
    /// the rightmost, below 0 the leftmost, as its reference says), the right
    /// inlet's message goes out there, and a bang in the left inlet reports
    /// the open outlet out of the left one. Pd: the clamped choice is the
    /// prefix a [list prepend] puts on the message, [route 1 .. n] sends it
    /// on, and an [f] holds it for the report.
    void gate(const Box& b, const std::vector<std::string>& args) {
        if (args.size() > 2 || !std::all_of(args.begin(), args.end(), isIntLiteral))
            return refuse(b, "gate", "outlet count and initial choice, as ints");
        const int n = args.empty() ? 1 : std::atoi(args[0].c_str());
        if (n < 1 || n > 64) return refuse(b, "gate", "between 1 and 64 outlets");
        if (messageInto(b, 0, "next")) return refuse(b, "gate", "its next message (the next outlet) is not translated yet");
        const int given = args.size() > 1 ? std::atoi(args[1].c_str()) : 0;
        const std::string init = std::to_string(given < 0 ? 1 : std::min(given, n));
        std::string routes = "route";
        for (int k = 1; k <= n; ++k) routes += " " + std::to_string(k);
        Fragment f;
        f.records = {"#X obj " + at(b.x, b.y - 80) + " route bang",           // 0
                     "#X obj " + at(b.x + 40, b.y - 60) + " i",               // 1
                     "#X obj " + at(b.x + 40, b.y - 40) + " moses 0",         // 2
                     "#X msg " + at(b.x + 40, b.y - 20) + " 1",               // 3
                     "#X obj " + at(b.x + 90, b.y - 20) + " min " + std::to_string(n),   // 4
                     "#X obj " + at(b.x + 40, b.y) + " t f f",                // 5
                     "#X obj " + at(b.x, b.y + 20) + " list prepend " + init, // 6
                     "#X obj " + at(b.x, b.y + 40) + " " + routes,            // 7
                     "#X obj " + at(b.x + 140, b.y) + " f " + init,           // 8 the report
                     "#X obj " + at(b.x, b.y + 60) + " t a"};                 // 9 the left outlet
        f.internal = {{0, 0, 8, 0}, {0, 1, 1, 0}, {1, 0, 2, 0}, {2, 0, 3, 0}, {2, 1, 4, 0}, {3, 0, 5, 0},
                      {4, 0, 5, 0}, {5, 1, 8, 1}, {5, 0, 6, 1}, {6, 0, 7, 0}, {7, 0, 9, 0}, {8, 0, 9, 0}};
        f.inlets = {{0, 0}, {6, 0}};
        f.outlets.emplace_back(9, 0);
        for (int k = 1; k < n; ++k) f.outlets.emplace_back(7, k);
        frags_[b.id] = std::move(f);
    }

    /// Max's [loadbang] (and live.thisdevice) also bangs when banged.
    void loadBang(const Box& b) {
        if (!connected(b, false, 0)) return obj(b, "loadbang");
        // Only a bang (or "loadbang") fires it again; live.thisdevice's other
        // messages (setwidth, getstate) do not touch its left outlet.
        Fragment f;
        f.records = {"#X obj " + at(b.x, b.y) + " loadbang", "#X obj " + at(b.x, b.y + 20) + " t b",
                     "#X obj " + at(b.x + 60, b.y - 20) + " route bang loadbang"};
        f.internal = {{0, 0, 1, 0}, {2, 0, 1, 0}, {2, 1, 1, 0}};
        f.inlets = {{2, 0}};
        f.outlets = {{1, 0}};
        frags_[b.id] = std::move(f);
    }

    /// A Max message box. Its "---" names are the device's own, as in [s]
    /// and [r]; a quoted run is one symbol in Max, which a Pd message cannot
    /// hold; its right inlet sets its contents without output (Pd's "set").
    /// Pd does not expand $0 in a message's ";" receiver, so a message that
    /// sends to a device-local name has each ";" part as its own message into
    /// an [s $0-name], all fired in Max's order (the outlet's part first)
    /// from one [t a ...]; a "---" name anywhere else in it is refused.
    void message(const Box& b) {
        if (b.text.find('"') != std::string::npos) return refuse(b, "message", "quoted symbols are not translated yet");
        std::vector<std::string> parts{""};
        for (const char c : b.text) {
            if (c == ';') parts.emplace_back();
            else parts.back() += c;
        }
        bool local = false;
        for (std::size_t k = 1; k < parts.size(); ++k) {
            const auto w = words(parts[k]);
            local = local || (!w.empty() && w[0].rfind("---", 0) == 0);
        }
        for (std::size_t k = 0; k < parts.size(); ++k) {
            auto w = words(parts[k]);
            for (std::size_t j = k == 0 ? 0 : 1; j < w.size(); ++j)
                if (w[j].find("---") != std::string::npos)
                    return refuse(b, "message", "a device-local (---) name inside a message is not translated");
        }
        const bool dollars = b.text.find('$') != std::string::npos;
        if (!local) {
            const std::string record = "#X msg " + at(b.x, b.y) + " " + pdEscape(b.text);
            if (!connected(b, false, 1) && !dollars) return single(b, record);
            Fragment f;
            f.records = {record};
            f.inlets = {{0, 0}};
            f.outlets = {{0, 0}};
            if (connected(b, false, 1)) {
                // A bang sets the contents to "bang" (Max's reference), not to nothing.
                f.records.push_back("#X obj " + at(b.x + 60, b.y - 40) + " list prepend set");
                f.records.push_back("#X obj " + at(b.x + 60, b.y - 20) + " list trim");
                f.records.push_back("#X obj " + at(b.x + 60, b.y - 60) + " route bang");
                f.records.push_back("#X msg " + at(b.x + 140, b.y - 40) + " set bang");
                f.internal = {{1, 0, 2, 0}, {2, 0, 0, 0}, {3, 0, 4, 0}, {4, 0, 0, 0}, {3, 1, 1, 0}};
                f.inlets.emplace_back(3, 0);
            }
            if (dollars) keepArguments(b, f, 0);
            frags_[b.id] = std::move(f);
            return;
        }
        if (connected(b, false, 1)) return refuse(b, "message", "setting a message that sends to a device-local name is not translated");
        Fragment f;
        const int n = static_cast<int>(parts.size());
        std::string t = "t";
        for (int k = 0; k < n; ++k) t += " a";
        f.records.push_back("#X obj " + at(b.x, b.y - 30) + " " + t);
        for (int k = 0; k < n; ++k) {
            const auto w = words(parts[static_cast<std::size_t>(k)]);
            std::string body;
            for (std::size_t j = k == 0 ? 0 : 1; j < w.size(); ++j) body += (body.empty() ? "" : " ") + w[j];
            const int msg = static_cast<int>(f.records.size());
            f.records.push_back("#X msg " + at(b.x + 80.0 * k, b.y) + (body.empty() ? "" : " " + pdEscape(body)));
            f.internal.push_back({0, n - 1 - k, msg, 0});   // [t] fires right to left: the first part first
            if (k > 0 && !w.empty()) {
                const std::string dest = w[0].rfind("---", 0) == 0 ? "\\$0-" + w[0].substr(3) : w[0];
                f.records.push_back("#X obj " + at(b.x + 80.0 * k, b.y + 20) + " s " + dest);
                f.internal.push_back({msg, 0, msg + 1, 0});
            }
        }
        f.inlets = {{0, 0}};
        f.outlets = {{1, 0}};
        if (dollars) keepArguments(b, f, 0);
        frags_[b.id] = std::move(f);
    }

    /// A Max message box keeps what its $ arguments last took: a bang sends
    /// them again. A Pd message box keeps nothing. So a number or list on
    /// its way in is also stored in a [list], which a bang sends instead.
    void keepArguments(const Box& b, Fragment& f, int target) {
        // Everything but a bang or a "set" is made a list ([list] keeps a
        // message's first word as its first element, where Pd's message box
        // would drop it), goes on, and is stored: the box sends the same now
        // and when banged later, a symbol too. "set ..." goes to the box as
        // "set ...": it changes the box's text without output, as Max's does.
        const int route = static_cast<int>(f.records.size());
        f.records.push_back("#X obj " + at(b.x - 60, b.y - 80) + " route bang set");
        f.records.push_back("#X obj " + at(b.x - 60, b.y - 40) + " list");
        f.records.push_back("#X obj " + at(b.x, b.y - 60) + " t a a");
        f.records.push_back("#X obj " + at(b.x, b.y - 70) + " list");
        f.records.push_back("#X obj " + at(b.x + 60, b.y - 70) + " list prepend set");
        f.records.push_back("#X obj " + at(b.x + 60, b.y - 50) + " list trim");
        const int store = route + 1, both = route + 2, asList = route + 3, again = route + 4;
        f.internal.push_back({route, 0, store, 0});
        f.internal.push_back({store, 0, target, 0});
        f.internal.push_back({route, 2, asList, 0});
        f.internal.push_back({asList, 0, both, 0});
        f.internal.push_back({both, 1, store, 1});
        f.internal.push_back({both, 0, target, 0});
        f.internal.push_back({route, 1, again, 0});
        f.internal.push_back({again, 0, again + 1, 0});
        f.internal.push_back({again + 1, 0, target, 0});
        f.inlets[0] = {route, 0};
    }

    /// Max's number box (an int unless its format is 6, Decimal
    /// Floating-Point) and flonum, limited to their minimum and maximum
    /// (what they output, as their reference says).
    void numberBox(const Box& b, bool intBox) {
        const auto attr = [&](const char* key) -> std::optional<double> {
            if (b.raw.contains(key) && b.raw[key].is_number()) return b.raw[key].get<double>();
            return std::nullopt;
        };
        const int format = static_cast<int>(attr("format").value_or(0));
        if (intBox && (format == 7 || format == 8)) return refuse(b, b.maxClass, "the List and Text formats are not translated");
        const bool truncate = intBox && format != 6;
        const auto lo = attr("minimum"), hi = attr("maximum");
        Fragment f;
        if (truncate) f.records.push_back("#X obj " + at(b.x, b.y - 40) + " i");
        if (lo || hi)
            f.records.push_back("#X obj " + at(b.x, b.y - 20) + " clip " + number(lo.value_or(-1e37)) + " " + number(hi.value_or(1e37)));
        const int atom = static_cast<int>(f.records.size());
        f.records.push_back("#X floatatom " + at(b.x, b.y) + " 5 0 0 0 - - - 0");
        for (int k = 0; k < atom; ++k) f.internal.push_back({k, 0, k + 1, 0});
        // "set n" shows n without output, as in Max (the atom's own "set"),
        // truncated and limited as an output would be; a bang sends what it
        // shows.
        const int route = static_cast<int>(f.records.size());
        f.records.push_back("#X obj " + at(b.x, b.y - 60) + " route set bang");
        int last = route;
        if (truncate) {
            f.records.push_back("#X obj " + at(b.x + 60, b.y - 50) + " i");
            f.internal.push_back({last, 0, last + 1, 0});
            last = static_cast<int>(f.records.size()) - 1;
        }
        if (lo || hi) {
            f.records.push_back("#X obj " + at(b.x + 60, b.y - 40) + " clip " + number(lo.value_or(-1e37)) + " " + number(hi.value_or(1e37)));
            f.internal.push_back({last, 0, static_cast<int>(f.records.size()) - 1, 0});
            last = static_cast<int>(f.records.size()) - 1;
        }
        f.records.push_back("#X msg " + at(b.x + 60, b.y - 30) + " set \\$1");
        const int setter = static_cast<int>(f.records.size()) - 1;
        f.internal.push_back({last, 0, setter, 0});
        f.internal.push_back({setter, 0, atom, 0});
        f.internal.push_back({route, 1, atom, 0});
        f.internal.push_back({route, 2, 0, 0});
        f.inlets = {{route, 0}};
        f.outlets = {{atom, 0}};
        frags_[b.id] = std::move(f);
    }

    /// Max [selector~ n init]: the left inlet picks one of n signal inputs
    /// (0 is silence), switched at once. Pd: each input times (choice == k),
    /// summed into one [*~ 1].
    void selector(const Box& b, const std::vector<std::string>& args) {
        if (args.size() > 2 || !std::all_of(args.begin(), args.end(), isIntLiteral))
            return refuse(b, "selector~", "input count and initial choice, as ints");
        if (fedSignal(b, 0)) return refuse(b, "selector~", "a signal choosing the input is not translated yet");
        const int n = args.empty() ? 1 : std::atoi(args[0].c_str());
        if (n < 1 || n > 64) return refuse(b, "selector~", "between 1 and 64 inputs");
        // A choice above n is the rightmost input (its reference); below 0,
        // silence as 0 is (an assumption, read with reading 14).
        const int init = std::clamp(args.size() > 1 ? std::atoi(args[1].c_str()) : 0, 0, n);
        Fragment f;
        f.records.push_back("#X obj " + at(b.x, b.y - 60) + " i");   // 0: the choice
        f.inlets.emplace_back(0, 0);
        f.records.push_back("#X obj " + at(b.x, b.y - 40) + " clip 0 " + std::to_string(n));   // 1
        f.internal.push_back({0, 0, 1, 0});
        const int sum = 2;
        f.records.push_back("#X obj " + at(b.x, b.y + 40) + " *~ 1");
        for (int k = 1; k <= n; ++k) {
            const int eq = static_cast<int>(f.records.size());
            f.records.push_back("#X obj " + at(b.x + 50.0 * k, b.y - 20) + " == " + std::to_string(k));
            f.records.push_back("#X obj " + at(b.x + 50.0 * k, b.y) + " *~ " + (k == init ? "1" : "0"));
            f.internal.push_back({1, 0, eq, 0});
            f.internal.push_back({eq, 0, eq + 1, 1});
            f.internal.push_back({eq + 1, 0, sum, 0});
            f.inlets.emplace_back(eq + 1, 0);
        }
        f.outlets = {{sum, 0}};
        frags_[b.id] = std::move(f);
    }

    /// Max [slide~ up down]: y += (x - y) / (x > y ? up : down), each at
    /// least 1. Written as that recursion in [fexpr~]; the inlets set them.
    void slide(const Box& b, const std::vector<std::string>& args) {
        if (args.size() > 2 || !std::all_of(args.begin(), args.end(), isNumber))
            return refuse(b, "slide~", "up and down as numbers");
        if (fedSignal(b, 1) || fedSignal(b, 2)) return refuse(b, "slide~", "signal-rate up and down are not translated yet");
        if (messageInto(b, 0, "reset")) return refuse(b, "slide~", "its reset message (output to 0) is not translated yet");
        const std::string up = args.empty() ? "1" : args[0];
        const std::string down = args.size() > 1 ? args[1] : "1";
        Fragment f;
        f.records = {"#X obj " + at(b.x, b.y) +
                         " fexpr~ \\$y1 + (\\$x1 - \\$y1) / if(\\$x1 > \\$y1 \\, max(\\$f2 \\, 1) \\, max(\\$f3 \\, 1))",
                     "#X obj " + at(b.x + 120, b.y - 60) + " loadbang", "#X msg " + at(b.x + 120, b.y - 40) + " " + up,
                     "#X msg " + at(b.x + 170, b.y - 40) + " " + down};
        f.internal = {{1, 0, 2, 0}, {1, 0, 3, 0}, {2, 0, 0, 1}, {3, 0, 0, 2}};
        f.inlets = {{0, 0}, {0, 1}, {0, 2}};
        f.outlets = {{0, 0}};
        frags_[b.id] = std::move(f);
    }

    /// Max [biquad~] takes a0 a1 a2 b1 b2 for y = a0 x + a1 x1 + a2 x2 - b1 y1 -
    /// b2 y2; Pd's takes fb1 fb2 ff1 ff2 ff3 with the feedback ADDED. So a
    /// coefficient list is reordered and its b's negated on the way in.
    /// Max's biquad~ smooths its coefficient changes unless its `smooth`
    /// attribute is 0 (on by default; its reference does not say how). Pd's
    /// biquad~ takes a new list at once. So coefficients that change at run
    /// time translate only with smoothing off; coefficients given as
    /// arguments are the filter from the start.
    void biquad(const Box& b, const std::vector<std::string>& typed) {
        bool smooth = true;
        if (b.raw.contains("saved_object_attributes") && b.raw["saved_object_attributes"].is_object() &&
            b.raw["saved_object_attributes"].contains("smooth") && b.raw["saved_object_attributes"]["smooth"].is_number())
            smooth = b.raw["saved_object_attributes"]["smooth"].get<double>() != 0;
        std::vector<std::string> args;
        for (std::size_t k = 0; k < typed.size(); ++k) {
            if (typed[k] == "@smooth" && k + 1 < typed.size() && isNumber(typed[k + 1])) {
                smooth = std::strtod(typed[k + 1].c_str(), nullptr) != 0;
                ++k;
                continue;
            }
            args.push_back(typed[k]);
        }
        if (!(args.empty() || args.size() == 5) || !std::all_of(args.begin(), args.end(), isNumber))
            return refuse(b, "biquad~", "five coefficients or none");
        for (int k = 1; k <= 5; ++k)
            if (connected(b, false, k)) return refuse(b, "biquad~", "signal-rate coefficients are not translated yet");
        const bool changes = std::any_of(lines_.begin(), lines_.end(), [&](const Line& l) {
            return l.dst == b.id && l.inlet == 0 && !isSignalSource(l);
        });
        if (smooth && changes)
            return refuse(b, "biquad~",
                          "Max smooths coefficient changes (its smooth attribute, on by default) in a way not yet "
                          "read; with @smooth 0 it translates");
        if (!args.empty()) {
            // Pd's biquad~ silences coefficients whose poles lie outside the
            // unit circle; Max's runs away with them. Refused, not silenced.
            // Pd's own test (d_filter.c, sigbiquad_list), condition for
            // condition, on the numbers Pd will read (the text written below),
            // in Pd's t_float: run as a 32-bit and as a 64-bit float, the two
            // builds ADI makes. Unstable in either (on the unit circle the two
            // can disagree) is refused rather than guessed.
            const auto stableAs = [](auto fb1, auto fb2) {
                using T = decltype(fb1);
                const T disc = fb1 * fb1 + 4 * fb2;
                if (disc < 0) return fb2 >= T(-1);
                return fb1 <= T(2) && fb1 >= T(-2) && T(1) - fb1 - fb2 >= 0 && T(1) + fb1 - fb2 >= 0;
            };
            const std::string t1 = number(-std::strtod(args[3].c_str(), nullptr)), t2 = number(-std::strtod(args[4].c_str(), nullptr));
            const bool in64 = stableAs(std::strtod(t1.c_str(), nullptr), std::strtod(t2.c_str(), nullptr));
            const bool in32 = stableAs(std::strtof(t1.c_str(), nullptr), std::strtof(t2.c_str(), nullptr));
            if (!in64 && !in32) return refuse(b, "biquad~", "unstable coefficients: Pd's biquad~ silences them, Max's runs away");
            if (!in64 || !in32)
                return refuse(b, "biquad~", "a pole on the unit circle: Pd's 32- and 64-bit stability tests disagree on it");
        }
        const auto neg = [](const std::string& v) { return number(-std::strtod(v.c_str(), nullptr)); };
        const std::string init = args.empty() ? "" : " " + neg(args[3]) + " " + neg(args[4]) + " " + args[0] + " " +
                                                         args[1] + " " + args[2];
        Fragment f;
        f.records = {"#X obj " + at(b.x, b.y) + " biquad~" + init,
                     "#X obj " + at(b.x + 80, b.y - 40) +
                         " expr -\\$f4 \\; -\\$f5 \\; \\$f1 \\; \\$f2 \\; \\$f3",
                     "#X obj " + at(b.x + 80, b.y - 20) + " pack f f f f f"};
        // [expr] spreads a list over its inlets and fires right to left, so
        // the pack's hot inlet is filled last.
        // "clear" zeroes the filter's history, as Max's does (Pd's "set 0 0").
        f.records.push_back("#X obj " + at(b.x + 80, b.y - 60) + " route clear");
        f.records.push_back("#X msg " + at(b.x + 160, b.y - 40) + " set 0 0");
        f.internal = {{1, 0, 2, 0}, {1, 1, 2, 1}, {1, 2, 2, 2}, {1, 3, 2, 3}, {1, 4, 2, 4}, {2, 0, 0, 0},
                      {3, 0, 4, 0}, {4, 0, 0, 0}, {3, 1, 1, 0}};
        f.inlets = {{0, 0}};
        f.controlInlets[0] = {3, 0};   // a coefficient list goes through the reorder
        f.outlets = {{0, 0}};
        frags_[b.id] = std::move(f);
    }

    void trigger(const Box& b, const std::vector<std::string>& args) {
        Fragment f;
        std::string t = "t";
        std::vector<int> ints;
        for (std::size_t k = 0; k < args.size(); ++k) {
            const std::string& a = args[k];
            if (a == "b" || a == "f" || a == "l" || a == "s") t += " " + a;
            else if (a == "i") { t += " f"; ints.push_back(static_cast<int>(k)); }
            else return refuse(b, b.words[0], "only b, i, f, l and s outlets are translated ('" + a + "')");
        }
        f.records.push_back("#X obj " + at(b.x, b.y) + " " + t);
        for (std::size_t k = 0; k < args.size(); ++k) f.outlets.emplace_back(0, static_cast<int>(k));
        for (const int k : ints) {
            const int i = static_cast<int>(f.records.size());
            f.records.push_back("#X obj " + at(b.x + 30.0 * k, b.y + 20) + " i");
            f.internal.push_back({0, k, i, 0});
            f.outlets[static_cast<std::size_t>(k)] = {i, 0};
        }
        frags_[b.id] = std::move(f);
    }

    /// Max's [pack]/[unpack] are int by default ("0 0"); a slot is float when
    /// its argument is written as one ("0.") or is "f", a symbol for "s".
    void packing(const Box& b, const std::string& cls, std::vector<std::string> args) {
        if (args.empty()) args = {"0", "0"};
        Fragment f;
        std::string text = cls;
        std::vector<int> ints;
        for (std::size_t k = 0; k < args.size(); ++k) {
            const std::string& a = args[k];
            const bool pack = cls == "pack";
            if (isIntLiteral(a) || a == "i") { text += pack ? " " + (a == "i" ? std::string("0") : a) : " f"; ints.push_back(static_cast<int>(k)); }
            else if (isFloatLiteral(a)) text += pack ? " " + a : " f";
            else if (a == "f" || a == "s") text += " " + a;
            else return refuse(b, cls, "only int, float and symbol slots are translated ('" + a + "')");
        }
        if (cls == "pack" && std::none_of(args.begin(), args.end(), [](const std::string& a) { return a == "s"; })) {
            // Max's [pack] distributes a list into its left inlet and sends a
            // bang's stored list, as Pd's does; it holds ints in its int
            // slots. So the values go into Pd's [pack] as they come, and the
            // int slots are truncated on the way out.
            std::string raw = "pack", spread = "unpack", repack = "pack";
            for (const auto& a : args) {
                raw += " " + (a == "i" || a == "f" ? std::string("0") : a);
                spread += " f";
                repack += " f";
            }
            f.records.push_back("#X obj " + at(b.x, b.y) + " " + raw);
            for (std::size_t k = 0; k < args.size(); ++k) f.inlets.emplace_back(0, static_cast<int>(k));
            f.outlets = {{0, 0}};
            if (!ints.empty()) {
                f.records.push_back("#X obj " + at(b.x, b.y + 20) + " " + spread);
                f.records.push_back("#X obj " + at(b.x, b.y + 60) + " " + repack);
                f.internal.push_back({0, 0, 1, 0});
                for (std::size_t k = 0; k < args.size(); ++k) {
                    const auto slot = static_cast<int>(k);
                    if (std::find(ints.begin(), ints.end(), slot) == ints.end()) {
                        f.internal.push_back({1, slot, 2, slot});
                        continue;
                    }
                    const int i = static_cast<int>(f.records.size());
                    f.records.push_back("#X obj " + at(b.x + 30.0 * slot, b.y + 40) + " i");
                    f.internal.push_back({1, slot, i, 0});
                    f.internal.push_back({i, 0, 2, slot});
                }
                f.outlets = {{2, 0}};
            }
        } else if (cls == "pack") {
            for (const int k : ints) {
                f.records.push_back("#X obj " + at(b.x + 30.0 * k, b.y - 20) + " i");
            }
            const int op = static_cast<int>(f.records.size());
            f.records.push_back("#X obj " + at(b.x, b.y) + " " + text);
            for (std::size_t k = 0; k < args.size(); ++k) f.inlets.emplace_back(op, static_cast<int>(k));
            for (std::size_t j = 0; j < ints.size(); ++j) {
                f.internal.push_back({static_cast<int>(j), 0, op, ints[j]});
                f.inlets[static_cast<std::size_t>(ints[j])] = {static_cast<int>(j), 0};
            }
            f.outlets = {{op, 0}};
        } else {
            f.records.push_back("#X obj " + at(b.x, b.y) + " " + text);
            for (std::size_t k = 0; k < args.size(); ++k) f.outlets.emplace_back(0, static_cast<int>(k));
            for (const int k : ints) {
                const int i = static_cast<int>(f.records.size());
                f.records.push_back("#X obj " + at(b.x + 30.0 * k, b.y + 20) + " i");
                f.internal.push_back({0, k, i, 0});
                f.outlets[static_cast<std::size_t>(k)] = {i, 0};
            }
        }
        frags_[b.id] = std::move(f);
    }

    void subpatcher(const Box& b) {
        if (!b.raw.contains("patcher") || !b.raw["patcher"].is_object())
            return refuse(b, b.words[0], "a subpatcher with no patcher in it");
        std::string name;
        for (std::size_t k = 1; k < b.words.size(); ++k) name += (k > 1 ? "_" : "") + b.words[k];
        if (name.empty()) name = "subpatch";
        // A [p] shares its parent's arguments: inside a bpatcher, its #1 is the bpatcher's.
        nest(b, b.words[0], name, "p " + name + "/", sub_);
    }

    /// A bpatcher whose patcher is embedded in the device becomes a [pd name]
    /// like a [p], with its arguments and its #0 replaced in every box inside
    /// (its own [p]s too); the view it shows changes nothing heard. One that
    /// names a patcher file outside the device is refused: that file is not in
    /// the .amxd.
    void bpatcher(const Box& b) {
        if (!embedded(b) || !b.raw.contains("patcher") || !b.raw["patcher"].is_object())
            return refuse(b, "bpatcher", "its patcher is a file outside the device (not embedded)");
        std::string name = b.raw.value("name", std::string());
        if (name.size() > 7 && name.compare(name.size() - 7, 7, ".maxpat") == 0) name.resize(name.size() - 7);
        name = plainName(name);
        if (name.empty()) name = "bpatcher";
        Substitution sub;
        sub.active = true;
        sub.instance = "bp-" + plainName(prefix_ + b.id);
        if (b.raw.contains("args")) {
            const json& a = b.raw["args"];
            if (!a.is_array()) return refuse(b, "bpatcher", "its arguments are not a list");
            for (const auto& v : a) {
                if (v.is_number_integer()) {
                    sub.args.push_back(std::to_string(v.get<long long>()));
                } else if (v.is_number()) {
                    // Max keeps a float argument a float ("2."), so [+ #1] stays the float form.
                    std::string s = number(v.get<double>());
                    if (!isFloatLiteral(s)) s += ".";
                    sub.args.push_back(s);
                } else if (v.is_string() && !v.get<std::string>().empty() &&
                           v.get<std::string>().find_first_of(" \t\"$#") == std::string::npos) {
                    sub.args.push_back(v.get<std::string>());
                } else {
                    return refuse(b, "bpatcher", "an argument that is not a number or a plain word");
                }
            }
        }
        nest(b, "bpatcher", name, "bpatcher " + name + "/", sub);
    }

    void nest(const Box& b, const std::string& cls, const std::string& name, const std::string& path,
              const Substitution& sub) {
        if (depth_ + 1 > kPdWalkMaxDepth) return refuse(b, cls, "nested too deep");
        Canvas child(out_, prefix_ + path, signalInletsOf(b), depth_ + 1, sub);
        if (!child.read(b.raw["patcher"])) return;
        const std::size_t before = out_.refused.size();
        const std::string inner = child.body();
        if (out_.refused.size() != before || !out_.error.empty()) return;
        single(b, "#N canvas 0 50 600 400 " + name + " 0;\n" + inner + "#X restore " + at(b.x, b.y) + " pd " + name);
    }

    void liveControl(const Box& b, bool toggle = false) {
        for (int o = 1; o < 4; ++o)
            if (connected(b, true, o)) return refuse(b, b.maxClass, "only its value outlet is translated");
        for (int i = 0; i < 4; ++i)
            if (connected(b, false, i)) return refuse(b, b.maxClass, "setting it from the patch is not translated");
        json v = json::object();
        if (b.raw.contains("saved_attribute_attributes") && b.raw["saved_attribute_attributes"].contains("valueof"))
            v = b.raw["saved_attribute_attributes"]["valueof"];
        const auto num = [&](const char* key, double def) {
            if (!v.contains(key)) return def;
            const json& j = v[key];
            if (j.is_number()) return j.get<double>();
            if (j.is_array() && !j.empty() && j[0].is_number()) return j[0].get<double>();
            return def;
        };
        const int type = static_cast<int>(num("parameter_type", 0));
        if (type != 2 && !toggle && num("parameter_steps", 0) >= 2)
            return refuse(b, b.maxClass, "its Steps quantize the parameter, which [adi.param] does not yet");
        std::string name = v.value("parameter_longname", b.raw.value("varname", std::string("Parameter")));
        std::replace(name.begin(), name.end(), ' ', '_');
        double lo = num("parameter_mmin", 0.0), hi = num("parameter_mmax", 127.0);
        std::vector<std::string> items;
        if (type == 2) {
            if (v.contains("parameter_enum") && v["parameter_enum"].is_array())
                for (const auto& e : v["parameter_enum"]) {
                    std::string s = e.is_string() ? e.get<std::string>() : e.dump();
                    std::replace(s.begin(), s.end(), ' ', '_');
                    items.push_back(s);
                }
            if (items.size() < 2) return refuse(b, b.maxClass, "an enum with fewer than two items");
            lo = 0;
            hi = static_cast<double>(items.size() - 1);
        }
        if (toggle) {   // live.text in toggle mode: off and on, nothing else
            lo = 0;
            hi = 1;
            items.clear();
        }
        const bool initialOn = num("parameter_initial_enable", 0) != 0;
        double def = initialOn ? num("parameter_initial", lo) : lo;
        if (!(lo < hi)) return refuse(b, b.maxClass, "its range is empty");
        def = std::clamp(def, lo, hi);
        static const std::map<int, std::string> units{{2, "ms"}, {3, "Hz"}, {4, "dB"}, {5, "%"}};
        const int style = static_cast<int>(num("parameter_unitstyle", 1));
        const std::string unit = units.count(style) ? units.at(style) : "-";
        std::string curve = "lin";
        if (type == 1) curve = "int";
        if (type == 2) curve = "menu";
        if (toggle) curve = "toggle";
        const double exponent = num("parameter_exponent", 1.0);
        if (type == 0 && exponent > 0 && std::fabs(exponent - 1.0) > 1e-12) curve = number(exponent);
        const int id = static_cast<int>(out_.parameters.size()) + 1;
        std::string text = "adi.param \\$0 " + std::to_string(id) + " " + number(lo) + " " + number(hi) + " " +
                           number(def) + " " + unit + " " + curve + " " + name;
        for (const auto& i : items) text += " " + i;
        out_.parameters.push_back(v.value("parameter_longname", name));
        obj(b, text);
    }

    AmxdTranslation& out_;
    std::string prefix_;
    std::set<int> signalInlets_;
    int depth_ = 0;
    Substitution sub_;
    std::vector<Box> boxes_;
    std::vector<Line> lines_;
    std::map<std::string, std::size_t> index_;
    std::map<std::string, Fragment> frags_;
    std::vector<std::string> inletBoxes_, outletBoxes_;
};

}  // namespace

AmxdTranslation translateMaxPatcher(std::string_view text, AmxdKind kind) {
    AmxdTranslation out;
    out.kind = kind;
    if (kind != AmxdKind::AudioEffect) {
        out.error = "only audio effects are translated in this step";
        return out;
    }
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("patcher") || !doc["patcher"].is_object()) {
        out.error = "the patcher is not readable JSON";
        return out;
    }
    Canvas top(out, "", {}, 0);
    if (!top.read(doc["patcher"])) return out;
    const std::string body = top.body();
    if (!out.ok()) return out;
    out.pdPatch = "#N canvas 0 50 900 600 12;\n" + body;
    return out;
}

AmxdTranslation translateAmxd(std::span<const std::uint8_t> bytes) {
    AmxdTranslation fail;
    const auto tag = [&](std::size_t at, const char* t) {
        return at + 4 <= bytes.size() && std::equal(t, t + 4, bytes.begin() + static_cast<std::ptrdiff_t>(at));
    };
    if (!tag(0, "ampf") || bytes.size() < 12) {
        fail.error = "not an .amxd file";
        return fail;
    }
    AmxdKind kind = AmxdKind::Unknown;
    std::size_t at = 0;
    std::optional<std::string> patch;
    while (at + 8 <= bytes.size()) {
        const std::uint32_t len = le32(bytes, at + 4);
        const std::size_t body = at + 8;
        if (body + len > bytes.size()) {
            fail.error = "a chunk runs past the end of the file";
            return fail;
        }
        if (tag(at, "ampf") && len >= 4) {
            if (tag(body, "aaaa")) kind = AmxdKind::AudioEffect;
            else if (tag(body, "mmmm")) kind = AmxdKind::MidiEffect;
            else if (tag(body, "iiii")) kind = AmxdKind::Instrument;
        } else if (tag(at, "ciph")) {
            fail.kind = kind;
            fail.error = "an encrypted device (Live's own); not read";
            return fail;
        } else if (tag(at, "ptch")) {
            if (len >= 4 && tag(body, "mx@c")) {
                fail.kind = kind;
                fail.error = "a frozen device (mx@c); not read in this step";
                return fail;
            }
            std::string s(reinterpret_cast<const char*>(bytes.data() + body), len);
            while (!s.empty() && s.back() == '\0') s.pop_back();
            patch = std::move(s);
        }
        at = body + len;
    }
    if (!patch) {
        fail.kind = kind;
        fail.error = "no patcher in the file";
        return fail;
    }
    return translateMaxPatcher(*patch, kind);
}

}  // namespace adi::device
