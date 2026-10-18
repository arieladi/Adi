# Waiting on the director

Tests and checks that only Adi's own hardware or hands can do. Each says what
to run, what it proves, and where the result is written. When one is done, its
result goes into the log it names, and the row here is struck through with the
date, not deleted.

| # | What | Needs | How | Result goes to |
|---|---|---|---|---|
| 1 | **BLAKE3 on AVX-512** (ADR-0153) | the Lenovo laptop, Intel Core i9 11th gen (AVX-512), with Claude Code installed, later in the project | `tools\bench_blake3.bat` on the laptop. It prints portable, SSE4.1, AVX2 and AVX-512 medians; the last line is the AVX-512 number. The four digests must agree. | a row beside the Ryzen's in `collab/win.md`, and a short ADR if AVX-512 is slower than AVX2 there (it can be, from clock-down) |
| 2 | **ASIO heard** (ADR-0137) | Adi's audio interface and its ASIO driver (FlexASIO if it has none) | `adi_play <project.adi> --type ASIO --tone`, then listen for the 220 Hz tone | `collab/win.md`, and the ASIO row of `docs/FEATURES.md` |
| 3 | **A project's clips heard** (ADR-0151) | any output device | `adi_play <project.adi>` plays the project's audio clips from 0 (`--from S` to start later). `adi_play <project.adi> --render 3` proves it without speakers: win's 44.1 kHz test clip rendered at -6.0 dBFS in a 48 kHz project, zero underruns, 2026-09-24 | `collab/win.md` |
| 4 | **High sample rates on real hardware** (ADR-0157) | Adi's interface at 192 kHz, and at 352.8 or 384 kHz if it offers them | `adi_play <project.adi> --rate 192000`, then again at the highest rate the interface lists (`--type` as needed); a project whose clips are 44.1 and 96 kHz files exercises the conversion | `collab/win.md` |
| 5 | **Live's pan law and solo, measured** (ADR-0163) | Ableton Live 12, any meter that reads dBFS | On an audio track playing a -6 dBFS sine (the same on both channels), Stereo Pan mode, read each channel's peak with the pan at the centre, at 25R (half-way) and at 50R (hard right). ADI's Live law predicts -6.0/-6.0 at the centre, -11.3/-3.7 at 25R, and silent/-3.0 at 50R. Then: solo a group's child (is the group heard?), solo a group (are its children?), and solo a track that a compressor on another track uses as its sidechain key (does the key still work?) | ADR-0163 d2 and d4, in a new ADR if Live differs |
| 6 | **The device strip's height** (ADR-0203 d7, A1-15) | Live 12 at 100 % interface zoom, any screen | Read the fixed device area's height in zoom-independent units (the director's figure is 169; a 2000-px screenshot of his Mac showed 193 px, consistent but not proof). ADI's strip minimum takes the measured value | `collab/win.md`, ADR-0203's AWAITING note |
| 7 | **Sends and returns, nine readings** (ADR-0203 d3) | Live 11 (the Mac) now; Live 12 again | (a) Where a new send knob starts and its top value. (b) Does Save as Default Audio Track keep send levels? (c) In a new Set, is Solo in Place on? (d) Track A sends to a Pre return; solo B: is A still heard through the return? (e) Does the Main track have mute and solo? (f) Enable a return's own send and feed it a click: how late does the loop come back, does it change with the buffer size, is a slow plug-in in the loop compensated, can two returns feed each other in a ring, and does Live allow any other routing loop? (g) A compressor triggered Post Mixer from a muted track, then from a track silenced by another's solo: does the ducking stop? (h) Which tap point a new sidechain chooser starts on; does Track Delay move a Pre FX or Post FX tap? (i) Bounce Group in Place includes the sends used inside the group? (Live 12 only) | `collab/win.md`; the engine's send tests take the numbers |
| 8 | **Track Delay** (ADR-0203 d4) | Live 11 or 12 | Its smallest and largest value, what the selector beside it chooses, whether the Main track and returns have one | `collab/win.md` |
| 9 | **Shifter's Pitch-mode latency** (ADR-0203 d4, A4-20) | Live 11.1 or newer | Hover the title bar and read the latency in the Status Bar; then with Delay Compensation off, play one click through Shifter at Dry/Wet 100 % beside the same click on a plain track, record both and read the gap, at 0 and +7 semitones, at a short, the default and a long Window. ADI adds exactly that much and no more | `collab/win.md`; `test_shifter` |
| 10 | **Auto Filter side by side** (ADR-0203 d8) | Live 12 only (Live 11 has the older design) | The parity check of ADR-0108 on ADI's Auto Filter, all ten types | the device's checklist |
| 11 | **Plug-in failures and the Configure-mode warning** (ADR-0203 d8) | Live 11 or 12 | Open a Set whose plug-in is not installed: where does Live say so (the device, the Status Bar, both)? In Configure Mode delete a parameter that has automation: the warning's words, and what happens to the automation. Open a plug-in window, move it, delete the device, Undo: does the window come back, and where? | `collab/win.md`; ADR-0180 d7 if it differs |
| 12 | **Automation** (ADR-0203 d7) | Live 11 or 12 | Move an automated fader while not recording, save, reopen: is Re-Enable Automation still lit? Alt-drag a volume segment into a curve and save the Set, so win can read how the curve is stored | `collab/win.md`; the automation shape table |
| 13 | **Files and MIDI** (ADR-0203 d6, d7) | Live 11 or 12 | Save As, then Save a Copy: which file are you left working in after each? Import a MIDI file with several tracks: what arrives, is its tempo taken; Export MIDI Clip: which file type? Which MIDI timecode frame rates are offered and which is the default; how quickly does Live follow a changing incoming clock (Resync on Live 12) | `collab/win.md` |
| 14 | **Fresh-install defaults** (ADR-0203 d21) | a fresh Live 12 on this PC | Create Fades on Clip Edges on or off; the driver type offered first on Windows with ASIO present; Takeover Mode; Record Quantization; whether Live picks up controllers and pitch bend when playback starts mid-clip (chase); the Settings tabs, counted and named; the theme | `collab/win.md`; the registry defaults that say "once measured" |
| 15 | **Editing behaviours ADI follows once measured** (ADR-0203 d7) | Live 11 or 12 | Does a recorded MIDI clip snap to whole bars? Scrub behaviour. Does muting a group mute its tracks? Notes starting slightly before a clip. Does Link Audio show peers by itself (Live 12). How a tuning is loaded and shown in the note ruler (Live 12) | `collab/win.md`; the registry rows `record.snapMidiPartsToBars`, `mixing.groupMuteMutesSources`, `midi.extendEarlyNotes` |
| 16 | **Settings defaults the document could not read in the manual** (ADR-0203 d11, d21) | a fresh Live 12 on this PC; Live 11 for the crossfader reading | How Live picks an auto-assigned track colour and whether its switch starts on; what leaves a MIDI port when Live stops; whether Live on Windows scales a plug-in's window with the display; the export dialog's default file type and bit depth; whether a Post send follows the crossfader (Live 11, then 12). Each result sets the registry default or the Master Reference row that says "read off Live" | `collab/win.md`; the registry defaults and Part V of the Master Reference |

Rows 6 to 16 are the readings Master Reference v1.0 asked for (ADR-0203 d24). Each result is labelled with the Live version it was taken on and re-checked on Live 12; the director's Mac runs Live 11.

Any listening row can start from a generated project:

```bat
adi_tool create demo.adi
python tools\make_demo_project.py demo.adi --project-rate 48000 --clip-rates 44100,96000
adi_play demo.adi --render 4
adi_play demo.adi
```

The first `adi_play` proves the path offline; the second is the one to
listen to. `--rate 192000` for row 4. `--vst3-uid` adds an instrument track,
and `--midi` gives it a four-bar clip, an arpeggio and then a held chord:
with `--vst3-uid "VST3-Surge XT-1e1bcfb-190e4fbd"` it is Surge XT playing
(`adi_play --list` prints the UIDs installed).

## Measured so far, for comparison

BLAKE3, 1 GiB in memory, MSVC `/O2`, median of five runs (ADR-0153):

| Machine | portable | SSE4.1 | AVX2 | AVX-512 |
|---|---|---|---|---|
| win's desktop, Ryzen 7 5700X3D | 648 | 1,700 | 3,537 | no AVX-512 on this CPU |
| Adi's laptop, i9 11th gen | | | | **waiting (row 1)** |

linux's i5-3550S measured portable against SSE4.1 only, with GCC, in ADR-0147 d8.
