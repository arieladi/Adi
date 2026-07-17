/*
 * ui.js — HEB ENG MIX FIX (v3) — floating visuals
 * ----------------------------------------------------------------------------
 * Two floating elements, both positioned off the *exact caret coordinates*:
 *
 *   • the override tooltip (STATE 3): yellow, pinned ABOVE the caret while the
 *     machine is live-converting keystrokes;
 *   • the suggestion badge (Grammarly-style): white pill pinned BELOW the
 *     caret, offering a one-click / Tab fix from the Suggest API.
 *
 * Caret coordinates: native <input>/<textarea> expose no caret rect, so we use
 * the "mirror div" technique — clone the field's text metrics into a hidden
 * div, drop a marker span at the caret index, and measure it. contenteditable
 * uses the live Selection range rect directly. Every placement is clamped into
 * the viewport so neither element can render off-screen.
 * ----------------------------------------------------------------------------
 */
(function (root, factory) {
  const api = factory();
  if (typeof window !== "undefined") window.HEBFIX_UI = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(this, function () {
  "use strict";

  let tip = null;   // override tooltip singleton
  let badge = null; // fix panel singleton

  // Computed-style props the mirror div must copy for faithful caret metrics.
  const MIRROR_PROPS = [
    "boxSizing","width","height","overflowX","overflowY",
    "borderTopWidth","borderRightWidth","borderBottomWidth","borderLeftWidth",
    "paddingTop","paddingRight","paddingBottom","paddingLeft",
    "fontStyle","fontVariant","fontWeight","fontStretch","fontSize",
    "lineHeight","fontFamily","textAlign","textTransform","textIndent",
    "letterSpacing","wordSpacing","tabSize","direction"
  ];

  // ---- caret coordinates -----------------------------------------------------

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
    const lineHeight = parseInt(style.lineHeight, 10) || parseInt(style.fontSize, 10) || 16;

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

  /** Clamp a floating box into the viewport; flips above when out of room. */
  function place(node, r, below) {
    // Measure invisibly first so clamping uses real dimensions.
    node.style.visibility = "hidden";
    node.style.display = "inline-flex";
    const w = node.offsetWidth, h = node.offsetHeight;

    let top = below ? r.top + r.height + 6 : r.top - h - 8;
    if (below && top + h > window.innerHeight - 4) top = r.top - h - 8; // flip up
    if (!below && top < 4) top = r.top + r.height + 6;                  // flip down
    top = Math.max(4, Math.min(top, window.innerHeight - h - 4));

    const left = Math.max(4, Math.min(r.left, window.innerWidth - w - 8));

    node.style.top = top + "px";
    node.style.left = left + "px";
    node.style.visibility = "";
  }

  // ---- override tooltip (STATE 3) ---------------------------------------------

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
    if (tip) place(tip, caretRect(el, isField), false);
  }

  function hide(el) {
    if (tip) tip.classList.remove("hebfix-visible");
    if (el) el.classList.remove("hebfix-active");
  }

  // ---- fix panel (Grammarly-style, all misspellings at once) -------------------

  function ensureBadge() {
    if (badge) return badge;
    badge = document.createElement("div");
    badge.className = "hebfix-badge";
    // Keep the field focused: eat mousedown before it can blur the input.
    badge.addEventListener("mousedown", (e) => e.preventDefault());
    (document.body || document.documentElement).appendChild(badge);
    return badge;
  }

  /**
   * Show every pending fix in a floating panel below the caret.
   * `fixes` is [{bad, text, …}]; handlers = {onFix(fix), onFixAll, onDismiss}.
   * Rows are rebuilt on each call, so applying one fix just re-renders.
   */
  function showFixes(el, isField, fixes, handlers) {
    const b = ensureBadge();
    b.textContent = "";

    for (const fix of fixes) {
      const row = document.createElement("div");
      row.className = "hebfix-fix-row";
      row.setAttribute("role", "button");
      row.innerHTML =
        '<span class="hebfix-bad" dir="auto"></span>' +
        '<span class="hebfix-arrow">→</span>' +
        '<span class="hebfix-good" dir="auto"></span>';
      row.querySelector(".hebfix-bad").textContent = fix.bad;
      row.querySelector(".hebfix-good").textContent = fix.text;
      row.addEventListener("click", () => handlers.onFix(fix));
      b.appendChild(row);
    }

    const foot = document.createElement("div");
    foot.className = "hebfix-panel-foot";
    foot.innerHTML =
      '<span class="hebfix-fixall" role="button">' +
      '<span class="hebfix-check">✓</span>Fix all <kbd>Tab</kbd></span>' +
      '<span class="hebfix-badge-close" role="button" title="Dismiss">✕</span>';
    foot.querySelector(".hebfix-fixall")
      .addEventListener("click", () => handlers.onFixAll());
    foot.querySelector(".hebfix-badge-close")
      .addEventListener("click", () => handlers.onDismiss());
    b.appendChild(foot);

    place(b, caretRect(el, isField), true);
    b.classList.add("hebfix-visible");
  }

  function badgeVisible() {
    return !!(badge && badge.classList.contains("hebfix-visible"));
  }

  function hideBadge() {
    if (!badge) return;
    badge.classList.remove("hebfix-visible");
    // place() sets an inline display for measuring; clear it so the hidden
    // panel can't sit invisibly over the page and swallow clicks.
    badge.style.display = "none";
  }

  return { show, position, hide, caretRect, showFixes, hideBadge, badgeVisible };
});
