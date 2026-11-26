# The sanity check of 26 November 2026

**Why it exists.** On 26 November the director saw that ADI draws no waveform while audio records, and
that the plan had no row for it. He ordered a check of the whole program for other basic DAW behaviour
that is missing and unplanned. His rule from that day: **the basics come first.** Every gap below is a
fix under ADR-0248 d1 and is worked now, whether or not the hold has ended (ADR-0257 d1).

**The register.** This paper is the register of its own rows (DF-W59). A row is struck here, with its pull request, when its fix is on main.

**How it was made.** Six audits of main at `5ebc7453f`. Each wrote its own list of basics from general
DAW practice and Live 12, before reading ADI's documents, and then traced every item to the window's
code, the engine, the plan and the register. One audit asked only what the screen should show. Nothing
was run in the window: a state rests on reading code, tests and the two walk reports of 25 November.
Two items were then checked by test and are marked.

**How the gaps slipped.**
- `docs/FEATURES.md` ticks a row when the file format can store it, not when the feature works. Punch
  in and out carries that tick.
- The parity sheets hold rows marked "open" that no plan or register row picks up, and some are stale.
- Every list ADI kept was a list of commands. None listed what a musician expects to see.
- Settings shows about 160 controls that nothing reads, and thirteen Control Bar buttons answer "not
  built yet".

**States.** *Missing*: not built and owned by no row. *Engine only*: built, with no way to reach it in
the window. *Late*: owned by a row, placed too late for how basic it is. *To run*: read in code as
suspect; needs one run. A row id in the last column is the owner that already existed.

**Waves.** 1 = the weekend of 27 to 29 November, in a queue of `collab/prompts/2026-11-27-*.md`.
2 = the week after. 3 = stays with its row.

## Defects

| SC | What happens | State | Queue | Wave | Row |
|---|---|---|---|---|---|
| SC-01 | **The Main track can be deleted,** by a user and by an agent's changeset. Checked by a preview on a new project | Missing | tracks | 1 | |
| SC-02 | **A click in the lanes while the song plays moves the playback there.** While recording it cuts the take short. In Live only the insert marker moves | Missing | recording | 1 | |
| SC-03 | Continue (Shift+Space) loses the stop point after any lane click | Missing | recording | 1 | |
| SC-04 | The Status Bar's length of a time selection is wrong after a tempo change | Missing | look | 2 | |
| SC-05 | Edit > Delete Track deletes the whole track while a clip is selected; with several tracks selected it deletes one | Late | tracks | 1 | worklist F3 |
| SC-06 | The CPU meter always reads "0 %" | Late | look | 1 | DEV-24, MX-28 |
| SC-07 | **Save As leaves the recordings behind** in the old project's folder | Missing | project | 1 | |
| SC-08 | About 160 of 222 Settings controls have no reader (Test Tone, Language, Open on start among them) | Missing | project | 1 | WTS-07 |
| SC-09 | A plug-in's own state is not captured at Ctrl+S, Save As or quit | Late | project | 1 | PD-03 |
| SC-10 | The Monitor switch has no effect on MIDI: live notes follow Arm alone | Late | midi | 1 | MX-09 |
| SC-11 | An unwarped clip after a tempo change: its tail may be cut | To run | clips | 1 | |
| SC-12 | Thirteen Control Bar buttons answer "not built yet". Each is wired, or greyed with its reason | Missing | look | 2 | |

## Recording audio

| SC | Basic | State | Queue | Wave | Row |
|---|---|---|---|---|---|
| SC-20 | The waveform of a take as it records | Missing | recording | 1 | DF-E90, DF-W58 |
| SC-21 | A count-in: the Metronome's menu, and the count shown counting | Engine only | recording | 1 | N49 |
| SC-22 | A take placed with latency compensated. Today every overdub lands late | Late | recording | 1 | MX-09 |
| SC-23 | Choosing which input a track records, and a mono take from a mono input. Today every track records inputs 1 and 2 | Late | recording | 1 | MX-25 |
| SC-24 | The recorded file's name and folder, as Live's | Missing | recording | 1 | |
| SC-25 | The recording's file type and bit depth | Engine only | recording | 2 | |
| SC-26 | Punch in and out | Missing | recording | 2 | |
| SC-27 | Recording with the loop on | Engine only | recording | 2 | CT-02 |
| SC-28 | An audio device that stops or is unplugged says so | Missing | recording | 1 | |

## Audio clips

| SC | Basic | State | Queue | Wave | Row |
|---|---|---|---|---|---|
| SC-30 | Trimming a clip's start (its left edge) | Engine only | clips | 1 | ED-04 |
| SC-31 | Clip gain, and the waveform's height following it | Engine only | clips | 1 | TM-WT-06 |
| SC-32 | Warp on and off on a clip already placed | Engine only | clips | 1 | TM-WT-02 |
| SC-33 | Reversing a clip. Today a reversed clip is silent | Missing | clips | 2 | |
| SC-34 | Import Audio File from a menu | Missing | project | 1 | |
| SC-35 | A file dropped onto a clip replaces what it covers | Late | clips | 2 | DF-W28 |
| SC-36 | Waveform detail when zoomed in. Today one resolution: a long file is drawn as blocks | Missing | clips | 1 | |
| SC-37 | A clip's own colour in the Arrangement | Engine only | look | 1 | |
| SC-38 | Where a dragged sample or device will land, shown before the drop | Missing | look | 2 | |
| SC-39 | A looped clip's repeats marked | Missing | clips | 2 | |
| SC-40 | A missing file marked on its clip, and relinked | Late | project | 2 | TM-MD-03 |

## MIDI and instruments

| SC | Basic | State | Queue | Wave | Row |
|---|---|---|---|---|---|
| SC-50 | **Recording MIDI into a clip,** with the notes appearing as they are played | Missing | midi | 1 | ADR-0256 |
| SC-51 | Playing notes from the computer keyboard | Late | midi | 1 | WIN-05 |
| SC-52 | A track that plays as soon as it has an instrument: armed as Live arms it, and MIDI ports on as Live has them | Missing | midi | 1 | |
| SC-53 | Hearing a note when it is clicked, drawn or moved, and from the piano keys; the keys lit while a note sounds | Late | midi | 1 | MI-03 |
| SC-54 | Space in the Note Editor plays from where the user clicked | Missing | midi | 2 | |
| SC-55 | The sustain pedal, pitch wheel and mod wheel in OneShot and Sampler, and proved into a plug-in | Missing | midi | 1 | |
| SC-56 | Program and bank changes | Missing | midi | 3 | |
| SC-57 | A .mid file dragged in, and a clip exported as one | Late | midi | 2 | TM-IC-02 |
| SC-58 | Capture MIDI | Engine only | midi | 2 | CT-06 |
| SC-59 | MIDI Learn | Engine only | midi | 3 | WIN-07 |
| SC-60 | Overdub and record quantization | Late | midi | 2 | CT-03 |
| SC-61 | A plug-in instrument dragged from the Browser; an instrument dropped below the tracks makes a MIDI track | Missing | tracks | 1 | |
| SC-62 | An instrument that sounds with nothing loaded into it | Ruled out | | | ADR-0169 d4, d5; the director's choice T |
| SC-63 | A MIDI track's meter lit by its notes | Late | look | 2 | MX-01 |

## Transport, ruler and grid

| SC | Basic | State | Queue | Wave | Row |
|---|---|---|---|---|---|
| SC-70 | **The ruler and grid as the approved sheet and as Live.** Plain ticks, not hooks; bar lines darker than beat lines; the grid's spacing above the time ruler at the lower right; a new project opens as a new Live Set does, on whole bars | Missing | look | 1 | DF-W20 |
| SC-71 | The grid and the ruler's labels follow the zoom below a quarter note | Late | look | 2 | DF-W50 |
| SC-72 | The view scrolls to the insert marker (Home, a locator, a typed position) | Missing | look | 1 | |
| SC-73 | The tempo, position, loop and signature fields answer a drag, the arrows and the wheel | Missing | look | 1 | |
| SC-74 | A tempo change placed in the song, and tempo shown along it | Engine only | look | 2 | |
| SC-75 | Stop pressed while stopped returns to 1.1.1 | Missing | look | 2 | |
| SC-76 | Follow in a new project as Live has it | To run | look | 1 | |

## Tracks, mixer and devices

| SC | Basic | State | Queue | Wave | Row |
|---|---|---|---|---|---|
| SC-80 | Duplicating a track; cut, copy and paste of a track | Missing | tracks | 1 | |
| SC-81 | Meters that show left and right. Today both bars are one number | Missing | clips | 1 | |
| SC-82 | A fader or send reset by a double-click; fine adjustment with Shift; a value typed into a knob | Missing | tracks | 1 | |
| SC-83 | Reordering devices by dragging, and to another track | Engine only | tracks | 1 | |
| SC-84 | A plug-in dragged from the Browser; a device dropped below the tracks makes a track | Missing | tracks | 1 | |
| SC-85 | Saving a device's preset and loading it back | Missing | tracks | 2 | the director's choice U |
| SC-86 | Freeze, Flatten and Bounce as commands in the window | Engine only | tracks | 2 | TM-FC-01, TM-FC-04 |
| SC-87 | The Track Activator in the automation chooser; Draw Mode for automation; clearing an envelope | Late | tracks | 2 | AU-01, AU-08 |
| SC-88 | A compressor, a gate and a limiter found in the Browser | Late | tracks | 3 | DEV-22, DF-M28 |
| SC-89 | A plug-in that failed its scan or its load shown as such | Late | look | 2 | DEV-25 |
| SC-90 | A muted track's clips dimmed; a bypassed device greyed | Engine only | look | 1 | |
| SC-91 | Which panel has the keyboard, and a sign that more tracks lie below | Missing | look | 2 | |
| SC-92 | The Audio To chooser reads "Main", as Live 12 | Missing | look | 1 | |

## Project, files and the shell

| SC | Basic | State | Queue | Wave | Row |
|---|---|---|---|---|---|
| SC-100 | **The way back to a project.** Open Recent; the Open chooser starts in the projects folder; that folder is one the user can see; an untouched Untitled file is not left behind | Missing | project | 1 | |
| SC-102 | An About box with the version and the licences; an icon | Missing | project | 1 | |
| SC-103 | A log file, so a fault can be reported | Missing | project | 1 | |
| SC-104 | A folder chosen with a button in Settings, and read without a restart | Missing | project | 1 | |
| SC-105 | A double-click or Enter on a sample in the Browser loads it | Missing | project | 1 | |
| SC-106 | Save As over an existing name asks and replaces | Engine only | project | 2 | N45 |
| SC-107 | An installer and the .adi file association | Missing | project | 2 | |
| SC-108 | The Browser's edge dragged to resize it | Engine only | project | 2 | |
| SC-109 | High DPI and a second monitor, walked | To run | look | 2 | DF-W53 |

## The director's remarks from the window, 26 November

SC-70 above is the first of them.

| SC | Remark | State | Queue | Wave | Row |
|---|---|---|---|---|---|
| SC-110 | **No AI tab at the Browser's foot.** The search bar is the one place to search and to ask the agent. The tab goes now; the row that sends the text to the agent arrives with the agent's own connection | Ruled, ADR-0257 d12 | look | 1 | AGT-06 |
| SC-111 | **The Info box is larger:** as wide as the Browser's column (win's reading of his drawing) | Ruled, ADR-0257 d12 | look | 1 | |
| SC-112 | **The Info box's analyser.** The spectrogram by default, as ruled on 18 November and never built; the box's own switch between the spectrogram, the spectrum in SPAN's look, the spectrum in the look of Live's Spectrum, and the Classic Scope small. Today it shows a plain spectrum because it follows the Analysers page | Ruled, ADR-0244 d7 and ADR-0257 d12 | look | 1 | DF-W22 |

## What changes so it cannot recur

| SC | Change | Owner |
|---|---|---|
| SC-P1 | `docs/FEATURES.md` says "built" separately from "the format can store it" | win |
| SC-P2 | Every "open" row of a parity sheet is a plan or register row, or is struck; stale rows corrected | win, with the window lane |
| SC-P3 | Each parity sheet gains a part "what the screen shows", and each walk checks it | win |
| SC-P4 | The dead-control sweep (WTS-07) moves up: every control works or says why | win |
| SC-P5 | The repeatable real-window script (WTS-05, WTS-06) starts when the hold ends | the control seat |

## To read in Live 12 before building

These rows wait for one look at Live: SC-52 (does a track arm when an instrument is loaded, and do MIDI
ports start with Track on), SC-54 (does a click in the Note Editor move the song's insert marker), SC-70
and SC-76 (a new Set's zoom and whether Follow is on), SC-71 (the grid's spacing at each zoom), SC-75
(Stop pressed twice), SC-82 (what resets a fader).
