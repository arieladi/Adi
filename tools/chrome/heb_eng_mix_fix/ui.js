/*
 * ui.js — HEB ENG MIX FIX (v2) — STATE 3 visuals
 * ----------------------------------------------------------------------------
 * The yellow feedback shown while the override state is active:
 *   • a floating tooltip pinned just above the typing caret, and
 *   • a soft yellow glow around the field being auto-fixed.
 *
 * Native <input>/<textarea> can't highlight a sub-range of their own text, so
 * caret pixel coordinates are computed with the well-known "mirror div"
 * technique; contenteditable uses the live Selection range rectangle directly.
 * ----------------------------------------------------------------------------
 */
(function (root, factory) {
  const api = factory();
  if (typeof window !== "undefined") window.HEBFIX_UI = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(this, function () {
  "use strict";

  let tip = null; // the singleton tooltip element

  // Computed-style properties the mirror div must copy to place the caret right.
  const MIRROR_PROPS = [
    "boxSizing","width","height","overflowX","overflowY",
    "borderTopWidth","borderRightWidth","borderBottomWidth","borderLeftWidth",
    "paddingTop","paddingRight","paddingBottom","paddingLeft",
    "fontStyle","fontVariant","fontWeight","fontStretch","fontSize",
    "lineHeight","fontFamily","textAlign","textTransform","textIndent",
    "letterSpacing","wordSpacing","tabSize"
  ];

  /**
   * Caret rectangle (viewport coords) for an <input>/<textarea>, via a hidden
   * mirror div that reproduces the field's text metrics.
   */
  function fieldCaretRect(el) {
    const pos = el.selectionEnd;
    const style = window.getComputedStyle(el);
    const div = document.createElement("div");
    const isInput = el.nodeName === "INPUT";

    div.style.position = "absolute";
    div.style.visibility = "hidden";
    div.style.whiteSpace = isInput ? "nowrap" : "pre-wrap";
    div.style.wordWrap = "break-word";
    div.style.top = "0";
    div.style.left = "-9999px";
    MIRROR_PROPS.forEach((p) => { div.style[p] = style[p]; });

    div.textContent = el.value.slice(0, pos);
    if (isInput) div.textContent = div.textContent.replace(/\s/g, " ");

    const marker = document.createElement("span");
    marker.textContent = el.value.slice(pos) || ".";
    div.appendChild(marker);
    document.body.appendChild(div);

    const rect = el.getBoundingClientRect();
    const top = rect.top + (marker.offsetTop - el.scrollTop);
    const left = rect.left + Math.min(marker.offsetLeft - el.scrollLeft, el.clientWidth);
    const lineHeight = parseInt(style.lineHeight, 10) || parseInt(style.fontSize, 10) || 16;

    document.body.removeChild(div);
    return { top, left, height: lineHeight };
  }

  /** Caret rectangle for a contenteditable via the live selection range. */
  function editableCaretRect() {
    const sel = window.getSelection();
    if (!sel || sel.rangeCount === 0) return null;
    let rect = sel.getRangeAt(0).getBoundingClientRect();
    if (rect && (rect.top || rect.left || rect.height)) {
      return { top: rect.top, left: rect.left, height: rect.height || 16 };
    }
    return null;
  }

  function caretRect(el, isField) {
    try {
      return isField ? fieldCaretRect(el) : editableCaretRect();
    } catch (e) {
      return null;
    }
  }

  function ensureTip() {
    if (tip) return tip;
    tip = document.createElement("div");
    tip.className = "hebfix-tooltip";
    tip.setAttribute("dir", "ltr");
    tip.innerHTML =
      '<span class="hebfix-dot"></span>' +
      "Auto-Fixing Hebrew — press <b>Alt+Shift</b> to switch";
    (document.body || document.documentElement).appendChild(tip);
    return tip;
  }

  /** Show/refresh the tooltip above the caret and glow the active field. */
  function show(el, isField) {
    const t = ensureTip();
    el.classList.add("hebfix-active");
    position(el, isField);
    t.classList.add("hebfix-visible");
  }

  function position(el, isField) {
    if (!tip) return;
    const r = caretRect(el, isField) || (function () {
      const b = el.getBoundingClientRect();
      return { top: b.top, left: b.left + 8, height: b.height };
    })();
    // Pin above the caret; clamp into the viewport.
    let top = r.top - 34;
    if (top < 4) top = r.top + r.height + 6; // flip below if no room above
    let left = Math.max(4, Math.min(r.left, window.innerWidth - 260));
    tip.style.top = top + "px";
    tip.style.left = left + "px";
  }

  /** Hide the tooltip and remove the field glow. */
  function hide(el) {
    if (tip) tip.classList.remove("hebfix-visible");
    if (el) el.classList.remove("hebfix-active");
  }

  return { show, position, hide, caretRect };
});
