"""usage: chain_step.py WORKTREE BRANCH PREV [OLD:NEW ...]

Builds one link of the engine chain in WORKTREE (already `git checkout --detach origin/BRANCH`):
merges PREV with diff3 markers, resolves the conflict shapes the engine's parallel branches
produce, renumbers the branch's own schema minors OLD->NEW on the lines the branch itself added
(diff against origin/main), freezes PREV's schema.sql as history/schema-1.<NEW-1>.sql, rebuilds a
two-step branch's intermediate frozen file, and stages everything. It does not commit. Exit 1 with
the file list when a block needs a human.

Rules for a diff3 block (ours = the branch, theirs = PREV, base = merge base):
  empty base                               -> keep both, theirs first
  OPS.md "**N ops** -- a P0, b P1 ..."     -> base + (ours-base) + (theirs-base), per number
  README.md "verified on every change"     -> theirs (the count left the README in #423)
  docs/format/history/schema-1.N.sql       -> theirs (the chain's frozen file)
  test_textproj_store.cpp kTables size     -> base + (ours-base) + (theirs-base)
  test_textproj_store.cpp "schema 1.x"      -> theirs when theirs is the kSchemaMinor string
  test_migrate.cpp stored sample rate       -> theirs (same check, the chain's spelling)
  store.hpp kSchemaMinor                    -> theirs' line, number NEW, ours' last "; 1.x: ..." appended
  FIXUPS[(branch, file)]                    -> that text (a hand merge, recorded once)
  schema.sql / SPEC.md / validate_schema.py / docs/ENGINE-*.md / docs/OPS.md other version lines
                                            -> ours (renumbered afterwards) when ours carries 1.OLD
  anything else                             -> human
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

wt = Path(sys.argv[1])
branch, prev = sys.argv[2], sys.argv[3]
renum = [tuple(int(x) for x in a.split(":")) for a in sys.argv[4:]]
old_set = {o for o, _ in renum}

# hand merges, keyed by (branch, file): a list of (needle found in OURS, the resolved block)
FIXUPS = {
    # the Codex seat's shapes branch merged edits and lanes itself: its OPS.md automation section
    # is the newer text of the same section the chain carries from lanes
    ("engine/automation-shapes", "adi_daw/docs/OPS.md"): [("The lifecycle operations above supply lane creation and deletion.", "ours")],
    # send-automation's hearing model keeps the chain's hearing flags (cue-reference) on its call
    ("engine/send-automation", "adi_daw/src/adi/engine/mixer.cpp"): [
        ("const Hearing hears = hearing(hearingModel, plan);",
         lambda o, b, t: [l.replace("hearing(hearingModel, plan);", "hearing(hearingModel, plan, flags);") for l in o]),
    ],
    # the chain's session block (playbackModel_, from the stack) already carries send-automation's
    # suspended-writing set
    ("engine/send-automation", "adi_daw/src/adi/engine/session.cpp"): [
        ("suspended.insert(writingAutomation_.begin(), writingAutomation_.end());", "theirs"),
    ],
    ("engine/split-stereo-pan", "adi_daw/src/adi/engine/snapshot.cpp"): [("a.panMode != b.panMode", [
        "        a.volumeDb != b.volumeDb || a.pan != b.pan || a.panMode != b.panMode ||",
        "        a.panLeft != b.panLeft || a.panRight != b.panRight || a.delaySamples != b.delaySamples ||",
        "        a.clips.size() != b.clips.size())",
    ])],
    ("engine/track-recording-options", "adi_daw/src/adi/engine/snapshot.cpp"): [("a.recording != b.recording", [
        "        a.kind != b.kind || a.recording != b.recording || a.color != b.color || a.muted != b.muted || a.soloed != b.soloed ||",
        "        a.volumeDb != b.volumeDb || a.pan != b.pan || a.panMode != b.panMode ||",
        "        a.panLeft != b.panLeft || a.panRight != b.panRight || a.delaySamples != b.delaySamples ||",
        "        a.clips.size() != b.clips.size())",
    ])],
    ("engine/track-roles", "adi_daw/src/adi/engine/snapshot.cpp"): [
        ("a.role != b.role", [
            "        a.kind != b.kind || a.role != b.role || a.recording != b.recording || a.color != b.color || a.muted != b.muted || a.soloed != b.soloed ||",
            "        a.volumeDb != b.volumeDb || a.pan != b.pan || a.panMode != b.panMode ||",
            "        a.panLeft != b.panLeft || a.panRight != b.panRight || a.delaySamples != b.delaySamples ||",
            "        a.clips.size() != b.clips.size())",
        ]),
        ("hasRole?\"t.role", [
            "            \"  IFNULL(t.parent_id, 0), t.index_in_parent, t.kind, t.color, IFNULL(m.delay_samples,0), \" +",
            "            std::string(hasRole?\"t.role \":\"NULL \")+",
        ]),
    ],
    ("engine/return-send-tap", "adi_daw/src/adi/engine/snapshot.cpp"): [("a.returnSendTap != b.returnSendTap", [
        "        a.kind != b.kind || a.role != b.role || a.recording != b.recording || a.returnSendTap != b.returnSendTap || a.color != b.color || a.muted != b.muted || a.soloed != b.soloed ||",
        "        a.volumeDb != b.volumeDb || a.pan != b.pan || a.panMode != b.panMode ||",
        "        a.panLeft != b.panLeft || a.panRight != b.panRight || a.delaySamples != b.delaySamples ||",
        "        a.clips.size() != b.clips.size())",
    ])],
    # cue-reference adds HearingFlags to StripSet::sync; the chain (delay-compensation-switch) adds a
    # bool before it. Merged: (..., bool delayCompensation=true, const HearingFlags& = {}). The only
    # four-argument caller is session.cpp; the tests use the defaults.
    ("engine/cue-reference", "adi_daw/src/adi/engine/mixer.cpp"): [("const HearingFlags& flags) {", [
        "                       const std::shared_ptr<StripAutomation>& automation,bool delayCompensation, const HearingFlags& flags) {",
    ])],
    ("engine/cue-reference", "adi_daw/src/adi/engine/mixer.hpp"): [("const HearingFlags& = {}", [
        "    void sync(const rows::Model&, const Transport*, const std::shared_ptr<StripAutomation>&,",
        "              bool delayCompensation=true, const HearingFlags& = {});",
    ])],
    ("engine/cue-reference", "adi_daw/src/adi/engine/session.cpp"): [("stripAutomation_,hearingFlags);", [
        "    HearingFlags hearingFlags;hearingFlags.soloInPlace=cueSettings_.soloInPlace;",
        "    hearingFlags.soloIsCue=spec_.performanceState && cueSettings_.cueMode;hearingFlags.referenceTrack=cueSettings_.projectReferenceTrack;",
        "    strips_.sync(model_, &transport_, stripAutomation_,spec_.delayCompensation,hearingFlags);   // ADR-0163: before the graph names them",
    ])],
    # clip-properties captures a deleted clip with its own helper; the chain's clip_edit::captureDeleted
    # (phase-c-editing) already lists clip_properties among the clip's child tables and validates the
    # image, so the delete inverse keeps the chain's helper
    ("engine/clip-properties", "adi_daw/src/adi/ops_catalog.cpp"): [("clip_properties::captureDeleted", "theirs")],
    # track-freeze adds three columns (frozen, freeze_media_id, f.track_id) and a join to the track
    # query; the chain holds 10 = delay_samples and 11 = role, so they become 12, 13, 14 (EDITS move
    # the reader). The transport op rows it only re-wrapped keep the chain's handlers.
    # the query conflict takes the chain's text at both steps; EDITS then add the freeze columns
    # and join to the chain's final query (the step that has the role column)
    ("engine/track-freeze", "adi_daw/src/adi/engine/snapshot.cpp"): [("t.frozen, ", "theirs")],
    ("engine/track-freeze", "adi_daw/src/adi/ops_catalog.cpp"): [
        ("     \"Start playback\",", "theirs"),
        ("group.setFreezeScope", [
            "    {\"group.setFreezeScope\", \"Set captured whole-group freeze scope\", Scope::Edit, EngineImpact::GraphRebuild,",
            "     kFFreeze, false, false, freeze::groupApply, freeze::groupInverse, \"\"},",
            "    {\"track.setFreeze\", \"Set captured track freeze state\", Scope::Edit, EngineImpact::GraphRebuild, kFFreeze,",
            "     false, false, freeze::apply, freeze::inverse, \"\"},",
            "    {\"track.create\", \"Create a track\", Scope::Edit, EngineImpact::GraphRebuild,",
            "     kFTrackCreate, false, false, trackCreateApply, trackCreateInverse, \"track.delete\"},",
            "    {\"track.setRole\", \"Set an anchor role\", Scope::Edit, EngineImpact::GraphRebuild,",
            "     kFTrackRole, false, false, trackRoleApply, trackRoleInverse, \"\"},",
            "    {\"track.delete\", \"Delete a track\", Scope::Edit, EngineImpact::GraphRebuild,",
            "     kFId, false, false, trackDeleteApply, trackDeleteInverse, \"track.create\"},",
            "    {\"track.setParent\", \"Move a track into or out of a group\", Scope::Edit,",
            "     EngineImpact::GraphRebuild, kFSetParent, false, false,",
            "     trackSetParentApply, trackSetParentInverse, \"\"},",
        ]),
    ],
    # midi-controller-playback re-wrapped the test dispatch line and added one call: the chain's
    # line with that call inserted after try{
    ("engine/midi-controller-playback", "adi_daw/tests/test_midi_clips.cpp"): [("sessionControllers",
        lambda o, b, t: [t[0].replace("try{", "try{if(only==\"all\"||only==\"controllers\")sessionControllers();", 1)])],
    # a comment both sides reworded (NI stems in the chain, normalization on the branch)
    ("engine/export-normalization", "adi_daw/src/adi/render/queue.hpp"): [("Normalization", [
        "// runner exports direct wet sources as WAV, or NI Stems through niStems. Noise",
        "// shaping, dry processing and isolated through-master stems need support. Normalization",
        "// captures the selected source once and verifies the final encoded WAV.",
    ])],
    # the branch only moved the ops.hpp include down; the chain added two includes after it
    ("engine/midi-note-transforms", "adi_daw/src/adi/ops_catalog.cpp"): [("", [
        "#include \"adi/ops.hpp\"",
        "#include \"adi/note_flags_ops.hpp\"",
        "#include \"adi/builtin_catalog.hpp\"",
    ])],
    ("engine/snapshot-audio-peaks", "adi_daw/src/adi/engine/snapshot.cpp"): [("a.audio == b.audio", [
        "           a.muted == b.muted && a.gainDb == b.gainDb && a.fadeInTicks==b.fadeInTicks &&",
        "           a.fadeOutTicks==b.fadeOutTicks && a.fadeInCurve==b.fadeInCurve && a.fadeOutCurve==b.fadeOutCurve &&",
        "           a.fadeInTension==b.fadeInTension && a.fadeOutTension==b.fadeOutTension &&",
        "           a.contentOffsetNs==b.contentOffsetNs && a.loopBeginNs==b.loopBeginNs &&",
        "           a.loopEndNs==b.loopEndNs && a.sourceEndNs==b.sourceEndNs && a.fadeClockNs==b.fadeClockNs && a.fadeClockTicks==b.fadeClockTicks &&",
        "           a.audio == b.audio;",
    ])],
}
# whole-line edits after the merge, keyed by (branch, file): (old line, new line), each exactly once
EDITS = {
    ("engine/track-roles", "adi_daw/src/adi/engine/snapshot.cpp"): [
        ("            if (!st.getColumn(10).isNull()) t.role = st.getColumn(10).getString();",
         "            if (!st.getColumn(11).isNull()) t.role = st.getColumn(11).getString();  // 10 is delay_samples (chain)"),
    ],
    # track-freeze's columns sit behind the chain's delay_samples (10) and role (11); the query edits
    # are optional because the role line exists only at the link's second (main-chain) merge step
    ("engine/track-freeze", "adi_daw/src/adi/engine/snapshot.cpp"): [
        ("            std::string(hasRole?\"t.role \":\"NULL \")+",
         "            std::string(hasRole?\"t.role \":\"NULL \")+ \", t.frozen, t.freeze_media_id, f.track_id \"", "optional"),
        ("            \"FROM tracks t LEFT JOIN mixer_strip m ON m.track_id = t.id \"",
         "            \"FROM tracks t LEFT JOIN mixer_strip m ON m.track_id = t.id LEFT JOIN frozen_renders f ON f.track_id=t.id \"", "optional"),
        ("            t.frozen = st.getColumn(10).getInt() != 0;",
         "            t.frozen = st.getColumn(12).getInt() != 0;  // 10 delay_samples, 11 role (chain)"),
        ("            if (!st.getColumn(11).isNull())",
         "            if (!st.getColumn(13).isNull())"),
        ("                t.freezeMediaId = st.getColumn(11).getInt64();",
         "                t.freezeMediaId = st.getColumn(13).getInt64();"),
        ("            t.freezeMode = !st.getColumn(12).isNull() ? \"wet\" : t.frozen ? \"unverified\" : \"\";",
         "            t.freezeMode = !st.getColumn(14).isNull() ? \"wet\" : t.frozen ? \"unverified\" : \"\";"),
    ],
    # the chain's clip_edit::restoreDeleted line precedes it and returns; this one would be dead
    ("engine/clip-properties", "adi_daw/src/adi/ops_catalog.cpp"): [
        ("        if (p.contains(\"row\")) { clip_properties::restoreDeleted(c,p); return true; }", None),
    ],
    # phase-c-editing already declared rows::Clip::endMarkerTicks (for N33); keep one
    ("engine/clip-properties", "adi_daw/src/adi/store_rows.hpp"): [
        ("    std::optional<std::int64_t> endMarkerTicks = {};", None),
    ],
}


def git(*args, check=True):
    r = subprocess.run(["git", *args], cwd=wt, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode:
        raise SystemExit(f"git {' '.join(args)} failed:\n{r.stderr}")
    return r


def mapnum(line):
    """Renumber every OLD token on the line to NEW, all pairs at once."""
    if not renum:
        return line
    def one(o, n, s):
        s = re.sub(rf"(?<![\d.])1\.{o}(?!\d)", f"1.\x00{n}\x00", s)
        s = re.sub(rf"\b10{o}\b", f"10\x00{n}\x00", s)
        s = re.sub(rf"'{o}'", f"'\x00{n}\x00'", s)
        s = re.sub(rf"\{{{o},( *)\{{", lambda m: "{\x00" + str(n) + "\x00," + m.group(1) + "{", s)
        s = re.sub(rf"kSchemaMinor = {o};", f"kSchemaMinor = \x00{n}\x00;", s)
        return s
    for o, n in renum:
        line = one(o, n, line)
    return line.replace("\x00", "")


def has_old(line):
    return any(re.search(rf"(?<![\d.])1\.{o}(?!\d)|\b10{o}\b|'{o}'|\{{{o}, *\{{|kSchemaMinor = {o};", line) for o in old_set)


def has_new(line):
    """The line already carries a renumbered minor (a second merge step of the same link)."""
    return any(re.search(rf"(?<![\d.])1\.{n}(?!\d)|\b10{n}\b|'{n}'|\{{{n}, *\{{|kSchemaMinor = {n};", line) for _, n in renum)


TERM_SPLIT = re.compile(r"\s*(?:&&|\|\|)\s*")
TERM_RE = re.compile(r"^[A-Za-z_][\w.()]*\s*[!=]=\s*[A-Za-z_][\w.()]*$")


def comparison_union(o, b, t, cr):
    """ours, base, theirs are each a run of `a.x == b.x` (or !=) terms joined by one operator, ending
    in ';' or ')'. Both sides only added terms. Result: theirs with ours' new terms appended."""
    def parse(side):
        text = " ".join(l.strip() for l in side).strip()
        if not text:
            return None
        ender = text[-1]
        if ender not in ";)":
            return None
        body = text[:-1].strip()
        ops = set(re.findall(r"&&|\|\|", body))
        if len(ops) > 1:
            return None
        terms = [x.strip() for x in TERM_SPLIT.split(body) if x.strip()]
        if not all(TERM_RE.match(x) for x in terms):
            return None
        return terms, (ops.pop() if ops else None), ender
    po, pb, pt = parse(o), parse(b), parse(t)
    if not (po and pb and pt):
        return None
    ot, oop, oe = po; bt, bop, be = pb; tt, top, te = pt
    op = top or oop or bop
    if oe != te or (oop and top and oop != top) or not set(bt) <= set(tt) or not set(bt) <= set(ot):
        return None
    extra = [x for x in ot if x not in tt]
    if not extra:
        return [l + cr for l in t]
    indent = re.match(r"\s*", t[-1]).group(0)
    last = t[-1].rstrip()
    assert last.endswith(te)
    lines = t[:-1] + [last[:-1].rstrip() + f" {op}", indent + f" {op} ".join(extra) + te]
    return [l + cr for l in lines]


def three_way_line(o, b, t):
    """Both sides changed one line in places that do not overlap: apply both edits to base, on
    tokens split after each ';' (a statement list) or, failing that, after each ','."""
    import difflib
    for splitter in (r"(?<=[;{}])", r"(?<=,)"):
        split = lambda s: [x for x in re.split(splitter, s) if x]
        ob, bb, tb = split(o), split(b), split(t)
        def edits(side):
            sm = difflib.SequenceMatcher(None, bb, side, autojunk=False)
            return [(i1, i2, side[j1:j2]) for tag, i1, i2, j1, j2 in sm.get_opcodes() if tag != "equal"]
        eo, et = edits(ob), edits(tb)
        ok = True
        for a1, a2, _ in eo:
            for c1, c2, _ in et:
                if a1 == a2 and c1 == c2:
                    continue  # two insertions at the same point are fine, theirs first
                if a1 < c2 and c1 < a2 or (a1 == a2 and c1 <= a1 < c2) or (c1 == c2 and a1 <= c1 < a2):
                    ok = False
        if not ok:
            continue
        out, pos = [], 0
        for i1, i2, rep, _side in sorted([(i1, i2, rep, 0) for i1, i2, rep in et] + [(i1, i2, rep, 1) for i1, i2, rep in eo], key=lambda e: (e[0], e[3])):
            out.extend(bb[pos:i1]); out.extend(rep); pos = max(pos, i2)
        out.extend(bb[pos:])
        return "".join(out)
    return None


def three_way_lines(o, b, t):
    """Line-wise three-way merge of a block: both sides' edits to base applied when their line
    ranges do not overlap (two insertions at one point are fine, theirs first)."""
    import difflib
    if not b:
        return None
    def edits(side):
        sm = difflib.SequenceMatcher(None, b, side, autojunk=False)
        return [(i1, i2, side[j1:j2]) for tag, i1, i2, j1, j2 in sm.get_opcodes() if tag != "equal"]
    eo, et = edits(o), edits(t)
    for a1, a2, _ in eo:
        for c1, c2, _ in et:
            if a1 == a2 and c1 == c2:
                continue
            if (a1 < c2 and c1 < a2) or (a1 == a2 and c1 <= a1 < c2) or (c1 == c2 and a1 <= c1 < a2):
                return None
    out, pos = [], 0
    for i1, i2, rep, _side in sorted([(i1, i2, rep, 0) for i1, i2, rep in et] + [(i1, i2, rep, 1) for i1, i2, rep in eo], key=lambda e: (e[0], e[3])):
        out.extend(b[pos:i1]); out.extend(rep); pos = max(pos, i2)
    out.extend(b[pos:])
    return out


OPS_RE = re.compile(r"^\*\*(\d+) ops\*\* — (\d+) P0, (\d+) P1, (\d+) P2, (\d+) P3(.*)$")
KT_RE = re.compile(r"TableCoverage, (\d+)>")


def resolve_block(rel, ours, base, theirs):
    """Return the resolved lines or None (human)."""
    cr = "\r" if theirs and theirs[0].endswith("\r") else ""
    strip = lambda ls: [l.rstrip("\r") for l in ls]
    o, b, t = strip(ours), strip(base), strip(theirs)
    if not base:
        # the same addition made by both (several engine branches carry the same block)
        def run_inside(small, big):
            return any(big[i:i + len(small)] == small for i in range(len(big) - len(small) + 1))
        if o and run_inside(o, t):
            return theirs
        if t and run_inside(t, o):
            return ours
        # C++ only: drop from ours every run of three or more lines that theirs already holds
        # (DDL and prose repeat short runs legitimately)
        if not rel.endswith((".cpp", ".hpp")):
            return theirs + ours
        kept, i = [], 0
        while i < len(o):
            best = 0
            for L in range(len(o) - i, 2, -1):
                if run_inside(o[i:i + L], t):
                    best = L
                    break
            if best:
                i += best
            else:
                kept.append(ours[i]); i += 1
        return theirs + kept
    if rel.endswith("docs/OPS.md") and len(o) == len(b) == len(t) == 1:
        mo, mb, mt = OPS_RE.match(o[0]), OPS_RE.match(b[0]), OPS_RE.match(t[0])
        if mo and mb and mt:
            nums = [int(mo.group(i)) + int(mt.group(i)) - int(mb.group(i)) for i in range(1, 6)]
            return [f"**{nums[0]} ops** — {nums[1]} P0, {nums[2]} P1, {nums[3]} P2, {nums[4]} P3{mt.group(6)}" + cr]
    if rel.endswith("README.md") and any("verified on every change" in l for l in t):
        return theirs
    if "docs/format/history/schema-1." in rel:
        return theirs
    if rel.endswith("textproj_store.cpp") and len(o) == len(b) == len(t) == 1 and KT_RE.search(b[0]):
        g = lambda l: int(KT_RE.search(l).group(1))
        n = g(o[0]) + g(t[0]) - g(b[0])
        return [KT_RE.sub(f"TableCoverage, {n}>", t[0]) + cr]
    if rel.endswith("tests/test_textproj_store.cpp") and any("std::to_string(kSchemaMinor)" in l for l in t):
        return theirs
    if rel.endswith("tests/test_migrate.cpp"):
        return theirs  # main's migrate test is authoritative; the branches only restated its sample-rate check
    if rel.endswith("src/adi/store.hpp") and len(o) == len(t) == 1 and "kSchemaMinor" in o[0] and "kSchemaMinor" in t[0] and renum:
        new = max(n for _, n in renum)
        tail = mapnum(o[0].rsplit(";", 1)[-1].rstrip())
        line = re.sub(r"kSchemaMinor = \d+;", f"kSchemaMinor = {new};", t[0]).rstrip() + ";" + tail
        return [line + cr]
    for needle, fix in FIXUPS.get((branch, rel), []):
        if (needle == "" and not o) or any(needle in l for l in o if needle):
            if fix == "theirs":
                return theirs
            if fix == "ours":
                return ours
            return [l + cr for l in (fix(o, b, t) if callable(fix) else fix)]
    # a criss-cross link (CHAIN_RESOLVE): the branch merged its dependencies itself, so its text
    # of OPS.md's automation section is the newest; the chain's copy came from those dependencies
    if os.environ.get("CHAIN_RESOLVE") == "1" and rel.endswith("docs/OPS.md") and any("`automation." in l for l in o):
        return ours
    # ours deleted the base outright and theirs no longer carries it either (it replaced it with
    # something else): the deletion is already in theirs
    def contains_run(small, big):
        return bool(small) and any(big[i:i + len(small)] == small for i in range(len(big) - len(small) + 1))
    if not o and b and t and not contains_run(b, t):
        return theirs
    # a single line both sides edited in different places: three-way merge on ';'-terminated tokens
    if len(o) == len(b) == len(t) == 1:
        merged = three_way_line(o[0], b[0], t[0])
        if merged is not None:
            return [merged + cr]
    # one side only wrapped base in new lines (base is a contiguous run inside it) while the other
    # rewrote base: the wrapper's surrounding lines around the rewrite
    def wraps(outer, inner):
        for i in range(len(outer) - len(inner) + 1):
            if outer[i:i + len(inner)] == inner:
                return i
        return None
    if b:
        i = wraps(t, b)
        if i is not None and t != b:
            return [l + cr for l in t[:i] + o + t[i + len(b):]]
        i = wraps(o, b)
        if i is not None and o != b:
            return [l + cr for l in o[:i] + t + o[i + len(b):]]
    # a chain of comparisons both sides extended (snapshot.cpp's same()): theirs plus ours' extra terms
    merged = comparison_union(o, b, t, cr)
    if merged is not None:
        return merged
    # an #include block both sides touched: theirs, then ours' includes theirs lacks
    if all(re.match(r'\s*(#include\s+["<][^">]+[">]|$)', l) for l in o + b + t) and (o or t):
        extra = [l for l in o if l.strip() and l not in t]
        return [l + cr for l in t + extra]
    # both sides changed different lines of the block: apply both edits line-wise
    merged = three_way_lines(o, b, t)
    if merged is not None:
        return [l + cr for l in merged]
    versiony = ("docs/format/schema.sql", "docs/format/SPEC.md", "tools/validate_schema.py", "docs/OPS.md")
    if (rel.endswith(versiony) or "/docs/ENGINE-" in rel) and renum and o and all(has_old(l) or has_new(l) for l in o if l.strip()):
        return ours
    return None


def resolve_file(path):
    text = path.read_text(encoding="utf-8", newline="")
    lines = text.split("\n")
    m = lambda k: lines[k].rstrip("\r")
    out, i, n, kept, human = [], 0, len(lines), 0, []
    rel = path.relative_to(wt).as_posix()
    while i < n:
        if m(i).startswith("<<<<<<< "):
            j = i + 1; ours = []
            while not m(j).startswith("||||||| "): ours.append(lines[j]); j += 1
            j += 1; base = []
            while m(j) != "=======": base.append(lines[j]); j += 1
            j += 1; theirs = []
            while not m(j).startswith(">>>>>>> "): theirs.append(lines[j]); j += 1
            r = resolve_block(rel, ours, base, theirs)
            if r is None:
                human.append(i + 1); out.extend(lines[i:j + 1])
            else:
                out.extend(r); kept += 1
            i = j + 1
        else:
            out.append(lines[i]); i += 1
    path.write_text("\n".join(out), encoding="utf-8", newline="")
    return kept, human


# files several engine branches add in different versions ("added by both" in the merge): the
# version to keep whenever the two sides diverge (one is not a pure extension of the other)
FILE_PICK = {
    "adi_daw/src/adi/row_capture.hpp": "origin/engine/phase-c-editing",
    "adi_daw/src/adi/row_capture.cpp": "origin/engine/phase-c-editing",
}


def both_added(f):
    """True when the conflict on f has no base stage (both sides added the file)."""
    stages = {l.split()[2] for l in git("ls-files", "-u", "--", f).stdout.split("\n") if l.strip()}
    return stages and "1" not in stages


def resolve_both_added(f):
    ours = git("show", f":2:{f}").stdout
    theirs = git("show", f":3:{f}").stdout
    if ours == theirs:
        choice, why = theirs, "identical"
    elif "docs/format/history/schema-1." in f:
        choice, why = theirs, "the chain's frozen schema (the branch's copy is rebuilt as its intermediate)"
    else:
        import difflib
        ol, tl = ours.split("\n"), theirs.split("\n")
        ops = [t for t, *_ in difflib.SequenceMatcher(None, tl, ol, autojunk=False).get_opcodes() if t != "equal"]
        if ops and all(t == "insert" for t in ops):
            choice, why = ours, "the branch's extends the chain's"
        elif ops and all(t == "delete" for t in ops):
            choice, why = theirs, "the chain's extends the branch's"
        elif f in FILE_PICK:
            choice, why = git("show", f"{FILE_PICK[f]}:{f}").stdout, f"FILE_PICK {FILE_PICK[f]}"
        else:
            return False
    (wt / f).write_text(choice, encoding="utf-8", newline="")
    git("add", "--", f)
    print(f"  {f}: added by both, kept {why}")
    return True


# 1. merge
prev_name = prev.replace("chain/", "").replace("origin/", "")
# a branch that merged its dependencies itself meets the chain's merge of them from the other
# side: a criss-cross, whose recursive merge base is synthetic and carries nested markers the
# rules refuse; CHAIN_RESOLVE=1 uses git's single-base strategy for such links
import os
strategy = ["-s", "resolve"] if os.environ.get("CHAIN_RESOLVE") == "1" else []
git("-c", "merge.conflictStyle=diff3", "merge", "--no-edit", *strategy,
    "-m", f"Merge {prev_name} into {branch} (win: the engine stack of 4 November)",
    "-m", "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>", prev, check=False)
conflicted = [l for l in git("diff", "--name-only", "--diff-filter=U").stdout.split("\n") if l]
humans = {}
for f in conflicted:
    if both_added(f):
        if resolve_both_added(f):
            continue
        print(f"  {f}: added by both and divergent, no FILE_PICK: HUMAN")
        humans[f] = ["both added"]
        continue
    kept, human = resolve_file(wt / f)
    print(f"  {f}: {kept} block(s) resolved" + (f", HUMAN at lines {human}" if human else ""))
    if human:
        humans[f] = human

# 1b. recorded whole-line edits
for (eb, ef), edits in EDITS.items():
    if eb != branch:
        continue
    p = wt / ef
    text = p.read_text(encoding="utf-8", newline="")
    for edit in edits:
        old, new = edit[0], edit[1]
        optional = len(edit) > 2 and edit[2] == "optional"
        lines = text.split("\n")
        hits = [k for k, l in enumerate(lines) if l.rstrip("\r") == old]
        if not hits and (new is None or optional or any(l.rstrip("\r") == new for l in lines)):
            continue  # already done by an earlier step of the same link, or not this step's shape
        if len(hits) != 1:
            print(f"  EDIT in {ef} matched {len(hits)} lines: {old[:60]}"); humans[ef] = ["edit"]
            continue
        k = hits[0]
        if new is None:
            del lines[k]
        else:
            lines[k] = new + ("\r" if lines[k].endswith("\r") else "")
        text = "\n".join(lines)
    p.write_text(text, encoding="utf-8", newline="")
    print(f"  edited {ef}")

# 1c. regex edits (whole file), keyed by (branch, file): (pattern, replacement); at least one hit
# is required unless the replacement is already present
REGEX_EDITS = {
    # snapshot-remarks returned early from the snapshot builder when a project has no remarks; that
    # was the builder's last section on its branch, but every later link appends sections after it
    # (sample slots among them), so the early return becomes a guarded block
    ("engine/snapshot-remarks", "adi_daw/src/adi/engine/snapshot.cpp"): [
        (r"    // Most projects have no remarks: avoid reading device ownership at all\.\r?\n"
         r"    try \{if\(db\.execAndGet\(\"SELECT EXISTS\(SELECT 1 FROM remarks\)\"\)\.getInt\(\)==0\)return snap;\}\r?\n"
         r"    catch\(const std::exception&\)\{return snap;\}",
         "    // Most projects have no remarks: avoid reading device ownership at all. A guarded\n"
         "    // block, not an early return: the sections other items append below still run.\n"
         "    bool hasRemarks=false;\n"
         "    try {hasRemarks=db.execAndGet(\"SELECT EXISTS(SELECT 1 FROM remarks)\").getInt()!=0;}\n"
         "    catch(const std::exception&){hasRemarks=false;}\n"
         "    if(hasRemarks) {"),
        (r"(            else snap->remarks\.push_back\(std::make_shared<const RemarkNode>\(std::move\(remark\)\)\);\r?\n        \}\r?\n    \}catch\(const std::exception&\)\{\})",
         r"\1" + "\n    }"),
    ],
    # tests that created a table to simulate a later branch's schema: that table is native in the
    # chain, so the create must tolerate it (same column names, so the inserts still fit)
    ("engine/snapshot-automation", "adi_daw/tests/test_automation_snapshot.cpp"): [
        (r'db\.exec\("CREATE TABLE automation_segment_ranges\(', 'db.exec("CREATE TABLE IF NOT EXISTS automation_segment_ranges('),
    ],
    ("engine/clip-details", "adi_daw/tests/test_clip_details.cpp"): [
        (r'exec\("CREATE TABLE clip_properties\(', 'exec("CREATE TABLE IF NOT EXISTS clip_properties('),
    ],
    # the clip-properties test simulated N17's clip_playback_details with its own columns; N17 is
    # native in the chain with different columns, and its own tests cover the seam
    ("engine/clip-properties", "adi_daw/tests/test_clip_properties.cpp"): [
        (r'(void retainedClocks\(\) \{\r?\n    Fixture f\("retained"\);auto& db=f\.db\(\);)',
         r'\1' + '\n    if(db.execAndGet("SELECT count(*) FROM sqlite_master WHERE name=\'clip_playback_details\'").getInt()>0)return; // N17 is native here; its own tests cover the seam (chain)'),
        # the upgraded file's minor is whatever the chain says at that link
        (r"store->schemaMinor\(\)==11 && store->upgradedFromMinor\(\)==10", "store->schemaMinor()==adi::kSchemaMinor && store->upgradedFromMinor()==10"),
    ],
    # the snapshot-automation test's hand-made automation_segment_ranges is native from N17 on, whose
    # CHECK/UNIQUE constraints refuse the invalid rows before the reader can
    ("engine/phase-c-editing", "adi_daw/tests/test_automation_snapshot.cpp"): [
        (r'(        )db\.exec\(std::string\("UPDATE automation_segment_ranges SET "\)\+change\);(\r?\n)(        const auto bad=)',
         r'\1try { db.exec(std::string("UPDATE automation_segment_ranges SET ")+change); }\2\1catch(const std::exception&) { continue; } // the native table refuses it before the reader can (chain)\2\3'),
        (r'    db\.exec\("INSERT INTO automation_segment_ranges SELECT 14,(.*?)"\);(\r?\n)    const auto duplicate=SnapshotBuilder::fromStore\(\*f\.store,\*prior\);\2    check\(duplicate->findAutomationLane\(1\)->envelopes\[0\]->points\.empty\(\) &&\2          !duplicate->findAutomationLane\(1\)->envelopes\[0\]->problem\.empty\(\),"duplicate storage ordinal refuses the whole envelope"\);',
         r'    try {\2        db.exec("INSERT INTO automation_segment_ranges SELECT 14,\1");\2        const auto duplicate=SnapshotBuilder::fromStore(*f.store,*prior);\2        check(duplicate->findAutomationLane(1)->envelopes[0]->points.empty() &&\2              !duplicate->findAutomationLane(1)->envelopes[0]->problem.empty(),"duplicate storage ordinal refuses the whole envelope");\2    } catch(const std::exception&) { check(true,"duplicate storage ordinal is refused by the native UNIQUE(data_id,record_index)"); } // chain'),
    ],
    # SessionSpec gains performanceState (cue-reference) before delayCompensation: the positional
    # initializer in the delay-compensation test would set the wrong field
    ("engine/delay-compensation-switch", "adi_daw/tests/test_delay_compensation.cpp"): [
        (r"SessionSpec\{2,48000,64,precision,initialCompensation\}",
         "SessionSpec{.channels=2,.sampleRate=48000,.maxFrames=64,.precision=precision,.delayCompensation=initialCompensation}"),
    ],
    # tests that compared the upgraded file's minor with the branch's own number: the current
    # minor is whatever the chain says at that link
    # ...and the tests that reconstructed "the frozen predecessor" by dropping their own table and
    # marking the file 1.9: in the chain every later minor's table would collide with its migration
    # step, so they drop everything added since 1.9, in reverse order (children before parents)
    ("engine/split-stereo-pan", "adi_daw/tests/test_split_pan.cpp"): [
        (r"p\.store->schemaMinor\(\)==10 &&", "p.store->schemaMinor()==adi::kSchemaMinor &&"),
        (r'old\.exec\("DROP TABLE mixer_pan_settings"\);',
         '{auto gone=adi::tablesAddedAfter(9);for(auto it=gone.rbegin();it!=gone.rend();++it) old.exec("DROP TABLE "+*it);} /* every table since 1.9: the predecessor is 1.9 itself, not this minor alone (chain) */'),
    ],
    ("engine/track-recording-options", "adi_daw/tests/test_track_recording.cpp"): [
        (r"project\.store->schemaMinor\(\)==10 &&", "project.store->schemaMinor()==adi::kSchemaMinor &&"),
        (r'store\.db\(\)\.exec\("DROP TABLE track_recording_options"\);',
         '{auto gone=adi::tablesAddedAfter(9);for(auto it=gone.rbegin();it!=gone.rend();++it) store.db().exec("DROP TABLE "+*it);} /* every table since 1.9 (chain) */'),
    ],
    ("engine/return-send-tap", "adi_daw/tests/test_return_send_tap.cpp"): [
        (r"upgraded->schemaMinor\(\)==10 &&", "upgraded->schemaMinor()==adi::kSchemaMinor &&"),
    ],
    # the shapes branch registers the point ops under its own name beside the chain's registration
    # of the same descriptors: one registration
    ("engine/automation-shapes", "adi_daw/src/adi/ops_catalog.cpp"): [
        (r"\r?\n        const auto pointOps = automation::descriptors\(\);\r?\n        v\.insert\(v\.end\(\), pointOps\.begin\(\), pointOps\.end\(\)\);", ""),
        # ...and keep-both doubled the two-line lane registration
        (r"(\r?\n        const auto lanes = automation::laneDescriptors\(\);\r?\n        v\.insert\(v\.end\(\), lanes\.begin\(\), lanes\.end\(\)\);)\1", r"\1"),
    ],
    # phase C's captureDeleted restores a deleted clip's row exactly on undo (Live's undo), where
    # main's inverse recreated it without its muted state; the seams test deactivated two clips,
    # computed but never submitted their reactivation, deleted and undid them, and expected them
    # back active: it now submits the reactivation it computed
    ("engine/phase-c-editing", "adi_daw/tests/test_ui_seams.cpp"): [
        (r'check\(on\.ops\.size\(\) == 2 && on\.ops\[0\]\.payload\["muted"\] == false && on\.ops\[0\]\.label == "Activate Clips",',
         'check(on.ops.size() == 2 && on.ops[0].payload["muted"] == false && on.ops[0].label == "Activate Clips" &&\n'
         '              sub.submit(std::move(on.ops)).ok, // submitted: undo restores a deleted clip exactly, muted included (chain)'),
    ],
    # clip-properties sets its playEnd right after the unwarped source length; phase-c's warp block
    # replaces clip->length below it, so a warped clip would stop at its unwarped end (the three
    # warp "heard" suites): set it after the warp length and the source-end pin instead
    ("engine/clip-properties", "adi_daw/src/adi/engine/clip_playback.cpp"): [
        (r"\r?\n            clip->playEnd = clip->length;(?=\r?\n            const auto origin=)", ""),
        (r"(?<!\(chain\)\n)(?<!\(chain\)\r\n)(            if \(!row\.loopEnabled && row\.endMarkerTicks\) \{)",
         "            clip->playEnd = clip->length; // after the warp length and the source-end pin (chain)\n" r"\1"),
    ],
    # the single-base merge of this criss-cross link matched send-automation's two insertions against
    # repeated loop text and placed them after the loop / after the next function; put each back
    # where the branch has it (the writing-lane sweep, and beginAutomationWrite)
    ("engine/send-automation", "adi_daw/src/adi/engine/session.cpp"): [
        (r"(        for \(const auto& \[key, laneId\] : stripAutomation_->laneFor\) known = known \|\| laneId == \*it;\r?\n)"
         r"(        for \(const auto& \[key, laneId\] : deviceAutomation_->laneFor\) known = known \|\| laneId == \*it;\r?\n"
         r"        it = known \? std::next\(it\) : writingAutomation_\.erase\(it\);\r?\n    \}\r?\n)"
         r"        for \(const auto& \[key, laneId\] : stripAutomation_->sendLaneFor\) known = known \|\| laneId == \*it;\r?\n",
         r"\1        for (const auto& [key, laneId] : stripAutomation_->sendLaneFor) known = known || laneId == *it;" "\n" r"\2"),
        (r"(?m)(    if \(stripAutomation_\)\r?\n        for \(const auto& \[key, id\] : stripAutomation_->laneFor\) known = known \|\| id == laneId;\r?\n)"
         r"(    if \(deviceAutomation_\)\r?\n        for \(const auto& \[key, id\] : deviceAutomation_->laneFor\) known = known \|\| id == laneId;\r?\n"
         r"    if \(!known\) return false;[\s\S]*?^bool Session::finishAutomationWrite[\s\S]*?^\}\r?\n\r?\n)"
         r"    if \(stripAutomation_\)\r?\n        for \(const auto& \[key, id\] : stripAutomation_->sendLaneFor\) known = known \|\| id == laneId;\r?\n",
         r"\1    if (stripAutomation_)" "\n" r"        for (const auto& [key, id] : stripAutomation_->sendLaneFor) known = known || id == laneId;" "\n" r"\2"),
    ],
    # default-returns answers the window's N41: New makes Live's default Set with A Reverb and
    # B Delay (ids 6 and 7, before Main in the arrangement), so the window's tests written for a
    # five-track New (#330) count seven and add their test track as 8
    ("engine/default-returns", "adi_daw/tests/test_ui_document.cpp"): [
        (r"made->tracks\.size\(\) == 5 && made->findTrack\(1\) != nullptr &&", "made->tracks.size() == 7 && made->findTrack(1) != nullptr &&"),
        (r'made->findTrack\(5\)->name == "4 Audio",\r?\n          "New makes Main and Live\'s default Set: 1 MIDI, 2 MIDI, 3 Audio, 4 Audio"\);',
         'made->findTrack(5)->name == "4 Audio" && made->findTrack(6)->name == "A Reverb" &&\n              made->findTrack(7)->name == "B Delay",\n          "New makes Main and Live\'s default Set: 1 MIDI, 2 MIDI, 3 Audio, 4 Audio, A Reverb, B Delay (N41)");'),
        (r"model\(\)\.tracks\.size\(\) == 5", "model().tracks.size() == 7"),
        (r'add\.payload = \{\{"id", 6\}, \{"kind", "audio"\}, \{"name", "Tone"\}\};', 'add.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Tone"}};'),
        (r"return id == 6 \? std::vector<engine::Node \*>\{&tone\}", "return id == 8 ? std::vector<engine::Node *>{&tone}"),
        (r"reopen->view\(\)\.current\(\)->tracks\.size\(\) == 6,", "reopen->view().current()->tracks.size() == 8,"),
        (r'ui::dropAudio\(\*dropped->view\(\)\.current\(\), 6, 0, "kick", import\)', 'ui::dropAudio(*dropped->view().current(), 8, 0, "kick", import)'),
        (r"dropped->view\(\)\.current\(\)->findTrack\(6\)", "dropped->view().current()->findTrack(8)"),
        (r'audioTrack\.payload = \{\{"id", 6\}, \{"kind", "audio"\}, \{"name", "Drums"\}\};', 'audioTrack.payload = {{"id", 8}, {"kind", "audio"}, {"name", "Drums"}};'),
        (r"Live's default Set \(#307\), tracks 1 to 5, so the drop's audio track is a sixth\.", "Live's default Set (#307, with its two returns since N41), tracks 1 to 7, so the drop's audio track is an eighth."),
    ],
    ("engine/default-returns", "adi_daw/tests/test_ui_shell.cpp"): [
        (r'check\(laneOrder\(root\) == std::vector<std::int64_t>\{2, 3, 4, 5, 1\} &&\r?\n              root\.reader\(\)\.findTrack\(2\)->name == "1 MIDI" && root\.reader\(\)\.findTrack\(5\)->name == "4 Audio",\r?\n          "New shows Live\'s default Set: 1 MIDI, 2 MIDI, 3 Audio, 4 Audio, then Main"\);',
         'check(laneOrder(root) == std::vector<std::int64_t>{2, 3, 4, 5, 6, 7, 1} &&\n              root.reader().findTrack(2)->name == "1 MIDI" && root.reader().findTrack(5)->name == "4 Audio" &&\n              root.reader().findTrack(7)->name == "B Delay",\n          "New shows Live\'s default Set: 1 MIDI, 2 MIDI, 3 Audio, 4 Audio, A Reverb, B Delay, then Main (N41)");'),
    ],
    # session.cpp gains an adi::engine::detail namespace from the chain, which shadows adi::detail
    # for track-freeze's unqualified calls
    ("engine/track-freeze", "adi_daw/src/adi/engine/session.cpp"): [
        (r"(?<![\w:])detail::signedInteger\(", "adi::detail::signedInteger("),
    ],
}
for (eb, ef), rx in REGEX_EDITS.items():
    if eb != branch:
        continue
    p = wt / ef
    text = p.read_text(encoding="utf-8", newline="")
    for pattern, repl in rx:
        text, k = re.subn(pattern, repl, text)
        # a stacked link runs its edits twice (stack base, then predecessor): an edit already in
        # place is recognised by its replacement's first literal line, back-references stripped
        already = re.sub(r"\\[0-9]", "", repl).split("\n")[0].strip()
        if not k and already and already not in text:  # a deletion or a back-reference-only edit already done is fine
            print(f"  REGEX EDIT in {ef} matched nothing: {pattern}"); humans[ef] = ["regex edit"]
    p.write_text(text, encoding="utf-8", newline="")
    print(f"  regex-edited {ef}")

# 1c''. a criss-cross link's keep-both leaves exact duplicates behind: a second #include, a test's
# add_executable / target_link_libraries / add_test lines repeated; keep the first of each
for f in conflicted:
    p = wt / f
    if not p.exists() or not (f.endswith((".cpp", ".hpp")) or f.endswith("CMakeLists.txt")):
        continue
    text = p.read_text(encoding="utf-8", newline="")
    lines = text.split("\n"); seen = set(); out = []
    for l in lines:
        key = l.rstrip("\r")
        dup = key.startswith("#include") or (f.endswith("CMakeLists.txt") and "_tests" in key and key.strip())
        if dup and key in seen:
            continue
        if dup:
            seen.add(key)
        out.append(l)
    if len(out) != len(lines):
        p.write_text("\n".join(out), encoding="utf-8", newline="")
        print(f"  deduped {f}: {len(lines) - len(out)} line(s)")

# 1c'''' . the op catalogue's registration list: branches that integrated their dependencies carry
# their own copy of a registration (`const auto X = ...;` then `v.insert(v.end(), X...)`), and a
# keep-both or single-base merge can stack it on the chain's copy; collapse an adjacent repeat of
# such a two-line block, on every link, conflicted or not
catalog = wt / "adi_daw/src/adi/ops_catalog.cpp"
if catalog.exists():
    lines = catalog.read_text(encoding="utf-8", newline="").split("\n")
    out, k, removed = [], 0, 0
    while k < len(lines):
        block = lines[k:k + 2]
        if (len(block) == 2 and block[0].strip().startswith("const auto ") and "v.insert(v.end()" in block[1]
                and len(out) >= 2 and [l.rstrip("\r") for l in out[-2:]] == [l.rstrip("\r") for l in block]):
            k += 2; removed += 1
            continue
        out.append(lines[k]); k += 1
    if removed:
        catalog.write_text("\n".join(out), encoding="utf-8", newline="")
        print(f"  ops_catalog.cpp: {removed} repeated registration(s) collapsed")

# 1c'''. OPS.md's headline counts what its tables hold (a criss-cross link's count arithmetic adds
# a delta the branch had already reconciled itself); the row shape is validate_ops.py's
ops_md = wt / "adi_daw/docs/OPS.md"
if ops_md.exists():
    txt = ops_md.read_text(encoding="utf-8", newline="")
    OPS_ROW = re.compile(r"^\| `([a-z][a-zA-Z0-9]*\.[a-zA-Z0-9]+)` \| ([etsh]) \| (\*\*P\*\*|[NSGP]) \| ([^|]+?) \| (P[0-3]) \|\r?$", re.M)
    ops_rows = OPS_ROW.findall(txt)
    if ops_rows:
        per = {p: sum(1 for r in ops_rows if r[4] == p) for p in ("P0", "P1", "P2", "P3")}
        headline = f"**{len(ops_rows)} ops** — {per['P0']} P0, {per['P1']} P1, {per['P2']} P2, {per['P3']} P3"
        txt2, k = re.subn(r"\*\*\d+ ops\*\* — \d+ P0, \d+ P1, \d+ P2, \d+ P3", headline, txt)
        if k == 1 and txt2 != txt:
            ops_md.write_text(txt2, encoding="utf-8", newline="")
            print(f"  OPS.md headline: {headline}")

# 1c'. test_migrate.cpp: two branches pin the fixture's sample rate at 48 kHz while the others keep
# main's fixture and widen the expectation (48 kHz before 1.9, 44.1 kHz from 1.9); the chain keeps
# main's fixture everywhere so the one expectation holds at every link
tmp = wt / "adi_daw/tests/test_migrate.cpp"
if tmp.exists():
    text = tmp.read_text(encoding="utf-8", newline="")
    text2, k = re.subn(
        r"(    // Preserve an explicit stored rate across every predecessor, including\r?\n    // 1\.9 whose default already changed to 44\.1 kHz\.\r?\n)?"
        r"    db\.exec\(\"INSERT INTO project\(id, name, sample_rate\) VALUES \(1, 'Old Song', 48000\)\"\);",
        "    db.exec(\"INSERT INTO project(id, name) VALUES (1, 'Old Song')\");", text)
    if k:
        tmp.write_text(text2, encoding="utf-8", newline="")
        print("  test_migrate.cpp: fixture back to main's (no pinned sample rate)")

# 1d. textproj_store.cpp's kTables size is the number of entries it holds (both sides add rows,
# and the size line merges cleanly when only one side touched it)
tp = wt / "adi_daw/src/adi/textproj_store.cpp"
if tp.exists():
    text = tp.read_text(encoding="utf-8", newline="")
    m = re.search(r"std::array<TableCoverage, (\d+)> kTables\{\{(.*?)^\s*\}\};", text, re.S | re.M)
    if m:
        count = len(re.findall(r'^\s*\{"', m.group(2), re.M))
        if count != int(m.group(1)):
            text = text[:m.start(1)] + str(count) + text[m.end(1):]
            tp.write_text(text, encoding="utf-8", newline="")
            print(f"  kTables size {m.group(1)} -> {count}")

# 2. renumber the branch's own added lines (never inside the frozen history)
if renum:
    files = [l for l in git("diff", "--name-only", "origin/main", f"origin/{branch}").stdout.split("\n") if l]
    for f in files:
        p = wt / f
        if not p.exists() or p.suffix in (".adi", ".wav", ".png") or "docs/format/history/" in f:
            continue
        diff = git("diff", "origin/main", f"origin/{branch}", "--", f).stdout
        added = {l[1:].rstrip("\r") for l in diff.split("\n") if l.startswith("+") and not l.startswith("+++")}
        added = {l for l in added if has_old(l)}
        if not added:
            continue
        text = p.read_text(encoding="utf-8", newline="")
        lines = text.split("\n")
        changed = 0
        for k, l in enumerate(lines):
            s = l.rstrip("\r")
            if s in added:
                lines[k] = mapnum(s) + ("\r" if l.endswith("\r") else "")
                changed += 1
        p.write_text("\n".join(lines), encoding="utf-8", newline="")
        print(f"  renumbered {changed} line(s) in {f}")
    # 2b. schema.sql's header names the current minor: the chain's predecessor wrote its own there
    top = max(n for _, n in renum)
    sp = wt / "adi_daw/docs/format/schema.sql"
    st = sp.read_text(encoding="utf-8", newline="")
    st2, k = re.subn(r"^(--  Normative schema, version 1\.)\d+", lambda m: m.group(1) + str(top), st, count=1, flags=re.M)
    if k:
        sp.write_text(st2, encoding="utf-8", newline="")
        print(f"  schema.sql header says 1.{top}")
    # 3. freeze PREV's schema as history/schema-1.<NEW-1>.sql for the lowest NEW
    lowest_new = min(n for _, n in renum)
    hist = wt / "adi_daw/docs/format/history"
    prev_schema = git("show", f"{prev}:adi_daw/docs/format/schema.sql").stdout
    (hist / f"schema-1.{lowest_new - 1}.sql").write_text(prev_schema, encoding="utf-8", newline="")
    print(f"  froze {prev}'s schema.sql as schema-1.{lowest_new - 1}.sql")
    # 3b. a two-step branch froze its own intermediate schema-1.<OLD>.sql: rebuild it as
    # schema-1.<NEW>.sql = PREV's schema + the hunks of the branch's own 1.<OLD-1> -> 1.<OLD> diff
    # that are not version lines, with the version lines set to NEW
    for o, n in renum:
        own = git("show", f"origin/{branch}:adi_daw/docs/format/history/schema-1.{o}.sql", check=False)
        if own.returncode:
            continue
        before = git("show", f"origin/{branch}:adi_daw/docs/format/history/schema-1.{o - 1}.sql").stdout
        with tempfile.TemporaryDirectory() as td:
            a, bp, target = Path(td) / "a.sql", Path(td) / "b.sql", Path(td) / "t.sql"
            a.write_text(before, encoding="utf-8", newline="")
            bp.write_text(own.stdout, encoding="utf-8", newline="")
            target.write_text(prev_schema, encoding="utf-8", newline="")
            d = subprocess.run(["diff", "-u", str(a), str(bp)], capture_output=True, text=True, encoding="utf-8").stdout
            subprocess.run(["patch", "-p0", "--no-backup-if-mismatch", str(target)], input=d, capture_output=True, text=True, encoding="utf-8")
            rej = Path(str(target) + ".rej")
            rejected = rej.read_text(encoding="utf-8") if rej.exists() else ""
            bad = [l for l in rejected.split("\n") if l.startswith(("+", "-")) and not l.startswith(("+++", "---"))
                   and not re.search(r"Normative schema, version|user_version|schema_minor|schema_major", l)]
            text = target.read_text(encoding="utf-8", newline="")
        text = re.sub(r"^(--  Normative schema, version 1\.)\d+", lambda m: m.group(1) + str(n), text, count=1, flags=re.M)
        text = re.sub(r"(PRAGMA user_version\s*=\s*)\d+", lambda m: m.group(1) + str(1000 + n), text, count=1)
        text = re.sub(r"(\('schema_minor',\s*')\d+(')", lambda m: m.group(1) + str(n) + m.group(2), text, count=1)
        (hist / f"schema-1.{n}.sql").write_text(text, encoding="utf-8", newline="")
        print(f"  rebuilt the intermediate frozen schema-1.{n}.sql from schema-1.{n - 1}.sql + the branch's 1.{o} objects"
              + (f"; REJECTED non-version lines: {bad[:4]}" if bad else ""))
        if bad:
            humans[f"history/schema-1.{n}.sql"] = ["patch rejects"]

leftover = [f for f in conflicted if re.search(r"^<<<<<<< ", (wt / f).read_text(encoding="utf-8", newline=""), re.M)]
if humans or leftover:
    print("HUMAN:", humans or leftover)
    sys.exit(1)
git("add", "-u", "adi_daw")
if renum:
    git("add", "adi_daw/docs/format/history")
print("STAGED")
