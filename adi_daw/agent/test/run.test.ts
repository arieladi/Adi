// SPDX-License-Identifier: AGPL-3.0-or-later

import { spawn } from "node:child_process";
import { createServer } from "node:http";
import type { IncomingMessage, Server, ServerResponse } from "node:http";
import { once } from "node:events";
import { setTimeout as wait } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { checkProvider, runAgent } from "../src/run.js";
import type { RunRequest, ToolName, ToolPort } from "../src/run.js";

const request = (model = "faux"): RunRequest => ({
  id: "run",
  project: "owned-project",
  request: "Lower the bass",
  provider: { kind: "faux", model },
});

const limits = { steps: 8, tokens: 32_000, wallMs: 2000, mutations: 8, identicalCalls: 2 };
const signal = () => new AbortController().signal;

function recordedPort() {
  const calls: { name: ToolName; args: Record<string, unknown> }[] = [];
  const port: ToolPort = async (name, args) => {
    calls.push({ name, args });
    return name === "project_read" ? { result: { head: 8, tracks: [{ name: "Bass" }] } }
      : { result: { status: "awaiting_review" } };
  };
  return { calls, port };
}

describe("the agent loop", () => {
  it("reads, checks the op schema and proposes a change through the parent", async () => {
    const { calls, port } = recordedPort();
    const result = await runAgent(request(), port, signal(), limits);
    expect(result).toMatchObject({ reason: "completed", text: "The bass change is ready to review.", steps: 4 });
    expect(result.tokens).toBeGreaterThan(0);
    expect(calls.map(call => call.name)).toEqual(["project_read", "ops_describe", "changeset_propose"]);
    expect(calls.every(call => !Object.hasOwn(call.args, "project"))).toBe(true);
  });

  it("stops before a third identical call at the same project head", async () => {
    const { calls, port } = recordedPort();
    expect((await runAgent(request("faux:repeat"), port, signal(), limits)).reason).toBe("repeated_tool_call");
    expect(calls).toHaveLength(2);
  });

  it("refuses every call in a batch containing a changeset", async () => {
    const { calls, port } = recordedPort();
    expect((await runAgent(request("faux:parallel"), port, signal(), limits)).reason).toBe("parallel_write");
    expect(calls).toHaveLength(0);
  });

  it("runs a batch of read tools sequentially and refuses unknown tools", async () => {
    let running = 0;
    let peak = 0;
    const calls: ToolName[] = [];
    const port: ToolPort = async name => {
      peak = Math.max(peak, ++running);
      calls.push(name);
      await wait(1);
      --running;
      return { result: { head: 8 } };
    };
    expect((await runAgent(request("faux:reads"), port, signal(), limits)).reason).toBe("completed");
    expect(peak).toBe(1);
    expect(calls).toEqual(["project_read", "ops_describe"]);
    expect((await runAgent(request("faux:unknown"), port, signal(), limits)).reason).toBe("completed");
    expect(calls).toHaveLength(2);
  });

  it("enforces model-step, token and mutation allowances", async () => {
    const { calls, port } = recordedPort();
    expect(await runAgent(request("faux:steps"), port, signal(), limits))
      .toMatchObject({ reason: "step_limit", steps: 8 });
    const before = calls.length;
    expect(await runAgent(request(), port, signal(), { ...limits, tokens: 1 }))
      .toMatchObject({ reason: "token_limit", steps: 0 });
    expect(calls).toHaveLength(before);
    expect((await runAgent(request(), port, signal(), { ...limits, mutations: 0 })).reason).toBe("mutation_limit");
    expect(calls.map(call => call.name)).not.toContain("changeset_propose");
  });

  it("aborts an outstanding tool when wall time expires or the caller cancels", async () => {
    let aborted = false;
    const pending: ToolPort = (_name, _args, abort) => new Promise(() => {
      abort.addEventListener("abort", () => { aborted = true; }, { once: true });
    });
    expect((await runAgent(request("faux:wait"), pending, signal(), { ...limits, wallMs: 400 })).reason)
      .toBe("wall_time_limit");
    expect(aborted).toBe(true);
    const cancellation = new AbortController();
    aborted = false;
    const cancelled: ToolPort = (name, args, abort) => {
      const work = pending(name, args, abort);
      setImmediate(() => cancellation.abort());
      return work;
    };
    expect((await runAgent(request("faux:wait"), cancelled, cancellation.signal, limits)).reason).toBe("cancelled");
    expect(aborted).toBe(true);
  });
});

async function listen(server: Server) {
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const address = server.address();
  if (!address || typeof address === "string") throw new Error("missing loopback address");
  return `http://127.0.0.1:${address.port}`;
}

function replyToProbe(request: IncomingMessage, response: ServerResponse) {
  request.resume();
  response.writeHead(200, { "Content-Type": "text/event-stream" });
  const chunk = { id: "chatcmpl-local", object: "chat.completion.chunk", model: "stub", created: 1 };
  response.write(`data: ${JSON.stringify({ ...chunk, choices: [
    { index: 0, delta: { role: "assistant", content: "OK" }, finish_reason: null },
  ] })}\n\n`);
  response.end(`data: ${JSON.stringify({ ...chunk, choices: [
    { index: 0, delta: {}, finish_reason: "stop" },
  ], usage: { prompt_tokens: 4, completion_tokens: 1, total_tokens: 5 } })}\n\ndata: [DONE]\n\n`);
}

describe("provider checks", () => {
  it.each(["local", "lan"] as const)("makes an actual OpenAI-compatible %s model request", async kind => {
    let requestedPath = "";
    const server = createServer((request, response) => {
      requestedPath = request.url ?? "";
      replyToProbe(request, response);
    });
    try {
      const endpoint = await listen(server);
      expect(await checkProvider({ kind, model: "stub", endpoint: endpoint + "/v1" },
        AbortSignal.timeout(2000))).toBe(true);
      expect(requestedPath).toBe("/v1/chat/completions");
    } finally {
      server.close();
    }
  });

  it.each([["local", "127.0.0.1"], ["lan", "localhost"]] as const)(
    "uses a direct %s loopback connection when Node starts with an ambient proxy", async (kind, host) => {
      let requests = 0;
      let proxyRequests = 0;
      const endpoint = createServer((request, response) => {
        ++requests;
        replyToProbe(request, response);
      });
      const proxy = createServer((_request, response) => {
        ++proxyRequests;
        response.writeHead(502).end();
      });
      proxy.on("connect", (_request, socket) => {
        ++proxyRequests;
        socket.end("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
      });
      try {
        const providerUrl = await listen(endpoint);
        const proxyUrl = await listen(proxy);
        const destination = new URL("/v1", providerUrl);
        destination.hostname = host;
        const helper = fileURLToPath(new URL("../dist/main.js", import.meta.url));
        const child = spawn(process.execPath, [helper], {
          env: { ...process.env, NODE_USE_ENV_PROXY: "1", NO_PROXY: "", no_proxy: "",
            HTTP_PROXY: proxyUrl, HTTPS_PROXY: proxyUrl, http_proxy: proxyUrl, https_proxy: proxyUrl },
          stdio: ["pipe", "pipe", "pipe"],
          timeout: 4000,
        });
        const exit = once(child, "close");
        let stdout = "";
        child.stdout.setEncoding("utf8").on("data", (part: string) => { stdout += part; });
        child.stdin.end(JSON.stringify({ type: "check", id: "direct-loopback",
          provider: { kind, model: "stub", endpoint: destination.href } }) + "\n");
        expect(await exit).toEqual([0, null]);
        expect(proxyRequests).toBe(0);
        expect(requests).toBe(1);
        expect(JSON.parse(stdout)).toEqual({ type: "ready", id: "direct-loopback", ok: true });
      } finally {
        endpoint.close();
        proxy.close();
      }
    });

  it("never follows a redirect or treats an off-machine server as local", async () => {
    let redirects = 0;
    let redirectedRequests = 0;
    const otherOrigin = createServer((_request, response) => {
      ++redirectedRequests;
      response.writeHead(500).end();
    });
    const redirectedEndpoint = await listen(otherOrigin);
    const server = createServer((_request, response) => {
      ++redirects;
      response.writeHead(307, { Location: redirectedEndpoint + "/others" }).end();
    });
    try {
      const endpoint = await listen(server);
      expect(await checkProvider({ kind: "local", model: "stub", endpoint }, AbortSignal.timeout(2000))).toBe(false);
      expect(redirects).toBe(1);
      expect(redirectedRequests).toBe(0);
      expect(await checkProvider({ kind: "local", model: "stub", endpoint: "https://example.invalid/v1" }, signal()))
        .toBe(false);
    } finally {
      server.close();
      otherOrigin.close();
    }
  });
});
