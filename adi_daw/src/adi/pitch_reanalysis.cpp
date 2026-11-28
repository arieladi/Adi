// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_reanalysis.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <exception>
#include <new>
#include <utility>

namespace adi::clip_edit {
namespace {

PitchReanalysisResult failure(media::ClipPitchProblem problem) {
    PitchReanalysisResult result;
    result.rebuilt.problem = problem;
    return result;
}

} // namespace

PitchReanalysisResult preparePitchReanalysis(const Store& store, std::int64_t clipId,
                                             PitchReanalysisPolicy policy, std::stop_token stop,
                                             audio::DecodeCache* decode) {
    if (audio::onAudioThread) return failure(media::ClipPitchProblem::Realtime);
    if (stop.stop_requested()) return failure(media::ClipPitchProblem::Cancelled);
    PitchReanalysisResult result;
    if (policy != PitchReanalysisPolicy::PreserveCompatible && policy != PitchReanalysisPolicy::ResetAuthored) {
        result.error = "Unknown pitch reanalysis policy";
        return result;
    }

    try {
        auto& database = const_cast<SQLite::Database&>(store.db());
        std::optional<SQLite::Transaction> snapshot;
        if (sqlite3_get_autocommit(database.getHandle()) != 0)
            snapshot.emplace(database, SQLite::TransactionBehavior::DEFERRED);
        result.rebuilt = media::buildClipPitchAnalysis(store, clipId, stop, decode, nullptr);
        if (!result.rebuilt) return result;

        auto saved = readClipPitchEdits(store, clipId);
        if (stop.stop_requested()) return failure(media::ClipPitchProblem::Cancelled);
        if (!saved.error.empty()) {
            result.error = std::move(saved.error);
            return result;
        }
        if (saved.edits && policy == PitchReanalysisPolicy::PreserveCompatible) {
            result.kept = resolveClipPitchEdits(*saved.edits, *result.rebuilt.data);
        } else if (saved.edits) {
            auto request = prepareClipPitchEdits(*result.rebuilt.data, {});
            if (!request.request) {
                result.error = std::move(request.error);
                return result;
            }
            SQLite::Statement path(database, "SELECT file FROM pragma_database_list WHERE name='main'");
            if (!path.executeStep()) {
                result.error = "Pitch reanalysis needs a project connection";
                return result;
            }
            auto& clear = result.clear.emplace();
            clear.sourceFile = path.getColumn(0).getString();
            clear.options.expectHead.emplace(OpJournal(const_cast<Store&>(store)).headSeq());
            request.request->label = "Reanalyze clip pitch";
            clear.requests.push_back(std::move(*request.request));
            clear.ok = true;
        }
        if (stop.stop_requested()) return failure(media::ClipPitchProblem::Cancelled);
        if (snapshot) snapshot->commit();
        return result;
    } catch (const std::bad_alloc&) {
        return failure(media::ClipPitchProblem::Resource);
    } catch (const std::exception& error) {
        result.kept.reset();
        result.clear.reset();
        result.error = error.what();
        return result;
    }
}

} // namespace adi::clip_edit
