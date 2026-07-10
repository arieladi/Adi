'use strict';
/* =============================================================================
   adi_visualizers_and_meters — shared analysis + drawing engine
   -----------------------------------------------------------------------------
   Pure HTML5 Canvas + native Web Audio API. Used by BOTH:
     • the browser demo (demo/index.html)            — one Renderer, full dashboard
     • the Stream Deck plugin (app.html / plugin.js)  — one Renderer per action

   It exposes a single global, `window.AVM`, so it works as a classic <script>
   (no bundler, no ES modules) in a plain browser and inside Stream Deck's
   embedded Chromium runtime.

   Design:
     • ONE AudioEngine captures stereo input via getUserMedia and runs the
       AudioWorklet "meter-processor" (true L/R RMS, peak, correlation, balance
       + raw PCM frames). It fills shared ring buffers and the shared METER.
     • Each Renderer owns its own analysis scratch + ballistics so multiple
       views (e.g. 4 Stream Deck + dials) never clobber each other's state.

   NOTE on input: getUserMedia captures the default *input* device, not system
   output. To analyze what's playing, select a loopback device (VB-Cable on
   Windows, BlackHole on macOS) as the input.
   ============================================================================ */

(function (root) {

  /* ---------------------------------------------------------------- constants */
  const RING = 1 << 17;            // 131072 samples per channel (~2.7 s @ 48 kHz)
  const RMASK = RING - 1;

  // All selectable views (the plugin lets each action pick one of these)
  const VIEWS = ['spectrum', 'scope', 'waveform', 'meters', 'bands', 'gonio', 'corr', 'bal'];

  // Default config for each view. Cloned per action instance by the consumer.
  const DEFAULTS = {
    // Spectrum defaults mirror the SPAN "Spectrum Mode Editor" reference.
    spectrum: {
      window: 'hann', blockSize: 2048, overlap: 0.598, avgTime: 1057, slope: 4.5,
      freqLo: 15.2, freqHi: 20000, rangeLo: -78, rangeHi: 0,
      filled: true, pivot: 1000, color: '#d6ff7a', fill: 0.16,
      // tap readout (SPAN-style mouse hover): tuning for the note name, snap to
      // the strongest nearby column (fat-finger aid), seconds before auto-hide.
      // markerX (0..1, transient) is injected by the consumer, never persisted.
      tuneA4: 440, snap: true, markerHold: 6,
    },
    scope: {
      channel: 'left', trigger: 'rising', threshold: 0.0, timeMs: 20, amp: 1.0,
      color: '#46e0c8', showCursors: false, cursorX: 0.5, cursorY: 0.5,
    },
    waveform: {
      channel: 'mono', windowMs: 1500, filled: true, color: '#ff8a3d', fill: 0.22,
    },
    meters: { color: '#7fe06a' },
    bands: {},
    gonio: { color: '#38f0a0' },
    corr: {},
    bal: {},
  };

  /* --------------------------------------------- shared (single-capture) state */
  let SR = 48000;                                   // live sample rate
  let analyserL = null, analyserR = null, dataL = null, dataR = null;

  // Live meter values pushed from the worklet (shared by all renderers)
  const METER = { rmsL: 0, rmsR: 0, peakL: 0, peakR: 0, corr: 0, bal: 0 };

  /* ----------------------------------------------------------- ring buffers */
  const ringL = new Float32Array(RING);
  const ringR = new Float32Array(RING);
  const ringM = new Float32Array(RING);
  let ringW = 0;

  function ringPush(cl, cr, cm) {
    const n = cl.length;
    for (let i = 0; i < n; i++) {
      const w = (ringW + i) & RMASK;
      ringL[w] = cl[i]; ringR[w] = cr[i]; ringM[w] = cm[i];
    }
    ringW = (ringW + n) & RMASK;
  }
  // Copy the most recent `count` samples (oldest -> newest) into `out`.
  function ringRead(src, out, count) {
    const start = (ringW - count) & RMASK;
    for (let i = 0; i < count; i++) out[i] = src[(start + i) & RMASK];
  }

  /* ----------------------------------------------------------------- helpers */
  const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
  const lin2db = (x) => 20 * Math.log10(x + 1e-12);

  function hexA(hex, a) {
    const h = hex.replace('#', '');
    const r = parseInt(h.substring(0, 2), 16);
    const g = parseInt(h.substring(2, 4), 16);
    const b = parseInt(h.substring(4, 6), 16);
    return 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
  }

  // Analysis windows (returns Float32Array of length n)
  function makeWindow(type, n) {
    const w = new Float32Array(n);
    const PI2 = 2 * Math.PI, PI4 = 4 * Math.PI, PI6 = 6 * Math.PI, PI8 = 8 * Math.PI;
    for (let i = 0; i < n; i++) {
      const x = i / (n - 1);
      let v = 1;
      switch (type) {
        case 'hann':            v = 0.5 - 0.5 * Math.cos(PI2 * x); break;
        case 'hamming':         v = 0.54 - 0.46 * Math.cos(PI2 * x); break;
        case 'blackman':        v = 0.42 - 0.5 * Math.cos(PI2 * x) + 0.08 * Math.cos(PI4 * x); break;
        case 'blackman-harris': v = 0.35875 - 0.48829 * Math.cos(PI2 * x) + 0.14128 * Math.cos(PI4 * x) - 0.01168 * Math.cos(PI6 * x); break;
        case 'flattop':         v = 0.21557895 - 0.41663158 * Math.cos(PI2 * x) + 0.277263158 * Math.cos(PI4 * x) - 0.083578947 * Math.cos(PI6 * x) + 0.006947368 * Math.cos(PI8 * x); break;
        case 'rect': default:   v = 1; break;
      }
      w[i] = v;
    }
    return w;
  }

  /* ---------------------------------------------------------------- FFT class
     Iterative radix-2 Cooley-Tukey with precomputed bit-reversal + twiddles.
     Reuses its own re/im scratch arrays (no per-call allocation). n must be 2^k. */
  class FFT {
    constructor(n) {
      this.n = n;
      const bits = Math.round(Math.log2(n));
      this.rev = new Uint32Array(n);
      for (let i = 0; i < n; i++) {
        let x = i, r = 0;
        for (let j = 0; j < bits; j++) { r = (r << 1) | (x & 1); x >>= 1; }
        this.rev[i] = r;
      }
      this.cos = new Float32Array(n >> 1);
      this.sin = new Float32Array(n >> 1);
      for (let i = 0; i < (n >> 1); i++) {
        const t = -2 * Math.PI * i / n;
        this.cos[i] = Math.cos(t);
        this.sin[i] = Math.sin(t);
      }
      this.re = new Float32Array(n);
      this.im = new Float32Array(n);
    }
    // Forward FFT on real-valued `input` (already windowed). Result in this.re/this.im.
    forward(input) {
      const n = this.n, re = this.re, im = this.im, rev = this.rev, C = this.cos, S = this.sin;
      for (let i = 0; i < n; i++) { re[i] = input[rev[i]]; im[i] = 0; }
      for (let size = 2; size <= n; size <<= 1) {
        const half = size >> 1;
        const step = n / size;
        for (let i = 0; i < n; i += size) {
          let k = 0;
          for (let j = i; j < i + half; j++) {
            const c = C[k], s = S[k];
            const tre = re[j + half] * c - im[j + half] * s;
            const tim = re[j + half] * s + im[j + half] * c;
            re[j + half] = re[j] - tre; im[j + half] = im[j] - tim;
            re[j] += tre; im[j] += tim;
            k += step;
          }
        }
      }
    }
  }

  /* ------------------------------------------------- spectrum drawing helpers */
  const NICE_FREQS = [10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
  const LABEL_FREQS = new Set([100, 1000, 10000]);
  function fmtHz(f) { return f >= 1000 ? (f / 1000) + 'k' : '' + f; }

  // Nearest equal-tempered note for a frequency (a4 = tuning reference in Hz).
  const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  function noteFor(freq, a4) {
    a4 = a4 || 440;
    if (!(freq > 0)) return null;
    const m = 69 + 12 * Math.log2(freq / a4);
    const nearest = Math.round(m);
    const cents = Math.round((m - nearest) * 100);
    const name = NOTE_NAMES[((nearest % 12) + 12) % 12];
    const octave = Math.floor(nearest / 12) - 1;
    return { midi: nearest, name: name, octave: octave, cents: cents, label: name + octave };
  }
  // Compact frequency for the tap readout ("62.4Hz", "110.0Hz", "742Hz", "2.45kHz").
  function fmtFreq(f) {
    if (f < 100) return f.toFixed(1) + 'Hz';
    if (f < 1000) return Math.round(f) + 'Hz';
    return (f / 1000).toFixed(f < 10000 ? 2 : 1) + 'kHz';
  }

  const DB_TOP = 6, DB_BOT = -60;                   // peak/RMS meter scale
  const BANDS = [31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
  const BAND_LABELS = { 31.5: '31', 125: '125', 500: '500', 2000: '2k', 8000: '8k' };

  /* ===========================================================================
     Renderer — owns all per-instance analysis scratch + display ballistics.
     One per browser canvas / per Stream Deck action instance.
     =========================================================================== */
  class Renderer {
    constructor() {
      // spectrum runtime
      this.fft = null; this.input = null; this.power = null;
      this.win = null; this.winType = null; this.winSum = 1;
      this.col = null; this.cols = 0; this.binLo = null; this.binHi = null;
      this.mapSig = ''; this.fmin = 20; this.fmax = 20000; this.lr = 1;
      // reusable scratch
      this.read = new Float32Array(1 << 18);
      this.scopeBuf = new Float32Array(1 << 18);
      this.waveBuf = new Float32Array(RING);
      this.gL = new Float32Array(4096); this.gR = new Float32Array(4096);
      this.wmin = new Float32Array(2048); this.wmax = new Float32Array(2048);
      // meter ballistics (peak-hold)
      this.hold = { rmsL: -120, rmsR: -120, pkL: -120, pkR: -120, holdL: -120, holdR: -120, holdTL: 0, holdTR: 0 };
      this.gonioInit = false;
    }

    /* ------------------------------------------------ spectrum: setup helpers */
    ensureFFT(C) {
      const n = C.blockSize;
      if (!this.fft || this.fft.n !== n) {
        this.fft = new FFT(n);
        this.input = new Float32Array(n);
        this.power = new Float32Array((n >> 1) + 1);
      }
      this.ensureWindow(C);
    }
    ensureWindow(C) {
      const n = C.blockSize, type = C.window;
      if (!this.win || this.win.length !== n || this.winType !== type) {
        this.win = makeWindow(type, n);
        this.winType = type;
        let s = 0; for (let i = 0; i < n; i++) s += this.win[i];
        this.winSum = s || 1;
      }
    }
    // Recompute the log-frequency -> bin column mapping when geometry/params change.
    ensureMap(w, C) {
      const n = C.blockSize;
      const sig = w + '|' + n + '|' + C.freqLo + '|' + C.freqHi + '|' + SR;
      if (this.mapSig === sig && this.col && this.cols === w) return;
      const fmin = Math.max(C.freqLo, SR / n);
      const fmax = Math.min(C.freqHi, SR / 2);
      const lr = Math.log(fmax / fmin);
      const binLo = new Int32Array(w), binHi = new Int32Array(w);
      const half = n >> 1;
      for (let x = 0; x < w; x++) {
        const f0 = fmin * Math.exp(lr * (x / w));
        const f1 = fmin * Math.exp(lr * ((x + 1) / w));
        let lo = Math.ceil(f0 * n / SR);
        let hi = Math.floor(f1 * n / SR);
        lo = clamp(lo, 1, half); hi = clamp(hi, 1, half);
        if (hi < lo) { const nb = clamp(Math.round((0.5 * (f0 + f1)) * n / SR), 1, half); lo = nb; hi = nb; }
        binLo[x] = lo; binHi[x] = hi;
      }
      this.binLo = binLo; this.binHi = binHi; this.cols = w;
      this.fmin = fmin; this.fmax = fmax; this.lr = lr;
      const col = new Float32Array(w); col.fill(C.rangeLo);
      this.col = col;
      this.mapSig = sig;
    }

    /* ----------------------------------------------- spectrum: compute + draw */
    computeSpectrum(w, C, dt) {
      const n = C.blockSize;
      this.ensureFFT(C);
      this.ensureMap(w, C);

      const ov = clamp(C.overlap, 0, 0.95);
      const K = clamp(Math.round(1 / (1 - ov)), 1, 4);     // averaging passes
      const hop = Math.max(1, Math.floor(n * (1 - ov)));
      const need = n + (K - 1) * hop;

      ringRead(ringM, this.read, need);

      const power = this.power, win = this.win, input = this.input, fft = this.fft;
      power.fill(0);
      for (let p = 0; p < K; p++) {
        const start = (K - 1 - p) * hop;                   // [0 .. need-n]
        for (let i = 0; i < n; i++) input[i] = this.read[start + i] * win[i];
        fft.forward(input);
        const re = fft.re, im = fft.im;
        for (let k = 0; k <= (n >> 1); k++) power[k] += re[k] * re[k] + im[k] * im[k];
      }

      const invK = 1 / K;
      const norm = 2 / this.winSum;                        // full-scale sine -> ~0 dBFS
      const pivot = C.pivot || 1000;
      const tau = Math.max(0.001, C.avgTime / 1000);
      const a = 1 - Math.exp(-dt / tau);                   // temporal EMA coefficient

      const col = this.col, binLo = this.binLo, binHi = this.binHi;
      const fbin = SR / n;
      for (let x = 0; x < w; x++) {
        const lo = binLo[x], hi = binHi[x];
        let mx = 0;
        for (let k = lo; k <= hi; k++) if (power[k] > mx) mx = power[k];  // peak-pick
        const amp = norm * Math.sqrt(mx * invK);
        let db = 20 * Math.log10(amp + 1e-12);
        const fc = (0.5 * (lo + hi)) * fbin;
        db += C.slope * Math.log2(Math.max(fc, 1) / pivot);              // pink-noise tilt
        col[x] += a * (db - col[x]);
      }
    }
    drawSpectrum(ctx, w, h, C, dt) {
      this.computeSpectrum(w, C, dt);
      ctx.clearRect(0, 0, w, h);

      const top = C.rangeHi, bot = C.rangeLo, span = (top - bot) || 1;
      const yOf = (db) => h - clamp((db - bot) / span, 0, 1) * h;
      const xOf = (f) => w * Math.log(f / this.fmin) / this.lr;

      // grid
      ctx.lineWidth = 1;
      ctx.font = '7px "SF Mono", monospace';
      ctx.textBaseline = 'alphabetic';
      for (let i = 0; i < NICE_FREQS.length; i++) {
        const f = NICE_FREQS[i];
        if (f < this.fmin || f > this.fmax) continue;
        const x = Math.round(xOf(f)) + 0.5;
        ctx.strokeStyle = LABEL_FREQS.has(f) ? 'rgba(255,255,255,0.10)' : 'rgba(255,255,255,0.045)';
        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
        if (LABEL_FREQS.has(f)) {
          ctx.fillStyle = 'rgba(150,160,170,0.6)';
          ctx.fillText(fmtHz(f), x + 2, h - 3);
        }
      }
      for (let db = Math.ceil(top / 12) * 12; db >= bot; db -= 12) {
        const y = Math.round(yOf(db)) + 0.5;
        ctx.strokeStyle = db === 0 ? 'rgba(255,255,255,0.14)' : 'rgba(255,255,255,0.05)';
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
      }

      const col = this.col;
      if (!col) return;
      ctx.beginPath();
      ctx.moveTo(0, yOf(col[0]));
      for (let x = 1; x < w; x++) ctx.lineTo(x, yOf(col[x]));

      if (C.filled) {
        ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.closePath();
        const g = ctx.createLinearGradient(0, 0, 0, h);
        g.addColorStop(0, hexA(C.color, Math.min(0.9, C.fill + 0.5)));
        g.addColorStop(1, hexA(C.color, C.fill * 0.25));
        ctx.fillStyle = g;
        ctx.fill();
        ctx.beginPath();
        ctx.moveTo(0, yOf(col[0]));
        for (let x = 1; x < w; x++) ctx.lineTo(x, yOf(col[x]));
      }
      ctx.strokeStyle = C.color;
      ctx.lineWidth = 1.2;
      ctx.stroke();

      if (C.markerX != null) this.drawSpectrumMarker(ctx, w, h, C);
    }

    /* ------------------------------------- spectrum: tap readout (SPAN-style)
       Resolve the transient marker (C.markerX 0..1) to a column / frequency /
       displayed dB, optionally snapping to the strongest column nearby so a
       fingertip on the 200px touch slot lands on the actual peak. */
    spectrumReadout(w, C) {
      if (C.markerX == null || !this.col || this.cols !== w) return null;
      let x = Math.round(clamp(C.markerX, 0, 1) * (w - 1));
      if (C.snap !== false) {
        const R = Math.max(2, Math.round(w * 0.04));   // ~±8px on a 200px slot
        let best = x;
        for (let i = Math.max(0, x - R); i <= Math.min(w - 1, x + R); i++) {
          if (this.col[i] > this.col[best]) best = i;
        }
        x = best;
      }
      let f = this.fmin * Math.exp(this.lr * ((x + 0.5) / w));
      // Refine with the last FFT power spectrum when available: the 200px
      // column grid alone is ~60 cents wide in the bass — too coarse for a
      // note readout. Find the true peak bin (climbing out of the column if
      // the peak straddles its edge) + log-domain parabolic interpolation,
      // which is exact for a gaussian-ish windowed peak (sub-Hz at 110 Hz).
      const p = this.power;
      const n = C.blockSize;
      if (p && this.binLo && this.cols === w) {
        const half = n >> 1;
        let k = this.binLo[x];
        for (let i = this.binLo[x]; i <= this.binHi[x]; i++) if (p[i] > p[k]) k = i;
        while (k + 1 <= half && p[k + 1] > p[k]) k++;
        while (k - 1 >= 1 && p[k - 1] > p[k]) k--;
        if (p[k] > 0) {
          let kk = k;
          if (k >= 1 && k + 1 <= half && p[k - 1] > 0 && p[k + 1] > 0) {
            const y0 = Math.log(p[k - 1]), y1 = Math.log(p[k]), y2 = Math.log(p[k + 1]);
            const den = y0 - 2 * y1 + y2;
            const d = den !== 0 ? 0.5 * (y0 - y2) / den : 0;
            if (d > -1 && d < 1) kk = k + d;
          }
          const fr = kk * SR / n;
          if (fr > 0) f = fr;
        }
      }
      return { x: x, f: f, db: this.col[x], note: noteFor(f, C.tuneA4 || 440) };
    }
    drawSpectrumMarker(ctx, w, h, C) {
      const r = this.spectrumReadout(w, C);
      if (!r) return;
      const top = C.rangeHi, bot = C.rangeLo, span = (top - bot) || 1;
      const y = h - clamp((r.db - bot) / span, 0, 1) * h;
      const x = r.x + 0.5;

      ctx.setLineDash([3, 3]);
      ctx.strokeStyle = hexA(C.color, 0.75);
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
      ctx.setLineDash([]);

      // dot on the curve: colored halo + white core, sized to read on the strip
      ctx.fillStyle = hexA(C.color, 0.45);
      ctx.beginPath(); ctx.arc(x, y, 4, 0, 2 * Math.PI); ctx.fill();
      ctx.fillStyle = '#ffffff';
      ctx.beginPath(); ctx.arc(x, y, 1.8, 0, 2 * Math.PI); ctx.fill();

      // header readout — font scales with the surface so it stays legible on
      // the 100px-tall touch slot AND on 144px keys (SPAN's header line, shrunk)
      const n = r.note;
      const cents = n ? (n.cents === 0 ? '±0' : (n.cents > 0 ? '+' : '') + n.cents) : '';
      const txt = fmtFreq(r.f) + '  ' + (n ? n.label + ' ' + cents + '¢' : '') + '  ' + r.db.toFixed(1) + 'dB';
      const fs = Math.max(8, Math.round(h * 0.09));
      ctx.font = '600 ' + fs + 'px "SF Mono", monospace';
      const pad = 3;
      const tw = ctx.measureText(txt).width;
      const bh = fs + 2 * pad;
      ctx.fillStyle = 'rgba(6,8,10,0.78)';
      ctx.fillRect(0, 0, Math.min(w, tw + 2 * pad + 2), bh);
      ctx.textBaseline = 'middle';
      ctx.fillStyle = '#ffffff';
      ctx.fillText(txt, pad, bh / 2 + 0.5);
      ctx.textBaseline = 'alphabetic';
    }

    /* ----------------------------------------------------------- oscilloscope */
    drawScope(ctx, w, h, S) {
      ctx.clearRect(0, 0, w, h);
      const src = S.channel === 'left' ? ringL : S.channel === 'right' ? ringR : ringM;
      const N = Math.max(64, Math.min(RING - 4096, Math.round(S.timeMs / 1000 * SR)));
      const guard = Math.min(N, 4096);
      const total = N + guard;
      const buf = this.scopeBuf;
      ringRead(src, buf, total);

      // trigger: locate a crossing inside the guard region; else free-run on newest N
      let t0 = guard;
      if (S.trigger !== 'free') {
        const th = S.threshold;
        for (let i = 1; i < guard; i++) {
          const a = buf[i - 1], b = buf[i];
          if (S.trigger === 'rising' && a < th && b >= th) { t0 = i; break; }
          if (S.trigger === 'falling' && a > th && b <= th) { t0 = i; break; }
        }
      }

      const mid = h * 0.5, halfH = h * 0.45, amp = S.amp;
      ctx.strokeStyle = 'rgba(255,255,255,0.10)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(0, mid + 0.5); ctx.lineTo(w, mid + 0.5); ctx.stroke();

      ctx.beginPath();
      for (let px = 0; px < w; px++) {
        const idx = t0 + ((px / w) * N | 0);
        const y = mid - buf[idx] * amp * halfH;
        if (px === 0) ctx.moveTo(0, y); else ctx.lineTo(px, y);
      }
      ctx.strokeStyle = S.color; ctx.lineWidth = 1.2; ctx.stroke();

      if (S.showCursors) {
        const cx = clamp(S.cursorX, 0, 1) * w;
        const cy = clamp(S.cursorY, 0, 1) * h;
        ctx.setLineDash([3, 3]);
        ctx.strokeStyle = hexA(S.color, 0.85); ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(cx + 0.5, 0); ctx.lineTo(cx + 0.5, h); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(0, cy + 0.5); ctx.lineTo(w, cy + 0.5); ctx.stroke();
        ctx.setLineDash([]);

        const tMs = S.cursorX * S.timeMs;
        const samples = Math.round(tMs / 1000 * SR);
        const hz = tMs > 0 ? 1000 / tMs : 0;
        const lin = (mid - cy) / (amp * halfH);
        const db = lin2db(Math.abs(lin));

        ctx.font = '7px "SF Mono", monospace';
        ctx.fillStyle = hexA(S.color, 0.95);
        ctx.textBaseline = 'top';
        const tx = cx < w - 84 ? cx + 4 : cx - 80;
        ctx.fillText(tMs.toFixed(2) + ' ms  ' + samples + ' smp', tx, 3);
        ctx.fillText(hz.toFixed(1) + ' Hz', tx, 12);
        ctx.fillText(lin.toFixed(3) + '  ' + db.toFixed(1) + ' dB', tx, 21);
      }
    }

    /* --------------------------------------------------------------- waveform */
    drawWaveform(ctx, w, h, W) {
      ctx.clearRect(0, 0, w, h);
      const src = W.channel === 'left' ? ringL : W.channel === 'right' ? ringR : ringM;
      let N = Math.max(64, Math.round(W.windowMs / 1000 * SR));
      N = Math.min(N, RING);
      const buf = this.waveBuf;
      ringRead(src, buf, N);

      const mid = h * 0.5, halfH = h * 0.46;
      if (this.wmin.length < w) { this.wmin = new Float32Array(w); this.wmax = new Float32Array(w); }
      const mn = this.wmin, mx = this.wmax;
      const per = N / w;
      for (let px = 0; px < w; px++) {
        let s = (px * per) | 0, e = ((px + 1) * per) | 0; if (e <= s) e = s + 1;
        let lo = Infinity, hi = -Infinity;
        for (let i = s; i < e; i++) { const v = buf[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
        mn[px] = lo; mx[px] = hi;
      }

      ctx.strokeStyle = 'rgba(255,255,255,0.08)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(0, mid + 0.5); ctx.lineTo(w, mid + 0.5); ctx.stroke();

      ctx.beginPath();
      ctx.moveTo(0, mid - mx[0] * halfH);
      for (let px = 1; px < w; px++) ctx.lineTo(px, mid - mx[px] * halfH);
      for (let px = w - 1; px >= 0; px--) ctx.lineTo(px, mid - mn[px] * halfH);
      ctx.closePath();
      if (W.filled) { ctx.fillStyle = hexA(W.color, W.fill); ctx.fill(); }
      ctx.strokeStyle = W.color; ctx.lineWidth = 1; ctx.stroke();
    }

    /* ------------------------------------------------------ peak / RMS meters */
    updateMeters(dt) {
      const H = this.hold;
      H.rmsL = lin2db(METER.rmsL);
      H.rmsR = lin2db(METER.rmsR);
      H.pkL = lin2db(METER.peakL);
      H.pkR = lin2db(METER.peakR);
      // peak-hold: jump up instantly, hold 1.5 s, then fall ~18 dB/s
      if (H.pkL >= H.holdL) { H.holdL = H.pkL; H.holdTL = 0; }
      else { H.holdTL += dt; if (H.holdTL > 1.5) H.holdL -= 18 * dt; }
      if (H.pkR >= H.holdR) { H.holdR = H.pkR; H.holdTR = 0; }
      else { H.holdTR += dt; if (H.holdTR > 1.5) H.holdR -= 18 * dt; }
    }
    drawMeters(ctx, w, h, dt) {
      this.updateMeters(dt);
      const H = this.hold;
      ctx.clearRect(0, 0, w, h);
      const pad = 3, scaleW = 15;
      const yOf = (db) => h - pad - clamp((db - DB_BOT) / (DB_TOP - DB_BOT), 0, 1) * (h - 2 * pad);

      ctx.font = '6px "SF Mono", monospace';
      ctx.textBaseline = 'middle';
      ctx.fillStyle = 'rgba(150,160,170,0.55)';
      ctx.strokeStyle = 'rgba(255,255,255,0.05)';
      for (const db of [0, -12, -24, -36, -48]) {
        const y = Math.round(yOf(db)) + 0.5;
        ctx.beginPath(); ctx.moveTo(scaleW, y); ctx.lineTo(w, y); ctx.stroke();
        ctx.fillText(db === 0 ? '0' : '' + db, 0, y);
      }

      const gap = 4;
      const barW = (w - scaleW - gap) / 2 - gap;
      const x0 = scaleW + gap;
      this._bar(ctx, x0, pad, barW, h - 2 * pad, H.rmsL, H.pkL, H.holdL, yOf, 'L');
      this._bar(ctx, x0 + barW + gap, pad, barW, h - 2 * pad, H.rmsR, H.pkR, H.holdR, yOf, 'R');
    }
    _bar(ctx, x, y, bw, bh, rmsDb, pkDb, holdDb, yOf, label) {
      ctx.fillStyle = 'rgba(255,255,255,0.04)';
      ctx.fillRect(x, y, bw, bh);
      const top = yOf(rmsDb);
      const g = ctx.createLinearGradient(0, y + bh, 0, y);
      g.addColorStop(0, '#2fae5e');
      g.addColorStop(0.7, '#7fe06a');
      g.addColorStop(0.9, '#ffd166');
      g.addColorStop(1, '#ff5d5d');
      ctx.fillStyle = g;
      ctx.fillRect(x, top, bw, (y + bh) - top);
      const yp = yOf(pkDb);
      ctx.fillStyle = 'rgba(255,255,255,0.55)';
      ctx.fillRect(x, yp - 0.5, bw, 1);
      const yh = yOf(holdDb);
      ctx.fillStyle = holdDb > -0.1 ? '#ff5d5d' : '#e8eef3';
      ctx.fillRect(x, yh - 1, bw, 1.5);
      ctx.fillStyle = 'rgba(150,160,170,0.7)';
      ctx.font = '6px "SF Mono", monospace';
      ctx.textBaseline = 'bottom';
      ctx.fillText(label, x + bw / 2 - 2, y + bh - 0.5);
    }

    /* ---------------------------------------------------- correlation / balance */
    drawCorr(ctx, w, h) {
      ctx.clearRect(0, 0, w, h);
      const pad = 2, mid = h / 2, usable = w - 2 * pad;
      const g = ctx.createLinearGradient(pad, 0, w - pad, 0);
      g.addColorStop(0, '#ff5d5d'); g.addColorStop(0.5, '#ffd166'); g.addColorStop(1, '#4ad27a');
      ctx.fillStyle = g;
      ctx.globalAlpha = 0.25; ctx.fillRect(pad, mid - 3, usable, 6); ctx.globalAlpha = 1;
      ctx.strokeStyle = 'rgba(255,255,255,0.18)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(w / 2 + 0.5, mid - 4); ctx.lineTo(w / 2 + 0.5, mid + 4); ctx.stroke();
      const v = clamp(METER.corr, -1, 1);
      const mx = pad + (v * 0.5 + 0.5) * usable;
      ctx.fillStyle = v < 0 ? '#ff7b7b' : '#7fe0a2';
      ctx.fillRect(mx - 1, mid - 5, 2, 10);
      ctx.font = '6px "SF Mono", monospace'; ctx.fillStyle = 'rgba(150,160,170,0.7)';
      ctx.textBaseline = 'middle';
      ctx.fillText('-1', pad, mid); ctx.fillText('+1', w - pad - 8, mid);
    }
    drawBal(ctx, w, h) {
      ctx.clearRect(0, 0, w, h);
      const pad = 2, mid = h / 2, usable = w - 2 * pad;
      ctx.fillStyle = 'rgba(255,255,255,0.05)';
      ctx.fillRect(pad, mid - 3, usable, 6);
      ctx.strokeStyle = 'rgba(255,255,255,0.18)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(w / 2 + 0.5, mid - 4); ctx.lineTo(w / 2 + 0.5, mid + 4); ctx.stroke();
      const v = clamp(METER.bal, -1, 1);            // + = right-heavy
      const mx = pad + (v * 0.5 + 0.5) * usable;
      ctx.fillStyle = '#6fe3c4';
      ctx.fillRect(mx - 1, mid - 5, 2, 10);
      ctx.font = '6px "SF Mono", monospace'; ctx.fillStyle = 'rgba(150,160,170,0.7)';
      ctx.textBaseline = 'middle';
      ctx.fillText('L', pad, mid); ctx.fillText('R', w - pad - 5, mid);
    }

    /* ----------------------------------------------------------- goniometer
       Vectorscope with phosphor persistence (mid/side rotated so mono = vertical). */
    drawGonio(ctx, w, h, G, resized) {
      if (resized || !this.gonioInit) { ctx.fillStyle = '#0a0d10'; ctx.fillRect(0, 0, w, h); this.gonioInit = true; }
      ctx.fillStyle = hexA('#0a0d10', 0.30);
      ctx.fillRect(0, 0, w, h);

      const cx = w / 2, cy = h / 2, r = Math.min(w, h) * 0.46;
      ctx.strokeStyle = 'rgba(255,255,255,0.06)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(cx, 2); ctx.lineTo(cx, h - 2);
      ctx.moveTo(2, cy); ctx.lineTo(w - 2, cy); ctx.stroke();

      const N = Math.min(2048, this.gL.length);
      ringRead(ringL, this.gL, N);
      ringRead(ringR, this.gR, N);

      const k = 0.70710678;
      const p = new Path2D();
      for (let i = 0; i < N; i++) {
        const l = this.gL[i], rr = this.gR[i];
        const mid = (l + rr) * k;     // vertical axis
        const side = (l - rr) * k;    // horizontal axis
        const px = cx + side * r;
        const py = cy - mid * r;
        p.rect(px, py, 1.1, 1.1);
      }
      ctx.fillStyle = hexA((G && G.color) || '#38f0a0', 0.7);
      ctx.fill(p);
    }

    /* -------------------------------------------------------- octave bands
       Reads the shared AnalyserNodes (created by the AudioEngine). */
    drawBands(ctx, w, h) {
      ctx.clearRect(0, 0, w, h);
      if (!analyserL) return;
      analyserL.getFloatFrequencyData(dataL);
      analyserR.getFloatFrequencyData(dataR);
      const NF = analyserL.frequencyBinCount;
      const binHz = SR / analyserL.fftSize;

      const top = 0, bot = -60, span = top - bot;
      const padB = 9;
      const usableH = h - padB - 2;
      const yOf = (db) => 2 + usableH - clamp((db - bot) / span, 0, 1) * usableH;

      const n = BANDS.length;
      const slot = w / n;
      const subW = Math.max(1.5, slot * 0.34);

      for (let i = 0; i < n; i++) {
        const f = BANDS[i];
        const cx = i * slot + slot / 2;
        const lDb = this._bandLevel(dataL, NF, binHz, f);
        const rDb = this._bandLevel(dataR, NF, binHz, f);
        this._bandBar(ctx, cx - subW - 1, subW, lDb, yOf, usableH);
        this._bandBar(ctx, cx + 1, subW, rDb, yOf, usableH);
        if (BAND_LABELS[f]) {
          ctx.font = '6px "SF Mono", monospace';
          ctx.fillStyle = 'rgba(150,160,170,0.55)';
          ctx.textBaseline = 'bottom';
          const t = BAND_LABELS[f];
          ctx.fillText(t, cx - t.length * 1.8, h - 1);
        }
      }
    }
    _bandLevel(data, NF, binHz, f) {
      const lo = clamp(Math.floor(f / Math.SQRT2 / binHz), 0, NF - 1);
      const hi = clamp(Math.ceil(f * Math.SQRT2 / binHz), 0, NF - 1);
      let mx = -200;
      for (let k = lo; k <= hi; k++) if (data[k] > mx) mx = data[k];
      return mx;
    }
    _bandBar(ctx, x, bw, db, yOf, usableH) {
      const baseY = 2 + usableH;
      ctx.fillStyle = 'rgba(255,255,255,0.04)';
      ctx.fillRect(x, 2, bw, usableH);
      const y = yOf(db);
      const g = ctx.createLinearGradient(0, baseY, 0, 2);
      g.addColorStop(0, '#2fae5e');
      g.addColorStop(0.75, '#7fe06a');
      g.addColorStop(1, '#ffd166');
      ctx.fillStyle = g;
      ctx.fillRect(x, y, bw, baseY - y);
    }

    /* ------------------------------------------------------------- dispatch */
    // Draw any view. `cfg` is the per-view config object; `dt` seconds since last
    // frame; `resized` true when the backing surface changed (resets gonio trail).
    draw(view, ctx, w, h, cfg, dt, resized) {
      switch (view) {
        case 'spectrum': return this.drawSpectrum(ctx, w, h, cfg, dt);
        case 'scope':    return this.drawScope(ctx, w, h, cfg);
        case 'waveform': return this.drawWaveform(ctx, w, h, cfg);
        case 'meters':   return this.drawMeters(ctx, w, h, dt);
        case 'bands':    return this.drawBands(ctx, w, h);
        case 'gonio':    return this.drawGonio(ctx, w, h, cfg, resized);
        case 'corr':     return this.drawCorr(ctx, w, h);
        case 'bal':      return this.drawBal(ctx, w, h);
        default:         return this.drawSpectrum(ctx, w, h, cfg, dt);
      }
    }
  }

  /* ----------------------------------------------------------- AudioWorklet src
     Inlined as a Blob so addModule() works from file:// and inside the Stream
     Deck CEF runtime (a separate worklet .js can fail CORS there). */
  const WORKLET_SRC = `
class MeterProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.tau = 0.3;
    this.rmsL = 0; this.rmsR = 0;
    this.ell = 1e-12; this.err = 1e-12; this.elr = 0;
    this.pkL = 0; this.pkR = 0;
    this.CH = 1024;
    this.bl = new Float32Array(this.CH);
    this.br = new Float32Array(this.CH);
    this.bm = new Float32Array(this.CH);
    this.w = 0;
  }
  process(inputs) {
    const inp = inputs[0];
    if (!inp || inp.length === 0) return true;
    const L = inp[0];
    const R = inp.length > 1 ? inp[1] : inp[0];
    const n = L.length;
    if (!n) return true;
    const dt = n / sampleRate;
    const a = 1 - Math.exp(-dt / this.tau);

    let msL = 0, msR = 0, ell = 0, err = 0, elr = 0;
    for (let i = 0; i < n; i++) {
      const xl = L[i], xr = R[i];
      msL += xl * xl; msR += xr * xr;
      ell += xl * xl; err += xr * xr; elr += xl * xr;
      const al = xl < 0 ? -xl : xl, ar = xr < 0 ? -xr : xr;
      if (al > this.pkL) this.pkL = al;
      if (ar > this.pkR) this.pkR = ar;

      this.bl[this.w] = xl; this.br[this.w] = xr; this.bm[this.w] = 0.5 * (xl + xr);
      if (++this.w >= this.CH) {
        const cl = this.bl, cr = this.br, cm = this.bm;
        this.bl = new Float32Array(this.CH);
        this.br = new Float32Array(this.CH);
        this.bm = new Float32Array(this.CH);
        this.w = 0;
        const denom = Math.sqrt(this.ell * this.err) || 1e-12;
        let corr = this.elr / denom; if (corr > 1) corr = 1; else if (corr < -1) corr = -1;
        const rL = Math.sqrt(this.rmsL), rR = Math.sqrt(this.rmsR);
        const bal = (rR - rL) / ((rR + rL) || 1e-12);
        this.port.postMessage(
          { pcmL: cl, pcmR: cr, pcmM: cm, rmsL: rL, rmsR: rR, peakL: this.pkL, peakR: this.pkR, corr: corr, bal: bal },
          [cl.buffer, cr.buffer, cm.buffer]
        );
        this.pkL = 0; this.pkR = 0;
      }
    }
    this.rmsL += a * (msL / n - this.rmsL);
    this.rmsR += a * (msR / n - this.rmsR);
    this.ell  += a * (ell / n - this.ell);
    this.err  += a * (err / n - this.err);
    this.elr  += a * (elr / n - this.elr);
    return true;
  }
}
registerProcessor('meter-processor', MeterProcessor);
`;

  /* ===========================================================================
     AudioEngine — one shared stereo capture feeding the ring buffers + METER.
     =========================================================================== */
  class AudioEngine {
    constructor() {
      this.ac = null; this.stream = null; this.running = false;
      this.srcNode = null; this.worklet = null;
      this.onError = null;                 // optional callback(err)
    }
    async start(opts) {
      if (this.running) return;
      opts = opts || {};
      const audio = {
        echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 2,
      };
      if (opts.deviceId) audio.deviceId = { exact: opts.deviceId };

      const stream = await navigator.mediaDevices.getUserMedia({ audio, video: false });
      this.stream = stream;

      const ac = new (window.AudioContext || window.webkitAudioContext)();
      await ac.resume();
      this.ac = ac;
      SR = ac.sampleRate;                  // never hardcode 48k

      const blob = new Blob([WORKLET_SRC], { type: 'application/javascript' });
      const url = URL.createObjectURL(blob);
      await ac.audioWorklet.addModule(url);
      URL.revokeObjectURL(url);

      const srcNode = ac.createMediaStreamSource(stream);
      const splitter = ac.createChannelSplitter(2);
      analyserL = ac.createAnalyser(); analyserR = ac.createAnalyser();
      analyserL.fftSize = 4096; analyserR.fftSize = 4096;
      analyserL.smoothingTimeConstant = 0.5; analyserR.smoothingTimeConstant = 0.5;
      dataL = new Float32Array(analyserL.frequencyBinCount);
      dataR = new Float32Array(analyserR.frequencyBinCount);

      const worklet = new AudioWorkletNode(ac, 'meter-processor');
      worklet.port.onmessage = (e) => {
        const d = e.data;
        ringPush(d.pcmL, d.pcmR, d.pcmM);
        METER.rmsL = d.rmsL; METER.rmsR = d.rmsR;
        METER.peakL = d.peakL; METER.peakR = d.peakR;
        METER.corr = d.corr; METER.bal = d.bal;
      };

      // Silent sink keeps every node "pulled" without feeding audio back out.
      const sink = ac.createGain(); sink.gain.value = 0;
      srcNode.connect(worklet); worklet.connect(sink);
      srcNode.connect(splitter);
      splitter.connect(analyserL, 0);
      splitter.connect(analyserR, 1);
      analyserL.connect(sink); analyserR.connect(sink);
      sink.connect(ac.destination);

      this.srcNode = srcNode; this.worklet = worklet;
      this.running = true;
    }
    async stop() {
      this.running = false;
      try { if (this.stream) this.stream.getTracks().forEach((t) => t.stop()); } catch (e) { /* ignore */ }
      try { if (this.ac) await this.ac.close(); } catch (e) { /* ignore */ }
      this.ac = null; this.stream = null; this.srcNode = null; this.worklet = null;
      analyserL = analyserR = dataL = dataR = null;
    }
    async listInputs() {
      try {
        const devs = await navigator.mediaDevices.enumerateDevices();
        return devs.filter((d) => d.kind === 'audioinput');
      } catch (e) { return []; }
    }
  }

  /* -------------------------------------------------------------- public API */
  root.AVM = {
    RING, VIEWS, DEFAULTS,
    FFT, Renderer, AudioEngine,
    clamp, lin2db, hexA, makeWindow, fmtHz, noteFor, fmtFreq,
    get SR() { return SR; },
    get METER() { return METER; },
  };

})(typeof window !== 'undefined' ? window : globalThis);
