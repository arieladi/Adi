# Scale context

On the Store owner thread, call `engine::readClipScaleContext(store, clipId)`
after editing or navigating history. It resolves N32's inherited scale at the
clip's arrangement start, or its N33-compatible local 12-TET override. It
reads no note blobs and makes no project or history changes. A caller-owned
read transaction remains open; otherwise the reader owns one coherent read.
Missing references, invalid degrees and unsupported inherited linear/ramp
placement return an error, without silently substituting another scale.

`compileScaleContext(scale, tuning, error)` also accepts owner-supplied values.
An implicit scale uses twelve relative mask bits and root pitch-class names.
An explicit tuning uses ADR-0178's degree membership and ignores that legacy
mask. Degree zero must be unison, degree IDs and cent offsets strictly rise,
and all offsets lie below the positive finite period. Sparse degree IDs,
non-octave periods, author names, description and `edo`/`scala` provenance
remain exact. Empty membership remains empty. Definitions have at most 65,536
degrees. This API supplies no preset list, importer or new write operation.

Hold the immutable context for the lifetime of its reader or graph generation.
Its spans and string views live as long as that context. Owner edits cannot
change a held context. Compilation, database reads and `grid` are owner work;
they reject callback use. Copy/retire contexts on the owner too, so releasing
the final shared owner never frees a graph's data on the audio thread.

`includesDegree`, `nearest`, `match` and `contains` perform bounded, allocation
free queries with no SQL, locks or I/O. Pitch queries require the absolute
cent pitch of degree zero (`tonicCents`). The root is a 12-TET pitch class,
not an invented octave anchor for an arbitrary tuning. `match` finds a literal
tuning pitch within an explicit cent tolerance; `contains` also tests scale
membership. `nearest(..., scaleOnly=true)` searches scale members and breaks
ties toward the lower pitch. None of these retunes stored notes.

`grid(low, high, tonic, scaleOnly, maxRows)` projects an inclusive ruler or
Fold list. Rows retain degree IDs, period indices and author names, falling
back to numeric degree IDs for unnamed explicit tuning degrees. The default
budget is 8,192 rows, maximum 65,536. A budget/range failure returns no partial
rows. Pitch and tonic inputs are limited to absolute values at most 1e9 cents;
repeat indices are limited to +/-2^50 to bound numeric conversions.

Rows optionally offer the existing ANOT MIDI key plus float cent offset.
`encodeMidiPitch` reports the actual float error and refuses errors beyond the
caller limit (default 0.0001 cents), invalid inputs and pitches outside
[-1200, 13900] cents. The double tuning definition is still authoritative;
no arbitrary-double native storage promise is implied. Editor/device wiring,
Scala/KBM import, reference frequency, tuning bypass and Retune Set On Loading
are separate work. Hardware, UI and measured Live parity are unverified here.
