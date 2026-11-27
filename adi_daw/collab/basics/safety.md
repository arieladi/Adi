# Basics safety report

The basics-safety queue is claimed. SC-159 is in Linux verification. The original Store
lost the committed edit in the live-file copy and left the recovered edit in the WAL.
The patched Store passed all 82 checks. The Debug document, window-state and shell suites
passed with DISPLAY unset, as in CI. The document and window-state suites also passed with
Xvfb. The Xvfb shell run hit an X11 BadWindow error. In a native Xvfb launch the app opened
the synced project and displayed the warning in the Status Bar.

The changeset preview exceeded its 60-second bound on the original Store and with the fix
under software emulation. WTS-01 and its VST3 acceptance test passed. The built-in realtime
test reached the default 30-minute runner timeout and is running with a 90-minute local limit.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|

## For the director
