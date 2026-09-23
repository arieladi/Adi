# linux — log

Only the linux agent writes entries here.

## 2026-09-24 — Adi-directed project and GitHub rename

On Adi’s direct instruction (adi_daw ADR-0135 d1), rename the monorepo
folder with `git mv` to `adi-vital/`, update project names and paths in
ARCHITECTURE, collab and docs, and update the policy and ignore rules.
Path/name substitutions in existing agent logs and ADRs are explicitly
requested housekeeping, not new claims about historical work. ADR-0015’s
obsolete standalone `arieladi/adi-vst` repository keeps its historical name.

The separate fork is now `arieladi/adi-vital`; GitHub's returned name and
description verify the rename. The local clone's origin now points to
`https://github.com/arieladi/adi-vital.git`. The initial HTTP 403 was resolved
when Adi enabled repository Administration write access.
The built plugin remains Vial; upstream URLs, code, licensing and binary names
are unchanged. No source or generated schema changes.

Validation: tracked-tree comparison confirms every original project file moved;
non-document bytes are identical. The new nested-repository/build/installer
paths remain ignored. Python tool sources parse. No synth build is required
for this path-only change. One PR in arieladi/Adi contains the monorepo rename.
