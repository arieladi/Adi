// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: base-62 lexicographic fractional sibling keys (Rocicorp / Figcraft
// algorithm, PLAN C3).

#include "adi/collab/fractional_key.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::collab {

namespace {

constexpr std::string_view kDigits =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr std::size_t kBase = 62U;
constexpr std::string_view kSmallestInteger = "A00000000000000000000000000";
constexpr std::string_view kZeroInteger = "a0";

[[nodiscard]] std::optional<std::size_t> digitIndex(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<std::size_t>(c - '0');
    }
    if (c >= 'A' && c <= 'Z') {
        return 10U + static_cast<std::size_t>(c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return 36U + static_cast<std::size_t>(c - 'a');
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t integerLength(char head) noexcept {
    if (head >= 'a' && head <= 'z') {
        return static_cast<std::size_t>(head - 'a') + 2U;
    }
    if (head >= 'A' && head <= 'Z') {
        return static_cast<std::size_t>('Z' - head) + 2U;
    }
    return 0U;
}

[[nodiscard]] std::optional<std::string> incrementInteger(std::string_view x) {
    const char head = x[0];
    std::string digits(x.substr(1U));
    bool carry = true;
    for (std::size_t idx = digits.size(); carry && idx > 0U; --idx) {
        const std::size_t pos = idx - 1U;
        const auto d = digitIndex(digits[pos]);
        if (!d.has_value()) {
            return std::nullopt;
        }
        const std::size_t next = *d + 1U;
        if (next == kBase) {
            digits[pos] = '0';
        } else {
            digits[pos] = kDigits[next];
            carry = false;
        }
    }
    if (!carry) {
        return std::string(1U, head) + digits;
    }
    if (head == 'Z') {
        return std::string(kZeroInteger);
    }
    if (head == 'z') {
        return std::nullopt;
    }
    const char nextHead = static_cast<char>(head + 1);
    if (nextHead > 'a') {
        digits.push_back('0');
    } else {
        digits.pop_back();
    }
    return std::string(1U, nextHead) + digits;
}

[[nodiscard]] std::optional<std::string> decrementInteger(std::string_view x) {
    const char head = x[0];
    std::string digits(x.substr(1U));
    bool borrow = true;
    for (std::size_t idx = digits.size(); borrow && idx > 0U; --idx) {
        const std::size_t pos = idx - 1U;
        const auto d = digitIndex(digits[pos]);
        if (!d.has_value()) {
            return std::nullopt;
        }
        if (*d == 0U) {
            digits[pos] = 'z';
        } else {
            digits[pos] = kDigits[*d - 1U];
            borrow = false;
        }
    }
    if (!borrow) {
        return std::string(1U, head) + digits;
    }
    if (head == 'a') {
        return std::string("Zz");
    }
    if (head == 'A') {
        return std::nullopt;
    }
    const char nextHead = static_cast<char>(head - 1);
    if (nextHead < 'Z') {
        digits.push_back('z');
    } else {
        digits.pop_back();
    }
    return std::string(1U, nextHead) + digits;
}

/// Recursive base-62 fractional midpoint between `a` and `b` (`a < b`).
[[nodiscard]] std::string midpoint(std::string_view a, std::optional<std::string_view> b) {
    if (b.has_value()) {
        std::size_t common = 0U;
        while (common < b->size()) {
            const char ca = common < a.size() ? a[common] : '0';
            if (ca != (*b)[common]) {
                break;
            }
            ++common;
        }
        if (common > 0U) {
            const std::string_view tailA = common < a.size() ? a.substr(common) : std::string_view{};
            return std::string(b->substr(0U, common)) + midpoint(tailA, b->substr(common));
        }
    }

    const std::size_t digitA = a.empty() ? 0U : *digitIndex(a[0]);
    const std::size_t digitB = b.has_value() ? *digitIndex((*b)[0]) : kBase;
    if (digitB - digitA > 1U) {
        const std::size_t midDigit = (digitA + digitB + 1U) / 2U;
        return std::string(1U, kDigits[midDigit]);
    }
    if (b.has_value() && b->size() > 1U) {
        return std::string(b->substr(0U, 1U));
    }
    const std::string_view tailA = a.size() > 1U ? a.substr(1U) : std::string_view{};
    return std::string(1U, kDigits[digitA]) + midpoint(tailA, std::nullopt);
}

[[nodiscard]] bool fillBalanced(std::optional<std::string_view> lower,
                                std::optional<std::string_view> upper,
                                std::size_t count,
                                std::vector<std::string>& out) {
    if (count == 0U) {
        return true;
    }
    if (count == 1U) {
        const auto k = keyBetween(lower, upper);
        if (!k.has_value()) {
            return false;
        }
        out.push_back(*k);
        return true;
    }
    if (!upper.has_value()) {
        auto cur = keyBetween(lower, std::nullopt);
        if (!cur.has_value()) {
            return false;
        }
        out.push_back(*cur);
        for (std::size_t i = 1U; i < count; ++i) {
            cur = keyBetween(out.back(), std::nullopt);
            if (!cur.has_value()) {
                return false;
            }
            out.push_back(*cur);
        }
        return true;
    }
    if (!lower.has_value()) {
        std::vector<std::string> rev;
        rev.reserve(count);
        auto cur = keyBetween(std::nullopt, upper);
        if (!cur.has_value()) {
            return false;
        }
        rev.push_back(*cur);
        for (std::size_t i = 1U; i < count; ++i) {
            cur = keyBetween(std::nullopt, rev.back());
            if (!cur.has_value()) {
                return false;
            }
            rev.push_back(*cur);
        }
        std::reverse(rev.begin(), rev.end());
        out.insert(out.end(), rev.begin(), rev.end());
        return true;
    }

    const std::size_t midCount = count / 2U;
    const auto mid = keyBetween(lower, upper);
    if (!mid.has_value()) {
        return false;
    }
    if (!fillBalanced(lower, std::string_view(*mid), midCount, out)) {
        return false;
    }
    out.push_back(*mid);
    return fillBalanced(std::string_view(*mid), upper, count - midCount - 1U, out);
}

}  // namespace

bool validateFractionalKey(std::string_view key) noexcept {
    if (key.empty() || key == kSmallestInteger) {
        return false;
    }
    const std::size_t intLen = integerLength(key[0]);
    if (intLen == 0U || key.size() < intLen) {
        return false;
    }
    for (std::size_t i = 1U; i < key.size(); ++i) {
        if (!digitIndex(key[i]).has_value()) {
            return false;
        }
    }
    // Canonical fractional suffix never ends with '0'.
    if (key.size() > intLen && key.back() == '0') {
        return false;
    }
    return true;
}

std::optional<std::string> keyBetween(std::optional<std::string_view> lower,
                                      std::optional<std::string_view> upper) {
    if (lower.has_value() && !validateFractionalKey(*lower)) {
        return std::nullopt;
    }
    if (upper.has_value() && !validateFractionalKey(*upper)) {
        return std::nullopt;
    }
    if (lower.has_value() && upper.has_value() && *lower >= *upper) {
        return std::nullopt;  // Refuse swapped or equal bounds (PLAN C3).
    }

    if (!lower.has_value() && !upper.has_value()) {
        return std::string(kZeroInteger);
    }

    if (!lower.has_value()) {
        const std::size_t lenB = integerLength((*upper)[0]);
        const std::string_view intB = upper->substr(0U, lenB);
        const std::string_view fracB = upper->substr(lenB);
        if (intB == kSmallestInteger) {
            return std::string(intB) + midpoint("", fracB);
        }
        if (intB < *upper) {
            return std::string(intB);
        }
        const auto dec = decrementInteger(intB);
        if (!dec.has_value()) {
            return std::nullopt;
        }
        if (*dec == kSmallestInteger) {
            return *dec + midpoint("", std::nullopt);
        }
        return *dec;
    }

    if (!upper.has_value()) {
        const std::size_t lenA = integerLength((*lower)[0]);
        const std::string_view intA = lower->substr(0U, lenA);
        const std::string_view fracA = lower->substr(lenA);
        const auto inc = incrementInteger(intA);
        if (inc.has_value()) {
            return *inc;
        }
        return std::string(intA) + midpoint(fracA, std::nullopt);
    }

    const std::size_t lenA = integerLength((*lower)[0]);
    const std::size_t lenB = integerLength((*upper)[0]);
    const std::string_view intA = lower->substr(0U, lenA);
    const std::string_view fracA = lower->substr(lenA);
    const std::string_view intB = upper->substr(0U, lenB);
    const std::string_view fracB = upper->substr(lenB);

    if (intA == intB) {
        return std::string(intA) + midpoint(fracA, fracB);
    }
    const auto incA = incrementInteger(intA);
    if (!incA.has_value()) {
        return std::nullopt;
    }
    if (*incA < *upper) {
        return *incA;
    }
    return std::string(intA) + midpoint(fracA, std::nullopt);
}

std::optional<std::vector<std::string>> nKeysBetween(std::optional<std::string_view> lower,
                                                     std::optional<std::string_view> upper,
                                                     std::size_t count) {
    if (lower.has_value() && !validateFractionalKey(*lower)) {
        return std::nullopt;
    }
    if (upper.has_value() && !validateFractionalKey(*upper)) {
        return std::nullopt;
    }
    if (lower.has_value() && upper.has_value() && *lower >= *upper) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    out.reserve(count);
    if (!fillBalanced(lower, upper, count, out)) {
        return std::nullopt;
    }
    return out;
}

std::vector<std::string> respaceKeys(std::size_t count) {
    auto res = nKeysBetween(std::nullopt, std::nullopt, count);
    return res.has_value() ? std::move(*res) : std::vector<std::string>{};
}

std::vector<std::string> reuseKeysForOrder(std::span<const std::string> currentKeys) {
    const std::size_t n = currentKeys.size();
    if (n == 0U) {
        return {};
    }

    // Collect indices carrying valid existing keys and compute their longest
    // strictly increasing subsequence (LIS) so untouched items keep their keys.
    std::vector<std::size_t> validIdx;
    validIdx.reserve(n);
    for (std::size_t i = 0U; i < n; ++i) {
        if (validateFractionalKey(currentKeys[i])) {
            validIdx.push_back(i);
        }
    }

    std::vector<bool> keep(n, false);
    if (!validIdx.empty()) {
        const std::size_t m = validIdx.size();
        std::vector<std::size_t> tails;       // indices into validIdx
        std::vector<std::size_t> prev(m, m);  // predecessor link
        tails.reserve(m);

        for (std::size_t k = 0U; k < m; ++k) {
            const std::string& key = currentKeys[validIdx[k]];
            const auto it = std::lower_bound(
                tails.begin(), tails.end(), key,
                [&](std::size_t tailPos, const std::string& target) {
                    return currentKeys[validIdx[tailPos]] < target;
                });
            const auto pos = static_cast<std::size_t>(it - tails.begin());
            if (pos > 0U) {
                prev[k] = tails[pos - 1U];
            }
            if (it == tails.end()) {
                tails.push_back(k);
            } else {
                *it = k;
            }
        }

        for (std::size_t cur = tails.back(); cur < m; cur = prev[cur]) {
            keep[validIdx[cur]] = true;
            if (prev[cur] == m) {
                break;
            }
        }
    }

    std::vector<std::string> result(n);
    for (std::size_t i = 0U; i < n; ++i) {
        if (keep[i]) {
            result[i] = currentKeys[i];
        }
    }

    // Fill each contiguous gap of non-kept indices using balanced nKeysBetween.
    std::size_t i = 0U;
    while (i < n) {
        if (keep[i]) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && !keep[j]) {
            ++j;
        }
        const std::optional<std::string_view> lower =
            i > 0U ? std::optional<std::string_view>(result[i - 1U]) : std::nullopt;
        const std::optional<std::string_view> upper =
            j < n ? std::optional<std::string_view>(result[j]) : std::nullopt;
        const auto gap = nKeysBetween(lower, upper, j - i);
        if (!gap.has_value()) {
            return respaceKeys(n);
        }
        for (std::size_t k = 0U; k < gap->size(); ++k) {
            result[i + k] = (*gap)[k];
        }
        i = j;
    }

    return result;
}

}  // namespace adi::collab
