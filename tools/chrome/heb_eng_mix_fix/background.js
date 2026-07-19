/*
 * background.js — HEB ENG MIX FIX (v7) — MV3 service worker
 * ----------------------------------------------------------------------------
 * Minimal. The layout fix is fully local: state lives in chrome.storage.local
 * and is read/written by the content script and popup, so the worker only
 * seeds defaults on first install. No network access of any kind.
 * ----------------------------------------------------------------------------
 */
chrome.runtime.onInstalled.addListener((details) => {
  if (details.reason === "install") {
    chrome.storage.local.set({ enabled: true, manual: false });
  }
});
