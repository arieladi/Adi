// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the store adapter — src/adi/textproj_store.cpp.
//
// Split in two on purpose, matching the seam the adapter is built around:
//
//   * the PURE half builds a rows::Model by hand and asserts on the Tree and
//     the text. No database, no temp file. Every shaping decision — default
//     omission, hierarchy, the cycle guard, where a clip hangs — is exercised
//     here, because none of them need a file on disk and a test that opens one
//     to check a default is a slow test that fails for the wrong reasons.
//
//   * the END-TO-END half creates a real .adi, drives it through the op
//     registry, and projects it. That half is what proves readModel agrees with
//     the schema, which is the one thing hand-built Models cannot show.
//
// No framework, matching tests/test_main.cpp.

#include "temp_directory.hpp"

#include "adi/history.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/ops.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "adi/textproj_store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace adi;
using namespace adi::textproj;

namespace {

int g_failures = 0;
int g_checks = 0;

void eq(const std::string& got, const std::string& want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n--- got ---\n%s\n--- want ---\n%s\n--- end ---\n",
                    what, got.c_str(), want.c_str());
    }
}

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}

void section(const char* s) { std::printf("[%s]\n", s); }

/// Commit one op and say WHY if it did not take. `check(res.ok, "name")`
/// loses the registry's own message, which is the only thing that explains a
/// payload the schema no longer accepts.
void commits(OpJournal& j, const OpRequest& r, const char* what) {
    const CommitResult res = j.commit(r);
    check(res.ok, std::string(what) + (res.ok ? "" : ": " + res.error));
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

/// A Model with one named track and nothing else. The baseline every
/// default-omission test perturbs.
rows::Model oneTrack(const char* name = "Bass") {
    rows::Model m;
    rows::Track t;
    t.id = 1;
    t.kind = "audio";
    t.name = name;
    m.tracks.push_back(t);
    return m;
}

std::string render(const rows::Model& m) { return project(buildTree(m)).text; }

/// The projection without its header block.
///
/// Every project has one and it is four lines of constants, so repeating it in
/// twenty expectations would bury what each of them is actually about. An
/// unnamed project labels as `#0` -- quoted, because `#` is not bare-safe --
/// and that is correct rather than unfortunate: the alternative is naming the
/// block after the FILE, which would make a rename rewrite the diff.
std::string body(const rows::Model& m) {
    const std::string all = render(m);
    const std::string head =
        "project \"#0\"\n"
        "  format 0.1\n"
        "  schema " + std::to_string(kSchemaMajor) + "." + std::to_string(kSchemaMinor) + "\n"
        "  ppq 5765760\n"
        "  defaults 1\n";
    if (all.rfind(head, 0) == 0) return all.substr(head.size());
    return all;   // a named project, or a header that changed: show everything
}

// ===========================================================================
//  Positions
// ===========================================================================

void testPositions() {
    section("metersOf, and whether a position token is unique");

    const std::int64_t q = 5765760;

    // The converter is the adapter's half; renderPosition is mac's and is
    // tested in test_textproj.cpp. What is checked here is the seam.
    {
        const auto m = metersOf({});
        check(m.size() == 1 && m[0].numerator == 4 && m[0].denominator == 4 &&
                  m[0].start_ticks == 0,
              "an empty signature map becomes 4/4 from zero");
    }
    {
        const auto m = metersOf({{4 * q, 3, 4}});
        check(m.size() == 2 && m[0].start_ticks == 0 && m[0].numerator == 4,
              "a map that starts late gets 4/4 prepended -- there is no defined "
              "meter before its first event");
        check(m.size() == 2 && m[1].start_ticks == 4 * q && m[1].numerator == 3,
              "and the real event follows it");
    }
    {
        const auto m = metersOf({{0, 7, 8}});
        check(m.size() == 1 && m[0].numerator == 7 && m[0].denominator == 8,
              "a map that starts at zero is passed through unchanged");
    }

    // A position token must identify one position. Ordering, labels and the
    // byte-identity R1 depends on are all downstream of rendered content, so
    // two distinct positions rendering the same token is not cosmetic.
    //
    // The case is a meter change that does NOT land on a bar line, which the
    // schema permits and nothing forbids. 4/4 for six quarters, then 3/4 --
    // the change arrives half way through bar 2.
    {
        const std::vector<Meter> ragged{Meter{0, 4, 4}, Meter{6 * q, 3, 4}};
        const std::string at4 = renderPosition(4 * q, ragged);
        const std::string at6 = renderPosition(6 * q, ragged);
        check(at4 != at6,
              "a mid-bar meter change leaves the token unique: 4q -> " + at4 +
                  ", 6q -> " + at6);
    }
}

// ===========================================================================
//  Defaults
// ===========================================================================

void testDefaultsOmitted() {
    section("defaults are omitted (TEXT-PROJECTION 3)");

    eq(body(oneTrack()), "trk Bass\n",
       "a default track is one line -- the whole readability argument");

    {
        rows::Model m = oneTrack();
        m.tracks[0].muted = true;
        m.tracks[0].soloed = true;
        eq(body(m), "trk Bass\n  muted\n  soloed\n",
           "a true flag is its bare name, and only true flags appear");
    }
    {
        rows::Model m = oneTrack();
        m.tracks[0].kind = "midi";
        eq(body(m), "trk Bass\n  kind midi\n",
           "kind appears when it is not the default for the role space");
    }
    {
        // A return track's role IS its kind, so saying both is noise.
        rows::Model m = oneTrack("Verb");
        m.tracks[0].kind = "return";
        eq(body(m), "ret Verb\n", "a return says ret, and does not also say kind");
    }
    {
        rows::Model m = oneTrack();
        rows::MixerStrip s;
        s.trackId = 1;
        m.strips.push_back(s);
        eq(body(m), "trk Bass\n", "a mixer strip at its defaults adds nothing");

        m.strips[0].volumeDb = -3.5;
        m.strips[0].width = 0.5;
        eq(body(m), "trk Bass\n  vol -3.5\n  width 0.5\n",
           "and a changed fader inlines onto the track");
    }
    {
        // ADR-0174: summing on a group, inlined like the strip, defaults omitted.
        rows::Model m = oneTrack("Drums");
        m.tracks[0].kind = "group";
        rows::GroupSumming g;
        g.trackId = 1;
        m.summing.push_back(g);
        const std::string off = body(m);
        check(off.find("summing") == std::string::npos, "summing at its defaults adds nothing");
        m.summing[0].enabled = true;
        m.summing[0].flavor = "console.la";
        m.summing[0].driveDb = 6.0;
        const std::string on = body(m);
        check(on.find("  summing\n") != std::string::npos && on.find("  summingFlavor console.la\n") != std::string::npos &&
                  on.find("  summingDrive 6.0\n") != std::string::npos,
              "and on, a flavour and a drive inline onto the group:\n" + on);
    }
    {
        // Bit comparison, not `!=`. This is the check that fails the moment
        // someone simplifies `sameBits(v, dflt)` to `v != dflt`, because
        // -0.0 != 0.0 is false and the line would silently vanish.
        rows::Model m = oneTrack();
        rows::MixerStrip s;
        s.trackId = 1;
        s.volumeDb = -0.0;
        m.strips.push_back(s);
        eq(body(m), "trk Bass\n  vol -0.0\n",
           "-0.0 is not the default 0.0 and must survive the omission rule");
    }
}

// ===========================================================================
//  Hierarchy, and the cycle a corrupt file can hold
// ===========================================================================

void testHierarchy() {
    section("hierarchy, containment and the parent_id cycle");

    {
        rows::Model m;
        rows::Track g;
        g.id = 1; g.kind = "group"; g.name = "Drums"; g.indexInParent = 0;
        rows::Track k;
        k.id = 2; k.kind = "audio"; k.name = "Kick"; k.indexInParent = 0; k.parentId = 1;
        m.tracks = {g, k};
        eq(body(m), "trk Drums\n  kind group\n  trk Kick\n",
           "a child track prints inside its parent");

        const Projection p = project(buildTree(m));
        eq(p.designators[1], "\"/trk/Drums\"", "a top-level track carries its role space");
        eq(p.designators[2], "\"/trk/Drums/Kick\"",
           "and a nested one does not repeat it at every depth");
    }
    {
        // `tracks.parent_id` forbids only `id <> parent_id`. Two tracks can
        // legally point at each other, and a naive recursive build never
        // returns. Both must still appear: a track missing from the output is
        // a diff that says it was deleted.
        rows::Model m;
        rows::Track a;
        a.id = 1; a.kind = "audio"; a.name = "A"; a.parentId = 2;
        rows::Track b;
        b.id = 2; b.kind = "audio"; b.name = "B"; b.parentId = 1;
        m.tracks = {a, b};
        const std::string out = body(m);
        check(contains(out, "trk A"), "a cycle still projects its first track");
        check(contains(out, "trk B"), "and its second");
        check(out == "trk A\ntrk B\n" || out == "trk B\ntrk A\n",
              "a cycle detaches to the top level rather than nesting: " + out);
    }
    {
        rows::Model m = oneTrack();
        m.tracks[0].parentId = 99;   // a parent that is not there
        eq(body(m), "trk Bass\n", "a dangling parent makes a root, not a dropped track");
    }
    {
        // A clip belongs to a track and MAY belong to a lane. Containment says
        // it prints inside the nearest owner it has.
        rows::Model m = oneTrack();
        rows::Lane l;
        l.id = 7; l.trackId = 1; l.name = "Take 2"; l.kind = "take";
        m.lanes.push_back(l);
        rows::Clip c;
        c.id = 3; c.trackId = 1; c.laneId = 7; c.kind = "audio"; c.name = "Verse";
        c.posTicks = 0; c.lengthTicks = 4 * 5765760;
        m.clips.push_back(c);
        eq(body(m),
           "trk Bass\n"
           "  lane \"Take 2\"\n"
           "    clip Verse\n"
           "      at 1|1|0\n"
           "      len 1/1\n",
           "a clip in a lane prints under the lane, not the track");

        m.clips[0].laneId.reset();
        eq(body(m),
           "trk Bass\n"
           "  clip Verse\n"
           "    at 1|1|0\n"
           "    len 1/1\n"
           "  lane \"Take 2\"\n",
           "and under the track when it has no lane");
    }
}

// ===========================================================================
//  References
// ===========================================================================

void testReferences() {
    section("the references that survive containment");

    {
        rows::Model m;
        rows::Track a;
        a.id = 1; a.kind = "audio"; a.name = "Kick"; a.indexInParent = 0;
        rows::Track b;
        b.id = 2; b.kind = "return"; b.name = "Verb"; b.indexInParent = 1;
        m.tracks = {a, b};
        rows::Routing r;
        r.id = 1; r.srcKind = "track"; r.srcId = 1; r.dstKind = "track"; r.dstId = 2;
        r.kind = "send"; r.gainDb = -6.0;
        m.routing.push_back(r);
        eq(body(m),
           "trk Kick\n"
           "ret Verb\n"
           "route send\n"
           "  gain -6.0\n"
           "  from -> \"/trk/Kick\"\n"
           "  to -> \"/ret/Verb\"\n",
           "a send names both endpoints by designator");
    }
    {
        // A hardware port is not a row and never was (SPEC 6.7). Rendering one
        // as `!unresolved` would report a healthy project as broken.
        rows::Model m = oneTrack();
        rows::Routing r;
        r.id = 1; r.srcKind = "hw_in"; r.srcId = 3; r.dstKind = "track"; r.dstId = 1;
        r.kind = "main";
        m.routing.push_back(r);
        const std::string out = body(m);
        check(contains(out, "from hw_in:3"),
              "a hardware endpoint is an attribute, not a reference: " + out);
        check(!contains(out, "unresolved"),
              "and it does not report a healthy project as broken: " + out);
    }
    {
        rows::Model m = oneTrack();
        rows::Routing r;
        r.id = 1; r.srcKind = "track"; r.srcId = 1; r.dstKind = "track"; r.dstId = 404;
        r.kind = "send";
        m.routing.push_back(r);
        check(contains(body(m), "to -> \"!unresolved(track)\""),
              "a genuinely missing endpoint renders unresolved, never as a number");
    }
    {
        rows::Model m = oneTrack();
        rows::Media f;
        f.id = 4;
        f.hashBlake3 = "1f4a9c2e7b0d3a5100000000";
        f.origName = "kick.wav";
        m.media.push_back(f);
        rows::Clip c;
        c.id = 3; c.trackId = 1; c.kind = "audio"; c.name = "Hit";
        c.posTicks = 0; c.lengthTicks = 5765760;
        m.clips.push_back(c);
        rows::AudioClip ac;
        ac.clipId = 3; ac.mediaId = 4; ac.srcLenFrames = 1000;
        m.audioClips.push_back(ac);
        const std::string out = body(m);
        check(contains(out, "media -> \"/media/1f4a9c2e7b0d3a51\""),
              "media is addressed by a content prefix, not an id: " + out);
    }
}

// ===========================================================================
//  Coverage (TEXT-PROJECTION 10)
// ===========================================================================

void testCoverage() {
    const adi::test::TempDirectory scratch("textproj_store", "testCoverage");
    const fs::path& dir = scratch.path();
    section("the coverage manifest names every table, and only real ones");

    StoreError e = StoreError::Ok;
    const auto store = Store::createEmpty(dir / "coverage.adi", e);
    check(store != nullptr, "a store to read the live schema from");
    if (!store) return;

    std::set<std::string> live;
    SQLite::Statement st(store->db(),
                         "SELECT name FROM sqlite_master WHERE type='table' "
                         "AND name NOT LIKE 'sqlite_%'");
    while (st.executeStep()) live.insert(st.getColumn(0).getString());

    std::set<std::string> manifest;
    for (const TableCoverage& c : coverage()) manifest.insert(std::string(c.table));

    // Both directions. A table added to the schema and not to the manifest is
    // a table silently missing from every projection; a table in the manifest
    // that no longer exists is a claim about nothing. The first is the one that
    // matters and the second is how you find out a rename half-landed.
    for (const auto& name : live)
        check(manifest.count(name) != 0,
              "schema table '" + name + "' is on the coverage manifest");
    for (const auto& name : manifest)
        check(live.count(name) != 0,
              "manifest table '" + name + "' exists in the schema");

    for (const TableCoverage& c : coverage())
        if (c.status == Coverage::Excluded)
            check(!c.reason.empty(),
                  "excluded table '" + std::string(c.table) + "' says why");
}

// ===========================================================================
//  End to end
// ===========================================================================

void testEndToEnd() {
    const adi::test::TempDirectory scratch("textproj_store", "testEndToEnd");
    const fs::path& dir = scratch.path();
    section("a real .adi, driven through the op registry");

    StoreError e = StoreError::Ok;
    const auto store = Store::createEmpty(dir / "e2e.adi", e);
    check(store != nullptr, "created a project");
    if (!store) return;

    OpJournal j(*store);
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}};
    commits(j, r, "track.create");
    r.opType = "track.create";
    r.payload = {{"id", 2}, {"kind", "audio"}, {"name", "Bass"}, {"index", 1}};
    commits(j, r, "a second track");
    r.opType = "clip.create";
    r.payload = {{"id", 5}, {"track", 1}, {"kind", "midi"}, {"name", "Riff"},
                 {"pos", 0}, {"length", 23063040}};
    commits(j, r, "clip.create");
    r.opType = "note.insert";
    r.payload = {{"clip", 5}, {"note", 1}, {"start", 0}, {"dur", 5765760},
                 {"key", 60}, {"vel", 96}};
    commits(j, r, "note.insert");
    r.opType = "mixer.setVolume";
    r.payload = {{"id", 2}, {"db", -6.0}};
    commits(j, r, "mixer.setVolume");

    const rows::Model m = rows::readModel(*store);
    check(m.problems.empty(),
          "every table read cleanly: " +
              (m.problems.empty() ? std::string("-") : m.problems.front()));
    check(m.tracks.size() == 2, "two tracks read back");
    check(m.clips.size() == 1, "one clip read back");
    check(m.notes.size() == 1, "the note came out of the ANOT blob, not a table");

    const Projection p = projectStore(*store);
    check(p.status == OrderStatus::Exact,
          "the order is canonical -- the oracle only means something if it is");
    eq(p.text,
       "project \"#0\"\n"
       "  format 0.1\n"
       "  schema " + std::to_string(kSchemaMajor) + "." + std::to_string(kSchemaMinor) + "\n"
       "  ppq 5765760\n"
       "  defaults 1\n"
       "trk Keys\n"
       "  kind midi\n"
       "  monitor 2\n"
       "  clip Riff\n"
       "    at 1|1|0\n"
       "    len 1/1\n"
       "    kind midi\n"
       "    note \"#0\"\n"
       "      at 1|1|0 key 60 dur 1/4 vel 96\n"
       "trk Bass\n"
       "  vol -6.0\n",
       "the whole projection of a real project, byte for byte");
}

/// ADR-0021's property, which is the reason this whole layer exists.
void testSessionStateIsInvisible() {
    const adi::test::TempDirectory scratch("textproj_store", "testSessionStateIsInvisible");
    const fs::path& dir = scratch.path();
    section("session state cannot reach the projection (ADR-0021)");

    StoreError e = StoreError::Ok;
    const auto store = Store::createEmpty(dir / "session.adi", e);
    check(store != nullptr, "created a project");
    if (!store) return;

    OpJournal j(*store);
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", 1}, {"kind", "audio"}, {"name", "Only"}};
    commits(j, r, "one track");

    const std::string before = projectStore(*store).text;
    check(before == projectStore(*store).text, "projecting twice gives the same bytes");

    // Everything a UI would scribble while the user clicks around.
    store->db().exec("INSERT INTO session_state(key, value) VALUES ('sel', 'track:1')");
    store->db().exec("INSERT INTO ui_view(scope_kind, scope_id, key, value) "
                     "VALUES ('track', 1, 'height', '220')");
    store->db().exec("INSERT INTO window_state(kind, x, y, w, h) "
                     "VALUES ('mixer', 10, 20, 800, 600)");

    eq(projectStore(*store).text, before,
       "and none of the Layer 3 tables move a single byte");
}

/// `Model::problems` is only worth having if it is reachable, and the way to
/// know is to reach it.
void testCorruptBlobIsNamed() {
    const adi::test::TempDirectory scratch("textproj_store", "testCorruptBlobIsNamed");
    const fs::path& dir = scratch.path();
    section("a corrupt note stream is named, not swallowed and not thrown");

    StoreError e = StoreError::Ok;
    const auto store = Store::createEmpty(dir / "corrupt.adi", e);
    check(store != nullptr, "created a project");
    if (!store) return;

    OpJournal j(*store);
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}};
    commits(j, r, "track.create");
    r.opType = "clip.create";
    r.payload = {{"id", 5}, {"track", 1}, {"kind", "midi"}, {"name", "Riff"},
                 {"pos", 0}, {"length", 23063040}};
    commits(j, r, "clip.create");
    r.opType = "note.insert";
    r.payload = {{"clip", 5}, {"note", 1}, {"start", 0}, {"dur", 5765760},
                 {"key", 60}, {"vel", 96}};
    commits(j, r, "note.insert");

    // Truncate the stream to less than one header. Reachable in the field from
    // a full volume mid-write, and the reason StreamReader is total.
    store->db().exec("UPDATE event_streams SET data = X'414E4F5401' WHERE clip_id = 5");

    const rows::Model m = rows::readModel(*store);
    check(m.notes.empty(), "no note is invented out of a broken stream");
    check(m.problems.size() == 1,
          "exactly one problem is reported, naming the clip");
    check(!m.problems.empty() && contains(m.problems[0], "clips#5"),
          "and it says which clip: " +
              (m.problems.empty() ? std::string("(none)") : m.problems[0]));

    // The projection still has to come out. A reader that refuses a damaged
    // file is a reader that cannot tell you what is still in it.
    const Projection p = project(buildTree(m));
    check(contains(p.text, "clip Riff"),
          "the clip still projects; only its notes are gone");
}

}  // namespace

// ===========================================================================
//  Remarks (ADR-0131, ADR-0139)
// ===========================================================================

rows::Remark remarkOn(const char* kind, std::int64_t id, const char* text,
                      std::int64_t created = 1790000000000000) {
    rows::Remark r;
    r.id = 1;
    r.targetKind = kind;
    r.targetId = id;
    r.text = text;
    r.createdUtc = created;
    return r;
}

void testRemarksModel() {
    section("remarks: containment, author, anchors that are gone (ADR-0139)");

    {
        auto m = oneTrack();
        m.remarks.push_back(remarkOn("track", 1, "muddy below 200 Hz"));
        eq(body(m),
           "trk Bass\n"
           "  remark \"#0\"\n"
           "    created 1790000000000000\n"
           "    text \"muddy below 200 Hz\"\n",
           "a user's remark is a child of its track; `user` is the omitted default");
    }
    {
        auto m = oneTrack();
        auto r = remarkOn("track", 1, "try a high-pass");
        r.author = "agent";
        r.actorDetail = "model-x";
        r.resolved = true;
        m.remarks.push_back(r);
        const std::string b = body(m);
        check(contains(b, "    by agent\n") && contains(b, "    detail model-x\n") &&
                  contains(b, "    resolved\n"),
              "an agent's remark always says so, with who and whether resolved:\n" + b);
    }
    {
        auto m = oneTrack();
        rows::Clip c;
        c.id = 5;
        c.trackId = 1;
        c.kind = "midi";
        c.name = "Riff";
        c.posTicks = 0;
        c.lengthTicks = 5765760;
        m.clips.push_back(c);
        m.remarks.push_back(remarkOn("clip", 5, "comp this"));
        check(contains(body(m), "  clip Riff\n") &&
                  contains(body(m), "    remark \"#0\"\n"),
              "a clip's remark nests under the clip, not the track:\n" + body(m));
    }
    {
        auto m = oneTrack();
        rows::DeviceChain ch;
        ch.id = 3;
        ch.trackId = 1;
        m.deviceChains.push_back(ch);
        rows::Device d;
        d.id = 7;
        d.chainId = 3;
        d.name = "EQ";
        m.devices.push_back(d);
        auto r = remarkOn("device", 7, "too bright");
        r.paramId = "gain";
        m.remarks.push_back(r);
        const std::string b = body(m);
        check(contains(b, "trk Bass\n  remark \"#0\"\n    device EQ\n    param gain\n"),
              "a device's remark sits on the device's track and names the device "
              "and parameter:\n" + b);
    }
    {
        auto m = oneTrack();
        m.remarks.push_back(remarkOn("track", 99, "orphan"));
        eq(body(m),
           "trk Bass\n"
           "remark \"#0\"\n"
           "  created 1790000000000000\n"
           "  text orphan\n"
           "  on -> \"!unresolved(track)\"\n",
           "a remark whose track is gone is kept, at top level, unresolved");
    }
    {
        auto m = oneTrack();
        m.remarks.push_back(remarkOn("track", 1, "line one\nline two \xE2\x80\xAE"));
        const std::string b = body(m);
        check(contains(b, "text \"line one\\nline two \\u{202E}\"\n"),
              "remark text is escaped: a newline or a bidi override cannot forge "
              "the lines around it:\n" + b);
    }
    {
        // Two remarks on one track, in either storage order: the same text.
        auto a = oneTrack();
        // Written first but alphabetically last, so only the time can put it first.
        auto first = remarkOn("track", 1, "zebra", 1);
        auto second = remarkOn("track", 1, "apple", 2);
        second.id = 2;
        a.remarks = {first, second};
        auto b = oneTrack();
        b.remarks = {second, first};
        check(render(a) == render(b), "remark order is the content's, not the storage's");
        check(render(a).find("text zebra") < render(a).find("text apple") &&
                  render(a).find("text apple") != std::string::npos,
              "and it is by the time each was written");
    }
}

void testRemarksEndToEnd() {
    const adi::test::TempDirectory scratch("textproj_store", "testRemarksEndToEnd");
    section("remarks through the op registry, undo and redo, byte for byte");

    StoreError e = StoreError::Ok;
    const auto store = Store::createEmpty(scratch.path() / "remarks.adi", e);
    if (!store) return;
    OpJournal j(*store);
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", 1}, {"kind", "audio"}, {"name", "Bass"}};
    commits(j, r, "track.create");
    const std::string before = projectStore(*store).text;

    r.opType = "remark.add";
    r.payload = {{"id", 1}, {"kind", "track"}, {"target", 1}, {"author", "agent"},
                 {"detail", "model-x"}, {"text", "check the low end"},
                 {"created", 1790000000000000}};
    r.actor = Actor::Agent;
    commits(j, r, "remark.add");

    const rows::Model m = rows::readModel(*store);
    check(m.problems.empty() && m.remarks.size() == 1 && m.remarks[0].author == "agent",
          "the remark is read back into the model");
    const Projection p = projectStore(*store);
    check(p.status == OrderStatus::Exact, "the order is canonical");
    eq(p.text,
       "project \"#0\"\n"
       "  format 0.1\n"
       "  schema " + std::to_string(kSchemaMajor) + "." + std::to_string(kSchemaMinor) + "\n"
       "  ppq 5765760\n"
       "  defaults 1\n"
       "trk Bass\n"
       "  remark \"#0\"\n"
       "    by agent\n"
       "    detail model-x\n"
       "    created 1790000000000000\n"
       "    text \"check the low end\"\n",
       "the agent's remark projected, byte for byte");

    History h(*store);
    check(h.undo().ok && projectStore(*store).text == before,
          "undo takes the remark out of the projection");
    check(h.redo().ok && projectStore(*store).text == p.text,
          "redo puts back exactly the same text");
}

void testPitchEdits() {
    section("staged pitch edits project by content, including dormant edits");
    const adi::test::TempDirectory scratch("textproj_store", "pitchEdits");
    const auto path = scratch.path() / "pitch.adi";
    std::ifstream ddl(ADI_PITCH_SCHEMA_PATH);
    if (!ddl) throw std::runtime_error("could not open staged pitch SQL");
    const std::string schema{std::istreambuf_iterator<char>(ddl), std::istreambuf_iterator<char>()};
    const std::string hash(64, 'a');
    const std::string replacement(64, 'b');
    const auto seed = [&](const fs::path& file, std::int64_t id, bool freshPitch = false) {
        StoreError error{};
        auto store = Store::createEmpty(file, error);
        if (!store) throw std::runtime_error(toString(error));
        auto& db = store->db();
        if (freshPitch) db.exec(schema);
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Voice');");
        SQLite::Statement clip(db, "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) "
                                   "VALUES(?,1,'audio','Take',0,1441440)");
        clip.bind(1, id);
        clip.exec();
        SQLite::Statement media(db, "INSERT INTO media_files(id,hash_blake3,sample_rate,channels,frames) "
                                    "VALUES(11,?,48000,1,2000),(12,?,48000,1,2000)");
        media.bind(1, hash);
        media.bind(2, replacement);
        media.exec();
        SQLite::Statement audio(db, "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) "
                                    "VALUES(?,11,0,2000)");
        audio.bind(1, id);
        audio.exec();
        return store;
    };
    auto store = seed(path, 19);
    const auto baseline = projectStore(*store).text;
    store.reset();
    StoreError error{};
    store = Store::open(path, error, true);
    check(store && store->readOnly() && projectStore(*store).text == baseline &&
              !store->db().tableExists("clip_pitch_edits"),
          "read-only 1.23 projection has no pitch rows and leaves its schema intact");
    store.reset();
    store = Store::open(path, error);
    if (!store) throw std::runtime_error(toString(error));
    auto& db = store->db();
    const auto version = db.execAndGet("PRAGMA user_version").getInt();
    {
        SQLite::Transaction rolledBack(db);
        db.exec(schema);
        check(db.tableExists("clip_pitch_edits"), "staged pitch DDL is visible inside its transaction");
    }
    check(!db.tableExists("clip_pitch_edits") && projectStore(*store).text == baseline,
          "rolling back the staged DDL restores the old projection and schema");
    db.exec(schema);
    check(db.execAndGet("PRAGMA user_version").getInt() == version && projectStore(*store).text == baseline,
          "installing empty staged DDL preserves existing content and the actual schema version");

    const auto setPitch = [&](Store& target, std::int64_t id, std::uint64_t segmentId, double cents) {
        const media::ClipPitchAnalysis analysis{id, {hash, 48000, 1, 2000}, 0, 2000, false, 1, 0,
            {48000, 2000, {{100, 500, 6100, 0, 0, {{200, 6100}}}, {700, 1000, 6400, 0, 0, {{800, 6400}}}}}};
        audio::PitchNoteEdit adjusted;
        adjusted.startFrame = 120;
        adjusted.endFrame = 540;
        adjusted.pitchShiftCents = cents;
        adjusted.straighten = 0.5;
        adjusted.straightenRange = audio::PitchStraightenRange{160, 250};
        adjusted.tiltCents = -20;
        adjusted.tiltGeometry = {0.4, 12, -7};
        adjusted.formantShiftCents = -125.5;
        adjusted.gainDb = 3.5;
        adjusted.drift = 0.75;
        adjusted.preserveExpression = true;
        adjusted.transitionFrames = 200;
        audio::PitchNoteEdit muted;
        muted.noteIndex = 1;
        muted.startFrame = 700;
        muted.endFrame = 1000;
        muted.gainDb = -std::numeric_limits<double>::infinity();
        const audio::PitchSegmentation geometry{{{segmentId, 100, 500}, {segmentId + 2, 700, 1000}}, segmentId + 3};
        const auto prepared = clip_edit::prepareClipPitchEdits(analysis, {{adjusted, muted}}, &geometry);
        if (!prepared.request) throw std::runtime_error(prepared.error);
        OpJournal journal(target);
        commits(journal, *prepared.request, "commit literal pitch geometry and controls");
    };
    setPitch(*store, 19, 47, 100.25);
    const auto authored = projectStore(*store).text;
    const std::string pitchBlock =
        "    pitch edits\n"
        "      source " + hash + "\n"
        "      sourceFrames 2000\n"
        "      sampleRate 48000\n"
        "      analysisVersion 1\n"
        "      analysisConfiguration 0\n"
        "      segments\n"
        "      segment 100 500\n"
        "      segment 700 1000\n"
        "      pitchEdit 100 500 to 120 540 pitch 100.25 straighten 0.5 straightenRange 160 250"
        " tilt -20.0 tiltAnchor 0.4 tiltStart 12.0 tiltEnd -7.0 formant -125.5 gain 3.5"
        " drift 0.75 preserveExpression transition 200\n"
        "      pitchEdit 700 1000 to 700 1000 muted\n";
    check(contains(authored, pitchBlock), "source-bound geometry, timing and the complete authored controls render under the clip");

    auto other = seed(scratch.path() / "equivalent.adi", 271, true);
    const auto schemaObjects = [](SQLite::Database& database) {
        SQLite::Statement query(database, "SELECT type, name, tbl_name, COALESCE(sql, '') FROM sqlite_schema "
                                          "ORDER BY type, name, tbl_name");
        std::vector<std::array<std::string, 4>> objects;
        while (query.executeStep()) objects.push_back({query.getColumn(0).getString(), query.getColumn(1).getString(),
                                                      query.getColumn(2).getString(), query.getColumn(3).getString()});
        return objects;
    };
    check(schemaObjects(db) == schemaObjects(other->db()) &&
              other->db().execAndGet("PRAGMA user_version").getInt() == version,
          "staged upgrade and fresh staged creation produce the same schema objects and version");
    setPitch(*other, 271, 830, 100.25);
    eq(projectStore(*other).text, authored, "SQL and authored segment IDs do not affect canonical text");

    setPitch(*store, 19, 47, 101.25);
    auto adjustedText = authored;
    if (const auto pitch = adjustedText.find("pitch 100.25"); pitch != std::string::npos)
        adjustedText.replace(pitch, std::string_view("pitch 100.25").size(), "pitch 101.25");
    check(adjustedText != authored && projectStore(*store).text == adjustedText,
          "one changed control modifies only its literal pitch line");
    History history(*store);
    check(history.undo().ok && projectStore(*store).text == authored && history.redo().ok &&
              projectStore(*store).text == adjustedText, "pitch Undo and Redo reproduce the exact projection");
    check(history.undo().ok, "restore the original edit for dormant projection checks");

    OpRequest detach;
    detach.opType = "clip.detachAudio";
    detach.payload = {{"id", 19}};
    OpJournal journal(*store);
    commits(journal, detach, "detach the source without clearing its authored pitch");
    check(contains(projectStore(*store).text, pitchBlock), "detached pitch edits remain under their owning clip");
    OpRequest attach;
    attach.opType = "clip.attachAudio";
    attach.payload = {{"id", 19}, {"media", 12}, {"frames", 2000}};
    commits(journal, attach, "attach different source content");
    check(contains(projectStore(*store).text, pitchBlock), "a replacement source preserves the original pitch binding");
    check(history.undo().ok && history.undo().ok && projectStore(*store).text == authored,
          "Undoing the source replacement restores the exact active projection");

    OpRequest empty;
    empty.opType = "clip.setPitchEdits";
    empty.payload = {{"id", 19}, {"hash", hash}, {"sampleRate", 48000}, {"sourceFrames", 2000},
        {"version", 1}, {"configuration", 0}, {"rows", Payload::array()},
        {"segmentation", {{"nextId", 1}, {"segments", Payload::array()}}}};
    commits(journal, empty, "author an empty set of pitch segments");
    const auto emptyText = projectStore(*store).text;
    check(contains(emptyText, "      segments\n") && !contains(emptyText, "      segment ") &&
              !contains(emptyText, "      pitchEdit "), "explicitly empty geometry remains distinguishable");
    check(history.undo().ok && projectStore(*store).text == authored,
          "undoing empty geometry restores all source spans and edits");

    db.exec("UPDATE clip_pitch_edits SET edits=X'FF' WHERE clip_id=19");
    check(contains(projectStore(*store).text, "    pitchUnreadable\n"),
          "corrupt authored data leaves a deterministic marker on the owning clip");
}

void testPitchExtractionMarker() {
    section("staged pitch extraction provenance belongs to the MIDI output");
    const adi::test::TempDirectory scratch("textproj_store", "pitchExtraction");
    const auto path = scratch.path() / "pitch-extraction.adi";
    std::ifstream ddl(ADI_PITCH_SCHEMA_PATH);
    if (!ddl) throw std::runtime_error("could not open staged pitch SQL");
    const std::string schema{std::istreambuf_iterator<char>(ddl), std::istreambuf_iterator<char>()};
    StoreError error{};
    auto store = Store::createEmpty(path, error);
    if (!store) throw std::runtime_error(toString(error));
    store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(19,'audio','Voice',0),(57,'midi','Notes',1);"
        "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) VALUES"
        "(71,19,'audio','Source',0,1441440),(84,57,'midi','Extracted',0,1441440),"
        "(92,57,'midi','Independent',1441440,1441440)");
    const auto oldText = projectStore(*store).text;
    store.reset();
    store = Store::open(path, error, true);
    check(store && store->readOnly() && !store->db().tableExists("clip_pitch_extractions") &&
              projectStore(*store).text == oldText && !contains(oldText, "pitchExtraction"),
          "a read-only project without staged provenance keeps its exact text");
    store.reset();
    store = Store::open(path, error);
    if (!store) throw std::runtime_error(toString(error));
    auto& db = store->db();
    db.exec(schema);
    eq(projectStore(*store).text, oldText, "an empty staged extraction table changes no text");
    db.exec("INSERT INTO clip_pitch_extractions(clip_id) VALUES(84)");
    const std::string clipHeader = "  clip Extracted\n    at 1|1|0\n    len 1/16\n    kind midi\n";
    const auto clip = oldText.find(clipHeader);
    if (clip == std::string::npos) throw std::runtime_error("missing canonical extraction fixture clip");
    auto marked = oldText;
    marked.insert(clip + clipHeader.size(), "    pitchExtraction\n");
    eq(projectStore(*store).text, marked, "only the extracted clip gains the canonical marker");
    unsigned markerReads = 0;
    sqlite3_trace_v2(db.getHandle(), SQLITE_TRACE_STMT, [](unsigned, void* state, void* statement, void*) -> int {
        const auto* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if (sql && std::string_view(sql).find("clip_pitch_extractions") != std::string_view::npos)
            ++*static_cast<unsigned*>(state);
        return 0;
    }, &markerReads);
    const auto ordinary = render(rows::readModel(*store));
    sqlite3_trace_v2(db.getHandle(), 0, nullptr, nullptr);
    check(ordinary == oldText && markerReads == 0, "the ordinary model read skips extraction provenance");
    store.reset();
    store = Store::open(path, error, true);
    check(store && store->readOnly() && projectStore(*store).text == marked,
          "staged marker survives reopen for canonical read-only projection");
    store.reset();
    store = Store::open(path, error);
    if (!store) throw std::runtime_error(toString(error));
    store->db().exec("DELETE FROM clips WHERE id=71");
    check(contains(projectStore(*store).text, clipHeader + "    pitchExtraction\n"),
          "the output marker remains renderable after the source clip is deleted");
    store->db().exec("DELETE FROM clips WHERE id=84");
    check(store->db().execAndGet("SELECT COUNT(*) FROM clip_pitch_extractions").getInt() == 0 &&
              !contains(projectStore(*store).text, "pitchExtraction"),
          "deleting the output cascades the marker and leaves no extraction text");
    store->db().exec("DROP TABLE clip_pitch_extractions; CREATE TABLE clip_pitch_extractions(incomplete INTEGER)");
    check(contains(projectStore(*store).text, "  pitchExtractionUnreadable\n"),
          "unreadable provenance leaves a deterministic marker on the project");
}

int main() {
    // Unbuffered, so the last line before a crash survives. On Windows a
    // crashing test binary loses its whole block-buffered stdout, and the
    // harness then prints a blank line where a failure should be -- which is
    // how adi_device_tests' 383 KB overrun looked like a harness glitch for
    // two runs before anyone ran the binary directly.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    try {
        testPositions();
        testDefaultsOmitted();
        testHierarchy();
        testReferences();
        testCoverage();
        testEndToEnd();
        testSessionStateIsInvisible();
        testCorruptBlobIsNamed();
        testRemarksModel();
        testRemarksEndToEnd();
        testPitchEdits();
        testPitchExtractionMarker();
    } catch (const std::exception& ex) {
        // A modal abort dialog on Windows is a worse failure than the failure.
        std::printf("\nFAILED -- exception escaped: %s\n", ex.what());
        return 1;
    }

    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
