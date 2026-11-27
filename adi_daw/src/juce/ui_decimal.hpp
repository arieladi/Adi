// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#ifdef ADI_UI_FLOAT_CHARCONV
#include <array>
#include <charconv>
#include <system_error>
#else
#include <ios>
#include <limits>
#include <locale>
#include <sstream>
#endif

namespace adi::ui::decimal {

// Read a complete finite decimal. The editor handles trimming, units and a leading '+'.
inline std::optional<double> parse(std::string_view text) {
    if (text.empty() || text.front() == '+')
        return std::nullopt;
    double value = 0;
#ifdef ADI_UI_FLOAT_CHARCONV
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value))
        return std::nullopt;
#else
    // Formatted extraction can also accept hexadecimal floats.
    if (text.find_first_not_of("-+eE.0123456789") != std::string_view::npos)
        return std::nullopt;
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    if (!input.eof() || input.bad() || !std::isfinite(value))
        return std::nullopt;
    // Some libraries set failbit for a representable subnormal conversion.
    if (input.fail() && (value == 0 || std::abs(value) > std::numeric_limits<double>::min()))
        return std::nullopt;
    // Formatted extraction can round a nonzero out-of-range value to zero.
    if (value == 0 && text.substr(0, text.find_first_of("eE")).find_first_of("123456789") != std::string_view::npos)
        return std::nullopt;
#endif
    return value;
}

inline std::string format(double value) {
#ifdef ADI_UI_FLOAT_CHARCONV
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
    return converted.ec == std::errc{} ? std::string(buffer.data(), converted.ptr) : std::string{};
#else
    std::ostringstream output;
    output.imbue(std::locale::classic());
    const auto magnitude = std::abs(value);
    const bool scientific = magnitude != 0 && (magnitude < 1e-4 || magnitude >= 1e6);
    if (scientific)
        output.setf(std::ios_base::scientific, std::ios_base::floatfield);
    // General formatting switches notation outside this range. Round-trip
    // at the fewest significant digits without displaying binary noise.
    const int last = std::numeric_limits<double>::max_digits10 - (scientific ? 1 : 0);
    for (int precision = scientific ? 0 : 6;; ++precision) {
        output.str(std::string{});
        output.precision(precision);
        output << value;
        auto text = output.str();
        if (precision == last || parse(text) == value)
            return text;
    }
#endif
}

} // namespace adi::ui::decimal
