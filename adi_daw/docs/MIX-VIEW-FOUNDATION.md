# Mix View foundation

This slice connects the existing StackModel to actual device chains and the visible STACK
surface (ADR-0236/0237). Each row follows the project's track order and each chain follows its
published device order. A device is identified by its authored ID and an opaque runtime-backed
publication stamp; a renamed display label is never its identity.

## Device rows and edits

StackModel owns geometry, hit testing, hover, focus, selection, search, pins, lane scrolling,
navigator and capture retention. The surface realizes the visible margin and retained edit
targets. Value-only parameter publications preserve the existing components. The default
**With tools** list contains channels with an actual native Mix View function, keeping their
ordinary inserts in real chain order. Membership uses native identity, never a display label,
bypass or loading status. **All channels** also shows empty/ordinary-only chains, making the
first-function add path discoverable. Neither mode creates processors or reserved slots.
An explicit channel shortcut switches the visible selector to **All channels** when only tool
membership excludes that valid target, then lands on its chain. It does not clear search, pins,
folds or project scope, and eligible/invalid targets do not change the selector.

All track records and ancestry remain available for scope, search and key roles. Search changes
the displayed list, not audio targets or settings. A hidden nonmember ancestor's STACK fold
does not suppress eligible descendants; returning to All channels restores its saved fold.
A still-live captured item retains its row through a presentation filter until release.

Expanded tiles reuse DevicePanelView and the document's parameter publication. Folded tiles show
the name and activator. Parameters use the existing ParamOps/ProjectDocument gesture and undo
authority. Native plug-in windows use the existing owned editor host. Rename, clipboard, sample
assignment and activation use the same device commands as Device View.

A retained edit keeps its target and publication alive. Removal, replacement, rejection, hide,
tab change and teardown end its capture. Both the view and the document validate the expected
target before editing. A stale callback must neither write a replacement instance nor fall
through to a track-level Delete/Rename command. Search, pins and row-size settings currently
last for the window lifetime; serializing the full model view state is a later UI slice.
Search, channel-scope, row-size and toolbar-button focus own their keyboard handling: a stale
device focus must not make Cut/Delete/Duplicate fall through to another device or track.
The toolbar wraps for constrained widths without changing the row geometry law.

## Focus inside a channel row

Row enlargement and tool enlargement are separate presentation states. The inspected tool
uses the row's available height while neighboring tools stay compact. Each tool's width,
tier, painting and hit rectangle follow its own height. Hover changes presentation only;
explicit keyboard focus retains priority until actual pointer movement and a fresh dwell.
Layout-generated motion cannot hand a stationary pointer to another tool.

A bounded transition set uses the existing frame clock and keeps the inspected horizontal
position anchored while neighboring tools shrink. Lane arrows and wheel navigation continue
from the displayed offset. There is no added DSP, analyzer demand or per-tool timer.

Drag, text and menu capture retain the last completed displayed pose, including intermediate
animation geometry. Painting, canvas hits and child bounds use that same pose. Stable target
validation remains separate from geometry; a stale visible item cannot edit a replacement.
Presentation callbacks may synchronously replace or destroy a surface, so model walks finish
before invoking controls and a nested complete presentation supersedes an interrupted one.
Release resumes from the retained pose through the common gesture and undo authority.

## Shared track analysis

### Optional right dock

The Mix View can show a right analyzer dock beside STACK or OVERVIEW while retaining the
separate analyzer tab. It starts hidden with Main selected. Meters, Phase and Spectrum use
the existing views and broker; compatible source, tap and channel settings share one domain,
cursor and spectral transform. Only the meter view reads the existing weak Main loudness
projection. Selected follows the current channel; a missing selection does not fall back to Main.

Visibility, requested width and source settings are per-window view preferences, not audio
parameters or project undo entries. Controls wrap within the compact column. Insufficient
space hides the dock and releases its demand while preserving the requested preference.
Hidden, frozen, suspended and closed presentations reuse the existing demand-release paths.
The existing analyzer service and clock remain shared; audio algorithms and analyzer numerical
kernels are unchanged.

Requested dock/header layout changes wait while a STACK drag, text editor or menu owns its
capture. The applied STACK rectangle and header geometry stay fixed through the edit; the
latest request applies after release. Whole-surface hide, tab changes and minimization retain
their existing immediate cleanup. Failed preference saves restore controls without discarding
a frozen picture or changing its analysis demand. Analyzer controls own their edit-command
context, preventing destructive keys from reaching an unrelated channel or device; ordinary
project Undo retains its existing authority.

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

The full analyzer selects Main or Selected, tap point and channel basis, with Spectrum, Scope,
Meters, Phase and Spectrogram modes. Compact Info follows that presentation. Compatible requests
retain the same Hann/1024 domain; Reference/Compare remains a separate implementation slice.

Spectrogram offers the original ADI palette plus 46 public scientific palette families,
preview strips and Reverse. Source pins, full licenses and the RGB8 conversion summary are in
`third_party_notices/analysis_palettes/`; no other application's lookup table is claimed.
Parent palette/reverse preferences persist per window. Compact Info follows them, while a new
floating analyzer initially copies the parent and can make its own lifetime-local choice.

Palette changes recolor cached intensity/gap indices, including a visible frozen picture,
without changing its measurement or analysis demand. The original default's 256-color lookup
and intensity indexing are retained. Owner frames prepare the raster; painting reads that
image without regenerating palette or raster data. Discarding a measurement also discards its
raster so recreated-domain counters cannot alias old pixels. Embedded and floating analyzer
control focus protects the common app edit-command context. A source-time freshness barrier
for resume while another consumer keeps the domain alive remains a separate follow-up.

The document supplies MX-30 leased source/device endpoints with independent consumer cursors,
negotiated history, source generations and safe storage retirement. Unusable old-rate taps are
unavailable rather than displayed with a false frequency scale. DEV-05/MX-24 own compensated
device-specific key routes. These are separate from visual demand.

## Processing integration

Actual devices continue through the established Session/Graph chain and shared parameter
authority. Native EQ and independently enabled Clip-to-Limiter processing are wired through
that same chain; device-area pages and Mix View rows address the same nodes. Unmasking,
compressor, de-esser and gate native admission remain later slices, not substitute processing.
ADI Still remains an ordinary plug-in. Analyzers stay outside the processing chain.

## Main meter readouts

Meters mode can show the existing Main programme measurement: momentary, short-term and
integrated LUFS, plus the held aggregate true peak in dBTP. This is a read-only projection of
Session's persistent Main service before cue/metronome, not another meter, transform, tap or
worker. Its fields have independent measurement intervals; they are not a sample-synchronous
snapshot. Channel-basis selection still controls the analysis plot, while this strip remains
explicitly labelled as the aggregate Main programme measurement.

Only visible, unfrozen actual Main/Post Mixer views read those values. Selected non-Main,
device-input/output and other tap points cannot display Main loudness under another source's
name. The default Spectrum presentation is unchanged. Info, full and floating views share
the same presentation, with weak document-owner bindings revoked before teardown. Prepared
versus audio-adopted format identities suppress stale readings during a format change.
An unavailable/unwarmed field is shown as `--`, without claiming that every negative-infinity
reading is an unwarmed window. Freeze and Reset affect the local picture only; they never
pause or reset the shared Main measurement. Closing the last view adds no meter DSP work.

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
