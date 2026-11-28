#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later

import { stdin, stdout } from "node:process";
import { checkProvider, runAgent } from "./run.js";
import type { ToolPort, ToolReply } from "./run.js";
import type { ProviderConfig } from "./providers.js";

const maxLine = 4 * 1024 * 1024;
const checkTimeoutMs = 15_000;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);
const boundedText = (value: unknown, limit: number): value is string =>
  typeof value === "string" && value.length > 0 && value.length <= limit;

function providerConfig(value: unknown): ProviderConfig | undefined {
  if (!isRecord(value) || !boundedText(value.model, 256)) return undefined;
  const { kind, model } = value;
  const endpoint = value.endpoint === "" ? undefined : value.endpoint;
  const vendor = value.vendor === "" ? undefined : value.vendor;
  const credentialAccount = value.credentialAccount === "" ? undefined : value.credentialAccount;
  if (kind !== "local" && kind !== "lan" && kind !== "cloud" && kind !== "faux") return undefined;
  if (endpoint !== undefined && !boundedText(endpoint, 2048)) return undefined;
  if (vendor !== undefined && !boundedText(vendor, 64)) return undefined;
  if (credentialAccount !== undefined && !boundedText(credentialAccount, 256)) return undefined;
  return { kind, model, endpoint, vendor, credentialAccount };
}

async function* inputLines(): AsyncGenerator<string> {
  let parts: Buffer[] = [];
  let bytes = 0;
  let discarded = false;
  for await (const chunk of stdin as AsyncIterable<Buffer>) {
    let start = 0;
    while (start < chunk.length) {
      const end = chunk.indexOf(10, start);
      const part = chunk.subarray(start, end < 0 ? chunk.length : end);
      bytes += part.length;
      if (bytes > maxLine) {
        parts = [];
        discarded = true;
      } else if (!discarded) {
        parts.push(part);
      }
      if (end < 0) break;
      if (!discarded) yield Buffer.concat(parts, bytes).toString("utf8");
      parts = [];
      bytes = 0;
      discarded = false;
      start = end + 1;
    }
  }
  if (!discarded && bytes > 0) yield Buffer.concat(parts, bytes).toString("utf8");
}

function parseFrame(line: string) {
  let depth = 0;
  let quoted = false;
  let escaped = false;
  for (const letter of line) {
    if (quoted) {
      if (!escaped && letter === '"') quoted = false;
      escaped = !escaped && letter === "\\";
    } else if (letter === '"') {
      quoted = true;
    } else if (letter === "{" || letter === "[") {
      if (++depth > 64) return undefined;
    } else if (letter === "}" || letter === "]") {
      --depth;
    }
  }
  try {
    const value: unknown = JSON.parse(line);
    return isRecord(value) ? value : undefined;
  } catch {
    return undefined;
  }
}

function send(value: Record<string, unknown>) {
  stdout.write(JSON.stringify(value) + "\n");
}

class Session {
  private callSequence = 0;
  private active?: { id: string; kind: "check" | "run"; abort: AbortController; done: Promise<void> };
  private pending?: { call: string; reply: (result: ToolReply) => void };

  async close() {
    if (this.active?.kind === "run") this.active.abort.abort();
    await this.active?.done;
  }

  handle(frame: Record<string, unknown>) {
    if (frame.type === "cancel") {
      if (this.active?.kind === "run" && this.active.id === frame.id) this.active.abort.abort();
      return;
    }
    if (frame.type === "tool_result") {
      if (this.pending && this.pending.call === frame.call) {
        this.pending.reply({ result: frame.result, error: frame.error ?? undefined });
      }
      return;
    }
    if ((frame.type !== "check" && frame.type !== "run") || !boundedText(frame.id, 128)) return;
    const { id, type } = frame;
    const provider = providerConfig(frame.provider);
    if (type === "check") {
      if (this.active || !provider) {
        send({ type: "ready", id, ok: false });
        return;
      }
      const abort = new AbortController();
      const signal = AbortSignal.any([abort.signal, AbortSignal.timeout(checkTimeoutMs)]);
      const done = checkProvider(provider, signal)
        .then(ok => send({ type: "ready", id, ok }))
        .finally(() => { this.active = undefined; });
      this.active = { kind: "check", id, abort, done };
      return;
    }

    const { request, project } = frame;
    if (this.active || !provider || !boundedText(request, 64 * 1024) || !boundedText(project, 128)) {
      send({ type: "ended", id, reason: this.active ? "busy" : "invalid_request", steps: 0, tokens: 0 });
      return;
    }
    const abort = new AbortController();
    const done = runAgent({ id, request, project, provider }, this.toolPort(id), abort.signal)
      .then(({ reason, steps, tokens, text }) => {
        if (text !== undefined) send({ type: "answer", id, text });
        send({ type: "ended", id, reason, steps, tokens });
      }).finally(() => { this.active = undefined; });
    this.active = { kind: "run", id, abort, done };
  }

  private toolPort(id: string): ToolPort {
    return (name, args, signal) => new Promise<ToolReply>(resolve => {
      if (signal.aborted || this.pending) {
        resolve({ error: "tool request unavailable" });
        return;
      }
      const call = String(++this.callSequence);
      const reply = (result: ToolReply) => {
        this.pending = undefined;
        signal.removeEventListener("abort", cancelled);
        resolve(result);
      };
      const cancelled = () => reply({ error: "tool request cancelled" });
      this.pending = { call, reply };
      signal.addEventListener("abort", cancelled, { once: true });
      send({ type: "tool", id, call, name, args });
    });
  }
}

async function main() {
  const session = new Session();
  for await (const line of inputLines()) {
    const frame = parseFrame(line);
    if (frame) session.handle(frame);
  }
  await session.close();
}

void main().catch(() => { process.exitCode = 1; });
