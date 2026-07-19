/*
 * content.js — adi spell chekcer
 * ----------------------------------------------------------------------------
 * Runs in every frame. When the popup asks ("asc-fix"), the frame that owns
 * the focused editable element:
 *
 *   1. reads the full text of the active <input>/<textarea>/contenteditable;
 *   2. sends it to background.js, which POSTs it to LanguageTool;
 *   3. applies the first replacement of every match — IN REVERSE ORDER
 *      (end of string -> beginning), so earlier offsets stay valid while the
 *      string shrinks/grows under later edits;
 *   4. writes the corrected text back with the native value setter and fires
 *      `input` + `change` so React/Vue register the update;
 *   5. answers the popup with { status, fixed } for its status line.
 *
 * Frames WITHOUT the focused field stay silent (no sendResponse), so the one
 * frame that owns the field is the one the popup hears back from.
 * ----------------------------------------------------------------------------
 */
(function () {
  "use strict";

  // ---- pure replacement logic (exported for tests) ---------------------------

  /**
   * Apply LanguageTool matches to `text`, first replacement each, applied
   * end-to-start so offsets never invalidate. Returns {text, fixed}.
   */
  function applyMatches(text, matches) {
    const usable = (matches || [])
      .filter((m) =>
        Number.isInteger(m.offset) && Number.isInteger(m.length) &&
        m.offset >= 0 && m.length > 0 && m.offset + m.length <= text.length &&
        Array.isArray(m.replacements) && m.replacements.length &&
        typeof m.replacements[0].value === "string")
      .sort((a, b) => b.offset - a.offset);   // REVERSE order: end -> start

    let out = text;
    let fixed = 0;
    let lastStart = Infinity;                  // guard against overlapping matches
    for (const m of usable) {
      const end = m.offset + m.length;
      if (end > lastStart) continue;           // overlaps one we already applied
      out = out.slice(0, m.offset) + m.replacements[0].value + out.slice(end);
      lastStart = m.offset;
      fixed++;
    }
    return { text: out, fixed };
  }

  // ---- the focused editable ---------------------------------------------------

  /** The active editable element of THIS frame, or null. */
  function activeEditable() {
    const el = document.activeElement;
    if (!el) return null;
    if (el.nodeName === "TEXTAREA" && !el.disabled && !el.readOnly) return el;
    if (el.nodeName === "INPUT" && !el.disabled && !el.readOnly) {
      const type = (el.type || "text").toLowerCase();
      if (["text", "search", "url", "tel", "email", ""].includes(type)) return el;
    }
    if (el.isContentEditable) return el;
    return null;
  }

  function readText(el) {
    return "value" in el && el.nodeName !== "DIV" ? el.value : el.innerText;
  }

  /** Framework-safe write-back with native events. */
  function writeText(el, text) {
    if (el.nodeName === "TEXTAREA" || el.nodeName === "INPUT") {
      const proto = el.nodeName === "TEXTAREA"
        ? HTMLTextAreaElement.prototype
        : HTMLInputElement.prototype;
      Object.getOwnPropertyDescriptor(proto, "value").set.call(el, text);
      el.setSelectionRange(text.length, text.length);
      el.dispatchEvent(new Event("input", { bubbles: true }));
      el.dispatchEvent(new Event("change", { bubbles: true }));
    } else {
      // contenteditable: select-all + insertText keeps the undo stack alive.
      el.focus();
      const sel = window.getSelection();
      const range = document.createRange();
      range.selectNodeContents(el);
      sel.removeAllRanges();
      sel.addRange(range);
      document.execCommand("insertText", false, text);
    }
  }

  // ---- popup message: fix the focused field -----------------------------------

  if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.onMessage) {
    chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
      if (!msg || msg.type !== "asc-fix") return;

      const el = activeEditable();
      if (!el) return;                        // not our frame — stay silent

      const text = readText(el);
      if (!text.trim()) {
        sendResponse({ status: "empty" });
        return;
      }

      chrome.runtime.sendMessage({ type: "asc-check", text }, (res) => {
        if (chrome.runtime.lastError || !res) {
          sendResponse({ status: "network" });
          return;
        }
        if (res.error) {
          sendResponse({ status: res.error });
          return;
        }
        const { text: fixedText, fixed } = applyMatches(text, res.matches);
        if (!fixed || fixedText === text) {
          sendResponse({ status: "clean" });
          return;
        }
        // The user may have kept typing while LanguageTool was thinking.
        if (readText(el) !== text) {
          sendResponse({ status: "changed" });
          return;
        }
        writeText(el, fixedText);
        sendResponse({ status: "fixed", fixed });
      });
      return true; // async response
    });
  }

  // Node test hook.
  if (typeof module !== "undefined" && module.exports) {
    module.exports = { applyMatches };
  }
})();
