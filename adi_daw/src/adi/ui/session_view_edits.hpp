// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Session View's edits that need the project's rows (TM-SV-05): a clip copied into a slot
// (Duplicate, Paste, a Ctrl-drag, a drag to another track, a duplicated scene) is the clip's whole
// image under a new identity, so the commands name it (`SlotCopy`) and this side builds it.
//
// On the Store's owner thread, as the window's other prepared edits are; commit the result with
// `clip_edit::commitPrepared`: one batch, one step of Undo.
#pragma once

#include "adi/clip_commands.hpp"
#include "adi/ui/session_view_commands.hpp"

#include <string>

namespace adi {
class Store;
}

namespace adi::ui::session_view {

/// Takes the images and launch settings of the clipboard's clips into `board.images`, so a Paste
/// after a Cut (whose clips are gone) still has them. Call it before the Cut commits.
[[nodiscard]] bool snapshotClipboard(Store&, Clipboard& board, std::string& error);

/// The outcome's `edits` with each of its `copies` in place: the clip's copy (`clip.duplicate`),
/// its slot (`session.setSlotClip`), its Stop button where the copy keeps one off, and its launch
/// settings (mode, quantization, Legato, velocity, Fade, RAM, Follow Action). A copy takes its
/// clip from `board`'s images where they hold it, else from the project. New clip and row
/// identities come from the project, one past the largest.
[[nodiscard]] clip_edit::PreparedEdit prepareEdit(Store&, const Outcome&, const Clipboard* board = nullptr);

} // namespace adi::ui::session_view
