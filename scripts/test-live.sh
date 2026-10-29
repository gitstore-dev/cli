#!/bin/sh
# Explicit opt-in only. Never runs as part of default CI.
set -eu
cd "$(dirname "$0")/.."
exec python3 scripts/test_live.py "$@"
