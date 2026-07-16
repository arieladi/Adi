/*
 * background.js — HEB ENG MIX FIX (MV3 service worker)
 * ----------------------------------------------------------------------------
 * Minimal. State lives in chrome.storage.local and is read/written directly by
 * the content script and popup, so the worker only needs to seed sensible
 * defaults the first time the extension is installed. It stays dormant
 * otherwise (no persistent background page — pure event-driven MV3).
 * ----------------------------------------------------------------------------
 */
chrome.runtime.onInstalled.addListener((details) => {
  if (details.reason === "install") {
    chrome.storage.local.set({ enabled: true, aggressive: false, manual: false });
  }
});
