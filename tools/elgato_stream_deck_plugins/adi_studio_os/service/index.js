// studioos-service — the Node backend for the Studio OS Stream Deck plugin.
//
// Runs on the Stream Deck app's own bundled Node 20 (see scripts/install-*), so
// the user never installs a runtime. The CEF frontend cannot open a MIDI port,
// synthesise a keystroke or spawn a process; this closes that gap over a
// loopback WebSocket. Protocol: docs/IPC.md.
//
// Safety property worth stating plainly: when the last client disconnects, every
// sounding note is silenced. A plugin crash mid-nudge or mid-drum-hit must never
// leave a note stuck on in rekordbox or Ableton.
import { WsServer } from "./ws-server.js";
import { MidiPorts, DEFAULT_PORTS } from "./midi.js";
import * as OS from "./os.js";
import * as Home from "./home.js";

const PORT = Number(process.env.STUDIOOS_PORT || 9011);

// Timestamped stderr logging — LaunchAgent/Task Scheduler capture this to a file
// and it is the only window into the service once it is running headless.
const log = {
  info: (m) => console.error(`${stamp()} INFO  ${m}`),
  warn: (m) => console.error(`${stamp()} WARN  ${m}`),
  error: (m) => console.error(`${stamp()} ERROR ${m}`),
};
function stamp() { return new Date().toISOString().replace("T", " ").slice(0, 19); }

OS.setLogger(log);
Home.setLogger(log);

const midi = new MidiPorts(log);
const server = new WsServer({ port: PORT, logger: log });

// ---------------------------------------------------------------- dispatch
// Every handler returns a value (or a promise) when the caller sent an `id`,
// and is fire-and-forget otherwise. Realtime verbs never await anything.
const HANDLERS = {
  "hello": () => ({ service: "studioos", version: VERSION, platform: OS.platform, ports: DEFAULT_PORTS }),

  // --- MIDI (realtime) ---
  "midi.noteOn":  (m) => midi.get(m.port).noteOn(num(m.ch), num(m.note), m.vel == null ? 127 : num(m.vel)),
  "midi.noteOff": (m) => midi.get(m.port).noteOff(num(m.ch), num(m.note)),
  "midi.cc":      (m) => midi.get(m.port).cc(num(m.ch), num(m.cc), num(m.val)),
  "midi.tap":     (m) => midi.get(m.port).tap(num(m.ch), num(m.note), num(m.ms)),
  "midi.panic":   (m) => (m.port ? midi.panic(m.port) : midi.panicAll(), true),
  "midi.open":    (m) => (midi.open(m.port, m.name), true),
  "midi.ports":   () => midi.status(),

  // --- OS ---
  "os.key":       (m) => OS.key(m.token),
  "os.type":      (m) => OS.type(m.text),
  "os.hotkey":    (m) => OS.hotkey(m.combo),
  "os.action":    (m) => OS.action(m.name),
  "os.volume":    (m) => OS.volume(m.delta),
  "os.mute":      () => OS.mute(),
  "os.zoom":      (m) => OS.zoom(m.dir),
  "os.appSwitch": (m) => OS.appSwitch(m.dir),
  "os.launch":    (m) => OS.launch(m.app),
  // Which named actions exist on THIS machine — drives tile visibility so
  // the hub never paints a key whose target is not installed.
  "os.actions":   () => OS.actionAvailability(),
  "os.rescan":    () => OS.rescanApps(),

  // --- smart home ---
  "home.dim":     (m) => Home.dim(m.level),
  "home.status":  () => Home.status(),
};

function num(v) { const n = Number(v); return Number.isFinite(n) ? n : 0; }

server.onMessage = async (msg, client) => {
  const handler = HANDLERS[msg && msg.t];
  if (!handler) {
    if (msg && msg.id) client.send({ id: msg.id, ok: false, error: `unknown verb "${msg.t}"` });
    return;
  }
  try {
    const result = await handler(msg);
    if (msg.id) client.send({ id: msg.id, ok: true, result });
  } catch (e) {
    log.error(`${msg.t} threw: ${e?.message ?? e}`);
    if (msg.id) client.send({ id: msg.id, ok: false, error: String(e?.message ?? e) });
  }
};

server.onConnect = (client) => {
  log.info(`client connected (${server.clients.size} total)`);
  client.send({ t: "ready", platform: OS.platform, version: VERSION });
};

server.onDisconnect = () => {
  log.info(`client disconnected (${server.clients.size} remaining)`);
  if (server.clients.size === 0) {
    log.info("no clients left — silencing all MIDI");
    midi.panicAll();
  }
};

const VERSION = "2.0.0";

// ------------------------------------------------------------------ lifecycle
function shutdown(signal) {
  log.info(`${signal} — shutting down`);
  midi.closeAll();
  server.close();
  process.exit(0);
}
process.on("SIGINT", () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));

// A crash that takes the service down with notes sounding is the one failure
// mode that is audible in a mix, so silence first and let the supervisor restart.
process.on("uncaughtException", (e) => {
  log.error(`uncaught: ${e?.stack ?? e}`);
  try { midi.panicAll(); } catch { /* nothing left to do */ }
  process.exit(1);
});

server.listen();
log.info(`studioos-service ${VERSION} on ${OS.platform}, node ${process.versions.node}`);
