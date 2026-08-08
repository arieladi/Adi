// Virtual MIDI output, adapted from the verified rekordbox plugin's midi-out.js.
//
// Two changes from the original: it manages SEVERAL named ports at once (Studio
// OS drives rekordbox and a DAW simultaneously, and they must not share a MIDI
// stream), and it tracks every sounding note per port so a client disconnect can
// silence the rig instead of leaving notes stuck on.
//
// Backed by easymidi -> @julusian/midi with PREBUILT N-API binaries
// (darwin-arm64 / darwin-x64 / win32-x64 / win32-arm64) in vendor/, so nothing
// is ever compiled on the user's machine. This is what makes the old
// midi_control C++ helper unnecessary on Windows.
//
// Platform behaviour (RtMidi):
//   macOS / Linux — creates a real virtual CoreMIDI/ALSA source.
//   Windows       — WinMM has no virtual ports, so it attaches to an existing
//                   loopMIDI port (exact name, then substring, then any
//                   "loopMIDI") and retries until one appears.
import { createRequire } from "node:module";
import path from "node:path";
import os from "node:os";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const vendorRequire = createRequire(path.join(HERE, "vendor", "_resolve_.cjs"));

let easymidi = null;
function lib() {
  if (!easymidi) easymidi = vendorRequire("easymidi");
  return easymidi;
}

const RETRY_SCAN_MS = 3000;
const RETRY_FAIL_MS = 5000;

// Logical port id -> default device name. The frontend addresses ports by id so
// the user-visible name stays configurable without touching module code.
export const DEFAULT_PORTS = {
  rekordbox: "Adi RekordBox Controller",
  studio: "Adi Studio OS MIDI",
};

class Port {
  constructor(id, name, logger) {
    this.id = id;
    this.portName = name;
    this.log = logger;
    this.out = null;
    this.openedName = null;
    this.timer = null;
    this.warnedNoPort = false;
    this.dead = false;
    this.virtual = os.platform() !== "win32";
    this.sounding = new Set();   // "ch:note" currently on
  }

  get connected() { return this.out !== null; }

  configure(name) {
    const clean = String(name ?? "").trim() || DEFAULT_PORTS[this.id] || this.id;
    if (clean === this.portName && this.out) return;
    this.portName = clean;
    this.close();
    this.openSoon(0);
  }

  openSoon(ms) {
    clearTimeout(this.timer);
    this.timer = setTimeout(() => { this.timer = null; this.tryOpen(); }, ms);
  }

  tryOpen() {
    if (this.out || this.dead) return;
    let em;
    try { em = lib(); }
    catch (e) {
      this.dead = true;
      this.log.error?.(`[${this.id}] vendored easymidi failed to load — reinstall the service? ${e?.message ?? e}`);
      return;
    }
    try {
      if (this.virtual) {
        this.out = new em.Output(this.portName, true);
        this.openedName = this.portName;
        this.log.info?.(`[${this.id}] virtual MIDI port "${this.portName}" created`);
      } else {
        const ports = em.getOutputs();
        const want = this.portName.toLowerCase();
        const name =
          ports.find((p) => p.toLowerCase() === want) ??
          ports.find((p) => p.toLowerCase().includes(want)) ??
          ports.find((p) => /loopmidi/i.test(p));
        if (!name) {
          if (!this.warnedNoPort) {
            this.log.warn?.(
              `[${this.id}] no MIDI port matching "${this.portName}" (and no loopMIDI port) — ` +
              `create one in loopMIDI; retrying every ${RETRY_SCAN_MS / 1000}s. Seen: [${ports.join(", ")}]`);
            this.warnedNoPort = true;
          }
          this.openSoon(RETRY_SCAN_MS);
          return;
        }
        this.out = new em.Output(name);
        this.openedName = name;
        this.log.info?.(`[${this.id}] attached to MIDI port "${name}"`);
      }
      this.warnedNoPort = false;
    } catch (e) {
      this.log.error?.(`[${this.id}] MIDI port open failed: ${e?.message ?? e}`);
      this.openSoon(RETRY_FAIL_MS);
    }
  }

  send(type, msg) {
    if (!this.out) {
      // Drop and nudge the reconnect loop, but never clobber a pending schedule:
      // sustained input (the 140ms browse auto-repeat) would otherwise push
      // tryOpen out forever.
      if (!this.dead && !this.timer) this.openSoon(250);
      return false;
    }
    try { this.out.send(type, msg); return true; }
    catch (e) {
      this.log.error?.(`[${this.id}] MIDI send failed, reconnecting: ${e?.message ?? e}`);
      this.close();
      this.openSoon(1000);
      return false;
    }
  }

  noteOn(channel, note, velocity = 127) {
    const okSend = this.send("noteon", { note, velocity, channel });
    if (okSend) this.sounding.add(`${channel}:${note}`);
    return okSend;
  }

  noteOff(channel, note) {
    this.sounding.delete(`${channel}:${note}`);
    return this.send("noteoff", { note, velocity: 0, channel });
  }

  tap(channel, note, ms = 0) {
    if (!this.noteOn(channel, note)) return false;
    if (ms > 0) setTimeout(() => this.noteOff(channel, note), ms);
    else this.noteOff(channel, note);
    return true;
  }

  cc(channel, controller, value) {
    return this.send("cc", { controller, value, channel });
  }

  // Silence everything this port is holding. Called on client disconnect and on
  // explicit panic — a plugin that crashes mid-nudge must not leave a note on.
  panic() {
    for (const key of [...this.sounding]) {
      const [ch, note] = key.split(":").map(Number);
      this.noteOff(ch, note);
    }
    this.sounding.clear();
  }

  close() {
    clearTimeout(this.timer);
    this.timer = null;
    this.warnedNoPort = false;
    if (this.out) {
      this.panic();
      try { this.out.close(); } catch { /* RtMidi can throw if the OS tore it down */ }
      this.out = null;
      this.openedName = null;
    }
  }
}

export class MidiPorts {
  constructor(logger = console) {
    this.log = logger;
    this.ports = new Map();
  }

  // Ports are created lazily: opening a virtual CoreMIDI source the user never
  // uses would clutter their MIDI Studio for no reason.
  get(id) {
    const key = String(id || "studio");
    if (!this.ports.has(key)) {
      this.ports.set(key, new Port(key, DEFAULT_PORTS[key] || key, this.log));
      this.ports.get(key).openSoon(0);
    }
    return this.ports.get(key);
  }

  open(id, name) { this.get(id).configure(name); }
  panic(id) { id ? this.get(id).panic() : this.panicAll(); }
  panicAll() { for (const p of this.ports.values()) p.panic(); }
  closeAll() { for (const p of this.ports.values()) p.close(); }

  /* What the OS currently exposes, plus what we hold — drives the PI's port list
     and the "is rekordbox going to see this?" readout.

     Both directions are reported because the two platforms answer in different
     lists, which is easy to get backwards (verified on this machine):
       macOS   a virtual Output we CREATE is published as a SOURCE, so it shows
               up in getInputs() — that is the entry rekordbox and Ableton read
               from. getOutputs() stays empty unless real MIDI hardware is
               attached.
       Windows we ATTACH to an existing loopMIDI port, which is a real
               destination, so it appears in getOutputs(). */
  status() {
    let outputs = [], sources = [];
    try { const em = lib(); outputs = em.getOutputs(); sources = em.getInputs(); }
    catch { /* vendor tree unavailable */ }
    return {
      outputs,          // destinations we can attach to (the Windows route)
      sources,          // what this machine publishes — our virtual ports land here
      available: [...new Set([...sources, ...outputs])],   // union, for a simple UI list
      ports: [...this.ports.values()].map((p) => ({
        id: p.id, name: p.portName, opened: p.openedName,
        connected: p.connected, virtual: p.virtual, sounding: p.sounding.size,
      })),
    };
  }
}
