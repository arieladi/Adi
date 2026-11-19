// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"

namespace adi::notes {
[[nodiscard]] std::span<const OpDescriptor> diffOps();
bool noteInsertApply(OpContext&,const Payload&,std::string&);
bool noteInsertInverse(OpContext&,const Payload&,Payload&,std::string&);
bool noteDeleteApply(OpContext&,const Payload&,std::string&);
bool noteDeleteInverse(OpContext&,const Payload&,Payload&,std::string&);
bool noteMoveApply(OpContext&,const Payload&,std::string&);
bool noteMoveInverse(OpContext&,const Payload&,Payload&,std::string&);
bool noteResizeApply(OpContext&,const Payload&,std::string&);
bool noteResizeInverse(OpContext&,const Payload&,Payload&,std::string&);
bool noteVelocityApply(OpContext&,const Payload&,std::string&);
bool noteVelocityInverse(OpContext&,const Payload&,Payload&,std::string&);
} // namespace adi::notes
