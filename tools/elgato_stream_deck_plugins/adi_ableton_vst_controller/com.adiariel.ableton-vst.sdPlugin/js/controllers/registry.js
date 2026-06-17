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
