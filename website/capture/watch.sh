#!/bin/bash
# watch.sh <dir>: print each survey result as it lands, then DONE
cd "${RUNS:-$HOME/halo-capture-runs}/$1"; n=0
while true; do
  c=$(wc -l < survey.log)
  if [ "$c" -gt "$n" ]; then
    sed -n "$((n+1)),${c}p" survey.log | python3 -c '
import sys, json
for l in sys.stdin:
    try:
        r = json.loads(l)
        print(r["map"], r["yaw"], "frames", r.get("frames"), "light", round(r.get("light_diff", 0), 1), "loop", round(r.get("loop_psnr", 0), 1), r.get("error", ""), flush=True)
    except Exception:
        print(l.strip()[:200], flush=True)'
    n=$c
  fi
  [ -f survey/results.json ] && { echo DONE; break; }
  pgrep -f survey.py > /dev/null || { echo "survey.py exited"; break; }
  sleep 5
done
