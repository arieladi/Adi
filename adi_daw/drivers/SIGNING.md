# Signing the ADI virtual audio device — the SignPath Foundation application, prepared

SignPath Foundation signs qualifying open-source projects for free, through
SignPath's platform and its GitHub Actions connector. `VirtualDrivers/
Virtual-Audio-Driver`, on the same `sysvad` base as ours, has shipped
Foundation-signed kernel builds since July 2025, so the route is proven for
exactly this kind of driver (ADR-0118).

**The application is a form sent by email, reviewed by people. It is not a pull
request.** It carries the applicant's identity and accepts the Foundation's
terms for the project, so **Adi sends it**. This file exists so that on the day
the preconditions below hold, sending it is a copy-paste.

## Preconditions, from the Foundation's published conditions

Do not apply before every box is ticked; an application that fails them is
declined and costs the reviewers' time.

- [ ] The driver is **released in the form to be signed**: at least one tagged
      pre-release of the unsigned driver package exists on GitHub.
- [ ] A **GitHub Actions release workflow** builds the driver from source with
      the WDK and produces the package as a CI artefact. Only that workflow will
      sign; **local builds are never signed.**
- [ ] The **licence is OSI-approved**: `drivers/LICENSE` is MIT. (The repository
      is GPLv3; both are OSI licences.)
- [ ] The repository is **public and visibly maintained**: recent commits, an
      issue tracker, documentation (this directory's README, ADR-0106 to
      ADR-0119).
- [ ] **Two-factor authentication** is enabled on the GitHub account(s) that
      approve releases.
- [ ] A **release approver** is named (Adi), and the process for approving a
      signing request is written down (below).
- [ ] The build is **reproducible from the public repository**: no private
      dependencies, no vendored binaries; `sysvad` is fetched from
      `microsoft/Windows-driver-samples` at a pinned commit or copied in with
      its MIT header intact.

## Draft answers for the form

Fill in from these; keep them true on the day of sending.

| Field | Answer |
|---|---|
| Project name | ADI DAW — virtual audio device (`adi_daw/drivers/adi-virtual-audio`) |
| Project URL | `https://github.com/arieladi/Adi` (path `adi_daw/drivers/`) — or the graduated repository if ADR-0013 has happened by then |
| Licence | MIT (driver); the surrounding DAW is GPLv3 |
| What is signed | A Windows kernel-mode audio driver package (`.sys`, `.inf`, `.cat`) for x64 (and ARM64 if built) exposing two virtual endpoints, "ADI DAW Stream Output" and "ADI DAW Stream Input" |
| Why it needs signing | Windows 10 and 11 x64 load only signed kernel drivers; users must not enable test-signing mode |
| Build system | GitHub Actions, workflow `<name>.yml` in the repository, WDK on `windows-latest`; the artefact uploaded to SignPath is the unsigned package produced by that workflow and nothing else |
| Who approves signing requests | Adi Ariel (repository owner), 2FA enabled |
| Release process | A tag on `main` triggers the release workflow; the workflow submits the artefact to SignPath; the approver reviews the diff since the last signed release and approves; the signed package is attached to the GitHub release |
| Users | Musicians using ADI DAW who want the DAW's output as an input device in Zoom, Discord, OBS or Parsec without third-party virtual cables |
| Related projects | `VirtualDrivers/Virtual-Audio-Driver` (same base, Foundation-signed since 2025) — cited as precedent, not as our code |

## The signing policy this project commits to

1. Only the release workflow signs. No developer machine ever holds a
   certificate; test-signing is used locally and never distributed.
2. Every signing request is approved by a named person after reviewing the
   changes since the previous signed release.
3. The signed artefact is published only as a GitHub release asset next to the
   source tag it was built from.
4. The driver never phones home, never updates itself, and never touches any
   endpoint it did not create (ADR-0118 decision 3).

## Fallback

If the Foundation declines, the route is an EV certificate registered on a
Microsoft Partner Center account and attestation signing, whose cost is then
stated in the ADR log. The driver ships in the first release only once signed,
whichever route it took (ADR-0117 decision b).

## Sources

- SignPath Foundation conditions for open-source projects:
  https://signpath.org/terms.html
- SignPath's open-source programme: https://signpath.io/solutions/open-source-community
- Microsoft attestation signing: https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation
- Virtual-Audio-Driver releases (the precedent): https://github.com/VirtualDrivers/Virtual-Audio-Driver/releases
