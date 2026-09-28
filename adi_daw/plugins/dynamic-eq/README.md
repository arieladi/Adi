# ADI Dynamic EQ

Working name; the director names the release. A separate AGPLv3 CLAP,
`com.adi.dynamic-eq`, implementing ADR-0195 d1/d2.

## Source and DSP

The source is [ZL Audio's ZLEqualizer](https://github.com/ZL-Audio/ZLEqualizer),
pinned to `3468a3ac85f5c1f9d16083acbee5b1339984d53b`, the same revision as
`plugins/external/zlequalizer`. CMake rejects another revision. Fetch its
submodules too. The newer `reference/ZLEqualizer` checkout and the drone's
summaries were used to locate code; numeric limits were checked in the pinned
source (`source/zlp/zlp_definitions.hpp`).

`Source/PluginProcessor.{hpp,cpp}` is directly adapted from that revision,
with its copyright/license headers retained. Changes are the class name,
editor construction and an explicit output float cast for MSVC's warning wall.
DSP, buses, state, parameter IDs, dynamic detection,
sidechain, smoothing, latency and filter structures remain ZL's. The build
compiles its `source/dsp` and `source/zlp` directly from the fetched tree.
No upstream file is patched. Nothing links this DSP into `adi_core`.

The new editor has a response graph and band nodes, with no ZL panel dials in
the main view. Double-click opens typed frequency, gain, Q and signed dynamic
range for that band; the All parameters tab retains every advanced control.
The host also receives the full original parameter list. The drawn curve is
explicitly a **static minimum-phase reference**, not a live dynamic-response
meter, spectrum analyser, or plot of the other phase structures. It stops at
Nyquist. The audio processor retains those structures.

## Graph interactions

These rules implement the ADR's table, checked against the locally held
Pro-Q 3 manual, printed pages 9, 10 and 15. No manual text or images are
included. `Gestures.hpp` defines the behavior without JUCE or DSP dependencies;
the editor calls it and translates the results into host parameter edits.

| Input | Result |
|---|---|
| Node drag | Move frequency horizontally; vertically move gain for a bell/shelf, or Q for cuts, notch and band-pass. |
| Wheel over a node or during its drag | Widen or narrow Q. |
| Ctrl/Cmd vertical drag | Adjust Q regardless of the selected shape. |
| Alt drag | Choose the horizontal or vertical axis from the first movement and keep it; Ctrl/Cmd makes the vertical axis Q. |
| Shift with drag/wheel | Apply one tenth of the usual movement. |
| Alt wheel | Adjust the signed dynamic range. |
| Ctrl/Cmd wheel | Adjust gain. |
| Alt + Ctrl/Cmd wheel | Move gain while compensating the range, keeping the dynamic endpoint fixed. |
| Alt click | Toggle band bypass. |
| Alt + Ctrl/Cmd click | Advance to the next filter shape. |
| Alt + Shift click | Advance to the next slope. |
| Node double-click | Enter exact values. |
| Node right-click | Open band actions and shape choices. |
| Curve drag | Add a band; Alt makes the new band dynamic. |

ADI sensitivity choices, not claims about another product: a wheel detent is
one quarter-octave of Q or 1 dB; Shift scales by 0.1. The graph spans 10 Hz to
30 kHz and +/-30 dB. Click/drag separation is 3 pixels. A newly drawn dynamic
band's endpoint starts at unity (range = minus the drawn gain); Alt-wheel can
then move it. ZL stores an absolute target gain, so the adapter writes
`target_gain = gain + range`, bounded by ZL's -30..30 dB limits. The combined
wheel gesture also preserves that endpoint at gain limits. Only parameters
that actually change receive host gesture boundaries; a drag shares one
boundary per changed parameter until release. Wheel edits during a drag do
not reset the other values.

## Build and verify

Use a short build path on Windows. The fetched sources can be shared read-only
with the upstream build; use a separate build directory. The CLAP bridge is
the existing pin `55525c9858d4b25687be7759a5e0f70eccef218e`.

```sh
cmake -S plugins/dynamic-eq -B build-dynamic-eq -DCMAKE_BUILD_TYPE=Release \
  -DZL_SOURCE=/path/to/pinned/ZLEqualizer
cmake --build build-dynamic-eq --target adi_dynamic_eq_CLAP \
  dynamic_eq_gestures dynamic_eq_processor dynamic_eq_clap
ctest --test-dir build-dynamic-eq --output-on-failure
```

On Windows run from an x64 Visual Studio developer shell, with `-G Ninja`.
Nothing is installed into system plug-in directories. The binary is under
`adi_dynamic_eq_artefacts/Release/CLAP/`.

Tests:

- The pure gesture suite covers every row, modifier precedence, axis locking,
  endpoint preservation, bounds and malformed deltas. It also runs as
  `adi_dynamic_eq_gesture_tests` in the normal seven-ABI headless CI.
- The processor test compiles an independent upstream processor oracle from
  the fetched pin, renaming only symbols and removing editor construction.
  It compares actual rendered outputs and measured frequency magnitudes for
  all eleven shapes at 44.1, 48 and 96 kHz. Dynamic envelopes are compared in
  Minimum, SVF and Parallel modes with internal/external sidechain. Tests
  also prove attenuation, parameter bounds, range mapping, state restore,
  GUI event routing, curve magnitudes and zero ordinary `operator new`
  allocations in warmed-up Minimum-mode processing.
- The CLAP ABI test loads the delivered binary, checks identity, all 609
  parameters, ports and lifecycle, and processes buffers offline. It never
  opens an audio device.
- Giving `dynamic_eq_processor` an absolute PNG path saves an offscreen editor
  snapshot. The processor and CLAP tests belong to this separate build;
  they are not included in the DAW's headless README count.

Measured on Windows/MSVC: static and dynamic upstream comparisons had **zero
sample difference**, the compared frequency magnitudes had **zero difference**,
and CLAP unity-path error was **zero**. Native macOS/Linux plug-in builds and
interactive side-by-side checking against the commercial plug-in remain for
review; the manual contract is covered by tests here.
