#!/bin/sh
# usage: chain_push.sh -- pushes every chain/engine/<b> tag to origin/engine/<b> (fast-forward only)
# and retargets each PR's base to its chain predecessor's branch, in the merge order. The first
# PR (#382, main-loudness) keeps base main. Prints what it did; stops at the first failed push.
R=arieladi/Adi
cd /d/wt-refresh || exit 1
ORDER="main-loudness transport-loop-metronome snapshot-track-delay markers-locators snapshot-automation history-list clip-details note-active ui-param-capture plugin-editors split-stereo-pan phase-c-editing device-copy library-devices mixer-test-runtime track-recording-options controller-targets delay-compensation-switch snapshot-remarks track-delete-undo track-roles snapshot-routing-ids snapshot-audio-peaks return-send-tap audio-input-recording ni-stems midi-note-transforms midi-controller-editing history-retention collect-project cue-reference clip-properties sample-slots default-returns track-freeze bounce-commands export-normalization history-compaction midi-controller-playback log"
git fetch -q origin
# CHAIN_ORDER / CHAIN_FIRST_BASE: push another order (the Codex seat's branches), whose first PR
# keeps the given base instead of main
[ -n "$CHAIN_ORDER" ] && ORDER="$CHAIN_ORDER"
prev=""; first_base=${CHAIN_FIRST_BASE:-main}
for b in $ORDER; do
  head=$(git rev-parse "chain/engine/$b") || exit 1
  if ! git merge-base --is-ancestor "origin/engine/$b" "$head"; then echo "engine/$b: origin moved since the chain was built; STOP"; exit 1; fi
  if [ "$(git rev-parse origin/engine/$b)" = "$head" ]; then echo "engine/$b: already at $head"; else
    git push -q origin "$head:refs/heads/engine/$b" || { echo "engine/$b: PUSH FAILED"; exit 1; }
    echo "engine/$b: pushed $(echo $head | cut -c1-8)"
  fi
  n=$(gh pr list --repo $R --head "engine/$b" --state open --json number --jq '.[0].number')
  if [ -z "$n" ]; then echo "engine/$b: no open PR"; prev="$b"; continue; fi
  want="$first_base"; [ -n "$prev" ] && want="engine/$prev"
  base=$(gh pr view $n --repo $R --json baseRefName --jq .baseRefName)
  if [ "$base" != "$want" ]; then gh pr edit $n --repo $R --base "$want" >/dev/null && echo "  #$n base $base -> $want" || echo "  #$n BASE EDIT FAILED"; fi
  prev="$b"
done
echo "PUSH DONE"
