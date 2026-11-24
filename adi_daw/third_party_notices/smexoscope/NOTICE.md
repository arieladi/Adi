# s(M)exoscope acquisition provenance

Copyright (C) 2003 Bram de Jong. Modernization/documentation by Matthijs Hollemans,
December 2024, following Armando Montanez's JUCE port and Jason Perez's fixes.

ADI's `src/adi/dsp/triggered_scope.cpp` derives continuous trigger/holdoff,
DC-filter and ordered-extrema mechanics from `Source/Smexoscope.cpp` in
https://github.com/hollance/sMexoscope at
`caa10448df6c923bb5bc0f099a4bad4bc705c09d`.
That file's SHA256 is
`5291FF471B8E8BA6B95FE5AE0718501FE2F67EF3AADB4B4A44CC30B93D3B34C8`.

The pinned source grants GPL version 3; it does not supply a separate project
grant of later versions. This derived component is labeled GPL-3.0-only.
The full GNU GPL version 3 is in the adjacent LICENSE.txt.

Classic scope uses the same pinned checkout's eight PNGs unchanged in
`src/juce/smexoscope/images`. Original GUI design and readout artwork by
Sean of Deadskinboy. The original CustomKnob, CustomSlider, MultiStateButton,
TextElement, WaveDisplay and dimension definitions are under
`src/juce/smexoscope/widgets`. Header includes target JUCE modules directly.
WaveDisplay also rejects unavailable columns and repaints after crosshair
gestures. The adjacent Smexoscope.h projects immutable capture data on the ADI
message thread.

`src/juce/ui_scope_classic.cpp` adapts the pinned PluginEditor layout and
parameter/readout mapping to Mix View and the detached analyser. The DAW
shares acquisition through its existing worker; GUI instances neither process audio
nor poll controls on a separate timer.
