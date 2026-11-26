# Basics plug-ins

The Windows 10 walker's queue (`collab/prompts/2026-11-27-basics-plugins.md`). Headless on that machine:
Release builds with warnings as errors, 2 jobs beside another session. Real plug-ins are measured through
ADI's own hosts with `adi_vst3_probe --param-reach` and `adi_clap_probe --param-reach`: three fresh
instances on one timeline (no change, the value set by the host, the same value as automation), the level
over the same window.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-310 | `basics/sc-310-vst3-param-to-processor` `5697e374` | `Vst3Device::setParam` also posts the value to a per-parameter slot (atomic value and sequence, made with the parameter list at construction); the next raw process call sends every changed slot through `IParameterChanges` at the segment's start, before automation. No lock or allocation on the audio thread; a slot that finds the pool full is retried; the pool holds every parameter, so a project's rows reach the processor in one call. ADR-0073's cure, built. Touched the plug-in host (`src/juce/vst3_host.*`), both probes, the owned VST3 fixture (it now records what its processor received) and `tests/test_vst3_key.cpp` | `adi_vst3_key_tests` valuesReachProcessor, realtime and offline (set before the first block as at project open, from the panel, back to default, the last of two): 8 failures before, 168/0 after. Probes before the fix, host vs automation: Surge XT playing -22.0 vs -58.4 dB; ADPTR Hype playing -15.3 vs -36.9; FabFilter Pro-C 3 stopped and playing -13.1 vs -29.8; Saturn 2 -14.6 vs -42.3. After: equal to automation in every case (also Diva, Hive, Presswerk). CLAP measured with the same test: Surge XT, Pro-C 3, Saturn 2 equal automation without any change. `adi_vst3_probe --fixture` 225/0, acceptance wts01 and wts01_vst3 0 FAIL | PR requested |
