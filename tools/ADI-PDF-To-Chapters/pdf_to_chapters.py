"""ADI-PDF-To-Chapters: split long PDF manuals into chapter-sized PDFs.

Drop PDFs next to this script and run it (run.cmd). For every <name>.pdf it writes
a folder <name>/ holding one PDF per chapter, plus an INDEX.md listing the sections
in each file. The rules:

  - one file per chapter; a chapter longer than --max-pages (30) is cut at section
    starts into balanced parts, going a level deeper only where it has to;
  - short chapters stay on their own, except that leading one-page chapters (a
    welcome page, a disclaimer) are folded into the chapter after them;
  - a chapter or section that starts mid-page puts that page in both files, so no
    file ever loses the lines above its first heading;
  - filenames carry the manual's own printed page numbers: "06 - Arrangement View
    [p160-181].pdf"; inside, the original page labels are kept and every heading
    is a bookmark.

The structure comes from, in order: the PDF's bookmarks; a clickable table of
contents (link targets; numbering or indentation for depth); heading font sizes.
Bookmarks that are one- or two-page topics rather than chapters (web-help PDFs)
are treated as one chapter and cut into balanced parts at topic starts.

Two traps it handles. Some PDF writers (WeasyPrint, LibreOffice) give every page
one resource dictionary naming every image in the book, so a naive split copies
all the images into every part; each page is pruned to what it draws first. And a
bookmark's height alone doesn't say whether a section starts at the top of its
page, so the page text above the heading is checked instead.

Every page of every part is checked against the original (same content stream,
same rendered pixels) unless --no-verify is given.
"""
from __future__ import annotations

import argparse
import logging
import math
import re
import statistics
import sys
from collections import Counter
from pathlib import Path

try:
    import pdfplumber
    import pypdfium2 as pdfium
    import pypdfium2.raw as pdfium_c
    from pypdf import PdfReader, PdfWriter
    from pypdf.generic import DictionaryObject, Fit, IndirectObject, NameObject, StreamObject
except ImportError as e:
    sys.exit(f"missing dependency: {e.name} -- run install.cmd first")

# pypdf and pdfminer warn about every harmless quirk of real-world PDFs
logging.getLogger("pypdf").setLevel(logging.ERROR)
logging.getLogger("pdfminer").setLevel(logging.ERROR)

FRONT_RE = re.compile(r"^\s*((table\s+of\s+)?contents|toc|cover|title\s*page|copyright|imprint)\b", re.I)
NUM_RE = re.compile(r"^\s*(\d+)(?:\.\d+)*\.?\s+(?=\S)")
SECTION_NUM_RE = re.compile(r"^\s*(\d+(?:\.\d+)*)\.?\s+(?=\S)")
BARE_RE = re.compile(r"^\s*\d+(?:\.(?:\d+|[a-z]+|[ivx]+))*\.?\s+(?=\S)")
TOC_ENTRY_RE = re.compile(r"(\.{3,}|(\s\.){3,}|…)\s*(\d+|[ivxlc]+)\s*$", re.I)
TOC_LINE_RE = re.compile(r"^(?P<title>.*?)(?:\s*[.·…_](?:\s*[.·…_])+\s*|\s+)(?P<num>\d+|[ivxlc]+)$")
OUTPUT_FILE_RE = re.compile(r" \[p[^\]]+\]\.pdf$")


def clean(t: str) -> str:
    t = t.replace("�", "'").replace("’", "'").replace("​", "")
    return re.sub(r"\s+", " ", t).strip()


def bare(t: str) -> str:
    """Title without its section number: '4.6 Library' -> 'Library'."""
    return BARE_RE.sub("", t, count=1)


def node(title, page, top, kids=None):
    return {"title": clean(title), "page": page, "top": top, "kids": kids or []}


def pos(n):
    return (n["page"], -(n["top"] if n["top"] is not None else 0))


def walk_nodes(nodes):
    for n in nodes:
        yield n
        yield from walk_nodes(n["kids"])


class Pages:
    """Page text and heading positions, read with pdfium."""

    def __init__(self, path: Path):
        self.doc = pdfium.PdfDocument(str(path))
        self._chars = {}

    def chars(self, page_no):
        """(centre y, character) for every visible character on the page."""
        if page_no not in self._chars:
            tp = self.doc[page_no - 1].get_textpage()
            out = []
            for i in range(tp.count_chars()):
                ch = chr(pdfium_c.FPDFText_GetUnicode(tp.raw, i))
                if ch.isspace() or ch == "\x00":
                    continue
                _, b, _, t = tp.get_charbox(i)
                if t > b:
                    out.append(((b + t) / 2, ch))
            self._chars[page_no] = out
        return self._chars[page_no]

    def text_above(self, page_no, top, body_top):
        """Text on the page between the running header and a heading at `top`."""
        if top is None:
            return "(heading position unknown)"
        return "".join(ch for y, ch in self.chars(page_no) if top + 1 < y <= body_top + 3)

    def looks_like_toc(self, page_no):
        text = self.doc[page_no - 1].get_textpage().get_text_range()
        return sum(1 for line in text.splitlines() if TOC_ENTRY_RE.search(line)) >= 5

    def locate(self, page_no, title):
        """Top of `title` on the page (the largest-type match), or None."""
        tp = self.doc[page_no - 1].get_textpage()
        title = clean(title)
        for q in dict.fromkeys((title[:60], bare(title)[:40], bare(title)[:20])):
            if len(q) < 3:
                continue
            best = None
            s = tp.search(q, match_case=False)
            while hit := s.get_next():
                idx, cnt = hit
                size = pdfium_c.FPDFText_GetFontSize(tp.raw, idx)
                top = max(tp.get_charbox(i)[3] for i in range(idx, idx + cnt))
                if best is None or size > best[0] + 0.1:
                    best = (size, top)
            if best:
                return round(best[1] + 1, 1)
        return None


# --------------------------------------------------------------------------- structure

def structure_from_outline(reader):
    """Chapters from the PDF's bookmarks: (doc_title, front_nodes, chapters) or None."""
    def walk(items):
        out = []
        for it in items:
            if isinstance(it, list):
                (out[-1]["kids"] if out else out).extend(walk(it))
                continue
            try:
                page = reader.get_destination_page_number(it)
            except Exception:
                page = None
            top = getattr(it, "top", None)
            out.append(node(str(it.title), page + 1 if page is not None and page >= 0 else None,
                            float(top) if isinstance(top, (int, float)) else None))
        return out

    def hoist(nodes):  # bookmarks without a page: keep their children in their place
        out = []
        for n in nodes:
            n["kids"] = hoist(n["kids"])
            out.extend(n["kids"] if n["page"] is None else [n])
        return out

    try:
        tree = walk(reader.outline)
    except Exception:
        return None
    doc_title = None
    while len(tree) == 1 and tree[0]["kids"]:  # one root wrapping everything
        doc_title = tree[0]["title"]
        tree = tree[0]["kids"]
    tree = sorted(hoist(tree), key=pos)
    front = []
    while len(tree) > 1 and (FRONT_RE.match(tree[0]["title"]) or (tree[0]["page"] == 1 and tree[1]["page"] <= 3)):
        front.append(tree.pop(0))
    if len(tree) < 2:
        return None
    return doc_title, front, tree


def link_target(reader, annot, page_index):
    """(page, top) a link annotation jumps to, or None."""
    dest = annot.get("/Dest")
    if dest is None and "/A" in annot:
        action = annot["/A"].get_object()
        if action.get("/S") == "/GoTo":
            dest = action.get("/D")
    if dest is None:
        return None
    dest = dest.get_object() if isinstance(dest, IndirectObject) else dest
    try:
        if isinstance(dest, list):
            ref = dest[0]
            page = page_index.get(ref.idnum) if isinstance(ref, IndirectObject) else int(ref)
            top = dest[3] if len(dest) > 3 and str(dest[1]) == "/XYZ" else None
        else:
            named = reader.named_destinations
            d = named.get(str(dest)) or named.get(str(dest).lstrip("/"))
            if d is None:
                return None
            page, top = reader.get_destination_page_number(d), d.top
    except Exception:
        return None
    if page is None or page < 0:
        return None
    return page + 1, float(top) if isinstance(top, (int, float)) else None


def structure_from_toc_links(path, reader, pages):
    """Chapters from a clickable table of contents: each line's link gives the
    target, its indent the depth. (None, front_nodes, chapters) or None."""
    page_index = {pg.indirect_reference.idnum: i for i, pg in enumerate(reader.pages)}
    toc = []
    for i in range(min(60, len(reader.pages))):
        links = []
        for a in reader.pages[i].get("/Annots") or []:
            a = a.get_object()
            if a.get("/Subtype") != "/Link":
                continue
            tgt = link_target(reader, a, page_index)
            if tgt and tgt[0] > i + 1:
                ys = [float(a["/Rect"][1]), float(a["/Rect"][3])]
                links.append({"lo": min(ys), "hi": max(ys), "target": tgt})
        if len(links) >= 5:
            toc.append((i + 1, links))
        elif toc:
            break
    if not toc:
        return None

    entries = []
    with pdfplumber.open(str(path)) as pdf:
        for page_no, links in toc:
            lines = []
            for line in pdf.pages[page_no - 1].extract_text_lines():
                chars = [c for c in line["chars"] if c["text"].strip()]
                if not chars:
                    continue
                cur = {"text": line["text"].strip(), "x0": line["x0"], "chars": chars,
                       "size": Counter(round(c["size"], 1) for c in chars).most_common(1)[0][0],
                       "hi": max(c["y1"] for c in chars), "lo": min(c["y0"] for c in chars)}
                prev = lines[-1] if lines else None
                # a long title wraps: a line without a page number, then its continuation
                # (same type, not indented less, no section number of its own) right below
                if (prev and not TOC_LINE_RE.match(prev["text"]) and TOC_LINE_RE.match(cur["text"])
                        and not SECTION_NUM_RE.match(cur["text"]) and cur["x0"] >= prev["x0"] - 1
                        and abs(cur["size"] - prev["size"]) <= 0.6
                        and prev["lo"] - cur["hi"] < 0.8 * (prev["hi"] - prev["lo"])):
                    prev.update(text=f"{prev['text']} {cur['text']}", chars=prev["chars"] + chars, lo=cur["lo"])
                    continue
                lines.append(cur)
            used = set()
            for line in lines:
                hi, lo = line["hi"], line["lo"]
                best, best_ov = None, 0.0
                for k, lk in enumerate(links):
                    ov = min(hi, lk["hi"]) - max(lo, lk["lo"])
                    if k not in used and ov > best_ov:
                        best, best_ov = k, ov
                if best is None or best_ov < 0.4 * min(hi - lo, lk_height(links[best])):
                    continue
                used.add(best)
                m = TOC_LINE_RE.match(line["text"])
                title = (m.group("title") if m else line["text"]).strip(" .·…_")
                size = Counter(round(c["size"], 1) for c in line["chars"]).most_common(1)[0][0]
                if title:
                    entries.append({"title": title, "x0": round(line["x0"]), "size": size, "toc_page": page_no,
                                    "page": links[best]["target"][0], "top": links[best]["target"][1]})
    # a table of contents only moves forward through the book: a big jump back means
    # another linked list has started (Reaper follows its TOC with a "what's new" list)
    furthest = 0
    for k, e in enumerate(entries):
        if e["page"] < furthest - 2:
            entries = entries[:k]
            break
        furthest = max(furthest, e["page"])
    if len(entries) < 5:
        return None
    set_toc_levels(entries)
    for e in entries:
        if e["top"] is None:
            e["top"] = pages.locate(e["page"], e["title"])
    front, chapters = nest(entries)
    return (None, front, chapters) if len(chapters) >= 2 else None


def lk_height(link):
    return link["hi"] - link["lo"]


def set_toc_levels(entries):
    """Depth of each TOC entry: from its numbering ('2.24' is level 2) when the
    entries are numbered; else from its indent, measured separately on odd and even
    pages (books mirror their margins); else from its type size."""
    nums = [SECTION_NUM_RE.match(e["title"]) for e in entries]
    if sum(1 for m in nums if m) >= 0.8 * len(entries):
        known = {}  # indent -> level, from the numbered entries, per odd/even page
        for e, m in zip(entries, nums):
            if m:
                e["level"] = min(m.group(1).count(".") + 1, 4)
                known.setdefault(e["toc_page"] % 2, []).append((e["x0"], e["level"]))
        for e, m in zip(entries, nums):
            if not m:  # e.g. "Index": the level of the numbered entries nearest its indent
                ref = known.get(e["toc_page"] % 2) or [x for v in known.values() for x in v]
                e["level"] = min(ref, key=lambda r: (abs(r[0] - e["x0"]), r[1]))[1]
        return
    indented = False
    for parity in (0, 1):
        group = [e for e in entries if e["toc_page"] % 2 == parity]
        clusters = []
        for x in sorted({e["x0"] for e in group}):
            if clusters and x - clusters[-1][-1] <= 2:
                clusters[-1].append(x)
            else:
                clusters.append([x])
        level_of = {x: i + 1 for i, c in enumerate(clusters) for x in c}
        for e in group:
            e["level"] = min(level_of[e["x0"]], 4)
        indented |= len(clusters) > 1
    if not indented:
        sizes = sorted({e["size"] for e in entries}, reverse=True)
        for e in entries:
            e["level"] = min(sizes.index(e["size"]) + 1, 4)


def structure_from_headings(path, n_pages):
    """Chapters from heading type sizes: every size clearly above the body text is
    a heading level, biggest first. (None, front_nodes, chapters) or None."""
    lines, toc_pages = [], set()
    with pdfplumber.open(str(path)) as pdf:
        for page_no, pg in enumerate(pdf.pages, 1):
            page_lines = []
            for line in pg.extract_text_lines():
                chars = [c for c in line["chars"] if c["text"].strip()]
                if not chars:
                    continue
                page_lines.append({"page": page_no, "text": line["text"].strip(), "n": len(chars),
                                   "size": Counter(round(c["size"], 1) for c in chars).most_common(1)[0][0],
                                   "top": round(max(c["y1"] for c in chars) + 1, 1)})
            if sum(1 for l in page_lines if TOC_ENTRY_RE.search(l["text"])) >= 5:
                toc_pages.add(page_no)
            lines += page_lines
    if not lines:
        return None
    weight = Counter()
    for l in lines:
        weight[l["size"]] += l["n"]
    body = weight.most_common(1)[0][0]
    heads = [l for l in lines if l["size"] >= body + 0.5 and l["page"] not in toc_pages
             and 2 <= len(l["text"]) <= 120 and re.search(r"[A-Za-z]", l["text"])]
    clusters = []
    for s in sorted({h["size"] for h in heads}, reverse=True):
        if clusters and clusters[-1][-1] - s <= 0.35:
            clusters[-1].append(s)
        else:
            clusters.append([s])
    clusters = [c for c in clusters if sum(1 for h in heads if h["size"] in c) >= 2][:4]
    if not clusters:
        return None
    level_of = {s: i + 1 for i, c in enumerate(clusters) for s in c}
    entries = []
    for h in heads:
        if h["size"] not in level_of:
            continue
        lv = level_of[h["size"]]
        prev = entries[-1] if entries else None
        if prev and prev["level"] == lv and prev["page"] == h["page"] and 0 < prev["last"] - h["top"] < 1.6 * h["size"]:
            prev["title"] += " " + h["text"]  # a heading wrapped onto two lines
            prev["last"] = h["top"]
            continue
        entries.append({"title": h["text"], "page": h["page"], "top": h["top"], "level": lv, "last": h["top"]})
    front, chapters = nest(entries)
    return (None, front, chapters) if len(chapters) >= 2 else None


def nest(entries):
    """Flat (level, title, page, top) entries -> (front_nodes, chapter tree). Entries
    before the first level-1 entry are front matter."""
    first = next((i for i, e in enumerate(entries) if e["level"] == 1), None)
    if first is None:
        return [], []
    front = [node(e["title"], e["page"], e["top"]) for e in entries[:first]]
    tree, stack = [], []
    for e in entries[first:]:
        n = node(e["title"], e["page"], e["top"])
        while stack and stack[-1][0] >= e["level"]:
            stack.pop()
        (stack[-1][1]["kids"] if stack else tree).append(n)
        stack.append((e["level"], n))
    return front, tree


# --------------------------------------------------------------------------- planning

class Planner:
    def __init__(self, pages: Pages, n_pages, max_pages, body_top):
        self.pages, self.n_pages, self.max, self.body_top = pages, n_pages, max_pages, body_top

    def above(self, n):
        return self.pages.text_above(n["page"], n["top"], self.body_top)

    def at_top(self, n):
        return not self.above(n)

    def end_before(self, nxt):
        """Last page of whatever comes before `nxt`: shared if `nxt` starts mid-page."""
        return nxt["page"] - 1 if self.at_top(nxt) else nxt["page"]

    def cut_points(self, unit, end, deep):
        """Section starts inside the unit; below any section longer than the limit
        (or below every top-level section, if deep) their sub-section starts too."""
        out = []

        def walk(entries, end, level):
            for i, e in enumerate(entries):
                e_end = entries[i + 1]["page"] if i + 1 < len(entries) else end
                out.append(e)
                if e["kids"] and (e_end - e["page"] + 1 > self.max or (deep and level == 2)):
                    walk(e["kids"], e_end, level + 1)

        walk(unit["kids"], end, 2)
        first = {}
        for e in sorted(out, key=pos):  # one cut per page: the earliest entry on it
            if e["page"] > unit["page"]:
                first.setdefault(e["page"], e)
        return list(first.values()), out

    def partition(self, start, end, cuts):
        """Fewest parts with every part within the limit (if possible), then the
        split whose longest part is shortest. [(first, last, cut_entry)], longest."""
        pts = [None] + cuts

        def span(i, j):
            s = start if pts[i] is None else pts[i]["page"]
            return s, end if j == len(pts) else self.end_before(pts[j])

        n, inf = len(pts), float("inf")
        # a section longer than the limit with nothing to cut it at can't be helped:
        # aim for the limit or that section's length, whichever is longer
        target = max([self.max] + [span(i, i + 1)[1] - span(i, i + 1)[0] + 1 for i in range(n)])
        for k in range(max(1, math.ceil((end - start + 1) / self.max)), n + 1):
            dp = [[(inf, None)] * (k + 1) for _ in range(n + 1)]
            dp[0][0] = (0, None)
            for j in range(1, n + 1):
                for m in range(1, k + 1):
                    for i in range(j):
                        if dp[i][m - 1][0] == inf:
                            continue
                        s, e = span(i, j)
                        v = max(dp[i][m - 1][0], e - s + 1)
                        if v < dp[j][m][0]:
                            dp[j][m] = (v, i)
            if dp[n][k][0] <= target or k == n:
                parts, j, m = [], n, k
                while j > 0:
                    i = dp[j][m][1]
                    parts.append((*span(i, j), pts[i]))
                    j, m = i, m - 1
                return parts[::-1], dp[n][k][0]

    def plan(self, doc_title, front, chapters):
        """The list of files to write: dicts with id, name, start, end, sections."""
        chapters = sorted(chapters, key=pos)
        ends = [self.end_before(chapters[i + 1]) if i + 1 < len(chapters) else self.n_pages
                for i in range(len(chapters))]
        for ch, e in zip(chapters, ends):
            ch["end"] = e

        # chapter ids: the manual's own numbers if it numbers its chapters, else 1, 2, 3...
        nums = [NUM_RE.match(c["title"]) for c in chapters]
        numbered = sum(1 for m in nums if m) >= 0.6 * len(chapters)
        last = 0
        for c, m in zip(chapters, nums):
            last = int(m.group(1)) if numbered and m else last + 1
            c["num"] = last
        if len({c["num"] for c in chapters}) != len(chapters):
            for i, c in enumerate(chapters, 1):
                c["num"] = i

        def span(c):
            return c["end"] - c["page"] + 1

        # bookmarks that are topics of a page or two, not chapters: treat the whole
        # book as one chapter and cut it into balanced parts at topic starts
        if len(chapters) >= 6 and statistics.median(span(c) for c in chapters) <= 3:
            units = [{"members": chapters, "page": chapters[0]["page"], "top": chapters[0]["top"],
                      "end": chapters[-1]["end"], "packed": True}]
        else:
            units = [{"members": [c], "page": c["page"], "top": c["top"], "end": c["end"]} for c in chapters]
            # a leading one-page chapter (welcome, disclaimer) joins the chapter after it
            while len(units) > 1 and span(units[0]["members"][-1]) <= 1:
                a, b = units.pop(0), units[0]
                units[0] = {"members": a["members"] + b["members"], "page": a["page"], "top": a["top"],
                            "end": b["end"]}

        plan = []
        first = units[0] if units else None
        front_end = self.end_before(first) if first else self.n_pages
        if front_end >= 1:
            has_toc = any(FRONT_RE.match(n["title"]) for n in front) or any(
                self.pages.looks_like_toc(pg) for pg in range(1, front_end + 1))
            name = "Cover and Contents" if has_toc else ("Cover" if front_end == 1 else "Front Matter")
            plan.append({"id": None, "name": name, "start": 1, "end": front_end,
                         "sections": [n["title"] for n in front]})
        for u in units:
            ms = u["members"]
            if u.get("packed"):
                uid, name, kids = "01", doc_title or "Manual", ms
            elif len(ms) == 1:
                uid, name, kids = f"{ms[0]['num']:02d}", bare(ms[0]["title"]), ms[0]["kids"]
            else:
                uid = f"{ms[0]['num']:02d}-{ms[-1]['num']:02d}"
                titles = [bare(m["title"]) for m in ms]
                name = ", ".join(titles[:-1]) + " and " + titles[-1]
                kids = [dict(m) for m in ms]  # the member chapters become its sections
            unit = {"page": u["page"], "top": u["top"], "kids": kids}
            if len(ms) > 1 and not u.get("packed"):
                sections = [t for m in ms for t in [m["title"]] + [k["title"] for k in m["kids"]]]
            else:
                sections = [k["title"] for k in kids]
            if u["end"] - u["page"] + 1 <= self.max:
                plan.append({"id": uid, "name": name, "start": u["page"], "end": u["end"], "sections": sections})
                continue
            cuts, entries = self.cut_points(unit, u["end"], deep=False)
            parts, _ = self.partition(u["page"], u["end"], cuts)
            d_cuts, d_entries = self.cut_points(unit, u["end"], deep=True)
            d_parts, d_worst = self.partition(u["page"], u["end"], d_cuts)
            if len(d_parts) < len(parts) and d_worst <= self.max:  # deeper cuts only if they save a part
                parts, entries = d_parts, d_entries
            entries = sorted(entries, key=pos)
            for k, (s, e, cut) in enumerate(parts):
                lo = (0, 0) if cut is None else pos(cut)
                hi = pos(parts[k + 1][2]) if k + 1 < len(parts) else (10 ** 9, 0)
                inside = [x for x in entries if lo <= pos(x) < hi] or [{"title": name}]
                a, b = bare(inside[0]["title"]), bare(inside[-1]["title"])
                plan.append({"id": f"{uid}{chr(97 + k)}", "name": f"{name} - {a if a == b else f'{a} to {b}'}",
                             "start": s, "end": e, "sections": [x["title"] for x in inside],
                             "cut": None if cut is None else {"title": cut["title"], "above": self.above(cut)}})
        return plan


# --------------------------------------------------------------------------- writing

NAME_TOKEN_RE = re.compile(rb"/([^\s/\[\]()<>{}%]+)")


def prune_resources(reader):
    """Give every page (and every form, pattern and soft mask it draws) its own
    resource dictionary holding only the names its content stream uses."""
    done = set()

    def prune(res, content):
        names = set(NAME_TOKEN_RE.findall(content))
        new = DictionaryObject(res)
        for cat in ("/XObject", "/Pattern", "/Shading", "/ExtGState"):
            d = res.get(cat)
            if d is None:
                continue
            kept = DictionaryObject({NameObject(k): v for k, v in d.get_object().items() if k.encode()[1:] in names})
            new[NameObject(cat)] = kept
            for v in kept.values():
                prune_object(v)
        return new

    def prune_object(ref):
        key = ref.idnum if isinstance(ref, IndirectObject) else id(ref)
        if key in done:
            return
        done.add(key)
        o = ref.get_object()
        if isinstance(o, StreamObject) and "/Resources" in o:
            o[NameObject("/Resources")] = prune(o["/Resources"].get_object(), o.get_data())
        elif isinstance(o, DictionaryObject):
            sm = o.get("/SMask")
            if isinstance(sm, DictionaryObject) and "/G" in sm:
                prune_object(sm.raw_get("/G"))

    for pg in reader.pages:
        if "/Resources" in pg:
            pg[NameObject("/Resources")] = prune(pg["/Resources"].get_object(), content_bytes(pg))


def content_bytes(page):
    # not `if contents`: a content stream is a dictionary too, and an empty one is falsy
    contents = page.get_contents()
    return contents.get_data() if contents is not None else b""


ROMAN = {"i": 1, "v": 5, "x": 10, "l": 50, "c": 100, "d": 500, "m": 1000}


def roman_value(s):
    if not s or any(ch not in ROMAN for ch in s):
        return None
    v = [ROMAN[ch] for ch in s]
    return sum(-a if a < b else a for a, b in zip(v, v[1:] + [0]))


def label_runs(labels):
    """Page labels as runs [first, last, style, start] (decimal or lowercase
    roman), or [i, i, None, text] for a label that is neither."""
    runs = []
    for i, lab in enumerate(labels):
        style, val = ("/D", int(lab)) if lab.isdigit() else ("/r", roman_value(lab))
        if val is None:
            runs.append([i, i, None, lab])
        elif runs and runs[-1][2] == style and runs[-1][1] == i - 1 and runs[-1][3] + (i - runs[-1][0]) == val:
            runs[-1][1] = i
        else:
            runs.append([i, i, style, val])
    return runs


def add_bookmarks(writer, lo, hi, top_nodes):
    """Every heading on pages lo..hi, at its position; a chapter or section begun
    in an earlier file appears as '(cont.)' pointing at the first page."""
    def has_inside(n):
        return lo <= n["page"] <= hi or any(has_inside(k) for k in n["kids"])

    def add(nodes, parent, top_level):
        for n in nodes:
            if n["page"] > hi:
                continue
            inside = n["page"] >= lo
            if not inside and not (n.get("end", 0) >= lo if top_level else has_inside(n)):
                continue
            if inside:
                fit = Fit.xyz(top=n["top"]) if n["top"] is not None else Fit.fit()
                item = writer.add_outline_item(n["title"], n["page"] - lo, parent=parent, fit=fit)
            else:
                item = writer.add_outline_item(f"{n['title']} (cont.)", 0, parent=parent, fit=Fit.fit())
            add(n["kids"], item, False)

    add(top_nodes, None, True)


def file_name(p, labels, used_ids, max_len):
    """'06 - Arrangement View [p160-181].pdf', at most max_len characters (Windows
    paths stop at 260, so the name is shortened to fit the folder it goes in)."""
    pid = p["id"] if p["id"] is not None else ("00" if "00" not in used_ids else "")
    a, b = labels[p["start"] - 1], labels[p["end"] - 1]
    prefix = f"{pid} - " if pid else ""
    suffix = f" [p{a if p['start'] == p['end'] else f'{a}-{b}'}].pdf"
    room = max(20, max_len - len(prefix) - len(suffix))
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "-", p["name"].replace(": ", " - "))
    if len(name) > room and " to " in name:  # "Chapter - First to Last" -> "Chapter - First"
        name = name.rsplit(" to ", 1)[0]
    if len(name) > room:
        name = name[:room - 1].rsplit(" ", 1)[0] + "…"
    return prefix + name.rstrip(" .") + suffix


def pixels(doc, index):
    bm = doc[index].render(scale=0.3)
    raw, row = bytes(bm.buffer), bm.width * bm.n_channels
    return b"".join(raw[r * bm.stride:r * bm.stride + row] for r in range(bm.height))  # without row padding


# --------------------------------------------------------------------------- driver

def split_pdf(path: Path, max_pages, force, dry_run, verify):
    out_dir = path.with_suffix("")
    if out_dir.exists() and any(out_dir.iterdir()) and not force and not dry_run:
        print(f"  skipped: {out_dir.name}/ already exists (use --force to redo it)")
        return True
    reader = PdfReader(str(path))
    if reader.is_encrypted and not reader.decrypt(""):
        print("  skipped: the PDF is password-protected")
        return False
    n_pages = len(reader.pages)
    labels = reader.page_labels
    pages = Pages(path)

    found, how = structure_from_outline(reader), "its bookmarks"
    if not found:
        found, how = structure_from_toc_links(path, reader, pages), "its clickable table of contents"
    if not found:
        print("  no bookmarks or clickable contents; reading heading sizes...")
        found, how = structure_from_headings(path, n_pages), "heading type sizes"
    if found:
        doc_title, front, chapters = found
    else:  # nothing to go on: balanced page ranges
        how, doc_title, front = "nothing (no structure found): plain page ranges", None, []
        chapters = []
    for n in walk_nodes(chapters):  # bookmarks without a position: find the heading on the page
        if n["top"] is None:
            n["top"] = pages.locate(n["page"], n["title"])
    doc_title = doc_title or clean(str((reader.metadata or {}).get("/Title") or "")) or path.stem
    if re.search(r"\.(odt|docx?|indd|pages)$", doc_title, re.I):
        doc_title = path.stem

    tops = [n["top"] for n in walk_nodes(chapters) if n["top"] is not None]
    body_top = max(tops) if tops else float(reader.pages[0].mediabox.top)
    planner = Planner(pages, n_pages, max_pages, body_top)
    if chapters:
        plan = planner.plan(doc_title, front, chapters)
    else:
        k = max(1, math.ceil(n_pages / max_pages))
        size = math.ceil(n_pages / k)
        plan = [{"id": f"{i + 1:02d}", "name": f"Part {i + 1}", "start": s, "end": min(n_pages, s + size - 1),
                 "sections": []} for i, s in enumerate(range(1, n_pages + 1, size))]
    for p in plan:
        p["pages"] = p["end"] - p["start"] + 1
    covered = {pg for p in plan for pg in range(p["start"], p["end"] + 1)}
    assert covered == set(range(1, n_pages + 1)), "internal error: the plan misses pages"
    used_ids = {p["id"] for p in plan}
    max_len = min(150, 245 - len(str(out_dir.resolve())) - 1)
    for p in plan:
        p["file"] = file_name(p, labels, used_ids, max_len)

    print(f"  {n_pages} pages; structure from {how}; {len(chapters)} chapters -> {len(plan)} files, "
          f"longest {max(p['pages'] for p in plan)} pages")
    for p in plan:
        print(f"    {p['file']}")
    for p in plan:
        if p.get("cut") and p["cut"]["above"]:
            print(f"    ({p['id']} starts mid-page at '{p['cut']['title']}': that page is in both files)")
    pages.doc.close()
    if dry_run:
        return True

    out_dir.mkdir(exist_ok=True)
    if force:
        for f in out_dir.iterdir():
            if f.is_file() and (OUTPUT_FILE_RE.search(f.name) or f.name == "INDEX.md"):
                f.unlink()
    prune_resources(reader)
    top_nodes = [dict(n, end=plan[0]["end"]) for n in front] + chapters
    for p in plan:
        w = PdfWriter()
        w.append(reader, pages=(p["start"] - 1, p["end"]), import_outline=False)
        for a, b, style, start in label_runs([labels[i] for i in range(p["start"] - 1, p["end"])]):
            if style:
                w.set_page_label(a, b, style=style, start=start)
            else:
                w.set_page_label(a, b, prefix=start)
        add_bookmarks(w, p["start"], p["end"], top_nodes)
        w.add_metadata({"/Title": f"{doc_title} - {p['name']} (pp. {labels[p['start'] - 1]}-{labels[p['end'] - 1]})"})
        w.page_mode = "/UseOutlines"
        with open(out_dir / p["file"], "wb") as f:
            w.write(f)

    lines = [f"# {path.name}, split into chapters", "",
             f"{n_pages} pages, structure taken from {how}. One file per chapter; chapters over {max_pages}",
             "pages are cut at section starts into parts. A chapter or section that starts mid-page",
             "shares that page with the file before it. Page numbers in the filenames and inside each",
             "file match the original. Written by tools/ADI-PDF-To-Chapters.", ""]
    for p in plan:
        lines.append(f"- **{p['file'][:-4]}**")
        if p["sections"]:
            lines.append(f"  {'; '.join(p['sections'])}")
    (out_dir / "INDEX.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    total = sum((out_dir / p["file"]).stat().st_size for p in plan) / 1e6
    print(f"  wrote {len(plan)} files ({total:.1f} MB; the original is {path.stat().st_size / 1e6:.1f} MB)")
    if not verify:
        return True
    print("  verifying every page against the original...", flush=True)
    original = pdfium.PdfDocument(str(path))
    fresh = PdfReader(str(path))  # unpruned, as on disk
    problems = []
    for p in plan:
        part_reader = PdfReader(str(out_dir / p["file"]))
        part = pdfium.PdfDocument(str(out_dir / p["file"]))
        if len(part_reader.pages) != p["pages"]:
            problems.append(f"{p['file']}: {len(part_reader.pages)} pages, expected {p['pages']}")
            continue
        if part_reader.page_labels != [labels[i] for i in range(p["start"] - 1, p["end"])]:
            problems.append(f"{p['file']}: page labels differ")
        for k in range(p["pages"]):
            src = p["start"] - 1 + k
            if content_bytes(part_reader.pages[k]) != content_bytes(fresh.pages[src]):
                problems.append(f"{p['file']}: page {labels[src]} content differs")
            elif pixels(part, k) != pixels(original, src):
                problems.append(f"{p['file']}: page {labels[src]} renders differently")
        part.close()
    original.close()
    for prob in problems:
        print(f"  PROBLEM {prob}")
    print(f"  checked all {sum(p['pages'] for p in plan)} pages in the {len(plan)} files: "
          f"{'OK' if not problems else f'{len(problems)} problems'}")
    return not problems


# --------------------------------------------------------------------------- rebuilding an index

def part_outline(reader):
    """The bookmarks of one chapter file as nested nodes: title, page (0-based), kids."""
    def walk(items):
        out = []
        for it in items:
            if isinstance(it, list):
                if out:
                    out[-1]["kids"].extend(walk(it))
                else:
                    out.extend(walk(it))
                continue
            try:
                page = reader.get_destination_page_number(it)
            except Exception:
                page = None
            out.append({"title": clean(it.title or ""), "page": page, "kids": []})
        return out
    try:
        return walk(reader.outline)
    except Exception:
        return []


def index_folder(out_dir: Path, max_pages, force):
    """Rebuild INDEX.md for a folder of chapter files from the bookmarks inside them.

    For use when the source PDF is gone. The sections listed per file are the
    headings one level below each file's top-level bookmark(s), which is what the
    split itself lists; a file whose top-level bookmark has no children in it is
    listed by that bookmark alone."""
    parts = sorted(f for f in out_dir.iterdir() if f.is_file() and OUTPUT_FILE_RE.search(f.name))
    if not parts:
        return False
    index = out_dir / "INDEX.md"
    if index.exists() and not force:
        print(f"  skipped: {out_dir.name}/INDEX.md already exists (use --force to redo it)")
        return True
    doc_title, total, last_label, overlaps, entries = None, 0, None, 0, []
    for f in parts:
        r = PdfReader(str(f))
        title = clean(str((r.metadata or {}).get("/Title") or ""))
        if doc_title is None and " - " in title:
            doc_title = title.split(" - ")[0]
        labels = r.page_labels
        total += len(r.pages)
        shared = bool(labels) and labels[0] == last_label  # a section started mid-page: that page is in both files
        overlaps += shared
        last_label = labels[-1] if labels else None
        stem = OUTPUT_FILE_RE.sub("", f.name)
        segs = stem.split(" - ")
        front = len(segs) == 2 and FRONT_RE.match(segs[1]) or segs[0] in ("Cover", "Cover and Contents", "Front Matter")
        tops = part_outline(r)
        sections = []  # what a fresh split lists: the headings one level below the file's chapter(s)
        if not front:
            for top in tops:
                kids = [k for k in top["kids"] if k["title"]]
                if len(tops) > 1:  # several chapters in one file: each chapter, then its sections
                    sections.append(top)
                sections.extend(kids)
        # the heading a cut part starts at, from its name: "05b - Project Window - Left Zone to Right Zone"
        starts_at = bare(segs[-1].split(" to ")[0]) if len(segs) >= 3 else None
        entries.append({"stem": stem, "sections": sections, "shared": shared, "starts_at": starts_at})
    for prev, cur in zip(entries, entries[1:]):
        # a section that starts on the shared page is bookmarked in both files; it belongs to the
        # file whose name starts with it, otherwise to the earlier one
        if cur["shared"] and prev["sections"] and cur["sections"] \
                and prev["sections"][-1]["title"] == cur["sections"][0]["title"]:
            if bare(cur["sections"][0]["title"]) == cur["starts_at"]:
                prev["sections"].pop()
            else:
                cur["sections"].pop(0)
    lines = []
    for e in entries:
        lines.append(f"- **{e['stem']}**")
        if e["sections"]:
            lines.append(f"  {'; '.join(s['title'] for s in e['sections'])}")
    doc_title = doc_title or out_dir.name
    head = [f"# {doc_title}, split into chapters", "",
            f"{total - overlaps} pages in {len(parts)} files. One file per chapter; chapters over {max_pages}",
            "pages are cut at section starts into parts. A chapter or section that starts mid-page",
            "shares that page with the file before it. Page numbers in the filenames and inside each",
            "file match the original. This index was rebuilt from the bookmarks inside the chapter",
            "files by tools/ADI-PDF-To-Chapters (--index); the source PDF was no longer present.", ""]
    index.write_text("\n".join(head + lines) + "\n", encoding="utf-8")
    print(f"  wrote {index.name}: {len(parts)} files, {total - overlaps} pages, from '{doc_title}'")
    return True


def main():
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Split every PDF in a folder into chapter-sized PDFs.")
    ap.add_argument("folder", nargs="?", type=Path, default=here,
                    help="folder holding the PDFs (default: the folder this script is in)")
    ap.add_argument("--max-pages", type=int, default=30, help="longest file allowed before a chapter is cut (30)")
    ap.add_argument("--force", action="store_true", help="redo PDFs whose output folder already exists")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, write nothing")
    ap.add_argument("--no-verify", action="store_true", help="skip the page-by-page check against the original")
    ap.add_argument("--index", action="store_true",
                    help="rebuild INDEX.md for FOLDER (a folder of chapter files) or for every such folder "
                         "inside it, from the bookmarks in the chapter files; for when the source PDF is gone")
    args = ap.parse_args()

    if args.index:
        folders = [args.folder] if any(OUTPUT_FILE_RE.search(f.name) for f in args.folder.iterdir() if f.is_file()) \
            else sorted(d for d in args.folder.iterdir() if d.is_dir())
        done = 0
        for d in folders:
            print(f"\n{d.name}/")
            done += bool(index_folder(d, args.max_pages, args.force))
        print(f"\ndone: {done} folder(s) indexed")
        return 0

    pdfs = sorted(args.folder.glob("*.pdf"))
    if not pdfs:
        print(f"no PDFs in {args.folder} -- drop some next to this script and run it again")
        return 0
    failed = []
    for path in pdfs:
        print(f"\n{path.name}")
        try:
            if not split_pdf(path, args.max_pages, args.force, args.dry_run, not args.no_verify):
                failed.append(path.name)
        except Exception as e:  # one bad PDF shouldn't stop the rest
            print(f"  FAILED: {type(e).__name__}: {e}")
            failed.append(path.name)
    print(f"\ndone: {len(pdfs) - len(failed)} of {len(pdfs)} PDFs OK" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
