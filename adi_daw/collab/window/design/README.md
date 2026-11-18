# The design sheets

The look of ADI's window is settled with the director one surface at a time (ADR-0243 d1). A sheet
is the picture he agreed to and the words that go with it. A seat builds the look of a surface only
from its sheet, and the built surface is then shown to him against it.

The pictures are drawings to talk about, not builds. They are ADI's own drawing throughout.

## The drawings' sources

[`src/`](src/) holds the pages that draw the pictures. Each builds one 1920x1080 SVG, so every
position, size, radius and colour on a sheet is a number in its source. Open a page in a browser;
read it to measure.

| Page | Draws |
|---|---|
| `language.html` | the design language |
| `arrangement.html` | the Arrangement |
| `mixview6.html` with `rows6.js` | the Mix View's Stack: the rows, the lens sizes, the cards, the "+" |
| `focus2.html` | Focus |
| `analysers.html` | the Analysers (places and sizes only) |
| `lib.js`, `lib2.js` | the shared parts: the colour table, the ring knob and its states, buttons, groups, choosers, the Control Bar, the Info box, the meter |

## Round 1 — 17 and 18 November 2026 (ADR-0244)

| Sheet | Picture | His word |
|---|---|---|
| The design language | [language.png](language.png) | "design language looks very good" |
| The Arrangement, with the Browser, the Info box and the Device View | [arrangement.png](arrangement.png) | "as a design guide that seems good enough" |
| The Mix View, the Stack | [mix-view.png](mix-view.png) | "yha thats good" |
| The Mix View, Focus | [focus.png](focus.png) | "lets take it as approved for now" |
| The Mix View, the Analysers | [analysers.png](analysers.png) | "lets take it as approved for now" |

The name stamped at the top right of each picture is the proposal he answered. Where a later
answer of his changes something on an earlier picture, the words below win over the picture.

### What "approved for now" covers

He approved the last two sheets with the words "its hard for me to rap my mind around it so lets
take it as approved for now". Their layout is agreed. The look of each cloned tool on them is not
the one drawn: it is the source's (the next section).

### The rule for everything ADI clones

His words, 18 November: "the elements we are cloning (BX_meter, SPAN, VISU, BLUPRINT, s(m)exoscope
oscope2 psyscope pro, fuser, trackspacer) ... all the abilities we are cloning... = they should
look the same as the plugins we are cloning (about the same at least)".

- A cloned tool takes its source's layout, its read-outs and the way its display is drawn. Nobody
  invents a new look for it. It is still drawn by ADI: no other maker's bitmaps, logos or fonts.
- The spectrum as drawn on the sheets (the SPAN and BluPrint kind) "looks good"; a third choice is
  added, a spectrum drawn as Live's Spectrum device draws it.
- The scope "should be literally the exo scope embedded in there as we actually have s(m)exoscope
  code as it is open code": the s(M)exoscope display and its controls themselves, from its source,
  not a redrawing of them (ADR-0183 d5 already gives it its own buffer).
- The page of a cloned tool needs no new sheet. It is shown to him as built, beside the source.

| ADI's part | Its source | Where it stands |
|---|---|---|
| The meter at the right of the Mix View | bx_meter | every Mix View page |
| Spectrum | SPAN and BluPrint; Live's Spectrum as a third look | the Info box, the Analysers |
| Spectrogram | VISU | the Info box (the default there), the Analysers |
| Scope | s(M)exoscope, embedded; OScope and PsyScope Pro for the grid-synced and many-channel scope (ADR-0167) | the Analysers, Focus |
| Reference and match | BluPrint | Focus (Match), the Analysers |
| Unmask | Trackspacer and FUSER | the Stack's Unmask tool, Focus |

## The design language ([language.png](language.png))

- **Base.** Live stays the base for the Arrangement, the Browser and the Device View: the same
  controls in the same order and places. What changes is how they are drawn.
- **Theme.** Dark is the default, as Live 12's dark. Every colour comes from the theme; themes are a
  setting and several ship (dark, the mid grey built today, light, and a darker, cooler one for the
  Mix View).
- **Buttons (B).** Related buttons share one rounded group, as Bitwig draws them: the transport, the
  loop and punch set, undo and redo, the view switch. States: rest, pointer over, pressed, on, on
  for record (red), on for a value (blue), disabled.
- **Knob (C).** The ring knob: the value is the ring. One knob everywhere, in three sizes. A knob
  that starts from the centre draws its ring from the centre; an automated one carries a red dot; a
  modulated one a second arc; a disabled one is grey.
- **Boxes, choosers, switches, tabs (D).** As drawn.
- **Density (F).** Two, a setting: Compact, with Live 12's sizes, is the default; Open is the same
  panel with more room.
- **A device is never a box of dials.** "nothing in adi daw can look like a generic design-less
  device." Every device has a display that shows what it does.

## The Arrangement ([arrangement.png](arrangement.png))

- **Control Bar.** Live's controls in Live's order, in rounded groups, with the Arrangement / Mix
  View switch at the right.
- **Over the track names.** Switch sides (the track names to the other side); "Show", a chooser
  that shows only the chosen groups; the show and hide switches for the track row's sections.
- **Groups.** A track inside a group is as wide as the group and not indented. It carries a line in
  the group's colour at its left, as in Live; a track in a group inside a group carries two lines,
  one for each group.
- **Clips.** A clip always shows what is in it: the waveform of an audio clip, the notes of a MIDI
  clip (DF-W21).
- **Info box.** The analyser in it is the spectrogram by default, with a switch to the spectrum. A
  click on it opens the Analysers.
- **Device View.** The EQ, the compressor and the limiter shown here are the Mix View's own panels.
  The Pd EQ Eight, limiter and compressor are ordinary devices, like a VST or a CLAP plug-in, and
  not part of the Mix View. Every other Pd device keeps its place and gets a designed panel of its
  own, one sheet for each device. A third-party plug-in shows a small panel with "Open editor".

## The Mix View, the Stack ([mix-view.png](mix-view.png))

Numbers are the circles on the picture.

1. **It takes the window.** The Mix View button switches the whole window: no Browser and no Device
   View beside it.
2. **Song strip.** An overview of the song with the ruler. He moves in the timeline here as in the
   Arrangement, to play different parts.
3. **The Mix View's bar.** Its pages, "With tools" (only the channels that carry a tool) and
   "+ Function". The pages are Stack, Focus and Analysers, as the two later pictures draw the bar;
   the analyser pages listed on this picture belong to the Analysers.
4. **Rows slider and Grow.** The slider sets how many channels are seen at once. With "Grow under
   the pointer" on, the list is a lens: the row under the pointer is the largest, the rows next to
   it are partly grown, the rest stay small; rows push each other and nothing overlaps; the tool
   under the pointer is wider than its row-mates. With the pointer on the meter or the analyser,
   every row is small again.
5. **Left column.** Search, then the list of channels with their levels; under it "Show", the
   filters by group.
6. **Info box.** Bottom left, as in the Arrangement; a click opens the Analysers.
7. **A row.** One channel. Thin level bars at its left edge; the channel's name with its colour dot
   and small M and S at the row's left. There is no channel block at the right.
8. **A tool.** A card with no title bar, only a small turned label at its left edge. The EQ is drawn
   as a grid, a thin spectrum and one clean curve; its bands show as the card grows, and a band's
   read-out floats by the band. Dynamics tools keep their knobs at every size and gain more
   controls as they grow.
9. **The name tag.** Name, colour, M, S.
10. **The meter.** At the far right on every page, with bx_meter's layout and readings: Balance and
    Correlation at the head; Peak, Dynamic and Rms numbers; the LEFT and RIGHT bars with peak, RMS
    and holds; the two floating dynamic-range bars between them; Float; Reset, the weighting, the
    mode; Solo L, R, Filter, M, S. "good start we will improve later".
11. **A third-party plug-in** (no circle on this picture; "Saturator X" in the Drum Bus row) is a
    chip in the row. A click opens its editor over the Mix View; closing it leaves him in the Mix
    View. No line joins the editor to its chip.
12. **Sounding now.** A filter that shows only the channels sounding at the moment it is chosen. It
    is a snapshot: "what i chose at that moment stays" until he refreshes it or picks another
    filter.
13. **The "+".** Every chain ends with one. Its menu lists the Mix View's tools first (one of each
    in a channel, so those already in the chain are grey), then devices and plug-ins.

Still open, by his word: what else a channel's name tag carries, "until compare to Ableton 12 and
test for some time myself"; it is easy to add later.

## The Mix View, Focus ([focus.png](focus.png))

His wish: "solo say just two channels in the arrangement view and then hit the mixview button ...
to go straight to the focus page and work on these two channels making sure they work good together
with tools like our fuser clone or trackspacer clone or eq match abilities".

- **The way in.** While tracks are soloed, the Mix View button opens Focus on them; otherwise it
  opens the Stack. (Win's reading of "when tracks are followed"; his to correct.)
- **Channels.** Two or more: A, B, C and on, with a "+" to add one.
- **The main.** One channel is the main, the one the others make room for; a star on a channel
  makes it the main.
- **Solo.** Leaving Focus keeps the solos. Un-soloing a channel in Focus takes it out, and the
  first one still soloed becomes the main. (Win's reading of "if i unsolo them in focus one of them
  becomes the main focus"; his to correct.)
- **Hear.** A, B, C, all, or what is being removed.
- **Together.** One large display of all the spectra, hatched where the main is covered, with what
  Unmask takes out of the others hanging from the top and handles for the range.
- **Tools.** Unmask, Match, Align and Scope to begin with; "i want all the tools but need to build
  around it slowly lets start with this and add as needed".
- **Below.** The channels' own chains, edited as in the Stack.

## The Mix View, the Analysers ([analysers.png](analysers.png))

The large analyser screen, a page of the Mix View, reached by a click on the Info box.

- **Pages.** "All" shows every analyser at once; each also has a page of its own: Spectrogram,
  Spectrum, Scope, Phase, Loudness, Analysis.
- **Source.** Main by default, any channel from the list, or the selected channel followed.
- **Each analyser** is the clone of its source and looks like it (the table above); the drawing on
  the sheet stands only for its place and size.
- **Analysis.** A table of what stands out in each channel: peak, loudness, dynamic range, width,
  correlation, clipped samples, what masks it, a note.
- **Cost.** Nothing here costs processing while the view is closed (ADR-0237).

## What is not on a sheet yet

Each Pd device's panel; the Clip View; the Mixer and the track rows in detail; the Browser; menus,
dialogs and Settings; the contents of a channel's name tag in the Mix View. They are rows in
`docs/DEFERRED.md` (DF-W20, DF-W23).
