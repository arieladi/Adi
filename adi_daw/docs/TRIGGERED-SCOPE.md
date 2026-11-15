# Continuous triggered-scope component

`dsp::TriggeredScope` captures one selected, contiguous finite float lane. It is
a numerical component, not the existing latest-window Scope mode, a source
subscription or a shipped analyzer UI. Nothing is linked into the audio graph.
Its two prepared arrays hold ordered extrema and inclusive source-sample ranges;
columns are acquisition units independent of viewport width.

Four trigger laws are available: Free restarts on the sample after all columns
fill; Rising/Falling use inclusive current and strict previous threshold tests;
Internal advances a sample-based phase. Edge holdoff increments before its test
and saturates safely. Free/Internal ignore it. Triggering retains the preceding
capture before ingesting the trigger sample. Strict extrema preserve the order
of their last update. The acquisition counter starts at1, emits at most one
column per sample and subtracts1 once, including at zoom-in speeds.

The optional normalized helper supplies the original public source equations:
gain `10^(6*A-3)`, level `2*L-1`, columns/sample `10^(1.5-5*T)`, edge holdoff
`int(10^(4*H))`, internal cycles/sample `10^(2.5*S-5)`. Settings defaults are ADI
component conveniences, not an imported factory preset. Internal speed is per
sample, so its Hz depends on the sample rate. The DC detector always advances;
its display use is optional. The source's1e-10 DC-state floor is retained. Gain
and [-1,1] clipping precede triggering/extrema; original input is never changed.

`current()` and `previous()` are borrowed views of only valid columns, expiring
on the next mutation. Previous may be partial. A progressive renderer can draw
the previous trace first and overwrite its prefix with current, reproducing
the original live-array presentation without an O(columns) trigger clear/copy.
A synchronized renderer uses previous alone. Invalid remaining tails mean zero
only in that projection, not actual captured silence. No pixels/artwork or
cross-thread mutable arrays are copied from the original editor.

Preparation accepts Fs8..384kHz,1..8192 columns, finite threshold[-1,1],
gain[.001,1000], columns/sample[.0001,32], internal cycles/sample[0,1] and
holdoff1..10000. Invalid settings/allocation failures preserve the entire old
stream. Changed controls require owner-stopped preparation. Processing has one
serialized owner and no allocation/free, pow/log, locks, atomics or virtual
per-sample call. Separate precise arithmetic prevents a fast caller from changing
the DC/phase/counter associations; the owner retains its ambient FP environment.

Checked processing validates all input before mutation. Trusted processing skips
that scan for an already-finite producer. Nonempty spans require nonnegative,
contiguous source sequence numbers with a representable one-past end. A gap is a
refusal, never silently joined. `reset()` invalidates both captures, starts a new
epoch and resets numerical state without allocation; an adapter calls it at a
source/gap change. This intentionally replaces the legacy presentation's stale
arrays surviving reset. Freeze and fresh Resume remain presentation/lease policy,
outside this component. There is no acquisition on a sparse UI-frame window.

The acquisition mechanics derive from the GPLv3 s(M)exoscope source by Bram de
Jong, modernized by Matthijs Hollemans, pinned at
`caa10448df6c923bb5bc0f099a4bad4bc705c09d` in
[hollance/sMexoscope](https://github.com/hollance/sMexoscope).
See `third_party_notices/smexoscope/NOTICE.md` and `LICENSE.txt` for attribution
and license. No proprietary reference observation belongs in these sources.

Generated tests and original-source numerical comparison do not establish live
interaction, complete analyzer equivalence or a CPU percentage. Continuous shared
tap acquisition and immutable full/Info publication require a later integration.
