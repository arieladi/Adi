# Linear-magnitude spectrum average primitive

`dsp::LinearMagnitudeAverage` is an owner-thread numerical component, not a new
analyzer mode or source subscription. Raw Spectrum remains the product default.
There is no broker/UI, FFT, audio, worker/timer, LUFS or reference-calibration change.

Prepare accepts 1..32769 bins, finite positive sample rate and decay time, and
1..65536 hop samples. Its intermediate windows-per-second, duration and reciprocal
must remain finite and positive. Zero/negative time is rejected, not interpreted
as bypass. No factory time is implicit. Invalid configuration or allocation failure
preserves the complete previous configuration and values.

Coefficient association is explicit binary64: compute `Fs/H`, then `T*(Fs/H)`,
then its reciprocal; `a = 1 - pow(0.1, reciprocal)`. T describes a 20 dB magnitude
decay, not a conventional one-pole tau. Each real analysis hop updates binary64
state with separately rounded subtraction, multiplication and addition:
`delta = magnitude - old; adjustment = delta*a; next = old + adjustment`.
Neither coefficient zero nor one substitutes a different update expression.
The numeric translation unit disables contraction/LTO and is out of line even
when the caller uses fast arithmetic. No per-process floating-point environment
changes are made. Float inputs widen once per bin; double inputs stay double.
The owner supplies a consistent ambient rounding/denormal environment. Exact
results are scoped to that environment; very long decays can enter subnormal
ranges and their timing is platform-dependent. There is no silence floor or
long-run denormal CPU guarantee in this numerical contract.

`prepare` begins a zero-valued numerical epoch, reusing storage if its size matches.
`setDecay20dBSeconds` retains all histories, and `reset` zeros values without
changing configuration, coefficient or storage. They do not touch another service.
Processing/reset use no heap, pow/log, mutex, atomic or source access.

`processChecked` preflights input shape, finite nonnegative bins and finite
representable next states before any mutation. It refuses malformed data rather
than silently sanitizing it. `processTrustedFinite` skips that scan for an internal
producer which has already established those preconditions; prepared/null/length
guards remain. Signed zero is accepted. No floor, denormal clamp or implicit gain
calibration is applied. Borrowed magnitude spans expire on the next non-const call
or destruction. Preparation/control mutation and processing must not be concurrent.

A future adapter must call the primitive once per real FFT hop, before dB/floor
conversion; UI frames and output republication are not hops. Compatible consumers
should share one state behind the existing transform; different averaging times
need distinct histories but not duplicate FFTs. Source/format/gap epochs and
presentation-local Freeze/Reset remain separate integration contracts. This
component makes no CPU percentage, native numerical parity or listening claim.

## Optional shared-domain profile

`SharedAnalysis::Average1s` requests an optional fixed one-second 20 dB magnitude
decay. Raw remains the default; this broker API does not add a visible UI mode or
preferences. Compatible Raw, Average1s and Spectrogram requests share their existing
source, cursor and FFT path. Average-only demand avoids projecting unused Raw dB
bins. First average demand starts zero and waits for a new completed hop containing
post-activation samples; queued old windows and output republication do not count.
Same-source gaps/overruns retain that physical first-demand fence while resetting
numerical values. Average-only waiting performs no unused FFT for old windows.
Another already-live average consumer shares its current immutable publication.
Real zero-magnitude hops still advance the decay when their FFT call is omitted.

`Lease::readAverage()` returns an optional immutable sidecar with its exact Raw
window, status, epoch, processed-hop count and shared dB-bin payload. Window and
sidecar publish together; independent read calls can still cross publications.
Like `read()`, a combined-output lease refuses a sidecar whose window lacks any
newly requested field until the complete output union is published.
Unchanged numeric data reuse the bin payload during metadata/history republication.
The existing Raw Result layout is unchanged, including its allocation path when
Average1s is absent. Last average demand releases owned state and sidecar while
externally retained snapshots remain valid. Compatible Raw/Spectrogram demand
needs no duplicate transform; Average-only demand activates the existing transform
path plus demand-driven averaging/projection. There is no new worker, timer or
audio processing.

Source/format activation prepares the numerical state on the owner. Gaps/overruns
reset only its numerical epoch. Finite time samples can overflow the float FFT;
the checked entry rejects those magnitudes and publishes `InvalidInput` without an
average curve, leaving Raw/Spectrogram and their source histories unchanged. The
first rejected hop resets the average once; an invalid run retains zero state,
and the next valid completed hop recovers from zero. There is no silent sanitation
of FFT magnitudes, added floor or floating-point environment change.

Preparation failure leaves average unavailable and preserves other active outputs.
An unavailable average-only domain does no worker/FFT retries; a later owner source
activation or renewed demand can retry preparation. A missing/refused sidecar is
null; a mixed Raw/average publication can explicitly carry `Unavailable`. Failure
to allocate an optional publication never suppresses the Raw publication; a later
genuine publication event can retry. Compatible consumers' local Freeze/Clear must
retain the existing presentation-only law and never reset another consumer's state.
