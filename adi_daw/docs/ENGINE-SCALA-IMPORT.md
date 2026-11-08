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
  its spacing and bytes are retained. After it, blank/comment lines are skipped.
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
  one-based last physical line consumed at a parse error, zero for metadata or
  whole-input failures. A successful result has line zero.

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
