#!/bin/bash
# follow.sh <log> <process pattern>: print the log's new lines while the process runs, then FINISHED
n=0
while :; do
  running=$(pgrep -f "$2" > /dev/null && echo 1)
  c=$(wc -l < "$1" 2> /dev/null || echo 0)
  [ "$c" -gt "$n" ] && { sed -n "$((n + 1)),${c}p" "$1"; n=$c; }
  [ -z "$running" ] && { echo FINISHED; break; }
  sleep 5
done
