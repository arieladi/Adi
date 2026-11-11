# Scala scale input

`engine::parseScala(bytes, tuningId, displayName)` parses already-read `.scl`
bytes on the owner into an owned `TuningDefinition`. It performs no file/SQL
I/O, identity reservation or project mutation. The caller supplies a positive
explicit project identity and display name, then reviews or compiles the
result with `compileScaleContext`. In an integration with native tuning edits,
pass this definition to `tunings::definitionPayload` for `tuning.create` or
`tuning.replace`; submit the key assignment in the same journal transaction.
The parser's code depends only on the immutable context API, independently of
that writer branch. Native op payloads capture resolved double cent values,
so log replay never reparses a file or recalculates a logarithm.

Accepted grammar and limits:

- Optional UTF-8 BOM, LF/CRLF/CR lines, and an optional final newline.
- A line whose first non-space character is `!` is a comment. The first
  non-comment physical line is the description, including an empty line;
  its spacing and bytes are retained. After it, blank/comment lines are skipped,
  except for the reference-pitch safety fence below.
- The next content line begins with a positive unsigned decimal interval count,
  1..65,536. Numeric tokens end at ASCII whitespace or `!`; trailing annotations
  on count/interval lines are ignored. No degree names are inferred from them.
- Exactly that many interval content lines follow. A token containing `.` is
  finite decimal cents (a leading plus and decimal exponent are supported).
  Otherwise it is a positive integer ratio `a/b`, or integer ratio `a/1`.
  Ratio operands are decimal integers at most 2^53-1, retained exactly in double.
  A decimal ratio or scientific token without a decimal point is refused.
- Intervals strictly rise above implicit unison. The last becomes period_cents;
  stored degrees are unison zero and intervals 1..n-1. Non-octave periods are
  retained. Ratios convert once to double cents with log1p for near-unison
  precision; no ratio string or duplicate pitch truth is persisted (ADR-0178).
- Extra content after the declared intervals refuses. Up to 8 MiB input and
  65,536 bytes per physical line/name; embedded NUL and oversized skipped
  comments also refuse. No partial definition escapes a failure. `line` is the
  one-based physical source line for an error (the directive itself for a
  reference refusal), zero for pre-source identity/whole-input failures. A
  successful result has line zero.

Description/name bytes are owned verbatim; no filename, encoding conversion,
preset label, keyboard mapping, reference frequency or absolute tonic is
inferred. An unnamed degree remains unnamed and the context can show its
numeric coordinate. Parsed notes are not automatically retuned. Callback use
refuses before copying or allocating.

These fixtures validate the accepted grammar, ratio/cents precision, non-octave
periods, budgets, malformed input and the real native MIDI-key/cent-offset note
handoff. The official Scala format page returned proxy 403 on this Linux seat;
this is no claim of full Scala/Live importer parity. UI/file chooser wiring,
KBM, all external-file quirks and measured Live comparison remain integration
or review work. A refused file never silently becomes a different scale.

## Temporary reference-pitch safety fence (M0 / TU-01)

The current relative definition has no stored absolute reference or keyboard
map. Active post-pitch `! @ABL REFERENCE_PITCH octave degree hertz` is therefore
checked before publishing a definition. Octave is -2..8, degree is a zero-based
index into the actual authored cents, and hertz is positive and finite. Numeric
arguments may be quoted; missing, extra, invalid, duplicate or prematurely
placed active directives refuse with their physical line. An extra leading
`!` disables a directive. Unrelated descriptive comments keep SCL semantics.

For an octave period, the reference must agree with the existing 12-TET C
anchor (Live C-2 = MIDI0) plus that degree's authored cents. The comparison
allows 0.0001 cent for numeric printing roundoff; it never retunes a definition.
An offset requiring a stored reference is refused. Synthetic fixtures cover
31-EDO's degree23 at440Hz and degree0 at263.0921Hz: neither can silently become
a C-anchored definition. Already aligned references, including degree18 of
24-EDO at440Hz, retain their original relative coordinates. Explicit reference
mapping on a non-octave period also refuses until the reference/map store exists;
plain relative tritave input remains supported.

This fence is not full ASCL/KBM import, default keyboard-index mapping, source
byte preservation or measured Live parity. No absolute reference is guessed for
a plain relative SCL source, and accepted metadata is not persisted as a second
pitch truth. ADR-0227's TU-02 replaces these temporary refusals with stored
reference/source fidelity; its full acceptance and tuning6/Live readings remain
M6 work. Input/count/line budgets retain their existing order; callback use
still refuses before parsing or copying source material.
