#!/bin/bash
# Collect the imports mac32 does not cover yet, from the programs given (apps,
# program files, Steam game folders), into tools/wanted.txt for gen_api.py.
#   tools/wanted.sh ~/Developer/mac32-apps/apps/*/*.app "Portal"
here="$(cd "$(dirname "$0")/.." && pwd)"
for t in "$@"; do "$here/bin/mac32" check --all "$t" 2>/dev/null | sed -n 's/^    _//p'; done |
	sort -u > "$here/tools/wanted.txt"
echo "$(wc -l < "$here/tools/wanted.txt") names in tools/wanted.txt"
