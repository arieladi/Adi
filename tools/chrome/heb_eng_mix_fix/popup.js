/*
 * popup.js — HEB ENG MIX FIX (v7)
 * Reads/writes the two toggles from chrome.storage.local. The content script
 * listens for storage changes, so toggling here takes effect live in open tabs.
 */
(function () {
  "use strict";

  const enabledEl = document.getElementById("enabled");
  const manualEl = document.getElementById("manual");
  const statusEl = document.getElementById("status");

  function render(enabled, manual) {
    manualEl.disabled = !enabled;
    if (!enabled) {
      statusEl.textContent = "Turned off";
    } else if (manual) {
      statusEl.textContent = "Manual — select text, press Ctrl+E";
    } else {
      statusEl.textContent = "Automatic — start typing";
    }
  }

  // Hydrate UI from storage.
  chrome.storage.local.get(
    { enabled: true, manual: false },
    (s) => {
      enabledEl.checked = s.enabled;
      manualEl.checked = s.manual;
      render(s.enabled, s.manual);
    }
  );

  enabledEl.addEventListener("change", () => {
    chrome.storage.local.set({ enabled: enabledEl.checked });
    render(enabledEl.checked, manualEl.checked);
  });

  manualEl.addEventListener("change", () => {
    chrome.storage.local.set({ manual: manualEl.checked });
    render(enabledEl.checked, manualEl.checked);
  });
})();
