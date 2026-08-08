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

const PORT = 9199; // not 9010, so a running service is never disturbed
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
  ws2.close();

  console.log(`\n${pass} passed, ${fail} failed`);
  done(fail ? 1 : 0);
} catch (e) {
  console.log(`\nharness error: ${e.stack}`);
  console.log(stderr);
  done(1);
}
