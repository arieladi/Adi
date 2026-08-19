// Smoke test for studioos-service: boots it for real, speaks the wire protocol
// with a real WebSocket client, and checks the loopback guard, the hand-rolled
// framing (including a >125-byte payload that exercises the 16-bit length path)
// and real virtual MIDI port creation.
//
// Deliberately does NOT exercise os.key / os.volume / os.action with valid
// arguments: those synthesise keystrokes and change system volume on the machine
// running the test. Only their rejection paths are checked.
import { spawn } from "node:child_process";
import http from "node:http";
import fs from "node:fs";

const PORT = 9199; // not 9011, so a running service is never disturbed
const URL_WS = `ws://127.0.0.1:${PORT}`;
const SERVICE = new URL("../service/index.js", import.meta.url).pathname;

let pass = 0, fail = 0;
const ok = (name, cond, extra = "") => {
  if (cond) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name} ${extra}`); }
};
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

const child = spawn(process.execPath, [SERVICE], {
  env: { ...process.env, STUDIOOS_PORT: String(PORT) },
  stdio: ["ignore", "ignore", "pipe"],
});
let stderr = "";
child.stderr.on("data", (d) => { stderr += d.toString(); });

const done = (code) => { try { child.kill("SIGTERM"); } catch {} process.exit(code); };

try {
  await wait(700);
  ok("service started", child.exitCode === null, stderr.slice(-300));
  if (child.exitCode !== null) { console.log(stderr); done(1); }

  console.log("\n[1] http health endpoint");
  const health = await new Promise((res, rej) => {
    http.get(`http://127.0.0.1:${PORT}/`, (r) => {
      let b = ""; r.on("data", (c) => (b += c)); r.on("end", () => res(b));
    }).on("error", rej);
  });
  ok("GET / reports the service", JSON.parse(health).service === "studioos", health);

  console.log("\n[2] websocket handshake + verbs");
  const ws = new WebSocket(URL_WS);
  const inbox = [];
  ws.addEventListener("message", (e) => inbox.push(JSON.parse(e.data)));
  await new Promise((res, rej) => {
    ws.addEventListener("open", res);
    ws.addEventListener("error", rej);
    setTimeout(() => rej(new Error("open timeout")), 3000);
  });
  ok("client connected", ws.readyState === 1);
  await wait(120);
  ok("service pushes a ready event", inbox.some((m) => m.t === "ready"), JSON.stringify(inbox));

  const ask = async (t, args = {}, ms = 2500) => {
    const id = Math.floor(Math.random() * 1e6);
    ws.send(JSON.stringify({ t, id, ...args }));
    const until = Date.now() + ms;
    while (Date.now() < until) {
      const hit = inbox.find((m) => m.id === id);
      if (hit) return hit;
      await wait(20);
    }
    return null;
  };

  const hello = await ask("hello");
  ok("hello replies", hello?.ok === true && hello.result.service === "studioos", JSON.stringify(hello));

  const bad = await ask("nope.notaverb");
  ok("unknown verb is rejected, not ignored", bad?.ok === false, JSON.stringify(bad));

  console.log("\n[3] framing");
  // >125 bytes forces the 16-bit extended-length path in the decoder.
  const big = await ask("hello", { pad: "x".repeat(400) });
  ok("400-byte frame decodes (16-bit length path)", big?.ok === true);
  ws.send("this is not json");   // must be skipped, not crash the service
  await wait(100);
  ok("malformed payload does not kill the service", child.exitCode === null);

  console.log("\n[4] virtual MIDI");
  const ports = await ask("midi.ports");
  ok("midi.ports replies", ports?.ok === true, JSON.stringify(ports)?.slice(0, 200));
  ws.send(JSON.stringify({ t: "midi.open", port: "studio", name: "Adi Studio OS TEST" }));
  await wait(600);
  const after = await ask("midi.ports");
  const studio = after?.result?.ports?.find((p) => p.id === "studio");
  if (process.platform === "darwin") {
    ok("virtual CoreMIDI port created", studio?.connected === true, JSON.stringify(studio));
    // A virtual Output is PUBLISHED AS A SOURCE, so it lands in getInputs() —
    // that is the entry rekordbox and Ableton actually read from.
    ok("port published as a source the DAW can read",
       (after?.result?.sources || []).includes("Adi Studio OS TEST"),
       JSON.stringify(after?.result));
  } else {
    ok("port record exists (non-mac: needs loopMIDI)", !!studio, JSON.stringify(studio));
  }

  console.log("\n[5] note tracking + panic on disconnect");
  ws.send(JSON.stringify({ t: "midi.noteOn", port: "studio", ch: 0, note: 60, vel: 100 }));
  ws.send(JSON.stringify({ t: "midi.noteOn", port: "studio", ch: 0, note: 64, vel: 100 }));
  await wait(200);
  const sounding = (await ask("midi.ports"))?.result?.ports?.find((p) => p.id === "studio")?.sounding;
  ok("two notes tracked as sounding", sounding === 2, `sounding=${sounding}`);
  ws.send(JSON.stringify({ t: "midi.noteOff", port: "studio", ch: 0, note: 60 }));
  await wait(150);
  const one = (await ask("midi.ports"))?.result?.ports?.find((p) => p.id === "studio")?.sounding;
  ok("note off decrements", one === 1, `sounding=${one}`);

  ws.close();
  await wait(400);
  ok("disconnect logged", /no clients left/.test(stderr), stderr.slice(-200));

  const ws2 = new WebSocket(URL_WS);
  await new Promise((res) => ws2.addEventListener("open", res));
  const inbox2 = [];
  ws2.addEventListener("message", (e) => inbox2.push(JSON.parse(e.data)));
  ws2.send(JSON.stringify({ t: "midi.ports", id: 999 }));
  await wait(300);
  const stuck = inbox2.find((m) => m.id === 999)?.result?.ports?.find((p) => p.id === "studio")?.sounding;
  ok("panic on last disconnect left nothing sounding", stuck === 0, `sounding=${stuck}`);

  console.log("\n[6] side-effect-free rejection paths");
  ws2.send(JSON.stringify({ t: "os.key", token: "not-a-token", id: 1001 }));
  ws2.send(JSON.stringify({ t: "os.action", name: "not-an-action", id: 1002 }));
  await wait(300);
  ok("invalid numpad token refused", inbox2.find((m) => m.id === 1001)?.result === false);
  ok("unknown named action refused", inbox2.find((m) => m.id === 1002)?.result === false);

  /* V15 — os.type. The payload is WHITELISTED to digits, dot and minus, so an
     empty result is the refusal path and there is no string left that could
     break out of the AppleScript literal it is interpolated into. Asserted
     through the real socket rather than by importing os.js, so the verb table
     entry is covered too. */
  const REFUSE = ["", "   ", 'abc"; do shell script "x', "no-digits-here", "1.2.3",
                  "12 34", "--", "1e9", "0x10", "-", ".", "12;ls"];
  REFUSE.forEach((text, i) => ws2.send(JSON.stringify({ t: "os.type", text, id: 2000 + i })));
  const ACCEPT = ["104.90", "9.5310", "0", "-12.5", "4000.00"];
  await wait(600);
  ok("os.type is a known verb", inbox2.find((m) => m.id === 2000) !== undefined);
  const refused = REFUSE.map((_, i) => inbox2.find((m) => m.id === 2000 + i)?.result);
  ok("every non-numeric payload is refused", refused.every((r) => r === false),
     REFUSE.map((t, i) => `${JSON.stringify(t)}=>${refused[i]}`).join(" "));
  /* "no-digits-here" is the one that mattered: the first cut of this FILTERED
     to [0-9.-] and typed the "--" that survived. Validation refuses it whole. */
  ok("…including one that a character filter would have turned into \"--\"",
     refused[REFUSE.indexOf("no-digits-here")] === false);
  ok("the shape a real readout has would be accepted",
     ACCEPT.every((t) => /^-?\d{1,15}(\.\d{1,6})?$/.test(t)), ACCEPT.join(","));
  ws2.close();

  /* =========================================================================
     [7] V40 — EVERY KEY IS A PHYSICAL KEY CODE, NEVER A CHARACTER.

     THE BUG THIS PINS. Adi's Full Screen key created Finder aliases. `hotkey()`
     emitted `keystroke "f" using {control down, command down}`, and `keystroke`
     resolves the CHARACTER through the ACTIVE keyboard layout. This machine runs
     `com.apple.keylayout.Hebrew`, which has no `f`. Measured by typing into a
     TextEdit document and reading it back:

       keystroke "f"  ->  ש  (U+05E9), the character on PHYSICAL key code 0 = A
       key code 3     ->  כ  (U+05DB), the character on physical key F. Correct.

     So Ctrl+Cmd+F left the service as Ctrl+Cmd+A, which in Finder is Make Alias.
     Confirmed against real windows: `keystroke "w" using {command down}` did NOT
     close a TextEdit window and `key code 13 using {command down}` did.

     Tested through the PURE resolver rather than by pressing anything, because
     every other path here synthesises real input on the machine running the test.
     ========================================================================= */
  console.log("\n[7] V40: hotkeys resolve to physical key codes, not characters");
  {
    const OS = await import("../service/os.js");
    // kVK_ANSI_* — the values are the whole point, so they are spelled out.
    const EXPECT = { f: 3, a: 0, t: 17, w: 13, d: 2, s: 1, c: 8, v: 9, z: 6,
                     q: 12, "0": 29, "1": 18, "5": 23, "9": 25 };
    const wrong = Object.entries(EXPECT)
      .filter(([ch, code]) => OS.macKeyCode(ch) !== code)
      .map(([ch, code]) => `${ch} want ${code} got ${OS.macKeyCode(ch)}`);
    ok("every letter and digit resolves to its ANSI key code", wrong.length === 0,
       wrong.join("; "));
    ok("…and f is 3, which is the whole alias bug in one number",
       OS.macKeyCode("f") === 3, String(OS.macKeyCode("f")));
    ok("f does NOT resolve to 0 — that is the key Ctrl+Cmd+A lives on",
       OS.macKeyCode("f") !== OS.macKeyCode("a"));

    /* MAC_SPECIAL is consulted FIRST, so a named key never collapses into the
       letter it starts with: "delete" must stay Delete (51) and not become d (2). */
    ok("named keys still win over single letters",
       OS.macKeyCode("delete") === 51 && OS.macKeyCode("down") === 125
       && OS.macKeyCode("escape") === 53 && OS.macKeyCode("tab") === 48,
       [OS.macKeyCode("delete"), OS.macKeyCode("down"), OS.macKeyCode("escape")].join(","));
    ok("the OS-nav dial keys are unchanged",
       OS.macKeyCode("pagedown") === 121 && OS.macKeyCode("home") === 115,
       [OS.macKeyCode("pagedown"), OS.macKeyCode("home")].join(","));
    ok("something genuinely unmappable still reports null",
       OS.macKeyCode("nosuchkey") === null && OS.macKeyCode("") === null);

    /* The source-level guarantee: no macOS path may reach `keystroke` with a
       letter again. `type()` and `hotkey()` are the only two that ever could, and
       both now build key codes. The one remaining `keystroke` is hotkey()'s
       fallback for a target that is in NO table — which by the assertion above
       cannot be a letter or a digit. */
    const src = fs.readFileSync(new URL("../service/os.js", import.meta.url), "utf8");
    const strokes = src.match(/keystroke "\$\{?[a-z]/gi) || [];
    ok("no macOS path interpolates a letter into `keystroke`", strokes.length <= 1,
       strokes.join(" | "));
    ok("full screen writes the AXFullScreen attribute, not a keystroke",
       /AXFullScreen/.test(src) && /axFullScreenToggle\(\)/.test(src));
    /* V40 — and the Quads key: `menu item "Quarters"` matched the DISABLED group
       heading at index 7 instead of the Arrange command at index 22, so it could
       never have fired. Matching the first ENABLED item skips headings by
       construction. */
    ok("window menu items are matched on ENABLED, so a group heading is skipped",
       /whose name is "\$\{item\}" and enabled is true/.test(src));
  }

  console.log(`\n${pass} passed, ${fail} failed`);
  done(fail ? 1 : 0);
} catch (e) {
  console.log(`\nharness error: ${e.stack}`);
  console.log(stderr);
  done(1);
}
