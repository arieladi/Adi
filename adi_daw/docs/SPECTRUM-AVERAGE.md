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
