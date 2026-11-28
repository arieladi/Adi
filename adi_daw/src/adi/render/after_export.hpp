// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/material_commands.hpp"

namespace adi::render {

/// Explicit post-export action on the destination Store's owner thread.
/// The caller supplies the target track, IDs and insertion point. The
/// delivery result supplies the source path and its verified content hash.
[[nodiscard]] CommitResult importAfterExport(Store&, const Result&, clip_edit::ImportOptions,
                                               double referenceRate, Actor = Actor::User);
[[nodiscard]] CommitResult importAfterExport(Store&, const DeliveryResult&, clip_edit::ImportOptions,
                                               double referenceRate, Actor = Actor::User);

} // namespace adi::render
