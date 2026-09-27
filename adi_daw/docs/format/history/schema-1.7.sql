-- ============================================================================
--  ADI DAW  —  .adi project format
--  Normative schema, version 1.0 (DRAFT — see SPEC.md, nothing is frozen)
--
--  This file is executable and is verified by tools/validate_schema.py on
--  every commit. If you change it, run that script.
--
--  Conventions used throughout:
--    *  Musical positions are i64 ticks at ADI_PPQ = 5765760 (SPEC §4.2).
--    *  Linear positions are i64 nanoseconds (SPEC §4.3).
--    *  time_base 0 = musical (pos_ticks set, pos_ns NULL)
--                 1 = linear  (pos_ns set,  pos_ticks NULL)
--    *  Every BLOB in the core tier carries the 16-byte stream header
--       described in SPEC §6.3. Readers stride by its rec_size, never by
--       sizeof(struct).
--    *  Booleans are INTEGER 0/1.
--    *  Timestamps are INTEGER microseconds since the Unix epoch, UTC.
--    *  Colours are INTEGER 0xAARRGGBB.
--    *  EVERY table is STRICT. Without it, SQLite's flexible typing lets a
--       TEXT value sit in a REAL column, and sqlite3_column_double() then
--       coerces it silently -- so a reader returns a number that is not what
--       is stored, and nothing anywhere reports a problem. There are 23 REAL
--       columns here. STRICT requires SQLite 3.37+ (Nov 2021); see ADR-0029.
-- ============================================================================

PRAGMA application_id = 1094994225;   -- 0x41444931 = 'ADI1'
PRAGMA user_version   = 1007;         -- schema_major*1000 + schema_minor
PRAGMA encoding       = 'UTF-8';
PRAGMA foreign_keys   = ON;

-- ============================================================================
--  LAYER 0 — CONTAINER
-- ============================================================================

CREATE TABLE adi_meta (
    key         TEXT PRIMARY KEY,
    value       TEXT
) STRICT, WITHOUT ROWID;

-- Seeded on create. schema_major/minor duplicate user_version in readable form;
-- user_version remains authoritative.
INSERT INTO adi_meta(key, value) VALUES
    ('schema_major',        '1'),
    ('schema_minor',        '7'),
    ('project_uuid',        ''),      -- stable identity across Save As
    ('created_utc',         ''),
    ('created_by',          ''),      -- "ADI DAW 0.1.0 (win32-x64)"
    ('modified_utc',        ''),
    ('modified_by',         ''),
    ('oplog_max_entries',   '10000'), -- SPEC §8.3 compaction policy
    ('oplog_max_age_days',  '90');

-- Advisory single-writer lock (SPEC §3.6). One row, id = 1.
CREATE TABLE session_lock (
    id              INTEGER PRIMARY KEY CHECK (id = 1),
    host            TEXT    NOT NULL,
    pid             INTEGER NOT NULL,
    app_version     TEXT    NOT NULL,
    acquired_utc    INTEGER NOT NULL,
    heartbeat_utc   INTEGER NOT NULL
) STRICT;

-- ============================================================================
--  LAYER 1 — CORE: project, time maps
-- ============================================================================

CREATE TABLE project (
    id                  INTEGER PRIMARY KEY CHECK (id = 1),
    name                TEXT    NOT NULL DEFAULT '',
    sample_rate         INTEGER NOT NULL DEFAULT 48000,
    ppq                 INTEGER NOT NULL DEFAULT 5765760,  -- SPEC §4.2; informational
    length_ticks        INTEGER NOT NULL DEFAULT 0,
    -- Timecode origin for linear-time / video work.
    timecode_origin_ns  INTEGER NOT NULL DEFAULT 0,
    frame_rate_num      INTEGER NOT NULL DEFAULT 25,
    frame_rate_den      INTEGER NOT NULL DEFAULT 1,
    -- Default record/render targets.
    render_bit_depth    INTEGER NOT NULL DEFAULT 24,
    author              TEXT    NOT NULL DEFAULT '',
    notes               TEXT    NOT NULL DEFAULT '',
    CHECK (ppq > 0),
    CHECK (sample_rate > 0)
) STRICT;

CREATE TABLE tempo_map (
    id          INTEGER PRIMARY KEY,
    pos_ticks   INTEGER NOT NULL,
    bpm         REAL    NOT NULL,
    -- 0 jump (constant until next), 1 linear ramp, 2 bezier ramp
    curve       INTEGER NOT NULL DEFAULT 0 CHECK (curve BETWEEN 0 AND 2),
    tension     REAL    NOT NULL DEFAULT 0.0,
    CHECK (bpm > 0.0 AND bpm < 1000.0),
    CHECK (pos_ticks >= 0)
) STRICT;
CREATE UNIQUE INDEX idx_tempo_pos ON tempo_map(pos_ticks);

-- Deliberately separate from tempo_map: they change at different places
-- and conflating them is a classic format bug (SPEC §4.4).
CREATE TABLE time_signature_map (
    id          INTEGER PRIMARY KEY,
    pos_ticks   INTEGER NOT NULL,
    numerator   INTEGER NOT NULL CHECK (numerator   BETWEEN 1 AND 255),
    denominator INTEGER NOT NULL CHECK (denominator IN (1,2,4,8,16,32,64,128)),
    CHECK (pos_ticks >= 0)
) STRICT;
CREATE UNIQUE INDEX idx_sig_pos ON time_signature_map(pos_ticks);

-- Key/scale over time. Feeds the chord track, scale-aware editing, and gives
-- the AI agent harmonic context without having to infer it.
CREATE TABLE key_map (
    id          INTEGER PRIMARY KEY,
    pos_ticks   INTEGER NOT NULL,
    root        INTEGER NOT NULL CHECK (root BETWEEN 0 AND 11),  -- 0 = C
    scale_id    TEXT    NOT NULL DEFAULT 'major',
    -- Bitmask of the 12 pitch classes, LSB = root. Lets us carry any scale,
    -- including ones we have no name for, without a lookup table.
    scale_mask  INTEGER NOT NULL DEFAULT 2741,                   -- major
    CHECK (pos_ticks >= 0)
) STRICT;
CREATE UNIQUE INDEX idx_key_pos ON key_map(pos_ticks);

CREATE TABLE markers (
    id          INTEGER PRIMARY KEY,
    time_base   INTEGER NOT NULL DEFAULT 0 CHECK (time_base IN (0,1)),
    pos_ticks   INTEGER,
    pos_ns      INTEGER,
    length_ticks INTEGER,      -- cycle markers have length; position markers do not
    length_ns   INTEGER,
    name        TEXT    NOT NULL DEFAULT '',
    color       INTEGER,
    -- 'position' | 'cycle' | 'chapter' | 'sync'
    kind        TEXT    NOT NULL DEFAULT 'position',
    CHECK ((time_base = 0 AND pos_ticks IS NOT NULL AND pos_ns    IS NULL)
        OR (time_base = 1 AND pos_ns    IS NOT NULL AND pos_ticks IS NULL))
) STRICT;

-- Cubase Arranger Track / Ableton locators: named sections that can be
-- reordered into a play list without moving any clips.
CREATE TABLE arranger_sections (
    id          INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL DEFAULT '',
    start_ticks INTEGER NOT NULL,
    end_ticks   INTEGER NOT NULL,
    color       INTEGER,
    CHECK (end_ticks > start_ticks)
) STRICT;

CREATE TABLE arranger_chain (
    id          INTEGER PRIMARY KEY,
    ord         INTEGER NOT NULL,
    section_id  INTEGER NOT NULL REFERENCES arranger_sections(id) ON DELETE CASCADE,
    repeats     INTEGER NOT NULL DEFAULT 1 CHECK (repeats >= 1)
) STRICT;
CREATE INDEX idx_arranger_chain_ord ON arranger_chain(ord);

-- ============================================================================
--  LAYER 1 — CORE: tracks, lanes, mixer, routing
-- ============================================================================

CREATE TABLE tracks (
    id              INTEGER PRIMARY KEY,
    parent_id       INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
    index_in_parent INTEGER NOT NULL DEFAULT 0,
    -- ADR-0044 removed 'folder'. A group is ONE object: a container in the
    -- timeline and a summing bus in the mixer, the Ableton model. Carrying a
    -- signal-free folder alongside it is the Cubase split we are rejecting,
    -- and it is the one users would pick by accident.
    --
    -- ADR-0045: 'audio', 'midi' and 'instrument' are HINTS. They set the icon
    -- and the default device; nothing may infer from them what a track is
    -- allowed to contain. A MIDI clip on an 'audio' track is legal and always
    -- was. The other values name a role in the signal graph and do bind.
    kind            TEXT    NOT NULL CHECK (kind IN (
                        'audio','midi','instrument','group','return',
                        'master','vca','marker','tempo','signature','chord',
                        'arranger','video','transposition')),
    name            TEXT    NOT NULL DEFAULT '',
    color           INTEGER,
    -- Timeline behaviour
    time_base       INTEGER NOT NULL DEFAULT 0 CHECK (time_base IN (0,1)),
    frozen          INTEGER NOT NULL DEFAULT 0,
    freeze_media_id INTEGER REFERENCES media_files(id) ON DELETE SET NULL,
    locked          INTEGER NOT NULL DEFAULT 0,
    -- Transport state
    muted           INTEGER NOT NULL DEFAULT 0,
    soloed          INTEGER NOT NULL DEFAULT 0,
    solo_defeat     INTEGER NOT NULL DEFAULT 0,
    record_armed    INTEGER NOT NULL DEFAULT 0,
    monitor_mode    INTEGER NOT NULL DEFAULT 0 CHECK (monitor_mode IN (0,1,2)), -- off/in/auto
    input_ref       TEXT,
    -- Cubase-style per-track automation mode
    automation_mode INTEGER NOT NULL DEFAULT 0
                    CHECK (automation_mode BETWEEN 0 AND 5), -- read/touch/latch/cross/overwrite/trim
    CHECK (id <> parent_id)
) STRICT;
CREATE INDEX idx_tracks_parent ON tracks(parent_id, index_in_parent);

-- Take lanes / comping (SPEC §6.2). A comp is clips across lanes with an
-- active-region selection — the model both Ableton 11+ and Cubase use.
CREATE TABLE lanes (
    id          INTEGER PRIMARY KEY,
    track_id    INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    ord         INTEGER NOT NULL DEFAULT 0,
    name        TEXT    NOT NULL DEFAULT '',
    kind        TEXT    NOT NULL DEFAULT 'take' CHECK (kind IN ('take','comp','automation')),
    muted       INTEGER NOT NULL DEFAULT 0,
    is_comp_target INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX idx_lanes_track ON lanes(track_id, ord);

CREATE TABLE mixer_strip (
    track_id    INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
    volume_db   REAL    NOT NULL DEFAULT 0.0,
    pan         REAL    NOT NULL DEFAULT 0.0 CHECK (pan BETWEEN -1.0 AND 1.0),
    pan_law     INTEGER NOT NULL DEFAULT 0,
    width       REAL    NOT NULL DEFAULT 1.0,
    input_gain_db REAL  NOT NULL DEFAULT 0.0,
    phase_invert INTEGER NOT NULL DEFAULT 0,
    -- Per-channel delay compensation offset, in samples, positive or negative.
    delay_samples INTEGER NOT NULL DEFAULT 0,
    vca_group_id INTEGER REFERENCES tracks(id) ON DELETE SET NULL
) STRICT;

-- Since 1.7 (ADR-0173, ADR-0174): native analog summing on a GROUP track. Each
-- child is played through the flavour's channel half before the group's sum,
-- and the sum through its buss half. `flavor` is a stable text key
-- ('console9', 'every.c7', ...) with deliberately no CHECK over the list: a
-- later minor adds only whole objects (ADR-0144), so a CHECK would freeze the
-- flavours for ever. A reader names a key it does not know, and plays none.
-- A row on a track that is not a group is named and not played.
CREATE TABLE group_summing (
    track_id    INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
    enabled     INTEGER NOT NULL DEFAULT 0 CHECK (enabled IN (0, 1)),
    flavor      TEXT    NOT NULL DEFAULT 'console9',
    drive_db    REAL    NOT NULL DEFAULT 0.0 CHECK (drive_db BETWEEN -12.0 AND 24.0)
) STRICT;

-- One table for every signal connection: outputs, sends, sidechains, cue
-- feeds, VCA links. Cubase Direct Routing (several simultaneous outputs on one
-- channel) falls out for free; an output column on `tracks` would forbid it.
--
-- The endpoint kinds are of TWO different sorts, and conflating them is how the
-- 'bus' bug got in (ADR-0029):
--
--   INTERNAL -- 'track' and 'device'. src_id/dst_id is a ROW ID in that table.
--               These have referential integrity, and `adi_tool check` verifies
--               them. A polymorphic column cannot carry a FOREIGN KEY, which is
--               the price of one routing table instead of six.
--   EXTERNAL -- 'hw_in' and 'hw_out'. The id is a hardware PORT INDEX on the
--               current audio device, not a row anywhere. It resolves at load
--               time against whatever interface is present, and failing to
--               resolve is normal (a project moved to another studio), not
--               corruption.
--
-- 'bus' was in this list and is GONE. There is no `buses` table and there was
-- never going to be one: a bus in this model is a track whose kind is 'group',
-- 'return' or 'master'. The CHECK therefore permitted a reference kind whose
-- target could not exist -- a dangling reference by construction, which the
-- text projection surfaced as "!unresolved(bus)" because it had nowhere to look.
CREATE TABLE routing (
    id          INTEGER PRIMARY KEY,
    src_kind    TEXT    NOT NULL CHECK (src_kind IN ('track','device','hw_in','hw_out')),
    src_id      INTEGER NOT NULL,
    dst_kind    TEXT    NOT NULL CHECK (dst_kind IN ('track','device','hw_in','hw_out')),
    dst_id      INTEGER NOT NULL,
    kind        TEXT    NOT NULL CHECK (kind IN ('main','send','sidechain','vca','cue')),
    ord         INTEGER NOT NULL DEFAULT 0,
    gain_db     REAL    NOT NULL DEFAULT 0.0,
    pan         REAL    NOT NULL DEFAULT 0.0,
    pre_fader   INTEGER NOT NULL DEFAULT 0,
    enabled     INTEGER NOT NULL DEFAULT 1,
    -- Who owns this row (ADR-0044). Grouping creates, rewrites and deletes
    -- 'auto' rows freely and MUST NOT touch a 'user' one, so redirecting a
    -- child's output by hand survives every later regroup.
    --
    -- The default is 'user' on purpose. A row written by anything that has
    -- not thought about this -- a converter, a migration, a hand-repaired
    -- file -- is one automatic grouping must leave alone. The safe default
    -- is the one that loses nothing.
    origin      TEXT    NOT NULL DEFAULT 'user' CHECK (origin IN ('auto','user')),
    -- Channel mapping for partial / multichannel connections, NULL = straight.
    channel_map BLOB
) STRICT;
CREATE INDEX idx_routing_src ON routing(src_kind, src_id);
CREATE INDEX idx_routing_dst ON routing(dst_kind, dst_id);

-- ============================================================================
--  LAYER 1 — CORE: clips and their content
-- ============================================================================

CREATE TABLE clips (
    id              INTEGER PRIMARY KEY,
    track_id        INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    lane_id         INTEGER REFERENCES lanes(id)  ON DELETE SET NULL,
    kind            TEXT    NOT NULL CHECK (kind IN ('audio','midi','automation','video','marker')),
    name            TEXT    NOT NULL DEFAULT '',
    color           INTEGER,
    -- Placement. Every clip is on the timeline (ADR-0037): it has a position in
    -- whichever domain it declares, enforced by the CHECK at the end of the
    -- table. Both columns stay nullable because only one domain applies at a
    -- time, not because a clip may be unplaced.
    time_base       INTEGER NOT NULL DEFAULT 0 CHECK (time_base IN (0,1)),
    pos_ticks       INTEGER,
    pos_ns          INTEGER,
    length_ticks    INTEGER,
    length_ns       INTEGER,
    -- Loop window, separate from placement, so a looping clip is ONE object
    -- rather than N repeats (SPEC §6.2).
    loop_enabled    INTEGER NOT NULL DEFAULT 0,
    loop_start_ticks INTEGER NOT NULL DEFAULT 0,
    loop_len_ticks  INTEGER,
    -- Content offset: where inside the clip's material playback begins.
    content_offset_ticks INTEGER NOT NULL DEFAULT 0,
    -- Common clip properties
    muted           INTEGER NOT NULL DEFAULT 0,
    gain_db         REAL    NOT NULL DEFAULT 0.0,
    fade_in_ticks   INTEGER NOT NULL DEFAULT 0,
    fade_out_ticks  INTEGER NOT NULL DEFAULT 0,
    fade_in_curve   INTEGER NOT NULL DEFAULT 1,
    fade_out_curve  INTEGER NOT NULL DEFAULT 1,
    -- Non-NULL when this clip is an alias of another (Cubase shared copies,
    -- FL Studio pattern instances; Live has none): edits propagate to every
    -- alias of the same source. clip.makeUnique ends one (ADR-0160).
    alias_of        INTEGER REFERENCES clips(id) ON DELETE SET NULL,
    CHECK (time_base = 1 OR pos_ns IS NULL),
    -- ADR-0037: a clip is placed, in exactly the domain it declares.
    CHECK ((time_base = 0 AND pos_ticks IS NOT NULL)
        OR (time_base = 1 AND pos_ns    IS NOT NULL))
) STRICT;
CREATE INDEX idx_clips_track ON clips(track_id, pos_ticks);
CREATE INDEX idx_clips_lane  ON clips(lane_id);

-- MIDI content. One blob per clip per stream kind — the granularity rule in
-- SPEC §6.3. Editing one note rewrites this clip's note blob and nothing else.
CREATE TABLE event_streams (
    id          INTEGER PRIMARY KEY,
    clip_id     INTEGER NOT NULL REFERENCES clips(id) ON DELETE CASCADE,
    -- 'notes' (ANOT) | 'cc' (ACTL) | 'sysex' (ASYX) | 'pitchbend' | 'aftertouch'
    stream_kind TEXT    NOT NULL,
    -- For per-controller streams: CC number / channel. NULL for 'notes'.
    channel     INTEGER,
    controller  INTEGER,
    data        BLOB    NOT NULL       -- 16-byte stream header + records
) STRICT;
-- IFNULL() rather than the bare columns: in SQLite two NULLs are distinct, so
-- a UNIQUE index over a nullable column does not enforce what it looks like it
-- enforces -- a clip could acquire two 'notes' streams. Caught by
-- tools/validate_schema.py check 4.
CREATE UNIQUE INDEX idx_stream_clip ON event_streams(
    clip_id, stream_kind, IFNULL(channel, -1), IFNULL(controller, -1));

-- Per-note expression (SPEC §6.3.2). One row per note per dimension, so editing
-- a single note's pressure curve rewrites only that curve.
-- This is first-class: it is how a Continuum / Osmose / Seaboard performance
-- survives at full resolution, decoupled from the MPE transport that carried it.
CREATE TABLE note_expression (
    id          INTEGER PRIMARY KEY,
    clip_id     INTEGER NOT NULL REFERENCES clips(id) ON DELETE CASCADE,
    note_id     INTEGER NOT NULL,      -- matches ANOT.note_id within this clip
    dimension   INTEGER NOT NULL,      -- 0 pitch, 1 pressure, 2 timbre, 3 gain, 4 pan, >=64 plugin
    data        BLOB    NOT NULL       -- AEXP stream
) STRICT;
CREATE UNIQUE INDEX idx_nexp ON note_expression(clip_id, note_id, dimension);

CREATE TABLE audio_clips (
    clip_id         INTEGER PRIMARY KEY REFERENCES clips(id) ON DELETE CASCADE,
    media_id        INTEGER NOT NULL REFERENCES media_files(id),
    -- Read window in FRAMES at the source file's own immutable sample rate,
    -- so it never needs rescaling if the project rate changes (SPEC §4.3).
    src_start_frames INTEGER NOT NULL DEFAULT 0,
    src_len_frames  INTEGER NOT NULL,
    -- Warping
    warp_enabled    INTEGER NOT NULL DEFAULT 0,
    warp_mode       TEXT    NOT NULL DEFAULT 'none',
    warp_markers    BLOB,               -- AWRP stream: (src_frame, tick) pairs
    -- Pitch / formant
    transpose_semis REAL    NOT NULL DEFAULT 0.0,
    formant_shift   REAL    NOT NULL DEFAULT 0.0,
    reverse         INTEGER NOT NULL DEFAULT 0,
    channel_mode    INTEGER NOT NULL DEFAULT 0  -- stereo/left/right/mono-sum
) STRICT;

-- ============================================================================
--  LAYER 1 — CORE: automation
-- ============================================================================

CREATE TABLE automation_lanes (
    id            INTEGER PRIMARY KEY,
    -- What the lane belongs to.
    owner_kind    TEXT    NOT NULL CHECK (owner_kind IN ('track','device','clip','project','routing')),
    owner_id      INTEGER NOT NULL,
    -- What it controls. param_ref is a plugin parameter id for devices, or a
    -- well-known string ('volume','pan','mute',...) for built-ins.
    param_ref     TEXT    NOT NULL,
    param_name    TEXT    NOT NULL DEFAULT '',
    time_base     INTEGER NOT NULL DEFAULT 0 CHECK (time_base IN (0,1)),
    -- 'normalized' 0..1 | 'real' (dB, Hz, ms) | 'enum'
    -- Prefer 'real' where knowable: normalized automation becomes meaningless
    -- if the plugin is missing or remaps between versions (SPEC §6.3.3).
    value_domain  TEXT    NOT NULL DEFAULT 'normalized',
    unit          TEXT    NOT NULL DEFAULT '',
    default_value REAL    NOT NULL DEFAULT 0.0,
    min_value     REAL,
    max_value     REAL,
    enabled       INTEGER NOT NULL DEFAULT 1,
    visible       INTEGER NOT NULL DEFAULT 0,
    height        INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX idx_autolane_owner ON automation_lanes(owner_kind, owner_id);

-- Track-scoped automation has clip_id NULL. Clip-scoped automation (Ableton
-- clip envelopes / modulation) sets clip_id, and its times are clip-relative.
CREATE TABLE automation_data (
    id          INTEGER PRIMARY KEY,
    lane_id     INTEGER NOT NULL REFERENCES automation_lanes(id) ON DELETE CASCADE,
    clip_id     INTEGER REFERENCES clips(id) ON DELETE CASCADE,
    data        BLOB    NOT NULL        -- AAUT stream
) STRICT;
CREATE UNIQUE INDEX idx_autodata ON automation_data(lane_id, IFNULL(clip_id, -1));

-- ============================================================================
--  LAYER 2 — PLUGIN STATE
-- ============================================================================

CREATE TABLE plugin_refs (
    id          INTEGER PRIMARY KEY,
    -- Every format this table can NAME, which is deliberately wider than the
    -- set ADI can HOST (SPEC §7.4, ADR-0041). We host VST3, and CLAP once it
    -- is written; an 'au' or 'vst2' row opens as a bypassed placeholder with
    -- its state preserved, because a converter from a macOS project must be
    -- able to say a device was there rather than silently discard it.
    format      TEXT    NOT NULL CHECK (format IN ('vst3','vst2','clap','au','auv3','lv2','ladspa','internal')),
    uid         TEXT    NOT NULL,       -- format-specific unique id, hex/URI
    vendor      TEXT    NOT NULL DEFAULT '',
    name        TEXT    NOT NULL DEFAULT '',
    version     TEXT    NOT NULL DEFAULT '',
    subtype     TEXT    NOT NULL DEFAULT '',   -- 'instrument' | 'effect' | 'midi_effect'
    path_hint   TEXT    NOT NULL DEFAULT '',
    is_shell    INTEGER NOT NULL DEFAULT 0,
    shell_id    INTEGER
) STRICT;
-- IFNULL for the same reason as idx_stream_clip: shell_id is NULL for every
-- non-shell plugin, and NULLs are distinct in a SQLite UNIQUE index.
CREATE UNIQUE INDEX idx_plugin_uid ON plugin_refs(format, uid, IFNULL(shell_id, -1));

-- Nested chains: Ableton Instrument / Audio Effect / Drum Racks (SPEC §6.6).
CREATE TABLE device_chains (
    id              INTEGER PRIMARY KEY,
    parent_device_id INTEGER REFERENCES devices(id) ON DELETE CASCADE,
    track_id        INTEGER REFERENCES tracks(id)  ON DELETE CASCADE,
    ord             INTEGER NOT NULL DEFAULT 0,
    name            TEXT    NOT NULL DEFAULT '',
    -- Zones, as (low, high, low_fade, high_fade). NULL = full range.
    key_zone        BLOB,
    vel_zone        BLOB,
    chain_zone      BLOB,
    muted           INTEGER NOT NULL DEFAULT 0,
    soloed          INTEGER NOT NULL DEFAULT 0,
    -- Exactly one owner: a chain hangs off a rack device or directly off a track.
    CHECK ((parent_device_id IS NULL) <> (track_id IS NULL))
) STRICT;

CREATE TABLE devices (
    id              INTEGER PRIMARY KEY,
    chain_id        INTEGER NOT NULL REFERENCES device_chains(id) ON DELETE CASCADE,
    ord             INTEGER NOT NULL DEFAULT 0,
    plugin_ref_id   INTEGER REFERENCES plugin_refs(id),
    name            TEXT    NOT NULL DEFAULT '',   -- user rename, may differ from plugin name
    enabled         INTEGER NOT NULL DEFAULT 1,
    -- 'rack' devices own nested device_chains; their plugin_ref_id is NULL.
    is_rack         INTEGER NOT NULL DEFAULT 0,
    rack_kind       TEXT,
    preset_name     TEXT    NOT NULL DEFAULT '',
    latency_samples INTEGER NOT NULL DEFAULT 0,
    -- Non-NULL when this device runs on another machine (ADR-0053). The
    -- plugin is still a VST3 or a CLAP -- where it runs is not what it is --
    -- so plugin_refs.format is untouched and a project built against a server
    -- opens on a machine that has the plugin locally with nothing but this
    -- column set to NULL.
    remote_host_id  INTEGER REFERENCES remote_hosts(id) ON DELETE SET NULL,
    -- Opt out of signal-driven suspension (ADR-0043). Plugins lie about
    -- getTailSamples(): reporting kNoTail and then producing a tail is common,
    -- and a tail that depends on a parameter is usually reported as whatever
    -- it was at one setting. This is the escape hatch that turns a report of
    -- a reverb being cut off from unfalsifiable into a two-click fix.
    always_process  INTEGER NOT NULL DEFAULT 0 CHECK (always_process IN (0,1)),
    -- Set when the plugin could not be instantiated on last load (SPEC §7.1).
    -- The device stays in the chain as a bypassed placeholder; it is NEVER
    -- dropped, because dropping it silently rewires the signal path.
    missing         INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX idx_devices_chain ON devices(chain_id, ord);

-- Machines that host plugins for us (ADR-0053). The DAW is the client; there
-- is no wrapper plugin in the graph, so latency, state and suspension stay
-- legible to the host instead of hiding inside an opaque device.
--
-- `enabled` is 0 until the user says otherwise. Audio and plugin state leaving
-- this machine is AI-AGENT 2's "anything that leaves the machine" category, and
-- a project that silently streams an unreleased album to an IP address because
-- it was opened is not a feature.
CREATE TABLE remote_hosts (
    id          INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL DEFAULT '',
    host        TEXT    NOT NULL,          -- hostname or address; LAN is assumed
    port        INTEGER NOT NULL,
    protocol    TEXT    NOT NULL DEFAULT 'audiogridder',
    enabled     INTEGER NOT NULL DEFAULT 0 CHECK (enabled IN (0,1)),
    last_seen_utc INTEGER
) STRICT;
CREATE UNIQUE INDEX idx_remote_host ON remote_hosts(host, port);

-- Opaque device state, stored once per distinct value (ADR-0038).
--
-- A VST3 chunk is opaque and routinely large -- a sampler with embedded
-- samples, a convolution impulse, a wavetable. Storing the bytes inline in
-- plugin_state AND again in every ops.inverse that reverts to them makes the
-- undo log the largest thing in the file, growing with the number of tweaks
-- rather than the size of the project. Content addressing, exactly as ADR-0032
-- does it for media: twenty tweaks that end where they started cost one blob.
--
-- Orphans are collected explicitly (SPEC §7.2) and `adi_tool check` verifies
-- both directions -- no unreferenced row, no reference to an absent hash.
-- Content addressing without a referential check is a slower way to lose data.
CREATE TABLE state_blobs (
    hash_blake3 TEXT    PRIMARY KEY,
    data        BLOB    NOT NULL,
    size_bytes  INTEGER NOT NULL
) STRICT, WITHOUT ROWID;

-- Plugin state is not always one stream (SPEC §7): VST3 has component +
-- controller, LV2 has state + files, AU has a classinfo dict. Separate rows.
--
-- KEYED ON (device_id, stream_role), WITH NO SURROGATE id (ADR-0057). The
-- surrogate was referenced by nothing and cost an allocation: ADR-0021 7.3
-- says an op that INSERTs a row carries that row's id in its payload, so
-- `device.loadState` would have had to invent one -- or read SQLite to find
-- out whether the row already existed, which is the ambient read 0021
-- forbids. A natural key makes the op an UPSERT with nothing to allocate.
CREATE TABLE plugin_state (
    device_id   INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    stream_role TEXT    NOT NULL,       -- 'component'|'controller'|'state'|'classinfo'|'chunk'|'files'
    -- The bytes live in state_blobs, shared with whatever op payloads and
    -- inverses reference the same value (ADR-0038).
    state_hash  TEXT    NOT NULL REFERENCES state_blobs(hash_blake3),
    format_hint TEXT    NOT NULL DEFAULT '',
    PRIMARY KEY (device_id, stream_role)
) STRICT, WITHOUT ROWID;

-- The missing-plugin safety net (SPEC §7.1). Redundant while the plugin loads;
-- the entire reason the project is still workable when it does not.
--
-- KEYED ON (device_id, param_id), WITH NO SURROGATE id -- same reason as
-- plugin_state above, and it bites harder here. `device.setParam` is
-- coalescable and fires on every knob movement; an id in that payload would
-- mean allocating one the first time each parameter is ever touched, and
-- knowing WHICH time that was is an ambient read.
--
-- normalized_value is NOT NULL and real_value is nullable, and that asymmetry
-- is the VST3 edge stated in the schema rather than in a comment elsewhere:
-- VST3 exposes a real value only as a display STRING (getParamStringByValue),
-- so a real value is recoverable for CLAP, Pd and native devices and
-- sometimes not for VST3. The normalized value is always authoritative; the
-- real value is the readable one where it can be had (ADR-0057).
CREATE TABLE plugin_params (
    device_id        INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    param_id         TEXT    NOT NULL,
    name             TEXT    NOT NULL DEFAULT '',
    normalized_value REAL    NOT NULL DEFAULT 0.0,
    real_value       REAL,
    display          TEXT    NOT NULL DEFAULT '',
    unit             TEXT    NOT NULL DEFAULT '',
    flags            INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (device_id, param_id)
) STRICT, WITHOUT ROWID;

-- The expression route a device plays with (ADR-0134 d7, ADR-0146). NO ROW IS
-- AUTO: the route resolves from what the plugin declares, and a new instance
-- takes its default from the application's capability registry. A row is the
-- user's choice, stored in the project because it changes what the plugin
-- plays: the project must sound the same on a machine whose registry says
-- otherwise. Keyed on the device, with no surrogate id, like plugin_state
-- (ADR-0057). The values are the engine's ExpressionRoute names.
CREATE TABLE device_expression_routes (
    device_id   INTEGER PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE,
    route       TEXT    NOT NULL CHECK (route IN ('note_expression','mpe_midi','plain'))
) STRICT;

-- The plug-in panel (ADR-0150, ADR-0154): the parameters a device shows as
-- sliders, in order. NO ROW IN device_panels IS LIVE'S DEFAULT: every
-- parameter when the plug-in declares 64 or fewer, none when it declares more
-- (Live 12 manual 23.3.1). A row marks the panel CONFIGURED, and its
-- parameters are exactly device_panel_params' rows -- which may be none, a
-- panel the user emptied. Per instance and saved in the project, as in Live.
-- Written only by device.setPanel, and by device.insert / device.remove for
-- undo. param_id has no foreign key: a parameter has no plugin_params row
-- until its first edit (ADR-0057), and a missing plug-in keeps its panel.
CREATE TABLE device_panels (
    device_id   INTEGER PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE
) STRICT;
CREATE TABLE device_panel_params (
    device_id   INTEGER NOT NULL REFERENCES device_panels(device_id) ON DELETE CASCADE,
    ord         INTEGER NOT NULL,
    param_id    TEXT    NOT NULL,
    PRIMARY KEY (device_id, param_id)
) STRICT, WITHOUT ROWID;
CREATE UNIQUE INDEX idx_panel_ord ON device_panel_params(device_id, ord);

-- Rack macros and their mappings (SPEC §6.6).
CREATE TABLE macros (
    id          INTEGER PRIMARY KEY,
    device_id   INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    ord         INTEGER NOT NULL,
    name        TEXT    NOT NULL DEFAULT '',
    value       REAL    NOT NULL DEFAULT 0.0,
    color       INTEGER
) STRICT;
CREATE UNIQUE INDEX idx_macro_ord ON macros(device_id, ord);

CREATE TABLE macro_mappings (
    id              INTEGER PRIMARY KEY,
    macro_id        INTEGER NOT NULL REFERENCES macros(id) ON DELETE CASCADE,
    target_device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    target_param_ref TEXT   NOT NULL,
    range_min       REAL    NOT NULL DEFAULT 0.0,
    range_max       REAL    NOT NULL DEFAULT 1.0,
    curve           INTEGER NOT NULL DEFAULT 1,
    inverted        INTEGER NOT NULL DEFAULT 0
) STRICT;

-- ============================================================================
--  LAYER 1 — MEDIA POOL (SPEC §10)
-- ============================================================================

CREATE TABLE media_files (
    id              INTEGER PRIMARY KEY,
    -- Content addressing: dedupe, integrity, and relink-by-content all from
    -- this one column (SPEC §10.1).
    hash_blake3     TEXT    NOT NULL,
    orig_name       TEXT    NOT NULL DEFAULT '',
    rel_path        TEXT,               -- relative to the .adi
    abs_path_hint   TEXT,
    sample_rate     INTEGER,
    channels        INTEGER,
    frames          INTEGER,
    duration_ns     INTEGER,
    format          TEXT    NOT NULL DEFAULT '',
    bit_depth       INTEGER,
    size_bytes      INTEGER,
    -- Always 0 from schema 1.1 (ADR-0127, ADR-0136): media is never embedded.
    -- Kept rather than dropped so a 1.0 reader can open a 1.1 file (SPEC
    -- §11); the triggers below refuse anything else; removed at 2.0.
    embedded        INTEGER NOT NULL DEFAULT 0,
    missing         INTEGER NOT NULL DEFAULT 0,
    imported_utc    INTEGER,
    -- Cached peaks for waveform drawing. Regenerable; never authoritative.
    peaks           BLOB
) STRICT;
-- UNIQUE, because ADR-0005 says content addressing is what gives dedupe --
-- "the same sample dropped in twenty times is one file". A non-unique index
-- made that a comment rather than a rule: nothing stopped twenty rows holding
-- one hash, and then relink-by-content has no single answer and the pool is not
-- a pool. Reported twice by mac while building the text projection, where
-- hash-as-designator needs it.
--
-- PARTIAL, on hash_blake3 <> '', so a row whose hash is not yet computed does
-- not collide with every other such row. A lookup by '' is not a thing anyone
-- does, so the index loses nothing by excluding them.
CREATE UNIQUE INDEX idx_media_hash ON media_files(hash_blake3)
    WHERE hash_blake3 <> '';

-- RETIRED (ADR-0127, ADR-0136). Schema 1.0 could embed audio here; from 1.1
-- media is never embedded, and a shareable project is a ZIP (SPEC §10.4).
-- The table stays, empty and locked, because SPEC §11 promises a 1.0 reader
-- can open any 1.x file, and that reader queries it. Dropped at 2.0.
CREATE TABLE media_blobs (
    media_id    INTEGER NOT NULL REFERENCES media_files(id) ON DELETE CASCADE,
    chunk_index INTEGER NOT NULL,
    data        BLOB    NOT NULL,
    PRIMARY KEY (media_id, chunk_index)
) STRICT, WITHOUT ROWID;

-- The lock. Triggers rather than a CHECK: a CHECK cannot be added to an
-- existing column, and a trigger can be dropped by a test that needs to build
-- a 1.0-shaped file.
CREATE TRIGGER media_never_embedded_insert
    BEFORE INSERT ON media_files WHEN NEW.embedded <> 0
BEGIN SELECT RAISE(ABORT, 'ADR-0127: media is never embedded in the .adi'); END;
CREATE TRIGGER media_never_embedded_update
    BEFORE UPDATE OF embedded ON media_files WHEN NEW.embedded <> 0
BEGIN SELECT RAISE(ABORT, 'ADR-0127: media is never embedded in the .adi'); END;
CREATE TRIGGER media_blobs_forbidden
    BEFORE INSERT ON media_blobs
BEGIN SELECT RAISE(ABORT, 'ADR-0127: media is never embedded in the .adi'); END;

-- ============================================================================
--  LAYER 1 — CORE: remarks anchored to objects (ADR-0131, schema 1.2)
-- ============================================================================

-- A note a person or the agent pins to a track, a clip, a device, or one
-- parameter of a device. Project data: it travels with the file and every
-- change to it is an op (remark.add/edit/resolve/remove, OPS.md 9.12), so it
-- undoes like anything else.
--
-- (target_kind, target_id) is POLYMORPHIC, like routing's endpoints, so no
-- FOREIGN KEY can hold it and nothing cascades. That is deliberate: deleting a
-- track leaves its remarks in place, undoing the delete re-anchors them, and
-- `adi_tool check` reports a remark whose target is gone as the warning
-- `remark.danglingTarget` (SPEC 6.8).
--
-- `author` is who WROTE the text, which is what the UI marks (ADR-0131 d3);
-- `ops.actor` stays the record of who submitted the op. `text` is untrusted
-- input to the agent: it is read as context and never obeyed (ADR-0131 d5,
-- AI-AGENT 7.3).
CREATE TABLE remarks (
    id           INTEGER PRIMARY KEY,
    target_kind  TEXT    NOT NULL CHECK (target_kind IN ('track','clip','device')),
    target_id    INTEGER NOT NULL,
    -- A plugin_params.param_id: set only on a device's remark. No FK either --
    -- a parameter has no row until its first edit (ADR-0057).
    param_id     TEXT,
    author       TEXT    NOT NULL CHECK (author IN ('user','agent')),
    actor_detail TEXT    NOT NULL DEFAULT '',   -- the model or person, as ops.actor_detail
    text         TEXT    NOT NULL CHECK (length(text) > 0),
    created_utc  INTEGER NOT NULL,              -- from the op payload, never a clock (OPS.md 7)
    resolved     INTEGER NOT NULL DEFAULT 0 CHECK (resolved IN (0,1)),
    CHECK (param_id IS NULL OR target_kind = 'device')
) STRICT;
CREATE INDEX idx_remarks_target ON remarks(target_kind, target_id);

-- ============================================================================
--  LAYER 3 — SESSION: the op log (SPEC §8)
-- ============================================================================

-- Every mutation to layers 1, 2 and 4 appends a row here in the SAME
-- transaction that performs it. There is no valid state in which the project
-- changed and the log did not.
CREATE TABLE ops (
    seq         INTEGER PRIMARY KEY AUTOINCREMENT,
    txn_id      INTEGER NOT NULL,       -- ops sharing a txn_id undo as ONE unit
    parent_seq  INTEGER REFERENCES ops(seq),  -- history is a TREE, not a stack
    ts_utc      INTEGER NOT NULL,
    -- The accountability column for the AI agent: "what did it change, and
    -- when" is a query, not a forensic exercise.
    actor       TEXT    NOT NULL CHECK (actor IN ('user','agent','script','import','migration','remote')),
    actor_detail TEXT   NOT NULL DEFAULT '',
    op_type     TEXT    NOT NULL,
    target_kind TEXT    NOT NULL DEFAULT '',
    target_id   INTEGER,
    -- Deterministic CBOR with short string keys — ADR-0016, settled by
    -- ADR-0025. Deterministic (lexicographic keys), NOT RFC 8949 §4.2
    -- canonical (length-first); the two are different and we mean the first.
    payload     BLOB,
    inverse     BLOB,                   -- enough to revert without replaying
    label       TEXT    NOT NULL DEFAULT '',   -- what the undo menu shows
    tags        TEXT    NOT NULL DEFAULT '',
    -- ADR-0021 §7.5: what was selected when this transaction began, so undo can
    -- restore it. ADVISORY -- never an input to a handler, purely a UI hint, and
    -- droppable by compaction.
    --
    -- Its own BLOB column rather than living in `tags`, which is where ADR-0021
    -- put it: `tags` is TEXT, and once ADR-0029 made every table STRICT a CBOR
    -- blob could no longer be stored there. Two ADRs that were each correct in
    -- isolation, and the interaction was noticed only when something tried to
    -- write one. See ADR-0030.
    sel_before  BLOB
) STRICT;
CREATE INDEX idx_ops_txn    ON ops(txn_id);
CREATE INDEX idx_ops_parent ON ops(parent_seq);
CREATE INDEX idx_ops_actor  ON ops(actor, ts_utc);

-- Named branches of the undo tree. Undoing and then doing something new forks
-- rather than destroying — which for an AI-assisted DAW is the difference
-- between the agent being usable and being frightening (SPEC §8.2).
CREATE TABLE op_branches (
    id           INTEGER PRIMARY KEY,
    name         TEXT    NOT NULL DEFAULT '',
    head_seq     INTEGER REFERENCES ops(seq),
    created_utc  INTEGER NOT NULL,
    is_current   INTEGER NOT NULL DEFAULT 0 CHECK (is_current IN (0,1)),
    created_by   TEXT    NOT NULL DEFAULT 'user'
) STRICT;
-- Every project has a current branch from the moment it is created, so nothing
-- has to cope with "no branch yet". head_seq NULL means the branch is at the
-- root: nothing done, nothing to undo.
INSERT INTO op_branches(id, name, head_seq, created_utc, is_current, created_by)
     VALUES (1, 'main', NULL, 0, 1, 'system');

-- Exactly one branch is current. A partial index rather than a convention,
-- because "the current branch" with two claimants is a corrupt undo tree and
-- the failure would surface much later, as the wrong history (ADR-0026).
CREATE UNIQUE INDEX idx_branch_current ON op_branches(is_current)
    WHERE is_current = 1;

-- History snapshots (ADR-0128, schema 1.3): a NAME ON A POINT in the op log.
-- A row copies nothing -- the log already is the history (ADR-0030) and
-- already branches -- so taking one costs a row, and reverting to one moves the
-- head there and leaves everything after it reachable as a branch (SPEC 8.6).
--
-- Named history_snapshots, not `snapshots`: that table is Cubase's mixer
-- snapshots and track versions (below), a different thing that a 1.2 reader
-- already knows by that name (ADR-0140).
--
-- History metadata, NOT an op (ADR-0128 d5): taking, renaming and reverting
-- are not undoable, because an op that renamed history would be history.
CREATE TABLE history_snapshots (
    id           INTEGER PRIMARY KEY,
    name         TEXT    NOT NULL CHECK (length(name) > 0),
    -- The point: the head when it was taken. NULL is the root, before any op.
    op_seq       INTEGER REFERENCES ops(seq),
    -- The branch that was current when it was taken. A record, not a pointer:
    -- op_seq is what a revert follows, and a later fork can leave this branch
    -- holding a different line (ADR-0140).
    branch_id    INTEGER NOT NULL REFERENCES op_branches(id),
    created_utc  INTEGER NOT NULL,   -- passed in by the caller, never a clock read
    auto         INTEGER NOT NULL DEFAULT 0 CHECK (auto IN (0,1))
) STRICT;
CREATE INDEX idx_hsnap_seq ON history_snapshots(op_seq);

-- AI-AGENT §6.8: what the user asked the agent, stored beside the transaction
-- it produced, so "what did I ask it, and what did it do" is a query six months
-- later (ADR-0146). Written in the SAME transaction as that txn's ops, by the
-- Propose-tier changeset's Apply (ADR-0145 d9). Log metadata like the ops rows
-- themselves: not an op, never undone, not in the replay digest. No foreign
-- key: ops.txn_id is not a unique key.
CREATE TABLE agent_requests (
    txn_id       INTEGER PRIMARY KEY,
    request      TEXT    NOT NULL,
    actor_detail TEXT    NOT NULL DEFAULT '',   -- model and version, as ops.actor_detail
    created_utc  INTEGER NOT NULL               -- passed in by the caller, never a clock read
) STRICT;

-- ADR-0161: who wrote each op, and a Lamport clock, so a future remote session
-- can order and merge two logs (SPEC 8.8). Beside `ops` rather than in it:
-- ADR-0144 lets a minor add whole objects only, and a column added to `ops`
-- could not be upgraded by a CREATE. Log metadata like the ops rows: not an
-- op, never undone, not in the replay digest.
--
-- A client is one open session of one application on one machine, not a
-- person: `actor` and `actor_detail` already say user or agent. 32 lowercase
-- hex characters, random, new each time a Store is opened.
CREATE TABLE op_clients (
    client_id      TEXT    PRIMARY KEY
                   CHECK (length(client_id) = 32 AND client_id NOT GLOB '*[^0-9a-f]*'),
    label          TEXT    NOT NULL DEFAULT '',   -- "ADI DAW 0.1.0 on ADI-DESKTOP", for display
    first_seen_utc INTEGER NOT NULL
) STRICT;

-- One row per op written since 1.6. `lamport` is one more than every clock
-- the writer has seen, the local log included, so (lamport, client_id) orders
-- every op of every client consistently with causality. An op without a row
-- predates 1.6: it reads as client unknown, lamport = seq, and a 1.6 writer's
-- first clock is above every such seq.
CREATE TABLE op_clocks (
    seq        INTEGER PRIMARY KEY REFERENCES ops(seq) ON DELETE CASCADE,
    client_id  TEXT    NOT NULL REFERENCES op_clients(client_id),
    lamport    INTEGER NOT NULL CHECK (lamport > 0)
) STRICT;
CREATE UNIQUE INDEX idx_opclk_lamport ON op_clocks(lamport, client_id);

-- ============================================================================
--  LAYER 3 — SESSION: UI, windows, controllers, snapshots
-- ============================================================================

CREATE TABLE session_state (
    key     TEXT PRIMARY KEY,
    value   TEXT
) STRICT, WITHOUT ROWID;

CREATE TABLE ui_view (
    id          INTEGER PRIMARY KEY,
    scope_kind  TEXT    NOT NULL,       -- 'project'|'track'|'clip'|'device'|'editor'
    scope_id    INTEGER,
    key         TEXT    NOT NULL,       -- 'height'|'folded'|'zoom_x'|'scroll_y'|...
    value       TEXT    NOT NULL
) STRICT;
CREATE UNIQUE INDEX idx_uiview ON ui_view(scope_kind, IFNULL(scope_id,-1), key);

CREATE TABLE window_state (
    id              INTEGER PRIMARY KEY,
    kind            TEXT    NOT NULL,   -- 'plugin'|'editor'|'mixer'|'browser'|'main'
    ref_id          INTEGER,            -- device_id for plugin windows
    x               INTEGER NOT NULL,
    y               INTEGER NOT NULL,
    w               INTEGER NOT NULL,
    h               INTEGER NOT NULL,
    -- Restoring to x=3200 on a machine that no longer has a second monitor puts
    -- the window offscreen. Readers MUST clamp to the current arrangement.
    monitor_id      TEXT    NOT NULL DEFAULT '',
    monitor_bounds  TEXT    NOT NULL DEFAULT '',
    visible         INTEGER NOT NULL DEFAULT 0,
    always_on_top   INTEGER NOT NULL DEFAULT 0,
    scale_pct       INTEGER NOT NULL DEFAULT 100
) STRICT;

-- Project-scoped controller bindings: surfaces, Stream Deck, Continuum,
-- generic MIDI. Project-scoped rather than global, because an orchestral
-- template and a club track want different maps, and because a project handed
-- to a collaborator should arrive with its controller layout intact.
CREATE TABLE controller_maps (
    id           INTEGER PRIMARY KEY,
    device_name  TEXT    NOT NULL DEFAULT '',
    protocol     TEXT    NOT NULL DEFAULT 'midi',  -- 'midi'|'osc'|'mackie'|'hui'
    channel      INTEGER,
    msg_type     TEXT,                  -- 'cc'|'note'|'pb'|'nrpn'|'sysex'|'osc_path'
    msg_num      INTEGER,
    osc_path     TEXT,
    target_kind  TEXT    NOT NULL,
    target_id    INTEGER,
    target_param TEXT    NOT NULL DEFAULT '',
    mode         INTEGER NOT NULL DEFAULT 0,       -- absolute/relative/toggle
    takeover     INTEGER NOT NULL DEFAULT 0,       -- jump/pickup/scale
    range_min    REAL    NOT NULL DEFAULT 0.0,
    range_max    REAL    NOT NULL DEFAULT 1.0,
    enabled      INTEGER NOT NULL DEFAULT 1
) STRICT;

-- Mixer snapshots (Cubase) and track versions: named states a user can recall.
CREATE TABLE snapshots (
    id           INTEGER PRIMARY KEY,
    kind         TEXT    NOT NULL,      -- 'mixer'|'track_version'|'arrangement'
    scope_id     INTEGER,
    name         TEXT    NOT NULL DEFAULT '',
    created_utc  INTEGER NOT NULL,
    data         BLOB    NOT NULL
) STRICT;

-- ============================================================================
--  LAYER 4 — EXTENSIONS (SPEC §9)
-- ============================================================================

-- Readers MUST preserve rows here byte-for-byte across load/save, MUST NOT
-- fail to open because of one, and MUST warn before render/export when an
-- unknown row is criticality='essential'. Silent degradation at render time is
-- data loss with extra steps.
CREATE TABLE extensions (
    id          INTEGER PRIMARY KEY,
    ns          TEXT    NOT NULL,       -- reverse-DNS; 'org.adidaw.' reserved
    key         TEXT    NOT NULL,
    scope_kind  TEXT    NOT NULL DEFAULT 'project',
    scope_id    INTEGER,
    criticality TEXT    NOT NULL DEFAULT 'advisory'
                CHECK (criticality IN ('advisory','essential')),
    min_reader  INTEGER NOT NULL DEFAULT 0,   -- min user_version that can interpret
    mime        TEXT    NOT NULL DEFAULT 'application/octet-stream',
    data        BLOB
) STRICT;
CREATE UNIQUE INDEX idx_ext ON extensions(ns, key, scope_kind, IFNULL(scope_id,-1));

-- ============================================================================
--  END
-- ============================================================================
