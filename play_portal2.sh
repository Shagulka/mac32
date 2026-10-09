#!/bin/bash
# Play Portal 2 (Valve's 32-bit Mac build) through mac32.
#   ./play_portal2.sh [extra game options, e.g. -fullscreen]
# Needs Steam running and logged in: Steam checks that you own the game.
# Log: ~/Library/Logs/mac32-portal2.log
here="$(cd "$(dirname "$0")" && pwd)"
P2="$HOME/Library/Application Support/Steam/steamapps/common/Portal 2"
[ -x "$here/Mac32.app/Contents/MacOS/mac32" ] || { echo "build first: make -C \"$here\""; exit 1; }
pgrep -x steam_osx >/dev/null || echo "warning: Steam does not seem to be running; the game will refuse to start"
cd "$P2" || exit 1
export SteamAppId=620 SteamGameId=620
exec "$here/Mac32.app/Contents/MacOS/mac32" -L bin/osx32 ./portal2_osx -game portal2 "$@" \
	> "$HOME/Library/Logs/mac32-portal2.log" 2>&1
