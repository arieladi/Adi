# Original acceptance assets from the director's research

Evidence-only test assets for `docs/RESEARCH-INTAKE.md` section 6.9 and ADR-0245 d8.
These files are not production implementation or a new plan. No reference-application
projects, renders, caches, screen captures, installed binaries or results are included.
The specifications need separate product acceptance. Passing generator and diagnostic self-tests
does not establish host compatibility, timing correctness or CPU efficiency. The opt-in
[DF-E21 host test](df_e21/README.md) records its pre-implementation baseline separately.

| Asset | Existing plan rows | What is retained |
|---|---|---|
| `signals/generate66.py`, `expected66.json` | MX-01, MX-04, MX-33, DEV-22, EX acceptance | 11 original signals at 44.1/48/96 kHz, mono/stereo; float32 WAV hashes |
| `signals/generate_edges_latency.py`, `expected_edges_latency.json` | ED-10, MX-04, DEV-26 | Five impulse offsets, constant input and independent-channel dense code; seven original inputs |
| `midi/` | MI rows, CT-03, DEV-17, DEV-18, DEV-26 | Eight SMF files, generator and every event/tick; no MPE-zone or bend-range setup is implied |
| `tuning/` | TU-01, TU-02, TU-03; ADR-0178 | Valid, invalid, non-monotonic and unspecified-format inputs with separate format/policy expectations |
| `specs/grid/` | DEV-15 | Ten original diagnostic specifications, not executable patches |
| `specs/pd-segment-contract.json` | DEV-13 | Arbitrary event-segment boundaries; outer callback divisibility is insufficient |
| `specs/macro-composition-domain.json` | DEV-06 | Multiple sources, reversed ranges, discrete targets and stable IDs; no assumed reference equation |
| `plugins/vst3/`, `plugins/clap/`, `plugins/time/` | DEV-23, DEV-26, MX-04; ADR-0245 d8 | Retained host-context probes plus enhanced time observer/fixed-delay positive controls; source and software self-tests |
| `plugins/scanner/` | DEV-25 | Factory counters and bounded failure controls, plus isolated sequence specification |
| `performance/` | DEV-23 Windows acceptance | Explicit PID+creation+image sampler and paired run statistics with matched A/A and frequency gates |

## Reproduce the input files

Use Python with `requirements.txt` (NumPy 2.5.4 and SciPy 1.18.1 were used for the
retained hashes). From this folder:

```text
python signals/generate66.py make --out generated/audio66
python signals/generate_edges_latency.py --out generated/edges
python midi/generate.py --out generated/midi
python verify_assets.py --audio66 generated/audio66 --edges generated/edges --midi generated/midi
```

Windows CI runs `python test_generated_assets.py` with the pinned requirements. It verifies
the checked-in SHA-256 manifest and regenerates the input files in a temporary directory.

The expected hashes identify file bytes, including historical WAV containers. The seven
edge/latency files use a minimal IEEE-float RIFF container without a fact chunk; preserve
these bytes for parity. A stricter importer can require a separately identified derived
container. Never silently rewrite the expected hashes. Cross-platform math-library or
NumPy differences can change floating-point bytes: a mismatch is reported, not normalized
away. The generator's comparison command uses an explicit offset only, with no resampling
or automatic normalization; omitted prefix/tail must be reviewed separately.

## Build the diagnostic plug-ins

The original code is MIT licensed (`LICENSE`). Public interface dependencies are fetched
separately, not copied into this patch. `plugins/fetch_sdk.py` pins these revisions and
retains their original license files in the dependency checkout:

| Dependency | Exact revision | License |
|---|---|---|
| Steinberg VST3 pluginterfaces | `4f547e8e102b47de4a8b8aaf343c73b700786372` | MIT |
| free-audio CLAP | `a47f6badb49d948fd009998f28309cdab78979c9` | MIT |

From a Windows x64 MSVC developer terminal:

```text
python plugins/fetch_sdk.py --out generated/sdk
python plugins/build.py --sdk generated/sdk --out generated/plugin-build
cd generated\plugin-build
clap_selftest.exe
vst3_selftest.exe
time_selftest.exe
```

The common builder includes the time/delay module. Its alternative portable build script
under `plugins/time/probe/` uses `VST3_SDK_ROOT`, naming the directory containing
`pluginterfaces`, and builds beside its source. Prefer the common builder to keep products
outside source. No script installs a plug-in or
launches a DAW. Failure scanner modes must run in an isolated owned child. A scanner row
is not successful audio instantiation. The time probe records sparse markers only when `abs(sample) > 1e-7`; the
threshold changes logging only, not DSP, RMS/PCM capture or silence flags.
Trace memory, bounds, dropped records and context
validity flags must be checked before interpreting a probe log.

## Windows process sampling

`performance/lean_cpu.py` is an explicit-process adaptation of the director's research.
It uses persistent handles and GetProcessTimes; raw cycle counters are never seconds.
`inspect --pid PID --out identity.json` reads one exact identity. Add only independently
verified helper/observer identities to a version-1 scope manifest. There is no automatic
adoption by process name. `observe` accepts a monotonic control-file protocol with four
ordered boundaries (`run_start`, `kept_start`, `kept_end`, `run_end`), explicit target PID,
condition and transport/foreground evidence. A caller must supply that evidence; this
sampler does not operate the UI or manufacture a transport observation.

The initial marker also requires `fixture_sha256` and `operator_start_evidence`; these
are retained in the header. The final marker carries `scope_audit` with
`before_after_review_passed` and `scope_complete_operator_claim`. These are explicit
operator/harness assertions, not discoveries made by this counter collector. Missing
review is rejected by `lean_stats.py`. Preserve the controller source and observation
artifacts with each private run. Never reuse the old control-field names with this
adaptation: it uses `target_pid`, `target_foreground`, and the role `target`.

The preserved paired design requires at least five A/B pairs and equally many preceding
A/A pairs (eight for device-cost claims), alternating AB/BA, identical observer metadata,
and no reuse of a physical run. `lean_stats.py` also requires fixture hashes and a reviewed
before/after scope record. Its gate rejects frequency mismatch/missing covariates or
incomplete declared scope; no cross-DAW ranking is authorized. One logical CPU is the
denominator; process counters cannot establish callback deadlines, physical I/O or audio
continuity. Calibrate the observer with the receiving workload before performance use.

The statistics are per-app condition analyses. There is no cross-DAW ranking
helper (ADR-0248 d7). The historical `ranking_allowed` output key is retained for
format compatibility: it reports within-target analytical gates only and never
authorizes a DAW ranking. Confidence intervals, A/A controls, frequency and scope
checks, counters and callback statistics remain available.

```text
python -m unittest discover -s performance -p "test_*.py"
python performance/test_stats_schema.py
python performance/stats_original.py --self-test
python performance/lean_cpu.py calibrate --seconds 5 --out generated/observer-calibration.json
```

No historical process measurements are distributed. The original reference campaigns,
source provenance and adaptation records remain private with the director's research.
