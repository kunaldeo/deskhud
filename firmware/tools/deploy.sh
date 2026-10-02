#!/usr/bin/env bash
# Build the firmware and install it over Wi-Fi (needs a paired PC running the monitoring service).
set -euo pipefail
cd "$(dirname "$0")/.."
. ./env.sh >/dev/null 2>&1
idf.py build 2>&1 | grep -E 'error|binary size' || true
deskhud=${DESKHUD:-$(command -v deskhud || echo ../monitor/target/release/deskhud)}
"$deskhud" ota build/deskhud.bin 2>&1 | grep -v '░' | tail -3
