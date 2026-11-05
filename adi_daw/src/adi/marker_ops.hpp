// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"

namespace adi::marker {
bool create(OpContext&, const Payload&, std::string&);
bool createInverse(OpContext&, const Payload&, Payload&, std::string&);
bool erase(OpContext&, const Payload&, std::string&);
bool eraseInverse(OpContext&, const Payload&, Payload&, std::string&);
bool move(OpContext&, const Payload&, std::string&);
bool moveInverse(OpContext&, const Payload&, Payload&, std::string&);
bool rename(OpContext&, const Payload&, std::string&);
bool renameInverse(OpContext&, const Payload&, Payload&, std::string&);
bool color(OpContext&, const Payload&, std::string&);
bool rowInverse(OpContext&, const Payload&, Payload&, std::string&);
bool resize(OpContext&, const Payload&, std::string&);
bool note(OpContext&, const Payload&, std::string&);
bool prompt(OpContext&, const Payload&, std::string&);
bool detailsInverse(OpContext&, const Payload&, Payload&, std::string&);
} // namespace adi::marker
