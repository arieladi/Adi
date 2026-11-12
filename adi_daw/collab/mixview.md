# Mix View seat

## 12 November 2026 — function and analysis continuation in progress

**Item:** `mixview/functions-analysis-20261112`, based on main `7aeeb098` plus the foundation PR #537 (merge `ae6cd083`). The director explicitly asks us to continue beyond the separate live gate. Win reviews and merges each slice. The private decision guide requested by the mission is written; it separates settled ADR-0237 rules from the remaining director choices. The Clip/Limiter composition question is pending and does not block the independent work below.

**Cross-lane ownership declared:** device-area opt-in function tabs/+ and same-chain movement in the window; one shared analyzer interface and process-wide demand-driven scheduler; MX-30 source leases, independent cursors, device input/output capture, timestamp/generation metadata and callback-safe storage retirement in `engine/{scope,session,graph,host,process}`; extraction of the existing ADI limiter arithmetic into a framework-neutral core and its normal DeviceInstance adapter. These paths are coordinated within the Mix View seat. No reserved processor slots, parallel edit authority or new control/catalog ops. The #512 fence remains untouched.

**Verification boundary:** strict headless audio comparisons and synthetic peerless component tests will precede review. Live interaction/capture remains exclusively with the director's Claude session. New CPU timing remains deferred at the director's request; this item will report structural demand and audio invariance checks, not invent CPU parity numbers. Private source snapshots, measurements and reference research remain outside public sources/tests.

## 12 November 2026 — actual-chain foundation ready for review

**Item:** `mixview/stack-foundation-20261112`, current commit `cae3c61e3a1ac18c3ff7754355290619c8dbf179`, based on main `e5baefec`. Draft PR [#537](https://github.com/arieladi/Adi/pull/537). Initial foundation `b7c0fe05`, portable publication follow-up `a62f8257`, explicit UI CI coverage `9db1d58a`, explicit coordinate casts for Clang `cae3c61e`. The director directly authorized connecting StackModel to actual chains and visible Mix View, followed by shared analysis/processing integration. This is the foundation slice, not the completed unified DSP feature. Win reviews/merges; no self-merge.

**Result:** STACK now uses the existing StackModel for actual-chain rows, geometry, hit/navigation/selection, filtering, pins and retained targets. It reuses DevicePanelView and ProjectDocument/ParamOps for device commands, parameter editing, gesture ownership and undo. Runtime-backed publication stamps prevent stale callbacks from writing a replacement. Hide/teardown/rejection/deletion/format interruption and cancelled asynchronous menus have explicit cleanup. Empty tracks receive no reserved processor slots. Compatible Main spectrum consumers in Info and OVERVIEW share one document-owned analysis result/transform and release their demand when hidden.

**Cross-lane paths for review:**

- Engine: one hunk in `src/adi/engine/param_ops.cpp`, checking detached runtimes before their own capture barrier so an interrupted owner can retire. No graph, routing, host SDK or DSP algorithm change.
- Window: `src/adi/ui/{stack_model,parameter_feed,project_document,shared_analysis}`, visible `ui_stack`, `ui_devices`, shell/Mix View/floating/main wiring and their tests. There is one model/runtime and one existing edit authority, not a parallel device chain or undo mechanism.
- Win/build: stable JUCE 9.0.5 pin (`d60b7a78`) in fetch script/docs, isolated CMake dependency-root support and matching fake-peer signature. `.github/workflows/ui.yml` explicitly builds/runs StackModel/shared-analysis/shared-spectrum alongside existing UI tiers. Shared main checkout/dependencies remain unchanged.
- The #512 fence (`ops*`, `changeset*`, `src/main.cpp`) is untouched. Native reference observations and private research stay outside public sources/tests.

**Validation:** Windows x64 MSVC 19.44 `/W4 /WX` Debug and Release compile. Ten targeted suites pass **3,549 checks**; focused null-native-peer Mix View subset **117/117**. Full offscreen UI goldens pass unchanged. Schema, ops, Pd and CI validators pass; diff check clean. Compiled source-copy mutants reject stale stamp admission, false latency, stale End filtering and cancelled-menu actions. No product source was planted. Full repository `tools/test_all.sh` and the remote CI matrix have not been run locally; draft remains for cross-lane review/CI and the separate live gate.

**Portability follow-up:** hosted macOS compilation caught libc++ lacking atomic shared_ptr specialization. Result publication now uses a separate UI/worker mutex and pointer swap. No FFT, old-result destruction or audio callback takes that critical section. Independent lock-order review passed, and all ten targeted local suites passed again after the fix (3,549 checks). Remote CI is still a separate gate.

Strict Clang also required two explicit int-to-float mouseExit bounds conversions; semantics are unchanged. Focused Mix View reran 117/117 after that patch; independent warning audit found no further conversion/sign issue. Final Release package pins the current commit. Remote CI/live acceptance remain separate.

**Live/CPU state:** review package and a targeted handoff are prepared privately for the director's Claude tester. Codex has not locally launched ADI.exe, a native test host or popup, attempted desktop input/capture, or collected CPU measurements. The repository's existing hosted Windows CI has its own hidden-window smoke; that does not establish live interaction acceptance. CPU collection remains explicitly deferred by the director. Headless/offscreen correctness is not called live parity or CPU parity.

**Next integration / non-blocking boundaries:**

- The first analyzer targets Main only: PostFader/Mid/Hann/1024 with stereo peak/RMS. Selected-track following, comparisons, the complete shared analyzer suite and movable native function tabs remain later slices.
- MX-30 owns device tap endpoints, independent gapless cursors, negotiated history and safe tap-storage retirement. Current ScopeTap can retain an old sample rate; the adapter honestly marks that source unavailable after a rate change. Safe replacement is needed before removing this restriction.
- DEV-05/MX-24 own compensated device-specific key routes. The UI does not simulate missing sidechains.
- Unified opt-in EQ/unmasking, C, DS, G and L/Clip cores must enter the same Session nodes and parameter/undo authority. Still placement remains a director guide question. Existing ordinary devices continue processing unchanged through Session/Graph.
- Pins/search/row-size state currently lasts for the window lifetime. Native editor and visual interaction acceptance await Claude's report before claiming them verified.

Public implementation scope is in `docs/MIX-VIEW-FOUNDATION.md`. Next work follows ADR-0236/0237, keeping analyzer demand separate from processing lifetime and introducing no reserved slots.
