# Decode cache refusal, hint recovery and cancellation (DF-E28)

The desktop applies the maximum-size and minimum-free-space preferences at
startup and when settings change. Applying them leaves cache creation and
cleanup to normal cache operations. Updates retain the same cache object
and its live pins. The desktop reads Cache Folder on launch. Decoded PCM uses
the `decoded` child and waveform analysis uses the `analysed` child of the
selected absolute path. Both use the app cache parent by default. Each child
is created only when its cache is needed. Changing the setting takes effect
on the next launch so existing readers keep their files. Budgets remain
separate.

Before evicting any existing file for a new complete decode, `DecodeCache`
refuses an individual estimated Float32 size above its maximum. The estimate
is the existing `frames * channels * 4 + 4096` reservation. The error is
`decode.no_space`, with the estimate and configured maximum; it does not claim
that every cached file is pinned. This applies to a genuine oversized source
and to an implausible frame count in a damaged header. No partial WAV has been
written at this point.

An individual estimate that fits retains the existing policy: make room in LRU
order, preserve pinned files even if the aggregate remains above the cap, and
refuse when minimum disk headroom cannot be kept. The estimate is conservative
because it includes a 4 KiB header allowance. Unknown-length decoding, changes
to limits during a decode and other processes' disk use retain their existing
behavior; this is not a new global disk reservation or hard streaming quota.

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

If the completed file goes missing before size accounting, the caller receives a
cache write error. The failed read adds no use stamp, byte total or pin. A later
decode can rebuild the missing file. The warm-hit path checks its size before
updating its entry too.

`DecodeCacheHooks::afterPublish` is an empty-by-default test seam immediately
after successful publication/adoption and before bookkeeping. It runs outside
the mutex and must not throw or reenter the cache. The regression requests stop
there, then checks current bytes, saved hint when writable, zero unreturned pins,
retry and later trim. No timing-dependent race is needed.

The native regressions use generated repository FLAC fixtures and owned temporary
files, including a copy whose STREAMINFO declares four billion frames. Existing
complete decode, PCM stream and progressive decode suites cover the retained
architecture. Progressive admission already checks complete size/reservations
before eviction; its finish path already accounts publication without the final
cancellation gap. This change does not redesign that service, add cross-process
pin leases or turn the existing before/after source hashes into an atomic source
snapshot against noncooperating writers.
