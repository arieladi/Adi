/*
 * content.js — HEB ENG MIX FIX (v2)
 * ----------------------------------------------------------------------------
 * A bidirectional state machine that fixes Hebrew typed on an English layout.
 *
 *   STATE 1  Monitoring (passive)  — watch word boundaries. When two consecutive
 *            words (one, in Aggressive mode) are gibberish in English but valid
 *            Hebrew once mapped, fire the trigger.
 *   STATE 2  Active override        — backtrack-convert the trigger words, then
 *            intercept every subsequent keystroke and inject its Hebrew glyph.
 *   STATE 3  Visual alert           — yellow caret tooltip + field glow (ui.js).
 *   STATE 4  Reset                  — leave override the moment the user switches
 *            layout (Alt+Shift, or a real Hebrew keystroke arrives), presses
 *            Escape, or leaves the field.
 *
 * Manual mode (popup) bypasses the machine: only Ctrl+E converts a selection.
 *
 * Performance: one delegated capture-phase keydown listener. In passive state
 * real work happens only on Space/Enter; in override it is O(1) per key.
 * ----------------------------------------------------------------------------
 */
(function () {
  "use strict";

  const {
    keyToHeb, toHebrew, isWrongLayoutHebrew, HEBREW_CHAR
  } = window.HEBFIX;
  const HEB_WORDS = window.HEB_WORDS;
  const UI = window.HEBFIX_UI;

  // ---- settings (live) ------------------------------------------------------
  const settings = { enabled: true, manual: false, aggressive: false };

  chrome.storage.local.get(
    { enabled: true, manual: false, aggressive: false },
    (s) => Object.assign(settings, s)
  );
  chrome.storage.onChanged.addListener((changes, area) => {
    if (area !== "local") return;
    for (const k in changes) settings[k] = changes[k].newValue;
    // Any setting change that turns off the machine must drop any override.
    if (!settings.enabled || settings.manual) exitOverride();
  });

  // ---- machine state --------------------------------------------------------
  let mode = "passive";       // "passive" | "override"
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

  // ---- text insertion at the caret -----------------------------------------

  function insertText(ctx, text) {
    if (ctx.isField) {
      const el = ctx.el;
      const s = el.selectionStart, e = el.selectionEnd;
      el.value = el.value.slice(0, s) + text + el.value.slice(e);
      const c = s + text.length;
      el.setSelectionRange(c, c);
      el.dispatchEvent(new Event("input", { bubbles: true }));
    } else {
      // Handles the live selection + undo stack in contenteditable.
      document.execCommand("insertText", false, text);
    }
  }

  // ---- STATE 1: trigger detection ------------------------------------------

  /**
   * Given the text before the caret, decide whether the trailing word(s) should
   * fire the override. Default needs TWO consecutive wrong-layout words;
   * Aggressive fires on ONE. Returns {len, conv} (chars to replace + their
   * Hebrew) or null.
   */
  function triggerSegment(before) {
    if (!settings.aggressive) {
      const m = before.match(/(\S+)(\s+)(\S+)$/);
      if (!m) return null;
      if (!isWrongLayoutHebrew(m[1], HEB_WORDS)) return null;
      if (!isWrongLayoutHebrew(m[3], HEB_WORDS)) return null;
      return { len: m[0].length, conv: toHebrew(m[1]) + m[2] + toHebrew(m[3]) };
    }
    const m = before.match(/(\S+)$/);
    if (!m || !isWrongLayoutHebrew(m[1], HEB_WORDS)) return null;
    return { len: m[1].length, conv: toHebrew(m[1]) };
  }

  /** Try to fire on a form field; performs the backtrack replacement. */
  function tryTriggerField(el) {
    if (el.selectionStart !== el.selectionEnd) return false;
    const caret = el.selectionStart;
    const seg = triggerSegment(el.value.slice(0, caret));
    if (!seg) return false;
    const start = caret - seg.len;
    el.value = el.value.slice(0, start) + seg.conv + el.value.slice(caret);
    const c = start + seg.conv.length;
    el.setSelectionRange(c, c);
    el.dispatchEvent(new Event("input", { bubbles: true }));
    return true;
  }

  /** Try to fire on a contenteditable (single text node case). */
  function tryTriggerEditable() {
    const sel = window.getSelection();
    if (!sel || sel.rangeCount === 0 || !sel.isCollapsed) return false;
    const node = sel.anchorNode;
    if (!node || node.nodeType !== Node.TEXT_NODE) return false;
    const offset = sel.anchorOffset;
    const seg = triggerSegment(node.data.slice(0, offset));
    if (!seg) return false;
    const start = offset - seg.len;
    node.data = node.data.slice(0, start) + seg.conv + node.data.slice(offset);
    const range = document.createRange();
    const newOff = start + seg.conv.length;
    range.setStart(node, newOff);
    range.collapse(true);
    sel.removeAllRanges();
    sel.addRange(range);
    return true;
  }

  // ---- STATE 2/3: enter / leave override -----------------------------------

  function enterOverride(ctx) {
    mode = "override";
    overrideEl = ctx.el;
    overrideIsField = ctx.isField;
    UI.show(ctx.el, ctx.isField);
    scheduleReposition();
  }

  function exitOverride() {
    if (mode !== "override") return;
    UI.hide(overrideEl);
    mode = "passive";
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

  // ---- STATE 2/4: keystroke handling while in override ----------------------

  function handleOverrideKey(e) {
    // Focus moved elsewhere -> bail.
    const ctx = getContext(e.target);
    if (!ctx || ctx.el !== overrideEl) { exitOverride(); return; }

    // STATE 4: explicit layout-switch chord (Windows Alt+Shift) or Escape.
    if ((e.key === "Shift" && e.altKey) || (e.key === "Alt" && e.shiftKey) ||
        e.key === "Escape") {
      exitOverride();
      return;
    }

    // Never touch real shortcuts.
    if (e.ctrlKey || e.metaKey || e.altKey) return;

    // STATE 4: a genuine Hebrew keystroke means the OS layout is now Hebrew —
    // the user switched, so stop translating and let it flow natively.
    if (e.key.length === 1 && HEBREW_CHAR.test(e.key)) { exitOverride(); return; }

    // Live-map a printable layout key into its Hebrew glyph.
    const mapped = e.key.length === 1 ? keyToHeb(e.key) : null;
    if (mapped) {
      e.preventDefault();
      insertText(ctx, mapped);
      scheduleReposition();
      return;
    }

    // Non-mappable (space, digits, Enter…): let it through, stay in override.
    scheduleReposition();
  }

  // ---- Manual mode: convert the current selection on Ctrl+E ----------------

  function translateSelection(e) {
    const ctx = getContext(e.target);
    if (!ctx) return;
    const selected = ctx.isField
      ? ctx.el.value.slice(ctx.el.selectionStart, ctx.el.selectionEnd)
      : String(window.getSelection());
    if (!selected) return;
    const hebrew = toHebrew(selected);
    if (hebrew === selected) return;
    e.preventDefault();
    if (ctx.isField) {
      const start = ctx.el.selectionStart;
      ctx.el.value =
        ctx.el.value.slice(0, start) + hebrew +
        ctx.el.value.slice(ctx.el.selectionEnd);
      ctx.el.setSelectionRange(start, start + hebrew.length);
      ctx.el.dispatchEvent(new Event("input", { bubbles: true }));
    } else {
      document.execCommand("insertText", false, hebrew);
    }
  }

  // ---- the single delegated listener ---------------------------------------

  function onKeydown(e) {
    if (!settings.enabled) return;

    if (settings.manual) {
      if (e.ctrlKey && !e.altKey && !e.metaKey && (e.key === "e" || e.key === "E")) {
        translateSelection(e);
      }
      return;
    }

    if (mode === "override") { handleOverrideKey(e); return; }

    // STATE 1 (passive): evaluate only on a completed word.
    if (e.isComposing || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key !== " " && e.key !== "Enter") return;

    const ctx = getContext(e.target);
    if (!ctx) return;
    const fired = ctx.isField ? tryTriggerField(ctx.el) : tryTriggerEditable();
    if (fired) enterOverride(ctx);
    // We never preventDefault here: the Space/Enter keeps its native behavior.
  }

  document.addEventListener("keydown", onKeydown, true);

  // Drop override if the field loses focus, and keep the tooltip glued on scroll.
  document.addEventListener(
    "blur",
    (e) => { if (mode === "override" && e.target === overrideEl) exitOverride(); },
    true
  );
  window.addEventListener("scroll", scheduleReposition, true);
  window.addEventListener("resize", scheduleReposition);
})();
