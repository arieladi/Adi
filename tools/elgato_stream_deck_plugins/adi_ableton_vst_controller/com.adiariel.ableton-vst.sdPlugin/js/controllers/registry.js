'use strict';
/* =============================================================================
   registry.js — register the available DeviceController strategies.

   ADDING A NEW PREDEFINED VST:
     1. Create js/controllers/MyVstController.js extending AVC.DeviceController.
     2. <script> it in app.html (after DeviceController.js, before registry.js).
     3. Add one line here, keyed by the device's Live class_name:
          AVC.registry.register({ ctor: AVC.MyVstController, classNames: ['MyVst'] });
     4. (Optional) have the Python bridge send controller:"myvst" and key by hint.
   No other file needs to change — the orchestrator resolves the strategy from
   the selected device automatically.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.registry.register({ ctor: AVC.GenericController, hint: 'generic' });
AVC.registry.register({ ctor: AVC.EQ8Controller, hint: 'eq8', classNames: ['Eq8'] });

// VST3 plugins are matched by device name (they all share class_name "PluginDevice").
// Patterns are specific enough not to catch NI's "Massive" synth.
AVC.registry.register({
  ctor: AVC.PulsarMassiveController,
  names: [/pulsar\s*massive/i, /massive\s*passive/i, /\bmp[.\s-]?eq\b/i],
});

// FabFilter Pro-Q 3 (VST3) — matched by device name.
AVC.registry.register({
  ctor: AVC.ProQ3Controller,
  names: [/pro-?q\s*3/i, /fabfilter.*pro.?q/i, /\bpro-?q\b/i],
});

// Wavesfactory Spectre (VST3) — matched by device name.
AVC.registry.register({
  ctor: AVC.SpectreController,
  names: [/\bspectre\b/i, /wavesfactory.*spectre/i],
});

// Analog Obsession INDEQ (VST3) — matched by device name.
AVC.registry.register({
  ctor: AVC.IndeqController,
  names: [/\bindeq\b/i, /analog\s*obsession.*indeq/i],
});

// Valhalla DSP ValhallaRoom (VST3 reverb) — matched by device name.
AVC.registry.register({
  ctor: AVC.ValhallaRoomController,
  names: [/valhalla\s*room/i, /\bvalhallaroom\b/i],
});

// Valhalla DSP ValhallaVintageVerb (VST3 reverb) — matched by device name.
AVC.registry.register({
  ctor: AVC.ValhallaVintageVerbController,
  names: [/valhalla\s*vintage\s*verb/i, /vintage\s*verb/i, /\bvintageverb\b/i],
});

// Eventide Blackhole (H9 series reverb) — matched by device name.
AVC.registry.register({
  ctor: AVC.BlackholeController,
  names: [/\bblackhole\b/i, /eventide.*blackhole/i],
});

// Waves H-Delay (Hybrid Line delay; Stereo / Mono-Stereo / Mono) — by device name.
AVC.registry.register({
  ctor: AVC.HDelayController,
  names: [/\bh[-\s]?delay\b/i, /hdelay/i],
});

// Analog Obsession dBComp (compressor/limiter) — matched by device name.
AVC.registry.register({
  ctor: AVC.DbCompController,
  names: [/\bd[bB]\s*comp\b/i, /analog\s*obsession.*comp/i],
});

// Eventide Omnipressor (dynamics) — matched by device name.
AVC.registry.register({
  ctor: AVC.OmnipressorController,
  names: [/omnipressor/i, /eventide.*omnipressor/i],
});

// Newfangled Audio Saturate (spectral clipper / saturation) — matched by device
// name. Anchored so it won't catch Ableton's native "Saturator" (class Saturator).
AVC.registry.register({
  ctor: AVC.SaturateController,
  names: [/newfangled\s*saturate/i, /\bsaturate\b/i],
});

// RJ Studios SideMinder ME2 (dynamic stereo-width maximizer) — by device name.
AVC.registry.register({
  ctor: AVC.SideMinderController,
  names: [/sideminder/i, /side\s*minder/i],
});
