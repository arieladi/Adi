/*
 * popup.js — HEB ENG MIX FIX (v3)
 * Reads/writes the toggles from chrome.storage.local. The content script
 * listens for storage changes, so toggling here takes effect live in open tabs.
 */
(function () {
  "use strict";

  const enabledEl = document.getElementById("enabled");
  const manualEl = document.getElementById("manual");
  const onlineEl = document.getElementById("online");
  const statusEl = document.getElementById("status");

  function render(enabled, manual) {
    manualEl.disabled = !enabled;
    // Sentence suggestions only run in automatic mode.
    onlineEl.disabled = !enabled || manual;

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
    { enabled: true, manual: false, online: true },
    (s) => {
      enabledEl.checked = s.enabled;
      manualEl.checked = s.manual;
      onlineEl.checked = s.online;
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

  onlineEl.addEventListener("change", () => {
    chrome.storage.local.set({ online: onlineEl.checked });
  });
})();
