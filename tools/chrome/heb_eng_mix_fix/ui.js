/*
 * ui.js — HEB ENG MIX FIX (v4) — floating visuals
 * ----------------------------------------------------------------------------
 * 1) Layer A override tooltip + caret math (mirror-div technique).
 * 2) Grammarly-style word-level HIGHLIGHTING:
 *      • <input>/<textarea>  — a transparent overlay <div> that mirrors the
 *        field's box model, typography and scroll so wrapped text lands in the
 *        exact same pixels; misspelled words are wrapped in <mark> and given a
 *        squiggly underline. The overlay is pointer-events:none so typing and
 *        clicking pass straight through to the real field.
 *      • contenteditable     — real geometry is available, so we draw squiggle
 *        <div>s positioned off Range.getClientRects().
 *    A hover badge (shown when the pointer is over a flagged word) offers the
 *    single fix and "Fix all".
 *
 * THE ALIGNMENT MATH (the hard part):
 *   A textarea lays its text out inside its *content box*, whose width is
 *   `clientWidth - paddingLeft - paddingRight` (clientWidth already excludes any
 *   vertical scrollbar). So the mirror uses `box-sizing:content-box`, a content
 *   width of exactly that value, the same paddings, and is pinned to the field's
 *   *padding-box* origin (rect.left/top + border widths). Identical font metrics
 *   + identical wrap width ⇒ identical line breaks. Internal scroll is mirrored
 *   by copying scrollTop/scrollLeft. Getting width from clientWidth (not
 *   offsetWidth) is what prevents the classic scrollbar-induced drift.
 * ----------------------------------------------------------------------------
 */
(function (root, factory) {
  const api = factory();
  if (typeof window !== "undefined") window.HEBFIX_UI = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(this, function () {
  "use strict";

  // ---- shared helpers --------------------------------------------------------

  const num = (v) => parseFloat(v) || 0;
  const esc = (s) =>
    s.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));

  /** Clamp a floating box into the viewport; `below` flips it under the anchor. */
  function place(node, r, below) {
    node.style.visibility = "hidden";
    node.style.display = "inline-flex";
    const w = node.offsetWidth, h = node.offsetHeight;
    let top = below ? r.top + r.height + 6 : r.top - h - 8;
    if (below && top + h > window.innerHeight - 4) top = r.top - h - 8;
    if (!below && top < 4) top = r.top + r.height + 6;
    top = Math.max(4, Math.min(top, window.innerHeight - h - 4));
    const left = Math.max(4, Math.min(r.left, window.innerWidth - w - 8));
    node.style.top = top + "px";
    node.style.left = left + "px";
    node.style.visibility = "";
  }

  // ============================================================================
  // Layer A — override tooltip + caret coordinates
  // ============================================================================

  let tip = null;

  const MIRROR_PROPS = [
    "boxSizing","width","height","overflowX","overflowY",
    "borderTopWidth","borderRightWidth","borderBottomWidth","borderLeftWidth",
    "paddingTop","paddingRight","paddingBottom","paddingLeft",
    "fontStyle","fontVariant","fontWeight","fontStretch","fontSize",
    "lineHeight","fontFamily","textAlign","textTransform","textIndent",
    "letterSpacing","wordSpacing","tabSize","direction"
  ];

  /** Caret rect (viewport coords) for <input>/<textarea> via a throwaway mirror. */
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

  function editableCaretRect() {
    const sel = window.getSelection();
    if (!sel || sel.rangeCount === 0) return null;
    const rect = sel.getRangeAt(0).getBoundingClientRect();
    if (rect && (rect.top || rect.left || rect.height)) {
      return { top: rect.top, left: rect.left, height: rect.height || 16 };
    }
    return null;
  }

  function caretRect(el, isField) {
    let r = null;
    try { r = isField ? fieldCaretRect(el) : editableCaretRect(); } catch (e) {}
    if (r) return r;
    const b = el.getBoundingClientRect();
    return { top: b.top, left: b.left + 8, height: b.height };
  }

  function ensureTip() {
    if (tip) return tip;
    tip = document.createElement("div");
    tip.className = "hebfix-tooltip";
    tip.setAttribute("dir", "ltr");
    (document.body || document.documentElement).appendChild(tip);
    return tip;
  }

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

  // ============================================================================
  // Layer B — word-level highlighting + hover badge
  // ============================================================================

  // Which typography/box props the overlay must copy for pixel-perfect wrapping.
  const OVERLAY_FONT_PROPS = [
    "fontFamily","fontSize","fontWeight","fontStyle","fontVariant","fontStretch",
    "lineHeight","letterSpacing","wordSpacing","textTransform","textIndent",
    "textAlign","tabSize","direction","textRendering"
  ];

  let host = null;         // field or contenteditable currently highlighted
  let hostIsField = false;
  let hostNode = null;     // CE text node (contenteditable only)
  let items = [];          // [{start,end,original,corrected,key}], absolute offsets
  let handlers = null;     // {onFix, onFixAll, onDismiss}

  let overlay = null;      // mirror div (field)
  let layer = null;        // squiggle container (contenteditable)

  let hbadge = null;       // hover badge
  let hbadgeTimer = 0;
  let hoveredKey = null;

  // ---- the transparent textarea/input overlay --------------------------------

  /** Copy the field's box model + typography onto the mirror so text aligns. */
  function styleOverlay(o, el) {
    const cs = window.getComputedStyle(el);
    const rect = el.getBoundingClientRect();
    const padL = num(cs.paddingLeft), padR = num(cs.paddingRight);
    const padT = num(cs.paddingTop), padB = num(cs.paddingBottom);
    const bL = num(cs.borderLeftWidth), bT = num(cs.borderTopWidth);

    o.style.position = "fixed";
    o.style.boxSizing = "content-box";
    // Pin to the field's PADDING box (inside the border).
    o.style.left = (rect.left + bL) + "px";
    o.style.top = (rect.top + bT) + "px";
    // Content width = the field's text layout width (clientWidth excludes the
    // scrollbar), so wrapping matches exactly.
    o.style.width = Math.max(0, el.clientWidth - padL - padR) + "px";
    o.style.height = Math.max(0, el.clientHeight - padT - padB) + "px";
    o.style.paddingTop = cs.paddingTop;
    o.style.paddingRight = cs.paddingRight;
    o.style.paddingBottom = cs.paddingBottom;
    o.style.paddingLeft = cs.paddingLeft;

    OVERLAY_FONT_PROPS.forEach((p) => { o.style[p] = cs[p]; });
    o.style.whiteSpace = el.nodeName === "INPUT" ? "pre" : "pre-wrap";
    o.style.overflowWrap = "break-word";
    o.style.wordWrap = "break-word";
    o.style.wordBreak = cs.wordBreak;

    o.style.margin = "0";
    o.style.border = "0 solid transparent";
    o.style.overflow = "hidden";
    o.style.pointerEvents = "none";   // clicks/caret pass through to the field
    o.style.background = "transparent";
    o.style.color = "transparent";    // only the squiggles are visible
    o.style.zIndex = "2147483646";
  }

  function buildMarksHTML(text, corrections) {
    const sorted = corrections.slice().sort((a, b) => a.start - b.start);
    let html = "", last = 0;
    for (const c of sorted) {
      if (c.start < last || c.end > text.length) continue; // stale / overlapping
      html += esc(text.slice(last, c.start));
      html += '<mark class="hebfix-mark" data-key="' +
        esc(c.key).replace(/"/g, "&quot;") + '">' +
        esc(text.slice(c.start, c.end)) + "</mark>";
      last = c.end;
    }
    html += esc(text.slice(last));
    if (text.endsWith("\n")) html += "​"; // preserve trailing line height
    return html;
  }

  function renderField() {
    if (!overlay) {
      overlay = document.createElement("div");
      overlay.className = "hebfix-overlay";
      (document.body || document.documentElement).appendChild(overlay);
    }
    styleOverlay(overlay, host);
    overlay.innerHTML = buildMarksHTML(host.value, items);
    overlay.scrollTop = host.scrollTop;
    overlay.scrollLeft = host.scrollLeft;
    overlay.style.display = "block";
  }

  // ---- the contenteditable squiggle layer ------------------------------------

  function ensureLayer() {
    if (layer) return layer;
    layer = document.createElement("div");
    layer.className = "hebfix-squiggle-layer";
    layer.style.cssText =
      "position:fixed;left:0;top:0;pointer-events:none;z-index:2147483646;";
    (document.body || document.documentElement).appendChild(layer);
    return layer;
  }

  function renderEditable() {
    ensureLayer();
    layer.innerHTML = "";
    if (!hostNode) return;
    for (const c of items) {
      let rects;
      try {
        const r = document.createRange();
        r.setStart(hostNode, c.start);
        r.setEnd(hostNode, c.end);
        rects = r.getClientRects();
      } catch (e) { continue; }
      for (const rc of rects) {
        const u = document.createElement("div");
        u.className = "hebfix-squiggle";
        u.dataset.key = c.key;
        u.style.left = rc.left + "px";
        u.style.top = rc.top + "px";
        u.style.width = rc.width + "px";
        u.style.height = rc.height + "px";
        layer.appendChild(u);
      }
    }
    layer.style.display = "block";
  }

  // ---- public render / reposition / clear ------------------------------------

  /** Render highlights for `el`; `node` is the CE text node (else null). */
  function renderHighlights(el, isField, node, corrections, hnd) {
    host = el; hostIsField = isField; hostNode = node;
    items = corrections; handlers = hnd;
    if (!corrections.length) { clearHighlights(); return; }
    ensureMouseTracking();
    if (isField) { renderField(); if (layer) layer.style.display = "none"; }
    else { renderEditable(); if (overlay) overlay.style.display = "none"; }
  }

  function repositionHighlights() {
    if (!host || !items.length) return;
    if (hostIsField) renderField(); else renderEditable();
    if (hoveredKey) hideHoverBadge(); // geometry moved — drop the stale badge
  }

  function clearHighlights() {
    host = null; hostNode = null; items = []; handlers = null;
    if (overlay) { overlay.innerHTML = ""; overlay.style.display = "none"; }
    if (layer) { layer.innerHTML = ""; layer.style.display = "none"; }
    hideHoverBadge();
  }

  function hasHighlights() { return !!(host && items.length); }

  // ---- hover badge -----------------------------------------------------------

  function ensureHoverBadge() {
    if (hbadge) return hbadge;
    hbadge = document.createElement("div");
    hbadge.className = "hebfix-badge";
    hbadge.addEventListener("mousedown", (e) => e.preventDefault()); // keep focus
    hbadge.addEventListener("mouseenter", () => clearTimeout(hbadgeTimer));
    hbadge.addEventListener("mouseleave", scheduleHideBadge);
    (document.body || document.documentElement).appendChild(hbadge);
    return hbadge;
  }

  function showHoverBadge(c, rect) {
    const b = ensureHoverBadge();
    hoveredKey = c.key;
    b.textContent = "";

    const pair = document.createElement("div");
    pair.className = "hebfix-fix-row";
    pair.innerHTML =
      '<span class="hebfix-bad" dir="auto"></span>' +
      '<span class="hebfix-arrow">→</span>' +
      '<span class="hebfix-good" dir="auto"></span>';
    pair.querySelector(".hebfix-bad").textContent = c.original;
    pair.querySelector(".hebfix-good").textContent = c.corrected;
    b.appendChild(pair);

    const foot = document.createElement("div");
    foot.className = "hebfix-panel-foot";
    const many = items.length > 1;
    foot.innerHTML =
      '<span class="hebfix-fixone" role="button"><span class="hebfix-check">✓</span>Fix</span>' +
      (many ? '<span class="hebfix-fixall" role="button">Fix all <kbd>Tab</kbd></span>' : "") +
      '<span class="hebfix-badge-close" role="button" title="Ignore">✕</span>';
    foot.querySelector(".hebfix-fixone")
      .addEventListener("click", () => handlers && handlers.onFix(c));
    if (many) foot.querySelector(".hebfix-fixall")
      .addEventListener("click", () => handlers && handlers.onFixAll());
    foot.querySelector(".hebfix-badge-close")
      .addEventListener("click", () => handlers && handlers.onDismiss(c));
    b.appendChild(foot);

    place(b, { top: rect.top, left: rect.left, height: rect.height }, false);
    b.classList.add("hebfix-visible");
    b.style.display = "inline-flex";
  }

  function scheduleHideBadge() {
    clearTimeout(hbadgeTimer);
    hbadgeTimer = setTimeout(hideHoverBadge, 260);
  }
  function hideHoverBadge() {
    if (hbadge) { hbadge.classList.remove("hebfix-visible"); hbadge.style.display = "none"; }
    hoveredKey = null;
  }
  function badgeVisible() {
    return !!(hbadge && hbadge.classList.contains("hebfix-visible"));
  }

  // Which flagged word (if any) is under the pointer? Uses the live geometry of
  // the <mark>/squiggle elements (valid even though the overlay ignores events).
  function hitTest(x, y) {
    const nodes = hostIsField
      ? (overlay ? overlay.querySelectorAll("mark") : [])
      : (layer ? layer.querySelectorAll(".hebfix-squiggle") : []);
    for (const el of nodes) {
      const c = items.find((it) => it.key === el.dataset.key);
      if (!c) continue;
      for (const r of el.getClientRects()) {
        if (x >= r.left - 1 && x <= r.right + 1 && y >= r.top - 2 && y <= r.bottom + 8) {
          return { c, rect: r };
        }
      }
    }
    return null;
  }

  let mouseTracking = false;
  function ensureMouseTracking() {
    if (mouseTracking) return;
    mouseTracking = true;
    document.addEventListener("mousemove", (e) => {
      if (!host || !items.length) return;
      const hit = hitTest(e.clientX, e.clientY);
      if (hit) {
        clearTimeout(hbadgeTimer);
        if (hoveredKey !== hit.c.key) showHoverBadge(hit.c, hit.rect);
      } else if (badgeVisible()) {
        scheduleHideBadge();
      }
    }, true);
  }

  return {
    // Layer A
    show, position, hide, caretRect,
    // Layer B
    renderHighlights, repositionHighlights, clearHighlights, hasHighlights,
    hideHoverBadge, badgeVisible
  };
});
