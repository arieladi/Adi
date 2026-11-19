# Settings-bundle publication and import envelopes

`exportBundle` builds its JSON and ZIP in a uniquely reserved sibling directory
and publishes the closed archive through `media::replaceFile`. Existing archives
use lease-safe replacement. If the volume rejects that operation, choose a new
filename. For a new target on a volume without exclusive rename, the publisher
claims the final name with an exclusive hard link to the closed archive. When
both operations are unavailable, choose another folder. A target created by
another writer during the fallback keeps its original bytes.

App-owned `settings.json` and its last-good backup can use a classic whole-file
rename when the volume lacks lease-safe replacement. Before that rename, the
publisher releases its destination lease and rechecks the file identity and
bytes. The main document is also compared with the version read before the save.
The save reports a portability warning because an external writer can still
change the destination during the interval between verification and rename.
The backup publishes first. A backup failure preserves the main file and leaves
the pending settings available for retry.

Cleanup addresses only the directory this invocation successfully created. The
old filename-derived `<output>.staging` path and other preexisting sibling paths
are not deleted or adopted. Nested exports reserve independent stages. This is
private staging for a normal invocation, not PD-04/PD-05's future session-marker
and dead-session recovery system; a crash or cleanup refusal can leave a private
stage behind. No broad staging-directory sweep is performed.

An export's `BundleReport::publication` is present only after successful
publication. `durabilityConfirmed` reports the shared publisher's folder-sync
result; `warning` can be nonempty while `ok` is true. A caller must show such a
warning separately, not retry or delete a successfully published bundle. Warning
storage is prepared before publication; the success path afterward uses only
nonthrowing scalar/string moves. Import reports do not populate this field.
The optional per-call `BundleExportHooks` are diagnostic injection; ordinary
calls leave them empty.

Before import can change or log any setting, both `manifest.json` and
`settings.json` must be objects with an integer `schema` equal to the supported
`kSettingsSchema` (currently 1). Missing, older, newer, floating, string or other
malformed version values are rejected; integers are compared without narrowing
to `int`. No absent-schema legacy format or migration is implied.

The manifest must contain the string kind `adi-settings-bundle` and a string
application name matching the receiving application. The settings document must
also contain that same application string and an object `values`. Missing or
wrongly typed kind/app fields produce an unsuccessful report rather than a JSON
type exception. Any envelope rejection has zero applied settings and no returned
presets; the receiver's stored values, settings file and change log are untouched.
The input ZIP is read only.

Actual exported v1 archives retain their existing settings, preset selection
and role resolution behavior. This envelope gate does not change individual
value/preset handling, validate preset schemas, add migration, archive resource
limits, path-role hardening, transactional rollback after the gate, or UI warning
presentation. These remain separate PD-11 work.
