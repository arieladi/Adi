# Settings catalogue

Every setting the Settings Reference (v0.4) describes, generated from its
source so the two cannot drift, for the settings registry (`src/adi/settings/`,
ADR-0152) to fill from. Our own columns only: the setting, ADI's answer and its
status. The Live-manual column and the screenshots stay in the Word document
(`reference/DOCS/WORD/`, git-ignored). Status words: DECIDED (with the ADR),
DIRECTION (proposed), BACKLOG, WISH, REJECTED, NOTE.

Regenerate with `python tools/export_settings_catalogue.py` when the Word
document changes (it needs the git-ignored `reference/` tree); never edit
this file by hand.


## Part I — The Settings window itself / 4. What ADI does not need a setting for

| Setting | ADI | Status |
|---|---|---|
| Auto-save interval, number of auto-save files, project backups (Cubase §68a p.1608, REAPER §22.3.1) | Every op is committed as it happens (ADR-0003); the file is SQLite in WAL mode (ADR-0010). There is no unsaved state to lose and nothing to schedule. A *snapshot* command (a named copy) remains a File-menu action, not a timer. | DECIDED ADR-0003 |
| Maximum undo steps, maximum undo memory, save undo history with project, allow load of undo history (Cubase p.1609; REAPER §22.2.1, §22.13) | The history IS the project (ADR-0030); it is not held in memory and it does not have a step limit. Compaction is an explicit action (SPEC §8.3), not a setting. | DECIDED ADR-0030 |
| Store multiple redo paths (REAPER §22.13) | Branches are the model (ADR-0068): every undo-then-edit keeps the abandoned branch, and ADR-0111 opens a historical state in a silent tab. Always on. | DECIDED ADR-0068 |
| Reduce CPU use of silent tracks; do not process muted tracks (REAPER §22.6.1, §22.6.4); Suspend VST3 plug-in processing when no audio (Cubase p.1624) | Signal-driven suspension is the engine's default (ADR-0043), measured (BENCHMARKS.md): 315 sleeping tracks cost less than 8 awake ones. The opt-out is per device (`always_process`), because the reason to opt out is one plug-in that lies about its tail, not a global mood. | DECIDED ADR-0043 |
| Plug-in bridging, crash isolation, "Terminate immediately if a plug-in corrupts the heap" (REAPER §22.10.1); "Plugin crashed — reload" (Bitwig §16.3) | Sandboxing was rejected for latency and complexity (ADR-0047); a crash loses one gesture, never the session (ADR-0001). No setting, by design. The trade is stated in the Master Reference and stays visible. | REJECTED ADR-0047 |
| Anticipative FX processing, media buffering, advanced disk I/O (REAPER §22.6.2) | The engine is tuned for large blocks with sub-block accuracy (ADR-0042, ADR-0102); the worker pool (ADR-0102 d4) is not started because nothing needs it yet. When it is, it is a number of threads, not a page. | DECIDED ADR-0102 |
| Deactivate Punch In on Stop, MIDI Max. Feedback, Run Setup on Create New Project (Cubase) | Small behaviours that are decided by the parity checklists of the feature they belong to (ADR-0108), not exposed as switches. | NOTE |

## Part II — Live 12's ten tabs, setting by setting / 1. Display & Input

| Setting | ADI | Status |
|---|---|---|
| Language | Same; Hebrew from day one. JUCE shapes text with HarfBuzz and resolves direction with SheenBidi, both embedded since JUCE 8. RTL only inside text containers: track names, settings, labels, remarks. The timeline, playhead, waveforms and mixer never mirror. | DECIDED ADR-0125 R-08 |
| Zoom Display | Per window, not per display: every native window (arrangement, mixer, piano roll, each detached panel) keeps its own zoom, changed with Ctrl/Cmd + +/- or its window menu. Plugin windows are never scaled by us: they get the monitor DPI and their own menu. | DECIDED ADR-0129 d3-d4 |
| Outline View in Focus | Same; also the ADR-0112 view indicator | DECIDED parity |
| Scroll bars | Always / on hover | DECIDED parity |
| Follow behavior | Page / scroll / off, per view (Arrangement, editors); Bitwig calls it *Playhead follow mode* | DECIDED parity |
| Show user interface labels | Same | DECIDED parity |
| Use Tab Key to Move Focus | Same | DECIDED parity |
| Wrap Tab Navigation | Same | DECIDED parity |
| Move Clips with Arrow Keys | Same | DECIDED parity |
| Pen Tablet Mode | Same; REAPER adds *pen/tablet safe mode* that does not reposition the cursor when adjusting knobs (§22.8.6) — folded into this one switch | DECIDED parity |
| Permanent Scrub Areas | Same | DECIDED parity |
| Restore "Don't Show Again" warnings | Same | DECIDED parity |
| Zoom on Selection | ADI zooms around the cursor by default (ADR-0129). This switch restores Live's selection anchor for strict muscle memory. The overview edge handles and the `+`/`-` anchor are unaffected. | DECIDED ADR-0145 d2 |
| Knob / slider / value-box modes | Cubase's three choosers (§68a p.1598): value box *text on click* vs *drag*; knob *circular* / *relative circular* / *linear*; slider *jump* / *touch* / *ramp* / *relative*. Default is Live's behaviour; the choosers exist for Cubase hands. | DECIDED ADR-0125 R-09 |
| Mouse wheel changes parameters | Cubase's three-way switch, default *disable in scrollable areas*. | DIRECTION |
| Tooltips | Both: the Info View, docked and instant; floating tooltips after a 600 ms delay (500 to 800 settable), so a mouse sweep does not strobe. An object carrying a remark shows a pip, and the Info View splits to show the remark. | DECIDED ADR-0131 |

## Part II — Live 12's ten tabs, setting by setting / 2. Theme & Colors

| Setting | ADI | Status |
|---|---|---|
| Theme | Same, with *follow OS* the default. Themes are files (Cubase's User Color Scheme, REAPER's theme files) so a user can share one. | DECIDED parity |
| Palette tone | Same | DECIDED parity |
| High contrast | Same; Bitwig has a *Contrast* setting (§0.2.2.6) — a slider, adopted over a toggle | DECIDED parity |
| Grid line intensity | Same (Cubase: *Grid Overlay Intensity*, p.1604) | DECIDED parity |
| Brightness, color intensity, hue | Same | DECIDED parity |
| Auto-assign track colors | Cubase's five modes (p.1621): default colour / previous track / previous +1 / last applied / random. Live's on/off maps to *previous +1* / *default*. | DIRECTION |
| Clip color | Same | DECIDED parity |
| Accent colour | Cubase p.1618: one colour that highlights specific elements. Adopted — it is how a theme says "this is the selection". | DIRECTION |
| Track-type default colours | Cubase p.1619: a colour per track kind. Adopted for the kinds the schema has (audio, midi, instrument, group, return, master, vca). | DIRECTION |
| Waveform display | Bitwig: *Perceptual scale* for timeline waveforms (§0.2.2.6); Cubase: waveform brightness, outline intensity, background modulation (p.1604). Adopted: perceptual on/off, brightness. | DIRECTION |
| Event volume curve and fades display | Cubase p.1605: show volume curves *always / on mouse over*, fades likewise, the volume control *always / never / on hover*. Adopted with ADR-0115 (event volume curves). | DECIDED ADR-0115 |
| Meter appearance | Cubase Metering (p.1614): peak hold time, fallback rate, colour handles per scale; REAPER §22.7.8: update rate, decay, min/max dB, sticky clip indicators, pre-fader option. One *Meters* group on this page. | DECIDED ADR-0125 R-10 |

## Part II — Live 12's ten tabs, setting by setting / 3. Audio

| Setting | ADI | Status |
|---|---|---|
| Driver Type | Windows: WASAPI shared, WASAPI exclusive and **ASIO**; macOS CoreAudio; Linux ALSA or PipeWire/JACK (phase 3). ASIO is built (ADR-0137) and goes **only through JUCE's wrapper**: the raw ASIO bypass is rejected (ADR-0145 d5). The page notes that WASAPI shared grants its own period (measured 2026-09-23). | DECIDED ADR-0145 d5 |
| Audio Input Device / Audio Output Device | Same; *Use System Device* on every platform where the OS has a default. | DECIDED parity |
| Channel Configuration (Input Config / Output Config) | Same dialogs. Names are per device (§17.2). Bitwig adds a *Role* per output path — speakers / headphones / output (§0.2.2.2) — adopted, because the cue bus needs to know which output is the headphones. | DECIDED ADR-0125 R-11 |
| In/Out Sample Rate | Same. A project whose rate differs from the device raises a non-blocking bar: "Project is 48 kHz, hardware is 44.1 kHz", with **Switch Hardware** and **Resample Temporarily**, and a *Don't ask me again* checkbox that makes the choice silent. The project rate stays in the file. | DECIDED ADR-0145 d3 |
| Resample silently when rates differ | The setting the checkbox above turns on; switched off here, the bar comes back. *App* scope. | DECIDED ADR-0145 d3 |
| Default SR & Pitch Conversion | Same | DECIDED parity |
| Buffer Size | **64, 128, 256, 512, 1024, 2048, 4096**, chosen by hand, and the page shows *requested* and *granted* side by side (ADR-0049: the granted size is the only one that exists). Adi's ruling: players feel the step to 128 and nobody feels 64 against it, so 32 is not offered. No automatic switching (no ASIO-Guard). A driver that grants less than 64 still runs. The engine rebuilds without reloading a plug-in when it changes (ADR-0122 d6). | DECIDED ADR-0145 d5 |
| Input Latency / Output Latency / Overall Latency | Same three numbers, in samples and ms, plus the compensation headroom (ADR-0088) as a fourth | DECIDED parity |
| Driver Error Compensation | Same; the built-in loopback measurement (Live's tutorial) becomes a button: *Measure round trip* | DIRECTION |
| Test Tone, Tone Volume, Tone Frequency, CPU Usage Simulator | Same; the CPU simulator maps to the benchmark's synthetic load (BENCHMARKS.md) | DECIDED parity |
| Virtual audio device | ADI's own device: install / status / endpoint names ("ADI DAW Stream Output/Input", ADR-0119); signed via SignPath (ADR-0118). Windows first; a BlackHole-derived device on macOS. | DECIDED ADR-0118 |
| Loopback input | The master or any bus as a recordable input (ADR-0106). A chooser here, a track input everywhere else. | DECIDED ADR-0106 |
| Close device when the app is inactive | REAPER §22.6: close when stopped and inactive; when inactive and armed; when stopped and active. One chooser: *release the device when ADI is in the background: never / when stopped / always*. Exclusive-mode drivers make this necessary, not optional. | DIRECTION |
| Warn when a device cannot be opened | REAPER §22.6. Always warn; the setting is whether to fall back to the system device silently. | DIRECTION |
| Auto-bypass plug-ins whose PDC exceeds a threshold when armed | REAPER §22.6, Cubase *Delay Compensation Threshold (for Recording)* p.1623. Adopted as one threshold in ms: plug-ins above it are bypassed on armed tracks while recording, and the mix is otherwise fully compensated (ADR-0058). | DECIDED ADR-0125 R-12 |
| Linux audio backend and JACK transport | Phase 3: a backend dropdown, ALSA or PipeWire/JACK, and a *JACK transport sync* switch. No kernel, period or routing matrices on this page. | DECIDED ADR-0145 d4 |
| Automatic mute at a dB limit | REAPER §22.6.1 mutes a track or the master above a limit. Adopted, default off, as the one setting between a runaway feedback patch and a hearing test. | DIRECTION |

## Part II — Live 12's ten tabs, setting by setting / 4. Link

| Setting | ADI | Status |
|---|---|---|
| Show Link Toggle | Same | DECIDED parity |
| Start Stop Sync | Same | DECIDED parity |
| Link Audio: Audio | Link: yes, a public SDK. Link Audio: a wish, kept for zero-configuration compatibility with Live and iOS if a licence appears. **Open LAN audio is our own node**, built from SonoBus (GPLv3): send and receive devices with a DAW-rendered panel, a background I/O thread, and jitter buffer plus codec frame declared as latency so compensation aligns the stream. | DECIDED ADR-0126 |
| Link Audio: Name, Latency, Sync to Incoming Audio, peers list | For Link Audio, if it is ever licensed. The LAN node has its own panel instead: peer or group, jitter buffer, codec and bitrate, meters, connection state; the remote clock is resampled to ours. | WISH |
| PTP | ADR-0107: PTP as a shared timebase and a measurement across machines running AudioGridder servers (ADR-0053), with a whitelist of the grandmasters allowed. Lives on the Sync page (Part IV) beside Link. | DECIDED ADR-0145 d7 |

## Part II — Live 12's ten tabs, setting by setting / 5. Tempo & MIDI

| Setting | ADI | Status |
|---|---|---|
| Show Tempo Follower Toggle | Tempo Follower is a real-time tempo estimator on an audio input. WISH — a P2 feature, the setting arrives with it. | WISH |
| Tempo Follower: Input Channel (Ext. In) | With the feature | WISH |
| Resync External Hardware: Show Resync Button | With external sync | BACKLOG P2 |
| Control Surface 1-6: Control Surface / Input / Output / Dump | Same table; the script model is Bitwig's (controller extensions, §0.2.2.3) with auto-add when a known device appears. Push support is Ableton's and not assumed. | DECIDED ADR-0125 R-14 |
| Takeover Mode | Same three, global, overridable per controller | DECIDED parity |
| Master Focus Dial | One encoder bound here (port, channel, CC, encoder mode) controls the native control under the mouse, or the plugin parameter last touched: the gesture-begin that VST3 and CLAP already send (ADR-0124). A Note/CC push locks it; Shift divides the step by 10; a HUD shows [Device] → [Parameter] → [value], cyan following, amber locked. Every turn settles into one undoable op. | DECIDED ADR-0130 |
| MIDI Ports: Track | Same | DECIDED parity |
| MIDI Ports: Sync | Same | DECIDED parity |
| MIDI Ports: Remote | Same | DECIDED parity |
| MIDI Ports: MPE | Same. The expression route per plugin is remembered in an application capabilities registry, overridable in the device header (Auto / VST3 Expression / MIDI-MPE / Poly-AT), and the route in effect is stored on the device row so the project plays the same elsewhere. | DECIDED ADR-0134 d7 |
| Per port: MIDI Clock Sync Delay | Same; Bitwig offers an offset per output path too (§0.2.2.4) — both directions | DECIDED parity |
| Per port: Sync Type | Same | DECIDED parity |
| Per port: MTC Frame Rate, MTC Start Offset | Same | DECIDED parity |
| MIDI-CI support | Cubase p.1609: automatic detection and setup of MIDI-CI devices. MIDI 2.0 property exchange; worth having when the OS stacks expose it. | WISH |
| Chase events | Cubase p.1610: when locating, replay the controllers, program changes and pitch bend that would have been in effect. Not optional for correct playback; a chooser of which event types, default all. | DECIDED ADR-0125 R-15 |
| MIDI Thru, Reset on Stop, Insert Reset Events after Record | Cubase p.1610. Reset on stop and the recorded reset event are adopted as defaults; MIDI Thru is a track monitor setting, not global (Live §17.1). | DIRECTION |
| MIDI Filter (record / thru / channels / controllers) | Cubase p.1613. Adopted as a small page: message types and channels never recorded or never echoed. | DIRECTION |
| MIDI file import/export options | Cubase p.1611: export type 0/1, resolution, markers, locator range; import first patch/volume, controllers as automation, dissolve type 0, destination. Adopted in the Export dialog, not here. | BACKLOG P2 |

## Part II — Live 12's ten tabs, setting by setting / 6. File & Folder

| Setting | ADI | Status |
|---|---|---|
| Create Analysis Files | Analysis (peaks, transients, tempo guess) is stored in the ADI cache, never beside the user's samples. No setting: always cached, cache location below. | DECIDED ADR-0125 R-16 |
| Sample Editor | Same, plus *open a copy* vs *open in place* (REAPER §22.9 prompts to confirm the filename) | DECIDED parity |
| Temporary Folder | ADI has no unsaved Set: a new project is a file from its first op (ADR-0003). The equivalent is the *default projects folder*, below. | DECIDED ADR-0003 |
| Max Application | No Max for Live. The visual patching device is Pure Data (ADR-0035), edited in its own window inside ADI, built on plugdata (ADR-0145 d8); a *Pd externals path* takes this slot in Part IV. | REJECTED ADR-0035 |
| Decoding Cache: Minimum Free Space, Maximum Cache Size, Cache Folder, Cleanup | Same four controls; one cache for decoded audio, analysis and waveform peaks (REAPER §22.9.2 stores peaks in an alternate path when the media folder is read-only — ADI always uses the cache). | DECIDED parity |
| Default projects folder, default render folder, default record folder | REAPER §22.2.5: separate default paths for projects, renders, recordings. Adopted; recordings default to the project's own media folder (SPEC §5). | DIRECTION |
| Save project file references with relative paths | Always relative. **Media is never embedded in the .adi** (only plugin chunks, wavetables and preset blobs). **Collect and Export** copies every referenced file into the project audio/ folder, checks each BLAKE3 hash, and writes the .adi and audio/ into one ZIP that opens anywhere. A hash mismatch stops the export and names the file. | DECIDED ADR-0127 |
| Copy imported media into the project folder | REAPER §22.9.4 does it on import. Adopted as a three-way: *always / ask / never*, default *ask*, same wording as Live's Collect Files on Export. | DIRECTION |
| Open last project on startup; start-up template | Cubase p.1609 and Bitwig §0.2.2.1: *Open on start*: nothing / last project / a template. Adopted here. | DIRECTION |

## Part II — Live 12's ten tabs, setting by setting / 7. Library

| Setting | ADI | Status |
|---|---|---|
| Installation folder for Packs | ADI has no Packs store. Content is folders. The slot becomes *Content folders*: the list the browser indexes (ADR-0104, app-scoped browser). | DECIDED ADR-0104 |
| User Library location | Same: one User Library, one path | DECIDED parity |
| Collect Files on Export | Same three, same wording | DECIDED parity |
| Show Downloadable Packs | No store; nothing to hide | REJECTED no store |
| Show Splice, Show Cloud, Show Push | No third-party subscription services in the browser (ADR-0104: the browser is app-scoped and local). A *global sample library* folder set (from the V0.2 review) is the ADI answer to "one place for my sounds". | REJECTED ADR-0104 |
| Splice download folder | Not applicable | REJECTED |
| Browser scan behaviour | Cubase MediaBay p.1613: scan only when open; scan unknown file types; max items in results. Adopted: *index content folders in the background* on/off, and the maximum results count. | DIRECTION |
| Preview settings | Same place, in the browser | DECIDED parity |
| Export and import library metadata | Tags, BPMs and ratings to a JSON file keyed by each file's BLAKE3 hash, so they reach the right sample on a machine where the path differs. Paths stay local. A file is hashed once and again only if its size or time changed; an external drive is recognised by its volume, not its letter; names are compared in one Unicode form; new files are hashed in the background. | DECIDED ADR-0145 d11 |

## Part II — Live 12's ten tabs, setting by setting / 8. Plug-Ins

| Setting | ADI | Status |
|---|---|---|
| Rescan Plug-Ins | Same, including the crash-on-scan quarantine. The scan runs out of process so a crashing plug-in cannot take ADI down while scanning — the one place ADR-0047's no-sandbox rule does not apply, because nothing is playing. | DECIDED ADR-0125 R-17 |
| Use Audio Units v2 / v3 | No Audio Units (ADR-0041): an `au` row opens as a placeholder (ADR-0011). Not offered. | REJECTED ADR-0041 |
| Use VST2 Plug-In System Folders / Custom Folder + Browse | No VST2 (ADR-0015, ADR-0041). Placeholder rows only. | REJECTED ADR-0041 |
| Use VST3 Plug-In System Folders / Custom Folder + Browse | VST3: the format's default locations (JUCE's list) plus a list of custom folders, not one. Aliases inside a folder are followed (§23.4.1). | DECIDED ADR-0041 |
| CLAP folders | CLAP: the format's convention plus `CLAP_PATH` plus custom folders (ClapHost::defaultSearchPaths). On Linux the same list for LV2. REAPER §22.10.4 has the same page. | DECIDED ADR-0075, ADR-0145 d4 |
| Prefer CLAP over VST3 when both are installed | Bitwig §0.2.2.6: *All plug-ins* or *Preferred formats* with "Prefer CLAP over VST", "Prefer VST3 over VST2", "Prefer 64-bit". Adopted with CLAP preferred by default: CLAP's model is the one the device contract follows (ADR-0052 d5), and a browser that shows Surge XT twice is a browser with a bug. | DECIDED ADR-0125 R-18 |
| Plug-in blocks in the device view | **Exactly as Live** (ADR-0150): a plug-in with 64 or fewer modifiable parameters shows them all as sliders; one with more opens with an empty panel and the Configure hint. Serum 2 and Pro-Q 3 both have hundreds, so both open empty. Parameters join the panel through Configure mode (click one in the plug-in window), as temporary entries in the automation and X-Y choosers, when automation is recorded, and in MIDI, key or macro mapping mode. Per instance, saved in the project. ADI adds a **Parameter List** button: a searchable list of every declared parameter. Compact blocks are withdrawn. | DECIDED ADR-0150 d1-d3 |
| Auto-Open Plug-In Windows | Same | DECIDED parity |
| Multiple Plug-In Windows | Same | DECIDED parity |
| Auto-Hide Plug-In Windows | Same; REAPER §22.10 adds *only one FX chain window at a time* and *open the FX window on track selection change* — folded into this chooser | DECIDED parity |
| Plug-in editors always on top | Cubase p.1624, REAPER §22.10. Adopted, default on. Third-party GUIs float (ADR-0076, two-tier UI). | DECIDED ADR-0076 |
| Plug-in window scaling | HiDPI plug-in windows scale with the display (Cubase *Application Scaling*, REAPER HiDPI mode). One per-plug-in override: *scale 100 / 150 / 200 %*, for the plug-in that draws wrong. | DIRECTION |
| Warn before removing a modified effect | Cubase p.1624. Not needed: removal is an op and undo restores the parameters and the state (ADR-0038, ADR-0110). | REJECTED ADR-0003 |
| Create a MIDI track when loading an instrument | Cubase p.1624. Not applicable: ADI's instrument is a device on a MIDI track (Live's model). | REJECTED model |
| VST parameter automation notifications | REAPER §22.10.2: ignore when the window is closed / not from the UI thread / all / process all. ADI: parameter broadcasts become ops per ADR-0110 and are always processed; a per-plug-in *ignore broadcasts* override exists for the plug-in that floods. | DECIDED ADR-0110 |
| AudioGridder servers | The list of remote hosts (ADR-0053, `remote_hosts`), found by mDNS/Bonjour: address, status, latency. Part IV. | DECIDED ADR-0145 d6 |
| Blocklist | A visible list of disabled plug-ins with *re-enable*; the scan quarantine writes to it. | DIRECTION |

## Part II — Live 12's ten tabs, setting by setting / 9. Record, Warp & Launch

| Setting | ADI | Status |
|---|---|---|
| File Type | WAV, promoted in place to RF64 if a take crosses 4 GiB (a JUNK chunk reserved for the ds64 header, as EBU Tech 3306 describes): under 4 GiB every tool reads it, over it nothing is lost. `bext` (BWF) timecode and iXML are options on any recording. | DECIDED ADR-0132 d1-d2 |
| Bit Depth | 32-bit float for every recording. Export: 16 and 24-bit PCM, 32 and 64-bit float. | DECIDED ADR-0132 d1, d5 |
| Count-In | Same place as Live 12: the metronome menu. Bitwig keeps *Pre-Roll* in Settings (§0.2.2.6); Live's placement wins. | DECIDED parity |
| Exclusive Arm, Exclusive Solo | Same two switches | DECIDED parity |
| Clip Update Rate | Same | DECIDED parity |
| Record Session automation in | Session View returns last (ADR-0101); the switch arrives with it. | BACKLOG ADR-0101 |
| Start Playback with Record | Same, Shift included | DECIDED parity |
| Loop/Warp Short Samples | Same | DECIDED parity |
| Auto-Warp Long Samples | **Off by default**, a director-approved deviation: a dropped file plays raw. Pressing Warp runs Live's Auto-Warp on demand (tempo detected, 1.1.1 on the first downbeat, fitted to the grid), then Bitwig's choices (detect tempo changes or fixed; first beat or sample start); with no clear beat, an inline BPM field. | DECIDED ADR-0132 d6-d7 |
| Default Warp Mode | Same set; ADI's stretch is its own (ADR-0061), the mode names follow Live | DECIDED ADR-0061 |
| Create Fades on Clip Edges | **Off by default**, a director-approved deviation. Stated beside the switch: a clip that starts or ends away from a zero crossing clicks, which the 4 ms edge fade prevents. | DECIDED ADR-0132 d6 |
| Default Launch Mode | With Session View (ADR-0101) | BACKLOG ADR-0101 |
| Default Launch Quantization | With Session View | BACKLOG ADR-0101 |
| Select on Launch | With Session View | BACKLOG ADR-0101 |
| Select Next Scene on Launch | With Session View | BACKLOG ADR-0101 |
| Start Recording on Scene Launch | With Session View | BACKLOG ADR-0101 |
| Save Current Set as Default / Clear | Same: *Save current project as the default*; templates are projects (SPEC) | DECIDED parity |
| Record quantization default | A default here (1/16), inherited by new tracks, and a **Rec-Q toggle on every MIDI track header** ([1/16] or [Free]). The quantize is its own undo step, as in Live (§19.5), so undoing it keeps the take. | DECIDED ADR-0132 d8 |
| Auto-arm on select | Bitwig §0.2.2.6: which track types arm when selected; Cubase p.1600 has it per kind. Adopted, default *instrument and MIDI tracks*. | DIRECTION |
| Retrospective capture | MIDI: always listening, one Capture command, tempo and loop length guessed when stopped (Live). Audio: a RAM ring per armed or monitored input, **30 s by default** (Cubase *Audio Pre-Record Seconds*), 11.5 MB per stereo input at 48 kHz. | DECIDED ADR-0132 d9 |
| Keep Monitoring Latency in Recorded Audio | Per track, same default; the global default lives here | DECIDED parity |
| Auto monitoring mode | Cubase p.1623: manual / while record-enabled / while record running / tapemachine. Live's per-track model is kept; *tapemachine* is added as a fourth track option because it is what a tracking engineer expects. | DIRECTION |
| Loop recording: takes, discard incomplete | REAPER §22.6.7: discard incomplete first/last takes; threshold for a complete take. Adopted with take lanes (comping is P0, FEATURES §3). | DIRECTION |

## Part II — Live 12's ten tabs, setting by setting / 10. Licenses & Updates

| Setting | ADI | Status |
|---|---|---|
| Authorization | ADI is GPLv3 (ADR-0015). No licence, no authorisation, no serial. The page shows the licences ADI ships under: GPLv3, JUCE under AGPLv3 for the combined work (ADR-0048), and the third-party notices. | DECIDED ADR-0015 |
| Automatic updates | Check on launch (on/off), with release notes; the download is manual. No auto-install. | DIRECTION |
| Usage data | Off by default and opt-in, with the exact list of what is sent shown before the switch. Cubase's *usage logger* writes a local file for support (p.1608) — adopted as *write a diagnostic log*, local only. | DECIDED ADR-0125 R-20 |
| Crash reports | Opt-in; the report is a file the user can open before sending. | DIRECTION |
| Beta / early access | Bitwig §0.2.2.1: *tell me about Early Access releases*. Adopted as a channel chooser: stable / beta. | DIRECTION |

## Part III — What Cubase, Bitwig and REAPER have that Live does not / 1. Cubase 15 Preferences

| Setting | ADI | Status |
|---|---|---|
| Editing (p.1595): Auto Select Events under Cursor; Delete Overlaps; Parts Get Track Names; Lock Event Attributes (position / length / other); Track Selection Follows Event Selection; Automation Follows Events; Drag Delay | The behaviours the arrangement parity checklist must reproduce (ADR-0108, ADR-0115). Exposed as switches on an *Editing* page, defaults set to Live's behaviour where Live has one and Cubase's where it does not. *Automation Follows Events* is on by default. | DECIDED ADR-0125 R-21 |
| Editing — Audio (p.1597): treat muted events like deleted; mouse wheel for event volume and fades; on import: open options / use settings; on bounce: replace / keep; hitpoint detection; time-stretch algorithm; default warp algorithm | Wheel-for-volume-and-fades adopted with ADR-0115. Import and bounce prompts become one *ask / always / never* each. Hitpoints are always detected into the cache. | DIRECTION |
| Editing — Tool Modifiers (p.1601): every tool's modifier keys, editable | A modifier editor with three presets: *Live*, *Cubase*, *Bitwig*, chosen once on first launch and changed here. This is the mechanism behind the parity gate: the same gesture must do the same thing as the reference the user came from. | DECIDED ADR-0125 R-22 |
| Editing — Zoom (p.1602): quick zoom; selection start as anchor; zoom while locating in the ruler; horizontal-only zoom tool | Zoom anchor and zoom-in-ruler adopted (both are in Live too, §6.2, without a setting). Views are ADR-0112's. | DIRECTION |
| Editors (p.1603): default MIDI editor; double-click opens in a window or the lower zone; editor content follows event selection | ADI has one MIDI editor; *open in the lower zone or a window* is adopted (undocking is ADR-0063). Follows selection: on. | DECIDED ADR-0063 |
| Event Display (p.1603 to 1608): event names, borders, overlaps, grid overlay intensity, opacity, smallest track heights; audio waveform brightness, outlines, event volume curve and fade visibility; chord and pitch naming (English / German / Solfège, B vs H); folder track data display; MIDI part data mode; marker lines; track name font and width | Folded into Theme & Colors (Part II §2) where it is appearance, and into the editors where it is behaviour. Pitch naming with *B as H* adopted: the German and Hebrew markets both read H. | DIRECTION |
| General (p.1608): HiDPI, usage logger, language, auto save, tips, max undo, run setup on new project, open projects in last used view, open last project, author and company name | Covered in Part I §4 (no auto save, no undo limit) and Part II §6 and §10. *Default author name* becomes `project.author` for new projects — a Project-scope default. | DIRECTION |
| MIDI (p.1609 to 1613): MIDI-CI; thru; reset on stop; chase; display resolution; extend playback range of early notes; reset events after record; MIDI file import/export; MIDI filter | Part II §5. *Extend playback range of notes that start before the part* is adopted as a default (a note 5 ticks before the clip should sound), and *MIDI display resolution* is fixed by SPEC §4.2 (PPQ 5,765,760 informational) rather than chosen. | DIRECTION |
| Metering (p.1614): map input bus metering to the track; peak hold; fallback; colour handles per scale | Part II §2 (R-10). The +3 dB digital scale note is the kind of detail the parity checklist records. | DECIDED ADR-0125 R-10 |
| Record (p.1615): deactivate punch in on stop; stop after punch out; audio pre-record seconds; wave files over 4 GB; create images during record; Broadcast Wave strings; MIDI thru on record-enable; snap MIDI parts to bars; catch range; retrospective buffer; ASIO latency compensation default; add latency to MIDI thru; replace recording in editors | Part II §9 (R-19). *Snap MIDI parts to bars* on by default — Live's clips do the same. Broadcast Wave strings live in the Export dialog. | DIRECTION |
| Transport (p.1616): playback toggle triggers local preview; timecode subframes; user frame rate; stop while winding; wind speed and fast-wind factor; cursor colour; cycle on click in the ruler; locate on click in empty space; scrub volume, quality, inserts | Wind speed and *locate when clicked in empty space* adopted (Live locates on click too). Scrub settings adopted with the scrub feature. | DIRECTION |
| User Interface (p.1618 to 1622): application scaling; colour schemes; project, editor and ruler colours; track-type colours; MixConsole fader, section and strip colours; auto colour mode; selection brightness; colour strength | Part II §2. A theme file carries all of it; the page shows the ten that matter. | DIRECTION |
| VST (p.1622 to 1624): link panners; warn on real-time mixdown; default stereo panner mode; connect sends automatically; instruments follow read/write all; mute pre-send on mute; default send level; group mute mutes sources; delay compensation threshold; do not connect busses for external projects; warn on channel config change; auto monitoring; warn on overloads; create MIDI track on VSTi; warn before removing modified effects; open editor after load; sync plug-in program to track; suspend VST3 when silent; editors always on top | *Default send level* and *group mute mutes sources* adopted on a *Mixing* page (ADR-0072 rules sends; ADR-0044 groups). *Default pan law* is `mixer_strip.pan_law`, a Project default. The rest is answered in Parts I and II. | DIRECTION |
| VST — Control Room (p.1624): show volume in transport; auto-disable talkback; phones as preview channel; dim cue during talkback; exclusive device ports for monitor channels; reference level; main dim volume | Deferred to a v2 backlog (R-23). The first cycle follows Live's output model, with the headphones role of R-11 for pre-listen; interface software downstream does the rest. | BACKLOG v2, ADR-0125 d5 |
| VariAudio and Video (p.1625): inhibit shared-sample warnings; extract audio on video import | Extract audio on import: yes, always. The warnings are a one-time dialog with *don't show again*. | DIRECTION |
| Customizing (§66): setup panes, workspaces, profiles, Windows dialog, where settings are stored, Safe Mode | Workspaces are ADR-0112's views; profiles are Part I §3's presets; Safe Mode is R-06. | DECIDED ADR-0112 |
| Optimizing Audio Performance (§67): settings that affect performance; Audio Performance Monitor; ASIO-Guard | A performance panel (REAPER §22.12 too): CPU per track, real-time thread load, the worst block. ASIO-Guard is Cubase's anticipative processing; ADI's equivalent is the large-block engine (ADR-0042) and needs no guard, and the buffer is never switched automatically (ADR-0145 d5). | DECIDED ADR-0125 R-24 |

## Part III — What Cubase, Bitwig and REAPER have that Live does not / 2. Bitwig Studio Settings

| Setting | ADI | Status |
|---|---|---|
| Behavior (§0.2.2.1): audio import (original speed / stretch; detect tempo changes / assume fixed; insert from first beat / sample start); open on start; template; early access | Part II §9 and §6. The three import choices are the wording ADI uses for Live's Auto-Warp switch. | DIRECTION |
| Audio (§0.2.2.2): audio system; input and output device; sample rate; block size; named busses; Role per output (speakers / headphones / output) | Part II §3 (R-11). Named busses with roles are adopted; they are what the cue output and the virtual device endpoint names attach to. | DECIDED ADR-0125 R-11 |
| Controllers (§0.2.2.3): takeover; auto-add; controller extensions per device; generic keyboard and 8-knob presets; enable/disable; port assignment | Part II §5 (R-14). The extension model over Live's fixed surface list. | DECIDED ADR-0125 R-14 |
| Synchronization (§0.2.2.4): sync in: internal / MIDI clock (with input offset and a responsiveness slider) / Link; sync out per port: clock, start/stop, always send, SPP, MTC, per-port offset, MTC rate | A *Sync* page (Part IV) that merges Live's per-port table with Bitwig's per-port outputs and adds PTP (ADR-0107). The responsiveness slider for incoming tempo is adopted. | DIRECTION |
| Shortcuts (§0.2.2.5): every command mappable to keys and to MIDI; named mapping sets; computer keyboard as MIDI keyboard | A *Shortcuts* page. Live has none (its map is fixed, §41). Cubase (§65) and REAPER (§15) have editors with presets, macros and import/export. Adopted with presets named after the three references, so a Cubase user gets Cubase's keys on day one — the parity gate's counterpart for the keyboard. | DECIDED ADR-0125 R-25 |
| User Interface (§0.2.2.6): language; display profile (tablet, §18); scaling per display; contrast; playhead follow; perceptual waveforms | Part II §1 and §2. Display profiles: not adopted (no tablet UI planned). | DIRECTION |
| Recording (§0.2.2.6): auto-arm track types; pre-roll and metronome during it; record quantization | Part II §9. | DIRECTION |
| Locations (§0.2.2.6): my projects, my library, controller scripts, browser locations; plug-in locations; preferred formats | Part II §6, §7, §8 (R-18). | DECIDED ADR-0125 R-18 |
| Plug-ins (§16.3): hosting mode (within Bitwig / together / by plug-in / individually), per-plug-in overrides, crash handling | Rejected as a mode (ADR-0047); the page's per-plug-in list is the model for ADI's blocklist and per-plug-in overrides (scaling, ignore broadcasts, always process). | REJECTED ADR-0047 |

## Part III — What Cubase, Bitwig and REAPER have that Live does not / 3. REAPER Preferences

| Setting | ADI | Status |
|---|---|---|
| General: undo (§22.2.1); Find box (§22.2.2); tooltips (§22.2.3); export/import configuration (§22.2.4); default paths (§22.2.5); keyboard/multitouch (§22.2.6) | Find box (R-02), export/import (R-05) and default paths adopted. Undo settings: none (Part I §4). *Commit edit fields after one second of typing* adopted for the transport fields. | DIRECTION |
| Project (§22.3): template; prompt to save; open properties on new; relative paths; backups and auto-save (§22.3.1); track/send defaults (§22.3.2); item fade defaults (§22.3.3); loop defaults (§22.3.4) | Track/send defaults become a *New track* page (Project scope): fader gain, visible envelopes, height, show in mixer, main send, record config, meter mode, send default level and mode. Fades: Part II §9. Backups: none. | DECIDED ADR-0125 R-26 |
| Audio (§22.6): close device when inactive; warnings; PDC auto-bypass when armed; stop on disk failure; virtual loopback; channel naming; metronome output. Mute/Solo (§22.6.1): auto-mute at a limit; silent-track CPU; pre-fader sends obey mute; solo in place; solo in front with a dB level; solo bus | Part II §3. *Solo in front* (the rest of the mix at a low level behind the soloed track) is adopted with its dB level; *solo via a dedicated bus* is a mixing-page option. *Metronome output* chooser: yes. | DIRECTION |
| Buffering (§22.6.2), Seeking (§22.6.3), Playback (§22.6.4), Scrub/Jog (§22.6.5), Loop/Lane Recording (§22.6.7), Rendering (§22.6.8) | Buffering: none (ADR-0042). Seeking: *seek on click in ruler / empty track area / item* adopted as one chooser. Playback: *tiny fades on start and stop* is always on (ADR-0089's fade); *run FX when stopped* with a length, adopted; *stop at end of project*, adopted. Rendering: tails, freeze length, muted tracks in freeze (ADR-0059) — an *Export* page (Project scope). | DIRECTION |
| Appearance (§22.7): tooltips; peaks and loudness on hover; text rendering; toolbar scaling; track spacing; antialiasing; grid lines in lanes; filled envelopes; edit cursor highlight; guidelines; ruler/grid (§22.7.1); media items (§22.7.2); item buttons (§22.7.3); peaks/waveforms (§22.7.4); fades (§22.7.5); zoom/scroll (§22.7.6); track panels (§22.7.7); track meters (§22.7.8); envelope colours (§22.7.9) | A theme file carries the drawing; the Theme page (Part II §2) exposes intensity, brightness, waveform mode, meters. The rest is fixed by the parity checklists: how Live draws a clip is the spec, not a preference. | DIRECTION |
| Editing Behavior (§22.8): edit cursor moves on selection / paste / stop; link time selection and loop; clear loop points on ruler click; minimum selection; transient sensitivity; locked items and ripple; razor edits; dual trim; crossfades stay together; delete empty tracks; split all at cursor with no selection; envelope display (§22.8.1); automation (§22.8.2); locking (§22.8.3); automation items (§22.8.4); comping (§22.8.5); mouse (§22.8.6); MIDI editor (§22.8.7); spectral (§22.8.8) | The *Editing* page (R-21) takes: cursor-follows-selection, link loop to selection, transient sensitivity, ripple with locked items, split-at-cursor-with-no-selection. Automation: *reduce points when recording*, *mode after write* (Live: none; Cubase: none; REAPER: trim/read), *return speed* adopted. The mouse modifier tables are R-22's presets. | DIRECTION |
| Media (§22.9): offline when inactive; tail on apply FX; take FX on split; MIDI (§22.9.1): octave name offset, ticks per quarter note, multichannel import, tempo import prompts; peaks (§22.9.2); import (§22.9.4) | *MIDI octave name offset* (C3 vs C4 for middle C) adopted on the MIDI page: the one setting every keyboard player has an opinion on. Ticks per quarter note: fixed by SPEC. Import: Part II §6. | DIRECTION |
| Plug-ins (§22.10): window behaviour; positioning; undo points on close; compatibility (§22.10.1); VST (§22.10.2); ARA (§22.10.3); LV2/CLAP (§22.10.4); ReaScript; ReWire/DX; extensions (§22.10.7) | Part II §8. ARA: a P2 feature (Melodyne-class editing), the switch arrives with it. LV2: Linux phase 3 (ADR-0109). Extensions: ADI's scripting surface is the op API (ADR-0003, AI-AGENT.md); a *Scripts* page lists installed scripts and their tier. | DIRECTION |
| Project and file management (§22.11): clean current project directory; Performance meter (§22.12); Undo history (§22.13); UI tweaks (§22.14, §22.15) | Clean-up: *Manage Files* (Live §5.8 finds unused files) — a dialog, not a setting. Performance meter: R-24. Undo history window: ADR-0111. UI tweaks: HiDPI mode and modal window positioning adopted; the rest is theme. | DIRECTION |

## Part IV — The pages only ADI needs / 1. Engine

| Setting | ADI | Status |
|---|---|---|
| Block size and the granted size | Part II §3: 64 to 4096 by hand, requested and granted shown together | DECIDED ADR-0145 d5 |
| ASIO path | JUCE's ASIO wrapper, at every size. No raw bypass and no switch for one (ADR-0145 d5, replacing ADR-0134 d5's benchmark test). | DECIDED ADR-0145 d5 |
| Compensation headroom | The ring headroom the graph reserves so a latency change is a tap move and not a rebuild (ADR-0079, ADR-0088: the default is measured, 8192 samples). Advanced; shown with the measured value. | DECIDED ADR-0088 |
| Suspension | Signal-driven suspension is always on (ADR-0043). The page shows how many nodes are asleep (`GraphStats`) and links to each device's *Always process* switch. No global off. | DECIDED ADR-0043 |
| Processing threads | The worker pool of ADR-0102 d4 is not started; when it is, a count with *auto*. Until then the row reads *single audio thread, measured sufficient* with the benchmark numbers. | BACKLOG ADR-0102 |
| Denormals | Flushed for the callback (ADR-0042 d6). Not a setting. | DECIDED ADR-0042 |
| Sample-rate conversion quality | Normal / high, as Live | DECIDED parity |
| Virtual device and loopback | Part II §3 (ADR-0106, ADR-0118, ADR-0119): install state, endpoint names, driver version, and the SignPath signature status. | DECIDED ADR-0118 |
| Delay compensation threshold when recording | R-12 | DECIDED ADR-0125 R-12 |
| Performance panel | R-24: per-track cost, the audio thread's real-time load, the worst block, the number of sleeping nodes. | DECIDED ADR-0125 R-24 |

## Part IV — The pages only ADI needs / 2. Sync

| Setting | ADI | Status |
|---|---|---|
| Link | Part II §4 | DECIDED parity |
| MIDI clock and MTC, in and out, per port, with offsets | Live's per-port table (Part II §5) plus Bitwig's per-port outputs and the tempo responsiveness slider (Part III §2) | DIRECTION |
| LAN audio node | ADR-0126: defaults for new LAN nodes (jitter buffer, codec, bitrate) and the peer list; each node's own settings live in the device. | DECIDED ADR-0126 |
| PTP | ADR-0107: a shared timebase with AudioGridder servers, with each server's offset and jitter shown. A whitelist of network adapters, PTP domains and **grandmasters by IP and MAC address**; announcements from anything else, a smart TV on the same LAN, are ignored. On Windows an in-process user-space client ships natively. | DECIDED ADR-0145 d7 |
| AudioGridder servers | ADR-0053: the list of remote hosts, found by mDNS/Bonjour, with status and measured round trip. Each server's plugin list is cached **per application**, so the browser works offline; a project keeps only the plugins it uses. Without the server a device loads locally, and failing that as a placeholder keeping its state; **Remap Remote Host** moves every device on one server to another. AudioGridder is MIT. | DECIDED ADR-0145 d6 |

## Part IV — The pages only ADI needs / 3. AI agent

| Setting | ADI | Status |
|---|---|---|
| Tier | Observe / Propose / Apply (AI-AGENT.md). Default Observe. Changing the tier is itself logged. At Propose the agent queues a changeset, parameter ops included, and nothing reaches the graph until the user presses **Apply**; applied, it is one undo step. | DECIDED ADR-0145 d9 |
| Rate cap | Ops per minute the agent may commit at Apply tier (AI-AGENT §6). | DECIDED AI-AGENT |
| Endpoint | Which model service, where (local RPC or remote), with the key stored in the OS keychain, never in the settings file. | DIRECTION |
| What the agent may read and change | Reads the project (through ops and the projection, ADR-0021) and application settings; never the file system. Changes UI and workflow settings only, through its own logged pipeline (Part I §6). | DECIDED ADR-0125 R-07 |
| Stem separation and other async services | ADR-0064: async AI jobs; a chooser for local vs remote execution and a cache folder for results. | DECIDED ADR-0064 |

## Part IV — The pages only ADI needs / 4. Windows and views

| Setting | ADI | Status |
|---|---|---|
| View states | ADR-0112: Main (group focus), Macro (global), Micro (detailed); the default view on open and the follow mode per view. | DECIDED ADR-0112 |
| Undocking | ADR-0063: which panels may float; remember per display. | DECIDED ADR-0063 |
| Session View window | ADR-0101, ADR-0117: docked Ableton-style, detachable, MixConsole toggle (F3). Its defaults arrive with it. | BACKLOG ADR-0101 |
| Project tabs | ADR-0068: several projects open. An inactive tab is offline (graph and plugins unloaded, model and history kept) and rebuilt on return. | DECIDED ADR-0134 d1 |
| History | ADR-0128: a snapshot tree of named milestones and branches; revert never discards (what follows becomes a branch); open any node read-only in a silent tab; File › Take Snapshot, default name [Project] [YYYY-MM-DD HH:MM]. | DECIDED ADR-0128 |
| Pd device analyser view | ADR-0116: the floating second view of a Pd device; default open/closed. | DECIDED ADR-0116 |

## Part IV — The pages only ADI needs / 5. Music

| Setting | ADI | Status |
|---|---|---|
| Tuning | ADR-0103: the default tuning table for new projects (12-TET, or a maqam or other table from `tuning_systems`); Live 12 loads tuning systems per Set (§15) and ADI stores them as rows. | DECIDED ADR-0103 |
| Pitch naming | English / German / Solfège, B vs H, octave offset for middle C (Cubase p.1605, REAPER §22.9.1). | DIRECTION |
| Default time signature, tempo, PPQ display | Project defaults for new projects; PPQ is informational (SPEC §4.2). | DIRECTION |
| Buses | ADR-0113: 64 buses, no cables. Not a setting; the page says so where a user would look for it. | DECIDED ADR-0113 |

## Part IV — The pages only ADI needs / 6. Devices and DSP

| Setting | ADI | Status |
|---|---|---|
| Pure Data | ADR-0035, ADR-0040, ADR-0095: the externals path and default patch folder. A patch declares [adi.param name min max default] and gets native controls in the device view. The patch is stored as text in the project and edited in a node-based window inside ADI, built on plugdata. The agent never edits the text: it proposes changes in the editor, the user approves each one, and the undo log is the version history. | DECIDED ADR-0145 d8 |
| Native DSP nodes | ADR-0062: none to set; listed so the user knows the built-in devices have no external dependency. | NOTE |
| Freeze | ADR-0059: freeze renders with tails; the tail length and whether muted clips freeze silent (REAPER §22.6.8). | DECIDED ADR-0059 |
| Racks and macros | ADR-0060, ADR-0114: macro curve editing defaults (breakpoint count). | BACKLOG ADR-0060 |

207 settings.
