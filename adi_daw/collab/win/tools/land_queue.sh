#!/bin/sh
# usage: land_queue.sh N... -- land PRs in order. A PR is refreshed (close/reopen) so its CI uses
# main's current workflow; when it conflicts with main the queue stops with "CONFLICT N" for win.
R=arieladi/Adi
SP="$(cd "$(dirname "$0")" && pwd)"
for n in "$@"; do
  [ "$(gh pr view $n --repo $R --json state --jq .state)" = MERGED ] && { echo "$n already merged"; continue; }
  until m=$(gh pr view $n --repo $R --json mergeable --jq .mergeable) && [ "$m" != UNKNOWN ]; do sleep 20; done
  [ "$m" = CONFLICTING ] && { echo "CONFLICT $n"; exit 2; }
  gh pr close $n --repo $R >/dev/null && sleep 5 && gh pr reopen $n --repo $R >/dev/null && echo "$(date +%H:%M) $n refreshed"; sleep 90
  sh "$SP/land_top.sh" $n || { echo "STOP $n"; exit 1; }
  [ "$(gh pr view $n --repo $R --json state --jq .state)" = MERGED ] && echo "$(date +%H:%M) $n merged" || { echo "STOP $n not merged"; exit 1; }
  sleep 30
  # cancel only main runs of OLDER heads; the run on the merge just made is main's proof
  sha=$(gh pr view $n --repo $R --json mergeCommit --jq .mergeCommit.oid)
  for r in $(gh run list --repo $R --branch main --limit 8 --json databaseId,status,workflowName,headSha --jq ".[]|select(.status!=\"completed\" and .headSha!=\"$sha\" and (.workflowName==\"adi_daw\" or .workflowName==\"Step 7 UI\"))|.databaseId"); do gh run cancel $r --repo $R >/dev/null 2>&1; done
done
echo "QUEUE DONE"
