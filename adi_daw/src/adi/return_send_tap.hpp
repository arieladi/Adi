// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <map>
#include <string_view>
namespace adi {
enum class IndividualSendTap { PreFx, PostFx, PostMixer };

std::map<std::int64_t, IndividualSendTap> readIndividualSendTaps(const SQLite::Database&);
std::string_view individualSendTapName(IndividualSendTap) noexcept;
void requireNewSendSourceTrack(const SQLite::Database&, std::int64_t source);
bool allowedSendToGroupedReturn(const SQLite::Database&, std::int64_t source, std::int64_t destination,
                                std::optional<std::int64_t> movedTrack = {},
                                std::optional<std::int64_t> newParent = {});
/// Append these after the cloned tracks in the same journal transaction.
/// The caller supplies the old-to-new track and routing IDs.
std::vector<OpRequest> copyGroupSendRequests(const Store&,
    const std::map<std::int64_t, std::int64_t>& trackIds,
    const std::map<std::int64_t, std::int64_t>& routingIds);

struct ReturnSendTap {
    bool preFader=false,explicitChoice=false,legacyMixed=false;
    friend bool operator==(const ReturnSendTap&,const ReturnSendTap&)=default;
};
// Owner-thread readers. An absent preference preserves legacy per-edge audio;
// the derived value helps a producer inherit a uniform legacy choice.
std::map<std::int64_t,ReturnSendTap> readReturnSendTaps(const Store&);
std::optional<bool> explicitReturnSendTap(SQLite::Database&,std::int64_t);
bool allowSendEdgeTapEdit(SQLite::Database&,std::int64_t,bool,std::string&);
std::span<const OpDescriptor> returnSendTapOps();
/// Set a mixed return to Post, or toggle an already uniform return.
/// One journal request covers the return and every individual send override.
[[nodiscard]] OpRequest clickReturnSendTap(std::int64_t id);
struct PreparedReturnSend {
    bool ok=false;std::string error,sourceFile;
    std::vector<OpRequest> requests;CommitOptions options;
};
// Literal caller ids, one undo group; snapshot/head/file guards remain explicit.
PreparedReturnSend prepareSendToReturn(Store&,std::int64_t routingId,std::int64_t sourceTrack,std::int64_t returnTrack);
CommitResult commitSendToReturn(Store&,const PreparedReturnSend&,Actor=Actor::User);
}
