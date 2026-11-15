# Focus FIR scheduling and rate contract

The FIR kernel retains its complete support and `3h/2` reported and produced
latency, where `h` is 8192, 16384 or 32768 according to the prepared device
rate. Two uniform partitions of `p=h/2` samples use real transforms of size
`2p=h`. Captured block `i`, starting at `a=i*p`, is processed during the next
`p` actual samples and accumulated at `a+2p`. Its spectrum sum is
`X_i*K0(generation_i) + X_(i-1)*K1(generation_(i-1))`.

The prepared frequency/Q generation and extraction-enable mask are frozen at
the first actual source sample of each capture. Later admitted coefficients
apply at the next capture start. This replaces the previous end-of-whole-hop
selection, which could apply a later kernel to earlier captured samples. It
does not establish another product's automation behavior. Current scalar
envelope, bypass, solo, position, wet and gain controls remain live; legitimate
older convolution tails retain their own generations.

Five preallocated coefficient banks cover current capture, executing capture,
previous spectrum, an independently admitted active plan, and an immutable
owner-built offer. Reader leases follow source generations through their last
second-partition products. Owner-only dormant clearing releases these leases
and clears histories before reentry. Processing never builds a kernel, waits
for a worker, allocates a bank, or drains a complete transform at once.

Every FFT pack/recombination element, permutation scan, butterfly,
normalization/unpack element, complex product, complex sum, overlap add and
cursor transition is charged. For `L=log2(p)` and at most ten enabled nodes,
arithmetic costs at most `[372+22L]*p+84` units per job. The conservative cursor
and metadata allowance is 1280 units per job. Per-actual-sample quotas
637/659/681 respectively cover all three prepared sizes and meet the next
output deadline. First/last capture metadata is reserved from that sample's
quota. Zero-frame calls grant no work, configured capacity grants no work,
and unused quotas cannot accumulate into later bursts. These are operation
bounds, not CPU timings or equal-duration operation estimates.

Execution uses bounded contiguous chunks up to the remaining quota. Butterfly
row/stage cursors retain midpoint and stride rather than dividing each element;
spectral chunks hoist source-mask/kernel selection and retain the per-bin
current-product, previous-product, sum order. Overlap chunks stop at ring-wrap
boundaries. Chunking preserves the scalar primitive charges and arithmetic.

`firWorkUnits` is specifically the staged convolution counter. Separately,
`firMaintenanceUnits` charges every actual source-capture store, band read/
clear and dry-delay read/store: exactly 88 movement units per FIR frame
(`8+8*kNodes`). Those loops have fixed node/channel bounds, independent of h.
Ordinary sample-domain scalar/envelope arithmetic is outside both movement
and staged-convolution metrics; no whole-kernel copy/clear is hidden there.

The kernel frequency mapping, FIR attack/release coefficients and phase
minimum-bin mapping use the actual prepared sample rate. Existing 44.1 kHz
kernel equations, complete support and envelope precision boundaries remain;
the shared per-sample RMS law and unrelated IIR behavior are unchanged.
Partition summation can change floating-point roundoff, so complete-output
comparisons remain required. Corrected other-rate behavior is intentional.

Phase Match/Rotation transforms remain synchronous at their existing frame
boundaries and retain their latency. `phaseWorkUnits` reports those operations
separately. The FIR scheduler quota does not bound the complete callback with
phase active, nor the ordinary per-sample envelope/control work. CPU and live
acceptance remain separate from these headless numerical/operation checks.

`filterHops` retains its original `h`-sample source interval. Distinct FIR job
counters describe the new `p` execution blocks. All diagnostics are audio-owner
reads, not atomic cross-thread UI snapshots.
