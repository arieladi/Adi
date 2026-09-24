# 2026-09-24e — cloud's final mission on its current budget

linux (Codex) is out of tokens until 2026-10-01 and has no mission. mac returns
Saturday 2026-09-26. The director says cloud's budget covers one more mission,
so this one is built to land in two independently mergeable PRs: if the budget
ends after the first, nothing is left half-done.

---

## Claude cloud session

```text
You are agent cloud on arieladi/Adi, project adi_daw. One mission, ADR-0159, branch cloud/curves.
This is your final mission on the current budget: work in two PRs, each merged when green, and never
leave an open PR or a claim without a log entry saying why. You're in a Linux container: build and
test the Linux legs, not MSVC or JUCE. Don't stop to ask; decide, and write it down.

ANSWERS TO YOUR LAST REPORT
- Agent tier default: Propose. The catalogue's "Default Observe" was win's transcription error in the
  Settings Reference, not a ruling; the row cited AI-AGENT itself. Fixed on main by win/held-notes,
  and docs/SETTINGS.md now says it is settled.
- "wish" as a fifth absence kind: accepted. Theme following the OS, and App-scope new-track defaults:
  accepted. List settings stay comma-separated text until the Settings window needs a list editor.
- The clip worker's decoder pins, never released: win takes it (linux's code, and linux is away).
- #111 and #112 reviewed by win: MSVC /WX clean, 4330 checks with #113, nothing to fix.

BEFORE YOU START
Pull main. Read collab/README.md (claims: yours is the cloud/curves row; "Windows, and MSVC with
warnings as errors"), the top of collab/win.md, SPEC §6.3 (event streams; §6.3.2 AEXP, §6.3.3 AAUT),
schema.sql's automation_lanes and automation_data, ADR-0042 (sub-block accuracy), ADR-0054 (expression
resolution), ADR-0155 (how MIDI clips compile off the callback) and ADR-0158 (win's, just merged).

YOUR FILES
New: src/adi/engine/curves.{hpp,cpp}, src/adi/engine/automation.{hpp,cpp}, tests/test_curves.cpp,
tests/test_automation.cpp. Lent: src/adi/store_rows.* for the automation rows only, and SPEC §6.3.2
and §6.3.3 text. CMakeLists.txt is shared: your own targets only.
NOT YOURS: the rest of src/adi/engine/** and docs/format/** (win); src/adi/settings/** stays yours;
src/juce/** (mac).

PR 1 -- THE CURVE FORMULAS (ADR-0159 part one)
AEXP and AAUT points carry `curve` (0 hold, 1 linear, 2 exp, 3 log, 4 s-curve, 5 bezier) and `tension`
(-1..+1), and the SPEC names the shapes without defining them. Converters, the engine and the UI must
all draw the same line, so the formulas become normative in SPEC §6.3.2 (and §6.3.3 points to it).
- One pure evaluator: noexcept, no allocation, callable on the audio thread. Normalised shape
  u = shape(x, tension) for x in [0,1], then v0 + (v1 - v0) * u, with the endpoints exact.
- Required, each tested over a grid of x and tension:
  a. u(0) = 0 and u(1) = 1 exactly, so a segment starts at v0 and ends at v1 bit-for-bit.
  b. Monotonic for every shape and every tension in [-1, 1].
  c. Tension 0 is exactly linear for shapes 1 to 5: an importer that knows no tension gets a line.
  d. log is exp reflected through the segment's centre: log(x, t) = 1 - exp(1 - x, t).
  e. s-curve is symmetric: s(1 - x) = 1 - s(x).
  f. bezier is one quadratic control point set by tension; state where it sits.
  g. Hold is v0 on [x0, x1) and v1 at x1.
  h. Finite for every finite input; the evaluator clamps tension to [-1, 1].
- You choose the exp/log family and the sign convention of tension (which way positive bends);
  write both into the SPEC with the reason, and a small table of golden values so that an
  accidental formula change fails a test, not a listening session.
- The tempo map's own `curve` (0 jump, 1 linear, 2 bezier) is a different enum and out of scope.
- Plants, each failing first: an endpoint off by one ulp; tension 0 not linear for one shape;
  exp/log not reflections; a non-monotonic shape at tension 1; a golden value changed.
Merge PR 1 when all 19 CI checks pass on its head SHA. Then PR 2.

PR 2 -- AUTOMATION READ INTO THE ENGINE, NOT YET PLAYED (ADR-0159 part two)
Nothing in the engine reads automation_lanes or AAUT today. Build the part that turns the rows into
something the audio thread can evaluate; emitting parameter events from it is the next mission (win's
or linux's), because user override, touch and latch need the director's rulings first.
- rows::Model gains the automation lanes and their data (store_rows is lent for this).
- An immutable AutomationProgram compiled OFF the audio thread from the model, the tempo map and the
  session rate, like MidiClips (ADR-0155): per arrangement lane, points in session samples (time_base 0
  through the tempo map, 1 from nanoseconds), values in the lane's value_domain.
- Validation refuses a whole lane, never part of one, with a named problem: times out of order, a
  non-finite value, a value outside min..max where set, curve above 5, non-zero reserved bytes.
  Two points at one time are a step, allowed.
- valueAt(lane, sample) in O(log n); a fill of `count` values from a first sample at a stride, into a
  caller's buffer: noexcept, no allocation. Before the first point, the first value; after the last,
  the last value. Say so in the ADR.
- Clip envelopes (automation_data.clip_id set) and 'project'/'routing' owners are reported as not
  played yet. Tempo ramps are reported as ADR-0155 reports them.
- Tests: ticks and nanoseconds placement at 44.1 to 768 kHz, a step, every curve shape through the
  PR 1 evaluator, each refusal, TSan and ASan clean. Plants, each failing first: a lane half-accepted
  after a bad point; ticks placed without the tempo map; an allocation in fill (instrument it); the
  value after the last point wrong.

MSVC /WX RULES (win builds MSVC /WX after each merge; no CI leg does)
No bare std::getenv; std::setvbuf, never setbuf; no fopen/strcpy/sprintf family; no
std::filesystem::u8path; watch size_t-to-int narrowing and signed/unsigned compares. Plants must still
compile under /WX: no constant conditions (`false &&`), no unreachable code, no unused parameters;
weaken a comparison instead. Check counts must not depend on the platform.

DONE MEANS
Per PR: tools/test_all.sh green with the sanitizers, validators clean, README counts updated, plants
recorded, log in collab/cloud.md, CI green by head SHA, merged by you. At the end: ADR-0159 appended
to DECISIONS.md after main's newest entry (a blank line before every `---`: validate_schema now
checks it), 0159 marked used, your claims row removed. If the budget runs short after PR 1, stop
there cleanly: narrow the claims row to nothing, and say in collab/cloud.md what PR 2 still needs.
Report the PR numbers, the checks count, the formulas you chose, the plants, and anything you
couldn't do.
```
