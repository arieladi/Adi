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
    if (!settings.enabled || settings.manual) { exitOverride(); clearSpellcheck(); }
  });

  // ---- machine state --------------------------------------------------------
  let mode = "passive";        // "passive" | "override"
  let direction = null;        // "en2he" | "he2en" while overriding
  let overrideEl = null;
  let overrideIsField = false;

  // Layer B state.
  let idleTimer = 0;
  let idleCtx = null;                 // context the timer was armed for
  let applying = false;               // our own edits must not re-arm the timer
  let passId = 0;                     // invalidates stale async pass results
  // The live set of highlighted corrections for the focused field:
  //   { start, end, original, corrected, key }  (absolute offsets)
  let corrections = [];
  let hlEl = null;                    // element the corrections belong to
  let hlIsField = false;
  let hlNode = null;                  // CE text node (contenteditable only)
  const chunkCache = new Map();       // chunk text -> fixes|null (avoid re-querying)
  const dismissed = new Set();        // "bad→good" keys the user ✕-ed away
  const MAX_CHUNK_QUERIES = 8;        // API calls allowed per idle pass
  // Sliding-window size. Probed live: Google Suggest returns the cleanest,
  // highest-recall spelling diffs on SHORT windows — 3 words catches every
  // typo in a sentence, while 5-8 words miss most (Suggest reworders/drops).
  const WINDOW_WORDS = 3;
  const WINDOW_STRIDE = 2;            // overlap 1 so boundary words get context

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
    clearSpellcheck();
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

  /** (Re)arm the idle timer for the field being edited. */
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
    phrase = phrase.replace(/\s+$/, "");
    if (phrase.length < 4) return null;
    if (!/[a-zA-Zא-ת]/.test(phrase)) return null;
    return { phrase, base, node };
  }

  /**
   * The "sliding window": split the recent text into small overlapping windows
   * (WINDOW_WORDS words, stride WINDOW_STRIDE). Short windows are what make
   * Google Suggest behave as a spell-checker, and they sidestep its length
   * limit entirely. Overlap gives interior words context on both sides.
   */
  function chunkPhrase(phrase) {
    const words = [];
    const re = /\S+/g;
    let m;
    while ((m = re.exec(phrase))) words.push({ w: m[0], i: m.index });
    const chunks = [];
    for (let i = 0; i < words.length; i += WINDOW_STRIDE) {
      const grp = words.slice(i, i + WINDOW_WORDS);
      if (grp.length < 2 && chunks.length) break; // lone trailing word: covered
      const start = grp[0].i;
      const end = grp[grp.length - 1].i + grp[grp.length - 1].w.length;
      const text = phrase.slice(start, end);
      if (/[a-zA-Zא-ת]/.test(text)) chunks.push({ text, offset: start });
      if (grp.length < WINDOW_WORDS) break;
    }
    return chunks;
  }

  /** Read the current text of an absolute range, or null if it moved. */
  function rangeText(start, end) {
    try {
      return hlIsField
        ? hlEl.value.slice(start, end)
        : hlNode.data.slice(start, end);
    } catch (e) {
      return null;
    }
  }

  /** Drop corrections whose underlying text no longer matches (edits shifted). */
  function revalidateCorrections() {
    if (!hlEl) return;
    const before = corrections.length;
    corrections = corrections.filter((c) => rangeText(c.start, c.end) === c.original);
    if (corrections.length !== before) renderHighlights();
  }

  function renderHighlights() {
    if (!hlEl || !corrections.length) { UI.clearHighlights(); return; }
    UI.renderHighlights(hlEl, hlIsField, hlNode, corrections, {
      onFix: applyFix,
      onFixAll: applyAllFixes,
      onDismiss: dismissOne
    });
  }

  /** The idle pass: check each sliding window, then highlight every typo found. */
  function runSpellcheck() {
    const ctx = idleCtx;
    if (!ctx || !settings.enabled || settings.manual || !settings.online) return;
    if (mode === "override") return;
    // The focused element may be an ANCESTOR of the editable (custom elements
    // with delegatesFocus, e.g. Gemini's rich-textarea) — accept both shapes.
    const ae = document.activeElement;
    const focusOk = ae && (ae === ctx.el ||
      (ctx.el.contains && ctx.el.contains(ae)) ||
      (ae.contains && ae.contains(ctx.el)));
    if (!focusOk) return;

    const snap = snapshotPhrase(ctx);
    if (!snap) return;

    const chunks = chunkPhrase(snap.phrase);
    if (!chunks.length) return;

    const myPass = ++passId;
    const collected = [];
    let waiting = 0;
    let budget = MAX_CHUNK_QUERIES;

    const finish = () => {
      if (waiting > 0 || myPass !== passId) return;
      if (mode === "override") return;
      // The scanned text must not have changed while we were fetching.
      const nowFull = ctx.isField ? ctx.el.value : snap.node && snap.node.data;
      if (!nowFull ||
          nowFull.slice(snap.base, snap.base + snap.phrase.length) !== snap.phrase) return;

      // Turn window-relative diffs into absolute corrections; first-wins on
      // overlapping spans (the same word appears in two overlapping windows).
      collected.sort((a, b) => a.start - b.start || a.end - b.end);
      const fresh = [];
      let lastEnd = -1;
      for (const f of collected) {
        if (f.start < lastEnd) continue;
        const start = snap.base + f.start;
        const end = snap.base + f.end;
        const original = snap.phrase.slice(f.start, f.end);
        const key = original + "→" + f.corrected;
        if (dismissed.has(key) || original === f.corrected) continue;
        fresh.push({ start, end, original, corrected: f.corrected, key });
        lastEnd = f.end;
      }

      // Merge into the live set: replace everything inside the scanned span with
      // the fresh results, keep corrections outside it (earlier in a long doc).
      const spanStart = snap.base, spanEnd = snap.base + snap.phrase.length;
      hlEl = ctx.el; hlIsField = ctx.isField; hlNode = snap.node;
      corrections = corrections
        .filter((c) => c.end <= spanStart || c.start >= spanEnd)
        .concat(fresh)
        .sort((a, b) => a.start - b.start);
      renderHighlights();
    };

    const absorb = (chunk, fixes) => {
      if (!fixes) return;
      for (const f of fixes) {
        collected.push({
          start: chunk.offset + f.start,
          end: chunk.offset + f.end,
          corrected: f.corrected
        });
      }
    };

    // Newest text first: if the budget runs out, chunks nearest the caret win.
    for (const chunk of chunks.slice().reverse()) {
      if (chunkCache.has(chunk.text)) { absorb(chunk, chunkCache.get(chunk.text)); continue; }
      if (budget-- <= 0) continue;
      waiting++;
      try {
        chrome.runtime.sendMessage(
          { type: "hebfix-suggest", text: chunk.text },
          (fixes) => {
            waiting--;
            if (!chrome.runtime.lastError) {
              chunkCache.set(chunk.text, fixes || null);
              absorb(chunk, fixes);
            }
            finish();
          }
        );
      } catch (e) {
        waiting--; // extension reloaded mid-page — ignore
      }
    }
    finish(); // covers the all-cached case
  }

  // ---- applying corrections --------------------------------------------------

  /** Replace one correction's word; shift the offsets of those after it. */
  function applyOne(c) {
    if (rangeText(c.start, c.end) !== c.original) return false;
    const delta = c.corrected.length - (c.end - c.start);
    if (hlIsField) {
      hlEl.focus();
      const caret = hlEl.selectionStart;
      const newCaret = caret >= c.end ? caret + delta
        : caret > c.start ? c.start + c.corrected.length : caret;
      spliceField(hlEl, c.start, c.end, c.corrected, newCaret);
    } else {
      spliceNode(hlNode, c.start, c.end, c.corrected);
    }
    corrections = corrections.filter((x) => x !== c);
    for (const x of corrections) {
      if (x.start >= c.end) { x.start += delta; x.end += delta; }
    }
    return true;
  }

  /** Single Fix: replace only this word. */
  function applyFix(c) {
    UI.hideHoverBadge();
    applyOne(c);
    renderHighlights();
  }

  /** Fix All (Tab): replace every current correction, last-to-first. */
  function applyAllFixes() {
    UI.hideHoverBadge();
    for (const c of corrections.slice().sort((a, b) => b.start - a.start)) {
      applyOne(c);
    }
    corrections = [];
    renderHighlights();
  }

  /** ✕: ignore this specific correction (won't be re-offered). */
  function dismissOne(c) {
    dismissed.add(c.key);
    corrections = corrections.filter((x) => x !== c);
    UI.hideHoverBadge();
    renderHighlights();
  }

  /** Drop all highlights for the current field (blur / disable / resumed typing). */
  function clearSpellcheck() {
    corrections = [];
    hlEl = null; hlNode = null;
    UI.clearHighlights();
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

    // Spell-check hotkeys (only while this field has live corrections).
    if (corrections.length && hlEl && getContext(e.target) &&
        getContext(e.target).el === hlEl) {
      // Tab = Fix all.
      if (e.key === "Tab" && !e.ctrlKey && !e.metaKey && !e.altKey) {
        e.preventDefault();
        applyAllFixes();
        return;
      }
      // Escape just dismisses the hover badge if one is open.
      if (e.key === "Escape" && UI.badgeVisible()) { UI.hideHoverBadge(); return; }
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

  /** Every real edit (typing, paste, cut) re-arms the idle timer. */
  function onInput(e) {
    if (applying) return;                       // our own splices don't count
    if (!settings.enabled || settings.manual || !settings.online) return;
    const ctx = getContext(e.target);
    if (!ctx) return;
    // Keep existing highlights honest as the text shifts under them.
    if (hlEl === ctx.el) revalidateCorrections();
    armIdleTimer(ctx);
  }

  document.addEventListener("keydown", onKeydown, true);
  document.addEventListener("input", onInput, true);

  document.addEventListener(
    "blur",
    (e) => {
      if (mode === "override" && e.target === overrideEl) exitOverride();
      if (hlEl && e.target === hlEl) clearSpellcheck();
      if (idleCtx && e.target === idleCtx.el) { clearTimeout(idleTimer); idleCtx = null; }
    },
    true
  );

  // Keep the tooltip and the highlight overlay glued to the field as it moves.
  function onViewportChange() {
    scheduleReposition();
    UI.repositionHighlights();
  }
  window.addEventListener("scroll", onViewportChange, true);
  window.addEventListener("resize", onViewportChange);
})();
