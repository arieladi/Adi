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

Payload fieldSchema(const Field& f, Bounds bounds) {
    Payload s = Payload::object();
    if (f.required) s["type"] = jsonType(f.type);
    else s["type"] = Payload::array({jsonType(f.type), "null"});
    if (!f.description.empty()) s["description"] = std::string(f.description);

    switch (f.type) {
        case FieldType::Int:
            // The handler reads a std::int64_t; the schema says so.
            s["minimum"] = f.minimum ? boundValue(*f.minimum)
                                     : Payload(std::numeric_limits<std::int64_t>::min());
            s["maximum"] = f.maximum ? boundValue(*f.maximum)
                                     : Payload(std::numeric_limits<std::int64_t>::max());
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
            break;
        case FieldType::Object:
            if (!f.items.empty()) {
                const Payload inner = closedObject(f.items, bounds);
                s["properties"] = inner.at("properties");
                s["required"] = inner.at("required");
                s["additionalProperties"] = false;
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
            // [-2^63, 2^63): both ends are exact in a double.
            if (std::isfinite(d) && std::floor(d) == d && d >= -9223372036854775808.0 &&
                d < 9223372036854775808.0)
                v = static_cast<std::int64_t>(d);
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
