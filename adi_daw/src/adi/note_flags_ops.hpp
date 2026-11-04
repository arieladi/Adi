// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"

namespace adi::note_flags {
bool mute(OpContext&, const Payload&, std::string&);
bool muteInverse(OpContext&, const Payload&, Payload&, std::string&);
} // namespace adi::note_flags
