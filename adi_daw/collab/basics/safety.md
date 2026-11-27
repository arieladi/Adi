# Basics safety report

The basics-safety queue is claimed. SC-159 is in Linux verification. The original Store
lost the committed edit in the live-file copy and left the recovered edit in the WAL.
The patched Store passed all 82 checks. The document and window-state suites passed
under Xvfb. The Debug shell suite, native-window open and full acceptance run are pending.

The changeset preview exceeded its 60-second bound on the original Store and with the fix
under software emulation. The built-in realtime test is being retried with a longer local
runner timeout.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|

## For the director
