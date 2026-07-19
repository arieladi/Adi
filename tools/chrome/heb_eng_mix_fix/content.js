/*
 * content.js — HEB ENG MIX FIX (v7 — layout fix only)
 * ----------------------------------------------------------------------------
 * The local, instant, BIDIRECTIONAL keyboard-layout state machine — and
 * nothing else. (Spell-checking moved to its own extension: adi spell chekcer.)
 *
 *   STATE 1  Monitoring: at each word boundary, check the last two words in
 *            BOTH directions — English-typed-as-Hebrew ("nts bjns") and
 *            Hebrew-typed-as-English ("מקקג אם ןצפרםהק"). A sentence-level
 *            context gate blocks false positives inside genuine English
 *            ("just go for 100mb llm ?" must never convert). On a match, walk
 *            back to catch the whole wrong-layout run.
 *   STATE 2  Override: backtrack-convert the run, then live-map every
 *            subsequent keystroke into the intended language.
 *   STATE 3  Yellow caret tooltip + field glow (ui.js / content.css).
 *   STATE 4  Reset on Alt+Shift, a keystroke that natively matches the target
 *            language (the user really switched), Escape, or blur.
 *
 * Manual mode (popup): only Ctrl+E converts the selection, both directions.
 *
 * Performance: one delegated capture-phase keydown listener; real work happens
 * only on Space/Enter; in override it is O(1) per key. Fully offline.
 * ----------------------------------------------------------------------------
 */
(function () {
  "use strict";

  const {
    keyToHeb, hebKeyToEn, toHebrew, fromHebrew,
    isWrongLayoutHebrew, isWrongLayoutEnglish,
    isLikelyWrongHebrew, isLikelyWrongEnglish,
    contextAllowsHebrewFix, HEBREW_CHAR, LATIN_LETTER
  } = window.HEBFIX;
  const HEB_WORDS = window.HEB_WORDS;
  const EN_WORDS = window.EN_WORDS;
  const UI = window.HEBFIX_UI;

  // ---- settings (live) ------------------------------------------------------
  const settings = { enabled: true, manual: false };

  chrome.storage.local.get(
    { enabled: true, manual: false },
    (s) => Object.assign(settings, s)
  );
  chrome.storage.onChanged.addListener((changes, area) => {
    if (area !== "local") return;
    for (const k in changes) settings[k] = changes[k].newValue;
    if (!settings.enabled || settings.manual) exitOverride();
  });

  // ---- machine state --------------------------------------------------------
  let mode = "passive";        // "passive" | "override"
  let direction = null;        // "en2he" | "he2en" while overriding
  let overrideEl = null;
  let overrideIsField = false;

  // ---- element helpers ------------------------------------------------------

  function isTextFormField(el) {
    if (!el) return false;
    if (el.nodeName === "TEXTAREA") return !el.disabled && !el.readOnly;
    if (el.nodeName === "INPUT") {
      const type = (el.type || "text").toLowerCase();
      return ["text", "search", "url", "tel", ""].includes(type) &&
        !el.disabled && !el.readOnly;
    }
    return false;
  }

  function editableHost(el) {
    let n = el;
    while (n && n.nodeType === 1) {
      if (n.isContentEditable) return n;
      n = n.parentElement;
    }
    return null;
  }

  /** Resolve an event target to the editable surface we manage, or null. */
  function getContext(target) {
    if (isTextFormField(target)) return { el: target, isField: true };
    const host = editableHost(target);
    if (host) return { el: host, isField: false };
    return null;
  }

  /**
   * Framework-safe value assignment for <input>/<textarea>: use the native
   * prototype setter so React's value tracking sees the change, then announce
   * it with both `input` and `change`.
   */
  function setFieldValue(el, value) {
    const proto = el.nodeName === "TEXTAREA"
      ? HTMLTextAreaElement.prototype
      : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(proto, "value").set.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
    el.dispatchEvent(new Event("change", { bubbles: true }));
  }

  /** Replace [start,end) in a field and put the caret at `caret`. */
  function spliceField(el, start, end, text, caret) {
    setFieldValue(el, el.value.slice(0, start) + text + el.value.slice(end));
    el.setSelectionRange(caret, caret);
  }

  /** Insert text at the collapsed caret of the override surface. */
  function insertText(ctx, text) {
    if (ctx.isField) {
      const s = ctx.el.selectionStart;
      spliceField(ctx.el, s, ctx.el.selectionEnd, text, s + text.length);
    } else {
      document.execCommand("insertText", false, text); // preserves CE undo
    }
  }

  // ---- STATE 1: trigger detection --------------------------------------------

  /**
   * Inspect the text before the caret. If the last TWO words are wrong-layout
   * (either direction), extend the run backwards over further qualifying words
   * (so "מקקג אם ןצפרםהק ןא" converts wholesale) and return { len, conv, dir };
   * otherwise null.
   *
   * For EN→HE the pair check alone is not enough — technical English ("100mb
   * llm", acronyms, versions) can masquerade as mappable gibberish. The
   * sentence-level density gate (contextAllowsHebrewFix) must also agree that
   * the surrounding text really is mixed-up Hebrew.
   */
  function triggerSegment(before) {
    const m = before.match(/(\S+)(\s+)(\S+)$/);
    if (!m) return null;

    let dir = null;
    if (isWrongLayoutHebrew(m[1], HEB_WORDS) &&
        isWrongLayoutHebrew(m[3], HEB_WORDS)) {
      dir = "en2he";
    } else if (isWrongLayoutEnglish(m[1], HEB_WORDS, EN_WORDS) &&
               isWrongLayoutEnglish(m[3], HEB_WORDS, EN_WORDS)) {
      dir = "he2en";
    }
    if (!dir) return null;
    if (dir === "en2he" && !contextAllowsHebrewFix(before, HEB_WORDS)) return null;

    // Walk back over words that likely belong to the same wrong-layout run.
    const relaxed = dir === "en2he" ? isLikelyWrongHebrew : isLikelyWrongEnglish;
    let start = before.length - m[0].length;
    for (let i = 0; i < 8; i++) {
      const pm = before.slice(0, start).match(/(\S+)(\s+)$/);
      if (!pm || !relaxed(pm[1], HEB_WORDS, EN_WORDS)) break;
      start -= pm[0].length;
    }

    const seg = before.slice(start);
    return {
      len: seg.length,
      conv: dir === "en2he" ? toHebrew(seg) : fromHebrew(seg),
      dir
    };
  }

  /** Fire on a form field: backtrack-replace the run. Returns dir or null. */
  function tryTriggerField(el) {
    if (el.selectionStart !== el.selectionEnd) return null;
    const caret = el.selectionStart;
    const seg = triggerSegment(el.value.slice(0, caret));
    if (!seg) return null;
    const start = caret - seg.len;
    spliceField(el, start, caret, seg.conv, start + seg.conv.length);
    return seg.dir;
  }

  /** Fire on a contenteditable (single text node case). Returns dir or null. */
  function tryTriggerEditable() {
    const sel = window.getSelection();
    if (!sel || sel.rangeCount === 0 || !sel.isCollapsed) return null;
    const node = sel.anchorNode;
    if (!node || node.nodeType !== Node.TEXT_NODE) return null;
    const offset = sel.anchorOffset;
    const seg = triggerSegment(node.data.slice(0, offset));
    if (!seg) return null;
    const start = offset - seg.len;
    node.data = node.data.slice(0, start) + seg.conv + node.data.slice(offset);
    const range = document.createRange();
    range.setStart(node, start + seg.conv.length);
    range.collapse(true);
    sel.removeAllRanges();
    sel.addRange(range);
    return seg.dir;
  }

  // ---- STATES 2+3: enter / leave override -------------------------------------

  function enterOverride(ctx, dir) {
    mode = "override";
    direction = dir;
    overrideEl = ctx.el;
    overrideIsField = ctx.isField;
    UI.show(ctx.el, ctx.isField, dir);
    scheduleReposition();
  }

  function exitOverride() {
    if (mode !== "override") return;
    UI.hide(overrideEl);
    mode = "passive";
    direction = null;
    overrideEl = null;
  }

  let rafPending = 0;
  function scheduleReposition() {
    if (rafPending) return;
    rafPending = requestAnimationFrame(() => {
      rafPending = 0;
      if (mode === "override" && overrideEl) UI.position(overrideEl, overrideIsField);
    });
  }

  // ---- STATES 2+4: keystrokes while overriding ---------------------------------

  function handleOverrideKey(e) {
    const ctx = getContext(e.target);
    if (!ctx || ctx.el !== overrideEl) { exitOverride(); return; }

    // STATE 4: layout-switch chord or explicit veto.
    if ((e.key === "Shift" && e.altKey) || (e.key === "Alt" && e.shiftKey) ||
        e.key === "Escape") {
      exitOverride();
      return;
    }
    if (e.ctrlKey || e.metaKey || e.altKey) return; // never touch shortcuts

    if (e.key.length === 1) {
      // STATE 4: the keystroke natively matches the language we're producing —
      // the user really switched layouts, so step aside immediately.
      if (direction === "en2he" && HEBREW_CHAR.test(e.key)) { exitOverride(); return; }
      if (direction === "he2en" && LATIN_LETTER.test(e.key)) { exitOverride(); return; }

      // STATE 2: live-map the key into the intended language.
      const mapped = direction === "en2he"
        ? keyToHeb(e.key)
        : hebKeyToEn(e.key, e.shiftKey);
      if (mapped) {
        e.preventDefault();
        insertText(ctx, mapped);
        scheduleReposition();
        return;
      }
    }
    scheduleReposition(); // neutral key (space, digits, Backspace…) passes through
  }

  // ---- Manual mode: Ctrl+E converts the selection (both directions) ----------

  function translateSelection(e) {
    const ctx = getContext(e.target);
    if (!ctx) return;
    const selected = ctx.isField
      ? ctx.el.value.slice(ctx.el.selectionStart, ctx.el.selectionEnd)
      : String(window.getSelection());
    if (!selected) return;

    let heb = 0, lat = 0;
    for (const ch of selected) {
      if (/[א-ת]/.test(ch)) heb++;
      else if (LATIN_LETTER.test(ch)) lat++;
    }
    const converted = heb > lat ? fromHebrew(selected) : toHebrew(selected);
    if (converted === selected) return;
    e.preventDefault();

    if (ctx.isField) {
      const start = ctx.el.selectionStart;
      spliceField(ctx.el, start, ctx.el.selectionEnd, converted,
        start + converted.length);
      ctx.el.setSelectionRange(start, start + converted.length);
    } else {
      document.execCommand("insertText", false, converted);
    }
  }

  // ---- the single delegated listener ------------------------------------------

  function onKeydown(e) {
    if (!settings.enabled) return;

    if (settings.manual) {
      if (e.ctrlKey && !e.altKey && !e.metaKey && (e.key === "e" || e.key === "E")) {
        translateSelection(e);
      }
      return;
    }

    if (mode === "override") { handleOverrideKey(e); return; }

    // STATE 1 (passive): act only on completed words.
    if (e.isComposing || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key !== " " && e.key !== "Enter") return;

    const ctx = getContext(e.target);
    if (!ctx) return;

    const dir = ctx.isField ? tryTriggerField(ctx.el) : tryTriggerEditable();
    if (dir) enterOverride(ctx, dir);
    // Never preventDefault here: the Space/Enter keeps its native behavior.
  }

  document.addEventListener("keydown", onKeydown, true);

  document.addEventListener(
    "blur",
    (e) => { if (mode === "override" && e.target === overrideEl) exitOverride(); },
    true
  );
  window.addEventListener("scroll", scheduleReposition, true);
  window.addEventListener("resize", scheduleReposition);
})();
