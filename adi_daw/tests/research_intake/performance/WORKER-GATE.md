# Hosted graph worker measurements

The director's research supplied the original ADI Engine Probe and Windows
process sampler. This runner exercises ADI itself through the ASIO device at
48 kHz, with fixed stereo output and the actual granted block size. Its CSV
records the existing driver's callback bridge and engine processing bracket.

Build optimized x64 Windows `adi_play` and `adi_tool` with the DF-E23 hosted
worker implementation and the hardware diagnostics. Build the owned
`ADIEngineProbe.vst3` with the original probe build script in `../plugins`.
Use the matching scanner beside the player. Each project is disposable and
uses a private ADI profile and module directory. The player scans only that
directory. Paths and sizes below are examples.

```powershell
python worker_fixtures.py --player "$play" --tool "$tool" --module "$probe" --out "$evidence/heavy"
python worker_fixtures.py --player "$play" --tool "$tool" --module "$probe" --out "$evidence/light" --iterations 125
python worker_verify.py --player "$play" --fixtures "$evidence/heavy/fixtures.json" --out "$evidence/witness"
python worker_measure.py --player "$play" --fixtures "$evidence/heavy/fixtures.json" --out "$evidence/stress" --build "$commit" --warm 3 --keep 7
python worker_measure.py --player "$play" --fixtures "$evidence/light/fixtures.json" --out "$evidence/pairs" --build "$commit" --workers 0 5 --repeats 5 --warm 5 --keep 10
python worker_stats.py "$evidence/stress" "$evidence/pairs" --out "$evidence/summary.json"
```

The independent project has 64 tracks with one probe each. The serial project
uses one track with 64 probes and the same per-probe workload. Each probe
generates a quiet tone, and every instance is forced to process. The two
topologies have different graph overhead. Interpret worker contrasts within a
topology. `worker_verify.py` checks all native instance IDs and their actual
arithmetic counts in an untimed offline run. Its shared trace is disabled for
hardware captures.

The hardware script waits for the player's first-callback READY marker. It
checks the actual ASIO device, format and requested worker cap before collecting
an explicit-PID process sample and QPC-aligned callback data. It alternates
worker order between repetitions. Outputs include the raw device receipt,
callback CSV, process/PDH samples, file hashes and rejection reasons. Each
accepted callback falls wholly inside the retained window. The report retains
the actual allocation and worker activity because a serial graph can cap its
pool at zero.

Report callback p50, p95, p99 and maximum, observed duration overruns and
callback-start spacing separately. Process CPU is percent of one logical
processor, derived from GetProcessTimes and the sampled wall interval. ASIO
overload notifications can be unavailable. A duration overrun, long start
spacing or manager estimate alone cannot determine physical lost samples.
An overloaded process may service fewer callbacks in the same wall interval.
Use its process CPU and delivered callback count together.

These short workload checks cover the requested 256- and 2,048-sample
configurations. ADR-0229 also requires the wider tracking and mixing suite,
the specified output equivalence tests and verified worker real-time standing
before changing the default. Keep each timing protocol separate and report
within-target uncertainty. Save run projects, raw data and audio locally.
