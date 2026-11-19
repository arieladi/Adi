// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ops_schema.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace adi {
namespace {

const char* jsonType(FieldType t) {
    switch (t) {
        case FieldType::Int: return "integer";
        case FieldType::Real: return "number";
        case FieldType::Text: return "string";
        case FieldType::Bool: return "boolean";
        case FieldType::Array: return "array";
        case FieldType::Object: return "object";
    }
    return "null";
}

/// A declared bound as JSON: an integer when it is one, so `minimum: 0` reads
/// as it was meant on an integer field.
Payload boundValue(double v) {
    if (std::isfinite(v) && std::floor(v) == v && v >= -9.0e15 && v <= 9.0e15)
        return static_cast<std::int64_t>(v);
    return v;
}

Payload closedObject(std::span<const Field> fields, Bounds bounds);

Payload openValueRef() { return {{"$ref", "#/$defs/openValue"}}; }

/// What checkOpen enforces inside an open value (an Object with no declared
/// members, or an element of an Array with no declared items), as one
/// recursive definition: text, member names included, at most
/// kRemoteMaxTextLength characters; arrays and objects at most
/// kRemoteMaxArrayItems members; every member an open value again.
Payload openValueDef() {
    return {{"anyOf", Payload::array({
                          {{"type", Payload::array({"null", "boolean", "number"})}},
                          {{"type", "string"}, {"maxLength", kRemoteMaxTextLength}},
                          {{"type", "array"}, {"maxItems", kRemoteMaxArrayItems}, {"items", openValueRef()}},
                          {{"type", "object"},
                           {"maxProperties", kRemoteMaxArrayItems},
                           {"propertyNames", {{"maxLength", kRemoteMaxTextLength}}},
                           {"additionalProperties", openValueRef()}},
                      })}};
}

/// True when a field the remote schema shows, at any depth, is open.
bool hasOpenField(std::span<const Field> fields) {
    for (const auto& f : fields) {
        if (f.key.starts_with('_')) continue;
        if (f.type != FieldType::Array && f.type != FieldType::Object) continue;
        if (f.items.empty() || hasOpenField(f.items)) return true;
    }
    return false;
}

Payload fieldSchema(const Field& f, Bounds bounds) {
    Payload s = Payload::object();
    if (f.required) s["type"] = jsonType(f.type);
    else s["type"] = Payload::array({jsonType(f.type), "null"});
    if (!f.description.empty()) s["description"] = std::string(f.description);

    switch (f.type) {
        case FieldType::Int:
            // What an Int can hold: a signed 64-bit integer, or, for a note's
            // persisted id alone (fullUint64), an unsigned one too. The
            // schema says exactly what checkValue enforces.
            s["minimum"] = f.minimum ? boundValue(*f.minimum)
                                     : Payload(std::numeric_limits<std::int64_t>::min());
            if (f.maximum) s["maximum"] = boundValue(*f.maximum);
            else if (f.fullUint64) s["maximum"] = std::numeric_limits<std::uint64_t>::max();
            else s["maximum"] = std::numeric_limits<std::int64_t>::max();
            break;
        case FieldType::Real:
            if (f.minimum) s["minimum"] = boundValue(*f.minimum);
            if (f.maximum) s["maximum"] = boundValue(*f.maximum);
            break;
        case FieldType::Text: {
            if (!f.enumValues.empty()) {
                Payload values = Payload::array();
                for (const auto e : f.enumValues) values.push_back(std::string(e));
                // `enum` constrains every value, null included: an optional
                // field that may be null must list it.
                if (!f.required) values.push_back(nullptr);
                s["enum"] = std::move(values);
            }
            if (f.maxLength) s["maxLength"] = *f.maxLength;
            else if (bounds == Bounds::Remote) s["maxLength"] = kRemoteMaxTextLength;
            break;
        }
        case FieldType::Array:
            if (f.maxItems) s["maxItems"] = *f.maxItems;
            else if (bounds == Bounds::Remote) s["maxItems"] = kRemoteMaxArrayItems;
            if (!f.items.empty()) s["items"] = closedObject(f.items, bounds);
            else if (bounds == Bounds::Remote) s["items"] = openValueRef();
            break;
        case FieldType::Object:
            if (!f.items.empty()) {
                const Payload inner = closedObject(f.items, bounds);
                s["properties"] = inner.at("properties");
                s["required"] = inner.at("required");
                s["additionalProperties"] = false;
            } else if (bounds == Bounds::Remote) {
                // checkOpen on the field's own object: its member count and
                // member names are capped like any open object's, and every
                // member is an open value.
                s["maxProperties"] = kRemoteMaxArrayItems;
                s["propertyNames"] = {{"maxLength", kRemoteMaxTextLength}};
                s["additionalProperties"] = openValueRef();
            }
            break;
        case FieldType::Bool:
            break;
    }
    return s;
}

Payload closedObject(std::span<const Field> fields, Bounds bounds) {
    Payload properties = Payload::object();
    Payload required = Payload::array();
    for (const auto& f : fields) {
        // A '_' key is the op's own (its inverse's), never a caller's: the
        // remote boundary refuses it, so the remote schema does not show it.
        if (bounds == Bounds::Remote && f.key.starts_with('_')) continue;
        properties[std::string(f.key)] = fieldSchema(f, bounds);
        if (f.required) required.push_back(std::string(f.key));
    }
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

void normalizeFields(std::span<const Field> fields, Payload& p) {
    if (!p.is_object()) return;
    for (const auto& f : fields) {
        const auto it = p.find(std::string(f.key));
        if (it == p.end()) continue;
        Payload& v = *it;
        if (f.type == FieldType::Int && v.is_number_float()) {
            const double d = v.get<double>();
            // [-2^63, 2^63) as signed, [2^63, 2^64) as unsigned: every bound
            // is exact in a double, and so is every whole double inside them.
            if (std::isfinite(d) && std::floor(d) == d) {
                if (d >= -9223372036854775808.0 && d < 9223372036854775808.0)
                    v = static_cast<std::int64_t>(d);
                else if (d >= 9223372036854775808.0 && d < 18446744073709551616.0)
                    v = static_cast<std::uint64_t>(d);
            }
        } else if (f.type == FieldType::Array && v.is_array() && !f.items.empty()) {
            for (auto& element : v) normalizeFields(f.items, element);
        } else if (f.type == FieldType::Object && v.is_object() && !f.items.empty()) {
            normalizeFields(f.items, v);
        }
    }
}

}  // namespace

Payload opInputSchema(const OpDescriptor& d, Bounds bounds) {
    Payload s = closedObject(d.fields, bounds);
    if (!d.summary.empty()) s["description"] = std::string(d.summary);
    // The remote bounds reach inside an open value; the definition its
    // fields refer to sits at the root, and only where one is used.
    if (bounds == Bounds::Remote && hasOpenField(d.fields)) s["$defs"] = {{"openValue", openValueDef()}};
    return s;
}

void normalizeSubmission(const OpDescriptor& d, Payload& p) { normalizeFields(d.fields, p); }

Payload describeOp(const OpDescriptor& d, Bounds bounds) {
    Payload o = Payload::object();
    o["name"] = std::string(d.name);
    o["summary"] = std::string(d.summary);
    o["scope"] = std::string(toString(d.scope));
    o["engineImpact"] = std::string(toString(d.engineImpact));
    o["undoable"] = !d.ephemeral;
    o["coalescable"] = d.coalescable;
    o["inverse"] = d.inverseOp.empty() ? std::string("symmetric") : std::string(d.inverseOp);
    o["inputSchema"] = opInputSchema(d, bounds);
    return o;
}

}  // namespace adi
