// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <climits>
#include <string>

namespace adi::scan::detail {
// Exact native file-clock ticks, not a common epoch or duration unit. Some
// libraries use a signed 128-bit rep without a std::to_string overload (or
// standard-library integer traits). Work in that rep, including its minimum:
// only a remainder in [-9,9] is ever negated or narrowed. The binary-width
// buffer is a conservative decimal bound, at most 129 chars for a 128-bit rep.
template<class Integer> std::string tickDecimal(Integer value) {
    static_assert(sizeof(Integer)*CHAR_BIT<=128);
    static_assert(Integer{-1}<Integer{0},"file-clock count must be signed");
    static_assert(requires(Integer n){n%Integer{10};},"file-clock count must be integral");
    std::array<char,sizeof(Integer)*CHAR_BIT+1> buffer{};
    auto* end=buffer.data()+buffer.size();auto* first=end;
    const bool negative=value<Integer{0};
    do {
        const auto remainder=value%Integer{10};
        const auto digit=static_cast<unsigned>(remainder<Integer{0}?-remainder:remainder);
        *--first=static_cast<char>('0'+digit);
        value/=Integer{10};
    }while(value!=Integer{0});
    if(negative)*--first='-';
    return {first,end};
}
} // namespace adi::scan::detail
