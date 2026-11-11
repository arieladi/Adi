#!/bin/sh
# usage: land_top.sh N -- land a stack through its top PR N: wait for every check to finish; a job
# that failed because its runner died (shutdown signal, exit 143, lost communication) is rerun
# once; any other failure stops with its name; when green, merge_stack.sh retargets N to main and
# merges it. The PRs below N are then closed by close_landed.sh, not here.
R=arieladi/Adi
SP="$(cd "$(dirname "$0")" && pwd)"
n=$1; rerun_done=""
while :; do
  s=$(gh pr view $n --repo $R --json state --jq .state 2>/dev/null)
  [ "$s" = "MERGED" ] && { echo "$n already merged"; exit 0; }
  j=$(gh pr view $n --repo $R --json statusCheckRollup --jq '
      (.statusCheckRollup | group_by(.name) | map(sort_by(.startedAt // "") | last)) as $r
      | ($r | map(select(.conclusion==null or .conclusion=="")) | length) as $pending
      | ($r | map(select(.name!="matrix.name")) | map(select(.conclusion=="FAILURE" or .conclusion=="TIMED_OUT" or .conclusion=="STARTUP_FAILURE")) | map(.detailsUrl) | join(" ")) as $bad
      | ($r | map(select(.name!="matrix.name")) | group_by(.name) | map(select(any(.[]; .conclusion=="SUCCESS" or .conclusion=="SKIPPED"))) | length) as $ok
      | "\($pending)\t\($bad)\t\($ok)"' 2>/dev/null) || { sleep 120; continue; }
  p=$(printf '%s' "$j" | cut -f1); bad=$(printf '%s' "$j" | cut -f2); ok=$(printf '%s' "$j" | cut -f3)
  if [ -n "$bad" ] && [ "$p" = "0" ]; then
    infra=1; runs=""
    for url in $bad; do
      job=$(echo "$url" | grep -oE "job/[0-9]+" | cut -d/ -f2); run=$(echo "$url" | grep -oE "runs/[0-9]+" | cut -d/ -f2)
      log=$(gh api --allow-escape-sequences repos/$R/actions/jobs/$job/logs 2>/dev/null | tail -40)
      echo "$log" | grep -qE "received a shutdown signal|exit code 143|lost communication with the server|The operation was canceled" || infra=0
      runs="$runs $run"
    done
    if [ $infra = 1 ] && [ -z "$rerun_done" ]; then
      for r in $(echo $runs | tr ' ' '\n' | sort -u); do gh run rerun $r --repo $R --failed >/dev/null 2>&1 && echo "$(date -u +%H:%M) rerun $r (runner fault)"; done
      rerun_done=1; sleep 300; continue
    fi
    echo "STOP: $n has failed checks: $bad"; exit 1
  fi
  if [ "$p" = "0" ] && [ -z "$bad" ] && [ "$ok" -ge 20 ]; then
    date -u +%H:%M
    sh "$SP/merge_stack.sh" $n || { s=$(gh pr view $n --repo $R --json state --jq .state); [ "$s" = "MERGED" ] && echo "$n merged after all" || { echo "STOP: $n did not merge ($s)"; exit 1; }; }
    exit 0
  fi
  sleep 180
done
