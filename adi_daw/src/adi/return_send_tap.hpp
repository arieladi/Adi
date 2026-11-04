// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <map>
namespace adi {
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
struct PreparedReturnSend {
    bool ok=false;std::string error,sourceFile;
    std::vector<OpRequest> requests;CommitOptions options;
};
// Literal caller ids, one undo group; snapshot/head/file guards remain explicit.
PreparedReturnSend prepareSendToReturn(Store&,std::int64_t routingId,std::int64_t sourceTrack,std::int64_t returnTrack);
CommitResult commitSendToReturn(Store&,const PreparedReturnSend&,Actor=Actor::User);
}
