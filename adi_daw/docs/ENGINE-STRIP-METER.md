# Per-strip meter and consumed peak (N9, N48)

`Session::strips().stripFor(track)->meter()` exposes a `const StripMeter&` at a
stable strip address. `peak` and `rms` remain independent relaxed atomic scalar
readouts for the current callback, accumulated over its processing segments.
Values are linear post-fader amplitudes; RMS averages all channel samples.

The frame's consuming reader calls `meter.takePeak()` to obtain the largest
peak published since its previous call and atomically clear that hold to zero.
An unread transient survives later quieter callbacks, silence, mute, device stop
and graph preparation. A newly constructed meter starts empty. Taking the hold
leaves the callback peak/RMS unchanged. Consume once and fan that value out to
widgets that share the strip; separate consumers share one destructive readout.
The window owns the numeric Peak Level display, peak line, dB conversion,
ballistics and any visible reset/hold behavior. No timing placement is added to
the engine. Input monitoring levels remain the separate MX-09 work.

Each processing segment contributes its own peak to the hold. The cumulative
callback peak cannot be used for that contribution: if the frame consumed an
early transient between processing parts, a later quiet part would otherwise
publish the already consumed transient again. Both native audio widths use the
same bounded readout rules: NaN contributes zero, infinity or a wider magnitude
is bounded to float's largest finite value; actual audio is unchanged.

There is one render producer per strip. Readers only exchange the private held
value with positive zero. The producer uses a relaxed load and at most two
strong compare/exchange calls: if its first comparison fails, the reader has
cleared the value to zero; further clears leave zero unchanged, so the second
strong comparison can publish the new peak. A value covered by the loaded
maximum linearizes at that load. There is no callback retry loop, allocation,
reference-count operation, mutex, I/O or logging. `atomic<float>` is required to
be always lock-free as for the original readouts.

`adi_strip_meter_tests` covers both widths, callback and processing-part
boundaries, consumption between parts, quiet/muted/stopped strips, preparation,
bad values and recovery, independent strips, and a concurrent render/reader
stress. Existing scalar peak/RMS assertions remain. Compiler, fault and sanitizer
coverage is recorded with the lane's gate; a selected-unit sanitizer run does not
instrument the remainder of the engine or its dependencies.
