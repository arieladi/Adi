# Always-child plug-in discovery

Phase D/R-17/D-03 discovery now uses `adi_plugin_scanner` for every CLAP/VST3
candidate, including a project's direct VST3 path hint. There is no in-process
discovery switch or fallback. Runtime instantiation/audio remain in process as
D-03 requires. This branch depends on both independent foundations,
`engine/plugin-scanner-process` and `engine/plugin-scan-quarantine`.

`scan::Scanner::open(Config,error)` opens the shared version-2 PluginRegistry.
Config defaults to appdata's registry and an `adi_plugin_scanner[.exe]` beside
the current executable. Explicit paths must be absolute. Process bounds and
cancellation use ProcessOptions; the default deadline is 30 seconds and output
limit 8 MiB. The owner clock supplies nonnegative Unix microseconds. Use one
owner at a time per Scanner/registry connection, on an owner worker. These are
blocking services, not UI/audio callback work.

`probe(format,path,validator,context)` resolves an existing native canonical
path, commits a pending marker before launch and invokes the helper with an
explicit argument vector. It checks exit status and validates the whole packet,
then optional format validation, before token-checked completion. Only Ready
contains descriptors. Crashes, timeout, bad/oversized/mismatched metadata,
nonzero child exit and format rejection retain a named block. Existing pending/
blocked paths never launch again without explicit re-enable. Cancellation and
helper launch/configuration failures abandon their own marker; helper exit 78
means unsupported deployment. Completion failure/stale tickets publish nothing
and retain quarantine. Resource exceptions retain pending state. A pending row
means in-progress/interrupted, not proof of a plug-in crash. Native alias
resolution binds admission; no filename/Unicode case heuristic is used.

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
