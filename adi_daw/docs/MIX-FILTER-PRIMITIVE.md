# Shared matched-section primitive

`src/adi/mix/filters` is a framework-neutral preparation and sample-state component.
It is not registered as a native Mix View function and does not change the running
EQ, compressor, limiter or clipper engines.

`MatchRequest` supplies descending analog section polynomials, their angular
frequency scale, three positive distinct match points and a high-frequency
reference. The ordinary three-point response-matching law and its positive
constant-gain branch produce one normalized second-order section. Zero-response
and other singular branches remain unsupported. Shape-specific prototypes,
anchor selection, filter order, quality conversion, channel routing and control
cadence remain the caller's responsibility; this component does not replace those
graphs with a generic equalizer.

`design` is a preparation operation. `Designer` caches an exactly unchanged
request and retains the last accepted generation when a new request is refused.
Nonfinite inputs, nonpositive frequencies and out-of-domain match points are invalid. Duplicate points,
singular response/solver branches and nonfinite or non-strictly-stable results
are unsupported. Both cases leave the previous output unchanged. Additional
solver branches require their own explicit implementation and sound gates.

Each `DfiSection` owns one channel's four double Direct Form I histories and
publishes float after each section. Coefficient publication preserves those
histories; reset clears them. A cascade must retain that float boundary and its
ordered sections. Sample and span loops live in the component implementation,
compiled with precise arithmetic and contraction disabled as applicable; they
do not inherit a consumer's fast-math flags through inline sample arithmetic.
The span loop permits one call per section/segment rather than requiring one
external call for every sample.

Preparation, coefficient publication, reset and processing allocate no storage.
There are no workers, locks, atomics, subscription work or per-sample coefficient
redesign. Finite sample input and serialized ownership belong to the caller.
This is a structural resource contract, not a CPU benchmark or whole-product
performance claim.

The standalone CMake target builds `adi_mix_filters_tests` from generated signals.
Its checks cover analytic impulse response, selected response magnitudes,
configuration refusal, unchanged-request caching, recursive continuation,
reset, split processing, sample/span equivalence and callback allocation freedom.
Native-reference fixtures and measurement tools are not part of this source tree.
