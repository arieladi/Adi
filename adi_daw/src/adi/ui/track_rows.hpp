// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/store_rows.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/track_tree.hpp"
#include <unordered_map>
#include <vector>
namespace adi::ui {
// The tracks' tree (Master Reference 10, ADR-0112 d2; Live 12 manual 18.3): every track the
// snapshot shows, in its order and with its name, and from the project file's rows what it sits
// in, where, and whether it is a group or Main.
//
// THE DIRECTOR'S CHOICE OF 29 OCTOBER 2026, AND IT IS A STOPGAP. Until the snapshot carries a
// track's parent, index_in_parent, kind and colour (collab/window.md, N6), the window reads them
// from the store as the engine's MIDI player does. When the snapshot carries them, this file goes:
// STEP-7-PLAN §2.2 lists what changes with it.
//
// The folder colours stay those the window draws by place (0 here), so a group's folder matches its
// track's header.
inline void trackRowsFrom(const SnapshotReader &reader, const rows::Model &model,
                          std::vector<TrackRow> &out) {
    std::unordered_map<std::int64_t, const rows::Track *> byId;
    byId.reserve(model.tracks.size());
    for (const auto &t : model.tracks)
        byId.emplace(t.id, &t);
    const auto &tracks = reader.tracks();
    out.resize(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        auto &row = out[i];
        row.id = tracks[i]->id;
        row.name = tracks[i]->name;
        row.colour = 0;
        const auto at = byId.find(row.id);
        const auto *stored = at == byId.end() ? nullptr : at->second;
        row.parent = stored != nullptr && stored->parentId ? *stored->parentId : 0;
        row.index = stored != nullptr ? stored->indexInParent : 0;
        row.group = stored != nullptr && stored->kind == "group";
        row.main = stored != nullptr && stored->kind == "master";
    }
}
// The rows for a new snapshot. The project file is read again (`read` gives its rows::Model)
// only when the tree may have changed: the snapshot's tracks are not the rows' (one came or
// went), or the project has groups (a track can move into, out of or within one, and the snapshot
// does not show it). A project without groups whose tracks stay the same reads nothing and
// allocates nothing; only the names follow the snapshot.
//
// The cost the stopgap carries, gone with N6: with groups, every edit reads the project file.
template <class Read>
void refreshTrackRows(const SnapshotReader &reader, Read &&read, std::vector<TrackRow> &rows) {
    const auto &tracks = reader.tracks();
    bool same = rows.size() == tracks.size();
    for (std::size_t i = 0; same && i < tracks.size(); ++i)
        same = rows[i].id == tracks[i]->id && !rows[i].group && rows[i].parent == 0;
    if (!same) {
        trackRowsFrom(reader, read(), rows);
        return;
    }
    for (std::size_t i = 0; i < tracks.size(); ++i)
        rows[i].name = tracks[i]->name;
}
} // namespace adi::ui
