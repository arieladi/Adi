// SPDX-License-Identifier: GPL-3.0-or-later

#include "juce/pd_amxd.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

class Translator {
public:
    AmxdTranslation run(std::string_view text, AmxdKind kind) {
        out_.kind = kind;
        if (kind != AmxdKind::AudioEffect) {
            out_.error = "only audio effects are translated in this step";
            return std::move(out_);
        }
        const json doc = json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object() || !doc.contains("patcher") ||
            !doc["patcher"].is_object()) {
            out_.error = "the patcher is not readable JSON";
            return std::move(out_);
        }
        read(doc["patcher"]);
        if (!out_.error.empty()) return std::move(out_);
        for (const auto& b : boxes_) translate(b);
        if (!out_.refused.empty()) return std::move(out_);
        emit();
        return std::move(out_);
    }

private:
    void read(const json& patcher) {
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
                    return;
                }
                lines_.push_back(line);
            }
    }

    void refuse(const Box& b, const std::string& cls, std::string why) {
        out_.refused.push_back(AmxdRefusedBox{b.id, cls, b.text, std::move(why)});
    }

    bool connected(const Box& b, bool asSource, int port) const {
        for (const auto& l : lines_)
            if (asSource ? (l.src == b.id && l.outlet == port) : (l.dst == b.id && l.inlet == port))
                return true;
        return false;
    }
    bool signalInto(const Box& b, int inlet) const {
        for (const auto& l : lines_) {
            if (l.dst != b.id || l.inlet != inlet) continue;
            const Box& s = boxes_[index_.at(l.src)];
            const auto o = static_cast<std::size_t>(l.outlet);
            if (o < s.signalOutlet.size() && s.signalOutlet[o]) return true;
        }
        return false;
    }
    bool anyConnection(const Box& b) const {
        return std::any_of(lines_.begin(), lines_.end(),
                           [&](const Line& l) { return l.src == b.id || l.dst == b.id; });
    }

    /// Pd text for one box, or a refusal.
    void translate(const Box& b) {
        const std::string& mc = b.maxClass;
        if (mc == "comment" || mc == "live.comment") {
            pd_[b.id] = "#X text " + at(b) + " " + pdEscape(b.text);
            return;
        }
        if (mc == "panel" || mc == "fpic" || mc == "live.line") {
            if (anyConnection(b)) refuse(b, mc, "a decoration with connections");
            else dropped_.push_back(b.id);
            return;
        }
        if (mc == "message") {
            pd_[b.id] = "#X msg " + at(b) + " " + pdEscape(b.text);
            return;
        }
        if (mc == "flonum") {
            pd_[b.id] = "#X floatatom " + at(b) + " 5 0 0 0 - - - 0";
            return;
        }
        if (mc == "live.dial" || mc == "live.numbox" || mc == "live.slider") {
            liveControl(b);
            return;
        }
        if (mc != "newobj") {
            refuse(b, mc, "no translation for this kind of box yet");
            return;
        }
        if (b.words.empty()) {
            refuse(b, "newobj", "an empty object box");
            return;
        }
        if (b.text.find('"') != std::string::npos) {
            refuse(b, b.words[0], "quoted symbols are not translated yet");
            return;
        }
        const std::string& cls = b.words[0];
        const std::vector<std::string> args(b.words.begin() + 1, b.words.end());
        const auto allNumbers = std::all_of(args.begin(), args.end(), isNumber);
        const auto obj = [&](const std::string& text) { pd_[b.id] = "#X obj " + at(b) + " " + text; };

        if (cls == "*~" || cls == "+~" || cls == "-~" || cls == "/~") {
            if (args.size() > 1 || !allNumbers) return refuse(b, cls, "expects one number at most");
            // A signal into the right inlet needs Pd's signal form, which has
            // no argument; otherwise the argument form, whose right inlet
            // takes floats as Max's does.
            if (signalInto(b, 1)) obj(cls);
            else obj(cls + " " + (args.empty() ? std::string("0") : args[0]));
            return;
        }
        if (cls == "cycle~" || cls == "phasor~" || cls == "sig~") {
            if (args.size() > 1 || !allNumbers) return refuse(b, cls, "only a numeric frequency is translated");
            if (cls == "cycle~" && connected(b, false, 1))
                return refuse(b, cls, "its phase inlet is an offset in Max and a reset in Pd");
            obj((cls == "cycle~" ? std::string("osc~") : cls) + (args.empty() ? "" : " " + args[0]));
            return;
        }
        if (cls == "plugin~" || cls == "plugout~") {
            if (!allNumbers) return refuse(b, cls, "channel numbers only");
            std::string t = cls == "plugin~" ? "adc~" : "dac~";
            if (args.empty()) t += " 1 2";
            for (const auto& a : args) t += " " + a;
            obj(t);
            return;
        }
        if (cls == "loadbang") return obj("loadbang");
        if (cls == "live.thisdevice") {
            if (connected(b, true, 1) || connected(b, true, 2))
                return refuse(b, cls, "only its load bang (left outlet) is translated");
            return obj("loadbang");
        }
        if (cls == "metro") {
            if (args.size() != 1 || !isNumber(args[0])) return refuse(b, cls, "only a time in ms is translated");
            return obj("metro " + args[0]);
        }
        if (cls == "t" || cls == "trigger") {
            std::string t = "t";
            for (const auto& a : args) {
                if (a == "b" || a == "f" || a == "l" || a == "s") t += " " + a;
                else return refuse(b, cls, "only b, f, l and s outlets are translated ('" + a + "')");
            }
            return obj(t);
        }
        if (cls == "+" || cls == "-" || cls == "*" || cls == "/") {
            if (args.size() != 1 || !isFloatLiteral(args[0]))
                return refuse(b, cls, "Max's integer arithmetic (write the argument as a float, e.g. 2.)");
            return obj(cls + " " + args[0]);
        }
        if (cls == "s" || cls == "send" || cls == "r" || cls == "receive") {
            if (args.size() != 1 || isNumber(args[0])) return refuse(b, cls, "one name");
            std::string name = args[0];
            if (name.rfind("---", 0) == 0) name = "\\$0-" + name.substr(3);
            return obj(std::string(cls[0] == 's' ? "s " : "r ") + name);
        }
        if (cls == "print") return obj(b.text);
        refuse(b, cls, "no translation for this object yet");
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
        def = std::clamp(def, lo, hi);
        if (!(lo < hi)) return refuse(b, b.maxClass, "its range is empty");
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
        pd_[b.id] = "#X obj " + at(b) + " " + text;
    }

    static std::string at(const Box& b) {
        return std::to_string(static_cast<long>(std::lround(b.x))) + " " +
               std::to_string(static_cast<long>(std::lround(b.y)));
    }

    void emit() {
        std::ostringstream o;
        o << "#N canvas 0 50 900 600 12;\n";
        std::map<std::string, int> pdIndex;
        int n = 0;
        for (const auto& b : boxes_) {
            if (pd_.count(b.id) == 0) continue;
            o << pd_[b.id] << ";\n";
            pdIndex[b.id] = n++;
        }
        // Group connections by source outlet; fan-outs from control outlets
        // get an explicit trigger.
        std::map<std::pair<std::string, int>, std::vector<const Line*>> fan;
        for (const auto& l : lines_) fan[{l.src, l.outlet}].push_back(&l);
        std::ostringstream connects;
        for (auto& [key, ls] : fan) {
            const Box& src = boxes_[index_.at(key.first)];
            const auto o2 = static_cast<std::size_t>(key.second);
            const bool signal = o2 < src.signalOutlet.size() && src.signalOutlet[o2];
            if (signal || ls.size() == 1) {
                for (const Line* l : ls)
                    connects << "#X connect " << pdIndex.at(l->src) << " " << l->outlet << " "
                             << pdIndex.at(l->dst) << " " << l->inlet << ";\n";
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
            o << "#X obj " << static_cast<long>(std::lround(src.x)) << " "
              << static_cast<long>(std::lround(src.y)) + 25 << " " << t << ";\n";
            const int trig = n++;
            connects << "#X connect " << pdIndex.at(key.first) << " " << key.second << " " << trig << " 0;\n";
            // [t] fires its rightmost outlet first: the first destination.
            for (std::size_t i = 0; i < ls.size(); ++i)
                connects << "#X connect " << trig << " " << (ls.size() - 1 - i) << " "
                         << pdIndex.at(ls[i]->dst) << " " << ls[i]->inlet << ";\n";
        }
        o << connects.str();
        out_.pdPatch = o.str();
    }

    AmxdTranslation out_;
    std::vector<Box> boxes_;
    std::vector<Line> lines_;
    std::map<std::string, std::size_t> index_;
    std::map<std::string, std::string> pd_;
    std::vector<std::string> dropped_;
};

}  // namespace

AmxdTranslation translateMaxPatcher(std::string_view json, AmxdKind kind) {
    return Translator{}.run(json, kind);
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
