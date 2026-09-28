# Prompts

The mission prompts win wrote for the linux (Codex) and cloud sessions, exactly
as the director handed them over, oldest first. The claims table in
`../README.md` is the live state of who holds what; these are the record of
what was asked, so a report can be checked against its brief.

| File | linux | cloud | Result |
|---|---|---|---|
| `2026-09-24a-collect-export-and-migration.md` | Collect and Export, media ops | 1.x migration; remarks in the projection | #95; #92, #93 |
| `2026-09-24b-publish-library-changeset.md` | publish without hard links; library index | Propose changeset | #101, #102; #100 |
| `2026-09-24c-clip-playback-and-settings.md` | clip playback and the transport | settings store | #106; #105 |
| `2026-09-24d-midi-clips-and-decoder.md` | MIDI clips; clip rates to 768 kHz | decoder; the full settings registry | #113; #111, #112 |
| `2026-09-24e-cloud-curves.md` | (out until 2026-10-01) | curve formulas; automation read into the engine | #116, #117 |
| `2026-09-27a-mac-device-automation.md` | (mac) | the device-host half of plug-in automation | taken whole into 2026-09-26 (PRs 3 and 4) |
| `2026-09-27b-mac-scope-summing-suites.md` | (mac) | the UI for the scope, group summing, the Airwindows suites, track delay; the OScope/PsyScope research | mac's next mission, after 2026-09-26 |
| `2026-09-26-mac-return-host-half.md` | (mac) | back in the loop: CI renders a project; a `clap_host_t` per instance; automation the plug-ins hear; the generic panel | round 1 reported 2026-09-26: PR 1 is #134 |
| `2026-09-27-mac-round2.md` | (mac) | win's answers: ADR-0179 stays mac's, the analyser takes 0183; the macOS hang hypothesis; add_clap_chain fixed; 27a items 4 and 6 as API; win takes adi_play's drain | issued 2026-09-27 |
| `2026-09-27-mac-analyser.md` | (mac, second session) | the Pd analyser kept by the director: ADR-0183 in place of 0179, its own worktree and log, rebased and checked against ADR-0177, libpd through fetch_external.sh (ADR-0024) | issued 2026-09-27 |
| `2026-09-27-mac-round3.md` | (mac) | win's answers to round 3: PR 2 goes ahead; ADR-0179 extends ADR-0177 d4 to CLAP; param-indication's automation half in PR 3; PR 4's panel is ADR-0181 d3's record; `--expect-no-edits`; what ADR-0184 and ADR-0185 ask of mac (measure Live's default Device View height) | issued 2026-09-27 |
| `2026-09-27-mac-round4.md` | (mac, both sessions) | the decisions since round 3: the name ADI (ADR-0190); surfaces, the 169 px floor, visual streams, UMP through JUCE (ADR-0188); for #138: rebase, no externals from disk, the Pd runtime rules | issued 2026-09-27 |
| `2026-09-27-win-codex-mission1.md` | (win_codex) | onboarding, and the color-bass devices: DSP cores and tests (PR 1), then the compiled-in externals and `.pd` devices (PR 2) | issued 2026-09-27 |
| `2026-09-27-mac-round5.md` | (mac, both sessions) | host: PR 2 design confirmed, then the step-7 plan after PR 4; analyser: pthreads4w, the built-ins hook, MIDI into Pd, `[adi.sample]`'s host side, `adi.param.pd` and `adi.transport.pd` | issued 2026-09-27 |
| `2026-09-27-mac-round6.md` | (mac, both sessions) | host: the JUCE gate lesson, order unchanged, MIDI Learn in the step-7 plan; analyser: merge the stack, transport delegated to win_codex, the multi-track overlay and a shared masking measure (ADR-0195 d5) | issued 2026-09-27 |
| `2026-09-27-win-codex-mission2.md` | (win_codex) | transport on `NodeIo` (delegated from win), the color-bass devices' PR 2, then the Dynamic EQ (ADR-0195) | issued 2026-09-27 |
| `2026-09-28-win-codex-mission2b.md` | (win_codex) | #157 approved; the analyser's per-track taps (ChainInput, PreFader) before the color-bass PR 2 (ADR-0195 d5) | issued 2026-09-28 |
