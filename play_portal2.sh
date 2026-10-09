#!/bin/bash
# Play Portal 2 through mac32 (shortcut for: ./play_source.sh "Portal 2" ...).
exec "$(cd "$(dirname "$0")" && pwd)/play_source.sh" "Portal 2" "$@"
