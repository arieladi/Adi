// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/blob.hpp"
#include "adi/digest.hpp"
#include "adi/note_overlap.hpp"
#include "adi/note_overlap_detail.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace adi;
using notes::Diff;
using notes::Record;
using notes::OverlapAction;
using notes::OverlapRelation;
using notes::OverlapStatus;
namespace nd = adi::notes::detail;
using Bytes = std::vector<std::byte>;
using Records = std::map<std::uint64_t, Record>;
constexpr auto maximumId = std::numeric_limits<std::uint64_t>::max();
constexpr auto wideId = (std::uint64_t{1} << 63) + 17;
int checks = 0, failures = 0;

void check(bool value, const std::string& name) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", name.c_str()); }
}
void require(bool value, const std::string& name) {
    if (!value) throw std::runtime_error(name);
}

// Fixture construction and expected bytes deliberately avoid the production
// record encoder and overlap resolver. NoteRecord's packed layout is SPEC's.
Record record(std::uint64_t id, std::int64_t start, std::int64_t duration,
              std::uint8_t key = 60, std::size_t stride = 40) {
    require(stride >= sizeof(NoteRecord), "fixture stride");
    NoteRecord value{};
    value.note_id = id; value.start_ticks = start; value.dur_ticks = duration;
    value.key = key; value.vel_on = 87; value.vel_off = 43; value.channel = 3;
    value.probability = 10000; value.flags = 0x8007; value.tuning_cents = 17.25f;
    Record out; out.bytes.resize(stride);
    std::memcpy(out.bytes.data(), &value, sizeof value);
    for (std::size_t i = sizeof value; i < stride; ++i)
        out.bytes[i] = static_cast<std::byte>((id + i * 29u) & 255u);
    return out;
}
NoteRecord prefix(const Record& value) {
    require(value.bytes.size() >= sizeof(NoteRecord), "oracle prefix bounds");
    NoteRecord out{}; std::memcpy(&out, value.bytes.data(), sizeof out); return out;
}
Record alter(Record value, const std::function<void(NoteRecord&)>& change) {
    auto decoded = prefix(value); change(decoded);
    std::memcpy(value.bytes.data(), &decoded, sizeof decoded); return value;
}
nd::Stream stream(std::vector<Record> records, bool sorted = true,
                  std::uint16_t stride = 40, std::uint16_t version = 1,
                  Bytes trailer = {}) {
    require(records.size() <= std::numeric_limits<std::uint32_t>::max(), "fixture count");
    StreamHeader header{static_cast<std::uint32_t>(FourCC::Notes), version, stride,
                        static_cast<std::uint32_t>(records.size()),
                        sorted ? StreamFlags::SortedByTime : 0u};
    nd::Stream out;
    std::memcpy(out.header.data(), &header, sizeof header);
    out.records = std::move(records); out.trailer = std::move(trailer); return out;
}
Bytes encoded(const nd::Stream& value) {
    StreamHeader header{}; std::memcpy(&header, value.header.data(), sizeof header);
    require(value.records.size() <= std::numeric_limits<std::uint32_t>::max(), "oracle count");
    header.count = static_cast<std::uint32_t>(value.records.size());
    Bytes out(sizeof header + value.records.size() * header.rec_size + value.trailer.size());
    std::memcpy(out.data(), &header, sizeof header);
    for (std::size_t i = 0; i < value.records.size(); ++i) {
        const auto& item = value.records[i];
        require(item.bytes.size() == header.rec_size, "oracle stride matches header");
        std::memcpy(out.data() + sizeof header + i * header.rec_size,
                    item.bytes.data(), item.bytes.size());
    }
    if (!value.trailer.empty())
        std::memcpy(out.data() + sizeof header + value.records.size() * header.rec_size,
                    value.trailer.data(), value.trailer.size());
    return out;
}
std::optional<std::vector<Record>> readEncoded(const Bytes& bytes) {
    if (bytes.size() < sizeof(StreamHeader)) return std::nullopt;
    StreamHeader header{}; std::memcpy(&header, bytes.data(), sizeof header);
    if (header.fourcc != static_cast<std::uint32_t>(FourCC::Notes) ||
        header.rec_size < sizeof(NoteRecord) ||
        header.count > (bytes.size() - sizeof header) / header.rec_size) return std::nullopt;
    std::vector<Record> records;
    records.reserve(header.count);
    for (std::uint32_t i = 0; i < header.count; ++i) {
        const auto offset = sizeof header + static_cast<std::size_t>(i) * header.rec_size;
        Record item; item.bytes.resize(header.rec_size);
        std::memcpy(item.bytes.data(), bytes.data() + offset, header.rec_size);
        records.push_back(std::move(item));
    }
    return records;
}
Records byId(const std::vector<Record>& records) {
    Records result;
    for (const auto& item : records)
        require(result.emplace(prefix(item).note_id, item).second, "oracle unique identity");
    return result;
}
void checkRecords(const nd::OverlapPlan& plan, const Records& expected, const std::string& label) {
    const auto actual = readEncoded(plan.stream.bytes);
    check(actual.has_value(), label + ": valid complete encoded records");
    if (!actual) return;
    Records actualMap;
    bool unique = true;
    for (const auto& item : *actual) unique = actualMap.emplace(prefix(item).note_id, item).second && unique;
    check(unique && actualMap == expected, label + ": independent complete record map");
}
void sortTime(std::vector<Record>& records) {
    std::stable_sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
        return prefix(a).start_ticks < prefix(b).start_ticks;
    });
}
bool empty(const Diff& value) {
    return value.added.empty() && value.removed.empty() && value.changed.empty();
}
void rejected(const nd::OverlapPlan& plan, OverlapStatus status, const std::string& name) {
    check(!plan.ok() && plan.status == status && !plan.error.empty(), name + ": named refusal");
    check(empty(plan.intent) && !plan.stream.ok && !plan.stream.changed &&
              plan.stream.bytes.empty() && empty(plan.stream.effects) &&
              plan.collisions.empty() && plan.sideEffects.empty(), name + ": no partial result");
    if (status != OverlapStatus::IncomingOverlap)
        check(!plan.conflict, name + ": no unrelated conflict pair");
}
Payload binary(const Bytes& bytes) {
    std::vector<std::uint8_t> out; out.reserve(bytes.size());
    for (const auto value : bytes) out.push_back(std::to_integer<std::uint8_t>(value));
    return Payload::binary(std::move(out));
}
notes::Snapshot snapshot(const nd::Stream& baseline) {
    nd::CapturedNotes data;
    data.clipRow = {{"id", 1}, {"kind", "midi"}};
    data.streamRow = {{"id", -7}, {"clip_id", 1}, {"stream_kind", "notes"},
                      {"channel", nullptr}, {"controller", nullptr},
                      {"data", binary(encoded(baseline))}};
    data.sourceFile = "pure-note-overlap-fixture.adi";
    return nd::makeSnapshot(std::move(data));
}

void relationTable() {
    struct Case {
        std::int64_t begin, end;
        OverlapRelation relation;
        std::int64_t remaining;
    };
    const std::array cases{
        Case{0, 5, OverlapRelation::Disjoint, 10},
        Case{0, 10, OverlapRelation::Disjoint, 10},
        Case{20, 30, OverlapRelation::Disjoint, 10},
        Case{25, 30, OverlapRelation::Disjoint, 10},
        Case{10, 20, OverlapRelation::Equal, 0},
        Case{5, 25, OverlapRelation::IncomingCoversVictim, 0},
        Case{5, 20, OverlapRelation::IncomingCoversVictim, 0},
        Case{10, 25, OverlapRelation::IncomingCoversVictim, 0},
        Case{12, 18, OverlapRelation::IncomingInsideVictim, 2},
        Case{12, 20, OverlapRelation::IncomingStartsInside, 2},
        Case{12, 25, OverlapRelation::IncomingStartsInside, 2},
        Case{5, 12, OverlapRelation::IncomingEndsInside, 0},
        Case{10, 12, OverlapRelation::IncomingEndsInside, 0}
    };
    const std::array offsets{std::int64_t{0}, -(std::int64_t{1} << 54) + 3,
        (std::int64_t{1} << 53) + 1, std::numeric_limits<std::int64_t>::min() + 32,
        std::numeric_limits<std::int64_t>::max() - 64};
    for (const auto offset : offsets) {
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto& c = cases[i];
            const auto label = "literal relation " + std::to_string(i) + " at " + std::to_string(offset);
            std::string error;
            const auto relation = nd::classifyOverlap({offset + 10, offset + 20},
                                                       {offset + c.begin, offset + c.end}, error);
            check(relation && *relation == c.relation, label + ": independent classifier table");
            const auto victim = record(wideId, offset + 10, 10);
            const auto incoming = record(maximumId, offset + c.begin, c.end - c.begin);
            const auto baseline = stream({victim});
            const auto plan = nd::resolveOverlap(baseline, Diff{{incoming}, {}, {}});
            check(plan.ok(), label + ": prepare succeeds: " + plan.error);
            if (!plan.ok()) continue;
            std::vector<Record> expected;
            if (c.remaining)
                expected.push_back(alter(victim, [&](NoteRecord& n) { n.dur_ticks = c.remaining; }));
            expected.push_back(incoming); sortTime(expected);
            check(plan.stream.bytes == encoded(stream(expected)), label + ": exact final bytes and no right fragment");
            const bool collides = c.relation != OverlapRelation::Disjoint;
            check(plan.collisions.size() == (collides ? 1u : 0u) &&
                      plan.sideEffects.size() == (collides ? 1u : 0u), label + ": complete preview cardinality");
            if (plan.collisions.size() == 1) {
                const auto& collision = plan.collisions.front();
                check(collision.victim == wideId && collision.incoming == maximumId && collision.key == 60 &&
                          collision.relation == c.relation &&
                          collision.victimSpan == notes::OverlapSpan{offset + 10, offset + 20} &&
                          collision.incomingSpan == notes::OverlapSpan{offset + c.begin, offset + c.end},
                      label + ": exact identities, geometry and classification");
            }
            if (plan.sideEffects.size() == 1) {
                const auto& effect = plan.sideEffects.front();
                const auto wanted = c.remaining ? std::optional<Record>{expected.front()} : std::nullopt;
                check(effect.victim == wideId && effect.before == victim && effect.after == wanted &&
                          effect.action == (c.remaining ? OverlapAction::ShortenEnd : OverlapAction::Remove) &&
                          effect.causes == std::vector<std::uint64_t>{maximumId},
                      label + ": complete induced effect and retained identity");
            }
        }
    }
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const std::array invalid{notes::OverlapSpan{0, 0}, notes::OverlapSpan{2, 1},
                            notes::OverlapSpan{minimum, 0}, notes::OverlapSpan{minimum, maximum}};
    for (const auto bad : invalid) {
        std::string error;
        check(!nd::classifyOverlap(bad, {10, 11}, error) && !error.empty(),
              "invalid victim span refuses before a disjoint shortcut");
        error.clear();
        check(!nd::classifyOverlap({10, 11}, bad, error) && !error.empty(),
              "invalid incoming span refuses before a disjoint shortcut");
    }
    std::string error;
    check(nd::classifyOverlap({minimum, -1}, {maximum - 1, maximum}, error) == OverlapRelation::Disjoint,
          "maximum representable duration and extreme one-tick span stay exact");
}

void identityAndComposition() {
    const auto a = record(1, 0, 10, 60, 48);
    const auto b = record(2, 10, 10, 60, 48);
    const auto c = record(3, 20, 10, 60, 48);
    const Bytes trailer{std::byte{0x00}, std::byte{0xfa}, std::byte{0x71}};
    const auto baseline = stream({a, b, c}, true, 48, 9, trailer);
    const auto movedA = alter(a, [](NoteRecord& n) { n.start_ticks = 10; });
    const auto movedB = alter(b, [](NoteRecord& n) { n.start_ticks = 20; });
    const auto simultaneous = nd::resolveOverlap(baseline, Diff{{}, {}, {{b, movedB}, {a, movedA}}});
    check(simultaneous.ok(), "all selected old identities are excluded before simultaneous placement");
    if (simultaneous.ok()) {
        check(simultaneous.stream.bytes == encoded(stream({movedA, movedB}, true, 48, 9, trailer)),
              "selected source under another destination survives while the independent victim is removed");
        check(simultaneous.sideEffects.size() == 1 && simultaneous.sideEffects.front().victim == 3,
              "only the unselected destination victim is an induced effect");
    }
    const auto swappedA = alter(a, [](NoteRecord& n) { n.start_ticks = 10; });
    const auto swappedB = alter(b, [](NoteRecord& n) { n.start_ticks = 0; });
    const auto swap = nd::resolveOverlap(baseline, Diff{{}, {}, {{a, swappedA}, {b, swappedB}}});
    check(swap.ok() && swap.sideEffects.empty(), "group swap has no victim among selected originals");
    if (swap.ok()) check(swap.stream.bytes == encoded(stream({swappedB, swappedA, c}, true, 48, 9, trailer)),
                         "group swap retains exact IDs, flags, tails, header and trailer");
    const auto reversedA = alter(a, [](NoteRecord& n) { n.start_ticks = 50; n.dur_ticks = 3; });
    const auto reversedB = alter(b, [](NoteRecord& n) { n.start_ticks = -9; n.dur_ticks = 7; });
    const auto reverse = nd::resolveOverlap(baseline, Diff{{}, {}, {{a, reversedA}, {b, reversedB}}});
    check(reverse.ok() && reverse.sideEffects.empty(), "positive final spans may reverse selected order");
    if (reverse.ok()) check(reverse.stream.bytes == encoded(stream({reversedB, c, reversedA}, true, 48, 9, trailer)),
                            "reversed order preserves identities without negative durations");
    const auto copy = record(wideId, 0, 10, 60, 48);
    const auto copied = nd::resolveOverlap(baseline, Diff{{copy}, {}, {}});
    check(copied.ok(), "copy with fresh identity is admitted");
    if (copied.ok()) {
        checkRecords(copied, byId({copy, b, c}), "copy may remove its original as an ordinary victim");
        check(copied.sideEffects.size() == 1 && copied.sideEffects.front().victim == 1 &&
                  copied.sideEffects.front().action == OverlapAction::Remove,
              "copy does not exempt its original identity from collision");
    }
    const auto longVictim = record(4, 0, 100, 60, 48);
    const auto first = record(7, 0, 1, 60, 48), later = record(8, 50, 1, 60, 48);
    const auto removed = nd::resolveOverlap(stream({longVictim}, true, 48), Diff{{later, first}, {}, {}});
    check(removed.ok() && removed.collisions.size() == 2 && removed.sideEffects.size() == 1,
          "removal still reports every later original-victim collision");
    if (removed.sideEffects.size() == 1)
        check(removed.sideEffects.front().causes == std::vector<std::uint64_t>{7},
              "removal has one determining cause despite later diagnostics");
    const auto velocity = alter(longVictim, [](NoteRecord& n) { n.vel_on = 73; n.channel = 11; n.tuning_cents = -21; });
    const auto middle = record(9, 5, 2, 60, 48);
    const auto combined = nd::resolveOverlap(stream({longVictim}, true, 48),
                                            Diff{{middle}, {}, {{longVictim, velocity}}});
    const auto shortened = alter(velocity, [](NoteRecord& n) { n.dur_ticks = 5; });
    check(combined.ok(), "non-geometric requested fields combine with an induced shortening");
    if (combined.ok()) {
        checkRecords(combined, byId({shortened, middle}), "combined final requested and induced fields");
        check(combined.sideEffects.size() == 1 && combined.sideEffects.front().before == longVictim &&
                  combined.sideEffects.front().after == std::optional<Record>{shortened},
              "side-effect preview is original to complete final record");
    }
    const auto superseded = nd::resolveOverlap(stream({longVictim}, true, 48),
                                               Diff{{first}, {}, {{longVictim, velocity}}});
    check(superseded.ok() && superseded.intent.changed.size() == 1 &&
              superseded.stream.effects.changed.empty() && superseded.stream.effects.removed.size() == 1,
          "valid non-geometric requested edit can be superseded by a removal");
}

void keyAndHistoryScope() {
    const auto a = alter(record(1, 0, 20, 60, 48), [](NoteRecord& n) {
        n.channel = 0; n.tuning_cents = 1199.5f; n.flags = 0xf005;
    });
    const auto b = alter(record(2, 5, 10, 60, 48), [](NoteRecord& n) { n.channel = 7; n.flags = 0x8011; });
    const auto incoming = alter(record(3, 10, 2, 60, 48), [](NoteRecord& n) {
        n.channel = 15; n.tuning_cents = -1199.5f; n.flags = 0x9001;
    });
    const auto plan = nd::resolveOverlap(stream({a, b}, true, 48), Diff{{incoming}, {}, {}});
    check(plan.ok(), "same-key collision includes different channels, cents and inactive notes");
    if (plan.ok()) {
        const auto shortA = alter(a, [](NoteRecord& n) { n.dur_ticks = 10; });
        const auto shortB = alter(b, [](NoteRecord& n) { n.dur_ticks = 5; });
        checkRecords(plan, byId({shortA, shortB, incoming}), "key-only collision preserves every other field and tail");
        check(plan.collisions.size() == 2 && plan.sideEffects.size() == 2,
              "both overlapping historical victims are independently shortened");
    }
    const auto otherKey = alter(incoming, [](NoteRecord& n) { n.key = 61; });
    const auto unrelated = nd::resolveOverlap(stream({a, b}, true, 48), Diff{{otherKey}, {}, {}});
    check(unrelated.ok() && unrelated.sideEffects.empty() && unrelated.collisions.empty(),
          "another key does not normalize existing overlapping notes");
    if (unrelated.ok()) checkRecords(unrelated, byId({a, b, otherKey}), "unrelated historical overlap survives exactly");
    const auto changed = alter(a, [](NoteRecord& n) {
        n.channel = 14; n.tuning_cents = -23; n.flags ^= NoteFlags::Muted; n.vel_on = 66;
    });
    const auto nonGeometric = nd::resolveOverlap(stream({a, b}, true, 48), Diff{{}, {}, {{a, changed}}});
    check(nonGeometric.ok() && nonGeometric.collisions.empty() && nonGeometric.sideEffects.empty(),
          "channel, cents, flags and velocity changes alone are not placement drivers");
    if (nonGeometric.ok()) checkRecords(nonGeometric, byId({changed, b}), "non-geometric edit leaves existing overlap intact");
    const auto removed = nd::resolveOverlap(stream({a, b}, true, 48), Diff{{}, {a}, {}});
    check(removed.ok() && removed.sideEffects.empty() && removed.collisions.empty(),
          "explicit removal introduces no driver or induced side effect");
}

void canonicalOrder() {
    const auto a = record(wideId, 10, 5, 60, 48);
    const auto b = record(2, 0, 5, 61, 48);
    const auto c = record(maximumId, 10, 5, 62, 48);
    const auto ca = alter(a, [](NoteRecord& n) { n.vel_on = 77; });
    const auto cc = alter(c, [](NoteRecord& n) { n.vel_on = 78; });
    const auto low = record(1, 10, 5, 63, 48), high = record(3, 10, 5, 64, 48);
    const Bytes trailer{std::byte{0xfe}, std::byte{0}, std::byte{0x61}};
    const auto baseline = stream({a, b, c}, false, 48, 7, trailer);
    const Diff intent{{high, low}, {b}, {{c, cc}, {a, ca}}};
    std::string error;
    const auto ordered = nd::orderOverlapDiff(baseline, intent, error);
    check(ordered && *ordered == Diff{{low, high}, {b}, {{a, ca}, {c, cc}}},
          "canonical arrays use baseline ordinal and unsigned added identity");
    const auto plan = nd::resolveOverlap(baseline, intent);
    check(plan.ok(), "unsorted future stream accepts canonical diff");
    if (plan.ok()) check(plan.stream.bytes == encoded(stream({ca, cc, low, high}, false, 48, 7, trailer)),
                         "unsorted future stream retains baseline order and appends canonical additions");
    const auto sorted = nd::resolveOverlap(stream({b, a, c}, true, 48, 7, trailer), intent);
    check(sorted.ok(), "sorted future stream accepts canonical diff");
    if (sorted.ok()) check(sorted.stream.bytes == encoded(stream({ca, cc, low, high}, true, 48, 7, trailer)),
                           "equal-time originals precede ID-canonical additions without pitch sorting");
    const auto small = record(5, 0, 1, 1), wide = record(wideId, 0, 1, 2), maximum = record(maximumId, 0, 1, 3);
    const auto added = nd::resolveOverlap(stream({}), Diff{{maximum, small, wide}, {}, {}});
    check(added.ok() && added.stream.effects.added == std::vector<Record>{small, wide, maximum},
          "added IDs sort across the entire unsigned 64-bit domain");
    if (added.ok()) check(added.stream.bytes == encoded(stream({small, wide, maximum})),
                          "coincident additions have deterministic full-width identity order");
    error.clear();
    const auto noOpOrder = nd::orderOverlapDiff(baseline, Diff{{}, {}, {{c, c}, {a, a}}}, error);
    check(noOpOrder && noOpOrder->changed == std::vector<notes::Change>{{a, a}, {c, c}},
          "order-only helper retains outer byte-identical changes for later full comparison");
    const auto noOpPlan = nd::resolveOverlap(baseline, Diff{{}, {}, {{c, c}, {a, a}}});
    check(noOpPlan.ok() && !noOpPlan.stream.changed && empty(noOpPlan.intent) && empty(noOpPlan.stream.effects),
          "normalized inner intent omits byte-identical changes");
}

struct Random {
    std::uint64_t state = 0x5e3ca71bf08429d6ULL;
    std::uint64_t next() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
};
struct CellOracle {
    Records finalRecords;
    Diff effects;
    std::vector<notes::OverlapEffect> sideEffects;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
};

CellOracle occupiedPrefix(const nd::Stream& baseline, const Diff& intent) {
    // Independent discrete oracle. Positive note spans occupy integer cells.
    // The survivor is the initial run of unoccupied cells, not an interval
    // classifier or a call through any production planner.
    constexpr std::int64_t floor = -32, ceiling = 96;
    std::array<std::array<std::uint64_t, 128>, 3> cells{};
    const auto originals = byId(baseline.records);
    std::map<std::uint64_t, Record> changed, drivers;
    std::set<std::uint64_t> removed;
    for (const auto& item : intent.removed) removed.insert(prefix(item).note_id);
    for (const auto& pair : intent.changed) {
        const auto old = prefix(pair.oldRecord), fresh = prefix(pair.newRecord);
        changed.emplace(old.note_id, pair.newRecord);
        if (old.start_ticks != fresh.start_ticks || old.dur_ticks != fresh.dur_ticks || old.key != fresh.key)
            drivers.emplace(old.note_id, pair.newRecord);
    }
    for (const auto& item : intent.added) drivers.emplace(prefix(item).note_id, item);
    for (const auto& [id, item] : drivers) {
        const auto value = prefix(item);
        require(value.key >= 60 && value.key <= 62 && value.start_ticks >= floor &&
                    value.start_ticks + value.dur_ticks <= ceiling, "bounded oracle driver");
        const auto key = static_cast<std::size_t>(value.key - 60);
        for (auto tick = value.start_ticks; tick < value.start_ticks + value.dur_ticks; ++tick) {
            const auto index = static_cast<std::size_t>(tick - floor);
            require(index < cells[key].size() && cells[key][index] == 0, "disjoint oracle driver cells");
            cells[key][index] = id;
        }
    }
    CellOracle out;
    for (const auto& original : baseline.records) {
        const auto value = prefix(original);
        if (removed.contains(value.note_id)) continue;
        if (drivers.contains(value.note_id)) {
            out.finalRecords.emplace(value.note_id, drivers.at(value.note_id)); continue;
        }
        require(value.key >= 60 && value.key <= 62 && value.start_ticks >= floor &&
                    value.start_ticks + value.dur_ticks <= ceiling, "bounded oracle victim");
        const auto key = static_cast<std::size_t>(value.key - 60);
        auto fresh = changed.contains(value.note_id) ? changed.at(value.note_id) : original;
        std::set<std::uint64_t> encountered;
        std::int64_t freeCells = 0;
        std::uint64_t first = 0;
        for (std::int64_t cell = 0; cell < value.dur_ticks; ++cell) {
            const auto index = static_cast<std::size_t>(value.start_ticks + cell - floor);
            require(index < cells[key].size(), "oracle victim cell bounds");
            const auto incoming = cells[key][index];
            if (incoming) encountered.insert(incoming);
            if (!first) {
                if (incoming) first = incoming;
                else ++freeCells;
            }
        }
        std::vector<std::uint64_t> pairIds(encountered.begin(), encountered.end());
        std::sort(pairIds.begin(), pairIds.end(), [&](std::uint64_t left, std::uint64_t right) {
            const auto a = prefix(drivers.at(left)), b = prefix(drivers.at(right));
            return std::tuple{a.start_ticks, a.start_ticks + a.dur_ticks, left} <
                   std::tuple{b.start_ticks, b.start_ticks + b.dur_ticks, right};
        });
        for (const auto id : pairIds) out.pairs.emplace_back(value.note_id, id);
        if (freeCells == 0) {
            out.sideEffects.push_back({value.note_id, OverlapAction::Remove, {first}, original, std::nullopt});
            continue;
        }
        if (freeCells < value.dur_ticks) {
            fresh = alter(fresh, [&](NoteRecord& n) { n.dur_ticks = freeCells; });
            out.sideEffects.push_back({value.note_id, OverlapAction::ShortenEnd, {first}, original, fresh});
        }
        out.finalRecords.emplace(value.note_id, std::move(fresh));
    }
    for (const auto& item : intent.added) out.finalRecords.emplace(prefix(item).note_id, item);
    for (const auto& original : baseline.records) {
        const auto id = prefix(original).note_id;
        const auto found = out.finalRecords.find(id);
        if (found == out.finalRecords.end()) out.effects.removed.push_back(original);
        else if (found->second != original) out.effects.changed.push_back({original, found->second});
    }
    for (const auto& [id, item] : out.finalRecords) {
        if (!originals.contains(id)) out.effects.added.push_back(item);
    }
    return out;
}

void seededCellOracle() {
    Random random;
    for (unsigned trial = 0; trial < 128; ++trial) {
        const std::uint16_t stride = trial % 3 == 0 ? std::uint16_t{48} : std::uint16_t{40};
        std::vector<Record> baselineRecords;
        for (std::uint64_t id = 1; id <= 18; ++id) {
            const auto start = id <= 3 ? 70 + static_cast<std::int64_t>(id) :
                static_cast<std::int64_t>(random.next() % 64u) - 16;
            const auto duration = id <= 3 ? 3 : static_cast<std::int64_t>(random.next() % 20u) + 1;
            const auto key = static_cast<std::uint8_t>(60u + random.next() % 3u);
            baselineRecords.push_back(record(id, start, duration, key, stride));
        }
        baselineRecords.push_back(record(19, -25, 100, 60, stride));
        const bool sorted = trial % 2 == 0;
        if (sorted) sortTime(baselineRecords);
        const auto baseline = stream(baselineRecords, sorted, stride,
                                     stride == 48 ? std::uint16_t{9} : std::uint16_t{1},
                                     {std::byte{0xf8}, std::byte{0}, std::byte{0x73}});
        const auto originals = byId(baselineRecords);
        Diff intent;
        for (unsigned driver = 0; driver < 6; ++driver) {
            const auto start = static_cast<std::int64_t>((driver / 3) * 40u) - 20 +
                               static_cast<std::int64_t>(random.next() % 10u);
            const auto duration = static_cast<std::int64_t>(random.next() % 8u) + 1;
            const auto key = static_cast<std::uint8_t>(60u + driver % 3);
            if (driver < 3) {
                const auto old = originals.at(static_cast<std::uint64_t>(driver) + 1);
                intent.changed.push_back({old, alter(old, [&](NoteRecord& n) {
                    n.start_ticks = start; n.dur_ticks = duration; n.key = key;
                    n.channel = 15; n.tuning_cents = -35.5f;
                })});
            } else intent.added.push_back(record(100u + driver, start, duration, key, stride));
        }
        intent.removed.push_back(originals.at(4));
        const auto nonGeometric = originals.at(5);
        intent.changed.push_back({nonGeometric, alter(nonGeometric, [](NoteRecord& n) {
            n.vel_on = 51; n.channel = 12; n.flags ^= NoteFlags::Muted;
        })});
        const auto expected = occupiedPrefix(baseline, intent);
        const auto plan = nd::resolveOverlap(baseline, intent);
        const auto label = "cell oracle trial " + std::to_string(trial);
        check(plan.ok(), label + ": succeeds: " + plan.error);
        if (!plan.ok()) continue;
        checkRecords(plan, expected.finalRecords, label);
        check(plan.stream.effects == expected.effects, label + ": complete canonical effective diff");
        check(plan.sideEffects == expected.sideEffects, label + ": full effects and determining causes");
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
        bool geometry = true;
        Records drivers;
        for (const auto& item : intent.added) drivers.emplace(prefix(item).note_id, item);
        for (const auto& change : intent.changed) {
            const auto old = prefix(change.oldRecord), fresh = prefix(change.newRecord);
            if (old.start_ticks != fresh.start_ticks || old.dur_ticks != fresh.dur_ticks || old.key != fresh.key)
                drivers.emplace(old.note_id, change.newRecord);
        }
        for (const auto& collision : plan.collisions) {
            pairs.emplace_back(collision.victim, collision.incoming);
            if (!originals.contains(collision.victim) || !drivers.contains(collision.incoming)) {
                geometry = false; continue;
            }
            const auto victim = prefix(originals.at(collision.victim));
            const auto driver = prefix(drivers.at(collision.incoming));
            geometry = geometry && collision.key == victim.key && collision.key == driver.key &&
                collision.victimSpan == notes::OverlapSpan{victim.start_ticks, victim.start_ticks + victim.dur_ticks} &&
                collision.incomingSpan == notes::OverlapSpan{driver.start_ticks, driver.start_ticks + driver.dur_ticks};
        }
        check(pairs == expected.pairs && geometry, label + ": all original pairs in deterministic order");
        auto permuted = intent;
        std::reverse(permuted.added.begin(), permuted.added.end());
        std::reverse(permuted.changed.begin(), permuted.changed.end());
        const auto alternate = nd::resolveOverlap(baseline, permuted);
        check(alternate.ok() && alternate.stream.bytes == plan.stream.bytes &&
                  alternate.stream.effects == plan.stream.effects && alternate.intent == plan.intent &&
                  alternate.sideEffects == plan.sideEffects && alternate.collisions == plan.collisions,
              label + ": input permutation preserves material and preview");
    }
}

void conflictAndLimit() {
    const auto first = record(wideId, 0, 10, 60);
    const auto second = record(maximumId, 5, 10, 60);
    const auto third = record(4, 7, 10, 60);
    const auto higherKeyA = record(5, -100, 20, 61), higherKeyB = record(6, -99, 20, 61);
    Diff bad{{higherKeyB, third, second, higherKeyA, first}, {}, {}};
    const auto conflict = nd::resolveOverlap(stream({}), bad);
    rejected(conflict, OverlapStatus::IncomingOverlap, "intersecting incoming drivers");
    check(conflict.conflict && conflict.conflict->first == wideId && conflict.conflict->second == maximumId &&
              conflict.conflict->key == 60 && conflict.conflict->firstSpan == notes::OverlapSpan{0, 10} &&
              conflict.conflict->secondSpan == notes::OverlapSpan{5, 15},
          "incoming refusal names deterministic earliest key/time/unsigned-ID pair");
    std::reverse(bad.added.begin(), bad.added.end());
    const auto reversed = nd::resolveOverlap(stream({}), bad);
    check(reversed.status == OverlapStatus::IncomingOverlap && reversed.conflict == conflict.conflict,
          "input order never picks an incoming winner or different conflict pair");
    const auto tied = nd::resolveOverlap(stream({}), Diff{{record(1, 0, 10),
        record(maximumId, 0, 5), record(wideId, 0, 5), record(3, 0, 5)}, {}, {}});
    check(tied.status == OverlapStatus::IncomingOverlap && tied.conflict &&
              tied.conflict->first == 3 && tied.conflict->second == wideId &&
              tied.conflict->firstSpan == notes::OverlapSpan{0, 5} &&
              tied.conflict->secondSpan == notes::OverlapSpan{0, 5},
          "equal-start conflict sorts by end before full unsigned identity");
    const auto touching = nd::resolveOverlap(stream({}), Diff{{record(1, 0, 10), record(2, 10, 10)}, {}, {}});
    check(touching.ok() && touching.collisions.empty(), "touching incoming drivers are admitted");

    std::vector<Record> victims;
    for (std::uint64_t id = 1; id <= 1000; ++id) victims.push_back(record(id, -1, 202));
    Diff incoming;
    for (std::uint64_t i = 0; i < 100; ++i)
        incoming.added.push_back(record(10001 + i, static_cast<std::int64_t>(i * 2u), 1));
    const auto atLimit = nd::resolveOverlap(stream(victims), incoming);
    check(atLimit.ok() && atLimit.collisions.size() == notes::noteOverlapCollisionLimit &&
              atLimit.sideEffects.size() == 1000 && atLimit.stream.effects.changed.size() == 1000,
          "exact 100000-collision limit remains a complete successful plan");
    bool shortLeft = atLimit.ok();
    for (const auto& effect : atLimit.sideEffects) {
        if (!effect.after || effect.after->bytes.size() < sizeof(NoteRecord)) { shortLeft = false; continue; }
        const auto value = prefix(*effect.after);
        shortLeft = shortLeft && effect.action == OverlapAction::ShortenEnd &&
            value.start_ticks == -1 && value.dur_ticks == 1 && value.note_id == effect.victim &&
            effect.causes == std::vector<std::uint64_t>{10001};
    }
    check(shortLeft, "all later collisions count although every victim stops at the first driver");
    victims.push_back(record(1001, 198, 1));
    const auto overLimit = nd::resolveOverlap(stream(victims), incoming);
    rejected(overLimit, OverlapStatus::CollisionLimitExceeded, "exact 100001st original pair");
    const auto captured = snapshot(stream(victims));
    require(captured.ok, "collision-limit snapshot fixture");
    const auto publicFailure = notes::prepareOverlapDiff(captured, incoming);
    check(!publicFailure.ok() && publicFailure.status() == OverlapStatus::CollisionLimitExceeded &&
              publicFailure.requests().empty() && empty(publicFailure.intent()) && empty(publicFailure.effects()) &&
              publicFailure.collisions().empty() && publicFailure.sideEffects().empty(),
          "public collision-limit failure exposes no partial preview or request");
}

void malformedIntent() {
    const auto a = record(1, 0, 20, 60, 48), b = record(2, 30, 5, 61, 48);
    const auto baseline = stream({a, b}, true, 48);
    const auto incoming = record(3, -1, 2, 60, 48);
    const auto validChanged = alter(a, [](NoteRecord& n) { n.vel_on = 73; });
    std::vector<std::pair<std::string, Diff>> cases;
    cases.emplace_back("duplicate additions", Diff{{incoming, incoming}, {}, {}});
    cases.emplace_back("duplicate changes", Diff{{}, {}, {{a, validChanged}, {a, validChanged}}});
    cases.emplace_back("cross-section identity", Diff{{}, {a}, {{a, validChanged}}});
    cases.emplace_back("addition already exists", Diff{{a}, {}, {}});
    cases.emplace_back("removed identity missing", Diff{{}, {record(77, 1, 2, 60, 48)}, {}});
    cases.emplace_back("changed identity missing", Diff{{}, {}, {{record(77, 1, 2, 60, 48), record(77, 1, 3, 60, 48)}}});
    cases.emplace_back("changed identity changed", Diff{{}, {}, {{a, alter(validChanged, [](NoteRecord& n) { n.note_id = 9; })}}});
    cases.emplace_back("zero identity", Diff{{alter(incoming, [](NoteRecord& n) { n.note_id = 0; })}, {}, {}});
    cases.emplace_back("zero duration", Diff{{alter(incoming, [](NoteRecord& n) { n.dur_ticks = 0; })}, {}, {}});
    cases.emplace_back("overflowed end", Diff{{alter(incoming, [](NoteRecord& n) {
        n.start_ticks = std::numeric_limits<std::int64_t>::max(); n.dur_ticks = 1;
    })}, {}, {}});
    cases.emplace_back("key beyond range", Diff{{alter(incoming, [](NoteRecord& n) { n.key = 128; })}, {}, {}});
    cases.emplace_back("zero velocity", Diff{{alter(incoming, [](NoteRecord& n) { n.vel_on = 0; })}, {}, {}});
    cases.emplace_back("probability beyond range", Diff{{alter(incoming, [](NoteRecord& n) { n.probability = 10001; })}, {}, {}});
    cases.emplace_back("nonfinite cents", Diff{{alter(incoming, [](NoteRecord& n) { n.tuning_cents = std::numeric_limits<float>::infinity(); })}, {}, {}});
    cases.emplace_back("reserved prefix field", Diff{{alter(incoming, [](NoteRecord& n) { n.reserved = 1; })}, {}, {}});
    auto staleOld = a; staleOld.bytes.back() ^= std::byte{1};
    auto changedTail = validChanged; changedTail.bytes.back() ^= std::byte{1};
    auto wrongStride = validChanged; wrongStride.bytes.resize(40);
    auto wrongOldStride = a; wrongOldStride.bytes.resize(40);
    auto invalidOld = alter(a, [](NoteRecord& n) { n.vel_on = 0; });
    cases.emplace_back("erased stale old tail", Diff{{incoming}, {}, {{staleOld, validChanged}}});
    cases.emplace_back("erased modified new tail", Diff{{incoming}, {}, {{a, changedTail}}});
    cases.emplace_back("erased wrong new stride", Diff{{incoming}, {}, {{a, wrongStride}}});
    cases.emplace_back("erased wrong old stride", Diff{{incoming}, {}, {{wrongOldStride, validChanged}}});
    cases.emplace_back("erased invalid old prefix", Diff{{incoming}, {}, {{invalidOld, validChanged}}});
    const auto captured = snapshot(baseline);
    require(captured.ok, "malformed-intent baseline snapshot");
    for (const auto& [name, diff] : cases) {
        rejected(nd::resolveOverlap(baseline, diff), OverlapStatus::InvalidIntent, name);
        const auto prepared = notes::prepareOverlapDiff(captured, diff);
        check(!prepared.ok() && prepared.requests().empty() && empty(prepared.effects()) &&
                  prepared.sideEffects().empty() && prepared.collisions().empty(),
              name + ": public preparation refuses without partial effects");
    }
    // Narrow ordering admits valid old IDs without claiming full old-byte or
    // opaque-tail admission. The resolver above must still catch these cases.
    std::string error;
    const auto ordered = nd::orderOverlapDiff(baseline, Diff{{incoming}, {}, {{staleOld, validChanged}}}, error);
    check(ordered && ordered->changed.size() == 1 && ordered->changed.front().oldRecord == staleOld,
          "order helper does not erase stale old evidence before full admission");
    for (const auto& [name, diff] : cases) {
        if (name.starts_with("erased")) continue;
        error.clear();
        check(!nd::orderOverlapDiff(baseline, diff, error) && !error.empty(),
              name + ": ordering rejects malformed identity/prefix structure");
    }
    const auto rejectedSnapshot = notes::prepareOverlapDiff(notes::Snapshot{}, {});
    check(!rejectedSnapshot.ok() && rejectedSnapshot.status() == OverlapStatus::InvalidSnapshot &&
              rejectedSnapshot.requests().empty(), "default snapshot cannot produce a prepared edit");
    notes::Snapshot forged; forged.ok = true;
    check(!notes::prepareOverlapDiff(forged, {}).ok(), "public Snapshot status cannot manufacture immutable backing");
}

struct Fixture {
    test::TempDirectory directory{"note_overlap", "pure"};
    std::unique_ptr<Store> store;
    Fixture() {
        StoreError error{}; store = Store::create(directory.path() / "song.adi", error);
        require(static_cast<bool>(store), "create public-preparation fixture");
        store->db().exec("INSERT INTO tracks(id,kind) VALUES(1,'midi');"
                        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(1,1,'midi',0,1000),(2,1,'midi',1000,1000)");
    }
    void seed(const Bytes& bytes) {
        SQLite::Statement statement(store->db(), "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(-7,1,'notes',?)");
        statement.bind(1, bytes.data(), static_cast<int>(bytes.size())); statement.exec();
    }
};
struct SqlCounter {
    sqlite3* db;
    unsigned count = 0;
    explicit SqlCounter(Store& store) : db(store.db().getHandle()) {
        sqlite3_trace_v2(db, SQLITE_TRACE_STMT, [](unsigned, void* context, void*, void*) -> int {
            ++static_cast<SqlCounter*>(context)->count; return 0;
        }, this);
    }
    ~SqlCounter() { sqlite3_trace_v2(db, 0, nullptr, nullptr); }
};

void immutablePreparation() {
    static_assert(std::is_same_v<decltype(std::declval<const notes::PreparedOverlap&>().requests()), std::span<const OpRequest>>);
    static_assert(std::is_same_v<decltype(std::declval<const notes::PreparedOverlap&>().effects()), const Diff&>);
    static_assert(std::is_same_v<decltype(std::declval<const notes::PreparedOverlap&>().source()), const notes::Snapshot&>);
    notes::PreparedOverlap emptyPrepared;
    check(!emptyPrepared.ok() && emptyPrepared.status() == OverlapStatus::NotPrepared &&
              emptyPrepared.requests().empty() && empty(emptyPrepared.effects()) &&
              emptyPrepared.sideEffects().empty() && emptyPrepared.collisions().empty(),
          "default prepared value has no request or preview authority");
    Fixture fixture;
    const auto original = record(wideId, -5, 20, 60, 48);
    const auto incoming = record(maximumId, 4, 3, 60, 48);
    fixture.seed(encoded(stream({original}, true, 48, 9, {std::byte{0}, std::byte{0xff}})));
    const auto digest = digestProject(*fixture.store).text;
    auto capture = notes::captureNotes(*fixture.store, 1);
    require(capture.ok, "capture immutable fixture: " + capture.error);
    Diff intent{{incoming}, {}, {}};
    notes::PreparedOverlap prepared;
    {
        SqlCounter counter(*fixture.store);
        prepared = notes::prepareOverlapDiff(capture, intent);
        check(prepared.ok() && counter.count == 0, "overlap preparation performs no SQL on its owner's connection");
    }
    check(digestProject(*fixture.store).text == digest && OpJournal(*fixture.store).count() == 0,
          "capturing and preparing overlap do not mutate project or journal");
    check(prepared.ok() && prepared.requests().size() == 1 && !prepared.expectedHead() &&
              prepared.source().streamId() == std::optional<std::int64_t>{-7},
          "prepared source carries signed stream identity and captured null head");
    if (!prepared.ok() || prepared.requests().size() != 1) return;
    const auto packet = prepared.requests().front().payload;
    check(prepared.requests().front().opType == "note.applyDiff" &&
              packet.is_object() && packet.contains("overlap") && packet.at("overlap").is_object() &&
              packet.at("overlap").contains("version") && packet.at("overlap").at("version").is_number_integer() &&
              packet.at("overlap").at("version") == notes::noteOverlapVersion &&
              packet.contains("from") && packet.at("from") == prepared.source().from(),
          "immutable request carries the frozen overlap version and exact captured source hash");
    const auto effects = prepared.effects();
    const auto sourceFile = prepared.source().sourceFile();
    intent.added.clear(); capture = {};
    auto editedCopy = prepared.requests().front(); editedCopy.payload["overlap"]["version"] = 999;
    check(editedCopy.payload != packet && prepared.requests().front().payload == packet && prepared.effects() == effects,
          "caller intent and copied request mutations cannot alter the bound preview");
    const auto retained = prepared;
    auto moved = std::move(prepared);
    check(!prepared.ok() && prepared.status() == OverlapStatus::NotPrepared && prepared.requests().empty() &&
              moved.ok() && moved.requests().size() == 1 && retained.requests().size() == 1,
          "move empties the old value while const copies keep owned backing");
    const auto absent = notes::captureNotes(*fixture.store, 2);
    require(absent.ok && !absent.hasStream(), "absent snapshot fixture");
    const auto noOp = notes::prepareOverlapDiff(absent, {});
    check(noOp.ok() && noOp.requests().empty() && empty(noOp.effects()) &&
              noOp.source().from() == "absent" && !noOp.source().creationId() && !noOp.expectedHead(),
          "absent empty intent is a captured no-op without allocating a creation identity");
    const auto identical = notes::prepareOverlapDiff(retained.source(), Diff{{}, {}, {{original, original}}});
    check(identical.ok() && identical.requests().empty() && empty(identical.intent()) &&
              empty(identical.effects()) && identical.sideEffects().empty(),
          "byte-identical requested changes are a pure no-op with a captured source");
    fixture.store.reset();
    const auto detached = notes::prepareOverlapDiff(retained.source(), Diff{{incoming}, {}, {}});
    check(detached.ok() && detached.requests().size() == 1 && detached.requests().front().payload == packet &&
              retained.source().sourceFile() == sourceFile && moved.effects() == effects,
          "captured source and immutable preview survive Store close without recapture or defaults");
    const auto absentDetached = notes::prepareOverlapDiff(absent, {});
    check(absentDetached.ok() && absentDetached.requests().empty(), "absent captured no-op also survives owner closure");
}
} // namespace

int main() {
    try {
        relationTable(); identityAndComposition(); keyAndHistoryScope(); canonicalOrder();
        seededCellOracle(); conflictAndLimit(); malformedIntent(); immutablePreparation();
    } catch (const std::exception& exception) {
        check(false, std::string("unexpected fixture exception: ") + exception.what());
    }
    std::printf("note overlap: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
