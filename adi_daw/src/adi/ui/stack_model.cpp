// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/stack_model.hpp"
#include "adi/ui/arrangement_geometry.hpp"
#include "adi/ui/mix_view_provider.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
namespace adi::ui {
namespace stack {
namespace {
constexpr std::uint64_t kFnvBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr double kEps = 1e-9;

std::uint64_t hashBytes(std::uint64_t h, std::string_view s) noexcept {
    for (const char ch : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        h *= kFnvPrime;
    }
    return h;
}
std::uint64_t hashWord(std::uint64_t h, std::uint64_t v) noexcept {
    for (unsigned i = 0; i < 8; ++i) {
        h ^= (v >> (8u * i)) & 0xffu;
        h *= kFnvPrime;
    }
    return h;
}
int scalePx(int v, double k) noexcept {
    return static_cast<int>(std::floor(static_cast<double>(v) * k + 0.5));
}
std::size_t classAt(ItemClass c) noexcept { return static_cast<std::size_t>(c); }

// U+00C0 to U+017F (Latin-1 Supplement letters and Latin Extended-A), each to its base letters, or
// null to copy the character as it is (the multiplication and division signs).
const char *const kFoldLatin[0x180 - 0xC0] = {
    // U+00C0..U+00CF
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    // U+00D0..U+00DF
    "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "ss",
    // U+00E0..U+00EF
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    // U+00F0..U+00FF
    "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "y",
    // U+0100..U+010F
    "a", "a", "a", "a", "a", "a", "c", "c", "c", "c", "c", "c", "c", "c", "d", "d",
    // U+0110..U+011F
    "d", "d", "e", "e", "e", "e", "e", "e", "e", "e", "e", "e", "g", "g", "g", "g",
    // U+0120..U+012F
    "g", "g", "g", "g", "h", "h", "h", "h", "i", "i", "i", "i", "i", "i", "i", "i",
    // U+0130..U+013F
    "i", "i", "ij", "ij", "j", "j", "k", "k", "k", "l", "l", "l", "l", "l", "l", "l",
    // U+0140..U+014F
    "l", "l", "l", "n", "n", "n", "n", "n", "n", "n", "n", "n", "o", "o", "o", "o",
    // U+0150..U+015F
    "o", "o", "oe", "oe", "r", "r", "r", "r", "r", "r", "s", "s", "s", "s", "s", "s",
    // U+0160..U+016F
    "s", "s", "t", "t", "t", "t", "t", "t", "u", "u", "u", "u", "u", "u", "u", "u",
    // U+0170..U+017F
    "u", "u", "u", "u", "w", "w", "y", "y", "y", "z", "z", "z", "z", "z", "z", "s",
};

void foldAppend(std::string_view in, std::string &out) {
    const std::size_t n = in.size();
    std::size_t i = 0;
    while (i < n) {
        const auto b = static_cast<unsigned char>(in[i]);
        if (b < 0x80) {
            if (b >= 0x20 && b != 0x7f)
                out.push_back(b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : static_cast<char>(b));
            ++i;
            continue;
        }
        if (b >= 0xC2 && b <= 0xDF && i + 1 < n) {
            const auto c = static_cast<unsigned char>(in[i + 1]);
            if ((c & 0xC0u) == 0x80u) {
                const unsigned cp = ((b & 0x1Fu) << 6) | (c & 0x3Fu);
                if (cp >= 0xA0u) {
                    const char *rep = cp >= 0xC0u && cp < 0x180u ? kFoldLatin[cp - 0xC0u] : nullptr;
                    if (rep != nullptr)
                        out.append(rep);
                    else
                        out.append(in.substr(i, 2));
                }
                i += 2; // a C1 control (U+0080..U+009F) is dropped
                continue;
            }
        }
        out.push_back(static_cast<char>(b)); // longer sequences and malformed bytes, byte by byte
        ++i;
    }
}
} // namespace

bool Rect::contains(double px, double py) const noexcept {
    return px >= x && py >= y && px < x + w && py < y + h;
}

Constants Constants::scaled(double k) const {
    Constants s = *this;
    const auto px = [k](int &v) { v = scalePx(v, k); };
    px(s.baseMin);
    px(s.baseMax);
    px(s.baseDefault);
    for (auto &d : s.detents)
        px(d);
    px(s.detentSnap);
    px(s.gapMin);
    px(s.widthRefHeight);
    px(s.maxFixed);
    for (auto &f : s.tierFloors)
        px(f);
    px(s.chipColumn);
    px(s.chipHeight);
    px(s.chipInset);
    px(s.chipIndent);
    px(s.chipMinWidth);
    px(s.chipButton);
    px(s.chipButtonGap);
    px(s.chipButtonClear);
    px(s.chipButtonDy);
    px(s.foldInsetX);
    px(s.foldInsetY);
    px(s.foldSize);
    for (auto &w : s.w100)
        px(w);
    for (auto &w : s.w100Dormant)
        px(w);
    px(s.chainItemMin);
    px(s.pill);
    px(s.itemGap);
    px(s.trayPillGap);
    px(s.cardGap);
    px(s.laneMinForItems);
    px(s.proposedBase);
    px(s.chevronW);
    px(s.chevronH);
    px(s.laneWheelPx);
    px(s.navigatorWidth);
    px(s.navigatorMargin);
    px(s.navigatorCollapsed);
    px(s.navLine);
    px(s.navHeader);
    px(s.navIndent);
    px(s.navBarW);
    px(s.navBarH);
    px(s.navJumpTop);
    px(s.dragThreshold);
    px(s.subscribeMarginPx);
    return s;
}

int fisheyeHeight(int base, std::size_t distance, double listHeight, const Constants &c) noexcept {
    const int enlargedH = (c.enlargedPct * base + 50) / 100;
    const double room = listHeight - static_cast<double>(c.enlargedCapBaseRows * base);
    int cap = enlargedH;
    if (room < static_cast<double>(cap))
        cap = room < static_cast<double>(base) ? base : static_cast<int>(std::floor(room));
    cap = std::max(base, cap);
    switch (distance) {
    case 0:
        return cap;
    case 1:
        return std::max(base, std::min(cap, (c.near1Pct * base + 50) / 100));
    case 2:
        return std::max(base, std::min(cap, (c.near2Pct * base + 50) / 100));
    default:
        return base;
    }
}

int gapBetween(int above, int below, const Constants &c) noexcept {
    const int m = std::min(above, below);
    return std::max(c.gapMin, (c.gapPct * m + 50) / 100);
}

namespace {
// The same law for heights caught mid-animation (a capture holds them where they were).
double gapOf(double above, double below, const Constants &c) noexcept {
    const double m = std::min(above, below);
    return std::max(static_cast<double>(c.gapMin), std::floor(static_cast<double>(c.gapPct) * m / 100.0 + 0.5));
}
} // namespace

int chainItemWidth(ItemClass cls, double height, const Constants &c) noexcept {
    if (c.widthRefHeight <= 0 || height <= 0)
        return c.chainItemMin;
    const double w = static_cast<double>(c.w100[classAt(cls)]) *
                     std::pow(height / static_cast<double>(c.widthRefHeight), c.widthExponent);
    return std::max(c.chainItemMin, static_cast<int>(std::lround(w)));
}

int columnWidth(ItemClass cls, bool engagedSomewhere, int base, double factor, const Constants &c) noexcept {
    if (c.widthRefHeight <= 0 || base <= 0)
        return 0;
    const int w100 = engagedSomewhere ? c.w100[classAt(cls)] : c.w100Dormant[classAt(cls)];
    const double w = static_cast<double>(w100) * factor *
                     std::pow(static_cast<double>(base) / static_cast<double>(c.widthRefHeight), c.widthExponent);
    return static_cast<int>(std::lround(w));
}

Tier tierFor(double height, bool maximised, const Constants &c) noexcept {
    if (maximised)
        return Tier::Max;
    for (std::size_t k = 0; k < c.tierFloors.size(); ++k)
        if (height < static_cast<double>(c.tierFloors[k]))
            return static_cast<Tier>(k);
    return Tier::T5;
}

ProcessorPlan planProcessor(double listWidth, int base, std::span<const KindScope> kinds, const WindowViewState &view,
                            const Constants &c) noexcept {
    ProcessorPlan p;
    p.layout = view.moduleLayout;
    p.count = std::min(kinds.size(), kMaxModules);
    p.chipW = c.chipColumn;
    const double gap = c.itemGap, pillW = c.pill;
    std::size_t nc = 0, nt = 0;
    for (std::size_t k = 0; k < p.count; ++k) {
        const auto &kind = kinds[k];
        if (kind.engaged || (kind.keepsDormantColumn && view.showDormant && kind.dormant)) {
            p.place[k] = ModulePlace::Column;
            ++nc;
        } else if (view.showDormant && kind.dormant) {
            p.place[k] = ModulePlace::Tray;
            ++nt;
        } else
            p.place[k] = ModulePlace::Hidden;
    }
    const auto colW = [&](std::size_t k, double factor) {
        return static_cast<double>(columnWidth(kinds[k].widthClass, kinds[k].engaged, base, factor, c));
    };
    const auto processorWidth = [&](double factor) {
        double cols = 0;
        for (std::size_t k = 0; k < p.count; ++k)
            if (p.place[k] == ModulePlace::Column)
                cols += colW(k, factor);
        if (view.moduleLayout == ModuleLayout::Columns) {
            double w = cols;
            if (nc > 0)
                w += static_cast<double>(nc - 1) * gap;
            if (nt > 0)
                w += static_cast<double>(nt) * pillW + static_cast<double>(nt - 1) * c.trayPillGap;
            if (nc > 0 && nt > 0)
                w += gap;
            return w;
        }
        const std::size_t cells = nc + nt;
        return cells == 0 ? 0.0 : cols + static_cast<double>(nt) * pillW + static_cast<double>(cells - 1) * c.cardGap;
    };
    const double navFull = view.navigator ? (view.navigatorCollapsed ? c.navigatorCollapsed : c.navigatorWidth) : 0;
    const auto fixedFor = [&](double procW, double navW) {
        const std::size_t parts = 1 + (view.showChain ? 1u : 0u) + (procW > 0 ? 1u : 0u);
        return static_cast<double>(c.chipColumn) + static_cast<double>(parts - 1) * gap + procW + navW;
    };
    const auto fits = [&](double fixed) {
        const double lane = listWidth - fixed;
        if (!view.showChain || c.fit == FitRule::LaneNonNegative)
            return lane >= -kEps;
        return lane >= static_cast<double>(c.laneMinForItems) - kEps;
    };
    double factor = 1, navW = navFull;
    std::uint8_t step = 0;
    double procW = processorWidth(factor);
    double fixed = fixedFor(procW, navW);
    if (!fits(fixed)) {
        step = 1;
        factor = static_cast<double>(c.columnStepPct) / 100.0;
        procW = processorWidth(factor);
        fixed = fixedFor(procW, navW);
        if (!fits(fixed)) {
            step = 2;
            if (view.navigator && !view.navigatorCollapsed)
                navW = c.navigatorCollapsed;
            fixed = fixedFor(procW, navW);
            if (!fits(fixed))
                step = 3;
        }
    }
    p.budgetStep = step;
    p.factor = factor;
    p.navW = navW;
    p.processorW = procW;
    const double lane = listWidth - fixed;
    if (step == 3) {
        p.shortBy = view.showChain && c.fit == FitRule::LaneShowsItems ? static_cast<double>(c.laneMinForItems) - lane
                                                                       : -lane;
        p.proposeBase = base > c.proposedBase;
        p.proposeProcessorFirst = view.showChain || view.layout != RowLayout::ProcessorFirst;
    }
    p.laneW = view.showChain ? std::max(0.0, lane) : 0.0;
    p.laneMode = !view.showChain ? LaneMode::None
                 : p.laneW >= static_cast<double>(c.laneMinForItems) - kEps ? LaneMode::Items
                                                                            : LaneMode::Pills;
    double x = static_cast<double>(c.chipColumn) + gap;
    if (view.layout == RowLayout::ChainFirst) {
        if (view.showChain) {
            p.laneX = x;
            x += p.laneW + gap;
        }
        p.processorX = x;
    } else {
        p.processorX = x;
        if (procW > 0)
            x += procW + gap;
        if (view.showChain)
            p.laneX = x;
    }
    if (!view.showChain)
        p.laneX = p.processorX;
    if (view.moduleLayout == ModuleLayout::Columns) {
        double cx = p.processorX;
        for (std::size_t k = 0; k < p.count; ++k)
            if (p.place[k] == ModulePlace::Column) {
                p.x[k] = cx;
                p.w[k] = colW(k, factor);
                cx += p.w[k] + gap;
            }
        if (nt > 0) {
            p.trayX = nc > 0 ? cx : p.processorX;
            double tx = p.trayX;
            for (std::size_t k = 0; k < p.count; ++k)
                if (p.place[k] == ModulePlace::Tray) {
                    p.x[k] = tx;
                    p.w[k] = pillW;
                    tx += pillW + c.trayPillGap;
                }
            p.trayW = static_cast<double>(nt) * pillW + static_cast<double>(nt - 1) * c.trayPillGap;
        }
    } else {
        for (std::size_t k = 0; k < p.count; ++k) {
            p.x[k] = p.processorX;
            p.w[k] = p.place[k] == ModulePlace::Column ? colW(k, factor) : p.place[k] == ModulePlace::Tray ? pillW : 0.0;
        }
    }
    p.navX = listWidth - navW;
    p.rowsRight = p.navX;
    return p;
}

void foldForSearch(std::string_view in, std::string &out) {
    out.clear();
    foldAppend(in, out);
}

void normaliseSearchText(std::string_view in, std::size_t maxBytes, std::string &out) {
    out.clear();
    for (std::size_t i = 0; i < in.size();) {
        const auto b = static_cast<unsigned char>(in[i]);
        if (b < 0x20 || b == 0x7f) {
            ++i;
            continue;
        }
        if (b == 0xC2 && i + 1 < in.size()) {
            const auto c = static_cast<unsigned char>(in[i + 1]);
            if (c >= 0x80 && c < 0xA0) {
                i += 2;
                continue;
            }
        }
        out.push_back(static_cast<char>(b));
        ++i;
    }
    if (out.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0u) == 0x80u)
            --cut;
        out.resize(cut);
    }
}

namespace {
template <class T>
void sortUniquePositive(std::vector<T> &ids, std::size_t bound) {
    ids.erase(std::remove_if(ids.begin(), ids.end(), [](T id) { return id <= 0; }), ids.end());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.size() > bound)
        ids.resize(bound);
}
std::size_t boundOf(int v) noexcept { return v < 0 ? 0 : static_cast<std::size_t>(v); }
} // namespace

ViewState ViewState::normalised(const Constants &c) const {
    ViewState v = *this;
    auto &w = v.window;
    w.baseHeight = std::clamp(w.baseHeight, c.baseMin, std::max(c.baseMin, c.baseMax));
    if (static_cast<unsigned>(w.searchMode) > static_cast<unsigned>(SearchMode::Jump))
        w.searchMode = SearchMode::Filter;
    if (static_cast<unsigned>(w.layout) > static_cast<unsigned>(RowLayout::ProcessorFirst))
        w.layout = RowLayout::ChainFirst;
    if (static_cast<unsigned>(w.moduleLayout) > static_cast<unsigned>(ModuleLayout::Card))
        w.moduleLayout = ModuleLayout::Columns;
    if (static_cast<unsigned>(w.rowSpectrum) > static_cast<unsigned>(RowSpectrum::TrackInput))
        w.rowSpectrum = RowSpectrum::ProcessorInput;
    if (static_cast<unsigned>(w.maskingRule) > static_cast<unsigned>(MaskingRule::Focused))
        w.maskingRule = MaskingRule::RolePartner;
    if (static_cast<unsigned>(w.look) > static_cast<unsigned>(Look::Dark))
        w.look = Look::Live;
    std::string search;
    normaliseSearchText(w.search, boundOf(c.searchMaxBytes), search);
    w.search = search;
    if (w.scroll.track <= 0 || !std::isfinite(w.scroll.offset) || w.scroll.offset < 0)
        w.scroll.offset = 0;
    if (w.scroll.track < 0)
        w.scroll.track = 0;
    if (w.scroll.track == 0)
        w.scroll.offset = 0;
    std::vector<RangeEntry> ranges;
    for (const auto &r : w.moduleRange) {
        if (r.track <= 0 || (r.module != 0) == (r.device != 0) || r.device < 0 ||
            static_cast<unsigned>(r.range) > static_cast<unsigned>(DisplayRange::Db30))
            continue;
        ranges.push_back(r);
    }
    const auto rangeLess = [](const RangeEntry &a, const RangeEntry &b) {
        if (a.track != b.track)
            return a.track < b.track;
        if (a.module != b.module)
            return a.module < b.module;
        return a.device < b.device;
    };
    std::stable_sort(ranges.begin(), ranges.end(), rangeLess);
    ranges.erase(std::unique(ranges.begin(), ranges.end(),
                             [&](const RangeEntry &a, const RangeEntry &b) { return !rangeLess(a, b) && !rangeLess(b, a); }),
                 ranges.end());
    if (ranges.size() > boundOf(c.maxRanges))
        ranges.resize(boundOf(c.maxRanges));
    w.moduleRange = ranges;
    sortUniquePositive(w.folded, boundOf(c.maxFolded));
    auto &s = v.shared;
    sortUniquePositive(s.pins, boundOf(c.maxPins));
    std::vector<PinSet> sets;
    for (const auto &set : s.pinSets) {
        if (set.name.empty() || set.name.size() > boundOf(c.maxPinSetName))
            continue;
        if (std::any_of(sets.begin(), sets.end(), [&](const PinSet &kept) { return kept.name == set.name; }))
            continue;
        if (sets.size() >= boundOf(c.maxPinSets))
            break;
        PinSet kept = set;
        sortUniquePositive(kept.ids, boundOf(c.maxPins));
        sets.push_back(std::move(kept));
    }
    s.pinSets = std::move(sets);
    if (!(s.comparePair.a > 0 && s.comparePair.b >= 0 && s.comparePair.a != s.comparePair.b))
        s.comparePair = {};
    return v;
}

std::span<const ModuleKindInput> defaultModuleKinds() noexcept {
    static constexpr std::array<ModuleKindInput, 3> kinds{{
        {1, "ADI-Q", "adi-q eq", ItemClass::AdiQ, true, true},
        {2, "Ducking", "ducking sidechain sc", ItemClass::Ducking, false, false},
        {4, "ADI-L", "adi-l limiter", ItemClass::AdiL, false, false},
    }};
    return kinds;
}

RowsInput SnapshotRows::input(std::uint64_t generation, std::uint64_t providerRevision) const noexcept {
    RowsInput in;
    in.kinds = kinds;
    in.tracks = tracks;
    in.items = items;
    in.modules = modules;
    in.generation = generation;
    in.providerRevision = providerRevision;
    return in;
}

void tracksFromSnapshot(const SnapshotReader &reader, const ArrangementGeometry &geometry,
                        const MixViewProvider &provider, SnapshotRows &out) {
    out.kinds = defaultModuleKinds();
    const auto rows = geometry.rows(reader);
    const std::size_t n = rows.size(), nk = out.kinds.size();
    out.tracks.resize(n);
    out.modules.resize(n * nk);
    out.items.clear();
    const TrackId referenceTrack = provider.reference(reader.tracks());
    for (std::size_t i = 0; i < n; ++i) {
        const auto row = rows[i];
        const engine::TrackNode &node = *row.track;
        TrackInput &t = out.tracks[i];
        t = TrackInput{};
        t.id = node.id;
        t.parent = row.place.parent;
        t.depth = row.place.depth;
        t.group = row.place.group;
        t.main = row.place.main;
        t.ret = row.place.ret;
        std::uint64_t identity = hashBytes(hashBytes(kFnvBasis, "track:"), node.kind);
        t.identity = identity == 0 ? 1 : identity;
        t.inView = geometry.inView(node.id);
        const std::string_view roleName = node.role ? std::string_view(*node.role) : std::string_view();
        t.reference = node.id == referenceTrack || roleName == "reference";
        t.colour = node.color && *node.color >= 0 && *node.color < kPaletteColours
                       ? static_cast<std::int32_t>(*node.color)
                       : -1;
        t.roles = roleName == "kick"    ? role::Kick
                  : roleName == "bass"  ? role::Bass
                  : roleName == "vocal" ? role::Vocal
                                        : std::uint8_t{0};
        t.name = node.name;
        // MixViewProvider v1 reports three engaged flags per channel; the rest is ADI's rules:
        // no ducking on Main (nothing above it to key from), nothing on the Reference.
        const auto state = provider.slots(node);
        for (std::size_t k = 0; k < nk; ++k) {
            ModuleInput &m = out.modules[i * nk + k];
            m = ModuleInput{};
            const std::uint32_t key = out.kinds[k].key;
            bool engaged = false, absent = t.reference;
            if (key == 1)
                engaged = state.eq;
            else if (key == 2) {
                engaged = state.sidechain != SidechainMode::Off;
                absent = absent || t.main;
            } else if (key == 4)
                engaged = state.limiter;
            m.state = absent ? ModuleState::Absent : engaged ? ModuleState::Engaged : ModuleState::Dormant;
            if (m.state != ModuleState::Absent) {
                const std::uint64_t instance =
                    hashWord(hashWord(hashWord(kFnvBasis, static_cast<std::uint64_t>(node.id)), t.identity), key);
                m.instance = instance == 0 ? 1 : instance;
            }
        }
    }
}
} // namespace stack

using namespace stack;

namespace {
constexpr std::size_t kNpos = std::numeric_limits<std::size_t>::max();
constexpr TimeMs kLongAgo = -(TimeMs{1} << 50);
// A capacity: rows one setRows may move in or out of the shown list one by one before it rebuilds the
// list in a single pass instead.
constexpr std::size_t kMaxFlips = 16;
// A capacity: pins changed one by one, under pins only, before the shown list is rebuilt in one pass.
constexpr std::size_t kMaxTouched = 8;
} // namespace

// --- construction ---------------------------------------------------------------------------------

StackModel::StackModel(Constants constants) : c_(constants) {
    view_.baseHeight = c_.baseDefault;
    shared_.pins.reserve(boundOf(c_.maxPins));
    view_.folded.reserve(boundOf(c_.maxFolded));
    view_.search.reserve(boundOf(c_.searchMaxBytes) + 8);
    scratch_.reserve(4 * boundOf(c_.searchMaxBytes) + 64);
    queryFold_.reserve(boundOf(c_.searchMaxBytes) + 8);
    lastScrollAt_ = kLongAgo;
    captureEndAt_ = kLongAgo;
    plan_ = planProcessor(listW_, base(), {}, view_, c_);
}

void StackModel::nextGen(std::uint32_t &gen) noexcept {
    ++gens_;
    if (gens_ == 0)
        gens_ = 1;
    gen = gens_;
}

// --- inputs ---------------------------------------------------------------------------------------

bool StackModel::kindsDiffer(std::span<const ModuleKindInput> kinds) {
    const std::size_t n = std::min(kinds.size(), kMaxModules);
    if (n != kindCount_)
        return true;
    for (std::size_t k = 0; k < n; ++k) {
        const auto &a = kinds_[k];
        const auto &b = kinds[k];
        if (a.key != b.key || a.name != b.name || a.widthClass != b.widthClass ||
            a.keepsDormantColumn != b.keepsDormantColumn || a.dormantPreview != b.dormantPreview)
            return true;
        scratch_.clear();
        foldAppend(b.search, scratch_);
        if (scratch_ != a.searchFold)
            return true;
    }
    return false;
}

void StackModel::adoptKinds(std::span<const ModuleKindInput> kinds) {
    const std::size_t n = std::min(kinds.size(), kMaxModules);
    if (kinds.size() > kMaxModules)
        stats_.ignoredInputs += kinds.size() - kMaxModules;
    // Each record's modules follow their keys into the new order; a key that left takes its refs with it.
    std::array<std::size_t, kMaxModules> from{};
    for (std::size_t k = 0; k < n; ++k) {
        from[k] = kNpos;
        for (std::size_t old = 0; old < kindCount_; ++old)
            if (kinds_[old].key == kinds[k].key && kinds[k].key != 0)
                from[k] = old;
    }
    for (auto &r : rows_) {
        const auto before = r.modules;
        for (std::size_t k = 0; k < kMaxModules; ++k)
            r.modules[k] = k < n && from[k] != kNpos ? before[from[k]] : ModuleRec{};
    }
    for (std::size_t k = 0; k < n; ++k) {
        auto &kind = kinds_[k];
        kind.key = kinds[k].key;
        kind.name.assign(kinds[k].name);
        kind.searchFold.clear();
        foldAppend(kinds[k].search, kind.searchFold);
        kind.widthClass = kinds[k].widthClass;
        kind.keepsDormantColumn = kinds[k].keepsDormantColumn;
        kind.dormantPreview = kinds[k].dormantPreview;
        bool duplicate = kind.key == 0;
        for (std::size_t j = 0; j < k; ++j)
            duplicate = duplicate || kinds_[j].key == kind.key;
        if (duplicate) {
            ++stats_.ignoredInputs;
            kind.key = 0; // never addressable
        }
    }
    for (std::size_t k = n; k < kMaxModules; ++k)
        kinds_[k] = KindRec{};
    kindCount_ = n;
}

std::uint64_t StackModel::textHashOf(const TrackInput &t, const RowsInput &in, std::size_t ti) const {
    std::uint64_t h = hashBytes(kFnvBasis, t.name);
    h = hashWord(h, t.roles);
    if (t.itemBegin <= in.items.size() && t.itemCount <= in.items.size() - t.itemBegin)
        for (std::uint32_t j = 0; j < t.itemCount; ++j) {
            const auto &item = in.items[t.itemBegin + j];
            h = hashWord(hashBytes(h, item.name), static_cast<std::uint64_t>(item.device));
        }
    const std::size_t nk = in.kinds.size();
    if (!in.modules.empty())
        for (std::size_t k = 0; k < std::min(nk, kMaxModules); ++k)
            h = hashWord(h, in.modules[ti * nk + k].state == ModuleState::Engaged ? kinds_[k].key : 0u);
    return h;
}

void StackModel::buildHaystack(Rec &r, const TrackInput &t, const RowsInput &in, std::size_t ti) {
    ++stats_.haystackBuilds;
    r.nameFold.clear();
    foldAppend(t.name, r.nameFold);
    r.haystack.assign(r.nameFold);
    r.haystack.push_back('\x1f');
    for (const auto &item : r.items) {
        // the input's item name for this device (the record keeps no names)
        for (std::uint32_t j = 0; j < t.itemCount; ++j) {
            const auto &src = in.items[t.itemBegin + j];
            if (src.device == item.device) {
                foldAppend(src.name, r.haystack);
                r.haystack.push_back('\x1f');
                break;
            }
        }
    }
    const std::size_t nk = in.kinds.size();
    for (std::size_t k = 0; k < kindCount_; ++k)
        if (!in.modules.empty() && in.modules[ti * nk + k].state == ModuleState::Engaged && r.valid) {
            r.haystack.append(kinds_[k].searchFold);
            r.haystack.push_back('\x1f');
        }
    if ((t.roles & role::Kick) != 0)
        r.haystack.append("kick\x1f");
    if ((t.roles & role::Bass) != 0)
        r.haystack.append("bass\x1f");
    if ((t.roles & role::Vocal) != 0)
        r.haystack.append("vocal\x1f");
}

StackModel::Changes StackModel::updateRecord(Rec &r, const TrackInput &t, const RowsInput &in, std::size_t ti,
                                             bool fresh) {
    Changes ch;
    const bool baseValid = t.id > 0 && t.identity != 0;
    if (!baseValid)
        ++stats_.ignoredInputs;
    if (fresh) {
        r.id = t.id;
        r.dup = false;
        nextGen(r.gen);
        r.items.clear();
        for (auto &m : r.modules)
            m = ModuleRec{};
        r.laneFirst = 0;
        r.laneFirstDevice = 0;
        r.textHash = 0;
        r.name.clear();
        ch.any = ch.text = ch.structure = ch.membership = true;
    }
    if (!fresh && r.identity != t.identity) {
        // Another track under the same id (a kind change today, a new creation token later): every
        // ref to the row, its items and its modules dies.
        nextGen(r.gen);
        r.items.clear();
        for (auto &m : r.modules)
            m = ModuleRec{};
        ch.any = ch.text = ch.membership = true;
    }
    r.identity = t.identity;
    r.revision = t.revision;
    const bool valid = baseValid && !r.dup;
    if (r.valid != valid) {
        r.valid = valid;
        ch.any = ch.membership = true;
    }
    if (r.parent != t.parent || r.depth != t.depth) {
        r.parent = t.parent;
        r.depth = t.depth;
        ch.any = ch.structure = true;
    }
    if (r.group != t.group || r.main != t.main || r.ret != t.ret) {
        r.group = t.group;
        r.main = t.main;
        r.ret = t.ret;
        ch.any = true;
    }
    if (r.inView != t.inView || r.reference != t.reference) {
        r.inView = t.inView;
        r.reference = t.reference;
        ch.any = ch.membership = true;
    }
    if (r.colour != t.colour || r.roles != t.roles) {
        r.colour = t.colour;
        r.roles = t.roles;
        ch.any = true;
    }
    if (r.name != t.name) {
        r.name.assign(t.name);
        ch.any = ch.text = true;
        if (r.group)
            ch.groupName = true;
    }
    // modules: keyed by (row, kind key); kept while the instance stays and the module stays present
    const std::size_t nk = in.kinds.size();
    for (std::size_t k = 0; k < kindCount_; ++k) {
        ModuleInput src;
        if (!in.modules.empty())
            src = in.modules[ti * nk + k];
        if (src.state != ModuleState::Absent && src.instance == 0) {
            ++stats_.ignoredInputs;
            src.state = ModuleState::Absent;
        }
        if (kinds_[k].key == 0)
            src.state = ModuleState::Absent;
        ModuleRec &m = r.modules[k];
        if (src.state == ModuleState::Absent) {
            if (m.state != ModuleState::Absent || m.gen != 0) {
                m = ModuleRec{};
                ch.any = ch.text = true;
            }
            continue;
        }
        const bool same = m.state != ModuleState::Absent && m.instance == src.instance;
        if (!same) {
            nextGen(m.gen);
            ch.any = ch.text = true;
        }
        if (m.state != src.state)
            ch.any = ch.text = true;
        if (m.device != src.device)
            ch.any = true;
        m.state = src.state;
        m.instance = src.instance;
        m.device = src.device;
    }
    // items: keyed by device within the row, kept while the identity stays
    scratchItems_.clear();
    const bool inRange = t.itemBegin <= in.items.size() && t.itemCount <= in.items.size() - t.itemBegin;
    if (!inRange && t.itemCount > 0)
        ++stats_.ignoredInputs;
    if (inRange)
        for (std::uint32_t j = 0; j < t.itemCount; ++j) {
            const ItemInput &src = in.items[t.itemBegin + j];
            bool ok = src.device > 0 && src.identity != 0;
            for (std::size_t k = 0; ok && k < kindCount_; ++k)
                ok = !(r.modules[k].device != 0 && r.modules[k].device == src.device); // the processor's own device
            for (const auto &seen : scratchItems_)
                ok = ok && seen.device != src.device;
            if (!ok) {
                ++stats_.ignoredInputs;
                continue;
            }
            ItemRec item;
            item.device = src.device;
            item.identity = src.identity;
            item.cls = static_cast<unsigned>(src.cls) < kItemClasses ? src.cls : ItemClass::View;
            for (const auto &old : r.items)
                if (old.device == src.device && old.identity == src.identity) {
                    item.gen = old.gen;
                    break;
                }
            if (item.gen == 0)
                nextGen(item.gen);
            scratchItems_.push_back(item);
        }
    bool itemsSame = scratchItems_.size() == r.items.size();
    for (std::size_t j = 0; itemsSame && j < r.items.size(); ++j) {
        const auto &a = scratchItems_[j], &b = r.items[j];
        itemsSame = a.device == b.device && a.identity == b.identity && a.cls == b.cls && a.gen == b.gen;
    }
    if (!itemsSame) {
        r.items.swap(scratchItems_);
        ch.any = ch.text = true;
        // the lane's first item follows its device; a device that left leaves the index clamped
        r.laneFirst = resolveLaneFirst(r);
    }
    const std::uint64_t textHash = textHashOf(t, in, ti);
    if (textHash != r.textHash || ch.text) {
        r.textHash = textHash;
        buildHaystack(r, t, in, ti);
        ch.any = ch.text = true;
    }
    return ch;
}

std::uint32_t StackModel::resolveLaneFirst(const Rec &r) const noexcept {
    if (r.items.empty())
        return 0;
    if (r.laneFirstDevice != 0)
        for (std::size_t j = 0; j < r.items.size(); ++j)
            if (r.items[j].device == r.laneFirstDevice)
                return static_cast<std::uint32_t>(j);
    return std::min(r.laneFirst, static_cast<std::uint32_t>(r.items.size() - 1));
}

void StackModel::rebuildIndex() {
    const std::size_t n = rows_.size();
    byId_.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        byId_[i] = {rows_[i].id, static_cast<std::uint32_t>(i)};
    std::sort(byId_.begin(), byId_.end());
    for (std::size_t i = 0; i < n; ++i) {
        Rec &r = rows_[byId_[i].second];
        const bool dup = i > 0 && byId_[i - 1].first == byId_[i].first;
        if (dup && !r.dup)
            ++stats_.ignoredInputs;
        r.dup = dup;
        r.valid = r.id > 0 && r.identity != 0 && !dup;
    }
    for (auto &r : rows_)
        r.parentRec = r.parent != 0 && r.parent != r.id ? recOf(r.parent) : kNoRec;
}

std::uint32_t StackModel::recOf(TrackId id) const noexcept {
    const auto at = std::lower_bound(byId_.begin(), byId_.end(), std::pair<TrackId, std::uint32_t>{id, 0u});
    if (at == byId_.end() || at->first != id)
        return kNoRec;
    return at->second;
}

const StackModel::Rec *StackModel::recFor(TrackId id) const noexcept {
    const std::uint32_t i = recOf(id);
    return i == kNoRec || !rows_[i].valid ? nullptr : &rows_[i];
}

void StackModel::scopeAdd(const Rec &r, int sign) noexcept {
    if (!inScope(r))
        return;
    for (std::size_t k = 0; k < kindCount_; ++k) {
        auto &count = scope_[k][static_cast<std::size_t>(r.modules[k].state)];
        count = sign > 0 ? count + 1 : count - 1;
    }
}

void StackModel::recountScope() {
    for (auto &k : scope_)
        k = {};
    for (const auto &r : rows_)
        scopeAdd(r, +1);
}

bool StackModel::memberPlain(std::uint32_t rec) const noexcept {
    const Rec &r = rows_[rec];
    if (!inScope(r))
        return false;
    const bool searching = view_.searchMode == SearchMode::Filter && !queryFold_.empty();
    if (searching) {
        if (!r.match)
            return false;
    } else if (r.underFold)
        return false;
    if (view_.pinnedOnly && !pinned(r.id))
        return false;
    return true;
}

bool StackModel::member(std::uint32_t rec) const noexcept {
    if (cap_.active && rows_[rec].valid && rows_[rec].id == cap_.target.row.track)
        return true;
    return memberPlain(rec);
}

bool StackModel::computeMatch(std::uint32_t rec) const noexcept {
    if (queryFold_.empty())
        return true;
    const Rec &r = rows_[rec];
    if (r.haystack.find(queryFold_) != std::string::npos)
        return true;
    std::uint32_t up = r.parentRec;
    for (std::size_t steps = 0; up != kNoRec && steps <= rows_.size(); ++steps) {
        if (rows_[up].nameFold.find(queryFold_) != std::string::npos)
            return true;
        up = rows_[up].parentRec;
    }
    return false;
}

bool StackModel::computeUnderFold(std::uint32_t rec) const noexcept {
    std::uint32_t up = rows_[rec].parentRec;
    for (std::size_t steps = 0; up != kNoRec && steps <= rows_.size(); ++steps) {
        if (std::binary_search(view_.folded.begin(), view_.folded.end(), rows_[up].id))
            return true;
        up = rows_[up].parentRec;
    }
    return false;
}

std::optional<std::size_t> StackModel::shownOfRec(std::uint32_t rec) const noexcept {
    const auto at = std::lower_bound(shown_.begin(), shown_.end(), rec);
    if (at == shown_.end() || *at != rec)
        return std::nullopt;
    return static_cast<std::size_t>(at - shown_.begin());
}

void StackModel::rebuildShown() {
    ++stats_.fullPasses;
    shown_.clear();
    for (std::uint32_t i = 0; i < rows_.size(); ++i)
        if (member(i))
            shown_.push_back(i);
    ++shownVersion_;
}

void StackModel::editShown(std::uint32_t rec, bool memberNow) {
    const auto at = std::lower_bound(shown_.begin(), shown_.end(), rec);
    const bool there = at != shown_.end() && *at == rec;
    if (memberNow && !there)
        shown_.insert(at, rec);
    else if (!memberNow && there)
        shown_.erase(at);
    else
        return;
    ++stats_.shownEdits;
    ++shownVersion_;
}

Effects StackModel::setRows(const RowsInput &input, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (rowsSet_ && (input.generation != 0 || input.providerRevision != 0) && input.generation == rowsGeneration_ &&
        input.providerRevision == providerRevision_)
        return fx;
    rowsSet_ = true;
    rowsGeneration_ = input.generation;
    providerRevision_ = input.providerRevision;
    RowsInput in = input;
    const bool kindsChanged = kindsDiffer(in.kinds);
    if (kindsChanged)
        adoptKinds(in.kinds);
    if (in.modules.size() != in.tracks.size() * in.kinds.size()) {
        if (!in.modules.empty() || (!in.tracks.empty() && !in.kinds.empty()))
            ++stats_.ignoredInputs;
        in.modules = {};
    }
    // hold what stays still across the change
    if (!cap_.active && !jump_.active) {
        if (pointerOverRows())
            anchorPointer();
        else
            anchorFirstVisible();
    }
    const std::size_t oldFocusShown = focusRow_ != 0 ? shownIndex(focusRow_).value_or(kNpos) : kNpos;
    const std::size_t n = in.tracks.size();
    stats_.rowCompares += n;
    bool structural = n != rows_.size();
    for (std::size_t i = 0; !structural && i < n; ++i)
        structural = rows_[i].id != in.tracks[i].id;
    bool rematch = false, rebuild = kindsChanged;
    std::array<std::uint32_t, kMaxFlips> flips{};
    std::size_t flipCount = 0;
    if (!structural) {
        for (std::size_t i = 0; i < n; ++i) {
            Rec &r = rows_[i];
            const TrackInput &t = in.tracks[i];
            if (!kindsChanged && t.revision != 0 && t.revision == r.revision && t.identity == r.identity)
                continue;
            const auto rec = static_cast<std::uint32_t>(i);
            const bool wasMember = member(rec);
            scopeAdd(r, -1);
            const Changes ch = updateRecord(r, t, in, i, false);
            scopeAdd(r, +1);
            if (!ch.any)
                continue;
            ++stats_.rowUpdates;
            bump();
            if (ch.structure)
                structural = true;
            if (ch.groupName && !queryFold_.empty())
                rematch = true;
            if (ch.text || ch.membership)
                r.match = computeMatch(rec);
            if (member(rec) != wasMember) {
                if (flipCount < flips.size())
                    flips[flipCount++] = rec;
                else
                    rebuild = true;
            }
        }
    }
    if (structural) {
        // the full pass: records follow their ids into the input's order
        spare_.resize(n);
        taken_.assign(rows_.size(), 0);
        for (std::size_t i = 0; i < n; ++i) {
            const TrackInput &t = in.tracks[i];
            const std::uint32_t old = recOf(t.id);
            bool fresh = true;
            if (old != kNoRec && taken_[old] == 0) {
                taken_[old] = 1;
                spare_[i] = std::move(rows_[old]);
                fresh = false;
            } else
                spare_[i].dup = false;
            updateRecord(spare_[i], t, in, i, fresh);
        }
        rows_.swap(spare_);
        rebuildIndex();
        for (std::uint32_t i = 0; i < rows_.size(); ++i) {
            rows_[i].underFold = computeUnderFold(i);
            rows_[i].match = computeMatch(i);
        }
        recountScope();
        rebuild = true;
        bump();
    } else if (rematch) {
        ++stats_.fullPasses;
        for (std::uint32_t i = 0; i < rows_.size(); ++i)
            rows_[i].match = computeMatch(i);
        rebuild = true;
    }
    if (kindsChanged && !structural)
        recountScope();
    // capacities for the steady state: a capture's frozen lane, the shown list, the matches
    std::size_t longest = 0;
    for (const auto &r : rows_)
        longest = std::max(longest, r.items.size());
    if (longest > maxChain_) {
        maxChain_ = longest;
        frozenItems_.reserve(maxChain_ + 1);
        scratchItems_.reserve(maxChain_ + 1);
    }
    shown_.reserve(rows_.size());
    matches_.reserve(rows_.size());
    const auto versionBefore = shownVersion_;
    if (rebuild)
        rebuildShown();
    else
        for (std::size_t f = 0; f < flipCount; ++f)
            editShown(flips[f], member(flips[f]));
    checkCapture(now, fx);
    if (shownVersion_ != versionBefore)
        afterShownChange(now, oldFocusShown);
    replan();
    if (view_.searchMode == SearchMode::Jump && !queryFold_.empty())
        rebuildMatches();
    solveScroll();
    rehit(now);
    updateNavScroll();
    endEvent(fx, before);
    return fx;
}

void StackModel::afterShownChange(TimeMs now, std::size_t oldFocusShown) {
    (void)now;
    if (maximised_ != 0 && !shownIndex(maximised_))
        maximised_ = 0;
    if (enlarged_ != 0 && !shownIndex(enlarged_))
        enlarged_ = 0;
    if (landed_ != 0 && !shownIndex(landed_))
        landed_ = 0;
    if (jump_.active && !shownIndex(jump_.row))
        jump_ = Jump{};
    if (pairPending_ != 0 && !shownIndex(pairPending_))
        pairPending_ = 0;
    if (cap_.active) {
        reindexEntries();
        recomputeFrozenGaps();
    } else
        snapEntries();
    fixFocus(oldFocusShown);
}

void StackModel::fixFocus(std::size_t oldFocusShown) {
    if (focusRow_ == 0)
        return;
    if (shownIndex(focusRow_)) {
        if (focusItem_.place != ItemPlace::None && !live(focusItem_))
            focusItem_ = chipRef(focusRow_);
        return;
    }
    const std::uint32_t rec = recOf(focusRow_);
    const bool searching = view_.searchMode == SearchMode::Filter && !queryFold_.empty();
    if (rec != kNoRec && rows_[rec].valid && rows_[rec].underFold && !searching) {
        // folded away: the focus goes to the nearest folded group that is shown
        std::uint32_t up = rows_[rec].parentRec;
        for (std::size_t steps = 0; up != kNoRec && steps <= rows_.size(); ++steps) {
            if (std::binary_search(view_.folded.begin(), view_.folded.end(), rows_[up].id) &&
                shownOfRec(up).has_value()) {
                focusRow_ = rows_[up].id;
                focusItem_ = chipRef(focusRow_);
                return;
            }
            up = rows_[up].parentRec;
        }
    }
    if (!shown_.empty() && oldFocusShown != kNpos) {
        focusRow_ = rows_[shown_[std::min(oldFocusShown, shown_.size() - 1)]].id;
        focusItem_ = chipRef(focusRow_);
        return;
    }
    focusRow_ = 0;
    focusItem_ = ItemRef{};
}

void StackModel::replan() {
    if (planFrozen_)
        return;
    std::array<KindScope, kMaxModules> scope{};
    for (std::size_t k = 0; k < kindCount_; ++k) {
        scope[k].widthClass = kinds_[k].widthClass;
        scope[k].keepsDormantColumn = kinds_[k].keepsDormantColumn;
        scope[k].engaged = kinds_[k].key != 0 && scope_[k][static_cast<std::size_t>(ModuleState::Engaged)] > 0;
        scope[k].dormant = kinds_[k].key != 0 && scope_[k][static_cast<std::size_t>(ModuleState::Dormant)] > 0;
    }
    const auto next = planProcessor(listW_, base(), std::span<const KindScope>(scope.data(), kindCount_), view_, c_);
    if (!(next == plan_)) {
        plan_ = next;
        bump();
    }
}

Effects StackModel::setViewport(double listWidth, double listHeight, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    listWidth = std::isfinite(listWidth) ? std::max(0.0, listWidth) : 0.0;
    listHeight = std::isfinite(listHeight) ? std::max(0.0, listHeight) : 0.0;
    if (listWidth == listW_ && listHeight == listH_)
        return fx;
    if (!cap_.active && !jump_.active)
        anchorFirstVisible();
    listW_ = listWidth;
    listH_ = listHeight;
    replan();
    if (!cap_.active)
        snapEntries(); // the fisheye's cap follows the list's height
    solveScroll();
    rehit(now);
    updateNavScroll();
    bump();
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setSelection(TrackId id, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (id == selection_)
        return fx;
    selection_ = id;
    bump();
    if (c_.followSelection && !cap_.active)
        if (const auto i = shownIndex(id)) {
            focusByKey(*i, chipRef(id), now, fx);
            fx.select = 0; // read, never written back
        }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setKeyboardFocus(bool on, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (on == keyboardFocus_)
        return fx;
    keyboardFocus_ = on;
    if (!on) {
        focusOwns_ = false;
        // outside the list, the focused row was enlarged only because the keys were here
        if (!inside_ && leaveAt_ == kNever && view_.autoZoom && maximised_ == 0 && !cap_.active && enlarged_ != 0 &&
            enlarged_ == focusRow_) {
            setEnlarged(0, now);
            anchorFirstVisible();
            retarget(now);
            solveScroll();
        }
    }
    bump();
    endEvent(fx, before);
    return fx;
}

// --- geometry core --------------------------------------------------------------------------------

double StackModel::g0() const noexcept { return static_cast<double>(gapBetween(base(), base(), c_)); }

double StackModel::ease(double u) const noexcept {
    u = std::clamp(u, 0.0, 1.0);
    const double v = 1 - u;
    switch (c_.easing) {
    case Easing::Linear:
        return u;
    case Easing::QuadOut:
        return 1 - v * v;
    case Easing::CubicOut:
        break;
    }
    return 1 - v * v * v;
}

double StackModel::animE() const noexcept {
    if (!animActive_ || c_.heightAnimMs <= 0)
        return 1;
    return ease(static_cast<double>(now_ - animStart_) / static_cast<double>(c_.heightAnimMs));
}

const StackModel::Entry *StackModel::entryAt(std::size_t shown) const noexcept {
    for (std::size_t j = 0; j < entryCount_; ++j) {
        ++stats_.entryVisits;
        if (entries_[j].shown == shown)
            return &entries_[j];
        if (entries_[j].shown > shown)
            break;
    }
    return nullptr;
}

double StackModel::heightAt(std::size_t shown) const noexcept {
    const Entry *e = entryAt(shown);
    if (e == nullptr)
        return static_cast<double>(base());
    const double k = animE();
    return e->hFrom + (e->hTo - e->hFrom) * k;
}

double StackModel::targetHeightAt(std::size_t shown) const noexcept {
    const Entry *e = entryAt(shown);
    return e == nullptr ? static_cast<double>(base()) : e->hTo;
}

double StackModel::contentTop(std::size_t shown) const noexcept {
    const double k = animE(), b = base(), g = g0();
    double top = static_cast<double>(shown) * (b + g);
    for (std::size_t j = 0; j < entryCount_; ++j) {
        const Entry &e = entries_[j];
        if (e.shown >= shown)
            break;
        ++stats_.entryVisits;
        top += (e.hFrom + (e.hTo - e.hFrom) * k - b) + (e.gFrom + (e.gTo - e.gFrom) * k - g);
    }
    return top;
}

StackModel::Located StackModel::locate(double cy) const noexcept {
    Located found;
    const std::size_t count = shown_.size();
    if (count == 0)
        return found;
    if (cy < 0) {
        found.locus = Locus::Above;
        return found;
    }
    const double k = animE(), b = base(), g = g0(), p = b + g;
    double top = 0;
    std::size_t i = 0;
    for (std::size_t j = 0; j <= entryCount_; ++j) {
        const std::size_t end = j < entryCount_ ? entries_[j].shown : count;
        if (end > i) {
            const double spanEnd = top + static_cast<double>(end - i) * p;
            if (cy < spanEnd) {
                std::size_t r = i + static_cast<std::size_t>(std::floor((cy - top) / p));
                if (r >= end)
                    r = end - 1;
                const double rTop = top + static_cast<double>(r - i) * p;
                ++stats_.rowEvaluations;
                if (cy < rTop + b) {
                    found.locus = Locus::Row;
                    found.index = r;
                    found.top = rTop;
                    found.height = b;
                } else if (r + 1 < count) {
                    found.locus = Locus::Gap;
                    found.index = r;
                    found.top = rTop;
                    found.height = b;
                } else
                    found.locus = Locus::Below;
                return found;
            }
            top = spanEnd;
            i = end;
        }
        if (j == entryCount_)
            break;
        const Entry &e = entries_[j];
        ++stats_.entryVisits;
        const double h = e.hFrom + (e.hTo - e.hFrom) * k, gg = e.gFrom + (e.gTo - e.gFrom) * k;
        if (cy < top + h) {
            ++stats_.rowEvaluations;
            found.locus = Locus::Row;
            found.index = e.shown;
            found.top = top;
            found.height = h;
            return found;
        }
        if (e.shown + 1 >= count) {
            found.locus = Locus::Below;
            return found;
        }
        if (cy < top + h + gg) {
            found.locus = Locus::Gap;
            found.index = e.shown;
            found.top = top;
            found.height = h;
            return found;
        }
        top += h + gg;
        i = e.shown + 1;
    }
    found.locus = Locus::Below;
    return found;
}

Span StackModel::spanFor(double from, double to) const noexcept {
    Span s;
    const std::size_t count = shown_.size();
    if (count == 0 || to <= from)
        return s;
    const Located a = locate(from);
    std::size_t first = 0;
    switch (a.locus) {
    case Locus::Empty:
    case Locus::Below:
        return s;
    case Locus::Above:
        first = 0;
        break;
    case Locus::Row:
        first = a.index;
        break;
    case Locus::Gap:
        first = a.index + 1;
        break;
    }
    const Located z = locate(to);
    std::size_t last = count - 1;
    switch (z.locus) {
    case Locus::Empty:
    case Locus::Above:
        return s;
    case Locus::Row:
        if (z.top >= to - kEps) {
            if (z.index == 0)
                return s;
            last = z.index - 1;
        } else
            last = z.index;
        break;
    case Locus::Gap:
        last = z.index;
        break;
    case Locus::Below:
        last = count - 1;
        break;
    }
    if (first > last || first >= count)
        return s;
    s.first = first;
    s.last = last;
    s.empty = false;
    return s;
}

Span StackModel::visible() const noexcept { return spanFor(scroll_, scroll_ + listH_); }

Span StackModel::realised() const noexcept {
    const double m = listH_ * static_cast<double>(c_.realisedMarginPct) / 100.0;
    return spanFor(scroll_ - m, scroll_ + listH_ + m);
}

Span StackModel::subscribed() const noexcept {
    const double m = static_cast<double>(c_.subscribeMarginPx);
    return spanFor(scroll_ - m, scroll_ + listH_ + m);
}

double StackModel::contentHeight() const noexcept {
    const std::size_t count = shown_.size();
    if (count == 0)
        return 0;
    return contentTop(count - 1) + heightAt(count - 1);
}

double StackModel::maxScroll() const noexcept { return std::max(0.0, contentHeight() - listH_); }

double StackModel::maxHeight() const noexcept {
    const double b = base();
    const double room = listH_ - static_cast<double>(c_.maxGaps) * g0();
    double m = room;
    if (c_.maxMode == MaxMode::Fixed)
        m = std::min(static_cast<double>(c_.maxFixed), room);
    return std::max(b, std::floor(m));
}

std::size_t StackModel::buildTargets(std::array<Entry, kMaxEntries> &out) const noexcept {
    const std::size_t count = shown_.size();
    if (count == 0)
        return 0;
    const int b = base();
    const double g = g0();
    if (maximised_ != 0)
        if (const auto m = shownIndex(maximised_)) {
            Entry e;
            e.shown = *m;
            e.id = maximised_;
            e.hTo = e.hFrom = maxHeight();
            e.gTo = e.gFrom = g; // the row below sits at base: the gap is the base gap
            out[0] = e;
            return 1;
        }
    if (enlarged_ == 0)
        return 0;
    const auto at = shownIndex(enlarged_);
    if (!at)
        return 0;
    const std::size_t e = *at;
    const std::size_t lo = e >= 2 ? e - 2 : 0, hi = std::min(count - 1, e + 2);
    std::array<int, 5> h{};
    for (std::size_t i = lo; i <= hi; ++i)
        h[i - lo] = fisheyeHeight(b, i > e ? i - e : e - i, listH_, c_);
    std::size_t n = 0;
    for (std::size_t i = lo; i <= hi; ++i) {
        Entry t;
        t.shown = i;
        t.id = rows_[shown_[i]].id;
        t.hTo = h[i - lo];
        const int below = i + 1 <= hi ? h[i + 1 - lo] : b;
        t.gTo = i + 1 < count ? static_cast<double>(gapBetween(h[i - lo], below, c_)) : g;
        t.hFrom = t.hTo;
        t.gFrom = t.gTo;
        if (t.hTo == static_cast<double>(b) && t.gTo == g)
            continue;
        out[n++] = t;
    }
    return n;
}

void StackModel::retarget(TimeMs now) {
    if (cap_.active)
        return;
    std::array<Entry, kMaxEntries> targets{};
    const std::size_t nt = buildTargets(targets);
    const double k = animE(), b = base(), g = g0();
    std::array<Entry, 2 * kMaxEntries> merged{};
    std::array<bool, 2 * kMaxEntries> isTarget{};
    std::size_t nm = 0, a = 0, t = 0;
    bool moving = false;
    while (a < entryCount_ || t < nt) {
        Entry m;
        bool target = false;
        if (t >= nt || (a < entryCount_ && entries_[a].shown < targets[t].shown)) {
            const Entry &e = entries_[a++];
            m = e;
            m.hFrom = e.hFrom + (e.hTo - e.hFrom) * k;
            m.gFrom = e.gFrom + (e.gTo - e.gFrom) * k;
            m.hTo = b;
            m.gTo = g;
        } else if (a >= entryCount_ || targets[t].shown < entries_[a].shown) {
            m = targets[t++];
            m.hFrom = b;
            m.gFrom = g;
            target = true;
        } else {
            const Entry &e = entries_[a++];
            m = targets[t++];
            m.hFrom = e.hFrom + (e.hTo - e.hFrom) * k;
            m.gFrom = e.gFrom + (e.gTo - e.gFrom) * k;
            target = true;
        }
        if (m.hFrom == m.hTo && m.gFrom == m.gTo && m.hTo == b && m.gTo == g)
            continue;
        moving = moving || m.hFrom != m.hTo || m.gFrom != m.gTo;
        isTarget[nm] = target;
        merged[nm++] = m;
    }
    // for want of room, the rows nearest their rest height snap to it
    while (nm > kMaxEntries) {
        std::size_t pick = kNpos;
        double least = std::numeric_limits<double>::max();
        for (std::size_t j = 0; j < nm; ++j)
            if (!isTarget[j]) {
                const double d = std::abs(merged[j].hFrom - b) + std::abs(merged[j].gFrom - g);
                if (d < least) {
                    least = d;
                    pick = j;
                }
            }
        if (pick == kNpos)
            break;
        for (std::size_t j = pick; j + 1 < nm; ++j) {
            merged[j] = merged[j + 1];
            isTarget[j] = isTarget[j + 1];
        }
        --nm;
        ++stats_.entrySnaps;
    }
    entryCount_ = std::min(nm, kMaxEntries);
    for (std::size_t j = 0; j < entryCount_; ++j)
        entries_[j] = merged[j];
    animStart_ = now;
    animActive_ = moving && c_.heightAnimMs > 0;
    if (!animActive_)
        finishAnimation();
    bump();
}

void StackModel::finishAnimation() {
    const double b = base(), g = g0();
    std::size_t n = 0;
    for (std::size_t j = 0; j < entryCount_; ++j) {
        Entry e = entries_[j];
        e.hFrom = e.hTo;
        e.gFrom = e.gTo;
        if (e.hTo == b && e.gTo == g)
            continue;
        entries_[n++] = e;
    }
    entryCount_ = n;
    animActive_ = false;
}

void StackModel::snapEntries() {
    std::array<Entry, kMaxEntries> targets{};
    entryCount_ = buildTargets(targets);
    for (std::size_t j = 0; j < entryCount_; ++j)
        entries_[j] = targets[j];
    animActive_ = false;
    bump();
}

void StackModel::reindexEntries() {
    std::size_t n = 0;
    for (std::size_t j = 0; j < entryCount_; ++j) {
        Entry e = entries_[j];
        const auto at = shownIndex(e.id);
        if (!at)
            continue;
        e.shown = *at;
        entries_[n++] = e;
    }
    entryCount_ = n;
    std::sort(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(n),
              [](const Entry &x, const Entry &y) { return x.shown < y.shown; });
}

void StackModel::recomputeFrozenGaps() {
    const double g = g0();
    for (std::size_t j = 0; j < entryCount_; ++j) {
        Entry &e = entries_[j];
        const double h = e.hFrom + (e.hTo - e.hFrom) * animE();
        double below = base();
        if (j + 1 < entryCount_ && entries_[j + 1].shown == e.shown + 1) {
            const Entry &next = entries_[j + 1];
            below = next.hFrom + (next.hTo - next.hFrom) * animE();
        }
        const double gap = e.shown + 1 < shown_.size() ? gapOf(h, below, c_) : g;
        e.hFrom = e.hTo = h;
        e.gFrom = e.gTo = gap;
    }
    animActive_ = false;
}

// --- anchors and scroll ---------------------------------------------------------------------------

bool StackModel::pointerOverRows() const noexcept {
    return inside_ && pointerKnown_ && px_ >= 0 && py_ >= 0 && px_ < plan().rowsRight && py_ < listH_;
}

void StackModel::anchorPointer() {
    if (c_.anchor == Anchor::ListTop || !pointerOverRows() || shown_.empty()) {
        anchorFirstVisible();
        return;
    }
    const Located at = locate(py_ + scroll_);
    std::size_t r = 0;
    switch (at.locus) {
    case Locus::Empty:
        anchor_ = AnchorHold{};
        return;
    case Locus::Above:
        r = 0;
        break;
    case Locus::Row:
        r = at.index;
        break;
    case Locus::Gap:
        r = at.index + 1;
        break;
    case Locus::Below:
        r = shown_.size() - 1;
        break;
    }
    anchor_.row = rows_[shown_[r]].id;
    anchor_.viewTop = viewTop(r);
    anchor_.clamp = false; // hold the pointer's row even past the ends: the slack settles on the next scroll
    anchor_.fraction = -1;
    if (c_.anchor == Anchor::PointerRowFraction && at.locus == Locus::Row) {
        const double h = heightAt(r);
        if (h > 0)
            anchor_.fraction = (py_ - anchor_.viewTop) / h;
    }
}

void StackModel::anchorFirstVisible() {
    anchor_ = AnchorHold{};
    const Span v = visible();
    if (v.empty)
        return;
    anchor_.row = rows_[shown_[v.first]].id;
    anchor_.viewTop = viewTop(v.first);
    anchor_.clamp = true;
}

void StackModel::anchorRow(TrackId id, bool clamp) {
    const auto at = shownIndex(id);
    if (!at) {
        anchorFirstVisible();
        return;
    }
    anchor_ = AnchorHold{};
    anchor_.row = id;
    anchor_.viewTop = viewTop(*at);
    anchor_.clamp = clamp;
}

void StackModel::startJump(TrackId id, double goal, int duration, bool clamp, TimeMs now) {
    const auto at = shownIndex(id);
    if (!at)
        return;
    jump_ = Jump{};
    jump_.active = true;
    jump_.row = id;
    jump_.from = viewTop(*at);
    jump_.to = goal;
    jump_.start = now;
    jump_.duration = std::max(0, duration);
    jump_.moving = jump_.duration > 0;
    jump_.clamp = clamp;
    bump();
}

double StackModel::jumpTop(TimeMs t) const noexcept {
    if (!jump_.moving || jump_.duration <= 0)
        return jump_.to;
    const double u = static_cast<double>(t - jump_.start) / static_cast<double>(jump_.duration);
    return jump_.from + (jump_.to - jump_.from) * ease(u);
}

void StackModel::releaseJumpIfDone() {
    if (!jump_.active || jump_.moving || animActive_)
        return;
    const TrackId id = jump_.row;
    const bool clamp = jump_.clamp;
    jump_ = Jump{};
    anchorRow(id, clamp);
}

void StackModel::solveScroll() {
    const std::size_t count = shown_.size();
    const double old = scroll_;
    if (count == 0) {
        scroll_ = 0;
    } else {
        const double top = std::max(0.0, maxScroll());
        const auto bounded = [top](double s) { return std::clamp(s, 0.0, top); };
        bool solved = false;
        if (cap_.active)
            if (const auto at = shownIndex(cap_.target.row.track)) {
                scroll_ = contentTop(*at) - cap_.viewTop;
                solved = true;
            }
        if (!solved && jump_.active) {
            if (const auto at = shownIndex(jump_.row)) {
                const double s = contentTop(*at) - jumpTop(now_);
                scroll_ = jump_.clamp ? bounded(s) : s;
                solved = true;
            } else
                jump_ = Jump{};
        }
        if (!solved && anchor_.row != 0)
            if (const auto at = shownIndex(anchor_.row)) {
                double vt = anchor_.viewTop;
                if (anchor_.fraction >= 0)
                    vt = py_ - anchor_.fraction * heightAt(*at);
                const double s = contentTop(*at) - vt;
                scroll_ = anchor_.clamp ? bounded(s) : s;
                solved = true;
            }
        if (!solved)
            scroll_ = bounded(scroll_);
    }
    if (scroll_ != old)
        bump();
}

void StackModel::userScroll(double to, TimeMs now) {
    jump_ = Jump{};
    const double top = std::max(0.0, maxScroll());
    scroll_ = std::clamp(std::isfinite(to) ? to : 0.0, 0.0, top);
    lastScrollAt_ = now;
    if (pointerOverRows())
        anchorPointer();
    else
        anchorFirstVisible();
    solveScroll();
    rehit(now);
    bump();
}

double StackModel::revealGoal(std::size_t shown) const noexcept {
    // the row at its resting height, just inside the list
    const double h = targetHeightAt(shown);
    const double top = viewTop(shown);
    if (h >= listH_)
        return 0;
    return std::clamp(top, 0.0, listH_ - h);
}

void StackModel::startRevealIfCut(TimeMs now) {
    const TrackId id = maximised_ != 0 ? maximised_ : enlarged_;
    if (id == 0)
        return;
    const auto at = shownIndex(id);
    if (!at)
        return;
    const double top = viewTop(*at), h = heightAt(*at);
    if (top + h <= listH_ + 1e-6 || h > listH_)
        return;
    startJump(id, listH_ - h, c_.revealMs, false, now);
    if (!jump_.moving)
        releaseJumpIfDone();
    solveScroll();
    rehit(now);
}

// --- time -----------------------------------------------------------------------------------------

TimeMs StackModel::normaliseTime(TimeMs t) noexcept {
    if (!clockStarted_) {
        clockStarted_ = true;
        now_ = t;
        return t;
    }
    if (t >= now_)
        return t;
    if (now_ - t > static_cast<TimeMs>(c_.clockRebaseMs)) {
        rebase(t - now_);
        ++stats_.clockRebases;
        return t;
    }
    return now_;
}

void StackModel::rebase(TimeMs delta) noexcept {
    const auto shift = [delta](TimeMs &v) {
        if (v != kNever && v > kLongAgo / 2)
            v += delta;
    };
    shift(now_);
    shift(animStart_);
    shift(jump_.start);
    for (auto &a : laneAnims_)
        shift(a.start);
    shift(enteredAt_);
    shift(lastScrollAt_);
    shift(captureEndAt_);
    shift(leaveAt_);
    shift(landingEnd_);
    shift(enlargedSince_);
}

TimeMs StackModel::animEnd() const noexcept {
    return animActive_ ? animStart_ + c_.heightAnimMs : kNever;
}
TimeMs StackModel::jumpEnd() const noexcept {
    return jump_.active && jump_.moving ? jump_.start + jump_.duration : kNever;
}
TimeMs StackModel::landingDeadline() const noexcept { return landed_ != 0 ? landingEnd_ : kNever; }
TimeMs StackModel::leaveDeadline() const noexcept { return cap_.active ? kNever : leaveAt_; }
bool StackModel::landingActive() const noexcept { return landed_ != 0 && now_ < landingEnd_; }

TimeMs StackModel::promotionDeadline() const noexcept {
    if (!view_.autoZoom || cap_.active || maximised_ != 0 || focusOwns_ || !inside_ || candidate_ == 0 ||
        landed_ != 0 || candidate_ == enlarged_)
        return kNever;
    return std::max({enteredAt_ + c_.dwellMs, lastScrollAt_ + c_.scrollHoldOffMs, captureEndAt_ + c_.dwellMs});
}

TimeMs StackModel::laneDeadline() const noexcept {
    TimeMs d = kNever;
    for (const auto &a : laneAnims_)
        if (a.row != 0)
            d = std::min(d, a.start + c_.laneScrollMs);
    return d;
}

TimeMs StackModel::internalDeadline() const noexcept {
    return std::min({animEnd(), jumpEnd(), landingDeadline(), leaveDeadline(), promotionDeadline(), laneDeadline()});
}

TimeMs StackModel::nextDeadline() const noexcept { return internalDeadline(); }

bool StackModel::busy() const noexcept {
    if (animActive_ || (jump_.active && jump_.moving))
        return true;
    for (const auto &a : laneAnims_)
        if (a.row != 0)
            return true;
    return false;
}

void StackModel::advanceTo(TimeMs t) {
    t = normaliseTime(t);
    const bool wasBusy = busy();
    const TimeMs was = now_;
    for (int guard = 0; guard < 256; ++guard) {
        const TimeMs d = internalDeadline();
        if (d > t)
            break;
        processAt(std::max(d, now_));
    }
    now_ = t;
    if (wasBusy || busy()) {
        solveScroll();
        if (t != was)
            bump();
    }
}

void StackModel::processAt(TimeMs d) {
    now_ = d;
    solveScroll(); // everything where it is at d, before a deadline reads a view top or moves an anchor
    if (animEnd() <= d)
        onAnimEnd(d);
    if (jumpEnd() <= d)
        onJumpArrive(d);
    if (landingDeadline() <= d) {
        landed_ = 0;
        bump();
    }
    if (leaveDeadline() <= d)
        onLeave(d);
    if (promotionDeadline() <= d)
        promote(d);
    for (auto &a : laneAnims_)
        if (a.row != 0 && a.start + c_.laneScrollMs <= d) {
            a = LaneAnim{};
            bump();
        }
}

void StackModel::onAnimEnd(TimeMs d) {
    finishAnimation();
    releaseJumpIfDone();
    solveScroll();
    rehit(d);
    if (!cap_.active && (enlarged_ != 0 || maximised_ != 0) && !jump_.active) {
        if (buttonDown_)
            revealPending_ = true;
        else
            startRevealIfCut(d);
    }
    updateNavScroll();
    bump();
}

void StackModel::onJumpArrive(TimeMs d) {
    jump_.moving = false;
    releaseJumpIfDone();
    solveScroll();
    rehit(d);
    // a jump that outlasted the heights: the enlarged row may still be cut off at the bottom
    if (!cap_.active && !jump_.active && !animActive_ && (enlarged_ != 0 || maximised_ != 0)) {
        if (buttonDown_)
            revealPending_ = true;
        else
            startRevealIfCut(d);
    }
    updateNavScroll();
    bump();
}

void StackModel::onLeave(TimeMs d) {
    leaveAt_ = kNever;
    if (inside_ || cap_.active)
        return;
    if (!jump_.moving) {
        jump_ = Jump{};
        anchorFirstVisible(); // settles any slack the pointer left
    }
    if (view_.autoZoom && maximised_ == 0) {
        const TrackId target = keyboardFocus_ && focusRow_ != 0 && shownIndex(focusRow_) ? focusRow_ : 0;
        setEnlarged(target, d);
        retarget(d);
    }
    solveScroll();
    updateNavScroll();
}

void StackModel::promote(TimeMs d) {
    setEnlarged(candidate_, d);
    jump_ = Jump{};
    anchorPointer();
    retarget(d);
    solveScroll();
    updateNavScroll();
}

void StackModel::setEnlarged(TrackId id, TimeMs now) {
    if (id == enlarged_)
        return;
    enlarged_ = id;
    enlargedSince_ = now;
    bump();
}

Effects StackModel::advance(TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now); // the navigator's own scroll moves only at events and deadlines: no frame rate in it
    endEvent(fx, before);
    return fx;
}

// --- interaction ----------------------------------------------------------------------------------

std::optional<std::size_t> StackModel::pointerRowIndex() const noexcept {
    if (!pointerOverRows())
        return std::nullopt;
    const Located at = locate(py_ + scroll_);
    if (at.locus != Locus::Row)
        return std::nullopt;
    return at.index;
}

void StackModel::rehit(TimeMs now) {
    const auto at = pointerRowIndex();
    const TrackId r = at ? rows_[shown_[*at]].id : 0;
    if (r == candidate_)
        return;
    candidate_ = r;
    enteredAt_ = now;
}

ItemRef StackModel::chipRef(TrackId id) const noexcept {
    ItemRef ref;
    if (const Rec *r = recFor(id)) {
        ref.row = {r->id, r->gen};
        ref.gen = r->gen;
    }
    return ref;
}

void StackModel::focusByKey(std::size_t shown, const ItemRef &item, TimeMs now, Effects &fx) {
    const Rec &r = rows_[shown_[shown]];
    focusRow_ = r.id;
    focusItem_ = item.row.valid() ? item : chipRef(r.id);
    fx.select = r.id;
    bump();
    if (cap_.active)
        return;
    landed_ = 0;
    if (maximised_ == 0) {
        setEnlarged(r.id, now);
        focusOwns_ = view_.autoZoom;
    }
    retarget(now);
    // reveal it at its resting height, moving with the heights
    startJump(r.id, revealGoal(shown), c_.heightAnimMs, true, now);
    releaseJumpIfDone();
    solveScroll();
    updateNavScroll();
}

void StackModel::focusByClick(std::size_t shown, const ItemRef &item, TimeMs now, Effects &fx) {
    const Rec &r = rows_[shown_[shown]];
    focusRow_ = r.id;
    focusItem_ = item.row.valid() ? item : chipRef(r.id);
    fx.select = r.id;
    bump();
    if (!view_.autoZoom && !cap_.active && maximised_ == 0 && enlarged_ != r.id) {
        // with auto-zoom off only focus enlarges, and at once
        setEnlarged(r.id, now);
        jump_ = Jump{};
        anchorPointer();
        retarget(now);
        solveScroll();
    }
}

std::size_t StackModel::focusOrder(std::size_t shown, std::array<ItemRef, kFocusOrder> &out) const noexcept {
    std::size_t n = 0;
    const Rec &r = rows_[shown_[shown]];
    out[n++] = chipRef(r.id);
    const auto chain = [&] {
        if (plan().laneMode == LaneMode::None)
            return;
        for (const auto &item : r.items) {
            if (n >= out.size())
                return;
            ItemRef ref;
            ref.row = {r.id, r.gen};
            ref.place = ItemPlace::Chain;
            ref.device = item.device;
            ref.gen = item.gen;
            out[n++] = ref;
        }
    };
    const auto modules = [&] {
        const auto &p = plan();
        const auto add = [&](ModulePlace want) {
            for (std::size_t k = 0; k < kindCount_ && n < out.size(); ++k) {
                if (want != ModulePlace::Hidden && p.place[k] != want)
                    continue;
                const CellGeo cell = cellAt(shown, k);
                if (cell.content == CellContent::Hidden || cell.content == CellContent::Blank ||
                    cell.content == CellContent::Refused || !cell.ref.row.valid())
                    continue;
                out[n++] = cell.ref;
            }
        };
        if (p.layout == ModuleLayout::Columns) {
            add(ModulePlace::Column);
            add(ModulePlace::Tray);
        } else
            add(ModulePlace::Hidden); // card: the input's order
    };
    if (view_.layout == RowLayout::ChainFirst) {
        chain();
        modules();
    } else {
        modules();
        chain();
    }
    return n;
}

void StackModel::setLaneFirst(Rec &r, std::uint32_t index, TimeMs now, bool animate) {
    if (r.items.empty())
        return;
    index = std::min(index, static_cast<std::uint32_t>(r.items.size() - 1));
    const std::uint32_t from = resolveLaneFirst(r);
    if (index == from)
        return;
    // where the lane is now, in items, so a scroll during a scroll continues from it
    double pos = static_cast<double>(from);
    LaneAnim *anim = nullptr;
    for (auto &a : laneAnims_)
        if (a.row == r.id) {
            anim = &a;
            const double u = c_.laneScrollMs > 0 ? static_cast<double>(now - a.start) / static_cast<double>(c_.laneScrollMs) : 1.0;
            pos = a.from + (a.to - a.from) * ease(u);
        }
    r.laneFirst = index;
    r.laneFirstDevice = r.items[index].device;
    if (animate && c_.laneScrollMs > 0) {
        if (anim == nullptr)
            for (auto &a : laneAnims_)
                if (a.row == 0) {
                    anim = &a;
                    break;
                }
        if (anim == nullptr) {
            ++stats_.entrySnaps; // every lane animation in use: this one jumps
        } else {
            anim->row = r.id;
            anim->from = pos;
            anim->to = static_cast<double>(index);
            anim->start = now;
        }
    } else if (anim != nullptr)
        *anim = LaneAnim{};
    bump();
}

void StackModel::revealLaneItem(Rec &r, std::uint32_t index, std::size_t shown, TimeMs now) {
    const auto &p = plan();
    if (p.laneMode == LaneMode::None || r.items.empty())
        return;
    const double h = targetHeightAt(shown);
    std::uint32_t first = resolveLaneFirst(r);
    if (index < first) {
        setLaneFirst(r, index, now, true);
        return;
    }
    const bool pills = p.laneMode == LaneMode::Pills;
    const double gap = pills ? c_.trayPillGap : c_.itemGap;
    const auto widthOf = [&](std::uint32_t j) {
        const auto &item = r.items[j];
        if (pills || tierFor(h, false, c_) < c_.foldBelow[classAt(item.cls)])
            return static_cast<double>(c_.pill);
        return static_cast<double>(chainItemWidth(item.cls, h, c_));
    };
    // move the first item on until `index` ends inside the lane (or is the first)
    while (first < index) {
        double right = 0;
        for (std::uint32_t j = first; j <= index; ++j)
            right += widthOf(j) + (j > first ? gap : 0.0);
        if (right <= p.laneW + kEps)
            break;
        ++first;
    }
    setLaneFirst(r, first, now, true);
}

void StackModel::stepItem(int dir, TimeMs now, Effects &fx) {
    if (focusRow_ == 0)
        return;
    const auto at = shownIndex(focusRow_);
    if (!at)
        return;
    std::array<ItemRef, kFocusOrder> order{};
    const std::size_t n = focusOrder(*at, order);
    std::size_t pos = 0;
    for (std::size_t j = 0; j < n; ++j)
        if (order[j].place == focusItem_.place && order[j].module == focusItem_.module &&
            (focusItem_.place != ItemPlace::Chain || order[j].device == focusItem_.device))
            pos = j;
    const std::ptrdiff_t next = static_cast<std::ptrdiff_t>(pos) + dir;
    if (next < 0 || next >= static_cast<std::ptrdiff_t>(n))
        return;
    focusItem_ = order[static_cast<std::size_t>(next)];
    bump();
    if (focusItem_.place == ItemPlace::Chain && !cap_.active) {
        Rec &r = rows_[shown_[*at]];
        for (std::uint32_t j = 0; j < r.items.size(); ++j)
            if (r.items[j].device == focusItem_.device)
                revealLaneItem(r, j, *at, now);
    }
    (void)fx;
}

void StackModel::doMaximise(std::size_t shown, TimeMs now) {
    const TrackId id = rows_[shown_[shown]].id;
    maximised_ = id;
    landed_ = 0;
    retarget(now);
    startJump(id, g0(), c_.heightAnimMs, true, now); // framed by its two gaps
    releaseJumpIfDone();
    solveScroll();
    updateNavScroll();
}

void StackModel::doRestore(TimeMs now) {
    if (maximised_ == 0)
        return;
    const TrackId was = maximised_;
    maximised_ = 0;
    jump_ = Jump{};
    if (pointerOverRows())
        anchorPointer();
    else
        anchorRow(was, true);
    retarget(now);
    solveScroll();
    updateNavScroll();
}

void StackModel::pressRow(const Hit &hit, Button button, Mods mods, int clicks, TimeMs now, Effects &fx) {
    const bool left = button == Button::Left;
    if (button == Button::Middle)
        return;
    switch (hit.part) {
    case HitPart::Chip:
        focusByClick(hit.shown, chipRef(hit.row.track), now, fx);
        if (left && clicks >= 2)
            fx.showDevices = hit.row;
        if (left && clicks == 1 && !cap_.active && maximised_ != hit.row.track &&
            tierFor(static_cast<double>(base()), false, c_) <= c_.chipMaxesUpTo)
            doMaximise(hit.shown, now);
        break;
    case HitPart::ChipFold:
        if (left)
            fx = mergeFlags(fx, toggleFoldNow(hit.row, now));
        break;
    case HitPart::ChipPin:
        if (left)
            fx = mergeFlags(fx, pinNow(hit.row, mods, now));
        break;
    case HitPart::ChipMasking:
        if (left)
            fx = mergeFlags(fx, maskingNow(hit.row));
        break;
    case HitPart::ChipKick:
        break; // W7's
    case HitPart::ChainItem:
    case HitPart::ChainPill:
    case HitPart::ModuleItem:
    case HitPart::ModulePreview:
    case HitPart::ModulePill:
    case HitPart::ModuleRefused:
        focusByClick(hit.shown, hit.item.row.valid() && hit.part != HitPart::ModuleRefused ? hit.item : chipRef(hit.row.track),
                     now, fx);
        break;
    case HitPart::ChevronLeft:
    case HitPart::ChevronRight:
        if (left && !cap_.active)
            if (const std::uint32_t rec = recOf(hit.row.track); rec != kNoRec) {
                Rec &r = rows_[rec];
                const std::uint32_t first = resolveLaneFirst(r);
                if (hit.part == HitPart::ChevronRight)
                    setLaneFirst(r, first + 1, now, true);
                else if (first > 0)
                    setLaneFirst(r, first - 1, now, true);
            }
        break;
    case HitPart::RowSpace:
        focusByClick(hit.shown, chipRef(hit.row.track), now, fx);
        if (left && maximised_ != 0 && !cap_.active)
            doRestore(now);
        break;
    default:
        break;
    }
}

Effects StackModel::mergeFlags(Effects into, const Effects &from) noexcept {
    into.refused = into.refused || from.refused;
    into.windowViewChanged = into.windowViewChanged || from.windowViewChanged;
    into.sharedViewChanged = into.sharedViewChanged || from.sharedViewChanged;
    return into;
}

Effects StackModel::pointerMove(double x, double y, bool buttonDown, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const bool moved = !pointerKnown_ || x != px_ || y != py_;
    px_ = x;
    py_ = y;
    pointerKnown_ = true;
    buttonDown_ = buttonDown;
    const bool in = x >= 0 && y >= 0 && x < listW_ && y < listH_;
    if (navPress_.active && buttonDown_)
        navDrag(x, y, now);
    if (!in) {
        if (inside_) {
            inside_ = false;
            candidate_ = 0;
            if (!cap_.active)
                leaveAt_ = now + c_.leaveMs;
        }
        endEvent(fx, before);
        return fx;
    }
    if (!inside_) {
        inside_ = true;
        leaveAt_ = kNever;
    }
    if (moved && focusOwns_) {
        // the keys gave the enlarged row away; a move hands it back to hover after a fresh dwell
        focusOwns_ = false;
        candidate_ = 0;
    }
    // the landing's hold ends when the pointer moves into another row, which then needs its own dwell
    if (moved && landed_ != 0)
        if (const auto at = pointerRowIndex(); at && rows_[shown_[*at]].id != landed_) {
            landed_ = 0;
            candidate_ = 0;
            bump();
        }
    // rule 2: while heights move, the row under the pointer holds still under it
    if (moved && animActive_ && !jump_.active && !cap_.active && pointerOverRows())
        anchorPointer();
    if (moved)
        rehit(now);
    endEvent(fx, before);
    return fx;
}

Effects StackModel::pointerExit(TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (inside_) {
        inside_ = false;
        candidate_ = 0;
        if (!cap_.active)
            leaveAt_ = now + c_.leaveMs;
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::pointerDown(double x, double y, Button button, Mods mods, int clicks, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const bool moved = !pointerKnown_ || x != px_ || y != py_;
    px_ = x;
    py_ = y;
    pointerKnown_ = true;
    buttonDown_ = true;
    if (x >= 0 && y >= 0 && x < listW_ && y < listH_ && !inside_) {
        inside_ = true;
        leaveAt_ = kNever;
    }
    if (moved)
        rehit(now); // a press where the pointer was not is a move
    fx.takeFocus = true;
    fx.consumed = true;
    const Hit hit = hitTest(x, y);
    fx.hit = hit;
    switch (hit.part) {
    case HitPart::NavigatorLine:
    case HitPart::NavigatorEmpty:
    case HitPart::NavigatorHeader: {
        navPress_ = NavPress{};
        navPress_.active = button == Button::Left;
        navPress_.x = x;
        navPress_.y = y;
        navPress_.startScroll = scroll_;
        navPress_.onLine = hit.part == HitPart::NavigatorLine;
        navPress_.line = hit.shown;
        navPress_.shift = mods.shift;
        navPress_.inFrame = navigator().frame.contains(x, y);
        break;
    }
    default:
        pressRow(hit, button, mods, clicks, now, fx);
        break;
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::pointerUp(double x, double y, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    px_ = x;
    py_ = y;
    buttonDown_ = false;
    if (navPress_.active) {
        const double t = c_.dragThreshold;
        const bool click = !navPress_.dragging && navPress_.onLine && std::abs(y - navPress_.y) <= t &&
                           std::abs(x - navPress_.x) <= t;
        const NavPress press = navPress_;
        navPress_ = NavPress{};
        if (click)
            navClick(press.line, press.shift, now, fx);
    }
    if (revealPending_) {
        revealPending_ = false;
        if (!animActive_ && !cap_.active && !jump_.active)
            startRevealIfCut(now);
    }
    endEvent(fx, before);
    return fx;
}

void StackModel::navDrag(double x, double y, TimeMs now) {
    (void)x;
    const double dy = y - navPress_.y;
    if (!navPress_.dragging) {
        if (std::abs(dy) <= static_cast<double>(c_.dragThreshold) || !navPress_.inFrame)
            return;
        navPress_.dragging = true;
    }
    if (cap_.active || c_.navLine <= 0)
        return;
    userScroll(navPress_.startScroll + dy / static_cast<double>(c_.navLine) * pitch(), now);
    updateNavScroll();
}

void StackModel::navClick(std::size_t line, bool shift, TimeMs now, Effects &fx) {
    if (line >= shown_.size())
        return;
    const TrackId id = rows_[shown_[line]].id;
    if (shift) {
        if (pairPending_ == 0)
            pairPending_ = id;
        else if (pairPending_ == id)
            pairPending_ = 0;
        else {
            shared_.comparePair = {pairPending_, id};
            pairPending_ = 0;
            fx.sharedViewChanged = true;
        }
        bump();
        return;
    }
    if (cap_.active) {
        fx.refused = true;
        return;
    }
    focusRow_ = id;
    focusItem_ = chipRef(id);
    fx.select = id;
    landed_ = 0;
    if (maximised_ == 0) {
        setEnlarged(id, now);
        focusOwns_ = view_.autoZoom;
    }
    retarget(now);
    startJump(id, static_cast<double>(c_.navJumpTop), c_.navJumpMs, true, now);
    releaseJumpIfDone();
    solveScroll();
    updateNavScroll();
}

Effects StackModel::wheel(double x, double y, const WheelDelta &delta, Mods mods, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    fx.consumed = true;
    if (mods.ctrl || cap_.active) { // Ctrl+wheel does nothing in STACK; a capture ignores the wheel
        endEvent(fx, before);
        return fx;
    }
    if (mods.alt) {
        int steps = static_cast<int>(std::lround(-delta.notches));
        if (steps == 0 && delta.notches != 0)
            steps = delta.notches < 0 ? 1 : -1;
        if (steps == 0 && delta.pixelsY != 0)
            steps = delta.pixelsY < 0 ? 1 : -1;
        if (steps != 0)
            fx = mergeFlags(fx, stepBaseNow(steps, now));
        endEvent(fx, before);
        return fx;
    }
    if (mods.shift) {
        const Hit hit = hitTest(x, y);
        if (hit.row.valid()) {
            const std::uint32_t rec = recOf(hit.row.track);
            if (rec != kNoRec) {
                Rec &r = rows_[rec];
                int items = 0;
                if (delta.notches != 0) {
                    items = static_cast<int>(std::lround(delta.notches));
                    if (items == 0)
                        items = delta.notches > 0 ? 1 : -1;
                } else {
                    const double px = delta.pixelsX != 0 ? delta.pixelsX : delta.pixelsY;
                    if (laneWheelRow_ != r.id) {
                        laneWheelRow_ = r.id;
                        laneWheelAcc_ = 0;
                    }
                    laneWheelAcc_ += px;
                    const double per = std::max(1, c_.laneWheelPx);
                    while (laneWheelAcc_ >= per) {
                        laneWheelAcc_ -= per;
                        ++items;
                    }
                    while (laneWheelAcc_ <= -per) {
                        laneWheelAcc_ += per;
                        --items;
                    }
                }
                if (items != 0) {
                    const std::int64_t first = static_cast<std::int64_t>(resolveLaneFirst(r)) + items;
                    setLaneFirst(r, static_cast<std::uint32_t>(std::max<std::int64_t>(0, first)), now, true);
                }
            }
        }
        endEvent(fx, before);
        return fx;
    }
    const double dy = delta.notches != 0 ? delta.notches * pitch() : delta.pixelsY;
    if (dy != 0)
        userScroll(scroll_ + dy, now);
    updateNavScroll();
    endEvent(fx, before);
    return fx;
}

Effects StackModel::key(Key k, Mods mods, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const std::size_t count = shown_.size();
    switch (k) {
    case Key::Up:
    case Key::Down: {
        fx.consumed = true;
        if (count == 0)
            break;
        const auto at = focusRow_ != 0 ? shownIndex(focusRow_) : std::nullopt;
        std::size_t next = 0;
        if (!at) {
            const Span v = visible();
            next = v.empty ? 0 : (k == Key::Down ? v.first : v.last);
        } else if (k == Key::Up) {
            if (*at == 0)
                break;
            next = *at - 1;
        } else {
            if (*at + 1 >= count)
                break;
            next = *at + 1;
        }
        focusByKey(next, chipRef(rows_[shown_[next]].id), now, fx);
        break;
    }
    case Key::Left:
    case Key::Right:
        fx.consumed = true;
        stepItem(k == Key::Left ? -1 : 1, now, fx);
        break;
    case Key::Enter:
        fx.consumed = true;
        if (const auto at = focusRow_ != 0 ? shownIndex(focusRow_) : std::nullopt) {
            if (cap_.active)
                fx.refused = true;
            else if (maximised_ == focusRow_)
                doRestore(now);
            else
                doMaximise(*at, now);
        }
        break;
    case Key::Escape:
        if (cap_.active)
            fx.escape = EscapeStep::Capture;
        else if (maximised_ != 0) {
            doRestore(now);
            fx.escape = EscapeStep::RestoreMax;
        } else if (!view_.search.empty()) {
            clearSearch(now, fx);
            fx.escape = EscapeStep::ClearSearch;
        } else if (openedFrom_ != 0)
            fx.escape = EscapeStep::CloseMixView;
        fx.consumed = fx.escape != EscapeStep::None;
        break;
    case Key::Home:
    case Key::End:
    case Key::PageUp:
    case Key::PageDown:
        fx.consumed = true;
        if (cap_.active) {
            fx.refused = true;
            break;
        }
        if (k == Key::Home)
            userScroll(0, now);
        else if (k == Key::End)
            userScroll(maxScroll(), now);
        else
            userScroll(scroll_ + (k == Key::PageDown ? listH_ : -listH_), now);
        updateNavScroll();
        break;
    case Key::Tab:
        if (cap_.active && cap_.kind == CaptureKind::Text && cap_.target.place == ItemPlace::None) {
            // Live's rename Tab: commit this name, go to the next (or previous) track's
            fx.consumed = true;
            const TrackId was = cap_.target.row.track;
            fx.captureEnded = cap_.token;
            finishCapture(now, CaptureStatus::Ended);
            if (const auto at = shownIndex(was)) {
                const bool back = mods.shift;
                if ((back && *at > 0) || (!back && *at + 1 < shown_.size())) {
                    const std::size_t next = back ? *at - 1 : *at + 1;
                    focusByKey(next, chipRef(rows_[shown_[next]].id), now, fx);
                    fx.rename = rowRef(rows_[shown_[next]].id);
                }
            }
        }
        break;
    case Key::Rename:
        if (focusRow_ != 0 && shownIndex(focusRow_)) {
            fx.consumed = true;
            fx.rename = rowRef(focusRow_);
        }
        break;
    }
    endEvent(fx, before);
    return fx;
}

// --- commands -------------------------------------------------------------------------------------

Effects StackModel::stepBaseNow(int steps, TimeMs now) {
    const int b = base();
    int target = b;
    if (steps > 0) {
        for (int s = 0; s < steps; ++s) {
            const auto next = std::upper_bound(c_.detents.begin(), c_.detents.end(), target);
            if (next == c_.detents.end())
                break;
            target = *next;
        }
    } else {
        for (int s = 0; s < -steps; ++s) {
            const auto next = std::lower_bound(c_.detents.begin(), c_.detents.end(), target);
            if (next == c_.detents.begin())
                break;
            target = *(next - 1);
        }
    }
    return setBaseNow(target, now, false);
}

Effects StackModel::setBaseNow(int px, TimeMs now, bool snap) {
    Effects fx;
    if (cap_.active) {
        fx.refused = true;
        return fx;
    }
    int v = std::clamp(px, c_.baseMin, std::max(c_.baseMin, c_.baseMax));
    if (snap)
        for (const int d : c_.detents)
            if (std::abs(v - d) <= c_.detentSnap) {
                v = d;
                break;
            }
    if (v == base())
        return fx;
    jump_ = Jump{};
    if (pointerOverRows())
        anchorPointer();
    else
        anchorFirstVisible();
    view_.baseHeight = v;
    snapEntries();
    replan();
    lastScrollAt_ = now; // rows moved under the pointer: hover waits as after a scroll
    solveScroll();
    rehit(now);
    updateNavScroll();
    fx.windowViewChanged = true;
    bump();
    return fx;
}

Effects StackModel::setBaseHeight(int px, TimeMs now) {
    const auto before = beginEvent();
    advanceTo(now);
    Effects fx = setBaseNow(px, now, true);
    endEvent(fx, before);
    return fx;
}

Effects StackModel::stepBaseHeight(int detents, TimeMs now) {
    const auto before = beginEvent();
    advanceTo(now);
    Effects fx = stepBaseNow(detents, now);
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setAutoZoom(bool on, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.autoZoom == on)
        return fx;
    view_.autoZoom = on;
    focusOwns_ = false;
    if (!on && !cap_.active && maximised_ == 0) {
        // only focus enlarges from now on
        const TrackId target = focusRow_ != 0 && shownIndex(focusRow_) ? focusRow_ : 0;
        if (target != enlarged_) {
            setEnlarged(target, now);
            anchorFirstVisible();
            retarget(now);
            solveScroll();
        }
    }
    fx.windowViewChanged = true;
    bump();
    endEvent(fx, before);
    return fx;
}

void StackModel::layoutChanged(TimeMs now, Effects &fx) {
    replan();
    if (focusItem_.place != ItemPlace::None) {
        // a focused item that is no longer drawn hands the focus to its row
        bool drawn = false;
        if (const auto at = shownIndex(focusRow_)) {
            std::array<ItemRef, kFocusOrder> order{};
            const std::size_t n = focusOrder(*at, order);
            for (std::size_t j = 0; j < n; ++j)
                drawn = drawn || order[j] == focusItem_;
        }
        if (!drawn)
            focusItem_ = chipRef(focusRow_);
    }
    rehit(now);
    updateNavScroll();
    fx.windowViewChanged = true;
    bump();
}

Effects StackModel::setRowLayout(RowLayout layout, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.layout != layout) {
        view_.layout = layout;
        layoutChanged(now, fx);
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setModuleLayout(ModuleLayout layout, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.moduleLayout != layout) {
        view_.moduleLayout = layout;
        layoutChanged(now, fx);
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setShowChain(bool on, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.showChain != on) {
        view_.showChain = on;
        layoutChanged(now, fx);
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setShowDormant(bool on, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.showDormant != on) {
        view_.showDormant = on;
        layoutChanged(now, fx);
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setNavigator(bool shown, bool collapsed, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.navigator != shown || view_.navigatorCollapsed != collapsed) {
        view_.navigator = shown;
        view_.navigatorCollapsed = collapsed;
        layoutChanged(now, fx);
    }
    endEvent(fx, before);
    return fx;
}

void StackModel::membershipChanged(TimeMs now, bool toTop) {
    const std::size_t oldFocusShown = focusRow_ != 0 ? shownIndex(focusRow_).value_or(kNpos) : kNpos;
    if (!cap_.active && !jump_.active && !toTop)
        anchorFirstVisible();
    rebuildShown();
    afterShownChange(now, oldFocusShown);
    if (toTop && !cap_.active) {
        jump_ = Jump{};
        anchor_ = AnchorHold{};
        scroll_ = 0;
        anchorFirstVisible();
    }
    solveScroll();
    rehit(now);
    updateNavScroll();
    bump();
}

Effects StackModel::setPinnedOnly(bool on, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.pinnedOnly != on) {
        view_.pinnedOnly = on;
        membershipChanged(now, false);
        fx.windowViewChanged = true;
    }
    endEvent(fx, before);
    return fx;
}

void StackModel::applySearchChange(TimeMs now) {
    ++stats_.fullPasses;
    for (std::uint32_t i = 0; i < rows_.size(); ++i)
        rows_[i].match = computeMatch(i);
    if (view_.searchMode == SearchMode::Filter) {
        matches_.clear();
        matchAt_ = 0;
        membershipChanged(now, true); // a new query starts at the top
    } else {
        membershipChanged(now, false);
        rebuildMatches();
        jumpToMatch(now);
    }
}

void StackModel::rebuildMatches() {
    matches_.clear();
    matchAt_ = 0;
    if (queryFold_.empty())
        return;
    for (const std::uint32_t rec : shown_)
        if (rows_[rec].match)
            matches_.push_back(rec);
}

void StackModel::jumpToMatch(TimeMs now) {
    if (matches_.empty() || cap_.active)
        return;
    matchAt_ = std::min(matchAt_, matches_.size() - 1);
    startJump(rows_[matches_[matchAt_]].id, static_cast<double>(c_.navJumpTop), c_.navJumpMs, true, now);
    solveScroll();
    updateNavScroll();
}

Effects StackModel::setSearch(std::string_view text, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    normaliseSearchText(text, boundOf(c_.searchMaxBytes), scratch_);
    if (scratch_ == view_.search) {
        endEvent(fx, before);
        return fx;
    }
    view_.search.assign(scratch_);
    foldForSearch(view_.search, queryFold_);
    applySearchChange(now);
    fx.windowViewChanged = true;
    endEvent(fx, before);
    return fx;
}

void StackModel::clearSearch(TimeMs now, Effects &fx) {
    if (view_.search.empty())
        return;
    view_.search.clear();
    queryFold_.clear();
    applySearchChange(now);
    fx.windowViewChanged = true;
}

Effects StackModel::setSearchMode(SearchMode mode, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.searchMode != mode) {
        view_.searchMode = mode;
        applySearchChange(now);
        fx.windowViewChanged = true;
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::searchKey(Key k, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    switch (k) {
    case Key::Up:
    case Key::Down:
        fx.consumed = true;
        if (view_.searchMode == SearchMode::Jump && !matches_.empty()) {
            if (k == Key::Down && matchAt_ + 1 < matches_.size())
                ++matchAt_;
            else if (k == Key::Up && matchAt_ > 0)
                --matchAt_;
            else
                break;
            jumpToMatch(now);
            bump();
        }
        break;
    case Key::Enter: {
        fx.consumed = true;
        std::optional<std::size_t> at;
        if (view_.searchMode == SearchMode::Jump) {
            if (!matches_.empty())
                at = shownOfRec(matches_[std::min(matchAt_, matches_.size() - 1)]);
        } else if (!shown_.empty() && !queryFold_.empty())
            at = std::size_t{0};
        if (at) {
            focusByKey(*at, chipRef(rows_[shown_[*at]].id), now, fx);
            fx.takeFocus = true; // the list takes the keys
        }
        break;
    }
    case Key::Escape:
        fx.consumed = true;
        clearSearch(now, fx);
        fx.escape = EscapeStep::ClearSearch;
        break;
    default:
        break;
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setMaskingRule(MaskingRule rule, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.maskingRule != rule) {
        view_.maskingRule = rule;
        fx.windowViewChanged = true;
        bump();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setRowSpectrum(RowSpectrum spectrum, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.rowSpectrum != spectrum) {
        view_.rowSpectrum = spectrum;
        fx.windowViewChanged = true;
        bump();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setOverlay(bool on, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.overlay != on) {
        view_.overlay = on;
        fx.windowViewChanged = true;
        bump();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setLook(Look look, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (view_.look != look) {
        view_.look = look;
        fx.windowViewChanged = true;
        bump();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setModuleRange(const RangeEntry &entry, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (entry.track <= 0 || (entry.module != 0) == (entry.device != 0) || entry.device < 0 ||
        static_cast<unsigned>(entry.range) > static_cast<unsigned>(DisplayRange::Db30)) {
        fx.refused = true;
        ++stats_.ignoredInputs;
        endEvent(fx, before);
        return fx;
    }
    auto &ranges = view_.moduleRange;
    const auto less = [](const RangeEntry &a, const RangeEntry &b) {
        if (a.track != b.track)
            return a.track < b.track;
        if (a.module != b.module)
            return a.module < b.module;
        return a.device < b.device;
    };
    const auto at = std::lower_bound(ranges.begin(), ranges.end(), entry, less);
    if (at != ranges.end() && !less(entry, *at)) {
        if (at->range == entry.range) {
            endEvent(fx, before);
            return fx;
        }
        at->range = entry.range;
    } else {
        if (ranges.size() >= boundOf(c_.maxRanges)) {
            fx.refused = true;
            endEvent(fx, before);
            return fx;
        }
        ranges.insert(at, entry);
    }
    fx.windowViewChanged = true;
    bump();
    endEvent(fx, before);
    return fx;
}

bool StackModel::setPinned(TrackId id, bool on) {
    auto &list = shared_.pins;
    const auto at = std::lower_bound(list.begin(), list.end(), id);
    const bool there = at != list.end() && *at == id;
    if (on == there)
        return false;
    if (on) {
        if (list.size() >= boundOf(c_.maxPins))
            return false;
        list.insert(at, id);
    } else
        list.erase(at);
    return true;
}

void StackModel::pinsChanged(TimeMs now, std::span<const TrackId> touched, bool bulk) {
    bump();
    if (!view_.pinnedOnly)
        return; // membership does not depend on pins: nothing to rebuild
    if (bulk || touched.size() > kMaxTouched) {
        membershipChanged(now, false);
        return;
    }
    const std::size_t oldFocusShown = focusRow_ != 0 ? shownIndex(focusRow_).value_or(kNpos) : kNpos;
    if (!cap_.active && !jump_.active)
        anchorFirstVisible();
    const auto versionBefore = shownVersion_;
    for (const TrackId id : touched) {
        const std::uint32_t rec = recOf(id);
        if (rec != kNoRec && rows_[rec].valid)
            editShown(rec, member(rec));
    }
    if (shownVersion_ != versionBefore)
        afterShownChange(now, oldFocusShown);
    solveScroll();
    rehit(now);
    updateNavScroll();
}

Effects StackModel::pinNow(const RowRef &ref, Mods mods, TimeMs now) {
    Effects fx;
    if (!live(ref)) {
        fx.refused = true;
        return fx;
    }
    const TrackId id = ref.track;
    if (mods.alt) {
        shared_.pins.clear();
        setPinned(id, true);
        pinAnchor_ = id;
        pinsChanged(now, {}, true);
    } else if (mods.shift && pinAnchor_ != 0 && shownIndex(pinAnchor_) && shownIndex(id)) {
        const std::size_t a = *shownIndex(pinAnchor_), b = *shownIndex(id);
        bool any = false;
        for (std::size_t i = std::min(a, b); i <= std::max(a, b); ++i)
            any = setPinned(rows_[shown_[i]].id, true) || any;
        if (!any)
            return fx;
        pinsChanged(now, {}, true);
    } else {
        const bool on = !pinned(id);
        if (!setPinned(id, on)) {
            fx.refused = true;
            return fx;
        }
        pinAnchor_ = id;
        const std::array<TrackId, 1> touched{id};
        pinsChanged(now, touched, false);
    }
    fx.sharedViewChanged = true;
    return fx;
}

Effects StackModel::pin(const RowRef &ref, Mods mods, TimeMs now) {
    const auto before = beginEvent();
    advanceTo(now);
    Effects fx = pinNow(ref, mods, now);
    endEvent(fx, before);
    return fx;
}

Effects StackModel::pinSameColour(const RowRef &ref, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const Rec *r = live(ref) ? recFor(ref.track) : nullptr;
    if (r == nullptr) {
        fx.refused = true;
        endEvent(fx, before);
        return fx;
    }
    bool any = false;
    if (r->colour < 0)
        any = setPinned(r->id, true); // no colour of its own: only this track
    else {
        ++stats_.fullPasses;
        const std::int32_t colour = r->colour;
        for (const auto &other : rows_)
            if (inScope(other) && other.colour == colour)
                any = setPinned(other.id, true) || any;
    }
    if (any) {
        pinsChanged(now, {}, true);
        fx.sharedViewChanged = true;
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::storePinSet(std::string_view name, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (name.empty() || name.size() > boundOf(c_.maxPinSetName)) {
        fx.refused = true;
        endEvent(fx, before);
        return fx;
    }
    auto &sets = shared_.pinSets;
    auto at = std::find_if(sets.begin(), sets.end(), [&](const PinSet &s) { return s.name == name; });
    if (at == sets.end()) {
        if (sets.size() >= boundOf(c_.maxPinSets)) {
            fx.refused = true;
            endEvent(fx, before);
            return fx;
        }
        sets.push_back(PinSet{std::string(name), {}});
        at = sets.end() - 1;
    }
    at->ids = shared_.pins;
    fx.sharedViewChanged = true;
    bump();
    endEvent(fx, before);
    return fx;
}

Effects StackModel::recallPinSet(std::string_view name, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const auto &sets = shared_.pinSets;
    const auto at = std::find_if(sets.begin(), sets.end(), [&](const PinSet &s) { return s.name == name; });
    if (at == sets.end()) {
        fx.refused = true;
        endEvent(fx, before);
        return fx;
    }
    if (shared_.pins != at->ids) {
        shared_.pins.assign(at->ids.begin(), at->ids.end());
        pinsChanged(now, {}, true);
        fx.sharedViewChanged = true;
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::deletePinSet(std::string_view name, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    auto &sets = shared_.pinSets;
    const auto at = std::find_if(sets.begin(), sets.end(), [&](const PinSet &s) { return s.name == name; });
    if (at == sets.end())
        fx.refused = true;
    else {
        sets.erase(at);
        fx.sharedViewChanged = true;
        bump();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::setComparePair(TrackId a, TrackId b, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (!(a > 0 && b >= 0 && a != b)) {
        fx.refused = true;
        endEvent(fx, before);
        return fx;
    }
    if (!(shared_.comparePair == ComparePair{a, b})) {
        shared_.comparePair = {a, b};
        fx.sharedViewChanged = true;
        bump();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::clearComparePair(TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (!(shared_.comparePair == ComparePair{})) {
        shared_.comparePair = {};
        fx.sharedViewChanged = true;
        bump();
    }
    pairPending_ = 0;
    endEvent(fx, before);
    return fx;
}

Effects StackModel::maskingNow(const RowRef &ref) {
    Effects fx;
    if (!live(ref)) {
        fx.refused = true;
        return fx;
    }
    const TrackId other = focusRow_ != ref.track ? focusRow_ : 0;
    const ComparePair pair{ref.track, other};
    if (!(shared_.comparePair == pair)) {
        shared_.comparePair = pair;
        fx.sharedViewChanged = true;
        bump();
    }
    return fx;
}

Effects StackModel::maskingButton(const RowRef &ref, TimeMs now) {
    const auto before = beginEvent();
    advanceTo(now);
    Effects fx = maskingNow(ref);
    endEvent(fx, before);
    return fx;
}

Effects StackModel::maximise(const RowRef &ref, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const auto at = live(ref) ? shownIndex(ref.track) : std::nullopt;
    if (!at || cap_.active)
        fx.refused = true;
    else if (maximised_ != ref.track)
        doMaximise(*at, now);
    endEvent(fx, before);
    return fx;
}

Effects StackModel::restore(TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (cap_.active)
        fx.refused = true;
    else
        doRestore(now);
    endEvent(fx, before);
    return fx;
}

Effects StackModel::scrollLane(const RowRef &ref, int items, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const std::uint32_t rec = live(ref) ? recOf(ref.track) : kNoRec;
    if (rec == kNoRec || (cap_.active && cap_.target.row.track == ref.track)) {
        fx.refused = true;
        endEvent(fx, before);
        return fx;
    }
    Rec &r = rows_[rec];
    const std::int64_t first = static_cast<std::int64_t>(resolveLaneFirst(r)) + items;
    setLaneFirst(r, static_cast<std::uint32_t>(std::max<std::int64_t>(0, first)), now, true);
    endEvent(fx, before);
    return fx;
}

Effects StackModel::toggleFoldNow(const RowRef &ref, TimeMs now) {
    Effects fx;
    const Rec *r = live(ref) ? recFor(ref.track) : nullptr;
    if (r == nullptr || !r->group) {
        fx.refused = true;
        return fx;
    }
    auto &folded = view_.folded;
    const auto at = std::lower_bound(folded.begin(), folded.end(), ref.track);
    if (at != folded.end() && *at == ref.track)
        folded.erase(at);
    else {
        if (folded.size() >= boundOf(c_.maxFolded)) {
            fx.refused = true;
            return fx;
        }
        folded.insert(at, ref.track);
    }
    const std::size_t oldFocusShown = focusRow_ != 0 ? shownIndex(focusRow_).value_or(kNpos) : kNpos;
    if (!cap_.active) {
        jump_ = Jump{};
        anchorRow(ref.track, true); // the group keeps its place
    }
    ++stats_.fullPasses;
    for (std::uint32_t i = 0; i < rows_.size(); ++i)
        rows_[i].underFold = computeUnderFold(i);
    rebuildShown();
    afterShownChange(now, oldFocusShown);
    solveScroll();
    rehit(now);
    updateNavScroll();
    fx.windowViewChanged = true;
    bump();
    return fx;
}

Effects StackModel::toggleFold(const RowRef &group, TimeMs now) {
    const auto before = beginEvent();
    advanceTo(now);
    Effects fx = toggleFoldNow(group, now);
    endEvent(fx, before);
    return fx;
}

Effects StackModel::focusRow(const RowRef &ref, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const auto at = live(ref) ? shownIndex(ref.track) : std::nullopt;
    if (!at)
        fx.refused = true;
    else {
        focusByKey(*at, chipRef(ref.track), now, fx);
        fx.takeFocus = true;
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::focusItem(const ItemRef &ref, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const auto at = live(ref) ? shownIndex(ref.row.track) : std::nullopt;
    if (!at)
        fx.refused = true;
    else {
        focusByKey(*at, ref, now, fx);
        fx.takeFocus = true;
        if (ref.place == ItemPlace::Chain && !cap_.active) {
            Rec &r = rows_[shown_[*at]];
            for (std::uint32_t j = 0; j < r.items.size(); ++j)
                if (r.items[j].device == ref.device)
                    revealLaneItem(r, j, *at, now);
        }
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::reveal(const RowRef &ref, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const auto at = live(ref) ? shownIndex(ref.track) : std::nullopt;
    if (!at || cap_.active)
        fx.refused = true;
    else {
        startJump(ref.track, revealGoal(*at), c_.heightAnimMs, true, now);
        solveScroll();
        updateNavScroll();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::scrollTo(double contentY, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (cap_.active)
        fx.refused = true;
    else {
        userScroll(contentY, now);
        updateNavScroll();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::scrollBy(double dy, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (cap_.active)
        fx.refused = true;
    else {
        userScroll(scroll_ + dy, now);
        updateNavScroll();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::openFromChip(TrackId track, ItemKey landOn, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    const auto at = shownIndex(track);
    if (!at) {
        fx.refused = true;
        endEvent(fx, before);
        return fx;
    }
    ItemRef item = chipRef(track);
    if (landOn.module != 0) {
        const ItemRef m = moduleRef(track, landOn.module);
        if (m.gen != 0)
            item = m;
    } else if (landOn.device != 0) {
        const ItemRef d = chainRef(track, landOn.device);
        if (d.gen != 0)
            item = d;
    }
    openedFrom_ = track;
    focusRow_ = track;
    focusItem_ = item;
    fx.select = track;
    fx.takeFocus = true;
    bump();
    if (!cap_.active) {
        if (maximised_ == 0) {
            setEnlarged(track, now);
            landed_ = track;
            landingEnd_ = now + c_.landingHoldMs;
        }
        focusOwns_ = false;
        const double goal = c_.landingDen > 0 ? std::floor(listH_ * static_cast<double>(c_.landingNum) /
                                                           static_cast<double>(c_.landingDen))
                                              : 0.0;
        startJump(track, goal, 0, true, now); // there at once, then held while the row grows
        retarget(now);
        solveScroll();
        if (item.place == ItemPlace::Chain) {
            Rec &r = rows_[shown_[*at]];
            for (std::uint32_t j = 0; j < r.items.size(); ++j)
                if (r.items[j].device == item.device)
                    revealLaneItem(r, j, *at, now);
        }
        rehit(now); // the rows moved under a resting pointer: its dwell starts again
        updateNavScroll();
    }
    endEvent(fx, before);
    return fx;
}

Effects StackModel::clearOpenedFrom(TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    openedFrom_ = 0;
    endEvent(fx, before);
    return fx;
}

// --- the capture ----------------------------------------------------------------------------------

CaptureResult StackModel::beginCapture(CaptureKind kind, const ItemRef &target, TimeMs now) {
    CaptureResult result;
    const auto before = beginEvent();
    Effects fx;
    advanceTo(now);
    if (cap_.active)
        result.refusal = CaptureRefusal::Busy;
    else if (!live(target.row))
        result.refusal = CaptureRefusal::Stale;
    else if (!shownIndex(target.row.track))
        result.refusal = CaptureRefusal::NotShown;
    else if (target.place == ItemPlace::Module) {
        const Rec *r = recFor(target.row.track);
        std::size_t k = kNpos;
        for (std::size_t j = 0; j < kindCount_; ++j)
            if (kinds_[j].key == target.module && target.module != 0)
                k = j;
        if (k == kNpos)
            result.refusal = CaptureRefusal::Stale;
        else if (r->modules[k].state == ModuleState::Absent)
            result.refusal = CaptureRefusal::Unavailable;
        else if (r->modules[k].gen != target.gen)
            result.refusal = CaptureRefusal::Stale;
        else if (cellAt(*shownIndex(target.row.track), k).content == CellContent::Hidden)
            result.refusal = CaptureRefusal::NotShown;
    } else if (target.place == ItemPlace::Chain && !live(target))
        result.refusal = CaptureRefusal::Stale;
    if (result.refusal != CaptureRefusal::None) {
        endEvent(fx, before);
        return result;
    }
    const std::size_t shown = *shownIndex(target.row.track);
    // everything in flight stops where it is
    if (animActive_) {
        const double k = animE();
        for (std::size_t j = 0; j < entryCount_; ++j) {
            Entry &e = entries_[j];
            e.hFrom = e.hTo = e.hFrom + (e.hTo - e.hFrom) * k;
            e.gFrom = e.gTo = e.gFrom + (e.gTo - e.gFrom) * k;
        }
        animActive_ = false;
    }
    jump_ = Jump{};
    revealPending_ = false;
    const double top = viewTop(shown), h = heightAt(shown);
    freezeCapturedRow(shown, top, h);
    for (auto &a : laneAnims_)
        a = LaneAnim{};
    cap_.active = true;
    cap_.token = nextToken_++;
    if (nextToken_ == 0)
        nextToken_ = 1;
    cap_.kind = kind;
    cap_.target = target;
    cap_.viewTop = top;
    frozenPlan_ = plan_;
    planFrozen_ = true;
    result.ref.token = cap_.token;
    result.ref.kind = kind;
    result.ref.target = target;
    bump();
    endEvent(fx, before);
    return result;
}

void StackModel::freezeCapturedRow(std::size_t shown, double top, double h) {
    cap_.lane = laneAt(shown, top, h);
    cap_.lane.frozen = true;
    frozenItems_.clear();
    struct Collect {
        std::vector<ItemGeo> *out;
        void operator()(const ItemGeo &g) const { out->push_back(g); }
    } collect{&frozenItems_};
    layoutLane(shown, top, h, nullptr, detail::sinkFor<ItemGeo>(collect));
    for (std::size_t k = 0; k < kMaxModules; ++k)
        cap_.cells[k] = k < kindCount_ ? cellAt(shown, k) : CellGeo{};
}

CaptureStatus StackModel::captureStatus(const CaptureRef &ref) const noexcept {
    if (cap_.active && ref.token == cap_.token)
        return CaptureStatus::Live;
    if (ref.token != 0 && ref.token == lastToken_)
        return lastStatus_;
    return CaptureStatus::Ended;
}

CaptureStatus StackModel::captureStatus(const CaptureRef &ref, std::uint64_t currentGeneration) const noexcept {
    const CaptureStatus s = captureStatus(ref);
    if (s == CaptureStatus::Live && stale(currentGeneration))
        return CaptureStatus::Behind;
    return s;
}

std::optional<CaptureRef> StackModel::capture() const noexcept {
    if (!cap_.active)
        return std::nullopt;
    CaptureRef ref;
    ref.token = cap_.token;
    ref.kind = cap_.kind;
    ref.target = cap_.target;
    return ref;
}

Effects StackModel::endCapture(const CaptureRef &ref, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (cap_.active && ref.token == cap_.token)
        finishCapture(now, CaptureStatus::Ended);
    endEvent(fx, before);
    return fx;
}

void StackModel::finishCapture(TimeMs now, CaptureStatus status) {
    lastToken_ = cap_.token;
    lastStatus_ = status;
    const TrackId rowId = cap_.target.row.track;
    cap_ = Capture{};
    planFrozen_ = false;
    replan();
    const std::size_t oldFocusShown = focusRow_ != 0 ? shownIndex(focusRow_).value_or(kNpos) : kNpos;
    // the filters apply again: a row kept only for the capture leaves now
    const std::uint32_t rec = recOf(rowId);
    const auto versionBefore = shownVersion_;
    if (rec != kNoRec)
        editShown(rec, rows_[rec].valid && memberPlain(rec));
    if (shownVersion_ != versionBefore)
        afterShownChange(now, oldFocusShown);
    captureEndAt_ = now;
    // the scroll comes back inside the list; the heights cut by the capture finish
    jump_ = Jump{};
    if (shownIndex(rowId))
        anchorRow(rowId, true);
    else
        anchorFirstVisible();
    retarget(now);
    solveScroll();
    if (!inside_)
        leaveAt_ = now + c_.leaveMs;
    rehit(now);
    updateNavScroll();
    bump();
}

void StackModel::checkCapture(TimeMs now, Effects &fx) {
    if (!cap_.active)
        return;
    const ItemRef &t = cap_.target;
    const std::uint32_t rec = recOf(t.row.track);
    CaptureStatus lost = CaptureStatus::Live;
    if (rec == kNoRec || !rows_[rec].valid)
        lost = CaptureStatus::RowRemoved;
    else if (rows_[rec].gen != t.row.gen)
        lost = CaptureStatus::RowReplaced;
    else if (t.place == ItemPlace::Chain) {
        lost = CaptureStatus::ItemRemoved;
        for (const auto &item : rows_[rec].items)
            if (item.device == t.device)
                lost = item.gen == t.gen ? CaptureStatus::Live : CaptureStatus::ItemReplaced;
    } else if (t.place == ItemPlace::Module) {
        lost = CaptureStatus::ItemRemoved;
        for (std::size_t k = 0; k < kindCount_; ++k)
            if (kinds_[k].key == t.module && t.module != 0) {
                const ModuleRec &m = rows_[rec].modules[k];
                if (m.state != ModuleState::Absent)
                    lost = m.gen == t.gen ? CaptureStatus::Live : CaptureStatus::ItemReplaced;
            }
    }
    if (lost == CaptureStatus::Live)
        return;
    finishCapture(now, lost);
    fx.captureLost = lost;
}

// --- refs -----------------------------------------------------------------------------------------

bool StackModel::live(const RowRef &ref) const noexcept {
    const Rec *r = ref.gen != 0 ? recFor(ref.track) : nullptr;
    return r != nullptr && r->gen == ref.gen;
}

bool StackModel::live(const ItemRef &ref) const noexcept {
    if (!live(ref.row))
        return false;
    const Rec &r = *recFor(ref.row.track);
    switch (ref.place) {
    case ItemPlace::None:
        return true;
    case ItemPlace::Chain:
        for (const auto &item : r.items)
            if (item.device == ref.device)
                return item.gen == ref.gen && ref.gen != 0;
        return false;
    case ItemPlace::Module:
        for (std::size_t k = 0; k < kindCount_; ++k)
            if (kinds_[k].key == ref.module && ref.module != 0)
                return r.modules[k].state != ModuleState::Absent && r.modules[k].gen == ref.gen && ref.gen != 0;
        return false;
    }
    return false;
}

std::optional<Resolved> StackModel::resolve(const ItemRef &ref) const noexcept {
    if (!live(ref) || ref.place == ItemPlace::None)
        return std::nullopt;
    const Rec &r = *recFor(ref.row.track);
    Resolved out;
    if (ref.place == ItemPlace::Chain) {
        for (const auto &item : r.items)
            if (item.device == ref.device) {
                out.device = item.device;
                out.identity = item.identity;
                out.state = ModuleState::Engaged;
                out.cls = item.cls;
            }
        return out;
    }
    for (std::size_t k = 0; k < kindCount_; ++k)
        if (kinds_[k].key == ref.module) {
            out.device = r.modules[k].device;
            out.identity = r.modules[k].instance;
            out.state = r.modules[k].state;
            out.cls = kinds_[k].widthClass;
        }
    return out;
}

RowRef StackModel::rowRef(TrackId id) const noexcept {
    const Rec *r = recFor(id);
    return r == nullptr ? RowRef{} : RowRef{r->id, r->gen};
}

ItemRef StackModel::chainRef(TrackId id, DeviceId device) const noexcept {
    ItemRef ref;
    const Rec *r = recFor(id);
    if (r == nullptr)
        return ref;
    for (const auto &item : r->items)
        if (item.device == device) {
            ref.row = {r->id, r->gen};
            ref.place = ItemPlace::Chain;
            ref.device = device;
            ref.gen = item.gen;
        }
    return ref;
}

ItemRef StackModel::moduleRef(TrackId id, std::uint32_t kindKey) const noexcept {
    ItemRef ref;
    const Rec *r = recFor(id);
    if (r == nullptr || kindKey == 0)
        return ref;
    for (std::size_t k = 0; k < kindCount_; ++k)
        if (kinds_[k].key == kindKey && r->modules[k].state != ModuleState::Absent) {
            ref.row = {r->id, r->gen};
            ref.place = ItemPlace::Module;
            ref.module = kindKey;
            ref.device = r->modules[k].device;
            ref.gen = r->modules[k].gen;
        }
    return ref;
}

ItemRef StackModel::cellRef(const Rec &r, std::size_t k) const noexcept {
    ItemRef ref;
    if (k >= kindCount_ || r.modules[k].state == ModuleState::Absent || kinds_[k].key == 0)
        return ref;
    ref.row = {r.id, r.gen};
    ref.place = ItemPlace::Module;
    ref.module = kinds_[k].key;
    ref.device = r.modules[k].device;
    ref.gen = r.modules[k].gen;
    return ref;
}

// --- queries --------------------------------------------------------------------------------------

TrackId StackModel::shownTrack(std::size_t shown) const noexcept {
    return shown < shown_.size() ? rows_[shown_[shown]].id : 0;
}

std::optional<std::size_t> StackModel::shownIndex(TrackId id) const noexcept {
    const std::uint32_t rec = recOf(id);
    if (rec == kNoRec || !rows_[rec].valid)
        return std::nullopt;
    return shownOfRec(rec);
}

std::string_view StackModel::trackName(TrackId id) const noexcept {
    const Rec *r = recFor(id);
    return r == nullptr ? std::string_view() : std::string_view(r->name);
}

std::uint32_t StackModel::moduleKey(std::size_t kindIndex) const noexcept {
    return kindIndex < kindCount_ ? kinds_[kindIndex].key : 0;
}

bool StackModel::isCapturedShown(std::size_t shown) const noexcept {
    return cap_.active && shown < shown_.size() && rows_[shown_[shown]].id == cap_.target.row.track;
}

RowGeo StackModel::rowGeoAt(std::size_t shown, double top, double height, double target, bool animating,
                            std::optional<std::size_t> pointerIdx) const noexcept {
    const Rec &r = rows_[shown_[shown]];
    RowGeo g;
    g.ref = {r.id, r.gen};
    g.shown = shown;
    g.top = top;
    g.height = height;
    g.targetHeight = target;
    const bool maxed = r.id == maximised_;
    g.tier = tierFor(height, maxed && !animating && height == target, c_);
    g.targetTier = tierFor(target, maxed, c_);
    g.depth = r.depth;
    g.colour = r.colour;
    g.roles = r.roles;
    g.group = r.group;
    g.folded = r.group && std::binary_search(view_.folded.begin(), view_.folded.end(), r.id);
    g.animating = animating;
    g.enlarged = r.id == enlarged_;
    g.maximised = maxed;
    g.focused = r.id == focusRow_;
    g.selected = r.id == selection_;
    g.pinned = pinned(r.id);
    g.pointer = pointerIdx && *pointerIdx == shown;
    g.captured = cap_.active && r.id == cap_.target.row.track;
    g.retained = g.captured && !memberPlain(shown_[shown]);
    g.jumpTarget = view_.searchMode == SearchMode::Jump && !matches_.empty() &&
                   matches_[std::min(matchAt_, matches_.size() - 1)] == shown_[shown];
    g.pairA = shared_.comparePair.a == r.id;
    g.pairB = shared_.comparePair.b == r.id && r.id != 0;
    return g;
}

RowGeo StackModel::row(std::size_t shown) const noexcept {
    if (shown >= shown_.size())
        return RowGeo{};
    ++stats_.rowEvaluations;
    const Entry *e = entryAt(shown);
    const double k = animE();
    const double h = e != nullptr ? e->hFrom + (e->hTo - e->hFrom) * k : static_cast<double>(base());
    const double target = e != nullptr ? e->hTo : static_cast<double>(base());
    const bool animating = e != nullptr && animActive_ && e->hFrom != e->hTo;
    return rowGeoAt(shown, viewTop(shown), h, target, animating, pointerRowIndex());
}

void StackModel::walkRows(Span span, detail::Sink<RowGeo> sink) const {
    const std::size_t count = shown_.size();
    if (span.empty || count == 0 || span.first >= count)
        return;
    const std::size_t last = std::min(span.last, count - 1);
    const double k = animE(), b = base(), g = g0();
    std::size_t j = 0;
    double top = static_cast<double>(span.first) * (b + g);
    for (; j < entryCount_ && entries_[j].shown < span.first; ++j) {
        ++stats_.entryVisits;
        const Entry &e = entries_[j];
        top += (e.hFrom + (e.hTo - e.hFrom) * k - b) + (e.gFrom + (e.gTo - e.gFrom) * k - g);
    }
    top -= scroll_;
    const auto pointerIdx = pointerRowIndex();
    for (std::size_t i = span.first; i <= last; ++i) {
        double h = b, gg = g, target = b;
        bool animating = false;
        if (j < entryCount_ && entries_[j].shown == i) {
            ++stats_.entryVisits;
            const Entry &e = entries_[j++];
            h = e.hFrom + (e.hTo - e.hFrom) * k;
            gg = e.gFrom + (e.gTo - e.gFrom) * k;
            target = e.hTo;
            animating = animActive_ && e.hFrom != e.hTo;
        }
        ++stats_.rowEvaluations;
        sink(rowGeoAt(i, top, h, target, animating, pointerIdx));
        top += h + gg;
    }
}

namespace {
double laneAnimPosition(double from, double to, double e) noexcept { return from + (to - from) * e; }
} // namespace

void StackModel::layoutLane(std::size_t shown, double top, double h, LaneGeo *geo,
                            detail::Sink<ItemGeo> emit) const {
    const auto &p = plan();
    const Rec &r = rows_[shown_[shown]];
    LaneGeo L;
    L.mode = p.laneMode;
    L.x = p.laneX;
    L.width = p.laneW;
    L.count = static_cast<std::uint32_t>(r.items.size());
    if (L.mode == LaneMode::None || r.items.empty()) {
        if (geo != nullptr)
            *geo = L;
        return;
    }
    const bool pills = L.mode == LaneMode::Pills;
    const double gap = pills ? c_.trayPillGap : c_.itemGap;
    const Tier tier = tierFor(h, false, c_);
    const auto widthOf = [&](std::size_t j) {
        const auto &item = r.items[j];
        if (pills || tier < c_.foldBelow[classAt(item.cls)])
            return static_cast<double>(c_.pill);
        return static_cast<double>(chainItemWidth(item.cls, h, c_));
    };
    const std::uint32_t first = resolveLaneFirst(r);
    L.first = first;
    double pos = static_cast<double>(first);
    bool scrolling = false;
    for (const auto &a : laneAnims_)
        if (a.row == r.id && c_.laneScrollMs > 0) {
            const double u = static_cast<double>(now_ - a.start) / static_cast<double>(c_.laneScrollMs);
            pos = laneAnimPosition(a.from, a.to, ease(u));
            scrolling = u < 1;
        }
    const std::size_t n = r.items.size();
    const auto whole = static_cast<std::size_t>(std::floor(std::max(0.0, pos)));
    double offset = 0;
    for (std::size_t j = 0; j < whole && j < n; ++j)
        offset += widthOf(j) + gap;
    if (whole < n)
        offset += (pos - static_cast<double>(whole)) * (widthOf(whole) + gap);
    L.offset = offset;
    const double laneRight = L.x + L.width;
    const double chevY = top + std::floor((h - static_cast<double>(c_.chevronH)) / 2);
    std::size_t drawnFirst = n, drawnCount = 0;
    double x = L.x - offset;
    double lastRight = 0;
    bool clippedOnly = false;
    for (std::size_t j = 0; j < n; ++j) {
        const double w = widthOf(j);
        const double left = x, right = x + w;
        x += w + gap;
        bool draw = false, clipped = false;
        if (scrolling) {
            draw = right > L.x + kEps && left < laneRight - kEps;
            clipped = draw && (left < L.x - kEps || right > laneRight + kEps);
        } else if (j >= first) {
            if (right <= laneRight + kEps && drawnCount == j - first) // a run of fully visible items from `first`
                draw = true;
            else if (j == first && drawnCount == 0 && !pills && L.width >= c_.laneMinForItems - kEps) {
                draw = true; // not even the first fits: drawn cut to the lane
                clipped = true;
                clippedOnly = true;
            }
        }
        if (!draw)
            continue;
        ++stats_.itemEvaluations;
        if (drawnCount == 0)
            drawnFirst = j;
        ++drawnCount;
        lastRight = clipped ? std::min(right, laneRight) : right;
        ItemGeo g;
        g.ref.row = {r.id, r.gen};
        g.ref.place = ItemPlace::Chain;
        g.ref.device = r.items[j].device;
        g.ref.gen = r.items[j].gen;
        g.cls = r.items[j].cls;
        g.index = static_cast<std::uint32_t>(j);
        g.x = left;
        g.width = w;
        g.tier = tier;
        g.pill = pills || tier < c_.foldBelow[classAt(r.items[j].cls)];
        g.clipped = clipped;
        g.focused = focusItem_.place == ItemPlace::Chain && focusItem_.row.track == r.id &&
                    focusItem_.device == g.ref.device;
        if (emit.call != nullptr)
            emit(g);
    }
    if (drawnCount > 0) {
        L.drawnFirst = static_cast<std::uint32_t>(drawnFirst);
        L.drawnCount = static_cast<std::uint32_t>(drawnCount);
    }
    if (!pills) {
        const std::size_t end = drawnCount > 0 ? drawnFirst + drawnCount : first;
        L.right = end < n;
        if (L.right) {
            const double cx = (drawnCount > 0 ? (clippedOnly ? laneRight : lastRight) : laneRight) - c_.chevronW;
            L.rightChevron = {cx, chevY, static_cast<double>(c_.chevronW), static_cast<double>(c_.chevronH)};
        }
        L.left = first > 0 || pos > kEps;
        if (L.left)
            L.leftChevron = {L.x, chevY, static_cast<double>(c_.chevronW), static_cast<double>(c_.chevronH)};
    }
    if (geo != nullptr)
        *geo = L;
}

LaneGeo StackModel::laneAt(std::size_t shown, double top, double h) const noexcept {
    if (isCapturedShown(shown))
        return cap_.lane;
    LaneGeo L;
    layoutLane(shown, top, h, &L, detail::Sink<ItemGeo>{});
    return L;
}

LaneGeo StackModel::lane(std::size_t shown) const noexcept {
    if (shown >= shown_.size())
        return LaneGeo{};
    return laneAt(shown, viewTop(shown), heightAt(shown));
}

void StackModel::walkItems(std::size_t shown, detail::Sink<ItemGeo> sink) const {
    if (shown >= shown_.size())
        return;
    if (isCapturedShown(shown)) {
        for (const auto &g : frozenItems_) {
            ++stats_.itemEvaluations;
            sink(g);
        }
        return;
    }
    layoutLane(shown, viewTop(shown), heightAt(shown), nullptr, sink);
}

CellGeo StackModel::cellAt(std::size_t shown, std::size_t k) const noexcept {
    CellGeo cell;
    if (shown >= shown_.size() || k >= kindCount_)
        return cell;
    ++stats_.itemEvaluations;
    const Rec &r = rows_[shown_[shown]];
    const auto &p = plan();
    const KindRec &kind = kinds_[k];
    cell.kindIndex = k;
    cell.key = kind.key;
    const ModuleState state = r.modules[k].state;
    const auto contentFor = [&](ModulePlace place, double colW, double &w, bool &pill) {
        w = colW;
        pill = false;
        if (state == ModuleState::Engaged)
            return CellContent::Item;
        if (state == ModuleState::Absent) {
            w = c_.pill;
            pill = true;
            return CellContent::Refused;
        }
        if (!view_.showDormant)
            return place == ModulePlace::Column && p.layout == ModuleLayout::Columns ? CellContent::Blank
                                                                                      : CellContent::Hidden;
        if (place == ModulePlace::Column && kind.dormantPreview)
            return CellContent::DormantPreview;
        w = c_.pill;
        pill = true;
        return CellContent::DormantPill;
    };
    if (isCapturedShown(shown)) {
        // the captured row keeps its cells where they were; what they show follows the module
        for (std::size_t j = 0; j < kMaxModules; ++j)
            if (cap_.cells[j].key == kind.key && kind.key != 0) {
                cell = cap_.cells[j];
                cell.kindIndex = k;
                if (cell.content != CellContent::Hidden) {
                    double w = cell.width;
                    bool pill = cell.pill;
                    const double frozenW = cell.width;
                    cell.content = contentFor(cell.place == ModulePlace::Card ? ModulePlace::Column : cell.place,
                                              frozenW, w, pill);
                    if (cell.content == CellContent::Hidden)
                        cell.content = CellContent::Blank;
                    cell.width = std::min(w, frozenW);
                    cell.pill = pill || cell.width <= static_cast<double>(c_.pill) + kEps;
                }
                cell.ref = cellRef(r, k);
                cell.focused = focusItem_.place == ItemPlace::Module && focusItem_.row.track == r.id &&
                               focusItem_.module == kind.key;
                return cell;
            }
        return cell;
    }
    if (p.place[k] == ModulePlace::Hidden || kind.key == 0)
        return cell;
    if (p.layout == ModuleLayout::Columns) {
        cell.place = p.place[k];
        double w = 0;
        bool pill = false;
        if (p.place[k] == ModulePlace::Column)
            cell.content = contentFor(ModulePlace::Column, p.w[k], w, pill);
        else {
            cell.content = contentFor(ModulePlace::Tray, c_.pill, w, pill);
            w = c_.pill;
            pill = true;
        }
        cell.x = p.x[k];
        cell.width = w;
        cell.pill = pill;
    } else {
        // the combined card: the modules in the input's order, each at its width, packed
        double x = p.processorX;
        for (std::size_t j = 0; j < kindCount_; ++j) {
            if (p.place[j] == ModulePlace::Hidden || kinds_[j].key == 0)
                continue;
            const ModuleState sj = r.modules[j].state;
            double w = 0;
            bool pill = false;
            CellContent content = CellContent::Hidden;
            if (sj == ModuleState::Engaged) {
                content = CellContent::Item;
                w = p.place[j] == ModulePlace::Column ? p.w[j] : c_.pill;
                pill = p.place[j] != ModulePlace::Column;
            } else if (sj == ModuleState::Absent) {
                content = CellContent::Refused;
                w = c_.pill;
                pill = true;
            } else if (view_.showDormant) {
                if (p.place[j] == ModulePlace::Column && kinds_[j].dormantPreview) {
                    content = CellContent::DormantPreview;
                    w = p.w[j];
                } else {
                    content = CellContent::DormantPill;
                    w = c_.pill;
                    pill = true;
                }
            }
            if (j == k) {
                cell.place = ModulePlace::Card;
                cell.content = content;
                cell.x = x;
                cell.width = content == CellContent::Hidden ? 0 : w;
                cell.pill = pill;
                break;
            }
            if (content != CellContent::Hidden)
                x += w + c_.cardGap;
        }
    }
    cell.ref = cellRef(r, k);
    cell.focused = focusItem_.place == ItemPlace::Module && focusItem_.row.track == r.id &&
                   focusItem_.module == kind.key;
    return cell;
}

CellGeo StackModel::moduleCell(std::size_t shown, std::size_t kindIndex) const noexcept {
    return cellAt(shown, kindIndex);
}

void StackModel::walkCells(std::size_t shown, detail::Sink<CellGeo> sink) const {
    if (shown >= shown_.size())
        return;
    for (std::size_t k = 0; k < kindCount_; ++k) {
        const CellGeo cell = cellAt(shown, k);
        if (cell.content != CellContent::Hidden)
            sink(cell);
    }
}

ChipGeo StackModel::chipAt(std::size_t shown, double top, double h) const noexcept {
    const Rec &r = rows_[shown_[shown]];
    ChipGeo g;
    const double inset = c_.chipInset, button = c_.chipButton, bgap = c_.chipButtonGap;
    const double above = std::floor((h - static_cast<double>(c_.chipHeight)) / 2);
    g.buttonsAbove = above >= button + bgap + static_cast<double>(c_.chipButtonClear);
    const double beside = g.buttonsAbove ? 0.0 : 3 * (button + bgap);
    const double room = static_cast<double>(c_.chipColumn) - 2 * inset - beside - static_cast<double>(c_.chipMinWidth);
    const double indent = std::min(static_cast<double>(r.depth) * c_.chipIndent, std::max(0.0, room));
    const double x = inset + indent, y = top + above;
    const double w = static_cast<double>(c_.chipColumn) - 2 * inset - indent - beside;
    g.chip = {x, y, w, static_cast<double>(c_.chipHeight)};
    const auto buttonAt = [&](int k) {
        if (g.buttonsAbove)
            return Rect{x + k * (button + bgap), y - button - bgap, button, button};
        return Rect{x + w + bgap + k * (button + bgap), y + c_.chipButtonDy, button, button};
    };
    g.pin = buttonAt(0);
    g.masking = buttonAt(1);
    g.kick = buttonAt(2);
    if (r.group)
        g.fold = {x + c_.foldInsetX, y + c_.foldInsetY, static_cast<double>(c_.foldSize), static_cast<double>(c_.foldSize)};
    return g;
}

ChipGeo StackModel::chip(std::size_t shown) const noexcept {
    if (shown >= shown_.size())
        return ChipGeo{};
    return chipAt(shown, viewTop(shown), heightAt(shown));
}

Hit StackModel::hitRow(std::size_t shown, double x, double y, double top, double h) const noexcept {
    const Rec &r = rows_[shown_[shown]];
    Hit hit;
    hit.row = {r.id, r.gen};
    hit.item = chipRef(r.id);
    hit.shown = shown;
    hit.tier = tierFor(h, r.id == maximised_ && !animActive_, c_);
    hit.rect = {0, top, plan().rowsRight, h};
    hit.part = HitPart::RowSpace;
    if (x < static_cast<double>(c_.chipColumn)) {
        const ChipGeo g = chipAt(shown, top, h);
        if (g.pin.contains(x, y)) {
            hit.part = HitPart::ChipPin;
            hit.rect = g.pin;
        } else if (g.masking.contains(x, y)) {
            hit.part = HitPart::ChipMasking;
            hit.rect = g.masking;
        } else if (g.kick.contains(x, y)) {
            hit.part = HitPart::ChipKick;
            hit.rect = g.kick;
        } else if (r.group && g.fold.contains(x, y)) {
            hit.part = HitPart::ChipFold;
            hit.rect = g.fold;
        } else if (g.chip.contains(x, y)) {
            hit.part = HitPart::Chip;
            hit.rect = g.chip;
        }
        return hit;
    }
    const LaneGeo L = laneAt(shown, top, h);
    if (L.right && L.rightChevron.contains(x, y)) {
        hit.part = HitPart::ChevronRight;
        hit.rect = L.rightChevron;
        return hit;
    }
    if (L.left && L.leftChevron.contains(x, y)) {
        hit.part = HitPart::ChevronLeft;
        hit.rect = L.leftChevron;
        return hit;
    }
    if (L.mode != LaneMode::None && x >= L.x && x < L.x + L.width) {
        struct Find {
            double at;
            const ItemGeo **found;
            ItemGeo *store;
            void operator()(const ItemGeo &g) const {
                if (at >= g.x && at < g.x + g.width) {
                    *store = g;
                    *found = store;
                }
            }
        };
        ItemGeo store;
        const ItemGeo *found = nullptr;
        Find find{x, &found, &store};
        if (isCapturedShown(shown)) {
            for (const auto &g : frozenItems_)
                find(g);
        } else
            layoutLane(shown, top, h, nullptr, detail::sinkFor<ItemGeo>(find));
        if (found != nullptr) {
            hit.part = found->pill ? HitPart::ChainPill : HitPart::ChainItem;
            hit.item = found->ref;
            hit.rect = {found->x, top, found->width, h};
            return hit;
        }
        return hit;
    }
    for (std::size_t k = 0; k < kindCount_; ++k) {
        const CellGeo cell = cellAt(shown, k);
        if (cell.content == CellContent::Hidden || cell.content == CellContent::Blank)
            continue;
        if (x < cell.x || x >= cell.x + cell.width)
            continue;
        hit.kindIndex = k;
        hit.item = cell.ref.row.valid() ? cell.ref : hit.item;
        hit.rect = {cell.x, top, cell.width, h};
        switch (cell.content) {
        case CellContent::Item:
            hit.part = HitPart::ModuleItem;
            break;
        case CellContent::DormantPreview:
            hit.part = HitPart::ModulePreview;
            break;
        case CellContent::DormantPill:
            hit.part = HitPart::ModulePill;
            break;
        case CellContent::Refused:
            hit.part = HitPart::ModuleRefused;
            hit.item = chipRef(r.id);
            break;
        default:
            break;
        }
        return hit;
    }
    return hit;
}

Hit StackModel::hitTest(double x, double y) const noexcept {
    Hit hit;
    if (!(x >= 0 && y >= 0 && x < listW_ && y < listH_))
        return hit;
    const auto &p = plan();
    if (view_.navigator && p.navW > 0 && x >= p.navX) {
        hit.rect = {p.navX, 0, p.navW, listH_};
        if (y < static_cast<double>(c_.navHeader)) {
            hit.part = HitPart::NavigatorHeader;
            return hit;
        }
        hit.part = HitPart::NavigatorEmpty;
        if (x < p.navX + static_cast<double>(c_.navigatorMargin) || c_.navLine <= 0)
            return hit;
        const double line = std::floor((y - static_cast<double>(c_.navHeader) + navScroll_) / c_.navLine);
        if (line >= 0 && line < static_cast<double>(shown_.size())) {
            const auto i = static_cast<std::size_t>(line);
            hit.part = HitPart::NavigatorLine;
            hit.shown = i;
            hit.row = {rows_[shown_[i]].id, rows_[shown_[i]].gen};
            hit.item = chipRef(hit.row.track);
            hit.rect = {p.navX + c_.navigatorMargin, c_.navHeader + line * c_.navLine - navScroll_,
                        p.navW - c_.navigatorMargin, static_cast<double>(c_.navLine)};
        }
        return hit;
    }
    const Located at = locate(y + scroll_);
    switch (at.locus) {
    case Locus::Empty:
        hit.part = HitPart::Below;
        return hit;
    case Locus::Above:
        hit.part = HitPart::Above;
        return hit;
    case Locus::Below:
        hit.part = HitPart::Below;
        return hit;
    case Locus::Gap:
        hit.part = HitPart::Gap;
        hit.shown = at.index;
        return hit;
    case Locus::Row:
        break;
    }
    return hitRow(at.index, x, y, at.top - scroll_, at.height);
}

// --- the navigator --------------------------------------------------------------------------------

Span StackModel::frameSpan() const noexcept { return visible(); }

void StackModel::updateNavScroll() noexcept {
    const auto &p = plan();
    const double old = navScroll_;
    if (!view_.navigator || p.navW <= 0 || c_.navLine <= 0) {
        navScroll_ = 0;
    } else {
        const double linesH = std::max(0.0, listH_ - static_cast<double>(c_.navHeader));
        const double total = static_cast<double>(shown_.size()) * c_.navLine;
        const double most = std::max(0.0, total - linesH);
        const Span v = frameSpan();
        if (!v.empty) {
            const double ft = static_cast<double>(v.first) * c_.navLine;
            const double fb = static_cast<double>(v.last + 1) * c_.navLine;
            if (fb - navScroll_ > linesH)
                navScroll_ = fb - linesH;
            if (ft - navScroll_ < 0)
                navScroll_ = ft;
        }
        navScroll_ = std::clamp(navScroll_, 0.0, most);
    }
    if (navScroll_ != old)
        bump();
}

NavGeo StackModel::navigator() const noexcept {
    NavGeo g;
    const auto &p = plan();
    if (!view_.navigator || p.navW <= 0)
        return g;
    g.shown = true;
    g.collapsed = p.navW <= static_cast<double>(c_.navigatorCollapsed) + kEps;
    const double margin = c_.navigatorMargin;
    g.area = {p.navX + margin, 0, p.navW - margin, listH_};
    g.header = {g.area.x, 0, g.area.w, static_cast<double>(c_.navHeader)};
    g.lines = {g.area.x, static_cast<double>(c_.navHeader), g.area.w,
               std::max(0.0, listH_ - static_cast<double>(c_.navHeader))};
    g.scroll = navScroll_;
    const Span v = frameSpan();
    if (!v.empty)
        g.frame = {g.area.x, c_.navHeader + static_cast<double>(v.first) * c_.navLine - navScroll_, g.area.w,
                   static_cast<double>(v.size()) * c_.navLine};
    const std::size_t count = shown_.size();
    if (count > 0 && c_.navLine > 0) {
        const auto first = static_cast<std::size_t>(std::floor(navScroll_ / c_.navLine));
        const double end = std::ceil((navScroll_ + g.lines.h) / c_.navLine);
        const std::size_t last = std::min(count, static_cast<std::size_t>(std::max(0.0, end)));
        if (first < last) {
            g.lineSpan.first = first;
            g.lineSpan.last = last - 1;
            g.lineSpan.empty = false;
        }
    }
    return g;
}

NavLine StackModel::lineAt(std::size_t shown, const Span &onScreen) const noexcept {
    const Rec &r = rows_[shown_[shown]];
    ++stats_.lineEvaluations;
    NavLine line;
    line.ref = {r.id, r.gen};
    line.y = c_.navHeader + static_cast<double>(shown) * c_.navLine - navScroll_;
    line.indent = static_cast<double>(r.depth) * c_.navIndent;
    line.colour = r.colour;
    line.roles = r.roles;
    line.onScreen = onScreen.contains(shown);
    line.group = r.group;
    line.pinned = pinned(r.id);
    line.hit = !queryFold_.empty() && r.match;
    line.jumpTarget = view_.searchMode == SearchMode::Jump && !matches_.empty() &&
                      matches_[std::min(matchAt_, matches_.size() - 1)] == shown_[shown];
    line.pairA = shared_.comparePair.a == r.id;
    line.pairB = shared_.comparePair.b == r.id;
    line.pairPending = pairPending_ == r.id;
    return line;
}

NavLine StackModel::navigatorLine(std::size_t shown) const noexcept {
    if (shown >= shown_.size())
        return NavLine{};
    return lineAt(shown, visible());
}

void StackModel::walkLines(detail::Sink<NavLine> sink) const {
    const NavGeo g = navigator();
    if (!g.shown || g.lineSpan.empty)
        return;
    const Span onScreen = visible();
    for (std::size_t i = g.lineSpan.first; i <= g.lineSpan.last; ++i)
        sink(lineAt(i, onScreen));
}

// --- state ----------------------------------------------------------------------------------------

RowRef StackModel::enlarged() const noexcept { return rowRef(enlarged_); }

RowRef StackModel::pointerRow() const noexcept {
    const auto at = pointerRowIndex();
    return at ? RowRef{rows_[shown_[*at]].id, rows_[shown_[*at]].gen} : RowRef{};
}

Focus StackModel::focus() const noexcept {
    Focus f;
    f.row = rowRef(focusRow_);
    f.item = f.row.valid() ? focusItem_ : ItemRef{};
    f.keyboard = keyboardFocus_;
    return f;
}

RowRef StackModel::maximised() const noexcept { return rowRef(maximised_); }

bool StackModel::pinned(TrackId id) const noexcept {
    return std::binary_search(shared_.pins.begin(), shared_.pins.end(), id);
}

RowRef StackModel::jumpTarget() const noexcept {
    if (view_.searchMode != SearchMode::Jump || matches_.empty())
        return RowRef{};
    const Rec &r = rows_[matches_[std::min(matchAt_, matches_.size() - 1)]];
    return {r.id, r.gen};
}

TrackId StackModel::maskingReference(const RowRef &ref) const noexcept {
    const Rec *r = live(ref) ? recFor(ref.track) : nullptr;
    if (r == nullptr)
        return 0;
    const ComparePair pair = shared_.comparePair;
    if (pair.a != 0) {
        if (r->id != pair.a)
            return pair.a; // the pair's first member is everyone else's reference, and the second's
        if (pair.b != 0)
            return pair.b;
        // a pair of one: its member falls back to the rule
    }
    const auto firstWith = [&](std::uint8_t bit) -> TrackId {
        for (const auto &other : rows_)
            if (inScope(other) && (other.roles & bit) != 0 && other.id != r->id)
                return other.id;
        return 0;
    };
    const auto openedOrFocused = [&]() -> TrackId {
        if (openedFrom_ != 0 && openedFrom_ != r->id && recFor(openedFrom_) != nullptr)
            return openedFrom_;
        if (focusRow_ != 0 && focusRow_ != r->id)
            return focusRow_;
        return 0;
    };
    switch (view_.maskingRule) {
    case MaskingRule::RolePartner: {
        TrackId partner = 0;
        if ((r->roles & role::Bass) != 0)
            partner = firstWith(role::Kick);
        else if ((r->roles & role::Kick) != 0)
            partner = firstWith(role::Bass);
        return partner != 0 ? partner : openedOrFocused();
    }
    case MaskingRule::OpenedFrom:
        return openedOrFocused();
    case MaskingRule::Focused:
        return focusRow_ != r->id ? focusRow_ : 0;
    }
    return 0;
}

// --- view state -----------------------------------------------------------------------------------

ViewState StackModel::viewState() const {
    ViewState v;
    v.window = view_;
    v.shared = shared_;
    v.window.scroll = ScrollAnchor{};
    if (!shown_.empty()) {
        const Located at = locate(std::max(0.0, scroll_));
        std::size_t i = 0;
        switch (at.locus) {
        case Locus::Row:
            i = at.index;
            break;
        case Locus::Gap:
            i = std::min(at.index + 1, shown_.size() - 1);
            break;
        case Locus::Below:
            i = shown_.size() - 1;
            break;
        default:
            i = 0;
            break;
        }
        v.window.scroll.track = rows_[shown_[i]].id;
        // measured against the row at rest, so a row enlarged now does not move the view on reopening
        v.window.scroll.offset = std::clamp(std::max(0.0, scroll_) - contentTop(i), 0.0, static_cast<double>(base()));
    }
    return v;
}

Effects StackModel::applyViewState(const ViewState &state, TimeMs now) {
    Effects fx;
    const auto before = beginEvent();
    advanceTo(now);
    if (cap_.active) {
        fx.refused = true;
        endEvent(fx, before);
        return fx;
    }
    const ViewState v = state.normalised(c_);
    view_ = v.window;
    shared_ = v.shared;
    shared_.pins.reserve(boundOf(c_.maxPins));
    view_.folded.reserve(boundOf(c_.maxFolded));
    view_.search.reserve(boundOf(c_.searchMaxBytes) + 8);
    foldForSearch(view_.search, queryFold_);
    maximised_ = 0;
    landed_ = 0;
    jump_ = Jump{};
    focusOwns_ = false;
    if (!view_.autoZoom)
        setEnlarged(focusRow_, now);
    ++stats_.fullPasses;
    for (std::uint32_t i = 0; i < rows_.size(); ++i) {
        rows_[i].underFold = computeUnderFold(i);
        rows_[i].match = computeMatch(i);
    }
    const std::size_t oldFocusShown = focusRow_ != 0 ? shownIndex(focusRow_).value_or(kNpos) : kNpos;
    rebuildShown();
    afterShownChange(now, oldFocusShown);
    snapEntries();
    replan();
    rebuildMatches();
    anchor_ = AnchorHold{};
    scroll_ = 0;
    if (const auto at = shownIndex(v.window.scroll.track))
        scroll_ = contentTop(*at) + std::min(v.window.scroll.offset, static_cast<double>(base()));
    scroll_ = std::clamp(scroll_, 0.0, maxScroll());
    anchorFirstVisible();
    solveScroll();
    rehit(now);
    updateNavScroll();
    bump();
    endEvent(fx, before);
    return fx;
}

// --- event bookkeeping ----------------------------------------------------------------------------

std::uint64_t StackModel::beginEvent() const noexcept { return epoch_; }

void StackModel::endEvent(Effects &fx, std::uint64_t epochBefore) {
    // an event can make a deadline due at once (a dwell that was done before hover was allowed
    // again): it runs now, at the event's time, not at whichever frame comes next
    if (internalDeadline() <= now_)
        advanceTo(now_);
    const Span r = realised();
    const auto kept = retained();
    if (!(r == lastRealised_) || kept != lastRetained_ || shownVersion_ != lastShownVersion_) {
        lastRealised_ = r;
        lastRetained_ = kept;
        lastShownVersion_ = shownVersion_;
        ++realisedEpoch_;
    }
    fx.repaint = fx.repaint || epoch_ != epochBefore;
}

std::array<RowRef, 2> StackModel::retained() const noexcept {
    std::array<RowRef, 2> out{};
    const Span r = realised();
    if (cap_.active)
        if (const auto at = shownIndex(cap_.target.row.track); at && !r.contains(*at))
            out[0] = rowRef(cap_.target.row.track);
    if (focusRow_ != 0)
        if (const auto at = shownIndex(focusRow_); at && !r.contains(*at))
            out[1] = rowRef(focusRow_);
    return out;
}
} // namespace adi::ui
