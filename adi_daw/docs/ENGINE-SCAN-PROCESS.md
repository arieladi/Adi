# Scanner child-process boundary

`adi/scan_process.hpp` provides the owner-side process boundary for Phase D's
always-out-of-process scanner (R-17/D-03). It does not itself enumerate or load
plug-ins. ENGINE-PLUGIN-SCANNER.md now integrates this boundary with CLAP/VST3
child discovery and durable quarantine.

Call `scan::runProcess(absoluteExecutable, utf8Arguments, options)` on an owner
worker. The executable is explicit, with no PATH lookup, shell, command string
evaluation or process-wide working-directory change. Windows uses native UTF-16
paths and strict UTF-8 argument conversion with CRT quoting. POSIX passes each
argument directly to posix_spawn. Empty arguments, quotes, backslashes, spaces,
newlines and shell metacharacters remain argument data.

The result distinguishes invalid input, callback refusal, launch failure, normal
exit (including nonzero exit codes), crash, timeout, cancellation, output limit
and I/O failure. `succeeded()` requires normal exit zero. Output is raw stdout
bytes, including NUL; never parse a failed result as successful metadata. stderr
and stdin use the null device. Normal output is drained while the child runs;
there is no wait-before-read pipe deadlock. Each drain is bounded so output
cannot starve deadline/cancellation checks. Scanner helpers must reserve
its metadata channel before plug-in code runs, so incidental plug-in stdout
does not become metadata.

Options default to 30 seconds and 8 MiB output. Accepted bounds are 1–300,000 ms,
1 byte–8 MiB output, at most 256 arguments and a 128 KiB aggregate argv-byte
budget. Embedded NUL and relative executables refuse before launch. An optional
atomic cancellation flag must live until return. Already-set cancellation does
not start a child. Callback refusal precedes validation, allocation or OS calls.
The API can allocate and throw on owner resource exhaustion; spawned children
still have cleanup ownership during C++ unwinding.

Every child gets its own POSIX process group or Windows job. Timeout,
cancellation, output and I/O failure terminate the child and wait for it.
Normal exit also terminates remaining descendants; an inherited output writer
does not force the parent to wait for EOF. POSIX waitid/WNOWAIT reserves the
unreaped leader PID until group termination and waitpid; the application must
not ignore SIGCHLD/use SA_NOCLDWAIT or reap this API's children in another
reaper. No global signal handler is installed. Windows starts suspended,
assigns a kill-on-close job, then resumes, preventing a launch-before-job race;
only the intended pipe/null handles are inherited. Linux/glibc 2.34+ closes
other inherited descriptors in spawn actions; Darwin uses its close-on-exec
spawn policy. Other POSIX builds still require application descriptors to be
close-on-exec. Pipe descriptors are moved above 2 so closed standard
descriptors do not alias redirection actions. Caller handles/fds stay owned by
the caller. Cleanup waits are OS process teardown, not a hard real-time bound.

This is crash containment. It does not constrain filesystem/network privileges
or prevent intentionally hostile code from escaping a process group. Runtime
plug-in instantiation remains in process under D-03. No audio callback,
project opcode, schema or blob format changes here.

`adi_scan_process_tests` uses its own controlled child modes for actual crash,
hang, flood, binary/large output, stderr, exact argument transport, Unicode
executable paths, closed stdio, cancellation, timeout/normal-exit descendants,
repeated/concurrent launches and callback refusal. It checks direct POSIX child
reaping (Windows checks repeated-run handle balance). Each native test run
validates only its execution platform; Windows, Linux and macOS have independent
execution gates.

## Incremental binary stdout

`runProcessStream` reuses the same executable/argument validation, null
stdin/stderr, process ownership and cleanup. Existing `runProcess` options,
buffered results and scanner callers are unchanged. The streamed API requires a
positive `uint64_t` total-byte budget independent of the scanner's 8 MiB string
cap. It retains only a fixed read buffer, delivering nonempty binary spans
synchronously on the calling worker. Spans expire when the consumer returns;
their boundaries do not identify protocol records or audio frames.

The consumer returns `true` to acknowledge a chunk. `false`, a standard
exception or a nonstandard exception yields `ConsumerFailed`; the runner
terminates its child group/job and waits for the leader before returning. An
entire chunk that would cross the budget is refused before consumer entry.
`bytesDelivered` counts acknowledged whole chunks, not side effects made by a
consumer that later refused or threw. Bytes remain provisional after every
failure, including a later child crash/nonzero exit. The caller must discard or
roll back its partial output; only `succeeded()` establishes a complete stream.

Cancellation and deadline are checked before and after each consumer call as
well as while waiting. The consumer must be bounded and cooperative: a blocked
consumer cannot be interrupted by this synchronous API. The existing
1–300,000 ms timeout bound remains; teardown itself is not a hard deadline.
There is no asynchronous consumer, stderr capture, PCM framing, decoder launch,
cache admission or output publication in this primitive.

The owned-child tests add a handshake proving delivery before child exit,
12 MiB plus 137 bytes with incremental byte verification, exact/crossing bounds,
continuous output, cancellation during consumer work, consumer failure and
exceptions, crash/nonzero exit after binary bytes, inherited writers and
concurrent independent streams. Windows tests also compare handle counts after
the successful and failing streamed paths.
