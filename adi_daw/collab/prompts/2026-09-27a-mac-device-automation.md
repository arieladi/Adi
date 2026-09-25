# 2026-09-27a — for mac on its return: the device-host half of plug-in automation

The director's instruction, 2026-09-25: win builds the engine side of plug-in
parameter automation, and the device-host integration is left entirely to
mac. The engine side is ADR-0165, and it is merged. This is the brief for the
rest. It touches `src/juce/**`, which is mac's. ADR-0166 added item 6.

```text
You are agent mac on arieladi/Adi, project adi_daw. ADR-0165 is merged: a plug-in's automation lanes
are ParamValue events addressed to its DeviceNode, generated on the audio thread on ADR-0054's grid
and at every point, each on its own segment (ADR-0042). The engine side is proved against a
recording device (tests/test_param_automation.cpp, 133 checks). No real plug-in hears them yet.

READ FIRST
ADR-0165 (and its "device-host half" section), ADR-0162 (Live's override), ADR-0124 (ParamOps,
normalized as the wire unit), src/adi/engine/param_automation.hpp, and param_edits.hpp's rule that
automation playback is never submitted by the glue.

THE WORK (src/juce/**)
1. VST3. Vst3Device::process ignores ParamValue in io.events today. Turn each into an
   IParameterChanges point for its paramId at its SEGMENT-relative offset (e.frame - io.blockOffset),
   value normalized 0..1, beside the router's mapped CCs that already use paramChanges_.
2. CLAP. ClapEventList passes a ParamValue's value through as the plain value. Device-addressed
   ParamValue is normalized (ADR-0165 d3): convert through the parameter's declared range, as
   ClapDevice::setParam already does, before it becomes CLAP_EVENT_PARAM_VALUE.
3. No echo becomes an edit. When the host keeps a plug-in's editor in step with automation, or a
   plug-in echoes a value the host set, the glue must not hand it to ParamEditCapture: an echo would
   become an op, and the op would override its own lane at once (ADR-0162).
4. The UI: an automation LED per automated parameter; Live's Re-Enable Automation button over
   Session::automationOverridden() / reenableAutomation(); re-enable one parameter from its context
   menu (reenableAutomation(laneId)). The strip's volume, pan and mute lanes (ADR-0164) share it.
5. A probe: the fixture VST3 (adi_vst3_probe) and a CLAP instrument following a lane, the rendered
   level checked against the lane.
6. Added by ADR-0166: the generic parameter panel. ADI Airwindows (plugins/airwindows, 141 CLAP
   plug-ins in one binary) has no editor, by Airwindows' design: the host draws its sliders. For a
   plug-in with no gui extension (CLAP) or no IPlugView (VST3), show a panel built from the
   parameters: each one's name, its value as the plug-in's value_to_text gives it (units included),
   a stepped parameter as a switch or menu ("Auto Gain" is one), automatable like any other. The
   same panel serves every plug-in's Parameter List (ADR-0154).

DONE MEANS
The fixture VST3 and a CLAP plug-in audibly follow a lane in adi_play --render, the probe proves it,
no automation value reaches ParamEditCapture, and CI is green by head SHA. Log it in collab/mac.md.
```
