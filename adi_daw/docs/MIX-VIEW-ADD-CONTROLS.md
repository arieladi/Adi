# Add and remove functions in Mix View — DF-M21 / DF-M22

STACK has a **+ Function** control for the explicitly selected channel. It uses
the same prepared insertion authority and capability catalogue as Device View.
Choose All channels to select an empty channel when With Tools has no rows.
The menu shows checks for functions already present and disables unavailable
capabilities; it does not insert a reserved or inactive slot.

Its explicit target never changes the device strip's selected channel. A menu
retains the owning surface, selection epoch and current project snapshot;
closing/cancelling it, changing selection away and back, or replacing the project
prevents a late action from adding elsewhere. Presence and capability are checked
again at activation. A hidden Device View cannot cancel a visible Mix View's menu.
No parameter demand is acquired merely to populate the add menu.

Native activators in both expanded Device View and compact STACK now use one
common meaning: remove the function, with its settings recoverable by Undo.
Ordinary plug-in activators keep ordinary bypass. A refused removal restores the
widget's authored state. Retained identities are validated before mutation and
callbacks may replace or destroy a presentation without editing its replacement.
Held interactions are ended before native removal.

The additions change presentation/dispatch only. No DSP algorithm, parameter
mapping, processing order, quality, latency or analyzer work is changed. The
preset merge/replace policy and device-key schema remain separate work.

Verification uses generated software peers and real document insertion/Undo,
plus stale/cancelled owner and cross-surface selection checks. Live menu position,
keyboard/pointer feel and playback acceptance stay with Claude. No CPU ratio or
live verification follows from the component tests.
