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
 * Word-level LCS diff of `original` chunk vs Google's `suggestion`.
 * Returns an array of granular "correction objects", or null:
 *
 *   { original, corrected, index, start, end }
 *     original  — the exact substring of the chunk to replace
 *     corrected — what to replace it with
 *     index     — word index within the chunk (indexInChunk, per spec)
 *     start,end — char offsets within the chunk (used by content.js for
 *                 precise absolute positioning; ambiguity-free vs. index)
 *
 * Alignment rules (LCS over normalized words):
 *   • matched words          -> untouched
 *   • suggestion-only run     -> skipped (autocomplete padding like "spam")
 *   • original-only run       -> skipped (Google dropped a word; keep it)
 *   • replacement block:
 *       - equal word counts   -> emit ONE correction PER mismatched word
 *                                (maximum granularity, the common typo case)
 *       - unequal counts      -> emit the best-matching sub-range as one
 *                                correction ("recieve alot" -> "receive a lot"),
 *                                only if ≥60% similar (rejects unrelated queries)
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

  const fixes = [];
  let i = 0, j = 0, bi = 0, bj = 0; // block start cursors

  function pushFix(word, corrected) {
    if (word.norm === normWord(corrected)) return; // no real change
    fixes.push({
      original: original.slice(word.start, word.end),
      corrected,
      index: o.indexOf(word),
      start: word.start,
      end: word.end
    });
  }

  function flushBlock(oEnd, sEnd) {
    const oWords = o.slice(bi, oEnd);
    const sWords = s.slice(bj, sEnd);
    bi = oEnd; bj = sEnd;
    if (!oWords.length || !sWords.length) return; // pure insert/delete: skip

    // Equal counts -> per-word corrections (the granular, common case).
    if (oWords.length === sWords.length) {
      for (let k = 0; k < oWords.length; k++) {
        const a = oWords[k].norm, b = sWords[k].norm;
        // Only accept a word pair that is genuinely a typo of each other.
        if (a !== b && levenshtein(a, b) / Math.max(a.length, b.length) <= 0.5) {
          pushFix(oWords[k], sWords[k].raw);
        }
      }
      return;
    }

    // Unequal counts (merge/split) -> best-matching contiguous sub-range.
    const suggText = sWords.map((w) => w.raw).join(" ");
    const b = suggText.toLowerCase();
    let best = null;
    for (let from = 0; from < oWords.length; from++) {
      for (let to = from; to < oWords.length; to++) {
        const a = original.slice(oWords[from].start, oWords[to].end).toLowerCase();
        const ratio = levenshtein(a, b) / Math.max(a.length, b.length);
        if (!best || ratio < best.ratio) best = { from, to, ratio };
      }
    }
    if (best && best.ratio > 0 && best.ratio <= 0.4) {
      fixes.push({
        original: original.slice(oWords[best.from].start, oWords[best.to].end),
        corrected: suggText,
        index: o.indexOf(oWords[best.from]),
        start: oWords[best.from].start,
        end: oWords[best.to].end
      });
    }
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

  return fixes.length ? fixes.slice(0, 8) : null;
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
