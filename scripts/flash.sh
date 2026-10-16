#!/bin/bash
# Usage: scripts/flash.sh <firmware project dir> [port] [--monitor]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="${1:?usage: flash.sh <project dir> [port] [--monitor]}"
PORT="${2:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
if [ -z "$PORT" ]; then
    echo "no /dev/cu.usbmodem* port found" >&2
    exit 1
fi
. "$ROOT/scripts/idf_env.sh"
cd "$PROJECT"
if [ "$3" = "--monitor" ]; then
    idf.py -p "$PORT" flash monitor
else
    idf.py -p "$PORT" flash
fi
