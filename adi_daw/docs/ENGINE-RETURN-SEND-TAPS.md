# A return's send tap (N42)

TrackNode.returnSendTap is present on return tracks only; rows::Track exposes the
same ReturnSendTap. preFader is the chosen/derived value, explicitChoice says the
project stores a return choice, and legacyMixed marks an older return whose
incoming edges disagree. Every field participates in snapshot sharing. Empty
returns default to Post. An unchanged return shares its node; published frames
are immutable.

The window switches a return with return.setSendTap {id, pre}, one edit and one
undo step. It stores the choice even with no incoming sends and changes all of
the return's incoming send taps, enabled or disabled. It never changes cue or
main edges, another return, or an equal-number hardware endpoint. The persisted
choice controls ordinary new routing.connect sends into that return and the
actual engine's send tap. Snapshot.sends and rows::Routing.preFader report that
effective value; a raw mismatch is retained in rows::Routing.storedPreFader for
projection rather than silently rewriting the project while reading it.

For creating a send, prepareSendToReturn(store, routingId, sourceTrack, returnTrack)
captures literal caller ids and the legacy/default tap when needed. Submit with
commitSendToReturn; it checks project filename and the captured head and groups
the requests into one undo step. Keep the window's document/generation check too,
and reserve ids above every current routing kind (N39), not just send rows. The
producer does not allocate an id inside an op or query UI selection/settings.
A conflicting new routing.setPreFader edit on one edge of an explicitly chosen
return refuses with the return-op name; a same-value setter remains compatible.

Schema 1.10 adds only return_send_taps(track_id, pre_fader). Migration adds an
empty table without changing existing edge taps or sound. A read-only older
project gets the normal empty TEMP stand-in. Before the first explicit return
choice, legacy sends keep their individual raw taps. A uniform legacy value is
reported and inherited by the new-send producer. Mixed legacy sends remain
mixed, reported as legacyMixed; choosing Pre or Post explicitly makes them
uniform. A new send in that mixed/unchosen case uses the Post default. Historical
scalar payloads retain their original apply behavior; undoing the new choice
restores the exact old preference absence and raw mixed values.

The inverse captures choice presence and the ordered incoming id/tap set. A
restore verifies the expected current choice, flags and edge set before writing.
It updates only tap columns and uses UPSERT UPDATE, never row replacement, so
unrelated/future metadata and BLOBs stay in place. Undo of a newly created choice
removes that new row. A late batch failure rolls back preference, flags and log
as one journal transaction. N37's generic track-delete inverse discovers this
new FK child when the branches are combined; this branch does not duplicate it.

routing.disconnect also captures its full typed row for reconnect, including
channel-map/future BLOBs, gain, pan, order, enable and origin. An inverse reconnect
restores the raw row exactly instead of inheriting today's return choice. Old
six-field inverse packets remain accepted. Text projection inlines explicit
sendTap pre/post on the return, including stored Post, while retaining raw routing
fields; absent defaults add no attribute. No UI production or DSP code changes,
and no additional allocation, SQLite work or locking enters the audio callback.
