// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace adi::interchange::cfb {

using Bytes = std::vector<std::uint8_t>;
using ClassId = std::array<std::uint8_t, 16>; // Already in CFB wire order.

enum class EntryKind { Storage, Stream };
struct Entry {
    EntryKind kind;
    std::size_t parent; // Zero for root. Other IDs are one-based entry indices.
    std::string_view name;
    std::span<const std::uint8_t> data;
    ClassId classId{};
};

enum class Issue { InvalidName, InvalidParent, DuplicateName, WrongEntryFields, FileTooLarge };
struct Error {
    Issue issue;
    std::size_t entry; // Zero denotes the compound document as a whole.
};

/// Build a deterministic Microsoft Compound File Binary v3 document.
/// Parents precede their children. Names use the CFB ASCII subset (the AAF
/// storage and property names use this range). Payloads are copied once into
/// the output. The caller owns file I/O and keeps source buffers alive here.
[[nodiscard]] std::variant<Bytes, Error> write(ClassId root, std::span<const Entry> entries);

} // namespace adi::interchange::cfb
