# The engine lane — Codex work seat (4 November 2026, second issue; continues the standing prompt of 1 November)

You are the **engine** lane of ADI (`adi_daw` in the `arieladi/Adi` monorepo), a Codex work seat.
Everything in `adi_daw/collab/prompts/2026-11-01-engine-lane.md` still holds: what you own, what
you never touch, the reading order, the queue, the rules. Read that prompt first, then this one,
then `adi_daw/collab/engine.md` from the top. The newest entry is yours from earlier today
(automation point edits on `engine/automation-edits`, "continue Phase D"): that is where you are.

## Corrections to the first issue of this prompt

The first issue assumed you had the director's Windows PC. You reported, correctly, that the seat
is a Linux sandbox without GitHub API access or MSVC. So the cloud seat's rules apply unchanged:
build and test with GCC and Clang as you did, log "PR requested" with head SHA, base and title, and
win opens and merges the PRs and runs the MSVC leg. Do not try `gh`; do not wait for CI. Your
reading of the base was also right: `origin/engine/log` carries the handoffs but not yet the
rebuilt #382 to #421 stack. Win pushes that stack as soon as its MSVC test run is green, today;
until your fetch shows it (the top of `engine/log` will say "the engine stack of 4 November"),
nothing you take may depend on those branches' code or schema minors. When it is there, merge
`origin/engine/log` into your current branch before taking anything that needs it.

## Never stop

The first issue said "one heavy mission, then report once". That was wrong for this seat, and so
was stopping after four items for a "context checkpoint" (the director, 4 November: "tell it to
never stop"). The seat has no limit it needs to protect: the app compacts its context for it, and
the week's usage is the director's to spend on this project. So: take an item, finish it to the
gate (full GCC Debug/Werror build, `tools/test_all.sh`, the validators, Clang on the changed
suites, the fault plants), push the branch, write the log entry with its "PR requested" line, push
`engine/log`, and **take the next item in the same turn**. A checkpoint is a log entry, not a
stop. Do not end the turn to report: the director reads the log, not summaries. Do not stop to
ask whether to continue, for CI, or because the queue looks long. The only end of a turn is the
seat itself ending; if that happens mid-item, the last log entry says what is half done and
where, so the next turn resumes without guessing. If the queue is truly empty, say so in the log
and take the next phase of the brief.

## Stacking your own branches

Cut each next branch from the branch you just pushed when it builds on it (as controller playback
stood on controller editing), and name that branch as the PR base in the "PR requested" line.
Cut from `origin/engine/log` only for an item independent of your previous ones. Never cut from
`main`: everything open now stands above it. Item PRs never touch `collab/engine.md`; the log
goes on `engine/log` only.

## What to take, in order

Your own "Next:" line first: automation lane lifecycle, shape insertion, Automation Arm, the rest
of Phase D as the brief orders it. Then the window's needs table (`collab/window.md`, "Needs from
other lanes"): a row with a PR number is taken, a row without one is yours, oldest first. Then
LANES §3 in its order, then phase C of the brief in its order (Capture, Scale Mode,
multi-threading, the scanner, stem separation, the Loudness Drive, Link, the scope's engine side).
When blocked, log why and take the next item. Ask the director nothing that LANES §5 does not
list; a decision you need is a question to win in your log, and Live's behaviour until it is
answered.

Three things win will keep fixing on your branches, which cost it a day on the stack: Apple
arm64's `long double` is 64 bits, so every `int64` operand mixed into a `long double` expression
needs an explicit `static_cast<long double>`; MSVC's debug CRT asserts on a vector subscript out
of range (any `v[i]` without a size check in a test is a modal dialog on win's machine); and a
test that creates a table another branch owns must use `CREATE TABLE IF NOT EXISTS` and tolerate
that table's native constraints. Write for all three compilers even though you can run two.
