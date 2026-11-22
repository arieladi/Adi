#!/bin/sh
# usage: land_top.sh N -- land a stack through its top PR N: wait for every check to finish; a job
# that failed because its runner died (shutdown signal, exit 143, lost communication) is rerun
# once; any other failure stops with its name; when green, merge_stack.sh retargets N to main and
# merges it. The PRs below N are then closed by close_landed.sh, not here.
#
# "Green" is decided by land_checks.py beside this file (19 November): adi_daw's "gate" check is
# SUCCESS, nothing is pending or failed, and when Step 7 UI ran all three "GUI shell / ..." checks
# exist and are SUCCESS or SKIPPED (or its one Markdown-only placeholder is SKIPPED). The old rule,
# at least 20 OK checks, now applies only to a PR whose newest adi_daw run has no "gate" check (it
# ran before the gate existed), only with the same GUI shell rule, and only when two polls in a row
# agree, so a new run whose gate has not been created yet cannot pass it.
# A STACK TOP (base not main) must have a Step 7 UI run of its own: ui.yml's paths filter sees only
# the top's diff against the PR below, so a top that touches only adi_daw/collab, a workflow or a
# .github script starts none, and the runs below it are cancelled. land_checks.py then answers UI,
# and this dispatches ui.yml on the head branch, once per head commit, and waits for that run. A
# dispatched run is not in the PR's statusCheckRollup, so it is read with gh run list / gh run view
# and passed to land_checks.py after the PR's JSON.
# A draft runs no CI, so a draft N is marked ready first; merge_stack.sh would do that anyway.
R=arieladi/Adi
SP="$(cd "$(dirname "$0")" && pwd)"
# python3 where it is a real one; on Windows it can be the Store's stub, which only fails.
PY=python3; "$PY" -c '' >/dev/null 2>&1 || PY=python
n=$1; rerun_done=""; cancel_rerun_done=""; legacy_seen=""; ui_sha=""; ui_waits=0
while :; do
  s=$(gh pr view $n --repo $R --json state,isDraft,baseRefName,headRefName,headRefOid --jq '"\(.state) \(.isDraft) \(.baseRefName) \(.headRefName) \(.headRefOid)"' 2>/dev/null)
  # Git forbids * ? [ in a ref name, so the unquoted split cannot glob.
  set -- $s; state=${1:-}; draft=${2:-}; base=${3:-}; head=${4:-}; sha=${5:-}
  case "$state" in MERGED) echo "$n already merged"; exit 0;; esac
  if [ "$state $draft" = "OPEN true" ]; then
    gh pr ready $n --repo $R >/dev/null 2>&1 || { echo "STOP: $n is a draft and could not be marked ready"; exit 1; }
    echo "$(date -u +%H:%M) $n marked ready (a draft runs no CI)"
    sleep 120; continue
  fi
  # A stack top's dispatched Step 7 UI run on this head commit, if there is one (newest first).
  uj=""
  if [ -n "$base" ] && [ "$base" != main ] && [ -n "$sha" ]; then
    id=$(gh run list --repo $R --workflow ui.yml --event workflow_dispatch --branch "$head" --commit "$sha" --limit 1 --json databaseId --jq '.[0].databaseId // empty' 2>/dev/null) || { sleep 120; continue; }
    if [ -n "$id" ]; then
      ui_sha=$sha
      uj=$(gh run view "$id" --repo $R --json databaseId,headSha,status,conclusion,url,jobs --jq '{databaseId,headSha,status,conclusion,url,jobs:[.jobs[]|{name,status,conclusion,startedAt,completedAt,url}]}' 2>/dev/null) || { sleep 120; continue; }
    fi
  fi
  # tr: Python on Windows ends its line with CR LF, and the CR would ride on the last field.
  j=$( { gh pr view $n --repo $R --json statusCheckRollup,baseRefName,headRefOid 2>/dev/null; printf '%s\n' "$uj"; } | "$PY" -I "$SP/land_checks.py") || { sleep 120; continue; }
  j=$(printf '%s' "$j" | tr -d '\r')
  v=$(printf '%s' "$j" | cut -f1)
  bad=$(printf '%s' "$j" | cut -f3); cancelled=$(printf '%s' "$j" | cut -f4)
  gate=$(printf '%s' "$j" | cut -f6); gate_url=$(printf '%s' "$j" | cut -f7)
  [ "$v" = "LEGACY" ] || legacy_seen=""
  case "$v" in
  UI)
    # Everything else is green, but this stack top's head has no Step 7 UI run: the GUI code it
    # carries from the PRs below was never built. Dispatch one on the head branch, once per head
    # commit; workflow_dispatch makes markdown_only.sh answer code=true, so every shell builds.
    if [ "$ui_sha" != "$sha" ]; then
      gh workflow run ui.yml --repo $R --ref "$head" >/dev/null 2>&1 || { echo "STOP: $n: could not dispatch Step 7 UI on $head"; exit 1; }
      echo "$(date -u +%H:%M) $n: Step 7 UI dispatched on $head (a stack top whose own diff started no GUI shell run)"
      ui_sha=$sha; ui_waits=0
    else
      ui_waits=$((ui_waits + 1))
      [ $ui_waits -lt 10 ] || { echo "STOP: $n: Step 7 UI was dispatched on $head but no run on $sha appeared"; exit 1; }
    fi ;;
  CANCELLED)
    # A cancelled job (its time limit, or a runner taken away) has proved nothing: it is neither a
    # pass nor a failure of the change. #637 merged on 18 November with its macOS window job
    # cancelled at 15 minutes, because 20 others had passed. Rerun it once; a second cancellation
    # stops the landing.
    if [ -z "$cancel_rerun_done" ]; then
      for r in $(echo "$cancelled" | tr ' ' '\n' | grep -oE "runs/[0-9]+" | cut -d/ -f2 | sort -u); do gh run rerun $r --repo $R --failed >/dev/null 2>&1 && echo "$(date -u +%H:%M) rerun $r (cancelled job)"; done
      cancel_rerun_done=1; sleep 300; continue
    fi
    echo "STOP: $n has cancelled checks after a rerun: $cancelled"; exit 1 ;;
  FAILED)
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
    echo "STOP: $n has failed checks: $bad"; exit 1 ;;
  GATE)
    # The gate failed and no leg says why: read the gate's own log.
    echo "STOP: $n: gate is $gate and no other check failed: $gate_url"; exit 1 ;;
  LAND|LEGACY)
    if [ "$v" = "LEGACY" ] && [ -z "$legacy_seen" ]; then
      legacy_seen=1; sleep 180; continue
    fi
    date -u +%H:%M
    [ "$v" = "LEGACY" ] && echo "$n: no gate check on its newest run; landed on the old count"
    sh "$SP/merge_stack.sh" $n || { s=$(gh pr view $n --repo $R --json state --jq .state); [ "$s" = "MERGED" ] && echo "$n merged after all" || { echo "STOP: $n did not merge ($s)"; exit 1; }; }
    exit 0 ;;
  esac
  sleep 180
done
