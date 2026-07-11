/* Cue time math — shared by the app (browser) and the build/test script (Node).
   Internal unit: integer TENTHS OF A SECOND (rekordbox shows m:ss.t).

   Accepted input formats (all mean 5 min 18 s):
     5:18      m:ss        (rekordbox)
     5:18.6    m:ss.t      (rekordbox, tenths)
     5.18      mm.ss       (the old Excel convention: fraction = seconds)
     1:02:37   h:mm:ss
   Play Time = Cue out − Cue in, Set time = true running total.
*/
(function (root) {
  'use strict';

  // Parse a cue string into tenths of a second. Empty/invalid -> 0.
  function parseCue(v) {
    if (v == null) return 0;
    let s = String(v).trim().replace(/,/g, '.');
    if (!s) return 0;
    let neg = false;
    if (s.startsWith('-')) { neg = true; s = s.slice(1).trim(); }
    let sec;
    if (s.includes(':')) {
      const parts = s.split(':');
      if (parts.length > 3 || parts.some(p => p.trim() === '')) return 0;
      const nums = parts.map(Number);
      if (nums.some(n => !Number.isFinite(n) || n < 0)) return 0;
      sec = parts.length === 2
        ? nums[0] * 60 + nums[1]
        : nums[0] * 3600 + nums[1] * 60 + nums[2];
    } else {
      const n = Number(s);
      if (!Number.isFinite(n) || n < 0) return 0;
      const min = Math.trunc(n);
      const frac = Math.round((n - min) * 100); // Excel mm.ss: fraction digits are seconds
      sec = min * 60 + frac;
    }
    return (neg ? -1 : 1) * Math.round(sec * 10);
  }

  // True if the value looks like a mistake: malformed input (would parse as 0)
  // or a seconds/minutes part >= 60 (e.g. 5.75 or 5:75).
  function cueWarn(v) {
    if (v == null) return false;
    let s = String(v).trim().replace(/,/g, '.');
    if (!s) return false;
    if (s.startsWith('-')) s = s.slice(1).trim();
    if (s.includes(':')) {
      const parts = s.split(':');
      if (parts.length > 3 || parts.some(p => p.trim() === '')) return true;
      const nums = parts.map(Number);
      if (nums.some(n => !Number.isFinite(n) || n < 0)) return true;
      const ss = nums[parts.length - 1];
      const mm = parts.length === 3 ? nums[1] : NaN;
      return ss >= 60 || (Number.isFinite(mm) && mm >= 60);
    }
    const n = Number(s);
    if (!Number.isFinite(n) || n < 0) return true;
    return Math.round(Math.abs(n - Math.trunc(n)) * 100) >= 60;
  }

  // Play Time display: m:ss or m:ss.t when there are tenths (rekordbox style).
  function fmtPlay(t) {
    const neg = t < 0 ? '-' : ''; t = Math.abs(Math.round(t));
    const tenth = t % 10, sTot = (t - tenth) / 10;
    const m = Math.floor(sTot / 60), s = sTot % 60;
    return `${neg}${m}:${String(s).padStart(2, '0')}${tenth ? '.' + tenth : ''}`;
  }

  // Set time display: h:mm:ss (tenths rounded to the nearest second).
  function fmtClock(t) {
    const neg = t < 0 ? '-' : '';
    const total = Math.round(Math.abs(t) / 10);
    const h = Math.floor(total / 3600), m = Math.floor((total % 3600) / 60), s = total % 60;
    return `${neg}${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
  }

  function computeRows(tracks) {
    let cum = 0;
    return tracks.map(t => {
      const play = parseCue(t.cueOut) - parseCue(t.cueIn);
      cum += play;
      return { play, cum };
    });
  }

  const api = { parseCue, cueWarn, fmtPlay, fmtClock, computeRows };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.CueMath = api;
})(typeof self !== 'undefined' ? self : this);
