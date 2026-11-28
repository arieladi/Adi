// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/control/jobs.hpp"
#include "adi/media/separated_stems.hpp"

namespace adi::control {

/// Capture the specific source media window before starting inference.
/// Call on the project owner and pass the returned stop token to the worker.
[[nodiscard]] std::optional<std::stop_token> startStemJob(JobBook&, std::string_view id, Store&,
                                                          const media::SeparationResult& source);

/// On the owner, verify the same source window and use the existing stem
/// gateway. A conflict does not stage or publish any stem file.
[[nodiscard]] std::optional<JobInfo> finishStemJob(JobBook&, std::string_view id, Store&,
                                                   const media::SeparationResult&,
                                                   const media::SeparationImportOptions&, Actor);

}  // namespace adi::control
