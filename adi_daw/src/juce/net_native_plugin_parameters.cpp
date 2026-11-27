// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/net_native_plugin_parameters.hpp"
#include "juce/native_plugin_wire.hpp"
#include "juce/clap_host.hpp"

#include <set>
#include <string_view>

namespace adi::net::native_plugin_wire {
namespace {

constexpr std::size_t parameterLimit = 4096;
constexpr std::size_t entryBytes = 52;
constexpr std::size_t nameLimit = 256;
constexpr std::size_t unitLimit = 64;
constexpr std::uint32_t automatable = 1;
constexpr std::uint32_t configurable = 2;
constexpr std::uint32_t realRange = 4;

bool validText(std::string_view text, std::size_t bound) noexcept {
    return text.size() <= bound && text.find('\0') == std::string_view::npos;
}

bool valid(const device::RemoteParameter& parameter) {
    const auto& descriptor = parameter.descriptor;
    const auto scale = descriptor.maxReal - descriptor.minReal;
    return parameter.selector != CLAP_INVALID_ID && validText(descriptor.name, nameLimit) &&
           validText(descriptor.unit, unitLimit) && normalized(descriptor.defaultValue.normalized) &&
           normalized(parameter.current.normalized) &&
           static_cast<unsigned>(descriptor.shape) <= static_cast<unsigned>(device::ParamShape::Menu) &&
           descriptor.stepCount >= 0 && descriptor.stepCount <= 4096 &&
           std::isfinite(descriptor.minReal) && std::isfinite(descriptor.maxReal) &&
           std::isfinite(scale) && scale >= 0;
}

void putDouble(std::span<std::byte> bytes, std::size_t offset, double value) noexcept {
    put(bytes, offset, std::bit_cast<std::uint64_t>(value));
}

double getDouble(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return std::bit_cast<double>(get<std::uint64_t>(bytes, offset));
}

device::ParamValue paramValue(double value, const device::ParamDescriptor& descriptor) noexcept {
    if (!descriptor.hasRealRange) return device::ParamValue::fromNormalized(value);
    return device::ParamValue::withReal(value, std::lerp(descriptor.minReal, descriptor.maxReal, value));
}

void appendText(std::vector<std::byte>& bytes, std::string_view text) {
    const auto data = std::as_bytes(std::span(text));
    bytes.insert(bytes.end(), data.begin(), data.end());
}

std::string readText(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

bool encodeParameters(std::span<const device::RemoteParameter> parameters, std::vector<std::byte>& bytes) {
    if (parameters.size() > parameterLimit) return false;
    std::set<std::uint32_t> identities;
    std::vector<std::byte> encoded(4);
    put(encoded, 0, static_cast<std::uint32_t>(parameters.size()));
    for (const auto& parameter : parameters) {
        if (!valid(parameter) || !identities.insert(parameter.selector).second) return false;
        const auto& descriptor = parameter.descriptor;
        const auto offset = encoded.size();
        encoded.resize(offset + entryBytes);
        const auto entry = std::span(encoded).subspan(offset);
        put(entry, 0, parameter.selector);
        put(entry, 4, (descriptor.automatable ? automatable : 0u) |
                      (descriptor.configurable ? configurable : 0u) |
                      (descriptor.hasRealRange ? realRange : 0u));
        put(entry, 8, static_cast<std::uint32_t>(descriptor.shape));
        put(entry, 12, static_cast<std::uint32_t>(descriptor.stepCount));
        putDouble(entry, 16, descriptor.defaultValue.normalized);
        putDouble(entry, 24, parameter.current.normalized);
        putDouble(entry, 32, descriptor.minReal);
        putDouble(entry, 40, descriptor.maxReal);
        put(entry, 48, static_cast<std::uint16_t>(descriptor.name.size()));
        put(entry, 50, static_cast<std::uint16_t>(descriptor.unit.size()));
        appendText(encoded, descriptor.name);
        appendText(encoded, descriptor.unit);
    }
    bytes = std::move(encoded);
    return true;
}

bool decodeParameters(std::span<const std::byte> bytes, std::vector<device::RemoteParameter>& parameters) {
    if (!validLength(Message::Parameters, bytes.size())) return false;
    const auto count = get<std::uint32_t>(bytes, 0);
    bytes = bytes.subspan(4);
    if (count > parameterLimit || count > bytes.size() / entryBytes) return false;
    std::set<std::uint32_t> identities;
    std::vector<device::RemoteParameter> decoded;
    decoded.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (bytes.size() < entryBytes) return false;
        const auto flags = get<std::uint32_t>(bytes, 4);
        const auto shape = get<std::uint32_t>(bytes, 8);
        const auto steps = get<std::uint32_t>(bytes, 12);
        const auto name = get<std::uint16_t>(bytes, 48);
        const auto unit = get<std::uint16_t>(bytes, 50);
        if ((flags & ~(automatable | configurable | realRange)) != 0 ||
            shape > static_cast<std::uint32_t>(device::ParamShape::Menu) || steps > 4096 ||
            name > nameLimit || unit > unitLimit || entryBytes + name + unit > bytes.size()) return false;
        device::RemoteParameter parameter;
        parameter.selector = get<std::uint32_t>(bytes, 0);
        auto& descriptor = parameter.descriptor;
        descriptor.automatable = (flags & automatable) != 0;
        descriptor.configurable = (flags & configurable) != 0;
        descriptor.hasRealRange = (flags & realRange) != 0;
        descriptor.domain = descriptor.hasRealRange ? device::ParamDomain::Real : device::ParamDomain::Normalized;
        descriptor.shape = static_cast<device::ParamShape>(shape);
        descriptor.stepCount = static_cast<std::int32_t>(steps);
        descriptor.minReal = getDouble(bytes, 32);
        descriptor.maxReal = getDouble(bytes, 40);
        descriptor.defaultValue = paramValue(getDouble(bytes, 16), descriptor);
        parameter.current = paramValue(getDouble(bytes, 24), descriptor);
        descriptor.id = device::ClapDevice::paramIdToText(parameter.selector);
        descriptor.name = readText(bytes.subspan(entryBytes, name));
        descriptor.unit = readText(bytes.subspan(entryBytes + name, unit));
        if (!valid(parameter) || !identities.insert(parameter.selector).second) return false;
        decoded.push_back(std::move(parameter));
        bytes = bytes.subspan(entryBytes + name + unit);
    }
    if (!bytes.empty()) return false;
    parameters = std::move(decoded);
    return true;
}

} // namespace adi::net::native_plugin_wire
