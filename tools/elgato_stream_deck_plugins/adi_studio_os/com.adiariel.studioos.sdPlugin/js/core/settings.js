'use strict';
/* =============================================================================
   settings.js — the NAMESPACED settings store, and the single writer.

   V64. This is the piece D17 was waiting for, and it is the reason four separate
   features were stuck:

     D16   the rekordbox MIDI port name has no home — REQUIRED on Windows to
           match the loopMIDI port. The Property Inspector field existed and
           nothing ever read it.
     D17   rekordbox's six encoder accumulators reset on every launch. The seam
           was built and deliberately left unwired.
     —     MIDI Control's root note, scale, channel and bank reset too.
     —     the Visualizers' slot views, and (V64) the chosen audio input device.

   WHY IT HAD TO BE A STORE AND NOT A FIELD READ. D17's exact objection was that
   the Stream Deck's global settings object is ONE object shared by every module,
   so `setGlobalSettings` from two modules is a read-modify-write race — the same
   race the legacy rekordbox plugin had to fix in 1.0.1.0 after it clobbered the
   port name. A module that writes its own key directly reintroduces it.

   So: THIS FILE IS THE ONLY WRITER. Modules never call setGlobalSettings. They
   read and write namespaced keys here, this file owns the whole object, and it
   writes the merged result on a debounce. One writer, no race, and D17's
   objection is answered rather than worked around.

   THE DEBOUNCE IS NOT DECORATION. A dial spin is one write per detent, and the
   Stream Deck app persists global settings to disk. 800 ms matches the legacy
   plugin's own figure, and it goes through SOS.Timing because NOTHING in this
   frontend may call setTimeout — page timers in this hidden WebView are
   throttled to roughly once a minute with the window closed.

   READS BEFORE THE FIRST LOAD ARE THE INTERESTING CASE. getGlobalSettings is
   asynchronous, so every module boots before its settings arrive. `onReady`
   exists for that: a module registers a restore callback, and it runs once the
   real object lands. Modules that only ever read on demand can ignore it.
   ============================================================================= */

window.SOS = window.SOS || {};

SOS.Settings = (function () {
  var SAVE_MS = 800;

  var store = {};          // the WHOLE global-settings object, as last known
  var loaded = false;      // has the app answered getGlobalSettings yet?
  var waiting = [];        // onReady callbacks, drained once
  var saveTimer = null;

  function isObj(v) { return v !== null && typeof v === 'object' && !Array.isArray(v); }

  /* The app hands us the whole object. Replace rather than merge: the app's copy
     is the truth on disk, and merging our in-memory view over it would resurrect
     a key another writer had deleted. */
  function load(obj) {
    store = isObj(obj) ? obj : {};
    var first = !loaded;
    loaded = true;
    if (first) {
      var fns = waiting; waiting = [];
      fns.forEach(function (fn) {
        try { fn(); } catch (e) { SOS.SD.log('settings: restore failed — ' + e.message); }
      });
    }
  }

  function onReady(fn) {
    if (typeof fn !== 'function') return;
    if (loaded) { try { fn(); } catch (e) { SOS.SD.log('settings: restore failed — ' + e.message); } }
    else waiting.push(fn);
  }

  // ------------------------------------------------------------------- reading
  /* A namespace is one key on the shared object holding a plain sub-object, so a
     module can never see or clobber another's keys by accident. */
  function ns(name) { return isObj(store[name]) ? store[name] : {}; }

  function get(name, key, dflt) {
    var v = ns(name)[key];
    return v === undefined ? dflt : v;
  }

  function num(name, key, dflt, lo, hi) {
    var v = parseFloat(get(name, key, dflt));
    if (!isFinite(v)) return dflt;
    if (lo != null && v < lo) return lo;
    if (hi != null && v > hi) return hi;
    return v;
  }

  function str(name, key, dflt) {
    var v = get(name, key, dflt);
    return (typeof v === 'string' && v !== '') ? v : dflt;
  }

  // ------------------------------------------------------------------- writing
  function save() {
    saveTimer = null;
    try { SOS.SD.setGlobalSettings(store); }
    catch (e) { SOS.SD.log('settings: save failed — ' + e.message); }
  }

  function queueSave() {
    if (saveTimer !== null) SOS.Timing.cancel(saveTimer);
    saveTimer = SOS.Timing.after(SAVE_MS, save);
  }

  function set(name, key, value) {
    if (!isObj(store[name])) store[name] = {};
    if (store[name][key] === value) return;      // no write, no disk churn
    store[name][key] = value;
    queueSave();
  }

  /* Merge a whole sub-object in one call — for a module that saves several
     related values together (rekordbox's six levels, midictl's config). */
  function patch(name, obj) {
    if (!isObj(obj)) return;
    if (!isObj(store[name])) store[name] = {};
    var changed = false;
    Object.keys(obj).forEach(function (k) {
      if (store[name][k] !== obj[k]) { store[name][k] = obj[k]; changed = true; }
    });
    if (changed) queueSave();
  }

  /* PI fields land at the TOP level, because pi/inspector.html has always
     written them there and its four <input> ids are the contract. Exposed
     separately so a module reading a port name cannot accidentally reach into
     another module's namespace. */
  function top(key, dflt) {
    var v = store[key];
    return v === undefined || v === '' ? dflt : v;
  }

  return {
    load: load, onReady: onReady, isLoaded: function () { return loaded; },
    get: get, num: num, str: str, set: set, patch: patch, top: top,
    // exposed for scripts/test_core.mjs
    _store: function () { return store; },
    _flush: function () { if (saveTimer !== null) { SOS.Timing.cancel(saveTimer); save(); } },
    _reset: function () { store = {}; loaded = false; waiting = []; saveTimer = null; },
    SAVE_MS: SAVE_MS,
  };
})();
