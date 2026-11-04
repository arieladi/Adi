# Audio input recording (N20)

Session now captures actual AudioIo inputs into off-callback WAV writers. The
per-device enabled-channel configuration and the application's device opening
remain win/window work; this API does not invent a format for that setting.
Input numbers below address the channels actually supplied in AudioIo.in.

On the Session owner thread, call startRecording(AudioRecordingSpec, error).
The spec captures workFolder, sampleRate, maxFrames, ringFrames, encoding and
one or more {trackId, channels} targets. Each destination must be an armed
regular audio or MIDI track; rate and maximum block size must match the active
Session. A target's channel order is its file order, and several tracks may
record the same input. Float32 is the default and preserves the incoming bits;
PCM16/24 use the existing WAV writer's conversion. No graph-output or monitor
tap is substituted for a hardware input.

Capture starts on the first playing callback, at its actual Transport sample
position. Call session.recording().stop(), then poll() on the owner until it
returns an AudioRecordingResult. Transport stop also ends the take. Session
release or a device-format change stops the old capture. The owner must poll;
Session::tick does not consume and lose completed results. Cancel with
stop(true). If the driver is already quiescent, poll(true) can finalize without
another callback; that flag requires the device actually stopped. Destruction
has the same stopped-device contract as Session.

A stop publishes an idle generation. The worker is allowed to drain/finalize
only after the callback has acknowledged that newer generation, or under the
explicit stopped-device contract. Retired buffers, sinks and threads are freed
on the owner. Callback work is a bounded copy into a preallocated SPSC ring;
it allocates, locks, logs and performs file I/O nowhere. One worker writes all
targets, sharing the capture of repeated input channels. The ring is bounded
by 256 MiB, at most 64 distinct inputs and 256 targets, with callbacks up to
16384 frames at 8--768 kHz. Writer adapters are created off the callback; the
default sink is the actual WavWriter, including its RF64 support.

Missing inputs, oversized callbacks, queue overflow, an input stream jump or a
transport seek/loop wrap fail explicitly. A wrap inside a callback is detected
before those samples enter the take. A failed/finished capture retains its
private files for retry/recovery; explicit cancel deletes only its freshly
owned work directory after readers/writers have stopped. AudioRecordingResult
carries the failure/reason, actual rate, accepted frame count, start sample,
per-track channel/file metadata and the originating project's resolved filename.
Keep the originating window document generation beside it, especially if a
new document can reuse the same filename.

Use recording::prepareCommit(store, result, ClipIds[]) in
recording_commands.hpp, matching ids/names to the captured track order. It
checks the source project, targets, closed WAV dimensions and hashes under a
coherent read, captures the current journal head, and prepares media import/
relink, clip.create and clip.attachAudio requests. Current musical placement
must round-trip both captured sample boundaries exactly at the recording rate;
an ambiguous/extreme clock refuses. This follows the existing N5 audio-import
placement and Warp-off playback; it does not add new tempo-edit semantics.

recording::commit groups the entire pass into one native undo step. Replay
uses captured values and never records, hashes or analyzes again. Identical
takes deduplicate within the uncommitted batch and against the existing pool.
Reused external media is relinked to a verified owned recording asset. Each
publication uses a separate private copy because publishFile moves its source;
the original take survives a late SQL failure. A failed commit removes only
newly published files and leaves originals for a fresh prepare/retry. Identical
owned files retained by undo are reusable, while different content/symlinks
refuse overwrite. Successful commit removes private takes unless older media
history still refers to them. The command manages that cleanup: do not blindly
discard a retained source after success, because Undo may need its old path.
Undo retains the owned asset; Redo restores the playing clips without the
private preparation directory. Repeat commit cannot remove a successful asset.

This increment is continuous linear audio-input recording. It captures the
raw driver callback timeline; hardware/manual latency calibration, monitoring,
punch/loop takes, MIDI recording, record quantization and the 30-second Capture
memory are separate work. It does not open a device, configure its ports or
claim measured Live recording parity. The window can wire its recording action
and failure/recovery status to this API while its per-device settings are built.
