# adi-surge — Decision log

Append-only. Newest at the bottom. Superseding an earlier decision means writing
a **new** entry that says so — never edit an old one.

`ARCHITECTURE.md` explains how the system works. This file records *what was
decided and why*, so neither agent re-litigates a settled question or silently
reverses one.

Format: **ADR-NNNN — title**, then Context / Decision / Consequences.

**This project has its own ADR namespace.** It starts at 0001 and does not
continue `adi-vst`'s or `adi_daw`'s. Citations to theirs are always written in
full — "adi-vst ADR-0017", "adi_daw ADR-0052" — because a bare number here means
one of ours.

---

## ADR-0001 — Why a second AI synth, and why Surge XT

**Date:** 2026-09-20 · **Agent:** mac · **Directed by:** Adi

**Context.** `adi-vst` is a fork of Vital with an AI "describe a sound, get a
patch" feature being built into it. It works, the toolchain is verified, and it
ships a VST3. The obvious question is why build the same feature a second time
rather than finishing the first.

Three reasons, in order of how much they actually matter:

1. **Format coverage.** `adi-vst` ships VST3 only, and that is now a property of
   the repo rather than an accident — adi-vst ADR-0017 dropped CLAP there
   because JUCE 6.0.5 predates CLAP entirely, there is no `buildCLAP` flag in
   either `.jucer`, and adding one would mean a second build system against
   adi-vst ADR-0002. Surge XT is natively CLAP: `surge-xt_CLAP` is a first-class
   CMake target in upstream's own CI. The format we could not reach there is
   free here.

2. **Surge ships the machine-facing surface Vital does not.** `adi-vst` had to
   hand-build a headless VST3 validator (its ADR-0009) and a Python schema
   extractor that parses C++ parameter tables, because Vital exposes nothing.
   Surge ships OSC in and out with an address per parameter, `surgepy` Python
   bindings, a headless CLI, and a `ctest` suite. See ADR-0005.

3. **Two synths is a better product than one**, and the second one is cheap
   precisely because the AI design — sparse patch, merge not reset, undo before
   apply — was already worked out next door and is format-independent.

**Decision.** `adi-surge/` is a new project directory in the `arieladi/Adi`
monorepo, a fork of `surge-synthesizer/surge`, targeting CLAP, reusing
`adi-vst`'s AI design where it transfers and departing from it where Surge's
architecture is genuinely different.

**Consequences.** Two forks to keep current instead of one. That cost is real
and is not symmetric: Vital is effectively unmaintained upstream, so `adi-vst`'s
fork delta only grows from our side, whereas Surge is actively developed and we
will want to pull upstream regularly. Keeping our delta small is therefore a
harder constraint here than there, and it is written into `collab/README.md` as
a ground rule rather than left as good intentions.

This decision does **not** couple to `adi_daw`. Integration with the DAW is
intended but undesigned; see ADR-0003's last paragraph.

---

## ADR-0002 — `surge/` is a fork tracked against upstream, not a vendored copy

**Date:** 2026-09-20 · **Agent:** mac

**Context.** Surge XT's working tree is ~1.5 GB with all 22 submodules checked
out; the repository alone is 1.1 GB. It has to live somewhere, and the two
options are vendoring it into the monorepo or keeping it as its own repository
that the monorepo ignores.

`adi-vst` already answered this question for Vital (its ADR-0001) and the
reasoning transfers without modification: vendoring loses `git diff
upstream/main`, which is the only command that says what we have actually
forked.

**Decision.** `adi-surge/surge/` is its own git repository with a single remote
named **`upstream`** pointing at `https://github.com/surge-synthesizer/surge.git`.
The parent `Adi` repo gitignores `adi-surge/surge/`. Cloned at
`58914e59c` on `main` (Surge XT 1.4.0).

The clone is deliberately **not shallow**. A shallow clone cannot answer
`git diff upstream/main`, which is the entire point of the arrangement.

**Consequences.** The whole fork delta is one command:

```bash
git -C adi-surge/surge diff upstream/main --stat
```

Cost: the fork is not visible to anyone who only has the monorepo, so each agent
clones it themselves. Unlike adi-vst ADR-0011, **this does not block anybody**,
because the upstream is public and cloneable by both agents today. See ADR-0006.

`tools/fetch_surge.sh` performs the clone and the submodule init. Nothing about
this arrangement should be discovered by hand.

---

## ADR-0003 — CLAP is the primary target; VST3 and standalone are secondary

**Date:** 2026-09-20 · **Agent:** mac

**Context.** Surge XT builds CLAP, VST3, AU, LV2 and standalone from one CMake
tree. Building all of them on every change is slower than building one, and
"which one do we actually validate against" needs an answer before anyone writes
a build script.

The project exists in part to cover the format `adi-vst` could not
(ADR-0001), and CLAP is the format `adi_daw` has mandated hosting for
(adi_daw ADR-0052) specifically because of `CLAP_EVENT_PARAM_MOD` —
non-destructive parameter modulation, where a host-side modulator rides on top
of a parameter without overwriting the value the user dialled in.

**Decision.** `surge-xt_CLAP` is the primary build and validation target for
this project. VST3, AU, LV2 and standalone continue to build because they are
upstream's targets and we are not removing them, but they are not what a branch
is judged on. `collab/README.md`'s "what done means" names the CLAP target and
`ctest`, nothing else.

**Consequences.** This is the mirror image of adi-vst ADR-0007 + ADR-0017, which
settled on VST3-only over there. The two projects together therefore cover both
formats, which was the point.

**Scope.** This decides what *this plugin exports*. It says nothing about what
`adi_daw` *hosts*. adi_daw ADR-0052 mandates CLAP hosting and its own
ADR-0041 reasons that "hosting is not identity"; exporting a format and hosting
one are unrelated capabilities. **There is no decision yet about how `adi-surge`
and `adi_daw` integrate**, and nothing in this repo should assume one. When that
is designed it is a new ADR, here and probably there too.

---

## ADR-0004 — Submodules are Surge's own gitlinks, and the `protocol.file.allow` trap

**Date:** 2026-09-20 · **Agent:** mac

**Context.** Surge vendors its dependencies as git submodules — JUCE, the whole
`sst-*` family, LuaJIT, zstd, simde, PEGTL, fmt, pybind11,
`clap-juce-extensions` and more. `adi_daw` solved the equivalent problem with
`tools/fetch_external.sh`, which pins every external repo **by tag and by the
commit that tag pointed at**, and fails loudly if upstream re-points a tag
(adi_daw ADR-0024).

That machinery is not needed here, and it is worth writing down why rather than
copying it out of habit: `adi_daw`'s externals are independent clones, so "what
did we build against" is only answerable if we record it ourselves. Surge's are
**gitlinks in Surge's own tree**. Git checks out exactly the commit the
superproject names and `git submodule status` prefixes any drift with `+`. The
pin, the record and the verification are all already there.

**Two things were discovered the hard way and are absorbed by
`tools/fetch_surge.sh`:**

1. **`git submodule update --init --recursive` fails on a fresh clone** with
   `fatal: transport 'file' not allowed`, even though every URL in `.gitmodules`
   is `https`. Git ≥ 2.38 refuses the local `file://` transport by default
   (CVE-2022-39253) and git's submodule clone path tries the superproject's
   object store first. Verified on git 2.50.1 (Apple Git-155). The fix is
   `-c protocol.file.allow=always` **on that one command**.

2. **`.gitmodules` declares 23 submodules; only 22 are gitlinks.**
   `src/surge-rs/surge-rs` is a stale declaration with no gitlink behind it, so
   git never fetches it and `git submodule status` never mentions it. That is
   upstream's state, not a broken checkout. Nothing in the CMake build
   references it.

**Decision.** Do not pin or vendor Surge's submodules ourselves. Fetch them with
`bash adi-surge/tools/fetch_surge.sh`, which applies the `protocol.file.allow`
flag scoped to the single command that needs it.

**Do not set `protocol.file.allow=always` globally.** It is a global weakening
of a real mitigation for a problem that is local to this clone.

**Consequences.** Upgrading Surge is `git -C adi-surge/surge fetch upstream` and
a merge; the submodule pins come along with it, because they are part of the
commit. Verified: all 27 checked-out entries (22 top-level + 5 nested) report
clean, with no `+` drift markers.

---

## ADR-0005 — reserved: the AI write path

**Date:** 2026-09-20 · **Agent:** mac · **Status:** RESERVED, not yet written

Number claimed per `collab/README.md`. Subject: whether the AI drives Surge over
its existing OSC surface, through `surgepy` offline, or through a C++ overlay in
the plugin the way `adi-vst` is doing — and whether the cheap options are a
stepping stone to the in-plugin one or a dead end.

---

## ADR-0006 — The fork has no `origin` yet, and that does not block anyone

**Date:** 2026-09-20 · **Agent:** mac

**Context.** `adi-surge/surge` currently has exactly one remote, `upstream`,
pointing at `surge-synthesizer/surge`. There is nowhere to push our own commits.

adi-vst hit precisely this and it cost them: its ADR-0011 left the fork with no
`origin`, which meant the `mac` agent could not clone the C++ at all and was
limited to tooling and review until adi-vst ADR-0016 resolved it by creating
`arieladi/adi-vst-synth`. That was a real blockage, caused by the Vital fork
already carrying local modifications that existed nowhere public.

**The situation here is different in the way that matters.** Our clone is
currently byte-identical to `upstream/main` — we have not modified Surge at all
yet. So either agent can obtain the identical tree today, from a public URL,
with `tools/fetch_surge.sh`. Nobody is blocked.

**Decision.** Defer creating a fork origin until there is a fork delta to push.
Until then `tools/fetch_surge.sh` clones from `upstream` and both agents work
from identical trees.

**The moment the first commit lands in `surge/` that is not upstream's, this
becomes blocking and must be resolved in the same session** — do not repeat
adi-vst ADR-0011 by discovering it later. The resolution, when wanted, mirrors
adi-vst ADR-0016: a public GitHub repository under `arieladi`, added as
`origin`, GPLv3. **Creating it is Adi's call**, not an agent's, because it is his
GitHub account.

**Consequences.** Any local commit in `surge/` is unbacked up and invisible to
the other agent until an `origin` exists. Treat `surge/` as read-only until then,
and put work that does not strictly need to live inside Surge's tree — tooling,
schema extraction, the OSC harness — under `adi-surge/tools/`, which is in the
monorepo and therefore already shared.

---

## ADR-0007 — CMake ≥ 3.22 is a hard requirement, and a build must assert the artifact

**Date:** 2026-09-20 · **Agent:** mac

**Context.** Surge's root `CMakeLists.txt:2` says
`cmake_minimum_required(VERSION 3.15)`. That number is wrong in a way that
specifically damages this project.

`src/CMakeLists.txt:35-41` reads:

```cmake
if(${CMAKE_VERSION} VERSION_LESS 3.21)
  message(WARNING "CMake version is lower than 3.21. Skipping CLAP builds. ...")
  set(SURGE_BUILD_CLAP FALSE)
endif()
```

It is a **`WARNING`, not a `FATAL_ERROR`**. On CMake 3.15–3.20 the configure
succeeds, the build succeeds, the exit code is 0, and **there is no CLAP**. For
a project whose primary and only mandated target is the CLAP (ADR-0003), that is
a silent total failure disguised as a green build. It would be found at plugin
scan time, in a DAW, long after the build that caused it.

The true floor is higher than 3.21 anyway: `libs/JUCE/CMakeLists.txt:33` requires
**3.22**, and `libs/clap-juce-extensions/CMakeLists.txt:13` requires 3.21 as a
`FATAL_ERROR`.

**Decision.** Three things, all of them cheap:

1. **CMake ≥ 3.22 is a hard requirement for `adi-surge`**, stated in
   `ARCHITECTURE.md` §2.1 and in the Windows onboarding prompt. The 3.15 in
   Surge's root file is not to be trusted or quoted.
2. **Both agents record their CMake version in their log before the first
   build.** On Windows this is not a formality: CMake ships *inside* the Visual
   Studio install rather than on `PATH`, so the version is whatever VS bundled
   and has to be looked up rather than assumed.
3. **A build is not "successful" until `surge-xt_CLAP` exists on disk.** Any CI
   or build script we write asserts the artifact, never the exit code.

**Consequences.** Point 3 is the one that generalises, and it is the same
principle `adi_daw`'s CI header states — a green tick should prove a specific
claim rather than that something compiled. Here the specific claim is "a CLAP
came out", and it is precisely the claim the exit code does not make.

This is also a reminder that `cmake_minimum_required` in a vendored tree is a
floor for *that file*, not for the build. Check what the submodules demand.
