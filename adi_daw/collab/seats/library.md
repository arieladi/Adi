Codex took the library seat on 27 November 2026.

# Library engine seat

Engine chain `seats/library-chain` at `5bb8fdd5e`, based on `origin/main` at `17a688803`. Twelve focused Debug suites passed with GCC warnings as errors and all five validators passed. The full headless CTest run is in progress.

| Row | What changed | Proof | State |
|---|---|---|---|
| TM-MD-04 | One decoded PCM pass measures duration, tone, hit density, sample kind, BPM, key and similarity descriptors. ADIA stores a versioned result per content hash. The worker takes shown files first and supports Pause. | 24/24 generated major/minor keys, minimum separation 0.182. 200,000 distinct 80 ms WAVs finished in 233.681 s cold and 45.543 s warm. First 40 finished in shown order in 432 ms cold and 7 ms warm, with zero failures. An unknown section revision and mismatched header dimensions both rebuilt. | Engine proof passed. Authored key/provenance interchange is reserved below. |
| TM-MD-07 | Six measured filters, Similar ranking and one-undo cycling through ranked sample hashes. The existing Browser text candidate service composes with this query seam. | Real WAV kicks ranked each other ahead of the hat. The Browser chain matcher passed independent words, Unicode and tempo filtering. The playing OneShot's peak was 0.037669 before, 0.113007 after swap, and 0.037669 after one Undo. Re-ranking after a rescan preserved the current hash and advanced to the next candidate. | Engine proof passed. |
