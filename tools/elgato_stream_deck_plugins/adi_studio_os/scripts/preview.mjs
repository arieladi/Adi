// Render the real surface to a static HTML sheet so key artwork can be judged
// without touching hardware. Loads the actual core + modules, so what you see is
// exactly what setImage would receive.
//
//   node scripts/preview.mjs [outfile]
import fs from "node:fs";
import path from "node:path";

const ROOT = new URL("../com.adiariel.studioos.sdPlugin/", import.meta.url).pathname;
const OUT = process.argv[2] || path.join(ROOT, "..", "preview.html");

global.window = global;
global.WebSocket = class { constructor() { this.readyState = 0; } send() {} close() {} };

for (const f of ["js/core/sd-client.js", "js/core/surface.js", "js/core/render.js",
                 "js/core/ipc.js", "js/core/input.js", "js/core/nav.js", "js/core/states.js",
                 "js/modules/root.js", "js/modules/console.js", "js/modules/index.js"]) {
  (0, eval)(fs.readFileSync(path.join(ROOT, f), "utf8"));
}

const { Surface: S, Render: R, States, Nav, Modules } = SOS;

// Pretend the service is up and drive the REAL probe path, so tile visibility in
// the preview is produced by the same code the device runs. Availability is read
// live from service/os.js rather than hardcoded, so this sheet reflects what is
// actually installed on this machine.
const { actionAvailability } = await import("../service/os.js");
const avail = actionAvailability();
SOS.IPC.isOnline = () => true;
SOS.IPC.ask = () => Promise.resolve(avail);

Modules.install();
await Modules.Root.refreshAvailability();
console.log("tiles shown:", Object.entries(avail).filter(([, v]) => v.available).map(([k]) => k).join(", ") || "(none)");

function grid(label, stateIndex) {
  States.setState(stateIndex);
  let cells = "";
  for (let row = 0; row < S.ROWS; row++) {
    for (let col = 0; col < S.COLS; col++) {
      const b = S.btn(col, row);
      // Use the SAME decorate + keySpec the device uses, so the sheet cannot
      // drift from what setImage actually receives (it did once: the preview
      // forwarded `size` while states.js dropped it).
      const bind = States.decorate(b, States.resolveKey(b));
      const svg = bind ? R.key(States.keySpec(bind)) : R.key({ dim: true });
      cells += `<div class="k" title="btn ${b} (c${col},r${row})">${svg}<span class="n">${b}</span></div>`;
    }
  }
  let zones = "";
  for (let d = 1; d <= S.DIALS; d++) {
    const z = States.resolveDial(d) || {};
    zones += `<div class="z">${R.zone({ title: z.title, value: z.value, sub: z.sub, indicator: z.indicator, color: z.color })}</div>`;
  }
  return `<section><h2>${label}</h2><div class="grid">${cells}</div><div class="strip">${zones}</div></section>`;
}

const html = `<!doctype html><meta charset="utf-8"><title>Studio OS surface preview</title>
<style>
 body{margin:0;padding:24px;background:#17191c;color:#e8edf2;
      font:13px/1.5 ui-monospace,Menlo,monospace}
 h1{font-size:16px;margin:0 0 4px} p.s{color:#8a9096;margin:0 0 22px}
 h2{font-size:13px;color:#6fe3c4;margin:26px 0 8px;letter-spacing:.05em;text-transform:uppercase}
 .grid{display:grid;grid-template-columns:repeat(9,72px);gap:5px}
 .k{position:relative;width:72px;height:72px}
 .k svg{width:72px;height:72px;display:block;border-radius:9px}
 .k .n{position:absolute;top:-1px;left:1px;font-size:8px;color:#4a5057}
 .strip{display:flex;gap:2px;margin-top:10px;width:calc(9*72px + 8*5px)}
 .z svg{width:100px;height:50px;display:block}
</style>
<h1>Studio OS — surface preview</h1>
<p class="s">Exactly what setImage receives. Stream Deck + XL: 36 keys (9&times;4) + 6 dial zones.</p>
${grid("Root Hub &middot; State 0 (Numpad) &mdash; the power-on view", 0)}
${grid("Root Hub &middot; State 1 (Calculator)", 1)}
${grid("State 2 (Delay Calculator) &mdash; full device", 2)}
`;

fs.writeFileSync(OUT, html);
console.log("wrote " + OUT);
