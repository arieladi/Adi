# Onboarding prompt — `win`, for `adi-surge`

Paste the block below into a fresh Claude Code session on the Windows machine.
It is written to be self-contained: it assumes the agent has never seen this
project and cannot see this Mac.

Keep this file updated if the bootstrap facts change. It is a **prompt**, not a
protocol — the protocol is `collab/README.md` and the prompt tells you to read it.

---

```
You are the `win` agent on a new project called adi-surge. You work on Windows;
there is a second agent, `mac`, on a different machine and a different account.
You sync only through git. Read this whole message before running anything.

WHAT THE PROJECT IS

We are building AI-powered synths: describe a sound in words, the synth changes.
There are now two, deliberately:

  - adi-vst   a fork of Vital, ships a VST3. Already underway.
  - adi-surge a fork of Surge XT, ships a CLAP. THIS ONE. Brand new, bootstrapped
              today by `mac`, never built by anyone.

Same AI idea, two codebases, two plugin formats. Surge XT was chosen because it
is natively CLAP (JUCE 6.0.5 in adi-vst predates CLAP entirely, so CLAP was
dropped over there) and because it already ships the machine-facing surfaces
adi-vst had to hand-build: OSC with an address per parameter, Python bindings, a
headless CLI and a test suite. There is an intention to integrate with adi_daw
(our DAW, in the same monorepo) later, but NOTHING about that is designed yet and
you should not assume anything about it.

STEP 1 — GET THE MONOREPO

The project lives in the public monorepo github.com/arieladi/Adi, as the
directory adi-surge/, alongside adi_daw/ and adi-vst/.

If you already have the monorepo cloned on this machine, use that clone:
    git checkout main
    git pull
If you do not, clone it somewhere sensible under your own user profile:
    git clone https://github.com/arieladi/Adi.git

Confirm adi-surge/ exists and has ARCHITECTURE.md, docs/, collab/, tools/.
If it is not there, the bootstrap PR has not merged yet — say so and stop rather
than guessing; do not create the directory yourself.

STEP 2 — GET SURGE

adi-surge/surge/ is Surge XT itself. It is a SEPARATE git repository, gitignored
by the monorepo, so cloning the monorepo does NOT bring it. Fetch it with the
script, not by hand:

    bash adi-surge/tools/fetch_surge.sh

(Git Bash, or WSL, or translate it — it is a short script and the header
explains everything it does.) It clones surge-synthesizer/surge with the remote
named `upstream` and initialises all 22 submodules. About 1.5 GB when done.

Two traps it absorbs, which WILL bite you if you run git by hand instead:

  1. `git submodule update --init --recursive` fails with
         fatal: transport 'file' not allowed
     even though every URL in .gitmodules is https. That is git >= 2.38's
     CVE-2022-39253 mitigation. It needs `-c protocol.file.allow=always` scoped
     to that one command. Do NOT set that globally.
  2. .gitmodules declares 23 submodules but only 22 are real gitlinks.
     src/surge-rs/surge-rs is a stale upstream declaration with nothing behind
     it. That is not a broken checkout. Do not try to fix it.

Verify when it finishes:
    git -C adi-surge/surge submodule status --recursive | findstr /V "^ "
Any output means drift; empty means clean.

STEP 3 — READ, IN THIS ORDER

    adi-surge/collab/README.md      the protocol: roster, claims, branch rules
    adi-surge/collab/mac.md         what mac did, and notes addressed to you
    adi-surge/ARCHITECTURE.md       how it all works, with evidence for every claim
    adi-surge/docs/DECISIONS.md     ADR-0001..0006, settled; do not re-litigate

ARCHITECTURE.md is written FROM SOURCE, not from a build. Nobody has compiled
this on any platform. Sections marked PENDING are genuinely unknown, not
rhetorical. If you find something in it that is wrong, that is a useful result —
say so plainly.

STEP 4 — YOUR PART

In this order, and stop to report if any step surprises you:

  1. CHECK YOUR CMAKE VERSION FIRST. This is not a formality — see ADR-0007.
         call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
         cmake --version
     It MUST be >= 3.22. On CMake < 3.21, src/CMakeLists.txt:35-41 emits only a
     WARNING, sets SURGE_BUILD_CLAP FALSE, and the build then SUCCEEDS WITH NO
     CLAP. Exit code 0, no artifact. Surge's root file claims 3.15 is enough; it
     is not — libs/JUCE demands 3.22 and libs/clap-juce-extensions demands 3.21
     FATAL_ERROR. Record the version you have in your log.

     CMake and Ninja ship INSIDE the Visual Studio install and are not on PATH,
     so the version you get is whatever VS bundled. vcvars64.bat must be called
     in the same shell, which is why adi_daw drives its build from a .bat rather
     than from bash.

  2. Build the CLAP.
         cmake -S adi-surge\surge -B adi-surge\surge\build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
         cmake --build adi-surge\surge\build --config Release --target surge-xt_CLAP --parallel

     THEN ASSERT THE ARTIFACT, NOT THE EXIT CODE. Confirm the .clap actually
     exists under adi-surge\surge\build\surge_xt_products before you call it a
     success. The exit code is 0 either way.

     Good news on the hazard you might be expecting: the post-build copy that
     cost adi-vst four Windows builds (admin rights on C:\Program Files\Common
     Files\VST3) is pre-disabled here — SURGE_COPY_AFTER_BUILD is OFF by default
     (src/CMakeLists.txt:8) and staging goes into the build tree. Do NOT turn it
     on: the CLAP copy-after-build is not even implemented on Windows
     (ClapTargetHelpers.cmake:171-189 has Darwin and Linux branches only), so
     you would get an admin-rights VST3 copy attempt and no CLAP copy at all.

     Three other Windows-specific traps, all documented in ARCHITECTURE.md §2.6:
       - /WX is on with no escape hatch on MSVC (SURGE_SKIP_WERROR only covers
         clang/gcc), so code that is clean on mac can fail here on an unused
         variable. Suppression list is at CMakeLists.txt:213-221.
       - MSVC static runtime /MT is forced. Any prebuilt dependency built /MD
         gives LNK2038 — this WILL matter when the AI feature needs an HTTP
         client. Build new deps from source inside the CMake tree.
       - LTO is ON for Release. Use -DENABLE_LTO=OFF while iterating.

  3. Run the tests and RECORD THE COUNT.
         cd adi-surge\surge\build && ctest -j 4
     This is upstream's own suite — 147 catch2 TEST_CASEs / 409 SECTIONs,
     including a golden numeric harness at 1e-5 tolerance and an "All Patches
     Are Loadable" pass over 3561 .fxp files. It becomes our regression gate,
     the way adi-vst uses "15/15". The number goes in your log.

  4. Load the built CLAP in a host and confirm it makes sound. Reaper hosts CLAP
     natively. This is the thing adi-vst cannot do at all, so it is the first
     real proof the project premise holds.

  5. Only then, look at the open question below.

THE OPEN QUESTION — ADR-0005 IS RESERVED AND UNWRITTEN

How does the AI actually write into Surge? Three candidates, deliberately not
decided, and mac has explicitly asked you to push back rather than agree:

  (a) A C++ overlay inside the plugin, the way adi-vst is doing it. Real product
      UX, most work. Surge makes this much cheaper than Vital did: the GUI is
      plain JUCE (zero OpenGLContext references in src/), overlays are a registry
      (an enum value + a class + one case in createOverlay), and LuaEditors is
      existing precedent for text input inside a hosted plugin.
  (b) Drive Surge over its EXISTING OSC surface, possibly with zero C++ changes.
      /param/<oscName> sets any parameter by name, parameterFromOSCName is the
      reverse lookup, /doc emits name+type+min+max for every parameter, and it
      all reaches the audio thread through an existing lock-free oscRingBuf.
      UNKNOWN, and decisive: is the OSC server on by default, what ports, and
      what happens with several plugin instances in one DAW?
  (c) surgepy offline, for generating training data and proving the prompt->patch
      loop before any C++ exists.

If your build lands and you can open the OpenSoundControlSettings overlay, you
will know more about (b) than mac does. Say so.

HOW TO WORK HERE — NON-NEGOTIABLE

  - NEVER commit to main. Branch as win/<topic>, push, open a PR, merge it
    yourself when CI is green.
  - NEVER `git add -A` or `git add .`. The monorepo is PUBLIC and routinely holds
    other projects' untracked work in progress; a force-push does not fully undo
    a leak. Stage explicit paths: git add adi-surge/...
  - Touch ONLY adi-surge/**. Never edit, stage or revert anything under adi_daw/
    or adi-vst/, even to fix something obvious. Raise it instead.
  - Before anything destructive (reset --hard, clean, force-push, history
    rewrite): run `git status` and `git branch --show-current` as their OWN step
    and READ the output. Reset branches BY NAME. A reset --hard aimed at the
    wrong branch once destroyed unstaged adi_daw work that had never been staged
    and could not be recovered.
  - Claim paths in collab/README.md's claims table before you write, remove the
    row when your branch merges.
  - Log what you did in collab/win.md ONLY. Never write to collab/mac.md.
  - Decisions go in docs/DECISIONS.md as a new numbered ADR, append-only, and
    you reserve the number in the README table BEFORE writing the entry.
  - adi-surge has its OWN ADR namespace starting at 0001. Cite other projects'
    in full: "adi-vst ADR-0017", "adi_daw ADR-0052".
  - surge/ currently has NO `origin` remote, only `upstream`. That is fine while
    our tree is identical to upstream's, but it means any commit you make inside
    surge/ is unbacked-up and invisible to mac. Keep work in adi-surge/tools/
    until Adi creates the fork repo (ADR-0006).

Start with Step 1. Report after Step 4.2 with: did it build, what did ctest say,
and what did you find that ARCHITECTURE.md got wrong.
```
