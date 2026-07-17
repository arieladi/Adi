/*
 * content.js — HEB ENG MIX FIX (v3.1)
 * ----------------------------------------------------------------------------
 * Two cooperating layers:
 *
 * LAYER A — the local, instant, BIDIRECTIONAL layout state machine
 *   STATE 1  Monitoring: at each word boundary, check the last two words in
 *            BOTH directions — English-typed-as-Hebrew ("nts bjns") and
 *            Hebrew-typed-as-English ("מקקג אם ןצפרםהק"). On a match, walk
 *            further back to catch the whole wrong-layout run.
 *   STATE 2  Override: backtrack-convert the run, then live-map every
 *            subsequent keystroke into the intended language.
 *   STATE 3  Yellow caret tooltip + field glow (ui.js / content.css).
 *   STATE 4  Reset on Alt+Shift, a keystroke that natively matches the target
 *            language (the user really switched), Escape, or blur.
 *
 * LAYER B — the idle spell-checker (Grammarly-style)
 *   3 seconds after the user pauses typing, the recent text is sent to
 *   background.js, which asks the Google Suggest API for a corrected phrase
 *   and word-diffs it. ALL misspelled words come back as individual fixes,
 *   shown in a floating panel below the caret: click a row to fix that word,
 *   Tab (or "Fix all") to fix everything, ✕ to dismiss those suggestions.
 *
 * Performance: passive typing costs a couple of comparisons per key; layer B
 * runs at most once per pause, deduplicated by a phrase cache.
 * ----------------------------------------------------------------------------
 */
(function () {
  "use strict";

  const {
    keyToHeb, hebKeyToEn, toHebrew, fromHebrew,
    isWrongLayoutHebrew, isWrongLayoutEnglish,
    isLikelyWrongHebrew, isLikelyWrongEnglish,
    HEBREW_CHAR, LATIN_LETTER
  } = window.HEBFIX;
  const HEB_WORDS = window.HEB_WORDS;
  const EN_WORDS = window.EN_WORDS;
  const UI = window.HEBFIX_UI;

  // How long the user must pause before the spell-check pass runs.
  const IDLE_MS = window.__HEBFIX_IDLE_MS || 3000;
  // How much trailing text is checked per pass (Suggest queries are short).
  const CHECK_WINDOW = 300;

  // ---- settings (live) ------------------------------------------------------
  const settings = { enabled: true, manual: false, online: true };

  chrome.storage.local.get(
    { enabled: true, manual: false, online: true },
    (s) => Object.assign(settings, s)
  );
  chrome.storage.onChanged.addListener((changes, area) => {
    if (area !== "local") return;
    for (const k in changes) settings[k] = changes[k].newValue;
    if (!settings.enabled || settings.manual) { exitOverride(); hidePanel(); }
  });

  // ---- machine state --------------------------------------------------------
  let mode = "passive";        // "passive" | "override"
  let direction = null;        // "en2he" | "he2en" while overriding
  let overrideEl = null;
  let overrideIsField = false;

  // Layer B state.
  let idleTimer = 0;
  let idleCtx = null;                 // context the timer was armed for
  let pending = null;                 // {el,isField,node,phrase,fixes:[...]} behind the panel
  let applying = false;               // our own edits must not re-arm the timer
  const phraseCache = new Map();      // phrase -> fixes|null (avoid re-querying)
  const dismissed = new Set();        // "bad→good" keys the user ✕-ed away

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
    applying = true;
    setFieldValue(el, el.value.slice(0, start) + text + el.value.slice(end));
    el.setSelectionRange(caret, caret);
    applying = false;
  }

  /** Replace [start,end) inside a CE text node, shifting the caret sensibly. */
  function spliceNode(node, start, end, text) {
    applying = true;
    const sel = window.getSelection();
    const caretOff = sel && sel.anchorNode === node ? sel.anchorOffset : null;
    node.data = node.data.slice(0, start) + text + node.data.slice(end);
    if (caretOff !== null) {
      const delta = text.length - (end - start);
      const off = caretOff >= end ? caretOff + delta
        : caretOff > start ? start + text.length
        : caretOff;
      const r = document.createRange();
      r.setStart(node, Math.min(off, node.data.length));
      r.collapse(true);
      sel.removeAllRanges();
      sel.addRange(r);
    }
    applying = false;
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

  // ---- LAYER A / STATE 1: trigger detection ---------------------------------

  /**
   * Inspect the text before the caret. If the last TWO words are wrong-layout
   * (either direction), extend the run backwards over further qualifying words
   * (so "מקקג אם ןצפרםהק ןא" converts wholesale, not just the last pair) and
   * return { len, conv, dir }; otherwise null.
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

  // ---- LAYER A / STATES 2+3: enter / leave override --------------------------

  function enterOverride(ctx, dir) {
    hidePanel();
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

  // ---- LAYER A / STATES 2+4: keystrokes while overriding ----------------------

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

  // ---- LAYER B: idle spell-check pass -----------------------------------------

  /** (Re)arm the 3-second idle timer for the field being edited. */
  function armIdleTimer(ctx) {
    idleCtx = ctx;
    clearTimeout(idleTimer);
    idleTimer = setTimeout(runSpellcheck, IDLE_MS);
  }

  /** Snapshot the text to check: the trailing window before the caret. */
  function snapshotPhrase(ctx) {
    let full, node = null;
    if (ctx.isField) {
      full = ctx.el.value;
    } else {
      const sel = window.getSelection();
      if (!sel || sel.rangeCount === 0) return null;
      node = sel.anchorNode;
      if (!node || node.nodeType !== Node.TEXT_NODE) return null;
      full = node.data;
    }
    let base = 0;
    let phrase = full;
    if (phrase.length > CHECK_WINDOW) {
      base = phrase.length - CHECK_WINDOW;
      const cut = phrase.slice(base);
      const firstSpace = cut.search(/\s\S/); // start at a word boundary
      base += firstSpace >= 0 ? firstSpace + 1 : 0;
      phrase = full.slice(base);
    }
    // Trim trailing whitespace but keep offsets anchored to `base`.
    phrase = phrase.replace(/\s+$/, "");
    if (phrase.length < 5) return null;
    if (phrase.split(/\s+/).length < 2) return null;  // 1-word queries only
    if (!/[a-zA-Zא-ת]/.test(phrase)) return null;     // get autocomplete noise
    return { phrase, base, node };
  }

  /** The idle pass: query background, then show every misspelling found. */
  function runSpellcheck() {
    const ctx = idleCtx;
    if (!ctx || !settings.enabled || settings.manual || !settings.online) return;
    if (mode === "override" || UI.badgeVisible()) return;
    if (document.activeElement !== ctx.el &&
        !(ctx.el.contains && ctx.el.contains(document.activeElement))) return;

    const snap = snapshotPhrase(ctx);
    if (!snap) return;

    const deliver = (fixes) => {
      if (!fixes || !fixes.length) return;
      if (mode === "override") return;
      // The text must not have changed while we were fetching.
      const nowFull = ctx.isField ? ctx.el.value : snap.node && snap.node.data;
      if (!nowFull || nowFull.slice(snap.base, snap.base + snap.phrase.length) !== snap.phrase) return;

      const usable = fixes
        .map((f) => ({
          start: snap.base + f.start,
          end: snap.base + f.end,
          bad: snap.phrase.slice(f.start, f.end),
          text: f.text,
          key: snap.phrase.slice(f.start, f.end) + "→" + f.text
        }))
        .filter((f) => !dismissed.has(f.key) && f.bad !== f.text);
      if (!usable.length) return;

      pending = { el: ctx.el, isField: ctx.isField, node: snap.node, fixes: usable };
      UI.showFixes(ctx.el, ctx.isField, usable, {
        onFix: applyFix,
        onFixAll: applyAllFixes,
        onDismiss: dismissPanel
      });
    };

    if (phraseCache.has(snap.phrase)) {
      deliver(phraseCache.get(snap.phrase));
      return;
    }
    try {
      chrome.runtime.sendMessage(
        { type: "hebfix-suggest", text: snap.phrase },
        (fixes) => {
          if (chrome.runtime.lastError) return;
          phraseCache.set(snap.phrase, fixes || null);
          deliver(fixes);
        }
      );
    } catch (e) { /* extension reloaded mid-page — ignore */ }
  }

  /** Current text of a fix's range, to confirm the DOM hasn't moved on. */
  function fixIntact(f) {
    try {
      const t = pending.isField
        ? pending.el.value.slice(f.start, f.end)
        : pending.node.data.slice(f.start, f.end);
      return t === f.bad;
    } catch (e) {
      return false;
    }
  }

  /** Apply one fix; shift the offsets of the fixes that follow it. */
  function applyFix(fix) {
    if (!pending || !fixIntact(fix)) { hidePanel(); return; }
    const delta = fix.text.length - (fix.end - fix.start);

    if (pending.isField) {
      const el = pending.el;
      el.focus();
      const caret = el.selectionStart;
      const newCaret = caret >= fix.end ? caret + delta : caret;
      spliceField(el, fix.start, fix.end, fix.text, newCaret);
    } else {
      spliceNode(pending.node, fix.start, fix.end, fix.text);
    }

    pending.fixes = pending.fixes.filter((f) => f !== fix);
    for (const f of pending.fixes) {
      if (f.start >= fix.end) { f.start += delta; f.end += delta; }
    }
    if (pending.fixes.length) {
      UI.showFixes(pending.el, pending.isField, pending.fixes, {
        onFix: applyFix,
        onFixAll: applyAllFixes,
        onDismiss: dismissPanel
      });
    } else {
      hidePanel();
    }
  }

  /** Apply every remaining fix, last-to-first so offsets stay valid. */
  function applyAllFixes() {
    if (!pending) return;
    const list = pending.fixes.slice().sort((a, b) => b.start - a.start);
    for (const f of list) {
      if (!fixIntact(f)) continue;
      if (pending.isField) {
        const el = pending.el;
        el.focus();
        const caret = el.selectionStart;
        const delta = f.text.length - (f.end - f.start);
        const newCaret = caret >= f.end ? caret + delta : caret;
        spliceField(el, f.start, f.end, f.text, newCaret);
      } else {
        spliceNode(pending.node, f.start, f.end, f.text);
      }
    }
    hidePanel();
  }

  /** ✕: the user doesn't want THESE suggestions again. */
  function dismissPanel() {
    if (pending) for (const f of pending.fixes) dismissed.add(f.key);
    hidePanel();
  }

  /** Hide without recording anything (e.g. the user resumed typing). */
  function hidePanel() {
    pending = null;
    UI.hideBadge();
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

  // ---- the delegated listeners -------------------------------------------------

  function onKeydown(e) {
    if (!settings.enabled) return;

    if (settings.manual) {
      if (e.ctrlKey && !e.altKey && !e.metaKey && (e.key === "e" || e.key === "E")) {
        translateSelection(e);
      }
      return;
    }

    // Fix-panel hotkeys: Tab fixes all; typing on just hides the panel.
    if (UI.badgeVisible()) {
      if (e.key === "Tab" && !e.ctrlKey && !e.metaKey && !e.altKey) {
        e.preventDefault();
        applyAllFixes();
        return;
      }
      if (e.key === "Escape") { hidePanel(); return; }
      if (e.key.length === 1 || e.key === "Backspace" || e.key === "Enter") {
        hidePanel();
      }
    }

    if (mode === "override") { handleOverrideKey(e); return; }

    // LAYER A STATE 1 (passive): act only on completed words.
    if (e.isComposing || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key !== " " && e.key !== "Enter") return;

    const ctx = getContext(e.target);
    if (!ctx) return;

    const dir = ctx.isField ? tryTriggerField(ctx.el) : tryTriggerEditable();
    if (dir) enterOverride(ctx, dir);
    // Never preventDefault here: Space/Enter keep their native behavior.
  }

  /** Every real edit (typing, paste, cut) re-arms the 3s spell-check timer. */
  function onInput(e) {
    if (applying) return;                       // our own splices don't count
    if (!settings.enabled || settings.manual || !settings.online) return;
    const ctx = getContext(e.target);
    if (ctx) armIdleTimer(ctx);
  }

  document.addEventListener("keydown", onKeydown, true);
  document.addEventListener("input", onInput, true);

  document.addEventListener(
    "blur",
    (e) => {
      if (mode === "override" && e.target === overrideEl) exitOverride();
      if (pending && e.target === pending.el) hidePanel();
      if (idleCtx && e.target === idleCtx.el) { clearTimeout(idleTimer); idleCtx = null; }
    },
    true
  );
  window.addEventListener("scroll", scheduleReposition, true);
  window.addEventListener("resize", scheduleReposition);
})();
