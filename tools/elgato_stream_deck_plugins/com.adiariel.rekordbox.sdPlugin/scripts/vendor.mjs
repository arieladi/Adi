// Vendor the runtime MIDI stack into vendor/node_modules (COMMITTED).
//
// Why: easymidi -> @julusian/midi loads a native .node binding via
// pkg-prebuilds, so it cannot be bundled by rollup. The Stream Deck app runs
// bin/plugin.js with its own Node 20; the plugin resolves these packages at
// runtime through createRequire(<plugin>/vendor/_resolve_.cjs). Committing
// the prebuilt binaries means end users never run npm or node-gyp.
//
// Run after `npm install` (or after bumping easymidi/@julusian/midi):
//   node scripts/vendor.mjs
import { cpSync, rmSync, mkdirSync, writeFileSync, existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const SRC = path.join(ROOT, "node_modules");
const DST = path.join(ROOT, "vendor", "node_modules");

// Ship only the platforms the manifest supports (mac 13+, windows 11+).
const KEEP_PREBUILDS = ["midi-darwin-arm64", "midi-darwin-x64", "midi-win32-x64", "midi-win32-arm64"];

function need(rel) {
  const p = path.join(SRC, rel);
  if (!existsSync(p)) {
    console.error(`missing ${rel} — run npm install first`);
    process.exit(1);
  }
  return p;
}

rmSync(DST, { recursive: true, force: true });
mkdirSync(DST, { recursive: true });

// easymidi — pure JS sugar over @julusian/midi (only runtime files).
for (const f of ["index.js", "package.json", "LICENSE"]) {
  cpSync(path.join(need("easymidi"), f), path.join(DST, "easymidi", f));
}

// @julusian/midi — JS + the prebuilt native bindings we ship.
const JM = "@julusian/midi";
for (const f of ["midi.js", "binding-options.js", "package.json", "LICENSE", "lib"]) {
  cpSync(path.join(need(JM), f), path.join(DST, JM, f), { recursive: true });
}
for (const pb of KEEP_PREBUILDS) {
  const from = path.join(need(JM), "prebuilds", pb);
  if (!existsSync(from)) {
    console.error(`prebuild ${pb} missing from ${JM} — package layout changed?`);
    process.exit(1);
  }
  cpSync(from, path.join(DST, JM, "prebuilds", pb), { recursive: true });
}

// pkg-prebuilds — tiny runtime loader used by @julusian/midi.
for (const f of ["bindings.js", "lib", "package.json", "LICENSE"]) {
  cpSync(path.join(need("pkg-prebuilds"), f), path.join(DST, "pkg-prebuilds", f), { recursive: true });
}

// Anchor file for createRequire — resolution starts here, so require(...)
// finds vendor/node_modules first. Never executed.
writeFileSync(
  path.join(ROOT, "vendor", "_resolve_.cjs"),
  "// Anchor for createRequire — makes require() resolve from vendor/node_modules.\n" +
  "// Regenerate this tree with: node scripts/vendor.mjs\n"
);

console.log("vendored ->", path.relative(process.cwd(), DST));
console.log("prebuilds:", KEEP_PREBUILDS.join(", "));
