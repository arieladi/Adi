// SPDX-License-Identifier: AGPL-3.0-or-later
//
// AC-24 (CTL-02): the registry describes itself, with one authority.
//
// Every op's generated schema is a closed object, and a payload passes the
// schema exactly when the boundary (normalizeSubmission, then
// validateSubmission) accepts it -- checked over generated payloads for every
// registered op and a synthetic op that uses every kind of metadata, under
// both bounds profiles. The schema reader below is a deliberately small,
// independent reading of JSON Schema 2020-12's keywords as the generator uses
// them; it exists only here, so production has one validator.

#include "adi/ops.hpp"
#include "adi/ops_schema.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace adi;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void section(const char* s) { std::printf("[%s]\n", s); }

// --- an independent reading of the schema ---------------------------------------------

std::size_t codePoints(const std::string& s) {
    std::size_t n = 0;
    for (const char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) ++n;
    return n;
}

constexpr double kTwo63 = 9223372036854775808.0;

/// Exact comparison of two JSON numbers: -1, 0 or 1.
int compareNumbers(const Payload& a, const Payload& b) {
    if (a.is_number_float() && b.is_number_float()) {
        const double x = a.get<double>(), y = b.get<double>();
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (a.is_number_float() || b.is_number_float()) {
        const bool flip = !a.is_number_float();
        const double d = (flip ? b : a).get<double>();
        const Payload& i = flip ? a : b;
        int r = 0;
        if (i.is_number_unsigned() && i.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) {
            const auto u = i.get<std::uint64_t>();
            if (d >= 2.0 * kTwo63) r = 1;
            else if (d < 0) r = -1;
            else {
                const auto t = static_cast<std::uint64_t>(d);
                const double frac = d - static_cast<double>(t);
                r = t < u ? -1 : t > u ? 1 : (frac > 0 ? 1 : 0);
            }
        } else {
            const auto v = i.get<std::int64_t>();
            if (d >= kTwo63) r = 1;
            else if (d < -kTwo63) r = -1;
            else {
                const auto t = static_cast<std::int64_t>(d);
                const double frac = d - static_cast<double>(t);
                r = t < v ? -1 : t > v ? 1 : (frac > 0 ? 1 : frac < 0 ? -1 : 0);
            }
        }
        return flip ? -r : r;
    }
    // Two integers, either signed or unsigned.
    const bool au = a.is_number_unsigned(), bu = b.is_number_unsigned();
    if (au && bu) {
        const auto x = a.get<std::uint64_t>(), y = b.get<std::uint64_t>();
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (!au && !bu) {
        const auto x = a.get<std::int64_t>(), y = b.get<std::int64_t>();
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (au) {
        const auto x = a.get<std::uint64_t>();
        const auto y = b.get<std::int64_t>();
        if (y < 0) return 1;
        return x < static_cast<std::uint64_t>(y) ? -1 : x > static_cast<std::uint64_t>(y) ? 1 : 0;
    }
    return -compareNumbers(b, a);
}

bool isInteger(const Payload& v) {
    if (v.is_number_integer()) return true;
    if (!v.is_number_float()) return false;
    const double d = v.get<double>();
    return std::isfinite(d) && std::floor(d) == d;
}

bool typeIs(const std::string& t, const Payload& v) {
    if (t == "integer") return isInteger(v);
    if (t == "number") return v.is_number() && (!v.is_number_float() || std::isfinite(v.get<double>()));
    if (t == "string") return v.is_string();
    if (t == "boolean") return v.is_boolean();
    if (t == "array") return v.is_array();
    if (t == "object") return v.is_object();
    if (t == "null") return v.is_null();
    return false;
}

/// `root` is the op's whole schema, the document a "$ref" ("#/$defs/...")
/// points into.
bool schemaAccepts(const Payload& s, const Payload& v, const Payload& root) {
    if (s.contains("$ref")) {
        const auto ref = s.at("$ref").get<std::string>();
        if (!ref.starts_with("#/")) return false;
        const Payload::json_pointer at(ref.substr(1));
        if (!root.contains(at) || !schemaAccepts(root.at(at), v, root)) return false;
    }
    if (s.contains("anyOf")) {
        bool any = false;
        for (const auto& one : s.at("anyOf")) any = any || schemaAccepts(one, v, root);
        if (!any) return false;
    }
    if (s.contains("type")) {
        const auto& t = s.at("type");
        bool ok = false;
        if (t.is_string()) ok = typeIs(t.get<std::string>(), v);
        else
            for (const auto& one : t) ok = ok || typeIs(one.get<std::string>(), v);
        if (!ok) return false;
    }
    if (s.contains("enum")) {
        bool found = false;
        for (const auto& e : s.at("enum")) found = found || e == v;
        if (!found) return false;
    }
    if (v.is_number()) {
        if (s.contains("minimum") && compareNumbers(v, s.at("minimum")) < 0) return false;
        if (s.contains("maximum") && compareNumbers(v, s.at("maximum")) > 0) return false;
    }
    if (v.is_string() && s.contains("maxLength") &&
        codePoints(v.get<std::string>()) > s.at("maxLength").get<std::size_t>())
        return false;
    if (v.is_array()) {
        if (s.contains("maxItems") && v.size() > s.at("maxItems").get<std::size_t>()) return false;
        if (s.contains("items"))
            for (const auto& e : v)
                if (!schemaAccepts(s.at("items"), e, root)) return false;
    }
    if (v.is_object()) {
        if (s.contains("maxProperties") && v.size() > s.at("maxProperties").get<std::size_t>()) return false;
        if (s.contains("required"))
            for (const auto& k : s.at("required"))
                if (!v.contains(k.get<std::string>())) return false;
        const Payload props = s.value("properties", Payload::object());
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (s.contains("propertyNames") && !schemaAccepts(s.at("propertyNames"), Payload(it.key()), root))
                return false;
            if (props.contains(it.key())) {
                if (!schemaAccepts(props.at(it.key()), it.value(), root)) return false;
            } else if (s.contains("additionalProperties")) {
                const auto& extra = s.at("additionalProperties");
                if (extra.is_boolean()) {
                    if (!extra.get<bool>()) return false;
                } else if (!schemaAccepts(extra, it.value(), root)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool schemaAccepts(const Payload& schema, const Payload& v) { return schemaAccepts(schema, v, schema); }

bool boundaryAccepts(const OpDescriptor& d, Payload p, Bounds b) {
    normalizeSubmission(d, p);
    return validateSubmission(d, p, b).empty();
}

// --- payload generation ---------------------------------------------------------------

Payload sampleOf(const Field& f);

Payload baseOf(std::span<const Field> fields) {
    Payload p = Payload::object();
    for (const auto& f : fields)
        if (f.required) p[std::string(f.key)] = sampleOf(f);
    return p;
}

Payload sampleOf(const Field& f) {
    switch (f.type) {
        case FieldType::Int:
            if (f.minimum) return static_cast<std::int64_t>(std::ceil(*f.minimum));
            return 1;
        case FieldType::Real:
            if (f.minimum) return *f.minimum;
            return 0.5;
        case FieldType::Text:
            return f.enumValues.empty() ? std::string("x") : std::string(f.enumValues.front());
        case FieldType::Bool: return true;
        case FieldType::Array:
            if (!f.items.empty()) return Payload::array({baseOf(f.items)});
            return Payload::array();
        case FieldType::Object:
            if (!f.items.empty()) return baseOf(f.items);
            return Payload::object();
    }
    return nullptr;
}

std::vector<Payload> oddValues() {
    return {Payload(1),
            Payload(-1),
            Payload(1.5),
            Payload(1.0),
            Payload(-7.0),
            Payload("s"),
            Payload(true),
            Payload::array(),
            Payload::object(),
            Payload(nullptr),
            Payload(static_cast<std::uint64_t>(INT64_MAX) + 6u),
            Payload(std::numeric_limits<std::int64_t>::min()),
            Payload(1e19),
            Payload(2e19),
            Payload(kTwo63),
            Payload(-kTwo63),
            Payload(std::numeric_limits<std::uint64_t>::max())};
}

/// Every generated case for one op: true when the schema and the boundary agree on all.
int compareOp(const OpDescriptor& d, Bounds b, std::string& firstMismatch) {
    const Payload schema = opInputSchema(d, b);
    std::vector<Payload> cases;
    const Payload base = baseOf(d.fields);
    cases.push_back(base);
    for (const auto& f : d.fields) {
        const std::string k(f.key);
        if (!f.required) {
            Payload with = base;
            with[k] = sampleOf(f);
            cases.push_back(with);
            Payload nul = base;
            nul[k] = nullptr;
            cases.push_back(nul);
        } else {
            Payload without = base;
            without.erase(k);
            cases.push_back(without);
        }
        for (const auto& odd : oddValues()) {
            Payload p = base;
            p[k] = odd;
            cases.push_back(p);
        }
    }
    // Declared items: every odd value placed inside the element, the
    // element missing each required key, and an unknown key inside it.
    for (const auto& f : d.fields) {
        if (f.items.empty() || (f.type != FieldType::Array && f.type != FieldType::Object)) continue;
        const std::string k(f.key);
        const Payload element = baseOf(f.items);
        const auto place = [&](const Payload& inner) {
            Payload p = base;
            p[k] = f.type == FieldType::Array ? Payload::array({inner}) : inner;
            return p;
        };
        for (const auto& g : f.items) {
            const std::string k2(g.key);
            for (const auto& odd : oddValues()) {
                Payload inner = element;
                inner[k2] = odd;
                cases.push_back(place(inner));
            }
            if (g.required) {
                Payload inner = element;
                inner.erase(k2);
                cases.push_back(place(inner));
            }
        }
        Payload extra = element;
        extra["zzInner"] = 1;
        cases.push_back(place(extra));
    }

    Payload unknown = base;
    unknown["zzUnknown"] = 1;
    cases.push_back(unknown);
    cases.push_back(Payload::array());
    cases.push_back(Payload("x"));
    cases.push_back(Payload(nullptr));

    int mismatches = 0;
    for (const auto& c : cases) {
        const bool s = c.is_object() ? schemaAccepts(schema, c) : false;
        const bool v = boundaryAccepts(d, c, b);
        if (s != v) {
            if (!mismatches)
                firstMismatch = std::string(d.name) + " " + c.dump() + ": schema " + (s ? "accepts" : "refuses") +
                                ", boundary " + (v ? "accepts" : "refuses");
            ++mismatches;
        }
    }
    return mismatches;
}

// --- the synthetic op: every kind of metadata -------------------------------------------

constexpr std::string_view kModes[] = {"a", "b"};
constexpr Field kItem[] = {{"t", FieldType::Int, true, "time", {}, 0.0, 100.0},
                           {"v", FieldType::Real, false, "value", {}, -1.0, 1.0}};
constexpr Field kRich[] = {
    {"mode", FieldType::Text, true, "the mode", kModes},
    {"opt", FieldType::Text, false, "an optional mode", kModes},
    {"name", FieldType::Text, false, "a name", {}, std::nullopt, std::nullopt, 5},
    {"note", FieldType::Text, false, "free text, no declared length"},
    {"gain", FieldType::Real, false, "gain", {}, -12.0, 6.0},
    {"count", FieldType::Int, false, "count", {}, 0.0, 10.0},
    {"pts", FieldType::Array, false, "points", {}, std::nullopt, std::nullopt, std::nullopt, 3, kItem},
    {"bag", FieldType::Array, false, "anything, no declared size"},
    {"free", FieldType::Object, false, "anything, no declared members"},
    {"obj", FieldType::Object, false, "inner", {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt, kItem},
};

OpDescriptor richOp() {
    OpDescriptor d;
    d.name = "test.rich";
    d.summary = "A synthetic op with every kind of field metadata";
    d.scope = Scope::Edit;
    d.fields = kRich;
    return d;
}

std::vector<ValidationIssue> issuesOf(const OpDescriptor& d, Payload p, Bounds b = Bounds::Local) {
    normalizeSubmission(d, p);
    return validateSubmission(d, p, b);
}

bool names(const std::vector<ValidationIssue>& issues, const std::string& key) {
    for (const auto& i : issues)
        if (i.key == key) return true;
    return false;
}

// --- the sections ---------------------------------------------------------------------------

void testEveryOpIsClosedAndEquivalent() {
    section("AC-24: every registered op has a closed schema the boundary agrees with");
    const auto& reg = OpRegistry::instance();
    int ops = 0, totalMismatches = 0;
    std::string first;
    for (const auto& d : reg.all()) {
        ++ops;
        // The remote schema (the default) shows every field but the op's own
        // '_' keys; the local one shows them all.
        std::size_t callerFields = 0;
        for (const auto& f : d.fields)
            if (!f.key.starts_with('_')) ++callerFields;
        const Payload s = opInputSchema(d);
        const bool closed = s.value("type", "") == "object" && s.contains("additionalProperties") &&
                            s.at("additionalProperties") == false && s.contains("properties") &&
                            s.at("properties").size() == callerFields &&
                            opInputSchema(d, Bounds::Local).at("properties").size() == d.fields.size();
        check(closed, std::string(d.name) + ": a closed object with one property per field");
        for (const auto b : {Bounds::Local, Bounds::Remote}) {
            std::string mismatch;
            const int m = compareOp(d, b, mismatch);
            if (m && first.empty()) first = mismatch;
            totalMismatches += m;
        }
    }
    check(ops > 100, "the registry has its ops (" + std::to_string(ops) + ")");
    check(totalMismatches == 0, "schema and boundary agree on every generated payload" +
                                    (first.empty() ? std::string() : ": first mismatch " + first));
}

void testRichMetadata() {
    section("declared metadata: one authority, refusals name the key");
    const auto d = richOp();
    for (const auto b : {Bounds::Local, Bounds::Remote}) {
        std::string mismatch;
        check(compareOp(d, b, mismatch) == 0, "the synthetic op agrees with its schema " + mismatch);
    }

    check(issuesOf(d, {{"mode", "a"}}).empty(), "the base payload is accepted");
    check(names(issuesOf(d, {{"mode", "c"}}), "mode"), "a value outside the enum is refused, naming mode");
    check(issuesOf(d, {{"mode", "a"}, {"opt", nullptr}}).empty(), "an optional enum may be null");
    check(names(issuesOf(d, {{"mode", "a"}, {"name", "abcdef"}}), "name"), "six characters over five, naming name");
    check(issuesOf(d, {{"mode", "a"}, {"name", "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"}}).empty(),
          "five characters in ten bytes: maxLength counts characters");
    check(names(issuesOf(d, {{"mode", "a"}, {"gain", 7}}), "gain"), "above the maximum, naming gain");
    check(names(issuesOf(d, {{"mode", "a"}, {"gain", -12.5}}), "gain"), "below the minimum, naming gain");
    check(issuesOf(d, {{"mode", "a"}, {"count", 10}}).empty(), "a bound is inclusive");
    check(issuesOf(d, {{"mode", "a"}, {"count", 5.0}}).empty(), "5.0 is the integer 5 at the boundary");
    check(names(issuesOf(d, {{"mode", "a"}, {"count", 5.5}}), "count"), "5.5 is not an integer");
    check(names(issuesOf(d, {{"mode", "a"}, {"count", 11}}), "count"), "11 over a maximum of 10");
    check(names(issuesOf(d, {{"mode", "a"}, {"pts", Payload::array({{{"t", 1}}, {{"t", 2}}, {{"t", 3}}, {{"t", 4}}})}}),
                "pts"),
          "four points over a maxItems of three");
    check(names(issuesOf(d, {{"mode", "a"}, {"pts", Payload::array({Payload::object()})}}), "pts[0].t"),
          "a nested missing key is named by its path");
    check(names(issuesOf(d, {{"mode", "a"}, {"pts", Payload::array({{{"t", 1}, {"x", 1}}})}}), "pts[0].x"),
          "a nested unknown key is named by its path");
    check(issuesOf(d, {{"mode", "a"}, {"pts", Payload::array({{{"t", 1}, {"v", nullptr}}})}}).empty(),
          "a nested optional may be null");
    check(names(issuesOf(d, {{"mode", "a"}, {"pts", Payload::array({1})}}), "pts[0]"),
          "a nested element that is not an object is named");
    check(names(issuesOf(d, {{"mode", "a"}, {"obj", Payload::object()}}), "obj.t"),
          "a declared object is closed and complete");
    check(names(issuesOf(d, {{"mode", "a"}, {"count", static_cast<std::uint64_t>(INT64_MAX) + 1u}}), "count"),
          "a declared maximum holds for an unsigned value too");

    // An Int is an int64: every handler but the note ones reads it with
    // get<int64_t>, which would wrap anything above INT64_MAX to a negative.
    // Only a note's persisted id (fullUint64) is a full uint64.
    const auto& reg = OpRegistry::instance();
    const auto* vol = reg.find("mixer.setVolume");
    const auto* del = reg.find("note.delete");
    const std::uint64_t two63 = static_cast<std::uint64_t>(INT64_MAX) + 1u;
    for (const auto b : {Bounds::Local, Bounds::Remote}) {
        const std::string profile = b == Bounds::Local ? "local: " : "remote: ";
        check(vol && names(issuesOf(*vol, {{"id", std::numeric_limits<std::uint64_t>::max()}, {"db", 1.0}}, b), "id"),
              profile + "UINT64_MAX on a non-note Int is refused, naming id, never wrapped to -1");
        check(vol && names(issuesOf(*vol, {{"id", two63}, {"db", 1.0}}, b), "id"),
              profile + "2^63 is refused exactly (a double bound would let it through)");
        check(vol && issuesOf(*vol, {{"id", INT64_MAX}, {"db", 1.0}}, b).empty(), profile + "INT64_MAX is an Int");
        check(vol && issuesOf(*vol, {{"id", INT64_MIN}, {"db", 1.0}}, b).empty(), profile + "INT64_MIN is an Int");
        check(vol && names(issuesOf(*vol, {{"id", 1e19}, {"db", 1.0}}, b), "id"),
              profile + "1e19, a whole float above INT64_MAX, is refused on a non-note Int");
        check(del && issuesOf(*del, {{"clip", 1}, {"note", std::numeric_limits<std::uint64_t>::max()}}, b).empty(),
              profile + "a note id is a full uint64");
        check(del && issuesOf(*del, {{"clip", 1}, {"note", 1e19}}, b).empty(),
              profile + "1e19 is that note id at the boundary");
        check(del && names(issuesOf(*del, {{"clip", two63}, {"note", 1}}, b), "clip"),
              profile + "the clip beside a note id is still an int64");
    }
    check(vol && names(issuesOf(*vol, {{"id", 2e19}, {"db", 1.0}}), "id"),
          "a whole number beyond UINT64_MAX is refused, naming the key");
    check(del && names(issuesOf(*del, {{"clip", 1}, {"note", 2e19}}), "note"),
          "beyond UINT64_MAX is refused for a note id too");

    // The schema says the same.
    const auto* create = reg.find("track.create");
    if (create && del) {
        const Payload id = opInputSchema(*create, Bounds::Remote).at("properties").at("id");
        check(id.at("maximum") == Payload(INT64_MAX) && id.at("maximum").is_number_integer(),
              "the schema: track.create's id has maximum INT64_MAX: " + id.dump());
        const Payload note = opInputSchema(*del, Bounds::Remote).at("properties").at("note");
        check(note.at("maximum") == Payload(std::numeric_limits<std::uint64_t>::max()),
              "the schema: note.delete's note has maximum UINT64_MAX: " + note.dump());
    } else {
        check(false, "track.create and note.delete are registered");
    }
}

void testOpenValues() {
    section("the remote bounds reach inside an open object or array (P3.1)");
    const auto d = richOp();
    const std::string longText(kRemoteMaxTextLength + 1, 'x');
    const std::string fits(kRemoteMaxTextLength, 'x');
    check(issuesOf(d, {{"mode", "a"}, {"bag", Payload::array({longText})}}, Bounds::Local).empty(),
          "local: an open array's element is not capped");
    check(names(issuesOf(d, {{"mode", "a"}, {"bag", Payload::array({longText})}}, Bounds::Remote), "bag[0]"),
          "remote: an open array's long text element is refused, naming bag[0]");
    check(issuesOf(d, {{"mode", "a"}, {"bag", Payload::array({fits})}}, Bounds::Remote).empty(),
          "remote: 16,384 characters inside fit");
    Payload deep = Payload::array({{{"a", Payload::array({1, {{"b", longText}}})}}});
    check(names(issuesOf(d, {{"mode", "a"}, {"bag", deep}}, Bounds::Remote), "bag[0].a[1].b"),
          "remote: the cap holds at any depth, naming the path");
    Payload wide = Payload::array();
    for (std::size_t i = 0; i <= kRemoteMaxArrayItems; ++i) wide.push_back(static_cast<std::int64_t>(i));
    check(names(issuesOf(d, {{"mode", "a"}, {"bag", Payload::array({wide})}}, Bounds::Remote), "bag[0]"),
          "remote: a nested array of 4,097 items is refused");
    Payload longKey = Payload::object();
    longKey[longText] = 1;
    check(names(issuesOf(d, {{"mode", "a"}, {"bag", Payload::array({longKey})}}, Bounds::Remote), "bag[0]"),
          "remote: a member name is text, and capped too");

    // The remote schema states the same recursive caps: a payload passes it
    // exactly when the remote boundary accepts it, inside open values too.
    const Payload remoteSchema = opInputSchema(d, Bounds::Remote);
    const Payload localSchema = opInputSchema(d, Bounds::Local);
    check(remoteSchema.contains("$defs") && remoteSchema.at("$defs").contains("openValue"),
          "the remote schema defines openValue at its root");
    check(!localSchema.contains("$defs") && localSchema.dump().find("$ref") == std::string::npos,
          "the local schema has no open-value caps");
    check(remoteSchema.at("properties").at("bag").at("items") == Payload{{"$ref", "#/$defs/openValue"}},
          "an open array's items are open values");
    check(remoteSchema.at("properties").at("free").value("maxProperties", std::size_t{0}) == kRemoteMaxArrayItems &&
              remoteSchema.at("properties").at("free").at("additionalProperties") ==
                  Payload{{"$ref", "#/$defs/openValue"}},
          "an open object's members are capped and are open values");
    const auto* closedOp = OpRegistry::instance().find("mixer.setVolume");
    check(closedOp && !opInputSchema(*closedOp, Bounds::Remote).contains("$defs"),
          "an op with no open field carries no $defs");

    Payload manyMembers = Payload::object();
    for (std::size_t i = 0; i <= kRemoteMaxArrayItems; ++i) manyMembers["m" + std::to_string(i)] = 1;
    Payload topLongKey = Payload::object();
    topLongKey[longText] = 1;
    Payload fitsKey = Payload::object();
    fitsKey[fits] = Payload::array({fits, nullptr, true, 1.5, -3});
    Payload deepValid = Payload::array(
        {{{"a", Payload::array({1, {{"b", fits}, {"c", Payload::object()}}, Payload::array({Payload::array()})})},
          {"d", {{"e", {{"f", "x"}}}}}}});
    struct Case {
        const char* what;
        Payload payload;
        bool accepted;
    };
    const std::vector<Case> cases = {
        {"a long string inside an open object", {{"mode", "a"}, {"free", {{"k", longText}}}}, false},
        {"a long string deep inside an open object", {{"mode", "a"}, {"free", {{"k", {{"j", Payload::array({longText})}}}}}}, false},
        {"a long member name on the open object itself", {{"mode", "a"}, {"free", topLongKey}}, false},
        {"a long member name inside an open object", {{"mode", "a"}, {"free", {{"k", topLongKey}}}}, false},
        {"a long member name inside an open array", {{"mode", "a"}, {"bag", Payload::array({longKey})}}, false},
        {"a member name of exactly the cap", {{"mode", "a"}, {"free", fitsKey}}, true},
        {"an open object with 4,097 members", {{"mode", "a"}, {"free", manyMembers}}, false},
        {"a nested object with 4,097 members", {{"mode", "a"}, {"free", {{"k", manyMembers}}}}, false},
        {"a nested array of 4,097", {{"mode", "a"}, {"bag", Payload::array({wide})}}, false},
        {"a nested array of 4,097 in an open object", {{"mode", "a"}, {"free", {{"k", wide}}}}, false},
        {"a deep nested valid value in an open array", {{"mode", "a"}, {"bag", deepValid}}, true},
        {"a deep nested valid value in an open object", {{"mode", "a"}, {"free", {{"k", deepValid}}}}, true},
        {"a long text element of an open array", {{"mode", "a"}, {"bag", Payload::array({longText})}}, false},
        {"a null open object", {{"mode", "a"}, {"free", nullptr}}, true},
    };
    for (const auto& c : cases) {
        const bool s = schemaAccepts(remoteSchema, c.payload);
        const bool b = boundaryAccepts(d, c.payload, Bounds::Remote);
        check(s == b && b == c.accepted, std::string("remote: ") + c.what + ": schema " +
                                             (s ? "accepts" : "refuses") + ", boundary " +
                                             (b ? "accepts" : "refuses"));
        // Locally nothing inside an open value is capped, and the local
        // schema says so.
        check(schemaAccepts(localSchema, c.payload) == boundaryAccepts(d, c.payload, Bounds::Local),
              std::string("local: ") + c.what + ": schema and boundary agree");
    }

    const auto& reg = OpRegistry::instance();
    const auto* lane = reg.find("automation.createLane");
    check(lane != nullptr, "automation.createLane is registered");
    if (lane) {
        const Payload p = {{"id", 1}, {"row", {{"param_name", longText}}}};
        check(issuesOf(*lane, p, Bounds::Local).empty(), "local: createLane's open row is not capped");
        check(names(issuesOf(*lane, p, Bounds::Remote), "row.param_name"),
              "remote: createLane's row.param_name of 16,385 characters is refused");
    }
    const auto* insert = reg.find("device.insert");
    check(insert != nullptr, "device.insert is registered");
    if (insert) {
        Payload p = baseOf(insert->fields);
        p["params"] = Payload::array({{{"param", "p"}, {"name", longText}}});
        check(names(issuesOf(*insert, p, Bounds::Remote), "params[0].name"),
              "remote: device.insert's params[0].name of 16,385 characters is refused");
    }
}

void testInternalKeys() {
    section("a '_' key is the op's own: refused from a remote caller, absent from its schema (P3.3)");
    const auto& reg = OpRegistry::instance();
    const auto* mode = reg.find("mixer.setPanMode");
    const auto* left = reg.find("mixer.setPanLeft");
    const auto* fades = reg.find("clip.setFades");
    check(mode && left && fades, "the split pan ops and clip.setFades are registered");
    if (!mode || !left || !fades) return;
    const Payload restore = {{"id", 1}, {"mode", "invalid_mode"}, {"_restore", true}};
    check(issuesOf(*mode, restore, Bounds::Local).empty(), "local: the inverse may still say _restore");
    check(names(issuesOf(*mode, restore, Bounds::Remote), "_restore"),
          "remote: setPanMode with _restore is refused, naming _restore");
    check(names(issuesOf(*mode, {{"id", 1}, {"mode", "stereo"}, {"_erase", true}}, Bounds::Remote), "_erase"),
          "remote: setPanMode with _erase is refused");
    check(names(issuesOf(*mode, {{"id", 1}, {"mode", "stereo"}, {"_erase", nullptr}}, Bounds::Remote), "_erase"),
          "remote: even a null _erase is refused, as the schema without it says");
    check(names(issuesOf(*left, {{"id", 1}, {"pan", 5.0}, {"_erase", true}}, Bounds::Remote), "_erase"),
          "remote: setPanLeft with _erase is refused");
    check(names(issuesOf(*fades, {{"id", 1}, {"_restore", Payload::object()}}, Bounds::Remote), "_restore"),
          "remote: clip.setFades' _restore is refused");
    check(issuesOf(*mode, {{"id", 1}, {"mode", "split"}}, Bounds::Remote).empty(), "remote: the ordinary payload passes");

    const Payload remote = opInputSchema(*mode, Bounds::Remote).at("properties");
    const Payload local = opInputSchema(*mode, Bounds::Local).at("properties");
    check(!remote.contains("_restore") && !remote.contains("_erase") && remote.contains("mode"),
          "the remote schema omits the '_' keys: " + remote.dump());
    check(local.contains("_restore") && local.contains("_erase"), "the local schema keeps them");
}

void testBoundsProfiles() {
    section("the remote bounds: MAGDA's limits at the boundary, never for local callers");
    const auto d = richOp();
    const std::string longText(kRemoteMaxTextLength + 1, 'x');
    Payload bigBag = Payload::array();
    for (std::size_t i = 0; i <= kRemoteMaxArrayItems; ++i) bigBag.push_back(static_cast<std::int64_t>(i));

    check(issuesOf(d, {{"mode", "a"}, {"note", longText}}, Bounds::Local).empty(),
          "local: an undeclared text has no length cap (an automation pass or a diff may be long)");
    check(names(issuesOf(d, {{"mode", "a"}, {"note", longText}}, Bounds::Remote), "note"),
          "remote: 16 KiB characters + 1 is refused, naming note");
    check(issuesOf(d, {{"mode", "a"}, {"bag", bigBag}}, Bounds::Local).empty(), "local: an undeclared array has no cap");
    check(names(issuesOf(d, {{"mode", "a"}, {"bag", bigBag}}, Bounds::Remote), "bag"),
          "remote: 4097 elements are refused, naming bag");

    const Payload remote = opInputSchema(d, Bounds::Remote);
    const Payload local = opInputSchema(d, Bounds::Local);
    check(remote.at("properties").at("note").value("maxLength", std::size_t{0}) == kRemoteMaxTextLength,
          "the remote schema says the text cap");
    check(!local.at("properties").at("note").contains("maxLength"), "the local schema does not");
    check(remote.at("properties").at("name").value("maxLength", std::size_t{0}) == 5,
          "a declared length wins over the default");
    check(remote.at("properties").at("bag").value("maxItems", std::size_t{0}) == kRemoteMaxArrayItems,
          "the remote schema says the array cap");
}

void testCatalogueMetadata() {
    section("the catalogue's first declared constraints copy the SQL CHECKs");
    const auto& reg = OpRegistry::instance();
    const auto* connect = reg.find("routing.connect");
    check(connect != nullptr, "routing.connect is registered");
    if (!connect) return;
    const Payload s = opInputSchema(*connect);
    const auto& kinds = s.at("properties").at("kind").at("enum");
    check(kinds.size() == 5 && kinds.at(2) == "sidechain", "routing.connect's kind lists main..cue: " + kinds.dump());
    check(!s.at("properties").at("kind").value("description", std::string{}).empty(), "and says what each is");
    Payload bad = {{"id", 9}, {"srcKind", "track"}, {"src", 1}, {"dstKind", "track"}, {"dst", 2}, {"kind", "bogus"}};
    check(names(validateSubmission(*connect, bad), "kind"), "a bogus kind is refused before the database sees it");
    Payload good = bad;
    good["kind"] = "sidechain";
    check(validateSubmission(*connect, good).empty(), "a sidechain row passes");

    const auto* create = reg.find("track.create");
    check(create && opInputSchema(*create).at("properties").at("kind").at("enum").size() == 14,
          "track.create's kind lists the 14 kinds tracks.kind allows");

    const Payload described = describeOp(*connect);
    check(described.value("name", "") == "routing.connect" && described.contains("inputSchema") &&
              described.value("undoable", false),
          "describeOp: name, schema, undoable");
    // The messages every existing caller sees did not change.
    const auto* vol = reg.find("mixer.setVolume");
    check(vol && validateSubmission(*vol, {{"db", 1.0}}).front().problem == "required field missing",
          "unchanged: required field missing");
    check(vol && validateSubmission(*vol, {{"id", "x"}, {"db", 1.0}}).front().problem == "expected int",
          "unchanged: expected int");
    check(vol && validateSubmission(*vol, {{"id", 1}, {"db", 1.0}, {"x", 1}}).front().problem ==
                     "unknown field for mixer.setVolume",
          "unchanged: unknown field for <op>");
}

int runAll() {
    testEveryOpIsClosedAndEquivalent();
    testRichMetadata();
    testBoundsProfiles();
    testOpenValues();
    testInternalKeys();
    testCatalogueMetadata();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        return runAll();
    } catch (const std::exception& e) {
        std::printf("FAILED -- exception: %s\n", e.what());
        return 1;
    }
}
