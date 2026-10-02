#!/usr/bin/env bash
# Save the display's current screen as PNG: tools/screenshot.sh [out.png] [host]
set -euo pipefail
out=${1:-screenshot.png}; host=${2:-$(jq -r '.last_addr | to_entries[0].value' ~/.local/state/deskhud/state.json)}
id=$(curl -s "http://$host/api/info" | jq -r .id)
token=$(jq -r --arg id "$id" '.tokens[$id]' ~/.local/state/deskhud/state.json)
tmp=$(mktemp --suffix=.bmp); trap 'rm -f "$tmp"' EXIT
curl -sf -m 30 -H "Authorization: Bearer $token" "http://$host/api/screenshot" -o "$tmp"
magick "$tmp" "$out" && echo "$out"
