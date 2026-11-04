# Track recording options (N31)

TrackNode.recording carries recordEnabled/recordGrid, playEnabled/playGrid,
playForgivenessMs and keepMonitoringLatency. Every field participates in node
sharing. Missing rows, including a read-only earlier schema, give Rec-Q on at
1/16, Play-Q off with a retained 1/16 grid and zero forgiveness, and Keep
Monitoring Latency on. Snapshot rebuild preloads the optional rows once rather
than issuing one query per track.

The window submits track.setRecordQuantize {id,on,grid?},
track.setPlayQuantize {id,on,grid?,forgivenessMs?}, or
track.setKeepMonitoringLatency {id,on}. Disabling a quantizer retains its grid;
re-enabling without grid restores that choice. Grids are 1/4, 1/8, 1/16 and
1/32; explicit Play-Q forgiveness is 0..1000 ms. These are undoable edits.
Their complete typed row inverse restores absence on the first touch and
preserves unknown columns, nullable values and BLOBs on subsequent touches.

When adding a track, append trackRecordingDefaults(newId, recordChoice,
keepMonitoringLatency) to the same batch as track.create. Read recordChoice
from the app's record.quantize and keepMonitoringLatency from its
audio.keepMonitoringLatency before building the batch; the helper captures
those explicit values. The handler never reads app preferences on replay.
Kinds remain hints: an audio-kind track can still carry MIDI clips, so the
policy ops accept every existing track rather than infer capability from its
icon. For New, Store::createNewProject(path,error,recordChoice,keepMonitoringLatency)
captures the caller's defaults for all four initial tracks at the history root,
with no ops or extra undo step. The existing two-argument factory keeps the
fixed defaults. Invalid choices create no partial project file.

The row model and text projection also carry the policy. Fixed defaults are
omitted; changed values render recQuantizeOff, recGrid, playQuantize, playGrid,
playForgivenessMs and discardMonitoringLatency on the track.

Schema 1.14 adds track_recording_options and freezes the exact predecessor.
It rewrites no existing table or policy. Win must reconcile this additive minor
with other unmerged engine format additions when merging; this branch starts
at main and has no stack dependency. README table counts remain win-owned.

This need supplies the per-track persistence, ops and snapshot API. It does
not add a recording writer, post-take quantize action or live Play-Q scheduling.
The existing playQuantizeRelease helper remains the sample-domain calculation;
its graph pending-list integration and the recorder remain explicitly open
engine work. Until those consumers exist, changing stored policy alone changes
no live MIDI release time and no recorded audio timestamp.
