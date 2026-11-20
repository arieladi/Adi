# Decode cache admission, hint recovery and cancellation (DF-E28, DF-E34 b/c)

Before evicting any existing file for a new complete decode, `DecodeCache`
refuses an individual estimated Float32 size above its maximum. The estimate
is the existing `frames * channels * 4 + 4096` reservation. The error is
`decode.no_space`, with the estimate and configured maximum; it does not claim
that every cached file is pinned. This applies to a genuine oversized source;
physically impossible FLAC lengths are refused earlier as corrupt, as below.
No partial WAV has been written at this point.

The same mutex-held `DecodeCache::admitSpace` serves complete decoding and
progressive admission. The latter passes its checked sum of the incoming full
estimate and existing queued/running reservations. If current free space is
insufficient, the helper first totals existing ordinary files eligible for
eviction: no pinned entries and not the incoming hash. Missing index entries,
unreadable sizes and non-regular paths contribute nothing. If free plus those
bytes cannot meet the request while retaining minimum free space, admission
throws `decode.no_space` before any eviction, with the estimate and reachable
headroom. This refusal does not claim that every file is in use.

An admissible estimate retains the existing policy: make room in LRU order,
skip busy files, preserve pins even if the aggregate remains above the cap,
and check actual free space again after eviction. The existing in-use diagnostic
remains for eviction stopped by pins or busy files. Sums saturate and comparisons
use subtraction, including the eviction loop and public total-byte count; large
settings cannot wrap the demand into apparent headroom. The estimate includes
a conservative 4 KiB header allowance.

The preflight uses a conservative estimate, not exact physical feasibility or
guaranteed recoverability. Logical file lengths may understate allocation-unit
space recovered from small files, so a near-boundary request can refuse even
though deletion might physically make room. They may instead overestimate disk
bytes recoverable from sparse, compressed or hardlinked files; a holder may
refuse deletion; another process
may consume space or recreate a just-removed name. The final free-space check
still refuses without publishing incoming audio. A recreated file is not
re-added to this instance's inventory by eviction; normal subsequent hit or
startup inventory adopts it. No repeated directory scan, cross-process pin
lease, namespace snapshot or physical disk reservation is introduced. Unknown
stream lengths, changes to limits during decoding and global streaming quotas
retain their existing behavior.

FLAC's central decoder initialization measures the already-open stream's byte
length, then initializes the codec under RAII. [RFC 9639 sections 4.1, 8.2 and
9](https://www.rfc-editor.org/rfc/rfc9639.html) limit a frame to 65,535 interchannel
samples; every frame occupies at least one byte. Therefore
`ceil(declaredSamples / 65535) <= encodedBytes` is a necessary condition. The
division/remainder calculation cannot overflow. Including metadata and Ogg
overhead makes this deliberately loose and accepts extremely compressed
constant audio. Total zero still means unknown and is not rejected by this
guard. Native FLAC and Ogg FLAC share it through `FlacDecoder`, hence complete
decode and `openPcmStream`/`inspectPcm` agree. This catches impossible tiny-file
claims, not every malformed or padded stream: normal decoding must still check
the actual total. Stable sources remain required; this is not protection against
a concurrent writer modifying the same open file.

The optional `index.json` remains a hint. Each entry's `lastUse`, when present,
must be a signed or unsigned integer representable in `int64_t`; strings,
booleans, nulls, arrays, objects, floating values and larger unsigned values
invalidate that entry's hint. A missing `lastUse` still means zero. The directory
scan then re-adopts an existing hash-named WAV with its actual file size and a
zero use stamp when its hint was missing/bad. Other valid hints survive, and
startup does not alter PCM bytes or rewrite the input index. The maximum signed
stamp saturates on subsequent use instead of overflowing; such extreme stamps
can tie, without changing pin accounting.

A complete publication, including a valid same-dimension target that another
writer published first, is recorded in the in-memory inventory and decode
counter and passed through the best-effort index save before the final stop
check. If cancellation is observed there, the caller receives `decode.cancelled`
and acquires no pin. The complete, counted PCM remains available for a later hit
or ordinary eviction. Cancellation does not delete a published target or undo
another caller's pin. On-disk index persistence is still optional; scanning real
files recovers it after restart.

`DecodeCacheHooks::afterPublish` is an empty-by-default test seam immediately
after successful publication/adoption and before bookkeeping. It runs outside
the mutex and must not throw or reenter the cache. The regression requests stop
there, then checks current bytes, saved hint when writable, zero unreturned pins,
retry and later trim. No timing-dependent race is needed.

The regressions use real repository FLAC fixtures and owned temporary files.
`adi_decode_space_tests` exercises both admission routes with impossible disk
headroom, a missing inventory file, near-uint64 settings, queued reservations,
feasible LRU eviction, a Windows reader denying deletion, external space loss
and a reappearing cache file. A small test-only FLAC writer creates valid constant
frames with CRCs and a changed declared total; no external encoder is required
at test runtime. Complete and progressive paths must accept all valid silent PCM
and refuse an impossible four-billion-sample claim without touching the cache.
Existing complete, PCM stream and progressive suites remain regression coverage.
This changes neither the progressive worker pool nor its publication ownership.
