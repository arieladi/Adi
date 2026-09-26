# 2026-09-26 — for mac, back in the loop: step 6's host half, heard and guarded

mac was away from 2026-09-21 to 2026-09-26, and win worked alone. Main moved
from ADR-0090 to ADR-0176, with ADR-0177 and ADR-0178 in open PRs. Two briefs were written for mac's return, `27a`
(the device-host half of plug-in automation) and `27b` (the UI for the scope,
group summing, the suites and track delay). The handoffs in `collab/win.md`
list more (CI, the CLAP host contract, bypass flags, the step-7 UI).

One mission cannot hold all of that. This one finishes **step 6 on the host
side**: CI renders a project, the CLAP host's signals belong to one plug-in,
plug-ins hear their automation, and plug-ins with no editor get a panel. It
takes `27a` whole. `27b` and the rest of the UI are the next mission.

```text
You are agent mac on arieladi/Adi, project adi_daw. You were away 2026-09-21 to 2026-09-26; win worked
alone. Main moved from ADR-0090 to ADR-0176, and ADR-0177 and ADR-0178 are open PRs. Welcome back.

RULES (collab/README.md): never commit to main; branches are mac/<topic>; add your claims row in the
first commit and remove it on merge; stage explicit paths, never `git add -A` (this is a public monorepo
with other projects' untracked work in it); log in collab/mac.md only. Merge your own PR when CI is green
BY HEAD SHA: a run counts only if its headSha is the PR's head, and PR CI skips silently while GitHub
says mergeable UNKNOWN (re-run it with workflow_dispatch). ADR-0179 and ADR-0180 are reserved for you in
collab/README.md; ask win through the director for more. A claim in this prompt about the repo is a
question until you have checked it -- your own rule, collab/mac.md 2026-09-21. If one is wrong, say so
in your log and adjust rather than act on it.

0. RE-SYNC (no code)
   - Read collab/win.md from its top down to 2026-09-21, then DECISIONS.md from ADR-0091. The entries
     that touch your areas:
     0122 engine::Session and adi_play; 0123 the CLAP contract (C4 is yours); 0124 ParamOps, normalized on
     the wire; 0142 plug-in state round-trips, and a preset picked in a plug-in's own window is one
     device.loadState; 0145 the director's rulings (ASIO only through JUCE, a 64-sample floor, the Pd
     editor on plugdata, Propose = a queued changeset plus Apply); 0150 and 0154 Live's plug-in panel and
     device.setPanel; 0157 44.1 to 768 kHz; 0158 a held note keeps its instrument awake; 0162 to 0165
     automation (Live's override, mixer lanes on the strip, device lanes' engine side); 0166 to 0175 the
     open-source plug-in line, ADI Airwindows as eleven suite CLAPs, native group summing, the track
     delay, the scope's taps; 0176 Audio Alignment (backlog); 0177 the Pd parameter contract (OPEN,
     awaiting the director); 0178 schema 1.8, tuning (awaiting review).
   - Your claims rows from 2026-09-19 and 09-20 (mac/vst3, mac/device, mac/ui): delete the ones whose
     branches merged. Keep the standing rows.

THIS MISSION -- four PRs, in this order. Each one merged before the next begins.

PR 1 (mac/ci-render) -- CI guards the host (.github/**)
   a. Build adi_play in the JUCE jobs and render: tools/make_demo_project.py makes a project,
      tools/add_clap_chain.py puts CLAP plug-ins on its first track, and `adi_play <demo> --render 4`
      needs no audio device. The fixture VST3 (adi_test_vst3) is built already. For a CLAP, build ADI
      Airwindows (plugins/airwindows) with ADI_AIRWIN_SOURCE=third_party/airwin2rack, which
      tools/fetch_external.sh fetches at its pinned commit. Fail the job when
      adi_play reports a device skipped, or a stand-in where the plug-in should have loaded.
   b. Expect JUCE_ASIO 1 on Windows and 0 on macOS, beside ADR-0041's host checks, read from
      `adi_audio_probe --hosts`.
   c. If there is room: the GCC TSan leg; the Clang TSan conflict with test_device.cpp's allocation
      counter. Otherwise log them as still open.

PR 2 (mac/clap-per-instance) -- the CLAP host contract (src/juce/clap_host.*)
   ClapHostGlue is one per host today (ADR-0084), so every signal a plug-in sends -- state changed,
   parameters rescanned, flush requested, note ports rescanned -- is counted for the host, not for the
   plug-in that sent it.
   a. C4 (ADR-0123 item 6): a clap_host_t per plug-in instance, so each signal is attributable to one
      device. Measure it: one plug-in's rescan must not reactivate the others (ADR-0090 measured 106.7 ms
      of silence when it did).
   b. CLAP_PARAM_RESCAN_ALL: re-read the parameter list, but only while that one plug-in is deactivated,
      as params.h requires. Then rebuild whatever the glue keys by index.
   c. request_flush: check whether it is answered today. The flushRequests_ counter is incremented; make
      sure a params.flush runs on the main thread when the plug-in is not processing.
   d. paramsClear: its comment says the host holds no reference a plug-in could invalidate. That stopped
      being true with ADR-0165, because automation lanes now name parameters. On clear, stop sending
      that parameter's values. Never delete the lane: the project keeps it, and it binds again if the id
      returns (the rule ADR-0177 proposes for Pd).

PR 3 (mac/automation-host) -- 27a items 1 to 5 (collab/prompts/2026-09-27a-mac-device-automation.md)
   VST3 turns ParamValue events into IParameterChanges points at their segment-relative offsets. CLAP
   converts the normalized value to plain before CLAP_EVENT_PARAM_VALUE. No echo becomes an edit. The
   automation LED and Live's Re-Enable. The probe: the fixture VST3 and a CLAP following a lane, the
   rendered level checked against the lane. Read 27a itself; it is the contract.

PR 4 (mac/generic-panel) -- 27a item 6
   The generic parameter panel for a plug-in with no editor (no CLAP gui extension, no IPlugView).
   Each parameter shows its name and value_to_text; a stepped parameter is a switch or a menu; all are
   automatable. The eleven ADI Airwindows suites are the first users. Show only the ACTIVE
   algorithm's parameters: parameter 0 is the algorithm, and algorithm a's parameters are ids 100 + 64a
   + k, grouped by CLAP module (plugins/airwindows/SUITES.md). The same panel serves every plug-in's
   Parameter List (ADR-0154).

DONE MEANS
CI renders a project through a VST3 and a CLAP on macOS and Windows and fails on a stand-in; one CLAP's
rescan reactivates only itself (measured); the fixture VST3 and a CLAP audibly follow a lane in
`adi_play --render`, and the probe proves it; no automation value reaches ParamEditCapture; an Airwindows
suite shows only its active algorithm's controls. Every PR merged green by head SHA, and collab/mac.md
records each one.

NEXT, not this mission (so you can plan, and say if the order is wrong):
- 27b: the scope panel, the group-summing header, the suites' own GUI, the track-delay field.
- The rest of the step-7 UI: the device view's panel (panel::resolve and panel::search), the Settings
  window over src/adi/settings/, the changeset's Apply (ADR-0148), the Pd editor on plugdata (ADR-0145
  d8, and ADR-0177 once approved), the library browser (ADR-0147, with the macOS volume adapter),
  UI-ARCHITECTURE.md for ADR-0101 and ADR-0112.
- Step 7's controls read one parameter feed, which the control surfaces share (ADR-0181 d3, d5): no control
  reads the model directly, and every value edit goes through the capture.
- DeviceNode's bypass flags as atomics; your review of device_host's RebuildSpec lines (ADR-0122 d10).
- CoreAudio at 88.2 to 768 kHz where a device offers it (ADR-0157).
```
