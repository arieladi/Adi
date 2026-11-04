# Plug-in editors for the floating host (N8)

All calls and handle destruction belong to the message thread. No editor,
component, geometry or visibility operation touches the audio callback or the
project journal. Missing/headless devices return null and a readable error.

`DeviceInstance::createEditor(mode,error)` returns a shared `DeviceEditor` lease.
One editor exists per device; another open in the same mode returns that lease.
Close before changing modes. `close()` is idempotent. Device destruction closes
its editor **before** unloading the plug-in; a retained lease then safely reports
invalid and refuses further GUI work. There is no requirement for the window to
keep a removed device alive. The window should still dispose its content when
its device disappears.

## FloatingHost content

Include `juce/editor_host_component.hpp` in the JUCE target and compile
`editor_host_component.cpp`. Call `createEditorComponent(device,error)` and move
the returned component into `FloatingHost::openView`. Start with its existing
width/height. Call `PluginEditorComponent::frame()` from that window's existing
frame callback. This drains notifications and updates size/visibility; it adds
no timer. The bridge owns the editor lease and closes it with the content.
Duplicate owning wrappers are refused, rather than stealing a component or
closing another window's editor.

VST3 uses JUCE 9's `createEditorAndMakeActive` (the public host route to the
plug-in's editor/IPlugView). The exact editor component is a child of the bridge.
Its own size and host resize limits are respected. Plug-in-initiated resize
updates the bridge. Close calls `editorBeingDeleted` before deleting the editor,
then the processor may be destroyed. No generic generated editor is substituted
for a plug-in with no editor. JUCE's peer owns VST3 DPI handling.

CLAP Hosted mode calls `create(api,false)` and attaches a dedicated native child
peer within the floating host, using win32, cocoa or X11 for this platform.
It shows only after `set_parent` succeeds. The bridge follows the host's native
peer, position and physical-pixel scale; Cocoa uses logical pixels and never
calls `set_scale`. It converts physical sizes to JUCE logical client dimensions.
Size changes use `can_resize`, `adjust_size` then `set_size`. The protocol's axis
and aspect hints are available on the lease. Size supplied by a plug-in resize
request is acknowledged without echoing `set_size`. A failed native attachment
or show leaves `PluginEditorComponent::error()` for the caller to report.
Neither format's component belongs inline in the device strip.

## CLAP's native floating mode

ADR-0076 specifically describes `create(api,true)`. It remains available through
`createEditor(EditorMode::Floating,error)`. Call `setTitle`, optionally `attach`
with the main window's transient owner, then `show`; call `hide` on close if
retaining it, or `close` to free resources. Do not put a second blank JUCE window
around this mode. `size()` reports the plug-in's dimensions if supplied (0/0
means a native floating window owns geometry without exposing it). `poll()`
updates reported native geometry. Hosted mode always requires a valid client
size. Unsupported modes/APIs fail with a reason; no silent mode fallback occurs.

Win review: the newer N8/FloatingHost request needs Hosted mode; the older
ADR-0076's explicit floating CLAP call is a separate supported mode. The window
can choose that older native floating route without using the component bridge.
No ADR is rewritten in this engine PR.

## Notifications and lifecycle

`clap_host_gui` is advertised per instance. Its thread-safe callbacks only
publish fixed atomic mailboxes. `DeviceHost::tick`/`pumpMainThread` or the editor's
`poll()` drains them on the message thread. Resize/hint and visibility requests
coalesce; malformed dimensions and requests without a live editor are refused.
`closed`, including `was_destroyed=true`, is acknowledged with GUI `destroy`,
without hiding a window whose native connection was already lost. An obsolete
notification is cleared on close/reopen. A closed lease does not retain raw
plug-in/glue access. Hiding signals the plug-in to stop drawing; no editor work
is done on the audio thread.

The headless suite exercises the CLAP GUI protocol with fake handles, not an OS
window. The JUCE probe runs ownership, constrainer, resize and component tests
without an installed plug-in. Native parenting, HiDPI/multiple monitors and real
plug-in drawing still require the platform/UI parity run. Linux GCC/Clang syntax
checks do not claim macOS/MSVC or hardware parity.
