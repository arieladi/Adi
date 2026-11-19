// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace adi { class Store; }
namespace adi::media {
struct ArchiveControl;
struct FileResult;
namespace detail {
// Archive worker only: an open, private, writable snapshot whose owner does
// not close/mutate/reenter its Store from a progress observer. Ordinary Collect
// All and Save retains the public Store API and its unobserved copy path.
FileResult collectSnapshotMedia(Store&, const ArchiveControl&);
}
}
