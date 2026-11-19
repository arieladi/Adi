# Diagnostic platform contract

These are original fixtures from the director's research. They do not contain
reference-host measurements or claim production-host compatibility results.

## Mapping and publication

The keeper alone creates an exclusive named object, initially zero-filled. The
plug-in opens an existing object and validates its size, magic, record size and
capacity before retaining the mapping. A missing/short/incompatible mapping
disables tracing; it does not create a new unrelated buffer or change audio DSP.
The keeper must outlive all writers. Reset counters only with writers quiescent.

On Windows the logical name remains the native mapping name, including `Local\`
and the optional `_PCM` suffix. UTF-8 is converted to the original native UTF-16
mapping API without changing the characters. On POSIX the UTF-8 logical name maps
to `/adi_` followed by 16 lowercase hex digits
of FNV-1a/64: offset basis 14695981039346656037, XOR each unsigned input byte,
multiply by 1099511628211 modulo 2^64. Hash the suffix as part of the name. This
keeps names below macOS's length limit. Exclusive create detects an existing
name, including a hash collision. Owner cleanup unlinks; a recorder never unlinks.
POSIX objects have owner read/write permissions only.

The historical byte layouts remain: engine record 144 bytes/header 96, PCM chunk
64/header 80, time record 2488/header 56. All start after the same 4096-byte header
region; compile-time size/offset checks reject incompatible ABIs. Counter types
are explicitly 32/64 bits, not Unix `long`. Reservation uses atomic fetch-add;
completed records publish their sequence with release ordering and readers use
acquire. Capacity exhaustion increments lost counters without overwriting data.
Counters are required to be lock-free; no mutex fallback is permitted.

## Clock and metadata

Windows keeps QPC ticks and the actual QPC frequency. POSIX records
`CLOCK_MONOTONIC` nanoseconds with frequency 1,000,000,000. This is an observation
clock, not an audio clock; cross-platform epochs cannot be compared. Process IDs
come from the OS. Linux thread IDs come from `gettid`; macOS uses
`pthread_threadid_np` and records `UINT32_MAX` if its 64-bit result cannot fit the
historical field. macOS CPU number is `UINT32_MAX` (unavailable). POSIX priority is
`INT32_MIN`, not a fabricated Windows priority. Optional host-extension symbols
are dynamically looked up; absence retains the existing unavailable sentinels.

## Native modules and tests

Windows keeps the existing flat PE module names and `InitDll`/`ExitDll` exports.
macOS packages actual loadable `.vst3`/`.clap` bundles with Info.plist, exports
`bundleEntry`/`bundleExit` for VST3, and passes a real CFBundleRef during self-test
loading. Linux VST3 modules use `Contents/<processor>-linux/<name>.so` and
`ModuleEntry`/`ModuleExit`. CLAP keeps its actual `clap_entry` ABI. No installation,
system plug-in scan or application launch occurs in these self-tests.

All three tests load the compiled owned module and require committed records,
correct factory/capability behavior, DSP and cleanup. Additional platform tests
cover missing/undersized/invalid maps, exclusive ownership, overflow, clock/ID
metadata, cleanup and UTF-16 conversion. VST3 strings use UTF-16 code units on
every platform; they no longer alias Unix's wider `wchar_t`.

The project applies its normal strict warnings plus warnings-as-errors to all
owned targets. Both hosting CI legs build the four diagnostics and run the
three tests. The separate DF-E21 real-Session gate remains Windows-only and
disabled unless explicitly enabled by its implementing branch.

## Validation boundary

Windows x64 MSVC compilation and all three loaded-module self-tests were run
after this port, including exact 1024/4800-sample delays, sparse marker threshold,
raw transport bytes, parameter offsets, PCM and overflow. A supplementary Clang
19 frontend audit on Windows passed the project's conversion/alignment warning
flags. That audit does not compile or execute the POSIX branches.

Native macOS and Linux builds/runs were not available locally. Their loaders,
shared memory and bundle behavior require native CI evidence before treating the
port as verified on those platforms. Only the macOS hosting leg is configured in
this CI matrix. The historical Python capture utilities and standalone builder
remain Windows-only; external POSIX recording is not claimed by these tests.
