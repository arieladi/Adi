// SPDX-License-Identifier: GPL-3.0-or-later

#include "juce/pd_amxd.hpp"

#include <nlohmann/json.hpp>

#include "juce/pd_object_walk.hpp"

#include <algorithm>
#include <array>
#include <set>
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
    double x = 0, y = 0;
    std::vector<bool> signalOutlet;
    json raw;
};

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
    Canvas(AmxdTranslation& out, std::string prefix, std::set<int> signalInlets, int depth)
        : out_(out), prefix_(std::move(prefix)), signalInlets_(std::move(signalInlets)), depth_(depth) {}

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
                if (b.contains("patching_rect") && b["patching_rect"].is_array() &&
                    b["patching_rect"].size() >= 2) {
                    box.x = b["patching_rect"][0].get<double>();
                    box.y = b["patching_rect"][1].get<double>();
                }
                if (b.contains("outlettype") && b["outlettype"].is_array())
                    for (const auto& t : b["outlettype"]) {
                        const std::string s = t.is_string() ? t.get<std::string>() : "";
                        box.signalOutlet.push_back(s == "signal" || s == "multichannelsignal");
                    }
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
            const auto p = frags_.at(l.dst).in(l.inlet);
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
            // Max: right to left by destination x; equal x, lower box first.
            std::sort(ls.begin(), ls.end(), [&](const Line* a, const Line* b) {
                const Box& da = boxes_[index_.at(a->dst)];
                const Box& db = boxes_[index_.at(b->dst)];
                if (da.x != db.x) return da.x > db.x;
                return da.y > db.y;
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
        return b.maxClass == "newobj" && !b.words.empty() && (b.words[0] == "p" || b.words[0] == "patcher");
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

    void single(const Box& b, const std::string& record) { frags_[b.id] = Fragment{{record}, {}, {}, {}}; }
    void obj(const Box& b, const std::string& text) { single(b, "#X obj " + at(b.x, b.y) + " " + text); }

    /// A Max integer box: each listed inlet goes through [i] first; with
    /// `truncateOut`, the result through [i] too (integer division).
    void integer(const Box& b, const std::string& pdText, int inlets, bool truncateOut) {
        Fragment f;
        for (int k = 0; k < inlets; ++k) f.records.push_back("#X obj " + at(b.x + 40.0 * k, b.y - 20) + " i");
        const int op = inlets;
        f.records.push_back("#X obj " + at(b.x, b.y) + " " + pdText);
        for (int k = 0; k < inlets; ++k) {
            f.internal.push_back({k, 0, op, k});
            f.inlets.emplace_back(k, 0);
        }
        f.outlets = {{op, 0}};
        if (truncateOut) {
            f.records.push_back("#X obj " + at(b.x, b.y + 20) + " i");
            f.internal.push_back({op, 0, op + 1, 0});
            f.outlets = {{op + 1, 0}};
        }
        frags_[b.id] = std::move(f);
    }

    void translate(const Box& b) {
        const std::string& mc = b.maxClass;
        if (mc == "comment" || mc == "live.comment") return single(b, "#X text " + at(b.x, b.y) + " " + pdEscape(b.text));
        if (mc == "panel" || mc == "fpic" || mc == "live.line") {
            if (anyConnection(b)) refuse(b, mc, "a decoration with connections");
            return;
        }
        if (mc == "message") return single(b, "#X msg " + at(b.x, b.y) + " " + pdEscape(b.text));
        if (mc == "flonum") return single(b, "#X floatatom " + at(b.x, b.y) + " 5 0 0 0 - - - 0");
        if (mc == "number") {
            // Max's number box holds an int: what arrives is truncated first.
            Fragment f;
            f.records = {"#X obj " + at(b.x, b.y - 20) + " i", "#X floatatom " + at(b.x, b.y) + " 5 0 0 0 - - - 0"};
            f.internal.push_back({0, 0, 1, 0});
            f.inlets.push_back({0, 0});
            f.outlets.push_back({1, 0});
            frags_[b.id] = std::move(f);
            return;
        }
        if (mc == "inlet" || mc == "outlet") {
            const bool signal = mc == "inlet" ? (!b.signalOutlet.empty() && b.signalOutlet[0]) : fedSignal(b, 0);
            return obj(b, mc + (signal ? "~" : ""));
        }
        if (mc == "live.dial" || mc == "live.numbox" || mc == "live.slider") return liveControl(b);
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
            return obj(b, cls + " " + (args.empty() ? std::string("0") : args[0]));
        }
        if (cls == "cycle~" || cls == "phasor~" || cls == "sig~") {
            if (args.size() > 1 || !allNumbers) return refuse(b, cls, "only a numeric frequency is translated");
            if (cls == "cycle~" && connected(b, false, 1))
                return refuse(b, cls, "its phase inlet is an offset in Max and a reset in Pd");
            return obj(b, (cls == "cycle~" ? std::string("osc~") : cls) + (args.empty() ? "" : " " + args[0]));
        }
        if (cls == "plugin~" || cls == "plugout~") {
            if (depth_ > 0) return refuse(b, cls, "audio ports belong in the device's own patcher");
            if (!allNumbers) return refuse(b, cls, "channel numbers only");
            std::string t = cls == "plugin~" ? "adc~" : "dac~";
            if (args.empty()) t += " 1 2";
            for (const auto& a : args) t += " " + a;
            return obj(b, t);
        }
        if (cls == "loadbang") return obj(b, "loadbang");
        if (cls == "live.thisdevice") {
            if (connected(b, true, 1) || connected(b, true, 2))
                return refuse(b, cls, "only its load bang (left outlet) is translated");
            return obj(b, "loadbang");
        }
        if (cls == "metro") {
            if (args.size() != 1 || !isNumber(args[0])) return refuse(b, cls, "only a time in ms is translated");
            return obj(b, "metro " + args[0]);
        }
        if (cls == "t" || cls == "trigger") return trigger(b, args);
        if (cls == "pack" || cls == "unpack") return packing(b, cls, args);
        static const std::set<std::string> twoInlet{"+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "min", "max"};
        if (twoInlet.count(cls) != 0 || cls == "abs") {
            if (args.size() > 1 || !allNumbers) return refuse(b, cls, "expects one number at most");
            const bool isFloat = !args.empty() && isFloatLiteral(args[0]);
            if (isFloat) {
                if (cls == "%") return refuse(b, cls, "a float modulo has no Pd counterpart yet");
                return obj(b, b.text);
            }
            // Max's integer form: inputs truncated; '/' truncates its result too.
            const std::string pdText = cls + (args.empty() || cls == "abs" ? "" : " " + args[0]);
            return integer(b, pdText, cls == "abs" ? 1 : 2, cls == "/");
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
        if (cls == "pack") {
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
        if (depth_ + 1 > kPdWalkMaxDepth) return refuse(b, b.words[0], "nested too deep");
        std::string name;
        for (std::size_t k = 1; k < b.words.size(); ++k) name += (k > 1 ? "_" : "") + b.words[k];
        if (name.empty()) name = "subpatch";
        Canvas child(out_, prefix_ + "p " + name + "/", signalInletsOf(b), depth_ + 1);
        if (!child.read(b.raw["patcher"])) return;
        const std::size_t before = out_.refused.size();
        const std::string inner = child.body();
        if (out_.refused.size() != before || !out_.error.empty()) return;
        single(b, "#N canvas 0 50 600 400 " + name + " 0;\n" + inner + "#X restore " + at(b.x, b.y) + " pd " + name);
    }

    void liveControl(const Box& b) {
        if (connected(b, true, 1)) return refuse(b, b.maxClass, "only its value outlet is translated");
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
