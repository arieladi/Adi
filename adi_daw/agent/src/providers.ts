// SPDX-License-Identifier: AGPL-3.0-or-later

import { lookup } from "node:dns";
import type { StreamFn } from "@earendil-works/pi-agent-core";
import {
  createModels, fauxAssistantMessage, fauxProvider, fauxToolCall,
} from "@earendil-works/pi-ai";
import type { Model } from "@earendil-works/pi-ai";
import { Agent } from "undici";
import { streamSimple as streamCompatible } from "@earendil-works/pi-ai/api/openai-completions";
import { anthropicProvider } from "@earendil-works/pi-ai/providers/anthropic";
import { googleProvider } from "@earendil-works/pi-ai/providers/google";
import { openaiProvider } from "@earendil-works/pi-ai/providers/openai";
import { injectionResponses } from "./injection_fixtures.js";

export interface ProviderConfig {
  kind: "local" | "lan" | "cloud" | "faux";
  model: string;
  endpoint?: string;
  vendor?: string;
  credentialAccount?: string;
}

export interface Provider {
  model: Model<string>;
  stream: StreamFn;
}

const keychainService = "org.adidaw.agent";
const providerConnections = new Agent({
  connect: {
    // Pin localhost without changing the URL's host or its TLS server name.
    lookup: (hostname, options, callback) =>
      lookup(hostname === "localhost" ? "127.0.0.1" : hostname, options, callback),
  },
});
const proposal = {
  baseHead: 8,
  request: "Lower the bass",
  ops: [{ op: "mixer.setVolume", payload: { id: 3, db: -6 } }],
};

function fauxResponses(model: string) {
  const read = () => fauxAssistantMessage(fauxToolCall("project_read", { level: 0 }));
  switch (model) {
    case "faux":
      return [
        read(),
        fauxAssistantMessage(fauxToolCall("ops_describe", { names: ["mixer.setVolume"] })),
        fauxAssistantMessage(fauxToolCall("changeset_propose", proposal)),
        fauxAssistantMessage("The bass change is ready to review."),
      ];
    case "faux:repeat": return [read(), read(), read()];
    case "faux:wait": return [read()];
    case "faux:reads": return [
      fauxAssistantMessage([fauxToolCall("project_read", {}), fauxToolCall("ops_describe", {})]),
      fauxAssistantMessage("The project is ready to read."),
    ];
    case "faux:unknown": return [
      fauxAssistantMessage(fauxToolCall("shell", {})),
      fauxAssistantMessage("The tool was refused."),
    ];
    case "faux:steps": return Array.from({ length: 9 }, (_, index) =>
      fauxAssistantMessage(fauxToolCall("ops_describe", { query: "mixer." + index })));
    case "faux:parallel": return [
      fauxAssistantMessage([fauxToolCall("project_read", {}), fauxToolCall("changeset_propose", proposal)]),
    ];
    default: {
      const injection = injectionResponses(model);
      if (injection) return injection;
      throw new Error("unconfigured faux model");
    }
  }
}

function onlyOrigin(origin: string): typeof globalThis.fetch {
  return (input, options) => {
    const url = new URL(input instanceof Request ? input.url : input);
    if (url.origin !== origin) throw new Error("provider request left the consented origin");
    // A loopback URL is exempt from egress consent. An explicit direct
    // dispatcher keeps Node's ambient proxy configuration out of the route.
    const direct = { ...options, redirect: "error" as const, dispatcher: providerConnections };
    return fetch(input, direct);
  };
}

async function loadKey(account: string | undefined, signal: AbortSignal) {
  if (!account) return undefined;
  const { AsyncEntry } = await import("@napi-rs/keyring");
  const entry = new AsyncEntry(keychainService, account, { linux: { store: "secret-service" } });
  const password = await entry.getPassword(signal);
  if (!password) throw new Error("provider credential unavailable");
  return password;
}

function endpointOf(config: ProviderConfig): URL {
  if (!config.endpoint) throw new Error("provider needs an endpoint");
  const endpoint = new URL(config.endpoint);
  if ((endpoint.protocol !== "http:" && endpoint.protocol !== "https:") ||
      endpoint.username || endpoint.password || endpoint.search || endpoint.hash)
    throw new Error("invalid provider endpoint");
  if (config.kind === "local") {
    if (endpoint.hostname !== "localhost" && !endpoint.hostname.startsWith("127.") &&
        endpoint.hostname !== "[::1]")
      throw new Error("a local provider requires a loopback address");
    // A hostname beginning with 127. is not an IP literal.
    if (endpoint.hostname.startsWith("127.") && !/^127\.(?:\d{1,3}\.){2}\d{1,3}$/.test(endpoint.hostname))
      throw new Error("a local provider requires a loopback address");
  }
  return endpoint;
}

function compatibleModel(config: ProviderConfig, endpoint: URL): Model<"openai-completions"> {
  return {
    id: config.model,
    name: config.model,
    api: "openai-completions",
    provider: "adi-compatible",
    baseUrl: endpoint.href.replace(/\/+$/, ""),
    reasoning: false,
    input: ["text"],
    contextWindow: 32_000,
    maxTokens: 8_192,
    cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
    compat: { supportsStore: false, supportsDeveloperRole: false },
  };
}

export async function resolveProvider(config: ProviderConfig, signal: AbortSignal, probe = false): Promise<Provider> {
  if (config.kind === "faux") {
    const responses = fauxResponses(config.model);
    const faux = fauxProvider({ provider: "adi-faux", models: [{ id: config.model }] });
    const models = createModels();
    models.setProvider(faux.provider);
    faux.setResponses(probe ? [fauxAssistantMessage("OK")] : responses);
    return { model: faux.getModel(), stream: models.streamSimple.bind(models) };
  }

  if (config.kind === "local" || config.kind === "lan") {
    const endpoint = endpointOf(config);
    const model = compatibleModel(config, endpoint);
    const apiKey = await loadKey(config.credentialAccount, signal) ?? "adi-compatible-endpoint";
    const fetch = onlyOrigin(endpoint.origin);
    return {
      model,
      stream: (_model, context, options) => streamCompatible(model, context, {
        ...options, apiKey, fetch, cacheRetention: "none", maxRetries: 0, transport: "sse",
      }),
    };
  }

  if (config.endpoint || !config.credentialAccount) throw new Error("cloud provider needs a keychain account");
  const available = { openai: openaiProvider, anthropic: anthropicProvider, google: googleProvider };
  const vendor = config.vendor ?? "openai";
  if (vendor !== "openai" && vendor !== "anthropic" && vendor !== "google")
    throw new Error("unknown cloud provider");
  const models = createModels();
  models.setProvider(available[vendor]());
  const model = models.getModel(vendor, config.model);
  if (!model) throw new Error("unknown cloud model");
  const apiKey = await loadKey(config.credentialAccount, signal);
  if (!apiKey) throw new Error("provider credential unavailable");
  const fetch = onlyOrigin(new URL(model.baseUrl).origin);
  return {
    model,
    stream: (selected, context, options) => models.streamSimple(selected, context, {
      ...options, apiKey, fetch, cacheRetention: "none", maxRetries: 0, transport: "sse",
    }),
  };
}
