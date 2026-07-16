/*
 * content.js — HEB ENG MIX FIX
 * ----------------------------------------------------------------------------
 * Watches editable fields and, when the user finishes a "word" (space / Enter),
 * checks whether the last token is Hebrew that was accidentally typed with an
 * English layout. If so, it rewrites just that token in place, preserving the
 * caret and the surrounding text.
 *
 * Performance notes:
 *   • A single delegated `keydown` listener on `document` (capture phase) — no
 *     per-field listeners, no MutationObserver churn, works for fields added
 *     later and inside same-origin iframes (manifest has all_frames:true).
 *   • Work happens ONLY on the space / Enter boundary, never on every keystroke,
 *     so steady-state typing cost is a couple of cheap comparisons per key.
 *   • The heavy lifting (map + dictionary lookup) is O(word length) and runs at
 *     most once per completed word.
 * ----------------------------------------------------------------------------
 */
(function () {
  "use strict";

  const { evaluate, toHebrew } = window.HEBFIX;
  const HEB_WORDS = window.HEB_WORDS;

  // Live settings, hydrated from chrome.storage and kept in sync.
  //   manual === false -> Automatic: fix each word on Space/Enter (default).
  //   manual === true  -> Manual: only convert the current selection on Ctrl+E.
  const state = { enabled: true, aggressive: false, manual: false };

  chrome.storage.local.get(
    { enabled: true, aggressive: false, manual: false },
    (s) => {
      state.enabled = s.enabled;
      state.aggressive = s.aggressive;
      state.manual = s.manual;
    }
  );

  chrome.storage.onChanged.addListener((changes, area) => {
    if (area !== "local") return;
    if (changes.enabled) state.enabled = changes.enabled.newValue;
    if (changes.aggressive) state.aggressive = changes.aggressive.newValue;
    if (changes.manual) state.manual = changes.manual.newValue;
  });

  // ---- element helpers ------------------------------------------------------

  /** Is this an <input>/<textarea> we should touch? */
  function isTextFormField(el) {
    if (!el) return false;
    const tag = el.tagName;
    if (tag === "TEXTAREA") return !el.disabled && !el.readOnly;
    if (tag === "INPUT") {
      // Only free-text inputs; skip password/email/number/etc.
      const type = (el.type || "text").toLowerCase();
      const ok = ["text", "search", "url", "tel", ""].includes(type);
      return ok && !el.disabled && !el.readOnly;
    }
    return false;
  }

  /** Nearest contenteditable host, or null. */
  function editableHost(el) {
    let node = el;
    while (node && node.nodeType === 1) {
      if (node.isContentEditable) return node;
      node = node.parentElement;
    }
    return null;
  }

  // Match a trailing token (letters + the punctuation keys that map to Hebrew).
  const TRAILING_TOKEN = /[a-zA-Z,.;']+$/;

  // ---- input / textarea path ------------------------------------------------

  function handleFormField(el) {
    // Only operate on a collapsed caret at a word boundary.
    if (el.selectionStart == null || el.selectionStart !== el.selectionEnd) return;

    const caret = el.selectionStart;
    const before = el.value.slice(0, caret);
    const m = before.match(TRAILING_TOKEN);
    if (!m) return;

    const token = m[0];
    const replacement = evaluate(token, HEB_WORDS, state.aggressive);
    if (!replacement) return;

    const start = caret - token.length;
    el.value = el.value.slice(0, start) + replacement + el.value.slice(caret);

    // Place caret right after the replaced token (the space/Enter that triggered
    // us performs its own default action from here).
    const newCaret = start + replacement.length;
    el.setSelectionRange(newCaret, newCaret);

    // Let frameworks (React/Vue) notice the programmatic change.
    el.dispatchEvent(new Event("input", { bubbles: true }));
  }

  // ---- contenteditable path -------------------------------------------------

  function handleContentEditable() {
    const sel = window.getSelection();
    if (!sel || sel.rangeCount === 0 || !sel.isCollapsed) return;

    const node = sel.anchorNode;
    // Keep it safe & simple: only handle words that live inside a single text
    // node (the overwhelmingly common case). Bail otherwise.
    if (!node || node.nodeType !== Node.TEXT_NODE) return;

    const offset = sel.anchorOffset;
    const before = node.data.slice(0, offset);
    const m = before.match(TRAILING_TOKEN);
    if (!m) return;

    const token = m[0];
    const replacement = evaluate(token, HEB_WORDS, state.aggressive);
    if (!replacement) return;

    const start = offset - token.length;
    node.data = node.data.slice(0, start) + replacement + node.data.slice(offset);

    // Restore a collapsed caret just after the replacement.
    const range = document.createRange();
    const newOffset = start + replacement.length;
    range.setStart(node, newOffset);
    range.collapse(true);
    sel.removeAllRanges();
    sel.addRange(range);
  }

  // ---- manual mode: convert the current selection on Ctrl+E -----------------

  function translateSelection(e) {
    const target = e.target;

    // Pull the selected text from whichever surface has focus.
    let selected = "";
    const isField = isTextFormField(target);
    if (isField) {
      selected = target.value.slice(target.selectionStart, target.selectionEnd);
    } else if (editableHost(target)) {
      selected = String(window.getSelection());
    } else {
      return; // not an editable surface we manage
    }

    if (!selected) return; // nothing selected → let Ctrl+E do its normal thing

    // Manual is an explicit user action, so translate the whole selection
    // verbatim (every keystroke), no dictionary gate.
    const hebrew = toHebrew(selected);
    if (hebrew === selected) return;

    e.preventDefault();

    if (isField) {
      const start = target.selectionStart;
      target.value =
        target.value.slice(0, start) +
        hebrew +
        target.value.slice(target.selectionEnd);
      target.setSelectionRange(start, start + hebrew.length);
      target.dispatchEvent(new Event("input", { bubbles: true }));
    } else {
      // Replaces the live selection in contenteditable, preserving the undo
      // stack and firing input events. Works across multiple text nodes.
      document.execCommand("insertText", false, hebrew);
    }
  }

  // ---- the single delegated listener ---------------------------------------

  function onKeydown(e) {
    if (!state.enabled) return;

    // Manual mode: the only trigger is Ctrl+E on a selection.
    if (state.manual) {
      if (e.ctrlKey && !e.altKey && !e.metaKey && (e.key === "e" || e.key === "E")) {
        translateSelection(e);
      }
      return; // no automatic Space/Enter behavior in manual mode
    }

    // Automatic mode: fix the finished word on Space / Enter.
    // Ignore IME composition and modifier combos (shortcuts stay untouched).
    if (e.key !== " " && e.key !== "Enter") return;
    if (e.isComposing || e.ctrlKey || e.metaKey || e.altKey) return;

    const target = e.target;
    if (isTextFormField(target)) {
      handleFormField(target);
    } else if (editableHost(target)) {
      handleContentEditable();
    }
    // We never call preventDefault: the space/Enter keeps its native behavior
    // (insert space, newline, or submit) acting on the text we just rewrote.
  }

  // Capture phase so we run before the field processes the key, and before most
  // site handlers can stopPropagation().
  document.addEventListener("keydown", onKeydown, true);
})();
