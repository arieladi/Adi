/*
 * background.js — adi spell chekcer — MV3 service worker
 * ----------------------------------------------------------------------------
 * The API layer. content.js sends the focused field's text; this worker POSTs
 * it to the free LanguageTool endpoint and returns the raw matches array.
 *
 *   POST https://api.languagetool.org/v2/check
 *   body: language=auto&text=<user input>
 *
 * Free-tier limits handled here:
 *   • HTTP 429  -> { error: "rate" }   (20 requests/minute)
 *   • > 20k chars -> { error: "long" } (rejected before sending)
 *   • Unsupported language (LanguageTool has no Hebrew: detection comes back
 *     as "NoopLanguage", code zz — probed live) -> { error: "lang" }
 * ----------------------------------------------------------------------------
 */
"use strict";

const LT_URL = "https://api.languagetool.org/v2/check";
const MAX_CHARS = 20000; // free-API hard cap per request

/** POST the text to LanguageTool; resolve to {matches} or {error}. */
async function checkText(text) {
  if (text.length > MAX_CHARS) return { error: "long" };

  let res;
  try {
    res = await fetch(LT_URL, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams({ language: "auto", text })
    });
  } catch (e) {
    return { error: "network" };
  }

  if (res.status === 429) return { error: "rate" };
  if (!res.ok) return { error: "http-" + res.status };

  let data;
  try {
    data = await res.json();
  } catch (e) {
    return { error: "parse" };
  }

  // LanguageTool can't check this language (e.g. Hebrew): detection = zz.
  const code = data.language && data.language.code;
  if (code && code.startsWith("zz")) return { error: "lang" };

  return { matches: Array.isArray(data.matches) ? data.matches : [] };
}

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg || msg.type !== "asc-check" || typeof msg.text !== "string") return;
  checkText(msg.text)
    .then(sendResponse)
    .catch(() => sendResponse({ error: "network" }));
  return true; // keep the message channel open for the async response
});
