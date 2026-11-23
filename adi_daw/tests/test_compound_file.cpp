// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/compound_file.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

namespace {
using namespace adi::interchange::cfb;
constexpr std::uint32_t end = 0xfffffffe, freeSlot = 0xffffffff;
constexpr std::size_t sectorBytes = 512;
int checks = 0, failures = 0;

void check(bool pass, const char *message) {
    ++checks;
    if (!pass) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}

std::size_t sector(std::uint32_t sid) {
    return (static_cast<std::size_t>(sid) + 1) * sectorBytes;
}

std::uint32_t get32(const Bytes &bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(bytes.at(offset + i)) << (i * 8);
    return value;
}

std::size_t dirEntry(const Bytes &bytes, std::size_t id) {
    return sector(get32(bytes, 48)) + id * 128;
}

void streams() {
    constexpr ClassId clsid{0x0d, 0x01, 0x02, 0x01, 1};
    const Bytes shortData{0x90, 0x00, 0xf0, 0xff};
    Bytes largestMini(4095, 0x17), regular(4096, 0x53);
    largestMini.back() = 0xa0;
    regular.back() = 0xe1;
    const std::array entries{
        Entry{EntryKind::Storage, 0, "Header", {}, clsid},
        Entry{EntryKind::Stream, 1, "properties", shortData},
        Entry{EntryKind::Stream, 0, "Mini Cutoff", largestMini},
        Entry{EntryKind::Stream, 0, "Regular Cutoff", regular},
        Entry{EntryKind::Stream, 1, "empty", {}},
    };
    const auto written = write(clsid, entries);
    const auto repeated = write(clsid, entries);
    const auto *bytes = std::get_if<Bytes>(&written);
    const auto *again = std::get_if<Bytes>(&repeated);
    check(bytes && again && *bytes == *again, "compound image bytes are deterministic");
    if (!bytes)
        return;
    check(get32(*bytes, 0) == 0xe011cfd0 && get32(*bytes, 28) == 0x0009fffe &&
              get32(*bytes, 40) == 0 && get32(*bytes, 56) == 4096,
          "CFB v3 header has the signature, byte order, 512-byte sectors and 4096 cutoff");
    const auto root = dirEntry(*bytes, 0);
    const auto header = dirEntry(*bytes, 1);
    const auto small = dirEntry(*bytes, 2);
    const auto mini = dirEntry(*bytes, 3);
    const auto large = dirEntry(*bytes, 4);
    const auto empty = dirEntry(*bytes, 5);
    check(bytes->at(root + 66) == 5 && bytes->at(header + 66) == 1 &&
              bytes->at(small + 66) == 2 && get32(*bytes, empty + 116) == end &&
              get32(*bytes, empty + 120) == 0 &&
              std::ranges::equal(clsid, std::span(*bytes).subspan(root + 80, clsid.size())) &&
              std::ranges::equal(clsid, std::span(*bytes).subspan(header + 80, clsid.size())),
          "directory preserves storages and CLSIDs, zero-length streams and root type");
    const auto miniStart = sector(get32(*bytes, root + 116));
    const auto miniFat = sector(get32(*bytes, 60));
    check(get32(*bytes, root + 120) == 4160 && get32(*bytes, small + 116) == 0 &&
              get32(*bytes, mini + 116) == 1 && get32(*bytes, miniFat) == end &&
              get32(*bytes, miniFat + 4) == 2 && get32(*bytes, miniFat + 64 * 4) == end &&
              std::ranges::equal(shortData, std::span(*bytes).subspan(miniStart, shortData.size())) &&
              std::ranges::equal(largestMini, std::span(*bytes).subspan(miniStart + 64, largestMini.size())),
          "mini-sector allocation packs small streams and chains a cutoff-minus-one payload");
    const auto largeId = get32(*bytes, large + 116);
    const auto fat = sector(get32(*bytes, 76));
    check(largeId == 0 && get32(*bytes, fat) == 1 && get32(*bytes, fat + 7 * 4) == end &&
              std::ranges::equal(regular, std::span(*bytes).subspan(sector(largeId), regular.size())),
          "cutoff-size data is stored on a terminated regular FAT chain");
}

struct TreeHeight {
    unsigned blackDepth;
    bool red;
};

std::optional<TreeHeight> verifyTree(const Bytes &bytes, std::uint32_t id, unsigned depth) {
    if (id == freeSlot)
        return TreeHeight{0, false};
    if (depth > 10)
        return {};
    const auto offset = dirEntry(bytes, id);
    const bool red = bytes.at(offset + 67) == 0;
    const auto left = verifyTree(bytes, get32(bytes, offset + 68), depth + 1);
    const auto right = verifyTree(bytes, get32(bytes, offset + 72), depth + 1);
    if (!left || !right || (red && (left->red || right->red)) || left->blackDepth != right->blackDepth)
        return {};
    return TreeHeight{left->blackDepth + static_cast<unsigned>(!red), red};
}

void directoryTree() {
    std::array<std::string, 130> names;
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < names.size(); ++i) {
        names[i] = "unnamed" + std::to_string(i);
        entries.push_back(Entry{EntryKind::Stream, 0, names[i], {}});
    }
    bool balanced = true;
    for (std::size_t count = 0; count <= names.size(); ++count) {
        const auto document = write({}, std::span(entries).first(count));
        const auto *wire = std::get_if<Bytes>(&document);
        if (!wire) {
            balanced = false;
            break;
        }
        const auto tree = verifyTree(*wire, get32(*wire, dirEntry(*wire, 0) + 76), 0);
        balanced = balanced && tree && !tree->red;
    }
    check(balanced, "directory sibling trees preserve black height and black roots over incomplete levels");
}

void difat() {
    constexpr std::size_t payloadSize = 16 * 1024 * 1024 + 17;
    Bytes data(payloadSize, 0x84);
    const std::array entries{Entry{EntryKind::Stream, 0, "Essence", data}};
    const auto written = write({}, entries);
    const auto *bytes = std::get_if<Bytes>(&written);
    check(bytes != nullptr, "large payload crosses the 109 FAT references in the header");
    if (!bytes)
        return;
    const auto fatFirst = get32(*bytes, 76);
    const auto difatFirst = get32(*bytes, 68);
    const auto difatNext = get32(*bytes, sector(difatFirst) + 508);
    check(get32(*bytes, 44) > 236 && get32(*bytes, 72) == 2 &&
              get32(*bytes, sector(difatFirst)) == fatFirst + 109 &&
              get32(*bytes, sector(difatNext)) == fatFirst + 236 &&
              get32(*bytes, sector(difatNext) + 508) == end &&
              get32(*bytes, sector(fatFirst) + static_cast<std::size_t>(difatFirst) * 4) == 0xfffffffc,
          "extended DIFAT chains expose every FAT sector and mark reserved blocks");
    check(std::ranges::equal(data, std::span(*bytes).subspan(sector(0), payloadSize)),
          "extended FAT allocation leaves the full essence bytes intact");
}

void malformed() {
    const auto expect = [](std::span<const Entry> entries, Issue issue, std::size_t id) {
        const auto result = write({}, entries);
        const auto *error = std::get_if<Error>(&result);
        return error && error->issue == issue && error->entry == id;
    };
    const std::array duplicates{Entry{EntryKind::Storage, 0, "Header", {}},
                                Entry{EntryKind::Stream, 0, "HEADER", {}}};
    check(expect(duplicates, Issue::DuplicateName, 2), "sibling names cannot differ only in case");
    const std::array badName{Entry{EntryKind::Stream, 0, "A/B", {}}};
    check(expect(badName, Issue::InvalidName, 1), "reject reserved name delimiters");
    const std::array descendants{Entry{EntryKind::Stream, 0, "stream", {}},
                                 Entry{EntryKind::Stream, 1, "bad", {}}};
    check(expect(descendants, Issue::InvalidParent, 2), "a stream cannot own a child");
    const std::array selfParent{Entry{EntryKind::Storage, 1, "loop", {}}};
    check(expect(selfParent, Issue::InvalidParent, 1), "invalid parent ordering cannot form a cycle");
    const std::array streamClass{Entry{EntryKind::Stream, 0, "class", {}, {1}}};
    check(expect(streamClass, Issue::WrongEntryFields, 1), "stream directory entries cannot carry a storage class ID");
}
} // namespace

int main() {
    streams();
    directoryTree();
    difat();
    malformed();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
