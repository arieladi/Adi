#!/bin/sh
# usage: land_queue.sh N... -- land PRs in order. A PR is refreshed (close/reopen) so its CI uses
# main's current workflow; when it conflicts with main the queue stops with "CONFLICT N" for win.
# It never cancels a run on main (19 November): the loop that cancelled main runs of older heads
# meant the heavy macOS legs, which run only on main, did not finish for eight merges in a row.
# ci.yml and ui.yml no longer cancel main's runs either; GitHub keeps one waiting run per branch,
# so a landing burst ends in a finished run on main's newest head.
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
done
echo "QUEUE DONE"
