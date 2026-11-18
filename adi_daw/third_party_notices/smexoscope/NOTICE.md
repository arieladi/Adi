# s(M)exoscope acquisition provenance

Copyright (C) 2003 Bram de Jong. Modernization/documentation by Matthijs Hollemans,
December2024, following Armando Montanez's JUCE port and Jason Perez's fixes.

ADI's `src/adi/dsp/triggered_scope.cpp` derives continuous trigger/holdoff,
DC-filter and ordered-extrema mechanics from `Source/Smexoscope.cpp` in
https://github.com/hollance/sMexoscope at
`caa10448df6c923bb5bc0f099a4bad4bc705c09d`.
That file's SHA256 is
`5291FF471B8E8BA6B95FE5AE0718501FE2F67EF3AADB4B4A44CC30B93D3B34C8`.

The pinned source grants GPL version3; it does not supply a separate project
grant of later versions. This derived component is labeled GPL-3.0-only.
The full GNU GPL version3
is in the adjacent LICENSE.txt. ADI uses source-valued bounded buffers and owner
lifetimes; no artwork, original GUI, plug-in wrapper or raw concurrent editor
arrays are included. See docs/TRIGGERED-SCOPE.md for the authored differences.
