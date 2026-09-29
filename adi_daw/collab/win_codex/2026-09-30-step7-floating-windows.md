# Step 7.5 — checklist before implementation

Source: STEP-7-PLAN §3/4, handoff rulings and ADR-0063/0116/0183. Device View's expanded display is the manual comparison (Live 12 pp.443–444); ADI permits a separate floating analyser.
- Reparent an existing component without reconstructing it; docking returns that same identity.
- Also support a distinct second view of one shared analyser model while its first view remains alive.
- Persist rectangle and monitor identity in ui_view. Restore onto an available monitor and clamp to its usable bounds.
- Every native window owns its VBlankAttachment and uses app commands; no renderer owns a repaint timer.
- Poll device retirement before a frame. Deletion closes the window and clears its open state; undo restores only the device.
- First consumer: the analyser's track-overlay spectrum, read from the existing PreFader ScopeTap with the existing calibrated Spectrum on a worker. This does not substitute a scope buffer for ADR-0183's EXO tab or implement the deferred reassigned spectrogram.
- Test identity, second-view shared model, missing-monitor clamp, close/delete/undo, independent drains, software render and frame allocations.

Validation: MSVC /WX application build; 104 UI checks pass. Identity survives detach/dock; two analyser views share one worker; retirement closes and undo never reopens; lost-monitor/negative-coordinate restoration is tested. The worker reads a one-second PreFader tap and its 0 dB sine is checked after a far-bin floor check. Floating-frame allocation count is zero. Software analyser render is written to a file. JUCE exposes no EDID/stable display identifier: persisted portable identity is logical bounds plus DPI, with primary-area fallback/clamp when it no longer matches; no offscreen restore is accepted. The worker is analysis-only (33 ms cadence), never a repaint clock.
