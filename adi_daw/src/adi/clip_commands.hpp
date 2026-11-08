// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <memory>

namespace adi::edit_plan {struct CapturedDescription;struct Options;}

namespace adi::clip_edit {
/// Input coordinate, never an implicit conversion: 0 musical ticks, 1 ns.
struct TimePoint {int base=0;std::int64_t value=0;};
struct PreparedEdit {
    bool ok=false;
    std::string error,sourceFile;
    std::vector<OpRequest> requests;
    CommitOptions options;
    /// Immutable preview facts, bound to the captured arrangement requests.
    std::shared_ptr<const edit_plan::CapturedDescription> description;
};
struct ClipCopy {
    bool ok=false;
    std::string error,sourceFile;
    Payload image;
};
struct ClipCut {ClipCopy copy;PreparedEdit edit;};
[[nodiscard]] ClipCut prepareCutClip(Store&,std::int64_t,double referenceRate);
/// These prepare on the Store owner thread, off the callback. A background
/// worker can use its own snapshot connection. Keep its document generation
/// with the result and commit on that same live project's owner thread.
[[nodiscard]] ClipCopy copyClip(Store&,std::int64_t,double referenceRate);
[[nodiscard]] PreparedEdit preparePaste(Store&,const ClipCopy&,std::int64_t newId,
                                        std::int64_t track,TimePoint,double referenceRate);
[[nodiscard]] PreparedEdit prepareDuplicate(Store&,std::int64_t sourceId,std::int64_t newId,
                                            std::int64_t track,TimePoint,double referenceRate);
/// Compatibility overloads above capture the default Live policy. Owners with
/// AppSettings pass Options{rate, edit_plan::capturePolicy(settings)} explicitly.
[[nodiscard]] PreparedEdit preparePaste(Store&,const ClipCopy&,std::int64_t newId,
    std::int64_t track,TimePoint,const edit_plan::Options&);
[[nodiscard]] PreparedEdit prepareDuplicate(Store&,std::int64_t sourceId,std::int64_t newId,
    std::int64_t track,TimePoint,const edit_plan::Options&);
[[nodiscard]] PreparedEdit prepareSplit(Store&,std::int64_t sourceId,std::int64_t rightId,
                                        TimePoint,double referenceRate);
/// Shift the Set and its global envelopes, splitting material at the boundary.
/// Musical length; linear objects move by the held tempo immediately before it.
[[nodiscard]] PreparedEdit prepareInsertSilence(Store&,std::int64_t atTicks,
                                               std::int64_t ticks,double referenceRate);
/// Remove time across the Set, preserving the old context at the join.
[[nodiscard]] PreparedEdit prepareDeleteTime(Store&,std::int64_t beginTicks,
                                            std::int64_t endTicks,double referenceRate);
struct RangeSelection {
    std::vector<std::int64_t> tracks; // source order; paste supplies matching destinations
    TimePoint begin,end;
};
struct RangeCopy {
    bool ok=false;
    std::string error,sourceFile;
    RangeSelection selection;
    std::vector<Payload> images; // clipped material, offsets relative to selection
};
struct RangeCut {RangeCopy copy;PreparedEdit edit;};
/// Ordinary material ranges select only main-lane clips (NULL lane_id).
/// A sole surviving side keeps its clip and material-child identities.
/// insertTime shifts the whole Set but pastes the selected main-lane material;
/// the dedicated Time APIs below capture and shift clips from every lane.
[[nodiscard]] RangeCut prepareCutRange(Store&,const RangeSelection&,double referenceRate);
[[nodiscard]] RangeCut prepareCutRange(Store&,const RangeSelection&,const edit_plan::Options&);
[[nodiscard]] RangeCopy copyRange(Store&,const RangeSelection&,double referenceRate);
[[nodiscard]] PreparedEdit prepareDeleteRange(Store&,const RangeSelection&,double referenceRate,std::int64_t reserveClipId=0);
[[nodiscard]] PreparedEdit preparePasteRange(Store&,const RangeCopy&,const std::vector<std::int64_t>& tracks,
                                            TimePoint at,double referenceRate,bool insertTime=false);
[[nodiscard]] PreparedEdit prepareDuplicateRange(Store&,const RangeSelection&,double referenceRate,bool insertTime=false);
[[nodiscard]] PreparedEdit prepareDeleteRange(Store&,const RangeSelection&,const edit_plan::Options&,std::int64_t reserveClipId=0);
[[nodiscard]] PreparedEdit preparePasteRange(Store&,const RangeCopy&,const std::vector<std::int64_t>& tracks,
    TimePoint at,const edit_plan::Options&);
[[nodiscard]] PreparedEdit prepareDuplicateRange(Store&,const RangeSelection&,const edit_plan::Options&);
/// Whole-Set clipboard, including global maps and automation. Owner-thread
/// value; its media/tuning/lane identities are rechecked during preparation.
struct TimeCopy {
    bool ok=false;
    std::string error;
    RangeCopy material;
    std::int64_t ticks=0,nanos=0;
    double referenceRate=0;
    Payload timeline;
};
struct TimeCut {TimeCopy copy;PreparedEdit edit;};
[[nodiscard]] TimeCopy copyTime(Store&,std::int64_t beginTicks,std::int64_t endTicks,double referenceRate);
[[nodiscard]] TimeCut prepareCutTime(Store&,std::int64_t beginTicks,std::int64_t endTicks,double referenceRate);
[[nodiscard]] PreparedEdit preparePasteTime(Store&,const TimeCopy&,std::int64_t atTicks,double referenceRate);
[[nodiscard]] PreparedEdit prepareDuplicateTime(Store&,std::int64_t beginTicks,std::int64_t endTicks,double referenceRate);
/// Preserves ADR-0153's expected head, including a captured null head.
[[nodiscard]] CommitResult commitPrepared(Store&,const PreparedEdit&,Actor=Actor::User);
} // namespace adi::clip_edit
