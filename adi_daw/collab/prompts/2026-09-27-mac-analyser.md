# 2026-09-27 — for the second session on the Mac: the Pd analyser, renumbered and given its own place

A second Claude session on the Mac built the Pd spectrum analyser (ADR-0116's
floating second view) and vendored libpd. It committed twice onto
`mac/ci-render`, the mission session's branch, locally only, and spent
ADR-0179, which was reserved for the CLAP host contract. The mission session
moved the two commits to `mac/pd-analyser-wip` and touched nothing else.

The director keeps the work (2026-09-27). This is its brief, as issued. The
mission session gets `2026-09-27-mac-round2.md`.

```text
You are the second Claude session on the Mac, working on the Pd spectrum analyser (ADR-0116). You commit as
mac, and that is right: two sessions on one machine are one agent. The director keeps your work. win, the lead
on Windows, allocates ADR numbers and keeps the two Mac sessions apart. Read collab/README.md ("Two sessions
on one machine" and the reservation table) and ADR-0177 before anything else.

1. YOUR ADR IS ADR-0183, NOT 0179.
   0179 was reserved for the mission session's CLAP host contract before you spent it (ADR-0051: the table
   decides). Your two commits exist only on your machine, so fix them before anything is pushed:
   - 49b2ac4 (the analyser's contract and its four rulings) and c502c5a (libpd vendored, running, proved end
     to end): rename ADR-0179 to ADR-0183 everywhere, in the DECISIONS.md heading and in every reference;
   - collab/README.md: the 0183 row is already reserved for you; mark it `used`, and leave 0179's row alone;
   - adi_daw/README.md: the ADR count must match DECISIONS.md;
   - python adi_daw/tools/validate_schema.py: check 8 must pass.

2. YOUR OWN PLACE TO WORK.
   You and the mission session share one checkout, and that is how your commits landed on mac/ci-render.
   Work in a worktree of your own from now on, for example:
       git worktree add ../Adi-wt/pd-analyser mac/pd-analyser-wip
   Before every commit, run `git branch --show-current`. Never commit onto mac/ci-render, or onto any branch
   that carries the mission session's open PR.

3. REBASE ONTO MAIN, AND CHECK AGAINST ADR-0177.
   Main moved while you worked. ADR-0177, decided on 2026-09-26, is the Pd parameter contract:
       [adi.param $0 <id> <min> <max> <default> <unit> <curve> <name> [<items>...]]
   - a fixed author id, which is what automation keys on;
   - changes only through device.loadState;
   - adi.param.pd is a plain abstraction, so vanilla Pd still opens the patch.
   Your analyser's contract and its four rulings came before it. Where they touch parameters, align with
   ADR-0177, or say in ADR-0183 exactly what you amend and why. Do not leave two contracts.

4. LIBPD FOLLOWS ADR-0024.
   third_party/ is git-ignored, and every dependency comes through tools/fetch_external.sh, pinned by tag and
   the commit that tag pointed at, and verified on fetch. Record it in docs/EXTERNAL-CODE.md.
   - Licence: libpd and Pd are BSD-3 (ADR-0035). Check the headers at the pinned commit.
   - Build flags: libpd must be built multi-instance (MULTI=true, PDINSTANCE), as ADR-0035 requires.
   - What never goes in: libpd's source copied into tracked files.

5. YOUR LOG IS collab/mac-analyser.md, a new file that is yours alone. collab/mac.md stays the mission
   session's, so your two PRs never conflict on it. Add a claims row in collab/README.md for the paths you
   touch, and remove it on merge.

6. Then open your PR from mac/pd-analyser-wip. Merge it yourself when CI is green BY HEAD SHA: a run counts
   only if its headSha is the PR's head.
```
