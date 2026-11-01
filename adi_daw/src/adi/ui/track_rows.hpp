// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/store_rows.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/track_tree.hpp"
#include <unordered_map>
#include <vector>
namespace adi::ui {
// The project view's rows (Master Reference 10, ADR-0112 d2): every track the snapshot shows, in
// its order and with its name, and from the project file's rows what it sits in and whether it is
// a group or Main.
//
// The director's choice of 29 October 2026: until the snapshot carries a track's parent, kind and
// colour (collab/window.md, N6), the window reads them from the store as the engine's MIDI player
// does, only when the project view's menu opens or a group is chosen. If the engine's way proves
// better, it replaces this.
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
        row.group = stored != nullptr && stored->kind == "group";
        row.main = stored != nullptr && stored->kind == "master";
    }
}
} // namespace adi::ui
