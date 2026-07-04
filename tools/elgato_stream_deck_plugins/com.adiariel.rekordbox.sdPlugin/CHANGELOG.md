# Changelog — Adi Ariel RekordBox MIDI

## 1.0.0.0 — 2026-07-05

Initial release: class-compliant virtual MIDI controller for rekordbox
PERFORMANCE mode on the Stream Deck + XL (device type 13; 9×4 keys, 6 dials,
touch strip).

- Dual-deck control matrix (Deck A = Ch 1, Deck B = Ch 2, browser = Ch 3):
  hot cues 1–8 per deck with a local **shift layer** that switches to the
  delete notes; play/pause + CUE transport (with shifted secondary notes);
  momentary jog-nudge notes (held = sustained pitch bend); browser up/down
  (hold to auto-repeat) + tree⇄list focus toggle; volume dial push = load
  track.
- Encoders as absolute 7-bit CC with per-deck accumulators (volume CC 20,
  filter CC 21, tempo CC 22 — BPM only, no pitch/key), touch-strip LCD
  feedback (bar/gbar layouts), per-instance deck + sensitivity settings,
  values persisted in global settings. Filter push = snap to center; tempo
  push deliberately inert.
- Touch strip: per-dial 200×100 zones — tap left/right half = beat jump ◀/▶
  on that dial's deck.
- MIDI backend: easymidi → @julusian/midi with **committed prebuilt N-API 7
  bindings** (darwin-arm64/x64, win32-x64/arm64) vendored under `vendor/` —
  no npm/compiler for end users. macOS creates a virtual CoreMIDI source;
  Windows attaches to a loopMIDI port with auto-retry/reconnect (RtMidi
  cannot create virtual ports on Windows — and fails silently, hence the
  explicit platform branch). System-wake reopen handling.
- Manifest requires Stream Deck app 7.3+ (first version with device type 13)
  and declares the `AdiRekordBox` DeviceType-13 profile (export instructions
  in README). Node 20 runtime.
- Docs: full MIDI implementation chart incl. type-in-able 4-digit hex codes,
  rekordbox 7 MIDI LEARN walkthrough, plan-gating warning (Core+ or Hardware
  Unlock required; "Free Plus" does not include MIDI LEARN), loopMIDI setup,
  TempoSlider 14-bit→7-bit Type pitfall, PitchBendUp/Down CSV fallback.
- Hardened by an adversarial multi-agent review (21 confirmed findings fixed):
  manifest + layouts now validate 0-error against the official @elgato/schemas
  (FontSize as number, layout text items use `value` not `text` so the ◀/▶
  strip hints render, Windows 10 minimum, `AutoInstall: false` on the
  not-yet-exported profile, Debug off); single-writer global-settings merge
  (kills a read-modify-write race that could clobber the PI's port rename and
  then revert the live MIDI port); dials repaint after restoring persisted
  levels at startup; PI no longer rebuilds the port field mid-edit and escapes
  `&`/`<`/`"`; MidiOut gets a dead-flag for unrecoverable vendor-load failure
  and `send()` no longer resets a pending reconnect timer (browse auto-repeat
  could starve reconnection); launcher logs honestly (switchToProfile is
  fire-and-forget); `validate.py` warns when the profile export is missing.
- Verified: `scripts/validate.py`; manifest + 3 layouts 0 schema errors
  (Draft-7 against @elgato/schemas); bundle boots to the expected registration
  handshake; `scripts/midi_smoke.mjs` — 19-message loopback through a real
  virtual port using the committed vendor tree (mac arm64).
