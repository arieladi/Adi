# Scanner child-process boundary

`adi/scan_process.hpp` provides the owner-side process boundary for Phase D's
always-out-of-process scanner (R-17/D-03). It does not itself enumerate or load
plug-ins. CLAP/VST3 discovery and durable quarantine are the next integration
items; their existing scan methods are still in process on this branch.

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
cannot starve deadline/cancellation checks. A future scanner helper must reserve
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
reaping (Windows checks repeated-run handle balance). Linux GCC/Clang evidence
does not claim Windows/macOS execution; those legs remain win/CI's gate.
