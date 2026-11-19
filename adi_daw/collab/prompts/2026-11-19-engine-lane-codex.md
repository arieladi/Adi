# The engine lane — Codex work seat, cloud (19 November 2026): until your limit

As the director handed it over. Win's prompt; the seat's log is the record of what came of it.

```
Engine lane of ADI (adi_daw in arieladi/Adi), Codex work seat, cloud session.

HOW YOU WORK: UNTIL YOUR LIMIT
This is not a one-answer task. Work through the queue below, item after item, until your usage limit ends the session.
- After each piece: gate it, commit, push the branch, write the log entry and "PR requested", then start the next piece
  in the same turn. Do not end your turn to report: the log is the report.
- A question only the director can answer does not stop you. Write it at the top of your log under "FOR THE DIRECTOR"
  (the question, your recommendation, what it blocks), park that item and take the next.
- An item waiting on another seat, on CI or on win: note it and move on; come back when it clears.
- Before each new item: git fetch, and read the top entry of adi_daw/collab/win.md (win adds work and answers there)
  and the Engine table of adi_daw/docs/DEFERRED.md.
- Stop only if your execution environment is broken beyond retry (then say so in ONE line; never ask the director to
  install or connect anything), or if every remaining item is parked on the director.
- Told "continue": fetch, read your log's last entry, carry on from there.
Use everything you have: build, run, measure, stress, install what helps in your sandbox, write harnesses and test
plug-ins.

STEP 0
Run `pwd` and `git --version`. If an earlier workspace of yours holds uncommitted work, commit it to
engine/rescue-1118 and push it first.

READ ONCE, IN ORDER (all on main)
adi_daw/collab/prompts/2026-11-18-engine-fx-channels.md; ADR-0245 in adi_daw/docs/DECISIONS.md;
adi_daw/docs/RESEARCH-INTAKE.md sections 6.2, 6.4, 6.7; adi_daw/collab/prompts/2026-11-18-engine-lane-codex.md;
adi_daw/collab/win.md top entry; adi_daw/docs/DEFERRED.md Engine table.

YOUR FILES, AND THE OTHER ENGINE SEAT'S
A second engine seat now works on Windows ("engine-win", log adi_daw/collab/engine-win.md on branch engine-win/log).
It owns: the plug-in scanner and registry (plugin_scanner, scan_process, plugin_registry, plugin_scan_host,
juce_device_loader), file publication and the export queue (media/publish_file, render/queue), take lanes and
recording into them (CT-01 to CT-03), the measurement harness, and DF-E19 and DF-E20(c). Leave those to it. Everything
else in the engine is yours. Where you need something from its area, ask in your log.

THE QUEUE
A. The director's rulings of 18 November, one branch each, stacked where they depend:
   1. DF-E21: a hosted VST3 or CLAP plug-in is told the transport at the time of the audio at its own input (timeline
      position less the arrival latency your graph holds for that device). Measured since the ruling, on the director's
      research machine, with a probe behind a reporting delay: REAPER, Bitwig and Cubase all give the plug-in the clock
      of its incoming audio; Live does not (its clock runs ahead by the reported delay). Two precisions from those runs:
      - In a loop, the boundary that matters is the RECEIVING device's own: behind latency it wraps later than the
        transport does. Split that device's block at its own wrap. (REAPER and Cubase hand over a delayed block that
        crosses its own loop boundary unsplit; ADI splits.)
      - Before the song's start both the sample position and the musical position are negative and continuous (Cubase
        does this; REAPER clamps the musical position at zero; ADI follows the first).
   2. DF-E20 (a) and (b), in the same files: CLAP's unbounded tail is every value from INT32_MAX up; the VST3 path
      hands a plug-in only its main input, so a side-chain key never arrives.
   3. DF-E22: a return may live inside a group; a send may take Pre FX, Post FX or Post Mixer. Measured in Bitwig since
      the ruling, and matching ADR-0245 d9: direct children and children of nested groups can send to the group's
      return, tracks outside cannot, the group has no send to its own return, wet and dry sum before the group's
      devices, a return inside a group can send on to a sibling, to an outer group's return and to a top-level return,
      moving a track out removes its send, duplicating the group makes each copied send target the copied return,
      deleting the group removes the sends. Build to that.
   4. DF-E23: a hosted plug-in may run on a graph worker under its format's threading rules; your serial tests run with
      workers too. The measured gate on Windows is engine-win's, from your branch.
B. DF-E18, then DF-E13 to DF-E17, then every newer Engine row in the register that names your files (an outside review
   of the VST3 host, the mixer and plan, and undo compaction is being verified; its rows arrive as DF-E24 onward).
C. The queue of the morning prompt of 18 November (#530 and DF-E2, DF-E9, DF-E11, the small rows, the intermittents,
   what the window lane asked for, MI-08 and MI-09).
D. Then adi_daw/docs/PLAN.md, the long run: every row whose lane column names "engine" and whose status is not built,
   in milestone order M1 to M7, skipping engine-win's areas above and any row that waits on a choice of the director's
   (list those under FOR THE DIRECTOR). One row or one step of a row per branch, with its tests. Read the row's detail
   section and the ADRs it cites before you start it.

NOT TO BE BUILT
No look-ahead engine (ADR-0245 d12). Nothing of your delay compensation is rebuilt; it is the base.

REPORT
Never touch main. Gate each piece (GCC or Clang, warnings as errors, the suites), push, and write in
adi_daw/collab/engine.md on branch engine/log: "PR requested", branch, title, what you ran, what it depends on. Win
opens the pull requests, fixes the MSVC and Apple-clang legs on your branch, and lands them.
```
