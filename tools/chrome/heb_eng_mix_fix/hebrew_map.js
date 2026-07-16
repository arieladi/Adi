/*
 * hebrew_map.js
 * ----------------------------------------------------------------------------
 * Shared data + pure helper functions for HEB ENG MIX FIX.
 *
 * This file is loaded as the first content script (before content.js) and also
 * pulled into a Node context by the test harness. It therefore attaches its API
 * to a single namespace object and works both in the browser (window) and in
 * Node (module.exports), without any DOM dependencies.
 *
 * Everything here is PURE (no DOM, no events) so it is trivially testable and
 * cheap to call on every keystroke boundary.
 * ----------------------------------------------------------------------------
 */
(function (root, factory) {
  const api = factory();
  // Browser content-script world:
  if (typeof window !== "undefined") window.HEBFIX = api;
  // Node test harness:
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(this, function () {
  "use strict";

  /**
   * US-QWERTY key -> Standard Israeli (SI-1452) Hebrew keyboard letter.
   *
   * The mapping is a straight, position-agnostic transliteration of physical
   * keystrokes: a blind touch-typist who "forgot to switch the layout" presses
   * exactly the same keys they would in Hebrew, so we simply translate each key
   * to the glyph the Hebrew layout would have produced — including final forms,
   * which live on their own keys (ך=l, ם=o, ן=i, ף=;, ץ=.).
   */
  const KEY_TO_HEB = {
    // --- top letter row (q..p) ---
    q: "/", w: "'", e: "ק", r: "ר", t: "א", y: "ט",
    u: "ו", i: "ן", o: "ם", p: "פ",
    // --- home row (a..l + ; ') ---
    a: "ש", s: "ד", d: "ג", f: "כ", g: "ע", h: "י",
    j: "ח", k: "ל", l: "ך", ";": "ף", "'": ",",
    // --- bottom row (z..m + , . /) ---
    z: "ז", x: "ס", c: "ב", v: "ה", b: "נ", n: "מ",
    m: "צ", ",": "ת", ".": "ץ", "/": "."
  };

  // Characters that can be part of a "Hebrew-when-typed-in-English" token.
  // Includes the punctuation keys that actually produce Hebrew letters
  // (comma→ת, period→ץ, semicolon→ף) so words like "שבת" (a,c,) are caught.
  const TOKEN_CHAR = /[a-zA-Z,.;']/;

  // Keys that produce an actual Hebrew *letter* (used to require a token to
  // contain enough "real" letters before we treat it as a candidate).
  const HEB_LETTER_KEYS = new Set(
    "ertyuiopasdfghjklzxcvbnm".split("")   // consonants + finals on letter keys
      .concat([",", ".", ";"])              // ת ץ ף live on punctuation keys
  );

  /**
   * Translate a run of English keystrokes into the Hebrew glyphs they map to.
   * Unknown characters are passed through unchanged. Uppercase is folded to
   * lowercase first (Hebrew is caseless; Shift+letter yields the same glyph).
   * @param {string} text
   * @returns {string}
   */
  function toHebrew(text) {
    let out = "";
    for (const ch of text) {
      const lower = ch.toLowerCase();
      out += Object.prototype.hasOwnProperty.call(KEY_TO_HEB, lower)
        ? KEY_TO_HEB[lower]
        : ch;
    }
    return out;
  }

  // Common English words we must NEVER mangle. Any token whose lowercase,
  // punctuation-stripped form is in here is left alone even in aggressive mode.
  // (Kept deliberately small + high-frequency; the dictionary gate below is the
  // primary precision mechanism.)
  const ENGLISH_STOP = new Set([
    "a","an","the","and","or","but","if","of","to","in","on","at","by","for",
    "is","am","are","was","were","be","been","being","do","does","did","done",
    "he","she","it","we","you","they","i","me","my","we","us","our","your",
    "this","that","these","those","here","there","what","when","where","who",
    "why","how","not","no","yes","ok","okay","hi","hey","hello","bye","lol",
    "as","so","up","out","off","go","get","got","can","will","would","should",
    "could","have","has","had","new","old","see","use","www","com","http",
    "https","gmail","email","name","test","cool","nice","good","bad","yeah"
  ]);

  /**
   * Decide whether a raw English token should be auto-corrected to Hebrew.
   *
   * Precision-first design:
   *   • default mode  -> convert only if the mapped Hebrew is a known word
   *                      (from HEB_WORDS), which makes false positives on real
   *                      English essentially impossible.
   *   • aggressive    -> convert any all-mappable token that isn't a common
   *                      English word (higher recall, lower precision).
   *
   * @param {string} token  raw keystrokes just typed (no surrounding spaces)
   * @param {Set<string>} hebWords  dictionary of valid Hebrew words
   * @param {boolean} aggressive
   * @returns {string|null}  the Hebrew replacement, or null to leave as-is
   */
  function evaluate(token, hebWords, aggressive) {
    if (!token) return null;

    // Strip leading/trailing punctuation for length + veto checks, but map the
    // trimmed core (surrounding punctuation is preserved by the caller anyway).
    const core = token.replace(/^[,.;']+|[,.;']+$/g, "");
    if (core.length < 2) return null;

    // Must be composed only of mappable characters.
    for (const ch of core) {
      if (!TOKEN_CHAR.test(ch)) return null;
    }

    // Require at least two characters that map to real Hebrew letters, so pure
    // punctuation runs or single-letter noise never trigger.
    let letterCount = 0;
    for (const ch of core.toLowerCase()) {
      if (HEB_LETTER_KEYS.has(ch)) letterCount++;
    }
    if (letterCount < 2) return null;

    // Never touch obvious English.
    if (ENGLISH_STOP.has(core.toLowerCase())) return null;

    const hebrew = toHebrew(core);

    if (aggressive) return hebrew;

    // Default (safe) mode: only convert dictionary-confirmed Hebrew words.
    return hebWords && inDictionary(hebrew, hebWords) ? hebrew : null;
  }

  // Single-letter Hebrew prefixes that attach to words: definite article ה,
  // conjunction ו, and prepositions ב/ל/כ/מ/ש. Very common ("העולם" = ה+עולם),
  // so the dictionary gate strips up to two of them before giving up.
  const PREFIXES = new Set(["ה", "ו", "ב", "ל", "כ", "מ", "ש"]);

  /**
   * True if `word` is in the dictionary directly, or becomes a dictionary word
   * after peeling one or two leading Hebrew prefix letters.
   */
  function inDictionary(word, hebWords) {
    if (hebWords.has(word)) return true;
    let w = word;
    for (let depth = 0; depth < 2 && w.length > 2 && PREFIXES.has(w[0]); depth++) {
      w = w.slice(1);
      if (hebWords.has(w)) return true;
    }
    return false;
  }

  return { KEY_TO_HEB, TOKEN_CHAR, ENGLISH_STOP, toHebrew, evaluate, inDictionary };
});
