# Live's look — the first window against Live 11

The director's ruling of 27 October: the first window must look like Live, plus what the
Master Reference adds, and it is approved from pictures before any new window is built. This
file records where the look's numbers come from, and what still differs.

## The captures

Live 11.2.7 Suite on the director's Mac, default theme, 100 % zoom, 1440 × 900 points
(2880 × 1800 pixels). The scene is Live's default set: 1 MIDI, 2 MIDI, 3 Audio, 4 Audio,
returns A and B, Main. 4 Audio is selected, with EQ Eight and Auto Filter on it. Screenshots
are kept outside the repository, because the pictures are Ableton's. The test
`adi_ui_shell_tests` renders the same scene as ADI (`liveParity`, golden `live-parity.png`).
With `ADI_UI_PARITY=<folder>` it writes a 2x render to lay beside the capture.

## The measures (points at 100 %)

| Part | Live 11 | Where in ADI |
|---|---|---|
| Frame | #707070, 8 pt between panels and at the window's edges | `look::frame`, `look::gap` |
| Panel edges | 4 pt, rounded; #505050, the browser's #444444 | `look::border`, root paint, `BrowserPanel::paint` |
| Control bar | 35 pt including the gap; fields 17 pt tall at y 9. The middle group's left edge is 297 pt left of centre, the right group starts 215 pt from the right | `TransportBar::resized` |
| Browser | 8–316. Search row 21 pt, sidebar 113 pt, rows 19 pt, a 2 pt divider, list header #808080 19 pt, preview row 21 pt; round buttons outside the edge | `BrowserPanel` |
| Arrangement | From 324. Overview 16 pt with a 2 pt black outline, running to the H and W buttons. Beat ruler 29 pt with the numbers at its foot. Locator band 24 pt, dark gap 7 pt, lanes from 80 pt. Time ruler 18 pt | `ArrangementView` constants |
| Lanes | #858585, every 16 grid steps #8f8f8f; the selected track #9f9f9f/#aaaaaa; grid a quarter darker than its lane; 2 pt #505050 between tracks; default track height 88 pt | `paintLanes`, `laneHeight` |
| Track header | 2 pt rule, name column 93 pt in the track's colour, fold circle, bold name | `TrackHeaderList`, `kHeaderWidth = 95` |
| In/Out | 75 pt column; choosers #dcdcdc 13 pt tall on a 17 pt pitch; In/Auto/Off | `MixerStrip::paintInOut`, `kInOutWidth = 78` |
| Mixer column | 116 pt with the meter. Activator 45 pt #f4b43e, S and Arm 20 pt #b7b7b7. Volume 35 pt with #7ddbf3 fill (0 dB at 83 %), pan 45 pt. Meter 8 pt #191919 | `MixerStrip::resized`, `kControlsWidth` |
| Right-hand strip | 27 pt #505050; view switches at its head; I-O, R, M, D at its foot | `MixerPanel::paint`, `kRightStrip` |
| Detail row | 196 pt including edges. Info View 200 pt with a 17 pt #aaaaaa title bar; Device View from 216 | `deviceFloor = 196`, `kInfoWidth` |
| Device | 2 pt #aaaaaa edge, 17 pt title bar, orange activator, fold circle. Knob columns 58 × 54 pt; 11 pt between devices with two dark marks | `DevicePanelView`, `ParameterControl` |
| Knob | Dark 2.5 pt ring open at the foot, blue from the start to the value, a needle stopping short of the ring | `LiveLook::drawRotarySlider` |
| Status Bar | 38 pt; 22 pt fields with 3 pt #505050 edges. Message field 35 to w−384, a second field 206 pt, then the track box | `StatusBar` |

## What ADI adds (Master Reference)

- ADI-Q and ADI-L, the channel slots, on the mixer row where Live puts the sends (ADR-0204 d1).
- The AI tab at the foot of the browser's panes (Master Reference 3.1).
- Pd Devices in the sidebar where Live has Max for Live (Master Reference 9.3).
- Configure on a device's title bar, for the DAW-drawn panel (ADR-0150).

## What still differs, and why

| # | Live shows | ADI shows | Waits on |
|---|---|---|---|
| 1 | The browser's list (folders, devices) | The list's header only | The browser's content model (LANES §3b item 2) |
| 2 | Return tracks A and B and Main, last, with Post and the cue volume | Nothing below the tracks | The snapshot's track kind and returns (N6, N11) |
| 3 | MIDI tracks: All Ins, All Channels, No Output; no volume or pan; a dot meter | Every track drawn as an audio track | The track's kind (N6) |
| 4 | The sends' -inf boxes | ADI-Q and ADI-L in that row | Sends (N11); ADR-0204 d1 |
| 5 | In/Out choosers with the track's own routing | ADI's fixed routing: Ext. In, 1/2, Auto, Master | The routing in the snapshot (N11) |
| 6 | The loop brace in the locator band, the playhead's marker | None | The transport's loop |
| 7 | EQ Eight's and Auto Filter's own displays | The DAW-drawn knob panel | ADR-0150: ADI's devices are drawn from their parameters |
| 8 | Live's own typeface | The platform's sans-serif, sized to Live's widths | A font choice (no licence for Live's) |
| 9 | Bipolar knobs filled from the middle (Gain at 0 dB) | Filled from the start | A parameter's bipolarity in the panel record |
