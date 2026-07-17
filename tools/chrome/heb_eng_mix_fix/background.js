/*
 * background.js — HEB ENG MIX FIX (v3.1) — MV3 service worker
 * ----------------------------------------------------------------------------
 * The spell-check engine. content.js sends the text the user paused on; this
 * worker queries the Google Suggest API with the WHOLE phrase (single words
 * only get autocomplete noise — "helo"→"helos" — but phrases get real spelling
 * corrections: "helo wrold how are yu" → "hello world how are you"), then
 * word-diffs the answer against the input and returns ONLY the per-word fixes:
 *
 *   [{ start, end, text }, …]   — char offsets into the sent phrase
 *
 * The diff is an LCS alignment over normalized words, so Google's habit of
 * APPENDING words ("…spam emails") or DROPPING them never leaks into fixes —
 * pure insertions/deletions are skipped, only replacement blocks with high
 * similarity survive (e.g. "recieve alot" → "receive a lot").
 *
 * Charset gotcha (probed live): with hl=iw the API responds in windows-1255,
 * NOT UTF-8 — res.json() would mangle Hebrew, so we decode the raw bytes.
 * ----------------------------------------------------------------------------
 */
"use strict";

const SUGGEST_URL =
  "https://suggestqueries.google.com/complete/search?client=chrome&hl=iw&q=";

// ---- pure helpers (Node-testable) -------------------------------------------

/** Lowercase + strip edge punctuation, for word comparison. */
function normWord(w) {
  return w.replace(/^[^\p{L}\p{N}]+|[^\p{L}\p{N}]+$/gu, "").toLowerCase();
}

/** Classic Levenshtein distance (short strings only). */
function levenshtein(a, b) {
  const m = a.length, n = b.length;
  if (!m) return n;
  if (!n) return m;
  let prev = Array.from({ length: n + 1 }, (_, i) => i);
  for (let i = 1; i <= m; i++) {
    const cur = [i];
    for (let j = 1; j <= n; j++) {
      cur[j] = Math.min(
        prev[j] + 1,
        cur[j - 1] + 1,
        prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1)
      );
    }
    prev = cur;
  }
  return prev[n];
}

/**
 * Tokenize a phrase into words with CORE character offsets (edge punctuation
 * excluded, so replacing "reprot" inside "reprot." keeps the period).
 */
function tokenize(phrase) {
  const words = [];
  const re = /\S+/g;
  let m;
  while ((m = re.exec(phrase))) {
    const raw = m[0];
    const lead = raw.match(/^[^\p{L}\p{N}]*/u)[0].length;
    const trail = raw.match(/[^\p{L}\p{N}]*$/u)[0].length;
    const core = raw.slice(lead, raw.length - trail);
    if (!core) continue; // pure punctuation — not a word
    words.push({
      norm: core.toLowerCase(),
      start: m.index + lead,
      end: m.index + raw.length - trail
    });
  }
  return words;
}

/**
 * Word-level LCS diff of `original` vs Google's `suggestion`.
 * Returns per-word replacement fixes (offsets into `original`), or null.
 *
 *   • matched words       -> untouched
 *   • suggestion-only run -> skipped (that's autocomplete padding)
 *   • original-only run   -> skipped (Google dropped it; keep the user's word)
 *   • replacement block   -> a fix, but only when the two sides are ≥50%
 *                            similar (rejects unrelated popular queries)
 */
function diffFixes(original, suggestion) {
  if (typeof suggestion !== "string" || !suggestion) return null;
  const o = tokenize(original);
  const s = suggestion.split(/\s+/).filter(Boolean)
    .map((w) => ({ raw: w.replace(/^[^\p{L}\p{N}]+|[^\p{L}\p{N}]+$/gu, ""), norm: normWord(w) }))
    .filter((w) => w.norm);
  if (!o.length || !s.length) return null;

  // LCS table over normalized words.
  const m = o.length, n = s.length;
  const dp = Array.from({ length: m + 1 }, () => new Array(n + 1).fill(0));
  for (let i = m - 1; i >= 0; i--) {
    for (let j = n - 1; j >= 0; j--) {
      dp[i][j] = o[i].norm === s[j].norm
        ? dp[i + 1][j + 1] + 1
        : Math.max(dp[i + 1][j], dp[i][j + 1]);
    }
  }

  // Walk the alignment; gaps between matches become candidate blocks.
  const fixes = [];
  let i = 0, j = 0, bi = 0, bj = 0; // block start cursors
  function flushBlock(oEnd, sEnd) {
    const oWords = o.slice(bi, oEnd);
    const sWords = s.slice(bj, sEnd);
    if (oWords.length && sWords.length) {
      const suggText = sWords.map((w) => w.raw).join(" ");
      const b = suggText.toLowerCase();

      // Google often DROPS words inside a block ("the spel why"→"the spell").
      // Blaming the whole block would swallow the dropped word, so pick the
      // contiguous sub-range of original words that best matches the
      // suggestion and leave the rest untouched.
      let best = null;
      for (let from = 0; from < oWords.length; from++) {
        for (let to = from; to < oWords.length; to++) {
          const a = original
            .slice(oWords[from].start, oWords[to].end)
            .toLowerCase();
          const dist = levenshtein(a, b);
          const ratio = dist / Math.max(a.length, b.length);
          if (!best || ratio < best.ratio) {
            best = { from, to, dist, ratio };
          }
        }
      }
      if (best && best.dist > 0 && best.ratio <= 0.4) {
        fixes.push({
          start: oWords[best.from].start,
          end: oWords[best.to].end,
          text: suggText
        });
      }
    }
    bi = oEnd; bj = sEnd;
  }
  while (i < m && j < n) {
    if (o[i].norm === s[j].norm) {
      flushBlock(i, j);
      i++; j++; bi = i; bj = j;
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      i++;
    } else {
      j++;
    }
  }
  flushBlock(m, n);

  return fixes.length ? fixes.slice(0, 6) : null;
}

// ---- fetching -----------------------------------------------------------------

/** Query the Suggest API for `text`, diff, and return fixes (or null). */
async function fetchFixes(text) {
  const res = await fetch(SUGGEST_URL + encodeURIComponent(text));
  if (!res.ok) return null;

  const charset =
    (/charset=([\w-]+)/i.exec(res.headers.get("content-type") || "") ||
      [])[1] || "utf-8";
  let raw;
  try {
    raw = new TextDecoder(charset).decode(await res.arrayBuffer());
  } catch (e) {
    return null; // unknown charset label — bail rather than mangle Hebrew
  }

  let data;
  try {
    data = JSON.parse(raw);
  } catch (e) {
    return null;
  }

  const first =
    Array.isArray(data) && Array.isArray(data[1]) &&
    typeof data[1][0] === "string"
      ? data[1][0]
      : null;
  return diffFixes(text, first);
}

// ---- chrome wiring (skipped in Node tests) --------------------------------------

if (typeof chrome !== "undefined" && chrome.runtime) {
  chrome.runtime.onInstalled.addListener((details) => {
    if (details.reason === "install") {
      chrome.storage.local.set({ enabled: true, manual: false, online: true });
    }
  });

  chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
    if (!msg || msg.type !== "hebfix-suggest" || typeof msg.text !== "string") {
      return;
    }
    fetchFixes(msg.text)
      .then(sendResponse)
      .catch(() => sendResponse(null));
    return true; // keep the message channel open for the async response
  });
}

// Node test hook.
if (typeof module !== "undefined" && module.exports) {
  module.exports = { diffFixes, fetchFixes, tokenize, levenshtein, normWord };
}
