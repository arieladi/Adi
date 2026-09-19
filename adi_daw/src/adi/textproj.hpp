// SPDX-License-Identifier: GPL-3.0-or-later
//
// Canonical text projection — ADR-0007, docs/format/TEXT-PROJECTION.md.
//
// `adi export --text` renders a .adi to a deterministic, line-oriented form for
// diffing and review. It is DERIVED, never authoritative, and round-tripping
// through it is not a supported workflow (ADR-0007).
//
// Two consumers, and the second is why this exists now:
//
//   1. A human running `git diff` on a project under version control.
//   2. ADR-0021's replay oracle: apply an op log twice under deliberately
//      different UI state, project both, assert the two projections are
//      BYTE-IDENTICAL. An op that reads ambient state fails that test. If this
//      projection is not deterministic, that test cannot exist.
//
// This header is the pure part: the canonical renderers and the ordering
// primitives. Everything here is a total function of its arguments alone — no
// I/O, no locale, no allocation beyond the returned string, no dependency on
// SQLite. That is deliberate. The store adapter that fills a model from a
// database is a separate concern (ADR-0010), and the parts below are where
// determinism is actually won or lost, so they are testable without one.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace adi::textproj {

// ---------------------------------------------------------------------------
// Time (TEXT-PROJECTION 8)
// ---------------------------------------------------------------------------

/// SPEC 4.2. A whole note is four quarters.
inline constexpr std::int64_t kPPQ   = 5765760;
inline constexpr std::int64_t kWhole = 4 * kPPQ;   // 23'063'040

/// Durations render as reduced fractions of a whole note -- `1/16`, `3/8`,
/// `1/12`. Exact, independent of the tempo and signature maps, and directly
/// readable as a note value, which `beats|ticks` is not: under that form a
/// sixteenth reads `0|1441440` and the duration column is unreadable on the
/// majority of lines in a file.
///
/// A duration only reduces to a small fraction when it is on a grid. Recorded,
/// unquantised performance is the common case and does not reduce at all --
/// 1441441 ticks is 1441441/23063040 in lowest terms, which is exact and
/// useless. So a denominator above `kMaxDenominator` renders as raw ticks with
/// a `t` suffix instead. Both forms are exact; the fraction is the readable one
/// when it exists, and the tick count is honest when it does not.
inline constexpr std::int64_t kMaxDenominator = 1024;

[[nodiscard]] std::string renderDuration(std::int64_t ticks);

/// One time signature, and where it starts.
///
/// This type exists so that `renderPosition` can live beside `renderDuration`
/// rather than in the store adapter. The two are the other half of each other
/// and are equally determinism-critical -- ADR-0021's oracle compares bytes,
/// and a position rendered two ways fails it exactly as a duration would.
///
/// It is NOT a schema shape, which is what kept it out of this header the first
/// time. A `Meter` is a fact about music; `time_signature_map` is a table that
/// happens to store some. The adapter converts rows into these, and this layer
/// stays free of SQLite as it was meant to.
struct Meter {
    std::int64_t  start_ticks = 0;   ///< absolute, where this signature begins
    std::uint16_t numerator   = 4;
    std::uint16_t denominator = 4;   ///< a power of two; 4 is a quarter note

    /// Ticks in one bar under this signature.
    [[nodiscard]] std::int64_t barTicks() const {
        return denominator == 0 ? 0
             : kWhole * static_cast<std::int64_t>(numerator)
                      / static_cast<std::int64_t>(denominator);
    }
};

/// `bar|beat|tick`, 1-based in bar and beat, from an absolute tick position.
///
/// `meters` must be sorted ascending by `start_ticks` and is walked segment by
/// segment -- bar numbering accumulates across signature changes, so a single
/// division by the current meter is correct only up to the first change and
/// wrong after it. That is the same shape of bug as integrating tempo with one
/// bpm, which the engine's tests already guard against.
///
/// **No raw-tick gloss.** `bar|beat|tick` is exact, so a gloss would duplicate
/// rather than clarify, and a duplicated field is one more thing that can
/// disagree with itself inside a diff.
///
/// An empty map means 4/4 from zero. A negative position renders with a
/// negative bar rather than refusing: the projection is total.
[[nodiscard]] std::string renderPosition(std::int64_t ticks,
                                         const std::vector<Meter>& meters);

// ---------------------------------------------------------------------------
// Numbers (TEXT-PROJECTION 5)
// ---------------------------------------------------------------------------

/// The shortest decimal string that round-trips to the identical bit pattern.
///
/// `renderF32` takes a `float` and must never be handed a widened one: an f32
/// formatted through `double` yields `0.10000000149011612` where `0.1` is
/// correct and sufficient. AEXP.value, AEXP.tension, AAUT.tension and
/// ANOT.tuning_cents are all f32 (blob.hpp).
///
/// Guaranteed properties, none of which `std::to_chars` gives on its own:
///   * every finite value contains `.` or `e`, so a float never renders as a
///     bare integer and `-0.0` is visibly distinct from `0.0`;
///   * exponents are `e308` and `e-308`, never `e+308` and never zero-padded,
///     so the form is pinned here rather than inherited from whichever standard
///     library happens to be linked;
///   * non-finite values are `nan`, `inf`, `-inf`. NaN payloads and signalling
///     NaNs are NOT rendered: payload preservation is not consistent across
///     ADR-0022's matrix -- x87 on the i386 leg quietens signalling NaNs in
///     transit -- so rendering them would fail the replay oracle on our own CI.
[[nodiscard]] std::string renderF64(double v);
[[nodiscard]] std::string renderF32(float v);

// ---------------------------------------------------------------------------
// Strings (TEXT-PROJECTION 4)
// ---------------------------------------------------------------------------

/// True when `s` may appear unquoted in a label position.
[[nodiscard]] bool isBareSafe(std::string_view s);

/// `s` as a quoted, escaped string, including the surrounding quotes.
///
/// Escapes are exactly `\\ \" \n \r \t \u{XXXX}` plus `\x{HH}` for a byte that
/// is not valid UTF-8. A character MUST be escaped when it is `"` or `\`, a C0
/// control or U+007F, a C1 control, U+2028, U+2029, U+FEFF, or a bidi control
/// (U+061C, U+200E, U+200F, U+202A..U+202E, U+2066..U+2069).
///
/// The bidi rule is the one rule here that exists for a security reason rather
/// than a determinism one. U+202E RIGHT-TO-LEFT OVERRIDE in a track name
/// visually reverses the remainder of the rendered line, so a diff hunk can be
/// made to DISPLAY as something other than what it says. A format whose entire
/// purpose is human review must not be forgeable by its own content.
///
/// `\x{HH}` is an addition to the document's escape set. Names arrive from a
/// TEXT column with no validation, so invalid UTF-8 is reachable; rejecting it
/// would make the projector non-total, and substituting U+FFFD would be lossy
/// and would silently merge two distinct names. Escaping the raw byte is both
/// total and injective.
[[nodiscard]] std::string quoteString(std::string_view s);

/// `s` bare if it is safe, otherwise quoted.
[[nodiscard]] std::string renderLabelToken(std::string_view s);

// ---------------------------------------------------------------------------
// Labels and collisions (TEXT-PROJECTION 6.1)
// ---------------------------------------------------------------------------

/// Assign final labels to the members of one container, in that container's
/// already-fixed canonical order.
///
/// An empty base label falls back to `#n` at the member's 0-based rank --
/// including for positioned objects such as clips. Using the rendered position
/// as the label instead makes the label *be* the position, so moving the clip
/// changes its label, re-sorts it, relocates its whole block and churns every
/// designator citing it.
///
/// When a label collides, EVERY colliding member gains `~k` at its 1-based rank
/// among equally-labelled siblings -- the first included, so that the presence
/// of `~` is itself the signal "this name is not unique here" rather than a
/// silent demotion of whichever one came second.
[[nodiscard]] std::vector<std::string> assignLabels(
    const std::vector<std::string>& base_labels);

// ---------------------------------------------------------------------------
// Designators (TEXT-PROJECTION 6.2 - 6.4)
// ---------------------------------------------------------------------------
//
// Inlining every object that has exactly one owner turns most references into
// containment and removes the identifier entirely. Seven cross-references
// genuinely survive that, and these are what address them.

/// The role partition of the top-level track forest.
///
/// Sends overwhelmingly target returns and the master, and those rank spaces
/// are untouched by inserting an audio track -- so partitioning by role is a
/// cheap halving of the churn an insertion causes in every designator that
/// cites past it.
enum class TrackRole { Track, Return, Vca, Master, Global };

/// The root SEGMENT for a role: `trk`, `ret`, `vca`, `master`, `glob` -- with
/// no leading slash. `designator()` supplies every separator, so a segment that
/// carried one would produce `//trk` and a segment stripped of one that was
/// never there produces `rk`. The second is what happened.
[[nodiscard]] std::string_view roleRoot(TrackRole r);

/// The role a `tracks.kind` value belongs to. An unknown kind -- a newer
/// writer's -- is a Track, which keeps the projection total across a version
/// boundary rather than refusing a file it could mostly render.
[[nodiscard]] TrackRole roleOfKind(std::string_view kind);

/// A designator: a quoted, slash-separated path of already-assigned labels.
///
///     "/trk/Rhythm/Drums"      "/trk/Bass/clip/Verse"
///     "/scene/Chorus"          "/media/1f4a9c2e7b0d3a51"
///
/// The whole path is ONE quoted string, not a sequence of quoted segments, so
/// renaming `Bass` to `Bass Gtr` cannot change the token's shape. A literal `/`
/// inside a segment escapes to `\u{2F}` and therefore cannot forge a path
/// boundary; everything else follows the escape rules of section 4, including
/// the bidi controls.
///
/// Segments are labels as `assignLabels` returned them, and selectors such as
/// `clip`, `dev`, `auto`, `macro` -- passed in already, because which selector
/// applies is a fact about the schema and not about a string.
[[nodiscard]] std::string designator(const std::vector<std::string>& segments);

/// A reference whose target does not exist. Renders `"!unresolved(<kind>)"`,
/// never a number, and the caller is expected to fail under `--strict`.
///
/// ADR-0029 removed `'bus'` from `routing`'s CHECK, so the example this comment
/// used to give is now the one case that CANNOT arise. What remains is the
/// general one, and it is enough: `routing` is a polymorphic `(kind, id)`
/// reference that SQLite cannot enforce, so a row pointing at a deleted track
/// is reachable. ADR-0029 flags exactly that, and `adi_tool check` finds them.
[[nodiscard]] std::string unresolved(std::string_view kind);

/// Media is addressed by content, because the format already made content the
/// identity of a media file -- for dedup, integrity and relink. The schema
/// tells you which collection deserves hash addressing, and exactly one does.
///
/// Returns one prefix per input hash: the shortest length that is at least
/// `kMediaPrefixMin`, a multiple of 4, and distinguishes every DISTINCT hash in
/// the pool.
///
/// Two rows may legitimately share a hash -- `idx_media_hash` is not unique --
/// and when they do no prefix can separate them. They receive equal prefixes
/// here and are disambiguated by `assignLabels`, which is the same `~k`
/// treatment any other duplicate label gets.
inline constexpr std::size_t kMediaPrefixMin = 16;

[[nodiscard]] std::vector<std::string> mediaPrefixes(
    const std::vector<std::string>& hashes);

// ---------------------------------------------------------------------------
// Ordering (TEXT-PROJECTION 7)
// ---------------------------------------------------------------------------
//
// No ordering column in the core model is uniqueness-enforced -- tracks,
// lanes, clips, devices, device_chains and markers all permit two siblings to
// share an ordinal, and notes inside a blob have no ordinal at all. Since
// ADR-0037 removed `scenes` this holds without exception: NO core ordering
// column is unique. That simplifies the rule rather than complicating it. So the only thing that breaks a tie in storage is the row
// id, which is exactly what must not reach the output. Every collection's order
// therefore has to be a function of content alone.

/// One declared semantic key (TEXT-PROJECTION 7.3). Integers compare as
/// integers: a bytewise comparison of rendered position tokens sorts `10|1|0`
/// before `2|1|0`, which is deterministic and wrong-looking, and would be
/// reported as a bug forever. Sort keys are raw values; rendered tokens are
/// output only.
class SortKey {
public:
    static SortKey integer(std::int64_t v);
    static SortKey real(double v);
    static SortKey text(std::string v);
    static SortKey null();

    /// Total order: null < integer < real < text, then by value. Reals compare
    /// with -0.0 before +0.0 and NaN last, so the order is total over every
    /// bit pattern a REAL column can hold.
    [[nodiscard]] int compare(const SortKey& other) const;

    /// An injective, content-derived token, used to seed K3 refinement.
    ///
    /// NOT part of the output format -- nothing here reaches a projected file.
    /// It exists because refinement needs to tell two keys apart, and
    /// `compare()` only yields -1/0/1: seeding from a comparison collapses
    /// every distinct integer key into one colour and quietly costs refinement
    /// the discriminating power it was added for. Reals contribute their bit
    /// pattern, so -0.0 and +0.0 seed differently, as they must.
    [[nodiscard]] std::string seedToken() const;

private:
    enum class Tag { Null, Int, Real, Text };
    Tag tag_ = Tag::Null;
    std::int64_t i_ = 0;
    double d_ = 0.0;
    std::string s_;
};

/// One member of a collection, as the ordering machinery sees it.
struct Member {
    /// K1 -- the declared semantic keys for this collection, in declared order.
    std::vector<SortKey> keys;

    /// K2 -- this member's own skeleton projection: its full recursive
    /// rendering with every cross-reference token replaced by a single `?`.
    /// This is what breaks the circularity. Order depends on skeletons,
    /// skeletons contain no designators, and designators depend on order.
    std::string skeleton;

    /// K3 -- indices of sibling members this one cites, and that cite it.
    /// Refinement uses both directions: two tracks identical in every field are
    /// still distinguishable if different things send to them.
    std::vector<std::uint32_t> out_refs;
    std::vector<std::uint32_t> in_refs;
};

enum class OrderStatus {
    Exact,      ///< the order is canonical
    Ambiguous,  ///< a tied class exceeded the K4 bound; see TEXT-PROJECTION 11
};

struct OrderResult {
    /// `order[p]` is the index of the member at position `p`.
    std::vector<std::uint32_t> order;
    OrderStatus status = OrderStatus::Exact;
    /// The largest class that survived K3 still tied. 1 when nothing tied.
    std::size_t largest_tied_class = 1;
};

/// The K4 bound (TEXT-PROJECTION 7.2). Permutation minimisation is factorial in
/// the class size, so it is bounded, and above the bound the projector reports
/// `Ambiguous` rather than inventing an order. That is a deliberate choice of a
/// total-but-refusing projector over a total-but-arbitrary one: an arbitrary
/// order would be a leak wearing a canonical hat.
inline constexpr std::size_t kMaxTiedClass = 8;

/// Render a candidate ordering. Supplied by the caller because K4 minimises
/// over the OUTPUT, not over any property of the members -- that is what makes
/// it canonical rather than a choice of member.
using RenderFn = std::string (*)(const std::vector<std::uint32_t>& order,
                                 void* ctx);

/// Canonical order for one collection: K1 semantic keys, then K2 skeletons,
/// then K3 refinement to a fixed point, then K4 permutation minimisation.
///
/// `render` may be null. When it is, K4 cannot observe a difference between
/// tied members and they are left in refinement order -- correct when the class
/// is automorphic, which is the common case, and reported through
/// `largest_tied_class` when the caller needs to know it was not verified.
[[nodiscard]] OrderResult canonicalOrder(const std::vector<Member>& members,
                                         RenderFn render = nullptr,
                                         void* ctx = nullptr,
                                         std::size_t max_tied_class = kMaxTiedClass);

// ---------------------------------------------------------------------------
// The pipeline (TEXT-PROJECTION 9.1)
// ---------------------------------------------------------------------------
//
// Everything above is a piece; this is what puts them in the only order that
// works. The circularity is real and it is the whole difficulty:
//
//     a container's ORDER depends on its members' SKELETONS,
//     a member's DESIGNATOR depends on that order,
//     and a member's rendering contains designators.
//
// If rendering fed ordering, the projection would be defined in terms of
// itself. It is broken by rendering twice. Pass 1 renders SKELETONS, in which
// every cross-reference is the single token `?` -- so a skeleton contains no
// designator, and ordering can depend on it. Pass 2 has every order fixed, so
// labels and designators are determined, and the real text is emitted.
//
// This layer knows nothing about SQLite. The store adapter's job is to build a
// Tree; this turns a Tree into bytes. Keeping the seam there is what lets the
// hard part be tested without a database (ADR-0010).

/// A cross-reference to another node, or to nothing.
struct Ref {
    static constexpr std::size_t kDangling = static_cast<std::size_t>(-1);

    std::string role;                    ///< `send`, `out`, `vca`, `media`
    std::size_t target = kDangling;      ///< index into Tree::nodes
    std::string target_kind;             ///< for `!unresolved(<kind>)`
};

/// One object in the projection.
struct Node {
    /// The block keyword: `trk`, `clip`, `note`, `dev`.
    std::string kind;

    /// The path selector that precedes this node's label in a designator --
    /// `clip` in `/trk/Bass/clip/Verse`. Empty for nested tracks, which the
    /// hierarchy already addresses by name.
    std::string selector;

    /// The name before disambiguation. `assignLabels` turns this into the
    /// final label, which may gain `#n` or `~k`.
    std::string base_label;

    /// K1 for ordering among siblings of the same kind (TEXT-PROJECTION 7.3).
    std::vector<SortKey> keys;

    /// Pre-rendered `key value` lines, in the order they should appear. Already
    /// canonical: the caller has used renderF64, renderDuration and the rest.
    std::vector<std::string> attrs;

    std::vector<std::size_t> children;   ///< indices into Tree::nodes
    std::vector<Ref> refs;
};

struct Tree {
    std::vector<Node> nodes;
    std::vector<std::size_t> roots;
};

struct Projection {
    std::string text;

    /// `Ambiguous` when any container's order could not be made canonical.
    /// The ADR-0021 oracle should require `Exact`: comparing bytes only means
    /// something if the bytes were canonical.
    OrderStatus status = OrderStatus::Exact;
    std::size_t largest_tied_class = 1;

    /// Per node, by node index -- the quoted designator it was given. Exposed
    /// because a reference rendering correctly is worth asserting directly,
    /// not only through the text it appears in.
    std::vector<std::string> designators;
};

/// Order, label, designate, render.
[[nodiscard]] Projection project(const Tree& tree);

}  // namespace adi::textproj
