-- SPDX-License-Identifier: AGPL-3.0-or-later
-- Reserved 1.26 addition (TM-SV-02). Apply after the real 1.25 schema.
-- Development tests exercise these objects over 1.23 without publishing a
-- new user_version. Append these statements to schema.sql when 1.25 lands
-- and register their names in migrationSteps(); no existing object changes.

CREATE TABLE scenes (
    id                    INTEGER PRIMARY KEY CHECK (id > 0),
    ord                   INTEGER NOT NULL UNIQUE CHECK (ord >= 0),
    name                  TEXT NOT NULL DEFAULT '',
    color                 INTEGER CHECK (color BETWEEN 0 AND 69),
    tempo_bpm             REAL NOT NULL DEFAULT 120.0 CHECK (tempo_bpm BETWEEN 20.0 AND 999.0),
    tempo_enabled         INTEGER NOT NULL DEFAULT 0 CHECK (tempo_enabled IN (0,1)),
    signature_numerator   INTEGER NOT NULL DEFAULT 4 CHECK (signature_numerator BETWEEN 1 AND 99),
    signature_denominator INTEGER NOT NULL DEFAULT 4 CHECK (signature_denominator IN (1,2,4,8,16)),
    signature_enabled     INTEGER NOT NULL DEFAULT 0 CHECK (signature_enabled IN (0,1)),
    follow_enabled        INTEGER NOT NULL DEFAULT 0 CHECK (follow_enabled IN (0,1)),
    follow_duration_ticks INTEGER NOT NULL DEFAULT 23063040 CHECK (follow_duration_ticks > 0),
    follow_a              INTEGER NOT NULL DEFAULT 0 CHECK (follow_a BETWEEN 0 AND 9),
    follow_a_scene_id     INTEGER REFERENCES scenes(id) DEFERRABLE INITIALLY DEFERRED,
    follow_b              INTEGER NOT NULL DEFAULT 0 CHECK (follow_b BETWEEN 0 AND 9),
    follow_b_scene_id     INTEGER REFERENCES scenes(id) DEFERRABLE INITIALLY DEFERRED,
    follow_b_chance       REAL NOT NULL DEFAULT 0.0 CHECK (follow_b_chance BETWEEN 0.0 AND 100.0),
    CHECK ((follow_a = 9) = (follow_a_scene_id IS NOT NULL)),
    CHECK ((follow_b = 9) = (follow_b_scene_id IS NOT NULL))
) STRICT;

-- No row means an empty slot with a Stop button. A cleared cell can keep
-- its authored Stop override. A clip owns at most one cell.
CREATE TABLE clip_slots (
    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    scene_id INTEGER NOT NULL REFERENCES scenes(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    clip_id  INTEGER UNIQUE REFERENCES clips(id) ON DELETE SET NULL DEFERRABLE INITIALLY DEFERRED,
    has_stop INTEGER NOT NULL DEFAULT 1 CHECK (has_stop IN (0,1)),
    PRIMARY KEY (track_id, scene_id),
    CHECK (track_id > 0 AND scene_id > 0),
    CHECK (clip_id IS NULL OR clip_id > 0)
) STRICT, WITHOUT ROWID;
CREATE INDEX idx_clip_slots_scene ON clip_slots(scene_id);

CREATE TABLE clip_launch (
    clip_id               INTEGER PRIMARY KEY REFERENCES clips(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    launch_mode           INTEGER NOT NULL DEFAULT 0 CHECK (launch_mode BETWEEN 0 AND 3),
    launch_quant          INTEGER NOT NULL DEFAULT 0 CHECK (launch_quant BETWEEN 0 AND 14),
    legato                INTEGER NOT NULL DEFAULT 0 CHECK (legato IN (0,1)),
    velocity_amount       REAL NOT NULL DEFAULT 0.0 CHECK (velocity_amount BETWEEN 0.0 AND 1.0),
    follow_enabled        INTEGER NOT NULL DEFAULT 0 CHECK (follow_enabled IN (0,1)),
    follow_linked         INTEGER NOT NULL DEFAULT 1 CHECK (follow_linked IN (0,1)),
    follow_loop_count     INTEGER NOT NULL DEFAULT 1 CHECK (follow_loop_count > 0),
    follow_duration_ticks INTEGER NOT NULL DEFAULT 23063040 CHECK (follow_duration_ticks > 0),
    follow_a              INTEGER NOT NULL DEFAULT 0 CHECK (follow_a BETWEEN 0 AND 9),
    follow_a_scene_id     INTEGER REFERENCES scenes(id) DEFERRABLE INITIALLY DEFERRED,
    follow_b              INTEGER NOT NULL DEFAULT 0 CHECK (follow_b BETWEEN 0 AND 9),
    follow_b_scene_id     INTEGER REFERENCES scenes(id) DEFERRABLE INITIALLY DEFERRED,
    follow_b_chance       REAL NOT NULL DEFAULT 0.0 CHECK (follow_b_chance BETWEEN 0.0 AND 100.0),
    clip_fade             INTEGER NOT NULL DEFAULT 0 CHECK (clip_fade IN (0,1)),
    ram_mode              INTEGER NOT NULL DEFAULT 0 CHECK (ram_mode IN (0,1)),
    FOREIGN KEY (clip_id) REFERENCES clip_slots(clip_id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED,
    CHECK ((follow_a = 9) = (follow_a_scene_id IS NOT NULL)),
    CHECK ((follow_b = 9) = (follow_b_scene_id IS NOT NULL))
) STRICT;

-- A slotted clip is still the existing clip object, including its media,
-- note stream and timing domain. Membership in clip_slots is its address.
-- An absent parent is checked by the deferred foreign key, which permits
-- replay to install the slot first and validate when it inserts the clip.
CREATE TRIGGER clip_slots_validate_insert
BEFORE INSERT ON clip_slots
WHEN EXISTS (
    SELECT 1 FROM clips WHERE id = NEW.clip_id AND NOT (
        track_id = NEW.track_id AND lane_id IS NULL AND kind IN ('audio','midi') AND
        ((time_base = 0 AND pos_ticks IS 0 AND pos_ns IS NULL) OR
         (time_base = 1 AND pos_ns IS 0 AND (pos_ticks IS NULL OR pos_ticks IS 0)))
    )
)
BEGIN SELECT RAISE(ABORT, 'Session slot clip needs the same track, zero origin and no lane'); END;

CREATE TRIGGER clip_slots_validate_update
BEFORE UPDATE OF track_id, clip_id ON clip_slots
WHEN EXISTS (
    SELECT 1 FROM clips WHERE id = NEW.clip_id AND NOT (
        track_id = NEW.track_id AND lane_id IS NULL AND kind IN ('audio','midi') AND
        ((time_base = 0 AND pos_ticks IS 0 AND pos_ns IS NULL) OR
         (time_base = 1 AND pos_ns IS 0 AND (pos_ticks IS NULL OR pos_ticks IS 0)))
    )
)
BEGIN SELECT RAISE(ABORT, 'Session slot clip needs the same track, zero origin and no lane'); END;

CREATE TRIGGER clips_validate_session_insert
AFTER INSERT ON clips
WHEN EXISTS (
    SELECT 1 FROM clip_slots WHERE clip_id = NEW.id AND NOT (
        track_id = NEW.track_id AND NEW.lane_id IS NULL AND NEW.kind IN ('audio','midi') AND
        ((NEW.time_base = 0 AND NEW.pos_ticks IS 0 AND NEW.pos_ns IS NULL) OR
         (NEW.time_base = 1 AND NEW.pos_ns IS 0 AND (NEW.pos_ticks IS NULL OR NEW.pos_ticks IS 0)))
    )
)
BEGIN SELECT RAISE(ABORT, 'Session clip needs the slot track, zero origin and no lane'); END;

CREATE TRIGGER clips_validate_session_update
BEFORE UPDATE OF track_id, lane_id, kind, time_base, pos_ticks, pos_ns ON clips
WHEN EXISTS (
    SELECT 1 FROM clip_slots WHERE clip_id = OLD.id AND NOT (
        track_id = NEW.track_id AND NEW.lane_id IS NULL AND NEW.kind IN ('audio','midi') AND
        ((NEW.time_base = 0 AND NEW.pos_ticks IS 0 AND NEW.pos_ns IS NULL) OR
         (NEW.time_base = 1 AND NEW.pos_ns IS 0 AND (NEW.pos_ticks IS NULL OR NEW.pos_ticks IS 0)))
    )
)
BEGIN SELECT RAISE(ABORT, 'Session clip needs the slot track, zero origin and no lane'); END;

-- A scene owns the clips in its slots. Deleting a scene must never move
-- those clips to the Arrangement's zero point. Deleting a clip alone keeps
-- its slot's Stop state by the slot's SET NULL action.
CREATE TRIGGER scenes_delete_clips
BEFORE DELETE ON scenes
BEGIN
    DELETE FROM clips WHERE id IN (SELECT clip_id FROM clip_slots WHERE scene_id = OLD.id);
END;
