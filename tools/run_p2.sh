#!/bin/bash
# Run Portal 2 under mac32 for a fixed time and summarize the log.
#   tools/run_p2.sh [seconds] [extra mac32 flags...]
# Log: $MAC32_LOG (default /tmp/mac32-p2.log), capped at 50 MB.
secs=${1:-30}; shift || true
P2="$HOME/Library/Application Support/Steam/steamapps/common/Portal 2"
LOG=${MAC32_LOG:-/tmp/mac32-p2.log}
cd "$P2" || exit 1
export SteamAppId=620 SteamGameId=620	# what Steam sets when it launches the game
set -m					# own process group, so we can stop all of it
( "$HOME/Developer/mac32/Mac32.app/Contents/MacOS/mac32" -v "$@" -L bin/osx32 ./portal2_osx -game portal2 ${NOSOUND--nosound} -novid -windowed -w 1280 -h 720 $GAMEARGS 2>&1 \
	| head -c 50000000 > "$LOG" ) &
pg=$!
for ((i = 0; i < secs; i++)); do
	sleep 1
	kill -0 $pg 2>/dev/null || break
	# ACTIVATE=<second>: bring the game to the front then (as a click would)
	[ "$i" = "${ACTIVATE:-x}" ] && open -b com.khesin.mac32 && echo "activated at ${i}s"
	# SHOT=<second>[,<second>...]: capture the game window at those times
	for t in ${SHOT//,/ }; do
		if [ "$i" = "$t" ]; then
			pid=$(pgrep -g $pg -x mac32 | head -1)
			id=$("$HOME/Developer/mac32/tools/winid" "$pid" 2>/dev/null)
			if [ -n "$id" ]; then
				/usr/sbin/screencapture -x -o -l "$id" "${SHOTDIR:-/tmp}/p2-${t}s.png" && echo "screenshot: ${SHOTDIR:-/tmp}/p2-${t}s.png"
			else
				echo "screenshot at ${t}s: no window yet"
			fi
		fi
	done
done
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
