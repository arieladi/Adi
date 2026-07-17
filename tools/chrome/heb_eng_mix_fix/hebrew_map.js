/*
 * hebrew_map.js — HEB ENG MIX FIX (v3)
 * ----------------------------------------------------------------------------
 * Shared, PURE data + helpers: the bidirectional US-QWERTY <-> Hebrew key map,
 * single-key translation for live interception (both directions), and the
 * wrong-layout heuristics for BOTH directions:
 *
 *   EN→HE  "nts bjns"                  → the user meant "מאד נחמד"
 *   HE→EN  "מקקג אם ןצפרםהק"           → the user meant "need to improve"
 *
 * No DOM, no events — trivially testable in Node.
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
   * Position-agnostic transliteration of physical keystrokes, including final
   * forms which live on their own keys (ך=l ם=o ן=i ף=; ץ=.).
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
  // macOS Hebrew layouts emit U+05F3 HEBREW GERESH (׳) for the W key instead of
  // an ASCII apostrophe — alias it so "יקנרק׳" still reverses to "hebrew".
  HEB_TO_KEY["׳"] = "w";

  // Character classes.
  const HEBREW_CHAR = /[֐-׿]/;          // anything in the Hebrew block
  const HEB_LETTER = /[א-ת]/;           // א..ת including finals
  const LATIN_LETTER = /[a-zA-Z]/;
  const FINAL_LETTERS = /[ךםןףץ]/;

  // Characters allowed inside a candidate token, per direction.
  const EN_TOKEN = /^[a-zA-Z',.;/]+$/;
  const HE_TOKEN = /^[א-ת'׳,./]+$/;

  // ---- single-key live translation (STATE 2) --------------------------------

  /** English keystroke -> Hebrew glyph, or null when the key is layout-neutral. */
  function keyToHeb(ch) {
    if (typeof ch !== "string" || ch.length !== 1) return null;
    const lower = ch.toLowerCase();
    return Object.prototype.hasOwnProperty.call(KEY_TO_HEB, lower)
      ? KEY_TO_HEB[lower]
      : null;
  }

  /**
   * Hebrew keystroke -> English character, or null when layout-neutral.
   * Hebrew has no case, so Shift intent is signalled by the event's shiftKey —
   * pass it to get the uppercase letter the user actually wanted.
   */
  function hebKeyToEn(ch, shift) {
    if (typeof ch !== "string" || ch.length !== 1) return null;
    if (!Object.prototype.hasOwnProperty.call(HEB_TO_KEY, ch)) return null;
    const key = HEB_TO_KEY[ch];
    return shift && LATIN_LETTER.test(key) ? key.toUpperCase() : key;
  }

  // ---- whole-string transliteration ------------------------------------------

  /** English keystrokes -> the Hebrew they were meant to be (unknown chars pass). */
  function toHebrew(text) {
    let out = "";
    for (const ch of text) out += keyToHeb(ch) || ch;
    return out;
  }

  /** Hebrew glyphs -> the English keys that produce them (unknown chars pass). */
  function fromHebrew(text) {
    let out = "";
    for (const ch of text) {
      out += Object.prototype.hasOwnProperty.call(HEB_TO_KEY, ch)
        ? HEB_TO_KEY[ch]
        : ch;
    }
    return out;
  }

  // ---- heuristics: EN→HE direction ------------------------------------------

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

  function hasEnglishVowel(word) {
    return /[aeiouyw]/i.test(word);
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

  /** Strip edge punctuation and lowercase, for EN-typed token analysis. */
  function enCore(word) {
    return word.replace(/^[^a-zA-Z]+|[^a-zA-Z]+$/g, "").toLowerCase();
  }

  /**
   * Is this English-typed token really Hebrew on the wrong layout?
   * True when it is mappable, not common English, and EITHER maps to a known
   * Hebrew word OR has no English vowel (impossible shape for real English).
   */
  function isWrongLayoutHebrew(word, hebWords) {
    const core = enCore(word);
    if (core.length < 2 || !EN_TOKEN.test(core)) return false;
    if (ENGLISH_STOP.has(core)) return false;
    if (inDictionary(toHebrew(core), hebWords)) return true;
    return !hasEnglishVowel(core);
  }

  // ---- heuristics: HE→EN direction (the reverse) -----------------------------

  /** Strip edge chars that can't be part of a Hebrew-typed token. */
  function heCore(word) {
    return word.replace(
      /^[^א-ת'׳]+|[^א-ת'׳]+$/g, ""
    );
  }

  /**
   * Hebrew final letters (ך ם ן ף ץ) may ONLY appear at the end of a word.
   * A final anywhere earlier (e.g. ןצפרםהק) is impossible Hebrew — and exactly
   * what English typed on a Hebrew layout produces (i→ן, o→ם mid-word).
   */
  function hasFinalViolation(word) {
    return FINAL_LETTERS.test(word.slice(0, -1));
  }

  /**
   * Is this Hebrew-typed token really English on the wrong layout?
   * Vetoed when it's a real Hebrew word. Accepted when its reverse-mapped form
   * is a known English word, or when it breaks final-letter orthography AND
   * reverses to something vowel-bearing (English-shaped).
   */
  function isWrongLayoutEnglish(word, hebWords, enWords) {
    const core = heCore(word);
    if (core.length < 2 || !HE_TOKEN.test(core)) return false;
    let letters = 0;
    for (const ch of core) if (HEB_LETTER.test(ch)) letters++;
    if (letters < 2) return false;
    if (inDictionary(core, hebWords)) return false;   // real Hebrew — hands off
    const rev = fromHebrew(core);
    if (enWords && enWords.has(rev)) return true;
    return hasFinalViolation(core) && /[aeiouy]/.test(rev);
  }

  /**
   * Relaxed HE→EN check used only to EXTEND an already-confirmed run backwards:
   * once two words have proven the layout mix-up, a preceding token like "אם"
   * (a real Hebrew word, but reversing to "to") almost certainly belongs to the
   * same mistake — so the dictionary veto is dropped for the walk-back.
   */
  function isLikelyWrongEnglish(word, hebWords, enWords) {
    const core = heCore(word);
    if (core.length < 1 || !HE_TOKEN.test(core)) return false;
    const rev = fromHebrew(core);
    if (enWords && enWords.has(rev)) return true;
    return hasFinalViolation(core) && /[aeiouy]/.test(rev);
  }

  /** Relaxed EN→HE twin for the walk-back (same rules; kept for symmetry). */
  function isLikelyWrongHebrew(word, hebWords) {
    return isWrongLayoutHebrew(word, hebWords);
  }

  return {
    KEY_TO_HEB, HEB_TO_KEY,
    HEBREW_CHAR, HEB_LETTER, LATIN_LETTER,
    keyToHeb, hebKeyToEn, toHebrew, fromHebrew,
    hasEnglishVowel, hasFinalViolation, inDictionary,
    isWrongLayoutHebrew, isWrongLayoutEnglish,
    isLikelyWrongHebrew, isLikelyWrongEnglish
  };
});
