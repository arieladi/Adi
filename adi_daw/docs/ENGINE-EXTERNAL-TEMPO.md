# Coherent external-tempo performance generations

The owner may supply `SessionSpec::externalTempoBpm` when loading a new
performance Session. This is a prepared, constant-tempo playback generation,
intended for an external musical clock such as Link. It does not edit project
tempo automation, history, snapshots or the raw `Session::model()` projection.
Omitting the option retains the existing authored-tempo behavior.

The private playback model receives the external constant tempo before audio
clip/Warp preparation, MIDI placement and automation compilation. Transport
metadata and saved musical loop conversion now use that same private tempo.
Instrument BPM, event boundaries, rendered Warp, fader automation and loop
wraps therefore agree; changing only TransportInfo/display BPM would leave
prepared source timing wrong. Physical sample-rate re-preparation and ordinary
project refresh retain the selected external tempo. Refresh still imports
other project changes while authored tempo changes remain dormant in this
performance generation. Unwarped fixed source clocks retain their existing
native semantics; no whole-output speed resampling is introduced.

The optional value must be finite and within 20..999 BPM. It requires
`performanceState=true`; an accidental external-tempo option on an offline
generation refuses rather than silently changing an export. Existing frozen
tracks contain fingerprinted absolute-time audio, so this option refuses them
with an explicit unfreeze error. Playing that cached material with new musical
timing would require a separate frozen-audio adaptation; it is not assumed.
Invalid option validation precedes loader/model mutation and leaves the Session
available for a later valid load. Native Session loading also refuses callback
entry before diagnostics, allocation/free, SQL or file I/O.

The setting is immutable for that prepared generation. A tempo change requires
owner preparation/publication of the next coherent generation; no callback may
rewrite the model or construct a Warp cache. The application owns the generation
and its lifetime, and must coordinate publication with transport phase and the
first audible buffer time. This API does not join a Link network, start/stop
transport, schedule a quantum launch, enqueue an automatic rebuild or wire UI
settings. The independent Link SDK adapter and this native preparation primitive
are prerequisites for the next transport/playback integration. A selected
constant-tempo generation is not a claim of complete continuous Link following.

The native suite compares actual Session PCM against an otherwise identical
project authored at 60 BPM, while the original project retains 120/240 BPM
automation. Re-Pitch audio, fader automation, MIDI note start/end and instrument
transport metadata agree at both precisions and 44.1/48 kHz. Actual driver-rate
changes and refresh after further authored tempo edits preserve the same
behavior. Saved musical loops wrap at the externally prepared boundaries.
Native project digest, SQLite change count and journal checks prove preparation
and playback do not rewrite authored state. Callback new/delete/file I/O and a
native SQLite statement trace audit the load refusal; rendered callbacks retain
ready PCM without allocation/free/file I/O. An actual native Freeze capture and
commit exercise the frozen-cache refusal. Compiler/fault/full-gate evidence is
recorded in the engine log, without a measured Live or unrun platform claim.
