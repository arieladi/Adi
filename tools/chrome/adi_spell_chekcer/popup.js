/*
 * popup.js — adi spell chekcer
 * ----------------------------------------------------------------------------
 * One button. On click, ask the active tab's content scripts to fix the
 * focused text field; the frame that owns the field answers with a status.
 * If NO frame answers (nothing focused/editable), chrome.runtime.lastError
 * fires — that's the "No text field selected" case.
 * ----------------------------------------------------------------------------
 */
(function () {
  "use strict";

  const btn = document.getElementById("fixall");
  const statusEl = document.getElementById("status");

  const MESSAGES = {
    fixed:   (n) => `Fixed ${n} issue${n === 1 ? "" : "s"} ✓`,
    clean:   () => "No issues found ✓",
    empty:   () => "The text box is empty.",
    changed: () => "Text changed while checking — try again.",
    rate:    () => "Rate limit reached — wait a minute.",
    long:    () => "Text too long (20k char limit).",
    lang:    () => "Language not supported by LanguageTool.",
    network: () => "Network error — try again.",
    none:    () => "No text field selected on the page."
  };

  function show(status, n) {
    const make = MESSAGES[status] || ((x) => `Error: ${status}`);
    statusEl.textContent = make(n);
    statusEl.className =
      status === "fixed" || status === "clean" ? "ok" :
      status === "empty" ? "" : "err";
  }

  btn.addEventListener("click", () => {
    btn.disabled = true;
    btn.textContent = "Checking…";
    statusEl.textContent = "";
    statusEl.className = "";

    chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
      const tab = tabs && tabs[0];
      const done = (status, n) => {
        btn.disabled = false;
        btn.textContent = "✓ Fix All";
        show(status, n);
      };
      if (!tab || tab.id == null) { done("none"); return; }

      chrome.tabs.sendMessage(tab.id, { type: "asc-fix" }, (res) => {
        if (chrome.runtime.lastError || !res) {
          // No frame owned a focused editable (or no content script here).
          done("none");
          return;
        }
        done(res.status, res.fixed);
      });
    });
  });
})();
