// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <memory>
#include <utility>

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
[[nodiscard]] PreparedEdit prepareSetSource(Store&,std::int64_t clipId,std::int64_t mediaId,
                                            std::int64_t frames,std::int64_t sampleRate,OpRequest capturedMedia={});
struct ClipCopy {
    bool ok=false;
    std::string error,sourceFile;
    Payload image;
    Payload envelopes; // null when copied with envelopes locked or from a take lane
};
struct ClipCut {ClipCopy copy;PreparedEdit edit;};
[[nodiscard]] ClipCut prepareCutClip(Store&,std::int64_t,double referenceRate);
[[nodiscard]] ClipCut prepareCutClip(Store&,std::int64_t,const edit_plan::Options&);
/// These prepare on the Store owner thread, off the callback. A background
/// worker can use its own snapshot connection. Keep its document generation
/// with the result and commit on that same live project's owner thread.
[[nodiscard]] ClipCopy copyClip(Store&,std::int64_t,double referenceRate);
[[nodiscard]] ClipCopy copyClip(Store&,std::int64_t,const edit_plan::Options&);
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
/// Several clips copied at once, each to its own track and point (a clip drag
/// with the copy modifier): one captured batch, one Undo. newIds must be
/// distinct unused clip identities.
struct ClipPlacement {std::int64_t sourceId=0,newId=0,track=0;TimePoint point;};
[[nodiscard]] PreparedEdit prepareDuplicates(Store&,std::span<const ClipPlacement>,const edit_plan::Options&);
[[nodiscard]] PreparedEdit prepareSplit(Store&,std::int64_t sourceId,std::int64_t rightId,
                                        TimePoint,double referenceRate);
/// Two ordered cuts strictly inside one clip, in one explicit input clock.
/// The left piece keeps sourceId; middleId and rightId must be distinct unused
/// clip identities. Both cuts are one captured operation/undo step. The caller
/// may append clip.setMute for middleId before the single commitPrepared call.
[[nodiscard]] PreparedEdit prepareSplit(Store&,std::int64_t sourceId,
                                        std::int64_t middleId,std::int64_t rightId,
                                        TimePoint begin,TimePoint end,double referenceRate);
struct ClipSplit {
    std::int64_t clipId=0;
    TimePoint first;
    std::optional<TimePoint> second;
};
struct SplitRefusal {std::int64_t clipId=0;std::string error;};
struct PreparedSplits {
    PreparedEdit edit;
    std::vector<SplitRefusal> refused;
};
/// Allocate all new clip and child identities against one snapshot. Each clip
/// can supply one or two cuts. Commit edit once for one Undo of all valid cuts.
[[nodiscard]] PreparedSplits prepareSplits(Store&,std::span<const ClipSplit>,double referenceRate);
/// Reverse the selected audio, splitting at the optional timeline range edges.
/// All selected clips and new pieces commit as one undoable arrangement edit.
[[nodiscard]] PreparedEdit prepareReverse(Store&,std::span<const std::int64_t> clipIds,
    std::optional<std::pair<std::int64_t,std::int64_t>> range,double referenceRate);
/// Shift the Set and its global envelopes, splitting material at the boundary.
/// Musical length; linear objects move by the held tempo immediately before it.
[[nodiscard]] PreparedEdit prepareInsertSilence(Store&,std::int64_t atTicks,
                                               std::int64_t ticks,double referenceRate);
/// Insert the missing time at the start of a short bar, keeping its opening meter marker in place.
[[nodiscard]] PreparedEdit prepareCompleteFragmentaryBar(Store&,std::int64_t atTicks,
                                                        std::int64_t missingTicks,double referenceRate);
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
    std::vector<Payload> envelopes; // one snapshot per selected track for a time range
    std::vector<Payload> clipEnvelopes; // one snapshot per explicitly picked clip image
    bool clearsRange=true; // explicit clips clear only where their images land
};
struct RangeCut {RangeCopy copy;PreparedEdit edit;};
struct MidiConsolidation {
    PreparedEdit edit;
    std::int64_t clip = 0;
};
/// A single MIDI track with wholly contained, single-pass material and unique note IDs.
/// With picked clips, every clip overlapped by the output must be in that pick.
[[nodiscard]] MidiConsolidation prepareMidiConsolidation(Store&,const RangeSelection&,
    std::span<const std::int64_t> picked,const edit_plan::Options&);
/// Ordinary material ranges select only main-lane clips (NULL lane_id).
/// A sole surviving side keeps its clip and material-child identities.
/// insertTime shifts the whole Set but pastes the selected main-lane material;
/// the dedicated Time APIs below capture and shift clips from every lane.
[[nodiscard]] RangeCut prepareCutRange(Store&,const RangeSelection&,double referenceRate);
[[nodiscard]] RangeCut prepareCutRange(Store&,const RangeSelection&,const edit_plan::Options&);
[[nodiscard]] RangeCopy copyRange(Store&,const RangeSelection&,double referenceRate);
[[nodiscard]] RangeCopy copyRange(Store&,const RangeSelection&,const edit_plan::Options&);
[[nodiscard]] RangeCopy copySelectedClips(Store&,const RangeSelection&,std::span<const std::int64_t>,const edit_plan::Options&);
[[nodiscard]] RangeCut prepareCutClips(Store&,const RangeSelection&,std::span<const std::int64_t>,const edit_plan::Options&);
[[nodiscard]] PreparedEdit prepareDeleteRange(Store&,const RangeSelection&,double referenceRate,std::int64_t reserveClipId=0);
[[nodiscard]] PreparedEdit preparePasteRange(Store&,const RangeCopy&,const std::vector<std::int64_t>& tracks,
                                            TimePoint at,double referenceRate,bool insertTime=false);
[[nodiscard]] PreparedEdit prepareDuplicateRange(Store&,const RangeSelection&,double referenceRate,bool insertTime=false);
[[nodiscard]] PreparedEdit prepareDeleteRange(Store&,const RangeSelection&,const edit_plan::Options&,std::int64_t reserveClipId=0);
/// Capture the existing Arrangement material covered by a new audio clip.
[[nodiscard]] PreparedEdit prepareAudioDropReplacement(Store&,std::int64_t track,TimePoint begin,TimePoint end,
    const edit_plan::Options&,std::int64_t lastIncomingId);
[[nodiscard]] PreparedEdit preparePasteRange(Store&,const RangeCopy&,const std::vector<std::int64_t>& tracks,
    TimePoint at,const edit_plan::Options&);
[[nodiscard]] PreparedEdit prepareDuplicateRange(Store&,const RangeSelection&,const edit_plan::Options&);
[[nodiscard]] PreparedEdit prepareMoveRange(Store&,const RangeSelection&,TimePoint destination,const edit_plan::Options&);
/// Splice the selected portion of a take onto its track's main lane.
[[nodiscard]] PreparedEdit prepareTakeToMain(Store&, std::int64_t track, std::int64_t lane,
                                             TimePoint begin, TimePoint end, double referenceRate);
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
