// SPDX-License-Identifier: AGPL-3.0-or-later
//
// WIN-06 (docs/PLAN.md; WI6): fzy's optimal fuzzy matcher, ported to UTF-32 and JUCE-free, for the
// command palette. The scoring, its constants and the backtrace are fzy's (src/match.c, src/bonus.h,
// src/config.def.h), so its tests hold with their exact scores. A lower-case table of ADI's own
// (ASCII, Latin-1, Greek, Cyrillic) stands in for tolower and setlocale; Hebrew, which has no case,
// matches as it is.
//
// fzy: Copyright (c) 2014 John Hawthorn. Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to use, copy, modify,
// merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to
// whom the Software is furnished to do so, subject to the following conditions: The above copyright
// notice and this permission notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
// OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui::fuzzy {
using Score = double;
inline constexpr Score kGapLeading = -0.005, kGapTrailing = -0.005, kGapInner = -0.01;
inline constexpr Score kMatchConsecutive = 1.0, kMatchSlash = 0.9, kMatchWord = 0.8, kMatchCapital = 0.7,
                       kMatchDot = 0.6;
inline constexpr Score kMax = std::numeric_limits<Score>::infinity(), kMin = -std::numeric_limits<Score>::infinity();
inline constexpr std::size_t kMaxLength = 1024;

// Lower case, for matching: ASCII, Latin-1, Greek and Cyrillic; anything else as it is.
constexpr char32_t lower(char32_t c) noexcept {
    if ((c >= U'A' && c <= U'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return c + 0x20;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2)
        return c + 0x20; // Greek
    if (c >= 0x410 && c <= 0x42F)
        return c + 0x20; // Cyrillic
    if (c >= 0x400 && c <= 0x40F)
        return c + 0x50;
    return c;
}
// fzy's bonus index: 2 for a capital, 1 for a lower-case letter or a digit (and a letter with no case,
// so a Hebrew word's start counts), 0 for anything else.
constexpr int kind(char32_t c) noexcept {
    if (lower(c) != c)
        return 2;
    if ((c >= U'a' && c <= U'z') || (c >= U'0' && c <= U'9'))
        return 1;
    if (c >= 0xC0 && c != 0xD7 && c != 0xF7)
        return 1;
    return 0;
}
// fzy's bonus states: what the character before a match gives it.
constexpr Score bonus(char32_t last, char32_t c) noexcept {
    const int k = kind(c);
    if (k == 0)
        return 0;
    switch (last) {
    case U'/':
        return kMatchSlash;
    case U'-':
    case U'_':
    case U' ':
        return kMatchWord;
    case U'.':
        return kMatchDot;
    default:
        break;
    }
    return k == 2 && kind(last) == 1 && lower(last) == last && !(last >= U'0' && last <= U'9') ? kMatchCapital : 0;
}
// Every needle character in order in the haystack, ignoring case.
inline bool hasMatch(std::u32string_view needle, std::u32string_view haystack) noexcept {
    std::size_t at = 0;
    for (const auto n : needle) {
        const auto ln = lower(n);
        while (at < haystack.size() && lower(haystack[at]) != ln)
            ++at;
        if (at == haystack.size())
            return false;
        ++at;
    }
    return true;
}
// The score of a haystack that matches; with `positions`, the haystack index of each needle character
// along the best path (the latest of equal paths, as fzy picks).
inline Score match(std::u32string_view needle, std::u32string_view haystack, std::vector<std::size_t> *positions = nullptr) {
    if (needle.empty())
        return kMin;
    const std::size_t n = needle.size(), m = haystack.size();
    if (m > kMaxLength || n > m)
        return kMin;
    if (n == m) {
        if (positions != nullptr) {
            positions->resize(n);
            for (std::size_t i = 0; i < n; ++i)
                (*positions)[i] = i;
        }
        return kMax;
    }
    std::u32string ln(n, 0), lh(m, 0);
    for (std::size_t i = 0; i < n; ++i)
        ln[i] = lower(needle[i]);
    for (std::size_t j = 0; j < m; ++j)
        lh[j] = lower(haystack[j]);
    std::vector<Score> matchBonus(m);
    char32_t last = U'/';
    for (std::size_t j = 0; j < m; ++j) {
        matchBonus[j] = bonus(last, haystack[j]);
        last = haystack[j];
    }
    // D: the best score ending with a match here; M: the best score here. One row per needle character
    // (all kept when the positions are wanted, for the backtrace).
    std::vector<Score> D(n * m), M(n * m);
    for (std::size_t i = 0; i < n; ++i) {
        Score prevScore = kMin;
        const Score gap = i == n - 1 ? kGapTrailing : kGapInner;
        for (std::size_t j = 0; j < m; ++j) {
            Score &d = D[i * m + j];
            Score &here = M[i * m + j];
            if (ln[i] == lh[j]) {
                Score score = kMin;
                if (i == 0)
                    score = static_cast<Score>(j) * kGapLeading + matchBonus[j];
                else if (j > 0)
                    score = std::max(M[(i - 1) * m + j - 1] + matchBonus[j], D[(i - 1) * m + j - 1] + kMatchConsecutive);
                d = score;
                here = prevScore = std::max(score, prevScore + gap);
            } else {
                d = kMin;
                here = prevScore = prevScore + gap;
            }
        }
    }
    if (positions != nullptr) {
        positions->assign(n, 0);
        bool required = false;
        std::ptrdiff_t j = static_cast<std::ptrdiff_t>(m) - 1;
        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(n) - 1; i >= 0; --i)
            for (; j >= 0; --j) {
                const auto at = static_cast<std::size_t>(i) * m + static_cast<std::size_t>(j);
                if (D[at] != kMin && (required || D[at] == M[at])) {
                    required = i > 0 && j > 0 &&
                               M[at] == D[(static_cast<std::size_t>(i) - 1) * m + static_cast<std::size_t>(j) - 1] + kMatchConsecutive;
                    (*positions)[static_cast<std::size_t>(i)] = static_cast<std::size_t>(j--);
                    break;
                }
            }
    }
    return M[(n - 1) * m + m - 1];
}
// UTF-8 to UTF-32, an invalid byte read as itself.
inline std::u32string utf32(std::string_view s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const int more = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
        char32_t c = more == 0 ? b : static_cast<char32_t>(b & (0x3F >> more));
        std::size_t used = 1;
        for (int k = 0; k < more && i + used < s.size(); ++k, ++used) {
            const auto cont = static_cast<unsigned char>(s[i + used]);
            if ((cont & 0xC0) != 0x80)
                break;
            c = (c << 6) | (cont & 0x3F);
        }
        if (used != static_cast<std::size_t>(more) + 1) {
            c = b;
            used = 1;
        }
        out.push_back(c);
        i += used;
    }
    return out;
}
} // namespace adi::ui::fuzzy
