# Settings-bundle publication

`exportBundle` builds its JSON and ZIP in a uniquely reserved sibling directory
and publishes the closed archive through `media::replaceFile`. It preserves the
existing successful replacement behavior, while a staging, archive, flush or
rename failure leaves the previous destination intact. There is no copy fallback
and the final ZIP is never opened for writing by `ZipWriter`.

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

Existing bundle contents, role conversion, preset selection and import behavior
remain unchanged. This export safety slice does not validate imported schema
versions or add migration, archive resource limits, path-role hardening, import
transactions or UI warning presentation. These remain separate PD-11 work.
