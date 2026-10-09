#!/bin/bash
# Run Portal 2 under mac32 for a fixed time and summarize the log.
#   tools/run_p2.sh [seconds] [extra mac32 flags...]
# Log: $MAC32_LOG (default /tmp/mac32-p2.log), capped at 50 MB.
secs=${1:-30}; shift || true
P2="$HOME/Library/Application Support/Steam/steamapps/common/Portal 2"
LOG=${MAC32_LOG:-/tmp/mac32-p2.log}
cd "$P2" || exit 1
set -m					# own process group, so we can stop all of it
( "$HOME/Developer/mac32/mac32" -v "$@" -L bin/osx32 ./portal2_osx -game portal2 -nosound -novid -windowed -w 1280 -h 720 2>&1 \
	| head -c 50000000 > "$LOG" ) &
pg=$!
for ((i = 0; i < secs; i++)); do sleep 1; kill -0 $pg 2>/dev/null || break; done
if kill -0 $pg 2>/dev/null; then
	echo "(still running after ${secs}s; stopping process group $pg)"
	kill -TERM -$pg 2>/dev/null; sleep 1; kill -KILL -$pg 2>/dev/null
fi
wait $pg 2>/dev/null
echo "loaded: $(grep '^\[mac32\] loaded' "$LOG" | sed 's/.*\///; s/ at.*//' | tr '\n' ' ')"
echo "--- last lines:"
grep -v '^\[mac32\] \(unresolved\|initializer\|loaded\|objc:\)' "$LOG" | grep -v '^\[call\]\|^\[objc\]' | tail -${TAIL:-15}
echo "--- last calls:"
grep '^\[call\]\|^\[objc\]' "$LOG" | tail -${CALLS:-8}
