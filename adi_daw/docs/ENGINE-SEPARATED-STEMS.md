# Completed stem results into native material

`adi/media/separated_stems.hpp` is the owner-worker gateway for a completed,
closed model or service result. It does not run inference, download weights,
implement a service transport, or establish the quality of a separator. The
audio engine links no inference library. Run capture and commit off the audio
and UI threads, serially with the Store's other owner work; publish the resulting
snapshot to the window in the usual way. The producer must be finished before
capture. These checks are a data boundary, not a privilege sandbox against a
process that can modify the project's private files.

The caller supplies the registered source's media id and exact BLAKE3, a valid
source frame window, service/model/version and original request, then one named
WAV basename and exact BLAKE3 per returned stem. `weightsHash` identifies the
exact weights and is required for the `builtin` service. Service results can
omit it when unavailable. The result folder must be absolute and real; files
must be real regular files under that folder. Basename escapes, symlinks,
malformed UTF-8/NUL, repeated filenames, invalid hashes, occupied or repeated
allocated identities and invalid parent groups refuse before project writes.

Supply free group/track/clip/media ids, group parent/index, tick placement,
authored labels and optional palette indices, document generation and audit
time. N is 1..1024 as a resource limit; services are not restricted to four.
The registered source and all outputs are mono/stereo, with exactly the same
rate (up to 768 kHz) and window frame count. Accepted WAV encodings are the
native reader's PCM16, PCM24 and float32. No padding, resampling, trimming,
normalization, or inferred stem order is performed. Every sample must be finite.
Returned WAV bytes are preserved exactly.
Common placement preserves the producer's alignment; equal dimensions alone
cannot prove acoustic phase alignment or separation quality.
The default total result budget is
8 GiB, configurable up to 1 TiB. Fixed-buffer copies enforce the preflight size,
and hashes authenticate the captured bytes. A stop token and owner checkpoint
allow cancellation during the bounded I/O loops. Checkpoints must not reenter
the Store. Other Store users must not use the same connection concurrently.

`prepareSeparatedStems` captures validated immutable private copies beneath an
exclusively reserved `.adi-separated-*` project staging directory and native
requests, without writing project SQL rows. A common clip tick end covers the
exact window at the current tempo map; upward rounding can extend the clip by
less than one tick, never remove its final source sample. Unsupported placement
clocks refuse. Captured source media/head/client identity is checked again after
disk work. A reopened Store, changed head, wrong generation, or conflicting
caller head policy cannot commit an old result.

`commitSeparatedStems` creates content-addressed owned
`audio/separated-<BLAKE3>.wav` files without replacing an existing destination.
It verifies the publication copy's hash; existing destinations must be real
regular files with exactly that hash. Identical outputs reuse one native media
identity while retaining all authored stem tracks. Previously registered
identical audio is relinked to this owned pool through an ordinary native op;
undo restores its prior paths. Every media row, one group, N child audio tracks
and their clips form one **Separate stems** journal transaction. Clips have
Warp off, source offset zero, exact source frame counts and common placement.
Selection-before is advisory history metadata. No project opcode or schema
change is introduced.

The same SQL transaction inserts the exact original request, canonical
model/version/weights/source-window metadata and caller timestamp into the
existing `agent_requests` audit table keyed by transaction id. Operation
`actor_detail` carries the same provenance. The request row remains after undo,
as audit metadata; music digest/CBOR replay do not depend on it. Native operation
payloads replay without running the model or reopening its result folder.

Caller `beforeCommit` veto and journal head policy remain effective. SQL/veto/
cancellation failure rolls back the complete native action and audit row. It
may retain unreferenced content-addressed pool files for GC/retry: deleting a
published file could break another owner's media identity. Private stage files
are removed when the final prepared holder dies on its owner thread. Successful
commit consumes all shared holders; failed commit can retry. Callbacks refuse
both entry points before allocation, SQL or file I/O. Prepared holders and
their final destruction belong on the owner worker, never the audio callback.

The suite covers native undo/redo/reopen, actual CBOR BLOB replay under different
UI state, Session playback in both precisions, returned-byte equality, N-stem
services, source subwindows, exact single-frame coverage across supported rates,
deduplication, stale/reopened identities, cancellation, publication collision,
late veto rollback/retry, producer growth and private mutation during publication.
It makes no inference quality, installed four-stem model, GPU, or external
service parity claim. Model/process integration and installer weight licensing
and checksums remain separate work.
