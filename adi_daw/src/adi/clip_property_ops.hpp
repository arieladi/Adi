// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"

namespace adi::clip_properties {
bool start(OpContext&, const Payload&, std::string&);
bool end(OpContext&, const Payload&, std::string&);
bool loop(OpContext&, const Payload&, std::string&);
bool signature(OpContext&, const Payload&, std::string&);
bool scale(OpContext&, const Payload&, std::string&);
bool inverse(OpContext&, const Payload&, Payload&, std::string&);
} // namespace adi::clip_properties
