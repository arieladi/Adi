# New's default returns (N41)

Store::createNewProject now creates two MIDI tracks, two audio tracks, A Reverb,
B Delay, then Main. Track ids 2 to 5 remain the regular tracks, 6 and 7 are the
returns, and Main keeps id 1. Their root positions are 0 to 6. Every row has a
mixer strip; every regular track has an enabled Post send to each return at the
mixer's -150 dB minus-infinity floor. Return-to-return feedback sends are absent.
Implicit parent/Main output routing is unchanged.

A Reverb and B Delay each have a root device chain and an internal built-in
reference, org.adidaw.builtin.reverb and org.adidaw.builtin.delay. Their saved Mix
controls are 100 percent wet (canonical parameter ids 31 and 25 respectively),
with other controls at their native defaults. The native host/catalogue is N15,
engine/library-devices, the base of this branch. That branch must land first.
Headless sessions can compose withBuiltinDevices(), and the JUCE loader already
uses it for internal references. A build without these native devices preserves
the project identities/controls through its ordinary missing-device path.

All rows are initialized in the same root transaction, with no journal ops or
special UI undo protection. The first real edit is undoable; New itself remains
the root and survives reopen, undo/redo, forks and snapshots. Store::create and
createEmpty retain their lower-level meanings. The window continues calling the
same createNewProject factory and reads the returns/sends/devices through existing
model/snapshot APIs. New tracks must reserve ids above all current tracks, rather
than assuming that 6 is unused.

No schema, opcode or UI production change. N31's New overload can keep capturing
its recording choices for the four regular tracks (ids 2 to 5) when win combines
the independent changes. Native effect preset/render parity against Live remains
the existing devices/parity lane; this factory uses ADI's compiled native defaults
with wet-only return routing.
