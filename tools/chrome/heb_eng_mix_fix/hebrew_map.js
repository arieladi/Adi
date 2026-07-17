/*
 * hebrew_map.js — HEB ENG MIX FIX (v2)
 * ----------------------------------------------------------------------------
 * Shared, PURE data + helpers: the bidirectional US-QWERTY <-> Hebrew key map,
 * single-key translation for live interception, phrase transliteration, the
 * reverse direction, and the "wrong-layout" heuristics used by the state
 * machine. No DOM, no events — trivially testable in Node.
 * ----------------------------------------------------------------------------
 */
(function (root, factory) {
  const api = factory();
  if (typeof window !== "undefined") window.HEBFIX = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(this, function () {
  "use strict";

  /**
   * US-QWERTY key -> Standard Israeli (SI-1452) Hebrew glyph (unshifted layer).
   * Straight, position-agnostic transliteration of physical keystrokes,
   * including final forms which live on their own keys (ך=l ם=o ן=i ף=; ץ=.).
   */
  const KEY_TO_HEB = {
    // top letter row
    q: "/", w: "'", e: "ק", r: "ר", t: "א", y: "ט",
    u: "ו", i: "ן", o: "ם", p: "פ",
    // home row
    a: "ש", s: "ד", d: "ג", f: "כ", g: "ע", h: "י",
    j: "ח", k: "ל", l: "ך", ";": "ף", "'": ",",
    // bottom row
    z: "ז", x: "ס", c: "ב", v: "ה", b: "נ", n: "מ",
    m: "צ", ",": "ת", ".": "ץ", "/": "."
  };

  // Reverse map: Hebrew glyph -> the US key that produces it. Values above are
  // unique, so the inversion is unambiguous.
  const HEB_TO_KEY = {};
  for (const k in KEY_TO_HEB) HEB_TO_KEY[KEY_TO_HEB[k]] = k;

  // Anything in the Hebrew Unicode block (used to detect a real layout switch).
  const HEBREW_CHAR = /[֐-׿]/;

  // Characters that can be part of a wrong-layout Hebrew token: letters plus the
  // punctuation keys that actually produce Hebrew letters (,=ת .=ץ ;=ף).
  const TOKEN_CHAR = /[a-zA-Z,.;']/;

  // Keys that produce a real Hebrew *letter* (for the "enough letters" check).
  const HEB_LETTER_KEYS = new Set(
    "ertyuiopasdfghjklzxcvbnm".split("").concat([",", ".", ";"])
  );

  /**
   * Translate ONE typed character to its Hebrew glyph, or null if this key is
   * not affected by the layout (so the caller lets it through untouched).
   * Handles case: Shift+letter yields the same caseless Hebrew consonant.
   * @param {string} ch single character (e.g. a KeyboardEvent.key)
   */
  function keyToHeb(ch) {
    if (typeof ch !== "string" || ch.length !== 1) return null;
    const lower = ch.toLowerCase();
    return Object.prototype.hasOwnProperty.call(KEY_TO_HEB, lower)
      ? KEY_TO_HEB[lower]
      : null;
  }

  /** Transliterate a run of English keystrokes -> Hebrew (unknown chars pass). */
  function toHebrew(text) {
    let out = "";
    for (const ch of text) out += keyToHeb(ch) || ch;
    return out;
  }

  /** Reverse: Hebrew glyphs -> the English keys that produce them. */
  function fromHebrew(text) {
    let out = "";
    for (const ch of text) {
      out += Object.prototype.hasOwnProperty.call(HEB_TO_KEY, ch)
        ? HEB_TO_KEY[ch]
        : ch;
    }
    return out;
  }

  // ---- heuristics -----------------------------------------------------------

  // Common English words we must never treat as wrong-layout Hebrew.
  const ENGLISH_STOP = new Set([
    "a","an","the","and","or","but","if","of","to","in","on","at","by","for",
    "is","am","are","was","were","be","been","being","do","does","did","done",
    "he","she","it","we","you","they","i","me","my","us","our","your","its",
    "this","that","these","those","here","there","what","when","where","who",
    "why","how","not","no","yes","ok","okay","hi","hey","hello","bye","lol",
    "as","so","up","out","off","go","get","got","can","will","would","should",
    "could","have","has","had","new","old","see","use","www","com","http",
    "https","gmail","email","name","test","cool","nice","good","bad","yeah",
    "with","from","into","over","just","like","time","know","people","think"
  ]);

  /** Does the token contain an English vowel (y/w count, to spare cry/why/two)? */
  function hasEnglishVowel(word) {
    return /[aeiouyw]/i.test(word);
  }

  /** Is every character of the token mappable to the Hebrew layout? */
  function isMappable(word) {
    for (const ch of word) if (!TOKEN_CHAR.test(ch)) return false;
    return true;
  }

  // Single-letter Hebrew prefixes (ה/ו/ב/ל/כ/מ/ש) that attach to words.
  const PREFIXES = new Set(["ה", "ו", "ב", "ל", "כ", "מ", "ש"]);

  /** In dictionary directly, or after peeling up to two leading prefixes. */
  function inDictionary(word, hebWords) {
    if (!hebWords) return false;
    if (hebWords.has(word)) return true;
    let w = word;
    for (let d = 0; d < 2 && w.length > 2 && PREFIXES.has(w[0]); d++) {
      w = w.slice(1);
      if (hebWords.has(w)) return true;
    }
    return false;
  }

  /**
   * Core heuristic: is this raw English token really Hebrew typed on the wrong
   * layout?  True when it is all-mappable, not common English, and EITHER maps
   * to a known Hebrew word OR has no English vowel at all (an impossible shape
   * for English but normal once mapped — e.g. "nts"->מאד, "bjns"->נחמד).
   */
  function isWrongLayoutHebrew(word, hebWords) {
    const core = word.replace(/^[,.;']+|[,.;']+$/g, "").toLowerCase();
    if (core.length < 2 || !isMappable(core)) return false;
    if (ENGLISH_STOP.has(core)) return false;

    let letters = 0;
    for (const ch of core) if (HEB_LETTER_KEYS.has(ch)) letters++;
    if (letters < 2) return false;

    if (inDictionary(toHebrew(core), hebWords)) return true;
    return !hasEnglishVowel(core);
  }

  /**
   * Single-word gate for the silent auto-fix. In default mode a token is only
   * converted when it maps to a dictionary word; aggressive mode drops that and
   * converts any non-English mappable token.
   * @returns {string|null} the Hebrew replacement, or null to leave as-is.
   */
  function evaluate(word, hebWords, aggressive) {
    const core = word.replace(/^[,.;']+|[,.;']+$/g, "").toLowerCase();
    if (core.length < 2 || !isMappable(core)) return null;
    if (ENGLISH_STOP.has(core)) return null;

    let letters = 0;
    for (const ch of core) if (HEB_LETTER_KEYS.has(ch)) letters++;
    if (letters < 2) return null;

    const hebrew = toHebrew(core);
    if (aggressive) return hebrew;
    return inDictionary(hebrew, hebWords) ? hebrew : null;
  }

  return {
    KEY_TO_HEB, HEB_TO_KEY, HEBREW_CHAR, TOKEN_CHAR, ENGLISH_STOP,
    keyToHeb, toHebrew, fromHebrew,
    hasEnglishVowel, isMappable, inDictionary, isWrongLayoutHebrew, evaluate
  };
});
