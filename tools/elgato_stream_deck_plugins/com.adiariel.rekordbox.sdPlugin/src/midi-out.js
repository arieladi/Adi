// Virtual MIDI output for the RekordBox controller.
//
// Backed by easymidi -> @julusian/midi (node-midi fork with PREBUILT N-API
// binaries: darwin-arm64 / darwin-x64 / win32-x64 / win32-arm64), loaded from
// the committed vendor/ tree so end users never run node-gyp.
//
// Platform behaviour (RtMidi):
//   macOS / Linux — creates a real virtual CoreMIDI/ALSA source; rekordbox
//                   sees it as a class-compliant device named `portName`.
//   Windows      — WinMM has no virtual ports. The plugin instead attaches to
//                   an existing loopMIDI port (exact name match, then
//                   substring, then any port containing "loopMIDI") and keeps
//                   retrying every few seconds until one appears.
import { createRequire } from "node:module";
import path from "node:path";
import os from "node:os";
import { fileURLToPath } from "node:url";
import { DEFAULT_PORT_NAME } from "./midimap.js";

// Both src/ (dev) and bin/ (bundle) sit one level below the plugin root, so
// ../vendor resolves identically from either location.
const HERE = path.dirname(fileURLToPath(import.meta.url));
const vendorRequire = createRequire(path.join(HERE, "..", "vendor", "_resolve_.cjs"));

let easymidi = null;
function lib() {
  if (!easymidi) easymidi = vendorRequire("easymidi");
  return easymidi;
}

const RETRY_SCAN_MS = 3000; // waiting for a loopMIDI port to appear
const RETRY_FAIL_MS = 5000; // native open threw; back off a little longer

export class MidiOut {
  constructor(logger) {
    this.logger = logger;
    this.out = null;
    this.openedName = null;
    this.portName = DEFAULT_PORT_NAME;
    this.timer = null;
    this.warnedNoPort = false;
    this.dead = false; // vendor tree failed to load — retrying can't fix it
    this.virtual = os.platform() !== "win32";
  }

  get connected() {
    return this.out !== null;
  }

  // Set the desired port name (from global settings) and (re)open.
  configure(name) {
    const clean = String(name ?? "").trim() || DEFAULT_PORT_NAME;
    if (clean === this.portName && this.out) return;
    this.portName = clean;
    this.reopen();
  }

  reopen() {
    this.close();
    this.openSoon(0);
  }

  openSoon(ms) {
    clearTimeout(this.timer);
    this.timer = setTimeout(() => {
      this.timer = null;
      this.tryOpen();
    }, ms);
  }

  tryOpen() {
    if (this.out || this.dead) return;
    let em;
    try {
      em = lib();
    } catch (e) {
      this.dead = true; // stop retrying (and re-logging) — the vendor tree is broken
      this.logger?.error(`vendored easymidi failed to load — reinstall the plugin? ${e?.message ?? e}`);
      return;
    }
    try {
      if (this.virtual) {
        this.out = new em.Output(this.portName, true);
        this.openedName = this.portName;
        this.logger?.info(`virtual MIDI port "${this.portName}" created`);
      } else {
        const ports = em.getOutputs();
        const want = this.portName.toLowerCase();
        const name =
          ports.find((p) => p.toLowerCase() === want) ??
          ports.find((p) => p.toLowerCase().includes(want)) ??
          ports.find((p) => /loopmidi/i.test(p));
        if (!name) {
          if (!this.warnedNoPort) {
            this.logger?.warn(
              `no MIDI port matching "${this.portName}" (and no loopMIDI port) — ` +
              `create one in loopMIDI; retrying every ${RETRY_SCAN_MS / 1000}s. Seen: [${ports.join(", ")}]`);
            this.warnedNoPort = true;
          }
          this.openSoon(RETRY_SCAN_MS);
          return;
        }
        this.out = new em.Output(name);
        this.openedName = name;
        this.logger?.info(`attached to MIDI port "${name}"`);
      }
      this.warnedNoPort = false;
    } catch (e) {
      this.logger?.error(`MIDI port open failed: ${e?.message ?? e}`);
      this.openSoon(RETRY_FAIL_MS);
    }
  }

  // type: "noteon" | "noteoff" | "cc" — msg per easymidi
  // ({note, velocity, channel} / {controller, value, channel}).
  send(type, msg) {
    if (!this.out) {
      // Drop the message; nudge the reconnect loop — but never clobber an
      // already-pending schedule (sustained input like the 140 ms browse
      // auto-repeat would otherwise keep pushing tryOpen forever).
      if (!this.dead && !this.timer) this.openSoon(250);
      return false;
    }
    try {
      this.out.send(type, msg);
      return true;
    } catch (e) {
      // Port vanished (loopMIDI closed, device sleep) — drop and reconnect.
      this.logger?.error(`MIDI send failed, reconnecting: ${e?.message ?? e}`);
      this.close();
      this.openSoon(1000);
      return false;
    }
  }

  noteOn(channel, note, velocity = 127) {
    return this.send("noteon", { note, velocity, channel });
  }

  noteOff(channel, note) {
    return this.send("noteoff", { note, velocity: 0, channel });
  }

  // Momentary press-and-release in one go (hot cue taps, beat jump, browse).
  tap(channel, note) {
    const on = this.noteOn(channel, note);
    if (on) this.noteOff(channel, note);
    return on;
  }

  cc(channel, controller, value) {
    return this.send("cc", { controller, value, channel });
  }

  close() {
    clearTimeout(this.timer);
    this.timer = null;
    this.warnedNoPort = false;
    if (this.out) {
      try {
        this.out.close();
      } catch {
        // RtMidi close can throw if the OS already tore the port down
      }
      this.out = null;
      this.openedName = null;
    }
  }
}
