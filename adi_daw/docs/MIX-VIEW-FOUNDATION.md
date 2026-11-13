# Mix View foundation

This slice connects the existing StackModel to actual device chains and the visible STACK
surface (ADR-0236/0237). Each row follows the project's track order and each chain follows its
published device order. A device is identified by its authored ID and an opaque runtime-backed
publication stamp; a renamed display label is never its identity.

## Device rows and edits

StackModel owns geometry, hit testing, hover, focus, selection, search, pins, lane scrolling,
navigator and capture retention. The surface realizes the visible margin and retained edit
targets. Value-only parameter publications preserve the existing components. Empty tracks show
an empty chain; actual-device rows allocate no reserved processing slots.

Expanded tiles reuse DevicePanelView and the document's parameter publication. Folded tiles show
the name and activator. Parameters use the existing ParamOps/ProjectDocument gesture and undo
authority. Native plug-in windows use the existing owned editor host. Rename, clipboard, sample
assignment and activation use the same device commands as Device View.

A retained edit keeps its target and publication alive. Removal, replacement, rejection, hide,
tab change and teardown end its capture. Both the view and the document validate the expected
target before editing. A stale callback must neither write a replacement instance nor fall
through to a track-level Delete/Rename command. Search, pins and row-size settings currently
last for the window lifetime; serializing the full model view state is a later UI slice.

## Shared track analysis

The document owns SharedAnalysis. Visible consumers hold scoped leases keyed by track, tap point,
FFT size, window and channel basis. Compatible consumers read the same immutable result and
perform one transform. Different configurations share their underlying tap attachment without
sharing incompatible results. The worker analyses only new samples and waits indefinitely with
zero demand; the final consumer releases the audio tap.

The first presentation is a calibrated Main spectrum, with stereo peak/RMS, in the Mix View's
OVERVIEW and boxed Info area. Hover information temporarily replaces the boxed analyzer. These
components have no worker or timer of their own. Hidden, empty, clipped-out and minimized views
release demand; a detached visible Mix View keeps its own demand independently of the main
window. Document shutdown revokes the service before Session is destroyed.

Both presentations currently select Main only: PostFader, Mid, Hann, 1024 samples. Track selection,
reference comparison and the rest of the combined analyzer modes remain later slices.

This is an adapter over the existing newest-window track ScopeTap API. MX-30 still owns device
taps, gapless consumer cursors, negotiated history and safe storage retirement. An old-rate tap
is unavailable after a format change rather than being displayed with a false frequency scale.
DEV-05/MX-24 own compensated device-specific key routes. These are separate from visual demand.

## Processing integration

Actual devices continue through the established Session/Graph chain and shared parameter
authority. The next processing slice supplies the opt-in native function cores from ADR-0237:
EQ with unmasking, compressor, de-esser, gate, and limiter with clipping. Their movable device-area
tabs and Mix View rows must address those same nodes. ADI Still remains an ordinary plug-in for
now; no native function or EQ page is added. Separate standalone applications remain a guide
question. Analyzers stay outside the processing chain.

### Native EQ serial provider

The existing EQ function uses the shared serial processing adapter with EQ enabled and the
downstream stages disabled. All 730 parameter IDs, saved-state values and common edit/undo paths
remain the same. The adapter retains the authored EQ learning object across stopped reprepare.
An absent external key keeps the existing programme fallback; a connected silent key remains
silent. This compatibility path does not create a device sidechain route.

Scalar edits run without configuration-owner jobs. Prepared-plan changes use one demand-driven
owner, a complete aggregate latency offer and physical-callback admission. Main relays published
metadata; audio never starts or notifies the worker. Carve and Focus pages remain unavailable
until their real controller and device-key routes are connected.

A parked configuration failure retains the last applied audio plan. Its nonfatal issue is copied
through the common device publication and shown in the header with recovery guidance. This does
not mark the device unloaded, disable controls or end a gesture. Ordinary pending work has no
failure indication; the empty-status presentation is unchanged.

## Unregistered compressor component

`mix::compressor::Core` supplies Clean compression at the input rate with zero latency. It is
a framework-free component, not an available native Compressor function. The factory, channel
catalog and existing processing paths are unchanged. Other styles, quality conversion,
lookahead, hold, key EQ, audition and wet/dry mixing are not accepted component settings.

Validated settings and complete pointer/context/layout checks precede each contiguous segment.
Threshold, ratio, knee, range, attack/release and automatic controls retain running envelope
history. Prepare/reset start a new stream; settings-only restore does not rewind it. Key
source, link amount and M/S mode are fixed for that prepared stream; unsupported combinations
are rejected explicitly. A temporarily absent external key means silence on the same route
(`keyChannels=0`); partial/null planes are rejected. This is not a fabricated key source or
arbitrary live route migration.

The processing rate is rounded to float. Double I/O uses the same float engine and widens its
output; it is not a double-precision engine. Finite audio within float range is a caller
precondition, avoiding a redundant whole-buffer scan. Curve storage is prepared off audio,
coefficients use a bounded per-instance cache, invariant control work is retained, and a fixed
fully linked stream evaluates its identical detector envelope once. These are structural work
properties, not a measured CPU or whole-product parity claim. The component remains unregistered
until the complete controller/state/style/quality and real key-route paths are ready.

## Build and verification

JUCE is pinned to 9.0.5 / d60b7a78030661574a615b586d9325ba7a7fa7dd. CMake accepts an
`ADI_THIRD_PARTY` cache path for isolated worktree builds; the default remains `third_party`.
Dependency builds have explicit binary directories, including when sources are outside the tree.

The headless adapter, document and shared-analysis suites cover chain order, target generations,
undo/redo, stale callbacks and demand lifetime. `adi_ui_shell_tests --mixview-only` and
`adi_ui_shared_spectrum_tests` use synthetic software peers with null native handles. They do not
exercise a real desktop, presented frame timing, native plug-in editors or audio-device deadlines.
Visible behavior and native hosting remain to be verified by Claude's separate live testing session. CPU
comparisons remain a separate, coordinated measurement gate; no savings percentage is inferred
from source or component tests.
