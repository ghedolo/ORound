#!/bin/bash
# Usage: scripts/build.sh <firmware project dir>
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="${1:?usage: build.sh <project dir>}"
. "$ROOT/scripts/idf_env.sh"
cd "$PROJECT"
[ -f sdkconfig ] || idf.py set-target esp32s3
idf.py build
