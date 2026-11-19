# Controlled archive worker entry (PD-02 handoff)

`ProjectDocument::prepareCollectAndExport(zip, job)` remains the owner-thread
preparation step. The job contains only project and destination paths. The new
static worker overload is:

```cpp
media::FileResult ProjectDocument::runCollectAndExport(
    const ExportJob&, const media::ArchiveControl&);
```

It forwards the existing stop token/progress observer to `media::collectExport`
and returns its complete result: success, error, warning and typed cancellation.
The existing one-argument overload still returns `StoreFileResult`; it calls
this same path with an empty control and transfers its original three fields.
Both paths retain ordinary failure diagnostics and publication warnings.

Observers execute synchronously on the calling worker, and names in copied
progress records own their strings. The worker must keep its job/control alive
until return. It retains no document or live Store reference; the prepared job
can run after its document closes. A thrown observer exception is a failure,
not cancellation; the worker entry also converts nonstandard exceptions into
an error result. This does not promise recovery from resource exhaustion while
constructing an error string.

The underlying cancellation limits are unchanged: external copy and ZIP input
have bounded byte-work checkpoints; snapshot, embedded extraction and hashing
remain phase-based. Cancellation before publication creates no ZIP and removes
owned private staging. There is no post-publication cancellation claim.

The real document regression prepares a disposable project, starts an owned
worker, observes progress and cancels 128 KiB into external media copying. It
checks typed cancellation, no archive/stage and unchanged source media/project.
The same prepared job then succeeds through the legacy overload after closing
the document, with exact archived media readback. Existing-target refusal and
a nonstandard phase-observer failure are also checked.

No UI, progress-throttling/dispatch policy, persistence change or task manager
is added. The window still chooses when to start/stop its worker and how to post
copied progress and the completed result to its own thread.
