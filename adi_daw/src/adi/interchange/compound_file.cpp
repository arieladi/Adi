// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/compound_file.hpp"

#include <algorithm>
#include <bit>
#include <iterator>

namespace adi::interchange::cfb {
namespace {
using SectorId = std::uint32_t;
using DirectoryId = std::uint32_t;
constexpr SectorId endChain = 0xfffffffe, fatSector = 0xfffffffd, difatSector = 0xfffffffc;
constexpr DirectoryId noStream = 0xffffffff;
constexpr std::size_t sectorSize = 512, miniSize = 64, miniCutoff = 4096, directorySize = 128;
constexpr std::size_t sectorSlots = sectorSize / 4, headerDifatSlots = 109, difatSlots = sectorSlots - 1;
constexpr std::size_t sectorLimit = 0x7fffffffU / sectorSize - 1; // Excludes the header sector.
constexpr std::array<std::uint8_t, 8> signature{0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};

constexpr std::size_t blocks(std::size_t size, std::size_t unit) {
    return size / unit + (size % unit != 0);
}

constexpr std::size_t sectorOffset(SectorId id) {
    return (static_cast<std::size_t>(id) + 1) * sectorSize;
}

void put(Bytes &file, std::size_t offset, std::uint32_t value, unsigned length) {
    for (unsigned i = 0; i < length; ++i)
        file[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void copyAt(Bytes &file, std::size_t offset, std::span<const std::uint8_t> source) {
    std::ranges::copy(source, file.begin() + static_cast<std::ptrdiff_t>(offset));
}

bool validName(std::string_view name) {
    return !name.empty() && name.size() <= 31 && std::ranges::all_of(name, [](char ch) {
        return ch > 0 && static_cast<unsigned char>(ch) <= 0x7f &&
               ch != '/' && ch != '\\' && ch != ':' && ch != '!';
    });
}

char upper(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

bool nameLess(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return a.size() < b.size();
    return std::ranges::lexicographical_compare(a, b, {}, upper, upper);
}

struct DirectoryEntry {
    std::string_view name;
    std::uint8_t type = 0; // 1 storage, 2 stream, 5 root.
    bool red = false;
    DirectoryId left = noStream, right = noStream, child = noStream;
    SectorId start = 0;
    std::size_t size = 0;
    bool mini = false;
    ClassId classId{};
    std::vector<DirectoryId> children;
};

// Median recursion creates a tree with leaves on at most two levels.
// Coloring the deepest non-root entries red equalizes every black height.
DirectoryId makeTree(std::span<const DirectoryId> sorted, std::vector<DirectoryEntry> &directory,
                     std::size_t depth, std::size_t deepest) {
    if (sorted.empty())
        return noStream;
    const auto middle = sorted.size() / 2;
    const auto root = sorted[middle];
    auto &entry = directory[root];
    entry.red = depth != 0 && depth == deepest;
    entry.left = makeTree(sorted.first(middle), directory, depth + 1, deepest);
    entry.right = makeTree(sorted.subspan(middle + 1), directory, depth + 1, deepest);
    return root;
}

struct Region {
    SectorId first = endChain;
    std::size_t count = 0;
};

Region reserve(std::size_t &cursor, std::size_t count) {
    if (count == 0)
        return {};
    const auto first = static_cast<SectorId>(cursor);
    cursor += count;
    return {first, count};
}

void mark(Bytes &file, std::size_t table, SectorId index, SectorId next) {
    put(file, table + static_cast<std::size_t>(index) * 4, next, 4);
}

void link(Bytes &file, std::size_t table, Region region) {
    for (std::size_t i = 0; i < region.count; ++i) {
        const auto current = region.first + static_cast<SectorId>(i);
        mark(file, table, current, i + 1 == region.count ? endChain : current + 1);
    }
}

void writeHeader(Bytes &file, Region directory, Region miniFat, Region fat, Region difat) {
    copyAt(file, 0, signature);
    put(file, 24, 0x3e, 2);
    put(file, 26, 3, 2);
    put(file, 28, 0xfffe, 2);
    put(file, 30, 9, 2);
    put(file, 32, 6, 2);
    put(file, 44, static_cast<std::uint32_t>(fat.count), 4);
    put(file, 48, directory.first, 4);
    put(file, 56, static_cast<std::uint32_t>(miniCutoff), 4);
    put(file, 60, miniFat.first, 4);
    put(file, 64, static_cast<std::uint32_t>(miniFat.count), 4);
    put(file, 68, difat.first, 4);
    put(file, 72, static_cast<std::uint32_t>(difat.count), 4);
    for (std::size_t i = 0; i < headerDifatSlots; ++i) {
        const auto location = i < fat.count ? fat.first + static_cast<SectorId>(i) : noStream;
        put(file, 76 + i * 4, location, 4);
    }
}

void writeDifat(Bytes &file, Region difat, Region fat) {
    std::size_t index = headerDifatSlots;
    for (std::size_t i = 0; i < difat.count; ++i) {
        const auto id = difat.first + static_cast<SectorId>(i);
        const auto offset = sectorOffset(id);
        for (std::size_t j = 0; j < difatSlots; ++j, ++index) {
            const auto location = index < fat.count ? fat.first + static_cast<SectorId>(index) : noStream;
            put(file, offset + j * 4, location, 4);
        }
        put(file, offset + difatSlots * 4, i + 1 == difat.count ? endChain : id + 1, 4);
    }
}

void writeDirectory(Bytes &file, Region region, std::span<const DirectoryEntry> directory) {
    const auto base = sectorOffset(region.first);
    for (std::size_t i = 0; i < region.count * sectorSize / directorySize; ++i) {
        const auto offset = base + i * directorySize;
        if (i >= directory.size()) {
            put(file, offset + 68, noStream, 4);
            put(file, offset + 72, noStream, 4);
            put(file, offset + 76, noStream, 4);
            continue;
        }
        const auto &entry = directory[i];
        for (std::size_t j = 0; j < entry.name.size(); ++j)
            put(file, offset + j * 2, static_cast<std::uint8_t>(entry.name[j]), 2);
        put(file, offset + 64, static_cast<std::uint32_t>((entry.name.size() + 1) * 2), 2);
        file[offset + 66] = entry.type;
        file[offset + 67] = entry.red ? 0 : 1;
        put(file, offset + 68, entry.left, 4);
        put(file, offset + 72, entry.right, 4);
        put(file, offset + 76, entry.child, 4);
        copyAt(file, offset + 80, entry.classId);
        put(file, offset + 116, entry.start, 4);
        put(file, offset + 120, static_cast<std::uint32_t>(entry.size), 4);
    }
}
} // namespace

std::variant<Bytes, Error> write(ClassId root, std::span<const Entry> entries) {
    if (entries.size() >= sectorLimit * sectorSize / directorySize)
        return Error{Issue::FileTooLarge, 0};
    std::vector<DirectoryEntry> directory(entries.size() + 1);
    directory.front().name = "Root Entry";
    directory.front().type = 5;
    directory.front().classId = root;
    std::size_t miniCount = 0, regularCount = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto id = i + 1;
        const auto &source = entries[i];
        if (!validName(source.name))
            return Error{Issue::InvalidName, id};
        if (source.parent >= id || directory[source.parent].type == 2)
            return Error{Issue::InvalidParent, id};
        auto &entry = directory[id];
        entry.name = source.name;
        entry.classId = source.classId;
        if (source.kind == EntryKind::Storage) {
            if (!source.data.empty())
                return Error{Issue::WrongEntryFields, id};
            entry.type = 1;
        } else {
            if (source.classId != ClassId{})
                return Error{Issue::WrongEntryFields, id};
            entry.type = 2;
            entry.size = source.data.size();
            if (entry.size > sectorLimit * sectorSize)
                return Error{Issue::FileTooLarge, id};
            entry.mini = entry.size < miniCutoff;
            if (entry.mini) {
                entry.start = reserve(miniCount, blocks(entry.size, miniSize)).first;
            } else {
                entry.start = reserve(regularCount, blocks(entry.size, sectorSize)).first;
            }
            if (regularCount + blocks(miniCount, sectorSize / miniSize) > sectorLimit)
                return Error{Issue::FileTooLarge, id};
        }
        directory[source.parent].children.push_back(static_cast<DirectoryId>(id));
    }
    for (auto &entry : directory) {
        auto &children = entry.children;
        const auto comparator = [&](DirectoryId a, DirectoryId b) {
            return nameLess(directory[a].name, directory[b].name);
        };
        std::ranges::sort(children, comparator);
        const auto dup = std::ranges::adjacent_find(children, [&](DirectoryId a, DirectoryId b) {
            return !comparator(a, b);
        });
        if (dup != children.end())
            return Error{Issue::DuplicateName, std::max(*dup, *std::next(dup))};
        if (!children.empty()) {
            const auto deepest = static_cast<std::size_t>(std::bit_width(children.size()) - 1);
            entry.child = makeTree(children, directory, 0, deepest);
        }
    }

    auto next = regularCount;
    directory.front().size = miniCount * miniSize;
    const auto miniStream = reserve(next, blocks(directory.front().size, sectorSize));
    directory.front().start = miniStream.first;
    const auto directoryStream = reserve(next, blocks(directory.size(), sectorSize / directorySize));
    const auto miniFat = reserve(next, blocks(miniCount, sectorSlots));
    auto fatCount = blocks(next, sectorSlots);
    std::size_t difatCount = 0;
    // FAT entries cover their own sectors and the DIFAT sectors too.
    for (;;) {
        difatCount = fatCount > headerDifatSlots ? blocks(fatCount - headerDifatSlots, difatSlots) : 0;
        const auto required = blocks(next + fatCount + difatCount, sectorSlots);
        if (required == fatCount)
            break;
        fatCount = required;
    }
    const auto fat = reserve(next, fatCount);
    const auto difat = reserve(next, difatCount);
    if (next > sectorLimit)
        return Error{Issue::FileTooLarge, 0};

    Bytes file((next + 1) * sectorSize, 0);
    writeHeader(file, directoryStream, miniFat, fat, difat);
    writeDifat(file, difat, fat);
    writeDirectory(file, directoryStream, directory);
    const auto fatOffset = sectorOffset(fat.first);
    std::fill_n(file.begin() + static_cast<std::ptrdiff_t>(fatOffset), fat.count * sectorSize, 0xff);
    std::size_t miniFatOffset = 0;
    if (miniFat.count != 0) {
        miniFatOffset = sectorOffset(miniFat.first);
        std::fill_n(file.begin() + static_cast<std::ptrdiff_t>(miniFatOffset), miniFat.count * sectorSize, 0xff);
    }
    link(file, fatOffset, miniStream);
    link(file, fatOffset, directoryStream);
    link(file, fatOffset, miniFat);
    for (std::size_t i = 0; i < fat.count; ++i)
        mark(file, fatOffset, fat.first + static_cast<SectorId>(i), fatSector);
    for (std::size_t i = 0; i < difat.count; ++i)
        mark(file, fatOffset, difat.first + static_cast<SectorId>(i), difatSector);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto &entry = directory[i + 1];
        const auto data = entries[i].data;
        if (data.empty())
            continue;
        if (entry.mini) {
            copyAt(file, sectorOffset(miniStream.first) + static_cast<std::size_t>(entry.start) * miniSize, data);
            link(file, miniFatOffset, Region{entry.start, blocks(entry.size, miniSize)});
        } else {
            copyAt(file, sectorOffset(entry.start), data);
            link(file, fatOffset, Region{entry.start, blocks(entry.size, sectorSize)});
        }
    }
    return file;
}

} // namespace adi::interchange::cfb
