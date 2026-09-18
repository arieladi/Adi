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
-- ============================================================================

PRAGMA application_id = 1094994225;   -- 0x41444931 = 'ADI1'
PRAGMA user_version   = 1000;         -- schema_major*1000 + schema_minor
PRAGMA encoding       = 'UTF-8';
PRAGMA foreign_keys   = ON;

-- ============================================================================
--  LAYER 0 — CONTAINER
-- ============================================================================

CREATE TABLE adi_meta (
    key         TEXT PRIMARY KEY,
    value       TEXT
) WITHOUT ROWID;

-- Seeded on create. schema_major/minor duplicate user_version in readable form;
-- user_version remains authoritative.
INSERT INTO adi_meta(key, value) VALUES
    ('schema_major',        '1'),
    ('schema_minor',        '0'),
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
);

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
);

CREATE TABLE tempo_map (
    id          INTEGER PRIMARY KEY,
    pos_ticks   INTEGER NOT NULL,
    bpm         REAL    NOT NULL,
    -- 0 jump (constant until next), 1 linear ramp, 2 bezier ramp
    curve       INTEGER NOT NULL DEFAULT 0 CHECK (curve BETWEEN 0 AND 2),
    tension     REAL    NOT NULL DEFAULT 0.0,
    CHECK (bpm > 0.0 AND bpm < 1000.0),
    CHECK (pos_ticks >= 0)
);
CREATE UNIQUE INDEX idx_tempo_pos ON tempo_map(pos_ticks);

-- Deliberately separate from tempo_map: they change at different places
-- and conflating them is a classic format bug (SPEC §4.4).
CREATE TABLE time_signature_map (
    id          INTEGER PRIMARY KEY,
    pos_ticks   INTEGER NOT NULL,
    numerator   INTEGER NOT NULL CHECK (numerator   BETWEEN 1 AND 255),
    denominator INTEGER NOT NULL CHECK (denominator IN (1,2,4,8,16,32,64,128)),
    CHECK (pos_ticks >= 0)
);
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
);
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
);

-- Cubase Arranger Track / Ableton locators: named sections that can be
-- reordered into a play list without moving any clips.
CREATE TABLE arranger_sections (
    id          INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL DEFAULT '',
    start_ticks INTEGER NOT NULL,
    end_ticks   INTEGER NOT NULL,
    color       INTEGER,
    CHECK (end_ticks > start_ticks)
);

CREATE TABLE arranger_chain (
    id          INTEGER PRIMARY KEY,
    ord         INTEGER NOT NULL,
    section_id  INTEGER NOT NULL REFERENCES arranger_sections(id) ON DELETE CASCADE,
    repeats     INTEGER NOT NULL DEFAULT 1 CHECK (repeats >= 1)
);
CREATE INDEX idx_arranger_chain_ord ON arranger_chain(ord);

-- ============================================================================
--  LAYER 1 — CORE: tracks, lanes, mixer, routing
-- ============================================================================

CREATE TABLE tracks (
    id              INTEGER PRIMARY KEY,
    parent_id       INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
    index_in_parent INTEGER NOT NULL DEFAULT 0,
    -- 'folder' and 'group' are NOT the same thing (SPEC §6.1): a folder is an
    -- organisational container with no signal path (Cubase), a group is a real
    -- summing bus (Ableton). Merging them gets one of the two wrong.
    kind            TEXT    NOT NULL CHECK (kind IN (
                        'audio','midi','instrument','group','folder','return',
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
);
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
);
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
);

-- One table for every signal connection: outputs, sends, sidechains, cue
-- feeds, VCA links. Cubase Direct Routing (several simultaneous outputs on one
-- channel) falls out for free; an output column on `tracks` would forbid it.
CREATE TABLE routing (
    id          INTEGER PRIMARY KEY,
    src_kind    TEXT    NOT NULL CHECK (src_kind IN ('track','device','hw_in','hw_out','bus')),
    src_id      INTEGER NOT NULL,
    dst_kind    TEXT    NOT NULL CHECK (dst_kind IN ('track','device','hw_in','hw_out','bus')),
    dst_id      INTEGER NOT NULL,
    kind        TEXT    NOT NULL CHECK (kind IN ('main','send','sidechain','vca','cue')),
    ord         INTEGER NOT NULL DEFAULT 0,
    gain_db     REAL    NOT NULL DEFAULT 0.0,
    pan         REAL    NOT NULL DEFAULT 0.0,
    pre_fader   INTEGER NOT NULL DEFAULT 0,
    enabled     INTEGER NOT NULL DEFAULT 1,
    -- Channel mapping for partial / multichannel connections, NULL = straight.
    channel_map BLOB
);
CREATE INDEX idx_routing_src ON routing(src_kind, src_id);
CREATE INDEX idx_routing_dst ON routing(dst_kind, dst_id);

-- ============================================================================
--  LAYER 1 — CORE: session view (SPEC §6.5 — core, not an extension)
-- ============================================================================

CREATE TABLE scenes (
    id          INTEGER PRIMARY KEY,
    ord         INTEGER NOT NULL,
    name        TEXT    NOT NULL DEFAULT '',
    color       INTEGER,
    tempo       REAL,               -- NULL = do not change tempo on launch
    sig_num     INTEGER,
    sig_den     INTEGER
);
CREATE UNIQUE INDEX idx_scenes_ord ON scenes(ord);

CREATE TABLE clip_slots (
    id              INTEGER PRIMARY KEY,
    track_id        INTEGER NOT NULL REFERENCES tracks(id)  ON DELETE CASCADE,
    scene_id        INTEGER NOT NULL REFERENCES scenes(id)  ON DELETE CASCADE,
    clip_id         INTEGER REFERENCES clips(id) ON DELETE SET NULL,
    -- Launch behaviour
    launch_mode     INTEGER NOT NULL DEFAULT 0 CHECK (launch_mode BETWEEN 0 AND 3), -- trigger/gate/toggle/repeat
    launch_quant    INTEGER NOT NULL DEFAULT -1,  -- -1 = global, else ticks
    legato          INTEGER NOT NULL DEFAULT 0,
    velocity_to_vol REAL    NOT NULL DEFAULT 0.0,
    -- Follow actions
    follow_time_ticks   INTEGER,
    follow_action_a     INTEGER NOT NULL DEFAULT 0,
    follow_action_b     INTEGER NOT NULL DEFAULT 0,
    follow_chance_a     INTEGER NOT NULL DEFAULT 1,
    follow_chance_b     INTEGER NOT NULL DEFAULT 0,
    follow_enabled      INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX idx_slot_cell ON clip_slots(track_id, scene_id);

-- ============================================================================
--  LAYER 1 — CORE: clips and their content
-- ============================================================================

CREATE TABLE clips (
    id              INTEGER PRIMARY KEY,
    track_id        INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
    lane_id         INTEGER REFERENCES lanes(id)  ON DELETE SET NULL,
    kind            TEXT    NOT NULL CHECK (kind IN ('audio','midi','automation','video','marker')),
    name            TEXT    NOT NULL DEFAULT '',
    color           INTEGER,
    -- Placement. A clip owned only by a clip_slot has NULL position: it is not
    -- on the arrangement timeline.
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
    -- Non-NULL when this clip is an alias of another (Ableton linked clips,
    -- Cubase shared parts): edits propagate to every alias of the same source.
    alias_of        INTEGER REFERENCES clips(id) ON DELETE SET NULL,
    CHECK (time_base = 1 OR pos_ns IS NULL)
);
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
);
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
);
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
);

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
);
CREATE INDEX idx_autolane_owner ON automation_lanes(owner_kind, owner_id);

-- Track-scoped automation has clip_id NULL. Clip-scoped automation (Ableton
-- clip envelopes / modulation) sets clip_id, and its times are clip-relative.
CREATE TABLE automation_data (
    id          INTEGER PRIMARY KEY,
    lane_id     INTEGER NOT NULL REFERENCES automation_lanes(id) ON DELETE CASCADE,
    clip_id     INTEGER REFERENCES clips(id) ON DELETE CASCADE,
    data        BLOB    NOT NULL        -- AAUT stream
);
CREATE UNIQUE INDEX idx_autodata ON automation_data(lane_id, IFNULL(clip_id, -1));

-- ============================================================================
--  LAYER 2 — PLUGIN STATE
-- ============================================================================

CREATE TABLE plugin_refs (
    id          INTEGER PRIMARY KEY,
    format      TEXT    NOT NULL CHECK (format IN ('vst3','vst2','clap','au','auv3','lv2','ladspa','internal')),
    uid         TEXT    NOT NULL,       -- format-specific unique id, hex/URI
    vendor      TEXT    NOT NULL DEFAULT '',
    name        TEXT    NOT NULL DEFAULT '',
    version     TEXT    NOT NULL DEFAULT '',
    subtype     TEXT    NOT NULL DEFAULT '',   -- 'instrument' | 'effect' | 'midi_effect'
    path_hint   TEXT    NOT NULL DEFAULT '',
    is_shell    INTEGER NOT NULL DEFAULT 0,
    shell_id    INTEGER
);
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
);

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
    -- Set when the plugin could not be instantiated on last load (SPEC §7.1).
    -- The device stays in the chain as a bypassed placeholder; it is NEVER
    -- dropped, because dropping it silently rewires the signal path.
    missing         INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_devices_chain ON devices(chain_id, ord);

-- Plugin state is not always one stream (SPEC §7): VST3 has component +
-- controller, LV2 has state + files, AU has a classinfo dict. Separate rows.
CREATE TABLE plugin_state (
    id          INTEGER PRIMARY KEY,
    device_id   INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    stream_role TEXT    NOT NULL,       -- 'component'|'controller'|'state'|'classinfo'|'chunk'|'files'
    data        BLOB    NOT NULL,
    format_hint TEXT    NOT NULL DEFAULT ''
);
CREATE UNIQUE INDEX idx_pstate ON plugin_state(device_id, stream_role);

-- The missing-plugin safety net (SPEC §7.1). Redundant while the plugin loads;
-- the entire reason the project is still workable when it does not.
CREATE TABLE plugin_params (
    id               INTEGER PRIMARY KEY,
    device_id        INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    param_id         TEXT    NOT NULL,
    name             TEXT    NOT NULL DEFAULT '',
    normalized_value REAL    NOT NULL DEFAULT 0.0,
    real_value       REAL,
    display          TEXT    NOT NULL DEFAULT '',
    unit             TEXT    NOT NULL DEFAULT '',
    flags            INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX idx_pparam ON plugin_params(device_id, param_id);

-- Rack macros and their mappings (SPEC §6.6).
CREATE TABLE macros (
    id          INTEGER PRIMARY KEY,
    device_id   INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    ord         INTEGER NOT NULL,
    name        TEXT    NOT NULL DEFAULT '',
    value       REAL    NOT NULL DEFAULT 0.0,
    color       INTEGER
);
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
);

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
    embedded        INTEGER NOT NULL DEFAULT 0,
    missing         INTEGER NOT NULL DEFAULT 0,
    imported_utc    INTEGER,
    -- Cached peaks for waveform drawing. Regenerable; never authoritative.
    peaks           BLOB
);
CREATE INDEX idx_media_hash ON media_files(hash_blake3);

-- Embedding is a flag, not a different format (SPEC §10.4). Chunked so a large
-- file does not depend on a raised SQLITE_MAX_LENGTH in a third-party reader.
CREATE TABLE media_blobs (
    media_id    INTEGER NOT NULL REFERENCES media_files(id) ON DELETE CASCADE,
    chunk_index INTEGER NOT NULL,
    data        BLOB    NOT NULL,
    PRIMARY KEY (media_id, chunk_index)
) WITHOUT ROWID;

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
    payload     BLOB,                   -- encoding TBD — SPEC §12.1
    inverse     BLOB,                   -- enough to revert without replaying
    label       TEXT    NOT NULL DEFAULT '',   -- what the undo menu shows
    tags        TEXT    NOT NULL DEFAULT ''
);
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
);
-- Exactly one branch is current. A partial index rather than a convention,
-- because "the current branch" with two claimants is a corrupt undo tree and
-- the failure would surface much later, as the wrong history (ADR-0026).
CREATE UNIQUE INDEX idx_branch_current ON op_branches(is_current)
    WHERE is_current = 1;

-- ============================================================================
--  LAYER 3 — SESSION: UI, windows, controllers, snapshots
-- ============================================================================

CREATE TABLE session_state (
    key     TEXT PRIMARY KEY,
    value   TEXT
) WITHOUT ROWID;

CREATE TABLE ui_view (
    id          INTEGER PRIMARY KEY,
    scope_kind  TEXT    NOT NULL,       -- 'project'|'track'|'clip'|'device'|'editor'
    scope_id    INTEGER,
    key         TEXT    NOT NULL,       -- 'height'|'folded'|'zoom_x'|'scroll_y'|...
    value       TEXT    NOT NULL
);
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
);

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
);

-- Mixer snapshots (Cubase) and track versions: named states a user can recall.
CREATE TABLE snapshots (
    id           INTEGER PRIMARY KEY,
    kind         TEXT    NOT NULL,      -- 'mixer'|'track_version'|'arrangement'
    scope_id     INTEGER,
    name         TEXT    NOT NULL DEFAULT '',
    created_utc  INTEGER NOT NULL,
    data         BLOB    NOT NULL
);

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
);
CREATE UNIQUE INDEX idx_ext ON extensions(ns, key, scope_kind, IFNULL(scope_id,-1));

-- ============================================================================
--  END
-- ============================================================================
