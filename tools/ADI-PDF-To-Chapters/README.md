# ADI-PDF-To-Chapters

Splits long PDF manuals into chapter-sized PDFs, one topic per file and at most 30
pages, so a single subject can be read, or handed to a model, on its own.

```
run.cmd
  ReaperUserGuide.pdf   462 pages
  ->  ReaperUserGuide/
        00 - Cover and Contents [p1-14].pdf
        01 - Setting Up and Getting Started [p15-30].pdf
        ...
        07a - Managing and Editing Media Items - Using an External Editor to Item Mute and Solo Actions [p123-140].pdf
        07b - Managing and Editing Media Items - Displaying the Item Ruler to The Pop and Click Remover [p141-158].pdf
        ...
        INDEX.md          which sections are in which file
```

## Setup

```bash
install.cmd
```

Creates `.venv` next to the script and installs `pypdf`, `pypdfium2` and
`pdfplumber` (tested with 6.19, 5.13 and 0.11.10 on Python 3.14).

## Use

Drop PDFs into this folder and double-click `run.cmd`. For every `Name.pdf` it
writes a folder `Name/` beside it. The original PDFs are never changed, and a PDF
whose folder already exists is skipped, so you can keep adding manuals and running
it again.

From a terminal, options pass straight through:

```bash
run.cmd --dry-run
```

| Option | Effect |
|---|---|
| `--dry-run` | print the plan (every file it would write), write nothing |
| `--max-pages N` | longest file allowed before a chapter is cut (default 30) |
| `--force` | redo PDFs whose folder already exists (replaces only the files this tool wrote) |
| `--no-verify` | skip the page-by-page check (faster on very large manuals) |
| `--index` | rebuild `INDEX.md` for `FOLDER` (a folder of chapter files), or for every such folder inside it, from the bookmarks kept in the chapter files. For when the source PDF has been deleted. Existing indexes are kept unless `--force` |
| `FOLDER` | work on another folder instead of this one |

## What you get

- **One file per chapter.** A chapter longer than 30 pages is cut where a section
  starts, into parts of about equal length: `28a`, `28b`, ... It goes a level deeper
  (sub-sections) only when that's the only way to fit, or when it saves a part.
- **Short chapters stay on their own** rather than being mixed with unrelated ones.
  The exception is a leading one-page chapter (a welcome page, a disclaimer), which
  is folded into the chapter after it, e.g. Surge XT's
  `01-03 - Getting Started, Building From Source and Installing Surge XT`.
- **Nothing is cut off.** When a chapter or section starts mid-page, that page goes
  into both files, so each file has everything above its first heading.
- **Page numbers are the manual's own**, in the filename (`[p160-181]`, `[pi-xix]`)
  and inside each file (the original page labels are kept).
- **Every heading is a bookmark** in the file it's in, at its position on the page.
  A chapter carried over from the previous file shows as `(cont.)`.
- `INDEX.md` lists the sections in every file, to find which file covers what.
- Chapter numbers are the manual's own when it numbers its chapters, otherwise
  01, 02, ... in reading order. The cover and contents go in `00` (or in an
  unnumbered file if the manual has its own chapter 0).

## How it finds the chapters

It uses the first of these that works:

1. **The PDF's bookmarks.** Cover and contents entries at the front are set aside
   as front matter, and a single bookmark wrapping everything is unwrapped. If the
   bookmarks turn out to be topics of a page or two rather than chapters (web-help
   PDFs such as FabFilter's), the whole manual is treated as one chapter and cut
   into balanced parts at topic starts.
2. **A clickable table of contents.** Each contents line is matched to its link,
   which gives the exact target. The depth comes from the line's numbering (`2.24`
   is level 2) or, for unnumbered contents, from its indent, measured separately on
   odd and even pages because books mirror their margins. Reading stops at the
   first big jump back in target page, which is where a second linked list (a
   "what's new" list, say) starts.
3. **Heading type sizes.** Every font size clearly above the body text is a
   heading level, biggest first. A title that appears only once is ignored, and so
   are contents pages. This covers PDFs printed from web pages and word processors.
4. If none of those finds anything (a scanned PDF, say), it falls back to plain,
   equal page ranges.

Whether a heading starts at the top of its page is decided from the page text:
anything between the running header and the heading means the heading is
mid-page. The bookmark's height alone isn't enough. In one manual, a heading 19pt
below the top still had the end of the previous section above it.

## Checks

After writing, every page of every file is compared with the original: the same
content stream, and the same pixels when rendered. The page labels are checked
too. Problems are listed and the exit code is non-zero.

Some PDF writers (WeasyPrint, LibreOffice) give every page one resource list naming
every image in the book, so a plain split would copy all the images into every
file, turning 12-page files into 50 MB. Each page is pruned to what it actually
draws before copying. The render check is what proves nothing it needs was
dropped.

## Limits

- Titles are used exactly as the PDF has them, typos included.
- Heading-size detection can't see headings set in body-size type (bold only). It
  also folds sections set in smaller type, such as appendices, into the chapter
  before them.
- A printed table of contents without links is not parsed; heading sizes are used
  instead.

## Git

Manuals are usually someone else's copyrighted work, and this repository is
public. The `.gitignore` here tracks only the tool's own files, so the PDFs you
drop in and the folders written from them stay local.

## Licence

MIT (see `LICENSE`). The dependencies are installed from PyPI, not bundled:
pypdf (BSD-3-Clause), pypdfium2 (Apache-2.0 / BSD-3-Clause, with PDFium) and
pdfplumber (MIT).
