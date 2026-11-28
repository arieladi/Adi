# Windows shortcut discovery (DEV-25)

`scan::findCandidates` is the shared native finder used by both VST3 and CLAP.
On Windows it follows stored filesystem targets in `.lnk` files supplied as
explicit roots or found below configured search roots. A link to a module is
matched by the target's extension; a link to a directory searches that directory.
The link's own filename is not a plug-in identity. Arguments, working directory,
icon and launch settings are not executed or applied.

The resulting candidates remain sorted canonical UTF-8 paths. Overlapping roots,
direct links, directory links and ordinary OS junctions/symlinks use the same
canonical target and directory cycle checks. A matched bundle is emitted once;
the finder does not search inside it for nested candidates. Existing ordinary
root/directory-link behavior is retained. Link targets may leave the root, just
as an ordinary configured directory symlink can.

The Windows helper reads at most 1 MiB plus one detection byte from the `.lnk`
file, then gives a private memory stream to the system Shell Link object's
`IPersistStream::Load`. It retrieves `IShellLinkW::GetPath(SLGP_RAWPATH)` without
calling `Resolve`, `ShellExecute`, PIDL binding or shortcut persistence. No
shortcut file is updated. Advertised MSI installer links are refused. This is
filesystem discovery, not a general Shell shortcut launcher or installer.

Specific bounds and refusal behavior:

- A file over 1 MiB, a failed/incomplete read, malformed stream, failed COM
  operation, empty target or unsupported target is skipped. The existing
  vector-returning finder does not add per-shortcut diagnostics in this slice.
- The SDK documents MAX_PATH for GetPath. Raw targets at the last character
  boundary are refused to avoid accepting an ambiguous truncation: the supported
  raw target is at most 258 UTF-16 code units. Known environment variables are
  expanded with a separate 32,768-code-unit buffer bound including the terminator.
- Only an absolute filesystem path is accepted after expansion. A relative-only
  shortcut or shell namespace target is not guessed from the scanner's working
  directory. Portable relocation of such links remains separate work.
- A shortcut chain may contain at most 32 links. Repeating a canonical shortcut
  path terminates the chain. Directory cycles are independently stopped by the
  common walker's canonical visited set.
- A missing or moved target is skipped. A later explicit discovery can observe
  a newly available target; no cross-call negative shortcut cache is introduced.
- COM initialization is scoped to the calling owner thread. Existing STA/MTA
  ownership is respected; every successful initialization, including S_FALSE,
  is balanced. Audio-callback calls refuse before file/COM work.

There is no Shell target-recovery or network search. An explicitly stored UNC
path, like an explicit network scan root or junction target, still entails
ordinary filesystem access if followed. This slice adds neither a network
policy nor an OS-filesystem deadline; it does not claim that every filesystem
operation is bounded in wall time. It also does not impose a new total-directory
or total-entry cap on the existing walker. Finder aliases are not implemented.

`adi_scan_shortcut_tests` authors actual `.lnk` files using the Windows SDK and
points them to an owned compiled CLAP fixture, an owned VST3-shaped directory,
outside subdirectories, loops, missing/moved files and bounded chains. It checks
canonical deduplication, native junction behavior, callback refusal, COM thread
ownership, unchanged file bytes and no CLAP entry initialization. A directory
symlink case reports a skip if the account cannot create it. These are discovery
tests; the VST3-shaped directory is not presented as a validated plug-in, and
finding a path is not scanning its metadata or proving audio readiness.
The chain/cycle fixtures first save native links to owned empty directories,
then replace those directory paths with the authored link files. This models a
filesystem transition while preserving native Shell Link bytes: the ordinary
Shell writer otherwise flattens existing link targets or refuses the cycle.

The SDK contract comes from Microsoft's
[Shell Link persistence overview](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-shllink/a6c2f32d-2297-4727-bcd3-5d3669573bcb),
[GetPath](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllinkw-getpath),
[CreateStreamOnHGlobal](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-createstreamonhglobal)
and [COM initialization](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex).
The implementation uses existing Windows SDK/ole32 support, with no new external
dependency. It never loads the discovered plug-in in the finder process.
