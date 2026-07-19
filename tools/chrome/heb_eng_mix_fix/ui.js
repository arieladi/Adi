/*
 * ui.js — HEB ENG MIX FIX (v7) — the override tooltip
 * ----------------------------------------------------------------------------
 * One floating element: the Layer A override tooltip (yellow, pinned above the
 * caret while the machine is live-converting keystrokes), plus the caret math
 * it needs. Native <input>/<textarea> expose no caret rect, so a throwaway
 * "mirror div" clones the field's text metrics, drops a marker span at the
 * caret index, and measures it; contenteditable uses the live Selection rect.
 * ----------------------------------------------------------------------------
 */
(function (root, factory) {
  const api = factory();
  if (typeof window !== "undefined") window.HEBFIX_UI = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(this, function () {
  "use strict";

  let tip = null; // tooltip singleton

  const num = (v) => parseFloat(v) || 0;

  // ---- caret coordinates -----------------------------------------------------

  const MIRROR_PROPS = [
    "boxSizing","width","height","overflowX","overflowY",
    "borderTopWidth","borderRightWidth","borderBottomWidth","borderLeftWidth",
    "paddingTop","paddingRight","paddingBottom","paddingLeft",
    "fontStyle","fontVariant","fontWeight","fontStretch","fontSize",
    "lineHeight","fontFamily","textAlign","textTransform","textIndent",
    "letterSpacing","wordSpacing","tabSize","direction"
  ];

  /** Caret rect (viewport coords) for <input>/<textarea> via a mirror div. */
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
    const marker = document.createElement("span");
    marker.textContent = el.value.slice(pos) || ".";
    div.appendChild(marker);
    document.body.appendChild(div);
    const rect = el.getBoundingClientRect();
    const top = rect.top + (marker.offsetTop - el.scrollTop);
    const left = rect.left + Math.min(marker.offsetLeft - el.scrollLeft, el.clientWidth);
    const lineHeight = num(style.lineHeight) || num(style.fontSize) || 16;
    document.body.removeChild(div);
    return { top, left, height: lineHeight };
  }

  /** Caret rect for contenteditable via the live Selection. */
  function editableCaretRect() {
    const sel = window.getSelection();
    if (!sel || sel.rangeCount === 0) return null;
    const rect = sel.getRangeAt(0).getBoundingClientRect();
    if (rect && (rect.top || rect.left || rect.height)) {
      return { top: rect.top, left: rect.left, height: rect.height || 16 };
    }
    return null;
  }

  /** Caret rect with a graceful fallback to the field's own box. */
  function caretRect(el, isField) {
    let r = null;
    try { r = isField ? fieldCaretRect(el) : editableCaretRect(); } catch (e) {}
    if (r) return r;
    const b = el.getBoundingClientRect();
    return { top: b.top, left: b.left + 8, height: b.height };
  }

  /** Clamp the tooltip into the viewport; flips below when out of room. */
  function place(node, r) {
    node.style.visibility = "hidden";
    node.style.display = "inline-flex";
    const w = node.offsetWidth, h = node.offsetHeight;
    let top = r.top - h - 8;
    if (top < 4) top = r.top + r.height + 6; // flip below
    top = Math.max(4, Math.min(top, window.innerHeight - h - 4));
    const left = Math.max(4, Math.min(r.left, window.innerWidth - w - 8));
    node.style.top = top + "px";
    node.style.left = left + "px";
    node.style.visibility = "";
  }

  // ---- the override tooltip ----------------------------------------------------

  function ensureTip() {
    if (tip) return tip;
    tip = document.createElement("div");
    tip.className = "hebfix-tooltip";
    tip.setAttribute("dir", "ltr");
    (document.body || document.documentElement).appendChild(tip);
    return tip;
  }

  /** Show the yellow tooltip; `dir` is "en2he" or "he2en". */
  function show(el, isField, dir) {
    const t = ensureTip();
    t.innerHTML =
      '<span class="hebfix-dot"></span>Auto-Fixing ' +
      (dir === "he2en" ? "English" : "Hebrew") +
      " — press <b>Alt+Shift</b> to switch";
    el.classList.add("hebfix-active");
    position(el, isField);
    t.classList.add("hebfix-visible");
  }

  function position(el, isField) {
    if (tip) place(tip, caretRect(el, isField));
  }

  function hide(el) {
    if (tip) tip.classList.remove("hebfix-visible");
    if (el) el.classList.remove("hebfix-active");
  }

  return { show, position, hide, caretRect };
});
