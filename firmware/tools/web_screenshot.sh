#!/usr/bin/env bash
# Render the web settings page with the display's live settings (no pairing needed) for docs:
#   tools/web_screenshot.sh out.png [width height]
set -euo pipefail
out=$(realpath -m "${1:-web.png}"); w=${2:-1400}; h=${3:-1180}  # resolve before cd
cd "$(dirname "$0")/.."
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
data=$(deskhud get)
python3 - "$data" > "$tmp/index.html" <<'PY'
import json, sys
d = json.loads(sys.argv[1])
d["device"]["pcs"] = [p for p in d["device"].get("pcs", []) if p.get("host") != "Web browser" and not p.get("host","").startswith("web:")]
# Placeholders: no Wi-Fi name, addresses or host names in published screenshots.
d["device"].update(ssid="home", ip="192.168.1.40", hostname="deskhud")
for i, p in enumerate(d["device"]["pcs"]):
    p["host"] = "workstation" if i == 0 else f"pc-{i + 1}"
html = open("main/web/index.html").read()
print(html.replace("<script>", "<script>window.DEMO_DATA=" + json.dumps(d) + ";</script>\n<script>", 1))
PY
chromium --headless=new --no-sandbox --disable-gpu --user-data-dir="$tmp/profile" --hide-scrollbars \
  --window-size="$w,$h" --virtual-time-budget=2000 --screenshot="$out" "file://$tmp/index.html?demo" >/dev/null 2>&1
echo "$out"
