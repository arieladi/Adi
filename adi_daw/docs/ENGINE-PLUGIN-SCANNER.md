# Always-child plug-in discovery

Phase D/R-17/D-03 discovery uses `adi_plugin_scanner` for every CLAP/VST3
cache miss, including a project's direct VST3 path hint. A validated success
cache avoids another child for an unchanged candidate. There is no in-process
discovery switch or fallback. Runtime instantiation/audio remain in process as
D-03 requires. This branch depends on both independent foundations,
`engine/plugin-scanner-process` and `engine/plugin-scan-quarantine`.

`scan::Scanner::open(Config,error)` opens the shared version-3 PluginRegistry.
Config defaults to appdata's registry and an `adi_plugin_scanner[.exe]` beside
the current executable. Explicit paths must be absolute. Process bounds and
cancellation use ProcessOptions; the default deadline is 30 seconds and output
limit 8 MiB. The owner clock supplies nonnegative Unix microseconds. Use one
owner at a time per Scanner/registry connection, on an owner worker. These are
blocking services, not UI/audio callback work.

`probe(format,path,validator,context)` resolves an existing native canonical
path, checks typed negative state, commits a pending marker before success-cache lookup or launch and invokes the helper with an
explicit argument vector. It checks exit status and validates the whole packet,
then optional format validation, before token-checked completion. Only Ready
contains descriptors. Bad/oversized/mismatched metadata, ordinary nonzero child
exit and format rejection retain a named negative result. Reliably stamped
crashes/timeouts allow one further explicit probe before two strikes block more
children. Pending and manual/legacy blocks always need explicit re-enable.
Cancellation and helper launch/configuration/I/O failures abandon their own marker; helper exit 78
means unsupported deployment. Completion failure/stale tickets publish nothing
and retain quarantine. Resource exceptions retain pending state. A pending row
means in-progress/interrupted, not proof of a plug-in crash. Native alias
resolution binds admission; no filename/Unicode case heuristic is used.

## Version 3 success cache (DEV-25 first slice)

Version 3 adds `plugin_scan_cache`: keyed by format and canonical path,
holding a stamp, the entire bounded schema-1 packet, and an explicit timestamp.
Version-1/2 route and quarantine rows are preserved; old applications refuse
the newer registry rather than misreading it. The additive failure table below
also opens early success-only v3 databases without rewriting their existing rows.

The BLAKE3 stamp incorporates its own version, the shared protocol version,
format, canonical candidate identity and the helper executable's BLAKE3 digest.
The helper digest changes with its linked discovery code, including JUCE. It is
read on Scanner open and refreshed when helper size/time changes; a helper
changed during a probe prevents publication. An identical helper copy does not
invalidate the cache solely because its installation path differs.

For a flat candidate, both file size and full native modification time are
stamped. For a bundle, native module files (`.vst3`, `.clap`, `.dll`, `.so`,
`.dylib`, and files under `Contents/MacOS`) and `moduleinfo.json` are sorted and
stamped individually with their relative and canonical paths, sizes and times.
Directory modification time is not used. A bundle walk is sorted and bounded to
16,384 entries, 4,096 stamped files and 16 MiB of fingerprint text. Internal
directory links use canonical cycle detection; directory links leading outside
the bundle are not traversed. Unrecognized layouts use their ordinary files
as the fallback list; an empty layout still reaches the child and its normal
failure policy. If a reliable bounded stamp is unavailable, discovery still
runs in the child and returns an explicit `cacheWarning`; that result is not
cached. A large/unusual bundle therefore does not abort the discovery batch
solely because of cache policy. Candidate-level native aliases, including
relocated bundles, are resolved before admission.
Changing bytes while preserving **both** size and time is outside this metadata
invalidation contract; this is not a content-integrity or tamper-detection cache.
CLAP-declared invalidation sources and Finder-alias discovery remain later
DEV-25 work.

A cache hit still takes a pending ScanTicket. This is the same cross-application
admission as a child probe, so a pending or manually blocked candidate cannot
bypass quarantine by using cached descriptors. The packet is untrusted: bounded
SQL extraction, ordinary strict JSON decoding, and the VST3 format validator all
run again. A malformed packet is replaced only through a fresh isolated probe;
an oversized or invalid stored row reports unavailable. No ABI object bytes are
serialized. A valid cache packet is not evidence of runtime instantiation,
restored state or processed audio.

The candidate stamp is checked again after validation. If it changed, the
attempt is abandoned with no descriptors and no new quarantine; a subsequent
explicit probe can retry the stable candidate. A new success replaces the
whole module packet in the same transaction that completes its exact pending
ticket. A late user block or stale/foreign ticket cannot publish it, and a
failed SQL insert rolls the ticket deletion back. Warm hits complete their
ticket without rewriting the cached packet. Explicit re-enable removes both
the block and cached success, so its next probe is fresh.

Packets and stored-packet extraction cap at 8 MiB. Every reliably stamped success
is stored; a write failure rolls back and is reported. No full-cache count/sum
is performed per candidate, and no aggregate byte cap silently causes an endless
reprobe. Total persistent storage grows with discovered modules; seen-set cleanup
and retention policy remain later service work. These bounds do not weaken
existing process output limits or metadata decoding. SQL/file work stays off
the audio callback.

`adi_scan_cache_tests` exercises the real process runner and shared registry:
three unchanged probes including reopen launch one child; timestamp-only and
size-only/restored-time changes reprobe; bundle modules and moduleinfo invalidate
independently of directory time; canonical aliases reuse a success; helper build
changes invalidate; corrupt packets, late blocks, cancellation and mid-probe
file changes do not publish unvalidated/stale data; a late SQL refusal rolls
back; v2 migration preserves routes/blocks. `adi_plugin_scanner_tests` also
repeats a real CLAP module and checks that no new init PID records appear.

This is a cache foundation, not the whole DEV-25 row. It does not add time-based
failure expiry, a retry loop, a worker pool, a UI, a startup service, or catalogue-first
instance loading. The loader's lazy/hint scans still call this service; moving
discovery out of project open is a separate change. Saved JUCE identifier/path
spelling is unchanged. Readiness stages need separate runtime observations.

## Stamped negative results and two strikes (DEV-25 second slice)

The additive v3 `plugin_scan_failures` table records the same reliable stamp,
bounded diagnostic, explicit time, failure kind, strike count and (when blocked)
the exact automatic block row's id. Stamped admission is transactional:

- An empty class list or unchanged deterministic rejection returns `Failed`
  with `failureCacheHit=true` and the original reason, without another child.
  It has zero strikes and no block id or quarantine row. A live pending ticket
  or manual block takes precedence in the same admission transaction.
- The first crash or timeout returns `Failed`, `failureStrikes=1`, and removes
  its pending marker. A later explicit probe may run one more child. A second
  same-stamp crash/timeout, including one of each, retains the automatic block;
  subsequent probes reuse that failure with `failureStrikes=2`. There is no
  immediate retry inside one probe call. A successful attempt clears strikes.
- A changed reliable stamp retires only owned automatic failure/block state
  and stale success metadata before admitting a new attempt. A failure, like
  a success, must pass the post-probe stamp/helper stability check. A changed
  file during either outcome is abandoned for a later probe.
- Manual and inherited untyped v2/early-v3 blocks, and interrupted pending
  attempts, never auto-expire. Without a reliable stamp, crashes/timeouts keep
  conservative legacy immediate blocking; deterministic/empty results remain
  uncached failures and never quarantine. Both report `cacheWarning`.
- A completed failure removes prior success metadata in the same transaction.
  A late block/foreign ticket/SQL error cannot publish a failure or clear a new
  decision; transaction rollback restores the prior packet and pending ticket.

`Scanner::retry(format,path,error)` resolves an existing path with exactly the
same native canonicalization as probe, then clears block, positive and negative
state. Separator, dot-component, Windows case and supported native links cannot
reset a different spelling while leaving the actual canonical block behind.
Retry launches nothing itself. Missing files refuse rather than guessing a
canonical alias; owners with a stored canonical missing key may use the low-level
registry API. No GUI control is introduced here.

Triggers retire typed automatic ownership on a later block update/delete or
success insert, including writes made by an earlier v3 application that knows
only the original tables. Thus its manual block update cannot later be mistaken
for a stamped automatic block. Earlier v3 applications may still apply their
older immediate-block policy; those untyped results remain conservative legacy
blocks. Earlier success-only v3 writers do not understand negative results and
may reprobe them, but cannot bypass a pending/manual/two-strike block. Negative
results and first strikes are stored only in the failure table, never advertised
as active blocks by `scanBlocks`. Exit 78, launch failure, cancellation, unavailable
registry and process I/O infrastructure errors do not consume strikes. Output
limit or malformed protocol is a deterministic rejection. Changing timeout or
output limits alone does not change the module stamp: explicit retry is needed
to reconsider an already blocked result under changed process limits.

The focused suite counts real child launches for negative reuse, crash/timeout
strikes across reopen, changed-file recovery, successful second attempts and
retry aliases. It also covers cancellation, registry contention, late block,
SQL rollback, stale-positive removal, old-v3 block writes and additive migration.
OS symlink tests report SKIP when the account lacks the required privilege.
Windows also exercises a native directory junction through actual canonical
retry and removes only its owned reparse point, leaving the target intact.

## Read-only stored catalogue (DEV-25 hydration foundation)

`scan::readCatalogue(registry,error,limits)` opens a separate SQLite READONLY
connection and one read transaction. It does not open a Scanner, require/hash
the helper, create/migrate the registry, write a pending ticket, launch a child,
resolve/stat candidate paths or enumerate search roots. Its optional typed status
distinguishes `Missing`, `Legacy`, `Ready` and `Invalid`. Missing and validated
pre-cache v1/v2 schemas return no snapshot without creating or migrating a file.
Legacy admission verifies the ADI application ID, historical strict table/column
and primary-key layouts, v2 quarantine identity uniqueness, and absence of newer
cache/failure tables or unexpected legacy triggers. Foreign, future or malformed
registries return `Invalid`; a lowered version pragma on a v3 store is not legacy.
Both success-only v3 files and
v3 files with the additive failure table can be read without schema changes.

Only positive rows with no matching pending/manual/automatic block and no typed
failure (including a first strike or zero-strike negative) enter the snapshot.
That exclusion and the packets come from the same database transaction. Stored
types, timestamps, stamp text and packet lengths are checked before extraction;
the strict common metadata decoder then verifies format and exact stored path.
A legacy valid empty class list is excluded, not advertised as an installed
positive. Any corrupt included row or exhausted bound refuses the whole read,
never a healthy prefix. Bounds are 65,536 modules, 65,536 aggregate descriptors
and 64 MiB aggregate stored text, with tighter caller limits available; each
packet still has its 8 MiB bound. An invalid read never silently permits scanning.

The result is a **stored metadata snapshot**, not `State::Ready`, an instantiated
device or a guarantee that files/helper builds are still current. Each module
retains format, stored canonical path, opaque stamp, timestamp and Plugin fields.
VST3's data remains serialized XML: the JUCE adapter must still run its format
validator before installation, then restore the original absolute path-hint
spelling when computing saved JUCE identifiers. `Catalogue::find` performs an
exact in-memory lookup using a caller-supplied canonical key; it does not guess
aliases or touch the filesystem. A block added after the read requires a fresh
snapshot or an explicit use-time policy; a frozen snapshot is not live authority.

The focused `adi_scan_catalogue_tests` populate through real child processes,
remove only their owned helper, and verify unchanged child receipts, no pending
rows, unchanged registry/WAL bytes and unchanged data version after hydration.
They cover real two-strike exclusion, deliberately seeded stale positive/failure
overlaps, manual/pending exclusion, packet identity/corruption, empty legacy data,
bounds, older-v3 compatibility and transactions concurrent with owner updates.
Historical v1/v2 fixtures verify typed legacy admission, unchanged database bytes,
no migration, callback status preservation and malformed/foreign/future refusal.
The registry callback gate also refuses this reader before allocation/file I/O.

The core reader alone does not install metadata or decide discovery policy;
the real loader integration below supplies that boundary. Background startup
discovery, live freshness/block notifications and the manager UI remain later
work. Found metadata, runtime instantiation, state restoration and first audio
are still distinct states, not inferred from a stored catalogue read.

## Real loader catalogue hydration

`JuceDeviceLoader::hydrateCatalogue(error)` reads the configured/default registry
without opening the helper. It decodes **all** VST3 XML using the existing strict
format validator and stages CLAP refs before replacing either loader catalogue.
A bad later module therefore cannot publish an earlier healthy prefix. Metadata
refresh does not unload libraries or disturb existing device instances. Explicit
discovery also stages its two loader catalogues before replacing a complete one.

The loader attempts hydration once before its first external-format request.
The GUI and CLI additionally call it at project-open boundaries, so a new project
gets a fresh stored snapshot without child discovery or search-root enumeration.
Repeated instance creation does not repeat the SQL read. `setScanConfig` explicitly
clears staged metadata and resets hydration. A corrupt existing registry fails
closed for new external requests until an explicit successful refresh/scan.

After a v3 catalogue has been hydrated, including a valid **empty** catalogue, an unknown
identity returns a null device with a `NeedsDiscovery` diagnostic and increments
`Stats.needsDiscovery`. It never falls back to a path-hint scan, root walk or child.
Adding search roots prepares a later explicit `scan()`; it does not bypass this
policy. A fresh loader with no registry file or a validated historical v1/v2
schema retains the legacy discovery path. Read-only hydration never migrates;
only later actual owner discovery opens/migrates the legacy registry, retaining
route preferences and v2 quarantine rows. Foreign, future and corrupt files are
not treated as legacy or absence. A missing or validated legacy registry during
an explicit refresh retains an existing complete in-memory snapshot.
The existing explicit scan/CLI listing path remains available; automatic background
discovery and a GUI manager action are not added by this slice.

VST3 lookup retains exact saved JUCE identifiers. For an absolute saved path alias,
the loader resolves that hint and projects a temporary description onto its original
absolute spelling **only** when the stored canonical module matches. It then demands
the exact saved identifier; no class-hash match guesses another installed location,
and canonical cache/project rows are not rewritten. CLAP keeps stable descriptor id
and the existing canonical preferred-copy selection. VST3's stored class hashes
remain hashes, not fabricated native 128-bit CIDs.

The hosted fixture gate populates the persistent cache using real VST3 and CLAP
modules, removes its private helper, then creates both formats in fresh/reopened
loaders. It covers known identity without a hint, native same-module alias spelling,
unknown target/root traps, callback refusal, all-or-nothing VST3 XML validation,
manual quarantine and zero-strike negative exclusion. Historical v1/v2 fixtures
exercise the shared GUI/CLI explicit project-open hydration boundary as well as
lazy loader entry, then real saved-hint discovery and instantiation. They check
unchanged bytes before discovery and retained route/quarantine rows after migration;
foreign/newer/malformed controls remain closed even with a working helper and hint.
The CLAP PID receipt separates
its initial scanner child from subsequent in-parent runtime instantiation. This
proves discovery avoidance and instantiation, not state restoration or first audio
from those fixtures. The independent production render test continues checking
mono/four/six-channel PCM round trips without a hardware device.

The child reserves a dedicated copy of its stdout pipe before loading any
plug-in. It redirects ordinary stdout to the null stderr channel, so constructor,
init, factory, deinit and unload chatter cannot become metadata. CLAP strings
are bounded before copying (bad pointers crash only the child), descriptor count
is checked before enumeration, feature lists cap at 1,024 entries/65,536 bytes,
and library deinit/unload happens before packet publication. VST3 uses pinned
JUCE's headless discovery and serialized PluginDescriptions. Its factory probe
is compiled only in the helper target. The process runner handles crash,
deadline/cancellation, output limits, descendants and reaping; this is crash
containment, not a filesystem/network privilege sandbox.

The schema-1 UTF-8 JSON packet has exactly schema, format, path and plugins.
Each descriptor has exactly id, name, vendor, version, features, instrument and
data; types are exact. CLAP data is empty; VST3 data contains description XML.
The request's format/path must match exactly. Duplicate object keys (including
escaped spellings), duplicate ids, unknown/missing fields, future schemas,
NUL/bad UTF-8 and trailing/truncated data refuse the whole reply. Limits are
8 MiB packet, 65,536 descriptors, 65,536 raw bytes per JSON string, depth 8 and
4,194,304 preflight tokens (string starts, opening containers and separators);
decoded text also caps at 65,536 bytes. Escapes and
field overhead count against actual wire limits. No successful prefix is used.

A streaming SAX pass rejects duplicate/unknown keys before ordinary DOM parsing.
The DOM callback parser in pinned nlohmann 3.12 scans the growing parent array
after each object, making the large valid case quadratic even when nothing is
discarded. The streaming pass avoids that path. The complete supported
65,536-descriptor packet and a 65,537-descriptor refusal fit the byte/token
budget and are exercised in the focused suite. Parsing allocates on the owner;
there is no hard real-time or universal parent wall-clock claim.

VST3's required validator checks description XML before clearing quarantine:
no DTD/entity declarations, nested elements, mismatched path/id/name/vendor/
version/category/instrument or negative channel counts. A bad later description
cannot publish an earlier good prefix. The pure parent client stages owned
PluginDescriptions until the whole probe succeeds. JUCE includes file path
spelling in its stored identifier, so the parent restores its original absolute
JUCE path after validating the canonical response. Native alias tests exercise
this distinction. CLAP's stable descriptor id is independent of path; the
loader canonicalizes saved hints for its installed-copy tie-break.

`ClapHost::scan(paths,config)` and `Vst3Host::scan(paths,into,config)` return Report
with complete/cancelled and named file failures. Pure directory enumeration
loads nothing. Healthy siblings remain discoverable after a bad candidate;
cancelled/unavailable service retains the prior complete catalogue. Total owned
catalogue descriptors cap at 65,536. CLAP discovery leaves its runtime library
cache empty. Vst3Host::scanFile uses the same child boundary for a direct hint.
JuceDeviceLoader exposes setScanConfig/lastScanReport, uses those paths and
refuses to instantiate from an incomplete lazy scan. A failed VST3 hint reports
its failure directly. Existing running instances are not terminated by rescans.
Diagnostic probes, including the VST3 SDK fixtures and the raw CLAP runtime
probe, also obtain descriptors through the child before runtime instantiation.
Hosts/loaders also require one owner at a time. lastScanReport is borrowed only
between jobs; copy/publish reports for another thread after completion.
The window can read/re-enable PluginRegistry's owned blocklist; no UI is drawn
or automatically wired by this branch. Explicit runtime host APIs still load
devices; the scanner blocklist is discovery state, not a runtime sandbox.

CMake builds the helper with CLAP everywhere and VST3 when ADI_WITH_JUCE is ON.
Every JUCE executable that discovers plug-ins (probe, player, render probe and
app) depends on an always-run copy target placing the helper beside it, including
inside the app's MacOS directory. A helper-only change updates that copy without
requiring a consumer relink. Package/sign the matching helper with the app;
distribution signing is not done here. A CLAP-only helper returns unavailable
for VST3 instead of loading it elsewhere. `adi_clap_probe` and ordinary test
executables already share the helper's build directory.

The new default-build suite uses controlled real CLAP modules (including a
static-constructor crash fixture) for init/factory/descriptor/deinit/unload
crashes, hangs, noisy output, malformed/duplicate/oversized descriptors, exact
UTF-8 paths/values, child PID ownership, quarantine/re-enable, late owner block,
format rejection, cancellation, unavailable helper, catalogue retention and
callback refusal. The separate JUCE/headless suite uses the existing three-class
VST3 fixture and production decoder/client. Linux's modules-only focused build
can run it without X11/ALSA development packages; that is not a whole JUCE/UI,
MSVC/macOS/hardware or Live parity claim. Native adapters must rebuild for the
changed C++ scan signatures; external plug-in ABIs and project formats are
unchanged. See the two foundation documents for process and registry contracts.

## DEV-25's original 16 cases

The acceptance input is `plugins/scanner/cases.json` from the research-assets
intake (`a60371a84436bcbb4c5cb50bdacc8e62abd94343`). Its original `ScanProbe`
only returns factory metadata and deliberately cannot create an `IComponent`.
JUCE's real discovery path initializes a component before publishing a
description, so that diagnostic alone cannot prove successful ADI discovery.

`adi_scanner_case_sequence_tests` instead applies the same opt-in controls to
ADI's owned working VST3 fixture. With `ADI_SCAN_LOG` absent its normal three
classes and version stay unchanged. The acceptance process sets that variable
and the class/version/failure controls only in its own environment, copies the
native module into its private directory and calls the actual scanner helper.
Factory receipts identify every child and count `GetPluginFactory`, class
queries and `createInstance`. No failed fixture runs in the parent host. The
Windows-only gate deliberately tests real PE size changes, case aliases and a
native directory junction; its results do not imply native macOS/Linux coverage.

| Original case | Executable evidence |
| --- | --- |
| 1. Cold discovery of both classes | Two real components are described; factory/create-instance counters and child PID are recorded. |
| 2. Warm unchanged scan | Two cache hits, including scanner reopen, add no factory calls. |
| 3. Add one module | New module is probed; original persisted row remains byte-identical and warm. |
| 4. Timestamp-only change | One module is reprobed; the sibling record stays unchanged. |
| 5. Changed class count | Two classes become one; the complete persisted module loses the removed class. |
| 6. Changed version, stable identity | Version changes from 1.0.0 to 2.0.0; exact surviving saved UID remains stable. |
| 7. Changed size, restored timestamp | An owned PE receives one trailing byte, remains loadable, and is reprobed despite restored mtime. |
| 8. Null factory | Failed, zero-strike negative, no quarantine; unrelated row survives. |
| 9. Warm negative | Same failed fingerprint returns cached failure with unchanged factory counters. |
| 10. New fingerprint after automatic block | Two actual timeouts block a third child; changed mtime allows a successful replacement. |
| 11. Abnormal child exit | Explicit exit 73 is recorded and unrelated cache survives; this is not mislabeled a native crash. |
| 12. Caller timeout | Timed-out scanner PID has ended; an independently owned sentinel child completes; sibling cache survives. |
| 13. Slash-alias retry | Explicit retry clears the canonical manual block and launches exactly one fresh probe. |
| 14. Case-alias retry | Same assertion through Windows case-equivalent spelling. |
| 15. Directory-link retry | Same assertion through an actual owned junction; cleanup removes only the link. |
| 16. Readiness stages | Hosted real CLAP fixture: metadata collection/discovery, instantiation, state restoration and first exact processed PCM are checked separately. |

Set `ADI_SCANNER_CASE_EVIDENCE` to an owned output directory to retain cases
1–15 JSON, raw factory receipts and logical final registry rows outside the
test's automatically removed private directory. These are actual VST3 factory
tests. The existing `adi_scan_cache_tests` additionally tests protocol/race and
cache policy with a controlled helper; those packets are not claimed as native
VST3 factory execution. Catalogue `State::Ready` still means validated metadata,
not an instantiated/restored/processing audio device. No production readiness
enum, graph ownership change, automatic startup service or hardware test is
introduced by this acceptance gate.

### Persisted Session reopen

`adi_play_render_tests` also runs `scannerSessionReopenContracts` with both
owned hosted formats. Each case discovers a private copied native module,
loads an actual Session with a stereo WAV clip, changes native plug-in state,
and persists it through Session-bound `ParamOps::snapshot` and the ordinary
journal's captured transaction. The test checks the durable state row and its
actual BLAKE3-addressed bytes before closing every initial Session/loader and
the project Store.

It then removes the private scanner executable, reopens the `.adi` file with a
fresh Store/loader/Session, and requires one instantiated device, one restored
chunk, no placeholder and no implicit scan. The saved UID and opaque state must
match exactly. At 48 kHz, Float32 and a maximum block of 64, the first blocks
after seeks to samples 31, 1003 and 4097 contain exactly 19, 64 and 1 expected
stereo frames, with untouched surrounding output guards and no primed-clip
underrun. Graph transition fading is explicitly zero for this sample oracle.

The CLAP fixture's restored state changes its output to one quarter of its
input. The VST3 fixture's patch/Drive state is checked independently: Drive is
not an audio gain in this fixture, and its no-note path passes input through.
An exported fixture-only cumulative counter additionally proves each VST3
Session block reaches a successful nonempty plug-in process call; passthrough
PCM alone would not distinguish an accidentally skipped device. These receipts
are printed as `SCANNER_SESSION_REOPEN` JSON.

This is headless project/owner reopen in one process, with native Windows x64
validation. It does not claim an application-process restart, native macOS or
Linux execution, physical audio-device timing, relocated plug-ins, settings UI
or the remaining DEV-25 startup-service/discovery workflow. The test changes
no host-processing, graph-publication or production readiness interface.

### Explicit discovery operation

`JuceDeviceLoader::discover` is the synchronous owner operation for
`DiscoveryMode::Incremental` and `CacheOnly`. It returns an owning
`shared_ptr<const DiscoveryResult>` with an explicit outcome, copied report and
an immutable catalogue of the existing VST3 `PluginDescription` and CLAP
`ClapPluginRef` values. These are the format-validated values the loader uses;
there is no new registry or packet schema. A consumer can retain a result after
another operation or loader destruction. Publishing it between threads remains
the caller's job; the loader itself still has one owner.

Incremental walks its configured roots and uses the existing isolated helper,
success/negative cache, stamps and quarantine. Healthy candidates can publish
beside reported candidate failures. Cancellation, deployment failure or a
whole-catalogue validation failure does not publish a successful prefix. The
result names `Failed` or `Cancelled` and retains the last accepted catalogue,
if any. That pointer is not evidence that the failed refresh succeeded.

CacheOnly opens the registry read-only and performs both common and format
validation. It does not open/check a scanner helper, enumerate roots, stat
candidates or load a native module. Its results distinguish `Missing`,
validated pre-cache `Legacy`, a `Published` empty catalogue, and `Failed`
corruption. An explicit CacheOnly request with no usable cache leaves misses
as `NeedsDiscovery`, never a hidden scan. The existing `hydrateCatalogue`
compatibility wrapper still allows a fresh loader's documented missing/v1/v2
legacy discovery path; that path is not renamed CacheOnly. Corrupt cached
metadata fails closed for subsequent external instances, even if an older
immutable snapshot remains available for presentation.

The existing `scan()` delegates to Incremental. `adi_play --list` also uses
this operation, prints its candidate diagnostics, and returns nonzero when no
new catalogue was published. `--list --cache-only` lists validated stored
metadata only. `--list --search-only DIR` excludes platform default roots and
can be repeated; `--search DIR` otherwise retains its original additive
behavior. Cache-only/search-only options are refused for project playback.
`JuceDeviceLoader(false)` similarly accepts only explicitly added roots.

The strict real-module gate in `scanner_discovery.hpp` covers cold/warm/add,
cancel/deployment failure with old-snapshot retention, CacheOnly with its helper
removed and every candidate temporarily moved away, malformed later VST3 XML
refusing the whole snapshot, missing/v1/v2/empty states, callback refusal and
the actual headless command modes. Its CLAP entry log distinguishes scanner
children from parent runtime loading. The prior Session-reopen and legacy
compatibility gates remain in the same executable.

This is one necessary service boundary. K-worker scheduling, Clean rebuild,
startup scheduling, persistent application scan logs beyond command output,
native VST3 class-ID indexing, expanded alias traversal, expiry/retry service
integration and Settings UI remain separate DEV-25 work. Stored metadata is
neither proof of current filesystem freshness nor instantiated/audio readiness.
