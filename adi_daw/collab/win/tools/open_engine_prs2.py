"""Open a PR for every "PR requested" entry in origin/engine/log's engine.md whose branch has no open
PR. Body = the entry's section, verbatim. Base = the requested base, unless the entry names a
dependency branch that is an ancestor of the head: then the topmost such branch, so the PR's diff
is its own. argv[1] = folder for the body files. Prints one line per branch."""
import re, subprocess, sys, pathlib
R = "arieladi/Adi"
out = pathlib.Path(sys.argv[1])
def git(*a):
    return subprocess.run(["git", *a], capture_output=True, text=True, encoding="utf-8").stdout.strip()
def is_ancestor(a, b):
    return subprocess.run(["git", "merge-base", "--is-ancestor", a, b]).returncode == 0
log = git("show", "origin/engine/log:adi_daw/collab/engine.md")
sections = re.split(r"(?m)^(?=## )", log)
seen = set()
for section in sections:
    m = re.search(r"\*\*PR requested[^\n]*?head `(engine/[^`]+)` at \*\*`([0-9a-f]+)`\*\*", section)
    if not m:
        continue
    branch, head = m.group(1), m.group(2)
    if branch in seen or branch == "engine/log":
        continue
    seen.add(branch)  # newest entry per branch wins (sections run newest first)
    actual = git("rev-parse", "origin/" + branch)
    if not actual:
        print(branch, ": not on origin"); continue
    if actual != head:
        print(branch, f": origin at {actual[:8]}, entry says {head[:8]}; opening at origin's head")
    existing = subprocess.run(["gh", "pr", "list", "--repo", R, "--head", branch, "--state", "all", "--json", "number",
                               "--jq", ".[0].number // empty"], capture_output=True, text=True).stdout.strip()
    if existing:
        continue
    tm = re.search(r"Title: \*\*(.+?)\*\*", section)
    title = tm.group(1) if tm else section.splitlines()[0].lstrip("# ").strip()
    bm = re.search(r"base `(engine/[^`]+|main)`", section)
    base = bm.group(1) if bm else "engine/log"
    deps = [d for d in set(re.findall(r"`(engine/[a-z0-9-]+)`", section)) if d not in (branch, "engine/log")]
    anc = [d for d in deps if git("rev-parse", "--verify", "-q", "origin/" + d) and is_ancestor("origin/" + d, "origin/" + branch)]
    top = [d for d in anc if all(is_ancestor("origin/" + o, "origin/" + d) for o in anc if o != d)]
    if top:
        base = top[0]
    body = (f"Engine lane PR requested in `collab/engine.md` (head `{actual}`); opened by win as the lane's GitHub hand. "
            f"The entry, verbatim:\n\n{section.strip()}\n\n---\nStacked on `{base}`; win retargets to main when that lands.\n\n"
            "🤖 Generated with [Claude Code](https://claude.com/claude-code)\n")
    bf = out / (branch.replace("/", "_") + ".md")
    bf.write_text(body, encoding="utf-8")
    r = subprocess.run(["gh", "pr", "create", "--repo", R, "--head", branch, "--base", base, "--title", title, "--body-file", str(bf)],
                       capture_output=True, text=True)
    print(branch, "->", base, ":", (r.stdout + r.stderr).strip().splitlines()[-1])
