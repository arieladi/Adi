// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/store_rows.hpp"

#include "adi/blob.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

namespace adi::rows {
namespace {

// --- column helpers ---------------------------------------------------------
//
// Every one of these exists because the alternative is `st.getColumn(4).isNull()
// ? std::nullopt : std::optional(st.getColumn(4).getInt64())`, written eighty
// times, where getting the index wrong in the second half of the expression is
// invisible. Naming the column once per read is the point.

std::optional<std::int64_t> optInt(const SQLite::Statement& st, int i) {
    if (st.getColumn(i).isNull()) return std::nullopt;
    return st.getColumn(i).getInt64();
}

std::optional<std::string> optText(const SQLite::Statement& st, int i) {
    if (st.getColumn(i).isNull()) return std::nullopt;
    return st.getColumn(i).getString();
}

bool flag(const SQLite::Statement& st, int i) { return st.getColumn(i).getInt() != 0; }

/// Run one query, feeding each row to `onRow`. Returns false and names the
/// table in `m.problems` if anything threw.
///
/// The catch is the whole reason this is a function. Every table is read the
/// same way and every table can fail the same way -- a newer schema that
/// renamed a column, a file opened read-only mid-checkpoint -- and a projection
/// that throws out of the middle leaves the caller holding a half-built Model
/// with no way to tell which half.
template <typename Fn>
bool query(Model& m, const SQLite::Database& db, const char* table, const char* sql,
           Fn onRow) {
    try {
        SQLite::Statement st(const_cast<SQLite::Database&>(db), sql);
        while (st.executeStep()) onRow(st);
        return true;
    } catch (const std::exception& e) {
        m.problems.push_back(std::string(table) + ": " + e.what());
        return false;
    }
}

// --- notes ------------------------------------------------------------------

/// Decode one clip's `ANOT` blob into notes.
///
/// `at()` rather than `all()`: a clip's note count comes from the file, and
/// `all()` refuses above 8.4M records rather than allocating on a corrupt
/// header's word. Striding by the header's `rec_size` is what makes a v2 record
/// readable by this v1 build (ADR-0023) -- never `sizeof(NoteRecord)`.
void readNotesFor(Model& m, const Store& store, std::int64_t clipId) {
    const auto blob = store.getEventStream(clipId, "notes");
    if (!blob) return;

    const StreamReader<NoteRecord> r(*blob, FourCC::Notes);
    if (!r.ok()) {
        m.problems.push_back("clips#" + std::to_string(clipId) +
                             ": note stream is not readable (" +
                             std::string(toString(r.error())) + ")");
        return;
    }

    for (std::uint32_t i = 0; i < r.count(); ++i) {
        const auto rec = r.at(i);
        if (!rec) break;
        Note n;
        n.clipId = clipId;
        n.startTicks = rec->start_ticks;
        n.durTicks = rec->dur_ticks;
        n.key = rec->key;
        n.velOn = rec->vel_on;
        n.velOff = rec->vel_off;
        n.channel = rec->channel;
        n.probability = rec->probability;
        n.tuningCents = static_cast<double>(rec->tuning_cents);
        n.flags = rec->flags;
        m.notes.push_back(n);
    }
}

}  // namespace

Model readModel(const Store& store) {
    Model m;
    const SQLite::Database& db = store.db();

    // One read transaction around the whole read. Without it a writer on
    // another connection can commit between two of these queries and the
    // projection describes a project that never existed -- tracks from before
    // an edit, clips from after. SQLite gives us this for free in WAL mode; it
    // is not free if nobody asks for it.
    std::optional<SQLite::Transaction> txn;
    try {
        txn.emplace(const_cast<SQLite::Database&>(db), SQLite::TransactionBehavior::DEFERRED);
    } catch (const std::exception& e) {
        m.problems.push_back(std::string("read transaction: ") + e.what());
    }

    query(m, db, "project",
          "SELECT name, sample_rate, ppq, length_ticks, author, notes "
          "FROM project WHERE id = 1",
          [&](const SQLite::Statement& st) {
              m.project.name = st.getColumn(0).getString();
              m.project.sampleRate = st.getColumn(1).getInt64();
              m.project.ppq = st.getColumn(2).getInt64();
              m.project.lengthTicks = st.getColumn(3).getInt64();
              m.project.author = st.getColumn(4).getString();
              m.project.notes = st.getColumn(5).getString();
          });

    query(m, db, "tempo_map",
          "SELECT pos_ticks, bpm, curve, tension FROM tempo_map ORDER BY pos_ticks, id",
          [&](const SQLite::Statement& st) {
              TempoEvent e;
              e.posTicks = st.getColumn(0).getInt64();
              e.bpm = st.getColumn(1).getDouble();
              e.curve = st.getColumn(2).getInt64();
              e.tension = st.getColumn(3).getDouble();
              m.tempo.push_back(e);
          });

    query(m, db, "time_signature_map",
          "SELECT pos_ticks, numerator, denominator FROM time_signature_map "
          "ORDER BY pos_ticks, id",
          [&](const SQLite::Statement& st) {
              TimeSignature s;
              s.posTicks = st.getColumn(0).getInt64();
              s.numerator = st.getColumn(1).getInt64();
              s.denominator = st.getColumn(2).getInt64();
              m.signatures.push_back(s);
          });

    query(m, db, "tracks",
          "SELECT id, parent_id, index_in_parent, kind, name, color, time_base, "
          "       frozen, locked, muted, soloed, solo_defeat, record_armed, "
          "       monitor_mode, input_ref, automation_mode, freeze_media_id "
          "FROM tracks",
          [&](const SQLite::Statement& st) {
              Track t;
              t.id = st.getColumn(0).getInt64();
              t.parentId = optInt(st, 1);
              t.indexInParent = st.getColumn(2).getInt64();
              t.kind = st.getColumn(3).getString();
              t.name = st.getColumn(4).getString();
              t.color = optInt(st, 5);
              t.timeBase = st.getColumn(6).getInt64();
              t.frozen = flag(st, 7);
              t.locked = flag(st, 8);
              t.muted = flag(st, 9);
              t.soloed = flag(st, 10);
              t.soloDefeat = flag(st, 11);
              t.recordArmed = flag(st, 12);
              t.monitorMode = st.getColumn(13).getInt64();
              t.inputRef = optText(st, 14);
              t.automationMode = st.getColumn(15).getInt64();
              t.freezeMediaId = optInt(st, 16);
              m.tracks.push_back(std::move(t));
          });

    query(m, db, "mixer_strip",
          "SELECT track_id, volume_db, pan, pan_law, width, input_gain_db, "
          "       phase_invert, delay_samples, vca_group_id FROM mixer_strip",
          [&](const SQLite::Statement& st) {
              MixerStrip s;
              s.trackId = st.getColumn(0).getInt64();
              s.volumeDb = st.getColumn(1).getDouble();
              s.pan = st.getColumn(2).getDouble();
              s.panLaw = st.getColumn(3).getInt64();
              s.width = st.getColumn(4).getDouble();
              s.inputGainDb = st.getColumn(5).getDouble();
              s.phaseInvert = flag(st, 6);
              s.delaySamples = st.getColumn(7).getInt64();
              s.vcaGroupId = optInt(st, 8);
              m.strips.push_back(s);
          });

    query(m, db, "lanes",
          "SELECT id, track_id, ord, name, kind, muted, is_comp_target FROM lanes",
          [&](const SQLite::Statement& st) {
              Lane l;
              l.id = st.getColumn(0).getInt64();
              l.trackId = st.getColumn(1).getInt64();
              l.ord = st.getColumn(2).getInt64();
              l.name = st.getColumn(3).getString();
              l.kind = st.getColumn(4).getString();
              l.muted = flag(st, 5);
              l.isCompTarget = flag(st, 6);
              m.lanes.push_back(std::move(l));
          });

    query(m, db, "clips",
          "SELECT id, track_id, lane_id, kind, name, color, time_base, pos_ticks, "
          "       pos_ns, length_ticks, length_ns, loop_enabled, loop_start_ticks, "
          "       loop_len_ticks, content_offset_ticks, muted, gain_db, "
          "       fade_in_ticks, fade_out_ticks, fade_in_curve, fade_out_curve, "
          "       alias_of "
          "FROM clips",
          [&](const SQLite::Statement& st) {
              Clip c;
              c.id = st.getColumn(0).getInt64();
              c.trackId = st.getColumn(1).getInt64();
              c.laneId = optInt(st, 2);
              c.kind = st.getColumn(3).getString();
              c.name = st.getColumn(4).getString();
              c.color = optInt(st, 5);
              c.timeBase = st.getColumn(6).getInt64();
              c.posTicks = optInt(st, 7);
              c.posNs = optInt(st, 8);
              c.lengthTicks = optInt(st, 9);
              c.lengthNs = optInt(st, 10);
              c.loopEnabled = flag(st, 11);
              c.loopStartTicks = st.getColumn(12).getInt64();
              c.loopLenTicks = optInt(st, 13);
              c.contentOffsetTicks = st.getColumn(14).getInt64();
              c.muted = flag(st, 15);
              c.gainDb = st.getColumn(16).getDouble();
              c.fadeInTicks = st.getColumn(17).getInt64();
              c.fadeOutTicks = st.getColumn(18).getInt64();
              c.fadeInCurve = st.getColumn(19).getInt64();
              c.fadeOutCurve = st.getColumn(20).getInt64();
              c.aliasOf = optInt(st, 21);
              m.clips.push_back(std::move(c));
          });

    query(m, db, "audio_clips",
          "SELECT clip_id, media_id, src_start_frames, src_len_frames, warp_enabled, "
          "       warp_mode, transpose_semis, formant_shift, reverse, channel_mode "
          "FROM audio_clips",
          [&](const SQLite::Statement& st) {
              AudioClip a;
              a.clipId = st.getColumn(0).getInt64();
              a.mediaId = st.getColumn(1).getInt64();
              a.srcStartFrames = st.getColumn(2).getInt64();
              a.srcLenFrames = st.getColumn(3).getInt64();
              a.warpEnabled = flag(st, 4);
              a.warpMode = st.getColumn(5).getString();
              a.transposeSemis = st.getColumn(6).getDouble();
              a.formantShift = st.getColumn(7).getDouble();
              a.reverse = flag(st, 8);
              a.channelMode = st.getColumn(9).getInt64();
              m.audioClips.push_back(std::move(a));
          });

    query(m, db, "markers",
          "SELECT id, time_base, pos_ticks, pos_ns, length_ticks, length_ns, "
          "       name, color, kind FROM markers",
          [&](const SQLite::Statement& st) {
              Marker k;
              k.id = st.getColumn(0).getInt64();
              k.timeBase = st.getColumn(1).getInt64();
              k.posTicks = optInt(st, 2);
              k.posNs = optInt(st, 3);
              k.lengthTicks = optInt(st, 4);
              k.lengthNs = optInt(st, 5);
              k.name = st.getColumn(6).getString();
              k.color = optInt(st, 7);
              k.kind = st.getColumn(8).getString();
              m.markers.push_back(std::move(k));
          });

    query(m, db, "media_files",
          "SELECT id, hash_blake3, orig_name, rel_path, sample_rate, channels, "
          "       frames, format, bit_depth, embedded, missing FROM media_files",
          [&](const SQLite::Statement& st) {
              Media f;
              f.id = st.getColumn(0).getInt64();
              f.hashBlake3 = st.getColumn(1).getString();
              f.origName = st.getColumn(2).getString();
              f.relPath = optText(st, 3);
              f.sampleRate = optInt(st, 4);
              f.channels = optInt(st, 5);
              f.frames = optInt(st, 6);
              f.format = st.getColumn(7).getString();
              f.bitDepth = optInt(st, 8);
              f.embedded = flag(st, 9);
              f.missing = flag(st, 10);
              m.media.push_back(std::move(f));
          });

    query(m, db, "routing",
          "SELECT id, src_kind, src_id, dst_kind, dst_id, kind, ord, gain_db, "
          "       pan, pre_fader, enabled, origin FROM routing",
          [&](const SQLite::Statement& st) {
              Routing r;
              r.id = st.getColumn(0).getInt64();
              r.srcKind = st.getColumn(1).getString();
              r.srcId = st.getColumn(2).getInt64();
              r.dstKind = st.getColumn(3).getString();
              r.dstId = st.getColumn(4).getInt64();
              r.kind = st.getColumn(5).getString();
              r.ord = st.getColumn(6).getInt64();
              r.gainDb = st.getColumn(7).getDouble();
              r.pan = st.getColumn(8).getDouble();
              r.preFader = flag(st, 9);
              r.enabled = flag(st, 10);
              r.origin = st.getColumn(11).getString();
              m.routing.push_back(std::move(r));
          });

    // --- the device tables (ADR-0122) -----------------------------------
    //
    // Ordered where the consumer walks in order: chains by (track, ord, id),
    // devices by (chain, ord, id). A session builds a track's chain by
    // walking these as read, so the ORDER BY is part of the contract, not a
    // nicety -- see `Session::chainFor`.
    query(m, db, "plugin_refs",
          "SELECT id, format, uid, vendor, name, version, subtype, path_hint, "
          "       is_shell, shell_id FROM plugin_refs ORDER BY id",
          [&](const SQLite::Statement& st) {
              PluginRef r;
              r.id = st.getColumn(0).getInt64();
              r.format = st.getColumn(1).getString();
              r.uid = st.getColumn(2).getString();
              r.vendor = st.getColumn(3).getString();
              r.name = st.getColumn(4).getString();
              r.version = st.getColumn(5).getString();
              r.subtype = st.getColumn(6).getString();
              r.pathHint = st.getColumn(7).getString();
              r.isShell = flag(st, 8);
              r.shellId = optInt(st, 9);
              m.pluginRefs.push_back(std::move(r));
          });

    query(m, db, "device_chains",
          "SELECT id, parent_device_id, track_id, ord, name, muted, soloed "
          "FROM device_chains ORDER BY track_id, parent_device_id, ord, id",
          [&](const SQLite::Statement& st) {
              DeviceChain c;
              c.id = st.getColumn(0).getInt64();
              c.parentDeviceId = optInt(st, 1);
              c.trackId = optInt(st, 2);
              c.ord = st.getColumn(3).getInt64();
              c.name = st.getColumn(4).getString();
              c.muted = flag(st, 5);
              c.soloed = flag(st, 6);
              m.deviceChains.push_back(std::move(c));
          });

    query(m, db, "devices",
          "SELECT id, chain_id, ord, plugin_ref_id, name, enabled, is_rack, "
          "       rack_kind, preset_name, latency_samples, remote_host_id, "
          "       always_process, missing FROM devices ORDER BY chain_id, ord, id",
          [&](const SQLite::Statement& st) {
              Device d;
              d.id = st.getColumn(0).getInt64();
              d.chainId = st.getColumn(1).getInt64();
              d.ord = st.getColumn(2).getInt64();
              d.pluginRefId = optInt(st, 3);
              d.name = st.getColumn(4).getString();
              d.enabled = flag(st, 5);
              d.isRack = flag(st, 6);
              d.rackKind = optText(st, 7);
              d.presetName = st.getColumn(8).getString();
              d.latencySamples = st.getColumn(9).getInt64();
              d.remoteHostId = optInt(st, 10);
              d.alwaysProcess = flag(st, 11);
              d.missing = flag(st, 12);
              m.devices.push_back(std::move(d));
          });

    query(m, db, "plugin_params",
          "SELECT device_id, param_id, name, normalized_value, real_value, "
          "       display, unit, flags FROM plugin_params ORDER BY device_id, param_id",
          [&](const SQLite::Statement& st) {
              PluginParam p;
              p.deviceId = st.getColumn(0).getInt64();
              p.paramId = st.getColumn(1).getString();
              p.name = st.getColumn(2).getString();
              p.normalized = st.getColumn(3).getDouble();
              if (!st.getColumn(4).isNull()) p.real = st.getColumn(4).getDouble();
              p.display = st.getColumn(5).getString();
              p.unit = st.getColumn(6).getString();
              p.flags = st.getColumn(7).getInt64();
              m.pluginParams.push_back(std::move(p));
          });

    query(m, db, "plugin_state",
          "SELECT device_id, stream_role, state_hash, format_hint "
          "FROM plugin_state ORDER BY device_id, stream_role",
          [&](const SQLite::Statement& st) {
              PluginState s;
              s.deviceId = st.getColumn(0).getInt64();
              s.role = st.getColumn(1).getString();
              s.hash = st.getColumn(2).getString();
              s.formatHint = st.getColumn(3).getString();
              m.pluginState.push_back(std::move(s));
          });

    // Notes come through the Store's blob accessors rather than a SELECT,
    // because ADR-0009's granularity rule lives there and this should not be a
    // second place that knows how an event stream is addressed.
    for (const Clip& c : m.clips)
        if (c.kind == "midi") readNotesFor(m, store, c.id);

    if (txn) {
        try {
            txn->commit();   // a read transaction: commit and rollback are the same
        } catch (const std::exception&) {
        }
    }
    return m;
}

}  // namespace adi::rows
