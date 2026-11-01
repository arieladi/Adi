# Parity checklist — step 7.6, the mixer

Drafted by the window lane (ADR-0208) from the Live 12 manual (`live12-manual-en.pdf`, chapter
18, the PDF's pages) and ADR-0203 d3, d9, d14 and ADR-0204 d1, for win to run against Live 12
(ADR-0108). Rows describe the behaviour in our words. Status as in `parity-7.2-shell.md`.

| # | Live's behaviour | Manual / ADR | ADI in this PR | Status |
|---|---|---|---|---|
| 1 | Each arrangement track has its mixer controls in its own row (the Arrangement Track Controls) | §18.1, p. 389; §6.1, p. 162 | The mixer panel's rows sit level with the arrangement's lanes and scroll with them; the panel is ADR-0080's side slot | built; measure: Live's Mixer View of vertical strips is a separate view (row 13) |
| 2 | The Meter shows peak and RMS of the track's output | §18.1, p. 391 | Peak and RMS of the newest 1024 frames of the track's post-fader tap, wait-free, no allocation | built; the scalar meter of UI-ARCHITECTURE §9 is the engine's (window log, N9) |
| 3 | Volume adjusts the output level, -inf to +6 dB | §18.1, p. 391 | A horizontal fader with its value in dB; a drag is one undo step | partial: the sound follows on release, not during the drag, until win rules how a non-parameter drag coalesces (OpSubmitter's note) |
| 4 | Pan places the track; a double-click resets it | §18.1, p. 392 | A knob; double-click centres it | built |
| 5 | The Pan control's context menu switches to Split Stereo Pan Mode | §18.1, p. 392; ADR-0203 d9 | The menu shows Stereo Pan and a disabled Split Stereo Pan | open: a pan mode and two pans per strip in the format and the mixer (N12) |
| 6 | The Track Activator mutes the track's output and shows its number | §18.1, p. 392 | Built, through `track.setMute` | built |
| 7 | Solo solos the track | §18.1, p. 392 | Built, through `track.setSolo` | partial: Exclusive Solo, Ctrl/Cmd for several, and the S key arrive with the engine's solo work (ADR-0203 d3) |
| 8 | Arm Recording | §18.1, p. 392 | — | open: recording is not built |
| 9 | One send knob per return on every track; return strips after the tracks | ADR-0203 d3 | — | open: the snapshot carries neither the return tracks (a track's kind) nor the send rows (N6, N11) |
| 10 | A loudness meter (LUFS and true peak) on the Main track only | ADR-0203 d14 | — | open: it needs Main identified (N6) and a loudness meter's DSP (N13) |
| 11 | The ADI-Q and ADI-L slots on every channel and group, present but off | ADR-0204 d1 | Two slots on every row, off; a click says the slot waits for the plug-in | built (present and off); filled by the engine's slots and the clones lane |
| 12 | Auto Gain Stage, ADI's one offline command | ADR-0195 d4, ADR-0203 d14 | The mixer's right-click menu: gated RMS at -18 dBFS or integrated loudness at -18 LUFS; one undo step | built |
| 13 | The Mixer View: vertical strips, resizable taller for tick marks and a numeric field | §18.1, p. 390, 393 | — | open: a later 7.6 item |
| 14 | The mouse wheel over the mixer scrolls; it does not move a control | ADR-0203 d11 | The wheel scrolls the arrangement and the rows with it | built |
| 15 | Only what is on screen is realised | UI-ARCHITECTURE §10 | Rows for the visible lanes only, recycled on scroll; a steady frame allocates nothing | built |
