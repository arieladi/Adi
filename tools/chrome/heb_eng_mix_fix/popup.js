/*
 * popup.js — HEB ENG MIX FIX
 * Reads/writes the toggles from chrome.storage.local. The content script
 * listens for storage changes, so toggling here takes effect live in open tabs.
 */
(function () {
  "use strict";

  const enabledEl = document.getElementById("enabled");
  const manualEl = document.getElementById("manual");
  const aggressiveEl = document.getElementById("aggressive");
  const statusEl = document.getElementById("status");

  function render(enabled, manual) {
    manualEl.disabled = !enabled;
    // Aggressive only applies to automatic mode.
    aggressiveEl.disabled = !enabled || manual;

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
    { enabled: true, aggressive: false, manual: false },
    (s) => {
      enabledEl.checked = s.enabled;
      manualEl.checked = s.manual;
      aggressiveEl.checked = s.aggressive;
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

  aggressiveEl.addEventListener("change", () => {
    chrome.storage.local.set({ aggressive: aggressiveEl.checked });
  });
})();
