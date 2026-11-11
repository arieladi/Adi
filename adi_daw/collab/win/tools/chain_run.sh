#!/bin/sh
# usage: chain_run.sh START_BRANCH -- builds the engine chain in /d/wt-refresh from START (inclusive)
# on top of the previous link's chain/ tag (or origin/main for the first), tagging chain/<branch>.
# Stops at the first HUMAN step, leaving the merge in progress in the worktree.
# The README table-count failure of validate_schema is ignored until #423 lands (the chain is rebuilt then).
S="$(cd "$(dirname "$0")" && pwd)"
cd /d/wt-refresh || exit 1
ORDER="main-loudness transport-loop-metronome snapshot-track-delay markers-locators snapshot-automation history-list clip-details note-active ui-param-capture plugin-editors split-stereo-pan phase-c-editing device-copy library-devices mixer-test-runtime track-recording-options controller-targets delay-compensation-switch snapshot-remarks track-delete-undo track-roles snapshot-routing-ids snapshot-audio-peaks return-send-tap audio-input-recording ni-stems midi-note-transforms midi-controller-editing history-retention collect-project cue-reference clip-properties sample-slots default-returns track-freeze bounce-commands export-normalization history-compaction midi-controller-playback log"
renum() { case "$1" in
  split-stereo-pan) echo "10:11";; phase-c-editing) echo "10:12 11:13";; track-recording-options) echo "10:14";;
  return-send-tap) echo "10:15";; clip-properties) echo "11:16";; track-freeze) echo "10:17 11:18";;
  history-compaction) echo "10:19";; *) echo "";; esac; }
stackbase() { case "$1" in
  cue-reference) echo transport-loop-metronome;; clip-properties) echo markers-locators;; sample-slots) echo library-devices;;
  default-returns) echo library-devices;; track-freeze) echo track-delete-undo;; bounce-commands) echo phase-c-editing;;
  export-normalization) echo main-loudness;; history-compaction) echo history-retention;; midi-controller-playback) echo midi-controller-editing;; *) echo "";; esac; }
validators() {
  (cd adi_daw || exit 1
   python -I tools/validate_schema.py > /tmp/vs.log 2>&1
   if grep -E '^ *FAIL ' /tmp/vs.log | grep -qv 'README says [0-9]* tables'; then echo "  validate_schema FAILED:"; grep -E '^ *FAIL ' /tmp/vs.log | head -5; exit 1; fi
   python -I tools/validate_ops.py > /tmp/vo.log 2>&1 || { echo "  validate_ops FAILED:"; grep -E '^ *FAIL |Error' /tmp/vo.log | head -5; exit 1; }
   exit 0)
}
# CHAIN_ORDER / CHAIN_PREV override the stack's order and the first link's predecessor (the Codex
# seat's branches continue the chain from chain/engine/log)
[ -n "$CHAIN_ORDER" ] && ORDER="$CHAIN_ORDER"
start=$1; prev=${CHAIN_PREV:-origin/main}; go=0
for b in $ORDER; do
  if [ "$b" = "$start" ]; then go=1; fi
  if [ $go = 0 ]; then prev="chain/engine/$b"; continue; fi
  echo "=== engine/$b (prev $prev)"
  git merge --abort 2>/dev/null; git reset -q --hard; git clean -fdq adi_daw; git checkout -q --detach "origin/engine/$b" || exit 1
  sb=$(stackbase $b)
  if [ -n "$sb" ]; then
    echo "  stack base first: chain/engine/$sb"
    python -I "$S/chain_step.py" /d/wt-refresh "engine/$b" "chain/engine/$sb" $(renum $b) || { echo "STOP at $b (stack base merge)"; exit 1; }
    if ! git diff --cached --quiet; then
      git -c core.safecrlf=false commit -q --no-edit -m "Merge the chain's engine/$sb into engine/$b (win: the engine stack of 4 November)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" || exit 1
    fi
  fi
  python -I "$S/chain_step.py" /d/wt-refresh "engine/$b" "$prev" $(renum $b) || { echo "STOP at $b"; exit 1; }
  if ! git diff --cached --quiet; then
    git -c core.safecrlf=false commit -q --no-edit -m "Merge the chain's predecessor into engine/$b (win: the engine stack of 4 November)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" || exit 1
  fi
  validators || { echo "STOP at $b (validators)"; exit 1; }
  git tag -f "chain/engine/$b" >/dev/null; prev="chain/engine/$b"
done
echo "CHAIN DONE"
