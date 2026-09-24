# SPDX-License-Identifier: GPL-3.0-or-later
"""Export the Settings Reference's setting rows into adi_daw/docs/SETTINGS-CATALOGUE.md.

Runs the real generator (settings_content.py) against a recording stand-in for the
docx builder, so the catalogue is exactly the document's rows. Only our own
columns travel: the setting, the ADI answer and its status, with the page. The
manual-quotation paragraphs and the "Live source" column stay in the Word file.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
B = os.path.join(ROOT, 'reference', 'DOCS', 'WORD', '_build')   # git-ignored; present on win's machine
OUT = os.path.join(ROOT, 'docs', 'SETTINGS-CATALOGUE.md')
sys.path.insert(0, B)
import settings_content as C  # noqa: E402


class Rec:
    def __init__(self):
        self.part = ''
        self.page = ''
        self.rows = []   # (part, page, setting, adi, status)

    def heading(self, text, level=1, bookmark=None):
        if level == 1:
            self.part = text
            self.page = ''
        elif level == 2:
            self.page = text

    def table(self, header, rows, **kw):
        h = [x.lower() for x in (header or [])]
        if not h:
            return
        if 'adi proposal' in h:
            si, ai, st = 0, h.index('adi proposal'), h.index('status')
        elif len(h) == 3 and h[2] == 'status':
            si, ai, st = 0, 1, 2
        else:
            return
        for r in rows:
            self.rows.append((self.part, self.page, r[si], r[ai], r[st]))

    def __getattr__(self, name):
        return lambda *a, **k: None


D = Rec()
IMG = {k: '' for k in C.SETTINGS_FIGS}
C.part1_window(D, IMG)
C.part2_ableton(D, IMG)
C.part3_others(D, IMG)
C.part4_adi_only(D, IMG)


def clean(t):
    t = re.sub(r"\{(DECIDED|DIRECTION|BACKLOG|WISH|REJECTED|OPEN|NOTE):?([^}]*)\}",
               lambda m: m.group(1) + ((' ' + m.group(2)) if m.group(2) else ''), t)
    return t.replace('|', '/').replace('\n', ' ').strip()


lines = [
    "# Settings catalogue",
    "",
    "Every setting the Settings Reference (v0.4) describes, generated from its",
    "source so the two cannot drift, for the settings registry (`src/adi/settings/`,",
    "ADR-0152) to fill from. Our own columns only: the setting, ADI's answer and its",
    "status. The Live-manual column and the screenshots stay in the Word document",
    "(`reference/DOCS/WORD/`, git-ignored). Status words: DECIDED (with the ADR),",
    "DIRECTION (proposed), BACKLOG, WISH, REJECTED, NOTE.",
    "",
    "Regenerate with `python tools/export_settings_catalogue.py` when the Word",
    "document changes (it needs the git-ignored `reference/` tree); never edit",
    "this file by hand.",
    "",
]
current = None
count = 0
for part, page, setting, adi, status in D.rows:
    key = (part, page)
    if key != current:
        current = key
        lines += ["", f"## {clean(part)}" + (f" / {clean(page)}" if page else ""), "",
                  "| Setting | ADI | Status |", "|---|---|---|"]
    lines.append(f"| {clean(setting)} | {clean(adi)} | {clean(status)} |")
    count += 1
lines += ["", f"{count} settings.", ""]
open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
print('wrote', OUT, count, 'rows')
