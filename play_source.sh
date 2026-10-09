#!/bin/bash
# Play a 32-bit Mac Source engine game from your Steam library through mac32.
#   ./play_source.sh "Portal 2" [game options, e.g. -fullscreen -novid]
#   ./play_source.sh "Half-Life 2"
# The argument is the game's folder in steamapps/common. Needs Steam running and
# logged in (Steam checks that you own the game). Log: ~/Library/Logs/mac32-<folder>.log
here="$(cd "$(dirname "$0")" && pwd)"
steam="$HOME/Library/Application Support/Steam"
name="$1"; shift
[ -n "$name" ] || { echo "usage: $0 \"<game folder in steamapps/common>\" [game options]"; exit 1; }
game="$steam/steamapps/common/$name"
[ -d "$game" ] || { echo "not installed: $game"; exit 1; }
[ -x "$here/Mac32.app/Contents/MacOS/mac32" ] || { echo "build first: make -C \"$here\""; exit 1; }

# the launcher stub (hl2_osx, portal2_osx, ...) and its mod folder
exe=$(cd "$game" && ls -1 *_osx 2>/dev/null | head -1)
[ -n "$exe" ] || { echo "no *_osx launcher in $game (not a Mac Source game?)"; exit 1; }
mod=${exe%_osx}
# Steam app id from the manifest whose installdir is this folder
appid=$(grep -l "\"installdir\"[[:space:]]*\"$name\"" "$steam"/steamapps/appmanifest_*.acf 2>/dev/null | head -1 | sed 's/.*appmanifest_\([0-9]*\)\.acf/\1/')
[ -n "$appid" ] || { echo "cannot find the Steam app id for $name"; exit 1; }
pgrep -x steam_osx >/dev/null || echo "warning: Steam does not seem to be running; the game will refuse to start"

lib=bin/osx32; [ -d "$game/$lib" ] || lib=bin
cd "$game" || exit 1
export SteamAppId=$appid SteamGameId=$appid
echo "starting $name ($exe -game $mod, app $appid)"
exec "$here/Mac32.app/Contents/MacOS/mac32" -L "$lib" "./$exe" -game "$mod" "$@" \
	> "$HOME/Library/Logs/mac32-$name.log" 2>&1
