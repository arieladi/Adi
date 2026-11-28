// SPDX-License-Identifier: AGPL-3.0-or-later

import { spawn } from "node:child_process";
import { once } from "node:events";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";
import { expect, it } from "vitest";

it("binds a provider check to its id and proxies tools through JSONL", async () => {
  const helper = fileURLToPath(new URL("../dist/main.js", import.meta.url));
  const child = spawn(process.execPath, [helper], { stdio: ["pipe", "pipe", "pipe"] });
  const exit = once(child, "exit");
  const lines = createInterface({ input: child.stdout, crlfDelay: Infinity });
  const frames = lines[Symbol.asyncIterator]();
  const send = (frame: Record<string, unknown>) => child.stdin.write(JSON.stringify(frame) + "\n");
  const receive = async () => {
    const frame = await frames.next();
    if (frame.done) throw new Error("agent stopped before its next frame");
    return JSON.parse(frame.value);
  };
  try {
    const provider = { kind: "faux", model: "faux", endpoint: "", vendor: "", credentialAccount: "" };
    send({ type: "check", id: "new-launch", provider });
    expect(await receive()).toEqual({ type: "ready", id: "new-launch", ok: true });
    send({ type: "run", id: "run", project: "bound-project", request: "Lower the bass", provider });
    for (const name of ["project_read", "ops_describe", "changeset_propose"]) {
      const tool = await receive();
      expect(tool).toMatchObject({ type: "tool", id: "run", name });
      expect(typeof tool.call).toBe("string");
      expect(tool.args).not.toHaveProperty("project");
      send({ type: "tool_result", call: tool.call, result: { head: 8 } });
    }
    expect(await receive()).toMatchObject({ type: "answer", id: "run" });
    expect(await receive()).toMatchObject({ type: "ended", id: "run", reason: "completed" });
    child.stdin.end();
    expect(await exit).toEqual([0, null]);
  } finally {
    child.kill();
    lines.close();
  }
});
