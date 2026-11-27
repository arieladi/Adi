Codex took the library seat on 27 November 2026.

# Library engine seat

Engine chain `seats/library-chain` at `5bb8fdd5e`, based on `origin/main` at `17a688803`. GCC 14.2 Debug with warnings as errors, Pd and acceptance enabled, built successfully. Headless CTest passed all 288 suites in 261.50 s, including the real-time stress suite and both acceptance suites. Schema, ops, parity, Pd and CI validation all passed.

| Row | What changed | Proof | State |
|---|---|---|---|
| TM-MD-04 | One decoded PCM pass measures duration, tone, hit density, sample kind, BPM, key and similarity descriptors. ADIA stores a versioned result per content hash. The worker takes shown files first and supports Pause. | 24/24 generated major/minor keys, minimum separation 0.182. 200,000 distinct 80 ms WAVs finished in 233.681 s cold and 45.543 s warm. First 40 finished in shown order in 432 ms cold and 7 ms warm, with zero failures. An unknown section revision and mismatched header dimensions both rebuilt. | Engine proof passed. Authored key/provenance interchange is reserved below. |
| TM-MD-07 | Six measured filters, Similar ranking and one-undo cycling through ranked sample hashes. The existing Browser text candidate service composes with this query seam. | Real WAV kicks ranked each other ahead of the hat. The Browser chain matcher passed independent words, Unicode and tempo filtering. The playing OneShot's peak was 0.037669 before, 0.113007 after swap, and 0.037669 after one Undo. Re-ranking after a rescan preserved the current hash and advanced to the next candidate. | Engine proof passed. |
| TM-MD-08 | Fits scores open ERB bands, gaps in the reference's onset profile, and musical key compatibility. | In-key hat in a gap scored 1.000, colliding hat 0.750, tritone hat in a gap 0.700 and second bass 0.300. The 200,000-record top-64 medians were 0.030 ms for measured filters, 8.006 ms for Similar, and 26.260 ms for Fits. | Engine proof passed. |
| TM-WT-05 | The cached kind drives type filtering, audition loop eligibility, and the explicit Warp/import seam. Opt-in Warp builds two anchors for a detected loop or returns a clean one-shot. A consumed measurement may carry its content hash. | 75 generated 1/2/4/8/16-bar loops at 70–180 BPM, 44.1/48/96 kHz and ±8 ms onset jitter gave 75 correct bar counts and ≤0.000183 BPM error. A decoded 120 BPM loop gave two anchors and 8 quarter notes. Stale source hashes were rejected and Undo restored the clean clip. | Engine proof passed with provisional Live readings below. |
| TM-WT-01 | The library and existing Warp share one streaming onset implementation. Labelled scratch pilot compared the current timing path with pinned Mixxx/qm-dsp and Essentia. | Five append sizes produced identical onset frames. Six GuitarSet recordings gave beat F at ±70 ms of 0.1278 for current ADI, 0.4531 for Mixxx/qm-dsp, and 0.5604 for Essentia. See the pilot below. | Corpus gate pending. |

## Benchmark conditions

The 200,000-source run used one lowered-priority worker, distinct mono 8 kHz PCM files, the production decoder/analysis pipeline and workspace storage. Cold means a fresh ADIA folder. Source pages were eligible for OS caching. Full builds and tests shared the four-CPU quota. The run included caching and source verification. Directory enumeration and query publication have their own timings. An additional 200 two-second 44.1 kHz stereo fixture run took 2.579 s for the first 40 and 10.228 s for all cold under concurrent build load. The query timing used already measured synthetic records, GCC Release, a 64-row limit and the median of seven trials.

## Engine handoff

Supply the displayed listing keys alongside `SampleAnalysisCatalog::replaceFiles`. The catalog's worker starts paused. The owner can pause it between files, resume it, or change the priority of pending jobs as the visible list changes. A known hash deduplicates bytes across Places. For an unhashed listing use a stable path-and-revision key, so a changed file never inherits the old measurement. Returned query pointers own immutable records. Keep the app caches and optional ADIL annotations alive until the catalog stops.

Use the same candidate set from the Browser chain's `BrowserLibrarySnapshot::appendSamples` and `LibraryQuery` at both entry points. Match each candidate to a catalog record, feed that ordered list through `querySamples`, and identify returned candidates by row offset. The Browser chain service was tested by compiling its actual matcher beside this engine branch. For preview looping use `samplePreviewLoops`. For explicit Warp or the import prompt pass the existing setting, resolved classification, and measurement content hash as `ShortSampleWarpInput`. Use the same opt-in for Warp off on a generated full-source loop. Similar stepping uses already-ranked file/hash candidates and the existing undoable sample assignment.

`ALIB` v1/configuration 0 is a derived section in ADIA. It repeats dimensions inside the digest-covered payload, carries the type, tempo and key with source provenance, and stores full-file descriptors and bounded file tags. Eligible sources over 2 s also carry a separate first-2-s descriptor set. A malformed section is recomputed from the current source bytes. The analysis cache and per-file type/tempo are shared by the engine consumers. User tags, BPM and saved loop settings are read from ADIL on publication and overlaid on the published measurement. They are outside the evictable analysis data. Explicit key supplied by the caller has precedence.

Shared edits are in `CMakeLists.txt`, `audio/transients.*`, `library/index.*`, `media/analysis_cache.*`, `material_commands.*`, `warp_commands.*`, `sample_slots.*`, and the panel-free `ui/project_document.*`. The hot-swap changes bind the ranked content hash to the import, and acknowledge the initial sample state after the newly loaded device is realized so immediate Undo reaches live playback. Tests cover existing Warp and import behavior, the opt-in, and a real OneShot. Project edits reuse the current Warp and sample-assignment ops.

## For win

A. The labelled suite analyser selection in ADR-0226 d4 still needs a representative held-out corpus and independent comparators. Keep the shared onset detector and fallback key estimate behind the worker seam until that gate is met. The six-recording feasibility study below is too small to choose the suite analyser.

B. TM-MD-04's authored key/provenance sync needs an ADIL annotation-format decision. Recommend a backward-readable annotation update under a reserved interchange version if authored key is persisted. This bundle reads existing BPM/tags and saved loop choices. It accepts an explicit caller key and reads embedded keys without adding a field to user data.

## Readings owed

- Live 12.4's short/long Warp boundary, Auto on an explicit Warp press versus the import prompt, and whether ACID BPM wins in Warp. The 60 s short-Warp boundary is provisional. Forced short-loop timing rounds to a permitted bar count near the known BPM or supplied tempo hint. [Live 12.4 Warp manual](https://www.ableton.com/en/live-manual/12/audio-clips-tempo-and-warping/#warping-short-samples)
- Live's documented Sound Similarity limit is 60 s and its sound analysis uses the first 2 s. Published results run most-to-least similar. ADI uses measured spectral and envelope distance, hits, RMS, and lower-weight length, known key and tempo. Live's feature weighting and score cutoff need a comparative reading. [Live 12 manual](https://www.ableton.com/en/live-manual/12/live-concepts/#sound-similarity) · [Similarity FAQ](https://help.ableton.com/hc/en-us/articles/11386675465628-Sound-Similarity-Search-FAQ)
- MP4 tags and compressed or unsynchronized ID3 variants remain outside the bounded metadata reader. WAV/ACID, AIFF Apple Loops, regular ID3, FLAC and Ogg Vorbis are covered. BPM resolves user, file tag, ACID, then detection. A bare MIDI root does not supply a major/minor key.

## TM-WT-01 feasibility study

Six GuitarSet 4/4 guitar accompaniments from the CC BY 4.0 release, with their original beat and downbeat annotations, made a labelled pilot on this machine. The clips were `00_BN1-129-Eb_comp`, `00_Rock2-142-D_comp`, `01_Funk3-112-C#_comp`, `02_BN1-129-Eb_comp`, `02_Rock2-142-D_comp`, and `03_Funk3-112-C#_comp`. The candidate used Mixxx `902cfc2543d7ef65bda1678e6f3c3e98be4aa39d` and qm-dsp `4d2a4a4e0c2dd0ddf07bf9d2a224ded912712714` at 44.1 kHz. The existing ADI beats were reconstructed by quarter-tick interpolation of the captured AWRP anchors with the trim offset reapplied. Downbeat estimates were the bar anchors, excluding the end marker. The Mixxx comparator estimated BPM from median inter-beat distance. The third column uses Essentia RhythmExtractor2013 multifeature at 44.1 kHz.

| Macro measure | Current ADI | Mixxx/qm-dsp pilot | Essentia |
|---|---:|---:|---:|
| Beat F, 70 ms | 0.1278 | 0.4531 | 0.5604 |
| Downbeat F, 70 ms | 0.0593 | 0.3833 | — |
| CMLt | 0.0042 | 0.3255 | 0.5000 |
| AMLt | 0.0304 | 0.6657 | 0.7984 |
| Tempo Acc1 and Acc2, ±4% | 0/6 | 4/6 | 5/6 |

Current ADI onset F at ±10 ms was 0.2733 against per-string note starts coalesced within 10 ms. Candidate onset F and independent beat_this results are still owed. Essentia supplied beats and tempo. The corpus has six clips from three styles and one instrument. Gate results need broader instrumentation, meters, and held-out arrangements. The experiment and its reproducible setup are in the cloud workspace at `/workspace/scratch/adi-warp-feasibility/setup.sh`. [GuitarSet](https://zenodo.org/records/3371780) · [Mixxx](https://github.com/mixxxdj/mixxx/tree/902cfc2543d7ef65bda1678e6f3c3e98be4aa39d)

## Continued acceptance on 27 November

| Item | Change and proof | State |
|---|---|---|
| TM-MD-04, JSON round trip | Chain `7d15f9b59`. Library JSON carries authored key plus observed key strength and BPM provenance. The authored settings and two kinds of observations round-tripped between databases, source precedence was checked, and malformed imports rolled back both results. GCC warnings-as-errors linked fixture passed 65 checks. | Exchange slice pushed. |
| TM-MD-04, loudness | Chain `e655a3942`. The shared BS.1770 meter measures gated loudness in the same PCM pass and writes ALIB v2. A 1 kHz reference measured −15.0448 LUFS in mono and with an independent 5.1 LFE. Warm ADIA read-back was exact, and the sample-analysis suite passed 37 checks. | Loudness slice pushed. |
| TM-MD-04, automatic eviction | Chain `1892cf284`. The DAW settings bind the existing max bytes and free-space limits to ADIA. Warm startup can read immediately. Publications and subsequent preference changes enforce the cap. 66 analysis-cache checks, 5 trim checks, and 345 settings checks passed. | Analysed-cache policy pushed. |
| TM-MD-04, embedded tags | Chain `080c7f5b0`. Read-only TagLib v2.3.2 covers MP4, APE, Matroska and ID3 variants; WAV handling retains ACID, sampler and instrument metadata. The ALIB configuration advances. Warnings-as-errors passed 24 checks, including MP4 129.75 BPM and unchanged source bytes; the previous reader failed nine cases. | Tag read-back slice pushed. |
| TM-WT-05, settings-driven producers | Chain `ec2ea98b2`. Explicit Warp and import read the captured four-mode setting, use one content-addressed classification, and preserve authored defaults. A real 120 BPM WAV gave one bar on both producer paths at a 127 BPM song tempo. Ordinary drop performed no analysis, and the second explicit request reused the cache. The focused settings, Warp, and import suites passed 4/4. This commit also finishes routing TagLib compilation through its private target. | ADI policy and capture passed. Live 12.4 timing readings remain owed. |
