#!/bin/sh
set -eu
measure_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 "$measure_root/runtime/packaging/measure.py" "$@"
