# Clean with a filtered key

`adi_mix_clean_key` is an unregistered float-stereo composition of the existing
key-EQ and Clean compressor components. It adds no native function availability,
provider, device route, UI, host SDK or plug-in wrapper. The output is Programme;
the original Internal programme supplies the key. This is not a complete
compressor product or a controller/state-format implementation.

Each input span is copied once into two prepared float key planes. The key core
retains its M/S encoding, ordered Stereo/Mid/Side filters, inverse conversion,
precise double DFI histories and float section boundaries. Clean reads that
decoded filtered key through its existing explicit-key seam and applies gain to
the original programme. The inner child's External enum identifies the supplied
filtered key planes; it does not select a DAW external bus. There is no second
M/S conversion or filtered-key substitution for programme audio.

Preparation admits rates44.1..96kHz, maximum physical callback1..4096, fixed
link1/Mid, Auto controls off and Hold at or below float epsilon. The existing
legal static threshold/ratio/knee/range/unit-attack/release laws remain available.
The first optional moving path retains exactly one selected EQ lane in one
Stereo band, with immutable topology. Other styles, external devices, active
Hold, quality, lookahead, Character, audible monitors and runtime dynamics edits
are not admitted. Sampled native comparison coverage is nine selected enabled-EQ
streams; generated control corners establish authored mechanics, not a native
cross-product claim.

An all-bands-off key graph still retains the key core's defined M/S roundtrip.
It is not a true disabled bypass, and no byte identity with the older direct
Internal Clean path or a native factory default is implied.

Both children and one `2*maximumFrames` float scratch allocation are staged in
one replacement object. Any preparation failure preserves the previous complete
stream. No per-callback scratch allocation, workers, waits or timers are used.
Stopped reset retains the admitted EQ target, completes a pending ramp at that
target, clears both child histories and invalidates contexts; it is an
authored reset contract, not a new composed native reset/restore claim.

`beginBlock` accepts a typed mapped EQ target only at the original physical
callback boundary and only in its prepared lane. The fallible EQ plan runs
before opening the invariant-owned Clean callback. Refused planning leaves
both signal streams and the caller's output context unchanged. Fixed EQ accepts
no target. There is no running lifecycle action or within-block target API.

`process` accepts positive contiguous segments, with valid aligned complete
mutable planes pointing to the physical callback start. It checks context,
bounds and full-plane overlap, then scans the requested original input span for
nonfinite samples before copying audio or advancing either child. It does not
read future segments. A refused span can be corrected and retried without
changing its cursor or either history. After filtering, a second requested-span
scan protects Clean's finite-key precondition. Extreme finite programme can
overflow a float key boundary; this scan returns a latched `ProcessingFault`
before Clean writes the current programme span. The key histories may already
have advanced, so that fault is explicitly not an atomic retry refusal. An
unexpected owned-child failure also latches the fault instead of terminating
the host. Later begin/process calls refuse until stopped reset or successful
prepare; failed prepare leaves the fault latched. Reset clears histories and
snaps the retained target as described above. No input clamp, epsilon cutoff or
child DSP change hides the overflow. The additional copied-key scan is a
structural cost of this optional composition, not a CPU parity claim.

Generated tests compare arbitrary segmented rendering with the actual children
using independent whole-callback materialization. They cover legal static
controls, placements, strong allocation-failure continuation, typed-target and
context refusal, partial overlap/alignment/overflow, late nonfinite rejection,
future-span isolation, stopped reset and zero callback C++ allocation/free.
Private saved arrays and native observations remain outside public sources and
tests. CPU efficiency requires a later quiet matched-process comparison;
prepared storage and shared existing kernels are structural properties only.
