// Smart-home / room lighting.
//
// D12 ruling: no system chosen yet, so dial 4 is INERT. The verb, the handler
// and the driver seam are built anyway, so picking Hue / Home Assistant / Elgato
// later is filling in one driver rather than adding a feature — which is the
// whole point of deciding "not yet" instead of "not at all".
//
// Config is read from ~/.studioos/home.json so a driver can be pointed at a
// bridge without editing or reinstalling the service:
//   { "driver": "hue", "host": "192.168.1.x", "token": "...", "group": "1" }
import fs from "node:fs";
import path from "node:path";
import os from "node:os";

let log = console;
export function setLogger(l) { log = l; }

const CONFIG = path.join(os.homedir(), ".studioos", "home.json");

let cfg = null;
let warned = false;

function config() {
  if (cfg) return cfg;
  try { cfg = JSON.parse(fs.readFileSync(CONFIG, "utf8")); }
  catch { cfg = { driver: "none" }; }
  return cfg;
}

// Drivers are plain async functions of (level 0-100, cfg). Add one here and set
// "driver" in the config file; nothing else changes.
const DRIVERS = {
  none: async () => false,

  // Philips Hue local bridge API — no cloud, no account.
  hue: async (level, c) => {
    const bri = Math.round((clamp(level) / 100) * 254);
    const url = `http://${c.host}/api/${c.token}/groups/${c.group ?? 0}/action`;
    const res = await fetch(url, {
      method: "PUT",
      body: JSON.stringify({ on: bri > 0, bri: Math.max(1, bri) }),
    });
    return res.ok;
  },

  // Home Assistant REST API — drives whatever HA already controls.
  homeassistant: async (level, c) => {
    const res = await fetch(`${c.host}/api/services/light/turn_on`, {
      method: "POST",
      headers: { authorization: `Bearer ${c.token}`, "content-type": "application/json" },
      body: JSON.stringify({ entity_id: c.entity, brightness_pct: clamp(level) }),
    });
    return res.ok;
  },

  // Elgato Key Light / Light Strip — local HTTP on port 9123.
  elgato: async (level, c) => {
    const res = await fetch(`http://${c.host}:9123/elgato/lights`, {
      method: "PUT",
      body: JSON.stringify({ numberOfLights: 1, lights: [{ on: level > 0 ? 1 : 0, brightness: clamp(level) }] }),
    });
    return res.ok;
  },
};

function clamp(n) { return Math.max(0, Math.min(100, Math.round(Number(n) || 0))); }

export async function dim(level) {
  const c = config();
  const driver = DRIVERS[c.driver] || DRIVERS.none;
  if (driver === DRIVERS.none) {
    if (!warned) {
      warned = true;
      log.info?.(`lighting is inert (D12) — write ${CONFIG} with a driver to enable dial 4`);
    }
    return false;
  }
  try { return await driver(clamp(level), c); }
  catch (e) { log.error?.(`lighting "${c.driver}" failed: ${e?.message ?? e}`); return false; }
}

export function status() {
  const c = config();
  return { driver: c.driver ?? "none", configured: c.driver && c.driver !== "none", config: CONFIG };
}
