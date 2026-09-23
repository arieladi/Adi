# linux — log

Only the linux agent writes entries here.

## 2026-09-24 — Adi-directed project and GitHub rename

On Adi’s direct instruction (adi_daw ADR-0135 d1), rename the monorepo
folder with `git mv` to `adi-vital/`, update project names and paths in
ARCHITECTURE, collab and docs, and update the policy and ignore rules.
Path/name substitutions in existing agent logs and ADRs are explicitly
requested housekeeping, not new claims about historical work. ADR-0015’s
obsolete standalone `arieladi/adi-vst` repository keeps its historical name.

The requested separate-fork rename from `arieladi/adi-vst-synth` to
`arieladi/adi-vital` is blocked: GitHub PATCH returned HTTP 403,
"Resource not accessible by personal access token". The existing browser
session is signed out. The local clone’s origin is retained until the remote
rename succeeds. Documentation points to the intended final name; this PR
should remain draft until the GitHub rename and description update are verified.
The built plugin remains Vial; upstream URLs, code, licensing and binary names
are unchanged. No source or generated schema changes.

Validation: tracked-tree comparison confirms every original project file moved;
non-document bytes are identical. The new nested-repository/build/installer
paths remain ignored. Python tool sources parse. No synth build is required
for this path-only change. One PR in arieladi/Adi contains the monorepo rename.
